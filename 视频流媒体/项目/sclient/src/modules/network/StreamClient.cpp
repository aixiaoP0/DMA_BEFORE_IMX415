#include "modules/network/StreamClientInternal.h"

#include <chrono>

namespace sclient {

using network_internal::BuildSocketError;
using network_internal::MonotonicNowNs;

StreamClient::StreamClient()
        : socket_fd_(-1),
          running_(false),
          next_rtp_frame_sequence_(0),
          last_completed_frame_sequence_(0),
          has_last_completed_frame_sequence_(false),
          next_jitter_buffer_sequence_(0),
          has_next_jitter_buffer_sequence_(false),
          previous_network_latency_ms_(0.0),
          has_previous_network_latency_(false) {
}

StreamClient::~StreamClient() {
    Close();
}

bool StreamClient::Connect(const ClientConfig &config, std::string *error_message) {
    Close();
    config_ = config;
    ResetUdpState();
    ResetRtpState();

    AdaptiveJitterBufferConfig jitter_config;
    jitter_config.min_delay_ms = config_.udp_jitter_buffer_min_delay_ms;
    jitter_config.safety_factor = config_.udp_jitter_buffer_safety_factor;
    jitter_config.base_max_wait_ms = config_.udp_jitter_buffer_max_wait_ms;
    jitter_config.base_max_frames = config_.udp_jitter_buffer_max_frames;
    adaptive_jitter_.Configure(jitter_config);
    if (config_.udp_jitter_buffer_strategy == "off") {
        adaptive_jitter_.SetFixedMode(JitterBufferMode::kBypass);
    } else if (config_.udp_jitter_buffer_strategy == "low") {
        adaptive_jitter_.SetFixedMode(JitterBufferMode::kLowLatency);
    } else if (config_.udp_jitter_buffer_strategy == "smooth") {
        adaptive_jitter_.SetFixedMode(JitterBufferMode::kSmooth);
    } else if (config_.udp_jitter_buffer_strategy == "auto") {
        adaptive_jitter_.EnableAutoMode();
    }
    udp_datagram_buffer_.assign(config_.udp_max_datagram_size, 0);
    rtp_datagram_buffer_.assign(config_.udp_max_datagram_size, 0);

    const int socket_type = config_.transport == "tcp" ? SOCK_STREAM : SOCK_DGRAM;
    socket_fd_ = socket(AF_INET, socket_type, 0);
    if (socket_fd_ < 0) {
        if (error_message != nullptr) {
            *error_message = BuildSocketError("failed to create socket");
        }
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config_.port));
    if (inet_pton(AF_INET, config_.host.c_str(), &address.sin_addr) != 1) {
        if (error_message != nullptr) {
            *error_message = "invalid host: " + config_.host;
        }
        Close();
        return false;
    }

    if (config_.transport == "rtp") {
        if (bind(socket_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
            if (error_message != nullptr) {
                *error_message = BuildSocketError("failed to bind RTP socket");
            }
            Close();
            return false;
        }

        // 注册模式下，将同一个RTP接收socket连接到板端注册端口。
        // 后续keepalive和RTP接收共用此socket，确保NAT映射的来源端口一致。
        if (!config_.rtp_server_host.empty()) {
            sockaddr_in server_address{};
            server_address.sin_family = AF_INET;
            server_address.sin_port =
                    htons(static_cast<std::uint16_t>(config_.rtp_server_port));
            if (inet_pton(
                    AF_INET,
                    config_.rtp_server_host.c_str(),
                    &server_address.sin_addr) != 1) {
                if (error_message != nullptr) {
                    *error_message = "invalid RTP server host: " + config_.rtp_server_host;
                }
                Close();
                return false;
            }
            if (connect(
                    socket_fd_,
                    reinterpret_cast<sockaddr *>(&server_address),
                    sizeof(server_address)) != 0) {
                if (error_message != nullptr) {
                    *error_message = BuildSocketError("failed to connect RTP registration server");
                }
                Close();
                return false;
            }
        }
    } else if (connect(socket_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        if (error_message != nullptr) {
            *error_message = BuildSocketError("failed to connect");
        }
        Close();
        return false;
    }

    if (config_.transport != "tcp") {
        setsockopt(
                socket_fd_,
                SOL_SOCKET,
                SO_RCVBUF,
                &config_.udp_receive_buffer_bytes,
                sizeof(config_.udp_receive_buffer_bytes));
    }

    {
        timeval timeout{};
        timeout.tv_sec = (config_.transport == "tcp") ? 5 : 1;
        timeout.tv_usec = 0;
        setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }

    running_ = true;
    if (config_.transport != "rtp" || !config_.rtp_server_host.empty()) {
        if (!SendKeepAlive()) {
            if (error_message != nullptr) {
                *error_message = BuildSocketError("failed to send initial keepalive");
            }
            Close();
            return false;
        }
        keepalive_thread_ = std::thread(&StreamClient::KeepAliveLoop, this);
    }

    return true;
}

bool StreamClient::ReceiveFrame(ReceivedFrame *frame, std::string *error_message) {
    if (frame == nullptr || socket_fd_ < 0) {
        if (error_message != nullptr) {
            *error_message = "client is not connected";
        }
        return false;
    }

    if (config_.transport == "udp") {
        return ReceiveUdpFrame(frame, error_message);
    }
    if (config_.transport == "rtp") {
        return ReceiveRtpFrame(frame, error_message);
    }
    return ReceiveTcpFrame(frame, error_message);
}

UdpReceiveStats StreamClient::udp_receive_stats() const {
    std::lock_guard<std::mutex> lock(udp_receive_stats_snapshot_mutex_);
    return udp_receive_stats_snapshot_;
}

int StreamClient::BoundPort() const {
    if (socket_fd_ < 0) {
        return 0;
    }
    sockaddr_in address{};
    socklen_t address_length = sizeof(address);
    if (getsockname(socket_fd_, reinterpret_cast<sockaddr *>(&address), &address_length) != 0) {
        return 0;
    }
    return static_cast<int>(ntohs(address.sin_port));
}

void StreamClient::Close() {
    running_ = false;

    if (socket_fd_ >= 0) {
        shutdown(socket_fd_, SHUT_RDWR);
    }

    if (keepalive_thread_.joinable()) {
        keepalive_thread_.join();
    }

    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }

    udp_assemblies_.clear();
    udp_jitter_buffer_.clear();
    next_rtp_frame_sequence_ = 0;
    ResetUdpState();
    ResetRtpState();
}

bool StreamClient::SendKeepAlive() {
    protocol::MessageHeader header{};
    protocol::FillMessageMagic(header.head_id);
    header.message_type = static_cast<std::uint16_t>(protocol::MessageType::kKeepAlive);
    if (config_.transport != "udp") {
        header.payload_length = 0;
        std::lock_guard<std::mutex> lock(send_mutex_);
        return send(socket_fd_, &header, sizeof(header), MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(header));
    }

    const UdpReceiveStats stats = udp_receive_stats();
    protocol::UdpReceiverReport report{};
    report.report_timestamp_ns = MonotonicNowNs();
    report.datagrams_received = stats.datagrams_received;
    report.invalid_datagrams = stats.invalid_datagrams;
    report.fragments_received = stats.fragments_received;
    report.duplicate_fragments = stats.duplicate_fragments;
    report.timed_out_fragments = stats.timed_out_fragments;
    report.timed_out_frames = stats.timed_out_frames;
    report.completed_frames = stats.completed_frames;
    report.reordered_frames = stats.reordered_frames;
    report.jitter_samples = stats.jitter_samples;
    report.jitter_last_ms = stats.jitter_last_ms;
    report.jitter_avg_ms = stats.jitter_avg_ms;
    report.jitter_max_ms = stats.jitter_max_ms;

    header.payload_length = static_cast<std::uint32_t>(sizeof(report));
    char payload[sizeof(header) + sizeof(report)];
    std::memcpy(payload, &header, sizeof(header));
    std::memcpy(payload + sizeof(header), &report, sizeof(report));
    std::lock_guard<std::mutex> lock(send_mutex_);
    return send(socket_fd_, payload, sizeof(payload), MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(payload));
}

void StreamClient::KeepAliveLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.keepalive_interval_ms));
        if (!running_.load()) {
            break;
        }
        if (!SendKeepAlive()) {
            running_ = false;
            break;
        }
    }
}

void StreamClient::PublishUdpReceiveStatsSnapshot() {
    std::lock_guard<std::mutex> lock(udp_receive_stats_snapshot_mutex_);
    udp_receive_stats_snapshot_ = udp_receive_stats_;
}

}  // namespace sclient
