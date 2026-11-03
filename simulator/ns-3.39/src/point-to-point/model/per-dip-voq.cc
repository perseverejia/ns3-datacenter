/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */

#include "per-dip-voq.h"

#include "ns3/data-rate.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("PerDipVoq");
NS_OBJECT_ENSURE_REGISTERED(PerDipVoq);

TypeId
PerDipVoq::GetTypeId(void)
{
  static TypeId tid =
      TypeId("ns3::PerDipVoq")
          .SetParent<Object>()
          .SetGroupName("PointToPoint")
          .AddConstructor<PerDipVoq>()
          .AddAttribute("DefaultRate",
                        "Rate used by a newly created per-DIP VOQ.",
                        DataRateValue(DataRate("100Gbps")),
                        MakeDataRateAccessor(&PerDipVoq::m_defaultRate),
                        MakeDataRateChecker())
          .AddAttribute("MaxQueueBytes",
                        "Maximum bytes in one per-DIP VOQ; zero means unlimited.",
                        UintegerValue(0),
                        MakeUintegerAccessor(&PerDipVoq::m_maxQueueBytes),
                        MakeUintegerChecker<uint64_t>());
  return tid;
}

PerDipVoq::QueueState::QueueState()
    : outPort(0),
      dip(0),
      key(0),
      bytes(0),
      rate(DataRate(0)),
      nextSendTime(Seconds(0)),
      sendScheduled(false)
{
}

PerDipVoq::PerDipVoq()
    : m_defaultRate(DataRate("100Gbps")),
      m_maxQueueBytes(0)
{
}

PerDipVoq::~PerDipVoq()
{
}

void
PerDipVoq::DoDispose(void)
{
  for (std::map<uint64_t, QueueState>::iterator it = m_queues.begin();
       it != m_queues.end();
       ++it)
    {
      if (it->second.sendEvent.IsRunning())
        {
          Simulator::Cancel(it->second.sendEvent);
        }
    }

  m_queues.clear();
  m_sendCallback = SendCallback();
  Object::DoDispose();
}

void
PerDipVoq::SetSendCallback(SendCallback callback)
{
  m_sendCallback = callback;

  // 如果回调设置前已有积压，设置回调后立即启动这些队列。
  if (!m_sendCallback.IsNull())
    {
      for (std::map<uint64_t, QueueState>::iterator it = m_queues.begin();
           it != m_queues.end();
           ++it)
        {
          ScheduleNext(it->second);
        }
    }
}

uint64_t
PerDipVoq::MakeKey(uint32_t outPort, uint32_t dip)
{
  return (static_cast<uint64_t>(outPort) << 32) | dip;
}

PerDipVoq::QueueState&
PerDipVoq::GetOrCreateQueue(uint32_t outPort, uint32_t dip)
{
  const uint64_t key = MakeKey(outPort, dip);
  std::map<uint64_t, QueueState>::iterator it = m_queues.find(key);
  if (it != m_queues.end())
    {
      return it->second;
    }

  QueueState state;
  state.outPort = outPort;
  state.dip = dip;
  state.key = key;
  state.rate = m_defaultRate;
  state.nextSendTime = Simulator::Now();

  return m_queues.insert(std::make_pair(key, state)).first->second;
}

const PerDipVoq::QueueState*
PerDipVoq::FindQueue(uint32_t outPort, uint32_t dip) const
{
  std::map<uint64_t, QueueState>::const_iterator it =
      m_queues.find(MakeKey(outPort, dip));
  return (it == m_queues.end()) ? 0 : &it->second;
}

bool
PerDipVoq::Enqueue(Ptr<Packet> packet,
                   CustomHeader& ch,
                   uint32_t outPort,
                   uint32_t qIndex)
{
  if (!packet)
    {
      return false;
    }

  QueueState& queue = GetOrCreateQueue(outPort, ch.dip);
  const uint32_t size = packet->GetSize();

  if (m_maxQueueBytes != 0 && queue.bytes + size > m_maxQueueBytes)
    {
      NS_LOG_DEBUG("Drop packet: per-DIP VOQ is full, port="
                   << outPort << ", dip=" << ch.dip);
      return false;
    }

  Item item;
  item.packet = packet;
  item.header = ch;
  item.qIndex = qIndex;
  item.size = size;

  const bool wasEmpty = queue.items.empty();
  queue.items.push_back(item);
  queue.bytes += size;

  if (wasEmpty)
    {
      queue.nextSendTime = Simulator::Now();
    }

  ScheduleNext(queue);
  return true;
}

void
PerDipVoq::SetDipRate(uint32_t outPort, uint32_t dip, DataRate rate)
{
  QueueState& queue = GetOrCreateQueue(outPort, dip);
  const uint64_t oldBps = queue.rate.GetBitRate();
  const uint64_t newBps = rate.GetBitRate();

  if (queue.sendEvent.IsRunning())
    {
      Simulator::Cancel(queue.sendEvent);
    }
  queue.sendScheduled = false;

  // 按新旧速率缩放剩余等待时间，使运行中的调速及时生效。
  if (oldBps != 0 && newBps != 0 &&
      queue.nextSendTime > Simulator::Now())
    {
      const double scale = static_cast<double>(oldBps) /
                           static_cast<double>(newBps);
      const Time remaining = queue.nextSendTime - Simulator::Now();
      queue.nextSendTime =
          Simulator::Now() + Seconds(remaining.GetSeconds() * scale);
    }
  else if (oldBps == 0 && newBps != 0)
    {
      queue.nextSendTime = Simulator::Now();
    }

  queue.rate = rate;
  ScheduleNext(queue);
}

DataRate
PerDipVoq::GetDipRate(uint32_t outPort, uint32_t dip) const
{
  const QueueState* queue = FindQueue(outPort, dip);
  return queue == 0 ? m_defaultRate : queue->rate;
}

uint64_t
PerDipVoq::GetQueueBytes(uint32_t outPort, uint32_t dip) const
{
  const QueueState* queue = FindQueue(outPort, dip);
  return queue == 0 ? 0 : queue->bytes;
}

uint64_t
PerDipVoq::GetQueuePackets(uint32_t outPort, uint32_t dip) const
{
  const QueueState* queue = FindQueue(outPort, dip);
  return queue == 0 ? 0 : queue->items.size();
}

void
PerDipVoq::SetDefaultRate(DataRate rate)
{
  m_defaultRate = rate;
}

DataRate
PerDipVoq::GetDefaultRate(void) const
{
  return m_defaultRate;
}

void
PerDipVoq::SetMaxQueueBytes(uint64_t bytes)
{
  m_maxQueueBytes = bytes;
}

uint64_t
PerDipVoq::GetMaxQueueBytes(void) const
{
  return m_maxQueueBytes;
}

Time
PerDipVoq::GetPacingTime(uint32_t bytes, DataRate rate) const
{
  if (rate.GetBitRate() == 0)
    {
      return Seconds(0);
    }

  const double seconds = static_cast<double>(bytes) * 8.0 /
                         static_cast<double>(rate.GetBitRate());
  return Seconds(seconds);
}

void
PerDipVoq::ScheduleNext(QueueState& queue)
{
  if (queue.sendScheduled || queue.items.empty() ||
      queue.rate.GetBitRate() == 0 || m_sendCallback.IsNull())
    {
      return;
    }

  const Time now = Simulator::Now();
  const Time delay = queue.nextSendTime > now
                         ? queue.nextSendTime - now
                         : Seconds(0);

  queue.sendScheduled = true;
  if (delay == Seconds(0))
    {
      queue.sendEvent =
          Simulator::ScheduleNow(&PerDipVoq::DequeueAndSend, this, queue.key);
    }
  else
    {
      queue.sendEvent = Simulator::Schedule(
          delay, &PerDipVoq::DequeueAndSend, this, queue.key);
    }
}

void
PerDipVoq::DequeueAndSend(uint64_t key)
{
  std::map<uint64_t, QueueState>::iterator it = m_queues.find(key);
  if (it == m_queues.end())
    {
      return;
    }

  QueueState& queue = it->second;
  queue.sendScheduled = false;

  if (queue.items.empty() || queue.rate.GetBitRate() == 0 ||
      m_sendCallback.IsNull())
    {
      return;
    }

  Item item = queue.items.front();
  queue.items.pop_front();
  queue.bytes -= item.size;

  queue.nextSendTime =
      Simulator::Now() + GetPacingTime(item.size, queue.rate);

  // 这里直接回到原交换机/网卡发送路径；VOQ 不重复实现原来的功能。
  m_sendCallback(item.packet,
                 item.header,
                 queue.outPort,
                 item.qIndex);

  ScheduleNext(queue);
}

} // namespace ns3
