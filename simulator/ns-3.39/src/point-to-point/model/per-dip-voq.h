/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef PER_DIP_VOQ_H
#define PER_DIP_VOQ_H

#include <deque>
#include <map>
#include <stdint.h>

#include "ns3/callback.h"
#include "ns3/custom-header.h"
#include "ns3/data-rate.h"
#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/ptr.h"

namespace ns3 {

/**
 * \brief 一个精简的按 DIP 划分的出端口 VOQ Model。
 *
 * 队列主键为 (outPort, dip)。outPort 只是出端口命名空间；真正区分流量
 * 的主键是 dip。因此，去往同一 DIP 的不同流、不同 qIndex 都进入同一
 * 条 VOQ。qIndex 只作为报文元数据保留，出队后仍交给原交换机发送路径，
 * 原有优先级队列、PFC、ECN 等逻辑不需要在这里重复实现。
 *
 * 这个 Model 只负责三件事：
 *   1. 按 (outPort, dip) 缓存报文；
 *   2. 统计每条 VOQ 的实时字节数和包数；
 *   3. 按每个 DIP 配置的速率把报文交回原发送路径。
 */
class PerDipVoq : public Object
{
public:
  /** 参数依次为 packet、header、outPort、qIndex。 */
  typedef Callback<void, Ptr<Packet>, CustomHeader&, uint32_t, uint32_t>
      SendCallback;

  static TypeId GetTypeId(void);

  PerDipVoq();
  virtual ~PerDipVoq();

  /** 设置出队后的发送函数，通常回调原来的 SwitchSend。 */
  void SetSendCallback(SendCallback callback);

  /**
   * 把报文放入对应的 (outPort, ch.dip) 队列。
   * \return true 表示入队成功；false 表示空指针或超过队列上限。
   */
  bool Enqueue(Ptr<Packet> packet,
               CustomHeader& ch,
               uint32_t outPort,
               uint32_t qIndex);

  /** 设置/读取某个出端口上某个 DIP 的聚合发送速率。0 bps 表示暂停。 */
  void SetDipRate(uint32_t outPort, uint32_t dip, DataRate rate);
  DataRate GetDipRate(uint32_t outPort, uint32_t dip) const;

  /** 实时队列长度；不存在的队列返回 0。 */
  uint64_t GetQueueBytes(uint32_t outPort, uint32_t dip) const;
  uint64_t GetQueuePackets(uint32_t outPort, uint32_t dip) const;

  /** 新建 DIP 队列使用的初始速率。 */
  void SetDefaultRate(DataRate rate);
  DataRate GetDefaultRate(void) const;

  /** 单条 DIP 队列的字节上限；0 表示不限。 */
  void SetMaxQueueBytes(uint64_t bytes);
  uint64_t GetMaxQueueBytes(void) const;

protected:
  virtual void DoDispose(void);

private:
  struct Item
  {
    Ptr<Packet> packet;
    CustomHeader header;
    uint32_t qIndex;
    uint32_t size;
  };

  struct QueueState
  {
    uint32_t outPort;
    uint32_t dip;
    uint64_t key;
    std::deque<Item> items;
    uint64_t bytes;
    DataRate rate;
    Time nextSendTime;
    EventId sendEvent;
    bool sendScheduled;

    QueueState();
  };

  static uint64_t MakeKey(uint32_t outPort, uint32_t dip);
  QueueState& GetOrCreateQueue(uint32_t outPort, uint32_t dip);
  const QueueState* FindQueue(uint32_t outPort, uint32_t dip) const;

  void ScheduleNext(QueueState& queue);
  void DequeueAndSend(uint64_t key);
  Time GetPacingTime(uint32_t bytes, DataRate rate) const;

  std::map<uint64_t, QueueState> m_queues;
  DataRate m_defaultRate;
  uint64_t m_maxQueueBytes;
  SendCallback m_sendCallback;
};

} // namespace ns3

#endif // PER_DIP_VOQ_H
