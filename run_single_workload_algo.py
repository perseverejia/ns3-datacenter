#!/usr/bin/env python3
"""
crossDC workload 算法运行脚本。
默认混合流量模式（flow-file 长背景流 + Alistorage CDF 前景流），命令行只需指定算法：
    python3 run_single_workload_algo.py 13 FRP --load 0.2 --duration 0.02 

关闭背景流，回到 workload-only 模式：
    python3 run_single_workload_algo.py 13 FRP --enableFlowFileBackground 0 --load 0.2 --duration 0.02
"""

import argparse
import os
import re
import subprocess

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
NS3_DIR = os.path.join(REPO_ROOT, "simulator/ns-3.39")
CONFIG_FILE = "examples/PowerTCP/config-workload.txt"
DEFAULT_CDF = "examples/PowerTCP/Alistorage.txt"
DUMP_DIR = os.path.join(REPO_ROOT, "dump/workload")
RESULTS_DIR = os.path.join(REPO_ROOT, "results/workload")
FCT_DIR = os.path.join(RESULTS_DIR, "fct")
PFC_DIR = os.path.join(RESULTS_DIR, "pfc")


def run_and_plot(args):
    """运行 workload 仿真并绘图"""

    print(f"\n{'='*80}")
    print(f"仿真: {args.algo_name} (ccMode={args.ccMode}, load={args.load})")
    print(f"{'='*80}\n")

    os.makedirs(DUMP_DIR, exist_ok=True)
    os.makedirs(RESULTS_DIR, exist_ok=True)
    os.makedirs(FCT_DIR, exist_ok=True)
    os.makedirs(PFC_DIR, exist_ok=True)

    # 1. 更新配置文件
    config_path = os.path.join(NS3_DIR, CONFIG_FILE)
    with open(config_path, "r", encoding="utf-8") as f:
        content = f.read()

    suffix = f"{args.algo_name.lower()}_load{args.load}"
    if args.enableFlowFileBackground:
        suffix += "_bgflow"
    fct_out = os.path.join(FCT_DIR, f"fct_{suffix}.txt")
    pfc_out = os.path.join(PFC_DIR, f"pfc_{suffix}.txt")
    content = re.sub(r"FCT_OUTPUT_FILE\s+\S+", f"FCT_OUTPUT_FILE {fct_out}", content)
    content = re.sub(r"PFC_OUTPUT_FILE\s+\S+", f"PFC_OUTPUT_FILE {pfc_out}", content)

    with open(config_path, "w", encoding="utf-8") as f:
        f.write(content)

    config = CONFIG_FILE
    log_file = os.path.join(DUMP_DIR, f"algo_cc{args.ccMode}_load{args.load}{'_bgflow' if args.enableFlowFileBackground else ''}.log")

    try:
        # 2. 编译
        with open(log_file, "w", encoding="utf-8") as log:
            log.write("编译 crossDC-evaluation-workload...\n")
            subprocess.run(["./ns3", "build", "crossDC-evaluation-workload"], cwd=NS3_DIR,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
            log.write("✓ 编译完成\n\n")

        # 3. 运行仿真
        flow_end = args.flowEnd if args.flowEnd is not None else max(args.start, args.duration * 0.9)
        cdf_path = args.cdf
        if not os.path.isabs(cdf_path):
            root_cdf_path = os.path.join(REPO_ROOT, cdf_path)
            if os.path.exists(root_cdf_path):
                cdf_path = root_cdf_path

        log_file = os.path.join(DUMP_DIR, f"algo_cc{args.ccMode}_load{args.load}{'_bgflow' if args.enableFlowFileBackground else ''}.log")
        binary = os.path.join(NS3_DIR, "build/examples/PowerTCP/ns3.39-crossDC-evaluation-workload-optimized")
        cmd = [
            binary,
            f"--conf={config}",
            f"--algorithm={args.ccMode}",
            f"--cdfFileName={cdf_path}",
            f"--load={args.load}",
            f"--START_TIME={args.start}",
            f"--END_TIME={args.duration}",
            f"--FLOW_LAUNCH_END_TIME={flow_end}",
            f"--queryRequestRate={args.queryRequestRate}",
            f"--request={args.request}",
            f"--incast={args.incast}",
            f"--randomSeed={args.randomSeed}",
        ]
        if args.enableFlowFileBackground >= 0:
            cmd.append(f"--enableFlowFileBackground={args.enableFlowFileBackground}")
        if args.backgroundFlowFile:
            bg_path = args.backgroundFlowFile
            if not os.path.isabs(bg_path):
                root_bg_path = os.path.join(REPO_ROOT, bg_path)
                if os.path.exists(root_bg_path):
                    bg_path = root_bg_path
            cmd.append(f"--backgroundFlowFile={bg_path}")

        print("运行 workload 仿真...")
        with open(log_file, "a", encoding="utf-8") as log:
            subprocess.run(cmd, cwd=NS3_DIR, stdout=log, stderr=subprocess.STDOUT,
                           timeout=args.timeout, check=True)

        log_size = os.path.getsize(log_file)
        print(f"✓ 仿真完成 ({log_size/1024:.1f}KB)\n")

        # 4. 解析日志
        print("解析日志...")
        with open(log_file, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()

        queue_time, queue_kb = [], []
        flow_tp = {}
        workload_lines = []
        totals = {"flow": None, "query": None, "background": None}
        queue_switch = 85
        queue_port = 1

        for line in lines:
            line = line.strip()
            if "[WORKLOAD]" in line or "[WORKLOAD LEAF]" in line:
                workload_lines.append(line)

            m = re.search(r"Total flow:\s+(\d+)", line)
            if m:
                totals["flow"] = int(m.group(1))
            m = re.search(r"Total Query:\s+(\d+)", line)
            if m:
                totals["query"] = int(m.group(1))
            m = re.search(r"Total background flow:\s+(\d+)", line)
            if m:
                totals["background"] = int(m.group(1))

            if "[DCQCN_QLEN]" in line and args.ccMode in (1, 3, 7):
                parts = line.split()
                if len(parts) >= 5 and int(parts[2]) == queue_switch and int(parts[3]) == queue_port:
                    queue_time.append(float(parts[1]) / 1e9 * 1000)
                    queue_kb.append(float(parts[4]) / 1024.0)

            if "[FRP_DATA_SW]" in line and args.ccMode in (13, 14):
                parts = line.split()
                if len(parts) >= 8 and int(parts[2]) == queue_switch and int(parts[3]) == queue_port and int(parts[7]) == args.ccMode:
                    queue_time.append(float(parts[1]) * 1000)
                    queue_kb.append(float(parts[5]))

            m = re.search(
                r"\[FLOW TP\] Src (\d+) Dst (\d+) pg (\d+) sport (\d+) dport (\d+) "
                r"throughput ([\d.e+\-]+) time ([\d.e+\-]+)", line)
            if m:
                key = tuple(map(int, (m.group(1), m.group(2), m.group(4), m.group(5))))
                flow_tp.setdefault(key, {"times": [], "rates": []})
                flow_tp[key]["times"].append(float(m.group(7)) * 1000)
                flow_tp[key]["rates"].append(float(m.group(6)) / 1e9)

        active_flows = sorted(flow_tp)
        print(f"✓ 解析完成: Switch85 {len(queue_kb)} 点, 接收侧流 {len(active_flows)} 条")

        # 5. 绘图
        print("生成图像...")
        plt.rcParams["font.family"] = "DejaVu Sans"
        plt.rcParams["axes.unicode_minus"] = False
        colors = ["red", "blue", "green", "orange", "purple", "cyan", "lime", "brown", "pink", "gray",
                  "magenta", "olive", "teal", "navy", "maroon", "coral", "gold", "indigo"]

        fig, axes = plt.subplots(2, 1, figsize=(18, 12), sharex=True)
        fig.suptitle(f"{args.algo_name} (ccMode={args.ccMode}) - Workload Traffic Analysis, load={args.load}",
                     fontsize=18, fontweight="bold")

        ax2 = axes[0]
        print(f"绘制 {len(active_flows)} 条流的接收速率...")
        for idx, key in enumerate(active_flows[:15]):
            times = flow_tp[key]["times"]
            rates = flow_tp[key]["rates"]
            n = min(len(times), len(rates))
            if n == 0:
                continue
            times = times[:n]
            rates = rates[:n]
            ax2.plot(times, rates,
                     color=colors[idx % len(colors)], lw=2,
                     label=f"Flow {key[0]}->{key[1]} (sp={key[2]})", alpha=0.85)
        ax2.set_ylabel("Receive Throughput (Gbps)", fontsize=14, fontweight="bold")
        ax2.set_title(f"RX Throughput - per-flow ({len(active_flows)} flows)", fontsize=15, fontweight="bold")
        ax2.legend(loc="upper right", fontsize=10, ncol=3, framealpha=0.9)
        ax2.grid(True, alpha=0.3, linestyle="--")
        ax2.set_ylim(0, 110)
        ax2.tick_params(labelsize=11)

        ax3 = axes[1]
        if queue_time and queue_kb:
            ax3.fill_between(queue_time, 0, queue_kb, alpha=0.35, color="purple")
            ax3.plot(queue_time, queue_kb, color="purple", lw=2, label="Switch 85 Port 1")
            ax3.axhline(y=500, color="blue", ls="--", lw=2, alpha=0.7, label="qRef = 500KB")
            ax3.axhline(y=300, color="red", ls=":", lw=1.5, alpha=0.6, label="q_th = 300KB")
        ax3.set_ylabel("Queue Length (KB)", fontsize=14, fontweight="bold")
        ax3.set_xlabel("Time (ms)", fontsize=14, fontweight="bold")
        ax3.set_title("Switch 85 Queue Length (Port 1 - Bottleneck, →Host 53)", fontsize=15, fontweight="bold")
        ax3.legend(loc="upper right", fontsize=11, framealpha=0.9)
        ax3.grid(True, alpha=0.3, linestyle="--")
        max_q = max(queue_kb) if queue_kb else 1000
        ax3.set_ylim(-50, max_q * 1.15)
        ax3.tick_params(labelsize=11)

        plt.tight_layout()
        output_suffix = f"{args.algo_name.lower()}_cc{args.ccMode}_load{args.load}"
        if args.enableFlowFileBackground:
            output_suffix += "_bgflow"
        output_file = os.path.join(RESULTS_DIR, f"workload_{output_suffix}.png")
        plt.savefig(output_file, dpi=150, bbox_inches="tight")
        print(f"✓ 图像已保存: {output_file}\n")

        # 6. 打印统计
        fct_ns = []
        if os.path.exists(fct_out):
            with open(fct_out, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    parts = line.split()
                    if len(parts) < 7:
                        continue
                    try:
                        fct_ns.append(float(parts[6]))
                    except ValueError:
                        continue

        print(f"{'='*80}")
        print(f"统计信息 - {args.algo_name}")
        print(f"{'='*80}")
        print(f"Total background flow: {totals['background']}  Total flow: {totals['flow']}  Total Query: {totals['query']}")
        print(f"发现信息/负载行: {len(workload_lines)} 行")
        print(f"活跃流数量: {len(active_flows)}")
        print(f"队列点数量: {len(queue_kb)}")
        if queue_kb:
            print(f"队列平均值: {np.mean(queue_kb):.0f} KB")
            print(f"队列最小值: {min(queue_kb):.0f} KB")
            print(f"队列最大值: {max(queue_kb):.0f} KB")
        else:
            print("队列数据: 无")
        if fct_ns:
            fct_ms = np.array(fct_ns) / 1e6
            print(f"FCT 统计: 完成流={len(fct_ms)} 平均完成时间={np.mean(fct_ms):.6f} ms P99完成时间={np.percentile(fct_ms, 99):.6f} ms")
        else:
            print("FCT 统计: 无完成流数据")
        print(f"日志: {log_file}")
        print(f"FCT: {fct_out}")
        print(f"PFC: {pfc_out}")
        print(f"图像文件: {output_file}")
        print(f"{'='*80}\n")

        return output_file
    finally:
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ccMode", type=int)
    parser.add_argument("algo_name")
    parser.add_argument("--load", type=float, default=0.2)
    parser.add_argument("--duration", type=float, default=0.02)
    parser.add_argument("--start", type=float, default=0.001)
    parser.add_argument("--flowEnd", type=float, default=None)
    parser.add_argument("--cdf", default=DEFAULT_CDF)
    parser.add_argument("--queryRequestRate", type=float, default=0.0)
    parser.add_argument("--request", type=int, default=0)
    parser.add_argument("--incast", type=int, default=5)
    parser.add_argument("--randomSeed", type=int, default=7)
    parser.add_argument("--timeout", type=int, default=360)
    parser.add_argument("--enableFlowFileBackground", type=int, default=1)
    parser.add_argument("--backgroundFlowFile", default="")
    args = parser.parse_args()

    run_and_plot(args)


if __name__ == "__main__":
    main()
