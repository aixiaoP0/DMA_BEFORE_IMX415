#ifndef SSERVER_MODULES_TRANSPORT_RTP_RTPPACKETIZER_H
#define SSERVER_MODULES_TRANSPORT_RTP_RTPPACKETIZER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/net/H264AnnexB.h"
#include "common/net/RtpProtocol.h"
#include "common/model/EncodedFrame.h"

namespace sserver {
namespace modules {
namespace transport {
namespace rtp {

// RTP 打包器：将编码后的 H264 帧按 RTP 协议拆分为多个 packet
// 支持单 NAL 直接打包和 FU-A 分片打包两种模式
class RtpPacketizer {
public:
    RtpPacketizer(
            std::uint8_t payload_type,
            std::uint32_t clock_rate,
            std::size_t max_payload_size,
            std::uint32_t ssrc,
            bool enable_latency_extension);

    bool Packetize(
            common::model::EncodedFramePtr frame,
            std::vector<std::vector<std::uint8_t> > *packets,
            std::string *error_message);

private:
    std::uint32_t BuildTimestamp(const common::model::EncodedFrame &frame) const;
    bool PacketizeSingleNalu(
            const std::uint8_t *nalu_data,
            std::size_t nalu_size,
            bool marker,
            std::uint32_t timestamp,
            const common::net::RtpHeaderExtension *header_extension,
            std::vector<std::vector<std::uint8_t> > *packets);
    bool PacketizeFragmentedNalu(
            const std::uint8_t *nalu_data,
            std::size_t nalu_size,
            bool marker,
            std::uint32_t timestamp,
            const common::net::RtpHeaderExtension *header_extension,
            std::vector<std::vector<std::uint8_t> > *packets,
            std::string *error_message);

private:
    std::uint8_t payload_type_;
    std::uint32_t clock_rate_;
    std::size_t max_payload_size_;
    std::uint32_t ssrc_;
    bool enable_latency_extension_;
    std::uint16_t sequence_number_;
    std::vector<common::net::H264NaluView> nalus_;
};

}  // namespace rtp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_RTP_RTPPACKETIZER_H
