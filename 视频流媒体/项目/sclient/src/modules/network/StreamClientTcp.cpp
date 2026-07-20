#include "modules/network/StreamClientInternal.h"

namespace sclient {

namespace {

constexpr std::uint32_t kMaxPayloadLength = 50 * 1024 * 1024;
constexpr int kMaxConsecutiveTimeouts = 10;

}  // namespace

using network_internal::BuildSocketError;
using network_internal::MonotonicNowNs;

bool StreamClient::ReceiveAll(char *buffer, std::size_t length, std::string *error_message) {
    std::size_t received = 0;
    int consecutive_timeouts = 0;
    while (received < length) {
        const ssize_t result = recv(socket_fd_, buffer + received, length - received, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (++consecutive_timeouts >= kMaxConsecutiveTimeouts) {
                    if (error_message != nullptr) {
                        *error_message = "receive timed out";
                    }
                    return false;
                }
                continue;
            }
            if (error_message != nullptr) {
                *error_message = BuildSocketError("failed to receive data");
            }
            return false;
        }
        if (result == 0) {
            if (error_message != nullptr) {
                *error_message = "server closed the connection";
            }
            return false;
        }
        received += static_cast<std::size_t>(result);
        consecutive_timeouts = 0;
    }
    return true;
}

bool StreamClient::ReceiveTcpFrame(ReceivedFrame *frame, std::string *error_message) {
    if (!ReceiveAll(reinterpret_cast<char *>(&frame->header), sizeof(frame->header), error_message)) {
        return false;
    }
    if (!protocol::HasValidMessageMagic(frame->header)) {
        if (error_message != nullptr) {
            *error_message = "received invalid message magic";
        }
        return false;
    }

    std::uint32_t payload_length = frame->header.payload_length;
    if (payload_length > kMaxPayloadLength) {
        if (error_message != nullptr) {
            *error_message = "payload length exceeds maximum";
        }
        return false;
    }
    frame->sender_metadata_available = false;
    if (config_.expect_metadata) {
        if (payload_length < sizeof(frame->metadata)) {
            if (error_message != nullptr) {
                *error_message = "payload is smaller than metadata";
            }
            return false;
        }
        if (!ReceiveAll(reinterpret_cast<char *>(&frame->metadata), sizeof(frame->metadata), error_message)) {
            return false;
        }
        payload_length -= sizeof(frame->metadata);
        frame->sender_metadata_available = network_internal::HasSenderLatencyMetadata(frame->metadata);
    } else {
        std::memset(&frame->metadata, 0, sizeof(frame->metadata));
    }

    frame->payload.resize(payload_length);
    if (!frame->payload.empty() &&
        !ReceiveAll(reinterpret_cast<char *>(frame->payload.data()), frame->payload.size(), error_message)) {
        return false;
    }
    frame->receive_timestamp_ns = MonotonicNowNs();
    return true;
}

}  // namespace sclient
