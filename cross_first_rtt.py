#!/usr/bin/env python3
"""跨域流第一个 RTT (~5ms) 的速率演化: 线速burst -> 学费CNP -> 静默 -> 降速"""
import re
from collections import defaultdict

LOG = "/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/results/dump/algo_cc16_load0pct.log"
CUT_US = 15000  # 0~15ms

tx_re = re.compile(r"\[TX RATE\] Host (\d+) .* sport (\d+) dport (\d+) pg (\d+) txRate (\d+)bps t (\d+)ns")
rates = defaultdict(dict)   # t_us -> {host: rate_G}

with open(LOG) as f:
    for ln in f:
        m = tx_re.search(ln)
        if m:
            h, sp, dp, pg, bps, tns = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6))
            t_us = tns // 1000
            if t_us <= CUT_US:
                rates[t_us][h] = bps / 1e9

print(f"{'t(us)':>7} {'H0跨域':>9} {'H8跨域':>9} {'H6域内':>9} {'H7域内':>9}")
for t in sorted(rates):
    d = rates[t]
    row = [d.get(h) for h in (0, 8, 6, 7)]
    cells = " ".join(f"{v:>9.3f}" if v is not None else f"{'--':>9}" for v in row)
    print(f"{t:>7} {cells}")
