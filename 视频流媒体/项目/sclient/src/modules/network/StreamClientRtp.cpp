#include "modules/network/StreamClientInternal.h"

#include <cerrno>

#include "common/log/Logger.h"
#include "common/net/H264AnnexB.h"
#include "common/net/RtpProtocol.h"

namespace sclient {

using common::net::AppendAnnexBNalu;
using common::net::AppendAnnexBStartCode;
using common::net::ParseRtpHeader;
using common::net::RtpHeaderFields;
using common::net::RtpHeaderExtension;
using common::net::RtpLatencyExtension;
using common::net::ParseRtpLatencyExtension;
using network_internal::BuildSocketError;
using network_internal::MonotonicNowNs;

bool StreamClient::ReceiveRtpFrame(ReceivedFrame *frame, std::string *error_message) {
    while (running_.load()) {
        if (config_.udp_jitter_buffer_enabled && TryPopReadyRtpFrame(frame)) {
            return true;
        }

        const ssize_t received = recv(socket_fd_, rtp_datagram_buffer_.data(), rtp_datagram_buffer_.size(), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (config_.udp_jitter_buffer_enabled) {
                ReceivedFrame buffered_frame;
                if (TryPopReadyRtpFrame(&buffered_frame)) {
                    *frame = std::move(buffered_frame);
                    return true;
                }
            }
            if (error_message != nullptr) {
                *error_message = BuildSocketError("failed to receive RTP datagram");
            }
            return false;
        }
        if (received == 0) {
            common::log::Logger::Debug("received zero-length RTP datagram, ignoring");
            continue;
        }

        const std::uint8_t *packet_data = reinterpret_cast<const std::uint8_t *>(rtp_datagram_buffer_.data());
        const std::size_t packet_size = static_cast<std::size_t>(received);
        RtpHeaderFields header_fields;
        RtpHeaderExtension header_extension;
        std::size_t header_size = 0;
        if (!ParseRtpHeader(packet_data, packet_size, &header_fields, &header_size, &header_extension) ||
            packet_size <= header_size) {
            continue;
        }
        if (config_.rtp_payload_type >= 0 && header_fields.payload_type != static_cast<std::uint8_t>(config_.rtp_payload_type)) {
            continue;
        }

        if (!rtp_frame_assembly_.active ||
            rtp_frame_assembly_.timestamp != header_fields.timestamp ||
            rtp_frame_assembly_.ssrc != header_fields.ssrc) {
            rtp_frame_assembly_.payload.clear();
            rtp_frame_assembly_.timestamp = header_fields.timestamp;
            rtp_frame_assembly_.ssrc = header_fields.ssrc;
            rtp_frame_assembly_.next_sequence_number = static_cast<std::uint16_t>(header_fields.sequence_number + 1);
            rtp_frame_assembly_.active = true;
            rtp_frame_assembly_.has_sequence_number = true;
            rtp_frame_assembly_.capture_timestamp_ns = 0;
            rtp_frame_assembly_.transport_send_timestamp_ns = 0;
            rtp_frame_assembly_.sender_metadata_available = false;
            rtp_frame_assembly_.sender_metadata_invalid = false;
            rtp_frame_assembly_.frame_damaged = false;
            rtp_frame_assembly_.fu_in_progress = false;
        } else if (rtp_frame_assembly_.has_sequence_number &&
                   header_fields.sequence_number != rtp_frame_assembly_.next_sequence_number) {
            rtp_frame_assembly_.frame_damaged = true;
            rtp_frame_assembly_.fu_in_progress = false;
            rtp_frame_assembly_.next_sequence_number = static_cast<std::uint16_t>(header_fields.sequence_number + 1);
        } else {
            rtp_frame_assembly_.next_sequence_number = static_cast<std::uint16_t>(header_fields.sequence_number + 1);
            rtp_frame_assembly_.has_sequence_number = true;
        }

        if (config_.expect_metadata && !rtp_frame_assembly_.sender_metadata_invalid) {
            RtpLatencyExtension latency_extension;
            if (!ParseRtpLatencyExtension(header_extension, &latency_extension) ||
                latency_extension.capture_timestamp_ns == 0 ||
                latency_extension.transport_send_timestamp_ns == 0) {
                rtp_frame_assembly_.sender_metadata_available = false;
                rtp_frame_assembly_.sender_metadata_invalid = true;
            } else if (!rtp_frame_assembly_.sender_metadata_available) {
                rtp_frame_assembly_.capture_timestamp_ns = latency_extension.capture_timestamp_ns;
                rtp_frame_assembly_.transport_send_timestamp_ns = latency_extension.transport_send_timestamp_ns;
                rtp_frame_assembly_.sender_metadata_available = true;
            } else if (rtp_frame_assembly_.capture_timestamp_ns != latency_extension.capture_timestamp_ns) {
                rtp_frame_assembly_.sender_metadata_available = false;
                rtp_frame_assembly_.sender_metadata_invalid = true;
            } else {
                rtp_frame_assembly_.transport_send_timestamp_ns = latency_extension.transport_send_timestamp_ns;
            }
        }

        const std::uint8_t *payload_data = packet_data + header_size;
        const std::size_t payload_size = packet_size - header_size;
        if (payload_size > 0) {
            const std::uint8_t nal_type = static_cast<std::uint8_t>(payload_data[0] & 0x1F);
            if (nal_type > 0 && nal_type < 24) {
                AppendAnnexBNalu(payload_data, payload_size, &rtp_frame_assembly_.payload);
                rtp_frame_assembly_.fu_in_progress = false;
            } else if (nal_type == common::net::kH264FuAType && payload_size >= 2) {
                const std::uint8_t fu_header = payload_data[1];
                const bool start = (fu_header & 0x80) != 0;
                const bool end = (fu_header & 0x40) != 0;
                const std::uint8_t reconstructed_nal_header =
                        static_cast<std::uint8_t>((payload_data[0] & 0xE0) | (fu_header & 0x1F));

                if (start) {
                    if (payload_size <= 2) {
                        rtp_frame_assembly_.frame_damaged = true;
                        rtp_frame_assembly_.fu_in_progress = false;
                    } else {
                        AppendAnnexBStartCode(&rtp_frame_assembly_.payload);
                        rtp_frame_assembly_.payload.push_back(reconstructed_nal_header);
                        rtp_frame_assembly_.payload.insert(
                                rtp_frame_assembly_.payload.end(),
                                payload_data + 2,
                                payload_data + payload_size);
                        rtp_frame_assembly_.fu_in_progress = !end;
                    }
                } else if (!rtp_frame_assembly_.fu_in_progress || payload_size <= 2) {
                    rtp_frame_assembly_.frame_damaged = true;
                    rtp_frame_assembly_.fu_in_progress = false;
                } else {
                    rtp_frame_assembly_.payload.insert(
                            rtp_frame_assembly_.payload.end(),
                            payload_data + 2,
                            payload_data + payload_size);
                    if (end) {
                        rtp_frame_assembly_.fu_in_progress = false;
                    }
                }
            } else {
                rtp_frame_assembly_.frame_damaged = true;
                rtp_frame_assembly_.fu_in_progress = false;
            }
        }

        if (!header_fields.marker) {
            continue;
        }

        const std::uint64_t now_ns = MonotonicNowNs();
        const bool frame_ready =
                rtp_frame_assembly_.active &&
                !rtp_frame_assembly_.payload.empty() &&
                !rtp_frame_assembly_.frame_damaged &&
                !rtp_frame_assembly_.fu_in_progress;
        if (frame_ready) {
            ReceivedFrame completed_frame;
            std::memset(&completed_frame.header, 0, sizeof(completed_frame.header));
            protocol::FillMessageMagic(completed_frame.header.head_id);
            completed_frame.header.message_type = static_cast<std::uint16_t>(protocol::MessageType::kAvStream);
            completed_frame.header.payload_length = static_cast<std::uint32_t>(rtp_frame_assembly_.payload.size());
            std::memset(&completed_frame.metadata, 0, sizeof(completed_frame.metadata));
            completed_frame.metadata.sequence = static_cast<std::uint64_t>(rtp_frame_assembly_.timestamp);
            completed_frame.sender_metadata_available = false;
            if (config_.expect_metadata &&
                rtp_frame_assembly_.sender_metadata_available &&
                !rtp_frame_assembly_.sender_metadata_invalid) {
                completed_frame.metadata.capture_timestamp_ns = rtp_frame_assembly_.capture_timestamp_ns;
                completed_frame.metadata.transport_send_timestamp_ns = rtp_frame_assembly_.transport_send_timestamp_ns;
                completed_frame.sender_metadata_available = true;
            }
            completed_frame.receive_timestamp_ns = now_ns;
            completed_frame.payload = rtp_frame_assembly_.payload;

            if (config_.udp_jitter_buffer_enabled) {
                OnCompletedUdpFrame(completed_frame, now_ns);
                BufferCompletedUdpFrame(std::move(completed_frame));
                ResetRtpState();
                continue;
            }

            *frame = std::move(completed_frame);
        }
        ResetRtpState();
        if (frame_ready) {
            return true;
        }
    }

    if (error_message != nullptr) {
        *error_message = "client is stopping";
    }
    return false;
}

bool StreamClient::TryPopReadyRtpFrame(ReceivedFrame *frame) {
    if (frame == nullptr || udp_jitter_buffer_.empty()) {
        return false;
    }

    const std::uint64_t now_ns = MonotonicNowNs();
    return TryPopReadyUdpFrame(frame, now_ns);
}

}  // namespace sclient
