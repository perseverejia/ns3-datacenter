#!/usr/bin/env python3
"""
单算法仿真脚本：运行指定算法并生成图像
用法: python3 run_single_algo.py <ccMode> <algo_name>
示例: python3 run_single_algo.py 3 HPCC
"""

import subprocess
import sys
import os
import re
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

NS3_DIR = "/home/shemuping/newCode/ns3-FRP/simulator/ns-3.39"
DUMP_DIR = "/home/shemuping/newCode/ns3-FRP/dump" 
RESULTS_DIR = "/home/shemuping/newCode/ns3-FRP/results"
FCT_DIR = os.path.join(RESULTS_DIR, "fct")
PFC_DIR = os.path.join(RESULTS_DIR, "pfc")
CONFIG_FILE = "examples/PowerTCP/config.txt"  # 使用config.txt而不是config-burst.txt
DURATION = 0.020

os.makedirs(DUMP_DIR, exist_ok=True)
os.makedirs(RESULTS_DIR, exist_ok=True)
os.makedirs(FCT_DIR, exist_ok=True)
os.makedirs(PFC_DIR, exist_ok=True)

def run_and_plot(ccMode, algo_name):
    """运行仿真并绘图"""
    
    print(f"\n{'='*80}")
    print(f"仿真: {algo_name} (ccMode={ccMode})")
    print(f"{'='*80}\n")
    
    # 1. 更新配置
    config_path = os.path.join(NS3_DIR, CONFIG_FILE)
    with open(config_path, 'r') as f:
        content = f.read()
    
    content = re.sub(r'SIMULATOR_STOP_TIME\s*=\s*[\d.]+', f'SIMULATOR_STOP_TIME={DURATION}', content)
    content = re.sub(r'CC_MODE\s+\d+', f'CC_MODE {ccMode}', content)
    
    # 启用队列监控
    if 'ENABLE_QLEN_MON' not in content:
        # 在QLEN_MON_START前添加
        content = re.sub(r'(QLEN_MON_START)', f'ENABLE_QLEN_MON 1\n\1', content)
    else:
        content = re.sub(r'ENABLE_QLEN_MON\s+\d+', 'ENABLE_QLEN_MON 1', content)
    
    # 设置正确的时间范围 (0到仿真结束)
    content = re.sub(r'QLEN_MON_START\s+\d+', 'QLEN_MON_START 0', content)
    content = re.sub(r'QLEN_MON_END\s+\d+', f'QLEN_MON_END {int(DURATION * 1e9)}', content)
    
    # 设置 FCT / PFC 输出文件路径（按算法命名，分别放到 results/fct 和 results/pfc）
    fct_out = os.path.join(FCT_DIR, f"fct_{algo_name.lower()}.txt")
    pfc_out = os.path.join(PFC_DIR, f"pfc_{algo_name.lower()}.txt")
    content = re.sub(r'FCT_OUTPUT_FILE\s+\S+', f'FCT_OUTPUT_FILE {fct_out}', content)
    content = re.sub(r'PFC_OUTPUT_FILE\s+\S+', f'PFC_OUTPUT_FILE {pfc_out}', content)

    
    with open(config_path, 'w') as f:
        f.write(content)
    
    print(f"✓ 配置已更新")
    
    # 2. 编译
    print("编译中...")
    subprocess.run(f"cd {NS3_DIR} && ./ns3 build >/dev/null 2>&1", shell=True)
    print("✓ 编译完成\n")
    
    # 3. 运行仿真
    log_file = os.path.join(DUMP_DIR, f"algo_cc{ccMode}.log")
    print(f"运行仿真...")
    cmd = f"cd {NS3_DIR} && timeout 90 ./build/examples/PowerTCP/ns3.39-crossDC-evaluation-optimized --conf={CONFIG_FILE} --algorithm={ccMode} > {log_file} 2>&1"
    result = subprocess.run(cmd, shell=True, timeout=100)
    
    if result.returncode != 0:
        print(f"✗ 仿真失败")
        return
    
    log_size = os.path.getsize(log_file)
    print(f"✓ 仿真完成 ({log_size/1024:.1f}KB)\n")
    
    # 4. 解析日志
    print("解析日志...")
    with open(log_file, 'r') as f:
        lines = f.readlines()
    
    # 解析队列数据 - 使用Switch 10而不是Switch 13
    sw10_time, sw10_q = [], []
    for l in lines:
        l = l.strip()
        # DCQCN/HPCC/TIMELY 使用 DCQCN_QLEN，改为Switch 10
        if '[DCQCN_QLEN]' in l and ccMode in [1, 3, 7]:
            parts = l.split()
            # 监控的是Switch 10的Port 1（连host 4的端口）
            if len(parts) >= 5 and int(parts[2]) == 10 and int(parts[3]) == 1:
                sw10_time.append(float(parts[1]) / 1e9 * 1000)  # timestep -> ms
                sw10_q.append(float(parts[4]) / 1024.0)  # Bytes -> KB
        
        # FRP/ROCC 使用 FRP_DATA_SW，改为Switch 10
        if '[FRP_DATA_SW]' in l and ccMode in [13, 14]:
            parts = l.split()
            if len(parts) >= 8 and int(parts[2]) == 10 and int(parts[3]) == 1:
                if int(parts[7]) == ccMode:
                    sw10_time.append(float(parts[1]) * 1000)  # s -> ms
                    sw10_q.append(float(parts[5]))  # KB
    
    # 解析主机速率 (查找所有包含速率信息的日志)
    host_rates = {}
    host_times = {}  # 存储每个host的时间戳
    
    for l in lines:
        l = l.strip()
        
        # 所有算法（DCQCN/HPCC/TIMELY/FRP/ROCC）都使用TX RATE日志
        if '[TX RATE]' in l:
            match = re.search(r'\[TX RATE\].*Host (\d+).*m_rate=([\d.]+)Mbps.*nextAvail=(\d+)us', l)
            if match:
                host = int(match.group(1))
                rate_gbps = float(match.group(2)) / 1000
                time_us = int(match.group(3))
                
                if host not in host_rates:
                    host_rates[host] = []
                    host_times[host] = []
                host_rates[host].append(rate_gbps)
                host_times[host].append(time_us / 1000.0)
    
    # 生成时间轴 (线性分布)
    num_samples = max(len(rates) for rates in host_rates.values()) if host_rates else 0
    time_axis = np.linspace(0, DURATION * 1000, num_samples) if num_samples > 0 else []
    
    active_hosts = sorted(host_rates.keys())
    print(f"✓ 解析完成: Switch10 {len(sw10_time)} 点, 活跃主机 {len(active_hosts)} 个")
    print(f"  活跃主机列表: {active_hosts}")
    for h in active_hosts:
        print(f"    Host {h}: {len(host_rates[h])} 个速率数据点")
    print()
    
    # 5. 绘图
    print("生成图像...")
    plt.rcParams['font.family'] = 'DejaVu Sans'
    plt.rcParams['axes.unicode_minus'] = False
    
    fig, axes = plt.subplots(2, 1, figsize=(18, 12), sharex=True)
    fig.suptitle(f'{algo_name} (ccMode={ccMode}) - Burst Traffic Analysis',
                 fontsize=18, fontweight='bold')
    
    colors = ['red', 'blue', 'green', 'orange', 'purple', 'cyan', 'lime', 'brown', 'pink', 'gray',
              'magenta', 'olive', 'teal', 'navy', 'maroon', 'coral', 'gold', 'indigo']
    
    # 子图1: 主机速率
    ax1 = axes[0]
    print(f"绘制 {len(active_hosts)} 个流的速率...")
    for idx, h in enumerate(active_hosts[:15]):  # 最多显示15个流
        if host_rates[h]:
            c = colors[idx % len(colors)]
            rates = host_rates[h]
            times = host_times[h] if h in host_times and len(host_times[h]) == len(rates) else time_axis[:len(rates)]
            print(f"  Flow {h}: {len(rates)} 点, 速率范围 [{min(rates):.2f}, {max(rates):.2f}] Gbps, 时间范围 [{min(times):.3f}, {max(times):.3f}] ms")
            ax1.plot(times, rates, 
                    color=c, lw=2, label=f'Flow {h}', alpha=0.85)
    
    ax1.set_ylabel('Sending Rate (Gbps)', fontsize=14, fontweight='bold')
    ax1.set_title(f'Flow Sending Rates ({len(active_hosts)} flows)', fontsize=15, fontweight='bold')
    ax1.legend(loc='upper right', fontsize=10, ncol=3, framealpha=0.9)
    ax1.grid(True, alpha=0.3, linestyle='--')
    ax1.set_ylim(0, 110)
    ax1.tick_params(labelsize=11)
    
    # 子图2: 队列长度
    ax2 = axes[1]
    if sw10_time and sw10_q:
        ax2.fill_between(sw10_time, 0, sw10_q, alpha=0.35, color='purple')
        ax2.plot(sw10_time, sw10_q, color='purple', lw=2, label='Switch 10 Port 1')
        ax2.axhline(y=500, color='blue', ls='--', lw=2, alpha=0.7, label='qRef = 500KB')
        ax2.axhline(y=300, color='red', ls=':', lw=1.5, alpha=0.6, label='q_th = 300KB')
    
    ax2.set_ylabel('Queue Length (KB)', fontsize=14, fontweight='bold')
    ax2.set_xlabel('Time (ms)', fontsize=14, fontweight='bold')
    ax2.set_title('Switch 10 Queue Length (Port 1 - Bottleneck)', fontsize=15, fontweight='bold')
    ax2.legend(loc='upper right', fontsize=11, framealpha=0.9)
    ax2.grid(True, alpha=0.3, linestyle='--')
    
    max_q = max(sw10_q) if sw10_q else 1000
    ax2.set_ylim(-50, max_q * 1.15)
    ax2.tick_params(labelsize=11)
    
    plt.tight_layout()
    
    output_file = os.path.join(RESULTS_DIR, f'burst_{algo_name.lower()}_cc{ccMode}.png')
    plt.savefig(output_file, dpi=150, bbox_inches='tight')
    print(f"✓ 图像已保存: {output_file}\n")
    
    # 6. 打印统计
    print(f"{'='*80}")
    print(f"统计信息 - {algo_name}")
    print(f"{'='*80}")
    print(f"活跃流数量: {len(active_hosts)}")
    print(f"队列最大值: {max(sw10_q):.0f} KB" if sw10_q else "队列数据: 无")
    print(f"队列均值: {np.mean(sw10_q):.0f} KB" if sw10_q else "")
    print(f"图像文件: {output_file}")
    print(f"{'='*80}\n")
    
    return output_file

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("用法: python3 run_single_algo.py <ccMode> <algo_name>")
        print("示例: python3 run_single_algo.py 3 HPCC")
        print("\n支持的算法:")
        print("  1  - DCQCN")
        print("  3  - HPCC")
        print("  7  - TIMELY")
        print("  13 - FRP (ours)")
        print("  14 - ROCC")
        sys.exit(1)
    
    ccMode = int(sys.argv[1])
    algo_name = sys.argv[2]
    
    run_and_plot(ccMode, algo_name)
