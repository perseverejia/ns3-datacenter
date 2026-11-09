#include "grc-helper.h"

#include "ns3/abort.h"
#include "ns3/grc-settings.h"
#include "ns3/qbb-net-device.h"
#include "ns3/rdma-driver.h"
#include "ns3/switch-node.h"
#include "ns3/uinteger.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <queue>
#include <set>

namespace ns3
{
void
GrcHelper::Install(uint32_t ccMode,
                   const NodeContainer& nodes,
                   const std::vector<Ipv4Address>& addresses,
                   const std::map<uint32_t, uint32_t>& hostToDc,
                   const std::map<uint32_t, uint32_t>& switchToDc,
                   const std::set<uint32_t>& dciSwitches,
                   const std::map<std::string, std::string>& parameters,
                   uint32_t seed)
{
    // No global state, log files, callbacks or events for other CC modes.
    if (ccMode != 18) return;
    for (auto node = nodes.Begin(); node != nodes.End(); ++node) {
        if (auto sw = DynamicCast<SwitchNode>(*node)) {
            UintegerValue mode;
            sw->GetAttribute("CcMode", mode);
            NS_ABORT_MSG_IF(mode.Get() != 18, "GRC requires switch CcMode=18");
        }
    }
    NS_ABORT_MSG_IF(GrcSettings::enabled || Simulator::Now() != Seconds(0),
                    "Install GRC once at time zero");
    std::map<uint32_t, uint32_t> nodeToDc = switchToDc;
    nodeToDc.insert(hostToDc.begin(), hostToDc.end());
    NS_ABORT_MSG_IF(nodeToDc.size() != nodes.GetN(), "Every node needs an inferred DC");
    for (uint32_t id : dciSwitches)
        NS_ABORT_MSG_IF(!switchToDc.count(id), "DCI role assigned to a non-switch");
    GrcSettings::raw = {{"W_MAX", "4.0"},
                        {"W_K_SYNC_INTERVAL_MS", "30"},
                        {"BASE_RTT_WINDOW_MS", "10000"},
                        {"INV_DELTA", "6291456"},
                        {"BETA", "0.3"},
                        {"GUARANTEED_RATE_FACTOR", "0.25"},
                        {"WAN_EPOCH_US", "1000"},
                        {"RTT_EWMA_US", "1000"},
                        {"RTT_ENTRY_TIMEOUT_US", "30000"},
                        {"RTT_RTO_FILTER_US", "35000"},
                        {"EPOCH_START_NS", "0"},
                        {"GRC_SEED", "1"},
                        {"TRACE_EVENTS", "FALSE"},
                        {"LOG_DIR", "examples/CrossDC-Algos/results/grc"}};
    for (const auto& [key, value] : parameters)
    {
        NS_ABORT_MSG_IF(!GrcSettings::raw.count(key), "Unknown GRC parameter: " << key);
        GrcSettings::raw[key] = value;
    }
    GrcSettings::raw["GRC_SEED"] = std::to_string(seed);
    auto number = [](const char* key) {
        const auto& text = GrcSettings::raw.at(key);
        size_t used = 0;
        double value = std::stod(text, &used);
        NS_ABORT_MSG_IF(used != text.size() || !std::isfinite(value), "Invalid number: " << key);
        return value;
    };

    NS_ABORT_MSG_IF(
        number("EPOCH_START_NS") < 0 ||
            number("EPOCH_START_NS") != std::floor(number("EPOCH_START_NS")) ||
            number("WAN_EPOCH_US") < 1 ||
            number("WAN_EPOCH_US") != std::floor(number("WAN_EPOCH_US")) ||
            number("INV_DELTA") <= 0 || number("BETA") < 0 || number("BETA") >= 1 ||
            number("W_MAX") < 1 || number("GUARANTEED_RATE_FACTOR") <= 0 ||
            number("GUARANTEED_RATE_FACTOR") > 1 || number("W_K_SYNC_INTERVAL_MS") < 1 ||
            number("W_K_SYNC_INTERVAL_MS") != std::floor(number("W_K_SYNC_INTERVAL_MS")) ||
            number("BASE_RTT_WINDOW_MS") < number("WAN_EPOCH_US") / 1000.0 ||
            number("BASE_RTT_WINDOW_MS") != std::floor(number("BASE_RTT_WINDOW_MS")),
        "Invalid GRC numeric parameter");
    for (const char* key : {"RTT_EWMA_US", "RTT_ENTRY_TIMEOUT_US", "RTT_RTO_FILTER_US"})
        NS_ABORT_MSG_IF(number(key) < 1 || number(key) != std::floor(number(key)),
                        "Invalid GRC timing parameter: " << key);
    NS_ABORT_MSG_IF(number("RTT_ENTRY_TIMEOUT_US") >= number("RTT_RTO_FILTER_US"),
                    "GRC RTT entry timeout must be shorter than its RTO filter window");
    // The paper's complete controller always uses weighting, velocity, two-level sampling,
    // and ECN-to-CNP conversion. No ablation switches are exposed here.
    {
        const auto& value = GrcSettings::raw.at("TRACE_EVENTS");
        NS_ABORT_MSG_IF(!GrcSettings::IsEnabled(value) && value != "FALSE" && value != "false" &&
                            value != "0" && value != "NO" && value != "no" && value != "OFF" &&
                            value != "off",
                        "Invalid boolean: TRACE_EVENTS");
    }
    std::map<uint32_t, std::map<uint32_t, uint32_t>> routes;
    std::map<uint32_t, uint32_t> asDestination;
    std::map<uint32_t, std::map<uint32_t, uint32_t>> ports;
    for (uint32_t id = 0; id < nodes.GetN(); ++id)
    {
        Ptr<Node> node = nodes.Get(id);
        NS_ABORT_MSG_IF(node->GetId() != id, "GRC topology node IDs must match container indices");
        uint32_t dc = nodeToDc.at(id);
        if (hostToDc.count(id))
        {
            NS_ABORT_MSG_IF(node->GetNodeType() != 0 || id >= addresses.size(), "Invalid GRC host");
            NS_ABORT_MSG_IF(!GrcSettings::hostIpToDc.emplace(addresses[id].Get(), dc).second,
                            "Duplicate GRC host IP");
            auto driver = node->GetObject<RdmaDriver>();
            NS_ABORT_MSG_IF(driver && driver->m_rdma->m_cc_mode != 18,
                            "GRC requires the DCQCN endpoint path for CcMode=18");
            NS_ABORT_MSG_IF(driver && driver->m_rdma->m_ack_interval == 0,
                            "GRC requires ACKs for RTT measurement");
        }
        else
        {
            NS_ABORT_MSG_IF(!DynamicCast<SwitchNode>(node), "GRC switch role assigned to a host");
            if (dciSwitches.count(id))
            {
                NS_ABORT_MSG_IF(!asDestination.emplace(dc, id).second,
                                "Require one DCI per AS");
            }
        }
        for (uint32_t port = 0; port < node->GetNDevices(); ++port)
        {
            auto dev = DynamicCast<QbbNetDevice>(node->GetDevice(port));
            if (!dev || !dev->GetChannel())
                continue;
            auto channel = dev->GetChannel();
            NS_ABORT_MSG_IF(channel->GetNDevices() != 2, "GRC needs point-to-point links");
            auto other =
                channel->GetDevice(0) == dev ? channel->GetDevice(1) : channel->GetDevice(0);
            uint32_t next = other->GetNode()->GetId();
            NS_ABORT_MSG_IF(ports[id].count(next),
                            "Parallel links need an explicit routing policy");
            ports[id][next] = port;
        }
    }
    // Find DCI egress link rates for controller initialization; forwarding uses SwitchNode's routing table.
    for (const auto& [src, neighbors] : ports)
    {
        if (!dciSwitches.count(src))
            continue;
        std::map<uint32_t, uint32_t> parent{{src, src}};
        std::queue<uint32_t> q;
        q.push(src);
        while (!q.empty())
        {
            auto u = q.front();
            q.pop();
            for (const auto& [v, port] : ports[u])
                if (dciSwitches.count(v) && !parent.count(v))
                {
                    parent[v] = u;
                    q.push(v);
                }
        }
        for (const auto& [as, dst] : asDestination)
        {
            if (src == dst)
                continue;
            NS_ABORT_MSG_IF(!parent.count(dst), "Disconnected GRC DCI graph");
            uint32_t next = dst;
            while (parent.at(next) != src)
                next = parent.at(next);
            routes[src][as] = ports.at(src).at(next);
        }
    }
    for (const auto& [id, dc] : nodeToDc)
    {
        NS_ABORT_MSG_IF(!asDestination.count(dc), "AS has no DCI attachment");
        for (const auto& [next, port] : ports[id])
            NS_ABORT_MSG_IF(dc != nodeToDc.at(next) &&
                                !(dciSwitches.count(id) && dciSwitches.count(next)),
                            "Cross-AS links must join DCI switches");
    }
    for (const auto& [as, root] : asDestination)
    {
        std::set<uint32_t> reached{root};
        std::queue<uint32_t> q;
        q.push(root);
        while (!q.empty())
        {
            uint32_t id = q.front();
            q.pop();
            // Hosts must not become transit nodes when validating a DC's reachability.
            if (hostToDc.count(id))
                continue;
            for (const auto& [next, port] : ports[id])
                if (nodeToDc.at(next) == as && reached.insert(next).second)
                    q.push(next);
        }
        for (const auto& [id, dc] : nodeToDc)
            NS_ABORT_MSG_IF(dc == as && !reached.count(id),
                            "Disconnected intra-AS topology");
    }
    const auto& logDir = GrcSettings::raw.at("LOG_DIR");
    GrcLog::Open(logDir);
    for (auto node = nodes.Begin(); node != nodes.End(); ++node)
    {
        if (auto driver = (*node)->GetObject<RdmaDriver>())
            driver->TraceConnectWithoutContext("QpComplete", MakeCallback(&GrcLog::Completed));
    }
    std::ofstream resolved(std::filesystem::path(logDir) / "resolved-config.txt");
    for (const auto& [key, value] : GrcSettings::raw)
        resolved << key << ' ' << value << '\n';
    for (const auto& [id, dc] : nodeToDc)
        resolved << "NODE " << id << ' ' << dc << ' '
                 << (hostToDc.count(id) ? 0 : dciSwitches.count(id) ? 2 : 1) << '\n';
    for (const auto& [id, destinations] : routes)
        for (const auto& [as, port] : destinations)
            resolved << "ROUTE " << id << ' ' << as << ' ' << port << '\n';
    GrcSettings::enabled = true;
    for (uint32_t id : dciSwitches)
    {
        auto sw = DynamicCast<SwitchNode>(nodes.Get(id));
        // Retain the experiment's MMU, ECN, headroom and PFC configuration.
        sw->InitializeGrc(switchToDc.at(id), routes[id]);
    }
}
} // namespace ns3
