#include "dci-ext.h"

#include <algorithm>
#include <cstdio>
#include <iterator>

#include "ns3/abort.h"
#include "ns3/channel.h"
#include "ns3/custom-priority-tag.h"
#include "ns3/ipv4-header.h"
#include "ns3/node.h"
#include "ns3/ppp-header.h"
#include "ns3/qbb-header.h"
#include "ns3/random-variable.h"

namespace ns3 {

namespace {
constexpr uint64_t kDciExtMinRateBps = 1000000000ULL; // 停滞门限、探测下界及每流配额下界：1Gbps
}

void DciExtRcvFlow::RecordTx(uint32_t seq, uint32_t size, Time now) {
    if (seq < ackWatermark)
        return; // 已确认字节的重复包不能重新成为 RTT 样本。
    bool ambiguous = (uint64_t)seq < txWatermark;
    auto inserted = dataArrival.emplace(seq, DciExtRttStamp{now, ambiguous});
    if (!inserted.second)
        inserted.first->second.ambiguous = true; // 保留首次时间戳，不匹配到后来的重传。
    txWatermark = std::max(txWatermark, (uint64_t)seq + size);
}

bool DciExtRcvFlow::TakeRttSample(uint32_t ackSeq, Time now, Time& rtt) {
    auto end = dataArrival.lower_bound(ackSeq); // ACK 只确认 seq < ackSeq。
    if (end == dataArrival.begin())
        return false;
    auto sample = std::prev(end);
    bool valid = !sample->second.ambiguous && now > sample->second.sent;
    if (valid)
        rtt = now - sample->second.sent;
    dataArrival.erase(dataArrival.begin(), end);
    return valid;
}

DciExt::DciExt()
    : m_debug(true), m_nodeId(0), m_senderSide(false),
      m_burstInterval(MicroSeconds(4)),
      m_periodT(MicroSeconds(400)),
      m_dipBwBps(0), m_observeBytes(20ULL << 20), m_silentUs(MicroSeconds(400)),
      m_agingPeriods(2), m_dtUs(4), m_sigmaUs(3) {
    BuildTable();
    m_periodEvent = EventId();
}

DciExt::~DciExt() {
    Dispose();
}

void DciExt::Dispose() {
    Simulator::Remove(m_periodEvent);
    Simulator::Remove(m_sndTickEvent);
    m_periodEvent = EventId();
    m_sndTickEvent = EventId();
    for (auto& entry : m_sndFlows)
        Simulator::Remove(entry.second.burstEvent);
    m_sndFlows.clear();
    m_sndDips.clear();
    m_rcvFlows.clear();
    m_rcvWatermarks.clear();
    m_rcvDips.clear();
    m_sendCb.Nullify();
    m_getOutDevCb.Nullify();
    m_getDevCb.Nullify();
}

void DciExt::Configure(uint32_t nodeId, uint32_t burstIntervalUs, double periodTUs,
                       uint64_t dipBwBps, uint64_t observeMb, uint32_t silentUs,
                       uint32_t agingPeriods, double dtUs, double sigmaUs) {
    m_nodeId = nodeId;
    m_burstInterval = MicroSeconds(burstIntervalUs);
    m_periodT = MicroSeconds(periodTUs);
    m_dipBwBps = dipBwBps; // B = 拓扑中的服务器接入带宽，收发两侧一致
    m_observeBytes = observeMb << 20; // 沿用 dcix 的 MiB 单位
    m_silentUs = MicroSeconds(silentUs);
    m_agingPeriods = agingPeriods;
    m_dtUs = dtUs;
    m_sigmaUs = sigmaUs;

    // 接收侧按 T 反馈；发送侧在首个流离开静默期、完成首调后启动定时器。
    if (!m_senderSide && m_periodT > Seconds(0))
        m_periodEvent = Simulator::Schedule(m_periodT, &DciExt::PeriodicTick, this);
    m_sndTickEvent = EventId();
}

void DciExt::SetSendCallback(Callback<void, Ptr<Packet>, CustomHeader&> cb) {
    m_sendCb = cb;
}

void DciExt::SetGetOutDevCallback(Callback<Ptr<QbbNetDevice>, Ptr<Packet>, CustomHeader&> cb) {
    m_getOutDevCb = cb;
}

void DciExt::SetGetDevByIndexCallback(Callback<Ptr<NetDevice>, uint32_t> cb) {
    m_getDevCb = cb;
}

bool DciExt::IsLongLinkPort(Ptr<NetDevice> dev) const {
    // 长链路判定真值源 = topology 文件第 3 行的长链路编号声明（如 "14" = 第 14 条链路），
    // 由 workload 拓扑解析时在两端 QbbNetDevice 上显式设置 WanPort 标志；
    // 不再用 channel delay > 100us 阈值自判（阈值方案已被 topo 声明方案取代）
    if (dev == nullptr)
        return false;
    Ptr<QbbNetDevice> qdev = DynamicCast<QbbNetDevice>(dev);
    return (qdev != nullptr && qdev->IsWanPort());
}

// 与 dcix 相同的固定压制表：alpha0=0.2，g=1/256，CNP 间隔 4us。
// 只在构造时生成；控制时按目标速率比例查表，不追踪端侧 alpha 状态。
void DciExt::BuildTable() {
    const double g = 1.0 / 256.0;
    double alpha = 0.2;
    double multiplier = 1.0;
    m_M[0] = multiplier;
    for (uint32_t n = 1; n <= MAX_BURST_N; ++n) {
        alpha = (1.0 - g) * alpha + g;
        for (uint32_t k = 0; k < 3; ++k)
            alpha *= 1.0 - g;
        multiplier *= 1.0 - alpha / 2.0;
        m_M[n] = multiplier;
    }
}

uint32_t DciExt::LookupN(double ratio) const {
    if (ratio >= 1.0)
        return 0;
    for (uint32_t n = 1; n <= MAX_BURST_N; ++n)
        if (m_M[n] <= ratio)
            return n;
    return MAX_BURST_N;
}

// ---------------------------------------------------------------------------
// 挂钩点 1：入包（SwitchReceiveFromDevice）
// 返回 true = 拦截（仅 0xFA 到达发送方 DCI 时）
//
// 发送方 DCI：
//   - 跨域流数据包（DC 内入、出口长链路）：新流建表进观察期；
//     ECN 快速反馈：携带 ECN → 清 ECN 位 + 立即反馈 CNP（CNP 生成权垄断：
//     清位后接收方 host 不再生成走 5ms 长链路的慢速 CNP-ACK）
//   - 0xFA 从长链路来：拦截消费 → 更新表A R_avbl；结算发送速率并按原机制查表调速
// 接收方 DCI：
//   - 长链路数据包：建流表（WAN 对端 DCI = SenderDC，inPort 供反馈返回）
//   - ACK/NACK 来自 DC 内且回程走长链路：per-DIP ACK 差分累计（表一数据源）
// ---------------------------------------------------------------------------
bool DciExt::PacketIn(Ptr<NetDevice> device, Ptr<Packet> pkt, CustomHeader &ch) {
    bool fromLongLink = IsLongLinkPort(device);

    // ==================== 发送方 DCI ====================
    if (m_senderSide) {
        // 0xFA 反馈从长链路到达：拦截消费（报文到此为止，不进转发路径）
        if (fromLongLink && ch.l3Prot == 0xFA) {
            uint32_t dip = ((uint32_t)ch.ack.sport << 16) | ch.ack.dport;
            double rAvblBps = (double)ch.ack.seq * 1e3; // Kbps 编码 -> bps

            DciExtSndDip &e = m_sndDips[dip];
            e.rAvblBps = rAvblBps;
            e.hasGrant = true;
            Time now = Simulator::Now();
            e.sndRateBps = e.feedbackWindow.Settle(now, e.dataSentBytes);

            if (m_debug) {
                printf("[DCIEXT_FB] %lu node=%u dip=0x%08x rAvbl=%.3fG R_snd=%.3fG\n",
                       now.GetTimeStep(), m_nodeId, dip,
                       e.rAvblBps / 1e9, e.sndRateBps / 1e9);
            }

            // 当前配额控制：首个跨域 RTT 前用 P，之后用 0.9P。
            if (e.rAvblBps > 0 && e.sndRateBps > 0) {
                double vBps = ComputeV(e, e.rAvblBps);
                // v > P 用 v；v < P 时取 max(v, 0.9P)：压制带宽下限贴 P 的九折，让跨域流速贴近均分线
                double effBps = (vBps > e.rAvblBps) ? vBps : std::max(vBps, 0.9 * e.rAvblBps);
                if (e.sndRateBps > effBps) {
                    double ratio = effBps / e.sndRateBps;
                    for (auto &kv : m_sndFlows) {
                        DciExtSndFlow &f = kv.second;
                        if (f.dip != dip || f.state != DciExtSndFlow::CTL)
                            continue;           // 观察/静默期流不受本轮压制
                        if (f.burstRemaining > 0)
                            continue;           // 已有 burst 在发，避免并发
                        uint32_t n = LookupN(ratio);
                        if (n == 0)
                            n = 1;
                        if (m_debug) {
                            printf("[DCIEXT_CNP] %lu node=%u type=OVERLOAD n=%u dip=0x%08x "
                                   "R_snd=%.3fG P=%.3fG v=%.3fG infl=%.1fMB rttX=%.1fus ratio=%.3f\n",
                                   now.GetTimeStep(), m_nodeId, n, dip,
                                   e.sndRateBps / 1e9, e.rAvblBps / 1e9, vBps / 1e9,
                                   (double)(e.dataSentBytes - e.ackedBytes) / 1048576.0,
                                   e.rttCrossMaxUs, ratio);
                        }
                        f.burstRemaining = n;
                        BurstTick(kv.first);
                    }
                }
            }
            return true; // 拦截
        }

        // 跨域流 ACK 从长链路回来：ACK 水位差分扣减 Inflight + RTT_cross 采样
        // （dataArrival 匹配模式与接收方 DCI 一致；rttCrossMaxUs 取运行最大值）
        if (fromLongLink && ch.l3Prot == 0xFC) {
            DciExtFlowKey ackKey(ch.dip, ch.sip, ch.ack.dport, ch.ack.sport);
            auto fit = m_sndFlows.find(ackKey);
            if (fit != m_sndFlows.end()) {
                DciExtSndFlow &f = fit->second;
                auto dit = m_sndDips.find(f.dip);
                if (dit != m_sndDips.end()) {
                    DciExtSndDip &e = dit->second;
                    uint32_t ackSeq = ch.ack.seq;
                    if (ackSeq > f.ackWatermark) {
                        e.ackedBytes += (uint64_t)(ackSeq - f.ackWatermark);
                        f.ackWatermark = ackSeq;
                        // RTT_cross 采样：确认最近被确认包的出长链->ACK到达差（含两个 5ms 长链）
                        auto ub = f.dataArrival.lower_bound(ackSeq); // ACK 只确认 seq < ackSeq
                        if (ub != f.dataArrival.begin()) {
                            --ub;
                            double rttUs = (double)(Simulator::Now() - ub->second).GetTimeStep() / 1000.0;
                            if (rttUs > e.rttCrossMaxUs)
                                e.rttCrossMaxUs = rttUs;
                            f.dataArrival.erase(f.dataArrival.begin(), std::next(ub));
                        }
                    }
                }
            }
            return false; // ACK 放行转发（去往源主机）
        }

        // 跨域流数据包：DC 内入、出口走长链路
        if (!fromLongLink && ch.l3Prot == 0x11 && !m_getOutDevCb.IsNull()) {
            Ptr<QbbNetDevice> outDev = m_getOutDevCb(pkt, ch);
            if (outDev != nullptr && IsLongLinkPort(outDev)) {
                DciExtFlowKey key(ch.sip, ch.dip, ch.udp.sport, ch.udp.dport);
                bool isNew = (m_sndFlows.find(key) == m_sndFlows.end());

                const bool fastCnp = ch.GetIpv4EcnBits() != 0;
                // ECN 快速反馈（观察期照常）：清 ECN 位 + 立即 CNP
                if (ch.GetIpv4EcnBits() != 0) {
                    PppHeader ppp;
                    Ipv4Header iph;
                    pkt->RemoveHeader(ppp);
                    pkt->RemoveHeader(iph);
                    iph.SetTos(iph.GetTos() & 0xFC); // 清 ECN 低 2 位
                    pkt->AddHeader(iph);
                    pkt->AddHeader(ppp);
                    ch.m_tos &= 0xFC; // 同步已解析的 ch
                    if (m_debug) {
                        printf("[DCIEXT_ECN] %lu node=%u type=FAST_CNP sip=0x%08x sport=%u dport=%u\n",
                               Simulator::Now().GetTimeStep(), m_nodeId, ch.sip, ch.udp.sport, ch.udp.dport);
                    }
                }

                if (isNew) {
                    // 新流：观察 -> 学费 CNP -> 静默 -> T周期查表控制
                    DciExtSndFlow &f = m_sndFlows[key];
                    f.sip = ch.sip;
                    f.dip = ch.dip;
                    f.sport = ch.udp.sport;
                    f.dport = ch.udp.dport;
                    f.pg = (uint8_t)ch.udp.pg;
                    f.state = DciExtSndFlow::OBSERVE;
                    f.observeBytes = 0;
                    f.lastActive = Simulator::Now();

                    // Until the first 0xFA, each new flow may use B/4.
                    auto dit = m_sndDips.find(ch.dip);
                    if (dit == m_sndDips.end()) {
                        DciExtSndDip &e = m_sndDips[ch.dip];
                        e.feedbackWindow.Reset(Simulator::Now(), e.dataSentBytes);
                        e.timerWindow.Reset(Simulator::Now(), e.dataSentBytes);
                        if (m_debug) {
                            printf("[DCIEXT_NEW_DIP] %lu node=%u dip=0x%08x per_flow_start=%.3fG (B/4)\n",
                                   Simulator::Now().GetTimeStep(), m_nodeId, ch.dip, InitialFlowRateBps() / 1e9);
                        }
                    }
                }
                if (fastCnp) SendCnpForFlow(key);
            }
        }
        return false; // 数据包照常转发（ECN 已被清除）
    }

    // ==================== 接收方 DCI ====================
    if (fromLongLink && ch.l3Prot == 0x11) {
        // 正向数据包从长链路到达：建表并保留反馈返回端口。
        DciExtFlowKey key(ch.sip, ch.dip, ch.udp.sport, ch.udp.dport);
        auto inserted = m_rcvFlows.try_emplace(key);
        DciExtRcvFlow &f = inserted.first->second;
        if (inserted.second) {
            auto saved = m_rcvWatermarks.find(key);
            if (saved != m_rcvWatermarks.end()) {
                f.ackWatermark = saved->second.ack;
                f.txWatermark = saved->second.tx;
                m_rcvWatermarks.erase(saved);
            }
        }
        f.sip = ch.sip;
        f.dip = ch.dip;
        f.sport = ch.udp.sport;
        f.dport = ch.udp.dport;
        f.pg = (uint8_t)ch.udp.pg;
        f.inPort = device->GetIfIndex(); // 并行口在周期聚合时按对端 DCI 合并。
        f.lastActive = Simulator::Now();
        return false;
    }

    // CNP 生成权垄断（接收侧·独立帧）：host4 生成的独立 CNP 帧（l3Prot=0xFF，
    // 区别于 ACK 搭载位——后者已在下方剥除）不允许穿透长链路。它一旦到达跨域
    // 发送方会与 0xFA 配额形成双通道压制，并与域内 DCQCN 恢复周期耦合共振
    // （实测：跨域锯齿/82ms 断流/低位均衡）。跨域流拥塞信号唯一通道 = 0xFA 配额。
    // 本 DC 域内流的 CNP 保留，由出口是否为 WAN 区分。
    if (!fromLongLink && ch.l3Prot == 0xFF) {
        // 只拦截即将跨 WAN 返回源端的 CNP；DCI 也可能转发域内流量。
        if (m_getOutDevCb.IsNull())
            return false;
        Ptr<QbbNetDevice> outDev = m_getOutDevCb(pkt, ch);
        if (outDev == nullptr || !IsLongLinkPort(outDev))
            return false;
        if (m_debug) {
            printf("[DCIEXT_CNP_STRIP] %lu node=%u type=FRAME sip=0x%08x dip=0x%08x\n",
                   Simulator::Now().GetTimeStep(), m_nodeId, ch.sip, ch.dip);
        }
        return true; // 拦截丢弃（不进转发路径）
    }

    if (!fromLongLink && (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD)) {
        // 来自接收方 DC 的 ACK/NACK：只处理"回程走长链路"的受管流 ACK
        if (m_getOutDevCb.IsNull())
            return false;
        Ptr<QbbNetDevice> outDev = m_getOutDevCb(pkt, ch);
        if (outDev == nullptr || !IsLongLinkPort(outDev))
            return false;

        // CNP 生成权垄断（接收侧）：跨域流拥塞信号唯一通道 = 发送方 DCI 快速 CNP。
        // 接收方 host 会给 ECN 标记包的 ACK/NACK 搭 CNP 位（ECN 是 SW19/SW14 在
        // 接收 DC 内标的，发送侧清位够不到）——这种慢速 CNP 走 5ms 长链路回传，
        // 密度高达每包一个（实测 2380/ms），会把端侧 alpha 钉死在 1、速率压到下限。
        // 接收方 DCI 在 ACK/NACK 出长链路前统一剥除 CNP 位。
        if (ch.ack.flags & (1 << qbbHeader::FLAG_CNP)) {
            PppHeader ppp;
            Ipv4Header iph;
            qbbHeader qh;
            pkt->RemoveHeader(ppp);
            pkt->RemoveHeader(iph);
            pkt->RemoveHeader(qh);
            qh.ClearCnp();
            pkt->AddHeader(qh);
            pkt->AddHeader(iph);
            pkt->AddHeader(ppp);
            ch.ack.flags &= ~(uint8_t)(1 << qbbHeader::FLAG_CNP); // 同步已解析的 ch
            if (m_debug) {
                printf("[DCIEXT_CNP_STRIP] %lu node=%u dport=%u\n",
                       Simulator::Now().GetTimeStep(), m_nodeId, ch.ack.dport);
            }
        }

        // ACK 包的 sport/dport 与数据流反转
        DciExtFlowKey key(ch.dip, ch.sip, ch.ack.dport, ch.ack.sport);
        auto it = m_rcvFlows.find(key);
        if (it == m_rcvFlows.end()) {
            // 已老化流的迟到 ACK 仅推进保存的水位，不重新激活或补计历史字节。
            auto saved = m_rcvWatermarks.find(key);
            if (saved != m_rcvWatermarks.end() && ch.ack.seq > saved->second.ack)
                saved->second.ack = ch.ack.seq;
            return false; // 非活动受管流，原样放行
        }
        DciExtRcvFlow &f = it->second;
        f.lastActive = Simulator::Now();

        // ACK 序号差分累计（乱序期重复 NACK 序号相同 -> 增量 0，自动幂等）
        uint32_t ackSeq = ch.ack.seq;
        if (ackSeq > f.ackWatermark) {
            if (f.winAckBytes == 0)
                f.winFirstAckT = Simulator::Now(); // 本窗首个 ACK（首窗归一分母）
            f.winAckBytes += (uint64_t)(ackSeq - f.ackWatermark);
            f.ackWatermark = ackSeq;

            // PFC 会推迟 ACK；重传会使 ACK 与发送实例的对应关系有歧义。
            // 按 Karn 原则跳过重传候选，允许真实的短 RTT，无固定 20us 门限。
            Time rtt;
            if (f.TakeRttSample(ackSeq, Simulator::Now(), rtt)) {
                double rttUs = rtt.GetSeconds() * 1e6;
                f.rttCnt++;
                f.rttSumUs += rttUs;
                if (rttUs < f.rttMinUs) f.rttMinUs = rttUs;
                if (rttUs > f.rttMaxUs) f.rttMaxUs = rttUs;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 挂钩点 2：出队（SwitchNotifyDequeue）
//   发送方 DCI：受管流数据包从长链路端口出队时
//     表A：dataSentBytes 累计（R_snd 口径 = 实际出长链路的 payload 字节）
//     流表：OBSERVE -> 学费 CNP -> SILENT -> CTL
//   接收方 DCI：受管流数据包从 DC 内端口（非长链路）出队时
//     流表：winTxBytes 累计（txRate 口径）+ dataArrival[seq] 打点
//     （纯 DC 内 RTT 起点：不含数据在 SW22 深缓冲里的滞留时间）
// ---------------------------------------------------------------------------
void DciExt::NotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p) {
    if (qIndex == 0)
        return; // 控制队列（PFC/CNP/0xFA）不统计

    // 出端口设备（判定回调由 SwitchNode 注入）
    if (m_getDevCb.IsNull())
        return;
    Ptr<NetDevice> outDev = m_getDevCb(ifIndex);
    if (outDev == nullptr)
        return;
    bool isLongLink = IsLongLinkPort(outDev);

    if (!m_senderSide) {
        // ==================== 接收方：DC 内出队打点 ====================
        if (isLongLink)
            return; // 接收方只关心发往 DC 内的数据包（ACK 出 WAN 不计）

        // 从包 buffer 解析 UDP 四元组 + seq（PPP + IPv4 + UDP）
        const uint8_t *buf = p->GetBuffer();
        uint32_t base = PppHeader::GetStaticSize();
        if (buf[base + 9] != 0x11)
            return; // 非 UDP 数据包（ACK/NACK/PFC 跳过）

        uint32_t sip = ((uint32_t)buf[base + 12] << 24) | ((uint32_t)buf[base + 13] << 16) |
                       ((uint32_t)buf[base + 14] << 8) | buf[base + 15];
        uint32_t dip = ((uint32_t)buf[base + 16] << 24) | ((uint32_t)buf[base + 17] << 16) |
                       ((uint32_t)buf[base + 18] << 8) | buf[base + 19];
        uint16_t sport = (uint16_t)((buf[base + 20] << 8) | buf[base + 21]);
        uint16_t dport = (uint16_t)((buf[base + 22] << 8) | buf[base + 23]);

        DciExtFlowKey key(sip, dip, sport, dport);
        auto it = m_rcvFlows.find(key);
        if (it == m_rcvFlows.end())
            return; // 非受管流（DC 内本地流等）
        DciExtRcvFlow &f = it->second;
        Time now = Simulator::Now();
        f.lastActive = now;
        uint32_t payload = p->GetSize() - CustomHeader::GetStaticWholeHeaderSize();
        f.winTxBytes += payload;

        // qbbHeader: sport(2) dport(2) flags(2) pg(2) seq(4) → seq 在 L4+8 = base+28
        uint32_t seq = ((uint32_t)buf[base + 28] << 24) | ((uint32_t)buf[base + 29] << 16) |
                       ((uint32_t)buf[base + 30] << 8) | buf[base + 31];
        f.RecordTx(seq, payload, now);
        return;
    }

    // ==================== 发送方：长链路出队统计 ====================
    if (!isLongLink)
        return;

    // 从包 buffer 解析 UDP 四元组（PPP + IPv4 + UDP）
    const uint8_t *buf = p->GetBuffer();
    uint32_t base = PppHeader::GetStaticSize();
    if (buf[base + 9] != 0x11)
        return; // 非 UDP 数据包（ACK/NACK/PFC 跳过）

    uint32_t sip = ((uint32_t)buf[base + 12] << 24) | ((uint32_t)buf[base + 13] << 16) |
                   ((uint32_t)buf[base + 14] << 8) | buf[base + 15];
    uint32_t dip = ((uint32_t)buf[base + 16] << 24) | ((uint32_t)buf[base + 17] << 16) |
                   ((uint32_t)buf[base + 18] << 8) | buf[base + 19];
    uint16_t sport = (uint16_t)((buf[base + 20] << 8) | buf[base + 21]);
    uint16_t dport = (uint16_t)((buf[base + 22] << 8) | buf[base + 23]);

    DciExtFlowKey key(sip, dip, sport, dport);
    auto it = m_sndFlows.find(key);
    if (it == m_sndFlows.end())
        return; // 非受管流
    DciExtSndFlow &f = it->second;

    uint32_t payload = p->GetSize() - CustomHeader::GetStaticWholeHeaderSize();
    Time now = Simulator::Now();
    f.lastActive = now;

    // 表A 发送字节累计 + Inflight dataSent 累计
    DciExtSndDip &e = m_sndDips[dip];
    e.dataSentBytes += payload;
    if (!e.hasGrant)
        f.startupBytes += payload;

    // qbbHeader: sport(2) dport(2) flags(2) pg(2) seq(4) → seq 在 L4+8 = base+28
    uint32_t seq = ((uint32_t)buf[base + 28] << 24) | ((uint32_t)buf[base + 29] << 16) |
                   ((uint32_t)buf[base + 30] << 8) | buf[base + 31];
    f.dataArrival[seq] = now; // 重传覆盖旧时间戳（RTT 归属最新发送）

    // 观察期状态机推进
    if (f.state == DciExtSndFlow::OBSERVE) {
        f.observeBytes += payload;
        if (f.observeBytes >= m_observeBytes) {
            // 与 dcix 实现一致：两个学费 CNP，间隔 m_burstInterval；
            // 学费 burst 完成后进入固定静默期，后续按固定表首调。
            if (m_debug) {
                printf("[DCIEXT_CNP] %lu node=%u type=TUITION n=2 sip=0x%08x sport=%u dport=%u "
                       "observe_mib=%.1f\n",
                       now.GetTimeStep(), m_nodeId, f.sip, f.sport, f.dport,
                       (double)f.observeBytes / 1048576.0);
            }
            f.state = DciExtSndFlow::INIT_CNP;
            f.burstRemaining = 2;
            BurstTick(key);
        }
    } else if (f.state == DciExtSndFlow::SILENT && now >= f.silentUntil) {
        // 静默期满：比较当前 R_snd 与表A R_avbl，查表首次压制 → 转入受控
        const Time elapsed = now - (f.silentUntil - m_silentUs);
        const uint64_t bytes = e.hasGrant ? e.dataSentBytes - f.dipBytesAtSilence : f.startupBytes;
        const double sndRateNow = elapsed.IsStrictlyPositive()
                                      ? bytes * 8.0 / elapsed.GetSeconds() : 0.0;
        f.state = DciExtSndFlow::CTL;
        const double target = e.hasGrant ? e.rAvblBps : InitialFlowRateBps();
        if (sndRateNow > target && target > 0 && f.burstRemaining == 0) {
            double ratio = target / sndRateNow;
            uint32_t n = LookupN(ratio);
            if (n == 0)
                n = 1;
            f.burstRemaining = n;
            if (m_debug) {
                printf("[DCIEXT_CNP] %lu node=%u type=FIRST_CTL n=%u sport=%u ratio=%.3f R_snd=%.3fG P=%.3fG bytes=%lu dt_ns=%ld scope=%s\n",
                       now.GetTimeStep(), m_nodeId, n, f.sport, ratio, sndRateNow / 1e9,
                       target / 1e9, (unsigned long)bytes, (long)elapsed.GetNanoSeconds(),
                       e.hasGrant ? "dip" : "flow");
            }
            BurstTick(key);
        }

        // 发送方周期维持任务：以首调时刻为起点，每 T 结算 R_snd 并压制超配
        //（首个流进入 CTL 时启动一次；后续新流首调不再重复启动）
        if (m_sndTickEvent.IsExpired() || !m_sndTickEvent.IsRunning())
            m_sndTickEvent = Simulator::Schedule(m_periodT, &DciExt::SndTick, this);
    }
}

// 沿用原配额调整、反馈触发、周期控制及 burst 后重新测量。
double DciExt::ComputeV(const DciExtSndDip &e, double quotaBps) {
    if (e.rttCrossMaxUs <= 0)
        return quotaBps;
    return quotaBps * 0.9;
}

void DciExt::SndTick() {
    if (!m_senderSide)
        return;
    Time now = Simulator::Now();
    for (auto& [dip, e] : m_sndDips) {
        e.sndRateBps = e.timerWindow.Settle(now, e.dataSentBytes);
        const double quota = e.hasGrant ? e.rAvblBps : InitialFlowRateBps();
        const double target = ComputeV(e, quota);
        for (auto& [key, f] : m_sndFlows) {
            if (f.dip != dip || f.state != DciExtSndFlow::CTL || f.burstRemaining > 0)
                continue;
            const double measured = e.hasGrant ? e.sndRateBps
                                               : f.startupWindow.Settle(now, f.startupBytes);
            if (target <= 0 || measured <= target)
                continue;
            const double ratio = target / measured;
            f.burstRemaining = LookupN(ratio);
            if (m_debug) {
                printf("[DCIEXT_CNP] %lu node=%u type=HOLD n=%u dip=0x%08x "
                       "R_snd=%.3fG P=%.3fG v=%.3fG infl=%.1fMB rttX=%.1fus ratio=%.3f "
                       "sport=%u scope=%s\n",
                       now.GetTimeStep(), m_nodeId, f.burstRemaining, dip, measured / 1e9,
                       quota / 1e9, target / 1e9, (e.dataSentBytes - e.ackedBytes) / 1048576.0,
                       e.rttCrossMaxUs, ratio, f.sport, e.hasGrant ? "dip" : "flow");
            }
            BurstTick(key);
        }
    }
    m_sndTickEvent = Simulator::Schedule(m_periodT, &DciExt::SndTick, this);
}

// ---------------------------------------------------------------------------
// 接收方周期 T：RTT 驱动可用带宽探测 + 0xFA 反馈
//   1. 流老化：lastActive 距今 > agingPeriods×T 的流移出，保留累计 ACK 水位
//   2. 聚合 per DIP：N、ackRate（winAckBytes×8/T）、txRate（winTxBytes×8/T，
//      DCI→DC 出队口径）、纯DC内RTT样本（出队打点 -> ACK 回到差分）
//   3. RTT 探测状态机（RTT_base = 运行最小值；RTT_t = RTT_base + Dt）：
//      a. 首次获取 → R_avbl = 首窗活跃时长归一的 ACK 速率；
//         |RTT − RTT_t| ≤ σ → R_avbl = max(当前值, ACK 速率)
//      b. RTT > RTT_t + σ（排队超预期）→ R_avbl = min(ACK 速率, 当前值)，只降不升
//      c. RTT < RTT_t − σ（队列低于目标）→ 增速：
//         Wt = (W/RTT) × RTT_t，R_avbl = max(当前值, Wt/T)，只升不降
//         （W=上个T DCI发给DC的字节数；R_avbl ≈ txRate × RTT_t/RTT，clamp 到 B）
//   4~5. R_avbl 保持 DIP 级总量语义（分支 a = ACK 总速率，分支 c = 公式结果）。
//      分配：每流份额 = R_avbl / N（N 为活跃跨域流数，直接除以 N，不再上取 2 的幂）；
//      表二：每 SenderDC 份额 = 每流份额 × 该DC流数
//      （例：R_avbl=24G，N=3（DC A 2 条 + DC B 1 条）→ A=16G，B=8G）
//      （每流下限兜底 1Gbps，防 ACK 断流期反馈 0 配额）
//   6. 0xFA 发往每组（目的 IP = 该组活跃流的 sip）
// ---------------------------------------------------------------------------
void DciExt::PeriodicTick() {
    Time now = Simulator::Now();

    // 1. 老化：移出活动流计数，累计确认水位保留到恢复或实例销毁。
    Time aging = m_agingPeriods * m_periodT;
    for (auto it = m_rcvFlows.begin(); it != m_rcvFlows.end();) {
        if (now - it->second.lastActive > aging) {
            m_rcvWatermarks[it->first] = {it->second.ackWatermark, it->second.txWatermark};
            it = m_rcvFlows.erase(it);
        } else
            ++it;
    }

    // 2. 聚合（周期快照，从流表构建：含 txRate/RTT 桶）
    struct Agg {
        uint32_t n = 0;
        uint64_t ackBytes = 0;
        uint64_t txBytes = 0;   // 上个T DCI→DC 出队字节（W）
        Time firstAckT = Seconds(-1); // 本窗各流最早的首个 ACK 时刻
        uint32_t rttCnt = 0;
        double rttSumUs = 0;
        double rttMinUs = 1e18;
        double rttMaxUs = 0;
        struct Sender {
            uint32_t count = 0;
            uint32_t sip = 0;
            uint32_t wanPort = 0;
        };
        // All parallel WAN ports to one sender DCI share its per-DIP quota.
        std::map<uint32_t, Sender> senders; // peer DCI node -> aggregate (table two)
    };
    std::map<uint32_t, Agg> aggs;
    double tSec = m_periodT.GetSeconds();
    if (tSec <= 0)
        tSec = 1e-9;
    for (auto &kv : m_rcvFlows) {
        DciExtRcvFlow &f = kv.second;
        Agg &a = aggs[f.dip];
        a.n++;
        a.ackBytes += f.winAckBytes;
        a.txBytes += f.winTxBytes;
        if (f.winAckBytes > 0 && f.winFirstAckT >= Seconds(0) &&
            (a.firstAckT < Seconds(0) || f.winFirstAckT < a.firstAckT))
            a.firstAckT = f.winFirstAckT;
        if (f.rttCnt > 0) {
            a.rttCnt += f.rttCnt;
            a.rttSumUs += f.rttSumUs;
            if (f.rttMinUs < a.rttMinUs) a.rttMinUs = f.rttMinUs;
            if (f.rttMaxUs > a.rttMaxUs) a.rttMaxUs = f.rttMaxUs;
        }
        NS_ABORT_MSG_IF(m_getDevCb.IsNull(), "DCI-Ext needs WAN devices to identify sender DCs");
        auto inputDev = m_getDevCb(f.inPort);
        NS_ABORT_MSG_IF(!IsLongLinkPort(inputDev), "DCI-Ext flow has no WAN ingress");
        auto channel = inputDev->GetChannel();
        NS_ABORT_MSG_IF(!channel || channel->GetNDevices() != 2,
                        "DCI-Ext requires point-to-point WAN links");
        auto peer = channel->GetDevice(0) == inputDev ? channel->GetDevice(1) : channel->GetDevice(0);
        auto& sender = a.senders[peer->GetNode()->GetId()];
        if (sender.count++ == 0) {
            sender.sip = f.sip;
            sender.wanPort = f.inPort;
        }
        f.winAckBytes = 0; // 周期清零
        f.winFirstAckT = Seconds(-1);
        f.winTxBytes = 0;
        f.rttCnt = 0;
        f.rttSumUs = 0;
        f.rttMinUs = 1e18;
        f.rttMaxUs = 0;
    }

    // 3~6. RTT 探测 + 分配 + 反馈
    for (auto &kv : aggs) {
        uint32_t dip = kv.first;
        Agg &a = kv.second;
        double ackRateBps = (double)a.ackBytes * 8.0 / tSec;
        double txRateBps = (double)a.txBytes * 8.0 / tSec;
        double rttAvgUs = (a.rttCnt > 0) ? (a.rttSumUs / a.rttCnt) : -1.0;

        DciExtRcvDip &rd = m_rcvDips[dip];

        // 统计失真防护：ACK 返回速率过低（< 1Gbps，断流/恢复瞬态）时本 T 的
        // R_avbl 暂时保持；RTT_base 仍可使用可靠的逐包时序样本更新。
        // （PFC 振荡是 DC 内拥塞管理常态，振荡期间 ACK 正常流动、统计可信，不冻结）
        bool stalled = ackRateBps < kDciExtMinRateBps;

        // 仅由无重传歧义的正 RTT 更新运行最小值。PFC 等待只能使样本变大，
        // 不应按 20us 或 ACK 速率否决真实的短路径/低速样本。
        if (a.rttCnt > 0 && a.rttMinUs < rd.rttBaseUs) {
            rd.rttBaseUs = a.rttMinUs;
            rd.hasRttBase = true;
        }

        const char *branch = "HOLD";
        if (stalled) {
            branch = "STALL_HOLD";
        } else if (!rd.hasRAvbl) {
            // a. 首次获取 DIP 可用带宽：直接用 ACK 返回速率
            if (ackRateBps > 0) {
                // 首窗 ACK 只占窗口尾部（数据经 WAN 到达时刻与窗口边界无对齐），
                // 按"首个 ACK 到达 → 本窗结算"的活跃时长归一，消除窗口相位稀释
                double activeSec = (a.firstAckT >= Seconds(0))
                                       ? (now - a.firstAckT).GetSeconds() : tSec;
                if (activeSec <= 0 || activeSec > tSec)
                    activeSec = tSec; // 防御：异常时退回整窗口径
                double firstRateBps = (double)a.ackBytes * 8.0 / activeSec;
                if (firstRateBps > m_dipBwBps)
                    firstRateBps = m_dipBwBps; // clamp 到 DIP 带宽上界
                rd.rAvblBps = firstRateBps; // DIP 级总量：首窗活跃时长归一
                rd.hasRAvbl = true;
                branch = "FIRST_ACK";
            } else {
                branch = "WAIT"; // 还没有 ACK，不反债（保持 0）
            }
        } else if (rd.hasRttBase && a.rttCnt > 0) {
            double rttT = rd.rttBaseUs + m_dtUs;
            if (rttAvgUs >= rttT - m_sigmaUs && rttAvgUs <= rttT + m_sigmaUs) {
                // ACK 可能集中返回；确认速率不能把配额抬过实际接入带宽。
                rd.rAvblBps = std::min(m_dipBwBps, std::max(rd.rAvblBps, ackRateBps));
                branch = "STEADY";
            } else if (rttAvgUs > rttT + m_sigmaUs) {
                // b. 排队超预期：R_avbl 只降不升——取 ACK 速率与当前值最小
                //   （能走到此处 ackRate >= 1Gbps，非 0；压低后由发送方压制兑住）
                rd.rAvblBps = std::min(rd.rAvblBps, ackRateBps);
                branch = "CONGESTED";
            } else {
                // c. RTT < RTT_t - sigma：队列低于目标，增速探测
                //   Wt = (W/RTT) × RTT_t（已移除 1/D − 1/Dt 项：该项在 D→0 时
                //   发散，导致低 RTT 窗口 R_avbl 直冲 B 顶格，跨域流借 PROBE 抢占）
                if (a.txBytes == 0) {
                    branch = "PROBE_SKIP"; // 本窗无实际发送，W 无数据可判，保持不变
                } else {
                    double RTTs = rttAvgUs * 1e-6;     // 上个T实测RTT（s）
                    double rttTs = rttT * 1e-6;        // RTT_t（s）
                    double W = (double)a.txBytes;      // 上个T DCI→DC 发送量（B）
                    double Wt = (W / RTTs) * rttTs;
                    double rNewBps = Wt / tSec * 8.0; // Wt 为字节 → ×8 转 bps（与 ackRateBps 口径一致）
                    if (rNewBps > m_dipBwBps)
                        rNewBps = m_dipBwBps;      // clamp 到 B
                    if (rNewBps < kDciExtMinRateBps)
                        rNewBps = kDciExtMinRateBps; // 兜底防塌缩
                    rd.rAvblBps = std::max(rd.rAvblBps, rNewBps); // 只升不降
                    branch = "PROBE";
                }
            }
        } else {
            branch = "NO_SAMPLE"; // 尚无 RTT 样本：保持不变
        }

        if (m_debug) {
            printf("[DCIEXT_STATS] %lu node=%u dip=0x%08x N=%u txRate=%.3fG ackRate=%.3fG "
                   "rtt=%s%.1fus min=%s%.1fus max=%s%.1fus base=%s%.1fus rttT=%s%.1fus "
                   "samples=%u branch=%s R_avbl=%.3fG\n",
                   now.GetTimeStep(), m_nodeId, dip, a.n,
                   txRateBps / 1e9, ackRateBps / 1e9,
                   rttAvgUs < 0 ? "-" : "", rttAvgUs < 0 ? 0.0 : rttAvgUs,
                   a.rttCnt > 0 ? "" : "-", a.rttCnt > 0 ? a.rttMinUs : 0.0,
                   a.rttCnt > 0 ? "" : "-", a.rttCnt > 0 ? a.rttMaxUs : 0.0,
                   rd.hasRttBase ? "" : "-", rd.hasRttBase ? rd.rttBaseUs : 0.0,
                   rd.hasRttBase ? "" : "-", rd.hasRttBase ? rd.rttBaseUs + m_dtUs : 0.0,
                   a.rttCnt, branch, rd.rAvblBps / 1e9);
        }

        if (!rd.hasRAvbl) continue; // 尚无有效参考，不发送占位配额

        // 4. 每流份额 = R_avbl / N，N 为活跃跨域流数。
        uint64_t perFlow = (a.n > 0) ? (uint64_t)(rd.rAvblBps / a.n) : 0;
        // 每流配额至少为 1Gbps，为 ACK 断流/恢复阶段保留非零反馈配额。
        if (perFlow < kDciExtMinRateBps)
            perFlow = kDciExtMinRateBps;

        for (const auto& [senderGateway, sender] : a.senders) {
            // 5. 表二：每 SenderDC 分得 (R_avbl/N) × 该DC流数
            // Send one aggregate quota, not one overwriting quota per physical lane.
            double rAvblBps = (double)perFlow * sender.count;
            uint32_t dstIp = sender.sip;
            if (m_debug) {
                printf("[DCIEXT_SEND] %lu node=%u dip=0x%08x sender_port=%u flows=%u rAvbl=%.3fG dst=0x%08x sender_gateway=%u\n",
                       now.GetTimeStep(), m_nodeId, dip, sender.wanPort, sender.count,
                       rAvblBps / 1e9, dstIp, senderGateway);
            }
            SendDipFeedback(dip, dstIp, rAvblBps, sender.wanPort);
        }
    }

    m_periodEvent = Simulator::Schedule(m_periodT, &DciExt::PeriodicTick, this);
}

// ---------------------------------------------------------------------------
// CNP 独立包构造与发送（协议格式与接收端 ACK 一致：PPP + IPv4(0xFC) + qbbHeader(CNP)）
//   发送端收到后走 ReceiveAck -> CNP 位检查 -> cnp_received_mlx（原生 DCQCN 路径）
// ---------------------------------------------------------------------------
void DciExt::SendCnpForFlow(const DciExtFlowKey &key) {
    auto it = m_sndFlows.find(key);
    if (it == m_sndFlows.end())
        return;
    DciExtSndFlow &f = it->second;

    qbbHeader seqh;
    seqh.SetSeq(0);
    seqh.SetPG(f.pg);
    seqh.SetSport(f.dport); // 端口与数据流反转（接收端 ACK 的构造惯例）
    seqh.SetDport(f.sport);
    seqh.SetCnp();

    Ptr<Packet> newp = Create<Packet>(std::max(60 - 14 - 20 - (int)seqh.GetSerializedSize(), 0));
    newp->AddHeader(seqh);

    Ipv4Header head;
    head.SetDestination(Ipv4Address(f.sip));
    head.SetSource(Ipv4Address(f.dip));
    head.SetProtocol(0xFC);
    head.SetTtl(64);
    head.SetPayloadSize(newp->GetSize());
    head.SetIdentification(UniformVariable(0, 65536).GetValue());
    newp->AddHeader(head);

    PppHeader ppp;
    ppp.SetProtocol(0x0021); // EtherToPpp(0x800)
    newp->AddHeader(ppp);

    // q=0 最高优先级（控制报文队列，不受数据队列影响）
    MyPriorityTag pri;
    pri.SetPriority(0);
    newp->AddPacketTag(pri);

    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    newp->PeekHeader(ch);
    if (!m_sendCb.IsNull())
        m_sendCb(newp, ch);
}

// ---------------------------------------------------------------------------
// 0xFA DIP 反馈报文（接收方 -> 发送方，格式与 ACK 同：PPP + IPv4(0xFA) + qbbHeader）
//   sport/dport = DIP 高/低 16 位，seq = R_avbl（Kbps），目的 IP = 对端活跃流 sip
//   发送方 DCI 拦截消费；q0 最高优先级保证反馈不被数据队列延迟
// ---------------------------------------------------------------------------
void DciExt::SendDipFeedback(uint32_t dip, uint32_t dstIp, double rAvblBps, uint32_t wanPort) {
    qbbHeader seqh;
    seqh.SetSeq((uint32_t)(rAvblBps / 1e3)); // Kbps 编码
    seqh.SetPG(0);
    seqh.SetSport((uint16_t)(dip >> 16));    // DIP 高 16 位
    seqh.SetDport((uint16_t)(dip & 0xffff)); // DIP 低 16 位

    Ptr<Packet> newp = Create<Packet>(std::max(60 - 14 - 20 - (int)seqh.GetSerializedSize(), 0));
    newp->AddHeader(seqh);

    Ipv4Header head;
    head.SetDestination(Ipv4Address(dstIp));
    head.SetSource(Ipv4Address(dip));
    head.SetProtocol(0xFA);
    head.SetTtl(64);
    head.SetPayloadSize(newp->GetSize());
    head.SetIdentification(UniformVariable(0, 65536).GetValue());
    newp->AddHeader(head);

    PppHeader ppp;
    ppp.SetProtocol(0x0021);
    newp->AddHeader(ppp);

    MyPriorityTag pri;
    pri.SetPriority(0);
    newp->AddPacketTag(pri);

    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    newp->PeekHeader(ch);
    if (wanPort != 0 && !m_getDevCb.IsNull()) {
        auto device = DynamicCast<QbbNetDevice>(m_getDevCb(wanPort));
        if (device && device->IsWanPort() && device->IsLinkUp()) {
            device->SwitchSend(0, newp, ch);
            return;
        }
    }
    if (!m_sendCb.IsNull())
        m_sendCb(newp, ch);
}

void DciExt::BurstTick(DciExtFlowKey key) {
    auto it = m_sndFlows.find(key);
    if (it == m_sndFlows.end())
        return;
    DciExtSndFlow &f = it->second;
    if ((f.state != DciExtSndFlow::INIT_CNP && f.state != DciExtSndFlow::CTL) ||
        f.burstRemaining == 0)
        return;
    SendCnpForFlow(key);
    if (--f.burstRemaining > 0)
        f.burstEvent = Simulator::Schedule(m_burstInterval, &DciExt::BurstTick, this, key);
    else {
        // burst 结束：学费 burst → 转静默；重置 DIP 结算基准
        auto& dip = m_sndDips.at(f.dip);
        const Time now = Simulator::Now();
        if (f.state == DciExtSndFlow::INIT_CNP) {
            f.state = DciExtSndFlow::SILENT;
            f.silentUntil = now + m_silentUs;
            f.dipBytesAtSilence = dip.dataSentBytes;
            f.startupBytes = 0;
        }
        if (!dip.hasGrant)
            f.startupWindow.Reset(now, f.startupBytes);
        dip.feedbackWindow.Reset(now, dip.dataSentBytes);
        dip.timerWindow.Reset(now, dip.dataSentBytes);
    }
}

} // namespace ns3
