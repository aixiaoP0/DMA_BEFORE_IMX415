#include "modules/transport/rtp/RtpStreamingBackend.h"

#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <time.h>

#include "common/log/Logger.h"
#include "common/net/RtpProtocol.h"
#include "common/time/MonotonicClock.h"

namespace sserver {
namespace modules {
namespace transport {
namespace rtp {

namespace {

const std::uint64_t kMaxRtpPacingWindowNs = 2ULL * 1000ULL * 1000ULL;
const std::uint64_t kRtpPacingFrameFractionDivisor = 4ULL;
const std::uint64_t kMinRtpPacingIntervalNs = 50ULL * 1000ULL;

}  // namespace

RtpStreamingBackend::RtpStreamingBackend(
        const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder)
        : backend_name_("rtp"),
          latency_log_interval_frames_(120),
          socket_fd_(-1),
          bound_port_(0),
          has_remote_address_(false),
          send_latency_recorder_(send_latency_recorder),
          state_(core::ModuleState::kCreated),
          sent_frames_(0),
          sent_packets_(0),
          failed_frames_(0),
          fallback_frame_interval_ns_(0),
          previous_frame_capture_timestamp_ns_(0) {
}

RtpStreamingBackend::~RtpStreamingBackend() {
    shutdown();
}

bool RtpStreamingBackend::initialize(const core::ApplicationContext &context) {
    config_ = context.config.transport;
    latency_log_interval_frames_ = context.config.runtime.latency_log_interval_frames;
    if (context.config.capture.frame_interval_ms > 0) {
        fallback_frame_interval_ns_ =
                static_cast<std::uint64_t>(context.config.capture.frame_interval_ms) * 1000000ULL;
    } else if (context.config.capture.fps > 0) {
        fallback_frame_interval_ns_ = 1000000000ULL / static_cast<std::uint64_t>(context.config.capture.fps);
    } else {
        fallback_frame_interval_ns_ = 0;
    }
    previous_frame_capture_timestamp_ns_ = 0;
    state_ = core::ModuleState::kInitialized;
    return true;
}

bool RtpStreamingBackend::start() {
    if (!config_.enabled) {
        state_ = core::ModuleState::kRunning;
        return true;
    }

    if (!ConfigureRemoteAddress() || !OpenSocket()) {
        state_ = core::ModuleState::kFailed;
        return false;
    }

    packetizer_.reset(new RtpPacketizer(
            static_cast<std::uint8_t>(config_.rtp_payload_type),
            static_cast<std::uint32_t>(config_.rtp_clock_rate),
            config_.rtp_max_payload_size,
            static_cast<std::uint32_t>(config_.rtp_ssrc),
            config_.rtp_enable_latency_extension));
    if (!WriteSdpFile()) {
        CloseSocket();
        packetizer_.reset();
        state_ = core::ModuleState::kFailed;
        return false;
    }

    state_ = core::ModuleState::kRunning;
    return true;
}

void RtpStreamingBackend::stop() {
    packetizer_.reset();
    CloseSocket();
    has_remote_address_ = false;
    previous_frame_capture_timestamp_ns_ = 0;
    state_ = core::ModuleState::kStopped;
}

void RtpStreamingBackend::shutdown() {
    stop();
    state_ = core::ModuleState::kShutdown;
}

core::ModuleState RtpStreamingBackend::state() const {
    return state_.load();
}

void RtpStreamingBackend::Broadcast(common::model::EncodedFramePtr frame) {
    if (!config_.enabled || !frame || socket_fd_ < 0 || !packetizer_ || !has_remote_address_) {
        return;
    }

    std::vector<std::vector<std::uint8_t> > packets;
    std::string error_message;
    if (!packetizer_->Packetize(frame, &packets, &error_message)) {
        ++failed_frames_;
        if (!error_message.empty()) {
            common::log::Logger::Warn("rtp packetization failed: " + error_message);
        }
        return;
    }

    const std::chrono::nanoseconds pacing_interval =
            ComputePacingInterval(packets.size(), frame->capture_timestamp_ns);
    const std::chrono::steady_clock::time_point frame_send_start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < packets.size(); ++index) {
        std::vector<std::uint8_t> &packet = packets[index];
        if (packet.size() > config_.udp_max_datagram_size) {
            ++failed_frames_;
            common::log::Logger::Warn("failed to send RTP packet: packet size exceeds transport.udp_max_datagram_size");
            return;
        }
        PacePacketBurst(index, frame_send_start, pacing_interval);
        const std::uint64_t send_timestamp_ns = common::time::MonotonicNowNs();
        if (config_.rtp_enable_latency_extension) {
            if (!common::net::UpdateRtpLatencyExtensionTransportSendTimestamp(
                        packet.data(),
                        packet.size(),
                        send_timestamp_ns)) {
                ++failed_frames_;
                common::log::Logger::Warn("failed to update RTP latency header extension before send");
                return;
            }
        }

        const ssize_t sent = sendto(
                socket_fd_,
                packet.data(),
                packet.size(),
                0,
                reinterpret_cast<const sockaddr *>(&remote_address_),
                sizeof(remote_address_));
        if (sent != static_cast<ssize_t>(packet.size())) {
            ++failed_frames_;
            common::log::Logger::Warn("failed to send RTP packet: " + std::string(std::strerror(errno)));
            return;
        }
        ++sent_packets_;
    }

    ++sent_frames_;
    if (send_latency_recorder_ != nullptr && frame->capture_timestamp_ns != 0) {
        send_latency_recorder_->RecordNs(common::time::MonotonicNowNs() - frame->capture_timestamp_ns);
        if (latency_log_interval_frames_ > 0 &&
            sent_frames_ % static_cast<std::uint64_t>(latency_log_interval_frames_) == 0) {
            std::ostringstream stream;
            stream << "rtp transport stats sent_frames=" << sent_frames_
                   << " sent_packets=" << sent_packets_
                   << " failed_frames=" << failed_frames_;
            common::log::Logger::Info(stream.str());
            common::log::Logger::Info("transport latency rtp-broadcast " +
                                      send_latency_recorder_->Format("capture_to_send"));
        }
    }
}

int RtpStreamingBackend::bound_port() const {
    return bound_port_;
}

TransportBackend RtpStreamingBackend::backend() const {
    return TransportBackend::kRtp;
}

const std::string &RtpStreamingBackend::backend_name() const {
    return backend_name_;
}

bool RtpStreamingBackend::OpenSocket() {
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        common::log::Logger::Error("failed to create RTP UDP socket");
        return false;
    }

    setsockopt(socket_fd_, SOL_SOCKET, SO_SNDBUF, &config_.udp_send_buffer_bytes, sizeof(config_.udp_send_buffer_bytes));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config_.listen_port));
    if (inet_pton(AF_INET, config_.bind_address.c_str(), &address.sin_addr) != 1) {
        common::log::Logger::Error("invalid RTP bind address: " + config_.bind_address);
        CloseSocket();
        return false;
    }

    if (bind(socket_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        common::log::Logger::Error("failed to bind RTP UDP socket");
        CloseSocket();
        return false;
    }

    sockaddr_in bound_address{};
    socklen_t bound_length = sizeof(bound_address);
    if (getsockname(socket_fd_, reinterpret_cast<sockaddr *>(&bound_address), &bound_length) == 0) {
        bound_port_ = ntohs(bound_address.sin_port);
    } else {
        bound_port_ = config_.listen_port;
    }

    return true;
}

void RtpStreamingBackend::CloseSocket() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    bound_port_ = 0;
}

bool RtpStreamingBackend::ConfigureRemoteAddress() {
    remote_address_ = sockaddr_in();
    remote_address_.sin_family = AF_INET;
    remote_address_.sin_port = htons(static_cast<std::uint16_t>(config_.rtp_remote_port));
    if (inet_pton(AF_INET, config_.rtp_remote_host.c_str(), &remote_address_.sin_addr) != 1) {
        common::log::Logger::Error("invalid RTP remote host: " + config_.rtp_remote_host);
        has_remote_address_ = false;
        return false;
    }
    has_remote_address_ = true;
    return true;
}

bool RtpStreamingBackend::WriteSdpFile() const {
    if (config_.rtp_sdp_path.empty()) {
        return true;
    }

    std::ofstream output(config_.rtp_sdp_path.c_str(), std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        common::log::Logger::Error("failed to write RTP SDP file: " + config_.rtp_sdp_path);
        return false;
    }

    output << "v=0\n";
    output << "o=- 0 0 IN IP4 " << config_.rtp_remote_host << "\n";
    output << "s=sserver RTP stream\n";
    output << "c=IN IP4 " << config_.rtp_remote_host << "\n";
    output << "t=0 0\n";
    output << "m=video " << config_.rtp_remote_port << " RTP/AVP " << config_.rtp_payload_type << "\n";
    output << "a=rtpmap:" << config_.rtp_payload_type << " H264/" << config_.rtp_clock_rate << "\n";
    output << "a=fmtp:" << config_.rtp_payload_type << " packetization-mode=1\n";
    output << "a=recvonly\n";
    return true;
}

// 根据帧间隔和 packet 数量计算 Pacing 间隔，避免突发流量导致网络拥塞
// 将一帧的多个 RTP packet 分散在一个 pacing 窗口内均匀发送
std::chrono::nanoseconds RtpStreamingBackend::ComputePacingInterval(
        std::size_t packet_count,
        std::uint64_t current_capture_timestamp_ns) {
    if (packet_count < 4) {
        if (current_capture_timestamp_ns != 0) {
            previous_frame_capture_timestamp_ns_ = current_capture_timestamp_ns;
        }
        return std::chrono::nanoseconds::zero();
    }

    std::uint64_t frame_interval_ns = fallback_frame_interval_ns_;
    if (current_capture_timestamp_ns != 0) {
        if (previous_frame_capture_timestamp_ns_ != 0 &&
            current_capture_timestamp_ns > previous_frame_capture_timestamp_ns_) {
            frame_interval_ns = current_capture_timestamp_ns - previous_frame_capture_timestamp_ns_;
        }
        previous_frame_capture_timestamp_ns_ = current_capture_timestamp_ns;
    }

    if (frame_interval_ns == 0) {
        return std::chrono::nanoseconds::zero();
    }

    const std::uint64_t pacing_window_ns =
            std::min(kMaxRtpPacingWindowNs, frame_interval_ns / kRtpPacingFrameFractionDivisor);
    if (pacing_window_ns < kMinRtpPacingIntervalNs) {
        return std::chrono::nanoseconds::zero();
    }

    const std::uint64_t interval_ns = pacing_window_ns / static_cast<std::uint64_t>(packet_count - 1);
    if (interval_ns < kMinRtpPacingIntervalNs) {
        return std::chrono::nanoseconds::zero();
    }

    return std::chrono::nanoseconds(interval_ns);
}

// 按 pacing 间隔对单个 packet 实施精确等待，确保发送节奏均匀
void RtpStreamingBackend::PacePacketBurst(
        std::size_t packet_index,
        std::chrono::steady_clock::time_point frame_send_start,
        std::chrono::nanoseconds pacing_interval) const {
    if (packet_index == 0 || pacing_interval <= std::chrono::nanoseconds::zero()) {
        return;
    }

    const std::chrono::steady_clock::time_point target_send_time =
            frame_send_start + (pacing_interval * static_cast<std::chrono::nanoseconds::rep>(packet_index));
    const auto now = std::chrono::steady_clock::now();
    if (target_send_time <= now) {
        return;
    }

    const auto remaining = target_send_time - now;
    struct timespec ts;
    ts.tv_sec = std::chrono::duration_cast<std::chrono::seconds>(remaining).count();
    ts.tv_nsec = (remaining - std::chrono::duration_cast<std::chrono::seconds>(remaining)).count();
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
}

}  // namespace rtp
}  // namespace transport
}  // namespace modules
}  // namespace sserver
