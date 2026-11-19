// Fixed-flow motivation experiments on the GRC topology.
#include "measurements.h"

#include "ns3/core-module.h"
#include "ns3/error-model.h"
#include "ns3/grc-helper.h"
#include "ns3/internet-module.h"
#include "ns3/qbb-helper.h"
#include "ns3/rdma-client-helper.h"
#include "ns3/rdma-driver.h"
#include "ns3/switch-node.h"
#include "ns3/themis-helper.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <queue>
#include <set>
#include <sstream>

using namespace ns3;
extern std::set<uint32_t> qlen_mon_switches;

namespace
{
using Settings = std::map<std::string, std::string>;

Settings
ReadConfig(const std::string& file)
{
    std::ifstream input(file);
    NS_ABORT_MSG_IF(!input, "Cannot open " << file);
    Settings settings;
    for (std::string line; std::getline(input, line);) {
        std::istringstream row(line.substr(0, line.find('#')));
        std::string key, value;
        if (!(row >> key)) continue;
        std::getline(row >> std::ws, value);
        NS_ABORT_MSG_IF(value.empty() || !settings.emplace(key, value).second, "Invalid setting " << key);
    }
    return settings;
}

template <typename T>
std::vector<T>
ReadList(const Settings& cfg, const std::string& key)
{
    std::istringstream row(cfg.at(key));
    std::vector<T> items;
    for (T value; row >> value;) items.push_back(value);
    NS_ABORT_MSG_IF(!row.eof() || items.empty(), "Invalid list " << key);
    return items;
}

std::map<uint64_t, double>
ReadThresholds(const Settings& cfg, const std::string& key)
{
    std::istringstream row(cfg.at(key));
    std::map<uint64_t, double> thresholds;
    unsigned count;
    NS_ABORT_MSG_IF(!(row >> count), "Invalid " << key);
    for (unsigned i = 0; i < count; ++i) {
        uint64_t bps;
        double threshold;
        NS_ABORT_MSG_IF(!(row >> bps >> threshold), "Invalid " << key);
        thresholds[bps] = threshold;
    }
    return thresholds;
}

std::set<uint32_t>
ReadNodes(const Settings& cfg, const std::string& key)
{
    const auto ids = ReadList<uint32_t>(cfg, key);
    NS_ABORT_MSG_IF(ids.size() - 1 != ids.front(), "Invalid node count in " << key);
    return {ids.begin() + 1, ids.end()};
}

struct Edge {
    uint32_t peer, port;
    uint64_t delayNs, rate;
    bool wan;
};

NodeContainer nodes;
std::vector<std::vector<Edge>> edges;
std::vector<Ipv4Address> addresses;
std::map<uint32_t, uint32_t> hosts, switches;
std::set<uint32_t> dci;
std::vector<MeasuredFlow> flows;
std::map<std::pair<uint32_t, uint32_t>, uint64_t> pathRtt, pathRate;
uint64_t maxRtt = 0, maxBdp = 0;
std::ofstream completions, fct, pfc, bottleneck;
unsigned finished = 0;
bool autoStop;

void
RecordPfc(Ptr<QbbNetDevice> dev, uint32_t type)
{
    auto node = dev->GetNode();
    pfc << Simulator::Now().GetNanoSeconds() << ' ' << node->GetId() << ' '
        << node->GetNodeType() << ' ' << dev->GetIfIndex() << ' ' << type << '\n';
}

void
BuildTopology(const Settings& cfg)
{
    std::ifstream input(cfg.at("TOPOLOGY_FILE"));
    uint32_t count, switchCount, leaves, links, wanCount;
    NS_ABORT_MSG_IF(!(input >> count >> switchCount >> leaves >> links >> wanCount), "Invalid topology header");
    std::set<uint32_t> switchIds, wanLinks;
    for (auto entry : {std::make_pair(switchCount, &switchIds), std::make_pair(wanCount, &wanLinks)}) {
        for (unsigned i = 0; i < entry.first; ++i) {
            uint32_t id;
            NS_ABORT_MSG_IF(!(input >> id) || !entry.second->insert(id).second, "Invalid topology ID");
        }
    }
    edges.resize(count);
    addresses.resize(count);
    for (uint32_t id = 0; id < count; ++id) {
        if (switchIds.count(id)) {
            auto sw = CreateObject<SwitchNode>();
            sw->SetNodeType(1);
            sw->SetAttribute("EcnEnabled", BooleanValue(std::stoi(cfg.at("ENABLE_QCN"))));
            nodes.Add(sw);
        } else {
            nodes.Add(CreateObject<Node>());
        }
    }
    InternetStackHelper stack;
    stack.SetRoutingHelper(Ipv4GlobalRoutingHelper());
    stack.Install(nodes);
    auto errors = CreateObject<RateErrorModel>();
    auto errorRng = CreateObject<UniformRandomVariable>();
    errorRng->SetStream(50);
    errors->SetRandomVariable(errorRng);
    errors->SetAttribute("ErrorRate", DoubleValue(std::stod(cfg.at("ERROR_RATE_PER_LINK"))));
    errors->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));
    QbbHelper qbb;
    qbb.SetDeviceAttribute("ReceiveErrorModel", PointerValue(errors));
    Ipv4AddressHelper ipv4;
    for (unsigned i = 0; i < links; ++i) {
        uint32_t a, b;
        std::string rate, delay;
        double error;
        NS_ABORT_MSG_IF(!(input >> a >> b >> rate >> delay >> error) || a >= count || b >= count ||
                        a == b || error != 0, "Invalid motivation link " << i);
        qbb.SetDeviceAttribute("DataRate", StringValue(rate));
        qbb.SetChannelAttribute("Delay", StringValue(delay));
        auto devices = qbb.Install(nodes.Get(a), nodes.Get(b));
        const bool wan = wanLinks.count(i);
        for (unsigned j = 0; j < 2; ++j) {
            const auto id = j == 0 ? a : b;
            auto dev = DynamicCast<QbbNetDevice>(devices.Get(j));
            if (wan) {
                dci.insert(id);
                dev->SetWanPort(true);
            }
            if (!switchIds.count(id)) {
                auto ip = nodes.Get(id)->GetObject<Ipv4>();
                auto iface = ip->AddInterface(dev);
                ip->AddAddress(iface, Ipv4InterfaceAddress(Ipv4Address("10.0.0.1"), Ipv4Mask("/8")));
            }
            edges[id].push_back({j == 0 ? b : a, dev->GetIfIndex(),
                                 static_cast<uint64_t>(Time(delay).GetNanoSeconds()), DataRate(rate).GetBitRate(), wan});
            dev->TraceConnectWithoutContext("QbbPfc", MakeBoundCallback(&RecordPfc, dev));
        }
        ipv4.SetBase(Ipv4Address(0x0a000000u | ((i / 254 + 1) << 16) | ((i % 254 + 1) << 8)), Ipv4Mask("/24"));
        ipv4.Assign(devices);
    }

    // Each connected component of the LAN links is one DC, numbered by first leaf.
    std::map<uint32_t, uint32_t> dc;
    uint32_t nextDc = 0;
    for (auto root : switchIds) {
        if (dc.count(root)) continue;
        dc[root] = nextDc++;
        std::queue<uint32_t> pending;
        pending.push(root);
        while (!pending.empty()) {
            auto id = pending.front();
            pending.pop();
            for (const auto& e : edges[id]) {
                if (!e.wan && dc.emplace(e.peer, dc.at(id)).second) pending.push(e.peer);
            }
        }
    }
    std::map<uint32_t, uint32_t> local;
    for (uint32_t id = 0; id < count; ++id) {
        auto as = dc.at(id);
        addresses[id] = Ipv4Address(0x0b000000u | (as << 16) | ++local[as]);
        if (auto sw = DynamicCast<SwitchNode>(nodes.Get(id))) {
            switches[id] = as;
            sw->SetSwitchRealIp(addresses[id]);
        } else {
            hosts[id] = as;
        }
    }
}

void
ConfigureSwitches(const Settings& cfg, uint32_t mode, const std::string& configFile)
{
    const auto senders = ReadNodes(cfg, "GATEWAY_SRC"), receivers = ReadNodes(cfg, "GATEWAY_DST");
    const auto taxNodes = ReadNodes(cfg, "DCIEXT_SENDER_TAX_NODES");
    const auto kmin = ReadThresholds(cfg, "KMIN_MAP"), kmax = ReadThresholds(cfg, "KMAX_MAP");
    const auto pmax = ReadThresholds(cfg, "PMAX_MAP");
    const uint64_t buffer = std::stoull(cfg.at("BUFFER_SIZE")) << 20;
    for (const auto& [id, as] : switches) {
        auto sw = DynamicCast<SwitchNode>(nodes.Get(id));
        auto mmu = sw->m_mmu;
        mmu->SetAlphaIngress(std::stod(cfg.at("PFC_INGRESS_ALPHA")));
        mmu->SetAlphaEgress(UINT16_MAX);
        uint64_t totalHeadroom = 0;
        bool bridgeRx = mode == 16 && receivers.count(id);
        for (const auto& e : edges[id]) {
            mmu->ConfigEcn(e.port, kmin.at(e.rate), kmax.at(e.rate), pmax.at(e.rate));
            uint64_t headroom = bridgeRx ? (20u << 20) : e.rate * e.delayNs / 8 / 1000000000 * (dci.count(id) ? 3 : 40);
            for (unsigned q = 0; q < SwitchMmu::qCnt; ++q) mmu->SetHeadroom(headroom, e.port, q);
            totalHeadroom += headroom * SwitchMmu::qCnt;
            if (bridgeRx && e.wan) sw->DisablePfcOnPort(e.port);
        }
        mmu->SetBufferPool(buffer + totalHeadroom);
        mmu->SetIngressPool(std::stoull(cfg.at("PFC_INGRESS_POOL_MB")) << 20);
        mmu->SetEgressLosslessPool(buffer);
        mmu->node_id = id;
        if (mode == 15 && (senders.count(id) || receivers.count(id))) {
            const uint32_t role = receivers.count(id) ? 2 : 1;
            sw->SetConfigFilePath(configFile);
            sw->SetAttribute("GatewayType", UintegerValue(role));
            sw->ReadAtcConfigFromFile(configFile);
            sw->InitializeAtcGateway(role);
        }
        if (mode == 16 && (taxNodes.count(id) || bridgeRx)) {
            sw->InitializeDciExt(id, std::stoul(cfg.at("DCIEXT_BURST_INTERVAL_US")), std::stod(cfg.at("DCIEXT_T_US")),
                                 edges.at(hosts.begin()->first).front().rate, std::stoull(cfg.at("DCIEXT_OBSERVE_MB")),
                                 std::stoul(cfg.at("DCIEXT_SILENT_US")), std::stoul(cfg.at("DCIEXT_AGING_PERIODS")),
                                 !bridgeRx, std::stod(cfg.at("DCIEXT_DT_US")), std::stod(cfg.at("DCIEXT_SIGMA_US")));
            if (bridgeRx) sw->SetAttribute("EcnEnabled", BooleanValue(false));
        }
    }
}

void
Complete(Ptr<RdmaQueuePair> qp)
{
    auto id = [](Ipv4Address address) {
        return static_cast<uint32_t>(std::find(addresses.begin(), addresses.end(), address) - addresses.begin());
    };
    const auto src = id(qp->sip), dst = id(qp->dip);
    const auto start = qp->startTime.GetNanoSeconds(), duration = (Simulator::Now() - qp->startTime).GetNanoSeconds();
    completions << src << ' ' << dst << ' ' << qp->m_pg << ' ' << qp->dport << ' ' << qp->m_size
                << ' ' << start << ' ' << duration << ' ' << (hosts.at(src) != hosts.at(dst)) << '\n';
    const auto mtu = nodes.Get(src)->GetObject<RdmaDriver>()->m_rdma->m_mtu;
    const auto header = CustomHeader::GetStaticWholeHeaderSize() - IntHeader::GetStaticSize();
    auto serialization = [&](uint64_t bytes) { return std::llround(8e9L * (bytes + header) / pathRate.at({src, dst})); };
    uint64_t standalone = pathRtt.at({src, dst}) + (qp->m_size / mtu) * serialization(mtu);
    if (qp->m_size % mtu) standalone += serialization(qp->m_size % mtu);
    fct << std::hex << std::setfill('0') << std::setw(8) << qp->sip.Get() << ' ' << std::setw(8) << qp->dip.Get()
        << std::dec << ' ' << qp->sport << ' ' << qp->dport << ' ' << qp->m_size << ' ' << start << ' '
        << duration << ' ' << standalone << " 1\n";
    nodes.Get(dst)->GetObject<RdmaDriver>()->m_rdma->DeleteRxQp(qp->sip.Get(), qp->m_pg, qp->sport);
    if (++finished == flows.size() && autoStop) Simulator::Stop(MilliSeconds(1));
}

void
InstallHosts(const Settings& cfg, uint32_t mode)
{
    const std::map<std::string, std::string> attributes = {
        {"CLAMP_TARGET_RATE", "ClampTargetRate"}, {"ALPHA_RESUME_INTERVAL", "AlphaResumInterval"},
        {"RP_TIMER", "RPTimer"}, {"FAST_RECOVERY_TIMES", "FastRecoveryTimes"}, {"EWMA_GAIN", "EwmaGain"},
        {"DCQCN_BYTE_RECOVERY_ENABLED", "DcqcnByteRecoveryEnabled"}, {"DCQCN_BYTE_RECOVERY_THRESHOLD", "DcqcnByteRecoveryThreshold"},
        {"RATE_AI", "RateAI"}, {"RATE_HAI", "RateHAI"}, {"L2_BACK_TO_ZERO", "L2BackToZero"},
        {"L2_CHUNK_SIZE", "L2ChunkSize"}, {"L2_ACK_INTERVAL", "L2AckInterval"}, {"RATE_DECREASE_INTERVAL", "RateDecreaseInterval"},
        {"MIN_RATE", "MinRate"}, {"PACKET_PAYLOAD_SIZE", "Mtu"}, {"VAR_WIN", "VarWin"}, {"RATE_BOUND", "RateBound"}
    };
    for (const auto& [key, name] : attributes)
        Config::SetDefault("ns3::RdmaHw::" + name, StringValue(cfg.at(key)));
    for (const auto& [id, as] : hosts) {
        auto hw = CreateObject<RdmaHw>();
        hw->SetAttribute("CcMode", UintegerValue(mode));
        auto driver = CreateObject<RdmaDriver>();
        auto node = nodes.Get(id);
        driver->SetNode(node);
        driver->SetRdmaHw(hw);
        node->AggregateObject(driver);
        driver->Init();
        driver->TraceConnectWithoutContext("QpComplete", MakeCallback(&Complete));
    }
    RdmaEgressQueue::ack_q_idx = 3;
}

void
InstallRoutes()
{
    // The reference topology is a tree. Walk outwards from each destination.
    for (const auto& [dst, as] : hosts) {
        std::queue<uint32_t> pending;
        std::set<uint32_t> seen{dst};
        pending.push(dst);
        pathRate[{dst, dst}] = UINT64_MAX;
        while (!pending.empty()) {
            auto id = pending.front();
            pending.pop();
            for (const auto& e : edges[id]) {
                if (!seen.insert(e.peer).second) continue;
                const auto& reverse = edges[e.peer];
                auto port = std::find_if(reverse.begin(), reverse.end(), [&](const Edge& r) { return r.peer == id; })->port;
                const auto path = std::make_pair(e.peer, dst);
                auto rtt = pathRtt[path] = pathRtt[{id, dst}] + 2 * e.delayNs;
                auto rate = pathRate[path] = std::min(pathRate.at({id, dst}), e.rate);
                if (auto sw = DynamicCast<SwitchNode>(nodes.Get(e.peer))) {
                    sw->AddTableEntry(addresses[dst], port);
                    pending.push(e.peer);
                } else {
                    nodes.Get(e.peer)->GetObject<RdmaDriver>()->m_rdma->AddTableEntry(addresses[dst], port);
                    maxRtt = std::max(maxRtt, rtt);
                    maxBdp = std::max(maxBdp, rtt * rate / 1000000000 / 8);
                }
            }
        }
    }
}

void
SampleBottleneck(Time interval)
{
    uint32_t switchId = 0, port = 0, maximum = 0;
    for (const auto& [id, as] : switches) {
        if (dci.count(id)) continue;
        auto mmu = DynamicCast<SwitchNode>(nodes.Get(id))->m_mmu;
        for (const auto& e : edges[id]) {
            uint32_t bytes = 0;
            for (unsigned q = 0; q < SwitchMmu::qCnt; ++q) bytes += mmu->egress_bytes[e.port][q];
            if (bytes > maximum) { maximum = bytes; switchId = id; port = e.port; }
        }
    }
    bottleneck << Simulator::Now().GetNanoSeconds() << ' ' << switchId << ' ' << port << ' ' << maximum << '\n';
    Simulator::Schedule(interval, &SampleBottleneck, interval);
}

void
InstallFlows(const Settings& cfg, Ptr<UniformRandomVariable> ports)
{
    std::map<std::pair<uint32_t, uint32_t>, uint16_t> nextPort;
    for (const auto& [src, srcDc] : hosts)
        for (const auto& [dst, dstDc] : hosts)
            if (src != dst) nextPort[{src, dst}] = ports->GetInteger(10000, 10999);
    std::ifstream input(cfg.at("QUERY_FLOW_FILE"));
    uint32_t count;
    NS_ABORT_MSG_IF(!(input >> count) || count == 0, "Invalid flow count");
    for (uint32_t i = 0; i < count; ++i) {
        MeasuredFlow f{};
        uint64_t bytes;
        double offset;
        NS_ABORT_MSG_IF(!(input >> f.src >> f.dst >> f.pg >> f.dport >> bytes >> offset) || f.pg >= 8 ||
                        f.dport == 0 || bytes == 0 || bytes > UINT32_MAX || offset < 0 || !std::isfinite(offset) ||
                        hosts.at(f.dst) != std::stoul(cfg.at("FILE_DESTINATION_DC")), "Invalid flow " << i);
        const auto pair = std::make_pair(f.src, f.dst);
        f.port = nextPort.at(pair)++;
        f.pathRttNs = pathRtt.at(pair);
        bool global = std::stoi(cfg.at("GLOBAL_T"));
        auto rtt = global ? maxRtt : f.pathRttNs;
        auto window = global ? maxBdp : rtt * pathRate.at(pair) / 1000000000 / 8;
        RdmaClientHelper client(f.pg, addresses[f.src], addresses[f.dst], f.port, f.dport, bytes,
                                std::stoi(cfg.at("HAS_WIN")) ? window : 0, rtt, Simulator::GetMaximumSimulationTime());
        client.Install(nodes.Get(f.src)).Start(Seconds(std::stod(cfg.at("START_TIME")) + offset));
        flows.push_back(f);
    }
    std::string extra;
    NS_ABORT_MSG_IF(input >> extra, "Unexpected extra flow");
}
} // namespace

int
main(int argc, char* argv[])
{
    std::string configFile = "config.txt";
    uint32_t mode = 1, seed = 7;
    CommandLine cmd;
    cmd.AddValue("algorithm", "CC mode for the experiment", mode);
    cmd.AddValue("conf", "Experiment config file", configFile);
    cmd.AddValue("randomSeed", "ns-3 random seed", seed);
    cmd.Parse(argc, argv);
    const auto cfg = ReadConfig(configFile);
    NS_ABORT_MSG_IF(!(mode == 1 || mode == 15 || mode == 16 || mode == 18 || mode == 19) ||
                    std::stod(cfg.at("LOAD")) != 0, "Expected a motivation algorithm and fixed flows");
    RngSeedManager::SetSeed(seed);
    auto ports = CreateObject<UniformRandomVariable>();
    ports->SetStream(1000);
    // Preserve the random stream numbering used for ECN in the original runs.
    // Three other traffic-generator streams were allocated before building the network.
    for (int i = 0; i < 3; ++i) RngSeedManager::GetNextStreamIndex();
    IntHeader::mode = IntHeader::NONE;
    Config::SetDefault("ns3::QbbNetDevice::PauseTime", UintegerValue(5));
    Config::SetDefault("ns3::QbbNetDevice::QcnEnabled", BooleanValue(std::stoi(cfg.at("ENABLE_QCN"))));
    Config::SetDefault("ns3::QbbNetDevice::NicDelay", TimeValue(NanoSeconds(std::stoull(cfg.at("NIC_DELAY")))));
    for (auto id : ReadList<uint32_t>(cfg, "QLEN_MON_SWITCHES")) qlen_mon_switches.insert(id);
    pfc.open("pfc.txt");
    fct.open("fct.txt");
    completions.open("flows-fct.txt");
    bottleneck.open(cfg.at("BOTTLENECK_MON_FILE"));
    NS_ABORT_MSG_IF(!pfc || !fct || !completions || !bottleneck, "Cannot open experiment output");
    completions << "# File Flow FCT Results\n# Format: src_node dst_node pg dport flow_size(B) start_time(ns) fct(ns) is_cross_dc\n"
                << "# Note: FCT is raw (no compensation) for all flows\n";
    autoStop = std::stoi(cfg.at("QUERY_AUTO_STOP"));
    BuildTopology(cfg);
    ConfigureSwitches(cfg, mode, configFile);
    InstallHosts(cfg, mode);
    InstallRoutes();
    for (const auto& [id, as] : switches) {
        auto sw = DynamicCast<SwitchNode>(nodes.Get(id));
        sw->SetAttribute("CcMode", UintegerValue(mode));
        sw->SetAttribute("AckHighPrio", UintegerValue(1));
        sw->SetAttribute("MaxRtt", UintegerValue(maxRtt));
    }
    Settings grc, themis;
    for (const auto& [key, value] : cfg) {
        if (key.rfind("GRC_", 0) == 0) grc[key.substr(4)] = value;
        if (key.rfind("THEMIS_", 0) == 0) themis[key.substr(7)] = value;
    }
    if (mode == 18) GrcHelper::Install(mode, nodes, addresses, hosts, switches, dci, grc, seed);
    if (mode == 19) ThemisHelper::Install(mode, nodes, addresses, hosts, switches, dci, themis);
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();
    InstallFlows(cfg, ports);
    InstallMeasurements(nodes, flows, addresses);
    auto interval = NanoSeconds(std::stoull(cfg.at("BOTTLENECK_MON_INTERVAL")));
    NS_ABORT_MSG_IF(!interval.IsStrictlyPositive(), "Expected a positive sampling interval");
    Simulator::Schedule(interval, &SampleBottleneck, interval);
    Simulator::Stop(Seconds(std::stod(cfg.at("SIMULATOR_STOP_TIME"))));
    Simulator::Run();
    Simulator::Destroy();
    if (mode == 18) GrcSettings::Reset();
    return finished == flows.size() ? 0 : 1;
}
