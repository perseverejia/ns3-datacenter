import re, subprocess, shutil

BASE = '/home/gj/ns3/simulator/ns-3.39/examples/CrossDC-Algos'
CFG = BASE + '/config/workload/config-workload.txt'
SCRIPTS = BASE + '/scripts'
LOG = BASE + '/results/dump/algo_cc16_load0pct.log'

rows = []
for v in [1, 2, 3, 4, 5]:
    src = open(CFG).read()
    src = re.sub(r'(?m)^DCIEXT_DT_US \d+$', 'DCIEXT_DT_US %d' % v, src)
    src = re.sub(r'(?m)^DCIEXT_SIGMA_US \d+$', 'DCIEXT_SIGMA_US %d' % v, src)
    assert 'DCIEXT_DT_US %d' % v in src and 'DCIEXT_SIGMA_US %d' % v in src
    open(CFG, 'w').write(src)

    out = subprocess.run(['python3', 'run_single_workload_algo.py', '16', 'dciext', '--timeout', '900'],
                         cwd=SCRIPTS, capture_output=True, text=True, timeout=1100).stdout
    cross = re.search(r'Cross-DC:\s+avg=([\d.]+) ms, min=([\d.]+) ms, max=([\d.]+) ms', out)
    intra = re.search(r'Intra-DC:\s+avg=([\d.]+) ms, min=([\d.]+) ms, max=([\d.]+) ms', out)

    log = open(LOG, errors='ignore').read()
    shutil.copy(LOG, '/home/gj/ns3/results/dt_sweep_dt%d.log' % v)
    bs = {}
    for m in re.finditer(r'branch=(\w+)', log):
        bs[m.group(1)] = bs.get(m.group(1), 0) + 1
    ra = {}
    for m in re.finditer(r'R_avbl=([\d.]+)G', log):
        ra[float(m.group(1))] = ra.get(float(m.group(1)), 0) + 1
    dom_ra, dom_n = max(ra.items(), key=lambda x: x[1]) if ra else (0, 0)
    rtt_max = 0.0
    for m in re.finditer(r' rtt=([\d.]+)us', log):
        rtt_max = max(rtt_max, float(m.group(1)))

    row = dict(v=v,
               c_avg=float(cross.group(1)), c_min=float(cross.group(2)), c_max=float(cross.group(3)),
               i_avg=float(intra.group(1)), i_min=float(intra.group(2)), i_max=float(intra.group(3)),
               bs=bs, dom_ra=dom_ra, dom_n=dom_n, rtt_max=rtt_max)
    rows.append(row)
    print('Dt=sigma=%dus done: cross=%.1fms intra=%.1fms' % (v, row['c_avg'], row['i_avg']), flush=True)

print()
print('================ Dt=sigma 扫描汇总（base=最小值跟随, NicDelay=交换机0）================')
print('%-6s | %-22s | %-26s | %-30s | %s' % ('Dt=σ', 'Cross-DC FCT avg/min/max', 'Intra-DC FCT avg/min/max', 'branch 分布', 'R_avbl 主导 / RTTmax'))
print('-' * 130)
for r in rows:
    bs_s = ' '.join('%s:%d' % (k, n) for k, n in sorted(r['bs'].items(), key=lambda x: -x[1]))
    print('%-6d | %-22s | %-26s | %-30s | %.3fG(x%d) / %.0fus' % (
        r['v'],
        '%.1f/%.1f/%.1f' % (r['c_avg'], r['c_min'], r['c_max']),
        '%.1f/%.1f/%.1f' % (r['i_avg'], r['i_min'], r['i_max']),
        bs_s, r['dom_ra'], r['dom_n'], r['rtt_max']))

print()
print('公平性参考：Intra max-min（两域内流 FCT 差）与 总均值 (cross+intra)/2：')
for r in rows:
    print('  Dt=σ=%d: intra_spread=%.1fms, mean_all=%.1fms, cross+intra=%.1fms' % (
        r['v'], r['i_max'] - r['i_min'], (r['c_avg'] + r['i_avg']) / 2, r['c_avg'] + r['i_avg']))
