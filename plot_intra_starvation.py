#!/usr/bin/env python3
"""域内流前几个周期的速率演化图：0~12ms 细节 + 全程"""
import re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.font_manager import FontProperties
from collections import defaultdict

ZH = FontProperties(fname="/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc")

LOG = "/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/results/dump/algo_cc16_load0pct.log"

tx_re = re.compile(r"\[TX RATE\] Host (\d+) .* txRate (\d+)bps t (\d+)ns")
cnp_re = re.compile(r"\[DCQCN_CNP\] ([\d.]+)ms node=(\d+)")
rates = defaultdict(dict)
cnps = []
with open(LOG) as f:
    for ln in f:
        m = tx_re.search(ln)
        if m:
            h, bps, tns = int(m.group(1)), int(m.group(2)), int(m.group(3))
            rates[h][tns / 1e6] = bps / 1e9
            continue
        m = cnp_re.search(ln)
        if m:
            cnps.append((float(m.group(1)), int(m.group(2))))

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 8))

style = {6: ("tab:red", "Host6 (域内流1)"), 7: ("tab:orange", "Host7 (域内流2)"),
         0: ("tab:blue", "Host0 (跨域流1)"), 8: ("tab:cyan", "Host8 (跨域流2)")}

# --- 上图：0~12ms 细节 ---
for h in (6, 7, 0, 8):
    ts = sorted(t for t in rates[h] if t <= 12)
    vs = [rates[h][t] for t in ts]
    c, lbl = style[h]
    ax1.plot(ts, vs, color=c, lw=1.6, label=lbl)
c0 = [c[0] for c in cnps if c[1] in (6, 7)]
if c0:
    ax1.axvspan(min(c0), max(c0), color="red", alpha=0.12,
                label="域内CNP风暴 [%.2f,%.2f]ms" % (min(c0), max(c0)))
ax1.axvline(5.0, color="gray", ls="--", lw=1)
ax1.text(5.1, 22, "跨域数据到达接收方 (5ms)", fontsize=9, color="gray", fontproperties=ZH)
ax1.axhline(20, color="green", ls=":", lw=1)
ax1.text(0.1, 21, "2流公平值 20G", fontsize=9, color="green", fontproperties=ZH)
ax1.set_yscale("log")
ax1.set_ylim(0.05, 60)
ax1.set_xlim(0, 12)
ax1.set_xlabel("时间 (ms)", fontproperties=ZH)
ax1.set_ylabel("发送速率 (Gbps, log)", fontproperties=ZH)
ax1.set_title("域内流前几个周期：CNP 风暴过冲 → MIN_RATE 地板死锁（未回到公平值）", fontproperties=ZH)
ax1.legend(fontsize=9, loc="upper right", prop=ZH)
ax1.grid(alpha=0.3, which="both")

# --- 下图：全程 0~330ms ---
for h in (6, 7, 0, 8):
    ts = sorted(rates[h])
    vs = [rates[h][t] for t in ts]
    c, lbl = style[h]
    ax2.plot(ts, vs, color=c, lw=1.2, label=lbl)
ax2.axhline(20, color="green", ls=":", lw=1)
ax2.text(150, 21, "公平值 20G", fontsize=9, color="green", fontproperties=ZH)
ax2.set_xlim(0, 330)
ax2.set_xlabel("时间 (ms)", fontproperties=ZH)
ax2.set_ylabel("发送速率 (Gbps)", fontproperties=ZH)
ax2.set_title("全程视角：域内流长期饥饿在 ~0.6-0.8G，跨域流 ~19.4G 到 128ms 完成；域内流拖到 ~320ms", fontproperties=ZH)
ax2.legend(fontsize=9, prop=ZH)
ax2.grid(alpha=0.3)

plt.tight_layout()
out = "/home/gj/ns3/results/intra_dcqcn_starvation.png"
plt.savefig(out, dpi=130)
print("saved:", out)
