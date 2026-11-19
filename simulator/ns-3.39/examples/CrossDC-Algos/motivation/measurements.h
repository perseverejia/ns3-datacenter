#pragma once

#include "ns3/ipv4-address.h"
#include "ns3/node-container.h"

#include <vector>

struct MeasuredFlow
{
    uint32_t src, dst;
    uint16_t pg, port, dport;
    uint64_t pathRttNs;
};

void InstallMeasurements(const ns3::NodeContainer& nodes, const std::vector<MeasuredFlow>& flows,
                         const std::vector<ns3::Ipv4Address>& addresses);
