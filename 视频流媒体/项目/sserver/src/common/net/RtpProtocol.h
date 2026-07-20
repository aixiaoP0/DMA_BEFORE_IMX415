#ifndef SSERVER_COMMON_NET_RTPPROTOCOL_H
#define SSERVER_COMMON_NET_RTPPROTOCOL_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sserver {
namespace common {
namespace net {

static const std::size_t kRtpHeaderSize = 12;
static const std::size_t kRtpExtensionHeaderSize = 4;
static const std::uint8_t kRtpVersion = 2;
static const std::uint8_t kH264FuAType = 28;
static const std::uint16_t kRtpLatencyExtensionProfileId = 0x5353;
static const std::size_t kRtpLatencyExtensionPayloadSize = 16;
static const std::size_t kRtpLatencyExtensionWordSize = kRtpLatencyExtensionPayloadSize / 4;
static const std::size_t kRtpPacketOverheadWithLatencyExtension =
        kRtpHeaderSize + kRtpExtensionHeaderSize + kRtpLatencyExtensionPayloadSize;

struct RtpHeaderFields {
    bool marker = false;
    std::uint8_t payload_type = 96;
    std::uint16_t sequence_number = 0;
    std::uint32_t timestamp = 0;
    std::uint32_t ssrc = 0;
};

struct RtpHeaderExtension {
    std::uint16_t profile_id = 0;
    std::vector<std::uint8_t> payload;
};

struct RtpLatencyExtension {
    std::uint64_t capture_timestamp_ns = 0;
    std::uint64_t transport_send_timestamp_ns = 0;
};

inline std::uint16_t ReadBe16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) |
                                      static_cast<std::uint16_t>(data[1]));
}

inline std::uint32_t ReadBe32(const std::uint8_t *data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
            (static_cast<std::uint32_t>(data[1]) << 16) |
            (static_cast<std::uint32_t>(data[2]) << 8) |
            static_cast<std::uint32_t>(data[3]);
}

inline std::uint64_t ReadBe64(const std::uint8_t *data) {
    return (static_cast<std::uint64_t>(data[0]) << 56) |
           (static_cast<std::uint64_t>(data[1]) << 48) |
           (static_cast<std::uint64_t>(data[2]) << 40) |
           (static_cast<std::uint64_t>(data[3]) << 32) |
           (static_cast<std::uint64_t>(data[4]) << 24) |
           (static_cast<std::uint64_t>(data[5]) << 16) |
           (static_cast<std::uint64_t>(data[6]) << 8) |
           static_cast<std::uint64_t>(data[7]);
}

inline void WriteBe16(std::uint16_t value, std::uint8_t *data) {
    data[0] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[1] = static_cast<std::uint8_t>(value & 0xFF);
}

inline void WriteBe32(std::uint32_t value, std::uint8_t *data) {
    data[0] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    data[1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    data[2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[3] = static_cast<std::uint8_t>(value & 0xFF);
}

inline void WriteBe64(std::uint64_t value, std::uint8_t *data) {
    data[0] = static_cast<std::uint8_t>((value >> 56) & 0xFF);
    data[1] = static_cast<std::uint8_t>((value >> 48) & 0xFF);
    data[2] = static_cast<std::uint8_t>((value >> 40) & 0xFF);
    data[3] = static_cast<std::uint8_t>((value >> 32) & 0xFF);
    data[4] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    data[5] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    data[6] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[7] = static_cast<std::uint8_t>(value & 0xFF);
}

inline std::array<std::uint8_t, kRtpLatencyExtensionPayloadSize> SerializeRtpLatencyExtension(
        const RtpLatencyExtension &extension) {
    std::array<std::uint8_t, kRtpLatencyExtensionPayloadSize> payload {};
    WriteBe64(extension.capture_timestamp_ns, payload.data());
    WriteBe64(extension.transport_send_timestamp_ns, payload.data() + 8);
    return payload;
}

inline RtpHeaderExtension BuildRtpLatencyHeaderExtension(const RtpLatencyExtension &extension) {
    RtpHeaderExtension header_extension;
    header_extension.profile_id = kRtpLatencyExtensionProfileId;
    const std::array<std::uint8_t, kRtpLatencyExtensionPayloadSize> payload =
            SerializeRtpLatencyExtension(extension);
    header_extension.payload.assign(payload.begin(), payload.end());
    return header_extension;
}

inline bool ParseRtpLatencyExtension(
        const RtpHeaderExtension &header_extension,
        RtpLatencyExtension *extension) {
    if (extension == nullptr ||
        header_extension.profile_id != kRtpLatencyExtensionProfileId ||
        header_extension.payload.size() != kRtpLatencyExtensionPayloadSize) {
        return false;
    }

    extension->capture_timestamp_ns = ReadBe64(header_extension.payload.data());
    extension->transport_send_timestamp_ns = ReadBe64(header_extension.payload.data() + 8);
    return true;
}

inline bool WriteRtpHeader(
        const RtpHeaderFields &fields,
        const RtpHeaderExtension *extension,
        std::vector<std::uint8_t> *packet) {
    if (packet == nullptr) {
        return false;
    }

    const bool has_extension = extension != nullptr && !extension->payload.empty();
    if (has_extension && (extension->payload.size() % 4) != 0) {
        return false;
    }

    const std::size_t extension_size =
            has_extension ? (kRtpExtensionHeaderSize + extension->payload.size()) : 0;
    packet->assign(kRtpHeaderSize + extension_size, 0);
    (*packet)[0] = static_cast<std::uint8_t>((kRtpVersion << 6) | (has_extension ? 0x10 : 0x00));
    (*packet)[1] = static_cast<std::uint8_t>((fields.marker ? 0x80 : 0x00) | (fields.payload_type & 0x7F));
    WriteBe16(fields.sequence_number, packet->data() + 2);
    WriteBe32(fields.timestamp, packet->data() + 4);
    WriteBe32(fields.ssrc, packet->data() + 8);

    if (has_extension) {
        WriteBe16(extension->profile_id, packet->data() + kRtpHeaderSize);
        WriteBe16(
                static_cast<std::uint16_t>(extension->payload.size() / 4),
                packet->data() + kRtpHeaderSize + 2);
        std::copy(extension->payload.begin(), extension->payload.end(), packet->data() + kRtpHeaderSize + 4);
    }

    return true;
}

inline void WriteRtpHeader(const RtpHeaderFields &fields, std::vector<std::uint8_t> *packet) {
    WriteRtpHeader(fields, nullptr, packet);
}

inline bool ParseRtpHeader(
        const std::uint8_t *data,
        std::size_t size,
        RtpHeaderFields *fields,
        std::size_t *header_size,
        RtpHeaderExtension *extension = nullptr) {
    if (data == nullptr || fields == nullptr || size < kRtpHeaderSize) {
        return false;
    }

    const std::uint8_t version = static_cast<std::uint8_t>(data[0] >> 6);
    const bool has_padding = (data[0] & 0x20) != 0;
    const bool has_extension = (data[0] & 0x10) != 0;
    const std::uint8_t csrc_count = static_cast<std::uint8_t>(data[0] & 0x0F);
    const std::size_t base_header_size = kRtpHeaderSize + static_cast<std::size_t>(csrc_count) * 4;
    if (version != kRtpVersion || has_padding || size < base_header_size) {
        return false;
    }

    std::size_t computed_header_size = base_header_size;
    if (has_extension) {
        if (size < base_header_size + kRtpExtensionHeaderSize) {
            return false;
        }

        const std::uint16_t extension_words = ReadBe16(data + base_header_size + 2);
        const std::size_t extension_payload_size = static_cast<std::size_t>(extension_words) * 4;
        computed_header_size += kRtpExtensionHeaderSize + extension_payload_size;
        if (size < computed_header_size) {
            return false;
        }

        if (extension != nullptr) {
            extension->profile_id = ReadBe16(data + base_header_size);
            extension->payload.assign(
                    data + base_header_size + kRtpExtensionHeaderSize,
                    data + base_header_size + kRtpExtensionHeaderSize + extension_payload_size);
        }
    } else if (extension != nullptr) {
        extension->profile_id = 0;
        extension->payload.clear();
    }

    fields->marker = (data[1] & 0x80) != 0;
    fields->payload_type = static_cast<std::uint8_t>(data[1] & 0x7F);
    fields->sequence_number = ReadBe16(data + 2);
    fields->timestamp = ReadBe32(data + 4);
    fields->ssrc = ReadBe32(data + 8);
    if (header_size != nullptr) {
        *header_size = computed_header_size;
    }
    return true;
}

inline bool UpdateRtpLatencyExtensionTransportSendTimestamp(
        std::uint8_t *data,
        std::size_t size,
        std::uint64_t transport_send_timestamp_ns) {
    if (data == nullptr || size < kRtpHeaderSize + kRtpExtensionHeaderSize + kRtpLatencyExtensionPayloadSize) {
        return false;
    }

    const std::uint8_t version = static_cast<std::uint8_t>(data[0] >> 6);
    const bool has_padding = (data[0] & 0x20) != 0;
    const bool has_extension = (data[0] & 0x10) != 0;
    const std::uint8_t csrc_count = static_cast<std::uint8_t>(data[0] & 0x0F);
    const std::size_t base_header_size = kRtpHeaderSize + static_cast<std::size_t>(csrc_count) * 4;
    if (version != kRtpVersion || has_padding || !has_extension || size < base_header_size + kRtpExtensionHeaderSize) {
        return false;
    }

    if (ReadBe16(data + base_header_size) != kRtpLatencyExtensionProfileId ||
        ReadBe16(data + base_header_size + 2) != kRtpLatencyExtensionWordSize ||
        size < base_header_size + kRtpExtensionHeaderSize + kRtpLatencyExtensionPayloadSize) {
        return false;
    }

    WriteBe64(transport_send_timestamp_ns, data + base_header_size + kRtpExtensionHeaderSize + 8);
    return true;
}

}  // namespace net
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_NET_RTPPROTOCOL_H
