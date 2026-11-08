/*
 * MIT License
 *
 * Copyright (c) 2025 CAVER-LB
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "grc-wan-routing.h"

#include "grc-settings.h"

#include "ns3/abort.h"
#include "ns3/qbb-header.h"
#include <ns3/ipv4-header.h>
#include <ns3/ppp-header.h>
#include <ns3/qbb-net-device.h>


#include <algorithm>
#include <assert.h>
#include <cmath>
#include <limits>

namespace ns3
{

namespace
{
constexpr double kInitialWBeforeK = 2.5;
constexpr uint32_t kRttIdentifierMask = 0x00ffffff;

void
ClearIpv4EcnMark(Ptr<Packet> p, CustomHeader& ch)
{
    if (ch.GetIpv4EcnBits() == 0)
    {
        return;
    }

    PppHeader ppp;
    Ipv4Header ipv4;
    p->RemoveHeader(ppp);
    p->RemoveHeader(ipv4);
    ipv4.SetEcn(Ipv4Header::ECN_NotECT);
    ch.m_tos &= 0xFC;
    p->AddHeader(ipv4);
    p->AddHeader(ppp);
}

double
GetInitialWValue(double w_max)
{
    return std::max(1.0, std::min(kInitialWBeforeK, w_max));
}

Time
GetWKSyncInterval()
{
    const int64_t interval_ms =
        std::stoll(GrcSettings::raw.at("W_K_SYNC_INTERVAL_MS"));
    return MilliSeconds(std::max<int64_t>(1, interval_ms));
}
} // namespace

Time GrcWanRouting::epoch_duration = MicroSeconds(1000); // 1ms
bool GrcWanRouting::s_w_k_update_scheduled = false;
bool GrcWanRouting::s_w_k_initialized = false;
double GrcWanRouting::s_w_k = 0;
std::vector<double> GrcWanRouting::s_w_k_samples;

GrcWanRouting::GrcWanRouting() = default;

void
GrcWanRouting::DstDCHandler::Init(GrcWanRouting* wan_routing, int64_t max_rate)
{
    m_wanRouting = wan_routing;
    const double guaranteed_rate_factor =
        std::stod(GrcSettings::raw.at("GUARANTEED_RATE_FACTOR"));
    guaranteed_rate = static_cast<int64_t>(max_rate * guaranteed_rate_factor);

    const int64_t window_ms =
        std::stoll(GrcSettings::raw.at("BASE_RTT_WINDOW_MS"));
    base_rtt_window = MilliSeconds(window_ms);
    rtt_tau = MicroSeconds(std::stoll(GrcSettings::raw.at("RTT_EWMA_US")));

    ref_rate = guaranteed_rate;
    end_bytes = static_cast<int64_t>(ref_rate * GrcWanRouting::epoch_duration.GetSeconds());
}

void
GrcWanRouting::DstDCHandler::record_rtt(Time rtt, Time rto)
{
    // A retransmission can refresh the timestamp while the original ACK is still in flight.
    // Section III-B rejects samples more than one RTO below the current RTT estimate.
    // This heuristic does not reject all ambiguous samples from retransmissions within an RTO.
    if (rtt + rto < sensitive_rtt)
    {
        return;
    }
    rtt_sum += rtt;
    rtt_num++;

    // 记录EWMA RTT
    Time delta_t = Simulator::Now() - last_update_time;
    last_update_time = Simulator::Now();
    double weight1 = std::min(delta_t.GetSeconds() / rtt_tau.GetSeconds(), 1.0);
    sensitive_rtt =
        Seconds((1 - weight1) * sensitive_rtt.GetSeconds() + weight1 * rtt.GetSeconds());
}

Time
GrcWanRouting::DstDCHandler::update_base_rtt(Time now, Time epoch_rtt)
{
    // Expire old observations even if the path's propagation delay has increased.
    // Entries are ordered by time and strictly increasing RTT, so the front is
    // the minimum in (now - base_rtt_window, now]. Storage is bounded by the window.
    const Time cutoff = now - base_rtt_window;
    while (!base_rtt_minima.empty() && base_rtt_minima.front().first <= cutoff)
    {
        base_rtt_minima.pop_front();
    }
    while (!base_rtt_minima.empty() && base_rtt_minima.back().second >= epoch_rtt)
    {
        base_rtt_minima.pop_back();
    }
    base_rtt_minima.emplace_back(now, epoch_rtt);
    return base_rtt_minima.front().second;
}

int64_t
GrcWanRouting::DstDCHandler::get_std_bytes() const
{
    Time epoch_elapsed = Simulator::Now() - m_wanRouting->m_epoch_start_time;
    double ratio = 0.0;
    if (epoch_elapsed > Seconds(0))
    {
        ratio = epoch_elapsed.GetSeconds() / GrcWanRouting::epoch_duration.GetSeconds();
        ratio = std::min(ratio, 1.0);
    }
    return start_bytes + static_cast<int64_t>((end_bytes - start_bytes) * ratio);
}

bool
GrcWanRouting::DstDCHandler::update_and_check_cnp(uint32_t pkt_size)
{
    cur_bytes += pkt_size;
    int64_t std_bytes = get_std_bytes();
    int64_t bytes_diff = cur_bytes - std_bytes;
    uint32_t kmin = 100 * 1024;      // 100KB
    uint32_t kmax = 8 * 1024 * 1024; // 8MB
    double pmax = 0.5;
    if (bytes_diff <= kmin)
        return false;
    if (bytes_diff >= kmax)
        return true; // 关键：上阈值必丢

    double p = pmax * (double)(bytes_diff - kmin) / (double)(kmax - kmin);
    // 此时 p ∈ (0, pmax)
    double rand_val = std::generate_canonical<double, 53>(m_wanRouting->m_rng);
    return rand_val < p;
}

void
GrcWanRouting::Init(Ptr<Node> node, uint32_t dc, const std::map<uint32_t, uint32_t>& routes)
{
    m_switch_id = node->GetId();
    m_dc = dc;
    std::seed_seq seed{static_cast<uint32_t>(std::stoul(GrcSettings::raw.at("GRC_SEED"))),
                       m_switch_id};
    m_rng.seed(seed);
    m_hash_seed1 = m_rng();
    m_hash_seed2 = m_rng();
    epoch_duration = MicroSeconds(std::stoll(GrcSettings::raw.at("WAN_EPOCH_US")));
    m_rtt_entry_timeout = MicroSeconds(std::stoll(GrcSettings::raw.at("RTT_ENTRY_TIMEOUT_US")));
    m_rtt_rto_filter = MicroSeconds(std::stoll(GrcSettings::raw.at("RTT_RTO_FILTER_US")));
    for (const auto& [dst, port] : routes)
    {
        auto& handler = m_dcHandler[dst];
        int64_t max_rate = DynamicCast<QbbNetDevice>(node->GetDevice(port))->GetDataRate().GetBitRate() / 8;
        handler.Init(this, max_rate);
    }
    m_epoch_start_time = NanoSeconds(std::stoll(GrcSettings::raw.at("EPOCH_START_NS")));
    Simulator::Schedule(m_epoch_start_time, &GrcWanRouting::controlplane_logic, this);
    if (!s_w_k_update_scheduled)
    {
        s_w_k = 0.0;
        s_w_k_initialized = false;
        s_w_k_update_scheduled = true;
        Simulator::Schedule(m_epoch_start_time + GetWKSyncInterval(), &GrcWanRouting::update_w_k);
    }
}

void
GrcWanRouting::RouteInput(Ptr<Packet> p, CustomHeader& ch)
{
    switch (ch.l3Prot)
    {
    case 0x11: // UDP
        HandleUdpReceived(p, ch);
        return;
    case 0xFC:
        HandleAckReceived(p, ch);
        return;
    default:
        m_switchSendToDevCallback(p, ch);
        return;
    }
}

void
GrcWanRouting::HandleUdpReceived(Ptr<Packet> p, CustomHeader& ch)
{
    auto it = m_dcHandler.find(GrcSettings::hostIpToDc.at(ch.dip));
    // Only the source DC's gateway controls this flow; transit DCIs just forward it.
    if (GrcSettings::hostIpToDc.at(ch.sip) != m_dc || it == m_dcHandler.end())
    {
        m_switchSendToDevCallback(p, ch);
        return;
    }
    NS_ABORT_MSG_IF(Simulator::Now() < m_epoch_start_time, "GRC DATA precedes EPOCH_START_NS");
    auto& dc_handler = it->second;
    // per-epoch statistics
    dc_handler.epoch_pkt_cnt++;
    // RTT过滤
    // Sample DATA on the DCI. Matching cumulative ACK sequence numbers provide
    // the second sampling level; no endpoint tag or ACK-policy modification.
    const uint32_t seq_end = ch.udp.seq + p->GetSize() - ch.GetSerializedSize();
    const uint32_t hashed_seq =
        Hash4TupleSeq(ch.sip, ch.dip, ch.udp.sport, ch.udp.dport, seq_end) & kRttIdentifierMask;
    const uint32_t index =
        GetRttTableIndex(ch.sip, ch.dip, ch.udp.sport, ch.udp.dport, seq_end);
    auto& entry = rtt_table[index];
    // Refresh a retransmitted sample. A separate validity bit allows identifier zero.
    if (entry.valid && entry.hashed_seq == hashed_seq)
    {
        entry.timestamp = Simulator::Now();
    }
    else if (!entry.valid || Simulator::Now() - entry.timestamp > m_rtt_entry_timeout)
    {
        entry.valid = true;
        entry.hashed_seq = hashed_seq;
        entry.timestamp = Simulator::Now();
    }
    const bool ecn_marked = ch.GetIpv4EcnBits() != 0;
    // Algorithm 3 accounts for every packet before testing RED or an existing ECN mark.
    const bool rate_cnp = dc_handler.update_and_check_cnp(p->GetSize());
    if (rate_cnp || ecn_marked)
    {
        dc_handler.epoch_cnp_cnt++;
        send_cnp(ch, rate_cnp, ecn_marked);
        if (ecn_marked)
        {
            ClearIpv4EcnMark(p, ch);
        }
    }
    dc_handler.total_send_bytes += p->GetSize();
    m_switchSendToDevCallback(p, ch);
    return;
}

void
GrcWanRouting::HandleAckReceived(Ptr<Packet> p, CustomHeader& ch)
{
    auto dcIt = m_dcHandler.find(GrcSettings::hostIpToDc.at(ch.sip));
    // Measure only ACKs returning to this DC's sources, not transit ACKs.
    if (GrcSettings::hostIpToDc.at(ch.dip) != m_dc || dcIt == m_dcHandler.end())
    {
        m_switchSendToDevCallback(p, ch);
        return;
    }
    auto& dcHandler = dcIt->second;
    const uint32_t hashed_seq =
        Hash4TupleSeq(ch.dip, ch.sip, ch.ack.dport, ch.ack.sport, ch.ack.seq) & kRttIdentifierMask;
    const uint32_t index = GetRttTableIndex(ch.dip, ch.sip, ch.ack.dport, ch.ack.sport, ch.ack.seq);
    if (rtt_table[index].valid && rtt_table[index].hashed_seq == hashed_seq)
    {
        Time rtt = Simulator::Now() - rtt_table[index].timestamp;
        // Keep upstream's cross-DC stale-sample heuristic. This is a controller
        // filter only; it does not configure endpoint retransmission behavior.
        dcHandler.record_rtt(rtt, m_rtt_rto_filter);
        rtt_table[index].valid = false;
    }
    m_switchSendToDevCallback(p, ch);
}

void
GrcWanRouting::controlplane_logic()
{
    Simulator::Schedule(epoch_duration, &GrcWanRouting::controlplane_logic, this);
    m_epoch_start_time = Simulator::Now();
    for (auto& [dst_as, dc_handler] : m_dcHandler)
    {
        // Per-epoch feedback probability drives the concurrency estimate and weight.
        const uint64_t pkt_cnt = dc_handler.epoch_pkt_cnt;
        const uint64_t cnp_cnt = dc_handler.epoch_cnp_cnt;
        const double prob =
            (pkt_cnt == 0) ? 0.0 : (static_cast<double>(cnp_cnt) / static_cast<double>(pkt_cnt));
        const double w_max = std::stod(GrcSettings::raw.at("W_MAX"));
        // Section III-E records the concurrency estimate of this epoch, without
        // a separate smoothing or alternative global-k policy.
        const double x = std::pow(prob, 0.75) * dc_handler.ref_rate;
        dc_handler.latest_w_x = x;
        const double k_var = s_w_k_initialized ? s_w_k : std::numeric_limits<double>::quiet_NaN();
        const double w_var =
            s_w_k_initialized ? std::max(1.0, std::min(x * s_w_k, w_max)) : GetInitialWValue(w_max);
        if (x > 1e-9)
        {
            s_w_k_samples.push_back(x);
        }

        // Paper fairness measurements use the same epoch byte count as rate control.
        const uint64_t epoch_rate = dc_handler.total_send_bytes / epoch_duration.GetSeconds();
        const int64_t ref_before = dc_handler.ref_rate;
        const int64_t cur_bytes_before = dc_handler.cur_bytes;
        fprintf(GrcLog::rate_monitor,
                "%lu,%u,%u,%lu,%lu,%.6f,%.9g,%.9g\n",
                Simulator::Now().GetNanoSeconds(),
                m_dc,
                dst_as,
                epoch_rate,
                dc_handler.ref_rate,
                w_var,
                k_var,
                dc_handler.latest_w_x);

        if (dc_handler.sensitive_rtt > Seconds(0))
        { // rtt已经接收到第一个数据
            dc_handler.update_ref_rate();
        }
        // 将参考速率下发给速率控制模块
        dc_handler.start_bytes = dc_handler.end_bytes - dc_handler.cur_bytes;
        dc_handler.start_bytes = std::min(dc_handler.start_bytes, (int64_t)0LL);
        dc_handler.end_bytes =
            dc_handler.start_bytes + dc_handler.ref_rate * epoch_duration.GetSeconds();
        dc_handler.cur_bytes = 0;
        if (GrcLog::controller)
        {
            const int64_t epoch_rtt =
                dc_handler.rtt_num
                    ? static_cast<int64_t>(dc_handler.rtt_sum.GetNanoSeconds() / dc_handler.rtt_num)
                    : -1;
            const int64_t base_rtt =
                dc_handler.base_rtt_minima.empty()
                    ? -1
                    : dc_handler.base_rtt_minima.front().second.GetNanoSeconds();
            fprintf(
                GrcLog::controller,
                "%lld,%u,%u,%u,%llu,%lld,%lld,%d,%lld,%lld,%lld,%lld,%.12g,%.12g,"
                "%.12g,%llu,%llu,%lld,%lld,%lld,%d,%u\n",
                static_cast<long long>(Simulator::Now().GetNanoSeconds()),
                m_switch_id,
                m_dc,
                dst_as,
                static_cast<unsigned long long>(epoch_rate),
                static_cast<long long>(ref_before),
                static_cast<long long>(dc_handler.ref_rate),
                dc_handler.rtt_num,
                static_cast<long long>(epoch_rtt),
                static_cast<long long>(dc_handler.sensitive_rtt.GetNanoSeconds()),
                static_cast<long long>(base_rtt),
                static_cast<long long>(epoch_rtt >= 0 && base_rtt >= 0 ? epoch_rtt - base_rtt : -1),
                w_var,
                k_var,
                dc_handler.latest_w_x,
                static_cast<unsigned long long>(pkt_cnt),
                static_cast<unsigned long long>(cnp_cnt),
                static_cast<long long>(dc_handler.start_bytes),
                static_cast<long long>(dc_handler.end_bytes),
                static_cast<long long>(cur_bytes_before),
                static_cast<int>(dc_handler.rate_change_state),
                dc_handler.consecutive_state_epochs);
        }
        dc_handler.total_send_bytes = 0;
        dc_handler.rtt_sum = Seconds(0);
        dc_handler.rtt_num = 0;

        // Reset per-epoch statistics
        dc_handler.epoch_pkt_cnt = 0;
        dc_handler.epoch_cnp_cnt = 0;
    }
    fflush(GrcLog::rate_monitor);
}

void
GrcWanRouting::update_w_k()
{
    Simulator::Schedule(GetWKSyncInterval(), &GrcWanRouting::update_w_k);
    if (s_w_k_samples.empty())
    {
        return;
    }
    double w_max = std::stod(GrcSettings::raw.at("W_MAX"));
    std::vector<std::pair<double, int>> events;
    events.reserve(s_w_k_samples.size() * 2);
    for (double x : s_w_k_samples)
    {
        events.push_back(std::make_pair(1.0 / x, 1));
        events.push_back(std::make_pair(w_max / x, -1));
    }
    s_w_k_samples.clear();

    std::sort(events.begin(),
              events.end(),
              [](const std::pair<double, int>& a, const std::pair<double, int>& b) {
                  if (a.first != b.first)
                  {
                      return a.first < b.first;
                  }
                  return a.second > b.second;
              });

    int max_overlap = 0;
    int current_overlap = 0;
    for (size_t i = 0; i + 1 < events.size(); ++i)
    {
        current_overlap += events[i].second;
        if (events[i + 1].first > events[i].first && current_overlap > max_overlap)
        {
            max_overlap = current_overlap;
        }
    }

    current_overlap = 0;
    bool has_platform = false;
    double platform_left = 0.0;
    double platform_right = 0.0;
    double best_left = 0.0;
    double best_right = 0.0;
    double best_k = s_w_k;
    for (size_t i = 0; i + 1 < events.size(); ++i)
    {
        current_overlap += events[i].second;
        const double left = events[i].first;
        const double right = events[i + 1].first;
        if (right <= left || current_overlap != max_overlap)
        {
            continue;
        }
        if (!has_platform)
        {
            platform_left = left;
            platform_right = right;
            has_platform = true;
            continue;
        }
        if (left <= platform_right)
        {
            platform_right = right;
            continue;
        }
        if (platform_right - platform_left > best_right - best_left)
        {
            best_left = platform_left;
            best_right = platform_right;
        }
        platform_left = left;
        platform_right = right;
    }

    if (has_platform && platform_right - platform_left > best_right - best_left)
    {
        best_left = platform_left;
        best_right = platform_right;
    }
    if (best_right > best_left)
    {
        best_k = (best_left + best_right) / 2.0;
    }
    else
    {
        current_overlap = 0;
        for (const auto& event : events)
        {
            current_overlap += event.second;
            if (current_overlap == max_overlap)
            {
                best_k = event.first;
                break;
            }
        }
    }
    s_w_k = best_k;
    s_w_k_initialized = true;
}

void
GrcWanRouting::DstDCHandler::update_ref_rate()
{
    // Algorithm 2 uses only the actual sending rate of the preceding epoch.
    const int64_t rate_before = total_send_bytes / GrcWanRouting::epoch_duration.GetSeconds();
    int64_t upper_rate = std::max(guaranteed_rate, static_cast<int64_t>(rate_before * 1.2));
    int64_t pre_ref_rate = ref_rate;

    if (rtt_num == 0)
    {
        if (ref_rate > upper_rate)
        {
            ref_rate = upper_rate + (ref_rate - upper_rate) * 0.8;
        }
        return;
    }
    const Time cur_rtt = Seconds(rtt_sum.GetSeconds() / rtt_num);
    const Time min_rtt = update_base_rtt(Simulator::Now(), cur_rtt);
    double epochs_per_rtt = min_rtt.GetSeconds() / GrcWanRouting::epoch_duration.GetSeconds();
    if (epochs_per_rtt <= 0 || min_rtt.GetSeconds() <= 0)
    {
        return;
    }

    const double w_max = std::stod(GrcSettings::raw.at("W_MAX"));
    const double k_used = GrcWanRouting::s_w_k_initialized
                              ? GrcWanRouting::s_w_k
                              : std::numeric_limits<double>::quiet_NaN();
    const double w = GrcWanRouting::s_w_k_initialized
                         ? std::max(1.0, std::min(latest_w_x * k_used, w_max))
                         : GetInitialWValue(w_max);

    // Get target_rate and target_state
    const double delta = 1.0 / std::stod(GrcSettings::raw.at("INV_DELTA"));
    const double beta = std::stod(GrcSettings::raw.at("BETA"));
    Time queue_delay = cur_rtt - min_rtt;
    // Zero queuing delay means an unbounded target; compute the update step from base RTT below.
    double target_rate = queue_delay > Seconds(0) ? w / delta / queue_delay.GetSeconds()
                                                  : std::numeric_limits<double>::infinity();
    RateChangeState target_state = (target_rate > pre_ref_rate) ? INCREASE : DECREASE;
    if (rate_change_state == target_state)
    {
        consecutive_state_epochs++;
    }
    else
    {
        rate_change_state = target_state;
        consecutive_state_epochs = 1;
    }

    // Get v
    int v = 1;
    if (consecutive_state_epochs >= 10 * epochs_per_rtt)
    {
        v = 4;
    }
    else if (consecutive_state_epochs >= 5 * epochs_per_rtt)
    {
        v = 2;
    }

    int64_t step = static_cast<int64_t>(w * v / (delta * epochs_per_rtt * min_rtt.GetSeconds()));
    if (target_state == INCREASE)
    {
        ref_rate += step;
    }
    else
    {
        // Algorithm 2 shrinks the gap to the target, not the entire reference rate.
        const double beta_e = 1.0 - std::pow(1.0 - beta, 1.0 / epochs_per_rtt);
        const int64_t min_step =
            static_cast<int64_t>(std::ceil((pre_ref_rate - target_rate) * beta_e));

        step = std::max(step, min_step);
        step = std::min(step, pre_ref_rate); // avoid negative ref_rate

        ref_rate -= step;
    }
    if (ref_rate > upper_rate)
    {
        ref_rate = upper_rate + (ref_rate - upper_rate) * 0.8;
    }
}

void
GrcWanRouting::send_cnp(CustomHeader& ch, bool rateTriggered, bool ecnTriggered)
{
    // Local DCQCN consumes the CNP flag on ACKs, not standalone 0xFF CNP.
    // Sequence zero cannot advance snd_una (Acknowledge only accepts increases).
    // Thus this feedback neither acknowledges undelivered DATA nor triggers NACK.
    qbbHeader seqh;
    seqh.SetPG(ch.udp.pg);
    seqh.SetSport(ch.udp.dport);
    seqh.SetDport(ch.udp.sport);
    seqh.SetSeq(0);
    seqh.SetCnp();

    Ptr<Packet> newp = Create<Packet>(6);
    newp->AddHeader(seqh);

    Ipv4Header ipv4h; // Prepare IPv4 header
    ipv4h.SetDestination(Ipv4Address(ch.sip));
    // Source为当前设备
    ipv4h.SetSource(Ipv4Address(ch.dip));
    ipv4h.SetProtocol(0xFC); // Existing DCQCN congestion-feedback encoding
    ipv4h.SetTtl(64);
    ipv4h.SetPayloadSize(newp->GetSize());
    ipv4h.SetIdentification(0);
    newp->AddHeader(ipv4h);

    PppHeader ppp;
    ppp.SetProtocol(0x0021);
    newp->AddHeader(ppp);
    // send
    CustomHeader ch2(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    newp->PeekHeader(ch2);

    GrcLog::Event("cnp_generate",
                  m_switch_id,
                  ch.sip,
                  ch.dip,
                  ch.udp.sport,
                  ch.udp.dport,
                  ch.udp.pg,
                  newp->GetUid(),
                  ch.udp.seq,
                  rateTriggered ? (ecnTriggered ? "grc_and_ecn" : "grc_rate") : "dci_ecn");

    m_switchSendToDevCallback(newp, ch2);
}

uint32_t
GrcWanRouting::Hash4Tuple(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport)
{
    union {
        uint8_t u8[4 + 4 + 2 + 2];
        uint32_t u32[3];
    } buf;

    buf.u32[0] = sip;
    buf.u32[1] = dip;
    buf.u32[2] = sport | ((uint32_t)dport << 16);
    return ECMPHash(buf.u8, 12, m_hash_seed1);
}

uint32_t
GrcWanRouting::Hash4TupleSeq(uint32_t sip,
                             uint32_t dip,
                             uint16_t sport,
                             uint16_t dport,
                             uint32_t seq)
{
    union {
        uint8_t u8[4 + 4 + 2 + 2 + 4];
        uint32_t u32[4];
    } buf;

    buf.u32[0] = sip;
    buf.u32[1] = dip;
    buf.u32[2] = sport | ((uint32_t)dport << 16);
    buf.u32[3] = seq;
    return ECMPHash(buf.u8, 16, m_hash_seed2);
}

uint32_t
GrcWanRouting::GetRttTableIndex(uint32_t sip,
                                uint32_t dip,
                                uint16_t sport,
                                uint16_t dport,
                                uint32_t seq)
{
    // UDP is fixed, so these four fields are all variable fields of the IP 5-tuple.
    // Algorithm 1: one 16-entry bucket per flow, then a sequence-number hash within it.
    const uint32_t bucket = Hash4Tuple(sip, dip, sport, dport) % (rtt_table_size / 16);
    const uint32_t offset =
        ECMPHash(reinterpret_cast<const uint8_t*>(&seq), sizeof(seq), m_hash_seed2) % 16;
    return bucket * 16 + offset;
}

uint32_t
GrcWanRouting::ECMPHash(const uint8_t* key, size_t len, uint32_t seed)
{
    uint32_t h = seed;
    if (len > 3)
    {
        const uint32_t* key_x4 = (const uint32_t*)key;
        size_t i = len >> 2;
        do
        {
            uint32_t k = *key_x4++;
            k *= 0xcc9e2d51;
            k = (k << 15) | (k >> 17);
            k *= 0x1b873593;
            h ^= k;
            h = (h << 13) | (h >> 19);
            h += (h << 2) + 0xe6546b64;
        } while (--i);
        key = (const uint8_t*)key_x4;
    }
    if (len & 3)
    {
        size_t i = len & 3;
        uint32_t k = 0;
        key = &key[i - 1];
        do
        {
            k <<= 8;
            k |= *key--;
        } while (--i);
        k *= 0xcc9e2d51;
        k = (k << 15) | (k >> 17);
        k *= 0x1b873593;
        h ^= k;
    }
    h ^= len;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

void
GrcWanRouting::SetSwitchSendToDevCallback(SwitchSendToDevCallback switchSendToDevCallback)
{
    m_switchSendToDevCallback = switchSendToDevCallback;
}

} // namespace ns3
