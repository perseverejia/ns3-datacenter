/*
 * Workload Generator Header for CrossDC-Algos
 * 
 * 独立的流量生成模块头文件
 */

#ifndef WORKLOAD_GENERATOR_H
#define WORKLOAD_GENERATOR_H

#include <vector>
#include <map>
#include <string>
#include <cstdint>
#include "ns3/node.h"
#include "ns3/ipv4-address.h"

// ==================== 数据结构定义 ====================

struct Interface {
    uint32_t idx;
    bool up;
    uint64_t delay;
    uint64_t bw;
    
    Interface() : idx(0), up(false) {}
};

struct FlowInput {
    uint64_t src, dst, pg, maxPacketCount, port, dport;
    double start_time;
    uint32_t idx;
    uint64_t last_recv_bytes;
    bool is_query_flow;
};

struct WorkloadTopology {
    std::vector<uint32_t> allHostIds;
    std::vector<uint32_t> hostFacingLeafIds;
    std::map<uint32_t, std::vector<uint32_t>> leafToHostIds;
    std::map<uint32_t, uint64_t> leafDownlinkCapacityBps;
    std::map<uint32_t, uint64_t> leafUplinkCapacityBps;
};

// ==================== 函数声明 ====================

/**
 * 发现工作负载拓扑（结果存储在全局变量中）
 */
void DiscoverWorkloadTopology(uint32_t serverCountLimit = 0,
                              uint32_t leafCountLimit = 0,
                              uint64_t leafServerCapacityFallback = 0,
                              uint64_t spineLeafCapacityFallback = 0,
                              uint32_t spineCountFallback = 0);

/**
 * 初始化工作负载端口（无参数版本）
 */
void InitializeWorkloadPorts();

/**
 * 添加工作负载流
 */
void AddWorkloadFlow(uint32_t src,
                     uint32_t dst,
                     uint32_t pg,
                     uint64_t flowSize,
                     double startTime,
                     long &flowCount,
                     long &totalFlowSize,
                     uint16_t forcedDport = 0,
                     bool isQueryFlow = false);

/**
 * 安装背景工作负载（PER_DC 模式）
 * @param load 负载率（0.0-1.0）
 * @param cdfTable CDF 表
 * @param START_TIME 开始时间
 * @param FLOW_LAUNCH_END_TIME 流启动结束时间
 * @param flowCount 流计数器（引用）
 * @param totalFlowSize 总流量（引用）
 * @param refBandwidth 参考带宽（bps），默认 10Gbps
 */
void InstallBackgroundWorkload(double load,
                               void *cdfTable,
                               double START_TIME,
                               double FLOW_LAUNCH_END_TIME,
                               long &flowCount,
                               long &totalFlowSize,
                               uint64_t refBandwidth = 10000000000ULL);

/**
 * 从文件加载查询流
 */
void InstallQueryFlowFile(const std::string& queryFlowFile,
                          double startTime,
                          long &queryFlowCount,
                          long &totalQueryFlowSize);

// ==================== 工具函数 ====================

double poission_gen_interval(double requestRate);
uint32_t rand_range_uint(uint32_t min, uint32_t max);

#endif // WORKLOAD_GENERATOR_H
