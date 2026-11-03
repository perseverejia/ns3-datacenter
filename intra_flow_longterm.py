#!/usr/bin/env python3
"""域内流(Host6/7)全时段速率轨迹粗采样 + 流完成时间"""
import re
from collections import defaultdict

LOG = "/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/results/dump/algo_cc16_load0pct.log"

tx_re = re.compile(r"\[TX RATE\] Host (\d+) .* txRate (\d+)bps t (\d+)ns")
rates = defaultdict(dict)
with open(LOG) as f:
    for ln in f:
        m = tx_re.search(ln)
        if m:
            h, bps, tns = int(m.group(1)), int(m.group(2)), int(m.group(3))
            rates[h][tns // 1000] = bps / 1e9

# 每 2ms 采样
print("=== 速率轨迹(每2ms采样, Gbps) ===")
print(f"{'t(ms)':>7} {'H6域内':>8} {'H7域内':>8} {'H0跨域':>8} {'H8跨域':>8}")
tmax = max(max(rates[h]) for h in rates)
t = 2000
while t <= min(tmax, 150000):
    row = [rates[h].get(t, float("nan")) for h in (6, 7, 0, 8)]
    print(f"{t/1000:>7.1f} " + " ".join(f"{v:>8.3f}" if v == v else f"{'--':>8}" for v in row))
    t += 2000

# 最后的非零速率时间
print("\n=== 各流最后有效打点 ===")
for h in (6, 7, 0, 8):
    nz = [t for t, v in rates[h].items() if v > 0.5]
    if nz:
        print(f"  Host{h}: 最后速率>0.5G 的打点 t={max(nz)/1000:.2f}ms, "
              f"峰值={max(rates[h].values()):.3f}G")
    else:
        print(f"  Host{h}: 无 >0.5G 打点")
