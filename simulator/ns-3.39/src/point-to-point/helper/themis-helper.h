#pragma once

#include "ns3/ipv4-address.h"
#include "ns3/node-container.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace ns3
{
// Uses the experiment's inferred DC roles and ordinary SwitchNode routes.
class ThemisHelper
{
  public:
    static void Install(uint32_t ccMode,
                        const NodeContainer& nodes,
                        const std::vector<Ipv4Address>& hostAddresses,
                        const std::map<uint32_t, uint32_t>& hostToDc,
                        const std::map<uint32_t, uint32_t>& switchToDc,
                        const std::set<uint32_t>& dciSwitches,
                        const std::map<std::string, std::string>& parameters);
};
} // namespace ns3
