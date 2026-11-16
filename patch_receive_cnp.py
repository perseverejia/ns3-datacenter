#!/usr/bin/env python3
"""接通 DCI-Ext 端侧刀链: ReceiveCnp 的 ccMode=16 分支接入 cnp_received_mlx"""

P = "/home/gj/ns3/simulator/ns-3.39/src/point-to-point/model/rdma-hw.cc"
L = open(P).readlines()

# 定位 ReceiveCnp 里 lazy init 块结束后的 return 0;
start = None
for i, l in enumerate(L):
    if "if (qp->m_rate == 0)" in l and "lazy initialization" in l:
        start = i
        break
assert start is not None, "lazy init anchor not found"

ret = None
for i in range(start, start + 30):
    if L[i].rstrip() == "\treturn 0;":
        ret = i
        break
assert ret is not None, "return 0; after lazy init not found"
assert "}" in L[ret - 1], f"unexpected: {L[ret-1]!r}"

ins = [
    "\tif (m_cc_mode == 16) {\n",
    "\t\t// DCI-Ext: 发送方 DCI 的 CNP（学费/FIRST_CTL/HOLD/FAST）接入端侧\n",
    "\t\t// DCQCN 状态机——v3 重构遗漏的端侧消费路径；首次 CNP 触发 alpha=1\n",
    "\t\t// 初始化 + 启动 1us alpha 调度，后续 CNP 置位由 4us 定时器消费降速\n",
    "\t\tcnp_received_mlx(qp);\n",
    "\t}\n",
]
L[ret:ret] = ins
open(P, "w").writelines(L)
print("rdma-hw.cc: ReceiveCnp ccMode=16 branch inserted")

# ---------- config: CLAMP 恢复 1, 删除 SND_TICK(跟随 T=500) ----------
P = "/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/config/workload/config-workload.txt"
L = open(P).readlines()
out = []
for l in L:
    s = l.strip()
    if s == "CLAMP_TARGET_RATE 0":
        out.append("CLAMP_TARGET_RATE 1\n")
    elif s.startswith("DCIEXT_SND_TICK_US"):
        continue  # 删除 -> 跟随 T=500, 恢复与 alpha 动力学自洽的节拍
    else:
        out.append(l)
open(P, "w").writelines(out)
print("config: CLAMP=1, SND_TICK removed (follows T=500)")
