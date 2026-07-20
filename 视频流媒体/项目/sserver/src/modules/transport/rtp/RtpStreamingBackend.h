#ifndef SSERVER_MODULES_TRANSPORT_RTP_RTPSTREAMINGBACKEND_H
#define SSERVER_MODULES_TRANSPORT_RTP_RTPSTREAMINGBACKEND_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <netinet/in.h>

#include "common/metrics/LatencyRecorder.h"
#include "config/AppConfig.h"
#include "modules/transport/ITransportBackend.h"
#include "modules/transport/rtp/RtpPacketizer.h"

namespace sserver {
namespace modules {
namespace transport {
namespace rtp {

class RtpStreamingBackend : public ITransportBackend {
public:
    explicit RtpStreamingBackend(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder);
    ~RtpStreamingBackend() override;

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
    bool OpenSocket();
    void CloseSocket();
    bool ConfigureRemoteAddress();
    bool WriteSdpFile() const;
    std::chrono::nanoseconds ComputePacingInterval(
            std::size_t packet_count,
            std::uint64_t current_capture_timestamp_ns);
    void PacePacketBurst(
            std::size_t packet_index,
            std::chrono::steady_clock::time_point frame_send_start,
            std::chrono::nanoseconds pacing_interval) const;

private:
    config::TransportConfig config_;
    std::string backend_name_;
    int latency_log_interval_frames_;
    int socket_fd_;
    int bound_port_;
    sockaddr_in remote_address_;
    bool has_remote_address_;
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder_;
    std::unique_ptr<RtpPacketizer> packetizer_;
    std::atomic<core::ModuleState> state_;
    std::uint64_t sent_frames_;
    std::uint64_t sent_packets_;
    std::uint64_t failed_frames_;
    std::uint64_t fallback_frame_interval_ns_;
    std::uint64_t previous_frame_capture_timestamp_ns_;
};

}  // namespace rtp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_RTP_RTPSTREAMINGBACKEND_H
