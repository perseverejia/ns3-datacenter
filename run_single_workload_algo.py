#!/usr/bin/env python3
"""
crossDC workload algorithm runner script.
Workload parameters live in simulator/ns-3.39/examples/PowerTCP/config-workload.txt.
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


REPO_ROOT = os.path.dirname(os.path.abspath(__file__))
NS3_DIR = os.path.join(REPO_ROOT, "simulator/ns-3.39")
CONFIG_FILE = "examples/PowerTCP/config-workload.txt"
DUMP_DIR = os.path.join(REPO_ROOT, "dump/workload")
RESULTS_DIR = os.path.join(REPO_ROOT, "results/workload")
FCT_DIR = os.path.join(RESULTS_DIR, "fct")
PFC_DIR = os.path.join(RESULTS_DIR, "pfc")
TXRATE_DIR = os.path.join(RESULTS_DIR, "txrate")
ATC_ALGO_MODE = 15
ATC_BASE_CC_MODE = 1


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
    return os.path.join(REPO_ROOT, path)


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
        r"pg (\d+) txRate ([\d.]+)bps t (\d+)ns"
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


def plot_tx_rate(tx_rates, output_path, algo_name, cc_mode, load):
    """Save a separate send-side TX-rate plot for query flows."""
    plt.rcParams["font.family"] = "DejaVu Sans"
    plt.rcParams["axes.unicode_minus"] = False
    colors = ["red", "blue", "green", "orange", "purple", "cyan", "lime", "brown",
              "pink", "gray", "magenta", "olive", "teal", "navy", "maroon", "coral",
              "gold", "indigo"]

    fig, ax = plt.subplots(figsize=(18, 7))
    fig.suptitle(f"{algo_name} (ccMode={cc_mode}) - TX Rate (Send Side), load={load}",
                 fontsize=18, fontweight="bold")
    active_flows = sorted(tx_rates)
    for idx, key in enumerate(active_flows):
        data = tx_rates[key]
        ax.plot(data["times"], data["rates"], color=colors[idx % len(colors)], lw=2,
                label=f"TX {key[0]}->{key[1]} (sp={key[2]}, dp={key[3]})", alpha=0.85)
    ax.set_xlabel("Time (ms)", fontsize=14, fontweight="bold")
    ax.set_ylabel("TX Rate (Gbps)", fontsize=14, fontweight="bold")
    ax.set_title(f"TX Rate - query flows ({len(active_flows)} flows, sample=100us)",
                 fontsize=15, fontweight="bold")
    ax.legend(loc="upper right", fontsize=10, ncol=3, framealpha=0.9)
    ax.grid(True, alpha=0.3, linestyle="--")
    ax.set_ylim(bottom=0)
    ax.tick_params(labelsize=11)
    fig.tight_layout()
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
    sim_cc_mode = ATC_BASE_CC_MODE if args.ccMode == ATC_ALGO_MODE else args.ccMode
    gateway_type = 2 if args.ccMode == ATC_ALGO_MODE else 0

    print(f"\n{'='*80}")
    print(f"Simulation: {args.algo_name} (ccMode={args.ccMode})")
    if args.ccMode == ATC_ALGO_MODE:
        print(f"ATC mode: simulator ccMode={sim_cc_mode} (DCQCN), gatewayType={gateway_type}")
    print(f"{'='*80}\n")

    os.makedirs(DUMP_DIR, exist_ok=True)
    os.makedirs(RESULTS_DIR, exist_ok=True)
    os.makedirs(FCT_DIR, exist_ok=True)
    os.makedirs(PFC_DIR, exist_ok=True)

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

        binary = os.path.join(NS3_DIR, "build/examples/PowerTCP/ns3.39-crossDC-evaluation-workload-optimized")
        cmd = [
            binary,
            f"--conf={config}",
            f"--algorithm={sim_cc_mode}",
            f"--gatewayType={gateway_type}",
            f"--randomSeed={args.randomSeed}",
            f"--fctOutputFile={fct_out}",
            f"--pfcOutputFile={pfc_out}",
            f"--queryFlowFctFile={query_fct_out}",
        ]

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

            if "[DCQCN_QLEN]" in line and sim_cc_mode in (1, 3, 7):
                parts = line.split()
                if len(parts) >= 5:
                    sw = int(parts[2])
                    port = int(parts[3])
                    t_ms = float(parts[1]) / 1e9 * 1000
                    q_kb = float(parts[4]) / 1024.0
                    all_queue_data.setdefault((sw, port), {"times": [], "queues": []})
                    all_queue_data[(sw, port)]["times"].append(t_ms)
                    all_queue_data[(sw, port)]["queues"].append(q_kb)

            if "[FRP_DATA_SW]" in line and sim_cc_mode in (13, 14):
                parts = line.split()
                if len(parts) >= 8 and int(parts[7]) == sim_cc_mode:
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
        top_congestion = congestion_points[:min(5, len(congestion_points))]

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
        output_file = os.path.join(RESULTS_DIR, f"workload_{args.algo_name.lower()}_cc{args.ccMode}_load{load_tag(load)}.png")
        plt.savefig(output_file, dpi=150, bbox_inches="tight")
        plt.close(fig)
        print(f"Plot saved: {output_file}\n")

        ip_to_node = parse_ip_to_node_map(lines)
        tx_rates = parse_tx_rate(lines, ip_to_node, query_flow_keys)
        os.makedirs(TXRATE_DIR, exist_ok=True)
        tx_rate_output_file = os.path.join(
            TXRATE_DIR,
            f"workload_txrate_{args.algo_name.lower()}_cc{args.ccMode}_load{load_tag(load)}.png",
        )
        if tx_rates:
            plot_tx_rate(tx_rates, tx_rate_output_file, args.algo_name, args.ccMode, load)
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
        # PFC 触发统计
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
            print(f"PFC stats: PAUSE={pfc_pause}  RESUME={pfc_resume}")
            if pfc_switches:
                sw_detail = ", ".join([f"SW{k}({v})" for k, v in sorted(pfc_switches.items(), key=lambda x: -x[1])])
                print(f"  PFC by switch: {sw_detail}")
        else:
            print(f"PFC stats: PAUSE=0  RESUME=0 (no events)")
        print(f"Image file: {output_file}")
        if tx_rates:
            print(f"TX RATE image file: {tx_rate_output_file}")

        if os.path.exists(query_fct_out):
            analyze_query_flow_fct(query_fct_out)

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

Examples:
  python3 run_single_workload_algo.py 1 DCQCN
  python3 run_single_workload_algo.py 13 FRP
  python3 run_single_workload_algo.py 15 ATC
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
    args = parser.parse_args()

    run_and_plot(args)


if __name__ == "__main__":
    main()
