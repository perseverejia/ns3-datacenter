import re, collections
L = "/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/results/dump/algo_cc16_load0pct.log"
BUCKET = 500  # us
ec = collections.defaultdict(lambda: [0, 0.0])   # (t0,node,if) -> [count, max_qlen]
cnp = collections.Counter()                      # t0 -> count (host 6/7)
pfc = collections.Counter()                      # t0 -> count
qlen = collections.defaultdict(list)             # (t0,sw,port) -> [qlen]
dci = collections.Counter()                      # t0 -> count (DCIEXT_CNP all types)
ecn_ecn = collections.defaultdict(collections.Counter)  # t0 -> {host: cnt}

p_ec = re.compile(r"\[ECN_MARK\] (\d+) node=(\d+) if=(\d+) q=(\d+) qlen=(\d+) sip=0x([0-9a-f]+) dip=0x([0-9a-f]+)")
p_cnp = re.compile(r"\[DCQCN_CNP_CHECK\] ([\d.]+)ms node=(\d+) cnp=(\d+)")
p_pfc = re.compile(r"\[PFC_TRIGGER\] (\d+) node=(\d+) if=(\d+) type=(\d+)")
p_ql = re.compile(r"\[DCQCN_QLEN\] (\d+) (\d+) (\d+) (\d+) mmu_eg=(-?\d+)")
p_dci = re.compile(r"\[DCIEXT_CNP\] (\d+) node=(\d+) type=(\w+)")
p_e2 = re.compile(r"\[DCQCN_ECN\] ([\d.]+)ms node=(\d+) ecn=(\d+)")

with open(L, errors="replace") as f:
    for line in f:
        m = p_ec.search(line)
        if m:
            t_ns, node, ifp, q, qlen_b = int(m.group(1)), m.group(2), m.group(3), m.group(4), int(m.group(5))
            if t_ns <= 10_000_000:
                k = (t_ns // (BUCKET*1000), node, ifp)
                ec[k][0] += 1
                ec[k][1] = max(ec[k][1], qlen_b)
            continue
        m = p_cnp.search(line)
        if m:
            t_ms, node = float(m.group(1)), m.group(2)
            if t_ms*1000 <= 10000:
                cnp[(int(t_ms*2), node)] += 1
            continue
        m = p_pfc.search(line)
        if m:
            t_ns = int(m.group(1))
            if t_ns <= 10_000_000:
                pfc[t_ns // (BUCKET*1000)] += 1
            continue
        m = p_ql.search(line)
        if m:
            t_ns, sw, port, ql = int(m.group(1)), m.group(2), m.group(3), int(m.group(4))
            if t_ns <= 10_000_000:
                qlen[(t_ns // (BUCKET*1000), sw, port)].append(ql)
            continue
        m = p_dci.search(line)
        if m:
            t_ns = int(m.group(1))
            if t_ns <= 10_000_000:
                dci[(t_ns // (BUCKET*1000), m.group(3))] += 1
            continue
        m = p_e2.search(line)
        if m:
            t_ms, node = float(m.group(1)), m.group(2)
            if t_ms*1000 <= 10000:
                ecn_ecn[int(t_ms*2)][node] += 1

print(f"{'t(ms)':<8}{'ECN_MARK(node,if)':<40}{'CNP->H6/H7':<14}{'PFC':<6}{'qlen SW19/14 KB':<30}{'DCIEXT_CNP':<20}")
for t0 in range(0, 21):
    t_start, t_end = t0*0.5, (t0+1)*0.5
    ecs = {f"n{k[1]}if{k[2]}": (v[0], v[1]/1024) for k, v in ec.items() if k[0] == t0}
    ec_str = " ".join(f"{k}:{v[0]}(max{v[1]:.0f}K)" for k, v in sorted(ecs.items()))
    c6 = cnp.get((t0, "6"), 0); c7 = cnp.get((t0, "7"), 0)
    p = pfc.get(t0, 0)
    qs = []
    for (k, sw, port), v in sorted(qlen.items()):
        if k == t0 and port in ("1","2","3") and sw in ("14","19","22"):
            qs.append(f"SW{sw}p{port}:{max(v)/1024:.0f}K")
    d = " ".join(f"{k}:{v}" for k, v in sorted(dci.items()) if k[0] == t0)
    print(f"{t_end:<8.1f}{ec_str[:38]:<40}{'H6='+str(c6)+' H7='+str(c7):<14}{p:<6}{' '.join(qs)[:28]:<30}{d[:18]}")
print()
print("DCQCN_ECN (end host) per 0.5ms:")
for t0 in range(0, 21):
    row = ecn_ecn.get(t0, {})
    if row:
        print(f"  {(t0+1)*0.5:5.1f}ms  " + "  ".join(f"H{k}={v}" for k, v in sorted(row.items())))
