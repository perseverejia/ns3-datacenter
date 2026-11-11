#include "ns3/ipv4.h"
#include <iomanip>
#include "ns3/packet.h"
#include "ns3/ipv4-header.h"
#include "ns3/pause-header.h"
#include "ns3/interface-tag.h"
#include "ns3/boolean.h"
#include "ns3/uinteger.h"
#include "ns3/double.h"
#include "switch-node.h"
#include "grc-settings.h"
#include "grc-wan-routing.h"
#include "qbb-net-device.h"
#include "ppp-header.h"
#include "ns3/int-header.h"
#include "ns3/simulator.h"
#include <cmath>
#include "ns3/tcp-header.h"
#include "ns3/udp-header.h"
#include "ns3/custom-priority-tag.h"
#include "ns3/feedback-tag.h"
#include "ns3/unsched-tag.h"
#include "ns3/icmpv4.h"  // 添加ICMP FRP支持

// DCQCN队列长度监控：指定交换机的出口队列长度
// 通过配置文件 QLEN_MON_SWITCHES 指定监控的交换机 ID 列表
// 如果列表为空，则监控所有交换机
std::set<uint32_t> qlen_mon_switches;

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("SwitchNode");

TypeId SwitchNode::GetTypeId (void)
{
	static TypeId tid = TypeId ("ns3::SwitchNode")
	                    .SetParent<Node> ()
	                    .AddConstructor<SwitchNode> ()
	                    .AddAttribute("EcnEnabled",
	                                  "Enable ECN marking.",
	                                  BooleanValue(false),
	                                  MakeBooleanAccessor(&SwitchNode::m_ecnEnabled),
	                                  MakeBooleanChecker())
	                    .AddAttribute("CcMode",
	                                  "CC mode.",
	                                  UintegerValue(0),
	                                  MakeUintegerAccessor(&SwitchNode::m_ccMode),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("AckHighPrio",
	                                  "Set high priority for ACK/NACK or not",
	                                  UintegerValue(0),
	                                  MakeUintegerAccessor(&SwitchNode::m_ackHighPrio),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("MaxRtt",
	                                  "Max Rtt of the network",
	                                  UintegerValue(9000),
	                                  MakeUintegerAccessor(&SwitchNode::m_maxRtt),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("PowerEnabled",
	                                  "Inserts Rxbytes instead of Txbytes in INT header",
	                                  BooleanValue(false),
	                                  MakeBooleanAccessor(&SwitchNode::PowerEnabled),
	                                  MakeBooleanChecker())
	                    
	                    // ========== Bifrost (ccMode=12) 属性 ==========
	                    .AddAttribute("BifrostEnabled",
	                                  "Enable Bifrost periodic PFC mechanism (only for designated switch)",
	                                  BooleanValue(false),
	                                  MakeBooleanAccessor(&SwitchNode::m_bifrostEnabled),
	                                  MakeBooleanChecker())
	                    .AddAttribute("BifrostPfcPeriodUs",
	                                  "Bifrost PFC sending period in microseconds",
	                                  UintegerValue(10),
	                                  MakeUintegerAccessor(&SwitchNode::m_bifrostPfcPeriodUs),
	                                  MakeUintegerChecker<uint32_t>())
	                    .AddAttribute("BifrostDeploySwitchId",
	                                  "Switch ID where Bifrost is deployed",
	                                  UintegerValue(13),
	                                  MakeUintegerAccessor(&SwitchNode::m_bifrostDeploySwitchId),
	                                  MakeUintegerChecker<uint32_t>())
	                    
	                    // ========== 通用反馈包属性 ==========
	                    .AddAttribute("SwitchFeedbackEnabled",
	                                  "Enable switch periodic feedback mechanism",
	                                  BooleanValue(false),
	                                  MakeBooleanAccessor(&SwitchNode::m_switchFeedbackEnabled),
	                                  MakeBooleanChecker())
	                    .AddAttribute("FeedbackInterval",
	                                  "Feedback generation interval",
	                                  TimeValue(MicroSeconds(100)),
	                                  MakeTimeAccessor(&SwitchNode::m_feedbackInterval),
	                                  MakeTimeChecker())
	                    
	                    // ========== ATC (ccMode=15) Gateway 属性 ==========
	                    .AddAttribute("GatewayType",
	                                  "Gateway type for ATC (0=none, 1=source, 2=destination, 3=bidirectional)",
	                                  UintegerValue(0),
	                                  MakeUintegerAccessor(&SwitchNode::m_gatewayType),
	                                  MakeUintegerChecker<uint32_t>())
	
	                    ;
	return tid;
}

SwitchNode::SwitchNode() {
	m_ecmpSeed = m_id;
	m_node_type = 1;
	m_mmu = CreateObject<SwitchMmu>();
	for (uint32_t i = 0; i < pCnt; i++)
		for (uint32_t j = 0; j < pCnt; j++)
			for (uint32_t k = 0; k < qCnt; k++)
				m_bytes[i][j][k] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_txBytes[i] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_lastPktSize[i] = m_lastPktTs[i] = 0;
	for (uint32_t i = 0; i < pCnt; i++)
		m_u[i] = 0;
	

	m_atcGateway.setSwitchSendToDevCallback(MakeCallback(&SwitchNode::SendToDev,this));
	m_atcGateway.setGetOutDevCallback(MakeCallback(&SwitchNode::GetOutDevice,this));
	// ========== 通用反馈包初始化 ==========
	m_switchFeedbackEnabled = false;
	m_feedbackInterval = MicroSeconds(10);
	m_activeFlows.clear();
	m_activeSrcDcIds.clear();
	m_switchRealIp = Ipv4Address("0.0.0.0");  // 默认IP

	// ========== Bifrost (ccMode=12) 初始化 ==========
	m_bifrostEnabled = false;
	m_bifrostPfcPeriodUs = 10;
	m_bifrostLastPfcTimeUs = 0;
	m_bifrostPfcEvent = EventId();

	// ========== ATC (ccMode=15) 初始化 ==========
	m_gatewayType = 0; // 默认不是 gateway

	// ========== DCI-Ext (ccMode=16) 初始化（默认关闭，惰性） ==========
	m_dciExtEnabled = false;
	m_dciExt.SetSendCallback(MakeCallback(&SwitchNode::SendToDev, this));
	m_dciExt.SetGetOutDevCallback(MakeCallback(&SwitchNode::GetOutDevice, this));
	m_dciExt.SetGetDevByIndexCallback(MakeCallback(&SwitchNode::GetDevByIndex, this));

	// ========== ePFC (ccMode=17) 初始化（默认关闭，惰性） ==========
	m_epfcEnabled = false;
	m_epfc.SetGateCallback(MakeCallback(&SwitchNode::EpfcGateHelper, this));
}

void SwitchNode::InitializeGrc(uint32_t dc, const std::map<uint32_t, uint32_t>& routes) {
	if (m_ccMode != 18) return;
	NS_ABORT_MSG_IF(!GrcSettings::enabled || m_grc, "GRC must be configured once before initialization");
	m_grc = CreateObject<GrcWanRouting>();
	m_grc->SetSwitchSendToDevCallback(MakeCallback(&SwitchNode::SendToDev, this));
	m_grc->Init(this, dc, routes);
}

Ptr<ThemisGateway> SwitchNode::InitializeThemis(uint32_t dc,
                                             const std::map<uint32_t, uint32_t>& hostIpToDc,
                                             const ThemisSettings& settings) {
	NS_ABORT_MSG_IF(m_themis, "THEMIS initialized twice");
	m_themis = CreateObject<ThemisGateway>();
	m_themis->Configure(this, dc, hostIpToDc, settings,
	                    MakeCallback(&SwitchNode::SendToDev, this));
	return m_themis;
}

void SwitchNode::DoDispose() {
	if (m_themis) {
		m_themis->Dispose();
		m_themis = nullptr;
	}
	Node::DoDispose();
}

int SwitchNode::GetOutDev(Ptr<const Packet> p, CustomHeader &ch) {
	// look up entries
	Ptr<Packet> cp = p->Copy();

	PppHeader ph; cp->RemoveHeader(ph);
	Ipv4Header ih; cp->RemoveHeader(ih);
	auto entry = m_rtTable.find(ih.GetDestination().Get());

	// no matching entry
	if (entry == m_rtTable.end())
		return -1;

	// entry found
	auto &nexthops = entry->second;

	// pick one next hop based on hash
	union {
		uint8_t u8[4 + 4 + 2 + 2];
		uint32_t u32[3];
	} buf = {};
	buf.u32[0] = ih.GetSource().Get();
	buf.u32[1] = ih.GetDestination().Get();
	if (ih.GetProtocol() == 0x6) {
		TcpHeader th; cp->PeekHeader(th);
		buf.u32[2] = th.GetSourcePort() | ((uint32_t)th.GetDestinationPort() << 16);
	}
	else if (ch.l3Prot == 0x11) {
		buf.u32[2] = ch.udp.sport | ((uint32_t)ch.udp.dport << 16);
	}
	else if (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD)
		buf.u32[2] = ch.ack.sport | ((uint32_t)ch.ack.dport << 16);
	else if (ch.l3Prot == 0xFF)
		buf.u32[2] = ch.cnp.fid | ((uint32_t)ch.cnp.qIndex << 16);

	uint32_t idx = EcmpHash(buf.u8, 12, m_ecmpSeed) % nexthops.size();
	// if (nexthops.size()>1){ std::cout << "selected " << idx << std::endl; }
	return nexthops[idx];
}

Ptr<QbbNetDevice>  SwitchNode::GetOutDevice(Ptr< Packet> p, CustomHeader &ch) {
	int idx = GetOutDev(p, ch);
	if (idx >= 0) {
		return DynamicCast<QbbNetDevice>(m_devices[idx]);
	}

	return nullptr;
}

void SwitchNode::CheckAndSendPfc(uint32_t inDev, uint32_t qIndex) {
	// DCI 长链路端口 PFC 禁用检查：防止 ACK 回路被阻断导致死锁
	if (m_pfcDisabledPorts.count(inDev))
		return;
	Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[inDev]);
	if (m_mmu->CheckShouldPause(inDev, qIndex)) {
		// PFC 触发观测：入口队列字节数、动态门限与 headroom 占用量（type 0 = LOSSLESS）
		printf("[PFC_TRIGGER] %lu switch=%u port=%u q=%u ingress_bytes=%lu threshold=%lu xoffUsed=%lu action=PAUSE\n",
			Simulator::Now().GetTimeStep(), m_id, inDev, qIndex,
			m_mmu->ingress_bytes[inDev][qIndex],
			m_mmu->Threshold(inDev, qIndex, "ingress", 0, 0),
			m_mmu->GetHdrmBytes(inDev, qIndex));
		device->SendPfc(qIndex, 0);
		// std::cout << "sending PFC" << std::endl;
		m_mmu->SetPause(inDev, qIndex);
	}
}
void SwitchNode::CheckAndSendResume(uint32_t inDev, uint32_t qIndex) {
	Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[inDev]);
	if (m_mmu->CheckShouldResume(inDev, qIndex)) {
		printf("[PFC_TRIGGER] %lu switch=%u port=%u q=%u ingress_bytes=%lu threshold=%lu xoffUsed=%lu action=RESUME\n",
			Simulator::Now().GetTimeStep(), m_id, inDev, qIndex,
			m_mmu->ingress_bytes[inDev][qIndex],
			m_mmu->Threshold(inDev, qIndex, "ingress", 0, 0),
			m_mmu->GetHdrmBytes(inDev, qIndex));
		device->SendPfc(qIndex, 1);
		m_mmu->SetResume(inDev, qIndex);
	}
}

void SwitchNode::SendToDev(Ptr<Packet>p, CustomHeader &ch) {
	int idx = GetOutDev(p, ch);
	if (idx >= 0) {
		NS_ASSERT_MSG(m_devices[idx]->IsLinkUp(), "The routing table look up should return link that is up");

		// determine the qIndex
		uint32_t qIndex=0;
		MyPriorityTag priotag;
		// IMPORTANT: MyPriorityTag should only be attached by lossy traffic. This tag indicates the qIndex but also indicates that it is "lossy". Never attach MyPriorityTag on lossless traffic.
		bool found = p->PeekPacketTag(priotag);

		// UnSchedTag is used by ABM. End-hosts explicitly tag packets of the first BDP so that ABM then prioritizes these packets in the buffer allocation.
		uint32_t unsched = 0;
		UnSchedTag tag;
		bool foundunSched = p->PeekPacketTag (tag);
		if (foundunSched) {
			unsched = tag.GetValue();
		}

		if (ch.l3Prot == 0xFF || ch.l3Prot == 0xFE || ch.l3Prot == 0xFB || ch.l3Prot == 0xFA || (m_ackHighPrio && (ch.l3Prot == 0xFD || ch.l3Prot == 0xFC))) { //QCN or PFC or ePFC-CCF or DCI-Ext DIP反馈 or NACK, go highest priority
			qIndex = 0;
		}
		else if (found) {
			qIndex = priotag.GetPriority();
			// std::cout << "using queue " << qIndex << std::endl;
		}
		else if (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD) {
			qIndex = ch.ack.pg;
		}
		else {
			qIndex = (ch.l3Prot == 0x06 ? 1 : ch.udp.pg); // For TCP/IP if the stack did not attach MyPriorityTag, put to queue 1.
		}

		// admission control
		InterfaceTag t;
		bool hasIngressTag = p->PeekPacketTag(t);
		uint32_t inDev = hasIngressTag ? t.GetPortId() : idx;

		// ePFC (ccMode=17): 打标判定 / CCF边缘建表 / 边缘S累计+超标评估
		// 位置说明：在 admission control 之前 —— MarkJudge 读到的是本包 admission 前的
		// ingress/egress 字节数（与 PFC CheckShouldPause 同刻判定）；不拦截，始终放行
		if (m_epfcEnabled) {
			m_epfc.PacketIn(inDev, idx, qIndex, p, ch);
		}
			
		// Bifrost: 统计ingress端口收到的字节数（仅在部署Bifrost的交换机上）
		if (m_bifrostEnabled && m_id == m_bifrostDeploySwitchId && inDev < m_devices.size()) {
			DynamicCast<QbbNetDevice>(m_devices[inDev])->totalBytesRcvd += p->GetSize();
		}
			
		if (qIndex != 0 && hasIngressTag) { //not highest priority
			// IMPORTANT: MyPriorityTag should only be attached by lossy traffic. This tag indicates the qIndex but also indicates that it is "lossy". Never attach MyPriorityTag on lossless traffic.
			if (m_mmu->CheckIngressAdmission(inDev, qIndex, p->GetSize(), found,unsched) && m_mmu->CheckEgressAdmission(idx, qIndex, p->GetSize(), found,unsched)) {			// Admission control
				m_mmu->UpdateIngressAdmission(inDev, qIndex, p->GetSize(), found, unsched);
				m_mmu->UpdateEgressAdmission(idx, qIndex, p->GetSize(), found);
			} else {
				if (m_ccMode == 18 && GrcLog::events && ch.l3Prot == 0x11)
                    GrcLog::Event("drop", GetId(), ch.sip, ch.dip, ch.udp.sport,
                                  ch.udp.dport, ch.udp.pg, p->GetUid(), ch.udp.seq, "mmu");
                return; // Drop
			}
			CheckAndSendPfc(inDev, qIndex);
		}
		if (inDev < m_devices.size()) {
			m_bytes[inDev][idx][qIndex] += p->GetSize();
		}
		m_devices[idx]->SwitchSend(qIndex, p, ch);
		DynamicCast<QbbNetDevice>(m_devices[idx])->totalBytesRcvd += p->GetSize(); // Attention: this is the egress port's total received packets. Not the ingress port.
	} else
		std::cout << "outdev not found! Dropped. This should not happen. Debugging required!" << std::endl;
	return; // Drop
}

uint32_t SwitchNode::EcmpHash(const uint8_t* key, size_t len, uint32_t seed) {
	uint32_t h = seed;
	if (len > 3) {
		const uint32_t* key_x4 = (const uint32_t*) key;
		size_t i = len >> 2;
		do {
			uint32_t k = *key_x4++;
			k *= 0xcc9e2d51;
			k = (k << 15) | (k >> 17);
			k *= 0x1b873593;
			h ^= k;
			h = (h << 13) | (h >> 19);
			h += (h << 2) + 0xe6546b64;
		} while (--i);
		key = (const uint8_t*) key_x4;
	}
	if (len & 3) {
		size_t i = len & 3;
		uint32_t k = 0;
		key = &key[i - 1];
		do {
			k <<= 8;
			k |= *key--;
		} while (--i);
		k *= 0xcc9e2d51;
		k = (k << 15) | (k >> 17);
		k *= 0x1b873593;
		h ^= k;
	}
	h ^= len;
	h ^= h >> 16;
	h *= 0x85ebca6b;
	h ^= h >> 13;
	h *= 0xc2b2ae35;
	h ^= h >> 16;
	return h;
}

void SwitchNode::SetEcmpSeed(uint32_t seed) {
	m_ecmpSeed = seed;
}

void SwitchNode::AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx) {
	uint32_t dip = dstAddr.Get();
	m_rtTable[dip].push_back(intf_idx);
}

void SwitchNode::ClearTable() {
	m_rtTable.clear();
}

// This function can only be called in switch mode
bool SwitchNode::SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch) {
    if (m_themis && m_themis->PacketIn(device->GetIfIndex(), packet, ch))
        return true;
    if (m_ccMode == 18 && m_grc) {
        m_grc->RouteInput(packet, ch);
        return true;
    }
	// DCI-Ext (ccMode=16): 发送方消费 0xFA 反馈（拦截）；其余建表/统计/ECN清除
	if (m_dciExtEnabled) {
		if (m_dciExt.PacketIn(device, packet, ch))
			return true; // 0xFA 已被发送方 DCI 消费，不再转发
	}

	// atcGateway处理
	bool ret = m_atcGateway.packetIn(device, packet, ch);
	if (ret)
	{
		return true;
	}
	
	SendToDev(packet, ch);
	return true;
}

void SwitchNode::SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p) {
	// Internal loop FIFO admission is paired with ThemisGateway's finite byte
	// accounting, not the normal MMU. Do not mark dummy/loop traffic with ECN.
	if (m_themis && m_themis->IsLoopPort(ifIndex)) return;
	InterfaceTag t;
	p->PeekPacketTag(t);

	// DCI-Ext (ccMode=16): 出队统计/门控检查（仅数据包，开销极小）
	if (m_dciExtEnabled && qIndex != 0) {
		m_dciExt.NotifyDequeue(ifIndex, qIndex, p);
	}

	// DCQCN队列长度监控：输出指定交换机的出口队列长度
	// 通过配置文件 QLEN_MON_SWITCHES 指定监控的交换机 ID 列表
	// 如果列表为空，则监控所有交换机
	// qlen_mon_switches 在文件开头已声明（全局命名空间）
	if (::qlen_mon_switches.empty() || ::qlen_mon_switches.count(m_id) > 0) {
		uint64_t nowTs = Simulator::Now().GetTimeStep();
		if (ifIndex < 64 && qIndex < 8 && nowTs - m_qlenLastTs[ifIndex][qIndex] > 100000) {
			m_qlenLastTs[ifIndex][qIndex] = nowTs;
			Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
			if (dev) {
				uint32_t qBytes = dev->GetQueue()->GetNBytesTotal();
				printf("[DCQCN_QLEN] %lu %u %u %u mmu_eg=%lu\n", 
					Simulator::Now().GetTimeStep(), m_id, ifIndex, qBytes,
					(unsigned long)m_mmu->egress_bytes[ifIndex][qIndex]);
			}
		}
	}

	MyPriorityTag priotag;
	bool found = p->PeekPacketTag(priotag);

	if (qIndex != 0) {
		uint32_t inDev = t.GetPortId();
		m_mmu->RemoveFromIngressAdmission(inDev, qIndex, p->GetSize(), found);
		m_mmu->RemoveFromEgressAdmission(ifIndex, qIndex, p->GetSize(), found);
		m_bytes[inDev][ifIndex][qIndex] -= p->GetSize();
        if (m_ecnEnabled)
        {
            bool egressCongested = m_mmu->ShouldSendCN(ifIndex, qIndex);
            if (egressCongested)
            {
                Ptr<QbbNetDevice> ecnDev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
                uint32_t ecnQlen = ecnDev ? ecnDev->GetQueue()->GetNBytesTotal() : 0;
                {
                    PppHeader eh; Ipv4Header eip;
                    Ptr<Packet> ec = p->Copy();
                    ec->RemoveHeader(eh); ec->RemoveHeader(eip);
                    printf("[ECN_MARK] %lu node=%u if=%u q=%u qlen=%u sip=0x%08x dip=0x%08x\n", 
                        Simulator::Now().GetTimeStep(), m_id, ifIndex, qIndex, ecnQlen,
                        eip.GetSource().Get(), eip.GetDestination().Get());
                }
                PppHeader ppp;
                Ipv4Header h;
                p->RemoveHeader(ppp);
                p->RemoveHeader(h);
                h.SetEcn((Ipv4Header::EcnType)0x03);
                p->AddHeader(h);
                p->AddHeader(ppp);
                if (m_atcGateway.m_gatewayType == 1)
                {                                    // ← 新增
                    m_atcGateway.m_congested = true; // ←
                }
            }
			else
			{
				if (m_atcGateway.m_gatewayType == 1)
				{
					m_atcGateway.m_congested = false;
				}
			}
        }
        //CheckAndSendPfc(inDev, qIndex);
		CheckAndSendResume(inDev, qIndex);
	}
	// PNP also sees CE marks just added by this ESW's own egress queue.
	if (m_themis) m_themis->NotifyDequeue(ifIndex, p);
	if (1) {
		uint8_t* buf = p->GetBuffer();
		if (buf[PppHeader::GetStaticSize() + 9] == 0x11) { // udp packet
			IntHeader *ih = (IntHeader*)&buf[PppHeader::GetStaticSize() + 20 + 8 + 6]; // ppp, ip, udp, SeqTs, INT
			Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
			if (m_ccMode == 3) { // HPCC or PowerTCP-INT
				if (!PowerEnabled)
					ih->PushHop(Simulator::Now().GetTimeStep(), m_txBytes[ifIndex], dev->GetQueue()->GetNBytesTotal(), dev->GetDataRate().GetBitRate());
				else
					ih->PushHop(Simulator::Now().GetTimeStep(), dev->GetQueue()->GetNBytesRxTotal(), dev->GetQueue()->GetNBytesTotal(), dev->GetDataRate().GetBitRate());
				// ih->PushHop(Simulator::Now().GetTimeStep(), m_txBytes[ifIndex], dev->GetQueue()->GetNBytesTotal(), dev->GetDataRate().GetBitRate());
			} else if (m_ccMode == 10) { // HPCC-PINT
				uint64_t t = Simulator::Now().GetTimeStep();
				uint64_t dt = t - m_lastPktTs[ifIndex];
				if (dt > m_maxRtt)
					dt = m_maxRtt;
				uint64_t B = dev->GetDataRate().GetBitRate() / 8; //Bps
				uint64_t qlen = dev->GetQueue()->GetNBytesTotal();
				double newU;

				/**************************
				 * approximate calc
				 *************************/
				int b = 20, m = 16, l = 20; // see log2apprx's paremeters
				int sft = logres_shift(b, l);
				double fct = 1 << sft; // (multiplication factor corresponding to sft)
				double log_T = log2(m_maxRtt) * fct; // log2(T)*fct
				double log_B = log2(B) * fct; // log2(B)*fct
				double log_1e9 = log2(1e9) * fct; // log2(1e9)*fct
				double qterm = 0;
				double byteTerm = 0;
				double uTerm = 0;
				if ((qlen >> 8) > 0) {
					int log_dt = log2apprx(dt, b, m, l); // ~log2(dt)*fct
					int log_qlen = log2apprx(qlen >> 8, b, m, l); // ~log2(qlen / 256)*fct
					qterm = pow(2, (
					                log_dt + log_qlen + log_1e9 - log_B - 2 * log_T
					            ) / fct
					           ) * 256;
					// 2^((log2(dt)*fct+log2(qlen/256)*fct+log2(1e9)*fct-log2(B)*fct-2*log2(T)*fct)/fct)*256 ~= dt*qlen*1e9/(B*T^2)
				}
				if (m_lastPktSize[ifIndex] > 0) {
					int byte = m_lastPktSize[ifIndex];
					int log_byte = log2apprx(byte, b, m, l);
					byteTerm = pow(2, (
					                   log_byte + log_1e9 - log_B - log_T
					               ) / fct
					              );
					// 2^((log2(byte)*fct+log2(1e9)*fct-log2(B)*fct-log2(T)*fct)/fct) ~= byte*1e9 / (B*T)
				}
				if (m_maxRtt > dt && m_u[ifIndex] > 0) {
					int log_T_dt = log2apprx(m_maxRtt - dt, b, m, l); // ~log2(T-dt)*fct
					int log_u = log2apprx(int(round(m_u[ifIndex] * 8192)), b, m, l); // ~log2(u*512)*fct
					uTerm = pow(2, (
					                log_T_dt + log_u - log_T
					            ) / fct
					           ) / 8192;
					// 2^((log2(T-dt)*fct+log2(u*512)*fct-log2(T)*fct)/fct)/512 = (T-dt)*u/T
				}
				newU = qterm + byteTerm + uTerm;

#if 0
				/**************************
				 * accurate calc
				 *************************/
				double weight_ewma = double(dt) / m_maxRtt;
				double u;
				if (m_lastPktSize[ifIndex] == 0)
					u = 0;
				else {
					double txRate = m_lastPktSize[ifIndex] / double(dt); // B/ns
					u = (qlen / m_maxRtt + txRate) * 1e9 / B;
				}
				newU = m_u[ifIndex] * (1 - weight_ewma) + u * weight_ewma;
				printf(" %lf\n", newU);
#endif

				/************************
				 * update PINT header
				 ***********************/
				uint16_t power = Pint::encode_u(newU);
				if (power > ih->GetPower())
					ih->SetPower(power);

				m_u[ifIndex] = newU;
			}
		}
		else {
			FeedbackTag Int;
			bool found;
			found = p->PeekPacketTag(Int);
			if (found) {
				Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
				Int.setTelemetryQlenDeq(Int.getHopCount(), dev->GetQueue()->GetNBytesTotal()); // queue length at dequeue
				Int.setTelemetryTsDeq(Int.getHopCount(), Simulator::Now().GetNanoSeconds()); // timestamp at dequeue
				Int.setTelemetryBw(Int.getHopCount(), dev->GetDataRate().GetBitRate());
				Int.setTelemetryTxBytes(Int.getHopCount(), m_txBytes[ifIndex]);
				Int.incrementHopCount(); // Incrementing hop count at Dequeue. Don't do this at enqueue.
				p->ReplacePacketTag(Int); // replacing the tag with new values
				// std::cout << "found " << Int.getHopCount() << std::endl;
			}
		}
	}
	
	// ========== 通用反馈包：跟踪活跃流 ==========
	if (m_switchFeedbackEnabled) {
		TrackActiveFlow(p, ifIndex);
	}
	
	m_txBytes[ifIndex] += p->GetSize();
	m_lastPktSize[ifIndex] = p->GetSize();
	m_lastPktTs[ifIndex] = Simulator::Now().GetTimeStep();
}

int SwitchNode::logres_shift(int b, int l) {
	static int data[] = {0, 0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
	return l - data[b];
}

int SwitchNode::log2apprx(int x, int b, int m, int l) {
	int x0 = x;
	int msb = int(log2(x)) + 1;
	if (msb > m) {
		x = (x >> (msb - m) << (msb - m));
#if 0
		x += + (1 << (msb - m - 1));
#else
		int mask = (1 << (msb - m)) - 1;
		if ((x0 & mask) > (rand() & mask))
			x += 1 << (msb - m);
#endif
	}
	return int(log2(x) * (1 << logres_shift(b, l)));
}

/* =========================================================================
 * 统一控制包配置与发送框架（去协议化设计）
 * ========================================================================= */

/**
 * 检查流是否有广域流竞争
 * 判断依据：交换机IP和源IP的第2段（数据中心ID）是否不同
 * 
 * IP格式: 11.{dc_id}.{id/256}.{id%256}
 * 子网掩码: 255.255.0.0 (前两段相同 = 同一DC)
 * 
 * @param srcIp 流的源IP地址
 * @return true 如果源IP和交换机属于不同数据中心（广域流）
 */


/**
 * 跟踪活跃流：在数据包出队时记录源IP和流信息
 * 作用：维护每个出口网卡的活跃源节点集合
 */
void SwitchNode::TrackActiveFlow(Ptr<Packet> p, uint32_t outPort) {
    if (!m_switchFeedbackEnabled) return;

    Ptr<Packet> cp = p->Copy();
    PppHeader ppp;
    cp->RemoveHeader(ppp);
    Ipv4Header ipv4;
    cp->RemoveHeader(ipv4);

    uint8_t proto = ipv4.GetProtocol();
    Ipv4Address srcIp = ipv4.GetSource();
    Ipv4Address dstIp = ipv4.GetDestination();

    // 调试：打印所有包的协议号
    static uint32_t dbgCount = 0;
    if (dbgCount < 5) {
        std::cout << "[TRACK DEBUG] Switch " << m_id << " port=" << outPort
                  << " proto=0x" << std::hex << (uint32_t)proto << std::dec
                  << " src=" << srcIp << " dst=" << dstIp << std::endl;
        dbgCount++;
    }

    // 只跟踪UDP/TCP数据流
    if (proto == 0x11 || proto == 0x06) {
        // 记录流端点 (srcIp=数据流源IP, dstIp=数据流目的IP)
        FlowEndpoints fe;
        fe.srcIp = srcIp;
        fe.dstIp = dstIp;
        m_activeFlows[outPort].insert(fe);

        // 提取源IP的DC-ID（IP第2段：11.{dc_id}.*.*）
        uint32_t srcIpValue = srcIp.Get();
        uint8_t srcDcId = (srcIpValue >> 16) & 0xFF;
        m_activeSrcDcIds[outPort].insert(srcDcId);

        // 调试：如果该端口有多个DC的流，输出提示
        if (m_activeSrcDcIds[outPort].size() > 1) {
            std::cout << "  [WAN DETECT] Switch " << m_id << " port=" << outPort
                      << " has multi-DC flows! src=" << srcIp << " (dc=" << (int)srcDcId << ")";
            std::cout << " DCs on port=";
            for (auto dc : m_activeSrcDcIds[outPort]) std::cout << (int)dc << ",";
            std::cout << std::endl;
        }
    }
}

/**
 * 统一公开接口：启动周期性反馈机制
 * 作用：在仿真开始时调用一次，启动周期驱动器
 */
void SwitchNode::StartPeriodicFeedbackMechanism(Time interval) {
    if (!m_switchFeedbackEnabled) {
        NS_LOG_INFO("Switch " << m_id << ": Feedback mechanism disabled");
        return;
    }
    
    NS_LOG_INFO("Switch " << m_id << ": Starting periodic feedback with interval " 
              << interval.As(Time::US));
    
    m_feedbackInterval = interval;
    Simulator::Schedule(interval, &SwitchNode::PeriodicFeedbackLoop, this, interval);
}

/**
 * 周期驱动器：纯粹的调度员
 * 作用：管理时间、筛选目标、触发任务函数
 */
void SwitchNode::PeriodicFeedbackLoop(Time interval) {
    if (!m_switchFeedbackEnabled) return;
    
    // 使用交换机的真实IP作为源IP（仅用于路由查找）
    Ipv4Address switchIp = m_switchRealIp;
    if (switchIp == Ipv4Address("0.0.0.0")) {
        uint32_t switchIpValue = (10 << 24) | (0 << 16) | (m_id << 8) | 254;
        switchIp = Ipv4Address(switchIpValue);
    }
    
    // 统计活跃流
    uint32_t totalActiveFlows = 0;
    for (auto const& [idx, flowSet] : m_activeFlows) {
        totalActiveFlows += flowSet.size();
    }
    if (totalActiveFlows > 0) {
        std::cout << "[FRP SEND] Switch " << m_id << " t=" << Simulator::Now().GetSeconds() 
                  << " activeFlows=" << totalActiveFlows << std::endl;
    }
    
    // 轮询当前周期内所有产生过流量的物理出口网卡
    for (auto const& [idx, flowSet] : m_activeFlows) {
        if (flowSet.empty()) continue;
        
            
        // 1. 调用配置函数获取组装好的"纯净控制包"
        Ptr<Packet> controlPayload = ConfigureFeedbackPayload(m_ccMode, idx);
            
        // 2. 遍历该网卡上的所有活跃流，批量把控制包投递回去
        for (auto const& fe : flowSet) {
            // FRP包IP封装：
            //   SIP = fe.dstIp (数据流的目的IP，帮助接收方匹配QP)
            //   DIP = fe.srcIp (数据流的源IP，即发送方，FRP包发给他)
            Ipv4Address frpSrcAddr = fe.dstIp;
            Ipv4Address frpDstAddr = fe.srcIp;

            // 查路由表验证转发端口
            auto routeEntry = m_rtTable.find(frpDstAddr.Get());
            std::string routeInfo = "NO_ROUTE";
            if (routeEntry != m_rtTable.end() && !routeEntry->second.empty()) {
                routeInfo = "ports=[";
                for (size_t pi = 0; pi < routeEntry->second.size(); pi++) {
                    if (pi > 0) routeInfo += ",";
                    routeInfo += std::to_string(routeEntry->second[pi]);
                }
                routeInfo += "]";
            }

            std::cout << "  [FRP SEND] port=" << idx << " -> srcIp=" << fe.srcIp
                      << " FRP<SIp=" << frpSrcAddr << ", DIp=" << frpDstAddr << ">"
                      << " route:" << routeInfo << std::endl;

            // 3. 调用发送函数完成发送，按 DIP 查路由表转发
            SendControlPacket(frpSrcAddr, frpDstAddr, controlPayload, 0xFF);
        }
    }
    
    // 账本清空，续订下一周期
    m_activeFlows.clear();
    m_activeSrcDcIds.clear();
    Simulator::Schedule(interval, &SwitchNode::PeriodicFeedbackLoop, this, interval);
}

/**
 * 核心任务函数 1：根据ccMode动态装配控制包载荷
 * 作用：SwitchNode不关心具体协议，只根据ccMode调用对应的头部类
 */
Ptr<Packet> SwitchNode::ConfigureFeedbackPayload(uint32_t ccMode,
                                                  uint32_t ifIndex) {
    Ptr<Packet> payload = Create<Packet>();
    Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[ifIndex]);
    
    // 调试日志：输出ccMode
    static bool logged = false;
    if (!logged && (ccMode == 13 || ccMode == 14)) {
        fprintf(stderr, "[SWITCH_DEBUG] Switch %u ConfigureFeedbackPayload ccMode=%u ifIndex=%u\n", m_id, ccMode, ifIndex);
        logged = true;
    }
    
    if (!dev) {
        NS_LOG_WARN("Invalid device at port " << ifIndex);
        return payload;
    }
    
    uint32_t currentQDepth = dev->GetQueue()->GetNBytesTotal();  // 标准单位: Byte
    uint64_t link_bps = dev->GetDataRate().GetBitRate();         // 标准单位: bps
    

    
    // 根据ccMode装配不同的控制包
    if (ccMode == 13 || ccMode == 14) {
        // ========== FRP 模式(13是ours, 14是ROCC) ==========

        // ===== 跨域流连续识别 (FRP 专用) =====
        // 1. 读出上周期累计的计数器和上周期队列深度
        const uint32_t CROSS_DC_CONSEC_LIMIT = 10;
        uint32_t crossDcCounter = m_crossDcCounter[ifIndex];
        uint32_t prevQBytes    = m_prevQBytes[ifIndex];

        // 2. 本轮采样转换成 cell 单位，与 frp-rate-calculator.cc 保持一致
        double qRefBytesLocal;
        if (link_bps >= 200ULL * 1000000000ULL) {
            qRefBytesLocal = 1048576.0;  // 1MB (200Gbps)
        } else if (link_bps >= 100ULL * 1000000000ULL) {
            qRefBytesLocal = 307200.0;   // 300KB (100Gbps)
        } else {
            qRefBytesLocal = 307200.0;   // 300KB (40Gbps)
        }
        const double qThBytesLocal  = 307200.0;   // 300KB 与 frp-rate-calculator.cc 一致
        const double qCurCellLocal  = static_cast<double>(currentQDepth) / 600.0;
        const double qRefCellLocal  = qRefBytesLocal / 600.0;
        const double qThCellLocal   = qThBytesLocal / 600.0;
        const double qOldCellLocal  = static_cast<double>(prevQBytes) / 600.0;

        // 3. 调用 FRP 计算器。本轮决策使用"上一轮累计"的计数器。
        double calculatedRateBps = m_frpCalculator.CalculateFairRate(
            ifIndex,
            link_bps,
            currentQDepth,
            ccMode - 13,
            ccMode,
            static_cast<uint8_t>(crossDcCounter > 255 ? 255 : crossDcCounter)
        );

        // 4. 更新计数器：两个条件同时成立才累加，任一不满足则重置
        if (qCurCellLocal > qOldCellLocal && qCurCellLocal > (qRefCellLocal + qThCellLocal)) {
            m_crossDcCounter[ifIndex] = (crossDcCounter < CROSS_DC_CONSEC_LIMIT + 5)
                                         ? crossDcCounter + 1 : crossDcCounter;
        } else {
            m_crossDcCounter[ifIndex] = 0;
        }
        // 5. 保存本周期队列深度，供下周期对比
        m_prevQBytes[ifIndex] = currentQDepth;

        // 2. 严格遵循控制包字段的局部量纲换算
        uint16_t fairRateField = static_cast<uint16_t>(calculatedRateBps / 10000000.0);  // 换算为 10Mbps 单元
        double qRefBytes = qRefBytesLocal;
        double qCurCell = qCurCellLocal;
        double qRefCell = qRefCellLocal;
        double qDevCell = qCurCell - qRefCell;
        int16_t qDevField   = static_cast<int16_t>(qDevCell);  // 换算为 600B Cell 单元 (可为负值)
        uint16_t linkRateField = static_cast<uint16_t>(link_bps / 10000000.0);  // 换算为 10Mbps 单元

        // cp_id: 使用Switch节点ID直接编码（避免哈希冲突）
        // 格式: 高12位 = Switch节点ID (0-4095), 低4位 = Port_ID (0-15)
        // 节点ID范围通常在0-63，确保12位足够且无冲突
        uint16_t switchNodeId = static_cast<uint16_t>(m_id & 0x0FFF);
        uint16_t cpId = (switchNodeId << 4) | (ifIndex & 0x0F);

        // 调试：输出linkRate计算
        std::cout << "  [FRP CALC] Switch " << m_id << " port=" << ifIndex
                  << " link_bps=" << link_bps << " linkRateField=" << linkRateField
                  << " fairRateField=" << fairRateField << " qDevField=" << qDevField
                  << " (qCur=" << qCurCell << " qRef=" << qRefCell << ")"
                  << " type=" << ccMode << std::endl;

        // 结构化数据日志。与 TX RATE 和 DCQCN 队列日志共用 stdout，避免
        // runner 合并 stdout/stderr 时发生记录交叉。
        double fairRateMbps = calculatedRateBps / 1e6;
        double qCurKB = static_cast<double>(currentQDepth) / 1024.0;
        printf("[FRP_DATA_SW] %.9f %u %u %.2f %.2f %.2f %d\n",
                Simulator::Now().GetSeconds(), m_id, ifIndex,
                fairRateMbps, qCurKB, (qDevField * 600.0 / 1024.0), ccMode);

        // 3. 装载并打包
        Icmpv4FrpFeedback frpHeader;
        frpHeader.SetFairRate(fairRateField);
        frpHeader.SetQDepth(qDevField);
        frpHeader.SetCpId(cpId);
        frpHeader.SetType(ccMode - 13);
        frpHeader.SetLinkRate(linkRateField);  // 把带宽塞进 15bit 空间传出
        payload->AddHeader(frpHeader);

        // 4. 广域流标志已随周期清空，无需单独清除

        // 5. 添加 ICMP 头部
        Icmpv4Header icmpHeader;
        icmpHeader.SetType(Icmpv4Header::ICMPV4_FRP_FEEDBACK);
        icmpHeader.SetCode(0);
        icmpHeader.EnableChecksum();
        payload->AddHeader(icmpHeader);

        NS_LOG_DEBUG("FRP Encap -> FairRate: " << fairRateField << "*10Mbps, "
                     << "LinkRate: " << linkRateField << "*10Mbps, "
                     << "QDev: " << qDevField << " cells(600B)");
    }
    else if (ccMode == 15) {
        // ========== 未来扩展：其他自定义模式 ==========
        NS_LOG_DEBUG("Custom mode " << ccMode << " not implemented");
    }
    else {
        NS_LOG_WARN("Unknown ccMode=" << ccMode << ", no feedback payload generated");
    }
    
    return payload;
}

/**
 * 核心任务函数 2：通用的网络层/链路层封装与发送
 * 作用：使用仿真平台的CustomHeader封装FRP包并送入发送引擎
 * 
 * 包格式：[CustomHeader(L2+L3, l3Prot=0x01)] [Icmpv4FrpFeedback payload]
 * CustomHeader内部已包含PPP+IPv4信息，不需要额外添加PppHeader/Ipv4Header
 */
void SwitchNode::SendControlPacket(Ipv4Address srcAddr,
                                    Ipv4Address dstAddr,
                                    Ptr<Packet> payload,
                                    uint8_t l3Prot) {
    Ptr<Packet> p = payload->Copy();

    // 构造CustomHeader (仿真平台的标准封装)
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header);
    ch.l3Prot = 0x01;  // ICMP - 让主机端路由到ReceiveIcmp

    // L2: PPP
    ch.pppProto = 0x0021;  // IPv4

    // L3: IPv4
    ch.sip = srcAddr.Get();
    ch.dip = dstAddr.Get();
    ch.m_ttl = 64;
    ch.ipid = rand() % 65536;

    // 设置payload大小
    ch.m_payloadSize = p->GetSize();

    // 将CustomHeader封装到packet前面
    p->AddHeader(ch);

    // 用MyPriorityTag标记为最高优先级(0)，避免被低优先级队列延迟
    MyPriorityTag prioTag;
    prioTag.SetPriority(0);  // queue 0 = highest priority
    p->AddPacketTag(prioTag);

    // 设置入端口标签，标识该包由交换机内部产生
    InterfaceTag intTag(m_id);
    p->AddPacketTag(intTag);

    std::cout << "  [FRP SEND] SendToDev: src=" << srcAddr << " dst=" << dstAddr
              << " pktSize=" << p->GetSize() << " ch.l3Prot=0x" << std::hex << (int)ch.l3Prot << std::dec << std::endl;

    // 选择出端口：仅根据 DIP 查路由表
    uint32_t outPort = (uint32_t)-1;
    auto entry = m_rtTable.find(dstAddr.Get());
    if (entry != m_rtTable.end() && !entry->second.empty()) {
        outPort = entry->second[0];  // 取第一个可用端口
        std::cout << "  [FRP SEND] Route-table outPort=" << outPort
                  << " (dst=" << dstAddr << ")" << std::endl;
    }

    if (outPort != (uint32_t)-1 && outPort < m_devices.size()) {
        m_devices[outPort]->SwitchSend(0, p, ch);  // qIndex=0, 最高优先级
        std::cout << "  [FRP SEND] Sent via port " << outPort << std::endl;
    } else {
        std::cout << "  [FRP SEND] ERROR: No valid outPort for dst=" << dstAddr << std::endl;
    }
}

// 调试接口：打印指向特定目的 IP 的路由表条目
void SwitchNode::PrintRoutingTableFor(Ipv4Address targetDst) {
    std::cout << "  [RTABLE] Switch " << m_id << " routes for dst=" << targetDst << " (0x"
              << std::hex << targetDst.Get() << std::dec << "):" << std::endl;
    auto entry = m_rtTable.find(targetDst.Get());
    if (entry == m_rtTable.end()) {
        std::cout << "    (no entry)" << std::endl;
        return;
    }
    std::cout << "    ports=[";
    for (size_t i = 0; i < entry->second.size(); i++) {
        if (i > 0) std::cout << ",";
        std::cout << entry->second[i];
    }
    std::cout << "]" << std::endl;
}

/**
 * 设置交换机真实IP地址
 * 用于FRP反馈包的源IP地址
 */
void SwitchNode::SetSwitchRealIp(Ipv4Address ip) {
    m_switchRealIp = ip;
    NS_LOG_INFO("Switch " << m_id << " real IP set to " << ip);
}

/**
 * 获取交换机真实IP地址
 */
Ipv4Address SwitchNode::GetSwitchRealIp() const {
    return m_switchRealIp;
}

// ========== Bifrost (ccMode=12) 实现 ==========

/**
 * 启动Bifrost周期PFC机制
 * 仅在启用了Bifrost的交换机（通常是指定的某个交换机）上调用
 */
void SwitchNode::StartBifrostPfcMechanism() {
    if (!m_bifrostEnabled) {
        return;
    }
    fprintf(stderr, "[BIFROST] Switch %u starting periodic PFC mechanism, period=%uus\n",
            m_id, m_bifrostPfcPeriodUs);
    m_bifrostLastPfcTimeUs = Simulator::Now().GetMicroSeconds();
    m_bifrostPfcEvent = Simulator::Schedule(
        MicroSeconds(m_bifrostPfcPeriodUs),
        &SwitchNode::SendBifrostPfcWithCustomTime,
        this);
}

/**
 * 发送Bifrost周期PFC到所有ingress端口和队列
 * 使用动态计算的暂停时间
 */
void SwitchNode::SendBifrostPfcWithCustomTime() {
    if (!m_bifrostEnabled) {
        return;
    }
    uint32_t now = Simulator::Now().GetMicroSeconds();
    m_bifrostLastPfcTimeUs = now;

    // 遍历所有ingress端口和队列
    for (uint32_t inDev = 0; inDev < m_devices.size(); inDev++) {
        Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[inDev]);
        if (!dev) {
            continue;
        }

        // 获取该端口上一个PFC周期内收到的总字节数
        // getNumRxBytes()内部逻辑：
        //   返回值 = totalBytesRcvd - numRxBytesLast  
        //          = 从上次调用到这次调用之间的字节数
        //          = 上一个PFC周期（m_bifrostPfcPeriodUs）内收到的字节数
        //   numRxBytesLast = totalBytesRcvd  (更新基准，为下一个周期做准备)
        uint32_t r = dev->getNumRxBytes();

        for (uint32_t qIndex = 0; qIndex < qCnt; qIndex++) {
            uint32_t qLenBytes = dev->GetQueue()->GetNBytes(qIndex);

            // 计算自定义暂停时间（动态计算逻辑待实现）
            uint32_t customTime = CalculateBifrostPauseTime(qIndex, qLenBytes, r);

            // 发送带自定义暂停时间的PFC
            dev->SendPfcWithTime(qIndex, 0, customTime);

            fprintf(stderr, "[BIFROST_PFC] t=%.3fus Sw=%u inDev=%u qIdx=%u "
                    "qLen=%uB r=%uBytes pauseTime=%uus\n",
                    (double)now, m_id, inDev, qIndex,
                    qLenBytes, r, customTime);
        }
    }

    // 调度下一次周期PFC
    m_bifrostPfcEvent = Simulator::Schedule(
        MicroSeconds(m_bifrostPfcPeriodUs),
        &SwitchNode::SendBifrostPfcWithCustomTime,
        this);
}

/**
 * 计算Bifrost PFC的暂停时间
 * 输入: 队列索引、当前队列长度（字节）、上一个周期收到的字节数
 * 输出: 暂停时间（微秒）
 * 当前为占位符实现 - 后续根据网络状态动态计算
 */
uint32_t SwitchNode::CalculateBifrostPauseTime(uint32_t qIndex, uint32_t qLenBytes, uint32_t r) {
    // TODO: 后续根据队列长度、r、链路速率等动态计算
    return 50;  // 占位符：固定50us
}

/**
 * 从配置文件读取 Bifrost 参数
 * @param configFile 配置文件路径
 */
void SwitchNode::ReadBifrostConfigFromFile(const std::string& configFile) {
    std::ifstream conf(configFile.c_str());
    if (!conf.is_open()) {
        std::cerr << "[BIFROST] Error: Cannot open config file: " << configFile << std::endl;
        return;
    }
    
    std::string key;
    uint32_t pfcPeriod = 10; // 默认值
    
    while (conf >> key) {
        if (key.compare("BIFROST_PFC_PERIOD") == 0) {
            conf >> pfcPeriod;
            break;
        }
        // 跳过行尾
        std::string line;
        std::getline(conf, line);
    }
    
    conf.close();
    
    m_bifrostPfcPeriodUs = pfcPeriod;
    std::cout << "[BIFROST] Switch " << GetId() << " read config: PFC period = " << pfcPeriod << " us" << std::endl;
}

/**
 * 初始化 Bifrost（在 DCI 交换机上启用）
 * @param switchId 交换机 ID
 */
void SwitchNode::InitializeBifrost(uint32_t switchId) {
    m_bifrostEnabled = true;
    m_bifrostDeploySwitchId = switchId;
    
    SetAttribute("CcMode", UintegerValue(12));
    SetAttribute("BifrostEnabled", BooleanValue(true));
    SetAttribute("BifrostPfcPeriodUs", UintegerValue(m_bifrostPfcPeriodUs));
    SetAttribute("BifrostDeploySwitchId", UintegerValue(switchId));
    
    StartBifrostPfcMechanism();
    
    std::cout << "[BIFROST] Initialized on DCI switch " << switchId 
              << " with PFC period = " << m_bifrostPfcPeriodUs << " us" << std::endl;
}

// ========== ATC (ccMode=15) Gateway 实现 ==========

/**
 * 设置配置文件路径
 * @param configPath 配置文件路径
 */
void SwitchNode::SetConfigFilePath(const std::string& configPath) {
    m_configFilePath = configPath;
    std::cout << "[ATC] Switch " << GetId() << " config file path set: " << configPath << std::endl;
}

/**
 * 配置 ATC 参数
 * @param config ATC 配置结构
 */
void SwitchNode::ConfigureAtc(const AtcConfig& config) {
    m_atcConfig = config;
    std::cout << "[ATC] Switch " << GetId() << " configured with:" << std::endl;
    std::cout << "  VOQ max size: " << m_atcConfig.voq_max_size << std::endl;
    std::cout << "  CNP notify interval: " << m_atcConfig.cnp_notify_interval_us << " us" << std::endl;
    std::cout << "  Min rate: " << m_atcConfig.min_rate << std::endl;
    std::cout << "  Max VOQ count: " << m_atcConfig.max_voq_count << std::endl;
}

/**
 * 初始化 ATC Gateway
 * @param gatewayType Gateway 类型: 1=source, 2=destination, 3=bidirectional
 */
void SwitchNode::InitializeAtcGateway(uint32_t gatewayType) {
    if (gatewayType == 0) {
        return; // 不是 gateway
    }
    
    m_gatewayType = gatewayType;
    
    // 配置 ATC 参数
    m_atcGateway.ConfigureAtcParams(
        m_atcConfig.voq_max_size,
        m_atcConfig.cnp_notify_interval_us,
        m_atcConfig.min_rate,
        m_atcConfig.rate_decrease_interval_us,
        m_atcConfig.rpg_time_reset_us,
        m_atcConfig.cnp_on_global_congestion,
        m_atcConfig.voq_react_to_cnp,
        m_atcConfig.voq_inflight_rtt_us
    );
    
    // 初始化 Gateway
    m_atcGateway.init(
        gatewayType,
        m_atcConfig.max_voq_count,
        m_atcConfig.max_voq_rate,
        m_atcConfig.long_haul_bandwidth,
        m_atcConfig.long_haul_delay
    );
    
    std::string gatewayTypeStr;
    if (gatewayType == 1) gatewayTypeStr = "source";
    else if (gatewayType == 2) gatewayTypeStr = "destination";
    else if (gatewayType == 3) gatewayTypeStr = "bidirectional";
    
    std::cout << "[ATC] Initialized " << gatewayTypeStr << " gateway on switch " << GetId() << std::endl;
}

// ============================================================================
// DCI-Ext (ccMode=16): DCI 交换机初始化
//   发送侧（senderSide=true）：DIP 预算 + 观察/CNP初始化/周期查表
//                               + ECN 快速反馈 + 0xFA 反馈消费
//   接收侧（senderSide=false）：表一/表二聚合 + ACK速率统计 + 周期T 0xFA 反馈
// ============================================================================
void SwitchNode::InitializeDciExt(uint32_t nodeId, uint32_t burstIntervalUs, double periodTUs,
                                   uint64_t dipBwBps, uint64_t observeMb,
                                   uint32_t agingPeriods, bool senderSide,
                                   double dtUs, double sigmaUs) {
    m_dciExtEnabled = true;
    m_dciExt.SetSenderSide(senderSide);
    m_dciExt.Configure(nodeId, burstIntervalUs, periodTUs, dipBwBps,
                       observeMb, agingPeriods, dtUs, sigmaUs);
    std::cout << "[DCIEXT] Initialized DCI-Ext v3 on switch " << GetId()
              << (senderSide ? " (SENDER: 表A+观察期+ECN反馈+0xFA消费)"
                             : " (RECEIVER: RTT探测+周期0xFA反馈)")
              << " (T=" << periodTUs << "us, dipBw=" << dipBwBps / 1e9 << "G"
              << ", observe=" << observeMb << "MB"
              << ", aging=" << agingPeriods << "T"
              << ", Dt=" << dtUs << "us, sigma=" << sigmaUs << "us)" << std::endl;
}

// 端口设备查询（DCI-Ext NotifyDequeue 判定出端口是否长链路用）
Ptr<NetDevice> SwitchNode::GetDevByIndex(uint32_t ifIndex) {
    if (ifIndex >= m_devices.size())
        return nullptr;
    return m_devices[ifIndex];
}

// 禁用指定端口的 PFC 发送（DCI 长链路端口防死锁）
void SwitchNode::DisablePfcOnPort(uint32_t port) {
    m_pfcDisabledPorts.insert(port);
    // 同时设置该端口忽略收到的 PFC 帧（双向禁用，彻底隔离长链路的 PFC 反压）
    if (port < m_devices.size()) {
        Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[port]);
        if (dev) {
            dev->SetIgnoreReceivedPfc(true);
        }
    }
    std::cout << "[DCIEXT] Disabled PFC on switch " << GetId() << " port " << port << " (send+receive)" << std::endl;
}

// ============================================================================
// ePFC (ccMode=17)：初始化（角色一打标 + 接收方DCI流表）+ 出口本地门控 helper
// ============================================================================
void SwitchNode::InitializeEpfc(uint32_t nodeId, uint32_t ingressMulPct, uint64_t egressThreshBytes,
                                uint64_t dciThreshBytes, double rttUs, double agingRtts,
                                double updateTUs) {
    m_epfcEnabled = true;
    m_epfc.SetMmu(m_mmu);
    m_epfc.Configure(nodeId, ingressMulPct, egressThreshBytes, dciThreshBytes,
                     rttUs, agingRtts, updateTUs);
    std::cout << "[EPFC] Initialized ePFC on switch " << GetId()
              << " (ingressMulPct=" << ingressMulPct
              << ", egressThresh=" << egressThreshBytes << "B"
              << ", dciThresh=" << dciThreshBytes << "B"
              << ", rtt=" << rttUs << "us, aging=" << agingRtts << "rtt"
              << ", updateT=" << updateTUs << "us)" << std::endl;
    // S 计量挂点注入：QbbNetDevice 实际出队（TransmitStart 前）回调本节点 helper，
    // 使 S 口径 = 物理发出字节（门控拦下的包不出队不计 S，门控真正限流）
    for (uint32_t i = 0; i < m_devices.size(); i++) {
        Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[i]);
        if (dev)
            dev->SetEpfcDequeueCallback(MakeCallback(&SwitchNode::EpfcDequeueHelper, this));
    }
}

// ePFC 门控回调：本地门控指定端口的指定 pg（独立于 PFC 状态，停的是自己出口的出队）
void SwitchNode::EpfcGateHelper(uint32_t portIdx, uint8_t pg, bool on) {
    if (portIdx >= m_devices.size())
        return;
    Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(m_devices[portIdx]);
    if (dev)
        dev->SetDciExtGate(pg, on);
}

// ePFC 出队计量回调：QbbNetDevice 实际出队（TransmitStart 前）触发。
//   读出队 Tag → 调 Epfc::DataDequeue 累计 S 并评估门控 ——
//   S 口径 = 物理发出字节：门控中被拦的包不出队、不经过此处（天然不计 S），
//   XON 后倾泻的存量也如实计入，修复旧实现（入队前计 S）的门控架空问题
void SwitchNode::EpfcDequeueHelper(uint32_t outPort, Ptr<Packet> p) {
    if (!m_epfcEnabled)
        return;
    EpfcSAccountTag tag;
    if (p->PeekPacketTag(tag)) {
        uint32_t payload = p->GetSize() - CustomHeader::GetStaticWholeHeaderSize();
        m_epfc.DataDequeue(outPort, tag, payload);
        p->RemovePacketTag(tag);
    }
}

/**
 * 从配置文件读取 ATC 参数
 * @param configFile 配置文件路径
 */
void SwitchNode::ReadAtcConfigFromFile(const std::string& configFile) {
    std::ifstream conf(configFile.c_str());
    if (!conf.is_open()) {
        std::cerr << "[ATC] Error: Cannot open config file: " << configFile << std::endl;
        return;
    }
    
    std::string key;
    AtcConfig config;
    
    while (conf >> key) {
        if (key.compare("ATC_VOQ_MAX_SIZE") == 0) {
            conf >> config.voq_max_size;
        } else if (key.compare("ATC_CNP_NOTIFY_INTERVAL") == 0) {
            conf >> config.cnp_notify_interval_us;
        } else if (key.compare("ATC_MIN_RATE") == 0) {
            conf >> config.min_rate;
        } else if (key.compare("ATC_RATE_DECREASE_INTERVAL") == 0) {
            conf >> config.rate_decrease_interval_us;
        } else if (key.compare("ATC_RPG_TIME_RESET") == 0) {
            conf >> config.rpg_time_reset_us;
        } else if (key.compare("ATC_CNP_ON_GLOBAL_CONGESTION") == 0) {
            uint32_t v;
            conf >> v;
            config.cnp_on_global_congestion = (v != 0);
        } else if (key.compare("ATC_VOQ_REACT_TO_CNP") == 0) {
            uint32_t v;
            conf >> v;
            config.voq_react_to_cnp = (v != 0);
        } else if (key.compare("ATC_VOQ_INFLIGHT_RTT_US") == 0) {
            conf >> config.voq_inflight_rtt_us;
        } else if (key.compare("MAX_ATC_VOQ_COUNT") == 0) {
            conf >> config.max_voq_count;
        } else if (key.compare("MAX_ATC_VOQ_RATE") == 0) {
            conf >> config.max_voq_rate;
        }
        // 跳过行尾
        std::string line;
        std::getline(conf, line);
    }
    
    conf.close();
    ConfigureAtc(config);
}

} /* namespace ns3 */
