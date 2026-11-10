#ifndef RDMA_QUEUE_PAIR_H
#define RDMA_QUEUE_PAIR_H

#include <ns3/object.h>
#include <ns3/packet.h>
#include <ns3/ipv4-address.h>
#include <ns3/data-rate.h>
#include <ns3/event-id.h>
#include <ns3/custom-header.h>
#include <ns3/int-header.h>
#include <vector>
//vamsi
#include <map>

namespace ns3 {

class RdmaQueuePair : public Object {
public:
	Time startTime;
	Ipv4Address sip, dip;
	uint16_t sport, dport;
	uint64_t m_size;
	uint64_t snd_nxt, snd_una; // next seq to send, the highest unacked seq
	uint16_t m_pg;
	uint16_t m_ipid;
	uint32_t m_win; // bound of on-the-fly packets
	uint32_t m_winFloor; // one recovery chunk when go-back-to-zero is enabled
	uint64_t m_baseRtt; // base RTT of this qp
	DataRate m_max_rate; // max rate
	bool m_var_win; // variable window size
	Time m_nextAvail;	//< Soonest time of next send
	uint32_t wp; // current window of packets
	uint32_t lastPktSize;
	uint32_t lastPayloadSize;
	Callback<void> m_notifyAppFinish;

// vamsi
	std::map<uint32_t,double> rates;
	double prevRtt;
	double prevCompletion;
	bool powerEnabled;
	Time stopTime;

	/******************************
	 * runtime states
	 *****************************/
	DataRate m_rate;	//< Current rate
	struct {
		DataRate m_targetRate;	//< Target rate
		EventId m_eventUpdateAlpha;
		double m_alpha;
		bool m_alpha_cnp_arrived; // indicate if CNP arrived in the last slot
		bool m_first_cnp; // indicate if the current CNP is the first CNP
		EventId m_eventDecreaseRate;
		bool m_decrease_cnp_arrived; // indicate if CNP arrived in the last slot
		uint32_t m_rpTimeStage;
		uint32_t m_rpCountStage;
		uint64_t m_rpByteCount;
		bool m_byteRecoveryActive;
		EventId m_rpTimer;
	} mlx;
	struct {
		uint32_t m_lastUpdateSeq;
		DataRate m_curRate;
		IntHop hop[IntHeader::maxHop];
		uint32_t keep[IntHeader::maxHop];
		uint32_t m_incStage;
		double m_lastGap;
		double u;
		struct {
			double u;
			double qRate;
			DataRate Rc;
			uint32_t incStage;
		}hopState[IntHeader::maxHop];
	} hp;
	struct {
			uint32_t m_lastUpdateSeq;
			DataRate m_curRate;
			IntHop hop[IntHeader::maxHop];
			uint32_t keep[IntHeader::maxHop];
			uint32_t m_incStage;
			double m_lastGap;
			double u;
			bool useInt;
			struct {
				double u;
				DataRate Rc;
				uint32_t incStage;
			}hopState[IntHeader::maxHop];
		} power;
	struct{
		uint32_t m_lastUpdateSeq;
		DataRate m_curRate;
		uint32_t m_incStage;
		uint64_t lastRtt;
		double rttDiff;
	} tmly;
	struct{
		uint32_t m_lastUpdateSeq;
		uint32_t m_caState;
		uint32_t m_highSeq; // when to exit cwr
		double m_alpha;
		uint32_t m_ecnCnt;
		uint32_t m_batchSizeOfAlpha;
	} dctcp;
	struct{
		uint32_t m_lastUpdateSeq;
		DataRate m_curRate;
		uint32_t m_incStage;
	}hpccPint;

	/**************************
	 * FRP (Fair Rate Protocol)
	 **********************/
	struct {
		uint16_t m_bottleneckCpId;       // 瓶颈拥塞点ID (Switch节点ID 12位+PortID 4位)
		DataRate m_bottleneckRate;       // 瓶颈速率
		Time m_lastBottleneckUpdate;     // 上次瓶颈更新时间
		bool m_timerExpired;             // 定时器是否超时
	} frp;

	struct {
		uint64_t lastSeq;
		Time lastTime;
		EventId m_eventId;
	} txRate;

	// ========== ePFC (ccMode=17)：发送方 CCF 设速状态 ==========
	double m_epfcLimitBps;      // 最近一次 CCF 的 rv（设速本体在 EpfcCcfRecv：m_rate=tgt=rv；此值作窗口内防爬升钳制，bps）
	Time m_epfcLimitUntil;      // 设速维持窗口截止时刻（暂停结束后再维持 2×RTT_internal；过期自动失效交回 DCQCN）
	Time m_epfcPauseUntil;      // 排空暂停截止时刻：收到 CCF 先停发，排空超量在途后再按 rv 发送
	                             //   暂停时长 Tp=(线速-rv)×RTT_internal/rv（多余在途量按 rv 排空所需时间）

	/***********
	 * methods
	 **********/
	static TypeId GetTypeId (void);
	RdmaQueuePair(uint16_t pg, Ipv4Address _sip, Ipv4Address _dip, uint16_t _sport, uint16_t _dport);
	void SetSize(uint64_t size);
	void SetWin(uint32_t win);
	void SetBaseRtt(uint64_t baseRtt);
	void SetVarWin(bool v);
	void SetAppNotifyCallback(Callback<void> notifyAppFinish);

	uint64_t GetBytesLeft();
	uint32_t GetHash(void);
	void Acknowledge(uint64_t ack);
	uint64_t GetOnTheFly();
	bool IsWinBound();
	uint64_t GetWin(); // window size calculated from m_rate
	bool IsFinished();
	uint64_t HpGetCurWin(); // window size calculated from hp.m_curRate, used by HPCC
	uint32_t incastFlow;

protected:
	void DoDispose() override;
};

class RdmaRxQueuePair : public Object { // Rx side queue pair
public:
	struct ECNAccount{
		uint16_t qIndex;
		uint8_t ecnbits;
		uint16_t qfb;
		uint16_t total;

		ECNAccount() { memset(this, 0, sizeof(ECNAccount));}
	};
	ECNAccount m_ecn_source;
	uint32_t sip, dip;
	uint16_t sport, dport;
	uint16_t m_ipid;
	uint32_t ReceiverNextExpectedSeq;
	Time m_nackTimer;
	uint32_t m_milestone_rx;
	uint32_t m_lastNACK;
	EventId QcnTimerEvent; // if destroy this rxQp, remember to cancel this timer
	uint64_t m_recv_bytes; // 累计按序接收的 payload 字节数（用于接收侧 goodput 统计）
	
		// ePFC (ccMode=17)：CCF 节流与 rv 采样状态（接收方 host 侧）
		Time m_epfcLastCcfTime;   // 上次 CCF 发出时刻（节流：每 RTT_internal 最多一个）
		Time m_epfcSampleTime;    // rv 采样窗口起点
		uint64_t m_epfcSampleBytes; // rv 采样窗口起点的累计接收字节

	static TypeId GetTypeId (void);
	RdmaRxQueuePair();
	uint32_t GetHash(void);
};

class RdmaQueuePairGroup : public Object {
public:
	std::vector<Ptr<RdmaQueuePair> > m_qps;
	//std::vector<Ptr<RdmaRxQueuePair> > m_rxQps;

	static TypeId GetTypeId (void);
	RdmaQueuePairGroup(void);
	uint32_t GetN(void);
	Ptr<RdmaQueuePair> Get(uint32_t idx);
	Ptr<RdmaQueuePair> operator[](uint32_t idx);
	void AddQp(Ptr<RdmaQueuePair> qp);
	//void AddRxQp(Ptr<RdmaRxQueuePair> rxQp);
	void Clear(void);
	void MaybeShrink();

protected:
	void DoDispose() override;
};

}

#endif /* RDMA_QUEUE_PAIR_H */
