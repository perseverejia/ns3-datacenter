import re
L = "/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/results/dump/algo_cc16_load0pct.log"
p = re.compile(r"\[TX RATE\] Host (\d+) sip (\d+) dip (\d+) sport (\d+) dport (\d+) pg \d+ txRate ([\d.]+)bps rt ([\d.]+)bps t (\d+)ns")
data = {}  # host -> list of (t_us, txG, rtG)
with open(L, errors="replace") as f:
    for line in f:
        m = p.search(line)
        if m:
            host = int(m.group(1))
            t_us = int(m.group(8)) / 1000.0
            if t_us <= 15000:
                data.setdefault(host, []).append((t_us, float(m.group(6))/1e9, float(m.group(7))/1e9))
for host in sorted(data):
    rows = sorted(data[host])
    print(f"===== Host {host} ({len(rows)} samples) =====")
    print(f"{'t(ms)':<8}{'txRate(G)':<12}{'Rt(G)':<10}")
    last = None
    for t, tx, rt in rows:
        # 降采样：每 0.2ms 打一行，或 Rt/tx 变化超过 0.3G 时打
        if last is None or t - last >= 0.2 or True:
            pass
        last = t
    # 直接输出 0~15ms 每 0.2ms
    step = 0.2
    idx = 0
    for tick in range(0, int(15/step)):
        t_end = (tick+1)*step*1000
        win = [r for r in rows if t_end - step*1000 <= r[0] < t_end]
        if win:
            t, tx, rt = win[-1]
            print(f"{t_end/1000:<8.1f}{tx:<12.2f}{rt:<10.2f}")
