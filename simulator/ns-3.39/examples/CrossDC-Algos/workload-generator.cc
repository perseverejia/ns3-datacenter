/*
 * Workload Generator Implementation for CrossDC-Algos
 * 
 * 独立的流量生成模块实现
 * 支持：PER_DC 模式背景流、查询流文件加载、跨数据中心流量识别
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
#include "cdf.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

using namespace ns3;

// ==================== 外部依赖声明 ====================
// 这些变量在主控文件中定义，通过 extern 引用
extern NodeContainer n;
extern std::map<Ptr<Node>, std::map<Ptr<Node>, Interface>> nbr2if;
extern std::vector<Ipv4Address> serverAddress;
extern std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint16_t>> portNumder;
extern std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint16_t>> DestportNumder;
extern std::vector<FlowInput> flows;
extern std::map<uint32_t, uint32_t> g_hostToDc;
extern uint32_t has_win, global_t;
extern uint64_t maxBdp, maxRtt;
extern std::map<Ptr<Node>, std::map<Ptr<Node>, uint64_t>> pairBdp;
extern std::map<uint32_t, std::map<uint32_t, uint64_t>> pairRtt;

// 工作负载拓扑全局变量
extern std::vector<uint32_t> allHostIds;
extern std::vector<uint32_t> hostFacingLeafIds;
extern std::map<uint32_t, std::vector<uint32_t>> leafToHostIds;
extern std::map<uint32_t, uint64_t> leafDownlinkCapacityBps;
extern std::map<uint32_t, uint64_t> leafUplinkCapacityBps;

// ==================== 工具函数实现 ====================

/**
 * 泊松过程间隔生成
 * @param requestRate 请求速率（flows/sec）
 * @return 时间间隔（秒）
 */
double poission_gen_interval(double requestRate) {
    if (requestRate <= 0) return 1e10;
    return -log(1.0 - (double)rand() / RAND_MAX) / requestRate;
}

/**
 * 生成指定范围内的随机整数
 * @param min 最小值（包含）
 * @param max 最大值（不包含）
 * @return 随机整数
 */
uint32_t rand_range_uint(uint32_t min, uint32_t max) {
    if (min >= max) return min;
    return min + rand() % (max - min);
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
        
        for (auto it = nbr2if[leaf].begin(); it != nbr2if[leaf].end(); it++) {
            if (!it->second.up)
                continue;
            Ptr<Node> neighbor = it->first;
            if (neighbor->GetNodeType() == 0) {
                // 下行链路（到主机）
                if (ContainsHost(leafToHostIds[leafId], neighbor->GetId())) {
                    downlinkCapacity += it->second.bw;
                }
            } else {
                // 上行链路（到脊交换机）
                uplinkCapacity += it->second.bw;
            }
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
            portNumder[src][dst] = port;
            DestportNumder[src][dst] = port;
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
                     bool isQueryFlow)
{
    // 参数验证
    if (src == dst || flowSize == 0)
        return;
    
    NS_ASSERT_MSG(src < n.GetN() && dst < n.GetN(),
                  "Workload flow node id out of range");
    NS_ASSERT_MSG(n.Get(src)->GetNodeType() == 0 && n.Get(dst)->GetNodeType() == 0,
                  "Workload selected a non-host node");
    
    // 分配端口
    if (portNumder[src].find(dst) == portNumder[src].end() || 
        portNumder[src][dst] >= UINT16_MAX - 1) {
        portNumder[src][dst] = (uint16_t)rand_range_uint(10000, 11000);
    }
    if (DestportNumder[src].find(dst) == DestportNumder[src].end() || 
        DestportNumder[src][dst] >= UINT16_MAX - 1) {
        DestportNumder[src][dst] = (uint16_t)rand_range_uint(10000, 11000);
    }
    
    uint16_t sport = portNumder[src][dst]++;
    uint16_t dport = forcedDport ? forcedDport : DestportNumder[src][dst]++;
    if (forcedDport && DestportNumder[src][dst] <= dport) {
        DestportNumder[src][dst] = dport + 1;
    }
    
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
    fi.is_query_flow = isQueryFlow;
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

// ==================== 背景流生成（PER_DC 模式）====================

/**
 * 安装背景工作负载（PER_DC 模式）
 * 
 * 负载计算公式：
 *   λ_total = (ρ * C_ref) / (8 * E[S])
 *   λ_per_host = λ_total / N_hosts
 * 
 * 其中：
 *   ρ = load (负载率, 0.0-1.0)
 *   C_ref = refBandwidth (参考带宽, bps)
 *   E[S] = avgFlowSize (平均流大小, bytes)
 *   N_hosts = numHostsPerDC (每个 DC 的主机数)
 */
void InstallBackgroundWorkload(double load,
                               void *cdfTablePtr,
                               double START_TIME,
                               double FLOW_LAUNCH_END_TIME,
                               long &flowCount,
                               long &totalFlowSize,
                               uint64_t refBandwidth)
{
    struct cdf_table *cdfTable = (struct cdf_table *)cdfTablePtr;
    
    double avgFlowSize = avg_cdf(cdfTable);
    NS_ASSERT_MSG(avgFlowSize > 0, "Average CDF flow size must be positive");
    
    std::cout << "[WORKLOAD BG] Installing background workload (PER_DC mode)" << std::endl;
    std::cout << "[WORKLOAD BG] Load: " << load << ", Reference bandwidth: " 
              << refBandwidth / 1e9 << " Gbps" << std::endl;
    std::cout << "[WORKLOAD BG] Average flow size: " << avgFlowSize << " bytes" << std::endl;
    
    // Step 1: 分离两个数据中心的主机
    std::vector<uint32_t> dc0_hosts;
    std::vector<uint32_t> dc1_hosts;
    
    for (uint32_t i = 0; i < n.GetN(); i++) {
        if (n.Get(i)->GetNodeType() == 0) {
            if (g_hostToDc.find(i) == g_hostToDc.end()) 
                continue;
            
            uint32_t dcId = g_hostToDc[i];
            if (dcId == 0) {
                dc0_hosts.push_back(i);
            } else if (dcId == 1) {
                dc1_hosts.push_back(i);
            }
        }
    }
    
    std::cout << "[WORKLOAD BG] DC0 hosts: " << dc0_hosts.size() 
              << ", DC1 hosts: " << dc1_hosts.size() << std::endl;
    
    if (dc0_hosts.empty() || dc1_hosts.empty()) {
        std::cerr << "[WORKLOAD BG] Warning: One or both DCs have no hosts!" << std::endl;
        return;
    }
    
    // 两个 DC 必须有相同数量的主机（镜像模式）
    uint32_t numHostsPerDC = std::min(dc0_hosts.size(), dc1_hosts.size());
    if (dc0_hosts.size() != dc1_hosts.size()) {
        std::cout << "[WORKLOAD BG] Warning: DC0 and DC1 have different host counts. "
                  << "Using min: " << numHostsPerDC << std::endl;
    }
    
    // Step 2: 计算每台主机的请求速率
    // PER_DC 模式：每个 DC 的总负载为参考带宽的 load 倍
    double requestRate = load * refBandwidth / (8.0 * avgFlowSize * numHostsPerDC);
    
    std::cout << "[WORKLOAD BG] Request rate per host: " << requestRate 
              << " flows/sec" << std::endl;
    
    // Step 3: 为 DC0 生成流模式
    struct BgFlowPattern {
        uint32_t src_idx;
        uint32_t dst_idx;
        uint64_t flowSize;
        double startTime;
    };
    std::vector<BgFlowPattern> flowPatterns;
    
    std::cout << "[WORKLOAD BG] Generating flows for DC0..." << std::endl;
    
    for (uint32_t i = 0; i < numHostsPerDC; i++) {
        double startTime = START_TIME + poission_gen_interval(requestRate);
        while (startTime < FLOW_LAUNCH_END_TIME && startTime > START_TIME) {
            // 随机选择目标主机（不能是自己）
            uint32_t dst_idx = rand_range_uint(0, numHostsPerDC);
            if (dst_idx == i) {
                startTime += poission_gen_interval(requestRate);
                continue;
            }
            
            // 从 CDF 生成流大小
            uint64_t flowSize = (uint64_t)gen_random_cdf(cdfTable);
            while (flowSize == 0) {
                flowSize = (uint64_t)gen_random_cdf(cdfTable);
            }
            
            // 记录流模式
            flowPatterns.push_back({i, dst_idx, flowSize, startTime});
            
            // 添加到 DC0
            AddWorkloadFlow(dc0_hosts[i], dc0_hosts[dst_idx], 3, flowSize, 
                          startTime, flowCount, totalFlowSize);
            
            startTime += poission_gen_interval(requestRate);
        }
    }
    
    // 统计 DC0 流量
    uint64_t dc0_totalBytes = 0;
    for (auto &p : flowPatterns) {
        dc0_totalBytes += p.flowSize;
    }
    std::cout << "[WORKLOAD BG] DC0: " << flowPatterns.size() << " flows, "
              << dc0_totalBytes << " bytes total" << std::endl;
    
    // Step 4: DC1 镜像相同的流模式
    std::cout << "[WORKLOAD BG] Mirroring flows to DC1..." << std::endl;
    uint64_t dc1_totalBytes = 0;
    for (auto &p : flowPatterns) {
        AddWorkloadFlow(dc1_hosts[p.src_idx], dc1_hosts[p.dst_idx], 3, 
                       p.flowSize, p.startTime, flowCount, totalFlowSize);
        dc1_totalBytes += p.flowSize;
    }
    std::cout << "[WORKLOAD BG] DC1: " << flowPatterns.size() << " flows, "
              << dc1_totalBytes << " bytes total" << std::endl;
    
    std::cout << "[WORKLOAD BG] Background flow generation complete. "
              << "Total flows: " << flowCount 
              << ", Total bytes: " << (dc0_totalBytes + dc1_totalBytes) << std::endl;
}

// ==================== 查询流文件加载 ====================

/**
 * 从文件加载查询流
 * 
 * 文件格式：
 *   第 1 行：流数量
 *   第 2+ 行：src dst pg dport flowSize startTime
 */
void InstallQueryFlowFile(const std::string& queryFlowFile,
                          double startTime,
                          long &queryFlowCount,
                          long &totalQueryFlowSize)
{
    if (queryFlowFile.empty()) {
        std::cout << "[QUERY FLOW] No query flow file specified, skip." << std::endl;
        return;
    }
    
    std::ifstream qf(queryFlowFile.c_str());
    if (!qf.is_open()) {
        std::cerr << "[QUERY FLOW] Error: Cannot open query flow file: " 
                  << queryFlowFile << std::endl;
        return;
    }
    
    uint32_t queryFlowNum = 0;
    qf >> queryFlowNum;
    
    std::cout << "[QUERY FLOW] Loading " << queryFlowNum 
              << " flows from " << queryFlowFile << std::endl;
    
    for (uint32_t i = 0; i < queryFlowNum; i++) {
        uint32_t src, dst, pg;
        uint16_t dport;
        uint64_t flowSize;
        double fileStartTime;
        
        qf >> src >> dst >> pg >> dport >> flowSize >> fileStartTime;
        
        // 验证参数
        if (src >= n.GetN() || dst >= n.GetN()) {
            std::cerr << "[QUERY FLOW] Warning: Flow " << i 
                      << " has invalid node id (src=" << src << ", dst=" << dst 
                      << "), skipping." << std::endl;
            continue;
        }
        
        if (n.Get(src)->GetNodeType() != 0 || n.Get(dst)->GetNodeType() != 0) {
            std::cerr << "[QUERY FLOW] Warning: Flow " << i 
                      << " selected a non-host node, skipping." << std::endl;
            continue;
        }
        
        if (src == dst) {
            std::cerr << "[QUERY FLOW] Warning: Flow " << i 
                      << " has src == dst, skipping." << std::endl;
            continue;
        }
        
        if (flowSize == 0) {
            std::cerr << "[QUERY FLOW] Warning: Flow " << i 
                      << " has zero flow size, skipping." << std::endl;
            continue;
        }
        
        // 计算调整后的启动时间
        double adjustedStartTime = startTime + fileStartTime;
        
        // 检查是否为跨 DC 流
        bool is_cross_dc = false;
        if (g_hostToDc.find(src) != g_hostToDc.end() && 
            g_hostToDc.find(dst) != g_hostToDc.end()) {
            is_cross_dc = (g_hostToDc[src] != g_hostToDc[dst]);
        }
        
        // 添加流
        AddWorkloadFlow(src, dst, pg, flowSize, adjustedStartTime,
                       queryFlowCount, totalQueryFlowSize, dport, true);
        
        std::cout << "[QUERY FLOW #" << i << "] "
                  << "src=" << src 
                  << " dst=" << dst
                  << " pg=" << pg
                  << " dport=" << dport
                  << " size=" << flowSize << " bytes"
                  << " start=" << adjustedStartTime << " sec"
                  << " cross_dc=" << (is_cross_dc ? "yes" : "no")
                  << std::endl;
    }
    
    qf.close();
    
    std::cout << "[QUERY FLOW] Total query flows loaded: " << queryFlowCount 
              << ", Total size: " << totalQueryFlowSize << " bytes" << std::endl;
}
