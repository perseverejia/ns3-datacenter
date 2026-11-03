import re
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

LOG = '/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/results/dump/algo_cc16_load0pct.log'
ts_l, avg_l, min_l, max_l, ra_l, ack_l, br_l = [], [], [], [], [], [], []
for line in open(LOG, errors='ignore'):
    m = re.search(r'DCIEXT_STATS\] (\d+) ', line)
    if not m:
        continue
    d = dict(re.findall(r'(\w+)=([-\d.]+)', line))
    rtt = float(d.get('rtt', -1))
    ts = int(m.group(1)) / 1e6
    ts_l.append(ts)
    avg_l.append(max(rtt, 0))
    min_l.append(max(float(d.get('min', 0)), 0))
    max_l.append(float(d.get('max', 0)))
    ra_l.append(float(d.get('R_avbl', 0)))
    ack_l.append(float(d.get('ackRate', 0)))
    br_l.append(re.search(r'branch=(\w+)', line).group(1))

print('ticks=%d span %.1f~%.1fms' % (len(ts_l), ts_l[0], ts_l[-1]))
# 关键转折
prev = None
for i, (t, ra, b) in enumerate(zip(ts_l, ra_l, br_l)):
    if prev is not None and abs(ra - prev) > 0.01:
        print('%8.1fms R_avbl %.3f -> %.3f G  branch=%s rtt=%.1f' % (t, prev, ra, b, avg_l[i]))
    prev = ra

fig, ax1 = plt.subplots(figsize=(13, 6))
ax1.plot(ts_l, avg_l, '-', color='tab:blue', lw=1.2, label='RTT avg (T=500us)')
ax1.plot(ts_l, max_l, '-', color='tab:red', lw=0.8, alpha=0.5, label='RTT max')
ax1.axhline(23.8, color='tab:green', ls='--', lw=1, alpha=0.7)
ax1.text(5, 25.5, 'RTT floor 23.8us', color='tab:green', fontsize=9)
ax1.axhspan(24, 36, color='orange', alpha=0.15)
ax1.axhline(30, color='orange', ls='--', lw=1)
ax1.text(5, 31.5, 'rttT=30us (base24+Dt6), band 24~36us', color='darkorange', fontsize=9)
ax1.set_xlabel('time (ms)')
ax1.set_ylabel('RTT (us)')
ax1.set_ylim(0, 400)
ax1.legend(loc='upper right', fontsize=9)
ax1.grid(alpha=0.3)
ax2 = ax1.twinx()
ax2.plot(ts_l, ra_l, color='purple', lw=1.4, label='R_avbl')
ax2.plot(ts_l, ack_l, color='gray', lw=0.7, alpha=0.5)
ax2.set_ylabel('R_avbl / ackRate (Gbps)', color='purple')
ax2.set_ylim(0, 45)
ax2.legend(loc='lower right', fontsize=9)
plt.title('base=24us + switch NicDelay=0: RTT & R_avbl (stable, no deadlock)')
plt.tight_layout()
plt.savefig('/home/gj/ns3/results/rtt_base24_nodelay.png', dpi=110)
print('saved png')
