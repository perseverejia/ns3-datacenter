#!/bin/bash
# 批量运行5种算法

echo "=========================================="
echo "批量仿真: DCQCN, HPCC, TIMELY, FRP, ROCC"
echo "=========================================="

declare -A ALGOS
ALGOS=( [1]="DCQCN" [3]="HPCC" [7]="TIMELY" [13]="FRP" [14]="ROCC" )

for ccMode in "${!ALGOS[@]}"; do
    algo=${ALGOS[$ccMode]}
    echo ""
    echo ">>> 运行 $algo (ccMode=$ccMode)..."
    python3 run_single_algo.py $ccMode $algo
done

echo ""
echo "=========================================="
echo "全部完成！"
echo "=========================================="
ls -lh /home/shemuping/newCode/ns3-FRP/results/burst_*.png
