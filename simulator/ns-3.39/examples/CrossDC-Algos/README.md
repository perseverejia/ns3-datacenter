# CrossDC-Algos 独立实验目录

独立的跨数据中心拥塞控制算法对比实验目录，不依赖 PowerTCP 目录。

## 目录结构

```
CrossDC-Algos/
├── config/
│   ├── topology/           # 拓扑文件
│   │   ├── topology.txt
│   │   ├── topology-testbed.txt
│   │   └── ...
│   ├── query-flow/         # query-flow 文件
│   │   ├── query-flow.txt
│   │   └── ...
│   └── workload/           # 配置文件
│       ├── config-workload.txt
│       ├── config-testbed.txt
│       └── ...
├── results/                # 仿真结果（运行时生成）
│   ├── dump/               # 日志文件
│   ├── fct/                # FCT 数据
│   └── pfc/                # PFC 数据
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
| 8 | DCTCP | DCTCP |
| 12 | Bifrost | DCI 交换机周期 PFC |
| 13 | FRP | 交换机周期反馈 |
| 14 | ROCC | FRP 变体 |
| 15 | ATC | 基于 DCQCN 的 gateway 模式 |
| 16 | DCIEXT | 接收方 DCI 交换机周期探测可用带宽（RTT 驱动，0xFA 反馈限速） |
| 17 | EPFC | 接收方 excess 带宽反馈（CCF 设速 + 流级门控） |
| 18 | GRC | DCI 网侧速率控制，端侧复用现有 DCQCN |
| 19 | THEMIS | ESW 上的 PNP/TRP，端侧复用现有 DCQCN |

## 使用方法

### 1. 配置文件（已完成）

配置文件已复制到对应子目录，路径已修改为相对路径：

```
config/
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
TOPOLOGY_FILE examples/CrossDC-Algos/config/topology/topology-multiDC.txt
QUERY_FLOW_FILE examples/CrossDC-Algos/config/query-flow/query-flow-multiDC.txt
CDF_FILE_NAME examples/CrossDC-Algos/config/workload/Alistorage.txt
```

主程序中的默认路径也已同步修改。

### 2. 运行仿真

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

# 按原来的 Bash 循环运行默认算法列表
bash examples/CrossDC-Algos/scripts/run_all_algos.sh

# 或者运行指定的算法
bash examples/CrossDC-Algos/scripts/run_all_algos.sh "1:DCQCN" "13:FRP"

# 使用场景专用配置；单算法脚本原有 --config 用法保持不变
python3 examples/CrossDC-Algos/scripts/run_single_workload_algo.py 16 DCIEXT \
  --config examples/CrossDC-Algos/config/workload/config-motivation1.txt
```

### 3. 查看结果

- **日志文件**：`results/dump/algo_cc{ccMode}_load{load}.log`
- **数据文件**：`results/fct/`, `results/pfc/`
- **瓶颈队列文件**：默认位于 `results/bottleneck_queue_length/`，可通过已有的 `BOTTLENECK_MON_FILE` 配置指定。
- **图片文件**：`plots/workload_{algo}_cc{ccMode}_load{load}.png`

每次实验从空的结果目录开始，只使用一份配置。批量脚本沿用默认的 `config-workload.txt`；
单算法脚本可通过原有的 `--config` 参数选择专用配置。
输出沿用原来的命名和目录，不添加配置名后缀，不混用其他配置或旧结果。
辅助监控文件的路径由所选配置中的配置项控制。

## 输出说明

### 主图（吞吐量 + 队列长度）

包含两个子图：

- **上图**：接收端吞吐量（RX Throughput）- 每个 query flow 的接收速率
- **下图**：Top 3 拥塞点的队列长度 - 自动检测的平均队列长度最大的端口

### TX Rate 图

独立的发送端速率图，显示每个 query flow 的发送速率变化，保存在 `plots/workload_txrate_{algo}_cc{ccMode}_load{load}.png`。

### 统计数据

脚本会输出：

- 流数量统计（总流数、query 流数、背景流数）
- 拥塞点检测（交换机、端口、平均/最大队列长度）
- FCT 统计（平均、P99、最大/最小）
- PFC 统计（Pause/Resume 次数）

## 与 PowerTCP 的区别

✅ **完全独立**：不依赖 PowerTCP 目录，可独立运行  
✅ **结构清晰**：配置、结果、图片分离  
✅ **易于扩展**：新增算法需实现代码并配置运行入口  
✅ **路径统一**：配置中的相对路径以 ns-3.39 根目录为基准

## 注意事项

1. **配置文件**直接使用本目录的 `config/` 文件
2. **配置中的相对路径**以 `ns-3.39` 为工作目录解析
3. **编译**仍然在 ns-3.39 根目录进行（`./ns3 build`）
4. **主程序** `crossDC-evaluation-workload.cc` 由现有运行脚本编译、执行并分析结果

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
│   ├── topology/               ✅ 拓扑文件
│   ├── query-flow/             ✅ query-flow 文件
│   └── workload/               ✅ 配置文件
├── results/                    ✅ 仿真结果
│   ├── dump/
│   ├── fct/
│   └── pfc/
├── plots/                      ✅ 图片输出
├── scripts/
│   ├── run_single_workload_algo.py  ✅ 单算法运行脚本
│   └── run_all_algos.sh        ✅ 批量运行脚本
└── README.md                   ✅ 本文件
```

## 当前测试场景

当前比较 DCQCN（模式 1）、DCI-Ext（模式 16）、GRC（模式 18）、ATC（模式 15）
和 THEMIS（模式 19），四个场景如下。
同一场景使用相同的拓扑、查询流、端侧参数和随机种子（`--randomSeed=7`）。
当前背景负载为 0，以下列出的查询流就是实际运行的全部流；每条流均为
300,000,000 B（300 MB，约 286.10 MiB），优先级组 `pg=3`。

查询流文件首行是流数量，后续各行格式为：

```text
src dst pg dport flowSize(B) startTime(s)
```

文件最后一列是相对启动偏移，实际仿真开始时间由
`实际开始时间 = 配置中的 START_TIME + 文件中的 startTime` 计算。
下表同时列出文件偏移和实际开始时间，单位为 ms；流大小单位为 B。
DC 编号采用程序推导的编号，从 DC0 开始。

### 1. multiDC：两个源 DC 与目的 DC 内流共同发送到同一主机

配置：[config-workload.txt](config/workload/config-workload.txt)；
拓扑：[topology-multiDC.txt](config/topology/topology-multiDC.txt)；
查询流：[query-flow-multiDC.txt](config/query-flow/query-flow-multiDC.txt)。

DC0 包含 H0–H3，DC1 包含 H4–H7，DC2 包含 H8–H11。
三个 DCI 分别为 SW21、SW22、SW23，跨域链路为 SW21↔SW22 和 SW22↔SW23，
均为 100 Gbps、单向传播时延 5 ms。主机接入链路为 40 Gbps。
两条跨域流分别从 DC0、DC2 发往 DC1，与 DC1 内的两条流共同竞争 H4 的接收带宽。
配置的 `START_TIME=0`，两条跨域流在 0 ms 启动，两条域内流在 5.01 ms 启动。

| 流 | 源主机（所属 DC） | 目的主机（所属 DC） | 类型 | 大小（B） | 文件偏移（ms） | 实际开始（ms） |
| --- | --- | --- | --- | ---: | ---: | ---: |
| 1 | H0（DC0） | H4（DC1） | 跨域 | 300000000 | 0 | 0 |
| 2 | H8（DC2） | H4（DC1） | 跨域 | 300000000 | 0 | 0 |
| 3 | H6（DC1） | H4（DC1） | 域内 | 300000000 | 5.01 | 5.01 |
| 4 | H7（DC1） | H4（DC1） | 域内 | 300000000 | 5.01 | 5.01 |

### 2. motivation1：一条跨域流与两条域内流

配置：[config-motivation1.txt](config/workload/config-motivation1.txt)；
拓扑：[topology-motivation.txt](config/topology/topology-motivation.txt)；
查询流：[query-flow-motivation1.txt](config/query-flow/query-flow-motivation1.txt)。
通过原有 `--config` 参数选择专用配置，拓扑、查询流和部署节点一起切换。

DC0 包含 H0–H3，DC1 包含 H4–H7；DCI 为 SW14、SW15，
跨域链路为 100 Gbps、单向传播时延 5 ms，主机接入链路为 40 Gbps。
H0→H6、H5→H6 与 H4→H6 均发往 H6，共享 SW11→H6 的 40 Gbps 接收链路。
配置的 `START_TIME=0`，跨域流在 0 ms 启动，两条域内流在 5.01 ms 启动。

| 流 | 源主机（所属 DC） | 目的主机（所属 DC） | 类型 | 大小（B） | 文件偏移（ms） | 实际开始（ms） |
| --- | --- | --- | --- | ---: | ---: | ---: |
| 1 | H0（DC0） | H6（DC1） | 跨域 | 300000000 | 0 | 0 |
| 2 | H5（DC1） | H6（DC1） | 域内 | 300000000 | 5.01 | 5.01 |
| 3 | H4（DC1） | H6（DC1） | 域内 | 300000000 | 5.01 | 5.01 |

### 3. motivation2：两条跨域流与一条域内流

配置：[config-motivation2.txt](config/workload/config-motivation2.txt)；
拓扑：[topology-motivation.txt](config/topology/topology-motivation.txt)；
查询流：[query-flow-motivation2.txt](config/query-flow/query-flow-motivation2.txt)。
同样通过 `--config` 选择专用配置。
DC 划分、带宽及传播时延与 motivation1 相同。

H0→H4、H1→H4 与 H6→H4 均发往 H4，共享 SW10→H4 的 40 Gbps 接收链路。
配置的 `START_TIME=0`，两条跨域流在 0 ms 启动，域内流在 5.01 ms 启动。

| 流 | 源主机（所属 DC） | 目的主机（所属 DC） | 类型 | 大小（B） | 文件偏移（ms） | 实际开始（ms） |
| --- | --- | --- | --- | ---: | ---: | ---: |
| 1 | H0（DC0） | H4（DC1） | 跨域 | 300000000 | 0 | 0 |
| 2 | H1（DC0） | H4（DC1） | 跨域 | 300000000 | 0 | 0 |
| 3 | H6（DC1） | H4（DC1） | 域内 | 300000000 | 5.01 | 5.01 |

### 4. testbed：跨域流先启动，域内流随后竞争同一接收端

配置：[config-testbed.txt](config/workload/config-testbed.txt)；
拓扑：[topology-testbed.txt](config/topology/topology-testbed.txt)；
查询流：[query-flow-testbed.txt](config/query-flow/query-flow-testbed.txt)。

DC0 包含 H0、H1，DC1 包含 H2、H3；DCI 为 SW6、SW7。
服务器接入链路 H0/H1↔SW4、H2/H3↔SW5 均为 **40 Gbps**；
SW4↔SW6、SW5↔SW7 及 SW6↔SW7 均为 **100 Gbps**。
SW6↔SW7 的单向传播时延为 5 ms，其余链路为 1.5 μs。
两条流都发往 H2，共享 SW5→H2 的 40 Gbps 接收链路。

配置的 `START_TIME=0.005 s`。因此文件中的 `0.000 s`、`0.005010 s`
分别对应仿真时间 **5 ms、10.01 ms**。

| 流 | 源主机（所属 DC） | 目的主机（所属 DC） | 类型 | 大小（B） | 文件偏移（ms） | 实际开始（ms） |
| --- | --- | --- | --- | ---: | ---: | ---: |
| 1 | H0（DC0） | H2（DC1） | 跨域 | 300000000 | 0 | 5 |
| 2 | H3（DC1） | H2（DC1） | 域内 | 300000000 | 5.01 | 10.01 |

### 运行与结果口径

四个场景的仿真上限均为 2 s，均开启查询流完成后自动停止。
FCT 从各条流的实际开始时间计起，域内和跨域流分别统计。当前 FCT 文件只记录已完成流，
分析时需单独记录未完成流，不能只用已完成流计算均值来代表全部流。

DCI-Ext 和 ATC 的部署节点由所选配置指定，motivation 专用配置已设置下表节点。
仅覆盖拓扑和查询流不会自动修改部署节点，应优先使用完整场景配置。
新流观察期为 `DCIEXT_OBSERVE_MB 1`，收发双方共用周期 `DCIEXT_T_US 400`：

| 场景 | 发送侧 DCI | 接收侧 DCI |
| --- | --- | --- |
| multiDC | SW21、SW23 | SW22 |
| motivation1 | SW14 | SW15 |
| motivation2 | SW14 | SW15 |
| testbed | SW6 | SW7 |
