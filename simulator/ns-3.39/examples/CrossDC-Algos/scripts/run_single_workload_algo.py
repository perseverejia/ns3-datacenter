#!/usr/bin/env python3
"""
crossDC workload algorithm runner script.
Workload parameters live in simulator/ns-3.39/examples/CrossDC-Algos/config/workload/config-workload.txt.
Specify only the algorithm here, for example:
    python3 run_single_workload_algo.py 13 FRP
"""

import argparse
import os
import re
import subprocess

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np


def analyze_query_flow_fct(query_fct_file):
    """Analyze query flow FCT and print statistics."""
    if not os.path.exists(query_fct_file):
        print(f"Query flow FCT file not found: {query_fct_file}")
        return

    print(f"\n{'='*80}")
    print("Query Flow FCT Analysis")
    print(f"{'='*80}")
    print(f"File: {query_fct_file}\n")

    flows = []
    with open(query_fct_file, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.strip().split()
            if len(parts) >= 8:
                flows.append({
                    "src": int(parts[0]),
                    "dst": int(parts[1]),
                    "pg": int(parts[2]),
                    "dport": int(parts[3]),
                    "size": int(parts[4]),
                    "start_ns": int(parts[5]),
                    "fct_ns": int(parts[6]),
                    "is_cross_dc": int(parts[7]),
                })

    if not flows:
        print("No query flows found!")
        return

    print(f"{'No.':<6} {'Src':<8} {'Dst':<8} {'PG':<6} {'Dport':<8} {'Size(MB)':<10} {'Start(ms)':<12} {'FCT(ms)':<12} {'Cross-DC':<10}")
    print("=" * 80)

    for idx, flow in enumerate(flows, 1):
        size_mb = flow["size"] / (1024 * 1024)
        start_ms = flow["start_ns"] / 1e6
        fct_ms = flow["fct_ns"] / 1e6
        cross_dc = "Yes" if flow["is_cross_dc"] == 1 else "No"
        print(f"{idx:<6} {flow['src']:<8} {flow['dst']:<8} {flow['pg']:<6} {flow['dport']:<8} "
              f"{size_mb:<10.2f} {start_ms:<12.3f} {fct_ms:<12.3f} {cross_dc:<10}")

    print("=" * 80)

    fct_values = np.array([f["fct_ns"] / 1e6 for f in flows])
    cross_dc_flows = [f for f in flows if f["is_cross_dc"] == 1]
    intra_dc_flows = [f for f in flows if f["is_cross_dc"] == 0]

    print("\nStatistics:")
    print(f"  Total query flows: {len(flows)}")
    print(f"  Cross-DC flows: {len(cross_dc_flows)}")
    print(f"  Intra-DC flows: {len(intra_dc_flows)}")
    print(f"\n  Average FCT: {np.mean(fct_values):.3f} ms")
    print(f"  Min FCT: {np.min(fct_values):.3f} ms")
    print(f"  Max FCT: {np.max(fct_values):.3f} ms")
    print(f"  P50 FCT: {np.percentile(fct_values, 50):.3f} ms")
    print(f"  P95 FCT: {np.percentile(fct_values, 95):.3f} ms")
    print(f"  P99 FCT: {np.percentile(fct_values, 99):.3f} ms")

    if cross_dc_flows and intra_dc_flows:
        cross_fct = np.array([f["fct_ns"] / 1e6 for f in cross_dc_flows])
        intra_fct = np.array([f["fct_ns"] / 1e6 for f in intra_dc_flows])
        print("\n  Cross-DC vs Intra-DC FCT Comparison:")
        print(f"    Cross-DC:  avg={np.mean(cross_fct):.3f} ms, min={np.min(cross_fct):.3f} ms, max={np.max(cross_fct):.3f} ms")
        print(f"    Intra-DC:  avg={np.mean(intra_fct):.3f} ms, min={np.min(intra_fct):.3f} ms, max={np.max(intra_fct):.3f} ms")
        print("    Note: FCT is raw (no compensation) for all flows")

    print(f"{'='*80}\n")


# 脚本位置: examples/CrossDC-Algos/scripts/run_single_workload_algo.py
# 需要回退 4 层到 ns-3.39 根目录
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CROSSDC_ALGOS_DIR = os.path.dirname(SCRIPT_DIR)  # examples/CrossDC-Algos
NS3_DIR = os.path.dirname(os.path.dirname(CROSSDC_ALGOS_DIR))  # ns-3.39
CONFIG_FILE = "examples/CrossDC-Algos/config/workload/config-workload.txt"
DUMP_DIR = os.path.join(CROSSDC_ALGOS_DIR, "results/dump")
RESULTS_DIR = os.path.join(CROSSDC_ALGOS_DIR, "results")
FCT_DIR = os.path.join(RESULTS_DIR, "fct")
PFC_DIR = os.path.join(RESULTS_DIR, "pfc")
TXRATE_DIR = os.path.join(RESULTS_DIR, "txrate")
PLOTS_DIR = os.path.join(CROSSDC_ALGOS_DIR, "plots")
ANALYSIS_DIR = os.path.join(RESULTS_DIR, "analysis")
ATC_ALGO_MODE = 15


def load_tag(value):
    try:
        percent = float(value) * 100
    except (TypeError, ValueError):
        return str(value).replace(".", "p")
    if percent.is_integer():
        return f"{int(percent)}pct"
    return f"{percent:g}pct".replace(".", "p")


def read_config_value(content, key, default=None):
    pattern = rf"^\s*{re.escape(key)}\s+(\S+)"
    match = re.search(pattern, content, re.MULTILINE)
    return match.group(1) if match else default


def resolve_output_path(path):
    if os.path.isabs(path):
        return path
    return os.path.join(NS3_DIR, path)


def resolve_ns3_path(path):
    if os.path.isabs(path):
        return path
    return os.path.join(NS3_DIR, path)


def load_query_flow_keys(query_flow_file):
    """Return {(src, dst, dport)} for query flows."""
    keys = set()
    if not query_flow_file:
        return keys

    path = resolve_ns3_path(query_flow_file)
    if not os.path.exists(path):
        print(f"Warning: query flow file not found, throughput plot will be empty: {path}")
        return keys

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        first = True
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if first:
                first = False
                if len(parts) == 1:
                    continue
            if len(parts) < 6:
                continue
            try:
                src = int(parts[0])
                dst = int(parts[1])
                dport = int(parts[3])
            except ValueError:
                continue
            keys.add((src, dst, dport))
    return keys


def parse_ip_to_node_map(lines):
    """Return {IPv4 uint32: node_id} from the routing-table audit log."""
    ip_to_node = {}
    pattern = re.compile(r"OK:\s*Host\s+(\d+)\s+\(IP=(\d+\.\d+\.\d+\.\d+)\)")
    for line in lines:
        match = pattern.search(line)
        if not match:
            continue
        octets = [int(octet) for octet in match.group(2).split(".")]
        ip_value = ((octets[0] << 24) | (octets[1] << 16) |
                    (octets[2] << 8) | octets[3])
        ip_to_node[ip_value] = int(match.group(1))
    return ip_to_node


def parse_tx_rate(lines, ip_to_node, query_flow_keys):
    """Return periodic send-side rates for query flows, in ms and Gbps."""
    tx_rates = {}
    pattern = re.compile(
        r"\[TX RATE\] Host (\d+) sip (\d+) dip (\d+) sport (\d+) dport (\d+) "
        r"pg (\d+) txRate ([\d.]+)bps(?: rt [\d.]+bps)? t (\d+)ns"
    )
    for line in lines:
        match = pattern.search(line)
        if not match:
            continue
        host, sip, dip, sport, dport, _pg, rate_bps, time_ns = match.groups()
        src_node = ip_to_node.get(int(sip), int(host))
        dst_node = ip_to_node.get(int(dip))
        if dst_node is None:
            continue
        dport = int(dport)
        if (src_node, dst_node, dport) not in query_flow_keys:
            continue
        key = (src_node, dst_node, int(sport), dport)
        data = tx_rates.setdefault(key, {"times": [], "rates": []})
        data["times"].append(int(time_ns) / 1e6)
        data["rates"].append(float(rate_bps) / 1e9)
    return tx_rates


def parse_cnp_events(lines, ip_to_node, query_flow_keys):
    """Return {flow_key: [CNP receive time in ms]} for query flows."""
    cnp_events = {}
    pattern = re.compile(
        r"\[DCQCN_CNP\]\s+([\d.]+)ms node=(\d+) cnp_received "
        r"sip=0x([0-9a-fA-F]+) dip=0x([0-9a-fA-F]+) "
        r"sport=(\d+) dport=(\d+)"
    )
    for line in lines:
        match = pattern.search(line)
        if not match:
            continue
        time_ms, node, sip, dip, sport, dport = match.groups()
        src_node = ip_to_node.get(int(sip, 16), int(node))
        dst_node = ip_to_node.get(int(dip, 16))
        if dst_node is None:
            continue
        flow_key = (src_node, dst_node, int(sport), int(dport))
        if (src_node, dst_node, int(dport)) not in query_flow_keys:
            continue
        cnp_events.setdefault(flow_key, []).append(float(time_ms))
    return cnp_events


def cnp_sample_windows(cnp_times, sample_interval_ms=0.1):
    """Collapse CNP events into 100us windows for a readable x-axis marker."""
    if not cnp_times:
        return np.array([])
    times = np.asarray(cnp_times)
    return np.unique(np.floor(times / sample_interval_ms) * sample_interval_ms)


def parse_topology_adjacency(topology_file):
    """解析拓扑文件，复现 C++ 的 ifIndex 分配（ifIndex 从 1 开始，0 为 loopback）。

    返回 (port_map, adj)：
      port_map[node][ifIndex] = 对端节点 id
      adj[node] = 邻居节点列表（用于推导流的最短路径）
    """
    port_map = {}
    adj = {}
    path = resolve_ns3_path(topology_file) if topology_file else ""
    if not path or not os.path.exists(path):
        print(f"Warning: topology file not found, PFC-to-flow mapping disabled: {path}")
        return port_map, adj
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        tokens = f.read().split()
    pos = 0

    def take(n):
        nonlocal pos
        vals = tokens[pos:pos + n]
        pos += n
        return vals

    header = take(5)
    if len(header) < 5:
        print(f"Warning: malformed topology header, PFC-to-flow mapping disabled: {path}")
        return port_map, adj
    _node_num, switch_num, _tors, link_num, long_link_count = (int(x) for x in header)
    take(switch_num)        # 交换机 id 列表
    take(long_link_count)   # 长链路索引列表
    counters = {}
    for _ in range(link_num):
        row = take(5)
        if len(row) < 2:
            break
        a, b = int(row[0]), int(row[1])
        ia = counters.get(a, 1)
        ib = counters.get(b, 1)
        counters[a] = ia + 1
        counters[b] = ib + 1
        port_map.setdefault(a, {})[ia] = b
        port_map.setdefault(b, {})[ib] = a
        adj.setdefault(a, []).append(b)
        adj.setdefault(b, []).append(a)
    return port_map, adj


def compute_shortest_path_edges(adj, src, dst):
    """BFS 求所有最短路径，返回有向边并集 {(u, v)}（u→v 方向）。"""
    from collections import deque
    if src == dst or src not in adj or dst not in adj:
        return set()
    dist = {src: 0}
    queue = deque([src])
    while queue:
        u = queue.popleft()
        for v in adj.get(u, []):
            if v not in dist:
                dist[v] = dist[u] + 1
                queue.append(v)
    if dst not in dist:
        return set()
    # 构建最短路 DAG 的前驱表
    prev = {src: []}
    queue = deque([src])
    while queue:
        u = queue.popleft()
        for v in adj.get(u, []):
            if dist.get(v) == dist[u] + 1 and dist[v] <= dist[dst]:
                if v not in prev:
                    prev[v] = []
                    queue.append(v)
                prev[v].append(u)
    # 从 dst 回溯收集所有边
    edges = set()
    stack = [dst]
    seen = set()
    while stack:
        v = stack.pop()
        if v == src or v in seen:
            continue
        seen.add(v)
        for u in prev.get(v, []):
            edges.add((u, v))
            if u not in seen:
                stack.append(u)
    return edges


def parse_pfc_events(pfc_file):
    """解析 pfc.txt，返回 [(time_ms, node, ifindex, ptype)]。

    type 含义：0=对端收到 PAUSE（接收通知），1=对端收到 RESUME（接收通知），
    2=发送 PAUSE，3=发送 RESUME。只取发送事件（2/3），避免同一事件重复计数。
    """
    events = []
    if not pfc_file or not os.path.exists(pfc_file):
        return events
    with open(pfc_file, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) != 5:
                continue
            try:
                t_ns, node, _ntype, ifindex, ptype = (int(x) for x in parts)
            except ValueError:
                continue
            if ptype in (2, 3):
                events.append((t_ns / 1e6, node, ifindex, ptype))
    return events


def map_pfc_events_to_flows(pfc_events, port_map, adj, flow_keys):
    """把 PFC 事件映射到受影响的查询流。

    node 从 ifindex 端口发出 PAUSE/RESUME，反压的是 对端→node 方向的链路；
    流路径经过该有向边则视为受影响。返回 {flow_key: {"pause": [...], "resume": [...]}}。
    """
    result = {}
    if not pfc_events:
        return result
    flow_edges = {}
    for key in flow_keys:
        flow_edges[key] = compute_shortest_path_edges(adj, key[0], key[1])
    unmapped = 0
    for t_ms, node, ifindex, ptype in pfc_events:
        peer = port_map.get(node, {}).get(ifindex)
        if peer is None:
            unmapped += 1
            continue
        edge = (peer, node)  # 被反压的方向：peer → node
        for key, edges in flow_edges.items():
            if edge in edges:
                bucket = result.setdefault(key, {"pause": [], "resume": []})
                bucket["pause" if ptype == 2 else "resume"].append(t_ms)
    if unmapped:
        print(f"Warning: {unmapped} PFC events skipped (node/ifindex not found in topology)")
    return result


def plot_tx_rate(tx_rates, cnp_events, pfc_events_by_flow, output_path, algo_name, cc_mode, load):
    """Save one TX-rate subplot per query flow, with CNP and PFC markers."""
    plt.rcParams["font.family"] = "DejaVu Sans"
    plt.rcParams["axes.unicode_minus"] = False
    colors = ["red", "blue", "green", "orange", "purple", "cyan", "lime", "brown",
              "pink", "gray", "magenta", "olive", "teal", "navy", "maroon", "coral",
              "gold", "indigo"]

    active_flows = sorted(tx_rates)
    fig, axes = plt.subplots(len(active_flows), 1, figsize=(18, max(4.5, 3.8 * len(active_flows))),
                             sharex=True, squeeze=False)
    fig.suptitle(f"{algo_name} (ccMode={cc_mode}) - Per-Flow TX Rate with CNP/PFC, load={load}",
                 fontsize=18, fontweight="bold")

    for idx, key in enumerate(active_flows):
        ax = axes[idx][0]
        data = tx_rates[key]
        ax.plot(data["times"], data["rates"], color=colors[idx % len(colors)], lw=2, alpha=0.9,
                label=f"TX {key[0]}->{key[1]} (sp={key[2]}, dp={key[3]})")

        raw_cnp_times = cnp_events.get(key, [])
        cnp_windows = cnp_sample_windows(raw_cnp_times)
        if len(cnp_windows):
            ax.scatter(cnp_windows, np.zeros(len(cnp_windows)), s=44, marker="o", color="black",
                       edgecolors="white", linewidths=0.5, zorder=5, clip_on=False,
                       label=f"CNP received ({len(raw_cnp_times)} events; 100us windows)")

        pfc_info = pfc_events_by_flow.get(key, {}) if pfc_events_by_flow else {}
        pause_times = np.asarray(pfc_info.get("pause", []))
        if len(pause_times):
            ax.scatter(pause_times, np.zeros(len(pause_times)), s=64, marker="^", color="red",
                       edgecolors="white", linewidths=0.4, zorder=6, clip_on=False,
                       label=f"PFC PAUSE on path ({len(pause_times)} events)")

        ax.set_ylabel("TX Rate (Gbps)", fontsize=12, fontweight="bold")
        ax.set_title(f"Flow {key[0]}->{key[1]} (sport={key[2]}, dport={key[3]})",
                     fontsize=13, fontweight="bold")
        ax.grid(True, alpha=0.3, linestyle="--")
        ax.set_ylim(bottom=0)
        ax.tick_params(labelsize=10)
        ax.legend(loc="upper right", fontsize=9, framealpha=0.9)

    axes[-1][0].set_xlabel("Time (ms)", fontsize=14, fontweight="bold")
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(output_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"TX RATE plot saved: {output_path}")


def render_template(template, suffix, args, load):
    return template.format(
        suffix=suffix,
        algo=args.algo_name.lower(),
        ccMode=args.ccMode,
        load=load,
        seed=args.randomSeed,
    )


def run_and_plot(args):
    """Run workload simulation and generate plots."""
    gateway_type = 2 if args.ccMode == ATC_ALGO_MODE else 0

    print(f"\n{'='*80}")
    print(f"Simulation: {args.algo_name} (ccMode={args.ccMode})")
    if args.ccMode == ATC_ALGO_MODE:
        print(f"ATC mode: simulator ccMode={args.ccMode} (DCQCN), gatewayType={gateway_type}")
    print(f"{'='*80}\n")

    os.makedirs(DUMP_DIR, exist_ok=True)
    os.makedirs(RESULTS_DIR, exist_ok=True)
    os.makedirs(FCT_DIR, exist_ok=True)
    os.makedirs(PFC_DIR, exist_ok=True)
    os.makedirs(PLOTS_DIR, exist_ok=True)

    config_path = os.path.join(NS3_DIR, CONFIG_FILE)
    with open(config_path, "r", encoding="utf-8") as f:
        content = f.read()

    load = read_config_value(content, "LOAD", "default")
    
    # 提取CDF名称（如 websearch, alistorage）
    cdf_path = read_config_value(content, "CDF_FILE_NAME", "")
    cdf_base = os.path.splitext(os.path.basename(cdf_path))[0].lower() if cdf_path else "unknown"
    
    # 格式化负载值（如 0.20）
    try:
        load_str = f"{float(load):.2f}"
    except ValueError:
        load_str = load
    
    suffix = f"{args.algo_name.lower()}_{cdf_base}_load{load_str}"

    fct_template = read_config_value(
        content,
        "FCT_OUTPUT_TEMPLATE",
        os.path.join(FCT_DIR, "fct_{suffix}.txt"),
    )
    pfc_template = read_config_value(
        content,
        "PFC_OUTPUT_TEMPLATE",
        os.path.join(PFC_DIR, "pfc_{suffix}.txt"),
    )
    query_template = read_config_value(
        content,
        "QUERY_FCT_OUTPUT_TEMPLATE",
        os.path.join(FCT_DIR, "query-flow-{suffix}.txt"),
    )
    query_flow_file = read_config_value(content, "QUERY_FLOW_FILE", "")
    # 命令行覆盖：同步 Python 侧使用的查询流文件（用于绘图时识别查询流）
    if args.query_flow:
        query_flow_file = args.query_flow
    query_flow_keys = load_query_flow_keys(query_flow_file)
    # print(f"Loaded {len(query_flow_keys)} query flow definitions for throughput plotting.")

    fct_out = resolve_output_path(render_template(fct_template, suffix, args, load))
    pfc_out = resolve_output_path(render_template(pfc_template, suffix, args, load))
    query_fct_out = resolve_output_path(render_template(query_template, suffix, args, load))
    for path in (fct_out, pfc_out, query_fct_out):
        os.makedirs(os.path.dirname(path), exist_ok=True)

    config = CONFIG_FILE
    log_file = os.path.join(DUMP_DIR, f"algo_cc{args.ccMode}_load{load_tag(load)}.log")

    try:
        with open(log_file, "w", encoding="utf-8") as log:
            log.write("Building crossDC-evaluation-workload...\n")
            subprocess.run(["./ns3", "build", "crossDC-evaluation-workload"], cwd=NS3_DIR,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
            log.write("Build complete\n\n")

        binary = os.path.join(NS3_DIR, "build/examples/CrossDC-Algos/ns3.39-crossDC-evaluation-workload-optimized")
        cmd = [
            binary,
            f"--conf={config}",
            f"--algorithm={args.ccMode}",
            f"--gatewayType={gateway_type}",
            f"--randomSeed={args.randomSeed}",
            f"--fctOutputFile={fct_out}",
            f"--pfcOutputFile={pfc_out}",
            f"--queryFlowFctFile={query_fct_out}",
        ]
        # 命令行覆盖拓扑/查询流文件（优先于配置文件）
        if args.topology:
            cmd.append(f"--topologyFile={args.topology}")
            print(f"Topology override: {args.topology}")
        if args.query_flow:
            cmd.append(f"--queryFlowFile={args.query_flow}")
            print(f"Query flow override: {args.query_flow}")

        print("Running workload simulation...")
        with open(log_file, "a", encoding="utf-8") as log:
            subprocess.run(cmd, cwd=NS3_DIR, stdout=log, stderr=subprocess.STDOUT,
                           timeout=args.timeout, check=True)

        log_size = os.path.getsize(log_file)
        print(f"Simulation complete ({log_size/1024:.1f}KB)\n")

        print("Parsing log...")
        with open(log_file, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()

        all_queue_data = {}
        flow_tp = {}
        workload_lines = []
        totals = {"flow": None, "query": None, "background": None}

        for line in lines:
            line = line.strip()
            if "[WORKLOAD]" in line or "[WORKLOAD LEAF]" in line:
                workload_lines.append(line)

            m = re.search(r"Total flows?:\s+(\d+)", line, re.IGNORECASE)
            if m:
                totals["flow"] = int(m.group(1))
            m = re.search(r"Total Query:\s+(\d+)", line)
            if m:
                totals["query"] = int(m.group(1))
            m = re.search(r"Total background flows?:\s+(\d+)", line, re.IGNORECASE)
            if m:
                totals["background"] = int(m.group(1))

            if "[DCQCN_QLEN]" in line and args.ccMode in (1, 3, 7, 15, 16, 17, 18):
                # ccMode=15 (ATC) / ccMode=16 (DCI-Ext) / ccMode=17 (ePFC) / ccMode=18 (GRC) 队列监控走同一 [DCQCN_QLEN] 输出（switch-node.cc 按交换机监控打印，与 cc_mode 无关）
                parts = line.split()
                if len(parts) >= 5:
                    sw = int(parts[2])
                    port = int(parts[3])
                    t_ms = float(parts[1]) / 1e9 * 1000
                    q_kb = float(parts[4]) / 1024.0
                    all_queue_data.setdefault((sw, port), {"times": [], "queues": []})
                    all_queue_data[(sw, port)]["times"].append(t_ms)
                    all_queue_data[(sw, port)]["queues"].append(q_kb)

            if "[FRP_DATA_SW]" in line and args.ccMode in (13, 14):
                parts = line.split()
                if len(parts) >= 8 and int(parts[7]) == args.ccMode:
                    sw = int(parts[2])
                    port = int(parts[3])
                    t_ms = float(parts[1]) * 1000
                    q_kb = float(parts[5])
                    all_queue_data.setdefault((sw, port), {"times": [], "queues": []})
                    all_queue_data[(sw, port)]["times"].append(t_ms)
                    all_queue_data[(sw, port)]["queues"].append(q_kb)

            m = re.search(
                r"\[FLOW TP\] Src (\d+) Dst (\d+) pg (\d+) sport (\d+) dport (\d+) "
                r"throughput ([\d.e+\-]+) time ([\d.e+\-]+)", line)
            if m:
                src = int(m.group(1))
                dst = int(m.group(2))
                sport = int(m.group(4))
                dport = int(m.group(5))
                if (src, dst, dport) not in query_flow_keys:
                    continue
                key = (src, dst, sport, dport)
                flow_tp.setdefault(key, {"times": [], "rates": []})
                flow_tp[key]["times"].append(float(m.group(7)) * 1000)
                flow_tp[key]["rates"].append(float(m.group(6)) / 1e9)

        MIN_SAMPLES = 5
        congestion_points = []
        for (sw, port), data in all_queue_data.items():
            queues = data["queues"]
            if len(queues) < MIN_SAMPLES:
                continue
            congestion_points.append({
                "switch": sw,
                "port": port,
                "avg_q_kb": float(np.mean(queues)),
                "max_q_kb": float(np.max(queues)),
                "n_samples": len(queues),
                "times": data["times"],
                "queues": queues,
            })
        congestion_points.sort(key=lambda x: x["avg_q_kb"], reverse=True)
        top_congestion = congestion_points[:min(3, len(congestion_points))]  # Top 3 most congested

        active_flows = sorted(flow_tp)
        if top_congestion:
            top_desc = ", ".join([f"SW{cp['switch']}p{cp['port']}(avg={cp['avg_q_kb']:.0f}KB)"
                                  for cp in top_congestion[:3]])
            print(f"Parse complete: {len(all_queue_data)} switch-port pairs monitored, top bottlenecks: {top_desc}, query receiver-side flows {len(active_flows)}")
        else:
            print(f"Parse complete: {len(all_queue_data)} switch-port pairs, query receiver-side flows {len(active_flows)}")

        print("Generating plots...")
        plt.rcParams["font.family"] = "DejaVu Sans"
        plt.rcParams["axes.unicode_minus"] = False
        colors = ["red", "blue", "green", "orange", "purple", "cyan", "lime", "brown", "pink", "gray",
                  "magenta", "olive", "teal", "navy", "maroon", "coral", "gold", "indigo"]

        fig, axes = plt.subplots(2, 1, figsize=(18, 12), sharex=True)
        fig.suptitle(f"{args.algo_name} (ccMode={args.ccMode}) - Workload Traffic Analysis, load={load}",
                     fontsize=18, fontweight="bold")

        ax2 = axes[0]
        print(f"Plotting receiver throughput for {len(active_flows)} query flows...")
        for idx, key in enumerate(active_flows):
            times = flow_tp[key]["times"]
            rates = flow_tp[key]["rates"]
            n = min(len(times), len(rates))
            if n == 0:
                continue
            ax2.plot(times[:n], rates[:n],
                     color=colors[idx % len(colors)], lw=2,
                     label=f"Query {key[0]}->{key[1]} (dp={key[3]})", alpha=0.85)
        ax2.set_ylabel("Receive Throughput (Gbps)", fontsize=14, fontweight="bold")
        ax2.set_title(f"RX Throughput - query flows ({len(active_flows)} flows)", fontsize=15, fontweight="bold")
        ax2.legend(loc="upper right", fontsize=10, ncol=3, framealpha=0.9)
        ax2.grid(True, alpha=0.3, linestyle="--")
        ax2.set_ylim(0, 110)
        ax2.tick_params(labelsize=11)

        ax3 = axes[1]
        if top_congestion:
            plot_colors = ["purple", "red", "orange", "green", "blue"]
            for i, cp in enumerate(top_congestion):
                color = plot_colors[i % len(plot_colors)]
                label = f"SW{cp['switch']} Port{cp['port']} (avg={cp['avg_q_kb']:.0f}KB)"
                ax3.fill_between(cp["times"], 0, cp["queues"], alpha=0.20, color=color)
                ax3.plot(cp["times"], cp["queues"], color=color, lw=1.5, label=label, alpha=0.85)
            ax3.axhline(y=500, color="blue", ls="--", lw=2, alpha=0.5, label="qRef = 500KB (old)")
            ax3.axhline(y=300, color="red", ls=":", lw=1.5, alpha=0.5, label="qRef = 300KB (current)")
        ax3.set_ylabel("Queue Length (KB)", fontsize=14, fontweight="bold")
        ax3.set_xlabel("Time (ms)", fontsize=14, fontweight="bold")
        ax3.set_title(f"Top {len(top_congestion)} Congestion Points Queue Length (auto-detected)", fontsize=15, fontweight="bold")
        ax3.legend(loc="upper right", fontsize=10, framealpha=0.9)
        ax3.grid(True, alpha=0.3, linestyle="--")
        max_q = max([cp["max_q_kb"] for cp in top_congestion], default=1000)
        ax3.set_ylim(-50, max_q * 1.15)
        ax3.tick_params(labelsize=11)

        plt.tight_layout()
        output_file = os.path.join(PLOTS_DIR, f"workload_{args.algo_name.lower()}_cc{args.ccMode}_load{load_tag(load)}.png")
        plt.savefig(output_file, dpi=150, bbox_inches="tight")
        plt.close(fig)
        print(f"Plot saved: {output_file}\n")

        ip_to_node = parse_ip_to_node_map(lines)
        tx_rates = parse_tx_rate(lines, ip_to_node, query_flow_keys)
        cnp_events = parse_cnp_events(lines, ip_to_node, query_flow_keys)
        # PFC 事件解析与受影响流映射（基于拓扑端口映射 + 最短路径）
        pfc_events = parse_pfc_events(pfc_out)
        topo_for_map = args.topology or read_config_value(content, "TOPOLOGY_FILE", "")
        port_map, topo_adj = parse_topology_adjacency(topo_for_map)
        pfc_by_flow = map_pfc_events_to_flows(pfc_events, port_map, topo_adj, sorted(tx_rates))
        os.makedirs(TXRATE_DIR, exist_ok=True)
        tx_rate_output_file = os.path.join(
            PLOTS_DIR,
            f"workload_txrate_{args.algo_name.lower()}_cc{args.ccMode}_load{load_tag(load)}.png",
        )
        if tx_rates:
            plot_tx_rate(tx_rates, cnp_events, pfc_by_flow, tx_rate_output_file, args.algo_name, args.ccMode, load)
        else:
            print("Warning: no query-flow TX RATE data was parsed; skipping TX RATE plot. "
                  "Check that periodic TX-rate sampling and routing-table audit logs are enabled.")

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
        print(f"Statistics - {args.algo_name}")
        print(f"{'='*80}")
        print(f"Total background flow: {totals['background']}  Total flow: {totals['flow']}  Total Query: {totals['query']}")
        print(f"Discovered info/load lines: {len(workload_lines)}")
        print(f"Active query flow count: {len(active_flows)}")
        print(f"\n{'='*80}")
        print("Congestion Point Detection (auto-identified by avg queue length)")
        print(f"{'='*80}")
        print(f"Total switch-port pairs monitored: {len(all_queue_data)}")
        print(f"Congestion points identified (>= {MIN_SAMPLES} samples): {len(congestion_points)}")
        if congestion_points:
            print(f"\n{'Rank':<6} {'Switch':<8} {'Port':<6} {'Avg Queue':<12} {'Max Queue':<12} {'Samples':<10}")
            print("-" * 70)
            for rank, cp in enumerate(congestion_points[:10], 1):
                marker = "  <-- TOP BOTTLENECK" if rank == 1 else ""
                print(f"  {rank:<4} SW{cp['switch']:<6} {cp['port']:<6} {cp['avg_q_kb']:<10.1f}KB "
                      f"{cp['max_q_kb']:<10.1f}KB {cp['n_samples']:<10}{marker}")
        if fct_ns:
            fct_ms = np.array(fct_ns) / 1e6
            print(f"\nFCT stats: completed flows={len(fct_ms)} avg completion time={np.mean(fct_ms):.6f} ms P99 completion time={np.percentile(fct_ms, 99):.6f} ms")
        else:
            print("\nFCT stats: No completed flow data")
        print(f"Log: {log_file}")
        print(f"FCT: {fct_out}")
        print(f"PFC: {pfc_out}")
        # PFC 触发统计（按链路分组，显示反压方向）
        if os.path.exists(pfc_out) and os.path.getsize(pfc_out) > 0:
            pfc_pause = 0
            pfc_resume = 0
            pfc_link_events = {}
            with open(pfc_out, 'r') as pf:
                for line in pf:
                    parts = line.strip().split()
                    if len(parts) == 5:
                        ptype = int(parts[4])
                        if ptype not in (2, 3):
                            continue  # 0/1 是对端接收通知，跳过
                        if ptype == 2:
                            pfc_pause += 1
                        elif ptype == 3:
                            pfc_resume += 1
                        peer = port_map.get(int(parts[1]), {}).get(int(parts[3]))
                        link_desc = f"SW{parts[1]}p{parts[3]}"
                        if peer is not None:
                            link_desc += f" (backpressure SW{peer}->SW{parts[1]})"
                        pfc_link_events.setdefault(link_desc, {"pause": 0, "resume": 0})
                        pfc_link_events[link_desc]["pause" if ptype == 2 else "resume"] += 1
            print(f"PFC stats: PAUSE={pfc_pause}  RESUME={pfc_resume}")
            for link_desc, cnt in sorted(pfc_link_events.items()):
                print(f"  {link_desc}: PAUSE={cnt['pause']} RESUME={cnt['resume']}")
        else:
            print(f"PFC stats: PAUSE=0  RESUME=0 (no events)")
        # ePFC (ccMode=17) 事件统计：打标 / CCF 生成与边缘建表 / 边缘暂停恢复
        epfc_stats = {}
        for line in lines:
            m2 = re.search(r"\[EPFC_(XOFF|XON|MARK|CCF_GEN|CCF_EDGE)\]", line)
            if m2:
                epfc_stats[m2.group(1)] = epfc_stats.get(m2.group(1), 0) + 1
        if epfc_stats:
            epfc_desc = "  ".join(f"{k}={v}" for k, v in sorted(epfc_stats.items()))
            print(f"ePFC stats: {epfc_desc}")
        print(f"Image file: {output_file}")
        if tx_rates:
            print(f"TX RATE image file: {tx_rate_output_file}")

        if os.path.exists(query_fct_out):
            analyze_query_flow_fct(query_fct_out)

        # 写入分析数据到文件
        os.makedirs(ANALYSIS_DIR, exist_ok=True)
        analysis_file = os.path.join(ANALYSIS_DIR, f"analysis_{args.algo_name.lower()}_cc{args.ccMode}_load{load_tag(load)}.txt")
        with open(analysis_file, 'w', encoding='utf-8') as af:
            af.write(f"{'='*80}\n")
            af.write(f"Statistics - {args.algo_name} (ccMode={args.ccMode})\n")
            af.write(f"{'='*80}\n")
            af.write(f"Total background flow: {totals['background']}  Total flow: {totals['flow']}  Total Query: {totals['query']}\n")
            af.write(f"Active query flow count: {len(active_flows)}\n\n")
            
            # 拥塞点检测
            af.write(f"{'='*80}\n")
            af.write("Congestion Point Detection\n")
            af.write(f"{'='*80}\n")
            af.write(f"Total switch-port pairs monitored: {len(all_queue_data)}\n")
            af.write(f"Congestion points identified: {len(congestion_points)}\n")
            if congestion_points:
                af.write(f"\n{'Rank':<6} {'Switch':<8} {'Port':<6} {'Avg Queue':<12} {'Max Queue':<12} {'Samples':<10}\n")
                af.write("-" * 70 + "\n")
                for rank, cp in enumerate(congestion_points[:10], 1):
                    marker = "  <-- TOP BOTTLENECK" if rank == 1 else ""
                    af.write(f"  {rank:<4} SW{cp['switch']:<6} {cp['port']:<6} {cp['avg_q_kb']:<10.1f}KB "
                              f"{cp['max_q_kb']:<10.1f}KB {cp['n_samples']:<10}{marker}\n")
            
            # FCT 统计
            af.write(f"\n{'='*80}\n")
            af.write("FCT Statistics\n")
            af.write(f"{'='*80}\n")
            if fct_ns:
                fct_ms = np.array(fct_ns) / 1e6
                af.write(f"Completed flows: {len(fct_ms)}\n")
                af.write(f"Average FCT: {np.mean(fct_ms):.6f} ms\n")
                af.write(f"Min FCT: {np.min(fct_ms):.6f} ms\n")
                af.write(f"Max FCT: {np.max(fct_ms):.6f} ms\n")
                af.write(f"P50 FCT: {np.percentile(fct_ms, 50):.6f} ms\n")
                af.write(f"P95 FCT: {np.percentile(fct_ms, 95):.6f} ms\n")
                af.write(f"P99 FCT: {np.percentile(fct_ms, 99):.6f} ms\n")
            else:
                af.write("No completed flow data\n")
            
            # PFC 统计
            af.write(f"\n{'='*80}\n")
            af.write("PFC Statistics\n")
            af.write(f"{'='*80}\n")
            if os.path.exists(pfc_out) and os.path.getsize(pfc_out) > 0:
                pfc_pause = 0
                pfc_resume = 0
                pfc_switches = {}
                with open(pfc_out, 'r') as pf:
                    for line in pf:
                        parts = line.strip().split()
                        if len(parts) == 5:
                            ptype = int(parts[4])
                            if ptype == 2:
                                pfc_pause += 1
                                sw_id = parts[1]
                                pfc_switches[sw_id] = pfc_switches.get(sw_id, 0) + 1
                            elif ptype == 3:
                                pfc_resume += 1
                af.write(f"PAUSE: {pfc_pause}\n")
                af.write(f"RESUME: {pfc_resume}\n")
                if pfc_switches:
                    sw_detail = ", ".join([f"SW{k}({v})" for k, v in sorted(pfc_switches.items(), key=lambda x: -x[1])])
                    af.write(f"PFC by switch: {sw_detail}\n")
            else:
                af.write("PAUSE: 0  RESUME: 0 (no events)\n")
            
            # Query 流分析
            af.write(f"\n{'='*80}\n")
            af.write("Query Flow FCT Analysis\n")
            af.write(f"{'='*80}\n")
            if os.path.exists(query_fct_out):
                query_flows = []
                with open(query_fct_out, "r", encoding="utf-8", errors="replace") as qf:
                    for line in qf:
                        if line.startswith("#"):
                            continue
                        parts = line.strip().split()
                        if len(parts) >= 8:
                            query_flows.append({
                                "src": int(parts[0]),
                                "dst": int(parts[1]),
                                "size": int(parts[4]),
                                "fct_ns": int(parts[6]),
                                "is_cross_dc": int(parts[7]),
                            })
                
                if query_flows:
                    cross_dc_flows = [f for f in query_flows if f["is_cross_dc"] == 1]
                    intra_dc_flows = [f for f in query_flows if f["is_cross_dc"] == 0]
                    
                    af.write(f"Total query flows: {len(query_flows)}\n")
                    af.write(f"Cross-DC flows: {len(cross_dc_flows)}\n")
                    af.write(f"Intra-DC flows: {len(intra_dc_flows)}\n\n")
                    
                    all_fct = np.array([f["fct_ns"] / 1e6 for f in query_flows])
                    af.write(f"Overall FCT: avg={np.mean(all_fct):.3f} ms, min={np.min(all_fct):.3f} ms, max={np.max(all_fct):.3f} ms\n")
                    af.write(f"P50={np.percentile(all_fct, 50):.3f} ms, P95={np.percentile(all_fct, 95):.3f} ms, P99={np.percentile(all_fct, 99):.3f} ms\n\n")
                    
                    if cross_dc_flows:
                        cross_fct = np.array([f["fct_ns"] / 1e6 for f in cross_dc_flows])
                        af.write(f"Cross-DC FCT: avg={np.mean(cross_fct):.3f} ms, min={np.min(cross_fct):.3f} ms, max={np.max(cross_fct):.3f} ms\n")
                    if intra_dc_flows:
                        intra_fct = np.array([f["fct_ns"] / 1e6 for f in intra_dc_flows])
                        af.write(f"Intra-DC FCT: avg={np.mean(intra_fct):.3f} ms, min={np.min(intra_fct):.3f} ms, max={np.max(intra_fct):.3f} ms\n")
                    
                    if cross_dc_flows and intra_dc_flows:
                        ratio = np.mean(cross_fct) / np.mean(intra_fct) if np.mean(intra_fct) > 0 else 0
                        af.write(f"\nCross-DC / Intra-DC FCT ratio: {ratio:.1f}x\n")
                else:
                    af.write("No query flow data\n")
            else:
                af.write("Query flow FCT file not found\n")
            
            af.write(f"\n{'='*80}\n")
        
        print(f"Analysis file: {analysis_file}")
        print(f"{'='*80}\n")
        return output_file
    finally:
        pass


def main():
    parser = argparse.ArgumentParser(
        description="""Workload simulation script.

Algorithm ccMode Mapping:
  dcqcn      -> ccMode=1   (DCQCN)
  hpcc       -> ccMode=3   (HPCC)
  timely     -> ccMode=7   (Timely)
  dctcp      -> ccMode=8   (DCTCP)
  frp        -> ccMode=13  (FRP)
  rocc       -> ccMode=14  (ROCC)
  atc        -> ccMode=15  (ATC)
  dciext     -> ccMode=16  (DCI-Ext: 接收侧 DCI 端口门控 + 逐流 CNP，部署交换机由 GATEWAY_DST 指定)
  epfc       -> ccMode=17  (ePFC: PFC 的流级扩展，所有交换机打标 + 边缘交换机沿 CCF 反馈掐断贡献者流)
  grc        -> ccMode=18  (GRC: DCI 网侧控制，端侧复用现有 DCQCN)

Examples:
  python3 run_single_workload_algo.py 1 DCQCN
  python3 run_single_workload_algo.py 13 FRP
  python3 run_single_workload_algo.py 15 ATC
  python3 run_single_workload_algo.py 16 DCIEXT
  python3 run_single_workload_algo.py 17 EPFC
  python3 run_single_workload_algo.py 18 GRC
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("ccMode", type=int,
                        help="Congestion control algorithm mode (see mapping above)")
    parser.add_argument("algo_name", type=str, help="Algorithm name (e.g., DCQCN, FRP, ROCC)")
    parser.add_argument("--randomSeed", type=int, default=7,
                        help="Random seed (default: 7)")
    parser.add_argument("--timeout", type=int, default=180,
                        help="Timeout in seconds (default: 180 = 3 minutes)")
    parser.add_argument("--config", type=str, default=None,
                        help="Config file path relative to ns-3.39 (default: config-testbed.txt)")
    parser.add_argument("--topology", type=str, default=None,
                        help="Override TOPOLOGY_FILE in config (relative to ns-3.39)")
    parser.add_argument("--query-flow", type=str, default=None,
                        help="Override QUERY_FLOW_FILE in config (relative to ns-3.39)")
    args = parser.parse_args()

    if args.config:
        global CONFIG_FILE
        CONFIG_FILE = args.config

    run_and_plot(args)


if __name__ == "__main__":
    main()
