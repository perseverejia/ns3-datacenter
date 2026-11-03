#!/usr/bin/env python3
"""提取域内流(Host6/7)与跨域流(Host0/8)在 0~8ms 的逐流速率演化 + DCQCN 事件"""
import re
from collections import defaultdict

LOG = "/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/results/dump/algo_cc16_load0pct.log"
CUT_US = 8000  # 只看前 8ms

rates = defaultdict(dict)
cnp = []
events = []

tx_re = re.compile(r"\[TX RATE\] Host (\d+) .* txRate (\d+)bps t (\d+)ns")
cnp_re = re.compile(r"\[DCQCN_CNP\] ([\d.]+)ms node=(\d+) .* rate=([\d.]+)G")
evt_re = re.compile(
    r"^(\d+) (rate dec|fast recovery|active inc|hyper inc): (\w+) (\w+) (\d+) (\d+) "
    r"\(([\d.]+) ([\d.]+)\)->\(([\d.]+) ([\d.]+)\)")

with open(LOG) as f:
    for ln in f:
        m = tx_re.search(ln)
        if m:
            h, bps, tns = int(m.group(1)), int(m.group(2)), int(m.group(3))
            t_us = tns // 1000
            if t_us <= CUT_US:
                rates[t_us][h] = bps / 1e9
            continue
        m = cnp_re.search(ln)
        if m:
            cnp.append((float(m.group(1)), int(m.group(2)), float(m.group(3))))
            continue
        m = evt_re.match(ln)
        if m:
            events.append((int(m.group(1)) // 1000, m.group(2),
                           m.group(3), m.group(4), m.group(5),
                           float(m.group(7)), float(m.group(8)),
                           float(m.group(9)), float(m.group(10))))

ts = sorted(rates)
print("=== [TX RATE] 轨迹 (Gbps) ===")
print(f"{'t(us)':>7} {'Host6(域内)':>12} {'Host7(域内)':>12} {'Host0(跨域)':>12} {'Host8(跨域)':>12}  域内合计")
for t in ts:
    d = rates[t]
    h6 = d.get(6, float("nan")); h7 = d.get(7, float("nan"))
    h0 = d.get(0, float("nan")); h8 = d.get(8, float("nan"))
    s = (h6 if h6 == h6 else 0) + (h7 if h7 == h7 else 0)
    print(f"{t:>7} {h6:>12.3f} {h7:>12.3f} {h0:>12.3f} {h8:>12.3f}  {s:.3f}G")

print("\n=== DCQCN CNP 事件计数 (0~8ms) ===")
early = [c for c in cnp if c[0] * 1000 <= CUT_US]
print(f"  共 {len(early)} 个, 最后一个: t={max(c[0] for c in early):.3f}ms" if early else "  无")

print("\n=== DCQCN 事件时间线 (0~8ms) ===")
for t_us, kind, sip, dip, sport, t0, r0, t1, r1 in sorted(events):
    if t_us <= CUT_US:
        print(f"  t={t_us/1000:>7.3f}ms {kind:<13} sport={sport} tgt {t0:.3f}->{t1:.3f}  rate {r0:.3f}->{r1:.3f}")
