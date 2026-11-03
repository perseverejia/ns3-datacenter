
/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation;
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#undef PGO_TRAINING
#define PATH_TO_PGO_CONFIG "path_to_pgo_config"

#include <iostream>
#include <fstream>
#include <unordered_map>
#include <set>
#include <time.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include "ns3/core-module.h"
#include "ns3/qbb-helper.h"
#include "ns3/point-to-point-helper.h"
#include "ns3/applications-module.h"
#include "ns3/internet-module.h"
#include "ns3/global-route-manager.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/ipv4-list-routing.h"
#include "ns3/ipv4-static-routing.h"
#include "ns3/packet.h"
#include "ns3/error-model.h"
#include <ns3/rdma.h>
#include <ns3/rdma-client.h>
#include <ns3/rdma-client-helper.h>
#include <ns3/rdma-driver.h>
#include <ns3/switch-node.h>
#include <ns3/sim-setting.h>
#include <ns3/switch-node.h>

extern "C"
{
#include "cdf.h"
}

#define LINK_CAPACITY_BASE 1000000000lu

using namespace ns3;
using namespace std;

NS_LOG_COMPONENT_DEFINE("GENERIC_SIMULATION");

uint32_t cc_mode = 1;
bool enable_qcn = true;
uint32_t packet_payload_size = 1000, l2_chunk_size = 0, l2_ack_interval = 0;
double pause_time = 5, simulator_stop_time = 3.01;
uint32_t query_auto_stop = 1;  // 1: 查询流完成后自动停止仿真, 0: 运行到 SIMULATOR_STOP_TIME
std::string data_rate, link_delay, topology_file, flow_file, trace_file, trace_output_file;
std::string fct_output_file = "fct.txt";
std::string pfc_output_file = "pfc.txt";
bool enable_flow_file_background = true;
std::string background_flow_file = "examples/CrossDC-Algos/config/flows/flow-background.txt";


// 只需要保留 gatewayNodeType 用于标识哪些交换机是 gateway
unordered_map<uint32_t, int> gatewayNodeType; // 1: local/source DCI, 2: remote/destination DCI
uint32_t gatewayFlag = 0; // ATC gateway 类型标志

// 长链路参数（用于 ATC Gateway 初始化）
uint64_t longHaulBandwidth = 0;
Time longHaulDelay = MicroSeconds(0);

double alpha_resume_interval = 55, rp_timer, ewma_gain = 1 / 16;
double rate_decrease_interval = 4;
uint32_t fast_recovery_times = 5;
std::string rate_ai, rate_hai, min_rate = "100Mb/s";
std::string dctcp_rate_ai = "1000Mb/s";

bool clamp_target_rate = false, l2_back_to_zero = false;
double error_rate_per_link = 0.0;
uint32_t has_win = 1;
uint32_t global_t = 1;
uint32_t mi_thresh = 5;
bool var_win = false, fast_react = true;
bool multi_rate = true;
bool sample_feedback = false;
double pint_log_base = 1.05;
double pint_prob = 1.0;
double u_target = 0.95;
uint32_t int_multi = 1;
bool rate_bound = true;

uint32_t ack_high_prio = 0;
uint64_t link_down_time = 0;
uint32_t link_down_A = 0, link_down_B = 0;

uint32_t enable_trace = 1;

uint32_t buffer_size = 16;

// 网卡处理延迟（纳秒）
uint64_t nic_delay_ns = 15000;  // 默认15μs = 15000ns

uint32_t qlen_dump_interval = 100000000, qlen_mon_interval = 100;   
uint64_t qlen_mon_start = 2000000000, qlen_mon_end = 2100000000;	
string qlen_mon_file;

// 瓶颈队列监控（每40us采样一次，记录全局队列最大的非DCI交换机）
uint32_t bottleneck_mon_interval = 40000; // 40微秒 = 40000 ns timestep
string bottleneck_mon_file;
set<uint32_t> dci_switches; // DCI交换机ID集合（拓扑解析时动态填充）
map<uint32_t, uint32_t> g_switchToDc; // 全局映射：交换机ID -> DC ID
map<uint32_t, uint32_t> g_hostToDc;   // 全局映射：主机ID -> DC ID

/**
 * 统一的跨域流判断函数
 * 根据源节点和目的节点的DC归属判断是否为跨域流
 * @param srcId 源节点ID（主机或交换机）
 * @param dstId 目的节点ID（主机或交换机）
 * @return true 如果是跨域流，否则返回 false
 */
inline bool IsCrossDcFlow(uint32_t srcId, uint32_t dstId) {
    uint32_t srcDc, dstDc;
    bool srcFound = false, dstFound = false;
    
    // 尝试从 hostToDc 查找
    if (g_hostToDc.find(srcId) != g_hostToDc.end()) {
        srcDc = g_hostToDc[srcId];
        srcFound = true;
    } else if (g_switchToDc.find(srcId) != g_switchToDc.end()) {
        srcDc = g_switchToDc[srcId];
        srcFound = true;
    }
    
    if (g_hostToDc.find(dstId) != g_hostToDc.end()) {
        dstDc = g_hostToDc[dstId];
        dstFound = true;
    } else if (g_switchToDc.find(dstId) != g_switchToDc.end()) {
        dstDc = g_switchToDc[dstId];
        dstFound = true;
    }
    
    // 如果都能找到且属于不同DC，则是跨域流
    return (srcFound && dstFound && srcDc != dstDc);
}

unordered_map<uint64_t, uint32_t> rate2kmax, rate2kmin;
unordered_map<uint64_t, double> rate2pmax;

/************************************************
 * Runtime varibles
 ***********************************************/
std::ifstream topof, flowf, tracef;

NodeContainer n;
NetDeviceContainer switchToSwitchInterfaces;
std::map< uint32_t, std::map< uint32_t, std::vector<Ptr<QbbNetDevice>> > > switchToSwitch;

// vamsi
std::map<uint32_t, uint32_t> switchNumToId;
std::map<uint32_t, uint32_t> switchIdToNum;
std::map<uint32_t, NetDeviceContainer> switchUp;
std::map<uint32_t, NetDeviceContainer> switchDown;
//NetDeviceContainer switchUp[switch_num];
std::map<uint32_t, NetDeviceContainer> sourceNodes;

NodeContainer servers;
NodeContainer tors;

uint64_t nic_rate;

uint64_t maxRtt, maxBdp;

struct Interface {
	uint32_t idx;
	bool up;
	uint64_t delay;
	uint64_t bw;

	Interface() : idx(0), up(false) {}
};
map<Ptr<Node>, map<Ptr<Node>, Interface> > nbr2if;
// Mapping destination to next hop for each node: <node, <dest, <nexthop0, ...> > >
map<Ptr<Node>, map<Ptr<Node>, vector<Ptr<Node> > > > nextHop;
map<Ptr<Node>, map<Ptr<Node>, uint64_t> > pairDelay;
map<Ptr<Node>, map<Ptr<Node>, uint64_t> > pairTxDelay;
map<uint32_t, map<uint32_t, uint64_t> > pairBw;
map<Ptr<Node>, map<Ptr<Node>, uint64_t> > pairBdp;
map<uint32_t, map<uint32_t, uint64_t> > pairRtt;

std::vector<Ipv4Address> serverAddress;

// maintain port number for each host pair
std::unordered_map<uint32_t, unordered_map<uint32_t, uint16_t> > portNumder;
std::unordered_map<uint32_t, unordered_map<uint32_t, uint16_t> > DestportNumder;

std::vector<uint32_t> allHostIds;
std::vector<uint32_t> hostFacingLeafIds;
std::map<uint32_t, std::vector<uint32_t> > leafToHostIds;
std::map<uint32_t, uint64_t> leafDownlinkCapacityBps;
std::map<uint32_t, uint64_t> leafUplinkCapacityBps;
uint32_t FAN = 5;

// Global counter for query flow completion
uint32_t g_queryFlowCompleted = 0;
uint32_t g_totalQueryFlows = 0;
bool g_simulationStopped = false;

// Long-distance RTT for cross-DC compensation (from topology.txt line 171: 0.5ms)
uint64_t g_longDistanceRtt = 0;  // Will be set from topology

// Query flow FCT output file
FILE* g_queryFlowFctFile = nullptr;

struct FlowInput {
	uint64_t src, dst, pg, maxPacketCount, port, dport;
	double start_time;
	uint32_t idx;
	uint64_t last_recv_bytes;  // Last sampled m_recv_bytes, used for delta goodput calculation
	bool is_query_flow;        // Flag to indicate if this is a query flow
};
FlowInput flow_input = {0};
uint32_t flow_num;
std::vector<FlowInput> flows;                              // 保存所有流的完整信息（含实际分配的 sport）

void ReadFlowInput() {
	if (flow_input.idx < flow_num) {
		flowf >> flow_input.src >> flow_input.dst >> flow_input.pg >> flow_input.dport >> flow_input.maxPacketCount >> flow_input.start_time;
		std::cout << "Flow " << flow_input.src << " " << flow_input.dst << " " << flow_input.pg << " " << flow_input.dport << " " << flow_input.maxPacketCount << " " << flow_input.start_time << " " << Simulator::Now().GetSeconds() << std::endl;
		NS_ASSERT(n.Get(flow_input.src)->GetNodeType() == 0 && n.Get(flow_input.dst)->GetNodeType() == 0);
		flows.push_back(flow_input);  // 保存流记录（此时 port 尚未分配，由 ScheduleFlowInputs 回填）
	}
}
void ScheduleFlowInputs() {
	while (flow_input.idx < flow_num && Seconds(flow_input.start_time) <= Simulator::Now()) {
		uint32_t port = portNumder[flow_input.src][flow_input.dst]++; // get a new port number
		flows[flow_input.idx].port = port;  // 回填实际分配的 sport，供 PrintResultsFlow 查找 RxQp
		RdmaClientHelper clientHelper(flow_input.pg, serverAddress[flow_input.src], serverAddress[flow_input.dst], port, flow_input.dport, flow_input.maxPacketCount, has_win ? (global_t == 1 ? maxBdp : pairBdp[n.Get(flow_input.src)][n.Get(flow_input.dst)]) : 0, global_t == 1 ? maxRtt : pairRtt[flow_input.src][flow_input.dst], Simulator::GetMaximumSimulationTime());
		ApplicationContainer appCon = clientHelper.Install(n.Get(flow_input.src));
//		appCon.Start(Seconds(flow_input.start_time));
		appCon.Start(Seconds(0)); // setting the correct time here conflicts with Sim time since there is already a schedule event that triggered this function at desired time.
		// get the next flow input
		flow_input.idx++;
		ReadFlowInput();
	}

	// schedule the next time to run this function
	if (flow_input.idx < flow_num) {
		Simulator::Schedule(Seconds(flow_input.start_time) - Simulator::Now(), ScheduleFlowInputs);
	} else { // no more flows, close the file
		flowf.close();
	}
}

// 全局映射表：IP地址 -> 节点ID
map<uint32_t, uint32_t> ipToNodeIdMap;  // key: IP (uint32_t), value: nodeId

/**
 * 结构体：用于辅助为主机分配 IP
 */
struct HostIpInfo {
    uint32_t hostId;
    uint32_t dcId;
};

/**
 * 自动识别拓扑并为所有交换机和主机分配真实 IP
 * 掩码: 255.255.0.0
 * IP规则: 11.{dc_id}.{local_id/256}.{local_id%256}  (local_id从1开始递增，完美避开.0)
 */
void AutoAssignTopologyIps(NodeContainer &n) {
    // ==================== 第1步: 识别交换机并收集链路 ====================
    vector<uint32_t> switchIds;
    for (uint32_t i = 0; i < n.GetN(); i++) {
        Ptr<Node> node = n.Get(i);
        if (node->GetNodeType() == 1) { // 1 = 交换机
            switchIds.push_back(node->GetId());
        }
    }
    
    NS_LOG_INFO("Found " << switchIds.size() << " switches");
    
    map<pair<uint32_t, uint32_t>, uint64_t> interSwitchLinks;
    for (auto it = nbr2if.begin(); it != nbr2if.end(); it++) {
        Ptr<Node> src = it->first;
        if (src->GetNodeType() != 1) continue;
        
        for (auto jt = it->second.begin(); jt != it->second.end(); jt++) {
            Ptr<Node> dst = jt->first;
            if (dst->GetNodeType() != 1) continue;
            
            if (src->GetId() < dst->GetId()) {
                interSwitchLinks[{src->GetId(), dst->GetId()}] = jt->second.delay;
            }
        }
    }
    
    // ==================== 第2步: BFS 划分数据中心 (交换机) ====================
    // 使用 dci_switches 判断跨 DC 链路（从 topology 文件显式声明）
    map<uint32_t, bool> visited;
    uint32_t dcId = 0;
    
    // Debug: print all inter-switch links
    std::cout << "[TOPO BFS] Inter-switch links:" << std::endl;
    for (auto& link : interSwitchLinks) {
        std::cout << "  Switch " << link.first.first << " <-> Switch " << link.first.second
                  << " delay=" << link.second << "ns";
        if (dci_switches.count(link.first.first) > 0 && dci_switches.count(link.first.second) > 0) {
            std::cout << " (CROSS-DC, DCI link)";
        }
        std::cout << std::endl;
    }
    
    for (uint32_t swId : switchIds) {
        if (visited[swId]) continue;
        
        queue<uint32_t> q;
        q.push(swId);
        visited[swId] = true;
        g_switchToDc[swId] = dcId;
        
        while (!q.empty()) {
            uint32_t curr = q.front();
            q.pop();
            
            for (auto& link : interSwitchLinks) {
                uint32_t src = link.first.first;
                uint32_t dst = link.first.second;
                
                uint32_t neighbor = 0;
                if (src == curr) neighbor = dst;
                else if (dst == curr) neighbor = src;
                else continue;
                
                // 使用 dci_switches 判断跨 DC 链路
                if (dci_switches.count(src) > 0 && dci_switches.count(dst) > 0) {
                    std::cout << "[TOPO BFS] Cross-DC link skipped: " << src << " <-> " << dst
                              << " (both are DCI switches)" << std::endl;
                    continue; // 跨DC链路，跳过
                }
                
                if (!visited[neighbor]) {
                    visited[neighbor] = true;
                    g_switchToDc[neighbor] = dcId;
                    q.push(neighbor);
                }
            }
        }
        
        dcId++; // 发现完一个连通子图，DC编号加1
    }
    
    std::cout << "[TOPO BFS] Total " << dcId << " datacenters discovered." << std::endl;
    for (auto& kv : g_switchToDc) {
        std::cout << "  Switch " << kv.first << " -> DC" << kv.second << std::endl;
    }
    
    // ==================== 第3步: 发现主机所属的 DC ====================
    for (uint32_t i = 0; i < n.GetN(); i++) {
        Ptr<Node> node = n.Get(i);
        if (node->GetNodeType() != 0) continue; // 0 = 主机
        
        uint32_t hostId = node->GetId();
        set<uint32_t> connectedDcs;
        
        for (auto it = nbr2if[node].begin(); it != nbr2if[node].end(); it++) {
            Ptr<Node> neighbor = it->first;
            if (neighbor->GetNodeType() == 1) { // 邻居是交换机
                uint32_t swId = neighbor->GetId();
                if (g_switchToDc.find(swId) != g_switchToDc.end()) {
                    connectedDcs.insert(g_switchToDc[swId]);
                }
            }
        }
        
        if (!connectedDcs.empty()) {
            g_hostToDc[hostId] = *connectedDcs.begin(); // 默认归属第一个发现的DC
        } else {
            std::cerr << "Warning: Host " << hostId << " not connected to any switch!" << std::endl;
        }
    }
    
    // ==================== 第4步: 统一分配真实 IP ====================
    // 为每个 DC 维护一个局部的 IP 后缀计数器，从 1 开始，有效防止 .0 网络号冲突
    map<uint32_t, uint32_t> dcLocalIpCounter;
    
    for (uint32_t i = 0; i < n.GetN(); i++) {
        Ptr<Node> node = n.Get(i);
        uint32_t nodeId = node->GetId();
        
        if (node->GetNodeType() == 1) { // 处理交换机
            Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(node);
            
            if (g_switchToDc.find(nodeId) == g_switchToDc.end()) {
                std::cerr << "ERROR: Switch " << nodeId << " not assigned to any DC!" << std::endl;
                continue;
            }
            
            uint32_t assignedDcId = g_switchToDc[nodeId];
            
            // 在该DC内为交换机生成局部ID
            if (dcLocalIpCounter.find(assignedDcId) == dcLocalIpCounter.end()) {
                dcLocalIpCounter[assignedDcId] = 1;
            }
            uint32_t localId = dcLocalIpCounter[assignedDcId]++;
            
            // 组装 IP: 11.{dcId}.{localId/256}.{localId%256}
            char ipStr[32];
            sprintf(ipStr, "11.%d.%d.%d", assignedDcId, localId / 256, localId % 256);
            Ipv4Address switchIp(ipStr);
            
            sw->SetSwitchRealIp(switchIp);
            
            // 记录IP到nodeId的映射
            ipToNodeIdMap[switchIp.Get()] = nodeId;
            
        } else if (node->GetNodeType() == 0) { // 处理主机
            if (g_hostToDc.find(nodeId) == g_hostToDc.end()) continue;
            
            uint32_t assignedDcId = g_hostToDc[nodeId];
            
            if (dcLocalIpCounter.find(assignedDcId) == dcLocalIpCounter.end()) {
                dcLocalIpCounter[assignedDcId] = 1;
            }
            uint32_t localId = dcLocalIpCounter[assignedDcId]++;
            
            char ipStr[32];
            sprintf(ipStr, "11.%d.%d.%d", assignedDcId, localId / 256, localId % 256);
            Ipv4Address hostIp(ipStr);
            
            // 更新serverAddress数组
            if (nodeId < serverAddress.size()) {
                serverAddress[nodeId] = hostIp;
            }
            
            // 更新ns-3的Ipv4接口配置并显式激活接口
            Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
            std::cout << "  [DEBUG] Host " << nodeId << " nInterfaces=" << ipv4->GetNInterfaces() << std::endl;
            for (uint32_t ifIdx = 0; ifIdx < ipv4->GetNInterfaces(); ifIdx++) {
                uint32_t nAddr = ipv4->GetNAddresses(ifIdx);
                for (uint32_t addrIdx = 0; addrIdx < nAddr; addrIdx++) {
                    Ipv4InterfaceAddress addr = ipv4->GetAddress(ifIdx, addrIdx);
                    std::cout << "    Interface[" << ifIdx << "] addr[" << addrIdx << "]=" << addr.GetLocal() << "/" << addr.GetMask() << " isUp=" << ipv4->IsUp(ifIdx) << std::endl;
                }
            }
            
            // 记录IP到nodeId的映射
            ipToNodeIdMap[hostIp.Get()] = nodeId;
        }
    }
}

/**
 * 从IP地址提取节点ID (用于qp_finish)
 * 使用全局映射表精确查找
 */
uint32_t ip_to_node_id(Ipv4Address ip) {
    uint32_t ipValue = ip.Get();
    
    // 在映射表中查找
    auto it = ipToNodeIdMap.find(ipValue);
    if (it != ipToNodeIdMap.end()) {
        return it->second;  // 返回精确的nodeId
    }
    
    // 如果没找到，打印警告并返回0
    std::cerr << "Warning: IP " << ip << " not found in ipToNodeIdMap!" << std::endl;
    return 0;
}

void qp_finish(FILE* fout, Ptr<RdmaQueuePair> q) {
	uint32_t sid = ip_to_node_id(q->sip), did = ip_to_node_id(q->dip);
	uint64_t base_rtt = pairRtt[sid][did], b = pairBw[sid][did];
	uint32_t total_bytes = q->m_size + ((q->m_size - 1) / packet_payload_size + 1) * (CustomHeader::GetStaticWholeHeaderSize() - IntHeader::GetStaticSize()); // translate to the minimum bytes required (with header but no INT)
	uint64_t standalone_fct = base_rtt + total_bytes * 8000000000lu / b;
	
	// Check if this is a query flow by matching sip, dip, and dport
	bool is_query = false;
	const FlowInput* query_flow_info = nullptr;
	for (const auto& flow : flows) {
		if (serverAddress[flow.src].Get() == q->sip.Get() &&
		    serverAddress[flow.dst].Get() == q->dip.Get() &&
		    flow.dport == q->dport &&
		    flow.is_query_flow) {
			is_query = true;
			query_flow_info = &flow;
			break;
		}
	}
	
	// sip, dip, sport, dport, size (B), start_time, fct (ns), standalone_fct (ns), is_query_flow
	fprintf(fout, "%08x %08x %u %u %lu %lu %lu %lu %d\n", q->sip.Get(), q->dip.Get(), q->sport, q->dport, q->m_size, q->startTime.GetTimeStep(), (Simulator::Now() - q->startTime).GetTimeStep(), standalone_fct, is_query ? 1 : 0);
	fflush(fout);

	// Write query flow to dedicated FCT file with node IDs and RTT compensation
	if (is_query && query_flow_info && g_queryFlowFctFile != nullptr) {
		uint64_t fct_ns = (Simulator::Now() - q->startTime).GetTimeStep();
		
		// Check if cross-DC flow using global topology mapping
		bool cross_dc = IsCrossDcFlow(sid, did);
		
		// Output format: src_node dst_node pg dport flow_size start_time_ns fct_ns is_cross_dc
		fprintf(g_queryFlowFctFile, "%u %u %lu %lu %lu %lu %lu %d\n",
		        query_flow_info->src,  // Source node ID
		        query_flow_info->dst,  // Destination node ID
		        query_flow_info->pg,   // Priority group
		        query_flow_info->dport, // Destination port
		        query_flow_info->maxPacketCount, // Flow size in bytes
		        q->startTime.GetTimeStep(), // Start time in ns
		        fct_ns,  // FCT in ns (compensated if cross-DC)
		        cross_dc ? 1 : 0);  // Cross-DC flag
		fflush(g_queryFlowFctFile);
	}

	// Count query flow completion and stop simulation if all query flows are done
	if (is_query) {
		g_queryFlowCompleted++;
		std::cout << "[QUERY FLOW] Completed " << g_queryFlowCompleted << "/" << g_totalQueryFlows 
		          << " query flows at time " << Simulator::Now().GetSeconds() << "s" << std::endl;
		
		// Stop simulation when all query flows complete (with 1ms grace period)
		if (query_auto_stop && g_queryFlowCompleted >= g_totalQueryFlows && !g_simulationStopped) {
			g_simulationStopped = true;
			std::cout << "[QUERY FLOW] All " << g_totalQueryFlows << " query flows completed! Stopping simulation..." << std::endl;
			Simulator::Stop(Seconds(0.001)); // Stop after 1ms grace period
		}
	}

	// remove rxQp from the receiver
	Ptr<Node> dstNode = n.Get(did);
	Ptr<RdmaDriver> rdma = dstNode->GetObject<RdmaDriver> ();
	rdma->m_rdma->DeleteRxQp(q->sip.Get(), q->m_pg, q->sport);
}

void get_pfc(FILE* fout, Ptr<QbbNetDevice> dev, uint32_t type) {
	fprintf(fout, "%lu %u %u %u %u\n", Simulator::Now().GetTimeStep(), dev->GetNode()->GetId(), dev->GetNode()->GetNodeType(), dev->GetIfIndex(), type);
}

struct QlenDistribution {
	vector<uint32_t> cnt; // cnt[i] is the number of times that the queue len is i KB

	void add(uint32_t qlen) {
		uint32_t kb = qlen / 1000;
		if (cnt.size() < kb + 1)
			cnt.resize(kb + 1);
		cnt[kb]++;
	}
};
map<uint32_t, map<uint32_t, QlenDistribution> > queue_result;
void monitor_buffer(FILE* qlen_output, NodeContainer *n) {
	for (uint32_t i = 0; i < n->GetN(); i++) {
		if (n->Get(i)->GetNodeType() == 1) { // is switch
			Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(n->Get(i));
			if (queue_result.find(i) == queue_result.end())
				queue_result[i];
			for (uint32_t j = 1; j < sw->GetNDevices(); j++) {
				uint32_t size = 0;
				for (uint32_t k = 0; k < SwitchMmu::qCnt; k++)
					size += sw->m_mmu->egress_bytes[j][k];
				queue_result[i][j].add(size);
			}
		}
	}
	if (Simulator::Now().GetTimeStep() % qlen_dump_interval == 0) {
		fprintf(qlen_output, "time: %lu\n", Simulator::Now().GetTimeStep());
		for (auto &it0 : queue_result)
			for (auto &it1 : it0.second) {
				fprintf(qlen_output, "%u %u", it0.first, it1.first);
				auto &dist = it1.second.cnt;
				for (uint32_t i = 0; i < dist.size(); i++)
					fprintf(qlen_output, " %u", dist[i]);
				fprintf(qlen_output, "\n");
			}
		fflush(qlen_output);
	}
	if (Simulator::Now().GetTimeStep() < qlen_mon_end)
		Simulator::Schedule(NanoSeconds(qlen_mon_interval), &monitor_buffer, qlen_output, n);
}

// 瓶颈队列监控：每 bottleneck_mon_interval 采样一次
// 记录所有非DCI交换机中队列长度最大的那个交换机-端口对
// 输出格式: 时间(ns) 交换机ID 端口号 队列长度(bytes)
void monitor_bottleneck_queue(FILE* output, NodeContainer* n) {
	uint32_t max_sw = 0, max_port = 0;
	uint32_t max_q = 0;
	
	for (uint32_t i = 0; i < n->GetN(); i++) {
		if (n->Get(i)->GetNodeType() != 1) continue; // 只处理交换机
		if (dci_switches.count(i) > 0) continue;     // 跳过DCI交换机
		
		Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(n->Get(i));
		for (uint32_t j = 1; j < sw->GetNDevices(); j++) {
			uint32_t qsize = 0;
			for (uint32_t k = 0; k < SwitchMmu::qCnt; k++)
				qsize += sw->m_mmu->egress_bytes[j][k];
			if (qsize > max_q) {
				max_q = qsize;
				max_sw = i;
				max_port = j;
			}
		}
	}
	
	// 输出: 时间(ns) 交换机ID 端口号 队列长度(bytes)
	fprintf(output, "%lu %u %u %u\n", 
	        Simulator::Now().GetTimeStep(), max_sw, max_port, max_q);
	fflush(output);
	
	// 递归调度
	Simulator::Schedule(NanoSeconds(bottleneck_mon_interval), 
	                   &monitor_bottleneck_queue, output, n);
}

void CalculateRoute(Ptr<Node> host) {
	// queue for the BFS.
	vector<Ptr<Node> > q;
	// Distance from the host to each node.
	map<Ptr<Node>, int> dis;
	map<Ptr<Node>, uint64_t> delay;
	map<Ptr<Node>, uint64_t> txDelay;
	map<Ptr<Node>, uint64_t> bw;
	// init BFS.
	q.push_back(host);
	dis[host] = 0;
	delay[host] = 0;
	txDelay[host] = 0;
	bw[host] = 0xfffffffffffffffflu;
	// BFS.
	for (int i = 0; i < (int)q.size(); i++) {
		Ptr<Node> now = q[i];
		int d = dis[now];
		for (auto it = nbr2if[now].begin(); it != nbr2if[now].end(); it++) {
			// skip down link
			if (!it->second.up)
				continue;
			Ptr<Node> next = it->first;
			// If 'next' have not been visited.
			if (dis.find(next) == dis.end()) {
				dis[next] = d + 1;
				delay[next] = delay[now] + it->second.delay;
				txDelay[next] = txDelay[now] + packet_payload_size * 1000000000lu * 8 / it->second.bw;
				bw[next] = std::min(bw[now], it->second.bw);
				// we only enqueue switch, because we do not want packets to go through host as middle point
				if (next->GetNodeType())
					q.push_back(next);
			}
			// if 'now' is on the shortest path from 'next' to 'host'.
			if (d + 1 == dis[next]) {
				nextHop[next][host].push_back(now);
			}
		}
	}
	for (auto it : delay)
		pairDelay[it.first][host] = it.second;
	for (auto it : txDelay)
		pairTxDelay[it.first][host] = it.second;
	for (auto it : bw)
		pairBw[it.first->GetId()][host->GetId()] = it.second;
}

void CalculateRoutes(NodeContainer &n) {
	for (int i = 0; i < (int)n.GetN(); i++) {
		Ptr<Node> node = n.Get(i);
		if (node->GetNodeType() == 0)
			CalculateRoute(node);
	}
}

void SetRoutingEntries() {
	// For each node.
	for (auto i = nextHop.begin(); i != nextHop.end(); i++) {
		Ptr<Node> node = i->first;
		auto &table = i->second;
		for (auto j = table.begin(); j != table.end(); j++) {
			// The destination node.
			Ptr<Node> dst = j->first;
			// The IP address of the dst.
			// 【修复】使用serverAddress数组获取目的IP，而非GetAddress(1,0)
			// GetAddress(1,0)取到的是临时IP 10.0.0.1，serverAddress已被AutoAssignTopologyIps更新为真实IP
			uint32_t dstNodeId = dst->GetId();
			Ipv4Address dstAddr;
			if (dst->GetNodeType() == 0 && dstNodeId < serverAddress.size()) {
				dstAddr = serverAddress[dstNodeId];
			} else {
				dstAddr = dst->GetObject<Ipv4>()->GetAddress(1, 0).GetLocal();
			}
			// The next hops towards the dst.
			vector<Ptr<Node> > nexts = j->second;
			for (int k = 0; k < (int)nexts.size(); k++) {
				Ptr<Node> next = nexts[k];
				uint32_t interface = nbr2if[node][next].idx;
				if (node->GetNodeType())
					DynamicCast<SwitchNode>(node)->AddTableEntry(dstAddr, interface);
				else {
					node->GetObject<RdmaDriver>()->m_rdma->AddTableEntry(dstAddr, interface);
				}
			}
		}
	}
}

// take down the link between a and b, and redo the routing
void TakeDownLink(NodeContainer n, Ptr<Node> a, Ptr<Node> b) {
	if (!nbr2if[a][b].up)
		return;
	// take down link between a and b
	nbr2if[a][b].up = nbr2if[b][a].up = false;
	nextHop.clear();
	CalculateRoutes(n);
	// clear routing tables
	for (uint32_t i = 0; i < n.GetN(); i++) {
		if (n.Get(i)->GetNodeType() == 1)
			DynamicCast<SwitchNode>(n.Get(i))->ClearTable();
		else
			n.Get(i)->GetObject<RdmaDriver>()->m_rdma->ClearTable();
	}
	DynamicCast<QbbNetDevice>(a->GetDevice(nbr2if[a][b].idx))->TakeDown();
	DynamicCast<QbbNetDevice>(b->GetDevice(nbr2if[b][a].idx))->TakeDown();
	// reset routing table
	SetRoutingEntries();

	// redistribute qp on each host
	for (uint32_t i = 0; i < n.GetN(); i++) {
		if (n.Get(i)->GetNodeType() == 0)
			n.Get(i)->GetObject<RdmaDriver>()->m_rdma->RedistributeQp();
	}
}

uint64_t get_nic_rate(NodeContainer &n) {
	for (uint32_t i = 0; i < n.GetN(); i++)
		if (n.Get(i)->GetNodeType() == 0)
			return DynamicCast<QbbNetDevice>(n.Get(i)->GetDevice(1))->GetDataRate().GetBitRate();
}

void PrintResults(std::map<uint32_t, NetDeviceContainer> ToR, uint32_t numToRs, double delay) {
	for (uint32_t i = 0; i < numToRs; i++) {
		double throughputTotal = 0;
		uint64_t torBuffer = 0;
		double power;
		for (uint32_t j = 0; j < ToR[i].GetN(); j++) {
			Ptr<QbbNetDevice> nd = DynamicCast<QbbNetDevice>(ToR[i].Get(j));
//			uint64_t txBytes = nd->getTxBytes();
			uint64_t txBytes = nd->GetQueue()->getTxBytes();
			double rxBytes = nd->getNumRxBytes();

			uint64_t qlen = nd->GetQueue()->GetNBytesTotal();
			uint64_t bw = nd->GetDataRate().GetBitRate(); //maxRtt

			torBuffer += qlen;
			double throughput = double(txBytes * 8) / delay;
			if (j == 16) { //  ToDo. very ugly hardcode here specific to the burst evaluation scenario where 16 is the receiver in flow-burstExp.txt.
				throughputTotal += throughput;
				power = (rxBytes * 8.0 / delay) * (qlen + bw * maxRtt * 1e-9) / (bw * (bw * maxRtt * 1e-9));

			}
			std::cout << "ToR " << i << " Port " << j << " throughput " << throughput << " txBytes " << txBytes << " qlen " << qlen << " time " << Simulator::Now().GetSeconds() << " normpower " << power << std::endl;
		}
		std::cout << "ToR " << i << " Total " << 0 << " throughput " << throughputTotal << " buffer " << torBuffer <<  " time " << Simulator::Now().GetSeconds() << std::endl;
	}
	Simulator::Schedule(Seconds(delay), PrintResults, ToR, numToRs, delay);
}


void PrintResultsFlow(double delay) {
	for (auto &fi : flows) {
		// 获取目标节点的 RdmaDriver
		Ptr<Node> dstNode = n.Get(fi.dst);
		Ptr<RdmaDriver> rdma = dstNode->GetObject<RdmaDriver>();
		if (rdma == NULL || rdma->m_rdma == NULL) continue;
		// 查找 RxQp（注意 GetRxQp 参数顺序错位，详见计划文档）
		Ptr<RdmaRxQueuePair> rxQp = rdma->m_rdma->GetRxQp(
			serverAddress[fi.dst].Get(),  // sip形参 ← 接收端IP
			serverAddress[fi.src].Get(),  // dip形参 ← 发送端IP
			fi.dport,                      // sport形参 ← 接收端dport
			(uint16_t)fi.port,             // dport形参 ← 发送端sport
			(uint16_t)fi.pg,
			false);                        // 不创建，查不到就跳过
		if (rxQp == NULL) continue;       // 流尚未开始或已结束

		// 计算 goodput（用结构体自带的 last_recv_bytes，无需 key/map，绝不碰撞）
		uint64_t current = rxQp->m_recv_bytes;
		uint64_t last = fi.last_recv_bytes;
		fi.last_recv_bytes = current;

		// 首次采样：只记录基线，不输出（避免数据累积时间 ≠ delay 导致高估）
		if (last == 0) continue;

		double throughput = (current >= last) ?
			double(current - last) * 8.0 / delay : 0.0;

		// 输出
		std::cout << "[FLOW TP] Src " << fi.src
		          << " Dst " << fi.dst
		          << " pg " << fi.pg
		          << " sport " << fi.port
		          << " dport " << fi.dport
		          << " throughput " << throughput
		          << " time " << Simulator::Now().GetSeconds()
		          << " m_recv_bytes " << current
		          << std::endl;
	}
	Simulator::Schedule(Seconds(delay), PrintResultsFlow, delay);
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/* Workload applications */

double poission_gen_interval(double avg_rate)
{
	if (avg_rate > 0)
		return -logf(1.0 - (double)rand() / RAND_MAX) / avg_rate;
	return 0;
}

uint32_t rand_range_uint(uint32_t min, uint32_t max)
{
	if (max <= min)
		return min;
	return min + (uint32_t)(((double)max - min) * rand() / RAND_MAX);
}

void PrintVector(const std::string &name, const std::vector<uint32_t> &values)
{
	std::cout << "[WORKLOAD] " << name << "=";
	for (size_t i = 0; i < values.size(); i++) {
		if (i) std::cout << ",";
		std::cout << values[i];
	}
	std::cout << std::endl;
}

bool ContainsHost(const std::vector<uint32_t> &hosts, uint32_t hostId)
{
	return std::find(hosts.begin(), hosts.end(), hostId) != hosts.end();
}

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

	for (uint32_t i = 0; i < n.GetN(); i++) {
		if (n.Get(i)->GetNodeType() == 0) {
			allHostIds.push_back(i);
		}
	}
	std::sort(allHostIds.begin(), allHostIds.end());

	for (uint32_t i = 0; i < n.GetN(); i++) {
		Ptr<Node> node = n.Get(i);
		if (node->GetNodeType() == 0)
			continue;

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

	for (uint32_t leafId : hostFacingLeafIds) {
		Ptr<Node> leaf = n.Get(leafId);
		uint64_t downlinkCapacity = 0;
		uint64_t uplinkCapacity = 0;
		for (auto it = nbr2if[leaf].begin(); it != nbr2if[leaf].end(); it++) {
			if (!it->second.up)
				continue;
			Ptr<Node> neighbor = it->first;
			if (neighbor->GetNodeType() == 0) {
				if (ContainsHost(leafToHostIds[leafId], neighbor->GetId())) {
					downlinkCapacity += it->second.bw;
				}
			} else {
				uplinkCapacity += it->second.bw;
			}
		}
		if (downlinkCapacity == 0 && leafServerCapacityFallback > 0) {
			downlinkCapacity = leafToHostIds[leafId].size() * leafServerCapacityFallback;
		}
		if (uplinkCapacity == 0 && spineLeafCapacityFallback > 0 && spineCountFallback > 0) {
			uplinkCapacity = spineLeafCapacityFallback * spineCountFallback;
		}
		leafDownlinkCapacityBps[leafId] = downlinkCapacity;
		leafUplinkCapacityBps[leafId] = uplinkCapacity;
	}

	PrintVector("allHostIds", allHostIds);
	PrintVector("hostFacingLeafIds", hostFacingLeafIds);
	for (uint32_t leafId : hostFacingLeafIds) {
		PrintVector("leaf " + std::to_string(leafId) + " hosts", leafToHostIds[leafId]);
	}
	NS_ASSERT_MSG(!allHostIds.empty(), "No host nodes discovered for workload");
	NS_ASSERT_MSG(!hostFacingLeafIds.empty(), "No host-facing leaf switches discovered for workload");
}

void InitializeWorkloadPorts()
{
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

std::vector<uint32_t> GetCandidateDestinationHosts(uint32_t srcLeafId, uint32_t srcHostId)
{
	std::vector<uint32_t> candidates;
	for (uint32_t leafId : hostFacingLeafIds) {
		if (leafId == srcLeafId)
			continue;
		for (uint32_t hostId : leafToHostIds[leafId]) {
			if (hostId != srcHostId)
				candidates.push_back(hostId);
		}
	}
	if (candidates.empty()) {
		for (uint32_t hostId : allHostIds) {
			if (hostId != srcHostId)
				candidates.push_back(hostId);
		}
	}
	return candidates;
}

uint32_t PickDestinationHost(uint32_t srcLeafId, uint32_t srcHostId)
{
	std::vector<uint32_t> candidates = GetCandidateDestinationHosts(srcLeafId, srcHostId);
	NS_ASSERT_MSG(!candidates.empty(), "No destination host available for workload flow");
	return candidates[rand_range_uint(0, candidates.size())];
}

uint32_t PickSourceHostForQuery(uint32_t dstLeafId, uint32_t dstHostId)
{
	return PickDestinationHost(dstLeafId, dstHostId);
}

void AddWorkloadFlow(uint32_t src,
                     uint32_t dst,
                     uint32_t pg,
                     uint64_t flowSize,
                     double startTime,
                     long &flowCount,
                     long &totalFlowSize,
                     uint16_t forcedDport = 0,
                     bool isQueryFlow = false)
{
	if (src == dst || flowSize == 0)
		return;
	NS_ASSERT_MSG(n.Get(src)->GetNodeType() == 0 && n.Get(dst)->GetNodeType() == 0,
	              "Workload selected a non-host node");
	NS_ASSERT_MSG(serverAddress[src].Get() != Ipv4Address("10.0.0.1").Get() &&
	              serverAddress[dst].Get() != Ipv4Address("10.0.0.1").Get(),
	              "Workload flow uses temporary serverAddress");

	if (portNumder[src].find(dst) == portNumder[src].end() || portNumder[src][dst] >= UINT16_MAX - 1) {
		portNumder[src][dst] = (uint16_t)rand_range_uint(10000, 11000);
	}
	if (DestportNumder[src].find(dst) == DestportNumder[src].end() || DestportNumder[src][dst] >= UINT16_MAX - 1) {
		DestportNumder[src][dst] = (uint16_t)rand_range_uint(10000, 11000);
	}

	uint16_t sport = portNumder[src][dst]++;
	uint16_t dport = forcedDport ? forcedDport : DestportNumder[src][dst]++;
	if (forcedDport && DestportNumder[src][dst] <= dport) {
		DestportNumder[src][dst] = dport + 1;
	}

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



void InstallBackgroundWorkload(double load,
                               struct cdf_table *cdfTable,
                               double START_TIME,
                               double FLOW_LAUNCH_END_TIME,
                               long &flowCount,
                               long &totalFlowSize)
{
	double avgFlowSize = avg_cdf(cdfTable);
	NS_ASSERT_MSG(avgFlowSize > 0, "Average CDF flow size must be positive");

	// Separate hosts into two data centers using global topology mapping
	std::vector<uint32_t> dc0_hosts;
	std::vector<uint32_t> dc1_hosts;
	
	for (uint32_t leafId : hostFacingLeafIds) {
		auto &hosts = leafToHostIds[leafId];
		for (uint32_t hostId : hosts) {
			if (g_hostToDc.find(hostId) == g_hostToDc.end()) continue;
			
			uint32_t dcId = g_hostToDc[hostId];
			if (dcId == 0) {
				dc0_hosts.push_back(hostId);
			} else if (dcId == 1) {
				dc1_hosts.push_back(hostId);
			}
		}
	}
	
	std::cout << "[WORKLOAD DC] DC0 hosts: " << dc0_hosts.size() 
	          << ", DC1 hosts: " << dc1_hosts.size() << std::endl;
	
	// Both DCs must have the same number of hosts for mirroring
	uint32_t numHostsPerDC = std::min(dc0_hosts.size(), dc1_hosts.size());
	
	// Step 1: Generate flow pattern for DC0, record (src_idx, dst_idx, flowSize, startTime)
	// Using index-based approach so DC1 can mirror with its own host list
	struct BgFlowPattern {
		uint32_t src_idx;   // Index into dc0_hosts / dc1_hosts
		uint32_t dst_idx;   // Index into dc0_hosts / dc1_hosts
		uint64_t flowSize;
		double startTime;
	};
	std::vector<BgFlowPattern> flowPatterns;
	
	std::cout << "[WORKLOAD DC0] Generating background flows for DC0 (PER_DC mode)" << std::endl;
	// PER_DC 模式：每个 DC 的总负载为参考带宽的 load 倍
	// 公式：λ_total = (ρ * C_ref) / (8 * E[S])
	// 其中 ρ 是负载率，C_ref 是参考带宽（10 Gbps），E[S] 是平均流大小
	// 每台主机的流到达率 = λ_total / numHostsPerDC
	double requestRate = load * 1e10 / (8.0 * avgFlowSize * numHostsPerDC); // 每台主机的流到达率
	
	for (uint32_t i = 0; i < numHostsPerDC; i++) {
		double startTime = START_TIME + poission_gen_interval(requestRate);
		while (startTime < FLOW_LAUNCH_END_TIME && startTime > START_TIME) {
			uint32_t dst_idx = rand_range_uint(0, numHostsPerDC);
			if (dst_idx == i) {
				startTime += poission_gen_interval(requestRate);
				continue;
			}
			
			uint64_t flowSize = (uint64_t)gen_random_cdf(cdfTable);
			while (flowSize == 0) {
				flowSize = (uint64_t)gen_random_cdf(cdfTable);
			}
			
			// Record pattern
			flowPatterns.push_back({i, dst_idx, flowSize, startTime});
			
			// Add to DC0
			AddWorkloadFlow(dc0_hosts[i], dc0_hosts[dst_idx], 3, flowSize, startTime, flowCount, totalFlowSize);
			startTime += poission_gen_interval(requestRate);
		}
	}
	
	uint64_t dc0_totalBytes = 0;
	for (auto &p : flowPatterns) {
		dc0_totalBytes += p.flowSize;
	}
	std::cout << "[WORKLOAD DC0] " << flowPatterns.size() << " flows, "
	          << dc0_totalBytes << " bytes total" << std::endl;
	
	// Step 2: Mirror the exact same flow pattern to DC1
	// Same flow sizes, same start times, same src/dst indices (mapped to DC1 hosts)
	std::cout << "[WORKLOAD DC1] Mirroring " << flowPatterns.size() << " flows to DC1 (nodes 53-105)" << std::endl;
	uint64_t dc1_totalBytes = 0;
	for (auto &p : flowPatterns) {
		AddWorkloadFlow(dc1_hosts[p.src_idx], dc1_hosts[p.dst_idx], 3, p.flowSize, p.startTime, flowCount, totalFlowSize);
		dc1_totalBytes += p.flowSize;
	}
	std::cout << "[WORKLOAD DC1] " << flowPatterns.size() << " flows, "
	          << dc1_totalBytes << " bytes total" << std::endl;
	
	std::cout << "[WORKLOAD] Background flow generation complete. "
	          << "DC0=" << dc0_totalBytes << "B, DC1=" << dc1_totalBytes << "B, "
	          << "Total flows: " << flowCount << std::endl;
}

void InstallQueryFlowFile(const std::string& queryFlowFile,
                          double startTime,
                          long& queryFlowCount,
                          long& totalQueryFlowSize)
{
	if (queryFlowFile.empty()) {
		std::cout << "No query flow file specified, skip query flows." << std::endl;
		return;
	}
	
	std::ifstream qf(queryFlowFile.c_str());
	NS_ASSERT_MSG(qf.is_open(), "Cannot open query flow file: " << queryFlowFile);
	
	uint32_t queryFlowNum = 0;
	qf >> queryFlowNum;
	
	// Set global query flow counter
	g_totalQueryFlows = queryFlowNum;
	g_queryFlowCompleted = 0;
	g_simulationStopped = false;
	
	std::cout << "[QUERY FLOW] Loading " << queryFlowNum 
	          << " flows from " << queryFlowFile << std::endl;
	
	for (uint32_t i = 0; i < queryFlowNum; i++) {
		uint32_t src, dst, pg;
		uint16_t dport;
		uint64_t flowSize;
		double fileStartTime; // 文件中存储的startTime（作为相对偏移）
		
		qf >> src >> dst >> pg >> dport >> flowSize >> fileStartTime;
		
		// Validation
		NS_ASSERT_MSG(src < n.GetN() && dst < n.GetN(), 
		             "Query flow node id out of range");
		NS_ASSERT_MSG(n.Get(src)->GetNodeType() == 0 && n.Get(dst)->GetNodeType() == 0,
		             "Query flow selected a non-host node");
		NS_ASSERT_MSG(src != dst, "Query flow src and dst must differ");
		NS_ASSERT_MSG(flowSize > 0, "Query flow size must be positive");
		
		// 直接使用文件中的时间作为启动时间（相对于 START_TIME 的偏移）
		double adjustedStartTime = startTime + fileStartTime;
		
		// Check if this is a cross-DC flow using global topology mapping
		bool is_cross_dc = IsCrossDcFlow(src, dst);
		
		// Add flow with query flow flag and adjusted start time
		AddWorkloadFlow(src, dst, pg, flowSize, adjustedStartTime,
		               queryFlowCount, totalQueryFlowSize, dport, true);
		
		// Output detailed info (queryable)
		std::cout << "[QUERY FLOW #" << i << "] "
		          << "src=" << src 
		          << " dst=" << dst
		          << " pg=" << pg
		          << " dport=" << dport
		          << " size=" << flowSize << " bytes"
		          << " start=" << startTime << " sec"
		          << " adjusted_start=" << adjustedStartTime << " sec"
		          << " cross_dc=" << (is_cross_dc ? "yes" : "no")
		          << std::endl;
	}
	qf.close();
	
	std::cout << "[QUERY FLOW] Total query flows: " << queryFlowCount << std::endl;
	if (queryFlowCount > 0) {
		std::cout << "[QUERY FLOW] Average flow size: "
		          << static_cast<double>(totalQueryFlowSize) / queryFlowCount
		          << " bytes" << std::endl;
	}
}




int main(int argc, char *argv[])
{
	clock_t begint, endt;
	begint = clock();
	std::ifstream conf;
	bool wien = false; // PowerTCP 启用标志（ccMode=4 时自动启用）
	bool delayWien = false; // Theta-PowerTCP 启用标志 

	uint32_t SERVER_COUNT = 0;
	uint32_t LEAF_COUNT = 0;
	uint32_t SPINE_COUNT = 0;
	uint64_t LEAF_SERVER_CAPACITY = 0;
	uint64_t SPINE_LEAF_CAPACITY = 0;
	double START_TIME = 0.001;
	double FLOW_LAUNCH_END_TIME = 0.045;
	double load = 0.2;
	unsigned randomSeed = 7;
	std::string queryFlowFile = "examples/CrossDC-Algos/config/query-flow/query-flow.txt";
	std::string queryFlowFctFile = "";  // Will be set via command line

	uint32_t algorithm = 3;
	uint32_t windowCheck = 1;
	uint32_t gatewayTypeOverride = UINT32_MAX;
	std::string confFile = "examples/CrossDC-Algos/config/workload/config-workload.txt";
	std::string cdfFileName = "examples/CrossDC-Algos/config/workload/Alistorage.txt";
	
	CommandLine cmd;
	cmd.AddValue("conf", "config file path", confFile);
	cmd.AddValue("wien", "enable wien --> wien enables PowerTCP.", wien);
	cmd.AddValue("delayWien", "enable wien delay --> delayWien enables Theta-PowerTCP (delaypowertcp) ", delayWien);
	cmd.AddValue("randomSeed", "Random seed, 0 for time-based seed", randomSeed);
	cmd.AddValue("SERVER_COUNT", "max servers per discovered host-facing leaf, 0 means all", SERVER_COUNT);
	cmd.AddValue("LEAF_COUNT", "max discovered host-facing leaves to use, 0 means all", LEAF_COUNT);
	cmd.AddValue("SPINE_COUNT", "fallback uplink count when topology capacity cannot be inferred", SPINE_COUNT);
	cmd.AddValue("LEAF_SERVER_CAPACITY", "fallback leaf-server capacity in Gbps if <1e9, or bps", LEAF_SERVER_CAPACITY);
	cmd.AddValue("SPINE_LEAF_CAPACITY", "fallback spine-leaf capacity in Gbps if <1e9, or bps", SPINE_LEAF_CAPACITY);
	cmd.AddValue("fctOutputFile", "FCT output file path", fct_output_file);
	cmd.AddValue("pfcOutputFile", "PFC output file path", pfc_output_file);
	cmd.AddValue("queryFlowFctFile", "Query flow FCT output file path", queryFlowFctFile);
	cmd.AddValue ("algorithm", "specify CC mode. This is added for my convinience. I prefer cmd rather than parsing files.", algorithm);
	cmd.AddValue("windowCheck", "windowCheck", windowCheck);
	cmd.AddValue("gatewayType", "Gateway type override, 0 disables gateway, 2 enables ATC gateway", gatewayTypeOverride);

	cmd.Parse (argc, argv);
	std::cout << confFile << endl;

	if (LEAF_SERVER_CAPACITY > 0 && LEAF_SERVER_CAPACITY < LINK_CAPACITY_BASE)
		LEAF_SERVER_CAPACITY *= LINK_CAPACITY_BASE;
	if (SPINE_LEAF_CAPACITY > 0 && SPINE_LEAF_CAPACITY < LINK_CAPACITY_BASE)
		SPINE_LEAF_CAPACITY *= LINK_CAPACITY_BASE;
	if (randomSeed == 0)
		srand((unsigned)time(NULL));
	else
		srand(randomSeed);
	conf.open(confFile.c_str());
	while (!conf.eof())
	{
		std::string key;
		conf >> key;
		if (key.compare("ENABLE_QCN") == 0)
		{
			uint32_t v;
			conf >> v;
			enable_qcn = v;
			if (enable_qcn)
				std::cout << "ENABLE_QCN\t\t\t" << "Yes" << "\n";
			else
				std::cout << "ENABLE_QCN\t\t\t" << "No" << "\n";
		}
		else if (key.compare("CLAMP_TARGET_RATE") == 0)
		{
			uint32_t v;
			conf >> v;
			clamp_target_rate = v;
			if (clamp_target_rate)
				std::cout << "CLAMP_TARGET_RATE\t\t" << "Yes" << "\n";
			else
				std::cout << "CLAMP_TARGET_RATE\t\t" << "No" << "\n";
		}
		else if (key.compare("PAUSE_TIME") == 0)
		{
			double v;
			conf >> v;
			pause_time = v;
			std::cout << "PAUSE_TIME\t\t\t" << pause_time << "\n";
		}
		else if (key.compare("DATA_RATE") == 0)
		{
			std::string v;
			conf >> v;
			data_rate = v;
			std::cout << "DATA_RATE\t\t\t" << data_rate << "\n";
		}
		else if (key.compare("LINK_DELAY") == 0)
		{
			std::string v;
			conf >> v;
			link_delay = v;
			std::cout << "LINK_DELAY\t\t\t" << link_delay << "\n";
		}
		else if (key.compare("PACKET_PAYLOAD_SIZE") == 0)
		{
			uint32_t v;
			conf >> v;
			packet_payload_size = v;
			std::cout << "PACKET_PAYLOAD_SIZE\t\t" << packet_payload_size << "\n";
		}
		else if (key.compare("L2_CHUNK_SIZE") == 0)
		{
			uint32_t v;
			conf >> v;
			l2_chunk_size = v;
			std::cout << "L2_CHUNK_SIZE\t\t\t" << l2_chunk_size << "\n";
		}
		else if (key.compare("L2_ACK_INTERVAL") == 0)
		{
			uint32_t v;
			conf >> v;
			l2_ack_interval = v;
			std::cout << "L2_ACK_INTERVAL\t\t\t" << l2_ack_interval << "\n";
		}
		else if (key.compare("L2_BACK_TO_ZERO") == 0)
		{
			uint32_t v;
			conf >> v;
			l2_back_to_zero = v;
			if (l2_back_to_zero)
				std::cout << "L2_BACK_TO_ZERO\t\t\t" << "Yes" << "\n";
			else
				std::cout << "L2_BACK_TO_ZERO\t\t\t" << "No" << "\n";
		}
		else if (key.compare("TOPOLOGY_FILE") == 0)
		{
			std::string v;
			conf >> v;
			topology_file = v;
			std::cout << "TOPOLOGY_FILE\t\t\t" << topology_file << "\n";
		}
		else if (key.compare("FLOW_FILE") == 0)
		{
			std::string v;
			conf >> v;
			flow_file = v;
			std::cout << "FLOW_FILE\t\t\t" << flow_file << "\n";
		}
		else if (key.compare("TRACE_FILE") == 0)
		{
			std::string v;
			conf >> v;
			trace_file = v;
			std::cout << "TRACE_FILE\t\t\t" << trace_file << "\n";
		}
		else if (key.compare("TRACE_OUTPUT_FILE") == 0)
		{
			std::string v;
			conf >> v;
			trace_output_file = v;
			if (argc > 2)
			{
				trace_output_file = trace_output_file + std::string(argv[2]);
			}
			std::cout << "TRACE_OUTPUT_FILE\t\t" << trace_output_file << "\n";
		}
		else if (key.compare("SIMULATOR_STOP_TIME") == 0)
		{
			double v;
			conf >> v;
			simulator_stop_time = v;
			std::cout << "SIMULATOR_STOP_TIME\t\t" << simulator_stop_time << "\n";
		}
		else if (key.compare("ALPHA_RESUME_INTERVAL") == 0)
		{
			double v;
			conf >> v;
			alpha_resume_interval = v;
			std::cout << "ALPHA_RESUME_INTERVAL\t\t" << alpha_resume_interval << "\n";
		}
		else if (key.compare("RP_TIMER") == 0)
		{
			double v;
			conf >> v;
			rp_timer = v;
			std::cout << "RP_TIMER\t\t\t" << rp_timer << "\n";
		}
		else if (key.compare("EWMA_GAIN") == 0)
		{
			double v;
			conf >> v;
			ewma_gain = v;
			std::cout << "EWMA_GAIN\t\t\t" << ewma_gain << "\n";
		}
		else if (key.compare("FAST_RECOVERY_TIMES") == 0)
		{
			uint32_t v;
			conf >> v;
			fast_recovery_times = v;
			std::cout << "FAST_RECOVERY_TIMES\t\t" << fast_recovery_times << "\n";
		}
		else if (key.compare("RATE_AI") == 0)
		{
			std::string v;
			conf >> v;
			rate_ai = v;
			std::cout << "RATE_AI\t\t\t\t" << rate_ai << "\n";
		}
		else if (key.compare("RATE_HAI") == 0)
		{
			std::string v;
			conf >> v;
			rate_hai = v;
			std::cout << "RATE_HAI\t\t\t" << rate_hai << "\n";
		}
		else if (key.compare("ERROR_RATE_PER_LINK") == 0)
		{
			double v;
			conf >> v;
			error_rate_per_link = v;
			std::cout << "ERROR_RATE_PER_LINK\t\t" << error_rate_per_link << "\n";
		}
		else if (key.compare("CC_MODE") == 0) {
			conf >> cc_mode;
			std::cout << "CC_MODE\t\t" << cc_mode << '\n';
		}
		else if (key.compare("POWERTCP_ENABLED") == 0) {
			uint32_t v;
			conf >> v;
			wien = (v != 0);
			std::cout << "POWERTCP_ENABLED\t\t" << (wien ? 1 : 0) << "\n";
		}
		else if (key.compare("POWERTCP_DELAY") == 0) {
			uint32_t v;
			conf >> v;
			delayWien = (v != 0);
			std::cout << "POWERTCP_DELAY\t\t\t" << (delayWien ? 1 : 0) << "\n";
		}

		else if (key.compare("GATEWAY_TYPE") == 0) {
			conf >> gatewayFlag;
			std::cout << "GATEWAY_TYPE		" << gatewayFlag << "\n";
		} else if (key.compare("GATEWAY_SRC") == 0) {
			int n_nodes;
			conf >> n_nodes;
			std::cout << "GATEWAY_SRC				";
			for (int i = 0; i < n_nodes; i++) {
				uint32_t nodeId;
				conf >> nodeId;
				auto it = gatewayNodeType.find(nodeId);
				gatewayNodeType[nodeId] = (it == gatewayNodeType.end()) ? 1 : (it->second | 1);
				std::cout << ' ' << nodeId;
			}
			std::cout << '\n';
		} else if (key.compare("GATEWAY_DST") == 0) {
			int n_nodes;
			conf >> n_nodes;
			std::cout << "GATEWAY_DST				";
			for (int i = 0; i < n_nodes; i++) {
				uint32_t nodeId;
				conf >> nodeId;
				auto it = gatewayNodeType.find(nodeId);
				gatewayNodeType[nodeId] = (it == gatewayNodeType.end()) ? 2 : (it->second | 2);
				std::cout << ' ' << nodeId;
			}
			std::cout << '\n';
			
		} else if (key.compare("RATE_DECREASE_INTERVAL") == 0) {
			double v;
			conf >> v;
			rate_decrease_interval = v;
			std::cout << "RATE_DECREASE_INTERVAL\t\t" << rate_decrease_interval << "\n";
		} else if (key.compare("MIN_RATE") == 0) {
			conf >> min_rate;
			std::cout << "MIN_RATE\t\t" << min_rate << "\n";
		} else if (key.compare("FCT_OUTPUT_FILE") == 0) {
			conf >> fct_output_file;
			std::cout << "FCT_OUTPUT_FILE\t\t" << fct_output_file << '\n';
		} else if (key.compare("HAS_WIN") == 0) {
			conf >> has_win;
			std::cout << "HAS_WIN\t\t" << has_win << "\n";
		} else if (key.compare("GLOBAL_T") == 0) {
			conf >> global_t;
			std::cout << "GLOBAL_T\t\t" << global_t << '\n';
		} else if (key.compare("MI_THRESH") == 0) {
			conf >> mi_thresh;
			std::cout << "MI_THRESH\t\t" << mi_thresh << '\n';
		} else if (key.compare("VAR_WIN") == 0) {
			uint32_t v;
			conf >> v;
			var_win = v;
			std::cout << "VAR_WIN\t\t" << v << '\n';
		} else if (key.compare("FAST_REACT") == 0) {
			uint32_t v;
			conf >> v;
			fast_react = v;
			std::cout << "FAST_REACT\t\t" << v << '\n';
		} else if (key.compare("U_TARGET") == 0) {
			conf >> u_target;
			std::cout << "U_TARGET\t\t" << u_target << '\n';
		} else if (key.compare("INT_MULTI") == 0) {
			conf >> int_multi;
			std::cout << "INT_MULTI\t\t\t\t" << int_multi << '\n';
		} else if (key.compare("RATE_BOUND") == 0) {
			uint32_t v;
			conf >> v;
			rate_bound = v;
			std::cout << "RATE_BOUND\t\t" << rate_bound << '\n';
		} else if (key.compare("ACK_HIGH_PRIO") == 0) {
			conf >> ack_high_prio;
			std::cout << "ACK_HIGH_PRIO\t\t" << ack_high_prio << '\n';
		} else if (key.compare("DCTCP_RATE_AI") == 0) {
			conf >> dctcp_rate_ai;
			std::cout << "DCTCP_RATE_AI\t\t\t\t" << dctcp_rate_ai << "\n";
		} else if (key.compare("PFC_OUTPUT_FILE") == 0) {
			conf >> pfc_output_file;
			std::cout << "PFC_OUTPUT_FILE\t\t\t\t" << pfc_output_file << '\n';
		} else if (key.compare("LINK_DOWN") == 0) {
			conf >> link_down_time >> link_down_A >> link_down_B;
			std::cout << "LINK_DOWN\t\t\t\t" << link_down_time << ' ' << link_down_A << ' ' << link_down_B << '\n';
		} else if (key.compare("ENABLE_TRACE") == 0) {
			conf >> enable_trace;
			std::cout << "ENABLE_TRACE\t\t\t\t" << enable_trace << '\n';
		} else if (key.compare("KMAX_MAP") == 0) {
			int n_k ;
			conf >> n_k;
			std::cout << "KMAX_MAP\t\t\t\t";
			for (int i = 0; i < n_k; i++) {
				uint64_t rate;
				uint32_t k;
				conf >> rate >> k;
				rate2kmax[rate] = k;
				std::cout << ' ' << rate << ' ' << k;
			}
			std::cout << '\n';
		} else if (key.compare("KMIN_MAP") == 0) {
			int n_k ;
			conf >> n_k;
			std::cout << "KMIN_MAP\t\t\t\t";
			for (int i = 0; i < n_k; i++) {
				uint64_t rate;
				uint32_t k;
				conf >> rate >> k;
				rate2kmin[rate] = k;
				std::cout << ' ' << rate << ' ' << k;
			}
			std::cout << '\n';
		} else if (key.compare("PMAX_MAP") == 0) {
			int n_k ;
			conf >> n_k;
			std::cout << "PMAX_MAP\t\t\t\t";
			for (int i = 0; i < n_k; i++) {
				uint64_t rate;
				double p;
				conf >> rate >> p;
				rate2pmax[rate] = p;
				std::cout << ' ' << rate << ' ' << p;
			}
			std::cout << '\n';
		} else if (key.compare("BUFFER_SIZE") == 0) {
			conf >> buffer_size;
			std::cout << "BUFFER_SIZE\t\t\t\t" << buffer_size << '\n';
		} else if (key.compare("QLEN_MON_FILE") == 0) {
			conf >> qlen_mon_file;
			std::cout << "QLEN_MON_FILE\t\t\t\t" << qlen_mon_file << '\n';
		} else if (key.compare("QLEN_MON_START") == 0) {
			conf >> qlen_mon_start;
			std::cout << "QLEN_MON_START\t\t\t\t" << qlen_mon_start << '\n';
		} else if (key.compare("QLEN_MON_END") == 0) {
			conf >> qlen_mon_end;
			std::cout << "QLEN_MON_END\t\t\t\t" << qlen_mon_end << '\n';
		} else if (key.compare("BOTTLENECK_MON_FILE") == 0) {
			conf >> bottleneck_mon_file;
			std::cout << "BOTTLENECK_MON_FILE\t\t" << bottleneck_mon_file << '\n';
		} else if (key.compare("BOTTLENECK_MON_INTERVAL") == 0) {
			conf >> bottleneck_mon_interval;
			std::cout << "BOTTLENECK_MON_INTERVAL\t" << bottleneck_mon_interval << " ns\n";
		} else if (key.compare("MULTI_RATE") == 0) {
			int v;
			conf >> v;
			multi_rate = v;
			std::cout << "MULTI_RATE\t\t\t\t" << multi_rate << '\n';
		} else if (key.compare("SAMPLE_FEEDBACK") == 0) {
			int v;
			conf >> v;
			sample_feedback = v;
			std::cout << "SAMPLE_FEEDBACK\t\t\t\t" << sample_feedback << '\n';
		} else if (key.compare("PINT_LOG_BASE") == 0) {
			conf >> pint_log_base;
			std::cout << "PINT_LOG_BASE\t\t\t\t" << pint_log_base << '\n';
		} else if (key.compare("PINT_PROB") == 0) {
			conf >> pint_prob;
			std::cout << "PINT_PROB\t\t\t\t" << pint_prob << '\n';
		} else if (key.compare("NIC_DELAY") == 0) {
			// 读取网卡处理延迟（统一纳秒单位）
			conf >> nic_delay_ns;
			std::cout << "NIC_DELAY\t\t\t\t" << nic_delay_ns << "ns (" 
			          << (nic_delay_ns / 1000.0) << "μs)" << '\n';
		}
		// ===== workload 参数从配置文件读取 =====
		else if (key.compare("CDF_FILE_NAME") == 0) {
			conf >> cdfFileName;
			std::cout << "CDF_FILE_NAME\t\t\t" << cdfFileName << '\n';
		}
		else if (key.compare("LOAD") == 0) {
			conf >> load;
			std::cout << "LOAD\t\t\t\t" << load << '\n';
		}
		else if (key.compare("START_TIME") == 0) {
			conf >> START_TIME;
			std::cout << "START_TIME\t\t\t" << START_TIME << '\n';
		}
		else if (key.compare("FLOW_LAUNCH_END_TIME") == 0) {
			conf >> FLOW_LAUNCH_END_TIME;
			std::cout << "FLOW_LAUNCH_END_TIME\t\t" << FLOW_LAUNCH_END_TIME << '\n';
		}
		else if (key.compare("QUERY_FLOW_FILE") == 0) {
			conf >> queryFlowFile;
			std::cout << "QUERY_FLOW_FILE		" << queryFlowFile << '\n';
		}
		else if (key.compare("QUERY_AUTO_STOP") == 0) {
			conf >> query_auto_stop;
			std::cout << "QUERY_AUTO_STOP		" << query_auto_stop << '\n';
		}
		else if (key.compare("ENABLE_FLOW_FILE_BACKGROUND") == 0) {
			uint32_t v;
			conf >> v;
			enable_flow_file_background = v;
			std::cout << "ENABLE_FLOW_FILE_BACKGROUND	" << enable_flow_file_background << '\n';
		}
		else if (key.compare("BACKGROUND_FLOW_FILE") == 0) {
			conf >> background_flow_file;
			std::cout << "BACKGROUND_FLOW_FILE		" << background_flow_file << '\n';
		}
		else if (key.compare("FCT_OUTPUT_TEMPLATE") == 0) {
			std::string ignored;
			conf >> ignored;
			std::cout << "FCT_OUTPUT_TEMPLATE		" << ignored << '\n';
		}
		else if (key.compare("PFC_OUTPUT_TEMPLATE") == 0) {
			std::string ignored;
			conf >> ignored;
			std::cout << "PFC_OUTPUT_TEMPLATE		" << ignored << '\n';
		}
		else if (key.compare("QUERY_FCT_OUTPUT_TEMPLATE") == 0) {
			std::string ignored;
			conf >> ignored;
			std::cout << "QUERY_FCT_OUTPUT_TEMPLATE	" << ignored << '\n';
		}
		fflush(stdout);
	}
	conf.close();
	if (gatewayTypeOverride != UINT32_MAX) {
		gatewayFlag = gatewayTypeOverride;
		std::cout << "GATEWAY_TYPE_OVERRIDE	" << gatewayFlag << "\n";
	}

	// Python selects the CC algorithm for each run; workload parameters stay in config-workload.txt.
	cc_mode = algorithm; // overrides configuration file
	has_win = windowCheck; // overrides configuration file
	var_win = windowCheck; // overrides configuration file
	
	// PowerTCP (ccMode=4): 自动启用
	if (cc_mode == 4) {
		wien = true; // 自动启用 PowerTCP
		std::cout << "[POWERTCP] ccMode=4 detected, automatically enabled PowerTCP" << std::endl;
	}
	

	// workload parameters are owned by config-workload.txt.
	//simulator_stop_time = END_TIME;


	Config::SetDefault("ns3::QbbNetDevice::PauseTime", UintegerValue(pause_time));
	Config::SetDefault("ns3::QbbNetDevice::QcnEnabled", BooleanValue(enable_qcn));
	
	// 设置全局网卡处理延迟
	Config::SetDefault("ns3::QbbNetDevice::NicDelay", TimeValue(NanoSeconds(nic_delay_ns)));
	NS_LOG_INFO("NIC delay set to " << nic_delay_ns << "ns (" << (nic_delay_ns / 1000.0) << "μs)");

	// set int_multi
	IntHop::multi = int_multi;
	// IntHeader::mode
	if (cc_mode == 7) // timely, use ts
		IntHeader::mode = IntHeader::TS;
	else if (cc_mode == 3 || cc_mode == 4) // hpcc (3) or powertcp (4), use int
		IntHeader::mode = IntHeader::NORMAL;
	else if (cc_mode == 10) // hpcc-pint
		IntHeader::mode = IntHeader::PINT;
	else // others, no extra header
		IntHeader::mode = IntHeader::NONE;

	// Set Pint
	if (cc_mode == 10) {
		Pint::set_log_base(pint_log_base);
		IntHeader::pint_bytes = Pint::get_n_bytes();
	}

	topof.open(topology_file.c_str());
	uint32_t node_num, switch_num, tors, link_num, trace_num;
	uint32_t long_link_count = 0; // 长链路数目
	topof >> node_num >> switch_num >> tors >> link_num >> long_link_count; // 读取长链路数目
	tors = switch_num;
	std::cout << node_num << " " << switch_num << " " << tors <<  " " << link_num << " " << long_link_count << std::endl;
	flow_num = 0;
	
	// Read long-distance link indices
	std::set<uint32_t> long_link_indices;
	for (uint32_t i = 0; i < long_link_count; i++) {
		uint32_t link_idx;
		topof >> link_idx;
		long_link_indices.insert(link_idx);
		std::cout << "[TOPOLOGY] Long-distance link index: " << link_idx << std::endl;
	}
	
	// Long-distance RTT will be set during link parsing below

	NodeContainer serverNodes;
	NodeContainer torNodes;
	NodeContainer spineNodes;
	NodeContainer switchNodes;
	NodeContainer allNodes;

	std::vector<uint32_t> node_type(node_num, 0);
	std::cout << "switch_num " << switch_num << std::endl;
	for (uint32_t i = 0; i < switch_num; i++) {
		uint32_t sid;
		topof >> sid;
		std::cout << "sid " << sid << std::endl;
		switchNumToId[i] = sid;
		switchIdToNum[sid] = i;
		if (i < tors) {
			node_type[sid] = 1;
		}
		else
			node_type[sid] = 2;

	}

	for (uint32_t i = 0; i < node_num; i++) {
		if (node_type[i] == 0) {
			Ptr<Node> node = CreateObject<Node>();
			n.Add(node);
			allNodes.Add(node);
			serverNodes.Add(node);
		}
		else {
			Ptr<SwitchNode> sw = CreateObject<SwitchNode>();
			n.Add(sw);
			switchNodes.Add(sw);
			allNodes.Add(sw);
			sw->SetAttribute("EcnEnabled", BooleanValue(enable_qcn));
			if (node_type[i] == 1) {
				torNodes.Add(sw);
				sw->SetNodeType(1);
			}
			else {
				spineNodes.Add(sw);
				sw->SetNodeType(2);
			}

		}
	}


	NS_LOG_INFO("Create nodes.");

	InternetStackHelper internet;
	Ipv4GlobalRoutingHelper globalRoutingHelper;
	internet.SetRoutingHelper (globalRoutingHelper);
	internet.Install(n);

	//
	// Assign temporary IP to each server (will be updated by AutoAssignTopologyIps)
	//
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType() == 0) { // is server
			serverAddress.resize(i + 1);
			serverAddress[i] = Ipv4Address("10.0.0.1");  // 临时IP，后面会被AutoAssignTopologyIps更新
		}
	}

	NS_LOG_INFO("Create channels.");

	//
	// Explicitly create the channels required by the topology.
	//

	Ptr<RateErrorModel> rem = CreateObject<RateErrorModel>();
	Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
	rem->SetRandomVariable(uv);
	uv->SetStream(50);
	rem->SetAttribute("ErrorRate", DoubleValue(error_rate_per_link));
	rem->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));

	FILE *pfc_file = fopen(pfc_output_file.c_str(), "w");

	QbbHelper qbb;
	Ipv4AddressHelper ipv4;
	for (uint32_t i = 0; i < link_num; i++)
	{
		uint32_t src, dst;
		std::string data_rate, link_delay;
		double error_rate;
		topof >> src >> dst >> data_rate >> link_delay >> error_rate;

		std::cout << src << " " << dst << " " << n.GetN() << " " << data_rate << " " << link_delay << " " << error_rate << std::endl;
		Ptr<Node> snode = n.Get(src), dnode = n.Get(dst);

		// Check if this is a long-distance link (from topology file)
		if (long_link_indices.count(i) > 0) {
			// Mark both endpoints as DCI switches
			dci_switches.insert(src);
			dci_switches.insert(dst);
			
			// Parse delay to calculate RTT
			double delay_ns = 0;
			if (link_delay.find("us") != std::string::npos) {
				delay_ns = std::stod(link_delay) * 1000;
			} else if (link_delay.find("ms") != std::string::npos) {
				delay_ns = std::stod(link_delay) * 1000000;
			} else if (link_delay.find("ns") != std::string::npos) {
				delay_ns = std::stod(link_delay);
			} else {
				delay_ns = std::stod(link_delay);
			}
			g_longDistanceRtt = static_cast<uint64_t>(delay_ns * 2);
			longHaulBandwidth = DataRate(data_rate).GetBitRate();
			longHaulDelay = Time(link_delay);
			
			std::cout << "[TOPOLOGY] Long-distance link #" << i << ": " << src << " <-> " << dst
			          << " delay=" << link_delay << " RTT=" << g_longDistanceRtt << "ns" << std::endl;
			std::cout << "[TOPOLOGY] DCI switches: " << src << ", " << dst << std::endl;
		}

		qbb.SetDeviceAttribute("DataRate", StringValue(data_rate));
		qbb.SetChannelAttribute("Delay", StringValue(link_delay));
		if (error_rate > 0)
		{
			Ptr<RateErrorModel> rem = CreateObject<RateErrorModel>();
			Ptr<UniformRandomVariable> uv = CreateObject<UniformRandomVariable>();
			rem->SetRandomVariable(uv);
			uv->SetStream(50);
			rem->SetAttribute("ErrorRate", DoubleValue(error_rate));
			rem->SetAttribute("ErrorUnit", StringValue("ERROR_UNIT_PACKET"));
			qbb.SetDeviceAttribute("ReceiveErrorModel", PointerValue(rem));
		}
		else
		{
			qbb.SetDeviceAttribute("ReceiveErrorModel", PointerValue(rem));
		}

		fflush(stdout);

		// Assigne server IP
		// Note: this should be before the automatic assignment below (ipv4.Assign(d)),
		// because we want our IP to be the primary IP (first in the IP address list),
		// so that the global routing is based on our IP
		NetDeviceContainer d = qbb.Install(snode, dnode);
		if (snode->GetNodeType() == 0) {
			Ptr<Ipv4> ipv4 = snode->GetObject<Ipv4>();
			ipv4->AddInterface(d.Get(0));
			ipv4->AddAddress(1, Ipv4InterfaceAddress(serverAddress[src], Ipv4Mask(0xff000000)));
		}
		if (dnode->GetNodeType() == 0) {
			Ptr<Ipv4> ipv4 = dnode->GetObject<Ipv4>();
			ipv4->AddInterface(d.Get(1));
			ipv4->AddAddress(1, Ipv4InterfaceAddress(serverAddress[dst], Ipv4Mask(0xff000000)));
		}


		if (!snode->GetNodeType()) {
			sourceNodes[src].Add(DynamicCast<QbbNetDevice>(d.Get(0)));
		}

		if (!snode->GetNodeType() && dnode->GetNodeType()) {
			switchDown[switchIdToNum[dst]].Add(DynamicCast<QbbNetDevice>(d.Get(1)));
		}


		if (snode->GetNodeType() && dnode->GetNodeType()) {
			switchToSwitchInterfaces.Add(d);
			switchUp[switchIdToNum[src]].Add(DynamicCast<QbbNetDevice>(d.Get(0)));
			switchUp[switchIdToNum[dst]].Add(DynamicCast<QbbNetDevice>(d.Get(1)));
			switchToSwitch[src][dst].push_back(DynamicCast<QbbNetDevice>(d.Get(0)));
			switchToSwitch[src][dst].push_back(DynamicCast<QbbNetDevice>(d.Get(1)));
		}

		// This is just to set up the connectivity between nodes. The IP addresses are useless
		// char ipstring[16];
		std::stringstream ipstring;
		ipstring << "10." << i / 254 + 1 << "." << i % 254 + 1 << ".0";
		// sprintf(ipstring, "10.%d.%d.0", i / 254 + 1, i % 254 + 1);
		ipv4.SetBase(ipstring.str().c_str(), "255.255.255.0");
		ipv4.Assign(d);

		// used to create a graph of the topology
		// 必须在ipv4.Assign(d)之后调用GetInterfaceForDevice
		// 因为Assign才会把NetDevice注册到Ipv4协议栈，之前GetInterfaceForDevice返回-1
		Ptr<Ipv4> sIpv4 = snode->GetObject<Ipv4>();
		Ptr<Ipv4> dIpv4 = dnode->GetObject<Ipv4>();

		nbr2if[snode][dnode].idx = sIpv4->GetInterfaceForDevice(d.Get(0));
		nbr2if[snode][dnode].up = true;
		nbr2if[snode][dnode].delay = DynamicCast<QbbChannel>(DynamicCast<QbbNetDevice>(d.Get(0))->GetChannel())->GetDelay().GetTimeStep();
		nbr2if[snode][dnode].bw = DynamicCast<QbbNetDevice>(d.Get(0))->GetDataRate().GetBitRate();
		nbr2if[dnode][snode].idx = dIpv4->GetInterfaceForDevice(d.Get(1));
		nbr2if[dnode][snode].up = true;
		nbr2if[dnode][snode].delay = DynamicCast<QbbChannel>(DynamicCast<QbbNetDevice>(d.Get(1))->GetChannel())->GetDelay().GetTimeStep();
		nbr2if[dnode][snode].bw = DynamicCast<QbbNetDevice>(d.Get(1))->GetDataRate().GetBitRate();

		// setup PFC trace
		DynamicCast<QbbNetDevice>(d.Get(0))->TraceConnectWithoutContext("QbbPfc", MakeBoundCallback (&get_pfc, pfc_file, DynamicCast<QbbNetDevice>(d.Get(0))));
		DynamicCast<QbbNetDevice>(d.Get(1))->TraceConnectWithoutContext("QbbPfc", MakeBoundCallback (&get_pfc, pfc_file, DynamicCast<QbbNetDevice>(d.Get(1))));
	}

	nic_rate = get_nic_rate(n);
	// config switch
	// The switch mmu runs Dynamic Thresholds (DT) by default.
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType()) { // is switch
			Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(n.Get(i));
			uint32_t shift = 3; // by default 1/8
			double alpha = 1.0 / 8;
			sw->m_mmu->SetAlphaIngress(alpha);
			sw->m_mmu->SetAlphaEgress(UINT16_MAX);

			bool isDciSwitch = (dci_switches.count(i) > 0); // Use DCI switch set from topology
			uint32_t headroomMultiplier = isDciSwitch ? 3 : 20;

			uint64_t totalHeadroom = 0;
			for (uint32_t j = 1; j < sw->GetNDevices(); j++) {

				for (uint32_t qu = 0; qu < 8; qu++) {
					Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(sw->GetDevice(j));
					// set ecn
					uint64_t rate = dev->GetDataRate().GetBitRate();
					NS_ASSERT_MSG(rate2kmin.find(rate) != rate2kmin.end(), "must set kmin for each link speed");
					NS_ASSERT_MSG(rate2kmax.find(rate) != rate2kmax.end(), "must set kmax for each link speed");
					NS_ASSERT_MSG(rate2pmax.find(rate) != rate2pmax.end(), "must set pmax for each link speed");
					sw->m_mmu->ConfigEcn(j, rate2kmin[rate], rate2kmax[rate], rate2pmax[rate]);
					// set pfc
					uint64_t delay = DynamicCast<QbbChannel>(dev->GetChannel())->GetDelay().GetTimeStep();
					uint32_t headroom = rate * delay / 8 / 1000000000 * headroomMultiplier;

					sw->m_mmu->SetHeadroom(headroom, j, qu);
					totalHeadroom += headroom;
				}

			}
			sw->m_mmu->SetBufferPool(buffer_size * 1024 * 1024 + totalHeadroom);
			sw->m_mmu->SetIngressPool(buffer_size * 1024 * 1024);
			sw->m_mmu->SetEgressLosslessPool(buffer_size * 1024 * 1024);
			sw->m_mmu->node_id = sw->GetId();

			// ATC (ccMode=15): 设置 Gateway 类型，SwitchNode 会自动初始化
			if (cc_mode == 15) {
				// 设置配置文件路径
				sw->SetConfigFilePath(confFile);
				
				auto gatewayIt = gatewayNodeType.find(i);
				if (gatewayIt != gatewayNodeType.end()) {
					// 设置 Gateway 类型并初始化
					sw->SetAttribute("GatewayType", UintegerValue(gatewayIt->second));
					sw->ReadAtcConfigFromFile(confFile);
					sw->InitializeAtcGateway(gatewayIt->second);
				}
			}
		}
	}

#if ENABLE_QP
	FILE *fct_output = fopen(fct_output_file.c_str(), "w");
	
	// Open query flow FCT file
	if (!queryFlowFile.empty()) {
		// If not specified, use default based on algorithm
		if (queryFlowFctFile.empty()) {
			std::string algo_suffix;
			if (algorithm == 13) algo_suffix = "frp";
			else if (algorithm == 14) algo_suffix = "rocc";
			else if (algorithm == 1) algo_suffix = "dcqcn";  // DCQCN is ccMode=1
			else if (algorithm == 3) algo_suffix = "hpcc";  // HPCC uses ccMode=3
			else if (algorithm == 4) algo_suffix = "powertcp";  // PowerTCP uses ccMode=4
		
			else if (algorithm == 7) algo_suffix = "timely";  // Timely is ccMode=7
			else if (algorithm == 10) algo_suffix = "dctcp";
			else algo_suffix = "unknown";
			
			queryFlowFctFile = "/home/shemuping/newCode/ns3-FRP/results/workload/fct/query-flow-" + algo_suffix + ".txt";
		}
		g_queryFlowFctFile = fopen(queryFlowFctFile.c_str(), "w");
		if (g_queryFlowFctFile != nullptr) {
			std::cout << "[QUERY FLOW] Output file: " << queryFlowFctFile << std::endl;
			// Write header comment
			fprintf(g_queryFlowFctFile, "# Query Flow FCT Results\n");
			fprintf(g_queryFlowFctFile, "# Format: src_node dst_node pg dport flow_size(B) start_time(ns) fct(ns) is_cross_dc\n");
			fprintf(g_queryFlowFctFile, "# Note: FCT is raw (no compensation) for all flows\n");
			fflush(g_queryFlowFctFile);
		} else {
			std::cerr << "Warning: Cannot open query flow FCT file: " << queryFlowFctFile << std::endl;
		}
	}
	//
	// install RDMA driver
	//
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType() == 0) { // is server
			// create RdmaHw
			Ptr<RdmaHw> rdmaHw = CreateObject<RdmaHw>();
			rdmaHw->SetAttribute("ClampTargetRate", BooleanValue(clamp_target_rate));
			rdmaHw->SetAttribute("AlphaResumInterval", DoubleValue(alpha_resume_interval));
			rdmaHw->SetAttribute("RPTimer", DoubleValue(rp_timer));
			rdmaHw->SetAttribute("FastRecoveryTimes", UintegerValue(fast_recovery_times));
			rdmaHw->SetAttribute("EwmaGain", DoubleValue(ewma_gain));
			rdmaHw->SetAttribute("RateAI", DataRateValue(DataRate(rate_ai)));
			rdmaHw->SetAttribute("RateHAI", DataRateValue(DataRate(rate_hai)));
			rdmaHw->SetAttribute("L2BackToZero", BooleanValue(l2_back_to_zero));
			rdmaHw->SetAttribute("L2ChunkSize", UintegerValue(l2_chunk_size));
			rdmaHw->SetAttribute("L2AckInterval", UintegerValue(l2_ack_interval));
			rdmaHw->SetAttribute("CcMode", UintegerValue(cc_mode));
			rdmaHw->SetAttribute("RateDecreaseInterval", DoubleValue(rate_decrease_interval));
			rdmaHw->SetAttribute("MinRate", DataRateValue(DataRate(min_rate)));
			rdmaHw->SetAttribute("Mtu", UintegerValue(packet_payload_size));
			rdmaHw->SetAttribute("MiThresh", UintegerValue(mi_thresh));
			rdmaHw->SetAttribute("VarWin", BooleanValue(var_win));
			rdmaHw->SetAttribute("FastReact", BooleanValue(fast_react));
			rdmaHw->SetAttribute("MultiRate", BooleanValue(multi_rate));
			rdmaHw->SetAttribute("SampleFeedback", BooleanValue(sample_feedback));
			rdmaHw->SetAttribute("TargetUtil", DoubleValue(u_target));
			rdmaHw->SetAttribute("RateBound", BooleanValue(rate_bound));
			rdmaHw->SetAttribute("DctcpRateAI", DataRateValue(DataRate(dctcp_rate_ai)));
			rdmaHw->SetAttribute("PowerTCPEnabled", BooleanValue(wien));
			rdmaHw->SetAttribute("PowerTCPdelay", BooleanValue(delayWien));
			rdmaHw->SetPintSmplThresh(pint_prob);
			// create and install RdmaDriver
			Ptr<RdmaDriver> rdma = CreateObject<RdmaDriver>();
			Ptr<Node> node = n.Get(i);
			rdma->SetNode(node);
			rdma->SetRdmaHw(rdmaHw);

			node->AggregateObject (rdma);
			rdma->Init();
			rdma->TraceConnectWithoutContext("QpComplete", MakeBoundCallback (qp_finish, fct_output));
		}
	}

#endif

	// set ACK priority on hosts
	if (ack_high_prio)
		RdmaEgressQueue::ack_q_idx = 0;
	else
		RdmaEgressQueue::ack_q_idx = 3;

	// setup routing
	// 先分配真实IP，再配置路由表，确保路由表使用正确的IP
	AutoAssignTopologyIps(n);
	CalculateRoutes(n);
	SetRoutingEntries();

	// ==================== 路由表审查区 ====================
	std::cout << "\n================ [ROUTING TABLE CHECK] ================" << std::endl;
	uint32_t total_empty_nodes = 0;
	for (uint32_t i = 0; i < node_num; i++) {
		Ptr<Node> node = n.Get(i);
		Ptr<Ipv4> ipv4 = node->GetObject<Ipv4>();
		Ptr<Ipv4RoutingProtocol> routingProto = ipv4->GetRoutingProtocol();
		
		// 尝试多种方式获取路由信息
		Ptr<Ipv4ListRouting> listRouting = DynamicCast<Ipv4ListRouting>(routingProto);
		Ptr<Ipv4StaticRouting> staticRouting = DynamicCast<Ipv4StaticRouting>(routingProto);
		
		if (listRouting) {
			for (uint32_t j = 0; j < listRouting->GetNRoutingProtocols(); j++) {
				int16_t priority;
				Ptr<Ipv4RoutingProtocol> proto = listRouting->GetRoutingProtocol(j, priority);
				Ptr<Ipv4StaticRouting> sr = DynamicCast<Ipv4StaticRouting>(proto);
				if (sr) {
					uint32_t routeCount = sr->GetNRoutes();
					std::cout << "  Node " << i << " [Type: " << node->GetNodeType() << "] static routes: " << routeCount << std::endl;
				}
			}
		} else if (staticRouting) {
			uint32_t routeCount = staticRouting->GetNRoutes();
			std::cout << "  Node " << i << " [Type: " << node->GetNodeType() << "] direct static routes: " << routeCount << std::endl;
		} else {
			std::cout << "  DEBUG: Node " << i << " routing type=" << routingProto->GetInstanceTypeId().GetName() << std::endl;
		}
		
		// 额外检查: RdmaHw的路由表 (m_rtTable)
		if (node->GetNodeType() == 0) { // 主机
			Ptr<RdmaDriver> rdmaDriver = node->GetObject<RdmaDriver>();
			if (rdmaDriver && rdmaDriver->m_rdma) {
				uint32_t rdmaRouteCount = rdmaDriver->m_rdma->m_rtTable.size();
				if (rdmaRouteCount == 0) {
					std::cout << "  WARNING: Host " << i << " RdmaHw m_rtTable is EMPTY!" << std::endl;
					total_empty_nodes++;
				} else {
					std::cout << "  OK: Host " << i << " (IP=" << serverAddress[i] << ") RdmaHw m_rtTable size: " << rdmaRouteCount << std::endl;
				}
			}
		} else if (node->GetNodeType() == 1) { // 交换机
			Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(node);
			if (sw) {
				const auto& rtTable = sw->GetRouteTable();
				std::cout << "  Switch " << i << " (RealIP=" << sw->GetSwitchRealIp() << ") m_rtTable size: " << rtTable.size() << std::endl;

				// 输出端口-连接映射（Ipv4接口索引 -> 对端节点信息）
				Ptr<Ipv4> swIpv4 = node->GetObject<Ipv4>();
				std::cout << "    [Port Map] Ipv4Ifs=" << swIpv4->GetNInterfaces() << std::endl;
				for (uint32_t ifIdx = 1; ifIdx < swIpv4->GetNInterfaces(); ifIdx++) {  // skip ifIdx=0 (loopback)
					Ptr<NetDevice> dev = swIpv4->GetNetDevice(ifIdx);
					if (!dev) continue;
					// 找对端节点
					Ptr<Channel> ch = dev->GetChannel();
					std::string peerInfo = "?";
					if (ch && ch->GetNDevices() == 2) {
						for (std::size_t di = 0; di < ch->GetNDevices(); di++) {
							Ptr<NetDevice> otherDev = ch->GetDevice(di);
							if (otherDev != dev) {
								Ptr<Node> peerNode = otherDev->GetNode();
								if (peerNode->GetNodeType() == 0) {
									// 找到对端主机IP
									uint32_t peerId = peerNode->GetId();
									if (peerId < serverAddress.size()) {
										std::ostringstream ipss;
										ipss << serverAddress[peerId];
										peerInfo = "Host" + std::to_string(peerId) + "(" + ipss.str() + ")";
									} else
										peerInfo = "Host" + std::to_string(peerId);
								} else {
									peerInfo = "Switch" + std::to_string(peerNode->GetId());
								}
							}
						}
					}
					// 获取链路带宽
					Ptr<QbbNetDevice> qbbDev = DynamicCast<QbbNetDevice>(dev);
					std::string bwStr = "?";
					if (qbbDev) {
						uint64_t bps = qbbDev->GetDataRate().GetBitRate();
						bwStr = std::to_string(bps / 1000000000) + "Gbps";
					}
					std::cout << "    If[" << ifIdx << "] -> " << peerInfo << " (" << bwStr << ")" << std::endl;
				}

				if (rtTable.empty()) {
					std::cout << "  WARNING: Switch " << i << " has EMPTY routing table!" << std::endl;
					total_empty_nodes++;
				} else {
					for (auto& kv : rtTable) {
						Ipv4Address dstIp(kv.first);
						std::cout << "    dst=" << dstIp << " -> ports=[";
						for (size_t pi = 0; pi < kv.second.size(); pi++) {
							if (pi > 0) std::cout << ",";
							std::cout << kv.second[pi];
						}
						std::cout << "]" << std::endl;
					}
				}
			}
		}
	}
	std::cout << "=======================================================\n" << std::endl;
	if (total_empty_nodes > 0) {
		NS_FATAL_ERROR("Simulation stopped: " << total_empty_nodes << " nodes have empty routing tables.");
	}

	//
	// get BDP and delay
	//
	maxRtt = maxBdp = 0;
	uint64_t minRtt = 1e9;
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType() != 0)
			continue;
		for (uint32_t j = 0; j < node_num; j++) {
			if (n.Get(j)->GetNodeType() != 0)
				continue;
			if (i == j)
				continue;
			uint64_t delay = pairDelay[n.Get(i)][n.Get(j)];
			uint64_t txDelay = pairTxDelay[n.Get(i)][n.Get(j)];
			uint64_t rtt = delay * 2 + txDelay;
			uint64_t bw = pairBw[i][j];
			uint64_t bdp = rtt * bw / 1000000000 / 8;
			pairBdp[n.Get(i)][n.Get(j)] = bdp;
			pairRtt[i][j] = rtt;
			if (bdp > maxBdp)
				maxBdp = bdp;
			if (rtt > maxRtt)
				maxRtt = rtt;
			if (rtt < minRtt)
				minRtt = rtt;
		}
	}
	printf("maxRtt=%lu maxBdp=%lu minRtt=%lu\n", maxRtt, maxBdp, uint64_t(minRtt));

	//
	// setup switch CC
	//
	for (uint32_t i = 0; i < node_num; i++) {
		if (n.Get(i)->GetNodeType()) { // switch
			Ptr<SwitchNode> sw = DynamicCast<SwitchNode>(n.Get(i));

			// Bifrost (ccMode=12): 在所有DCI交换机上启用周期PFC机制，其他交换机使用ccMode=1 (DCQCN)



			if (cc_mode == 12) {
				if (dci_switches.count(i) > 0) {
					// 从配置文件读取 Bifrost 参数并初始化
					sw->ReadBifrostConfigFromFile(confFile);
					sw->InitializeBifrost(i);
				} else {
					sw->SetAttribute("CcMode", UintegerValue(1)); // 非 DCI 交换机回退到 DCQCN
				}
			} else {
				// ccMode != 12 时，设置所有交换机为cc_mode
				sw->SetAttribute("CcMode", UintegerValue(cc_mode));
			} 
			
			sw->SetAttribute("MaxRtt", UintegerValue(maxRtt));
			sw->SetAttribute("PowerEnabled", BooleanValue(wien));

			// 当CC模式为13 (FRP) 或 14 (ROCC) 时，启动周期性反馈机制
			if (cc_mode == 13 || cc_mode == 14) {
				std::cout << "[FRP SETUP] Enabling FRP/ROCC feedback on switch " << i << " (cc_mode=" << cc_mode << ")" << std::endl;
				sw->SetAttribute("SwitchFeedbackEnabled", BooleanValue(true));  // 启用反馈机制
				Time feedbackInterval = MicroSeconds(40);  // FRP反馈周期: 40μs
				sw->StartPeriodicFeedbackMechanism(feedbackInterval);
				std::cout << "[FRP SETUP] FRP feedback enabled with interval=" << feedbackInterval.As(Time::US) << std::endl;
			}
		}
	}

	Ipv4GlobalRoutingHelper::PopulateRoutingTables();
	// Ptr<OutputStreamWrapper> routingStream = Create<OutputStreamWrapper> (&std::cout);
	// globalRoutingHelper.PrintRoutingTableAt (Seconds (0.0), n.Get(0), routingStream);

	NS_LOG_INFO("Create Applications.");

	Time interPacketInterval = Seconds(0.0000005 / 2);


	DiscoverWorkloadTopology(SERVER_COUNT, LEAF_COUNT, LEAF_SERVER_CAPACITY, SPINE_LEAF_CAPACITY, SPINE_COUNT);
	InitializeWorkloadPorts();

	// 生成背景流（CDF）
	NS_LOG_INFO("Initialize CDF table");
	struct cdf_table* cdfTable = new cdf_table();
	init_cdf(cdfTable);
	load_cdf(cdfTable, cdfFileName.c_str());
	std::cout << "[WORKLOAD] CDF " << cdfFileName
	          << " entries " << cdfTable->num_entry
	          << " avgFlowSize " << avg_cdf(cdfTable) << std::endl;

	// 背景流已删除，只保留查询流
	long flowCount = 0;
	long totalFlowSize = 0;
	std::cout << "[BACKGROUND] Background workload disabled (only query flows enabled)" << std::endl;

	// 生成查询流（文件）
	long queryFlowCount = 0;
	long totalQueryFlowSize = 0;
	InstallQueryFlowFile(queryFlowFile, START_TIME, queryFlowCount, totalQueryFlowSize);
	std::cout << "[WORKLOAD] Total flows: " << flows.size() << std::endl;

	topof.close();
	tracef.close();
	
	double delay = 1.5 * minRtt * 1e-9; // 10 micro seconds
	//Simulator::Schedule(Seconds(delay), PrintResults, switchDown, 1, delay);
	Simulator::Schedule(Seconds(delay), PrintResultsFlow, delay);  // 初始触发 per-flow 接收侧吞吐采样

	// AsciiTraceHelper ascii;
	//     qbb.EnableAsciiAll (ascii.CreateFileStream ("eval.tr"));
	
	// 自动生成瓶颈队列监控文件名（如果未手动指定）
	if (bottleneck_mon_file.empty()) {
		// 提取CDF名称（如 websearch, alistorage）
		std::string cdf_base = cdfFileName;
		size_t slash_pos = cdf_base.rfind('/');
		if (slash_pos != std::string::npos) cdf_base = cdf_base.substr(slash_pos + 1);
		size_t dot_pos = cdf_base.rfind('.');
		if (dot_pos != std::string::npos) cdf_base = cdf_base.substr(0, dot_pos);
		// 转小写
		std::transform(cdf_base.begin(), cdf_base.end(), cdf_base.begin(), ::tolower);
		
		// 算法名称（ATC特殊处理：algorithm=1 + gatewayFlag=2）
		std::string algo_name;
		if (algorithm == 13) algo_name = "frp";
		else if (algorithm == 14) algo_name = "rocc";
		else if (algorithm == 1 && gatewayFlag == 2) algo_name = "atc";
		else if (algorithm == 1) algo_name = "dcqcn";
		else if (algorithm == 3) algo_name = "hpcc";
		else if (algorithm == 4) algo_name = "powertcp";
		else if (algorithm == 7) algo_name = "timely";
		else if (algorithm == 15) algo_name = "atc";
		else algo_name = "unknown";
		
		// 构建文件名: bottleneck_{algo}_{cdf}_load{load}.txt
		char load_str[32];
		snprintf(load_str, sizeof(load_str), "%.2f", load);
		bottleneck_mon_file = "/home/gj/ns3/results/workload/bottleneck_queue_length/bottleneck_" 
		                      + algo_name + "_" + cdf_base + "_load" + load_str + ".txt";
	}
	
	// 启动瓶颈队列监控
	if (!bottleneck_mon_file.empty()) {
		// 确保输出目录存在
		system("mkdir -p /home/gj/ns3/results/workload/bottleneck_queue_length");
		
		FILE* bn_fp = fopen(bottleneck_mon_file.c_str(), "w");
		if (bn_fp) {
			std::cout << "[BOTTLENECK MON] Enabled, interval=" << bottleneck_mon_interval 
			          << "ns, output=" << bottleneck_mon_file << std::endl;
			Simulator::Schedule(NanoSeconds(bottleneck_mon_interval), 
			                   &monitor_bottleneck_queue, bn_fp, &n);
		} else {
			std::cerr << "Warning: Cannot open bottleneck monitor file: " << bottleneck_mon_file << std::endl;
		}
	}
	
	std::cout << "Running Simulation.\n";
	NS_LOG_INFO("Run Simulation.");
	Simulator::Stop(Seconds(simulator_stop_time));
	Simulator::Run();
	Simulator::Destroy();
	NS_LOG_INFO("Done.");
	endt = clock();
	std::cout << (double)(endt - begint) / CLOCKS_PER_SEC << "\n";
	
	// Close query flow FCT file
	if (g_queryFlowFctFile != nullptr) {
		fclose(g_queryFlowFctFile);
		std::cout << "[QUERY FLOW] Closed query flow FCT file: " << queryFlowFctFile << std::endl;
	}
}
