import re
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

LOG = '/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/results/dump/algo_cc16_load0pct.log'
ts_l, avg_l, min_l, max_l, ack_l = [], [], [], [], []
for line in open(LOG, errors='ignore'):
    m = re.search(r'DCIEXT_STATS\] (\d+) ', line)
    if not m:
        continue
    d = dict(re.findall(r'(\w+)=([-\d.]+)', line))
    rtt = float(d.get('rtt', -1))
    if rtt < 0:
        continue  # 无样本 tick（末尾 3 个）
    ts_l.append(int(m.group(1)) / 1e6)
    avg_l.append(rtt)
    min_l.append(float(d.get('min', 0)))
    max_l.append(float(d.get('max', 0)))
    ack_l.append(float(d.get('ackRate', 0)))

fig, ax1 = plt.subplots(figsize=(13, 6))
ax1.plot(ts_l, avg_l, '-', color='tab:blue', lw=1.2, label='RTT avg (per T=500us)')
ax1.plot(ts_l, max_l, '-', color='tab:red', lw=0.8, alpha=0.6, label='RTT max')
ax1.plot(ts_l, min_l, '-', color='tab:green', lw=0.8, alpha=0.8, label='RTT min')
ax1.axhline(98.6, color='tab:green', ls='--', lw=1, alpha=0.7)
ax1.text(120, 100.5, 'floor 98.6us', color='tab:green', fontsize=9)
ax1.axhspan(30, 42, color='orange', alpha=0.15)
ax1.axhline(36, color='orange', ls='--', lw=1)
ax1.text(120, 37.5, 'rttT=36us (base30+Dt6), target band 30~42us', color='darkorange', fontsize=9)
ax1.set_xlabel('time (ms)')
ax1.set_ylabel('RTT (us)')
ax1.set_ylim(0, 350)
ax1.legend(loc='upper right', fontsize=9)
ax1.grid(alpha=0.3)

ax2 = ax1.twinx()
ax2.plot(ts_l, ack_l, color='gray', lw=0.7, alpha=0.5)
ax2.set_ylabel('ackRate (Gbps)', color='gray')
ax2.tick_params(axis='y', colors='gray')
ax2.set_ylim(0, 50)

plt.title('Receiver SW22 per-T RTT evolution (RTT_base=30us fixed run)')
plt.tight_layout()
plt.savefig('/home/gj/ns3/results/rtt_base30_evolution.png', dpi=110)
print('saved. ticks=%d, span %.1f~%.1fms' % (len(ts_l), ts_l[0], ts_l[-1]))

# 10ms 细分段表
print()
print('%-8s %5s %9s %9s %9s' % ('ms_seg', 'cnt', 'avg_rtt', 'min', 'max'))
segs = {}
for t, a, mn, mx in zip(ts_l, avg_l, min_l, max_l):
    segs.setdefault(int(t // 10) * 10, []).append((a, mn, mx))
for k in sorted(segs):
    g = segs[k]
    n = len(g)
    print('%-8d %5d %9.1f %9.1f %9.1f' % (k, n, sum(x[0] for x in g)/n,
                                          min(x[1] for x in g), max(x[2] for x in g)))
