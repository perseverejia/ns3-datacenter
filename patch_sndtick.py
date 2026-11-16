#!/usr/bin/env python3
"""拆分发送方压制周期与接收方反馈周期: 新增 DCIEXT_SND_TICK_US"""
import sys

def find(lines, sub, start=0):
    hits = [i for i, l in enumerate(lines) if sub in l]
    assert hits, f"anchor not found: {sub!r}"
    return hits[0]

# ---------- 1. dci-ext.h ----------
P = "/home/gj/ns3/simulator/ns-3.39/src/point-to-point/model/dci-ext.h"
L = open(P).readlines()
i = find(L, "void SetSenderSide(bool enable)")
L.insert(i + 1, "    void SetSndTickUs(double us); // 发送方压制周期(us); 默认跟随 T\n")
i = find(L, "Time m_periodT;")
L.insert(i + 1, "    Time m_sndTickPeriod;        // 发送方 SndTick 周期; Configure 默认跟随 T\n")
open(P, "w").writelines(L)
print("dci-ext.h patched")

# ---------- 2. dci-ext.cc ----------
P = "/home/gj/ns3/simulator/ns-3.39/src/point-to-point/model/dci-ext.cc"
L = open(P).readlines()
i = find(L, "m_sndTickEvent = EventId();")   # Configure 尾部
L.insert(i + 1, "\tm_sndTickPeriod = m_periodT; // 压制周期默认跟随 T; 可被 SetSndTickUs 覆盖\n")
i = find(L, "void DciExt::SetSendCallback")
L[i:i] = [
    "void DciExt::SetSndTickUs(double us) {\n",
    "\tif (us > 0)\n",
    "\t\tm_sndTickPeriod = MicroSeconds(us);\n",
    "}\n",
    "\n",
]
n = sum(1 for l in L if "Simulator::Schedule(m_periodT, &DciExt::SndTick, this)" in l)
assert n == 2, f"SndTick schedule count = {n}"
for i, l in enumerate(L):
    if "Simulator::Schedule(m_periodT, &DciExt::SndTick, this)" in l:
        L[i] = l.replace("Schedule(m_periodT, &DciExt::SndTick", "Schedule(m_sndTickPeriod, &DciExt::SndTick")
open(P, "w").writelines(L)
print("dci-ext.cc patched (2 schedule sites)")

# ---------- 3. switch-node.h ----------
P = "/home/gj/ns3/simulator/ns-3.39/src/point-to-point/model/switch-node.h"
L = open(P).readlines()
i = find(L, "void InitializeDciExt")
L.insert(i, "      void SetDciExtSndTickUs(double us); // 透传: 发送方压制周期\n")
open(P, "w").writelines(L)
print("switch-node.h patched")

# ---------- 4. switch-node.cc ----------
P = "/home/gj/ns3/simulator/ns-3.39/src/point-to-point/model/switch-node.cc"
L = open(P).readlines()
i = find(L, "void SwitchNode::InitializeDciExt")
L[i:i] = [
    "void SwitchNode::SetDciExtSndTickUs(double us) {\n",
    "\tm_dciExt.SetSndTickUs(us);\n",
    "}\n",
    "\n",
]
open(P, "w").writelines(L)
print("switch-node.cc patched")

# ---------- 5. crossDC-evaluation-workload.cc ----------
P = "/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/crossDC-evaluation-workload.cc"
L = open(P).readlines()
i = find(L, "double dciext_t_us = 1000;")
L.insert(i + 1, "double dciext_snd_tick_us = 0;         // 发送方压制周期(us); 0=跟随 T\n")
i = find(L, 'std::cout << "DCIEXT_T_US\\t\\t\\t" << v << "\\n";')
L[i + 1:i + 1] = [
    "\t\t} else if (key.compare(\"DCIEXT_SND_TICK_US\") == 0) {\n",
    "\t\t\tdouble v;\n",
    "\t\t\tconf >> v;\n",
    "\t\t\tdciext_snd_tick_us = v;\n",
    "\t\t\tstd::cout << \"DCIEXT_SND_TICK_US\\t\\t\" << v << \"\\n\";\n",
]
j = find(L, "/*senderSide=*/true")
assert ");" in L[j + 1], f"unexpected line after senderSide call: {L[j+1]!r}"
L.insert(j + 2, "                                        if (dciext_snd_tick_us > 0) sw->SetDciExtSndTickUs(dciext_snd_tick_us);\n")
open(P, "w").writelines(L)
print("crossDC-evaluation-workload.cc patched")

# ---------- 6. config ----------
P = "/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/config/workload/config-workload.txt"
L = open(P).readlines()
i = find(L, "DCIEXT_T_US")
assert L[i].strip() == "DCIEXT_T_US 100", L[i]
L[i] = "DCIEXT_T_US 500\n"
L.insert(i + 1, "DCIEXT_SND_TICK_US 100\n")
open(P, "w").writelines(L)
print("config-workload.txt patched: T=500, SND_TICK=100")
