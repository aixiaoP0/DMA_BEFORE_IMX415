#include "modules/transport/tcp/TcpClientSession.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#include "common/log/Logger.h"
#include "common/net/StreamProtocol.h"
#include "common/time/MonotonicClock.h"

namespace sserver {
namespace modules {
namespace transport {
namespace tcp {

TcpClientSession::TcpClientSession(
        int socket_fd,
        const config::TransportConfig &config,
        const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder,
        int latency_log_interval_frames)
        : socket_fd_(socket_fd),
          config_(config),
          running_(false),
          send_latency_recorder_(send_latency_recorder),
          queue_wait_latency_recorder_(4096),
          send_time_latency_recorder_(4096),
          latency_log_interval_frames_(latency_log_interval_frames),
          sent_frames_(0),
          overflow_dropped_frames_(0),
          stale_dropped_frames_(0),
          dropped_incoming_non_keyframes_(0),
          backpressure_events_(0),
          max_queue_depth_(0),
          remote_endpoint_(BuildRemoteEndpoint()) {
    if (config_.enable_nodelay) {
        int flag = 1;
        setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    }
}

TcpClientSession::~TcpClientSession() {
    Stop();
}

bool TcpClientSession::Start() {
    if (socket_fd_ < 0) {
        return false;
    }
    running_ = true;
    receive_thread_ = std::thread(&TcpClientSession::ReceiveLoop, this);
    send_thread_ = std::thread(&TcpClientSession::SendLoop, this);
    return true;
}

void TcpClientSession::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        CloseSocket();
    } else {
        CloseSocket();
    }

    outbound_frames_.NotifyAll();

    if (receive_thread_.joinable() && receive_thread_.get_id() != std::this_thread::get_id()) {
        receive_thread_.join();
    }
    if (send_thread_.joinable() && send_thread_.get_id() != std::this_thread::get_id()) {
        send_thread_.join();
    }
    outbound_frames_.Clear();
}

bool TcpClientSession::IsRunning() const {
    return running_.load();
}

void TcpClientSession::EnqueueFrame(common::model::EncodedFramePtr frame) {
    if (!running_.load() || !frame) {
        return;
    }

    QueuedFrame queued_frame;
    queued_frame.frame = frame;
    queued_frame.enqueue_timestamp_ns = common::time::MonotonicNowNs();

    const std::size_t queue_depth_before_push = outbound_frames_.Size();
    if (queue_depth_before_push >= config_.max_pending_frames) {
        ++backpressure_events_;
    }

    // 队列丢帧策略：
    // - drop_oldest_non_key: 优先丢弃非关键帧，保护关键帧以维持解码连续性
    // - drop_oldest: 直接丢弃队列最旧的帧
    std::size_t dropped = 0;
    if (config_.queue_drop_policy == "drop_oldest_non_key") {
        if (!frame->is_keyframe &&
            queue_depth_before_push >= config_.max_pending_frames &&
            !outbound_frames_.AnyMatching([](const QueuedFrame &candidate) {
                return candidate.frame != nullptr && !candidate.frame->is_keyframe;
            })) {
            ++dropped_incoming_non_keyframes_;
            return;
        }

        dropped = outbound_frames_.PushDropSelective(
                queued_frame,
                config_.max_pending_frames,
                [](const QueuedFrame &candidate) {
                    return candidate.frame != nullptr && !candidate.frame->is_keyframe;
                });
    } else {
        dropped = outbound_frames_.PushDropOldestCountDropped(queued_frame, config_.max_pending_frames);
    }
    overflow_dropped_frames_ += static_cast<std::uint64_t>(dropped);

    const std::size_t current_depth = outbound_frames_.Size();
    if (current_depth > max_queue_depth_) {
        max_queue_depth_ = current_depth;
    }
}

std::string TcpClientSession::remote_endpoint() const {
    return remote_endpoint_;
}

void TcpClientSession::ReceiveLoop() {
    int keep_alive_budget = 5;
    while (running_.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd_, &read_fds);

        timeval timeout{};
        timeout.tv_sec = 2;
        timeout.tv_usec = 0;

        const int ready = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
        if (!running_.load()) {
            break;
        }

        if (ready == 0) {
            --keep_alive_budget;
            if (keep_alive_budget <= 0) {
                common::log::Logger::Warn("client keepalive timeout: " + remote_endpoint_);
                break;
            }
            continue;
        }

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            common::log::Logger::Warn("client select failed: " + remote_endpoint_);
            break;
        }

        common::net::MessageHeader header{};
        if (!ReceiveAll(reinterpret_cast<char *>(&header), sizeof(header))) {
            break;
        }

        if (common::net::HasValidMessageMagic(header) &&
            header.message_type == static_cast<std::uint16_t>(common::net::MessageType::kKeepAlive)) {
            keep_alive_budget = 5;
        }
    }

    running_ = false;
    CloseSocket();
    outbound_frames_.NotifyAll();
}

void TcpClientSession::SendLoop() {
    while (running_.load()) {
        QueuedFrame queued_frame;
        if (!outbound_frames_.WaitPopFor(&queued_frame, std::chrono::milliseconds(2))) {
            continue;
        }

        const std::uint64_t send_start_timestamp_ns = common::time::MonotonicNowNs();
        if (queued_frame.enqueue_timestamp_ns != 0 && send_start_timestamp_ns >= queued_frame.enqueue_timestamp_ns) {
            const std::uint64_t queue_wait_ns = send_start_timestamp_ns - queued_frame.enqueue_timestamp_ns;
            queue_wait_latency_recorder_.RecordNs(queue_wait_ns);
            if (config_.max_queue_wait_ms > 0 &&
                queued_frame.frame != nullptr &&
                !queued_frame.frame->is_keyframe &&
                queue_wait_ns > static_cast<std::uint64_t>(config_.max_queue_wait_ms) * 1000ULL * 1000ULL) {
                ++stale_dropped_frames_;
                continue;
            }
        }

        if (!SendFrame(queued_frame.frame, send_start_timestamp_ns)) {
            break;
        }

        const std::uint64_t send_end_timestamp_ns = common::time::MonotonicNowNs();
        if (send_end_timestamp_ns >= send_start_timestamp_ns) {
            send_time_latency_recorder_.RecordNs(send_end_timestamp_ns - send_start_timestamp_ns);
        }

        if (send_latency_recorder_ != nullptr && queued_frame.frame->capture_timestamp_ns != 0) {
            send_latency_recorder_->RecordNs(send_end_timestamp_ns - queued_frame.frame->capture_timestamp_ns);
            ++sent_frames_;
            if (latency_log_interval_frames_ > 0 &&
                sent_frames_ % static_cast<std::uint64_t>(latency_log_interval_frames_) == 0) {
                common::log::Logger::Info(
                        "transport latency " + remote_endpoint_ + " " + send_latency_recorder_->Format("capture_to_send"));
                common::log::Logger::Info(
                        "tcp queue stats " + remote_endpoint_ +
                        " current_depth=" + std::to_string(outbound_frames_.Size()) +
                        " max_depth=" + std::to_string(max_queue_depth_) +
                        " overflow_dropped_frames=" + std::to_string(overflow_dropped_frames_) +
                        " stale_dropped_frames=" + std::to_string(stale_dropped_frames_) +
                        " dropped_incoming_non_keyframes=" + std::to_string(dropped_incoming_non_keyframes_) +
                        " backpressure_events=" + std::to_string(backpressure_events_) +
                        " max_queue_wait_ms=" + std::to_string(config_.max_queue_wait_ms) +
                        " queue_drop_policy=" + config_.queue_drop_policy +
                        " " + queue_wait_latency_recorder_.Format("queue_wait") +
                        " " + send_time_latency_recorder_.Format("send_time"));
            }
        }
    }

    running_ = false;
    CloseSocket();
    outbound_frames_.NotifyAll();
}

bool TcpClientSession::ReceiveAll(char *buffer, std::size_t length) {
    std::lock_guard<std::mutex> lock(receive_mutex_);

    std::size_t received = 0;
    while (received < length && running_.load()) {
        const ssize_t result = recv(socket_fd_, buffer + received, length - received, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        received += static_cast<std::size_t>(result);
    }
    return received == length;
}

bool TcpClientSession::SendFrame(common::model::EncodedFramePtr frame, std::uint64_t send_start_timestamp_ns) {
    common::net::MessageHeader header{};
    common::net::FillMessageMagic(header.head_id);
    header.message_type = static_cast<std::uint16_t>(common::net::MessageType::kAvStream);
    header.sub_type = static_cast<std::uint16_t>(frame->type);

    common::net::FrameDiagnosticMetadata metadata{};
    if (config_.embed_frame_metadata) {
        metadata.sequence = frame->sequence;
        metadata.capture_timestamp_ns = frame->capture_timestamp_ns;
        metadata.encode_start_timestamp_ns = frame->encode_start_timestamp_ns;
        metadata.encode_end_timestamp_ns = frame->encode_end_timestamp_ns;
        metadata.transport_send_timestamp_ns = send_start_timestamp_ns;
        header.payload_length = static_cast<std::uint32_t>(sizeof(metadata) + frame->payload.size());
    } else {
        header.payload_length = static_cast<std::uint32_t>(frame->payload.size());
    }

    const char *metadata_ptr = config_.embed_frame_metadata
            ? reinterpret_cast<const char *>(&metadata)
            : nullptr;
    const std::size_t metadata_length = config_.embed_frame_metadata ? sizeof(metadata) : 0;
    const char *payload_ptr = frame->payload.empty()
            ? nullptr
            : reinterpret_cast<const char *>(frame->payload.data());
    return SendMessageParts(
            reinterpret_cast<const char *>(&header),
            sizeof(header),
            metadata_ptr,
            metadata_length,
            payload_ptr,
            frame->payload.size());
}

bool TcpClientSession::SendMessageParts(
        const char *header,
        std::size_t header_length,
        const char *metadata,
        std::size_t metadata_length,
        const char *payload,
        std::size_t payload_length) {
    std::lock_guard<std::mutex> lock(send_mutex_);

    iovec iovecs[3];
    std::size_t lengths[3] = {header_length, metadata_length, payload_length};
    const char *buffers[3] = {header, metadata, payload};
    std::size_t active_count = 0;

    for (std::size_t index = 0; index < 3; ++index) {
        if (buffers[index] != nullptr && lengths[index] > 0) {
            iovecs[active_count].iov_base = const_cast<char *>(buffers[index]);
            iovecs[active_count].iov_len = lengths[index];
            ++active_count;
        }
    }

    while (active_count > 0 && running_.load()) {
        msghdr message{};
        message.msg_iov = iovecs;
        message.msg_iovlen = active_count;

        const ssize_t result = sendmsg(socket_fd_, &message, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }

        std::size_t sent = static_cast<std::size_t>(result);
        std::size_t shift = 0;
        while (shift < active_count && sent >= iovecs[shift].iov_len) {
            sent -= iovecs[shift].iov_len;
            ++shift;
        }

        if (shift > 0) {
            for (std::size_t index = shift; index < active_count; ++index) {
                iovecs[index - shift] = iovecs[index];
            }
            active_count -= shift;
        }

        if (active_count > 0 && sent > 0) {
            iovecs[0].iov_base = static_cast<char *>(iovecs[0].iov_base) + sent;
            iovecs[0].iov_len -= sent;
        }
    }

    return active_count == 0;
}

void TcpClientSession::CloseSocket() {
    if (socket_fd_ >= 0) {
        shutdown(socket_fd_, SHUT_RDWR);
        close(socket_fd_);
        socket_fd_ = -1;
    }
}

std::string TcpClientSession::BuildRemoteEndpoint() const {
    sockaddr_in address{};
    socklen_t address_length = sizeof(address);
    if (getpeername(socket_fd_, reinterpret_cast<sockaddr *>(&address), &address_length) != 0) {
        return "unknown-client";
    }

    char ip_buffer[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &address.sin_addr, ip_buffer, sizeof(ip_buffer));
    return std::string(ip_buffer) + ":" + std::to_string(ntohs(address.sin_port));
}

}  // namespace tcp
}  // namespace transport
}  // namespace modules
}  // namespace sserver
