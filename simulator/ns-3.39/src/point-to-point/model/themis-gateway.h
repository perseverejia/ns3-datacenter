#ifndef NS3_THEMIS_GATEWAY_H
#define NS3_THEMIS_GATEWAY_H

#include "ns3/callback.h"
#include "ns3/custom-header.h"
#include "ns3/event-id.h"
#include "ns3/nstime.h"
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/tag.h"

#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace ns3 {

class SwitchNode;
class RdmaDriver;
class RdmaQueuePair;

// Simulation equivalents of the QP identifiers used by THEMIS. Unlike an IP/PG
// key, this also separates concurrent QPs between the same two hosts.
struct ThemisFlowKey {
    uint32_t sip, dip;
    uint16_t sport, dport, pg;
    bool operator<(const ThemisFlowKey& o) const {
        return std::tie(sip, dip, sport, dport, pg) <
               std::tie(o.sip, o.dip, o.sport, o.dport, o.pg);
    }
    static ThemisFlowKey Data(const CustomHeader& ch);
    static ThemisFlowKey Feedback(const CustomHeader& ch);
};

struct ThemisSettings {
    bool traceEvents = true;
    uint32_t alpha = 5;
    Time beta = MicroSeconds(500);
    uint32_t dummyPacketBytes = 1024;
};

// Internal recirculation metadata, not a change to any existing wire header.
class ThemisLoopTag : public Tag {
  public:
    uint32_t remaining = 0, ingress = 0;
    bool dummy = false;
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(TagBuffer b) const override;
    void Deserialize(TagBuffer b) override;
    void Print(std::ostream& os) const override;
};

// Adapted from the supplied themisGateway's packet interception/self-loop
// design. Instantiated only for CcMode=19, after normal topology/routing setup.
class ThemisGateway : public Object {
  public:
    static TypeId GetTypeId();
    ThemisGateway();
    void Configure(SwitchNode* node, uint32_t dc,
                   const std::map<uint32_t, uint32_t>& hostIpToDc,
                   const ThemisSettings& config,
                   Callback<void, Ptr<Packet>, CustomHeader&> send);
    void ConnectCompletionTrace(Ptr<RdmaDriver> driver);
    bool PacketIn(uint32_t ingress, Ptr<Packet> p, CustomHeader& ch);
    void NotifyDequeue(uint32_t outPort, Ptr<Packet> p);
    bool IsLoopPort(uint32_t port) const;

  private:
    void DoDispose() override;
    void Report() const;
    enum Status { NORMAL, THROTTLED, RECOVER };
    struct State {
        Status status = NORMAL;
        uint64_t cnpNum = 0;
        uint32_t alpha = 5, loopNum = 0;
        EventId recoveryEvent;
        // Tracks only packets actually buffered here. An upstream loss must
        // still reach the receiver's existing GBN/NACK recovery mechanism.
        std::multiset<uint32_t> pending;
        bool finished = false;
    };
    SwitchNode* m_node = nullptr;
    uint32_t m_dc = 0, m_loopTx = 0, m_loopRx = 0;
    ThemisSettings m_config;
    std::map<uint32_t, uint32_t> m_hostDc;
    Callback<void, Ptr<Packet>, CustomHeader&> m_send;
    std::vector<Ptr<RdmaDriver>> m_completionSources;
    std::map<ThemisFlowKey, State> m_states;
    uint64_t m_loopCapacity = 0, m_dummyTargetBytes = 0;
    uint64_t m_dataBytes = 0, m_dummyBytes = 0;
    uint64_t m_peakBytes = 0, m_pnpCount = 0, m_trpCount = 0;
    uint64_t m_enterCount = 0, m_exitCount = 0, m_passCount = 0;
    uint64_t m_dropCount = 0, m_recoveryCount = 0;

    bool IsSource(const ThemisFlowKey& key) const;
    uint64_t GetLoopBytes() const { return m_dataBytes + m_dummyBytes; }
    bool IsDestination(const ThemisFlowKey& key) const;
    bool IsWan(uint32_t port) const;
    void SetupLoop();
    void StartDummy();
    void SendLoop(Ptr<Packet> p, CustomHeader& ch);
    bool HandleLoop(Ptr<Packet> p, CustomHeader& ch, ThemisLoopTag tag);
    void ReceiveCnp(const ThemisFlowKey& key);
    void Recover(ThemisFlowKey key);
    void SendCnp(const CustomHeader& ch);
    void OnQpComplete(Ptr<RdmaQueuePair> qp);
    void Trace(const char* event, const ThemisFlowKey& key, uint32_t seq = 0) const;
};

} // namespace ns3
#endif
