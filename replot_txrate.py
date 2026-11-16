#!/usr/bin/env python3
"""Re-plot TX RATE from existing dump log (no re-simulation needed)."""
import re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

L = "/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/results/dump/algo_cc16_load0pct.log"
OUT = "/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/results/txrate/workload_txrate_dciext_cc16_load0pct.png"

QUERY_FLOWS = [
    (0, 4, 20031),
    (8, 4, 20032),
    (5, 4, 20031),
    (6, 4, 20032),
    (7, 4, 20033),
]
KEYS = set(QUERY_FLOWS)

ip2node = {}
pat_audit = re.compile(r"OK:\s*Host\s+(\d+)\s+\(IP=(\d+\.\d+\.\d+\.\d+)\)")
pat_tx = re.compile(
    r"\[TX RATE\] Host (\d+) sip (\d+) dip (\d+) sport (\d+) dport (\d+) "
    r"pg \d+ txRate ([\d.]+)bps(?: rt [\d.e+-]+bps)? t (\d+)ns"
)

series = {}
with open(L, errors="replace") as f:
    for line in f:
        m = pat_audit.search(line)
        if m:
            octets = [int(x) for x in m.group(2).split(".")]
            ip_val = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3]
            ip2node[ip_val] = int(m.group(1))
            continue
        m = pat_tx.search(line)
        if not m:
            continue
        host = int(m.group(1))
        src = ip2node.get(int(m.group(2)), host)
        dst = ip2node.get(int(m.group(3)))
        if dst is None:
            continue
        sport, dport = int(m.group(4)), int(m.group(5))
        if (src, dst, dport) not in KEYS:
            continue
        t_ms = int(m.group(7)) / 1e6
        rate_g = float(m.group(6)) / 1e9
        series.setdefault((src, dst, sport, dport), [[], []])
        series[(src, dst, sport, dport)][0].append(t_ms)
        series[(src, dst, sport, dport)][1].append(rate_g)

colors = ["red", "blue", "green", "orange", "purple"]
fig, ax = plt.subplots(figsize=(18, 7))
fig.suptitle("DCIEXT (ccMode=16) - TX Rate (Send Side), load=0", fontsize=18, fontweight="bold")
for idx, k in enumerate(sorted(series)):
    times, rates = series[k]
    order = sorted(range(len(times)), key=lambda i: times[i])
    ax.plot([times[i] for i in order], [rates[i] for i in order],
            color=colors[idx % len(colors)], lw=2,
            label=f"TX {k[0]}->{k[1]} (sp={k[2]}, dp={k[3]})", alpha=0.85)
    print(f"flow {k}: {len(times)} pts, {times[0]:.1f}~{times[-1]:.1f} ms")
ax.set_xlabel("Time (ms)", fontsize=14, fontweight="bold")
ax.set_ylabel("TX Rate (Gbps)", fontsize=14, fontweight="bold")
ax.set_title(f"TX Rate - query flows ({len(series)} flows, sample=100us)", fontsize=15, fontweight="bold")
ax.legend(loc="upper right", fontsize=10, ncol=3, framealpha=0.9)
ax.grid(True, alpha=0.3, linestyle="--")
ax.set_ylim(bottom=0)
ax.tick_params(labelsize=11)
fig.tight_layout()
fig.savefig(OUT, dpi=150, bbox_inches="tight")
plt.close(fig)
print(f"saved: {OUT}, flows: {len(series)}")
