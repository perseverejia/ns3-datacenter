#!/usr/bin/env python3
"""v4 压制表: 静默400us(α₀=0.2) + CNP间隔4us + 压制周期400us"""

# ---------- 1. dci-ext.cc BuildTable 重写 ----------
P = "/home/gj/ns3/simulator/ns-3.39/src/point-to-point/model/dci-ext.cc"
L = open(P).readlines()
start = None
for i, l in enumerate(L):
    if "void DciExt::BuildTable() {" in l:
        start = i
        break
assert start is not None, "BuildTable not found"
end = None
for i in range(start + 1, len(L)):
    if L[i].rstrip() == "}":
        end = i
        break
assert end is not None

new_body = """void DciExt::BuildTable() {
    // v4 表模型：静默 400us 后 α 从 1 衰减到 (255/256)^400 ≈ 0.209 → α₀=0.2 起步；
    // CNP 每 4us 一个（与刀消费 4us 对齐）；α 每 1us 更新：
    //   到达的 1us  α <- (1-g)α + g，随后 3us x(1-g)^3；
    // 每 4us 末消费一刀 m *= (1 - α/2)（单刀约 -10%，n 刀合成见表）
    const double g = 1.0 / 256.0;      // EWMA_GAIN（必须与端侧 config EWMA_GAIN 一致）
    const double alpha0 = 0.2;         // 静默 400us 后的起步 α
    m_M[0] = 1.0;
    double alpha = alpha0;
    double m = 1.0;
    for (uint32_t n = 1; n <= MAX_BURST_N; n++) {
        alpha = (1.0 - g) * alpha + g;           // CNP 到达的 1us
        for (int k = 0; k < 3; k++)
            alpha *= (1.0 - g);                  // 随后 3us 衰减
        m *= (1.0 - alpha / 2.0);                // 4us 末消费一刀
        m_M[n] = m;
    }
}
"""
L[start:end + 1] = [new_body]
open(P, "w").writelines(L)
print("dci-ext.cc: BuildTable rewritten (alpha0=0.2, 4us CNP gap)")

# ---------- 2. config ----------
P = "/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos/config/workload/config-workload.txt"
L = open(P).readlines()
out = []
for l in L:
    s = l.strip()
    if s == "DCIEXT_BURST_INTERVAL_US 1":
        out.append("DCIEXT_BURST_INTERVAL_US 4\n")
    elif s == "DCIEXT_SILENT_US 1000":
        out.append("DCIEXT_SILENT_US 400\n")
    elif s == "DCIEXT_T_US 500":
        out.append(l)
        out.append("DCIEXT_SND_TICK_US 400\n")
    else:
        out.append(l)
open(P, "w").writelines(out)
print("config: BURST_INTERVAL=4, SILENT=400, SND_TICK=400 (T=500 unchanged)")
