#!/bin/bash
# 批量运行所有算法的仿真
# 使用方法: ./run_all_algos.sh

set -e  # 遇到错误立即退出

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CROSSDC_DIR="$(dirname "$SCRIPT_DIR")"
NS3_DIR="$(dirname "$(dirname "$CROSSDC_DIR")")"

echo "=========================================="
echo "CrossDC-Algos 批量运行脚本"
echo "=========================================="
echo "CrossDC-Algos 目录: $CROSSDC_DIR"
echo "NS-3 目录: $NS3_DIR"
echo ""

# 定义要运行的算法列表
# 格式: ccMode:算法名称
declare -a ALGOS=(
    "1:DCQCN"
    "3:HPCC"
    "7:Timely"
    "12:Bifrost"
    "13:FRP"
    "14:ROCC"
    "15:ATC"
    "16:DCIEXT"
    "17:EPFC"
    "18:GRC"
    "19:THEMIS"
)

# 检查是否指定了特定的算法
if [ $# -gt 0 ]; then
    echo "运行指定的算法: $@"
    SELECTED_ALGOS=("$@")
else
    echo "运行所有算法..."
    SELECTED_ALGOS=("${ALGOS[@]}")
fi

echo ""
echo "=========================================="
echo "开始批量运行"
echo "=========================================="
echo ""

SUCCESS_COUNT=0
FAIL_COUNT=0
FAILED_ALGOS=()

for algo_spec in "${SELECTED_ALGOS[@]}"; do
    IFS=':' read -r CC_MODE ALGO_NAME <<< "$algo_spec"
    
    echo "------------------------------------------"
    echo "运行算法: $ALGO_NAME (ccMode=$CC_MODE)"
    echo "------------------------------------------"
    
    cd "$NS3_DIR"
    
    if python3 "$CROSSDC_DIR/scripts/run_single_workload_algo.py" "$CC_MODE" "$ALGO_NAME"; then
        echo "✅ $ALGO_NAME 运行成功"
        ((SUCCESS_COUNT+=1))
    else
        echo "❌ $ALGO_NAME 运行失败"
        ((FAIL_COUNT+=1))
        FAILED_ALGOS+=("$ALGO_NAME(ccMode=$CC_MODE)")
    fi
    
    echo ""
done

echo "=========================================="
echo "批量运行完成"
echo "=========================================="
echo "成功: $SUCCESS_COUNT"
echo "失败: $FAIL_COUNT"

if [ ${#FAILED_ALGOS[@]} -gt 0 ]; then
    echo ""
    echo "失败的算法:"
    for algo in "${FAILED_ALGOS[@]}"; do
        echo "  - $algo"
    done
fi

echo ""
echo "结果目录: $CROSSDC_DIR/results/"
echo "图片目录: $CROSSDC_DIR/plots/"
echo ""
