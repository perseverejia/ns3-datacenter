#pragma once

#include "ns3/grc-settings.h"
#include "ns3/ipv4-address.h"
#include "ns3/node-container.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace ns3
{
// Installs only GRC. Topology/DC roles and all non-GRC settings belong to the caller.
class GrcHelper
{
  public:
    static void Install(uint32_t ccMode,
                        const NodeContainer& nodes,
                        const std::vector<Ipv4Address>& hostAddresses,
                        const std::map<uint32_t, uint32_t>& hostToDc,
                        const std::map<uint32_t, uint32_t>& switchToDc,
                        const std::set<uint32_t>& dciSwitches,
                        const std::map<std::string, std::string>& parameters,
                        uint32_t seed);
};
} // namespace ns3
