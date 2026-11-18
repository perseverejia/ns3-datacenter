/*
 * Workload Generator Implementation for CrossDC-Algos
 * 
 * 独立的流量生成模块实现
 * 按 ToR 上行负载生成 DC 内背景流，直接生成跨 DC incast 文件流。
 */

#include "workload-generator.h"
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include <ns3/rdma.h>
#include <ns3/rdma-client.h>
#include <ns3/rdma-client-helper.h>
#include <ns3/rdma-driver.h>
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/custom-header.h"
#include "ns3/int-header.h"
#include "cdf.h"
#include <iomanip>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <tuple>

using namespace ns3;

// ==================== 外部依赖声明 ====================
// 这些变量在主控文件中定义，通过 extern 引用
extern NodeContainer n;
extern std::map<Ptr<Node>, std::map<Ptr<Node>, Interface>> nbr2if;
extern std::map<Ptr<Node>, std::map<Ptr<Node>, std::vector<Interface>>> neighborInterfaces;
extern std::vector<Ipv4Address> serverAddress;
extern std::vector<FlowInput> flows;
extern std::map<uint32_t, uint32_t> g_hostToDc;
extern uint32_t has_win, global_t;
extern uint64_t maxBdp, maxRtt;
extern uint32_t packet_payload_size, l2_ack_interval;
extern std::map<Ptr<Node>, std::map<Ptr<Node>, uint64_t>> pairBdp;
extern std::map<uint32_t, std::map<uint32_t, uint64_t>> pairRtt;

// 工作负载拓扑全局变量
extern std::vector<uint32_t> allHostIds;
extern std::vector<uint32_t> hostFacingLeafIds;
extern std::map<uint32_t, std::vector<uint32_t>> leafToHostIds;
extern std::map<uint32_t, uint64_t> leafDownlinkCapacityBps;
extern std::map<uint32_t, uint64_t> leafUplinkCapacityBps;

// ==================== 工具函数实现 ====================

namespace {
Ptr<UniformRandomVariable> workloadUniformRandomVariable;
Ptr<ExponentialRandomVariable> workloadArrivalRandomVariable;
Ptr<UniformRandomVariable> fileSelectionRandomVariable;
Ptr<UniformRandomVariable> fileStartRandomVariable;
std::map<std::tuple<uint32_t, uint32_t, uint16_t, uint16_t>, uint32_t> fileFlowIndex;
std::vector<uint32_t> fileFlowIndices;
struct HostPairPorts {
    uint16_t firstSource;
    uint32_t allocated;
    uint16_t nextDestination;
};
std::unordered_map<uint32_t, std::unordered_map<uint32_t, HostPairPorts>> workloadPorts;

constexpr uint32_t fileDestinationDc = 1; // B

bool IsAllowedLongDistanceDirection(uint32_t sourceDc, uint32_t destinationDc)
{
    // A--B--C: long-distance data enters B instead of transiting its DCI.
    return (sourceDc == 0 || sourceDc == 2) && destinationDc == fileDestinationDc;
}
}

void InitializeWorkloadRandomVariables(uint32_t randomSeed)
{
    RngSeedManager::SetSeed(randomSeed);
    workloadUniformRandomVariable = CreateObject<UniformRandomVariable>();
    workloadArrivalRandomVariable = CreateObject<ExponentialRandomVariable>();
    workloadUniformRandomVariable->SetStream(1000);
    workloadArrivalRandomVariable->SetStream(1001);
    fileSelectionRandomVariable = CreateObject<UniformRandomVariable>();
    fileStartRandomVariable = CreateObject<UniformRandomVariable>();
    fileSelectionRandomVariable->SetStream(1002);
    fileStartRandomVariable->SetStream(1003);
    fileFlowIndex.clear();
    fileFlowIndices.clear();
}

const FlowInput* FindFileFlow(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport)
{
    auto entry = fileFlowIndex.find({src, dst, sport, dport});
    return entry == fileFlowIndex.end() ? nullptr : &flows.at(entry->second);
}

const std::vector<uint32_t>& GetFileFlowIndices()
{
    return fileFlowIndices;
}

static std::map<uint32_t, std::vector<uint32_t>> CollectWorkloadDcHosts()
{
    std::map<uint32_t, std::vector<uint32_t>> dcHosts;
    for (uint32_t leafId : hostFacingLeafIds) {
        for (uint32_t hostId : leafToHostIds.at(leafId)) {
            NS_ABORT_MSG_IF(!g_hostToDc.count(hostId), "Host has no inferred DC: " << hostId);
            dcHosts[g_hostToDc.at(hostId)].push_back(hostId);
        }
    }
    for (auto& [dcId, hosts] : dcHosts) {
        std::sort(hosts.begin(), hosts.end());
        NS_ABORT_MSG_IF(std::adjacent_find(hosts.begin(), hosts.end()) != hosts.end(),
                        "Workload requires single-homed hosts in DC" << dcId);
    }
    return dcHosts;
}

/**
 * 泊松过程间隔生成
 * @param requestRate 请求速率（flows/sec）
 * @return 时间间隔（秒）
 */
double poission_gen_interval(double requestRate) {
    if (requestRate <= 0) return std::numeric_limits<double>::infinity();
    return workloadArrivalRandomVariable->GetValue(1.0 / requestRate, 0);
}

/**
 * 生成指定范围内的随机整数
 * @param min 最小值（包含）
 * @param max 最大值（不包含）
 * @return 随机整数
 */
uint32_t rand_range_uint(uint32_t min, uint32_t max) {
    if (min >= max) return min;
    return workloadUniformRandomVariable->GetInteger(min, max - 1);
}

/**
 * 检查主机是否在列表中
 */
static bool ContainsHost(const std::vector<uint32_t>& hosts, uint32_t hostId) {
    return std::find(hosts.begin(), hosts.end(), hostId) != hosts.end();
}

// ==================== 工作负载拓扑发现 ====================

/**
 * 发现工作负载拓扑（结果存储在全局变量中）
 */
void DiscoverWorkloadTopology(uint32_t serverCountLimit,
                              uint32_t leafCountLimit,
                              uint64_t leafServerCapacityFallback,
                              uint64_t spineLeafCapacityFallback,
                              uint32_t spineCountFallback)
{
    allHostIds.clear();
    hostFacingLeafIds.clear();
    leafToHostIds.clear();
    leafDownlinkCapacityBps.clear();
    leafUplinkCapacityBps.clear();
    
    std::cout << "[WORKLOAD TOPO] Discovering workload topology..." << std::endl;
    
    // Step 1: 收集所有主机节点
    for (uint32_t i = 0; i < n.GetN(); i++) {
        if (n.Get(i)->GetNodeType() == 0) {
            allHostIds.push_back(i);
        }
    }
    std::sort(allHostIds.begin(), allHostIds.end());
    std::cout << "[WORKLOAD TOPO] Found " << allHostIds.size() << " hosts" << std::endl;
    
    // Step 2: 发现面向主机的叶子交换机
    for (uint32_t i = 0; i < n.GetN(); i++) {
        Ptr<Node> node = n.Get(i);
        if (node->GetNodeType() == 0)
            continue;  // 跳过主机节点
        
        std::vector<uint32_t> hosts;
        for (auto it = nbr2if[node].begin(); it != nbr2if[node].end(); it++) {
            if (!it->second.up)
                continue;
            Ptr<Node> neighbor = it->first;
            if (neighbor->GetNodeType() == 0) {
                hosts.push_back(neighbor->GetId());
            }
        }
        if (hosts.empty())
            continue;
        
        std::sort(hosts.begin(), hosts.end());
        if (serverCountLimit > 0 && hosts.size() > serverCountLimit) {
            hosts.resize(serverCountLimit);
        }
        hostFacingLeafIds.push_back(i);
        leafToHostIds[i] = hosts;
    }
    
    std::sort(hostFacingLeafIds.begin(), hostFacingLeafIds.end());
    if (leafCountLimit > 0 && hostFacingLeafIds.size() > leafCountLimit) {
        hostFacingLeafIds.resize(leafCountLimit);
    }
    std::cout << "[WORKLOAD TOPO] Found " << hostFacingLeafIds.size() 
              << " host-facing leaf switches" << std::endl;
    
    // Step 3: 计算每个叶子交换机的上下行容量
    for (uint32_t leafId : hostFacingLeafIds) {
        Ptr<Node> leaf = n.Get(leafId);
        uint64_t downlinkCapacity = 0;
        uint64_t uplinkCapacity = 0;
        
        for (const auto& [neighbor, interfaces] : neighborInterfaces.at(leaf)) {
            bool downlink = neighbor->GetNodeType() == 0;
            if (downlink && !ContainsHost(leafToHostIds.at(leafId), neighbor->GetId()))
                continue;
            auto& capacity = downlink ? downlinkCapacity : uplinkCapacity;
            for (const auto& iface : interfaces)
                if (iface.up) capacity += iface.bw;
        }
        
        // 使用回退值
        if (downlinkCapacity == 0 && leafServerCapacityFallback > 0) {
            downlinkCapacity = leafToHostIds[leafId].size() * leafServerCapacityFallback;
        }
        if (uplinkCapacity == 0 && spineLeafCapacityFallback > 0 && spineCountFallback > 0) {
            uplinkCapacity = spineLeafCapacityFallback * spineCountFallback;
        }
        
        leafDownlinkCapacityBps[leafId] = downlinkCapacity;
        leafUplinkCapacityBps[leafId] = uplinkCapacity;
    }
    
    NS_ASSERT_MSG(!allHostIds.empty(), "No host nodes discovered for workload");
    NS_ASSERT_MSG(!hostFacingLeafIds.empty(), "No host-facing leaf switches discovered");
}

// ==================== 端口初始化 ====================

/**
 * 初始化工作负载端口映射（无参数版本）
 */
void InitializeWorkloadPorts()
{
    std::cout << "[WORKLOAD PORT] Initializing ports for " << allHostIds.size() 
              << " hosts" << std::endl;
    
    for (uint32_t src : allHostIds) {
        for (uint32_t dst : allHostIds) {
            if (src == dst)
                continue;
            uint16_t port = (uint16_t)rand_range_uint(10000, 11000);
            workloadPorts[src][dst] = {port, 0, port};
        }
    }
}

// ==================== 流添加 ====================

/**
 * 添加工作负载流
 * 创建 RDMA 流并安装应用
 */
void AddWorkloadFlow(uint32_t src,
                     uint32_t dst,
                     uint32_t pg,
                     uint64_t flowSize,
                     double startTime,
                     long &flowCount,
                     long &totalFlowSize,
                     uint16_t forcedDport,
                     bool isFileFlow)
{
    // 参数验证
    if (src == dst || flowSize == 0)
        return;
    NS_ABORT_MSG_IF(flowSize > UINT32_MAX, "RDMA cumulative ACKs use a 32-bit byte sequence; flow size exceeds UINT32_MAX");
    
    NS_ASSERT_MSG(src < n.GetN() && dst < n.GetN(),
                  "Workload flow node id out of range");
    NS_ASSERT_MSG(n.Get(src)->GetNodeType() == 0 && n.Get(dst)->GetNodeType() == 0,
                  "Workload selected a non-host node");

    const uint32_t sourceDc = g_hostToDc.at(src);
    const uint32_t destinationDc = g_hostToDc.at(dst);
    NS_ABORT_MSG_IF(sourceDc != destinationDc &&
                        !IsAllowedLongDistanceDirection(sourceDc, destinationDc),
                    "Long-distance flows must be A(DC0)/C(DC2) -> B(DC1); got DC"
                        << sourceDc << " -> DC" << destinationDc
                        << " (hosts " << src << " -> " << dst << ")");
    
    // 分配端口
    auto& ports = workloadPorts.at(src).at(dst);
    NS_ABORT_MSG_IF(ports.allocated == UINT16_MAX, "RDMA source ports exhausted for hosts "
                    << src << " -> " << dst);
    const uint16_t sport = 1 + (ports.firstSource - 1 + ports.allocated++) % UINT16_MAX;
    const uint16_t dport = forcedDport ? forcedDport : ports.nextDestination;
    if (dport >= ports.nextDestination)
        ports.nextDestination = dport == UINT16_MAX ? 1 : dport + 1;
    
    // 创建流输入记录
    FlowInput fi = {0};
    fi.src = src;
    fi.dst = dst;
    fi.pg = pg;
    fi.port = sport;
    fi.dport = dport;
    fi.maxPacketCount = flowSize;
    fi.start_time = startTime;
    fi.idx = flows.size();
    fi.last_recv_bytes = 0;
    fi.is_file_flow = isFileFlow;
    if (isFileFlow) {
        NS_ABORT_MSG_IF(!fileFlowIndex.emplace(std::make_tuple(src, dst, sport, dport), fi.idx).second,
                        "File flow port tuple reused; reduce concurrent flows per host pair");
        fileFlowIndices.push_back(fi.idx);
    }
    flows.push_back(fi);
    
    // 安装 RDMA 客户端应用
    RdmaClientHelper clientHelper(pg,
                                  serverAddress[src],
                                  serverAddress[dst],
                                  sport,
                                  dport,
                                  flowSize,
                                  has_win ? (global_t == 1 ? maxBdp : pairBdp[n.Get(src)][n.Get(dst)]) : 0,
                                  global_t == 1 ? maxRtt : pairRtt[src][dst],
                                  Simulator::GetMaximumSimulationTime());
    ApplicationContainer appCon = clientHelper.Install(n.Get(src));
    appCon.Start(Seconds(startTime));
    
    flowCount++;
    totalFlowSize += flowSize;
}

// ==================== 背景流生成（ToR 上行负载）====================

/**
 * 移植自旧 PowerTCP workload：
 *   λ_host = load * C_uplink / (8 * N_tor * E[S_wire] * P_cross_tor)
 *   P_cross_tor = (N_dc - N_tor) / (N_dc - 1)
 *   E[S_wire] = E[S] + (E[S] / MTU + 1) * (DATA_header + ACK_wire)
 * 目的主机在同 DC 的其他主机中均匀选择，包含同 ToR 目的地。
 * ACK 开销和包数上界沿用旧实现，不计入 INT，保证不同算法使用相同输入。
 */
void InstallBackgroundWorkload(double load,
                               void *cdfTablePtr,
                               double START_TIME,
                               double FLOW_LAUNCH_END_TIME,
                               long &flowCount,
                               long &totalFlowSize)
{
    struct cdf_table *cdfTable = (struct cdf_table *)cdfTablePtr;
    const double avgFlowSize = avg_cdf(cdfTable);
    NS_ABORT_MSG_IF(!std::isfinite(avgFlowSize) || avgFlowSize <= 0,
                    "Average CDF flow size must be positive and finite");
    NS_ABORT_MSG_IF(!std::isfinite(load), "Workload load must be finite");
    load = std::clamp(load, 0.0, 1.0);
    NS_ABORT_MSG_IF(!std::isfinite(START_TIME) || !std::isfinite(FLOW_LAUNCH_END_TIME) ||
                        START_TIME < 0 || FLOW_LAUNCH_END_TIME < START_TIME,
                    "Invalid workload launch window");
    NS_ABORT_MSG_IF(packet_payload_size == 0, "Packet payload size must be positive");

    const double dataPacketsUpperBound = avgFlowSize / packet_payload_size + 1;
    const uint32_t intHeaderBytes = IntHeader::GetStaticSize();
    const uint32_t dataHeaderBytes = CustomHeader::GetStaticWholeHeaderSize() - intHeaderBytes;
    const uint32_t ackWireBytes =
        std::max<uint32_t>(60, 14 + 20 + CustomHeader::GetAckSerializedSize() - intHeaderBytes);
    const double averageWorkloadFlowSize = avgFlowSize +
        dataPacketsUpperBound * (dataHeaderBytes + (l2_ack_interval > 0 ? ackWireBytes : 0));

    std::cout << "[WORKLOAD BG] Installing background workload (TOR_UPLINK mode)"
              << " load=" << load << " avgPayloadBytes=" << avgFlowSize
              << " avgWireBytes=" << averageWorkloadFlowSize << std::endl;

    const auto dcHosts = CollectWorkloadDcHosts();

    std::map<uint32_t, long> dcFlowCount;
    std::map<uint32_t, uint64_t> dcTotalBytes;
    for (uint32_t leafId : hostFacingLeafIds) {
        const auto& sources = leafToHostIds.at(leafId);
        const uint32_t dcId = g_hostToDc.at(sources.front());
        const auto& destinations = dcHosts.at(dcId);
        for (uint32_t src : sources) {
            NS_ABORT_MSG_IF(g_hostToDc.at(src) != dcId, "ToR spans multiple DCs: " << leafId);
        }
        const uint64_t uplinkCapacity = leafUplinkCapacityBps.at(leafId);
        const double crossTorProbability = destinations.size() > 1 ?
            double(destinations.size() - sources.size()) / (destinations.size() - 1) : 0;
        NS_ABORT_MSG_IF(crossTorProbability > 0 && uplinkCapacity == 0, "ToR has no uplink capacity: " << leafId);
        // A DC with only one selected ToR has no traffic that can load a ToR uplink.
        const double requestRate = crossTorProbability > 0 ?
            load * uplinkCapacity /
                (8 * sources.size() * averageWorkloadFlowSize * crossTorProbability) : 0;
        std::cout << "[WORKLOAD TOR] dc=" << dcId << " tor=" << leafId
                  << " hosts=" << sources.size() << " dcHosts=" << destinations.size()
                  << " downlinkBps=" << leafDownlinkCapacityBps.at(leafId)
                  << " uplinkBps=" << uplinkCapacity
                  << " targetBps=" << load * uplinkCapacity
                  << " crossTorProbability=" << crossTorProbability
                  << " requestRatePerHost=" << requestRate << std::endl;

        for (uint32_t src : sources) {
            double startTime = START_TIME + poission_gen_interval(requestRate);
            while (startTime < FLOW_LAUNCH_END_TIME && startTime > START_TIME) {
                uint32_t dst;
                do {
                    dst = destinations[rand_range_uint(0, destinations.size())];
                } while (dst == src);

                uint64_t flowSize;
                do {
                    flowSize = static_cast<uint64_t>(gen_value_cdf(cdfTable,
                        workloadUniformRandomVariable->GetValue(cdfTable->min_cdf,
                                                                cdfTable->max_cdf)));
                } while (flowSize == 0);
                AddWorkloadFlow(src, dst, 3, flowSize, startTime, flowCount, totalFlowSize);
                dcFlowCount[dcId]++;
                dcTotalBytes[dcId] += flowSize;
                startTime += poission_gen_interval(requestRate);
            }
        }
    }
    for (const auto& [dcId, hosts] : dcHosts) {
        std::cout << "[WORKLOAD BG] DC" << dcId << ": " << hosts.size() << " hosts, "
                  << dcFlowCount[dcId] << " flows, " << dcTotalBytes[dcId]
                  << " bytes total" << std::endl;
    }
    std::cout << "[WORKLOAD BG] Background flow generation complete. "
              << "Total flows: " << flowCount 
              << ", Total bytes: " << totalFlowSize << std::endl;
}

// ==================== 跨 DC 文件流：直接生成 ====================

void InstallFixedFlowFile(const std::string& path,
                          double startOffset,
                          double stopTime,
                          long& flowCount,
                          long& totalFlowSize)
{
    std::ifstream input(path);
    NS_ABORT_MSG_IF(!input.is_open(), "Cannot open fixed flow file: " << path);
    int64_t requested = 0;
    NS_ABORT_MSG_IF(!(input >> requested) || requested < 0 || requested > UINT32_MAX,
                    "Invalid fixed flow count: " << path);
    const uint32_t expected = requested;
    const auto precision = std::cout.precision();
    for (uint32_t index = 0; index < expected; ++index) {
        uint32_t src, dst, pg, dport;
        uint64_t size;
        double offset;
        NS_ABORT_MSG_IF(!(input >> src >> dst >> pg >> dport >> size >> offset),
                        "Invalid fixed flow row " << index << " in " << path);
        const double start = startOffset + offset;
        NS_ABORT_MSG_IF(src == dst || src >= n.GetN() || dst >= n.GetN() ||
                            !g_hostToDc.count(src) || !g_hostToDc.count(dst),
                        "Invalid fixed flow endpoints in row " << index);
        NS_ABORT_MSG_IF(pg >= 8 || dport == 0 || dport > UINT16_MAX || size == 0 ||
                            size > uint64_t(std::numeric_limits<long>::max() - totalFlowSize) ||
                            !std::isfinite(start) || offset < 0 || start < 0 || start >= stopTime,
                        "Invalid fixed flow parameters in row " << index);
        AddWorkloadFlow(src, dst, pg, size, start, flowCount, totalFlowSize, dport, true);
        const auto& flow = flows.back();
        std::cout << std::setprecision(17) << "[FILE FLOW #" << index << "] src=" << src
                  << " dst=" << dst << " pg=" << pg << " sport=" << flow.port
                  << " dport=" << dport << " size=" << size << " start=" << start
                  << " start_ns=" << Seconds(start).GetNanoSeconds() << " group=" << index
                  << " source_dc=" << g_hostToDc.at(src)
                  << " destination_dc=" << g_hostToDc.at(dst)
                  << " cross_dc=" << (g_hostToDc.at(src) != g_hostToDc.at(dst) ? "yes" : "no")
                  << std::endl;
    }
    std::string extra;
    NS_ABORT_MSG_IF(input >> extra, "Extra rows in fixed flow file: " << path);
    std::cout.precision(precision);
    std::cout << "[FILE FLOW] Generated " << flowCount << " flows, " << expected
              << " fixed entries, " << totalFlowSize << " bytes total" << std::endl;
}

void InstallCrossDcFileFlows(const FileFlowParameters& parameters,
                             double launchEndTime,
                             long& fileFlowCount,
                             long& totalFileFlowSize)
{
    if (parameters.count == 0) {
        std::cout << "[FILE FLOW] Generated 0 flows, 0 groups, 0 bytes total" << std::endl;
        return;
    }
    NS_ABORT_MSG_IF(parameters.sizeBytes == 0, "FILE_FLOW_SIZE_BYTES must be positive");
    NS_ABORT_MSG_IF(parameters.incast == 0, "FILE_INCAST must be positive");
    NS_ABORT_MSG_IF(!std::isfinite(parameters.startTime) ||
                        !std::isfinite(launchEndTime) || parameters.startTime < 0 ||
                        Seconds(launchEndTime) <= Seconds(parameters.startTime),
                    "Invalid file-flow launch window");
    NS_ABORT_MSG_IF(parameters.sizeBytes > uint64_t(std::numeric_limits<long>::max()) / parameters.count,
                    "File-flow byte total exceeds the statistics counter range");
    const auto dcHosts = CollectWorkloadDcHosts();
    auto destination = dcHosts.find(fileDestinationDc);
    NS_ABORT_MSG_IF(destination == dcHosts.end(), "No workload hosts in destination DC1");
    std::vector<uint32_t> sourceHosts;
    for (const auto& [dc, hosts] : dcHosts) {
        if (IsAllowedLongDistanceDirection(dc, fileDestinationDc))
            sourceHosts.insert(sourceHosts.end(), hosts.begin(), hosts.end());
    }
    NS_ABORT_MSG_IF(sourceHosts.empty(), "No remote source hosts for file flows");
    NS_ABORT_MSG_IF(std::min(parameters.incast, parameters.count) > sourceHosts.size(),
                    "FILE_INCAST requires more distinct remote senders than available hosts");

    // A bounded window and exact count are independent controls: draw one
    // uniform random launch per incast group, then preserve chronological order.
    const uint32_t groupCount = (uint64_t(parameters.count) + parameters.incast - 1) / parameters.incast;
    std::vector<Time> launchTimes;
    for (uint32_t group = 0; group < groupCount; ++group) {
        Time launch = Seconds(fileStartRandomVariable->GetValue(parameters.startTime, launchEndTime));
        launchTimes.push_back(std::min(launch, Seconds(launchEndTime) - NanoSeconds(1)));
    }
    std::sort(launchTimes.begin(), launchTimes.end());
    uint32_t generated = 0;
    const auto precision = std::cout.precision();
    for (uint32_t group = 0; group < groupCount; ++group) {
        const uint32_t dst = destination->second[fileSelectionRandomVariable->GetInteger(0, destination->second.size() - 1)];
        auto senders = sourceHosts;
        const uint32_t fan = std::min(parameters.incast, parameters.count - generated);
        for (uint32_t member = 0; member < fan; ++member) {
            const uint32_t selected = fileSelectionRandomVariable->GetInteger(member, senders.size() - 1);
            std::swap(senders[member], senders[selected]);
            const uint32_t src = senders[member];
            AddWorkloadFlow(src, dst, 3, parameters.sizeBytes, launchTimes[group].GetSeconds(),
                            fileFlowCount, totalFileFlowSize, 0, true);
            const auto& flow = flows.back();
            std::cout << std::setprecision(17) << "[FILE FLOW #" << generated << "] src=" << src
                      << " dst=" << dst << " pg=3 sport=" << flow.port << " dport=" << flow.dport
                      << " size=" << parameters.sizeBytes << " start=" << launchTimes[group].GetSeconds()
                      << " start_ns=" << launchTimes[group].GetNanoSeconds() << " group=" << group
                      << " source_dc=" << g_hostToDc.at(src)
                      << " destination_dc=" << fileDestinationDc << " cross_dc=yes" << std::endl;
            ++generated;
        }
    }
    std::cout.precision(precision);
    std::cout << "[FILE FLOW] Generated " << generated << " flows, " << groupCount
              << " groups, " << totalFileFlowSize << " bytes total" << std::endl;
}
