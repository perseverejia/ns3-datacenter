#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Themis 应用脚本 v3：剩余步骤（rdma-hw / workload / config / 脚本），Tab 缩进已按实际文件修正"""
import sys, re

BASE = "/home/gj/ns3/simulator/ns-3.39"

def load(p):
    with open(p, encoding='utf-8') as f:
        return f.read()

def save(p, s):
    with open(p, 'w', encoding='utf-8') as f:
        f.write(s)

def apply(path, old, new, expect=1):
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

# ============ rdma-hw.h 声明 UsesDcqcn ============
rhh = BASE + "/src/point-to-point/model/rdma-hw.h"
apply(rhh,
    '\tRdmaHw();\n',
    '\tRdmaHw();\n'
    '\tbool UsesDcqcn() const;\n')

# ============ rdma-hw.cc 定义 + 5 处模式列表 ============
rhc = BASE + "/src/point-to-point/model/rdma-hw.cc"
apply(rhc,
    'RdmaHw::RdmaHw() {\n'
    '\tm_epfcRttUs = 30.0; // ePFC (ccMode=17)：内部流 RTT 默认值（workload 可覆盖）\n'
    '}\n',
    'RdmaHw::RdmaHw() {\n'
    '\tm_epfcRttUs = 30.0; // ePFC (ccMode=17)：内部流 RTT 默认值（workload 可覆盖）\n'
    '}\n'
    '\n'
    'bool RdmaHw::UsesDcqcn() const {\n'
    '\treturn m_cc_mode == 1 || m_cc_mode == 15 || m_cc_mode == 16 ||\n'
    '\t       m_cc_mode == 17 || m_cc_mode == 18 || m_cc_mode == 19;\n'
    '}\n')

apply(rhc,
    'm_cc_mode == 1 || m_cc_mode == 16 || m_cc_mode == 17',
    'UsesDcqcn()', expect=5)

# ============ crossDC-evaluation-workload.cc 6 处 ============
wl = BASE + "/examples/CrossDC-Algos/crossDC-evaluation-workload.cc"

apply(wl,
    '#include "ns3/grc-helper.h"\n#include "ns3/qbb-helper.h"',
    '#include "ns3/grc-helper.h"\n#include "ns3/themis-helper.h"\n#include "ns3/qbb-helper.h"')

apply(wl,
    '\tstd::map<std::string, std::string> grcParameters;\n',
    '\tstd::map<std::string, std::string> grcParameters;\n'
    '\tstd::map<std::string, std::string> themisParameters;\n')

apply(wl,
    '\t\telse if (key.rfind("GRC_", 0) == 0) {\n'
    '\t\t\tconf >> grcParameters[key.substr(4)];\n'
    '\t\t}\n',
    '\t\telse if (key.rfind("GRC_", 0) == 0) {\n'
    '\t\t\tconf >> grcParameters[key.substr(4)];\n'
    '\t\t}\n'
    '\t\telse if (key.rfind("THEMIS_", 0) == 0) {\n'
    '\t\t\tconf >> themisParameters[key.substr(7)];\n'
    '\t\t}\n')

apply(wl,
    '\t\t\telse if (algorithm == 18) algo_suffix = "grc";\n',
    '\t\t\telse if (algorithm == 18) algo_suffix = "grc";\n'
    '\t\t\telse if (algorithm == 19) algo_suffix = "themis";\n')

apply(wl,
    '\tif (cc_mode == 18)\n'
    '\t\tGrcHelper::Install(cc_mode, n, serverAddress, g_hostToDc, g_switchToDc,\n'
    '\t\t                   dci_switches, grcParameters, randomSeed);\n',
    '\tif (cc_mode == 18)\n'
    '\t\tGrcHelper::Install(cc_mode, n, serverAddress, g_hostToDc, g_switchToDc,\n'
    '\t\t                   dci_switches, grcParameters, randomSeed);\n'
    '\n'
    '\tif (cc_mode == 19)\n'
    '\t\tThemisHelper::Install(cc_mode, n, serverAddress, g_hostToDc, g_switchToDc,\n'
    '\t\t                     dci_switches, themisParameters);\n')

apply(wl,
    '\t\telse if (algorithm == 18) algo_name = "grc";\n',
    '\t\telse if (algorithm == 18) algo_name = "grc";\n'
    '\t\telse if (algorithm == 19) algo_name = "themis";\n')

# ============ 两个 config 加 THEMIS 配置块 ============
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

# ============ run_all_algos.sh ============
sh = BASE + "/examples/CrossDC-Algos/scripts/run_all_algos.sh"
apply(sh,
    '    "18:GRC"\n)\n',
    '    "18:GRC"\n'
    '    "19:THEMIS"\n'
    ')\n')

# ============ run_single_workload_algo.py 3 处 ============
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
