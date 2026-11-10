/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
* Copyright (c) 2006 Georgia Tech Research Corporation, INRIA
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation;
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
* Author: Yibo Zhu <yibzh@microsoft.com>
*/
#ifndef QBB_NET_DEVICE_H
#define QBB_NET_DEVICE_H

#include "ns3/point-to-point-net-device.h"
#include "ns3/qbb-channel.h"
//#include "ns3/fivetuple.h"
#include "ns3/event-id.h"
#include "ns3/broadcom-egress-queue.h"
#include "ns3/ipv4.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"
#include "ns3/rdma-queue-pair.h"
#include <vector>
#include<map>
// #include <ns3/rdma.h>
// #define ENABLE_QP 1

namespace ns3 {

class RdmaEgressQueue : public Object{
public:
	static const uint32_t qCnt = 8;
	static uint32_t ack_q_idx;
	static uint32_t tcpip_q_idx;
	int m_qlast;
	uint32_t m_rrlast;
	Ptr<DropTailQueue<Packet>> m_ackQ; // highest priority queue
	Ptr<RdmaQueuePairGroup> m_qpGrp; // queue pairs

	// callback for get next packet
	typedef Callback<Ptr<Packet>, Ptr<RdmaQueuePair> > RdmaGetNxtPkt;
	RdmaGetNxtPkt m_rdmaGetNxtPkt;

	static TypeId GetTypeId (void);
	RdmaEgressQueue();
	Ptr<Packet> DequeueQindex(int qIndex);
	int GetNextQindex(bool paused[]);
	int GetLastQueue();
	uint32_t GetNBytes(uint32_t qIndex);
	uint32_t GetFlowCount(void);
	Ptr<RdmaQueuePair> GetQp(uint32_t i);
	void RecoverQueue(uint32_t i);
	void EnqueueHighPrioQ(Ptr<Packet> p);
	void CleanHighPrio(TracedCallback<Ptr<const Packet>, uint32_t> dropCb);
	void CompactFinishedQps();

	TracedCallback<Ptr<const Packet>, uint32_t> m_traceRdmaEnqueue;
	TracedCallback<Ptr<const Packet>, uint32_t> m_traceRdmaDequeue;

	Ptr<QbbNetDevice> qb_dev;
	bool dummy_paused[8];
	uint64_t hostDequeueIndex;

protected:
	void DoDispose() override;
};

/**
 * \class QbbNetDevice
 * \brief A Device for a IEEE 802.1Qbb Network Link.
 */
class QbbNetDevice : public PointToPointNetDevice 
{
public:
  static const uint32_t qCnt = 8;	// Number of queues/priorities used

  static TypeId GetTypeId (void);

  QbbNetDevice ();
  virtual ~QbbNetDevice ();

  /**
   * Receive a packet from a connected PointToPointChannel.
   *
   * This is to intercept the same call from the PointToPointNetDevice
   * so that the pause messages are honoured without letting
   * PointToPointNetDevice::Receive(p) know
   *
   * @see PointToPointNetDevice
   * @param p Ptr to the received packet.
   */
  virtual void Receive (Ptr<Packet> p);
  
  // 内部接收函数（延迟后调用）
  void DoReceive (Ptr<Packet> p);

  /**
   * Send a packet to the channel by putting it to the queue
   * of the corresponding priority class
   *
   * @param packet Ptr to the packet to send
   * @param dest Unused
   * @param protocolNumber Protocol used in packet
   */
  virtual bool Send(Ptr<Packet> packet, const Address &dest, uint16_t protocolNumber);
  virtual bool SwitchSend (uint32_t qIndex, Ptr<Packet> packet, CustomHeader &ch);
  Address GetRemote (void) const;
  virtual void SetReceiveCallback (NetDevice::ReceiveCallback cb);


  DataRate GetDataRate();

  /**
   * Get the size of Tx buffer available in the device
   *
   * @return buffer available in bytes
   */
  //virtual uint32_t GetTxAvailable(unsigned) const;

  /**
   * TracedCallback hooks
   */
  void ConnectWithoutContext(const CallbackBase& callback);
  void DisconnectWithoutContext(const CallbackBase& callback);

  bool Attach (Ptr<QbbChannel> ch);

   virtual Ptr<Channel> GetChannel (void) const;

   void SetQueue (Ptr<BEgressQueue> q);
   Ptr<BEgressQueue> GetQueue ();

   void SetQueueFifo (Ptr<DropTailQueue<Packet>> q);
   Ptr<DropTailQueue<Packet>> GetQueueFifo ();

   virtual bool IsQbb(void) const;
   void NewQp(Ptr<RdmaQueuePair> qp);
   void ReassignedQp(Ptr<RdmaQueuePair> qp);
   void TriggerTransmit(void);

	void SendPfc(uint32_t qIndex, uint32_t type); // type: 0 = pause, 1 = resume

	// ========== 长链路（WAN）端口标志：由 workload 拓扑解析显式设置 ==========
	// 真值来源 = topology 文件第 3 行的长链路编号列表（如 "14" = 第 14 条链路）；
	// 取代各模块自判 channel delay > 100us 的旧方案（阈值去留由拓扑声明决定）
	void SetWanPort(bool on) { m_wanPort = on; }
	bool IsWanPort(void) const { return m_wanPort; }
	bool m_wanPort;

	// ========== DCI-Ext (ccMode=16): 本地端口门控（独立于 PFC m_paused） ==========
	void SetDciExtGate(uint32_t qIndex, bool on);  // on=true 暂停受管数据 PG 出队
	bool m_dciExtGated[qCnt];  // DCI-Ext 门控标志（默认全 false，惰性）

	// ========== ePFC (ccMode=17): 实际出队时刻的 S 计量回调（SwitchNode 注入） ==========
	// 出队点（TransmitStart 前）回调，使 DataDequeue 的 S 口径 = 物理发出字节；
	// 门控中被拦的包不出队、不经过此处，天然不计 S
	void SetEpfcDequeueCallback(Callback<void, uint32_t, Ptr<Packet>> cb) { m_epfcDequeueCb = cb; }
	Callback<void, uint32_t, Ptr<Packet>> m_epfcDequeueCb;
	void SetIgnoreReceivedPfc(bool on) { m_ignoreReceivedPfc = on; }  // 设置是否忽略收到的 PFC
	bool m_ignoreReceivedPfc;  // 忽略收到的 PFC 帧（DCI 长链路端口防死锁）

	// ========== Bifrost: 自定义暂停时间的PFC发送 ==========
	void SendPfcWithTime(uint32_t qIndex, uint32_t type, uint32_t customTime);  // type: 0 = pause, 1 = resume

	TracedCallback<Ptr<const Packet>, uint32_t> m_traceEnqueue;
	TracedCallback<Ptr<const Packet>, uint32_t> m_traceDequeue;
	TracedCallback<Ptr<const Packet>, uint32_t> m_traceDrop;
	TracedCallback<uint32_t> m_tracePfc; // 0: resume, 1: pause
	uint64_t getTxBytes(){
				uint64_t temp=numTxBytes;
				numTxBytes=0;
				return temp;
			}
	uint64_t numTxBytes=0;
	uint64_t numTxBytesLast=0;
	uint64_t totalBytesSent=0;

	uint64_t getNumTxBytes(){
		uint64_t temp;
		temp = totalBytesSent-numTxBytesLast;
		numTxBytesLast=totalBytesSent;
		return temp;
	}

	uint64_t numRxBytes=0;
	uint64_t numRxBytesLast=0;
	uint64_t totalBytesRcvd=0;
	
	uint64_t getNumRxBytes(){
		uint64_t temp;
		temp = totalBytesRcvd-numRxBytesLast;
		numRxBytesLast=totalBytesRcvd;
		return temp;
	}

protected:

	//Ptr<Node> m_node;

  bool TransmitStart (Ptr<Packet> p);
  
  virtual void DoDispose(void);

  /// Reset the channel into READY state and try transmit again
  virtual void TransmitComplete(void);

  /// Look for an available packet and send it using TransmitStart(p)
  virtual void DequeueAndTransmit(void);

  /// Resume a paused queue and call DequeueAndTransmit()
  virtual void Resume(unsigned qIndex);

  bool ProcessHeader (Ptr<Packet> p, uint16_t& param);

    static uint16_t PppToEther (uint16_t proto);

    static uint16_t EtherToPpp (uint16_t proto);

  /**
   * The queues for each priority class.
   * @see class Queue
   * @see class InfiniteQueue
   */
  Ptr<BEgressQueue> m_queue;

  Ptr<DropTailQueue<Packet>> m_queueFifo;


  Ptr<QbbChannel> m_channel;
  
  //pfc
  bool m_qbbEnabled;	//< PFC behaviour enabled
  bool m_qcnEnabled;
  bool m_dynamicth;
  uint32_t m_pausetime;	//< Time for each Pause
  bool m_paused[qCnt];	//< Whether a queue paused
  bool dummy_paused[qCnt];

  //qcn

  /* RP parameters */
  EventId  m_nextSend;		//< The next send event
  /* State variable for rate-limited queues */

  
  // 网卡处理延迟（接收端）
  Time m_nicDelay;  // 从物理链路到协议栈的处理时延

public:
	Ptr<RdmaEgressQueue> m_rdmaEQ;
	void RdmaEnqueueHighPrioQ(Ptr<Packet> p);

	// callback for processing packet in RDMA
	typedef Callback<int, Ptr<Packet>, CustomHeader&> RdmaReceiveCb;
	RdmaReceiveCb m_rdmaReceiveCb;
	// callback for link down
	typedef Callback<void, Ptr<QbbNetDevice> > RdmaLinkDownCb;
	RdmaLinkDownCb m_rdmaLinkDownCb;
	// callback for sent a packet
	typedef Callback<void, Ptr<RdmaQueuePair>, Ptr<Packet>, Time> RdmaPktSent;
	RdmaPktSent m_rdmaPktSent;

	Ptr<RdmaEgressQueue> GetRdmaQueue();
	void TakeDown(); // take down this device
	void UpdateNextAvail(Time t);

	TracedCallback<Ptr<const Packet>, Ptr<RdmaQueuePair> > m_traceQpDequeue; // the trace for printing dequeue

	uint64_t hostDequeueIndex;
};

} // namespace ns3

#endif // QBB_NET_DEVICE_H
