#include "themis-helper.h"

#include "ns3/abort.h"
#include "ns3/rdma-driver.h"
#include "ns3/simulator.h"
#include "ns3/switch-node.h"
#include "ns3/themis-gateway.h"
#include "ns3/uinteger.h"

#include <cmath>

namespace ns3
{
void
ThemisHelper::Install(uint32_t ccMode,
                      const NodeContainer& nodes,
                      const std::vector<Ipv4Address>& addresses,
                      const std::map<uint32_t, uint32_t>& hostToDc,
                      const std::map<uint32_t, uint32_t>& switchToDc,
                      const std::set<uint32_t>& dciSwitches,
                      const std::map<std::string, std::string>& parameters)
{
    NS_ABORT_MSG_IF(Simulator::Now() != Seconds(0), "Install THEMIS at time zero");
    NS_ABORT_MSG_IF(dciSwitches.empty(), "THEMIS requires WAN links declared in the topology");
    ThemisSettings settings;
    for (const auto& [key, text] : parameters)
    {
        size_t used = 0;
        double value = std::stod(text, &used);
        NS_ABORT_MSG_IF(used != text.size() || !std::isfinite(value) || value < 0,
                        "Invalid THEMIS_" << key);
        if (key == "BETA_US") settings.beta = Seconds(value * 1e-6);
        else
        {
            NS_ABORT_MSG_IF(std::floor(value) != value || value > UINT32_MAX,
                            "THEMIS_" << key << " must be a 32-bit unsigned integer");
            if (key == "ALPHA") settings.alpha = value;
            else if (key == "DUMMY_PACKET_BYTES") settings.dummyPacketBytes = value;
            else if (key == "TRACE_EVENTS")
            {
                NS_ABORT_MSG_IF(value > 1, "THEMIS_TRACE_EVENTS must be 0 or 1");
                settings.traceEvents = value;
            }
            else NS_ABORT_MSG("Unknown THEMIS parameter: " << key);
        }
    }
    NS_ABORT_MSG_IF(settings.alpha < 2 || settings.beta <= Time(0), "Invalid THEMIS alpha/beta");
    NS_ABORT_MSG_IF(settings.dummyPacketBytes < 64 || settings.dummyPacketBytes > 65535,
                    "THEMIS dummy packet size must be 64..65535 bytes");
    std::map<uint32_t, uint32_t> hostIpToDc;
    std::vector<Ptr<RdmaDriver>> completionSources;
    for (const auto& [id, dc] : hostToDc)
    {
        NS_ABORT_MSG_IF(id >= nodes.GetN() || id >= addresses.size() ||
                            nodes.Get(id)->GetNodeType() != 0,
                        "Invalid THEMIS host/DC mapping");
        NS_ABORT_MSG_IF(!hostIpToDc.emplace(addresses[id].Get(), dc).second,
                        "Duplicate THEMIS host IP");
        auto driver = nodes.Get(id)->GetObject<RdmaDriver>();
        NS_ABORT_MSG_IF(!driver || !driver->m_rdma || driver->m_rdma->m_cc_mode != ccMode,
                        "THEMIS requires the existing DCQCN endpoint path");
        completionSources.push_back(driver);
    }
    for (uint32_t id : dciSwitches)
    {
        NS_ABORT_MSG_IF(id >= nodes.GetN() || !switchToDc.count(id),
                        "THEMIS DCI role assigned to a non-switch");
        auto sw = DynamicCast<SwitchNode>(nodes.Get(id));
        NS_ABORT_MSG_IF(!sw, "Invalid THEMIS DCI node");
        UintegerValue mode;
        sw->GetAttribute("CcMode", mode);
        NS_ABORT_MSG_IF(mode.Get() != ccMode, "THEMIS switch CC mode mismatch");
        auto gateway = sw->InitializeThemis(switchToDc.at(id), hostIpToDc, settings);
        for (const auto& driver : completionSources)
            gateway->ConnectCompletionTrace(driver);
    }
}
} // namespace ns3
