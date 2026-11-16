import re, collections
L = "/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/results/dump/algo_cc16_load0pct.log"
p_ql = re.compile(r"\[DCQCN_QLEN\] (\d+) (\d+) (\d+) (\d+) mmu_eg=(-?\d+)")
p_cnp7 = re.compile(r"\[DCQCN_CNP_CHECK\] ([\d.]+)ms node=7 cnp=(\d+)")
p_cnp6 = re.compile(r"\[DCQCN_CNP_CHECK\] ([\d.]+)ms node=6 cnp=(\d+)")
qlen = collections.defaultdict(list)   # (t0_100us, sw, port) -> qlen
cnp7, cnp6 = [], []
with open(L, errors="replace") as f:
    for line in f:
        m = p_ql.search(line)
        if m:
            t_ns, sw, port, ql = int(m.group(1)), m.group(2), m.group(3), int(m.group(4))
            if t_ns <= 10_000_000:
                qlen[(t_ns // 100_000, sw, port)].append(ql)
            continue
        m = p_cnp7.search(line)
        if m:
            cnp7.append(float(m.group(1))); continue
        m = p_cnp6.search(line)
        if m:
            cnp6.append(float(m.group(1))); continue

print("=== 0~10ms queue len (KB), sampled every 100us, showing MAX in bucket ===")
sws = sorted(set((k[1], k[2]) for k in qlen))
print(f"{'t(ms)':<7}" + "".join(f"SW{s}p{p:<8}" for s, p in sws))
for t0 in range(0, 100):
    row = []
    for sw, port in sws:
        v = qlen.get((t0, sw, port))
        row.append(f"{max(v)/1024:<10.0f}" if v else f"{'--':<10}")
    print(f"{(t0+1)/10:<7.1f}" + "".join(row))

print(f"\nCNPs to H6: {len(cnp6)}, first/last in 0-10ms:", [t for t in cnp6 if t <= 10][:40])
print(f"CNPs to H7: {len(cnp7)}, in 0-10ms:", [t for t in cnp7 if t <= 10][:40])
