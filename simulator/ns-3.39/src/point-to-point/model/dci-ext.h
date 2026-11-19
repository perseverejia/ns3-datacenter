#ifndef DCI_EXT_H
#define DCI_EXT_H

// DCIExt: OBSERVE -> 学费 CNP -> SILENT -> 固定乘子表控制。
// RTT 仅使用无重传歧义的已确认数据，不按固定时延门限过滤。

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

struct DciExtRttStamp {
    Time sent;
    bool ambiguous;
};

struct DciExtRcvWatermarks {
    uint32_t ack;
    uint64_t tx;
};

// 接收方流表项：受管跨域流（从长链路进入接收方 DC）
struct DciExtRcvFlow {
    uint32_t sip, dip;
    uint16_t sport, dport;
    uint8_t pg;
    uint32_t inPort;         // 长链路入端口；SenderDC 按对端 DCI 分组，合并并行口
    uint32_t ackWatermark;   // ACK 累计确认水位（全局累计，差分进 winAckBytes）
    uint64_t txWatermark;    // 曾从本 DCI 发出的最高字节边界，老化后保留
    uint64_t winAckBytes;    // 本周期 ACK 确认字节（DIP ACK 反馈速率口径）
    Time winFirstAckT;       // 本周期首个 ACK 到达时刻（首窗归一分母；-1s=本窗无ACK）
    uint64_t winTxBytes;     // 本周期 DCI→DC 出队字节（txRate 口径，纯DC内RTT打点同源）
    uint32_t rttCnt;         // 本周期 RTT 样本数
    double rttSumUs;         // 样本和（us）
    double rttMinUs;         // 样本最小值（us，RTT_base 运行最小值更新源）
    double rttMaxUs;         // 样本最大值（us）
    std::map<uint32_t, DciExtRttStamp> dataArrival;
    Time lastActive;         // 老化用（数据包或 ACK 到达即刷新）

    DciExtRcvFlow() : sip(0), dip(0), sport(0), dport(0), pg(0), inPort(0),
                      ackWatermark(0), txWatermark(0), winAckBytes(0), winFirstAckT(Seconds(-1)),
                      winTxBytes(0),
                      rttCnt(0), rttSumUs(0), rttMinUs(1e18), rttMaxUs(0),
                      lastActive(Seconds(0)) {}

    void RecordTx(uint32_t seq, uint32_t size, Time now);
    bool TakeRttSample(uint32_t ackSeq, Time now, Time& rtt);
};

// 接收方 per-DIP 可用带宽探测状态（RTT 驱动）
struct DciExtRcvDip {
    double rAvblBps;   // 当前可用带宽估计（0xFA 反馈值）
    bool hasRAvbl;     // 是否已首次获取（首次 = 首窗活跃时长归一的 ACK 速率）
    double rttBaseUs;  // DC 内 RTT 运行最小值（RTT_base）
    bool hasRttBase;

    DciExtRcvDip() : rAvblBps(0), hasRAvbl(false), rttBaseUs(1e18), hasRttBase(false) {}
};

// A feedback arrival and a sender timer each measure their own interval.
struct DciExtRateWindow {
    Time start;
    uint64_t startBytes = 0;
    void Reset(Time now, uint64_t total) { start = now; startBytes = total; }
    double Settle(Time now, uint64_t total) {
        const Time elapsed = now - start;
        const uint64_t bytes = total - startBytes;
        Reset(now, total);
        return elapsed.IsStrictlyPositive() ? bytes * 8.0 / elapsed.GetSeconds() : 0.0;
    }
};

// 发送方流表项：观察期状态机
struct DciExtSndFlow {
    uint32_t sip, dip;
    uint16_t sport, dport;
    uint8_t pg;
    enum State { OBSERVE = 0, INIT_CNP = 1, SILENT = 2, CTL = 3 };
    State state;             // 观察 -> 学费 CNP -> 静默 -> T周期控制
    uint64_t observeBytes;   // 观察期累计发送字节（payload 口径）
    Time silentUntil;
    uint64_t startupBytes = 0; // this flow's WAN payload since entering silence
    DciExtRateWindow startupWindow;
    uint64_t dipBytesAtSilence = 0; // aggregate WAN payload at this flow's silence boundary
    uint32_t burstRemaining; // 本轮待连发 CNP 个数
    EventId burstEvent;
    Time lastActive;
    uint32_t ackWatermark;   // ACK 累计确认水位（回程差分扣减 Inflight 用）
    std::map<uint32_t, Time> dataArrival; // seq -> 出长链路时刻（RTT_cross 采样匹配）

    DciExtSndFlow() : sip(0), dip(0), sport(0), dport(0), pg(0),
                      state(OBSERVE), observeBytes(0), silentUntil(Seconds(0)),
                      burstRemaining(0), lastActive(Seconds(0)),
                      ackWatermark(0) {
        burstEvent = EventId();
    }
};

// 发送方 表A：DIP 速度表【DIP, R_avbl, R_snd, Inflight, RTT_cross】
struct DciExtSndDip {
    bool hasGrant = false; // 0xFA carries an aggregate quota for this destination
    double rAvblBps;      // aggregate quota received in 0xFA (bps)
    double sndRateBps;    // R_snd：最近一次实际测量窗口的发送速率
    DciExtRateWindow feedbackWindow, timerWindow;
    uint64_t dataSentBytes; // 已出长链路数据字节累计（Inflight = dataSent - acked）
    uint64_t ackedBytes;    // 回程 ACK 已确认字节累计（水位差分）
    double rttCrossMaxUs;   // 跨域 RTT 运行最大值（us，无样本时为 0）

    DciExtSndDip() : rAvblBps(0), sndRateBps(0),
                     dataSentBytes(0), ackedBytes(0), rttCrossMaxUs(0) {}
};

class DciExt {
public:
    static const uint32_t MAX_BURST_N = 80; // 固定表及普通压制的 CNP 上限

    DciExt();
    ~DciExt();
    void Dispose(); // 取消所有引用本实例的定时器并释放回调

    // ---------- 初始化（宿主 SwitchNode::InitializeDciExt 调用） ----------
    void Configure(uint32_t nodeId, uint32_t burstIntervalUs, double periodTUs,
                   uint64_t dipBwBps, uint64_t observeMb, uint32_t silentUs,
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
    bool IsLongLinkPort(Ptr<NetDevice> dev) const;  // topology 显式声明的 WAN 端口
    void PeriodicTick();                            // 接收方周期 T：聚合表一/二 + 发 0xFA
    void SndTick();                                 // 发送方周期 T：首调后，每 T 结算并控制
    void SendCnpForFlow(const DciExtFlowKey &key);  // 构造并发出 1 个 CNP
    void SendDipFeedback(uint32_t dip, uint32_t dstIp, double rAvblBps,
                          uint32_t wanPort = 0); // 0xFA 反馈
    void BurstTick(DciExtFlowKey key);              // 连发定时器回调
    uint32_t LookupN(double ratio) const;
    double InitialFlowRateBps() const { return m_dipBwBps / 4.0; }
    double ComputeV(const DciExtSndDip &e, double quotaBps);  // 原配额调整：首个跨域 RTT 前为 P，之后为 0.9P

    bool m_debug;                 // 日志开关
    uint32_t m_nodeId;            // 宿主节点 ID（日志用）
    bool m_senderSide;            // 发送方 DCI（表A/观察期/ECN反馈），否则接收方（表一/二）

private:
    void BuildTable(); // 构造时一次初始化，之后只查表

    Time m_burstInterval;         // 普通控制的 CNP 连发间隔，默认 4us
    Time m_periodT;               // 收发双方统一周期 T：接收方反馈、发送方压制
    double m_dipBwBps;            // 拓扑中的服务器接入带宽 B，用于发送侧初值及探测上界
    uint64_t m_observeBytes;      // 新流观察期字节数，配置单位为 MiB
    Time m_silentUs;             // 学费 burst 完成后的静默时长
    uint32_t m_agingPeriods;      // 流老化时长 = N 个周期 T
    double m_dtUs;                // 期望排队时延 Dt（RTT_t = RTT_base + Dt）
    double m_sigmaUs;             // RTT 目标区间容差 sigma
    double m_M[MAX_BURST_N + 1];  // 固定 alpha0=0.2、4us CNP 间隔的压制乘子

    Callback<void, Ptr<Packet>, CustomHeader&> m_sendCb;
    Callback<Ptr<QbbNetDevice>, Ptr<Packet>, CustomHeader&> m_getOutDevCb;
    Callback<Ptr<NetDevice>, uint32_t> m_getDevCb;

    std::map<DciExtFlowKey, DciExtRcvFlow> m_rcvFlows;  // 接收方流表
    // 老化后保留确认/发送水位，避免重计 ACK，也避免将旧包重传当作首次发送。
    std::map<DciExtFlowKey, DciExtRcvWatermarks> m_rcvWatermarks;
    std::map<DciExtFlowKey, DciExtSndFlow> m_sndFlows;  // 发送方流表
    std::map<uint32_t, DciExtSndDip> m_sndDips;         // 发送方 表A: dip -> 速度表项
    std::map<uint32_t, DciExtRcvDip> m_rcvDips;         // 接收方 per-DIP RTT探测状态

    EventId m_periodEvent;       // 接收方周期 T 定时器
    EventId m_sndTickEvent;      // 发送方周期 T 定时器（首调时刻为起点）
};

} // namespace ns3

#endif // DCI_EXT_H
