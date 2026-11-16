#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Themis (ccMode=19) 应用脚本 v2
先修复 switch-node.h 的重复插入损坏，再应用剩余全部 Themis 改动。
幂等策略：每一步先检查目标 new 内容是否已存在，存在则 SKIP。"""
import sys, re

BASE = "/home/gj/ns3/simulator/ns-3.39"

def load(p):
    with open(p, encoding='utf-8') as f:
        return f.read()

def save(p, s):
    with open(p, 'w', encoding='utf-8') as f:
        f.write(s)

def apply(path, old, new, expect=1):
    """old 唯一匹配 expect 次才替换；new 已存在则 SKIP"""
    s = load(path)
    if new in s:
        print(f"SKIP {path}: already applied")
        return
    c = s.count(old)
    if c != expect:
        print(f"FAIL {path}: pattern count={c} expect={expect}")
        print("--- pattern head ---")
        print(old[:150])
        sys.exit(1)
    s = s.replace(old, new)
    save(path, s)
    print(f"OK   {path}: replaced {expect}")

def dedup_consecutive(path, text, copies):
    """把连续 copies 份 text 去重为 1 份；已经是 1 份则 SKIP"""
    s = load(path)
    if s.count(text) == 1:
        print(f"SKIP {path}: already clean")
        return
    dup = text * copies
    if dup not in s:
        print(f"FAIL {path}: expected {copies} consecutive copies not found (count={s.count(text)})")
        sys.exit(1)
    s = s.replace(dup, text)
    save(path, s)
    print(f"OK   {path}: deduped {copies} -> 1")

# ============ 第 1 步：修复 switch-node.h 重复损坏 ============
snh = BASE + "/src/point-to-point/model/switch-node.h"

# 1a. include ×5 -> ×1
dedup_consecutive(snh, '#include "themis-gateway.h"\n', 5)

# 1b. m_themis ×2 -> ×1
s = load(snh)
m = re.search(r'[ \t]*Ptr<ThemisGateway> m_themis;\n', s)
if not m:
    print("FAIL m_themis line not found"); sys.exit(1)
dedup_consecutive(snh, m.group(0), 2)

# 1c. InitializeThemis 声明 ×4 -> ×1
s = load(snh)
m = re.search(r'[ \t]*Ptr<ThemisGateway> InitializeThemis\(uint32_t dc,\n[ \t]*const std::map<uint32_t, uint32_t>& hostIpToDc,\n[ \t]*const ThemisSettings& settings\);\n', s)
if not m:
    print("FAIL InitializeThemis decl block not found"); sys.exit(1)
block = m.group(0)
c = s.count(block)
if c == 1:
    print("SKIP switch-node.h: InitializeThemis decl already clean")
elif c == 4 and block * 4 in s:
    save(snh, s.replace(block * 4, block))
    print("OK   switch-node.h: InitializeThemis decl deduped 4 -> 1")
else:
    print(f"FAIL switch-node.h: InitializeThemis decl count={c} or not consecutive"); sys.exit(1)

# ============ 第 2 步：switch-node.cc 的 3c / 3d ============
snc = BASE + "/src/point-to-point/model/switch-node.cc"

# 3c: SwitchNotifyDequeue 开头跳过自环端口
apply(snc,
    'void SwitchNode::SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p) {\n'
    '\tInterfaceTag t;',
    'void SwitchNode::SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p) {\n'
    '\t// Internal loop FIFO admission is paired with ThemisGateway\'s finite byte\n'
    '\t// accounting, not the normal MMU. Do not mark dummy/loop traffic with ECN.\n'
    '\tif (m_themis && m_themis->IsLoopPort(ifIndex)) return;\n'
    '\tInterfaceTag t;')

# 3d: SwitchNotifyDequeue 尾部 PNP 钩子
apply(snc,
    '\t\tCheckAndSendResume(inDev, qIndex);\n'
    '\t}\n'
    '\tif (1) {',
    '\t\tCheckAndSendResume(inDev, qIndex);\n'
    '\t}\n'
    '\t// PNP also sees CE marks just added by this ESW\'s own egress queue.\n'
    '\tif (m_themis) m_themis->NotifyDequeue(ifIndex, p);\n'
    '\tif (1) {')

# ============ 第 3 步：rdma-hw.h 声明 UsesDcqcn ============
rhh = BASE + "/src/point-to-point/model/rdma-hw.h"
apply(rhh,
    '    RdmaHw();\n',
    '    RdmaHw();\n'
    '    bool UsesDcqcn() const;\n')

# ============ 第 4 步：rdma-hw.cc 定义 + 5 处模式列表 ============
rhc = BASE + "/src/point-to-point/model/rdma-hw.cc"
apply(rhc,
    'RdmaHw::RdmaHw() {\n'
    '        m_epfcRttUs = 30.0; // ePFC (ccMode=17)：内部流 RTT 默认值（workload 可覆盖）\n'
    '}\n',
    'RdmaHw::RdmaHw() {\n'
    '        m_epfcRttUs = 30.0; // ePFC (ccMode=17)：内部流 RTT 默认值（workload 可覆盖）\n'
    '}\n'
    '\n'
    'bool RdmaHw::UsesDcqcn() const {\n'
    '        return m_cc_mode == 1 || m_cc_mode == 15 || m_cc_mode == 16 ||\n'
    '               m_cc_mode == 17 || m_cc_mode == 18 || m_cc_mode == 19;\n'
    '}\n')

apply(rhc,
    'm_cc_mode == 1 || m_cc_mode == 16 || m_cc_mode == 17',
    'UsesDcqcn()', expect=5)

# ============ 第 5 步：crossDC-evaluation-workload.cc 6 处 ============
wl = BASE + "/examples/CrossDC-Algos/crossDC-evaluation-workload.cc"

apply(wl,
    '#include "ns3/grc-helper.h"\n#include "ns3/qbb-helper.h"',
    '#include "ns3/grc-helper.h"\n#include "ns3/themis-helper.h"\n#include "ns3/qbb-helper.h"')

apply(wl,
    '        std::map<std::string, std::string> grcParameters;\n',
    '        std::map<std::string, std::string> grcParameters;\n'
    '        std::map<std::string, std::string> themisParameters;\n')

apply(wl,
    '                else if (key.rfind("GRC_", 0) == 0) {\n'
    '                        conf >> grcParameters[key.substr(4)];\n'
    '                }\n',
    '                else if (key.rfind("GRC_", 0) == 0) {\n'
    '                        conf >> grcParameters[key.substr(4)];\n'
    '                }\n'
    '                else if (key.rfind("THEMIS_", 0) == 0) {\n'
    '                        conf >> themisParameters[key.substr(7)];\n'
    '                }\n')

apply(wl,
    '                        else if (algorithm == 18) algo_suffix = "grc";\n',
    '                        else if (algorithm == 18) algo_suffix = "grc";\n'
    '                        else if (algorithm == 19) algo_suffix = "themis";\n')

apply(wl,
    '        if (cc_mode == 18)\n'
    '                GrcHelper::Install(cc_mode, n, serverAddress, g_hostToDc, g_switchToDc,\n'
    '                                    dci_switches, grcParameters, randomSeed);\n',
    '        if (cc_mode == 18)\n'
    '                GrcHelper::Install(cc_mode, n, serverAddress, g_hostToDc, g_switchToDc,\n'
    '                                    dci_switches, grcParameters, randomSeed);\n'
    '\n'
    '        if (cc_mode == 19)\n'
    '                ThemisHelper::Install(cc_mode, n, serverAddress, g_hostToDc, g_switchToDc,\n'
    '                                      dci_switches, themisParameters);\n')

apply(wl,
    '                else if (algorithm == 18) algo_name = "grc";\n',
    '                else if (algorithm == 18) algo_name = "grc";\n'
    '                else if (algorithm == 19) algo_name = "themis";\n')

# ============ 第 6 步：两个 config 加 THEMIS 配置块 ============
def add_themis_cfg(path):
    s = load(path)
    if 'THEMIS_ALPHA' in s:
        print(f"SKIP {path}: already applied")
        return
    m = re.search(r'(GRC_LOG_DIR[^\n]*\n\n)(# =+\n)(# \[HPCC\][^\n]*\n)', s)
    if not m:
        print(f"FAIL {path}: GRC/HPCC anchor not found")
        sys.exit(1)
    sep = m.group(2)
    blk = (sep
        + '# [THEMIS] ccMode=19 - PNP/TRP，端侧复用现有 DCQCN\n'
        + '# 反馈沿用本工程逐 ECN 包触发的方式，循环资源根据部署节点的拓扑派生\n'
        + sep
        + 'THEMIS_ALPHA 5\n'
        + 'THEMIS_BETA_US 500\n'
        + 'THEMIS_DUMMY_PACKET_BYTES 1024\n'
        + 'THEMIS_TRACE_EVENTS 1\n'
        + '\n')
    pos = m.end(1)
    s = s[:pos] + blk + s[pos:]
    save(path, s)
    print(f"OK   {path}: THEMIS config block added")

add_themis_cfg(BASE + "/examples/CrossDC-Algos/config/workload/config-workload.txt")
add_themis_cfg(BASE + "/examples/CrossDC-Algos/config/workload/config-testbed.txt")

# ============ 第 7 步：run_all_algos.sh ============
sh = BASE + "/examples/CrossDC-Algos/scripts/run_all_algos.sh"
apply(sh,
    '    "18:GRC"\n)\n',
    '    "18:GRC"\n'
    '    "19:THEMIS"\n'
    ')\n')

# ============ 第 8 步：run_single_workload_algo.py 3 处 ============
py = BASE + "/examples/CrossDC-Algos/scripts/run_single_workload_algo.py"
apply(py,
    'sim_cc_mode in (1, 3, 7, 16, 17, 18):',
    'sim_cc_mode in (1, 3, 7, 16, 17, 18, 19):')

apply(py,
    '  grc        -> ccMode=18  (GRC: DCI 网侧控制，端侧复用现有 DCQCN)\n',
    '  grc        -> ccMode=18  (GRC: DCI 网侧控制，端侧复用现有 DCQCN)\n'
    '  themis     -> ccMode=19  (THEMIS: PNP/TRP，端侧复用现有 DCQCN)\n')

apply(py,
    '  python3 run_single_workload_algo.py 18 GRC\n',
    '  python3 run_single_workload_algo.py 18 GRC\n'
    '  python3 run_single_workload_algo.py 19 THEMIS\n')

print("ALL DONE")
