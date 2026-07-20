#ifndef SSERVER_COMMON_MODEL_ENCODEDFRAME_H
#define SSERVER_COMMON_MODEL_ENCODEDFRAME_H

#include <cstdint>
#include <memory>
#include <vector>

namespace sserver {
namespace common {
namespace model {

enum class StreamPayloadType : std::uint16_t {
    kVideo = 3,
    kAudio = 4,
};

struct EncodedFrame {
    StreamPayloadType type = StreamPayloadType::kVideo;
    // 每帧的唯一追踪 ID，贯穿采集、传输、客户端诊断全链路
    std::uint64_t sequence = 0;
    std::uint64_t capture_timestamp_ns = 0;
    std::uint64_t encode_start_timestamp_ns = 0;
    std::uint64_t encode_end_timestamp_ns = 0;
    bool is_keyframe = false;
    std::vector<std::uint8_t> payload;
};

using EncodedFramePtr = std::shared_ptr<const EncodedFrame>;

}  // namespace model
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_MODEL_ENCODEDFRAME_H
