#include "grc-settings.h"

#include "grc-wan-routing.h"
#include "rdma-queue-pair.h"

#include "ns3/abort.h"

#include <filesystem>
#include <tuple>

namespace ns3
{
bool GrcSettings::enabled = false;
std::map<uint32_t, uint32_t> GrcSettings::hostIpToDc;
std::map<std::string, std::string> GrcSettings::raw;

namespace
{
using FlowKey = std::tuple<uint32_t, uint32_t, uint16_t, uint16_t, uint16_t>;
std::map<FlowKey, uint32_t> flowIds;
} // namespace

bool
GrcSettings::IsEnabled(const std::string& v)
{
    return v == "TRUE" || v == "true" || v == "1" || v == "YES" || v == "yes" || v == "ON" ||
           v == "on";
}

uint32_t
GrcSettings::FlowId(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint16_t pg)
{
    auto it = flowIds.emplace(FlowKey{sip, dip, sport, dport, pg}, flowIds.size()).first;
    return it->second;
}

void
GrcSettings::Reset()
{
    enabled = false;
    hostIpToDc.clear();
    raw.clear();
    flowIds.clear();
    GrcWanRouting::s_w_k_update_scheduled = false;
    GrcWanRouting::s_w_k_initialized = false;
    GrcWanRouting::s_w_k = 0;
    GrcWanRouting::s_w_k_samples.clear();
    GrcWanRouting::epoch_duration = MicroSeconds(1000);
    GrcLog::Close();
}

FILE *GrcLog::rate_monitor = nullptr, *GrcLog::workload_log = nullptr,
     *GrcLog::completion_log = nullptr;
FILE *GrcLog::events = nullptr, *GrcLog::controller = nullptr;

void
GrcLog::Open(const std::string& dir)
{
    NS_ABORT_MSG_IF(rate_monitor, "GRC logs already open");
    std::filesystem::create_directories(dir);
    auto open = [&](const char* name, const char* header) {
        FILE* file = fopen((std::filesystem::path(dir) / name).c_str(), "w");
        NS_ABORT_MSG_IF(!file, "Cannot open GRC log " << name);
        fputs(header, file);
        return file;
    };
    rate_monitor = open("rates.csv", "time_ns,src_as,dst_as,epoch_send_Bps,ref_Bps,w,k,nhat\n");
    workload_log = open("workload.csv", "flow,sip,dip,sport,dport,pg,bytes,start_ns\n");
    completion_log = open("completions.csv", "flow,finish_ns,fct_ns,bytes\n");
    if (GrcSettings::IsEnabled(GrcSettings::raw.at("TRACE_EVENTS")))
    {
        events = open("events.csv",
                      "time_ns,event,node,flow,sip,dip,sport,dport,pg,packet_uid,seq,reason,"
                      "rate_before_bps,rate_after_bps,target_bps,alpha,value\n");
        controller = open("controller.csv",
                          "time_ns,node,src_as,dst_as,epoch_send_Bps,ref_before_Bps,ref_after_Bps,"
                          "rtt_samples,epoch_rtt_ns,sensitive_rtt_ns,base_rtt_ns,queue_delay_ns,"
                          "w,k,nhat,packet_count,cnp_count,start_bytes,end_bytes,cur_bytes,state,"
                          "consecutive_epochs\n");
    }
}

void
GrcLog::Close()
{
    for (FILE** file : {&rate_monitor, &workload_log, &completion_log, &events, &controller})
        if (*file)
        {
            fclose(*file);
            *file = nullptr;
        }
}

void
GrcLog::Event(const char* event,
              uint32_t node,
              uint32_t sip,
              uint32_t dip,
              uint16_t sport,
              uint16_t dport,
              uint16_t pg,
              uint64_t packetUid,
              uint64_t seq,
              const char* reason,
              uint64_t rateBefore,
              uint64_t rateAfter,
              uint64_t target,
              double alpha,
              double value)
{
    if (!events)
        return;
    fprintf(events,
            "%lld,%s,%u,%u,%u,%u,%u,%u,%u,%llu,%llu,%s,%llu,%llu,%llu,%.12g,%.12g\n",
            static_cast<long long>(Simulator::Now().GetNanoSeconds()),
            event,
            node,
            GrcSettings::FlowId(sip, dip, sport, dport, pg),
            sip,
            dip,
            sport,
            dport,
            pg,
            static_cast<unsigned long long>(packetUid),
            static_cast<unsigned long long>(seq),
            reason,
            static_cast<unsigned long long>(rateBefore),
            static_cast<unsigned long long>(rateAfter),
            static_cast<unsigned long long>(target),
            alpha,
            value);
}

void
GrcLog::Completed(Ptr<RdmaQueuePair> qp)
{
    if (!completion_log)
        return;
    fprintf(completion_log,
            "%u,%lld,%lld,%llu\n",
            GrcSettings::FlowId(qp->sip.Get(), qp->dip.Get(), qp->sport, qp->dport, qp->m_pg),
            static_cast<long long>(Simulator::Now().GetNanoSeconds()),
            static_cast<long long>((Simulator::Now() - qp->startTime).GetNanoSeconds()),
            static_cast<unsigned long long>(qp->m_size));
}
} // namespace ns3
