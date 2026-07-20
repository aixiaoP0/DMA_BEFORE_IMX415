#ifndef SSERVER_MODULES_TRANSPORT_UDP_UDPSTREAMINGBACKEND_H
#define SSERVER_MODULES_TRANSPORT_UDP_UDPSTREAMINGBACKEND_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>

#include "common/metrics/LatencyRecorder.h"
#include "common/net/StreamProtocol.h"
#include "config/AppConfig.h"
#include "modules/transport/ITransportBackend.h"

namespace sserver {
namespace modules {
namespace transport {
namespace udp {

class UdpStreamingBackend : public ITransportBackend {
public:
    explicit UdpStreamingBackend(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder);
    ~UdpStreamingBackend() override;

    bool initialize(const core::ApplicationContext &context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;
    core::ModuleState state() const override;
    void Broadcast(common::model::EncodedFramePtr frame) override;
    int bound_port() const override;
    TransportBackend backend() const override;
    const std::string &backend_name() const override;

private:
    struct UdpClientEndpoint {
        sockaddr_in address;
        std::uint64_t last_seen_ns;
        common::net::UdpReceiverReport latest_report;
        std::uint64_t latest_report_timestamp_ns;
        bool has_report;
    };

    struct CachedUdpFragment {
        std::uint16_t fragment_index = 0;
        std::size_t datagram_size = 0;
        std::vector<char> datagram;
    };

    struct CachedUdpFrame {
        std::uint64_t frame_sequence = 0;
        std::uint64_t cached_timestamp_ns = 0;
        std::vector<CachedUdpFragment> fragments;
    };

    bool OpenSocket();
    void CloseSocket();
    void ReceiveLoop();
    void RegisterClient(
            const sockaddr_in &address,
            std::uint64_t now_ns,
            const common::net::UdpReceiverReport *report);
    void PruneStaleClientsLocked(std::uint64_t now_ns);
    std::vector<sockaddr_in> SnapshotClients(std::uint64_t now_ns);
    bool SendFrameFragments(common::model::EncodedFramePtr frame, const std::vector<sockaddr_in> &clients);
    void CacheFrameFragments(const CachedUdpFrame &frame);
    void PruneRetransmitCacheLocked(std::uint64_t now_ns);
    void HandleNackRequest(
            const sockaddr_in &address,
            std::uint64_t now_ns,
            const common::net::UdpNackHeader &nack_header,
            const std::vector<common::net::UdpNackItem> &nack_items);
    std::string FormatEndpoint(const sockaddr_in &address) const;
    std::string FormatClientReport(const UdpClientEndpoint &client) const;
    bool SameEndpoint(const sockaddr_in &lhs, const sockaddr_in &rhs) const;

private:
    config::TransportConfig config_;
    std::string backend_name_;
    int latency_log_interval_frames_;
    int socket_fd_;
    int bound_port_;
    std::atomic_bool running_;
    std::thread receive_thread_;
    std::mutex clients_mutex_;
    std::vector<UdpClientEndpoint> clients_;
    std::mutex retransmit_cache_mutex_;
    std::deque<CachedUdpFrame> retransmit_cache_;
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder_;
    std::atomic<core::ModuleState> state_;
    std::uint64_t sent_frames_;
    std::uint64_t dropped_fragmented_frames_;
    std::uint64_t sent_fragments_;
    std::uint64_t fec_fragments_sent_;
    std::uint64_t failed_fragments_;
    std::uint64_t nack_requests_received_;
    std::uint64_t nack_fragments_requested_;
    std::uint64_t retransmitted_fragments_sent_;
    std::uint64_t retransmit_fragment_misses_;
    std::uint64_t retransmit_fragments_throttled_;
    std::vector<char> send_datagram_;
};

}  // namespace udp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_UDP_UDPSTREAMINGBACKEND_H
