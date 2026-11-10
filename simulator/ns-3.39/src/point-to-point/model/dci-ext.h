#ifndef DCI_EXT_H
#define DCI_EXT_H

// ============================================================================
// DCI-Ext (ccMode=16) v3 - 表驱动跨域拥塞控制（收发双方各建表，无门控）
//
// 发送方 DCI（senderSide=true，如 SW14）：
//   表 A DIP 速度表【DIP, R_avbl, R_snd】：R_avbl 由 0xFA 反馈更新，
//       R_snd 在收到 0FA 时刻结算（对齐节拍）；R_snd > R_avbl → 查表连发 CNP
//   流表（观察期状态机）：新流前 m_observeBytes(20MB) 线速 → 发 1 个 CNP
//       → 静默 1ms（alpha 衰减到 ~1/256）→ R_snd vs R_avbl 查表首次压制 → CTL
//   ECN 快速反馈：跨域流数据包携带 ECN → 清 ECN 位 + 立即反馈 CNP 给源
//   （CNP 生成权垄断在发送方：清位使接收方 host 不再生成慢速 CNP-ACK）
//
// 接收方 DCI（senderSide=false，如 SW15）：
//   流表：受管跨域流（从长链路入），记录长链路入端口（= SenderDC 标识）与
//         ACK 差分累计（per-DIP ACK 反馈速率）
//   表一【DIP, N(总流数), ACK反馈速率】：周期 T 从流表聚合（含老化）
//   表二【DIP, SenderDC, 流数】：同一次聚合按入端口分组
//   周期 T 反馈：N<3 用 DIP 的 ACK 反馈速率，N>=3 用 DIP 带宽 B(40G)；
//     除法用移位（N 上取 2 的幂 P，份额 = base >> log2(P)）；
//     每 SenderDC 分得 份额×该DC流数，编码为 0xFA 发往对端
//
// 0xFA 反馈报文：PPP + IPv4(0xFA) + qbbHeader（与 ACK 同格式）：
//   sport/dport = DIP 高/低 16 位，seq = R_avbl（Kbps 单位），q0 最高优先级
//
// 查表连发（与 v2 相同）：LookupN(r) = 最小 n 使 M[n] <= r，
//   每流都降到同一比例 r → 聚合发送速率按比例收敛到 R_avbl
//
// 隔离性: 所有逻辑仅在本类内部, 由 SwitchNode::m_dciExtEnabled 控制（ccMode=16 才为 true）
// ============================================================================

#include <map>
#include <tuple>
#include "ns3/callback.h"
#include "ns3/custom-header.h"
#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/packet.h"
#include "ns3/ptr.h"
#include "ns3/simulator.h"
#include "qbb-net-device.h"

namespace ns3 {

// 流表键：(sip, dip, sport, dport)
typedef std::tuple<uint32_t, uint32_t, uint16_t, uint16_t> DciExtFlowKey;

// 接收方流表项：受管跨域流（从长链路进入接收方 DC）
struct DciExtRcvFlow {
    uint32_t sip, dip;
    uint16_t sport, dport;
    uint8_t pg;
    uint32_t inPort;         // 长链路入端口（ifIndex）= SenderDC 标识
    uint32_t ackWatermark;   // ACK 累计确认水位（全局累计，差分进 winAckBytes）
    uint64_t winAckBytes;    // 本周期 ACK 确认字节（DIP ACK 反馈速率口径）
    uint64_t winTxBytes;     // 本周期 DCI→DC 出队字节（txRate 口径，纯DC内RTT打点同源）
    uint32_t rttCnt;         // 本周期 RTT 样本数
    double rttSumUs;         // 样本和（us）
    double rttMinUs;         // 样本最小值（us，RTT_base 运行最小值更新源）
    double rttMaxUs;         // 样本最大值（us）
    std::map<uint32_t, Time> dataArrival; // seq -> 数据包从 DCI 出队（发往DC）时刻
    Time lastActive;         // 老化用（数据包或 ACK 到达即刷新）

    DciExtRcvFlow() : sip(0), dip(0), sport(0), dport(0), pg(0), inPort(0),
                      ackWatermark(0), winAckBytes(0), winTxBytes(0),
                      rttCnt(0), rttSumUs(0), rttMinUs(1e18), rttMaxUs(0),
                      lastActive(Seconds(0)) {}
};

// 接收方 per-DIP 可用带宽探测状态（RTT 驱动）
struct DciExtRcvDip {
    double rAvblBps;   // 当前可用带宽估计（0xFA 反馈值）
    bool hasRAvbl;     // 是否已首次获取（首次 = 直接用 ACK 返回速率）
    double rttBaseUs;  // DC 内 RTT 运行最小值（RTT_base）
    bool hasRttBase;
    bool lastCongested; // 上一周期分支是否为 CONGESTED（RTT > RTT_t+σ）

    DciExtRcvDip() : rAvblBps(0), hasRAvbl(false), rttBaseUs(1e18), hasRttBase(false), lastCongested(false) {}
};

// 发送方流表项：观察期状态机
struct DciExtSndFlow {
    uint32_t sip, dip;
    uint16_t sport, dport;
    uint8_t pg;
    enum State { OBSERVE = 0, SILENT = 1, CTL = 2 };
    State state;             // OBSERVE=20MB线速观察; SILENT=1ms静默; CTL=受查表控制
    uint64_t observeBytes;   // 观察期累计发送字节（payload 口径）
    Time silentUntil;        // 静默期满时刻（SILENT 态）
    uint32_t burstRemaining; // 本轮待连发 CNP 个数
    EventId burstEvent;
    Time lastActive;
    bool pendingSilent;      // 学费 5 连发中：burst 结束后才转入 SILENT（静默起点 = burst 结束）
    uint32_t ackWatermark;   // ACK 累计确认水位（回程差分扣减 Inflight 用）
    std::map<uint32_t, Time> dataArrival; // seq -> 出长链路时刻（RTT_cross 采样匹配）

    DciExtSndFlow() : sip(0), dip(0), sport(0), dport(0), pg(0),
                      state(OBSERVE), observeBytes(0), silentUntil(Seconds(0)),
                      burstRemaining(0), lastActive(Seconds(0)), pendingSilent(false),
                      ackWatermark(0) {
        burstEvent = EventId();
    }
};

// 发送方 表A：DIP 速度表【DIP, R_avbl, R_snd, Inflight, RTT_cross】
struct DciExtSndDip {
    double rAvblBps;      // 可用带宽 P（0xFA 更新；初始 = 长链路带宽 >> 2）
    uint64_t sndBytes;    // 出长链路字节累计（payload 口径，R_snd 结算源）
    double sndRateBps;    // R_snd：上次结算的发送速率（0xFA 到达时刻结算）
    Time lastSettle;      // 上次结算时刻
    uint64_t dataSentBytes; // 已出长链路数据字节累计（Inflight = dataSent - acked）
    uint64_t ackedBytes;    // 回程 ACK 已确认字节累计（水位差分）
    double rttCrossMaxUs;   // 跨域 RTT 运行最大值（us，无样本时为 0）

    DciExtSndDip() : rAvblBps(0), sndBytes(0), sndRateBps(0), lastSettle(Seconds(0)),
                     dataSentBytes(0), ackedBytes(0), rttCrossMaxUs(0) {}
};

class DciExt {
public:
    static const uint32_t MAX_BURST_N = 256; // 查表连发的最大 CNP 个数

    DciExt();

    // ---------- 初始化（宿主 SwitchNode::InitializeDciExt 调用） ----------
    void Configure(uint32_t nodeId, uint32_t burstIntervalUs, double periodTUs,
                   double dipBwGbps, uint64_t observeMb, uint32_t silentUs,
                   uint32_t agingPeriods, double dtUs, double sigmaUs);
    // 发送回调 -> SwitchNode::SendToDev（CNP / 0xFA 独立包发出）
    void SetSendCallback(Callback<void, Ptr<Packet>, CustomHeader&> cb);
    // 路由查询回调 -> SwitchNode::GetOutDevice（判定包出口是否走长链路）
    void SetGetOutDevCallback(Callback<Ptr<QbbNetDevice>, Ptr<Packet>, CustomHeader&> cb);
    // 端口设备查询回调 -> SwitchNode::GetDevByIndex（NotifyDequeue 判定出端口是否长链路）
    void SetGetDevByIndexCallback(Callback<Ptr<NetDevice>, uint32_t> cb);
    // 角色设置：true=发送方 DCI（表A+观察期+ECN反馈+0xFA消费），false=接收方 DCI（表一/二+0xFA生成）
    void SetSenderSide(bool enable) { m_senderSide = enable; }

    // ---------- 挂钩点（switch-node.cc 调用） ----------
    // 返回 true = 拦截该包（0xFA 到达发送方 DCI 即被消费，不再转发）
    bool PacketIn(Ptr<NetDevice> device, Ptr<Packet> pkt, CustomHeader &ch);
    void NotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p);

    // ---------- 内部 ----------
    bool IsLongLinkPort(Ptr<NetDevice> dev) const;  // channel delay > 100us
    void PeriodicTick();                            // 接收方周期 T：聚合表一/二 + 发 0xFA
    void SndTick();                                 // 发送方周期 T：首调时刻起，每 T 维持 R_snd ≤ R_avbl
    void SendCnpForFlow(const DciExtFlowKey &key);  // 构造并发出 1 个 CNP
    void SendDipFeedback(uint32_t dip, uint32_t dstIp, double rAvblBps); // 0xFA 反馈
    void BurstTick(DciExtFlowKey key);              // 连发定时器回调
    uint32_t LookupN(double ratio) const;           // 查表: 目标压制比例 -> 连发个数 n
    double ComputeV(DciExtSndDip &e, double tSec);  // BDP 管道填充: v = (P×(T+RTT_cross) - Inflight)/T

    bool m_debug;                 // 日志开关
    uint32_t m_nodeId;            // 宿主节点 ID（日志用）
    bool m_senderSide;            // 发送方 DCI（表A/观察期/ECN反馈），否则接收方（表一/二）

private:
    void BuildTable();            // 预计算 M[n] 压制乘子表

    Time m_burstInterval;         // CNP 连发间隔（对应 1us alpha 更新窗口）
    Time m_periodT;               // 收发双方统一周期 T：接收方反馈、发送方压制
    double m_dipBwBps;            // DIP 带宽 B（N>=3 时的分配基准）
    uint64_t m_observeBytes;      // 新流观察期字节数（20MB 线速）
    Time m_silentUs;              // 观察期满学费 CNP 后的静默时长（1ms，alpha 衰减）
    uint32_t m_agingPeriods;      // 流老化时长 = N 个周期 T
    double m_dtUs;                // 期望排队时延 Dt（RTT_t = RTT_base + Dt）
    double m_sigmaUs;             // RTT 目标区间容差 sigma

    double m_M[MAX_BURST_N + 1];  // M[n]: 连发 n 个 CNP 后速率与原速率之比（查表用）

    Callback<void, Ptr<Packet>, CustomHeader&> m_sendCb;
    Callback<Ptr<QbbNetDevice>, Ptr<Packet>, CustomHeader&> m_getOutDevCb;
    Callback<Ptr<NetDevice>, uint32_t> m_getDevCb;

    std::map<DciExtFlowKey, DciExtRcvFlow> m_rcvFlows;  // 接收方流表
    std::map<DciExtFlowKey, DciExtSndFlow> m_sndFlows;  // 发送方流表
    std::map<uint32_t, DciExtSndDip> m_sndDips;         // 发送方 表A: dip -> 速度表项
    std::map<uint32_t, DciExtRcvDip> m_rcvDips;         // 接收方 per-DIP RTT探测状态

    EventId m_periodEvent;       // 接收方周期 T 定时器
    EventId m_sndTickEvent;      // 发送方周期 T 定时器（首调时刻为起点）
};

} // namespace ns3

#endif // DCI_EXT_H
