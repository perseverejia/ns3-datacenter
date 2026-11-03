// ============================================================================
// ePFC (ccMode=17) v2 实现：详见 epfc.h 头部说明
// 挂点：SwitchNode::SendToDev（出端口确定后、入队前）
// ============================================================================

#include "epfc.h"
#include "switch-mmu.h"
#include "qbb-header.h"
#include "ppp-header.h"
#include "ns3/ipv4-header.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace ns3 {

Epfc::Epfc()
    : m_debug(true), m_nodeId(0), m_ingressMulPct(100),
      m_egressThresh(256 * 1024), m_dciThresh(256 * 1024),
      m_rttUs(30.0), m_agingRtts(2.0), m_updateTUs(1000.0), m_fixedRvBps(0),
      m_isRxDci(false), m_markCount(0), m_ccfTblCount(0),
      m_xoffCount(0), m_xonCount(0) {
}

void Epfc::Configure(uint32_t nodeId, uint32_t ingressMulPct, uint64_t egressThreshBytes,
                     uint64_t dciThreshBytes, double rttUs, double agingRtts,
                     double updateTUs) {
    m_nodeId = nodeId;
    m_ingressMulPct = ingressMulPct > 0 ? ingressMulPct : 100;
    m_egressThresh = egressThreshBytes;
    m_dciThresh = dciThreshBytes;
    m_rttUs = rttUs > 0 ? rttUs : 30.0;
    m_agingRtts = agingRtts > 0 ? agingRtts : 2.0;
    m_updateTUs = updateTUs > 0 ? updateTUs : 1000.0;
}

void Epfc::SetRxDciNodes(const std::set<uint32_t> &nodes) {
    m_rxDciNodes = nodes;
    m_isRxDci = nodes.count(m_nodeId) > 0;
}

void Epfc::SetMmu(Ptr<SwitchMmu> mmu) {
    m_mmu = mmu;
}

// ---------------------------------------------------------------------------
// 主入口（SendToDev 挂点）
// ---------------------------------------------------------------------------
void Epfc::PacketIn(uint32_t inDev, uint32_t outIdx, uint32_t qIndex,
                    Ptr<Packet> &p, CustomHeader &ch) {
    if (ch.l3Prot == 0xFB) { // CCF 回传包：接收方 DCI 更新流表（不拦截，继续转发）
        CcfUpdateTable(inDev, p, ch);
        return;
    }
    if (ch.l3Prot != 0x11) // 只处理数据包
        return;
    MarkJudge(inDev, outIdx, p, ch); // 角色一：打标（所有交换机）
    DataAccount(outIdx, p, ch);      // 角色三：贡献者流打计量 Tag（仅接收方 DCI 有表）
}

// ---------------------------------------------------------------------------
// 角色一：打标判定（拥塞贡献者识别，所有交换机逐包执行）
//   条件A: 入口队列超 PFC 动态门限×mul 且 出口队列超 egress 阈值
//   条件B: 本节点是接收方 DCI 且 出口队列超 DCI_Th
//   标记 = IPv4 TOS bit7（粘性：已标记的包不再重复处理）
// ---------------------------------------------------------------------------
void Epfc::MarkJudge(uint32_t inDev, uint32_t outIdx, Ptr<Packet> p, CustomHeader &ch) {
    if (ch.m_tos & EPFC_MARK_MASK)
        return; // 已打标（沿途粘性）
    uint8_t pg = ch.udp.pg;
    if (pg == 0)
        return; // 控制流量不打标

    uint64_t ingBytes = m_mmu->ingress_bytes[inDev][pg];
    uint64_t egBytes = m_mmu->egress_bytes[outIdx][pg];
    uint64_t ingThresh = (uint64_t)((double)m_mmu->Threshold(inDev, pg, "ingress", 0, 0) *
                                    (double)m_ingressMulPct / 100.0);
    bool condA = (ingBytes > ingThresh) && (egBytes > m_egressThresh);
    bool condB = m_isRxDci && (egBytes > m_dciThresh);
    if (!(condA || condB))
        return;

    // 打标：剥 PPP+IP → TOS bit7 置位 → 加回（ECN 低 2 位保留，二者正交）
    PppHeader ppp;
    Ipv4Header iph;
    p->RemoveHeader(ppp);
    p->RemoveHeader(iph);
    iph.SetTos(iph.GetTos() | EPFC_MARK_MASK);
    p->AddHeader(iph);
    p->AddHeader(ppp);
    ch.m_tos |= EPFC_MARK_MASK;
    m_markCount++;

    if (m_debug && (m_markCount <= 5 || m_markCount % 100000 == 0)) {
        printf("[EPFC_MARK] %lu node=%u inDev=%u out=%u pg=%u ingBytes=%lu ingThresh=%lu egBytes=%lu egThresh=%lu condA=%d condB=%d cnt=%lu\n",
               Simulator::Now().GetTimeStep(), m_nodeId, inDev, outIdx, pg,
               ingBytes, ingThresh, egBytes,
               condA ? m_egressThresh : m_dciThresh, condA ? 1 : 0, condB ? 1 : 0,
               m_markCount);
    }
}

// ---------------------------------------------------------------------------
// 角色二：CCF 到达 → 接收方 DCI 更新流表（旁路，不拦截转发）
//   CCF = 接收方 host 收到标记包后生成（协议号 0xFB，qbbHeader 格式）：
//     IP 头: src=数据接收方, dst=数据发送方
//     qbbHeader: sport/dport 与数据流反转, pg=数据流 pg
//     payload 前 16B: {rv_bps: u64, ts_ns: u64}
//   流表挂在 CCF 的入端口（= 该流数据从本交换机发出的出口端口）
//   其余交换机只转发不处理
// ---------------------------------------------------------------------------
void Epfc::CcfUpdateTable(uint32_t inDev, Ptr<Packet> &p, CustomHeader &ch) {
    if (!m_isRxDci)
        return; // 非接收方 DCI：只转发，不处理

    // 剥包读 CCF payload: {rv_bps, ts_ns}
    Ptr<Packet> cp = p->Copy();
    PppHeader ppp;
    Ipv4Header iph;
    qbbHeader qh;
    cp->RemoveHeader(ppp);
    cp->RemoveHeader(iph);
    cp->RemoveHeader(qh);
    uint8_t buf[16] = {0};
    cp->CopyData(buf, std::min<size_t>(16, cp->GetSize()));
    uint64_t rvBps = 0, tsNs = 0;
    memcpy(&rvBps, buf, 8);
    memcpy(&tsNs, buf + 8, 8);

    // 更新流表：数据流 key 从 CCF 头反转（CCF 的 sip/dip/sport/dport 均与数据流相反）
    //   更新窗机制：条目以 updateT（默认 1ms）为周期
    //   - 新窗（条目不存在/已失效/超窗）：以本 CCF 的 rv 为基准，令牌桶清零、
    //     补充基准重置，窗起点 ts=now
    //   - 窗内：rv 只取各 CCF 的最小值（防止单个污染 rv 覆盖真值，如排空期的 38G
    //           覆盖公平份额 19.139G），ts/tokens 不动（令牌余额连续演化）
    //   - 超窗：条目失效放行，等下一个 CCF 重建（拥塞缓解后流能重新被放开）
    EpfcFlowKey key(ch.dip, ch.sip, ch.ack.dport, ch.ack.sport);
    Time now = Simulator::Now();
    EpfcFlowEntry &e = m_egressTable[inDev].flows[key];
    bool newWindow = !e.isContributor ||
                     (now - e.ts).GetSeconds() > m_updateTUs * 1e-6;
    if (newWindow) {
        // 固定 rv 测试（v6.9）：表 rv 钉死为固定值，切断采样塌缩变量，
        // 单检验收方 DCI 门控（令牌桶）能否兑现 rv
        e.rvBps = m_fixedRvBps > 0 ? m_fixedRvBps : (double)rvBps;
        e.isContributor = true;
        e.ts = now;
        e.tokens = 0;       // 令牌桶与新窗同起点：新窗立即从零计（此前流在超速）
        e.lastRefill = now; // 惰性补充基准
        e.pg = (uint8_t)ch.ack.pg;
    } else {
        // 固定 rv 测试下窗内 MIN 棘轮旁路（表值不再被低样本拖拽）
        if (m_fixedRvBps <= 0 && (double)rvBps < e.rvBps)
            e.rvBps = (double)rvBps; // 窗内只降不升
    }
    e.lastCcf = now; // 老化判断用（每 CCF 刷新，与窗起点分离）

    // —— rv 改写：用表内净化值覆盖 CCF 携带的 rv，保持 DCI 表与发送方速率一致 ——
    //   host 采样窗(30μs)与门控半周期(30μs)错拍：采样落在 XOFF 期的样本 rv<1G 被
    //   生成保护拦截，只有落在 XON 倾泻期(~38G)的样本能发出来 → 到达此处的 CCF
    //   几乎全是峰值样本（幸存者偏差）。若原样转发，发送方会按 38G 直接设速冲高。
    //   仅当条目有效且原始 rv 高于表值时改写（低值已被 MIN 吸收进表，两者天然一致）。
    //   改写通过重建 payload 实现：Ptr<Packet>& 引用传递，p = np 传播回 SendToDev
    //   的转发路径；payload 无校验和覆盖（IPv4 不算 payload、qbbHeader 仅头部），
    //   改写无需重算校验和。
    bool rewritten = false;
    // 固定 rv 测试：改写改双向——竞争期低样本也改写为固定值，
    // 否则低样本 CCF 会把发送方钉在低位，门控无流量可管，实验失真
    bool needRw = (double)rvBps > e.rvBps ||
                  (m_fixedRvBps > 0 && (double)rvBps != e.rvBps);
    if (e.isContributor && e.rvBps > 0 && needRw) {
        PppHeader rppp;
        Ipv4Header riph;
        qbbHeader rqh;
        p->RemoveHeader(rppp);
        p->RemoveHeader(riph);
        p->RemoveHeader(rqh);
        uint8_t pbuf[16] = {0};
        p->CopyData(pbuf, std::min<size_t>(16, p->GetSize()));
        uint64_t newRv = (uint64_t)e.rvBps;
        memcpy(pbuf, &newRv, 8); // rv_bps 字段（前 8B，host 序与 GEN 侧 memcpy 写入一致）
        Ptr<Packet> np = Create<Packet>(pbuf, 16);
        np->AddHeader(rqh);
        np->AddHeader(riph);
        np->AddHeader(rppp);
        p = np; // 引用传播回转发路径
        rewritten = true;
    }
    m_ccfTblCount++;

    // 惰性启动全局老化 tick
    if (!m_agingEvent.IsRunning()) {
        m_agingEvent = Simulator::Schedule(MicroSeconds(m_rttUs / 2), &Epfc::AgingTick, this);
    }

    if (m_debug && (m_ccfTblCount <= 10 || m_ccfTblCount % 2000 == 0)) {
        printf("[EPFC_CCF_EDGE] %lu node=%u tblPort=%u rv=%.3fG winRv=%.3fG rw=%d newWin=%d ts_ns=%lu sip=0x%08x dip=0x%08x sport=%u dport=%u cnt=%lu\n",
               Simulator::Now().GetTimeStep(), m_nodeId, inDev,
               rvBps / 1e9, e.rvBps / 1e9, rewritten ? 1 : 0, newWindow ? 1 : 0,
               (uint64_t)e.ts.GetNanoSeconds(),
               key.sip, key.dip, key.sport, key.dport, m_ccfTblCount);
    }
}

// ---------------------------------------------------------------------------
// 角色三（入队挂点）：贡献者流包打计量 Tag（不累计）
//   outIdx = 数据包将发出的出口端口 = 表所在端口
//   令牌的实际消耗在出队挂点 DataDequeue：门控中被拦的包不出队不消耗，
//   XON 后倾泻的存量也如实扣减 —— 判据真实反映物理发出量
// ---------------------------------------------------------------------------
void Epfc::DataAccount(uint32_t outIdx, Ptr<Packet> p, CustomHeader &ch) {
    auto pit = m_egressTable.find(outIdx);
    if (pit == m_egressTable.end())
        return; // 本端口无表（非接收方 DCI 或暂无受管流）
    EpfcFlowKey key(ch.sip, ch.dip, ch.udp.sport, ch.udp.dport);
    auto fit = pit->second.flows.find(key);
    if (fit == pit->second.flows.end() || !fit->second.isContributor)
        return;
    // 更新窗已过期：本窗结束，本流放行（不计量，等下个 CCF 重建）
    if ((Simulator::Now() - fit->second.ts).GetSeconds() > m_updateTUs * 1e-6)
        return;
    // 打 Tag，随包穿过端口队列，在实际出队时刻消费（见 DataDequeue）
    EpfcSAccountTag tag;
    tag.SetKey(key);
    p->AddPacketTag(tag);
}

// ---------------------------------------------------------------------------
// 角色三（出队挂点）：实际出队时刻令牌消耗 + 门控评估
//   挂在 QbbNetDevice::DequeueAndTransmit（switch 分支）TransmitStart 前：
//   只有真正要上线的包才到这里 —— 门控中被拦的包不出队、不消耗令牌，
//   XON 后倾泻的存量如实扣减 → 发出量与令牌消耗严格对应，门控真正限流
// ---------------------------------------------------------------------------
void Epfc::DataDequeue(uint32_t outIdx, const EpfcSAccountTag &tag, uint32_t payload) {
    auto pit = m_egressTable.find(outIdx);
    if (pit == m_egressTable.end())
        return;
    auto fit = pit->second.flows.find(tag.GetKey());
    if (fit == pit->second.flows.end() || !fit->second.isContributor)
        return;
    // 更新窗已过期：本窗结束，本流放行
    if ((Simulator::Now() - fit->second.ts).GetSeconds() > m_updateTUs * 1e-6)
        return;
    RefillTokens(fit->second, Simulator::Now()); // 惰性补充至当前时刻
    fit->second.tokens -= (int64_t)payload;       // 消耗：与物理发出一一对应
    EvaluatePort(outIdx);
}

// ---------------------------------------------------------------------------
// 令牌惰性补充（v6.8）：tokens += rv/8 × dt，上限 burst = rv/8×RTT/2
//   burst = 一个 PortTick 周期（RTT/2 = 15μs）的补充量：XOFF 后恰好一个
//   tick 补满 → 周期吞吐恒 = rv。上限保证任何时刻可突发量有界（吸收
//   v6.5 时间配额「XON 信用清零+迟滞」导致的周期性大倾泻问题）。
//   与 credit 窗口的本质区别：补充只看时间，不看 ACK——长期速率 = rv
//   恒成立，与环路 RTT 动态变化无关（v6.7 实测环路仅 7.5μs 的教训）
// ---------------------------------------------------------------------------
void Epfc::RefillTokens(EpfcFlowEntry &e, Time now) {
    double dtSec = (now - e.lastRefill).GetSeconds();
    if (dtSec <= 0)
        return; // 同刻多次评估：无增量
    e.tokens = (int64_t)std::min((double)e.tokens + e.rvBps / 8.0 * dtSec,
                                 TokenBurst(e));
    e.lastRefill = now;
}

// ---------------------------------------------------------------------------
// 端口评估：任一贡献者流令牌透支 → 本地门控（per-pg）；全部回正 → 解除
//   判据（令牌桶模型 v6.8）：tokens < 0 → 信用耗尽仍在超速
//   tokens = 令牌余额：时间补充 rv/8×dt（惰性，上限 burst），出队 −payload。
//     XOFF 后停止出队（令牌不再消耗），时间补充自动回正 → tokens ≥ 0 → XON
//     → kick 出队。长期平均速率 = 补充率 = rv：与环路 RTT、ACK 速率完全
//     解耦（credit 窗口 v6.6/v6.7 的失败根因：等效允许吞吐 = 窗口/环路RTT，
//     环路 RTT 动态且实测仅 7.5μs，rv×15μs 窗口仍放行 38.3G，限不住）
// ---------------------------------------------------------------------------
void Epfc::EvaluatePort(uint32_t port) {
    auto pit = m_egressTable.find(port);
    if (pit == m_egressTable.end())
        return;
    EpfcPortState &ps = pit->second;
    Time now = Simulator::Now();

    bool anyExceed = false;
    std::set<uint8_t> exceedPgs;
    int64_t dbgTokens = 0;
    double dbgBurst = 0.0, dbgRv = 0.0;
    for (auto &kv : ps.flows) {
        EpfcFlowEntry &e = kv.second;
        if (!e.isContributor || e.rvBps <= 0)
            continue;
        // 更新窗过期：本流不再计入透支（放行）
        if ((now - e.ts).GetSeconds() > m_updateTUs * 1e-6)
            continue;
        RefillTokens(e, now); // 惰性补充：评估时刻先结清令牌
        if (e.tokens < 0) {
            anyExceed = true;
            exceedPgs.insert(e.pg);
            if (dbgRv == 0.0) { // 首条透支流（日志用）
                dbgTokens = e.tokens;
                dbgBurst = TokenBurst(e);
                dbgRv = e.rvBps;
            }
        }
    }

    if (anyExceed && !ps.gated) {
        ps.gated = true;
        ps.gatedPgs = exceedPgs;
        for (uint8_t pg : ps.gatedPgs)
            SendGate(port, pg, true);
        // 兜底定时器（时间补充驱动）：出队被拦后无 DataDequeue 触发源，
        // 需 PortTick 周期评估恢复（XON 判据 = tokens 回正）
        if (!ps.tickEvent.IsRunning())
            ps.tickEvent = Simulator::Schedule(MicroSeconds(m_rttUs / 2), &Epfc::PortTick, this, port);
        m_xoffCount++;
        if (m_debug && (m_xoffCount <= 10 || m_xoffCount % 5000 == 0)) {
            printf("[EPFC_XOFF] %lu node=%u port=%u pgs=%u tokens=%ld burst=%.0f rv=%.3fG cnt=%lu\n",
                   Simulator::Now().GetTimeStep(), m_nodeId, port,
                   ps.gatedPgs.empty() ? 0 : *ps.gatedPgs.begin(),
                   dbgTokens, dbgBurst, dbgRv / 1e9, m_xoffCount);
        }
    } else if (!anyExceed && ps.gated) {
        // 先置状态再 kick：SendGate(false) 触发的立即事件可能重入 EvaluatePort，
        // 必须先看到 gated=false，否则反复解除（v6.5 无限递归）。
        // 令牌桶无需信用重置：tokens 由时间补充自然回正演化
        ps.gated = false;
        for (uint8_t pg : ps.gatedPgs)
            SendGate(port, pg, false);
        ps.gatedPgs.clear();
        m_xonCount++;
        if (m_debug && (m_xonCount <= 10 || m_xonCount % 5000 == 0)) {
            printf("[EPFC_XON] %lu node=%u port=%u cnt=%lu\n",
                   Simulator::Now().GetTimeStep(), m_nodeId, port, m_xonCount);
        }
        // tickEvent 到期后自判 gated=false 自然停止（无需 Cancel）
    }
}

// ---------------------------------------------------------------------------
// 门控期间的周期恢复评估（每 RTT/2 一次）
// ---------------------------------------------------------------------------
void Epfc::PortTick(uint32_t port) {
    auto pit = m_egressTable.find(port);
    if (pit == m_egressTable.end() || !pit->second.gated)
        return;
    EvaluatePort(port);
    // EvaluatePort 可能已解除（gated=false → 不再重调度）
    auto pit2 = m_egressTable.find(port);
    if (pit2 != m_egressTable.end() && pit2->second.gated) {
        pit2->second.tickEvent =
            Simulator::Schedule(MicroSeconds(m_rttUs / 2), &Epfc::PortTick, this, port);
    }
}

// ---------------------------------------------------------------------------
// 全局老化 tick：表项 ts + agingRtts×RTT 无新 CCF → isContributor=false
//   （拥塞缓解 → 标记停止 → CCF 停止 → 自动放行，交回 DCQCN 接管）
// ---------------------------------------------------------------------------
void Epfc::AgingTick() {
    Time now = Simulator::Now();
    bool anyActive = false;
    for (auto &pt : m_egressTable) {
        for (auto &kv : pt.second.flows) {
            EpfcFlowEntry &e = kv.second;
            if (!e.isContributor)
                continue;
            // 更新窗过期：条目失效放行（下个 CCF 重建新窗，rv 重新采样）
            if ((now - e.ts).GetSeconds() > m_updateTUs * 1e-6) {
                e.isContributor = false;
                e.tokens = 0;
                if (m_debug) {
                    printf("[EPFC_WIN_EXP] %lu node=%u port=%u sip=0x%08x sport=%u winRv=%.3fG reset\n",
                           Simulator::Now().GetTimeStep(), m_nodeId, pt.first,
                           kv.first.sip, kv.first.sport, e.rvBps / 1e9);
                }
                continue;
            }
            if ((now - e.lastCcf).GetSeconds() > m_agingRtts * m_rttUs * 1e-6) {
                e.isContributor = false;
                e.tokens = 0;
                if (m_debug) {
                    printf("[EPFC_AGING] %lu node=%u port=%u sip=0x%08x sport=%u reset\n",
                           Simulator::Now().GetTimeStep(), m_nodeId, pt.first,
                           kv.first.sip, kv.first.sport);
                }
            } else {
                anyActive = true;
            }
        }
        // 门控中的端口重评估（全部老化 → 满足解除条件）
        if (pt.second.gated)
            EvaluatePort(pt.first);
    }
    if (anyActive) {
        m_agingEvent = Simulator::Schedule(MicroSeconds(m_rttUs / 2), &Epfc::AgingTick, this);
    } else {
        m_agingEvent = EventId(); // 全部老化：停 tick（新 CCF 到达时惰性重启）
    }
}

// ---------------------------------------------------------------------------
// 通过回调对指定端口执行本地门控/解除（SwitchNode 注入 → SetDciExtGate）
// ---------------------------------------------------------------------------
void Epfc::SendGate(uint32_t port, uint8_t pg, bool on) {
    if (!m_gateCb.IsNull())
        m_gateCb(port, pg, on);
}

} // namespace ns3
