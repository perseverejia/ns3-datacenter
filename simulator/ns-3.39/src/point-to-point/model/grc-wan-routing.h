#pragma once

#include "ns3/callback.h"
#include "ns3/grc-settings.h"
#include "ns3/node.h"
#include "ns3/object.h"
#include "ns3/packet.h"
#include "ns3/ptr.h"
#include "ns3/simulator.h"

#include <deque>
#include <map>
#include <random>
#include <vector>

namespace ns3
{
// GRC DCI controller, adapted from upstream commit 8c41acee54a4f4aa390fc533b2a3affed7c7c361.
// The upstream license is included in grc-wan-routing.cc.
class GrcWanRouting : public Object
{
    friend struct GrcSettings;

  public:
    GrcWanRouting();
    void Init(Ptr<Node> node, uint32_t dc, const std::map<uint32_t, uint32_t>& routes);
    void RouteInput(Ptr<Packet> p, CustomHeader& ch);

    typedef Callback<void, Ptr<Packet>, CustomHeader&> SwitchSendToDevCallback;
    void SetSwitchSendToDevCallback(SwitchSendToDevCallback switchSendToDevCallback);

  private:
    SwitchSendToDevCallback m_switchSendToDevCallback; // SwitchNode::SendToDev
    uint32_t m_switch_id = 0;
    uint32_t m_dc = 0;
    std::mt19937 m_rng; // Per-DCI randomness, independent of the workload's rand().
    uint32_t m_hash_seed1 = 0, m_hash_seed2 = 0;
    void HandleUdpReceived(Ptr<Packet> p, CustomHeader& ch);
    void HandleAckReceived(Ptr<Packet> p, CustomHeader& ch);

    /************数据平面延迟检测*********/
    static const inline int rtt_table_size = 64 * 16;

    struct RttEntry
    {
        bool valid = false;
        uint32_t hashed_seq = 0;
        Time timestamp = Seconds(0);
    } rtt_table[rtt_table_size];
    Time m_rtt_entry_timeout;
    Time m_rtt_rto_filter;

    struct DstDCHandler
    {
        DstDCHandler() = default;

        void Init(GrcWanRouting* wan_routing, int64_t max_rate);

        // RTT EWMA for the paper's RTO-based stale-ACK rejection heuristic.
        Time rtt_tau;
        Time sensitive_rtt = MicroSeconds(0);
        Time last_update_time = MicroSeconds(0);

        // Per-epoch RTT samples and a time-bounded minimum of epoch averages.
        Time rtt_sum = Seconds(0);
        int rtt_num = 0;
        void record_rtt(Time rtt, Time rto);
        Time base_rtt_window = Seconds(10);
        std::deque<std::pair<Time, Time>> base_rtt_minima;
        Time update_base_rtt(Time now, Time epoch_rtt);

        // 速率计算
        // controlplane para
        int64_t guaranteed_rate = 0;
        void update_ref_rate();

        // 速率控制模块
        int64_t ref_rate = 0; // 每秒发送的基准字节数，从10GB/s开始
        uint64_t total_send_bytes = 0;

        // CNP trigger statistics per epoch
        uint64_t epoch_pkt_cnt = 0;
        uint64_t epoch_cnp_cnt = 0;
        double latest_w_x = 0.0;

        enum RateChangeState
        {
            STABLE,
            INCREASE,
            DECREASE
        };

        RateChangeState rate_change_state = STABLE;
        uint32_t consecutive_state_epochs = 0;

        int64_t start_bytes = 0;
        int64_t end_bytes = 0;
        int64_t cur_bytes = 0;
        int64_t get_std_bytes() const;

        bool update_and_check_cnp(uint32_t pkt_size);

      private:
        GrcWanRouting* m_wanRouting = nullptr;
    };

    std::map<uint32_t, DstDCHandler> m_dcHandler;
    uint32_t Hash4Tuple(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport);
    uint32_t Hash4TupleSeq(uint32_t sip,
                           uint32_t dip,
                           uint16_t sport,
                           uint16_t dport,
                           uint32_t seq);
    uint32_t GetRttTableIndex(uint32_t sip,
                              uint32_t dip,
                              uint16_t sport,
                              uint16_t dport,
                              uint32_t seq);
    static uint32_t ECMPHash(const uint8_t* key, size_t len, uint32_t seed);
    // Configurable epoch origin (default zero) supports local sub-second workloads.
    Time m_epoch_start_time = Seconds(0);

    static Time epoch_duration;
    void controlplane_logic();
    static bool s_w_k_update_scheduled;
    static bool s_w_k_initialized;
    static double s_w_k;
    static std::vector<double> s_w_k_samples;
    static void update_w_k();

    // Congestion control module
    void send_cnp(CustomHeader& ch, bool rateTriggered, bool ecnTriggered);
};

} // namespace ns3
