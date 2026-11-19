#include "measurements.h"

#include "ns3/abort.h"
#include "ns3/qbb-net-device.h"
#include "ns3/rdma-driver.h"
#include "ns3/simulator.h"
#include "ns3/switch-node.h"

#include <fstream>

using namespace ns3;

namespace
{
struct Counter
{
    MeasuredFlow flow;
    Ipv4Address sip, dip;
    Ptr<RdmaHw> receiver;
    Ptr<RdmaRxQueuePair> rx;
    uint64_t qpBaseRttNs = 0;
    int64_t firstTxNs = -1;
    bool firstAckRecorded = false;
    uint64_t txPayload = 0;
    bool arrivedAtDci = false;
};

std::vector<Counter> counters;
std::ofstream rateFile, queueFile, arrivalFile, rttFile;

Ptr<QbbNetDevice>
PortTo(const NodeContainer& nodes, uint32_t node, uint32_t peer)
{
    auto local = nodes.Get(node);
    for (uint32_t i = 0; i < local->GetNDevices(); ++i)
    {
        auto port = DynamicCast<QbbNetDevice>(local->GetDevice(i));
        if (!port) continue;
        auto link = port->GetChannel();
        for (uint32_t j = 0; j < link->GetNDevices(); ++j)
            if (link->GetDevice(j)->GetNode()->GetId() == peer) return port;
    }
    NS_ABORT_MSG("Missing motivation topology edge " << node << " -> " << peer);
    return nullptr;
}

void
CountTx(Counter* counter, Ptr<const Packet>, Ptr<RdmaQueuePair> qp)
{
    const auto& f = counter->flow;
    if (qp->sip == counter->sip && qp->dip == counter->dip && qp->sport == f.port &&
        qp->dport == f.dport && qp->m_pg == f.pg) {
        if (counter->firstTxNs < 0) {
            counter->firstTxNs = Simulator::Now().GetNanoSeconds();
            counter->qpBaseRttNs = qp->m_baseRtt;
        }
        counter->txPayload += qp->lastPayloadSize;
    }
}

void
FirstProgressAck(Counter* counter, Ptr<const Packet> packet)
{
    if (counter->firstAckRecorded || counter->firstTxNs < 0) return;
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    packet->PeekHeader(ch);
    const auto& f = counter->flow;
    if (ch.l3Prot != 0xFC || ch.ack.seq == 0 || ch.sip != counter->dip.Get() ||
        ch.dip != counter->sip.Get() || ch.ack.sport != f.dport ||
        ch.ack.dport != f.port || ch.ack.pg != f.pg) return;
    counter->firstAckRecorded = true;
    const auto now = Simulator::Now().GetNanoSeconds();
    rttFile << f.src << ',' << f.dst << ',' << f.pathRttNs << ',' << counter->qpBaseRttNs
            << ',' << counter->firstTxNs << ',' << now << ',' << (now - counter->firstTxNs) << '\n';
}

void
FirstWanArrival(Counter* counter, Ptr<const Packet> packet)
{
    if (counter->arrivedAtDci) return;
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    packet->PeekHeader(ch);
    const auto& f = counter->flow;
    if (ch.l3Prot != 0x11 || ch.sip != counter->sip.Get() || ch.dip != counter->dip.Get() ||
        ch.udp.sport != f.port || ch.udp.dport != f.dport || ch.udp.pg != f.pg) return;
    counter->arrivedAtDci = true;
    arrivalFile << f.src << ',' << Simulator::Now().GetNanoSeconds() << '\n';
}

void
SampleCounters()
{
    auto ns = Simulator::Now().GetNanoSeconds();
    for (auto& counter : counters)
    {
        const auto& f = counter.flow;
        // Keep the live counter readable after the final ACK removes it from the RDMA map.
        if (!counter.rx)
            counter.rx = counter.receiver->GetRxQp(counter.dip.Get(), counter.sip.Get(),
                                                   f.dport, f.port, f.pg, false);
        rateFile << ns << ',' << f.src << ',' << counter.txPayload << ','
                 << (counter.rx ? counter.rx->m_recv_bytes : 0) << '\n';
    }
    Simulator::Schedule(MicroSeconds(50), &SampleCounters);
}

void
SampleQueues(Ptr<QbbNetDevice> dci, Ptr<QbbNetDevice> leaf, Ipv4Address destination)
{
    auto receiver = DynamicCast<SwitchNode>(dci->GetNode());
    auto themis = receiver->GetThemisGateway();
    const auto& atc = receiver->m_atcGateway;
    const auto voq = atc.destToVoqMap.find(destination);
    uint64_t voqBytes = voq == atc.destToVoqMap.end() ? 0 : atc.voqPool.at(voq->second).currentSize;
    uint64_t loopFifo = themis ? themis->GetLoopQueueBytes() : 0;
    uint64_t dciEgress = dci->GetQueue()->GetNBytesTotal();
    queueFile << Simulator::Now().GetNanoSeconds() << ',' << dciEgress
              << ',' << leaf->GetQueue()->GetNBytesTotal() << ',' << loopFifo
              << ',' << (themis ? themis->GetLoopDataBytes() : 0)
              << ',' << (themis ? themis->GetLoopDummyBytes() : 0)
              << ',' << voqBytes << ',' << dciEgress + loopFifo + voqBytes << '\n';
    if (Simulator::Now() < MilliSeconds(10))
        Simulator::Schedule(MicroSeconds(10), &SampleQueues, dci, leaf, destination);
}

} // namespace

void
InstallMeasurements(const NodeContainer& nodes, const std::vector<MeasuredFlow>& flows,
                    const std::vector<Ipv4Address>& addresses)
{
    rateFile.open("counters.csv");
    queueFile.open("queues.csv");
    arrivalFile.open("arrivals.csv");
    rttFile.open("rtt.csv");
    NS_ABORT_MSG_IF(!rateFile || !queueFile || !arrivalFile || !rttFile, "Cannot write motivation measurements");
    rttFile << "src,dst,path_rtt_ns,qp_base_rtt_ns,first_tx_ns,first_ack_ns,first_ack_rtt_ns\n";
    rateFile << "time_ns,src,tx_payload_bytes,rx_inorder_bytes\n";
    queueFile << "time_ns,dci_11_to_8_bytes,leaf_8_to_4_bytes,themis_loop_fifo_bytes,"
                 "themis_loop_resident_data_bytes,themis_loop_resident_dummy_bytes,"
                 "atc_voq_to_4_bytes,dci_queued_bytes\n";
    arrivalFile << "src,first_dci_rx_ns\n";

    counters.reserve(flows.size()); // bound callbacks retain pointers to vector elements
    for (const auto& f : flows)
    {
        const bool fromDc0 = f.src == 0 || f.src == 1;
        const bool fromDc1 = f.src == 2 || f.src == 3;
        NS_ABORT_MSG_IF(f.dst != 4 || (!fromDc0 && !fromDc1 && f.src != 5),
                        "Motivation probes support hosts 0, 1, 2, 3, 5 -> 4");
        auto& counter = counters.emplace_back(Counter{f, addresses.at(f.src), addresses.at(f.dst),
                                                       nodes.Get(f.dst)->GetObject<RdmaDriver>()->m_rdma,
                                                       nullptr});
        uint32_t sourceLeaf = fromDc0 ? 6 : fromDc1 ? 7 : 8;
        auto sourceNic = PortTo(nodes, f.src, sourceLeaf);
        sourceNic->TraceConnectWithoutContext("RdmaQpDequeue", MakeBoundCallback(&CountTx, &counter));
        sourceNic->TraceConnectWithoutContext("MacRx", MakeBoundCallback(&FirstProgressAck, &counter));
        if (f.src != 5)
            PortTo(nodes, 11, fromDc0 ? 9 : 10)->TraceConnectWithoutContext(
                "MacRx", MakeBoundCallback(&FirstWanArrival, &counter));
    }
    SampleQueues(PortTo(nodes, 11, 8), PortTo(nodes, 8, 4), addresses.at(4));
    SampleCounters();
}
