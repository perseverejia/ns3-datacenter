import re

LOG = '/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/results/dump/algo_cc16_load0pct.log'
rows = []
for line in open(LOG, errors='ignore'):
    m = re.search(r'DCIEXT_STATS\] (\d+) ', line)
    if not m:
        continue
    d = dict(re.findall(r'(\w+)=([-\d.]+)', line))
    b = re.search(r'branch=(\w+)', line).group(1)
    rows.append(dict(ts=int(m.group(1)) / 1e6, rtt=float(d.get('rtt', -1)),
                     tx=float(d.get('txRate', 0)), ack=float(d.get('ackRate', 0)),
                     ra=float(d.get('R_avbl', 0)), n=int(float(d.get('N', 0))), b=b))

# 参数（config-workload.txt / dci-ext.cc）
BASE, DT, SIGMA, T_us, B = 24.0, 6.0, 6.0, 500.0, 40e9
tSec = T_us * 1e-6

print('%-8s %-6s | %-7s %-7s %-7s | %-9s %-10s %-10s | %-10s %-10s | %-9s' % (
    'time', 'br', 'rtt', 'txRate', 'ackRate', 'D(us)', 'follow(G)', 'active(G)', 'rNew(G)', 'R_avbl', 'prev'))
print('-' * 118)
prev_ra = None
for i, r in enumerate(rows):
    if r['b'] != 'PROBE':
        prev_ra = r['ra']
        continue
    rtt = r['rtt']
    dUs = max(rtt - BASE, 1e-3)
    Ds, dtS, RTTs, rttTs = dUs * 1e-6, DT * 1e-6, rtt * 1e-6, (BASE + DT) * 1e-6
    W = r['tx'] * 1e9 * tSec / 8.0          # 上个T窗口出队字节
    Wt = (1.0 / Ds - 1.0 / dtS + W / RTTs) * rttTs
    rNew = Wt / tSec * 8.0
    rNew_c = min(max(rNew, B / 64.0), B)
    follow = (W / RTTs) * rttTs / tSec * 8.0   # 跟随项 bps = txRate*rttT/RTT
    active = (1.0 / Ds - 1.0 / dtS) * rttTs / tSec * 8.0  # 主动项 bps
    clamp = ' CLAMP->B' if rNew > B else (' CLAMP->B/64' if rNew < B / 64.0 else '')
    print('%-8.1f %-6s | %-7.1f %-7.3f %-7.3f | %-9.4f %-10.3f %-10.3f | %-10.3f %-10.3f | %-9.3f%s' % (
        r['ts'], 'PROBE', rtt, r['tx'], r['ack'], dUs, follow / 1e9, active / 1e9,
        rNew_c / 1e9, r['ra'], prev_ra if prev_ra is not None else -1, clamp))
    prev_ra = r['ra']

# 早期第一次 PROBE（25.6->35.92）上下文：打印 5.5~18.5ms 全部 tick 摘要
print()
print('=== 早期 tick 摘要（5.5~18.5ms，含第一次跳变前后的 branch）===')
for r in rows[:40]:
    print('%6.1fms %-11s rtt=%7.1f tx=%7.3fG ack=%7.3fG R_avbl=%7.3fG' % (
        r['ts'], r['b'], r['rtt'], r['tx'], r['ack'], r['ra']))
