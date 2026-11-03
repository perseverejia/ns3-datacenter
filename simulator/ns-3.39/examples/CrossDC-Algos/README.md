# CrossDC-Algos 独立实验目录

独立的跨数据中心拥塞控制算法对比实验目录，不依赖 PowerTCP 目录。

## 目录结构

```
CrossDC-Algos/
├── config/
│   ├── flows/              # 流文件
│   │   ├── flow-background.txt
│   │   ├── flow-websearch.txt
│   │   └── ...
│   ├── topology/           # 拓扑文件
│   │   ├── topology.txt
│   │   ├── topology-testbed.txt
│   │   └── ...
│   ├── query-flow/         # query-flow 文件
│   │   ├── query-flow.txt
│   │   └── ...
│   └── workload/           # 配置文件
│       ├── config-workload.txt
│       ├── config-bifrost.txt
│       └── ...
├── results/                # 仿真结果（运行时生成）
│   ├── dump/               # 日志文件
│   ├── fct/                # FCT 数据
│   ├── pfc/                # PFC 数据
│   └── txrate/             # TX Rate 数据
├── plots/                  # 画图结果（运行时生成）
├── scripts/                # 运行脚本
│   └── run_single_workload_algo.py
└── README.md               # 本文件
```

## 支持的算法

| ccMode | 算法名称 | 说明 |
|--------|---------|------|
| 1 | DCQCN | 标准 DCQCN |
| 3 | HPCC | 基于 INT 遥测 |
| 7 | Timely | 基于时间戳 |
| 10 | DCTCP | DCTCP |
| 12 | Bifrost | DCI 交换机周期 PFC |
| 13 | FRP | 交换机周期反馈 |
| 14 | ROCC | FRP 变体 |
| 15 | ATC | 基于 DCQCN 的 gateway 模式 |
| 16 | DCIEXT | 接收方 DCI 交换机周期探测可用带宽（RTT 驱动，0xFA 反馈限速） |
| 17 | EPFC | 接收方 excess 带宽反馈（CCF 设速 + 端口门控） |

## 使用方法

### 1. 配置文件（已完成）

配置文件已复制到对应子目录，路径已修改为相对路径：

```
config/
├── flows/
│   ├── flow-background.txt
│   └── flow.txt
├── topology/
│   ├── topology.txt
│   ├── topology-testbed.txt
│   └── topology-256.txt
├── query-flow/
│   ├── query-flow.txt
│   ├── query-flow-testbed.txt
│   └── query-flow-websearch.txt
└── workload/
    ├── config-workload.txt  (路径已修改)
    └── Alistorage.txt
```

所有配置文件中的路径已修改为相对路径格式：
```txt
TOPOLOGY_FILE examples/CrossDC-Algos/config/topology/topology.txt
FLOW_FILE examples/CrossDC-Algos/config/flows/flow.txt
QUERY_FLOW_FILE examples/CrossDC-Algos/config/query-flow/query-flow.txt
CDF_FILE_NAME examples/CrossDC-Algos/config/workload/Alistorage.txt
BACKGROUND_FLOW_FILE examples/CrossDC-Algos/config/flows/flow-background.txt
```

主程序中的默认路径也已同步修改。

### 3. 运行仿真

#### 运行单个算法

```bash
# 进入 ns-3.39 目录
cd simulator/ns-3.39

# 运行单个算法
python3 examples/CrossDC-Algos/scripts/run_single_workload_algo.py 1 DCQCN
python3 examples/CrossDC-Algos/scripts/run_single_workload_algo.py 13 FRP
python3 examples/CrossDC-Algos/scripts/run_single_workload_algo.py 12 Bifrost
```

#### 批量运行所有算法

```bash
# 进入 ns-3.39 目录
cd simulator/ns-3.39

# 运行所有算法（DCQCN, HPCC, Timely, FRP, ROCC, Bifrost）
bash examples/CrossDC-Algos/scripts/run_all_algos.sh

# 或者运行指定的算法
bash examples/CrossDC-Algos/scripts/run_all_algos.sh "1:DCQCN" "13:FRP"
```

### 4. 查看结果

- **日志文件**：`results/dump/algo_cc{ccMode}_load{load}.log`
- **数据文件**：`results/fct/`, `results/pfc/`, `results/txrate/`
- **图片文件**：`plots/workload_{algo}_cc{ccMode}_load{load}.png`

## 输出说明

### 主图（吞吐量 + 队列长度）

包含两个子图：
- **上图**：接收端吞吐量（RX Throughput）- 每个 query flow 的接收速率
- **下图**：Top 5 拥塞点的队列长度 - 自动检测的平均队列长度最大的端口

### TX Rate 图

独立的发送端速率图，显示每个 query flow 的发送速率变化。

### 统计数据

脚本会输出：
- 流数量统计（总流数、query 流数、背景流数）
- 拥塞点检测（交换机、端口、平均/最大队列长度）
- FCT 统计（平均、P99、最大/最小）
- PFC 统计（Pause/Resume 次数）

## 与 PowerTCP 的区别

✅ **完全独立**：不依赖 PowerTCP 目录，可独立运行  
✅ **结构清晰**：配置、结果、图片分离  
✅ **易于扩展**：新增算法只需添加配置文件  
✅ **路径统一**：所有路径基于 CrossDC-Algos 目录  

## 注意事项

1. **首次使用**需要从 PowerTCP 复制配置文件
2. **配置文件中的路径**需要修改为 CrossDC-Algos 的路径
3. **编译**仍然在 ns-3.39 根目录进行（`./ns3 build`）
4. **主程序** `crossDC-evaluation-workload.cc` 需要复制到 CrossDC-Algos 并修改默认路径（待完成）

## 已完成

- [x] 创建目录结构
- [x] 复制并修改画图脚本路径
- [x] 复制配置文件
- [x] 修改配置文件中的路径为相对路径
- [x] 修改主程序中的默认路径
- [x] 复制主程序到 CrossDC-Algos
- [x] 复制依赖文件 (cdf.c, cdf.h)
- [x] 创建独立的 CMakeLists.txt
- [x] 创建批量运行脚本 run_all_algos.sh

## 目录结构

```
CrossDC-Algos/
├── CMakeLists.txt              ✅ 独立编译配置
├── crossDC-evaluation-workload.cc  ✅ 主程序
├── cdf.c                       ✅ 依赖库
├── cdf.h                       ✅ 依赖库头文件
├── config/
│   ├── flows/                  ✅ 流文件
│   ├── topology/               ✅ 拓扑文件
│   ├── query-flow/             ✅ query-flow 文件
│   └── workload/               ✅ 配置文件
├── results/                    ✅ 仿真结果
│   ├── dump/
│   ├── fct/
│   ├── pfc/
│   └── txrate/
├── plots/                      ✅ 图片输出
├── scripts/
│   ├── run_single_workload_algo.py  ✅ 单算法运行脚本
│   └── run_all_algos.sh        ✅ 批量运行脚本
└── README.md                   ✅ 本文件
```
