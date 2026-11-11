#ifndef SWITCH_NODE_H
#define SWITCH_NODE_H

#include <unordered_map>
#include <set>
#include <ns3/node.h>
#include "qbb-net-device.h"
#include "switch-mmu.h"
#include "pint.h"
#include "frp-rate-calculator.h"
#include "ns3/atcGateway.h"
#include "dci-ext.h"
#include "epfc.h"
#include "grc-wan-routing.h"
#include "themis-gateway.h"

namespace ns3 {

class Packet;

// ========== ATC Gateway 配置结构 ==========
struct AtcConfig {
    uint32_t voq_max_size = 0;
    double cnp_notify_interval_us = 0.0;
    std::string min_rate = "";
    double rate_decrease_interval_us = 0.0;
    double rpg_time_reset_us = 0.0;
    bool cnp_on_global_congestion = false;
    bool voq_react_to_cnp = false;
    double voq_inflight_rtt_us = 0.0;
    uint32_t max_voq_count = 0;
    uint64_t max_voq_rate = 0;
    uint64_t long_haul_bandwidth = 0;
    Time long_haul_delay = MicroSeconds(0);
};

class SwitchNode : public Node{
	static const uint32_t pCnt = 257;	// Number of ports used
	static const uint32_t qCnt = 8;	// Number of queues/priorities used
	uint32_t m_ecmpSeed;
	std::unordered_map<uint32_t, std::vector<int> > m_rtTable; // map from ip address (u32) to possible ECMP port (index of dev)

	// monitor of PFC
	uint32_t m_bytes[pCnt][pCnt][qCnt]; // m_bytes[inDev][outDev][qidx] is the bytes from inDev enqueued for outDev at qidx
	
	uint64_t m_txBytes[pCnt]; // counter of tx bytes


	uint32_t m_lastPktSize[pCnt];
	uint64_t m_lastPktTs[pCnt]; // ns
	double m_u[pCnt];

protected:
    void DoDispose() override;
	bool m_ecnEnabled;
	uint32_t m_ccMode;
	uint64_t m_maxRtt;

	uint32_t m_ackHighPrio; // set high priority for ACK/NACK

	// vamsi
	bool PowerEnabled;

	// ========== Bifrost (ccMode=12) 周期PFC机制 ==========
	bool m_bifrostEnabled;              // 是否启用Bifrost周期PFC
	uint32_t m_bifrostPfcPeriodUs;      // PFC发送周期（微秒）
	uint32_t m_bifrostDeploySwitchId;   // Bifrost部署的交换机ID（从配置读取）
	uint32_t m_bifrostLastPfcTimeUs;    // 上次PFC发送时间（微秒）
	EventId m_bifrostPfcEvent;          // 周期性PFC事件

	// ========== ATC (ccMode=15) Gateway 配置 ==========
	AtcConfig m_atcConfig;              // ATC 配置参数
	uint32_t m_gatewayType;             // Gateway 类型: 0=none, 1=source, 2=destination, 3=bidirectional
	std::string m_configFilePath;       // 配置文件路径（用于读取算法参数）

	// ========== 通用反馈包机制 ==========
	bool m_switchFeedbackEnabled;      // 是否启用交换机反馈
	Time m_feedbackInterval;           // 反馈周期
	// 出口网卡 -> 活跃流信息集合
	// 每条流记录: (srcIp=数据流源IP, dstIp=数据流目的IP)
	// FRP包封装: SIP=dstIp, DIP=srcIp，帮助接收方用SIP+DIP匹配QP
	struct FlowEndpoints {
		Ipv4Address srcIp;   // 数据流源IP -> 作为FRP包的DIP
		Ipv4Address dstIp;   // 数据流目的IP -> 作为FRP包的SIP
		bool operator<(const FlowEndpoints& o) const {
			if (srcIp.Get() != o.srcIp.Get()) return srcIp.Get() < o.srcIp.Get();
			return dstIp.Get() < o.dstIp.Get();
		}
	};
	std::map<uint32_t, std::set<FlowEndpoints>> m_activeFlows;     // 出口网卡 -> 活跃流端点集合
	std::map<uint32_t, std::set<uint8_t>> m_activeSrcDcIds;      // 出口网卡 -> 活跃源IP的DC-ID集合
	Ipv4Address m_switchRealIp;        // 交换机的真实IP地址
	
	// FRP公平速率计算器
	FrpRateCalculator m_frpCalculator;  // 独立的FRP算法模块

	// 跨域流连续识别计数器 (per port)：仅在两个条件同时成立时累加，任一不满足则重置
	// 达到阈值后调流端抑制 lanbackoff
	std::map<uint32_t, uint32_t> m_crossDcCounter;

	// 上周期出口队列深度 (per port)，供跨域流识别条件对比 qCur > qOld 使用
	std::map<uint32_t, uint32_t> m_prevQBytes;

private:
	int GetOutDev(Ptr<const Packet>, CustomHeader &ch);
	Ptr<QbbNetDevice> GetOutDevice(Ptr<Packet> p, CustomHeader &ch);
	void SendToDev(Ptr<Packet>p, CustomHeader &ch);
    Ptr<GrcWanRouting> m_grc;
    Ptr<ThemisGateway> m_themis;
	static uint32_t EcmpHash(const uint8_t* key, size_t len, uint32_t seed);
	void CheckAndSendPfc(uint32_t inDev, uint32_t qIndex);
	void CheckAndSendResume(uint32_t inDev, uint32_t qIndex);
	
	// ========== 通用反馈包核心函数 ==========
	Ptr<Packet> ConfigureFeedbackPayload(uint32_t ccMode, uint32_t ifIndex);
	void SendControlPacket(Ipv4Address srcAddr, Ipv4Address dstAddr, Ptr<Packet> payload, uint8_t l3Prot);
public:
	// 调试接口：打印路由表
	std::unordered_map<uint32_t, std::vector<int> >* GetRtTablePtr() { return &m_rtTable; }
	void PrintRoutingTableFor(Ipv4Address targetDst);
private:
	void PeriodicFeedbackLoop(Time interval);
	void TrackActiveFlow(Ptr<Packet> p, uint32_t outPort);
	bool CheckHasWan(Ipv4Address srcIp);
public:
	Ptr<SwitchMmu> m_mmu;
	atcGateway m_atcGateway;

	// ========== DCI-Ext (ccMode=16) 成员 ==========
	DciExt m_dciExt;
	bool m_dciExtEnabled;
	std::set<uint32_t> m_pfcDisabledPorts;  // 禁用 PFC 发送的端口集合（DCI 长链路端口防死锁）

	// ========== ePFC (ccMode=17) 成员 ==========
	Epfc m_epfc;               // 三角色合一：打标 / 接收方DCI流表 / 出口端口门控
	bool m_epfcEnabled;

	static TypeId GetTypeId (void);
	SwitchNode();
	Ptr<ThemisGateway> InitializeThemis(uint32_t dc,
	                                  const std::map<uint32_t, uint32_t>& hostIpToDc,
	                                  const ThemisSettings& settings);
	void SetEcmpSeed(uint32_t seed);
	void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
	void ClearTable();
	uint32_t GetRouteTableSize() const { return m_rtTable.size(); }
	const std::unordered_map<uint32_t, std::vector<int>>& GetRouteTable() const { return m_rtTable; }
	void SetSwitchRealIp(Ipv4Address ip);  // 设置交换机真实IP
	Ipv4Address GetSwitchRealIp() const;   // 获取交换机真实IP
	bool SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch);
	void SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p);
	
	// ========== 统一公开接口 ==========
	void StartPeriodicFeedbackMechanism(Time interval);

	// ========== Bifrost (ccMode=12) 接口 ==========
	void StartBifrostPfcMechanism();                                            // 启动Bifrost周期PFC机制
	void SendBifrostPfcWithCustomTime();                                        // 发送带自定义暂停时间的PFC
	uint32_t CalculateBifrostPauseTime(uint32_t qIndex, uint32_t qLenBytes, uint32_t r);  // 计算暂停时间（r=上一个周期收到的字节数）
	void ReadBifrostConfigFromFile(const std::string& configFile);              // 从配置文件读取 Bifrost 参数
	void InitializeBifrost(uint32_t switchId);                                  // 初始化 Bifrost（在 DCI 交换机上启用）

	// ========== ATC (ccMode=15) Gateway 接口 ==========
	void SetConfigFilePath(const std::string& configPath);                    // 设置配置文件路径
	void ConfigureAtc(const AtcConfig& config);                                 // 配置 ATC 参数
	void InitializeAtcGateway(uint32_t gatewayType);                            // 初始化 ATC Gateway (1=source, 2=destination, 3=bidirectional)
	void ReadAtcConfigFromFile(const std::string& configFile);                  // 从配置文件读取 ATC 参数

	// ========== DCI-Ext (ccMode=16) 接口 ==========
	void InitializeDciExt(uint32_t nodeId, uint32_t burstIntervalUs, double periodTUs,
	                      double dipBwGbps, uint64_t observeMb,
	                      uint32_t agingPeriods, bool senderSide = false,
	                      double dtUs = 4.0, double sigmaUs = 3.0);   // 初始化 DCI-Ext v3（发送方表A/观察期/ECN反馈，接收方RTT探测+周期0xFA）
	Ptr<NetDevice> GetDevByIndex(uint32_t ifIndex);                            // 端口设备查询（DCI-Ext NotifyDequeue 判长链路用）
	void DisablePfcOnPort(uint32_t port);                                       // 禁用指定端口的 PFC 发送（DCI 长链路端口防死锁）

	// ========== ePFC (ccMode=17) 接口 ==========
	void InitializeEpfc(uint32_t nodeId, uint32_t ingressMulPct, uint64_t egressThreshBytes,
	                    uint64_t dciThreshBytes, double rttUs, double agingRtts,
	                    double updateTUs);   // 初始化 ePFC（ccMode=17 时所有交换机部署）
	void EpfcGateHelper(uint32_t portIdx, uint8_t pg, bool on);  // ePFC 门控回调：本地门控指定端口（SetDciExtGate）
	void EpfcDequeueHelper(uint32_t outPort, Ptr<Packet> p);  // ePFC 出队计量回调：读 Tag 调 DataDequeue（S 口径=物理发出）

	// ========== GRC (ccMode=18) 初始化（仅 DCI） ==========
	void InitializeGrc(uint32_t dc, const std::map<uint32_t, uint32_t>& routes);

	// for approximate calc in PINT
	int logres_shift(int b, int l);
	int log2apprx(int x, int b, int m, int l); // given x of at most b bits, use most significant m bits of x, calc the result in l bits
};

} /* namespace ns3 */

#endif /* SWITCH_NODE_H */
