#include "themis-gateway.h"

#include "switch-node.h"
#include "ns3/abort.h"
#include "ns3/boolean.h"
#include "ns3/double.h"
#include "ns3/interface-tag.h"
#include "ns3/ipv4-header.h"
#include "ns3/ppp-header.h"
#include "ns3/qbb-helper.h"
#include "ns3/qbb-header.h"
#include "ns3/rdma-driver.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <cstdio>

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED(ThemisLoopTag);
NS_OBJECT_ENSURE_REGISTERED(ThemisGateway);

ThemisGateway::ThemisGateway() = default;

TypeId ThemisGateway::GetTypeId() {
    static TypeId tid = TypeId("ns3::ThemisGateway").SetParent<Object>()
        .SetGroupName("PointToPoint").AddConstructor<ThemisGateway>();
    return tid;
}

ThemisFlowKey ThemisFlowKey::Data(const CustomHeader& ch) {
    return {ch.sip, ch.dip, ch.udp.sport, ch.udp.dport, ch.udp.pg};
}
ThemisFlowKey ThemisFlowKey::Feedback(const CustomHeader& ch) {
    return {ch.dip, ch.sip, ch.ack.dport, ch.ack.sport, ch.ack.pg};
}
TypeId ThemisLoopTag::GetTypeId() {
    static TypeId tid = TypeId("ns3::ThemisLoopTag").SetParent<Tag>()
        .SetGroupName("PointToPoint").AddConstructor<ThemisLoopTag>();
    return tid;
}
TypeId ThemisLoopTag::GetInstanceTypeId() const { return GetTypeId(); }
uint32_t ThemisLoopTag::GetSerializedSize() const { return 9; }
void ThemisLoopTag::Serialize(TagBuffer b) const {
    b.WriteU32(remaining); b.WriteU32(ingress); b.WriteU8(dummy);
}
void ThemisLoopTag::Deserialize(TagBuffer b) {
    remaining = b.ReadU32(); ingress = b.ReadU32(); dummy = b.ReadU8();
}
void ThemisLoopTag::Print(std::ostream& os) const {
    os << "remaining=" << remaining << " ingress=" << ingress << " dummy=" << dummy;
}

void ThemisGateway::Configure(SwitchNode* node, uint32_t dc,
                             const std::map<uint32_t, uint32_t>& hostIpToDc,
                             const ThemisSettings& config,
                             Callback<void, Ptr<Packet>, CustomHeader&> send) {
    m_node = node; m_dc = dc; m_hostDc = hostIpToDc;
    m_config = config; m_send = send;
    SetupLoop();
    printf("[THEMIS_CONFIG] node=%u dc=%u alpha=%u beta_ns=%llu\n",
           node->GetId(), dc, config.alpha,
           (unsigned long long)config.beta.GetNanoSeconds());
}

void ThemisGateway::ConnectCompletionTrace(Ptr<RdmaDriver> driver) {
    bool connected = driver->TraceConnectWithoutContext("QpComplete",
                        MakeCallback(&ThemisGateway::OnQpComplete, this));
    NS_ABORT_MSG_IF(!connected, "THEMIS cannot subscribe to the existing QpComplete trace");
    m_completionSources.push_back(driver);
}

void ThemisGateway::DoDispose() {
    if (m_node) Report();
    for (const auto& driver : m_completionSources)
        driver->TraceDisconnectWithoutContext("QpComplete",
            MakeCallback(&ThemisGateway::OnQpComplete, this));
    for (auto& entry : m_states) Simulator::Cancel(entry.second.recoveryEvent);
    m_completionSources.clear();
    m_states.clear();
    m_send.Nullify();
    m_node = nullptr;
    Object::DoDispose();
}

bool ThemisGateway::IsSource(const ThemisFlowKey& key) const {
    auto src = m_hostDc.find(key.sip), dst = m_hostDc.find(key.dip);
    return src != m_hostDc.end() && dst != m_hostDc.end() &&
           src->second == m_dc && dst->second != m_dc;
}
bool ThemisGateway::IsDestination(const ThemisFlowKey& key) const {
    auto src = m_hostDc.find(key.sip), dst = m_hostDc.find(key.dip);
    return src != m_hostDc.end() && dst != m_hostDc.end() &&
           src->second != m_dc && dst->second == m_dc;
}
bool ThemisGateway::IsLoopPort(uint32_t port) const {
    return port == m_loopTx || port == m_loopRx;
}
bool ThemisGateway::IsWan(uint32_t port) const {
    auto dev = DynamicCast<QbbNetDevice>(m_node->GetDevice(port));
    return dev && dev->IsWanPort();
}

void ThemisGateway::SetupLoop() {
    // Prefer an explicit topology loop; otherwise model internal recirculation
    // with the upstream NS-3 example's 1600 Gbps / 100 ns link.
    for (uint32_t i = 0; i < m_node->GetNDevices(); ++i) {
        auto dev = DynamicCast<QbbNetDevice>(m_node->GetDevice(i));
        if (!dev) continue;
        auto channel = DynamicCast<QbbChannel>(dev->GetChannel());
        if (!channel || channel->GetNDevices() != 2) continue;
        auto peer = channel->GetDevice(0) == dev ? channel->GetDevice(1) : channel->GetDevice(0);
        if (peer->GetNode() == m_node) {
            if (!m_loopTx) { m_loopTx = i; m_loopRx = peer->GetIfIndex(); }
        }
    }
    bool reused = m_loopTx != 0;
    if (!reused) {
        QbbHelper helper;
        helper.SetDeviceAttribute("DataRate", DataRateValue(DataRate("1600Gbps")));
        helper.SetDeviceAttribute("QbbEnabled", BooleanValue(false));
        helper.SetChannelAttribute("Delay", TimeValue(NanoSeconds(100)));
        NS_ABORT_MSG_IF(m_node->GetNDevices() + 2 > 257, "THEMIS exceeds switch port limit");
        auto devices = helper.Install(m_node, m_node);
        m_loopTx = devices.Get(0)->GetIfIndex(); m_loopRx = devices.Get(1)->GetIfIndex();
    }
    NS_ABORT_MSG_IF(m_loopTx >= 257 || m_loopRx >= 257, "THEMIS exceeds switch port limit");
    auto tx = DynamicCast<QbbNetDevice>(m_node->GetDevice(m_loopTx));
    auto rx = DynamicCast<QbbNetDevice>(m_node->GetDevice(m_loopRx));
    auto channel = DynamicCast<QbbChannel>(tx->GetChannel());
    NS_ABORT_MSG_IF(tx->GetDataRate().GetBitRate() == 0 || channel->GetDelay() < Time(0),
                    "Invalid THEMIS loop rate/delay");
    // Dedicated finite loop storage, using the supplied reference's queue
    // ceiling. This adds ESW resources; it is not a paper-specified partition.
    m_loopCapacity = 1000ull * 1024 * 1024;
    const uint64_t size = m_config.dummyPacketBytes;
    uint64_t bdp = static_cast<uint64_t>(tx->GetDataRate().GetBitRate() *
                                       channel->GetDelay().GetSeconds() / 8);
    m_dummyTargetBytes = ((std::max(2 * size, bdp + size) + size - 1) / size) * size;
    NS_ABORT_MSG_IF(m_dummyTargetBytes + 65536 >= m_loopCapacity,
                    "THEMIS loop buffer needs room for dummy packets and data");
    for (auto dev : {tx, rx}) {
        dev->SetAttribute("QbbEnabled", BooleanValue(false));
        dev->SetIgnoreReceivedPfc(true);
        dev->GetQueue()->SetAttribute("MaxBytes", DoubleValue(m_loopCapacity + 1));
    }
    printf("[THEMIS_LOOP_CONFIG] node=%u tx=%u rx=%u reused=%u rate_bps=%llu delay_ns=%llu buffer_bytes=%llu dummy_bytes=%llu dummy_packet_bytes=%u ordinary_buffer_bytes=%llu\n",
           m_node->GetId(), m_loopTx, m_loopRx, reused,
           (unsigned long long)tx->GetDataRate().GetBitRate(),
           (unsigned long long)channel->GetDelay().GetNanoSeconds(),
           (unsigned long long)m_loopCapacity,
           (unsigned long long)m_dummyTargetBytes, m_config.dummyPacketBytes,
           (unsigned long long)m_node->m_mmu->bufferPool);
}

void ThemisGateway::StartDummy() {
    // Fill on the first throttled packet and retire after draining. This gives
    // the first data packet a prefilled FIFO without idle perpetual events.
    while (m_dummyBytes < m_dummyTargetBytes) {
        auto p = Create<Packet>(m_config.dummyPacketBytes - 22);
        Ipv4Header ip;
        ip.SetProtocol(0); ip.SetTtl(1); ip.SetPayloadSize(p->GetSize());
        p->AddHeader(ip);
        PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp);
        ThemisLoopTag tag; tag.dummy = true; p->AddPacketTag(tag);
        m_dummyBytes += p->GetSize();
        CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
        p->PeekHeader(ch);
        SendLoop(p, ch);
    }
}
void ThemisGateway::SendLoop(Ptr<Packet> p, CustomHeader& ch) {
    NS_ABORT_MSG_IF(GetLoopBytes() > m_loopCapacity, "THEMIS loop accounting overflow");
    m_peakBytes = std::max(m_peakBytes, GetLoopBytes());
    // The dedicated port has its own finite accounting above. Its single FIFO
    // uses q0, with no MyPriorityTag (which would classify traffic as lossy).
    DynamicCast<QbbNetDevice>(m_node->GetDevice(m_loopTx))->SwitchSend(0, p, ch);
}

void ThemisGateway::ReceiveCnp(const ThemisFlowKey& key) {
    auto result = m_states.try_emplace(key);
    State& state = result.first->second;
    if (result.second) state.alpha = m_config.alpha;
    state.finished = false;
    state.status = THROTTLED;
    ++state.cnpNum; ++m_trpCount;
    // Literal ICNP Algorithm 1. This differs from the upstream cumulative
    // threshold/subtraction variant; see the accompanying migration notes.
    if (state.cnpNum % state.alpha == 1) {
        NS_ABORT_MSG_IF(state.loopNum == UINT32_MAX || state.alpha == UINT32_MAX,
                        "THEMIS loop/alpha counter exhausted");
        ++state.loopNum; ++state.alpha;
    }
    Simulator::Cancel(state.recoveryEvent);
    state.recoveryEvent = Simulator::Schedule(m_config.beta + NanoSeconds(1),
                                              &ThemisGateway::Recover, this, key);
    Trace("trp_cnp", key);
}
void ThemisGateway::Recover(ThemisFlowKey key) {
    auto it = m_states.find(key);
    if (it == m_states.end() || it->second.status != THROTTLED) return;
    State& state = it->second;
    state.status = RECOVER;
    state.cnpNum = 0;
    // Algorithm 1 does not specify an integer lower bound. A minimum of two
    // preserves its modulo test (alpha=1 could never yield remainder one).
    state.alpha = std::max(2u, state.alpha / 2);
    ++m_recoveryCount;
    Trace("recover", key);
    if (state.pending.empty()) {
        state.status = NORMAL; state.loopNum = 0; Trace("drained", key);
    }
}

bool ThemisGateway::PacketIn(uint32_t ingress, Ptr<Packet> p, CustomHeader& ch) {
    if (IsLoopPort(ingress)) {
        ThemisLoopTag tag;
        NS_ABORT_MSG_IF(!p->RemovePacketTag(tag), "Unmarked packet on THEMIS internal loop");
        return HandleLoop(p, ch, tag);
    }
    if ((ch.l3Prot == 0xFC || ch.l3Prot == 0xFD) &&
        (ch.ack.flags & (1u << qbbHeader::FLAG_CNP)) && !IsWan(ingress)) {
        auto key = ThemisFlowKey::Feedback(ch);
        if (IsDestination(key)) ReceiveCnp(key);
        return false; // Original feedback still reaches the sender's DCQCN.
    }
    if (ch.l3Prot != 0x11 || !IsWan(ingress)) return false;
    auto key = ThemisFlowKey::Data(ch);
    if (!IsDestination(key)) return false; // Includes transit DCs.
    auto it = m_states.find(key);
    if (it == m_states.end() || it->second.status == NORMAL) return false;
    State& state = it->second;
    if (!m_dataBytes) StartDummy();
    if (GetLoopBytes() + p->GetSize() > m_loopCapacity) {
        ++m_dropCount;
        return true; // Finite admission failure; existing GBN handles loss.
    }
    ThemisLoopTag tag;
    tag.ingress = ingress;
    tag.remaining = state.status == THROTTLED ? std::max(1u, state.loopNum) : 1;
    state.pending.insert(ch.udp.seq);
    p->AddPacketTag(tag);
    m_dataBytes += p->GetSize(); ++m_enterCount;
    SendLoop(p, ch);
    return true;
}

bool ThemisGateway::HandleLoop(Ptr<Packet> p, CustomHeader& ch, ThemisLoopTag tag) {
    if (tag.dummy) {
        if (m_dataBytes) { p->AddPacketTag(tag); SendLoop(p, ch); }
        else m_dummyBytes -= p->GetSize();
        return true;
    }
    auto key = ThemisFlowKey::Data(ch);
    auto it = m_states.find(key);
    NS_ABORT_MSG_IF(it == m_states.end() || it->second.pending.empty(),
                    "THEMIS lost recirculating flow state");
    State& state = it->second;
    ++m_passCount;
    if (tag.remaining) --tag.remaining;
    // Appendix D: recovery packets still traverse the loop once. Never release
    // a later buffered packet ahead of an earlier one. Tracking buffered
    // sequence numbers also handles retransmissions and non-unit packet sizes.
    bool oldest = ch.udp.seq == *state.pending.begin();
    bool ready = state.status != THROTTLED || tag.remaining == 0;
    if (!oldest || !ready) {
        p->AddPacketTag(tag); SendLoop(p, ch); return true;
    }
    state.pending.erase(state.pending.find(ch.udp.seq));
    m_dataBytes -= p->GetSize(); ++m_exitCount;
    p->AddPacketTag(InterfaceTag(tag.ingress));
    if (state.status == RECOVER && state.pending.empty()) {
        state.status = NORMAL; state.loopNum = 0; Trace("drained", key);
    }
    if (state.finished && state.pending.empty()) m_states.erase(it);
    return false; // Re-enter the ordinary routing/admission path exactly once.
}

void ThemisGateway::NotifyDequeue(uint32_t outPort, Ptr<Packet> p) {
    if (!IsWan(outPort)) return;
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    ch.getInt = 1; p->PeekHeader(ch);
    if (ch.l3Prot != 0x11 || ch.GetIpv4EcnBits() != CustomHeader::ECN_CE) return;
    auto key = ThemisFlowKey::Data(ch);
    if (!IsSource(key)) return;
    // Match the project's receiver policy: one notification per marked packet.
    SendCnp(ch); ++m_pnpCount; Trace("pnp_cnp", key, ch.udp.seq);
    PppHeader ppp; Ipv4Header ip;
    p->RemoveHeader(ppp); p->RemoveHeader(ip);
    ip.SetEcn(Ipv4Header::ECN_NotECT);
    p->AddHeader(ip); p->AddHeader(ppp);
}

void ThemisGateway::SendCnp(const CustomHeader& data) {
    qbbHeader ack;
    ack.SetSeq(0); ack.SetPG(data.udp.pg);
    ack.SetSport(data.udp.dport); ack.SetDport(data.udp.sport); ack.SetCnp();
    auto p = Create<Packet>(std::max(60 - 14 - 20 - (int)ack.GetSerializedSize(), 0));
    p->AddHeader(ack);
    Ipv4Header ip;
    ip.SetSource(Ipv4Address(data.dip)); ip.SetDestination(Ipv4Address(data.sip));
    ip.SetProtocol(0xFC); ip.SetTtl(64); ip.SetPayloadSize(p->GetSize());
    ip.SetIdentification(0); p->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp);
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    p->PeekHeader(ch);
    m_send(p, ch); // q0 by ACK protocol; no lossy-priority tag is attached.
}

void ThemisGateway::OnQpComplete(Ptr<RdmaQueuePair> qp) {
    ThemisFlowKey key{qp->sip.Get(), qp->dip.Get(), qp->sport, qp->dport, qp->m_pg};
    auto it = m_states.find(key);
    if (it == m_states.end()) return;
    Simulator::Cancel(it->second.recoveryEvent);
    if (it->second.pending.empty()) m_states.erase(it);
    else { it->second.finished = true; it->second.status = RECOVER; }
}

void ThemisGateway::Trace(const char* event, const ThemisFlowKey& key, uint32_t seq) const {
    if (!m_config.traceEvents) return;
    auto it = m_states.find(key);
    printf("[THEMIS_EVENT] %llu node=%u event=%s sip=0x%08x dip=0x%08x sport=%u dport=%u pg=%u seq=%u cnp_num=%llu alpha=%u loops=%u loop_bytes=%llu\n",
           (unsigned long long)Simulator::Now().GetNanoSeconds(), m_node->GetId(), event,
           key.sip, key.dip, key.sport, key.dport, key.pg, seq,
           (unsigned long long)(it == m_states.end() ? 0 : it->second.cnpNum),
           it == m_states.end() ? m_config.alpha : it->second.alpha,
           it == m_states.end() ? 0 : it->second.loopNum, (unsigned long long)GetLoopBytes());
}

void ThemisGateway::Report() const {
    printf("[THEMIS_SUMMARY] node=%u pnp_cnp=%llu trp_cnp=%llu loop_enter=%llu loop_exit=%llu loop_pass=%llu loop_drop=%llu recoveries=%llu peak_bytes=%llu data_bytes=%llu dummy_bytes=%llu flows=%zu\n",
           m_node->GetId(), (unsigned long long)m_pnpCount, (unsigned long long)m_trpCount,
           (unsigned long long)m_enterCount, (unsigned long long)m_exitCount,
           (unsigned long long)m_passCount, (unsigned long long)m_dropCount,
           (unsigned long long)m_recoveryCount, (unsigned long long)m_peakBytes,
           (unsigned long long)m_dataBytes, (unsigned long long)m_dummyBytes, m_states.size());
}

} // namespace ns3
