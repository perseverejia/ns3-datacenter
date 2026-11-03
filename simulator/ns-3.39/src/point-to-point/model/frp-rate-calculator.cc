/*
 * FRP (Fair Rate Protocol) 公平速率计算器实现
 * 
 * 核心公式: F_new = F_old - α*(q_cur - q_ref + lanbackoff) - β*(q_cur - q_old)
 * 
 * 量纲转换策略:
 *   1. 外部接口使用标准单位 (Byte, bps)
 *   2. 内部计算转换为局部单位 (600B Cell, 10Mbps)
 *   3. 计算完成后还原为标准单位
 */

#include "frp-rate-calculator.h"
#include "ns3/log.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("FrpRateCalculator");

FrpRateCalculator::FrpRateCalculator() {
    // FRP算法参数
    m_alpha = 0.125;
    m_beta = 0;
    m_scaleFactor = 20.0;           // scaleFactor = 12 (端侧 rdma-hw.cc 策略2 同步使用 12.0)
    m_tPeriodS = 40e-6;         // 40微秒 = 0.00004秒
    

    // 队列阈值和参考值 (单位: Byte)
    m_qThBytes = 307200.0;       // 300KB = 307200 Byte
    m_qRef40GBytes = 307200.0;   // 40Gbps 用 300KB
    m_qRef100GBytes = 307200.0;
    m_qRef200GBytes = 1048576.0; // 1MB = 1048576 Byte (200Gbps)
    
    NS_LOG_INFO("FRP Rate Calculator initialized: alpha=" << m_alpha 
                << ", beta=" << m_beta 
                << ", scaleFactor=" << m_scaleFactor
                << ", T=" << (m_tPeriodS * 1e6) << "μs");
}

FrpRateCalculator::~FrpRateCalculator() {}

double FrpRateCalculator::CalculateFairRate(uint32_t portId, uint64_t linkBps, uint32_t currentQBytes, bool hasWanFlow, uint8_t ccMode, uint8_t consecutiveCrossDcTrigger) {
    
    FrpPortState& state = m_portStates[portId];

    // 边界限制 (使用全局量纲 bps)
    double maxRateBps = linkBps * 0.95;
    double minRateBps = 100.0 * 1000000.0;  // 100Mbps

    // 初始化
    if (!state.isInitialized) {
        state.currentFairRateBps = maxRateBps;
        state.qOldBytes = 0.0;
        state.isInitialized = true;
        
        NS_LOG_INFO("Port " << portId << " initialized with rate=" << (maxRateBps / 1e6) << " Mbps");
    }

    // 根据带宽选择对应的q_ref (单位: Byte)
    double qRefBytes;
    if (linkBps >= 200ULL * 1000000000ULL) {
        qRefBytes = m_qRef200GBytes;
    } else if (linkBps >= 100ULL * 1000000000ULL) {
        qRefBytes = m_qRef100GBytes;
    } else {
        qRefBytes = m_qRef40GBytes;
    }

    // ========== ROCC 模式 (ccMode=14) ==========
    if (ccMode == 14) {
        minRateBps = 100.0 * 1000000.0;
        double fNewBps = state.currentFairRateBps;  // 默认保持当前速率
        
        // ROCC规则1: 队列过载且速率>12.5Gbps → 降至最小速率
        if ((currentQBytes > m_qThBytes + qRefBytes) && (state.currentFairRateBps > linkBps / 8.0)) {
            fNewBps = minRateBps;
            // 更新状态
            state.qOldBytes = static_cast<double>(currentQBytes);
            state.currentFairRateBps = fNewBps;
            return fNewBps;
        }
        // ROCC规则2: 队列超过2*qRef且速率>12.5Gbps → 减半
        else if ((currentQBytes - qRefBytes > qRefBytes) && (state.currentFairRateBps > linkBps / 8.0)) {
            fNewBps = state.currentFairRateBps / 2.0;
            // 更新状态
            state.qOldBytes = static_cast<double>(currentQBytes);
            state.currentFairRateBps = fNewBps;
            return fNewBps;
        }
        
    }

    // ========== FRP 模式 (ccMode=13) ==========
    
    // ==================== ⚡ 临时量纲转换区域 ⚡ ====================
    // 转换为局部单位进行计算
    
    // 1. 队列长度转换为 600B Cell 单位
    double qCurCell = static_cast<double>(currentQBytes) / 600.0;
    double qRefCell = qRefBytes / 600.0;
    double qOldCell = state.qOldBytes / 600.0;
    double qThCell  = m_qThBytes / 600.0;
    double k = 10000000.0/(600.0* 8.0); //进行lanbackoff的单位换算

    // 2. 速率转换为 10Mbps 单位
    double fOld_10Mbps = state.currentFairRateBps / 10000000.0;

    // 3. 计算 lanbackoff (ROCC模式下不使用lanbackoff)
    double lanbackoff = 0.0;
    if (ccMode == 13) {
        m_beta = 0.0;
        // FRP模式: 当队列超过q_th时计算lanbackoff
        if ((qCurCell - qRefCell) > qThCell) {
             lanbackoff = m_scaleFactor * (qCurCell - qRefCell) * m_tPeriodS - qOldCell;
        }
        // 上游判断为"跨域流独占"时，抑制本地退避项
        if (consecutiveCrossDcTrigger >= 10) {
            lanbackoff = 0.0;
        }
    }

    // ROCC模式 (ccMode=14): lanbackoff保持为0
    if (ccMode == 14) {
        lanbackoff = 0.0;
        m_beta = 1.0;
        
    } 
    // 4. 计算新的公平速率 (在10Mbps局部量纲下)
    // 公式: F_new = F_old - α*(q_cur - q_ref + lanbackoff) - β*(q_cur - q_old)
     double alpha_term = m_alpha * (qCurCell - qOldCell);
    double beta_term = m_beta * (qCurCell - qOldCell);
    double fNew_10Mbps = fOld_10Mbps - alpha_term - beta_term;


    // ==================== 还原为标准全局量纲 ====================
    double fNewBps = fNew_10Mbps * 10000000.0;
    double fNewBps_beforeClamp = fNewBps;

    // 5. 边界裁剪 (使用全局标准量纲 bps)
    if (fNewBps > maxRateBps) {
        fNewBps = maxRateBps;
    }
    if (fNewBps < minRateBps) {
        fNewBps = minRateBps;
    }
    if (fNewBps != fNewBps_beforeClamp) {
        std::cout << "[FRP CLAMP] port=" << portId
                  << " " << (fNewBps_beforeClamp/1e9) << "G -> " << (fNewBps/1e9) << "G"
                  << (fNewBps == minRateBps ? " (MIN)" : " (MAX)")
                  << std::endl;
    }

    // 6. 状态迭代更新 (保存为全局标准量纲)
    state.qOldBytes = static_cast<double>(currentQBytes);
    state.currentFairRateBps = fNewBps;

    NS_LOG_INFO("Port " << portId << " [FRP Core] "
                << "qCur=" << qCurCell << " cells, "
                << "qRef=" << qRefCell << " cells, "
                << "qOld=" << qOldCell << " cells, "
                << "lanbackoff=" << lanbackoff << " | "
                << "NewRate=" << (fNewBps / 1e6) << " Mbps");

    return fNewBps;
}

void FrpRateCalculator::ResetPort(uint32_t portId) {
    m_portStates.erase(portId);
    NS_LOG_INFO("Port " << portId << " state reset");
}

void FrpRateCalculator::ResetAll() {
    m_portStates.clear();
    NS_LOG_INFO("All port states reset");
}

} // namespace ns3
