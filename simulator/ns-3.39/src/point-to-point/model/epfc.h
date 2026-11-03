#ifndef EPFC_H
#define EPFC_H

// ============================================================================
// ePFC (ccMode=17) v2 - extended PFC：PFC 的流级扩展
//
// 动机：PFC 是端口+队列级暂停，会连坐受害者流并产生级联。
// ePFC 在 PFC 扩散之前掐断拥塞贡献者流，保护受害者。
//
// 三个角色（全部内嵌于本类，由 SwitchNode::SendToDev 统一挂点）：
//   角色一 打标（所有交换机，逐包判定）：
//     条件A: ingress_bytes[inDev][pg] > PFC动态门限×MUL 且 egress_bytes[outIdx][pg] > EGRESS_THRESH
//     条件B: 本节点是接收方DCI 且 egress_bytes[outIdx][pg] > DCI_TH
//     打标 = IPv4 TOS bit7 (0x80)，粘性（任一跳置位后沿途不清除）
//   角色二 CCF 生成（接收方 host）+ 流表更新（接收方 DCI）：
//     接收方 host 收到标记包 → 生成 CCF【五元组+rv+时间戳】(协议号 0xFB) 原路回传
//     发送方收到 CCF → 暂停排空（≤20μs）后按 rv 直接设速，维持 2×RTT_internal；
//       一个 CCF 周期 = 暂停 + 2×RTT，周期内到达的 CCF 直接丢弃，周期完才接收下一个
//     接收方 DCI（唯一）收到 CCF → 更新出口端口流表
//       【流五元组, rv, 是否贡献者, 时间戳】（表挂数据出口端口 = CCF 入端口）
//   角色三 门控执行（接收方 DCI 的出口端口，令牌桶模型 v6.8）：
//     每条流表项维护令牌桶 tokens（字节）：
//       补充: 惰性按时间 tokens += rv/8×dt，上限 burst = rv/8×RTT/2
//             （= PortTick 一个周期的补充量，与恢复节拍匹配）
//       消耗: 数据包从出口端口实际出队 → tokens −= payload（Tag 机制）
//     判据: tokens < 0 → 本地门控该端口该 pg（信用透支即超速；末包允许跨零）
//     恢复: 时间补充使 tokens 自动回正 → 解除门控（出队驱动 + PortTick 兜底）。
//           长期平均速率恒等于补充率 rv，与环路 RTT / ACK 速率完全解耦——
//           credit 窗口的失败根因：等效允许吞吐 = 窗口/环路RTT，环路 RTT
//           动态且实测仅 7.5μs，rv×15μs 窗口仍放行 38.3G，固定窗口限不住
//     更新窗: 表项以 updateT（默认 1ms）为周期：窗内 rv 只取各 CCF 的最小值
//           （防止单个污染 rv 覆盖真值），超窗后条目失效放行，下个 CCF 重建新窗
//     老化: lastCcf + 2×RTT 无新 CCF → isContributor=false（自动放行）
//     rv 改写: 表更新后，若 CCF 原始 rv 高于表内净化值，就地改写 CCF 的 rv 字段为表值——
//           host 采样窗(30μs)与门控半周期(30μs)错拍，只有采样到 XON 倾泻期的 CCF
//           能通过 <1G 生成保护（峰值幸存者偏差），若原样转发发送方会按 38G 设速冲高；
//           改写后发送方设速、DCI 门控判据、CCF 三者共用同一 rv（保持一致）
//
// v2 与 v1 的区别：流表只在接收方 DCI 维护一张（不再有沿途边缘建表），
//   执行原语从"发 PFC 帧"改为"本地门控"（PFC 帧只能叫停对端，方向不对）
//
// 隔离性: 所有逻辑仅在本类内部, 由 SwitchNode::m_epfcEnabled 控制（ccMode=17）
// ============================================================================

#include <map>
#include <set>
#include "ns3/callback.h"
#include "ns3/custom-header.h"
#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/ptr.h"
#include "ns3/simulator.h"
#include "ns3/tag.h"

namespace ns3 {

class SwitchMmu;

// ePFC 拥塞贡献者标记位：IPv4 TOS 的 bit7（与 ECN 低 2 位正交）
static const uint8_t EPFC_MARK_MASK = 0x80;

// 数据流五元组 key（数据流正向）
struct EpfcFlowKey {
    uint32_t sip, dip;
    uint16_t sport, dport;
    EpfcFlowKey() : sip(0), dip(0), sport(0), dport(0) {}
    EpfcFlowKey(uint32_t s, uint32_t d, uint16_t sp, uint16_t dp)
        : sip(s), dip(d), sport(sp), dport(dp) {}
    bool operator<(const EpfcFlowKey &o) const {
        if (sip != o.sip) return sip < o.sip;
        if (dip != o.dip) return dip < o.dip;
        if (sport != o.sport) return sport < o.sport;
        return dport < o.dport;
    }
};

// 拥塞数据流表表项（挂在接收方 DCI 的"数据出口端口"上）
//   ts = 更新窗起点（重建时设定，窗内不变）：超 updateT 窗失效
//   lastCcf = 最近 CCF 到达时刻（每 CCF 刷新，老化用）——与 ts 分离，
//   否则 2×RTT(60μs) 老化会把 1ms 更新窗拦腰截断
struct EpfcFlowEntry {
    double rvBps;        // 接收方速率（本更新窗内各 CCF 的最小值）
    bool isContributor;  // 拥塞贡献者标记（窗口失效/老化自动重置为 false）
    Time ts;             // 更新窗起点（超窗失效判据基准）
    Time lastCcf;        // 最近 CCF 到达时刻（老化判断用，每 CCF 刷新）
    int64_t tokens;      // 令牌桶余额（字节）：出队 −payload，按时间补充 rv/8×dt（判据直接用）
    Time lastRefill;     // 上次令牌补充时刻（惰性补充基准）
    uint8_t pg;          // 流的 PG（门控用）
    EpfcFlowEntry() : rvBps(0), isContributor(false), ts(Seconds(0)),
                      lastCcf(Seconds(0)), tokens(0), lastRefill(Seconds(0)),
                      pg(3) {}
};

// 出队计量 Tag：SendToDev（入端口队列前）打上，DequeueAndTransmit（实际出队后）消费。
//   目的：令牌消耗的口径 = 物理发出字节——门控中被拦的包不出队、不经过出队挂点，
//   天然不消耗；XON 后倾泻的存量也如实扣减。旧实现（v5）在入队前计量，
//   导致 XOFF 期积压的包不计、XON 后存量+新包线速倾泻，门控被架空。
//   Tag 只在本机内存传播（包上线即消失），不影响对端。
class EpfcSAccountTag : public Tag {
public:
    EpfcSAccountTag() {}
    void SetKey(const EpfcFlowKey &k) { m_key = k; }
    const EpfcFlowKey &GetKey() const { return m_key; }
    static TypeId GetTypeId() {
        static TypeId tid = TypeId("ns3::EpfcSAccountTag")
                                .SetParent<Tag>()
                                .AddConstructor<EpfcSAccountTag>();
        return tid;
    }
    TypeId GetInstanceTypeId() const override { return GetTypeId(); }
    uint32_t GetSerializedSize() const override { return 12; }
    void Serialize(TagBuffer i) const override {
        i.WriteU32(m_key.sip);
        i.WriteU32(m_key.dip);
        i.WriteU16(m_key.sport);
        i.WriteU16(m_key.dport);
    }
    void Deserialize(TagBuffer i) override {
        m_key.sip = i.ReadU32();
        m_key.dip = i.ReadU32();
        m_key.sport = i.ReadU16();
        m_key.dport = i.ReadU16();
    }
    void Print(std::ostream &os) const override {
        os << "EpfcS(" << m_key.sip << "," << m_key.dip << "," << m_key.dport << ")";
    }

private:
    EpfcFlowKey m_key;
};

// 出口端口状态（表所在端口 = 数据出口端口）
struct EpfcPortState {
    std::map<EpfcFlowKey, EpfcFlowEntry> flows;
    bool gated;                       // 本端口当前是否被 ePFC 门控（停止该 pg 出队）
    std::set<uint8_t> gatedPgs;       // 已门控的 pg 集合
    EventId tickEvent;                // 恢复评估兜底定时器（ACK 驱动为主）
    EpfcPortState() : gated(false) {}
};

class Epfc {
public:
    Epfc();

    // 参数配置（workload 调用）
    void Configure(uint32_t nodeId, uint32_t ingressMulPct, uint64_t egressThreshBytes,
                   uint64_t dciThreshBytes, double rttUs, double agingRtts,
                   double updateTUs);
    void SetMmu(Ptr<SwitchMmu> mmu);  // 非内联：SwitchMmu 前向声明下 Ptr 赋值需完整类型
    void SetDebug(bool on) { m_debug = on; }

    // 门控回调：(端口idx, pg, on=true门控/false解除) —— SwitchNode 注入
    void SetGateCallback(Callback<void, uint32_t, uint8_t, bool> cb) { m_gateCb = cb; }

    // 接收方 DCI 节点集合（表只在接收方 DCI 维护）
    void SetRxDciNodes(const std::set<uint32_t> &nodes);

    // 固定 rv 测试开关（>0 启用）：表 rv 钉死为固定值，窗内 MIN 棘轮旁路，
    // CCF 改写改为双向（低样本也改写为固定值——竞争期低样本不得拖低发送方）。
    // 目的：隔离 rv 塌缩变量，单检验收方 DCI 门控执行层（令牌桶）能否兑现 rv
    void SetFixedRvBps(double bps) { m_fixedRvBps = bps; }

    // 角色三（入队挂点）：贡献者流包打计量 Tag（表命中才打，不累计）
    void DataAccount(uint32_t outIdx, Ptr<Packet> p, CustomHeader &ch);

    // 主入口（SwitchNode::SendToDev 挂点，出端口已确定）
    //   l3Prot==0x11: 打标判定（所有交换机）+ 贡献者流打计量 Tag（入队前）；
    //                 令牌的实际消耗在出队挂点 DataDequeue（实际出队后）
    //   l3Prot==0xFB: CCF → 接收方 DCI 更新流表（旁路，不拦截转发）
    //   （v6.8 令牌桶化：ACK 不再参与计量，速率控制完全由时间补充驱动）
    void PacketIn(uint32_t inDev, uint32_t outIdx, uint32_t qIndex,
                  Ptr<Packet> &p, CustomHeader &ch); // p 引用：CCF rv 改写后传播回转发路径
    // 角色三（出队挂点，QbbNetDevice 回调 EpfcDequeueHelper 触发）：
    //   实际出队时刻 tokens −= payload + 门控评估 —— 口径 = 物理发出字节
    void DataDequeue(uint32_t outIdx, const EpfcSAccountTag &tag, uint32_t payload);

private:
    // 角色一：打标判定（所有交换机）
    void MarkJudge(uint32_t inDev, uint32_t outIdx, Ptr<Packet> p, CustomHeader &ch);
    // 角色二：CCF 到达接收方 DCI → 更新出口端口流表（旁路，不拦截转发）
    void CcfUpdateTable(uint32_t inDev, Ptr<Packet> &p, CustomHeader &ch);

    // 令牌惰性补充：tokens += rv/8×dt，上限 burst = rv/8×RTT/2
    void RefillTokens(EpfcFlowEntry &e, Time now);
    // 桶深上限（字节）= 一个 PortTick 周期（RTT/2）的补充量
    double TokenBurst(const EpfcFlowEntry &e) const {
        return e.rvBps / 8.0 * (m_rttUs / 2) * 1e-6;
    }
    // 端口评估：任一贡献者令牌透支 → 门控；全部回正 → 解除
    void EvaluatePort(uint32_t port);
    // 门控期间的周期恢复评估（门控中该端口仍有出队调用但被拦，需定时器驱动）
    void PortTick(uint32_t port);
    // 全局老化 tick：表项 ts + agingRtts×RTT 无新 CCF → isContributor=false
    void AgingTick();
    // 通过回调对指定端口执行本地门控/解除
    void SendGate(uint32_t port, uint8_t pg, bool on);

    bool m_debug;
    uint32_t m_nodeId;
    uint32_t m_ingressMulPct;   // 打标 ingress 阈值 = PFC动态门限 × pct/100
    uint64_t m_egressThresh;    // 打标 egress 阈值（绝对字节）
    uint64_t m_dciThresh;       // 接收方 DCI 出口阈值（绝对字节）
    double m_rttUs;             // 内部流 RTT（判据与节流基准）
    double m_agingRtts;         // 老化倍数（lastCcf + agingRtts×RTT 重置）
    double m_updateTUs;         // 流表更新窗时长（us）：窗内 rv 取 MIN，超窗失效放行
    double m_fixedRvBps;        // 固定 rv 测试（bps，0=正常采样驱动）

    bool m_isRxDci;                          // 本节点是否接收方 DCI
    std::set<uint32_t> m_rxDciNodes;         // 接收方 DCI 节点集合

    Ptr<SwitchMmu> m_mmu;
    Callback<void, uint32_t, uint8_t, bool> m_gateCb;

    std::map<uint32_t, EpfcPortState> m_egressTable; // 出口端口idx -> 拥塞数据流表
    EventId m_agingEvent;
    uint64_t m_markCount;       // 打标计数（日志用）
    uint64_t m_ccfTblCount;     // 流表更新计数（日志用）
    uint64_t m_xoffCount;       // 门控计数（日志节流用：逐包振荡下防刷屏）
    uint64_t m_xonCount;        // 解除计数（日志节流用）
};

} // namespace ns3

#endif /* EPFC_H */
