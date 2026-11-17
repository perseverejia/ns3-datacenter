# CrossDC-Algos 独立实验目录

独立的跨数据中心拥塞控制算法对比实验目录，不依赖 PowerTCP 目录。

## 目录结构

```
CrossDC-Algos/
├── config/
│   ├── topology/           # 拓扑文件
│   │   ├── topology.txt
│   │   ├── topology-micro.txt # 四个 micro 场景共用
│   │   └── ...
│   ├── query-flow/         # query-flow 文件
│   │   ├── query-flow-2-2.txt
│   │   ├── query-flow-2-1.txt
│   │   ├── query-flow-1-2.txt
│   │   └── query-flow-1-1.txt
│   └── workload/           # 配置文件
│       ├── config-micro.txt
│       ├── Alistorage.txt
│       └── websearch.txt
├── results/                # 运行时临时数据，当前已清空
├── plots/                  # 最终四场景、五算法对比图
│   ├── tx-rate.{png,pdf}
│   ├── queue-length.{png,pdf}
│   └── fct.{png,pdf}
├── scripts/                # 运行脚本
│   ├── run_single_workload_algo.py
│   └── run_all_algos.sh
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

### 1. 公共配置

四个查询流场景共用一份拓扑和一份配置，仅查询流文件不同。
查询流文件统一按 `query-flow-{短距流条数}-{长距流条数}.txt` 命名，
其中短距流指域内流，长距流指跨 DC 流：

```
config/
├── topology/
│   └── topology-micro.txt
├── query-flow/
│   ├── query-flow-2-2.txt
│   ├── query-flow-2-1.txt
│   ├── query-flow-1-2.txt
│   └── query-flow-1-1.txt
└── workload/
    ├── config-micro.txt
    ├── Alistorage.txt
    └── websearch.txt
```

所有配置文件中的路径已修改为相对路径格式：

```txt
TOPOLOGY_FILE examples/CrossDC-Algos/config/topology/topology-micro.txt
QUERY_FLOW_FILE examples/CrossDC-Algos/config/query-flow/query-flow-2-2.txt
CDF_FILE_NAME examples/CrossDC-Algos/config/workload/Alistorage.txt
```

主程序和运行脚本都默认使用 `config-micro.txt`，默认查询流为 `query-flow-2-2.txt`。
micro 与 workload 的参数一致，保留现有 micro 拓扑和默认输入；各场景通过覆盖查询流路径切换。

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

# 单算法切换场景只需指定查询流
python3 examples/CrossDC-Algos/scripts/run_single_workload_algo.py 16 DCIEXT \
  --query-flow examples/CrossDC-Algos/config/query-flow/query-flow-2-1.txt
```

批量脚本使用公共配置中的 `QUERY_FLOW_FILE` 选择场景。

### 3. 查看结果

最终对比图仅保留以下六个文件，历史实验及原始数据已清理：

| 指标 | PNG | PDF |
| --- | --- | --- |
| TX rate | [tx-rate.png](plots/tx-rate.png) | [tx-rate.pdf](plots/tx-rate.pdf) |
| 普通 DC 队列长度 | [queue-length.png](plots/queue-length.png) | [queue-length.pdf](plots/queue-length.pdf) |
| FCT | [fct.png](plots/fct.png) | [fct.pdf](plots/fct.pdf) |

五算法依次为 DCQCN、GRC、ATC、THEMIS、DCIExt；四场景为 1-1、1-2、2-1、2-2。
每组运行五次，固定 `randomSeed=7`、`RngSeed=1`，使用 `RngRun=1..5`。
TX/队列图展示第 1 次运行，FCT 展示五次运行的均值、各次结果和最小–最大范围。
TX 使用现有 `ns3::RdmaHw::TxRateSampleInterval=10` 属性采样，再计算 100µs 滑动窗、10µs 步长；
队列为每 40µs 采样的非 DCI 最大出口队列，单位为十进制 MB。FCT 不扣除跨域 RTT。

重新运行单算法脚本时，临时结果仍按以下路径生成：

- **日志文件**：`results/dump/algo_cc{ccMode}_load{load}.log`
- **数据文件**：`results/fct/`, `results/pfc/`
- **瓶颈队列文件**：默认位于 `results/bottleneck_queue_length/`，可通过已有的 `BOTTLENECK_MON_FILE` 配置指定。
- **图片文件**：`results/plots/workload_{algo}_cc{ccMode}_load{load}.png`

四个场景都使用 `config-micro.txt`。输出沿用原来的命名和目录，
同算法、同负载切换查询流后会覆盖同名结果；比较场景前先保存上一场景的结果。
辅助监控文件的路径由公共配置中的配置项控制。

## 输出说明

### 主图（吞吐量 + 队列长度）

包含两个子图：

- **上图**：接收端吞吐量（RX Throughput）- 每个 query flow 的接收速率
- **下图**：Top 3 拥塞点的队列长度 - 自动检测的平均队列长度最大的端口

### TX Rate 图

独立的发送端速率图，显示每个 query flow 的发送速率变化，保存在 `results/txrate/workload_txrate_{algo}_cc{ccMode}_load{load}.png`。

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
├── results/                    ✅ 临时数据（当前已清空）
├── plots/                      ✅ 最终 PNG/PDF 对比图
├── scripts/
│   ├── run_single_workload_algo.py  ✅ 单算法运行脚本
│   └── run_all_algos.sh        ✅ 批量运行脚本
└── README.md                   ✅ 本文件
```

## 当前测试场景

当前比较 DCQCN（模式 1）、DCI-Ext（模式 16）、GRC（模式 18）、ATC（模式 15）
和 THEMIS（模式 19），四个场景如下。
所有场景共用 [topology-micro.txt](config/topology/topology-micro.txt) 和
[config-micro.txt](config/workload/config-micro.txt)，随机种子默认为 `--randomSeed=7`。
当前背景负载为 0，以下列出的查询流就是实际运行的全部流；每条流均为
300,000,000 B（300 MB，约 286.10 MiB），优先级组 `pg=3`。

### 公共 micro 拓扑和部署

| DC | 主机 | ToR | 汇聚交换机 | DCI |
| --- | --- | --- | --- | --- |
| DC0 | H0–H3 | SW12、SW13 | SW18 | SW21 |
| DC1 | H4–H7 | SW14、SW15 | SW19 | SW22 |
| DC2 | H8–H11 | SW16、SW17 | SW20 | SW23 |

每台 ToR 连接两台主机。主机接入链路均为 40 Gbps，交换机间链路均为 100 Gbps。
跨域链路为 SW21↔SW22 和 SW22↔SW23，单向传播时延均为 5 ms；其余链路为 1.5 μs。

ATC 和 DCI-Ext 的发送侧固定为 SW21/SW23，接收侧固定为 SW22，
没有查询流的源 DC 保持空闲，无需切换部署节点。
micro 与 workload 两份配置的参数一致，保留默认输入、拓扑接入带宽自动推导和固定 1 Gbps 低速门限；
DCQCN 与 DCI-Ext 使用已确认的 `dcix-ack-rework` 实验参数。SW16–SW20 固定为
100 Gbps、RTT/BDP 窗口只计算往返传播时延（不加包串行化时间）。`NIC_DELAY` 为
15 μs，入口 PFC 池为 9 MiB、alpha 为 0.125。

DCI-Ext 的新流观察期为 `DCIEXT_OBSERVE_MB 20`（20 MiB，20,971,520 B）。
收发双方共用周期 `DCIEXT_T_US 400`，用于接收方反馈和发送方周期压制。
达到观察量后发两个学费 CNP，再进入 `DCIEXT_SILENT_US 400` 静默阶段。
离开静默期后直接查固定压制表：alpha0=0.2、g=1/256、CNP 间隔 4 μs，
表在构造时生成，普通压制最多 80 个 CNP，不维护 alpha 追踪状态或初值配置。
RTT 样本不使用固定 20 μs 下限；重传歧义样本被排除，发送水位跨流老化保留。

带宽基准 B 使用拓扑解析得到的服务器接入速率 `nic_rate`（当前为 40 Gbps，假设所有接入链路速率一致），无需单独配置。
发送侧新 DIP 的初始配额为 B/4，
接收侧 PROBE 候选速率上界为 B。总 ACK 速率低于 1 Gbps 时保持配额估计；
PROBE 候选速率下界和每流配额下界也固定为 1 Gbps。
RTT 目标与容差分别为 `DCIEXT_DT_US 10`、`DCIEXT_SIGMA_US 6`，流老化为 4 个反馈周期。

查询流文件首行是流数量，后续各行格式为：

```text
src dst pg dport flowSize(B) startTime(s)
```

文件最后一列是相对启动偏移，实际仿真开始时间由
`实际开始时间 = 配置中的 START_TIME + 文件中的 startTime` 计算。
公共配置的 `START_TIME=0`，因此文件偏移就是实际开始时间。
下表时间单位为 ms，流大小单位为 B。
DC 编号采用程序推导的编号，从 DC0 开始。

### 1. 2 条短距流 + 2 条长距流

查询流：[query-flow-2-2.txt](config/query-flow/query-flow-2-2.txt)。

两条跨域流分别从 DC0、DC2 发往 DC1，与 DC1 内的两条流共同竞争 H4 的接收带宽。

| 流 | 源主机（所属 DC） | 目的主机（所属 DC） | 类型 | 大小（B） | 文件偏移（ms） | 实际开始（ms） |
| --- | --- | --- | --- | ---: | ---: | ---: |
| 1 | H0（DC0） | H4（DC1） | 跨域 | 300000000 | 0 | 0 |
| 2 | H8（DC2） | H4（DC1） | 跨域 | 300000000 | 0 | 0 |
| 3 | H6（DC1） | H4（DC1） | 域内 | 300000000 | 5.01 | 5.01 |
| 4 | H7（DC1） | H4（DC1） | 域内 | 300000000 | 5.01 | 5.01 |

### 2. 2 条短距流 + 1 条长距流

查询流：[query-flow-2-1.txt](config/query-flow/query-flow-2-1.txt)。

H0→H6、H5→H6 与 H4→H6 均发往 H6，共享 SW15→H6 的 40 Gbps 接收链路。

| 流 | 源主机（所属 DC） | 目的主机（所属 DC） | 类型 | 大小（B） | 文件偏移（ms） | 实际开始（ms） |
| --- | --- | --- | --- | ---: | ---: | ---: |
| 1 | H0（DC0） | H6（DC1） | 跨域 | 300000000 | 0 | 0 |
| 2 | H5（DC1） | H6（DC1） | 域内 | 300000000 | 5.01 | 5.01 |
| 3 | H4（DC1） | H6（DC1） | 域内 | 300000000 | 5.01 | 5.01 |

### 3. 1 条短距流 + 2 条长距流

查询流：[query-flow-1-2.txt](config/query-flow/query-flow-1-2.txt)。

H0→H4、H1→H4 与 H6→H4 均发往 H4，共享 SW14→H4 的 40 Gbps 接收链路。

| 流 | 源主机（所属 DC） | 目的主机（所属 DC） | 类型 | 大小（B） | 文件偏移（ms） | 实际开始（ms） |
| --- | --- | --- | --- | ---: | ---: | ---: |
| 1 | H0（DC0） | H4（DC1） | 跨域 | 300000000 | 0 | 0 |
| 2 | H1（DC0） | H4（DC1） | 跨域 | 300000000 | 0 | 0 |
| 3 | H6（DC1） | H4（DC1） | 域内 | 300000000 | 5.01 | 5.01 |

### 4. 1 条短距流 + 1 条长距流

查询流：[query-flow-1-1.txt](config/query-flow/query-flow-1-1.txt)。

两条流共享 SW14→H4 的 40 Gbps 接收链路，DC2 空闲。
跨域流从 0 ms 开始，域内流从 5.01 ms 开始。

| 流 | 源主机（所属 DC） | 目的主机（所属 DC） | 类型 | 大小（B） | 文件偏移（ms） | 实际开始（ms） |
| --- | --- | --- | --- | ---: | ---: | ---: |
| 1 | H0（DC0） | H4（DC1） | 跨域 | 300000000 | 0 | 0 |
| 2 | H5（DC1） | H4（DC1） | 域内 | 300000000 | 5.01 | 5.01 |

### 运行与结果口径

四个场景的仿真上限均为 2 s，均开启查询流完成后自动停止。
FCT 从各条流的实际开始时间计起，域内和跨域流分别统计。当前 FCT 文件只记录已完成流，
分析时需单独记录未完成流，不能只用已完成流计算均值来代表全部流。
