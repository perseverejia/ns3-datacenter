#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""FIRST_ACK 首窗相位敏感修复：
接收方 DCI 首窗 ackRate 原口径 = 窗口内字节 / 整窗 T，数据经 WAN 到达时刻
与窗口边界无对齐，首窗只占尾部一段导致 R_avbl 初值被相位稀释（T=400us 时
8.18G vs 真实 ~20G）。改为按"首个 ACK 到达 → 本窗结算"的活跃时长归一。"""
import sys

BASE = "/home/gj/ns3/simulator/ns-3.39/src/point-to-point/model/"

def apply(path, old, new, expect=1):
    with open(path, encoding='utf-8') as f:
        s = f.read()
    if new in s:
        print(f"SKIP {path}: already applied")
        return
    c = s.count(old)
    if c != expect:
        print(f"FAIL {path}: count={c} expect={expect}\n--- pattern ---\n{old[:200]}")
        sys.exit(1)
    s = s.replace(old, new)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(s)
    print(f"OK   {path}")

# ---- 1. dci-ext.h: 流表项增加 winFirstAckT ----
H = BASE + "dci-ext.h"
apply(H,
    '    uint64_t winAckBytes;    // 本周期 ACK 确认字节（DIP ACK 反馈速率口径）\n'
    '    uint64_t winTxBytes;     // 本周期 DCI→DC 出队字节（txRate 口径，纯DC内RTT打点同源）\n',
    '    uint64_t winAckBytes;    // 本周期 ACK 确认字节（DIP ACK 反馈速率口径）\n'
    '    Time winFirstAckT;       // 本周期首个 ACK 到达时刻（首窗归一分母；-1s=本窗无ACK）\n'
    '    uint64_t winTxBytes;     // 本周期 DCI→DC 出队字节（txRate 口径，纯DC内RTT打点同源）\n')

apply(H,
    '                      ackWatermark(0), winAckBytes(0), winTxBytes(0),\n'
    '                      rttCnt(0), rttSumUs(0), rttMinUs(1e18), rttMaxUs(0),\n',
    '                      ackWatermark(0), winAckBytes(0), winFirstAckT(Seconds(-1)),\n'
    '                      winTxBytes(0),\n'
    '                      rttCnt(0), rttSumUs(0), rttMinUs(1e18), rttMaxUs(0),\n')

# ---- 2. dci-ext.cc: ACK 水位推进处记录首 ACK 时刻 ----
C = BASE + "dci-ext.cc"
apply(C,
    '        if (ackSeq > f.ackWatermark) {\n'
    '            f.winAckBytes += (uint64_t)(ackSeq - f.ackWatermark);\n'
    '            f.ackWatermark = ackSeq;\n',
    '        if (ackSeq > f.ackWatermark) {\n'
    '            if (f.winAckBytes == 0)\n'
    '                f.winFirstAckT = Simulator::Now(); // 本窗首个 ACK（首窗归一分母）\n'
    '            f.winAckBytes += (uint64_t)(ackSeq - f.ackWatermark);\n'
    '            f.ackWatermark = ackSeq;\n')

# ---- 3. Agg 结构体增加 firstAckT ----
apply(C,
    '        uint64_t ackBytes = 0;\n'
    '        uint64_t txBytes = 0;   // 上个T DCI→DC 出队字节（W）\n',
    '        uint64_t ackBytes = 0;\n'
    '        uint64_t txBytes = 0;   // 上个T DCI→DC 出队字节（W）\n'
    '        Time firstAckT = Seconds(-1); // 本窗各流最早的首个 ACK 时刻\n')

# ---- 4. 聚合循环：取最早首 ACK 时刻 ----
apply(C,
    '        a.ackBytes += f.winAckBytes;\n'
    '        a.txBytes += f.winTxBytes;\n',
    '        a.ackBytes += f.winAckBytes;\n'
    '        a.txBytes += f.winTxBytes;\n'
    '        if (f.winAckBytes > 0 && f.winFirstAckT >= Seconds(0) &&\n'
    '            (a.firstAckT < Seconds(0) || f.winFirstAckT < a.firstAckT))\n'
    '            a.firstAckT = f.winFirstAckT;\n')

# ---- 5. 周期清零处重置 ----
apply(C,
    '        f.winAckBytes = 0; // 周期清零\n'
    '        f.winTxBytes = 0;\n',
    '        f.winAckBytes = 0; // 周期清零\n'
    '        f.winFirstAckT = Seconds(-1);\n'
    '        f.winTxBytes = 0;\n')

# ---- 6. FIRST_ACK 分支：按活跃时长归一 ----
apply(C,
    '        } else if (!rd.hasRAvbl) {\n'
    '            // a. 首次获取 DIP 可用带宽：直接用 ACK 返回速率\n'
    '            if (ackRateBps > 0) {\n'
    '                rd.rAvblBps = ackRateBps; // DIP 级总量：ACK 总速率\n'
    '                rd.hasRAvbl = true;\n'
    '                branch = "FIRST_ACK";\n',
    '        } else if (!rd.hasRAvbl) {\n'
    '            // a. 首次获取 DIP 可用带宽：直接用 ACK 返回速率\n'
    '            if (ackRateBps > 0) {\n'
    '                // 首窗 ACK 只占窗口尾部（数据经 WAN 到达时刻与窗口边界无对齐），\n'
    '                // 按"首个 ACK 到达 → 本窗结算"的活跃时长归一，消除窗口相位稀释\n'
    '                double activeSec = (a.firstAckT >= Seconds(0))\n'
    '                                       ? (now - a.firstAckT).GetSeconds() : tSec;\n'
    '                if (activeSec <= 0 || activeSec > tSec)\n'
    '                    activeSec = tSec; // 防御：异常时退回整窗口径\n'
    '                double firstRateBps = (double)a.ackBytes * 8.0 / activeSec;\n'
    '                if (firstRateBps > m_dipBwBps)\n'
    '                    firstRateBps = m_dipBwBps; // clamp 到 DIP 带宽上界\n'
    '                rd.rAvblBps = firstRateBps; // DIP 级总量：首窗活跃时长归一\n'
    '                rd.hasRAvbl = true;\n'
    '                branch = "FIRST_ACK";\n')

print("ALL DONE")
