#include "ns3/atcGateway.h"
#include "ns3/packet.h"
#include "ns3/ppp-header.h"
#include "ns3/qbb-header.h"
#include "ns3/tcp-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/random-variable.h"
#include "point-to-point-net-device.h"
#include "ns3/custom-priority-tag.h"
#include "ns3/interface-tag.h"
#include "ns3/node.h"
#include <assert.h>
#include <sys/time.h>

namespace ns3{

atcGateway::atcGateway()
{
    m_debug = 0x3;
    m_gatewayStatus = 0;
    m_gatewayType = 0;
    m_congested = false;
    m_ecnbits = 0;
    m_voqMaxSize = VOQ_MAX_SIZE;
    m_cnpNotifyInterval = CNP_NOTIFY_INTERVAL;
    m_cnpOnGlobalCongestion = true;
    m_voqReactToCnp = true;
    m_voqInflightRtt = INTRA_DC_DELAY * 2;
    m_cnpByGlobalCongestion = 0;
    m_cnpByPacketEcn = 0;
    m_diagLongHaulFeedbackRecv = 0;
    m_diagLongHaulFeedbackSent = 0;
    m_diagLongHaulOverInflight = 0;
    m_diagLongHaulVoqOverload = 0;
    m_diagLongHaulResume = 0;
    m_diagThrottleCalls = 0;
    m_diagMaxLongHaulInflight = 0;
    m_diagMaxActiveVoqCount = 0;

     /******************************
	 * Mellanox's version of DCQCN
	 *****************************/
    m_g=0.00390625; //feedback weight
    m_alpha_resume_interval = 1.0;
    m_rateDecreaseInterval = 4.0;
    m_rpgTimeReset = 480.0;
    m_rateOnFirstCNP = 1.0;
    m_rpgThreshold = 1;
    m_EcnClampTgtRate = false;
    m_minRate = DataRate("100Mb/s");
    m_rai = DataRate("50Mb/s");	
	m_rhai = DataRate("100Mb/s");
}

atcGateway::~atcGateway(){}

void atcGateway::ConfigureAtcParams(uint32_t voqMaxSize,
                                    double cnpNotifyIntervalUs,
                                    const std::string& minRate,
                                    double rateDecreaseIntervalUs,
                                    double rpgTimeResetUs,
                                    bool cnpOnGlobalCongestion,
                                    bool voqReactToCnp,
                                    double voqInflightRttUs)
{
    m_voqMaxSize = voqMaxSize;
    m_cnpNotifyInterval = MicroSeconds(cnpNotifyIntervalUs);
    m_minRate = DataRate(minRate);
    m_rateDecreaseInterval = rateDecreaseIntervalUs;
    m_rpgTimeReset = rpgTimeResetUs;
    m_cnpOnGlobalCongestion = cnpOnGlobalCongestion;
    m_voqReactToCnp = voqReactToCnp;
    m_voqInflightRtt = MicroSeconds(voqInflightRttUs);

    if (m_debug & 0x2) {
        printf("+++atcGateway params voqMaxSize:%u cnpNotifyInterval:%.3fus minRate:%s rateDecreaseInterval:%.3fus rpgTimeReset:%.3fus cnpOnGlobal:%u voqReactToCnp:%u voqInflightRtt:%.3fus\n",
               m_voqMaxSize, cnpNotifyIntervalUs, minRate.c_str(), m_rateDecreaseInterval, m_rpgTimeReset,
               m_cnpOnGlobalCongestion ? 1 : 0, m_voqReactToCnp ? 1 : 0, voqInflightRttUs);
    }
}

void atcGateway::PrintAtcDebugSummary(const char* reason)
{
    if (!(m_debug & 0x2)) {
        return;
    }

    uint64_t totalEnqueued = 0;
    uint64_t totalSent = 0;
    uint64_t totalBlocked = 0;
    uint64_t totalAck = 0;
    uint64_t totalCnpAck = 0;
    uint64_t totalAckDeducted = 0;
    uint64_t maxQueueBytes = 0;
    uint64_t maxQueueLen = 0;
    uint64_t maxVoqInflight = 0;
    uint32_t activeVoqs = 0;

    for (std::vector<VoqEntry>::const_iterator it = voqPool.begin(); it != voqPool.end(); ++it) {
        totalEnqueued += it->diagBytesEnqueued;
        totalSent += it->diagBytesSent;
        totalBlocked += it->diagBlockedByInflight;
        totalAck += it->diagAckCount;
        totalCnpAck += it->diagCnpAckCount;
        totalAckDeducted += it->diagAckBytesDeducted;
        maxQueueBytes = std::max(maxQueueBytes, it->diagMaxCurrentSize);
        maxQueueLen = std::max(maxQueueLen, it->diagMaxQueueLen);
        maxVoqInflight = std::max(maxVoqInflight, it->diagMaxInflightBytes);
        if (it->isAllocated) {
            activeVoqs++;
        }
    }

    printf("+++atcGateway diag reason:%s gatewayType:%d activeVoqs:%u enqBytes:%lu sentBytes:%lu blockedInflight:%lu ack:%lu cnpAck:%lu ackDeducted:%lu maxQueueBytes:%lu maxQueuePkts:%lu maxVoqInflight:%lu lhRecv:%lu lhSent:%lu lhOver:%lu lhVoqOver:%lu lhResume:%lu throttle:%lu maxLhInflight:%lu maxActiveVoq:%u voqBypassPackets:%lu voqBypassBytes:%lu time:%f\n",
           reason, m_gatewayType, activeVoqs, totalEnqueued, totalSent, totalBlocked, totalAck, totalCnpAck,
           totalAckDeducted, maxQueueBytes, maxQueueLen, maxVoqInflight, m_diagLongHaulFeedbackRecv,
           m_diagLongHaulFeedbackSent, m_diagLongHaulOverInflight, m_diagLongHaulVoqOverload,
           m_diagLongHaulResume, m_diagThrottleCalls, m_diagMaxLongHaulInflight, m_diagMaxActiveVoqCount, m_voqBypassPackets, m_voqBypassBytes,
           Simulator::Now().GetSeconds());
}

void atcGateway::init(int type,uint32_t max_voq_count,uint64_t max_voq_rate, uint64_t longHaulBandwidth,Time longHaulDelay)
{
    m_gatewayStatus = 1;
    m_gatewayType = type;
    m_maxVoqCount = max_voq_count;
    m_maxVoqRate = max_voq_rate;

    // 初始化VOQ池
    voqPool.resize(m_maxVoqCount);
    for (uint32_t i = 0; i < m_maxVoqCount; i++) {
        voqPool[i].voqId = i;
        voqPool[i].currentSize = 0;
        voqPool[i].maxSize = m_voqMaxSize;
        voqPool[i].currentInflightBytes = 0;
        voqPool[i].maxInflightBytes =  m_maxVoqRate * m_voqInflightRtt.GetSeconds() / 8.0;
        voqPool[i].isAllocated = false;
        voqPool[i].lastTransmitTime = Seconds(0);
    }
    
    InitLongHaulState(longHaulBandwidth,longHaulDelay);
}

void atcGateway::setSwitchSendToDevCallback(Callback<void, Ptr<Packet>, CustomHeader&> cb)
{
    m_sendCallBack = cb;
}

void atcGateway::DoSwitchSendToDev(Ptr<Packet> p, CustomHeader& ch)
{
    m_sendCallBack(p, ch);
}

void atcGateway::setGetOutDevCallback(Callback<Ptr<QbbNetDevice>, Ptr<Packet>, CustomHeader&> cb)
{
    m_getOutDevCallBack = cb;
}

Ptr<QbbNetDevice> atcGateway::GetOutDevice(Ptr<Packet> p, CustomHeader& ch)
{
    return m_getOutDevCallBack(p, ch);
}

uint16_t atcGateway::EtherToPpp (uint16_t proto) {
	switch (proto) {
	case 0x0800: return 0x0021;   //IPv4
	case 0x86DD: return 0x0057;   //IPv6
	default: NS_ASSERT_MSG (false, "PPP Protocol number not defined!");
	}
	return 0;
}

// ========== VOQ管理函数 ==========

// 分配VOQ
int32_t atcGateway::AllocateVoq(Ipv4Address destIp) {
    // 检查是否已分配
    auto it = destToVoqMap.find(destIp);
    if (it != destToVoqMap.end()) {
        return it->second;
    }
    
    // 查找空闲VOQ
    for (uint32_t i = 0; i < m_maxVoqCount; i++) {
        if (!voqPool[i].isAllocated) {
            voqPool[i].isAllocated = true;
            voqPool[i].destIp = destIp;
            voqPool[i].dcqcn_initialized = true;
            voqPool[i].m_rate = DataRate(m_maxVoqRate);
            voqPool[i].mlx.m_targetRate = DataRate(m_maxVoqRate);
            
            destToVoqMap[destIp] = i;
            longHaulState.activeVoqCount++;
            
            if (m_debug & 0x2) {
                printf("+++atcGateway AllocateVoq voqId:%d destIp:%d  time:%.9f\n", 
                       i, destIp.Get(),  Simulator::Now().GetSeconds());
            }
            
            return i;
        }
    }
    
    // 如果没有空闲VOQ
    return -1;
}

// 释放VOQ
void atcGateway::ReleaseVoq(uint32_t voqId) {
    if (voqId >= voqPool.size()) return;
    
    VoqEntry& voq = voqPool[voqId];
    
    if (voq.isAllocated) {
        // 取消任何待处理的转发事件
        if (voq.transmitEvent.IsRunning()) {
            Simulator::Cancel(voq.transmitEvent);
        }
        
        // 清理队列
        while (!voq.packetQueue.empty()) {
            voq.packetQueue.pop();
        }
        
        // 重置VOQ状态
        voq.isAllocated = false;
        voq.currentSize = 0;
        voq.flights.clear();
        voq.currentInflightBytes = 0;
        Simulator::Cancel(voq.mlx.m_eventUpdateAlpha);
        Simulator::Cancel(voq.mlx.m_eventDecreaseRate);
        Simulator::Cancel(voq.mlx.m_rpTimer);
        voq.mlx = {};
        voq.mlx.m_alpha = 1;
        voq.mlx.m_first_cnp = true;
        voq.lastTransmitTime = Seconds(0);
        
        // 从映射中移除
        destToVoqMap.erase(voq.destIp);
        longHaulState.activeVoqCount--;
        
        voq.destIp = Ipv4Address();

        if (m_debug & 0x2) {
                printf("+++atcGateway ReleaseVoq voqId:%d  time:%.9f\n", 
                       voqId,   Simulator::Now().GetSeconds());
        }
    }
}

// VOQ统计
void atcGateway::VoqStat() {
    uint64_t maxTotalVoqBytes = 0;
    // 查找空闲VOQ
    for (uint32_t i = 0; i < m_maxVoqCount; i++) {
        if (voqPool[i].isAllocated) {
            if (voqPool[i].currentSize > longHaulState.maxVoqBytes) {
                longHaulState.maxVoqBytes = voqPool[i].currentSize;
            }
            maxTotalVoqBytes += voqPool[i].currentSize;
        }
    }

    if (maxTotalVoqBytes > longHaulState.maxTotalVoqBytes) {
        longHaulState.maxTotalVoqBytes = maxTotalVoqBytes;
    }
    return ;
}

// 计算数据包传输时间
Time atcGateway::CalculateTransmitTime(uint32_t pktSize, DataRate rate) {
    if (rate.GetBitRate() == 0) {
        return Seconds(0);
    }
    
    uint64_t packetSizeBits = pktSize * 8;
    double transmitTimeSeconds = static_cast<double>(packetSizeBits) / rate.GetBitRate();
    
    return Seconds(transmitTimeSeconds);
}

// 从VOQ转发数据包
void atcGateway::TransmitFromVoq(uint32_t voqId) {
    if (voqId >= voqPool.size()) return;
    
    VoqEntry& voq = voqPool[voqId];

    if (!voq.isAllocated) return;
    
    if (voq.packetQueue.empty()) {
        // 如果队列变空，考虑释放VOQ
        if (!voq.voqReleaseEvent.IsRunning()) {
            voq.voqReleaseEvent = Simulator::Schedule(Seconds(0.001), &atcGateway::CheckVoqRelease, this, voqId);
        }
        return;
    }
    
    // 取出队列头部的数据包
    Ptr<Packet> pkt = voq.packetQueue.front();
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    pkt->PeekHeader(ch);
    uint32_t pktSize = pkt->GetSize();

    auto [it, inserted] = voq.flights.try_emplace(
        VoqEntry::FlowKey{ch.sip, ch.udp.sport, ch.udp.dport, ch.udp.pg});
    auto& flight = it->second;
    if (inserted)
        flight.nextSeq = ch.udp.seq;
    // Only an in-order advance creates new ACKable flight. Out-of-order
    // packets still traverse the paced output so the NIC can report the gap.
    const bool advances = ch.udp.seq == flight.nextSeq;
    if (!advances || voq.currentInflightBytes < voq.maxInflightBytes)
    {
        voq.packetQueue.pop();
        voq.currentSize -= pktSize;
        voq.diagPacketsSent++;
        voq.diagBytesSent += pktSize;

        // 转发数据包
        DoSwitchSendToDev(pkt, ch);

        // 报文统计
        if (advances) {
            flight.nextSeq = ch.udp.seq + (pktSize - ch.GetSerializedSize());
            flight.outstanding.emplace_back(flight.nextSeq, pktSize);
            voq.currentInflightBytes += pktSize;
            voq.diagMaxInflightBytes = std::max(voq.diagMaxInflightBytes, voq.currentInflightBytes);
        }
    }
    else
    {
        voq.diagBlockedByInflight++;
    }
    
    // 更新最后转发时间
    voq.lastTransmitTime = Simulator::Now();
    
    // 计算下一个数据包的转发时间
    Time transmitTime = CalculateTransmitTime(pktSize, voq.m_rate);
    /*if ( Simulator::Now().GetSeconds() >= 0.0061 && Simulator::Now().GetSeconds() <= 0.0071 ) {
        printf("+++atcGateway TransmitFromVoq voqId:%d rate:%.3f sendInterval:%lu time:%.9f\n", 
            voqId, voq->m_rate.GetBitRate() * 1e-9, transmitTime.GetTimeStep(), Simulator::Now().GetSeconds());
    }*/
    
    // 调度下一次转发
    voq.transmitEvent = Simulator::Schedule(transmitTime, 
                                            &atcGateway::TransmitFromVoq, 
                                            this, voqId);
    
    /*if (m_debug & 0x2) {
        printf("+++atcGateway TransmitFromVoq voqId:%d destIp:%d queueSize:%lu time:%.9f\n", 
               voqId, voq.destIp.Get(), voq.currentSize, Simulator::Now().GetSeconds());
    }*/
}

// 检查VOQ是否可以释放
void atcGateway::CheckVoqRelease(uint32_t voqId) {
    if (voqId >= voqPool.size()) return;
    
    VoqEntry& voq = voqPool[voqId];
    
    // 如果队列仍然为空且没有正在运行的转发事件，释放VOQ
    if (voq.packetQueue.empty() && !voq.transmitEvent.IsRunning()) {
        ReleaseVoq(voqId);
    }
}

// 调度VOQ转发
void atcGateway::ScheduleVoqForward(uint32_t voqId) {
    if (voqId >= voqPool.size()) return;
    
    VoqEntry& voq = voqPool[voqId];
    if (voq.packetQueue.empty()) {
        return;
    }
    
    // 如果已经有转发事件在运行，不重复调度
    if (voq.transmitEvent.IsRunning()) {
        return;
    }
    
    // 立即开始转发第一个数据包
    TransmitFromVoq(voqId);
}

// 获取或创建调度表项
ScheduleEntry atcGateway::GetOrCreateScheduleEntry(Ipv4Address destIp) {
    auto it = scheduleTable.find(destIp);
    if (it != scheduleTable.end()) {
        return it->second;
    }
    
    // 创建新的调度表项
    ScheduleEntry entry;
    entry.destIp = destIp;
    entry.rateLimit = m_maxVoqRate; 
    entry.isCongested = false;
    entry.lastUpdateTime = Simulator::Now();
    scheduleTable[destIp] = entry;
    return entry;
}


void atcGateway::sendCnp(CustomHeader &ch, Ptr<NetDevice> ingressDev)
{
    qbbHeader seqh;
    seqh.SetSeq(0);
    seqh.SetPG(ch.udp.pg);
    seqh.SetSport(ch.udp.dport);
    seqh.SetDport(ch.udp.sport);
    seqh.SetIntHeader(ch.udp.ih);
	seqh.SetCnp();

    Ptr<Packet> newp = Create<Packet>(std::max(60 - 14 - 20 - (int)seqh.GetSerializedSize(), 0));
    newp->AddHeader(seqh);

    Ipv4Header head;
    head.SetDestination(Ipv4Address(ch.sip));
    head.SetSource(Ipv4Address(ch.dip));
    head.SetProtocol(0xFC);
    head.SetTtl(64);
    head.SetPayloadSize(newp->GetSize());
    head.SetIdentification(UniformVariable(0, 65536).GetValue());
    newp->AddHeader(head);

    PppHeader ppp;
    ppp.SetProtocol (EtherToPpp (0x800));
    newp->AddHeader (ppp);

    MyPriorityTag pri;
    pri.SetPriority(0);
    newp->AddPacketTag(pri);
    if (ingressDev != nullptr) {
        newp->AddPacketTag(InterfaceTag(ingressDev->GetIfIndex()));
    }

    CustomHeader bfCh(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    newp->PeekHeader(bfCh);
    DoSwitchSendToDev(newp, bfCh);

    if (m_debug & 0x1)
        printf("+++atcGateway sendCnp  time:%f\n",Simulator::Now().GetSeconds());
}

/******************************
 * Mellanox's version of DCQCN (VOQ版本)
 *****************************/
void atcGateway::UpdateAlphaMlxVoq(VoqEntry *voq) {
	if (voq->mlx.m_alpha_cnp_arrived) {
		voq->mlx.m_alpha = (1 - m_g) * voq->mlx.m_alpha + m_g;
	} else {
		voq->mlx.m_alpha = (1 - m_g) * voq->mlx.m_alpha;
	}
	voq->mlx.m_alpha_cnp_arrived = false;
	ScheduleUpdateAlphaMlxVoq(voq);
}

void atcGateway::ScheduleUpdateAlphaMlxVoq(VoqEntry *voq) {
	voq->mlx.m_eventUpdateAlpha = Simulator::Schedule(MicroSeconds(m_alpha_resume_interval), &atcGateway::UpdateAlphaMlxVoq, this, voq);
}

void atcGateway::cnp_received_mlx_voq(VoqEntry *voq) {
	voq->mlx.m_alpha_cnp_arrived = true;
	voq->mlx.m_decrease_cnp_arrived = true;
	if (voq->mlx.m_first_cnp) {
		voq->mlx.m_alpha = 1;
		voq->mlx.m_alpha_cnp_arrived = false;
		ScheduleUpdateAlphaMlxVoq(voq);
		ScheduleDecreaseRateMlxVoq(voq, 1);
		voq->mlx.m_targetRate = voq->m_rate = m_rateOnFirstCNP * voq->m_rate;
		voq->mlx.m_first_cnp = false;
	}
}

void atcGateway::CheckRateDecreaseMlxVoq(VoqEntry *voq) {
	ScheduleDecreaseRateMlxVoq(voq, 0);
	if (voq->mlx.m_decrease_cnp_arrived) {
#if PRINT_LOG
		printf("+++atcGateway %lu rate dec: (%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
		bool clamp = true;
		if (!m_EcnClampTgtRate) {
			if (voq->mlx.m_rpTimeStage == 0)
				clamp = false;
		}
		if (clamp)
			voq->mlx.m_targetRate = voq->m_rate;
		voq->m_rate = std::max(m_minRate, voq->m_rate * (1 - voq->mlx.m_alpha / 2));
		voq->mlx.m_rpTimeStage = 0;
		voq->mlx.m_decrease_cnp_arrived = false;
		Simulator::Cancel(voq->mlx.m_rpTimer);
		voq->mlx.m_rpTimer = Simulator::Schedule(MicroSeconds(m_rpgTimeReset), &atcGateway::RateIncEventTimerMlxVoq, this, voq);
#if PRINT_LOG
		printf("(%.3lf %.3lf)\n", voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
	}
}

void atcGateway::ScheduleDecreaseRateMlxVoq(VoqEntry *voq, uint32_t delta) {
	voq->mlx.m_eventDecreaseRate = Simulator::Schedule(MicroSeconds(m_rateDecreaseInterval) + NanoSeconds(delta), &atcGateway::CheckRateDecreaseMlxVoq, this, voq);
}

void atcGateway::RateIncEventTimerMlxVoq(VoqEntry *voq) {
	voq->mlx.m_rpTimer = Simulator::Schedule(MicroSeconds(m_rpgTimeReset), &atcGateway::RateIncEventTimerMlxVoq, this, voq);
	RateIncEventMlxVoq(voq);
	voq->mlx.m_rpTimeStage++;
}

void atcGateway::RateIncEventMlxVoq(VoqEntry *voq) {
	if (voq->mlx.m_rpTimeStage < m_rpgThreshold) {
		FastRecoveryMlxVoq(voq);
	} else if (voq->mlx.m_rpTimeStage == m_rpgThreshold) {
		ActiveIncreaseMlxVoq(voq);
	} else {
		HyperIncreaseMlxVoq(voq);
	}
}

void atcGateway::FastRecoveryMlxVoq(VoqEntry *voq) {
#if PRINT_LOG
	printf("+++atcGateway %lu fast recovery: (%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
	voq->m_rate = (voq->m_rate / 2) + (voq->mlx.m_targetRate / 2);

#if PRINT_LOG
	printf("(%.3lf %.3lf)\n", voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
}

void atcGateway::ActiveIncreaseMlxVoq(VoqEntry *voq) {
#if PRINT_LOG
	printf("+++atcGateway %lu active inc:  (%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
	voq->mlx.m_targetRate += m_rai;
	if (voq->mlx.m_targetRate > DataRate(m_maxVoqRate))
		voq->mlx.m_targetRate = DataRate(m_maxVoqRate);
	voq->m_rate = (voq->m_rate / 2) + (voq->mlx.m_targetRate / 2);
#if PRINT_LOG
	printf("(%.3lf %.3lf)\n", voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
}

void atcGateway::HyperIncreaseMlxVoq(VoqEntry *voq) {
#if PRINT_LOG
	printf("+++atcGateway %lu hyper inc:(%0.3lf %.3lf)->", Simulator::Now().GetTimeStep(), voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
	voq->mlx.m_targetRate += m_rhai;
	if (voq->mlx.m_targetRate > DataRate(m_maxVoqRate))
		voq->mlx.m_targetRate = DataRate(m_maxVoqRate);
    if ((voq->mlx.m_rpTimeStage >= 5) &&  voq->mlx.m_targetRate <  DataRate(m_maxVoqRate))
		voq->mlx.m_targetRate =  DataRate(m_maxVoqRate);
	voq->m_rate = (voq->m_rate / 2) + (voq->mlx.m_targetRate / 2);
#if PRINT_LOG
	printf("(%.3lf %.3lf)\n", voq->mlx.m_targetRate.GetBitRate() * 1e-9, voq->m_rate.GetBitRate() * 1e-9);
#endif
}

// ========== 主要的packetIn函数（VOQ版本） ==========
bool atcGateway::packetIn(Ptr<NetDevice> device, Ptr<Packet> pkt, CustomHeader &ch)
{
    if (m_gatewayStatus != 1) {
        return false;
    }

    if (m_gatewayType == 1) { // 发送端门廊
        if (ch.l3Prot == 0x11) { // RDMA UDP数据报文
            // A collapsed spine/DCI also forwards local traffic. Only WAN
            // egresses belong to the source gateway's long-haul accounting.
            auto egress = GetOutDevice(pkt, ch);
            if (!egress || !egress->IsWanPort())
                return false;
            m_ecnbits = ch.GetIpv4EcnBits();
            bool triggerByPacketEcn = (m_ecnbits != 0);
            bool triggerByGlobalCongestion = (m_cnpOnGlobalCongestion && m_congested);
            if (triggerByPacketEcn || triggerByGlobalCongestion) {
                if (!(cnpNotifyTime.count(ch.sip) > 0 && (Simulator::Now() - cnpNotifyTime[ch.sip]) < m_cnpNotifyInterval)) {
                    cnpNotifyTime[ch.sip] = Simulator::Now();
                    if (triggerByPacketEcn) {
                        m_cnpByPacketEcn++;
                    } else {
                        m_cnpByGlobalCongestion++;
                    }
                    sendCnp(ch, device); // 回复CNP
                    uint64_t cnpTotal = m_cnpByPacketEcn + m_cnpByGlobalCongestion;
                    if ((m_debug & 0x2) && (cnpTotal <= 20 || cnpTotal % 500 == 0)) {
                        printf("+++atcGateway CNP reason total:%lu packetEcn:%lu globalCong:%lu lastEcn:%u globalState:%u time:%f\n",
                               cnpTotal, m_cnpByPacketEcn, m_cnpByGlobalCongestion,
                               m_ecnbits, m_congested ? 1 : 0, Simulator::Now().GetSeconds());
                    }
                }
            }
            return false;
        }
        else if (ch.l3Prot == 0x6) { // LongHaulFeedback
            auto ingress = DynamicCast<QbbNetDevice>(device);
            if (!ingress || !ingress->IsWanPort())
                return false;
            return HandleLongHaulFeedback(device, pkt, ch);
        }
    }
    else if (m_gatewayType == 2) { // 接收端门廊（VOQ模式）
        if (ch.l3Prot == 0x11) { // RDMA UDP数据报文
            auto ingress = DynamicCast<QbbNetDevice>(device);
            if (!ingress || !ingress->IsWanPort())
                return false;
            return HandleDataPacketWithVoq(device, pkt, ch);
        }
        else if (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD) { // cumulative ACK or go-back-N NACK
            auto egress = GetOutDevice(pkt, ch);
            if (!egress || !egress->IsWanPort())
                return false;
            return HandleAckPacketWithVoq(device, pkt, ch);
        }
    }

    return false;
}

// 使用VOQ处理数据包
bool atcGateway::HandleDataPacketWithVoq(Ptr<NetDevice> device, Ptr<Packet> pkt, CustomHeader &ch) {

    // 提取目的IP
    Ipv4Address destIp = Ipv4Address(ch.dip);
    
    // 创建清理ECN标记的数据包副本
    Ptr<Packet> cleanPkt = pkt->Copy();
    if (ch.GetIpv4EcnBits()) { // 清除ECN标记
        PppHeader ppp;
        Ipv4Header h;
        cleanPkt->RemoveHeader(ppp);
        cleanPkt->RemoveHeader(h);
        h.SetEcn((Ipv4Header::EcnType)0x0);
        cleanPkt->AddHeader(h);
        cleanPkt->AddHeader(ppp);
        ch.m_tos &= 0xFC;
    }



    Ptr<QbbNetDevice> qbbDevice = DynamicCast<QbbNetDevice>(device);
    if (qbbDevice && qbbDevice->IsWanPort()) {
        RegisterLongHaulPeer(qbbDevice->GetIfIndex(), qbbDevice->GetNode()->GetId(), ch, qbbDevice);
    }
    
    const uint32_t packetBytes = cleanPkt->GetSize();
    const uint32_t requiredSpace = std::max<uint32_t>(device->GetMtu(), packetBytes);
    const int32_t voqId = requiredSpace <= m_voqMaxSize ? AllocateVoq(destIp) : -1;
    // ATC §IV-B: bypass the VOQ when less than one MTU remains. Direct
    // packets go through normal switch admission and count as WAN service.
    if (voqId < 0 || voqPool[voqId].maxSize - voqPool[voqId].currentSize < requiredSpace) {
        ++m_voqBypassPackets;
        m_voqBypassBytes += packetBytes;
        DoSwitchSendToDev(cleanPkt, ch);
        return true;
    }

    VoqEntry& voq = voqPool[voqId];
    // 将清理后的数据包存储到VOQ
    voq.packetQueue.push(cleanPkt);
    voq.currentSize += cleanPkt->GetSize();
    voq.diagPacketsEnqueued++;
    voq.diagBytesEnqueued += cleanPkt->GetSize();
    voq.diagMaxCurrentSize = std::max(voq.diagMaxCurrentSize, voq.currentSize);
    voq.diagMaxQueueLen = std::max<uint64_t>(voq.diagMaxQueueLen, voq.packetQueue.size());
    VoqStat();

    
    // 触发VOQ转发（使用VOQ自身的速率）
    if (!voq.transmitEvent.IsRunning()) {
        ScheduleVoqForward(voqId);
    }
    
    /*if (m_debug & 0x2) {
        printf("+++atcGateway VOQ store destIp:%d voqId:%d queueSize:%lu  time:%.9f\n", 
               destIp.Get(), voqId, voq.currentSize,  Simulator::Now().GetSeconds());
    }*/
    
    return true; // 拦截原始数据包
}

// 使用VOQ处理ACK包
bool atcGateway::HandleAckPacketWithVoq(Ptr<NetDevice> device, Ptr<Packet> pkt, CustomHeader &ch) {
    uint8_t cnp = (ch.ack.flags >> qbbHeader::FLAG_CNP) & 1;
    
    // 提取目的IP（对于ACK包，目的IP是原始数据包的源IP）
    Ipv4Address destIp = Ipv4Address(ch.sip);
    
    // 查找对应的VOQ
    auto voqIt = destToVoqMap.find(destIp);
    if (voqIt != destToVoqMap.end()) {
        uint32_t voqId = voqIt->second;
        VoqEntry& voq = voqPool[voqId];

        auto it = voq.flights.find({ch.dip, ch.ack.dport, ch.ack.sport, ch.ack.pg});
        if (it != voq.flights.end() && ch.ack.seq >= it->second.acknowledged) {
            auto& flight = it->second;
            flight.acknowledged = ch.ack.seq;
            while (!flight.outstanding.empty() && flight.outstanding.front().first <= ch.ack.seq) {
                const uint32_t bytes = flight.outstanding.front().second;
                voq.currentInflightBytes -= bytes;
                voq.diagAckBytesDeducted += bytes;
                flight.outstanding.pop_front();
            }
            if (ch.l3Prot == 0xFD) {
                // The receiver discarded this suffix and requested go-back-N.
                // Release its credit; later attempts are charged as they advance.
                for (const auto& packet : flight.outstanding)
                    voq.currentInflightBytes -= packet.second;
                flight.outstanding.clear();
                flight.nextSeq = ch.ack.seq;
            } else {
                flight.nextSeq = std::max(flight.nextSeq, ch.ack.seq);
            }
        }
        voq.diagAckCount++;
        // 处理CNP
        if (cnp) {
            voq.diagCnpAckCount++;
            // 使用VOQ版本的DCQCN处理CNP
            if (m_voqReactToCnp) {
                cnp_received_mlx_voq(&voq);
            }
            
            if (m_debug & 0x2) {
                printf("+++atcGateway CNP received for VOQ voqId:%d destIp:%d newRate:%lu time:%.9f\n", 
                       voqId, destIp.Get(), voq.m_rate.GetBitRate(), Simulator::Now().GetSeconds());
            }
            
            return false; // 转发携带CNP标记的ACK报文给上游
        }
    }
    
    return false; // 转发携带CNP标记的ACK报文给上游
}

// 限流本地转发速率
void atcGateway::ThrottleLocalForwarding(Ptr<NetDevice> dev, uint64_t rate) {
    if (!dev) return;
    
    Ptr<QbbNetDevice> qbbDev = DynamicCast<QbbNetDevice>(dev);
    if (qbbDev) {
        m_diagThrottleCalls++;
        // 使用QBB设备的速率控制
        qbbDev->SetDataRate(DataRate(rate));
    }
}

// 处理LongHaulFeedback
bool atcGateway::HandleLongHaulFeedback(Ptr<NetDevice> device, Ptr<Packet> pkt, CustomHeader &ch) {

    LongHaulState& state = sourceWanStates.empty() ? longHaulState :
        sourceWanStates.at(device->GetIfIndex());

    uint64_t remoteForwardedBytes =  (((uint64_t)ch.tcp.ack) << 32) | (uint64_t)ch.tcp.seq;
    uint32_t activeVoqCount = ch.tcp.dport;
    uint64_t rateLimit = 0;
    bool limit = false;
    m_diagLongHaulFeedbackRecv++;
    m_diagMaxActiveVoqCount = std::max(m_diagMaxActiveVoqCount, activeVoqCount);
    if ((m_diagLongHaulFeedbackRecv % 1000) == 0) {
        PrintAtcDebugSummary("periodic-longhaul-recv");
    }

    if (m_debug & 0x2)
         printf("+++atcGateway recv LongHaulFeedback localSentBytes:%lu remoteForwardedBytes:%lu  inflightBytes:%lu activeVoqCount:%d  time:%f node:%u sourceIf:%u counterMismatch:%u\n",
            state.localSentBytes,remoteForwardedBytes,
            state.localSentBytes >= remoteForwardedBytes ? state.localSentBytes - remoteForwardedBytes : 0,
            activeVoqCount,Simulator::Now().GetSeconds(),device->GetNode()->GetId(),device->GetIfIndex(),
            remoteForwardedBytes > state.localSentBytes ? 1u : 0u);

    if (state.localSentBytes >= remoteForwardedBytes) {
        uint64_t inflightBytes = state.localSentBytes - remoteForwardedBytes;
        m_diagMaxLongHaulInflight = std::max(m_diagMaxLongHaulInflight, inflightBytes);
        
        // 若在途字节数超过阈值，限流
        if (inflightBytes > state.maxInflightBytes) {
            m_diagLongHaulOverInflight++;
            
            if (state.inflightBytes > 0 && state.lastLocalSentBytes > 0 && inflightBytes > state.inflightBytes && state.localSentBytes > state.lastLocalSentBytes)
            {
                double ratio = 0.9;
                ratio = (double) (inflightBytes - state.inflightBytes) / (state.localSentBytes - state.lastLocalSentBytes);
                if (ratio >= 0.05 && ratio <= 0.95)
                {
                    double K = 1.0 / m_maxVoqCount;
                    ratio = 1.0 - ratio;
                    rateLimit = ratio*state.longHaulBandwidth;
                    if ((rateLimit != state.rateLimit) && (ratio >= K)) //波动才限流
                    {
                        ThrottleLocalForwarding(device, rateLimit); // 限流
                    }
                    state.rateLimit = rateLimit;
                }
                limit = true;
                if (m_debug & 0x2)
                    printf("+++atcGateway  LongHaulFeedback overinfligh inflightBytes:%lu lastinflightBytes:%lu rateLimit:%lu ratio:%f time:%f\n",inflightBytes,state.inflightBytes,state.rateLimit,ratio,Simulator::Now().GetSeconds());
            }
        }
        
        state.remoteForwardedBytes = remoteForwardedBytes;
        state.inflightBytes = inflightBytes;
     }
     state.lastLocalSentBytes = state.localSentBytes;

    
    if (activeVoqCount <= m_maxVoqCount) {
        // VOQ过载时限流
        state.activeVoqCount = activeVoqCount;
        bool voqOverload = activeVoqCount > (N_THRESHOLD_RATIO * m_maxVoqCount);
        if (voqOverload) {
            m_diagLongHaulVoqOverload++;
            double K = 1.0 / m_maxVoqCount;
            rateLimit = K*state.longHaulBandwidth;
            if (rateLimit != state.rateLimit) //波动才限流
            {
                ThrottleLocalForwarding(device, rateLimit); // 限流
            }
            state.rateLimit = rateLimit;
            limit = true;
            if (m_debug & 0x2)
                printf("+++atcGateway  LongHaulFeedback voqOverload activeVoqCount:%d K:%f rateLimit:%lu time:%f\n",activeVoqCount,K,state.rateLimit,Simulator::Now().GetSeconds());
        }
    }

    if (!limit && state.rateLimit < state.longHaulBandwidth) {
        m_diagLongHaulResume++;
        state.rateLimit = state.longHaulBandwidth;
        ThrottleLocalForwarding(device, state.rateLimit);
        if (m_debug & 0x2)
             printf("+++atcGateway LongHaulFeedback resume sendRate rateLimit:%lu time:%f\n",state.rateLimit,Simulator::Now().GetSeconds());
    }

    return true; 
}

// 初始化长距离状态
void atcGateway::ConfigureSourceWanPort(uint32_t port, uint64_t bandwidth, Time delay) {
    auto& state = sourceWanStates[port];
    state = longHaulState;
    state.longHaulBandwidth = bandwidth;
    state.longHaulDelay = delay;
    state.maxInflightBytes = bandwidth * (M * delay.GetSeconds() +
                                             LONGHAUL_FEEDBACK_PERIOD.GetSeconds()) / 8;
    state.rateLimit = bandwidth;
}

void atcGateway::InitLongHaulState(uint64_t longHaulBandwidth,Time longHaulDelay) {
    sourceWanStates.clear();
    longHaulState.longHaulBandwidth = longHaulBandwidth;
    longHaulState.longHaulDelay = longHaulDelay;
    longHaulState.localSentBytes = 0;
    longHaulState.lastLocalSentBytes = 0;
    longHaulState.lastLocalSentBytes = 0;
    longHaulState.remoteForwardedBytes = 0;
    longHaulState.inflightBytes = 0;
    longHaulState.maxVoqBytes = 0;
    longHaulState.maxTotalVoqBytes = 0;
    longHaulState.activeVoqCount = 0;
    if (longHaulFeedbackEvent.IsRunning()) {
        Simulator::Cancel(longHaulFeedbackEvent);
    }
    longHaulFeedbackPeers.clear();
    longHaulState.maxInflightBytes = (M * (longHaulState.longHaulBandwidth * longHaulState.longHaulDelay.GetSeconds()) + 
                        longHaulState.longHaulBandwidth * LONGHAUL_FEEDBACK_PERIOD.GetSeconds())/8;
    longHaulState.rateLimit = longHaulState.longHaulBandwidth; // 初始速率400Gbps
    if (m_debug & 0x2)
        printf("+++atcGateway longHaulState maxInflightBytes:%lu , rateLimit:%f Gbps, time:%f\n",longHaulState.maxInflightBytes,longHaulState.rateLimit*1e-9,Simulator::Now().GetSeconds());
}

void atcGateway::RegisterLongHaulPeer(uint32_t ingressIfIndex, uint32_t nodeId,
                                     const CustomHeader& ch, Ptr<QbbNetDevice> device) {
    auto inserted = longHaulFeedbackPeers.emplace(ingressIfIndex, LongHaulFeedbackPeer());
    if (!inserted.second) {
        return;
    }
    inserted.first->second.routeHeader = ch;
    inserted.first->second.nodeId = nodeId;
    inserted.first->second.wanDevice = device;
    if (m_debug & 0x2) {
        printf("+++atcGateway longHaulPeer node:%u peerIf:%u peerSip:%u time:%f\n",
               nodeId, ingressIfIndex, ch.sip, Simulator::Now().GetSeconds());
    }
    if (longHaulFeedbackPeers.size() == 1) {
        StartLongHaulFeedbackTimer();
    }
}

void atcGateway::RecordDataDequeue(Ptr<QbbNetDevice> egress, Ptr<const Packet> packet) {
    if (m_gatewayType == 1 && egress->IsWanPort()) {
        longHaulState.localSentBytes += packet->GetSize();
        if (!sourceWanStates.empty())
            sourceWanStates.at(egress->GetIfIndex()).localSentBytes += packet->GetSize();
        return;
    }
    if (m_gatewayType != 2 || egress->IsWanPort()) return;

    // The WAN ingress tag remains attached until the local egress dequeues.
    InterfaceTag ingress;
    if (!packet->PeekPacketTag(ingress)) {
        return;
    }
    auto peer = longHaulFeedbackPeers.find(ingress.GetPortId());
    if (peer != longHaulFeedbackPeers.end()) {
        peer->second.remoteForwardedBytes += packet->GetSize();
    }
}


// 启动长距离反馈定时器
void atcGateway::StartLongHaulFeedbackTimer() {
    longHaulFeedbackEvent = Simulator::Schedule(LONGHAUL_FEEDBACK_PERIOD, 
                                                &atcGateway::GenerateLongHaulFeedback, this);
}

// 生成长距离反馈
void atcGateway::GenerateLongHaulFeedback() {
    for (const auto& entry : longHaulFeedbackPeers) {
        const uint32_t ingressIfIndex = entry.first;
        const LongHaulFeedbackPeer& peer = entry.second;
        if (peer.wanDevice && !peer.wanDevice->IsLinkUp())
            continue;
        // 用TCP头封装LongHaulFeedback
        TcpHeader seqh;
        seqh.SetDestinationPort(longHaulState.activeVoqCount);
        seqh.SetSequenceNumber(SequenceNumber32(static_cast<uint32_t>(peer.remoteForwardedBytes)));
        seqh.SetAckNumber(SequenceNumber32(static_cast<uint32_t>(peer.remoteForwardedBytes >> 32)));

        Ptr<Packet> newp = Create<Packet>(std::max(60 - 14 - 20 - (int)seqh.GetSerializedSize(), 0));
        newp->AddHeader(seqh);

        Ipv4Header head;
        head.SetDestination(Ipv4Address(peer.routeHeader.sip));
        head.SetSource(Ipv4Address(peer.routeHeader.dip));
        head.SetProtocol(0x6); //tcp
        head.SetTtl(64);
        head.SetPayloadSize(newp->GetSize());
        head.SetIdentification(UniformVariable(0, 65536).GetValue());
        newp->AddHeader(head);

        PppHeader ppp;
        ppp.SetProtocol (EtherToPpp (0x800));
        newp->AddHeader (ppp);

        MyPriorityTag pri;
        pri.SetPriority(0);
        newp->AddPacketTag(pri);
        newp->AddPacketTag(InterfaceTag(ingressIfIndex));

        CustomHeader bfCh(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
        newp->PeekHeader(bfCh);
        // Return on the exact WAN that owns this counter, before ECMP can
        // select another source gateway or another source WAN interface.
        if (peer.wanDevice)
            peer.wanDevice->SwitchSend(0, newp, bfCh);
        else
            DoSwitchSendToDev(newp, bfCh);
        m_diagLongHaulFeedbackSent++;
        if ((m_diagLongHaulFeedbackSent % 1000) == 0) {
            PrintAtcDebugSummary("periodic-longhaul");
        }

        if (m_debug & 0x2)
            printf("+++atcGateway  send LongHaulFeedback remoteForwardedBytes:%lu, activeVoqCount:%d  time:%f node:%u peerIf:%u peerSip:%u\n",peer.remoteForwardedBytes,longHaulState.activeVoqCount,Simulator::Now().GetSeconds(),peer.nodeId,ingressIfIndex,peer.routeHeader.sip);
    }

    // 重置定时器
    if (!longHaulFeedbackPeers.empty()) {
        StartLongHaulFeedbackTimer();
    }
}


}
