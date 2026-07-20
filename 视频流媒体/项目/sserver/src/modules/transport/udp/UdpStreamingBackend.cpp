#include "modules/transport/udp/UdpStreamingBackend.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "common/log/Logger.h"
#include "common/net/StreamProtocol.h"
#include "common/time/MonotonicClock.h"

namespace sserver {
namespace modules {
namespace transport {
namespace udp {

namespace {

// 将 source 数据异或累加到 target 中，用于 FEC 前向纠错码的计算
void XorInto(std::vector<std::uint8_t> *target, const std::uint8_t *source, std::size_t length) {
    if (target == nullptr || source == nullptr || length == 0) {
        return;
    }

    if (target->size() < length) {
        target->resize(length, 0);
    }
    for (std::size_t index = 0; index < length; ++index) {
        (*target)[index] ^= source[index];
    }
}

bool IsKeepAlivePacket(const char *buffer, std::size_t length) {
    if (length < sizeof(common::net::MessageHeader)) {
        return false;
    }

    const common::net::MessageHeader *header = reinterpret_cast<const common::net::MessageHeader *>(buffer);
    return common::net::HasValidMessageMagic(*header) &&
           header->message_type == static_cast<std::uint16_t>(common::net::MessageType::kKeepAlive);
}

bool IsNackPacket(const char *buffer, std::size_t length) {
    if (length < sizeof(common::net::MessageHeader)) {
        return false;
    }

    const common::net::MessageHeader *header = reinterpret_cast<const common::net::MessageHeader *>(buffer);
    return common::net::HasValidMessageMagic(*header) &&
           header->message_type == static_cast<std::uint16_t>(common::net::MessageType::kUdpNack);
}

bool ParseKeepAliveReport(
        const char *buffer,
        std::size_t length,
        common::net::UdpReceiverReport *report) {
    if (report == nullptr || length < sizeof(common::net::MessageHeader)) {
        return false;
    }

    const common::net::MessageHeader *header = reinterpret_cast<const common::net::MessageHeader *>(buffer);
    if (!common::net::HasValidMessageMagic(*header) ||
        header->message_type != static_cast<std::uint16_t>(common::net::MessageType::kKeepAlive) ||
        header->payload_length < sizeof(common::net::UdpReceiverReport) ||
        length < sizeof(common::net::MessageHeader) + sizeof(common::net::UdpReceiverReport)) {
        return false;
    }

    std::memcpy(
            report,
            buffer + sizeof(common::net::MessageHeader),
            sizeof(common::net::UdpReceiverReport));
    return true;
}

bool ParseNackRequest(
        const char *buffer,
        std::size_t length,
        common::net::UdpNackHeader *nack_header,
        std::vector<common::net::UdpNackItem> *nack_items) {
    if (nack_header == nullptr || nack_items == nullptr || length < sizeof(common::net::MessageHeader)) {
        return false;
    }

    const common::net::MessageHeader *header = reinterpret_cast<const common::net::MessageHeader *>(buffer);
    if (!common::net::HasValidMessageMagic(*header) ||
        header->message_type != static_cast<std::uint16_t>(common::net::MessageType::kUdpNack) ||
        header->payload_length < sizeof(common::net::UdpNackHeader) ||
        length < sizeof(common::net::MessageHeader) + sizeof(common::net::UdpNackHeader)) {
        return false;
    }

    std::memcpy(
            nack_header,
            buffer + sizeof(common::net::MessageHeader),
            sizeof(common::net::UdpNackHeader));
    const std::size_t expected_payload_length =
            sizeof(common::net::UdpNackHeader) +
            static_cast<std::size_t>(nack_header->request_count) * sizeof(common::net::UdpNackItem);
    if (header->payload_length != expected_payload_length ||
        length < sizeof(common::net::MessageHeader) + expected_payload_length) {
        return false;
    }

    nack_items->resize(nack_header->request_count);
    if (!nack_items->empty()) {
        std::memcpy(
                nack_items->data(),
                buffer + sizeof(common::net::MessageHeader) + sizeof(common::net::UdpNackHeader),
                nack_items->size() * sizeof(common::net::UdpNackItem));
    }
    return true;
}

}  // namespace

UdpStreamingBackend::UdpStreamingBackend(
        const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder)
        : backend_name_("udp"),
          latency_log_interval_frames_(120),
          socket_fd_(-1),
          bound_port_(0),
          running_(false),
          send_latency_recorder_(send_latency_recorder),
          state_(core::ModuleState::kCreated),
          sent_frames_(0),
          dropped_fragmented_frames_(0),
          sent_fragments_(0),
          fec_fragments_sent_(0),
          failed_fragments_(0),
          nack_requests_received_(0),
          nack_fragments_requested_(0),
          retransmitted_fragments_sent_(0),
          retransmit_fragment_misses_(0),
          retransmit_fragments_throttled_(0) {
}

UdpStreamingBackend::~UdpStreamingBackend() {
    shutdown();
}

bool UdpStreamingBackend::initialize(const core::ApplicationContext &context) {
    config_ = context.config.transport;
    latency_log_interval_frames_ = context.config.runtime.latency_log_interval_frames;
    state_ = core::ModuleState::kInitialized;
    return true;
}

bool UdpStreamingBackend::start() {
    if (!config_.enabled) {
        state_ = core::ModuleState::kRunning;
        return true;
    }

    if (!OpenSocket()) {
        state_ = core::ModuleState::kFailed;
        return false;
    }

    running_ = true;
    receive_thread_ = std::thread(&UdpStreamingBackend::ReceiveLoop, this);
    state_ = core::ModuleState::kRunning;
    return true;
}

void UdpStreamingBackend::stop() {
    if (state_.load() != core::ModuleState::kRunning) {
        return;
    }

    running_ = false;
    CloseSocket();

    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(retransmit_cache_mutex_);
        retransmit_cache_.clear();
    }

    state_ = core::ModuleState::kStopped;
}

void UdpStreamingBackend::shutdown() {
    stop();
    state_ = core::ModuleState::kShutdown;
}

core::ModuleState UdpStreamingBackend::state() const {
    return state_.load();
}

void UdpStreamingBackend::Broadcast(common::model::EncodedFramePtr frame) {
    if (!config_.enabled || !frame || socket_fd_ < 0) {
        return;
    }

    const std::uint64_t now_ns = common::time::MonotonicNowNs();
    const std::vector<sockaddr_in> clients = SnapshotClients(now_ns);
    if (clients.empty()) {
        return;
    }

    if (!SendFrameFragments(frame, clients)) {
        ++dropped_fragmented_frames_;
        if (dropped_fragmented_frames_ == 1 || dropped_fragmented_frames_ % 30 == 0) {
            common::log::Logger::Warn("udp frame dropped because fragmentation or send failed");
        }
        return;
    }

        if (send_latency_recorder_ != nullptr && frame->capture_timestamp_ns != 0) {
            send_latency_recorder_->RecordNs(common::time::MonotonicNowNs() - frame->capture_timestamp_ns);
            ++sent_frames_;
            if (latency_log_interval_frames_ > 0 &&
                sent_frames_ % static_cast<std::uint64_t>(latency_log_interval_frames_) == 0) {
            common::log::Logger::Info("transport latency udp-broadcast " +
                                      send_latency_recorder_->Format("capture_to_send"));
            common::log::Logger::Info(
                    "udp transport stats"
                    " sent_frames=" + std::to_string(sent_frames_) +
                    " sent_fragments=" + std::to_string(sent_fragments_) +
                    " fec_fragments_sent=" + std::to_string(fec_fragments_sent_) +
                    " failed_fragments=" + std::to_string(failed_fragments_) +
                    " dropped_frames=" + std::to_string(dropped_fragmented_frames_) +
                    " nack_requests_received=" + std::to_string(nack_requests_received_) +
                    " nack_fragments_requested=" + std::to_string(nack_fragments_requested_) +
                    " retransmitted_fragments_sent=" + std::to_string(retransmitted_fragments_sent_) +
                    " retransmit_fragment_misses=" + std::to_string(retransmit_fragment_misses_) +
                    " retransmit_fragments_throttled=" + std::to_string(retransmit_fragments_throttled_));
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (std::size_t index = 0; index < clients_.size(); ++index) {
                if (clients_[index].has_report) {
                    common::log::Logger::Info(FormatClientReport(clients_[index]));
                }
            }
        }
    }
}

int UdpStreamingBackend::bound_port() const {
    return bound_port_;
}

TransportBackend UdpStreamingBackend::backend() const {
    return TransportBackend::kUdp;
}

const std::string &UdpStreamingBackend::backend_name() const {
    return backend_name_;
}

bool UdpStreamingBackend::OpenSocket() {
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        common::log::Logger::Error("failed to create udp transport socket");
        return false;
    }

    int reuse = 1;
    setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &config_.udp_receive_buffer_bytes, sizeof(config_.udp_receive_buffer_bytes));
    setsockopt(socket_fd_, SOL_SOCKET, SO_SNDBUF, &config_.udp_send_buffer_bytes, sizeof(config_.udp_send_buffer_bytes));

    const int current_flags = fcntl(socket_fd_, F_GETFL, 0);
    if (current_flags >= 0) {
        fcntl(socket_fd_, F_SETFL, current_flags | O_NONBLOCK);
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config_.listen_port));
    address.sin_addr.s_addr = inet_addr(config_.bind_address.c_str());

    if (bind(socket_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        common::log::Logger::Error("failed to bind udp transport socket");
        CloseSocket();
        return false;
    }

    if (config_.listen_port == 0) {
        sockaddr_in bound_address{};
        socklen_t bound_length = sizeof(bound_address);
        if (getsockname(socket_fd_, reinterpret_cast<sockaddr *>(&bound_address), &bound_length) == 0) {
            bound_port_ = ntohs(bound_address.sin_port);
            common::log::Logger::Info("udp transport bound to ephemeral port " + std::to_string(bound_port_));
        }
    } else {
        bound_port_ = config_.listen_port;
    }

    return true;
}

void UdpStreamingBackend::CloseSocket() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
}

void UdpStreamingBackend::ReceiveLoop() {
    std::vector<char> buffer(config_.udp_max_datagram_size);

    while (running_.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(socket_fd_, &read_fds);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200 * 1000;

        const int ready = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
        if (!running_.load()) {
            break;
        }
        if (ready == 0) {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            PruneStaleClientsLocked(common::time::MonotonicNowNs());
            continue;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            common::log::Logger::Warn("udp transport select failed");
            break;
        }

        sockaddr_in client_address{};
        socklen_t client_length = sizeof(client_address);
        const ssize_t received = recvfrom(
                socket_fd_,
                buffer.data(),
                buffer.size(),
                0,
                reinterpret_cast<sockaddr *>(&client_address),
                &client_length);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            common::log::Logger::Warn("udp transport recvfrom failed");
            break;
        }

        const std::uint64_t now_ns = common::time::MonotonicNowNs();
        if (IsKeepAlivePacket(buffer.data(), static_cast<std::size_t>(received))) {
            common::net::UdpReceiverReport report{};
            const bool has_report = ParseKeepAliveReport(buffer.data(), static_cast<std::size_t>(received), &report);
            RegisterClient(client_address, now_ns, has_report ? &report : nullptr);
        } else if (IsNackPacket(buffer.data(), static_cast<std::size_t>(received))) {
            common::net::UdpNackHeader nack_header{};
            std::vector<common::net::UdpNackItem> nack_items;
            if (ParseNackRequest(buffer.data(), static_cast<std::size_t>(received), &nack_header, &nack_items)) {
                RegisterClient(client_address, now_ns, nullptr);
                HandleNackRequest(client_address, now_ns, nack_header, nack_items);
            }
        }
    }

    running_ = false;
    CloseSocket();
}

void UdpStreamingBackend::RegisterClient(
        const sockaddr_in &address,
        std::uint64_t now_ns,
        const common::net::UdpReceiverReport *report) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    PruneStaleClientsLocked(now_ns);

    for (std::size_t index = 0; index < clients_.size(); ++index) {
        if (SameEndpoint(clients_[index].address, address)) {
            clients_[index].last_seen_ns = now_ns;
            if (report != nullptr) {
                clients_[index].latest_report = *report;
                clients_[index].latest_report_timestamp_ns = now_ns;
                clients_[index].has_report = true;
                common::log::Logger::Info(FormatClientReport(clients_[index]));
            }
            return;
        }
    }

    UdpClientEndpoint endpoint;
    endpoint.address = address;
    endpoint.last_seen_ns = now_ns;
    endpoint.latest_report_timestamp_ns = 0;
    endpoint.has_report = false;
    if (report != nullptr) {
        endpoint.latest_report = *report;
        endpoint.latest_report_timestamp_ns = now_ns;
        endpoint.has_report = true;
    }
    clients_.push_back(endpoint);
    common::log::Logger::Info("udp client registered: " + FormatEndpoint(address));
    if (endpoint.has_report) {
        common::log::Logger::Info(FormatClientReport(endpoint));
    }
}

void UdpStreamingBackend::PruneStaleClientsLocked(std::uint64_t now_ns) {
    const std::uint64_t timeout_ns = static_cast<std::uint64_t>(config_.udp_client_timeout_ms) * 1000ULL * 1000ULL;
    clients_.erase(
            std::remove_if(
                    clients_.begin(),
                    clients_.end(),
                    [now_ns, timeout_ns](const UdpClientEndpoint &client) {
                        return now_ns > client.last_seen_ns && now_ns - client.last_seen_ns > timeout_ns;
                    }),
            clients_.end());
}

std::vector<sockaddr_in> UdpStreamingBackend::SnapshotClients(std::uint64_t now_ns) {
    std::vector<sockaddr_in> snapshot;
    std::lock_guard<std::mutex> lock(clients_mutex_);
    PruneStaleClientsLocked(now_ns);
    snapshot.reserve(clients_.size());
    for (std::size_t index = 0; index < clients_.size(); ++index) {
        snapshot.push_back(clients_[index].address);
    }
    return snapshot;
}

// 将一帧拆分为多个 UDP 分片发送，支持：
// - 批量发送（sendmmsg）优化多客户端场景
// - FEC 前向纠错（XOR 奇偶校验分片）
// - NACK 重传缓存（缓存已发分片供后续重传）
bool UdpStreamingBackend::SendFrameFragments(
        common::model::EncodedFramePtr frame,
        const std::vector<sockaddr_in> &clients) {
    if (!frame) {
        return false;
    }

    const std::size_t header_size = sizeof(common::net::MessageHeader) + sizeof(common::net::UdpFrameFragmentHeader);
    if (config_.udp_target_payload_size <= header_size || config_.udp_max_datagram_size <= header_size) {
        return false;
    }

    const std::size_t max_fragment_payload_size = std::min(
            config_.udp_target_payload_size,
            config_.udp_max_datagram_size - header_size);
    const std::size_t fragment_count_size = frame->payload.empty()
            ? 1
            : (frame->payload.size() + max_fragment_payload_size - 1) / max_fragment_payload_size;
    if (fragment_count_size > 0xFFFFu) {
        common::log::Logger::Warn("udp frame dropped because fragment count exceeds protocol limit");
        return false;
    }

    const std::uint16_t fragment_count = static_cast<std::uint16_t>(fragment_count_size);
    common::net::MessageHeader header{};
    common::net::FillMessageMagic(header.head_id);
    header.message_type = static_cast<std::uint16_t>(common::net::MessageType::kAvStream);
    header.sub_type = static_cast<std::uint16_t>(frame->type);
    const std::uint64_t transport_send_timestamp_ns = common::time::MonotonicNowNs();
    CachedUdpFrame cached_frame;
    cached_frame.frame_sequence = frame->sequence;
    cached_frame.cached_timestamp_ns = transport_send_timestamp_ns;
    std::vector<std::uint8_t> fec_payload;

    if (send_datagram_.size() < config_.udp_max_datagram_size) {
        send_datagram_.resize(config_.udp_max_datagram_size);
    }

    const bool batch_send = clients.size() > 1;
    std::vector<struct mmsghdr> batch_messages;
    std::vector<struct iovec> batch_iovecs;
    if (batch_send) {
        batch_messages.resize(clients.size());
        batch_iovecs.resize(clients.size());
    }

    for (std::uint16_t fragment_index = 0; fragment_index < fragment_count; ++fragment_index) {
        const std::size_t fragment_offset = static_cast<std::size_t>(fragment_index) * max_fragment_payload_size;
        const std::size_t fragment_payload_size = frame->payload.empty()
                ? 0
                : std::min(max_fragment_payload_size, frame->payload.size() - fragment_offset);

        common::net::UdpFrameFragmentHeader fragment_header{};
        fragment_header.frame_sequence = frame->sequence;
        fragment_header.capture_timestamp_ns = config_.embed_frame_metadata ? frame->capture_timestamp_ns : 0;
        fragment_header.encode_start_timestamp_ns = config_.embed_frame_metadata ? frame->encode_start_timestamp_ns : 0;
        fragment_header.encode_end_timestamp_ns = config_.embed_frame_metadata ? frame->encode_end_timestamp_ns : 0;
        fragment_header.transport_send_timestamp_ns = config_.embed_frame_metadata ? transport_send_timestamp_ns : 0;
        fragment_header.frame_payload_size = static_cast<std::uint32_t>(frame->payload.size());
        fragment_header.fragment_offset = static_cast<std::uint32_t>(fragment_offset);
        fragment_header.fragment_index = fragment_index;
        fragment_header.fragment_count = fragment_count;
        fragment_header.fragment_role = static_cast<std::uint16_t>(common::net::UdpFragmentRole::kData);

        header.payload_length = static_cast<std::uint32_t>(sizeof(fragment_header) + fragment_payload_size);
        const std::size_t datagram_size = sizeof(header) + sizeof(fragment_header) + fragment_payload_size;

        std::memcpy(send_datagram_.data(), &header, sizeof(header));
        std::memcpy(send_datagram_.data() + sizeof(header), &fragment_header, sizeof(fragment_header));
        if (fragment_payload_size > 0) {
            std::memcpy(
                    send_datagram_.data() + sizeof(header) + sizeof(fragment_header),
                    frame->payload.data() + fragment_offset,
                    fragment_payload_size);
        }

        if (config_.udp_enable_nack) {
            CachedUdpFragment cached_fragment;
            cached_fragment.fragment_index = fragment_index;
            cached_fragment.datagram_size = datagram_size;
            cached_fragment.datagram.assign(send_datagram_.begin(), send_datagram_.begin() + static_cast<std::ptrdiff_t>(datagram_size));
            cached_frame.fragments.push_back(cached_fragment);
        }
        if (config_.udp_enable_fec && fragment_payload_size > 0) {
            XorInto(&fec_payload, frame->payload.data() + fragment_offset, fragment_payload_size);
        }

        bool sent_fragment = false;
        if (batch_send) {
            for (std::size_t ci = 0; ci < clients.size(); ++ci) {
                batch_iovecs[ci].iov_base = send_datagram_.data();
                batch_iovecs[ci].iov_len = datagram_size;
                std::memset(&batch_messages[ci], 0, sizeof(batch_messages[ci]));
                batch_messages[ci].msg_hdr.msg_iov = &batch_iovecs[ci];
                batch_messages[ci].msg_hdr.msg_iovlen = 1;
                batch_messages[ci].msg_hdr.msg_name = const_cast<sockaddr_in *>(&clients[ci]);
                batch_messages[ci].msg_hdr.msg_namelen = sizeof(clients[ci]);
            }
            const int sent_count = sendmmsg(socket_fd_, batch_messages.data(),
                                             static_cast<unsigned int>(batch_messages.size()), MSG_NOSIGNAL);
            if (sent_count > 0) {
                sent_fragment = true;
                sent_fragments_ += static_cast<std::uint64_t>(sent_count);
                failed_fragments_ += static_cast<std::uint64_t>(clients.size()) - static_cast<std::uint64_t>(sent_count);
            }
        } else if (!clients.empty()) {
            const ssize_t sent = sendto(
                    socket_fd_,
                    send_datagram_.data(),
                    datagram_size,
                    MSG_NOSIGNAL,
                    reinterpret_cast<const sockaddr *>(&clients[0]),
                    sizeof(clients[0]));
            if (sent == static_cast<ssize_t>(datagram_size)) {
                sent_fragment = true;
                ++sent_fragments_;
            } else {
                ++failed_fragments_;
            }
        }
        if (!sent_fragment) {
            return false;
        }
    }

    // 发送 FEC 奇偶校验分片：将所有数据分片异或合并后作为一个额外分片发送
    // 接收端可通过此分片恢复任意一个丢失的数据分片
    if (config_.udp_enable_fec && fragment_count > 1 && !fec_payload.empty()) {
        common::net::UdpFrameFragmentHeader parity_header{};
        parity_header.frame_sequence = frame->sequence;
        parity_header.capture_timestamp_ns = config_.embed_frame_metadata ? frame->capture_timestamp_ns : 0;
        parity_header.encode_start_timestamp_ns = config_.embed_frame_metadata ? frame->encode_start_timestamp_ns : 0;
        parity_header.encode_end_timestamp_ns = config_.embed_frame_metadata ? frame->encode_end_timestamp_ns : 0;
        parity_header.transport_send_timestamp_ns = config_.embed_frame_metadata ? transport_send_timestamp_ns : 0;
        parity_header.frame_payload_size = static_cast<std::uint32_t>(frame->payload.size());
        parity_header.fragment_offset = 0;
        parity_header.fragment_index = 0;
        parity_header.fragment_count = fragment_count;
        parity_header.fragment_role = static_cast<std::uint16_t>(common::net::UdpFragmentRole::kXorParity);

        header.payload_length = static_cast<std::uint32_t>(sizeof(parity_header) + fec_payload.size());
        const std::size_t parity_datagram_size = sizeof(header) + sizeof(parity_header) + fec_payload.size();
        std::memcpy(send_datagram_.data(), &header, sizeof(header));
        std::memcpy(send_datagram_.data() + sizeof(header), &parity_header, sizeof(parity_header));
        std::memcpy(
                send_datagram_.data() + sizeof(header) + sizeof(parity_header),
                fec_payload.data(),
                fec_payload.size());

        bool sent_parity = false;
        if (batch_send) {
            for (std::size_t ci = 0; ci < clients.size(); ++ci) {
                batch_iovecs[ci].iov_base = send_datagram_.data();
                batch_iovecs[ci].iov_len = parity_datagram_size;
                std::memset(&batch_messages[ci], 0, sizeof(batch_messages[ci]));
                batch_messages[ci].msg_hdr.msg_iov = &batch_iovecs[ci];
                batch_messages[ci].msg_hdr.msg_iovlen = 1;
                batch_messages[ci].msg_hdr.msg_name = const_cast<sockaddr_in *>(&clients[ci]);
                batch_messages[ci].msg_hdr.msg_namelen = sizeof(clients[ci]);
            }
            const int sent_count = sendmmsg(socket_fd_, batch_messages.data(),
                                             static_cast<unsigned int>(batch_messages.size()), MSG_NOSIGNAL);
            if (sent_count > 0) {
                sent_parity = true;
                sent_fragments_ += static_cast<std::uint64_t>(sent_count);
                fec_fragments_sent_ += static_cast<std::uint64_t>(sent_count);
                failed_fragments_ += static_cast<std::uint64_t>(clients.size()) - static_cast<std::uint64_t>(sent_count);
            }
        } else if (!clients.empty()) {
            const ssize_t sent = sendto(
                    socket_fd_,
                    send_datagram_.data(),
                    parity_datagram_size,
                    MSG_NOSIGNAL,
                    reinterpret_cast<const sockaddr *>(&clients[0]),
                    sizeof(clients[0]));
            if (sent == static_cast<ssize_t>(parity_datagram_size)) {
                sent_parity = true;
                ++sent_fragments_;
                ++fec_fragments_sent_;
            } else {
                ++failed_fragments_;
            }
        }
        if (!sent_parity) {
            return false;
        }
    }

    if (config_.udp_enable_nack && !cached_frame.fragments.empty()) {
        CacheFrameFragments(cached_frame);
    }

    return true;
}

void UdpStreamingBackend::CacheFrameFragments(const CachedUdpFrame &frame) {
    std::lock_guard<std::mutex> lock(retransmit_cache_mutex_);
    PruneRetransmitCacheLocked(common::time::MonotonicNowNs());
    retransmit_cache_.push_back(frame);
    while (retransmit_cache_.size() > config_.udp_retransmit_cache_frames) {
        retransmit_cache_.pop_front();
    }
}

void UdpStreamingBackend::PruneRetransmitCacheLocked(std::uint64_t now_ns) {
    const std::uint64_t max_age_ns =
            static_cast<std::uint64_t>(config_.udp_retransmit_cache_max_age_ms) * 1000ULL * 1000ULL;
    while (!retransmit_cache_.empty()) {
        const CachedUdpFrame &frame = retransmit_cache_.front();
        if (now_ns >= frame.cached_timestamp_ns && now_ns - frame.cached_timestamp_ns > max_age_ns) {
            retransmit_cache_.pop_front();
            continue;
        }
        break;
    }
}

// 处理客户端 NACK 重传请求：在重传缓存中查找请求的分片并重发
void UdpStreamingBackend::HandleNackRequest(
        const sockaddr_in &address,
        std::uint64_t now_ns,
        const common::net::UdpNackHeader & /* nack_header */,
        const std::vector<common::net::UdpNackItem> &nack_items) {
    if (!config_.udp_enable_nack || nack_items.empty() || socket_fd_ < 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(retransmit_cache_mutex_);
        PruneRetransmitCacheLocked(now_ns);
    }

    ++nack_requests_received_;
    nack_fragments_requested_ += nack_items.size();

    std::lock_guard<std::mutex> lock(retransmit_cache_mutex_);
    const std::size_t allowed_fragments =
            std::min<std::size_t>(nack_items.size(), config_.udp_retransmit_max_fragments_per_request);
    if (nack_items.size() > allowed_fragments) {
        retransmit_fragments_throttled_ += nack_items.size() - allowed_fragments;
    }

    for (std::size_t item_index = 0; item_index < allowed_fragments; ++item_index) {
        const common::net::UdpNackItem &item = nack_items[item_index];
        bool found_fragment = false;
        for (std::size_t frame_index = 0; frame_index < retransmit_cache_.size() && !found_fragment; ++frame_index) {
            const CachedUdpFrame &cached_frame = retransmit_cache_[frame_index];
            if (cached_frame.frame_sequence != item.frame_sequence) {
                continue;
            }
            for (std::size_t fragment_index = 0; fragment_index < cached_frame.fragments.size(); ++fragment_index) {
                const CachedUdpFragment &cached_fragment = cached_frame.fragments[fragment_index];
                if (cached_fragment.fragment_index != item.fragment_index) {
                    continue;
                }
                const ssize_t sent = sendto(
                        socket_fd_,
                        cached_fragment.datagram.data(),
                        cached_fragment.datagram_size,
                        MSG_NOSIGNAL,
                        reinterpret_cast<const sockaddr *>(&address),
                        sizeof(address));
                if (sent == static_cast<ssize_t>(cached_fragment.datagram_size)) {
                    ++retransmitted_fragments_sent_;
                }
                found_fragment = true;
                break;
            }
        }
        if (!found_fragment) {
            ++retransmit_fragment_misses_;
        }
    }
}

std::string UdpStreamingBackend::FormatEndpoint(const sockaddr_in &address) const {
    char ip_buffer[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &address.sin_addr, ip_buffer, sizeof(ip_buffer));
    return std::string(ip_buffer) + ":" + std::to_string(ntohs(address.sin_port));
}

std::string UdpStreamingBackend::FormatClientReport(const UdpClientEndpoint &client) const {
    const common::net::UdpReceiverReport &report = client.latest_report;
    const std::uint64_t total_fragment_attempts = report.fragments_received + report.timed_out_fragments;
    const double fragment_loss_percent = total_fragment_attempts == 0
            ? 0.0
            : static_cast<double>(report.timed_out_fragments) * 100.0 / static_cast<double>(total_fragment_attempts);

    return "udp client report"
            " endpoint=" + FormatEndpoint(client.address) +
            " completed_frames=" + std::to_string(report.completed_frames) +
            " reordered_frames=" + std::to_string(report.reordered_frames) +
            " fragments=" + std::to_string(report.fragments_received) +
            " duplicate_fragments=" + std::to_string(report.duplicate_fragments) +
            " timed_out_fragments=" + std::to_string(report.timed_out_fragments) +
            " timed_out_frames=" + std::to_string(report.timed_out_frames) +
            " invalid_datagrams=" + std::to_string(report.invalid_datagrams) +
            " fragment_loss=" + std::to_string(fragment_loss_percent) + "%" +
            " jitter_avg=" + std::to_string(report.jitter_avg_ms) + "ms" +
            " jitter_max=" + std::to_string(report.jitter_max_ms) + "ms";
}

bool UdpStreamingBackend::SameEndpoint(const sockaddr_in &lhs, const sockaddr_in &rhs) const {
    return lhs.sin_port == rhs.sin_port && lhs.sin_addr.s_addr == rhs.sin_addr.s_addr;
}

}  // namespace udp
}  // namespace transport
}  // namespace modules
}  // namespace sserver
