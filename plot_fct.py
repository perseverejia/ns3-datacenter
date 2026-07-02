#!/usr/bin/env python3
"""
FCT 柱状图对比脚本
核心思路：读 5 个 FCT 文件 → 过滤出小流 → 算平均/p99 → 画三张图。
只读已有结果，不触发仿真。
"""

import os
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

FCT_DIR = "/home/shemuping/newCode/ns3-FRP/results/fct"
RESULTS_DIR = "/home/shemuping/newCode/ns3-FRP/results"
ALGOS = [("dcqcn", "DCQCN"), ("hpcc", "HPCC"), ("timely", "TIMELY"),
         ("frp", "FRP"), ("rocc", "ROCC")]
SMALL_FLOW_SIZE = 10000          # 小流判定：m_size == 10000 (10KB)
CROSS_DC_THRESHOLD_NS = 100000   # standalone_fct > 100μs 视为 cross-DC


def parse_fct_file(filepath):
    """解析单个 FCT 文件，返回小流列表 [{fct_ns, standalone_ns, is_cross_dc}]"""
    flows = []
    if not os.path.exists(filepath):
        print(f"[WARN] 文件不存在: {filepath}")
        return flows
    with open(filepath, 'r') as f:
        for line in f:
            parts = line.split()
            if len(parts) < 8:
                continue
            m_size = int(parts[4])
            if m_size != SMALL_FLOW_SIZE:
                continue
            fct_ns = int(parts[6])
            standalone_ns = int(parts[7])
            is_cross = standalone_ns > CROSS_DC_THRESHOLD_NS
            flows.append({"fct_ns": fct_ns,
                          "standalone_ns": standalone_ns,
                          "is_cross": is_cross})
    return flows


def percentile(values, p):
    """简单 p99 分位数（最近秩法）"""
    if not values:
        return float('nan')
    s = sorted(values)
    k = int(np.ceil(p / 100.0 * len(s)))
    k = min(max(k, 1), len(s))
    return s[k - 1]


def aggregate(flows):
    """对一组流计算平均 FCT 和 p99 FCT（μs）"""
    if not flows:
        return float('nan'), float('nan'), 0
    fcts = [fl["fct_ns"] for fl in flows]
    avg = np.mean(fcts) / 1000.0
    p99 = percentile(fcts, 99) / 1000.0
    return avg, p99, len(fcts)


def main():
    algo_data = {}
    for fname, display in ALGOS:
        path = os.path.join(FCT_DIR, f"fct_{fname}.txt")
        flows = parse_fct_file(path)
        algo_data[display] = flows

    # 聚合统计
    names = [d for _, d in ALGOS]
    avg_all, p99_all = [], []
    avg_intra, avg_cross = [], []
    has_intra, has_cross = [], []

    print("\n================ FCT 统计 ================")
    print(f"{'算法':<8} {'小流数':<6} {'intra':<6} {'cross':<6} "
          f"{'avg(μs)':<12} {'p99(μs)':<12} "
          f"{'intra_avg(μs)':<14} {'cross_avg(μs)':<14}")
    for name in names:
        flows = algo_data[name]
        intra = [fl for fl in flows if not fl["is_cross"]]
        cross = [fl for fl in flows if fl["is_cross"]]
        a_all, p_all, n_all = aggregate(flows)
        a_in, _, n_in = aggregate(intra)
        a_cr, _, n_cr = aggregate(cross)
        avg_all.append(a_all); p99_all.append(p_all)
        avg_intra.append(a_in); avg_cross.append(a_cr)
        has_intra.append(n_in > 0); has_cross.append(n_cr > 0)
        print(f"{name:<10} {n_all:<10} {n_in:<6} {n_cr:<6} "
              f"{a_all:<12.1f} {p_all:<12.1f} "
              f"{a_in:<14.1f} {a_cr:<14.1f}")
    print("==========================================\n")

    # 颜色：五种算法各一种颜色
    color_map = {'DCQCN': '#1f77b4', 'HPCC': '#ff7f0e', 'TIMELY': '#2ca02c',
                 'FRP': '#17becf', 'ROCC': '#8c564b'}
    colors = [color_map[n] for n in names]

    # ---- 主图：平均 FCT ----
    fig, ax = plt.subplots(figsize=(10, 6))
    bars = ax.bar(names, avg_all, color=colors)
    ax.set_ylabel("Average FCT (μs)")
    ax.set_title("Average FCT of Flows per Algorithm")
    for bar, val in zip(bars, avg_all):
        if not np.isnan(val):
            ax.text(bar.get_x() + bar.get_width() / 2, val,
                    f'{val:.1f}', ha='center', va='bottom', fontsize=9)
    fig.tight_layout()
    fig.savefig(os.path.join(RESULTS_DIR, "fct_avg.png"), dpi=150)
    plt.close(fig)

    # ---- 辅图：p99 FCT ----
    fig, ax = plt.subplots(figsize=(10, 6))
    bars = ax.bar(names, p99_all, color=colors)
    ax.set_ylabel("p99 FCT (μs)")
    ax.set_title("p99 FCT of Flows per Algorithm")
    for bar, val in zip(bars, p99_all):
        if not np.isnan(val):
            ax.text(bar.get_x() + bar.get_width() / 2, val,
                    f'{val:.1f}', ha='center', va='bottom', fontsize=9)
    fig.tight_layout()
    fig.savefig(os.path.join(RESULTS_DIR, "fct_p99.png"), dpi=150)
    plt.close(fig)

    # ---- 补充图：intra vs cross 分组平均 FCT（对数 y 轴）----
    x = np.arange(len(names))
    width = 0.35
    fig, ax = plt.subplots(figsize=(12, 6))
    intra_vals = [v if h else 0 for v, h in zip(avg_intra, has_intra)]
    cross_vals = [v if h else 0 for v, h in zip(avg_cross, has_cross)]
    b1 = ax.bar(x - width / 2, intra_vals, width, label='intra-DC', color='steelblue')
    b2 = ax.bar(x + width / 2, cross_vals, width, label='cross-DC', color='orange')
    ax.set_yscale('log')
    ax.set_xticks(x)
    ax.set_xticklabels(names)
    ax.set_ylabel("Average FCT (μs, log)")
    ax.set_title("Average FCT: Intra-DC vs Cross-DC Flows")
    ax.legend()
    for bar, val, has in list(zip(b1, avg_intra, has_intra)) + list(zip(b2, avg_cross, has_cross)):
        if has and not np.isnan(val):
            ax.text(bar.get_x() + bar.get_width() / 2, val,
                    f'{val:.1f}', ha='center', va='bottom', fontsize=8)
    fig.tight_layout()
    fig.savefig(os.path.join(RESULTS_DIR, "fct_intra_cross.png"), dpi=150)
    plt.close(fig)

    print("已生成:")
    print(f"  {RESULTS_DIR}/fct_avg.png")
    print(f"  {RESULTS_DIR}/fct_p99.png")
    print(f"  {RESULTS_DIR}/fct_intra_cross.png")


if __name__ == "__main__":
    main()