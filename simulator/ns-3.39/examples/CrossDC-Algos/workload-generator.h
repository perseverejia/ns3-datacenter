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
    bool is_file_flow;
};

struct FileFlowParameters {
    uint32_t count = 0;            // Exact total number of file flows.
    uint64_t sizeBytes = 300000000; // Bytes per flow, not per incast group.
    uint32_t incast = 4;           // Distinct senders per group; final group may be smaller.
    double startTime = 0;
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
 * 初始化独立 ns-3 随机流：背景采样/到达 1000/1001，文件流端点/时间 1002/1003。
 * randomSeed 设置 ns-3 seed，RngRun 选择重复实验。
 */
void InitializeWorkloadRandomVariables(uint32_t randomSeed);

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
                     bool isFileFlow = false);

/**
 * 按每个 ToR 的实际总上行容量安装 DC 内背景流。
 * load 表示 ToR 上行 DATA + ACK 负载率（0.0-1.0），使用旧 PowerTCP 的
 * 包开销估计和跨 ToR 概率校正。各 DC 独立采样，不生成镜像或跨 DC 背景流。
 * @param load ToR 上行负载率（限制在 0.0-1.0）
 * @param cdfTable CDF 表
 * @param START_TIME 开始时间
 * @param FLOW_LAUNCH_END_TIME 流启动结束时间
 * @param flowCount 流计数器（引用）
 * @param totalFlowSize 总流量（引用）
 */
void InstallBackgroundWorkload(double load,
                               void *cdfTable,
                               double START_TIME,
                               double FLOW_LAUNCH_END_TIME,
                               long &flowCount,
                               long &totalFlowSize);

/**
 * 直接生成 A(DC0)/C(DC2) -> B(DC1) 文件流，按 incast 分组随机启动。
 */
void InstallCrossDcFileFlows(const FileFlowParameters& parameters,
                             double launchEndTime,
                             long& fileFlowCount,
                             long& totalFileFlowSize);

// QUERY_FLOW_FILE / --queryFlowFile select this source instead of generated
// foreground flows. LOAD independently controls the CDF background.
void InstallFixedFlowFile(const std::string& path,
                          double startOffset,
                          double stopTime,
                          long& flowCount,
                          long& totalFlowSize);

const FlowInput* FindFileFlow(uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport);
const std::vector<uint32_t>& GetFileFlowIndices();

// ==================== 工具函数 ====================

double poission_gen_interval(double requestRate);
uint32_t rand_range_uint(uint32_t min, uint32_t max);

#endif // WORKLOAD_GENERATOR_H
