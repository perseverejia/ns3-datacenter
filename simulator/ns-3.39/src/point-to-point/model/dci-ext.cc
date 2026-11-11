#include "dci-ext.h"

#include <cmath>
#include <cstdio>

#include "ns3/custom-priority-tag.h"
#include "ns3/ipv4-header.h"
#include "ns3/ppp-header.h"
#include "ns3/qbb-header.h"
#include "ns3/random-variable.h"

namespace ns3 {

DciExt::DciExt()
    : m_debug(true), m_nodeId(0), m_senderSide(false),
      m_burstInterval(MicroSeconds(4)),
      m_periodT(MicroSeconds(400)),
      m_dipBwBps(40e9), m_observeBytes(1000000ULL),
      m_agingPeriods(2), m_dtUs(4), m_sigmaUs(3) {
    BuildTable();
    m_periodEvent = EventId();
}

void DciExt::Configure(uint32_t nodeId, uint32_t burstIntervalUs, double periodTUs,
                       double dipBwGbps, uint64_t observeMb,
                       uint32_t agingPeriods, double dtUs, double sigmaUs) {
    m_nodeId = nodeId;
    m_burstInterval = MicroSeconds(burstIntervalUs);
    m_periodT = MicroSeconds(periodTUs);
    m_dipBwBps = dipBwGbps * 1e9;
    m_observeBytes = observeMb * 1000000ULL;
    m_agingPeriods = agingPeriods;
    m_dtUs = dtUs;
    m_sigmaUs = sigmaUs;

    // 接收侧按 T 反馈；发送侧在首个流完成 CNP 初始化后启动 T 定时器。
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

void DciExt::BuildTable() {
    // 原方案固定乘子表：在 DciExt 构造时初始化一次，之后只查表。
    // CNP 每 4us 一个（与刀消费 4us 对齐）；α 每 1us 更新：
    //   到达的 1us  α <- (1-g)α + g，随后 3us x(1-g)^3；
    // 每 4us 末消费一刀 m *= (1 - α/2)（单刀约 -10%，n 刀合成见表）
    const double g = 1.0 / 256.0;      // EWMA_GAIN（必须与端侧 config EWMA_GAIN 一致）
    const double alpha0 = 0.2;         // 原方案固定查表起点
    m_M[0] = 1.0;
    double alpha = alpha0;
    double m = 1.0;
    for (uint32_t n = 1; n <= MAX_BURST_N; n++) {
        alpha = (1.0 - g) * alpha + g;           // CNP 到达的 1us
        for (int k = 0; k < 3; k++)
            alpha *= (1.0 - g);                  // 随后 3us 衰减
        m *= (1.0 - alpha / 2.0);                // 4us 末消费一刀
        m_M[n] = m;
    }
}

// 目标压制比例 r（希望速率变为原来的 r 倍）-> 最小 n 使 M[n] <= r
uint32_t DciExt::LookupN(double ratio) const {
    if (ratio >= 1.0)
        return 0;
    // n 上限 80：极端 ratio（<0.1）时查表会给到百级连发，端侧刀链每 4us
    // 消费一次仍会连砍 20+ 刀直接砍崩——保险丝限幅
    const uint32_t maxN = 80;
    for (uint32_t n = 0; n <= maxN; n++) {
        if (m_M[n] <= ratio)
            return n;
    }
    return maxN;
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
//   - 长链路数据包：建流表（inPort = SenderDC 标识）
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
            Time now = Simulator::Now();
            double dt = (now - e.lastSettle).GetSeconds();
            if (e.lastSettle > Seconds(0) && dt > 0)
                e.sndRateBps = (double)e.sndBytes * 8.0 / dt; // R_snd 结算对齐反馈节拍
            e.sndBytes = 0;
            e.lastSettle = now;

            if (m_debug) {
                printf("[DCIEXT_FB] %lu node=%u dip=0x%08x rAvbl=%.3fG R_snd=%.3fG sndBytes_reset=%lu\n",
                       now.GetTimeStep(), m_nodeId, dip,
                       e.rAvblBps / 1e9, e.sndRateBps / 1e9, (unsigned long)(e.sndBytes));
            }

            // BDP 管道填充控制：v = (P×(T+RTT_cross) − Inflight)/T，ratio = v/R_snd
            //   替代原 R_avbl/R_snd——把“速率配额”改为“管道水位缺口折算的本周期应发速率”
            if (e.rAvblBps > 0 && e.sndRateBps > 0) {
                double vBps = ComputeV(e, m_periodT.GetSeconds());
                // v > P 用 v；v < P 时取 max(v, 0.9P)：压制带宽下限贴 P 的九折，让跨域流速贴近均分线
                double effBps = (vBps > e.rAvblBps) ? vBps : std::max(vBps, 0.9 * e.rAvblBps);
                if (e.sndRateBps > effBps) {
                    double ratio = effBps / e.sndRateBps;
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
                    for (auto &kv : m_sndFlows) {
                        DciExtSndFlow &f = kv.second;
                        if (f.dip != dip || f.state != DciExtSndFlow::CTL)
                            continue;           // 观察/静默期流不受本轮压制
                        if (f.burstRemaining > 0)
                            continue;           // 已有 burst 在发，避免并发
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
                    SendCnpForFlow(key);
                }

                if (isNew) {
                    // 新流：观察 -> CNP初始化 -> T周期查表控制
                    DciExtSndFlow &f = m_sndFlows[key];
                    f.sip = ch.sip;
                    f.dip = ch.dip;
                    f.sport = ch.udp.sport;
                    f.dport = ch.udp.dport;
                    f.pg = (uint8_t)ch.udp.pg;
                    f.state = DciExtSndFlow::OBSERVE;
                    f.observeBytes = 0;
                    f.lastActive = Simulator::Now();

                    // 表A 无此 DIP：初始可用带宽 = DIP 带宽 B >> 2（1/4，即 40G/4 = 10G）
                    // 注：不用长链路线速（100G/4=25G）——R_avbl 语义是"该 DIP 可分得的带宽"，
                    // 上界由 B 约束；首反馈到达前用保守初值，避免首查表压制不足
                    auto dit = m_sndDips.find(ch.dip);
                    if (dit == m_sndDips.end()) {
                        DciExtSndDip &e = m_sndDips[ch.dip];
                        e.rAvblBps = (double)((uint64_t)m_dipBwBps >> 2);
                        e.sndBytes = 0;
                        e.sndRateBps = 0;
                        e.lastSettle = Simulator::Now();
                        if (m_debug) {
                            printf("[DCIEXT_NEW_DIP] %lu node=%u dip=0x%08x init_rAvbl=%.3fG (dipBw/4)\n",
                                   Simulator::Now().GetTimeStep(), m_nodeId, ch.dip, e.rAvblBps / 1e9);
                        }
                    }
                }
            }
        }
        return false; // 数据包照常转发（ECN 已被清除）
    }

    // ==================== 接收方 DCI ====================
    if (fromLongLink && ch.l3Prot == 0x11) {
        // 正向数据包从长链路到达：建流表（受管流集合 + SenderDC 分组依据）
        DciExtFlowKey key(ch.sip, ch.dip, ch.udp.sport, ch.udp.dport);
        auto inserted = m_rcvFlows.try_emplace(key);
        DciExtRcvFlow &f = inserted.first->second;
        if (inserted.second) {
            auto saved = m_rcvAckWatermarks.find(key);
            if (saved != m_rcvAckWatermarks.end()) {
                f.ackWatermark = saved->second;
                m_rcvAckWatermarks.erase(saved);
            }
        }
        f.sip = ch.sip;
        f.dip = ch.dip;
        f.sport = ch.udp.sport;
        f.dport = ch.udp.dport;
        f.pg = (uint8_t)ch.udp.pg;
        f.inPort = device->GetIfIndex(); // 长链路入端口 = SenderDC 标识
        f.lastActive = Simulator::Now();
        return false;
    }

    // CNP 生成权垄断（接收侧·独立帧）：host4 生成的独立 CNP 帧（l3Prot=0xFF，
    // 区别于 ACK 搭载位——后者已在下方剥除）不允许穿透长链路。它一旦到达跨域
    // 发送方会与 0xFA 配额形成双通道压制，并与域内 DCQCN 恢复周期耦合共振
    // （实测：跨域锯齿/82ms 断流/低位均衡）。跨域流拥塞信号唯一通道 = 0xFA 配额。
    // 本 DC 域内流（host4→host5/6）的 CNP 不经过 SW22，不受影响。
    if (!fromLongLink && ch.l3Prot == 0xFF) {
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
            auto saved = m_rcvAckWatermarks.find(key);
            if (saved != m_rcvAckWatermarks.end() && ch.ack.seq > saved->second)
                saved->second = ch.ack.seq;
            return false; // 非活动受管流，原样放行
        }
        DciExtRcvFlow &f = it->second;
        f.lastActive = Simulator::Now();

        // ACK 序号差分累计（乱序期重复 NACK 序号相同 -> 增量 0，自动幂等）
        uint32_t ackSeq = ch.ack.seq;
        if (ackSeq > f.ackWatermark) {
            f.winAckBytes += (uint64_t)(ackSeq - f.ackWatermark);
            f.ackWatermark = ackSeq;

            // RTT 采样：ACK.seq = ReceiverNextExpectedSeq（下一个期望字节号），
            // 确认了所有 seq < ackSeq 的在途包——取 dataArrival 中最后一个
            // seq < ackSeq 且未消费的项（最近被确认的数据包，含 m_ack_interval
            // 里程碑多包一 ACK 的情况），其出队->ACK到达差即纯 DC 内 RTT。
            // 重复 NACK 不越水位不采样，重传重打时间戳，天然幂等。
            auto ub = f.dataArrival.lower_bound(ackSeq);
            if (ub != f.dataArrival.begin()) {
                --ub; // 最后一个 seq < ackSeq 的项 = 本 ACK 新确认的包
                double rttUs = (double)(Simulator::Now() - ub->second).GetTimeStep() / 1000.0;
                f.rttCnt++;
                f.rttSumUs += rttUs;
                if (rttUs < f.rttMinUs) f.rttMinUs = rttUs;
                if (rttUs > f.rttMaxUs) f.rttMaxUs = rttUs;
                f.dataArrival.erase(f.dataArrival.begin(), std::next(ub)); // 仅清理 seq < ackSeq
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// 挂钩点 2：出队（SwitchNotifyDequeue）
//   发送方 DCI：受管流数据包从长链路端口出队时
//     表A：sndBytes 累计（R_snd 口径 = 实际出长链路的 payload 字节）
//     流表：OBSERVE -> INIT_CNP -> CTL
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
        f.dataArrival[seq] = now; // 重传覆盖旧时间戳（RTT 归属最新发送）
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
    e.sndBytes += payload;
    e.dataSentBytes += payload;

    // qbbHeader: sport(2) dport(2) flags(2) pg(2) seq(4) → seq 在 L4+8 = base+28
    uint32_t seq = ((uint32_t)buf[base + 28] << 24) | ((uint32_t)buf[base + 29] << 16) |
                   ((uint32_t)buf[base + 30] << 8) | buf[base + 31];
    f.dataArrival[seq] = now; // 重传覆盖旧时间戳（RTT 归属最新发送）

    if (f.state == DciExtSndFlow::OBSERVE) {
        f.observeBytes += payload;
        if (f.observeBytes >= m_observeBytes) {
            f.state = DciExtSndFlow::INIT_CNP;
            InitCnpTick(key);
        }
    }
}

// 沿用原配额调整、反馈触发、周期控制及 burst 后重新测量。
double DciExt::ComputeV(DciExtSndDip &e, double tSec) {
    if (e.rttCrossMaxUs <= 0)
        return e.rAvblBps;
    return e.rAvblBps * 0.9;
}

void DciExt::SndTick() {
    if (!m_senderSide)
        return;
    Time now = Simulator::Now();
    for (auto &kv : m_sndDips) {
        DciExtSndDip &e = kv.second;
        double dt = (now - e.lastSettle).GetSeconds();
        if (e.lastSettle > Seconds(0) && dt > 0)
            e.sndRateBps = (double)e.sndBytes * 8.0 / dt;
        e.sndBytes = 0;
        e.lastSettle = now;

        if (e.rAvblBps > 0 && e.sndRateBps > 0) {
            // BDP 管道填充控制（与 0xFA OVERLOAD 同式）：v vs R_snd 驱动查表连发
            double vBps = ComputeV(e, m_periodT.GetSeconds());
            // v > P 用 v；v < P 时取 max(v, 0.9P)：压制带宽下限贴 P 的九折，让跨域流速贴近均分线
            double effBps = (vBps > e.rAvblBps) ? vBps : std::max(vBps, 0.9 * e.rAvblBps);
            if (e.sndRateBps > effBps) {
                double ratio = effBps / e.sndRateBps;
                uint32_t n = LookupN(ratio);
                if (n == 0)
                    n = 1;
                if (m_debug) {
                    printf("[DCIEXT_CNP] %lu node=%u type=HOLD n=%u dip=0x%08x "
                           "R_snd=%.3fG P=%.3fG v=%.3fG infl=%.1fMB rttX=%.1fus ratio=%.3f\n",
                           now.GetTimeStep(), m_nodeId, n, kv.first,
                           e.sndRateBps / 1e9, e.rAvblBps / 1e9, vBps / 1e9,
                           (double)(e.dataSentBytes - e.ackedBytes) / 1048576.0,
                           e.rttCrossMaxUs, ratio);
                }
                for (auto &fkv : m_sndFlows) {
                    DciExtSndFlow &f = fkv.second;
                    if (f.dip != kv.first || f.state != DciExtSndFlow::CTL)
                        continue;
                    if (f.burstRemaining > 0)
                        continue;
                    f.burstRemaining = n;
                    BurstTick(fkv.first);
                }
            }
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
//      a. 首次获取 → R_avbl = ACK 速率；|RTT − RTT_t| ≤ σ → R_avbl = ACK 速率
//         （限制：上一周期若为 CONGESTED，本周期不得立即用 ACK 抬回，保持不变）
//      b. RTT > RTT_t + σ（排队超预期）→ R_avbl = min(ACK 速率, 当前值)，只降不升
//      c. RTT < RTT_t − σ（队列低于目标）→ 增速：
//         Wt = (1/D − 1/Dt + W/RTT) × RTT_t，R_avbl = max(当前值, Wt/T)，只升不降
//         （W=上个T DCI发给DC的字节数，D=RTT−RTT_base；主导项 W/RTT×RTT_t
//           使 R_avbl ≈ txRate × RTT_t/RTT，即目标延迟比侧速率控制；
//           1/D、1/Dt 两项分子为 1 字节量级，仅作微调；clamp 到 B）
//   4~5. R_avbl 保持 DIP 级总量语义（分支 a = ACK 总速率，分支 c = 公式结果）。
//      分配：每流份额 = R_avbl / ceilPow2(N)（N=2 → /2；N=3 → 取 4）；
//      表二：每 SenderDC 份额 = 每流份额 × 该DC流数
//      （例：R_avbl=25.6G，N=3（DC A 2 条 + DC B 1 条）→ A = 25.6/4×2 = 12.8G，B = 25.6/4 = 6.4G）
//      （每流下限兜底 B/64，防 ACK 断流期反馈 0 配额死锁）
//   6. 0xFA 发往每组（目的 IP = 该组活跃流的 sip）
// ---------------------------------------------------------------------------
void DciExt::PeriodicTick() {
    Time now = Simulator::Now();

    // 1. 老化：移出活动流计数，累计确认水位保留到恢复或实例销毁。
    Time aging = m_agingPeriods * m_periodT;
    for (auto it = m_rcvFlows.begin(); it != m_rcvFlows.end();) {
        if (now - it->second.lastActive > aging) {
            m_rcvAckWatermarks[it->first] = it->second.ackWatermark;
            it = m_rcvFlows.erase(it);
        } else
            ++it;
    }

    // 2. 聚合（周期快照，从流表构建：含 txRate/RTT 桶）
    struct Agg {
        uint32_t n = 0;
        uint64_t ackBytes = 0;
        uint64_t txBytes = 0;   // 上个T DCI→DC 出队字节（W）
        uint32_t rttCnt = 0;
        double rttSumUs = 0;
        double rttMinUs = 1e18;
        double rttMaxUs = 0;
        std::map<uint32_t, uint32_t> senderCounts; // inPort -> 流数（表二）
        std::map<uint32_t, uint32_t> senderSip;    // inPort -> 代表 sip（0xFA 目的地址）
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
        if (f.rttCnt > 0) {
            a.rttCnt += f.rttCnt;
            a.rttSumUs += f.rttSumUs;
            if (f.rttMinUs < a.rttMinUs) a.rttMinUs = f.rttMinUs;
            if (f.rttMaxUs > a.rttMaxUs) a.rttMaxUs = f.rttMaxUs;
        }
        a.senderCounts[f.inPort]++;
        if (a.senderSip[f.inPort] == 0)
            a.senderSip[f.inPort] = f.sip;
        f.winAckBytes = 0; // 周期清零
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

        // 统计失真防护：ACK 返回速率过低（< B/10，断流/恢复瞬态）时本 T 的
        // ACK/RTT 统计不可信，R_avbl 与 RTT_base 全部保持不变。
        // （PFC 振荡是 DC 内拥塞管理常态，振荡期间 ACK 正常流动、统计可信，不冻结）
        bool stalled = ackRateBps < m_dipBwBps / 10.0;

        // RTT_base = 运行最小值（首个样本初始化，之后只降不升）
        if (!stalled && a.rttCnt > 0 && a.rttMinUs < rd.rttBaseUs) {
            rd.rttBaseUs = a.rttMinUs;
            rd.hasRttBase = true;
        }

        const char *branch = "HOLD";
        if (stalled) {
            branch = "STALL_HOLD";
        } else if (!rd.hasRAvbl) {
            // a. 首次获取 DIP 可用带宽：直接用 ACK 返回速率
            if (ackRateBps > 0) {
                rd.rAvblBps = ackRateBps; // DIP 级总量：ACK 总速率
                rd.hasRAvbl = true;
                branch = "FIRST_ACK";
            } else {
                branch = "WAIT"; // 还没有 ACK，不反债（保持 0）
            }
        } else if (rd.hasRttBase && a.rttCnt > 0) {
            double rttT = rd.rttBaseUs + m_dtUs;
            if (rttAvgUs >= rttT - m_sigmaUs && rttAvgUs <= rttT + m_sigmaUs) {
                // a. RTT 在目标区间：ACK 速率即真实可用带宽；但上一周期若为
                //    CONGESTED（排队超预期），压制刚生效，不得立即用 ACK 抬回
                //    更新取 max(ACK 速率, 当前值)：区间内 R_avbl 只升不降
                if (!rd.lastCongested) {
                    if (ackRateBps > 0) {
                        rd.rAvblBps = std::max(rd.rAvblBps, ackRateBps);
                        branch = "STEADY";
                    } else {
                        branch = "STEADY_ACK0"; // ACK 断流，保持不变
                    }
                } else {
                    branch = "STEADY_HOLD"; // 上周期过拥：保持不变，防止回弹
                }
            } else if (rttAvgUs > rttT + m_sigmaUs) {
                // b. 排队超预期：R_avbl 只降不升——取 ACK 速率与当前值最小
                //   （能走到此处 ackRate >= B/10，非 0；压低后由发送方压制兑住）
                rd.rAvblBps = std::min(rd.rAvblBps, ackRateBps);
                branch = "CONGESTED";
            } else {
                // c. RTT < RTT_t - sigma：队列低于目标，增速探测
                //   Wt = (1/D − 1/Dt + W/RTT) × RTT_t；R_avbl = Wt/T
                if (a.txBytes == 0) {
                    branch = "PROBE_SKIP"; // 本窗无实际发送，W 无数据可判，保持不变
                } else {
                double dUs = rttAvgUs - rd.rttBaseUs;
                if (dUs < 1e-3)
                    dUs = 1e-3; // 防除零（RTT 恰为历史最小值）
                double Ds = dUs * 1e-6;           // D（s）
                double dtS = m_dtUs * 1e-6;        // Dt（s）
                double RTTs = rttAvgUs * 1e-6;     // 上个T实测RTT（s）
                double rttTs = rttT * 1e-6;        // RTT_t（s）
                double W = (double)a.txBytes;      // 上个T DCI→DC 发送量（B）
                double Wt = (1.0 / Ds - 1.0 / dtS + W / RTTs) * rttTs;
                double rNewBps = Wt / tSec * 8.0; // Wt 为字节 → ×8 转 bps（与 ackRateBps 口径一致）
                if (rNewBps > m_dipBwBps)
                    rNewBps = m_dipBwBps;      // clamp 到 B
                if (rNewBps < (m_dipBwBps / 64.0))
                    rNewBps = m_dipBwBps / 64.0; // 兕底防塌缩
                rd.rAvblBps = std::max(rd.rAvblBps, rNewBps); // 只升不降
                branch = "PROBE";
                }
            }
        } else {
            branch = "NO_SAMPLE"; // 尚无 RTT 样本：保持不变
        }

        // 记录本周期分支，供下周期 STEADY 的回弹限制使用
        // （STALL_HOLD 的 RTT 统计不可信，保持原值不更新）
        if (branch != "STALL_HOLD")
            rd.lastCongested = (branch == "CONGESTED");

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

        // 4. 分配：移位除法，N 上取 2 的幂 P，每流份额 = R_avbl >> log2(P)
        uint32_t p = 1, shift = 0;
        while (p < a.n) {
            p <<= 1;
            shift++;
        }
        uint64_t perFlow = (uint64_t)rd.rAvblBps >> shift;
        // 兜底下限：ACK 差分瞬态归零时不得反馈 0 配额（否则发送方 ratio=0
        // → n=MAX 连发 → 永久死锁）。下限 dipBw/64（625Mbps）只作恢复期兜底
        uint64_t minPerFlow = (uint64_t)m_dipBwBps >> 6;
        if (perFlow < minPerFlow)
            perFlow = minPerFlow;

        for (auto &sc : a.senderCounts) {
            // 5. 表二：每 SenderDC 分得 (R_avbl/ceilPow2(N)) × 该DC流数
            double rAvblBps = (double)perFlow * sc.second;
            uint32_t dstIp = a.senderSip[sc.first];
            if (m_debug) {
                printf("[DCIEXT_SEND] %lu node=%u dip=0x%08x sender_port=%u flows=%u rAvbl=%.3fG dst=0x%08x\n",
                       now.GetTimeStep(), m_nodeId, dip, sc.first, sc.second,
                       rAvblBps / 1e9, dstIp);
            }
            SendDipFeedback(dip, dstIp, rAvblBps);
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
    if (!m_sendCb.IsNull()) {
        if (f.state != DciExtSndFlow::CTL)
            RecordCnp(f.cnp, Simulator::Now().GetNanoSeconds());
        m_sendCb(newp, ch);
    }
}

// ---------------------------------------------------------------------------
// 0xFA DIP 反馈报文（接收方 -> 发送方，格式与 ACK 同：PPP + IPv4(0xFA) + qbbHeader）
//   sport/dport = DIP 高/低 16 位，seq = R_avbl（Kbps），目的 IP = 对端活跃流 sip
//   发送方 DCI 拦截消费；q0 最高优先级保证反馈不被数据队列延迟
// ---------------------------------------------------------------------------
void DciExt::SendDipFeedback(uint32_t dip, uint32_t dstIp, double rAvblBps) {
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
    if (!m_sendCb.IsNull())
        m_sendCb(newp, ch);
}

// 原生参数与已有查表一致：g=1/256，alpha更新1us，降速消费4us。
void DciExt::AdvanceAlpha(DciExtInitCnpState &state, int64_t nowNs) {
    if (!state.initialized || nowNs < state.nextAlphaNs)
        return;
    int64_t slots = (nowNs - state.nextAlphaNs) / 1000 + 1;
    state.alpha *= std::pow(255.0 / 256.0, slots);
    if (state.pending)
        state.alpha += std::pow(255.0 / 256.0, slots - 1) / 256.0;
    state.pending = false;
    state.nextAlphaNs += slots * 1000;
}

void DciExt::RecordCnp(DciExtInitCnpState &state, int64_t nowNs) {
    if (!state.initialized) {
        state.initialized = true;
        state.pending = false;
        state.alpha = 1; // 首个 CNP 的原生行为
        state.nextAlphaNs = nowNs + 1000;
    } else {
        AdvanceAlpha(state, nowNs);
        state.pending = true;
    }
}

void DciExt::InitCnpTick(DciExtFlowKey key) {
    auto it = m_sndFlows.find(key);
    if (it == m_sndFlows.end() || it->second.state != DciExtSndFlow::INIT_CNP)
        return;
    DciExtSndFlow &f = it->second;
    SendCnpForFlow(key);
    NS_ABORT_MSG_IF(!f.cnp.initialized, "CNP initialization requires a send callback");
    auto after = f.cnp;
    AdvanceAlpha(after, after.nextAlphaNs);
    if (std::abs(after.alpha - 0.2) <= 0.002) {
        f.state = DciExtSndFlow::CTL;
        if (m_debug) {
            printf("[DCIEXT_FLOW_CTL] %lu node=%u sport=%u alpha_model=%.6f\n",
                   Simulator::Now().GetTimeStep(), m_nodeId, f.sport, after.alpha);
        }
        if (!m_sndTickEvent.IsRunning()) {
            // 首轮沿用启动阶段已累计的测量；不再等待一个额外 T。
            m_sndTickEvent = Simulator::Schedule(m_burstInterval, &DciExt::SndTick, this);
        }
        return; // 初始化完成，无额外静默/观察阶段
    }
    // 高alpha稀疏发包，靠原生衰减下降；低alpha每2us补一个CNP上升。
    // 最大间隔取原生alpha时间常数的1/4（256us/4），不是新的静默参数。
    int64_t delayNs = after.alpha < 0.2 ? 2000 : 64000;
    if (after.alpha > 0.2) {
        for (int64_t gap = 1000; gap <= 64000; gap += 1000) {
            auto candidate = f.cnp;
            RecordCnp(candidate, Simulator::Now().GetNanoSeconds() + gap);
            AdvanceAlpha(candidate, candidate.nextAlphaNs);
            if (candidate.alpha <= 0.202) {
                delayNs = gap;
                break;
            }
        }
    }
    f.burstEvent = Simulator::Schedule(NanoSeconds(delayNs), &DciExt::InitCnpTick, this, key);
}

void DciExt::BurstTick(DciExtFlowKey key) {
    auto it = m_sndFlows.find(key);
    if (it == m_sndFlows.end())
        return;
    DciExtSndFlow &f = it->second;
    if (f.state != DciExtSndFlow::CTL || f.burstRemaining == 0)
        return;
    SendCnpForFlow(key);
    if (--f.burstRemaining > 0)
        f.burstEvent = Simulator::Schedule(m_burstInterval, &DciExt::BurstTick, this, key);
    else {
        auto dit = m_sndDips.find(f.dip);
        if (dit != m_sndDips.end()) {
            dit->second.sndBytes = 0;
            dit->second.lastSettle = Simulator::Now();
        }
    }
}

} // namespace ns3
