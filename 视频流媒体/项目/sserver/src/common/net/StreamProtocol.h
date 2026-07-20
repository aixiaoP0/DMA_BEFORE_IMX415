#ifndef SSERVER_COMMON_NET_STREAMPROTOCOL_H
#define SSERVER_COMMON_NET_STREAMPROTOCOL_H

#include <cstdint>
#include <cstring>

namespace sserver {
namespace common {
namespace net {

enum class MessageType : std::uint16_t {
    kKeepAlive = 0,
    kAvStream = 1,
    kUdpNack = 2,
};

enum class UdpFragmentRole : std::uint16_t {
    kData = 0,
    kXorParity = 1,
};

static const char kMessageMagic[4] = {'C', 'C', 'T', 'C'};

#pragma pack(push, 1)
struct MessageHeader {
    char head_id[4];
    std::uint16_t message_type;
    std::uint16_t sub_type;
    std::uint32_t payload_length;
};

struct FrameDiagnosticMetadata {
    std::uint64_t sequence;
    std::uint64_t capture_timestamp_ns;
    std::uint64_t encode_start_timestamp_ns;
    std::uint64_t encode_end_timestamp_ns;
    std::uint64_t transport_send_timestamp_ns;
};

struct UdpFrameFragmentHeader {
    std::uint64_t frame_sequence;
    std::uint64_t capture_timestamp_ns;
    std::uint64_t encode_start_timestamp_ns;
    std::uint64_t encode_end_timestamp_ns;
    std::uint64_t transport_send_timestamp_ns;
    std::uint32_t frame_payload_size;
    std::uint32_t fragment_offset;
    std::uint16_t fragment_index;
    std::uint16_t fragment_count;
    std::uint16_t fragment_role;
    std::uint16_t reserved;
};

struct UdpReceiverReport {
    std::uint64_t report_timestamp_ns;
    std::uint64_t datagrams_received;
    std::uint64_t invalid_datagrams;
    std::uint64_t fragments_received;
    std::uint64_t duplicate_fragments;
    std::uint64_t timed_out_fragments;
    std::uint64_t timed_out_frames;
    std::uint64_t completed_frames;
    std::uint64_t reordered_frames;
    std::uint64_t jitter_samples;
    double jitter_last_ms;
    double jitter_avg_ms;
    double jitter_max_ms;
};

struct UdpNackHeader {
    std::uint64_t request_timestamp_ns;
    std::uint16_t request_count;
    std::uint16_t reserved;
};

struct UdpNackItem {
    std::uint64_t frame_sequence;
    std::uint16_t fragment_index;
    std::uint16_t reserved;
};
#pragma pack(pop)

inline void FillMessageMagic(char (&buffer)[4]) {
    std::memcpy(buffer, kMessageMagic, sizeof(kMessageMagic));
}

inline bool HasValidMessageMagic(const MessageHeader &header) {
    return std::memcmp(header.head_id, kMessageMagic, sizeof(kMessageMagic)) == 0;
}

}  // namespace net
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_NET_STREAMPROTOCOL_H
