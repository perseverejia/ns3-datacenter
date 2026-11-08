#pragma once

#include "ns3/custom-header.h"
#include "ns3/nstime.h"

#include <cstdio>
#include <map>
#include <string>

namespace ns3
{
class RdmaQueuePair;

// A scoped adapter for the upstream Settings dependency. No endpoint CNP policy lives here.
struct GrcSettings
{
    static bool enabled;
    static std::map<uint32_t, uint32_t> hostIpToDc;
    static std::map<std::string, std::string> raw;
    static bool IsEnabled(const std::string& value);
    static uint32_t FlowId(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint16_t pg);
    // Call only after Simulator::Destroy(): release context and reset upstream global W/k state.
    static void Reset();
};

struct GrcLog
{
    static FILE *rate_monitor, *workload_log, *completion_log;
    // Optional observational traces. They never consume RNG draws or affect scheduling.
    static FILE *events, *controller;
    static void Open(const std::string& directory);
    static void Close();
    static void Completed(Ptr<RdmaQueuePair> qp);
    static void Event(const char* event,
                      uint32_t node,
                      uint32_t sip,
                      uint32_t dip,
                      uint16_t sport,
                      uint16_t dport,
                      uint16_t pg,
                      uint64_t packetUid,
                      uint64_t seq,
                      const char* reason,
                      uint64_t rateBefore = 0,
                      uint64_t rateAfter = 0,
                      uint64_t target = 0,
                      double alpha = 0,
                      double value = 0);

};

} // namespace ns3
