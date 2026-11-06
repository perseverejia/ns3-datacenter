#include <ns3/simulator.h>
#include <ns3/seq-ts-header.h>
#include <ns3/udp-header.h>
#include <ns3/ipv4-header.h>
#include "ns3/ppp-header.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "ns3/data-rate.h"
#include "ns3/pointer.h"
#include "rdma-hw.h"
#include "ppp-header.h"
#include "qbb-header.h"
#include "cn-header.h"
#include "ns3/unsched-tag.h"
#include "ns3/icmpv4.h"  // 添加ICMP FRP支持
#include "epfc.h"        // ePFC (ccMode=17)：标记位定义
#include <cmath>         // std::abs
#include <cstring>       // memcpy
// ---- 日志宏：必须在所有 #if 使用点之前定义（预处理器单遍，后定义 = 未定义 = 0）----
#define PRINT_LOG 0     // DCQCN_CNP / DCQCN 恢复事件 / EPFC_CCF_RATE 等调试日志
#define TX_RATE_LOG 1   // [TX RATE] 逐流发送速率打点（plot_tx_rate 画图依赖）

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("RdmaHw");

TypeId RdmaHw::GetTypeId (void)
{
	static TypeId tid = TypeId ("ns3::RdmaHw")
	                    .SetParent<Object> ()
	                    .AddAttribute("MinRate",
	                                  "Minimum rate of a throttled flow",
	                                  DataRateValue(DataRate("100Mb/s")),
	                                  MakeDataRateAccessor(&RdmaHw::m_minRate),
	                                  MakeDataRateChecker())
	                    .AddAttribute("Mtu",
	                                  "Mtu.",
	                                  UintegerValue(1000),
	                                  MakeUintegerAccessor(&RdmaHw::m_mtu),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute ("CcMode",
	                                   "which mode of DCQCN is running",
	                                   UintegerValue(0),
	                                   MakeUintegerAccessor(&RdmaHw::m_cc_mode),
	                                   MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("NACKGenerationInterval",
	                                  "The NACK Generation interval",
	                                  DoubleValue(500.0),
	                                  MakeDoubleAccessor(&RdmaHw::m_nack_interval),
	                                  MakeDoubleChecker<double>())
	                    .AddAttribute("L2ChunkSize",
	                                  "Layer 2 chunk size. Disable chunk mode if equals to 0.",
	                                  UintegerValue(0),
	                                  MakeUintegerAccessor(&RdmaHw::m_chunk),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("L2AckInterval",
	                                  "Layer 2 Ack intervals. Disable ack if equals to 0.",
	                                  UintegerValue(0),
	                                  MakeUintegerAccessor(&RdmaHw::m_ack_interval),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("L2BackToZero",
	                                  "Layer 2 go back to zero transmission.",
	                                  BooleanValue(false),
	                                  MakeBooleanAccessor(&RdmaHw::m_backto0),
	                                  MakeBooleanChecker())
	                    .AddAttribute("EwmaGain",
	                                  "Control gain parameter which determines the level of rate decrease",
	                                  DoubleValue(1.0 / 16),
	                                  MakeDoubleAccessor(&RdmaHw::m_g),
	                                  MakeDoubleChecker<double>())
	                    .AddAttribute ("RateOnFirstCnp",
	                                   "the fraction of rate on first CNP",
	                                   DoubleValue(1.0),
	                                   MakeDoubleAccessor(&RdmaHw::m_rateOnFirstCNP),
	                                   MakeDoubleChecker<double> ())
	                    .AddAttribute("ClampTargetRate",
	                                  "Clamp target rate.",
	                                  BooleanValue(false),
	                                  MakeBooleanAccessor(&RdmaHw::m_EcnClampTgtRate),
	                                  MakeBooleanChecker())
	                    .AddAttribute("RPTimer",
	                                  "The rate increase timer at RP in microseconds",
	                                  DoubleValue(1500.0),
	                                  MakeDoubleAccessor(&RdmaHw::m_rpgTimeReset),
	                                  MakeDoubleChecker<double>())
	                    .AddAttribute("RateDecreaseInterval",
	                                  "The interval of rate decrease check",
	                                  DoubleValue(4.0),
	                                  MakeDoubleAccessor(&RdmaHw::m_rateDecreaseInterval),
	                                  MakeDoubleChecker<double>())
	                    .AddAttribute("FastRecoveryTimes",
	                                  "The rate increase timer at RP",
	                                  UintegerValue(5),
	                                  MakeUintegerAccessor(&RdmaHw::m_rpgThreshold),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("DcqcnByteRecoveryEnabled",
	                                  "Enable an additional DCQCN rate increase for each per-QP payload-byte threshold after a rate decrease.",
	                                  BooleanValue(false),
	                                  MakeBooleanAccessor(&RdmaHw::m_dcqcnByteRecoveryEnabled),
	                                  MakeBooleanChecker())
	                    .AddAttribute("DcqcnByteRecoveryThreshold",
	                                  "Payload bytes sent per additional DCQCN rate-increase event.",
	                                  UintegerValue(150 * 1024),
	                                  MakeUintegerAccessor(&RdmaHw::m_dcqcnByteRecoveryThreshold),
	                                  MakeUintegerChecker<uint64_t>(1))
	                    .AddAttribute("AlphaResumInterval",
	                                  "The interval of resuming alpha",
	                                  DoubleValue(55.0),
	                                  MakeDoubleAccessor(&RdmaHw::m_alpha_resume_interval),
	                                  MakeDoubleChecker<double>())
	                    .AddAttribute("RateAI",
	                                  "Rate increment unit in AI period",
	                                  DataRateValue(DataRate("5Mb/s")),
	                                  MakeDataRateAccessor(&RdmaHw::m_rai),
	                                  MakeDataRateChecker())
	                    .AddAttribute("RateHAI",
	                                  "Rate increment unit in hyperactive AI period",
	                                  DataRateValue(DataRate("50Mb/s")),
	                                  MakeDataRateAccessor(&RdmaHw::m_rhai),
	                                  MakeDataRateChecker())
	                    .AddAttribute("VarWin",
	                                  "Use variable window size or not",
	                                  BooleanValue(false),
	                                  MakeBooleanAccessor(&RdmaHw::m_var_win),
	                                  MakeBooleanChecker())
	                    .AddAttribute("FastReact",
	                                  "Fast React to congestion feedback",
	                                  BooleanValue(true),
	                                  MakeBooleanAccessor(&RdmaHw::m_fast_react),
	                                  MakeBooleanChecker())
	                    .AddAttribute("MiThresh",
	                                  "Threshold of number of consecutive AI before MI",
	                                  UintegerValue(5),
	                                  MakeUintegerAccessor(&RdmaHw::m_miThresh),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("TargetUtil",
	                                  "The Target Utilization of the bottleneck bandwidth, by default 95%",
	                                  DoubleValue(0.95),
	                                  MakeDoubleAccessor(&RdmaHw::m_targetUtil),
	                                  MakeDoubleChecker<double>())
	                    .AddAttribute("UtilHigh",
	                                  "The upper bound of Target Utilization of the bottleneck bandwidth, by default 98%",
	                                  DoubleValue(0.98),
	                                  MakeDoubleAccessor(&RdmaHw::m_utilHigh),
	                                  MakeDoubleChecker<double>())
	                    .AddAttribute("RateBound",
	                                  "Bound packet sending by rate, for test only",
	                                  BooleanValue(true),
	                                  MakeBooleanAccessor(&RdmaHw::m_rateBound),
	                                  MakeBooleanChecker())
	                    .AddAttribute("MultiRate",
	                                  "Maintain multiple rates in HPCC",
	                                  BooleanValue(true),
	                                  MakeBooleanAccessor(&RdmaHw::m_multipleRate),
	                                  MakeBooleanChecker())
	                    .AddAttribute("SampleFeedback",
	                                  "Whether sample feedback or not",
	                                  BooleanValue(false),
	                                  MakeBooleanAccessor(&RdmaHw::m_sampleFeedback),
	                                  MakeBooleanChecker())
	                    .AddAttribute("TimelyAlpha",
	                                  "Alpha of TIMELY",
	                                  DoubleValue(0.875),
	                                  MakeDoubleAccessor(&RdmaHw::m_tmly_alpha),
	                                  MakeDoubleChecker<double>())
	                    .AddAttribute("TimelyBeta",
	                                  "Beta of TIMELY",
	                                  DoubleValue(0.8),
	                                  MakeDoubleAccessor(&RdmaHw::m_tmly_beta),
	                                  MakeDoubleChecker<double>())
	                    .AddAttribute("TimelyTLow",
	                                  "TLow of TIMELY (ns)",
	                                  UintegerValue(50000),
	                                  MakeUintegerAccessor(&RdmaHw::m_tmly_TLow),
	                                  MakeUintegerChecker<uint64_t>())
	                    .AddAttribute("TimelyTHigh",
	                                  "THigh of TIMELY (ns)",
	                                  UintegerValue(500000),
	                                  MakeUintegerAccessor(&RdmaHw::m_tmly_THigh),
	                                  MakeUintegerChecker<uint64_t>())
	                    .AddAttribute("TimelyMinRtt",
	                                  "MinRtt of TIMELY (ns)",
	                                  UintegerValue(20000),
	                                  MakeUintegerAccessor(&RdmaHw::m_tmly_minRtt),
	                                  MakeUintegerChecker<uint64_t>())
	                    .AddAttribute("DctcpRateAI",
	                                  "DCTCP's Rate increment unit in AI period",
	                                  DataRateValue(DataRate("1000Mb/s")),
	                                  MakeDataRateAccessor(&RdmaHw::m_dctcp_rai),
	                                  MakeDataRateChecker())
	                    .AddAttribute("PintSmplThresh",
	                                  "PINT's sampling threshold in rand()%65536",
	                                  UintegerValue(65536),
	                                  MakeUintegerAccessor(&RdmaHw::pint_smpl_thresh),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("PowerTCPEnabled", "to enable PowerTCP", BooleanValue(false), MakeBooleanAccessor(&RdmaHw::PowerTCPEnabled), MakeBooleanChecker())
	                    .AddAttribute("PowerTCPdelay", "to enable PowerTCP in delaymode", BooleanValue(false), MakeBooleanAccessor(&RdmaHw::PowerTCPdelay), MakeBooleanChecker())
	                    .AddAttribute("TxRateSampleInterval",
	                                  "TX rate sampling interval in microseconds. Set to 0 to disable.",
	                                  DoubleValue(100.0),
	                                  MakeDoubleAccessor(&RdmaHw::m_txRateSampleInterval),
	                                  MakeDoubleChecker<double>(0.0))
	                    ;
	return tid;
}

RdmaHw::RdmaHw() {
	m_epfcRttUs = 30.0; // ePFC (ccMode=17)：内部流 RTT 默认值（workload 可覆盖）
}

void RdmaHw::SetNode(Ptr<Node> node) {
	m_node = node;
}
void RdmaHw::Setup(QpCompleteCallback cb) {
	for (uint32_t i = 0; i < m_nic.size(); i++) {
		Ptr<QbbNetDevice> dev = m_nic[i].dev;
		if (dev == NULL)
			continue;
		// share data with NIC
		dev->m_rdmaEQ->m_qpGrp = m_nic[i].qpGrp;
		// setup callback
		dev->m_rdmaReceiveCb = MakeCallback(&RdmaHw::Receive, this);
		dev->m_rdmaLinkDownCb = MakeCallback(&RdmaHw::SetLinkDown, this);
		dev->m_rdmaPktSent = MakeCallback(&RdmaHw::PktSent, this);
		// config NIC
		dev->m_rdmaEQ->m_rdmaGetNxtPkt = MakeCallback(&RdmaHw::GetNxtPacket, this);
	}
	// setup qp complete callback
	m_qpCompleteCallback = cb;
}

uint32_t RdmaHw::GetNicIdxOfQp(Ptr<RdmaQueuePair> qp) {
	auto it = m_rtTable.find(qp->dip.Get());
	if (it != m_rtTable.end() && !it->second.empty()) {
		return it->second[qp->GetHash() % it->second.size()];
	} else {
		std::cerr << "[FATAL] GetNicIdxOfQp: No route to dst=" << qp->dip 
		          << " (0x" << std::hex << qp->dip.Get() << std::dec << "), "
		          << "m_rtTable size=" << m_rtTable.size() << std::endl;
		for (auto& kv : m_rtTable) {
			Ipv4Address addr(kv.first);
			std::cerr << "  route entry: dst=" << addr << " -> ports=[";
			for (size_t i = 0; i < kv.second.size(); i++) {
				if (i > 0) std::cerr << ",";
				std::cerr << kv.second[i];
			}
			std::cerr << "]" << std::endl;
		}
		NS_ASSERT_MSG(false, "No route to " << qp->dip);
	}
}
uint64_t RdmaHw::GetQpKey(uint32_t dip, uint16_t sport, uint16_t pg) {
	return ((uint64_t)dip << 32) | ((uint64_t)sport << 16) | (uint64_t)pg;
}
Ptr<RdmaQueuePair> RdmaHw::GetQp(uint32_t dip, uint16_t sport, uint16_t pg) {
	uint64_t key = GetQpKey(dip, sport, pg);
	auto it = m_qpMap.find(key);
	if (it != m_qpMap.end())
		return it->second;
	return NULL;
}
void RdmaHw::AddQueuePair(uint64_t size, uint16_t pg, Ipv4Address sip, Ipv4Address dip, uint16_t sport, uint16_t dport, uint32_t win, uint64_t baseRtt, Callback<void> notifyAppFinish, Time stopTime) {
	// create qp
	Ptr<RdmaQueuePair> qp = CreateObject<RdmaQueuePair>(pg, sip, dip, sport, dport);
	qp->SetSize(size);
	qp->SetWin(win);
	qp->SetBaseRtt(baseRtt);
	qp->SetVarWin(m_var_win);
	qp->SetAppNotifyCallback(notifyAppFinish);
	qp->stopTime = stopTime;

	if (stopTime == Simulator::GetMaximumSimulationTime()-MicroSeconds(1)){
		qp->incastFlow = 1;
	}
	else{
		qp->incastFlow = 0;
	}

	// add qp
	uint32_t nic_idx = GetNicIdxOfQp(qp);
	m_nic[nic_idx].qpGrp->AddQp(qp);
	uint64_t key = GetQpKey(dip.Get(), sport, pg);
	m_qpMap[key] = qp;


	qp->powerEnabled = PowerTCPEnabled;

	// set init variables
	if (m_nic[nic_idx].dev == NULL) {
		std::cout << "sip " << sip << " dip " << dip << " sport " << sport  << " dport " << dport << std::endl;
	}
	DataRate m_bps = m_nic[nic_idx].dev->GetDataRate();
	if(win)
		qp->SetWin(m_bps.GetBitRate() * 1 * baseRtt * 1e-9 / 8);
	qp->m_rate = m_bps;
	qp->m_max_rate = m_bps;
	if (m_cc_mode == 1 || m_cc_mode == 16 || m_cc_mode == 17) { // mlx version (16=DCI-Ext, 17=ePFC 均复用 DCQCN 发送端)
		qp->mlx.m_targetRate = m_bps;
	} else if (m_cc_mode == 3) {
		qp->hp.m_curRate = m_bps;
		if (m_multipleRate) {
			for (uint32_t i = 0; i < IntHeader::maxHop; i++)
				qp->hp.hopState[i].Rc = m_bps;
		}
	} else if (m_cc_mode == 7) {
		qp->tmly.m_curRate = m_bps;
	} else if (m_cc_mode == 10) {
		qp->hpccPint.m_curRate = m_bps;
	}

	// Notify Nic
	m_nic[nic_idx].dev->NewQp(qp);
	if (m_txRateSampleInterval > 0) {
		qp->txRate.lastSeq = qp->snd_nxt;
		qp->txRate.lastTime = Simulator::Now();
		qp->txRate.m_eventId = Simulator::Schedule(MicroSeconds(m_txRateSampleInterval),
		                                            &RdmaHw::SampleTxRate, this, qp);
	}
}

void RdmaHw::DeleteQueuePair(Ptr<RdmaQueuePair> qp) {
	// remove qp from the m_qpMap
	uint64_t key = GetQpKey(qp->dip.Get(), qp->sport, qp->m_pg);
	m_qpMap.erase(key);
}

Ptr<RdmaRxQueuePair> RdmaHw::GetRxQp(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport, uint16_t pg, bool create) {
	uint64_t key = ((uint64_t)dip << 32) | ((uint64_t)pg << 16) | (uint64_t)dport;
	auto it = m_rxQpMap.find(key);
	if (it != m_rxQpMap.end())
		return it->second;
	if (create) {
		// create new rx qp
		Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
		// init the qp
		q->sip = sip;
		q->dip = dip;
		q->sport = sport;
		q->dport = dport;
		q->m_ecn_source.qIndex = pg;
		// store in map
		m_rxQpMap[key] = q;
		return q;
	}
	return NULL;
}
uint32_t RdmaHw::GetNicIdxOfRxQp(Ptr<RdmaRxQueuePair> q) {
	auto &v = m_rtTable[q->dip];
	if (v.size() > 0) {
		return v[q->GetHash() % v.size()];
	} else {
		NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
	}
}
void RdmaHw::DeleteRxQp(uint32_t dip, uint16_t pg, uint16_t dport) {
	uint64_t key = ((uint64_t)dip << 32) | ((uint64_t)pg << 16) | (uint64_t)dport;
	m_rxQpMap.erase(key);
}

int RdmaHw::ReceiveUdp(Ptr<Packet> p, CustomHeader &ch) {
	uint8_t ecnbits = ch.GetIpv4EcnBits();

	uint32_t payload_size = p->GetSize() - ch.GetSerializedSize();

	// DCQCN debug: log ECN reception on node 4 between 12-15ms
	double now_ms = Simulator::Now().GetSeconds() * 1000.0;
	uint32_t nodeId = m_node->GetId();
	if (nodeId == 4 && now_ms >= 12.0 && now_ms <= 15.0) {
		printf("[DCQCN_ECN] %.3fms node=%u ecn=%u sip=0x%08x seq=%u\n",
			now_ms, nodeId, ecnbits, ch.sip, ch.udp.seq);
	}

	// TODO find corresponding rx queue pair
	Ptr<RdmaRxQueuePair> rxQp = GetRxQp(ch.dip, ch.sip, ch.udp.dport, ch.udp.sport, ch.udp.pg, true);

	// ePFC (ccMode=17)：拥塞贡献者标记包（IPv4 TOS bit7）→ 节流生成 CCF 回发
	if (m_cc_mode == 17 && (ch.m_tos & EPFC_MARK_MASK)) {
		EpfcCcfGen(rxQp, ch);
	}
	if (ecnbits != 0) {
		rxQp->m_ecn_source.ecnbits |= ecnbits;
		rxQp->m_ecn_source.qfb++;
	}
	rxQp->m_ecn_source.total++;
	rxQp->m_milestone_rx = m_ack_interval;

	int x = ReceiverCheckSeq(ch.udp.seq, rxQp, payload_size);
	// 按序到达时累加接收字节（用于接收侧 goodput 统计）
	// x==1: 按序+需发ACK; x==5: 按序+无需ACK
	// x==2/x==4: 乱序(不累加,等重传补洞); x==3: 重复包(不累加)
	if (x == 1 || x == 5) {
		rxQp->m_recv_bytes += payload_size;
	}
	if (x == 1 || x == 2) { //generate ACK or NACK
		qbbHeader seqh;
		seqh.SetSeq(rxQp->ReceiverNextExpectedSeq);
		seqh.SetPG(ch.udp.pg);
		seqh.SetSport(ch.udp.dport);
		seqh.SetDport(ch.udp.sport);
		seqh.SetIntHeader(ch.udp.ih);
		if (ecnbits)
			seqh.SetCnp();

		Ptr<Packet> newp = Create<Packet>(std::max(60 - 14 - 20 - (int)seqh.GetSerializedSize(), 0));
		newp->AddHeader(seqh);

		Ipv4Header head;	// Prepare IPv4 header
		head.SetDestination(Ipv4Address(ch.sip));
		head.SetSource(Ipv4Address(ch.dip));
		head.SetProtocol(x == 1 ? 0xFC : 0xFD); //ack=0xFC nack=0xFD
		head.SetTtl(64);
		head.SetPayloadSize(newp->GetSize());
		head.SetIdentification(rxQp->m_ipid++);

		newp->AddHeader(head);
		AddHeader(newp, 0x800);	// Attach PPP header
		// send
		uint32_t nic_idx = GetNicIdxOfRxQp(rxQp);
		m_nic[nic_idx].dev->RdmaEnqueueHighPrioQ(newp);
		m_nic[nic_idx].dev->TriggerTransmit();
	}
	return 0;
}

// ---------------------------------------------------------------------------
// ePFC (ccMode=17)：接收方收到标记包 → 节流生成 CCF 回发
//   CCF = PPP + IPv4(0xFB) + qbbHeader + payload{rv_bps:8B, ts_ns:8B}
//   五元组由 IP 头（src=本机接收方, dst=数据源）与 qbbHeader（端口反转）携带
//   rv = 最近采样窗口的平均接收速率（窗口 ≈ CCF 节流周期 = 1×RTT_internal）
//   沿途每个边缘交换机（下一跳 host / 接收方DCI）旁路更新拥塞数据流表
// ---------------------------------------------------------------------------
void RdmaHw::EpfcCcfGen(Ptr<RdmaRxQueuePair> rxQp, CustomHeader &ch) {
	Time now = Simulator::Now();
	// 节流：每 RTT_internal 最多一个 CCF
	if (rxQp->m_epfcLastCcfTime != Time(0) &&
	    (now - rxQp->m_epfcLastCcfTime) < MicroSeconds(m_epfcRttUs))
		return;

	// rv：上一采样窗口的平均接收速率（bps）
	double windowSec = (now - rxQp->m_epfcSampleTime).GetSeconds();
	double rvBps = 0;
	if (rxQp->m_epfcSampleTime != Time(0) && windowSec > 1e-9) {
		rvBps = (double)(rxQp->m_recv_bytes - rxQp->m_epfcSampleBytes) * 8.0 / windowSec;
	}
	rxQp->m_epfcSampleBytes = rxQp->m_recv_bytes;
	rxQp->m_epfcSampleTime = now;
	rxQp->m_epfcLastCcfTime = now;
	if (rvBps < 1e9) {
		// rv 下限保护：<1G 视为采样污染，不发 CCF：
		// - 首个采样窗口无历史（rv=0）
		// - PFC 停顿期接收侧到达≈0（这是接收侧被暂停，不是发送方超额）
		// 若此时反馈，发送方会在窗口内被钳在污染 rv 上形成自锁；
		// 采样基准已更新，下一个标记包（>=1×RTT 后）即可携带真实 rv
		return;
	}

	// 造 CCF（q=0 高优先级，与 ACK 同通道，不受数据 PG 暂停影响）
	qbbHeader seqh;
	seqh.SetSeq(0);
	seqh.SetPG(ch.udp.pg);
	seqh.SetSport(ch.udp.dport); // 端口与数据流反转（回发惯例）
	seqh.SetDport(ch.udp.sport);
	uint8_t payload[16];
	uint64_t rvU64 = (uint64_t)rvBps;
	uint64_t tsNs = (uint64_t)now.GetNanoSeconds();
	memcpy(payload, &rvU64, 8);
	memcpy(payload + 8, &tsNs, 8);
	Ptr<Packet> newp = Create<Packet>(payload, 16);
	newp->AddHeader(seqh);

	Ipv4Header head;
	head.SetDestination(Ipv4Address(ch.sip)); // 数据流源（发送方 host）
	head.SetSource(Ipv4Address(ch.dip));      // 本机（接收方 host）
	head.SetProtocol(0xFB);                   // ePFC CCF
	head.SetTtl(64);
	head.SetPayloadSize(newp->GetSize());
	head.SetIdentification(rxQp->m_ipid++);
	newp->AddHeader(head);
	AddHeader(newp, 0x800); // Attach PPP header

	uint32_t nic_idx = GetNicIdxOfRxQp(rxQp);
	m_nic[nic_idx].dev->RdmaEnqueueHighPrioQ(newp);
	m_nic[nic_idx].dev->TriggerTransmit();

	printf("[EPFC_CCF_GEN] %lu node=%u sip=0x%08x dip=0x%08x sport=%u dport=%u pg=%u rv=%.3fG\n",
		Simulator::Now().GetTimeStep(), m_node->GetId(),
		ch.sip, ch.dip, ch.udp.sport, ch.udp.dport, ch.udp.pg, rvBps / 1e9);
}

// ePFC：发送方收到 CCF → 先暂停排空，再按 rv 传输（v6.1）
//   暂停 Tp = (线速-rv)×RTT_internal/rv，上限 m_epfcMaxPauseUs（必须 < 内部 RTT，防 ACK 在暂停期内到齐饿死）：
//   线速满管时超出部分在途量 (线速-rv)×RTT 按 rv 排空所需时间，避免直接切 rv 时旧突发仍堆在瓶颈处累积
//   暂停期由 IsWinBound 停发；到期后按 rv 传输 2×RTT_internal（窗口已按 rv 缩放）
//   一个 CCF 周期 = 暂停 + 2×RTT；周期内到达的 CCF 直接丢弃，周期完整走完后才接收下一个；
//   周期过期自动失效，交回 DCQCN 全权接管，直至下一个 CCF 开启新周期
void RdmaHw::EpfcCcfRecv(Ptr<Packet> p, CustomHeader &ch) {
	if (m_cc_mode != 17)
		return;
	// 读 CCF payload 前 16B: {rv_bps, ts_ns}
	Ptr<Packet> cp = p->Copy();
	PppHeader ppp;
	Ipv4Header iph;
	qbbHeader qh;
	cp->RemoveHeader(ppp);
	cp->RemoveHeader(iph);
	cp->RemoveHeader(qh);
	uint8_t buf[16] = {0};
	cp->CopyData(buf, std::min<size_t>(16, cp->GetSize()));
	uint64_t rvU64 = 0;
	memcpy(&rvU64, buf, 8);
	double rvBps = (double)rvU64;
	if (rvBps <= 0)
		return; // rv=0 无效（发送方在 edge 判据里同样跳过）
	// 找数据流 QP：CCF 的 sip=数据流 dip，ack.dport=数据流 sport
	Ptr<RdmaQueuePair> qp = GetQp(ch.sip, ch.ack.dport, ch.ack.pg);
	if (qp == NULL)
		return;
	// 周期内丢弃：暂停+2×RTT 的 CCF 周期未走完前，后续 CCF 直接扔掉——
	// 不刷新暂停/速率/窗口，保证一个周期（暂停→按rv传输2×RTT）完整执行；
	// 否则 30μs 节拍的 CCF 会无限续借暂停期（每个 CCF 重置 32μs），流被停发锁死
	if (qp->m_epfcLimitUntil > Simulator::Now()) {
		printf("[EPFC_CCF_DROP] %lu node=%u rv=%.3fG (cycle until %luus)\n",
			Simulator::Now().GetTimeStep(), m_node->GetId(),
			rvBps / 1e9, qp->m_epfcLimitUntil.GetMicroSeconds());
		return;
	}
	// v6.1：先暂停排空再按 rv 传输（双向拉起+压下）。
	// v2 自锁教训（0.095G/10ms）已由三层保护消除：
	//   ① 接收方 rv<1G 视为采样污染不发 CCF（源头挡污染）
	//   ② 标记随拥塞消失断流，不会持续灌入污染 rv
	//   ③ 窗口过期自动放权，速率从 rv 出发交回 DCQCN 自由演进
	double lineBps = (double)m_nic[GetNicIdxOfQp(qp)].dev->GetDataRate().GetBitRate();
	// 暂停仅用于「降速排空」：当前速率高于 rv 才需要暂停清在途超额；
	// 稳态（m_rate ≤ rv，由 CNP 下限钳制保证）再暂停会让平均速率跌到 0.75×rv
	bool needDrain = qp->m_rate.GetBitRate() > rvBps;
	double pauseSec = 0;
	if (needDrain && rvBps < lineBps)
		pauseSec = (lineBps - rvBps) * (m_epfcRttUs * 1e-6) / rvBps;
	// 暂停上限：必须 < 内部流 RTT（30μs），否则短 RTT 流的在途 ACK 会在暂停期内
	// 全部到齐，暂停结束后无任何事件触发发送轮询 → 流饿死。上限 20μs = 2/3 RTT
	double pauseCapSec = m_epfcMaxPauseUs * 1e-6;
	if (pauseSec > pauseCapSec)
		pauseSec = pauseCapSec;
	qp->m_rate = DataRate((uint64_t)rvBps);
	qp->mlx.m_targetRate = DataRate((uint64_t)rvBps);
	qp->m_epfcLimitBps = rvBps; // 防爬升兜底（RateIncEventMlx 尾部钳制）
	qp->m_epfcPauseUntil = Simulator::Now() + Seconds(pauseSec);
	qp->m_epfcLimitUntil = qp->m_epfcPauseUntil + MicroSeconds(2 * m_epfcRttUs);
	// 暂停到期主动唤醒：短 RTT 流（5→6 内部 RTT=30μs ≤ 暂停 32μs）的 ACK 会在
	// 暂停期内全部到齐，到期时零在途/零待ACK/CCF 已断流，事件驱动的发送器若无
	// 此 kick，DequeueAndTransmit 永远不会再被调用（实测 5→6 因此饿死）
	Simulator::Schedule(Seconds(pauseSec), &QbbNetDevice::TriggerTransmit,
		m_nic[GetNicIdxOfQp(qp)].dev);
	#if PRINT_LOG
	printf("[EPFC_CCF_RATE] %lu node=%u rv=%.3fG rate=%.3fG tgt=%.3fG pause=%luus until=%luus\n",
		Simulator::Now().GetTimeStep(), m_node->GetId(),
		rvBps / 1e9, qp->m_rate.GetBitRate() * 1e-9,
		qp->mlx.m_targetRate.GetBitRate() * 1e-9,
		(uint64_t)(pauseSec * 1e6), qp->m_epfcLimitUntil.GetMicroSeconds());
	#endif
}

int RdmaHw::ReceiveCnp(Ptr<Packet> p, CustomHeader &ch) {
	// QCN on NIC
	// This is a Congestion signal
	// Then, extract data from the congestion packet.
	// We assume, without verify, the packet is destinated to me
	uint32_t qIndex = ch.cnp.qIndex;
	if (qIndex == 1) {		//DCTCP
		std::cout << "TCP--ignore\n";
		return 0;
	}
	uint16_t udpport = ch.cnp.fid; // corresponds to the sport
	uint8_t ecnbits = ch.cnp.ecnBits;
	uint16_t qfb = ch.cnp.qfb;
	uint16_t total = ch.cnp.total;

	uint32_t i;
	// get qp
	Ptr<RdmaQueuePair> qp = GetQp(ch.sip, udpport, qIndex);
	if (qp == NULL)
		std::cout << "ERROR: QCN NIC cannot find the flow\n";
	// get nic
	uint32_t nic_idx = GetNicIdxOfQp(qp);
	Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;

	if (qp->m_rate == 0)			//lazy initialization
	{
		qp->m_rate = dev->GetDataRate();
		if (m_cc_mode == 1 || m_cc_mode == 16 || m_cc_mode == 17) { // DCI-Ext/ePFC 复用 DCQCN 发送端
			qp->mlx.m_targetRate = dev->GetDataRate();
		} else if (m_cc_mode == 3) {
			qp->hp.m_curRate = dev->GetDataRate();
			if (m_multipleRate) {
				for (uint32_t i = 0; i < IntHeader::maxHop; i++)
					qp->hp.hopState[i].Rc = dev->GetDataRate();
			}
		} else if (m_cc_mode == 7) {
			qp->tmly.m_curRate = dev->GetDataRate();
		} else if (m_cc_mode == 10) {
			qp->hpccPint.m_curRate = dev->GetDataRate();
		}
	}
	if (m_cc_mode == 16) {
		// DCI-Ext: 发送方 DCI 的 CNP（学费/FIRST_CTL/HOLD/FAST）接入端侧
		// DCQCN 状态机——v3 重构遗漏的端侧消费路径；首次 CNP 触发 alpha=1
		// 初始化 + 启动 1us alpha 调度，后续 CNP 置位由 4us 定时器消费降速
		cnp_received_mlx(qp);
	}
	return 0;
}

int RdmaHw::ReceiveAck(Ptr<Packet> p, CustomHeader &ch) {
	uint16_t qIndex = ch.ack.pg;
	uint16_t port = ch.ack.dport;
	uint32_t seq = ch.ack.seq;
	uint8_t cnp = (ch.ack.flags >> qbbHeader::FLAG_CNP) & 1;

	// DCQCN debug: log ACK/CNP reception on node 6 between 12-15ms
	double now_ms = Simulator::Now().GetSeconds() * 1000.0;
	uint32_t nodeId = m_node->GetId();
	if (nodeId == 6 && now_ms >= 12.0 && now_ms <= 15.0) {
		printf("[DCQCN_ACK] %.3fms node=%u cnp=%u seq=%u sip=0x%08x dport=%u pg=%u\n",
			now_ms, nodeId, cnp, seq, ch.sip, port, qIndex);
	}

	int i;
	Ptr<RdmaQueuePair> qp = GetQp(ch.sip, port, qIndex);
	if (qp == NULL) {
		// DCQCN debug: log QP lookup failure
		if (m_node->GetId() == 6 && Simulator::Now().GetSeconds() * 1000.0 >= 12.0 && Simulator::Now().GetSeconds() * 1000.0 <= 15.0) {
			printf("[DCQCN_QP_MISS] %.3fms node=6 sip=0x%08x dport=%u pg=%u cnp=%u\n",
				Simulator::Now().GetSeconds() * 1000.0, ch.sip, port, qIndex, cnp);
		}
		std::cout << "ERROR: " << "node:" << m_node->GetId() << ' ' << (ch.l3Prot == 0xFC ? "ACK" : "NACK") << " NIC cannot find the flow\n";
		return 0;
	}

	uint32_t nic_idx = GetNicIdxOfQp(qp);
	Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;
	if (m_ack_interval == 0)
		std::cout << "ERROR: shouldn't receive ack\n";
	else {
		if (!m_backto0) {
			qp->Acknowledge(seq);
		} else {
			uint32_t goback_seq = seq / m_chunk * m_chunk;
			qp->Acknowledge(goback_seq);
		}
		if (qp->IsFinished()) {
			QpComplete(qp);
		}
	}
	if (ch.l3Prot == 0xFD) // NACK
		RecoverQueue(qp);

	// handle cnp
	if (cnp) {
		if (m_node->GetId() == 6) {
			printf("[DCQCN_CNP_CHECK] %.3fms node=%u cnp=%u cc_mode=%u\n",
				Simulator::Now().GetSeconds() * 1000.0, m_node->GetId(), cnp, m_cc_mode);
		}
		if (m_cc_mode == 1 || m_cc_mode == 16 || m_cc_mode == 17) { // mlx version (16=DCI-Ext, 17=ePFC)
			cnp_received_mlx(qp);
		}
	}

	if (m_cc_mode == 3) {
		HandleAckHp(qp, p, ch);
	} else if (m_cc_mode == 7) {
		HandleAckTimely(qp, p, ch);
	} else if (m_cc_mode == 8) {
		HandleAckDctcp(qp, p, ch);
	} else if (m_cc_mode == 10) {
		HandleAckHpPint(qp, p, ch);
	}
	// ACK may advance the on-the-fly window, allowing more packets to send
	dev->TriggerTransmit();
	return 0;
}

int RdmaHw::Receive(Ptr<Packet> p, CustomHeader &ch) {
	//std::cout << "[RdmaHw::Receive] node=" << m_node->GetId() << " l3Prot=0x" << std::hex << (int)ch.l3Prot << std::dec << " pktSize=" << p->GetSize() << std::endl;
	if (ch.l3Prot == 0x11) { // UDP
		ReceiveUdp(p, ch);
	} else if (ch.l3Prot == 0xFF) { // CNP
		ReceiveCnp(p, ch);
	} else if (ch.l3Prot == 0xFD) { // NACK
		ReceiveAck(p, ch);
	} else if (ch.l3Prot == 0xFC) { // ACK
		ReceiveAck(p, ch);
	} else if (ch.l3Prot == 0xFB) { // ePFC CCF：发送方限速（2×RTT 内封顶 MIN(rate, rv)）
		EpfcCcfRecv(p, ch);
	} else if (ch.l3Prot == 0x01) { // ICMP
		ReceiveIcmp(p, ch);
	}
	return 0;
}

int RdmaHw::ReceiverCheckSeq(uint32_t seq, Ptr<RdmaRxQueuePair> q, uint32_t size) {
	uint32_t expected = q->ReceiverNextExpectedSeq;
	if (seq == expected) {
		q->ReceiverNextExpectedSeq = expected + size;
		if (q->ReceiverNextExpectedSeq >= q->m_milestone_rx) {
			q->m_milestone_rx += m_ack_interval;
			return 1; //Generate ACK
		} else if (q->ReceiverNextExpectedSeq % m_chunk == 0) {
			return 1;
		} else {
			return 5;
		}
	} else if (seq > expected) {
		// Generate NACK
		if (Simulator::Now() >= q->m_nackTimer || q->m_lastNACK != expected) {
			q->m_nackTimer = Simulator::Now() + MicroSeconds(m_nack_interval);
			q->m_lastNACK = expected;
			if (m_backto0) {
				q->ReceiverNextExpectedSeq = q->ReceiverNextExpectedSeq / m_chunk * m_chunk;
			}
			return 2;
		} else
			return 4;
	} else {
		// Duplicate.
		return 3;
	}
}
void RdmaHw::AddHeader (Ptr<Packet> p, uint16_t protocolNumber) {
	PppHeader ppp;
	ppp.SetProtocol (EtherToPpp (protocolNumber));
	p->AddHeader (ppp);
}
uint16_t RdmaHw::EtherToPpp (uint16_t proto) {
	switch (proto) {
	case 0x0800: return 0x0021;   //IPv4
	case 0x86DD: return 0x0057;   //IPv6
	default: NS_ASSERT_MSG (false, "PPP Protocol number not defined!");
	}
	return 0;
}

void RdmaHw::RecoverQueue(Ptr<RdmaQueuePair> qp) {
	qp->snd_nxt = qp->snd_una;
}

void RdmaHw::QpComplete(Ptr<RdmaQueuePair> qp) {
	NS_ASSERT(!m_qpCompleteCallback.IsNull());
	if (m_cc_mode == 1 || m_cc_mode == 16 || m_cc_mode == 17) { // DCI-Ext/ePFC 复用 DCQCN
		Simulator::Cancel(qp->mlx.m_eventUpdateAlpha);
		Simulator::Cancel(qp->mlx.m_eventDecreaseRate);
		Simulator::Cancel(qp->mlx.m_rpTimer);
	}
	Simulator::Cancel(qp->txRate.m_eventId);

	// This callback will log info
	// It may also delete the rxQp on the receiver
	m_qpCompleteCallback(qp);

	qp->m_notifyAppFinish();

	// delete the qp
	DeleteQueuePair(qp);
}

void RdmaHw::SetLinkDown(Ptr<QbbNetDevice> dev) {
	printf("RdmaHw: node:%u a link down\n", m_node->GetId());
}

void RdmaHw::AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx) {
	uint32_t dip = dstAddr.Get();
	m_rtTable[dip].push_back(intf_idx);
}

void RdmaHw::ClearTable() {
	m_rtTable.clear();
}

void RdmaHw::RedistributeQp() {
	// clear old qpGrp
	for (uint32_t i = 0; i < m_nic.size(); i++) {
		if (m_nic[i].dev == NULL)
			continue;
		m_nic[i].qpGrp->Clear();
	}

	// redistribute qp
	for (auto &it : m_qpMap) {
		Ptr<RdmaQueuePair> qp = it.second;
		uint32_t nic_idx = GetNicIdxOfQp(qp);
		m_nic[nic_idx].qpGrp->AddQp(qp);
		// Notify Nic
		m_nic[nic_idx].dev->ReassignedQp(qp);
	}
}

Ptr<Packet> RdmaHw::GetNxtPacket(Ptr<RdmaQueuePair> qp) {
	uint32_t payload_size = qp->GetBytesLeft();
	if (m_mtu < payload_size)
		payload_size = m_mtu;
	qp->lastPayloadSize = payload_size;
	Ptr<Packet> p = Create<Packet> (payload_size);
	uint32_t sentBytes = qp->m_size - qp->GetBytesLeft();
	uint32_t nic_idx = GetNicIdxOfQp(qp);
	DataRate m_bps = m_nic[nic_idx].dev->GetDataRate();
	double bdp = m_bps.GetBitRate() * 1 * qp->m_baseRtt * 1e-9 / 8;
	UnSchedTag unschedtag;
	if (sentBytes <= bdp){
		unschedtag.SetValue(1);
	}
	else{
		unschedtag.SetValue(0);
	}
	p->AddPacketTag(unschedtag);
	// add SeqTsHeader
	SeqTsHeader seqTs;
	seqTs.SetSeq (qp->snd_nxt);
	seqTs.SetPG (qp->m_pg);
	p->AddHeader (seqTs);
	// add udp header
	UdpHeader udpHeader;
	udpHeader.SetDestinationPort (qp->dport);
	udpHeader.SetSourcePort (qp->sport);
	p->AddHeader (udpHeader);
	// add ipv4 header
	Ipv4Header ipHeader;
	ipHeader.SetSource (qp->sip);
	ipHeader.SetDestination (qp->dip);
	ipHeader.SetProtocol (0x11);
	ipHeader.SetPayloadSize (p->GetSize());
	ipHeader.SetTtl (64);
	ipHeader.SetTos (0);
	ipHeader.SetIdentification (qp->m_ipid);
	p->AddHeader(ipHeader);
	// add ppp header
	PppHeader ppp;
	ppp.SetProtocol (0x0021); // EtherToPpp(0x800), see point-to-point-net-device.cc
	p->AddHeader (ppp);

	// update state
	qp->snd_nxt += payload_size;
	qp->m_ipid++;

	// return
	return p;
}

void RdmaHw::PktSent(Ptr<RdmaQueuePair> qp, Ptr<Packet> pkt, Time interframeGap) {
	qp->lastPktSize = pkt->GetSize();
//	SeqTsHeader seqTs;
//	pkt->PeekHeader(seqTs);
	uint32_t seq = qp->snd_nxt;
	qp->rates[qp->snd_nxt] = Simulator::Now().GetNanoSeconds();
	UpdateNextAvail(qp, interframeGap, pkt->GetSize());
	if (m_cc_mode == 1 || m_cc_mode == 16 || m_cc_mode == 17) { // DCI-Ext/ePFC 复用 DCQCN
		RateIncByteCounterMlx(qp, qp->lastPayloadSize);
	}

}

void RdmaHw::UpdateNextAvail(Ptr<RdmaQueuePair> qp, Time interframeGap, uint32_t pkt_size) {
        Time sendingTime;
        if (m_rateBound)
                sendingTime = interframeGap + qp->m_rate.CalculateBytesTxTime(pkt_size);
        else
                sendingTime = interframeGap + qp->m_max_rate.CalculateBytesTxTime(pkt_size);
        qp->m_nextAvail = Simulator::Now() + sendingTime;
}

void RdmaHw::SampleTxRate(Ptr<RdmaQueuePair> qp) {
	if (qp == NULL || qp->IsFinished() || m_txRateSampleInterval <= 0) {
		return;
	}

	Time now = Simulator::Now();
	uint64_t curSeq = qp->snd_nxt;
	uint64_t sentBytes = curSeq >= qp->txRate.lastSeq ? curSeq - qp->txRate.lastSeq : 0;
	double dtSec = (now - qp->txRate.lastTime).GetSeconds();
	double rateBps = dtSec > 0 ? static_cast<double>(sentBytes) * 8.0 / dtSec : 0.0;

	// Keep DCQCN queue logs and TX RATE logs on stdout.  Both use the same
	// stdio stream, so each printf call is serialized instead of being
	// interleaved after the runner redirects stdout and stderr to one file.
	#if TX_RATE_LOG
	printf("[TX RATE] Host %u sip %u dip %u sport %u dport %u pg %u txRate %.0fbps rt %.0fbps t %luns\n",
	        m_node->GetId(),
	        qp->sip.Get(), qp->dip.Get(),
	        qp->sport, qp->dport, qp->m_pg,
	        rateBps, (double)qp->mlx.m_targetRate.GetBitRate(), (unsigned long long)now.GetNanoSeconds());
	#endif

	qp->txRate.lastSeq = curSeq;
	qp->txRate.lastTime = now;
	qp->txRate.m_eventId = Simulator::Schedule(MicroSeconds(m_txRateSampleInterval),
	                                            &RdmaHw::SampleTxRate, this, qp);
}
void RdmaHw::ChangeRate(Ptr<RdmaQueuePair> qp, DataRate new_rate) {
#if 1
	Time sendingTime = qp->m_rate.CalculateBytesTxTime(qp->lastPktSize);
	Time new_sendintTime = new_rate.CalculateBytesTxTime(qp->lastPktSize);
	qp->m_nextAvail = qp->m_nextAvail + new_sendintTime - sendingTime;
	// update nic's next avail event
	uint32_t nic_idx = GetNicIdxOfQp(qp);
	m_nic[nic_idx].dev->UpdateNextAvail(qp->m_nextAvail);
#endif
	// change to new rate
	qp->m_rate = new_rate;
}

/******************************
 * Mellanox's version of DCQCN
 *****************************/
void RdmaHw::UpdateAlphaMlx(Ptr<RdmaQueuePair> q) {
#if PRINT_LOG
	//std::cout << Simulator::Now() << " alpha update:" << m_node->GetId() << ' ' << q->mlx.m_alpha << ' ' << (int)q->mlx.m_alpha_cnp_arrived << '\n';
	//printf("%lu alpha update: %08x %08x %u %u %.6lf->", Simulator::Now().GetTimeStep(), q->sip.Get(), q->dip.Get(), q->sport, q->dport, q->mlx.m_alpha);
#endif
	if (q->mlx.m_alpha_cnp_arrived) {
		q->mlx.m_alpha = (1 - m_g) * q->mlx.m_alpha + m_g; 	//binary feedback
	} else {
		q->mlx.m_alpha = (1 - m_g) * q->mlx.m_alpha; 	//binary feedback
	}
#if PRINT_LOG
	//printf("%.6lf\n", q->mlx.m_alpha);
#endif
	q->mlx.m_alpha_cnp_arrived = false; // clear the CNP_arrived bit
	ScheduleUpdateAlphaMlx(q);
}
void RdmaHw::ScheduleUpdateAlphaMlx(Ptr<RdmaQueuePair> q) {
	q->mlx.m_eventUpdateAlpha = Simulator::Schedule(MicroSeconds(m_alpha_resume_interval), &RdmaHw::UpdateAlphaMlx, this, q);
}

void RdmaHw::cnp_received_mlx(Ptr<RdmaQueuePair> q) {
	#if PRINT_LOG
	printf("[DCQCN_CNP] %.3fms node=%u cnp_received sip=0x%08x dip=0x%08x sport=%u dport=%u first_cnp=%d alpha=%.4f rate=%.3fG\n",
		Simulator::Now().GetSeconds() * 1000.0, m_node->GetId(), q->sip.Get(), q->dip.Get(), q->sport, q->dport,
		q->mlx.m_first_cnp, q->mlx.m_alpha, q->m_rate.GetBitRate() * 1e-9);
	#endif
	q->mlx.m_alpha_cnp_arrived = true; // set CNP_arrived bit for alpha update
	q->mlx.m_decrease_cnp_arrived = true; // set CNP_arrived bit for rate decrease
	if (q->mlx.m_first_cnp) {
		// init alpha
		q->mlx.m_alpha = 1;
		q->mlx.m_alpha_cnp_arrived = false;
		// schedule alpha update
		ScheduleUpdateAlphaMlx(q);
		// schedule rate decrease
		ScheduleDecreaseRateMlx(q, 1); // add 1 ns to make sure rate decrease is after alpha update
		// set rate on first CNP
		q->mlx.m_targetRate = q->m_rate = m_rateOnFirstCNP * q->m_rate;
		q->mlx.m_first_cnp = false;
	}
}

void RdmaHw::CheckRateDecreaseMlx(Ptr<RdmaQueuePair> q) {
	ScheduleDecreaseRateMlx(q, 0);
	if (q->mlx.m_decrease_cnp_arrived) {
#if PRINT_LOG
		printf("%lu rate dec: %08x %08x %u %u (%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), q->sip.Get(), q->dip.Get(), q->sport, q->dport, q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
#endif
		bool clamp = true;
		if (!m_EcnClampTgtRate) {
			if (q->mlx.m_rpTimeStage == 0)
				clamp = false;
		}
		if (clamp) {
			// ccMode=16：Rt 下限 = 1/4 链路带宽——CLAMP=1 把 Rt 锁到砍穿后的低速率，
			// 会让 FastRecovery 5 档全部向地板逼近（域内流 ~300ms 慢爬）；
			// 下限保证砍穿后 3~4 档（~4ms）即可回到 1/4 带宽平台
			if (m_cc_mode == 16) {
				uint32_t nic_idx = GetNicIdxOfQp(q);
				Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;
				DataRate floorRt(dev->GetDataRate().GetBitRate() / 4);
				q->mlx.m_targetRate = std::max(q->m_rate, floorRt);
			} else {
				q->mlx.m_targetRate = q->m_rate;
			}
		}
		// ePFC (ccMode=17)：CCF 设速窗口内 CNP 降速下限钳制在 rv——
		// CCF 每 90μs 才设速一次（周期机制），CNP 持续到达持续降速，降多设少会净向下漂移；
		// 窗口期由 ePFC 主导（CCF 即速率指令），DCQCN 降速不得低于 rv
		DataRate decFloor = m_minRate;
		if (m_cc_mode == 17 && q->m_epfcLimitUntil > Simulator::Now() && q->m_epfcLimitBps > 0)
			decFloor = DataRate((uint64_t)q->m_epfcLimitBps);
		q->m_rate = std::max(decFloor, q->m_rate * (1 - q->mlx.m_alpha / 2));
		// reset rate increase related things
		q->mlx.m_rpTimeStage = 0;
		q->mlx.m_rpCountStage = 0;
		q->mlx.m_rpByteCount = 0;
		q->mlx.m_byteRecoveryActive = m_dcqcnByteRecoveryEnabled;
		q->mlx.m_decrease_cnp_arrived = false;
		Simulator::Cancel(q->mlx.m_rpTimer);
		q->mlx.m_rpTimer = Simulator::Schedule(MicroSeconds(m_rpgTimeReset), &RdmaHw::RateIncEventTimerMlx, this, q);
#if PRINT_LOG
		printf("(%.3lf %.3lf)\n", q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
#endif
	}
}
void RdmaHw::ScheduleDecreaseRateMlx(Ptr<RdmaQueuePair> q, uint32_t delta) {
	q->mlx.m_eventDecreaseRate = Simulator::Schedule(MicroSeconds(m_rateDecreaseInterval) + NanoSeconds(delta), &RdmaHw::CheckRateDecreaseMlx, this, q);
}

void RdmaHw::RateIncEventTimerMlx(Ptr<RdmaQueuePair> q) {
	q->mlx.m_rpTimer = Simulator::Schedule(MicroSeconds(m_rpgTimeReset), &RdmaHw::RateIncEventTimerMlx, this, q);
	RateIncEventMlx(q, q->mlx.m_rpTimeStage);
	q->mlx.m_rpTimeStage++;
}
void RdmaHw::RateIncByteCounterMlx(Ptr<RdmaQueuePair> q, uint32_t payloadBytes) {
	if (!m_dcqcnByteRecoveryEnabled || !q->mlx.m_byteRecoveryActive || payloadBytes == 0) {
		return;
	}
	q->mlx.m_rpByteCount += payloadBytes;
	while (q->mlx.m_rpByteCount >= m_dcqcnByteRecoveryThreshold) {
		q->mlx.m_rpByteCount -= m_dcqcnByteRecoveryThreshold;
		RateIncEventMlx(q, q->mlx.m_rpCountStage);
		q->mlx.m_rpCountStage++;
	}
}
void RdmaHw::RateIncEventMlx(Ptr<RdmaQueuePair> q, uint32_t stage) {
	// check which increase phase: fast recovery, active increase, hyper increase
	if (stage < m_rpgThreshold) { // fast recovery
		FastRecoveryMlx(q);
	} else if (stage == m_rpgThreshold) { // active increase
		ActiveIncreaseMlx(q);
	} else { // hyper increase
		HyperIncreaseMlx(q);
	}
	// ePFC (ccMode=17)：CCF 设速窗口（2×RTT_internal）内增速钳制在 rv——防爬升兜底；
	// 设速本体在 EpfcCcfRecv（m_rate = tgt = rv）；CNP 降速不受影响
	if (m_cc_mode == 17 && q->m_epfcLimitUntil > Simulator::Now() && q->m_epfcLimitBps > 0) {
		double cap = q->m_epfcLimitBps;
		if (q->m_rate.GetBitRate() > cap)
			q->m_rate = DataRate((uint64_t)cap);
		if (q->mlx.m_targetRate.GetBitRate() > cap)
			q->mlx.m_targetRate = DataRate((uint64_t)cap);
	}
}

void RdmaHw::FastRecoveryMlx(Ptr<RdmaQueuePair> q) {
#if PRINT_LOG
	printf("%lu fast recovery: %08x %08x %u %u (%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), q->sip.Get(), q->dip.Get(), q->sport, q->dport, q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
#endif
	q->m_rate = (q->m_rate / 2) + (q->mlx.m_targetRate / 2);
#if PRINT_LOG
	printf("(%.3lf %.3lf)\n", q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
#endif
}
void RdmaHw::ActiveIncreaseMlx(Ptr<RdmaQueuePair> q) {
#if PRINT_LOG
	printf("%lu active inc: %08x %08x %u %u (%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), q->sip.Get(), q->dip.Get(), q->sport, q->dport, q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
#endif
	// get NIC
	uint32_t nic_idx = GetNicIdxOfQp(q);
	Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;
	// increate rate
	q->mlx.m_targetRate += m_rai;
	if (q->mlx.m_targetRate > dev->GetDataRate())
		q->mlx.m_targetRate = dev->GetDataRate();
	q->m_rate = (q->m_rate / 2) + (q->mlx.m_targetRate / 2);
#if PRINT_LOG
	printf("(%.3lf %.3lf)\n", q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
#endif
}
void RdmaHw::HyperIncreaseMlx(Ptr<RdmaQueuePair> q) {
#if PRINT_LOG
	printf("%lu hyper inc: %08x %08x %u %u (%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), q->sip.Get(), q->dip.Get(), q->sport, q->dport, q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
#endif
	// get NIC
	uint32_t nic_idx = GetNicIdxOfQp(q);
	Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;
	// increate rate
	q->mlx.m_targetRate += m_rhai;
	if (q->mlx.m_targetRate > dev->GetDataRate())
		q->mlx.m_targetRate = dev->GetDataRate();
	q->m_rate = (q->m_rate / 2) + (q->mlx.m_targetRate / 2);
#if PRINT_LOG
	printf("(%.3lf %.3lf)\n", q->mlx.m_targetRate.GetBitRate() * 1e-9, q->m_rate.GetBitRate() * 1e-9);
#endif
}

/***********************
 * High Precision CC
 ***********************/
void RdmaHw::HandleAckHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch) {
	uint32_t ack_seq = ch.ack.seq;
	// update rate
	if (ack_seq > qp->hp.m_lastUpdateSeq) { // if full RTT feedback is ready, do full update
		if (PowerTCPEnabled) {
			UpdateRatePower(qp, p, ch, false);
		}
		else
			UpdateRateHp(qp, p, ch, false);
	} else { // do fast react
		if (PowerTCPEnabled)
			FastReactPower(qp, p, ch);
		else
			FastReactHp(qp, p, ch);
	}
}

void RdmaHw::UpdateRateHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool fast_react) {
	uint32_t next_seq = qp->snd_nxt;
	bool print = !fast_react || true;


	if (qp->hp.m_lastUpdateSeq == 0) { // first RTT

		qp->hp.m_lastUpdateSeq = next_seq;
		// store INT
		IntHeader &ih = ch.ack.ih;
		NS_ASSERT(ih.nhop <= IntHeader::maxHop);
		for (uint32_t i = 0; i < ih.nhop; i++)
			qp->hp.hop[i] = ih.hop[i];
#if PRINT_LOG
		if (print) {
			printf("%lu %s %08x %08x %u %u [%u,%u,%u]", Simulator::Now().GetTimeStep(), fast_react ? "fast" : "update", qp->sip.Get(), qp->dip.Get(), qp->sport, qp->dport, qp->hp.m_lastUpdateSeq, ch.ack.seq, next_seq);
			for (uint32_t i = 0; i < ih.nhop; i++)
				printf(" %u %lu %lu", ih.hop[i].GetQlen(), ih.hop[i].GetBytes(), ih.hop[i].GetTime());
			printf("\n");
		}
#endif
	} else {
		// check packet INT
		IntHeader &ih = ch.ack.ih;
		if (ih.nhop <= IntHeader::maxHop) {
			double max_c = 0;
			bool inStable = false;
#if PRINT_LOG
			if (print)
				printf("%lu %s %08x %08x %u %u [%u,%u,%u]", Simulator::Now().GetTimeStep(), fast_react ? "fast" : "update", qp->sip.Get(), qp->dip.Get(), qp->sport, qp->dport, qp->hp.m_lastUpdateSeq, ch.ack.seq, next_seq);
#endif
			// check each hop
			double U = 0;
			uint64_t dt = 0;
			bool updated[IntHeader::maxHop] = {false}, updated_any = false;
			NS_ASSERT(ih.nhop <= IntHeader::maxHop);
			for (uint32_t i = 0; i < ih.nhop; i++) {
				if (m_sampleFeedback) {
					if (ih.hop[i].GetQlen() == 0 and fast_react)
						continue;
				}
				updated[i] = updated_any = true;
#if PRINT_LOG
				if (print)
					printf(" %u(%u) %lu(%lu) %lu(%lu)", ih.hop[i].GetQlen(), qp->hp.hop[i].GetQlen(), ih.hop[i].GetBytes(), qp->hp.hop[i].GetBytes(), ih.hop[i].GetTime(), qp->hp.hop[i].GetTime());
#endif
				uint64_t tau = ih.hop[i].GetTimeDelta(qp->hp.hop[i]);
				double duration = tau * 1e-9;
				double txRate = (ih.hop[i].GetBytesDelta(qp->hp.hop[i])) * 8 / duration;

				double u;
				u = txRate / ih.hop[i].GetLineRate() + (double)std::min(ih.hop[i].GetQlen(), qp->hp.hop[i].GetQlen()) * qp->m_max_rate.GetBitRate() / ih.hop[i].GetLineRate() / qp->m_win;


#if PRINT_LOG
				if (print)
					printf(" %.3lf %.3lf", txRate, u);
#endif
				if (!m_multipleRate) {
					// for aggregate (single R)
					if (u > U) {
						U = u;
						dt = tau;
					}
				} else {
					// for per hop (per hop R)
					if (tau > qp->m_baseRtt)
						tau = qp->m_baseRtt;
					qp->hp.hopState[i].u = (qp->hp.hopState[i].u * (qp->m_baseRtt - tau) + u * tau) / double(qp->m_baseRtt);
				}
				qp->hp.hop[i] = ih.hop[i];
			}

			DataRate new_rate;
			int32_t new_incStage;
			DataRate new_rate_per_hop[IntHeader::maxHop];
			int32_t new_incStage_per_hop[IntHeader::maxHop];
			if (!m_multipleRate) {
				// for aggregate (single R)
				if (updated_any) {
					if (dt > 1.0 * qp->m_baseRtt)
						dt = 1.0 * qp->m_baseRtt;


					qp->hp.u = (qp->hp.u * (qp->m_baseRtt - dt) + U * dt) / double(qp->m_baseRtt);
					max_c = qp->hp.u / m_targetUtil;

					if (max_c >= 1 || qp->hp.m_incStage >= m_miThresh) {
						new_rate = qp->hp.m_curRate / max_c + m_rai;
						new_incStage = 0;
					}
					else {
						new_rate = qp->hp.m_curRate + m_rai;
						new_incStage = qp->hp.m_incStage + 1;
					}

					if (new_rate < m_minRate)
						new_rate = m_minRate;
					if (new_rate > qp->m_max_rate)
						new_rate = qp->m_max_rate;
#if PRINT_LOG
					if (print)
						printf(" u=%.6lf U=%.3lf dt=%u max_c=%.3lf", qp->hp.u, U, dt, max_c);
#endif
#if PRINT_LOG
					if (print)
						printf(" rate:%.3lf->%.3lf\n", qp->hp.m_curRate.GetBitRate() * 1e-9, new_rate.GetBitRate() * 1e-9);
#endif
				}
			} else {
				// for per hop (per hop R)
				new_rate = qp->m_max_rate;
				for (uint32_t i = 0; i < ih.nhop; i++) {
					if (updated[i]) {
						double c = qp->hp.hopState[i].u / m_targetUtil;
						if (c >= 1 || qp->hp.hopState[i].incStage >= m_miThresh) {
							new_rate_per_hop[i] = qp->hp.hopState[i].Rc / c + m_rai;
							new_incStage_per_hop[i] = 0;
						} else {
							new_rate_per_hop[i] = qp->hp.hopState[i].Rc + m_rai;
							new_incStage_per_hop[i] = qp->hp.hopState[i].incStage + 1;
						}
						// bound rate
						if (new_rate_per_hop[i] < m_minRate)
							new_rate_per_hop[i] = m_minRate;
						if (new_rate_per_hop[i] > qp->m_max_rate)
							new_rate_per_hop[i] = qp->m_max_rate;
						// find min new_rate
						if (new_rate_per_hop[i] < new_rate)
							new_rate = new_rate_per_hop[i];
#if PRINT_LOG
						if (print)
							printf(" [%u]u=%.6lf c=%.3lf", i, qp->hp.hopState[i].u, c);
#endif
#if PRINT_LOG
						if (print)
							printf(" %.3lf->%.3lf", qp->hp.hopState[i].Rc.GetBitRate() * 1e-9, new_rate.GetBitRate() * 1e-9);
#endif
					} else {
						if (qp->hp.hopState[i].Rc < new_rate)
							new_rate = qp->hp.hopState[i].Rc;
					}
				}
#if PRINT_LOG
				printf("\n");
#endif
			}

			if (updated_any) {
				ChangeRate(qp, new_rate);
			}
			if (!fast_react) {
				if (updated_any) {
					qp->hp.m_curRate = new_rate;
					qp->hp.m_incStage = new_incStage;
				}
				if (m_multipleRate) {
					// for per hop (per hop R)
					for (uint32_t i = 0; i < ih.nhop; i++) {
						if (updated[i]) {
							qp->hp.hopState[i].Rc = new_rate_per_hop[i];
							qp->hp.hopState[i].incStage = new_incStage_per_hop[i];
						}
					}
				}
			}
		}
		if (!fast_react) {
			if (next_seq > qp->hp.m_lastUpdateSeq)
				qp->hp.m_lastUpdateSeq = next_seq; //+ rand() % 2 * m_mtu;
		}
	}
}

void RdmaHw::FastReactHp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch) {
	if (m_fast_react)
		UpdateRateHp(qp, p, ch, true);
}


/**********************
 * PowerTCP (Int/Delay versions) called from HandleAckHp function at the moment
 *********************/

void RdmaHw::UpdateRatePower(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool fast_react) {
	uint32_t next_seq = qp->snd_nxt;
	bool print = !fast_react || true;
	double prevRtt = qp->m_baseRtt;
	double prevCompletion = Simulator::Now().GetNanoSeconds();
	std::map<uint32_t, double>::iterator it = qp->rates.find(ch.ack.seq);
	DataRate old ;
	double rtt;

	if (it != qp->rates.end()) {
		prevRtt = Simulator::Now().GetNanoSeconds() - it->second;
		if (PowerTCPdelay) {
			qp->m_baseRtt = std::min(uint64_t(Simulator::Now().GetNanoSeconds() - it->second), qp->m_baseRtt);
		}
		prevCompletion = Simulator::Now().GetNanoSeconds();
        qp->rates.erase(it);
	}
	// check packet INT
	IntHeader &ih = ch.ack.ih;
	if (ih.nhop <= IntHeader::maxHop) {
		double max_c = 0;
		bool inStable = false;
		// check each hop
		double U = 0;
		uint64_t dt = 0;
		bool updated[IntHeader::maxHop] = {false}, updated_any = false;
		NS_ASSERT(ih.nhop <= IntHeader::maxHop);
		for (uint32_t i = 0; i < ih.nhop; i++) {
			if (m_sampleFeedback) {
				if (ih.hop[i].GetQlen() == 0 and fast_react)
					continue;
			}
			updated[i] = updated_any = true;

			uint64_t tau = ih.hop[i].GetTimeDelta(qp->hp.hop[i]);
			double duration = tau * 1e-9;
			double rxRate = (ih.hop[i].GetBytesDelta(qp->hp.hop[i])) * 8.0 / duration;
			double u;

			if (!PowerTCPdelay) {
				double A = rxRate;
				if (A < ih.hop[i].GetLineRate()*0.5 )
					A = ih.hop[i].GetLineRate()*0.5;
				// Power = (dequeue rate + queue gradient) * (queue length + bandwidth * baseRtt)
				// Same as Power = (Arrival rate) * (queue length + bandwidth * baseRtt)
				double power = ( A ) * (double(ih.hop[i].GetQlen() * 8.0) + ih.hop[i].GetLineRate() * (qp->m_baseRtt * 1e-9));
				double powerx = (power) / (ih.hop[i].GetLineRate() * (ih.hop[i].GetLineRate() * qp->m_baseRtt * 1e-9) );
				u = powerx; // PowerTCP
			}
			else {
				// delay approach
				double A = ( double(prevRtt - qp->prevRtt) / (prevCompletion - qp->prevCompletion) + 1  );
				if (A < 0.5)
					A = 0.5;
				double power = ( A ) * (prevRtt);
				double powerx = (power) / (1.05 * qp->m_baseRtt);
				u = powerx; // theta-PowerTCP
			}
			if (u > U) {
				U = u;
				if (PowerTCPdelay) {
					dt = prevCompletion - qp->prevCompletion;
				}
				else {
					dt = tau;
				}
			}
			qp->hp.hop[i] = ih.hop[i];
		}

		DataRate new_rate;
		int32_t new_incStage;
		DataRate new_rate_per_hop[IntHeader::maxHop];
		int32_t new_incStage_per_hop[IntHeader::maxHop];

		if (updated_any) {
			if (dt > 1.0 * qp->m_baseRtt)
				dt = 1.0 * qp->m_baseRtt;

			if (U < 0) {
				U = qp->hp.u;
			}
			qp->hp.u = (qp->hp.u * (1.0 * qp->m_baseRtt - dt) + U * dt) / double(1.0 * qp->m_baseRtt);
			if (!PowerTCPdelay) {
				max_c = qp->hp.u / m_targetUtil;
				new_rate = (0.9 * ( qp->hp.m_curRate / max_c + DataRate("150Mbps") ) + 0.1 * qp->hp.m_curRate);

			}
			else {
				max_c = qp->hp.u;
				new_rate = (0.7 * ( qp->hp.m_curRate / max_c + DataRate("150Mbps") ) + 0.3 * qp->hp.m_curRate);
			}
			if (new_rate < m_minRate)
				new_rate = m_minRate;
			if (new_rate > qp->m_max_rate)
				new_rate = qp->m_max_rate;
		}
		qp->prevRtt = prevRtt;
		qp->prevCompletion = Simulator::Now().GetNanoSeconds();
		if (updated_any) {
			ChangeRate(qp, new_rate);
		}
		if (!fast_react) {
			if (updated_any) {
				qp->hp.m_curRate = new_rate;
				qp->hp.m_incStage = new_incStage;
			}
		}
		if (!fast_react) {
			if (next_seq > qp->hp.m_lastUpdateSeq)
				qp->hp.m_lastUpdateSeq = next_seq;
		}
	}
}

void RdmaHw::FastReactPower(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch) {
	if (m_fast_react)
		UpdateRatePower(qp, p, ch, true);
}

/**********************
 * TIMELY
 *********************/
void RdmaHw::HandleAckTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch) {
	uint32_t ack_seq = ch.ack.seq;
	// update rate
	if (ack_seq > qp->tmly.m_lastUpdateSeq) { // if full RTT feedback is ready, do full update
		UpdateRateTimely(qp, p, ch, false);
	} else { // do fast react
		FastReactTimely(qp, p, ch);
	}
}
void RdmaHw::UpdateRateTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool us) {
	uint32_t next_seq = qp->snd_nxt;
	uint64_t rtt = Simulator::Now().GetTimeStep() - ch.ack.ih.ts;
	bool print = !us;
	if (qp->tmly.m_lastUpdateSeq != 0) { // not first RTT
		int64_t new_rtt_diff = (int64_t)rtt - (int64_t)qp->tmly.lastRtt;
		double rtt_diff = (1 - m_tmly_alpha) * qp->tmly.rttDiff + m_tmly_alpha * new_rtt_diff;
		double gradient = rtt_diff / m_tmly_minRtt;
		bool inc = false;
		double c = 0;
#if PRINT_LOG
		if (print)
			printf("%lu node:%u rtt:%lu rttDiff:%.0lf gradient:%.3lf rate:%.3lf", Simulator::Now().GetTimeStep(), m_node->GetId(), rtt, rtt_diff, gradient, qp->tmly.m_curRate.GetBitRate() * 1e-9);
#endif
		if (rtt < m_tmly_TLow) {
			inc = true;
		} else if (rtt > m_tmly_THigh) {
			c = 1 - m_tmly_beta * (1 - (double)m_tmly_THigh / rtt);
			inc = false;
		} else if (gradient <= 0) {
			inc = true;
		} else {
			c = 1 - m_tmly_beta * gradient;
			if (c < 0)
				c = 0;
			inc = false;
		}
		if (inc) {
			if (qp->tmly.m_incStage < 5) {
				qp->m_rate = qp->tmly.m_curRate + m_rai;
			} else {
				qp->m_rate = qp->tmly.m_curRate + m_rhai;
			}
			if (qp->m_rate > qp->m_max_rate)
				qp->m_rate = qp->m_max_rate;
			if (!us) {
				qp->tmly.m_curRate = qp->m_rate;
				qp->tmly.m_incStage++;
				qp->tmly.rttDiff = rtt_diff;
			}
		} else {
			qp->m_rate = std::max(m_minRate, qp->tmly.m_curRate * c);
			if (!us) {
				qp->tmly.m_curRate = qp->m_rate;
				qp->tmly.m_incStage = 0;
				qp->tmly.rttDiff = rtt_diff;
			}
		}
#if PRINT_LOG
		if (print) {
			printf(" %c %.3lf\n", inc ? '^' : 'v', qp->m_rate.GetBitRate() * 1e-9);
		}
#endif
	}
	if (!us && next_seq > qp->tmly.m_lastUpdateSeq) {
		qp->tmly.m_lastUpdateSeq = next_seq;
		// update
		qp->tmly.lastRtt = rtt;
	}
}
void RdmaHw::FastReactTimely(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch) {
}


/**********************
 * DCTCP
 *********************/
void RdmaHw::HandleAckDctcp(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch) {
	uint32_t ack_seq = ch.ack.seq;
	uint8_t cnp = (ch.ack.flags >> qbbHeader::FLAG_CNP) & 1;
	bool new_batch = false;

	// update alpha
	qp->dctcp.m_ecnCnt += (cnp > 0);
	if (ack_seq > qp->dctcp.m_lastUpdateSeq) { // if full RTT feedback is ready, do alpha update
#if PRINT_LOG
		printf("%lu %s %08x %08x %u %u [%u,%u,%u] %.3lf->", Simulator::Now().GetTimeStep(), "alpha", qp->sip.Get(), qp->dip.Get(), qp->sport, qp->dport, qp->dctcp.m_lastUpdateSeq, ch.ack.seq, qp->snd_nxt, qp->dctcp.m_alpha);
#endif
		new_batch = true;
		if (qp->dctcp.m_lastUpdateSeq == 0) { // first RTT
			qp->dctcp.m_lastUpdateSeq = qp->snd_nxt;
			qp->dctcp.m_batchSizeOfAlpha = qp->snd_nxt / m_mtu + 1;
		} else {
			double frac = std::min(1.0, double(qp->dctcp.m_ecnCnt) / qp->dctcp.m_batchSizeOfAlpha);
			qp->dctcp.m_alpha = (1 - m_g) * qp->dctcp.m_alpha + m_g * frac;
			qp->dctcp.m_lastUpdateSeq = qp->snd_nxt;
			qp->dctcp.m_ecnCnt = 0;
			qp->dctcp.m_batchSizeOfAlpha = (qp->snd_nxt - ack_seq) / m_mtu + 1;
#if PRINT_LOG
			printf("%.3lf F:%.3lf", qp->dctcp.m_alpha, frac);
#endif
		}
#if PRINT_LOG
		printf("\n");
#endif
	}

	// check cwr exit
	if (qp->dctcp.m_caState == 1) {
		if (ack_seq > qp->dctcp.m_highSeq)
			qp->dctcp.m_caState = 0;
	}

	// check if need to reduce rate: ECN and not in CWR
	if (cnp && qp->dctcp.m_caState == 0) {
#if PRINT_LOG
		printf("%lu %s %08x %08x %u %u %.3lf->", Simulator::Now().GetTimeStep(), "rate", qp->sip.Get(), qp->dip.Get(), qp->sport, qp->dport, qp->m_rate.GetBitRate() * 1e-9);
#endif
		qp->m_rate = std::max(m_minRate, qp->m_rate * (1 - qp->dctcp.m_alpha / 2));
#if PRINT_LOG
		printf("%.3lf\n", qp->m_rate.GetBitRate() * 1e-9);
#endif
		qp->dctcp.m_caState = 1;
		qp->dctcp.m_highSeq = qp->snd_nxt;
	}

	// additive inc
	if (qp->dctcp.m_caState == 0 && new_batch)
		qp->m_rate = std::min(qp->m_max_rate, qp->m_rate + m_dctcp_rai);
}

/*********************
 * HPCC-PINT
 ********************/
void RdmaHw::SetPintSmplThresh(double p) {
	pint_smpl_thresh = (uint32_t)(65536 * p);
}
void RdmaHw::HandleAckHpPint(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch) {
	uint32_t ack_seq = ch.ack.seq;
	if (rand() % 65536 >= pint_smpl_thresh)
		return;
	// update rate
	if (ack_seq > qp->hpccPint.m_lastUpdateSeq) { // if full RTT feedback is ready, do full update
		UpdateRateHpPint(qp, p, ch, false);
	} else { // do fast react
		UpdateRateHpPint(qp, p, ch, true);
	}
}

void RdmaHw::UpdateRateHpPint(Ptr<RdmaQueuePair> qp, Ptr<Packet> p, CustomHeader &ch, bool fast_react) {
	uint32_t next_seq = qp->snd_nxt;
	if (qp->hpccPint.m_lastUpdateSeq == 0) { // first RTT
		qp->hpccPint.m_lastUpdateSeq = next_seq;
	} else {
		// check packet INT
		IntHeader &ih = ch.ack.ih;
		double U = Pint::decode_u(ih.GetPower());

		DataRate new_rate;
		int32_t new_incStage;
		double max_c = U / m_targetUtil;

		if (max_c >= 1 || qp->hpccPint.m_incStage >= m_miThresh) {
			new_rate = qp->hpccPint.m_curRate / max_c + m_rai;
			new_incStage = 0;
		} else {
			new_rate = qp->hpccPint.m_curRate + m_rai;
			new_incStage = qp->hpccPint.m_incStage + 1;
		}
		if (new_rate < m_minRate)
			new_rate = m_minRate;
		if (new_rate > qp->m_max_rate)
			new_rate = qp->m_max_rate;
		ChangeRate(qp, new_rate);
		if (!fast_react) {
			qp->hpccPint.m_curRate = new_rate;
			qp->hpccPint.m_incStage = new_incStage;
		}
		if (!fast_react) {
			if (next_seq > qp->hpccPint.m_lastUpdateSeq)
				qp->hpccPint.m_lastUpdateSeq = next_seq; //+ rand() % 2 * m_mtu;
		}
	}
}

/**
 * 接收ICMP包并分发到具体处理函数
 */
int RdmaHw::ReceiveIcmp(Ptr<Packet> p, CustomHeader &ch) {
	// FRP反馈包的完整结构: [CustomHeader][ICMP Header][Icmpv4FrpFeedback]
	// IP封装: SIP=数据流目的IP, DIP=数据流源IP(=自己)
	// 发送方可由 DIP(=自己) + SIP(=数据流目的IP) 精确匹配QP

	// 移除CustomHeader
	p->RemoveHeader(ch);

	// 移除ICMP头部
	Icmpv4Header icmpHeader;
	p->RemoveHeader(icmpHeader);

	// 解析FRP payload
	Icmpv4FrpFeedback frpFb;
	if (p->GetSize() >= frpFb.GetSerializedSize()) {
		p->RemoveHeader(frpFb);

		// 提取FRP参数
		uint16_t fairRate = frpFb.GetFairRate();
		int16_t qDev = frpFb.GetQDepth();       // q_dev = qCur - qRef (int16_t, can be negative)
		uint16_t cpId = frpFb.GetCpId();
		bool type = frpFb.GetType();
		uint16_t linkRate = frpFb.GetLinkRate();

		// 解析IP头: SIP=数据流目的IP, DIP=数据流源IP(=自己)
		Ipv4Address flowDstIp = Ipv4Address(ch.sip);   // FRP包的SIP = 数据流的目的IP
		Ipv4Address flowSrcIp = Ipv4Address(ch.dip);   // FRP包的DIP = 数据流的源IP(=自己)

		// 用 dip=flowDstIp 精确匹配QP
		for (auto& kv : m_qpMap) {
			Ptr<RdmaQueuePair> qp = kv.second;
			if (qp->dip.Get() == flowDstIp.Get()) {
				HandleFrpFeedback(qp, fairRate, qDev, cpId, type, linkRate, flowDstIp);
			}
		}

		return 0;
	}

	// 不是FRP包，忽略
	return 0;
}


void RdmaHw::HandleFrpFeedback(Ptr<RdmaQueuePair> qp, uint16_t fairRate, int16_t qDev, uint16_t cpId, bool type, uint16_t linkRate, Ipv4Address flowDstIp) {
	// 1. 还原为全局标准单位 (bps, Bytes)
	double fairRateBps = static_cast<double>(fairRate) * 10000000.0;       // 10Mbps -> bps
	double qDevBytes  = static_cast<double>(qDev) * 600.0;  // 600B Cell -> Byte (qDev is int16_t, can be negative)
	double linkRateBps = static_cast<double>(linkRate) * 10000000.0;      // 10Mbps -> bps
	double maxRateBps  = linkRateBps * 0.95;
	double minRateBps  = 100* 1000000.0;  // 100 Mbps
	double T = 40e-6;         // 40微秒 = 0.00004秒

	// 跟踪所有接收到的FRP的最小linkRate (静态变量，跨调用保持)
	static double minLinkRateObservedBps = 1e18;
	if (linkRateBps < minLinkRateObservedBps) {
		minLinkRateObservedBps = linkRateBps;
	}
	// 使用历史最小linkRate作为maxRateBps
	maxRateBps = minLinkRateObservedBps * 0.95;

	// 输出FRP接收日志：记录瓶颈节点ID、链路速率、公平速率
	static uint32_t recvCounter = 0;
	if (recvCounter++ < 500) {  // 扩大到500条，能看到跨DC流完整生命周期
		fprintf(stderr, "[FRP_RECV_DETAIL] t=%.3fms Host=%u flow=%u->%u cpId=%u linkRate=%u*10Mbps(%.2fGbps) fairRate=%u*10Mbps(%.2fGbps) qDev=%dcells(%.2fKB) type=%d\n",
			Simulator::Now().GetSeconds() * 1000.0,
			m_node->GetId(),
			qp->sip.Get(), qp->dip.Get(),
			cpId,
			linkRate, linkRateBps/1e9,
			fairRate, fairRateBps/1e9,
			qDev, qDevBytes/1024.0,
			type ? 1 : 0);
	}

	// 2. 判断是否为退避的内部流
	//    IP格式：11.{dc_id}.*.*  ->  dc_id = (ip >> 16) & 0xFF
	uint8_t srcDcId = (qp->sip.Get() >> 16) & 0xFF;
	uint8_t dstDcId = (flowDstIp.Get() >> 16) & 0xFF;
	bool isBackoffInternalFlow = (srcDcId == dstDcId);
//  bool isBackoffInternalFlow = type && (srcDcId == dstDcId);
	// 3. 计算新速率 r
	double rBps;
	double currentRateBps = qp->m_rate.GetBitRate();

	if (type==1 || !isBackoffInternalFlow) {
		// 非退避流：r = F（公平速率）
		rBps = fairRateBps;
	} else {
		// 退避的内部流
		double qThBytes = 300.0 * 1024.0;  
		double deltaR = 1000.0 * 1000000.0;  // 1Gbps
		double N = (fairRate > 0) ? (static_cast<double>(linkRate) / static_cast<double>(fairRate)) : 100.0;
			if (N < 1.0) N = 1.0;
			if (N > 10.0) N = 10.0;
		if (qDevBytes < qThBytes) {
			// 策略1: 平滑调整
			rBps = fairRateBps - qDevBytes / ( N * T ) * 8.0;
			
		} else {
			// 策略2: 基于公平速率的退避 r = max(F_min, F - gamma*qDev)
			
			double scaleFactor = 20.0;
			double gamma = scaleFactor / N;
			// gamma*qDev 在 600B Cell 单位下计算，再转为 bps
			// r = F - gamma * qDev (qDev是cell数，gamma是无量纲，结果在10Mbps单位)
			double fairRate10Mbps = static_cast<double>(fairRate);
			double r_10Mbps = fairRate10Mbps - gamma * static_cast<double>(qDev);
			rBps = r_10Mbps * 10000000.0;
			if (rBps < minRateBps) rBps = minRateBps;
		}
	}

	// 4. 速率边界裁剪
	if (rBps > maxRateBps) rBps = maxRateBps;
	if (rBps < minRateBps) rBps = minRateBps;

	// 5. 瓶颈节点更新逻辑 + 定时器管理
	//    若 r < 当前速率 或 cpId == 上次瓶颈cpId，则更新瓶颈并调速
	bool shouldUpdateBottleneck = (rBps < currentRateBps) || (cpId == qp->frp.m_bottleneckCpId);
	
	if (shouldUpdateBottleneck) {
		// 只有当速率真正改变时才重置定时器
		bool rateChanged = (std::abs(rBps - currentRateBps) > 1e6);  // 1Mbps阈值
		bool cpIdChanged = (cpId != qp->frp.m_bottleneckCpId);
		
		qp->frp.m_bottleneckCpId = cpId;
		qp->frp.m_bottleneckRate = DataRate(rBps);
		
		if (rateChanged) {
			// 速率改变，重置定时器
			qp->frp.m_lastBottleneckUpdate = Simulator::Now();
			qp->frp.m_timerExpired = false;
			
			
		} else if (cpIdChanged) {
			// 瓶颈节点改变但速率相同，也重置定时器
			qp->frp.m_lastBottleneckUpdate = Simulator::Now();
			qp->frp.m_timerExpired = false;
		
		}
		// 如果速率和瓶颈节点都未改变，不重置定时器，让定时器继续计时
		
		ChangeRate(qp, DataRate(rBps));
	} else {
		// 检查定时器是否超时（0.1ms = 100微秒）
		Time timeout = MicroSeconds(100);  // 0.1ms定时器
		if (qp->frp.m_lastBottleneckUpdate.IsZero() || 
		    (Simulator::Now() - qp->frp.m_lastBottleneckUpdate) > timeout) {
			qp->frp.m_timerExpired = true;
		}
	}
	
	// 6. 定时器超时后的增速策略
	if (qp->frp.m_timerExpired && !shouldUpdateBottleneck) {
		double timeoutRateBps;
		if (type) {
			// ROCC：超时后直接翻倍
			timeoutRateBps = currentRateBps * 2.0;
		} else {
			// FRP：区分广域流和内部流
			if (!isBackoffInternalFlow) {
				// 广域流（跨DC）：r = 当前速率 + deltaR
				double deltaR = 200.0 * 1000000.0;  // 200 Mbps
				timeoutRateBps = currentRateBps + deltaR;
			} else {
				// 内部流（同DC）：r = 当前速率 * 2
				timeoutRateBps = currentRateBps * 2.0;
			}
		}
		// 速率边界裁剪
		if (timeoutRateBps > maxRateBps) timeoutRateBps = maxRateBps;
		if (timeoutRateBps < minRateBps) timeoutRateBps = minRateBps;
		
		// 应用增速
		ChangeRate(qp, DataRate(timeoutRateBps));
		qp->frp.m_timerExpired = false;  // 重置定时器标志
		
		// 更新日志中的r值
		rBps = timeoutRateBps;
		
		// 输出超时增速日志
		std::cout << "[FRP TIMEOUT] Host " << m_node->GetId() 
		          << " qp=" << qp->sip << "->" << qp->dip
		          << " timeout growth: curRate=" << (currentRateBps/1e6) << "Mbps"
		          << " -> newRate=" << (timeoutRateBps/1e6) << "Mbps"
		          << " (flowtype=" << (!isBackoffInternalFlow ? "WAN" : "LAN") << ")"
		          << std::endl;
	}

	// 调试输出（使用stderr确保无缓冲）
	fprintf(stderr, "[FRP RATE] Host %u qp=%u->%u F=%.2fG qDev=%dcells(%.2fKB) type=%d backoff=%d srcDc=%u dstDc=%u curRate=%.2fG r=%.2fG %s\n",
		m_node->GetId(),
		qp->sip.Get(), qp->dip.Get(),
		fairRateBps/1e9,
		qDev, qDevBytes/1024.0,
		type ? 1 : 0,
		isBackoffInternalFlow ? 1 : 0,
		srcDcId, dstDcId,
		currentRateBps/1e9,
		rBps/1e9,
		shouldUpdateBottleneck ? "UPDATED" : "SKIP");
}

}
