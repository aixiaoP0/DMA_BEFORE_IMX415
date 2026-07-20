#include "modules/capture/video/null/NullCaptureDevice.h"

#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

#include "common/time/MonotonicClock.h"

namespace sserver {
namespace modules {
namespace capture {

NullCaptureDevice::NullCaptureDevice(
        const config::CaptureConfig &config,
        const config::CodecConfig &codec_config)
        : config_(config),
          codec_config_(codec_config),
          opened_(false),
          streaming_(false),
          sequence_(0) {
}

bool NullCaptureDevice::Open() {
    opened_ = true;
    return true;
}

bool NullCaptureDevice::Start() {
    if (!opened_) {
        return false;
    }

    if (config_.null_payload_mode == "h264_test_pattern") {
        encoder_ = std::make_unique<modules::encoding::VideoEncoder>();
        std::string error_message;
        if (!encoder_->Initialize(
                    config_.width,
                    config_.height,
                    config_.fps,
                    codec_config_,
                    &error_message)) {
            encoder_.reset();
            return false;
        }
        synthetic_yuyv_frame_.assign(static_cast<std::size_t>(config_.width * config_.height * 2), 0);
    }

    streaming_ = true;
    return true;
}

common::model::EncodedFramePtr NullCaptureDevice::CaptureFrame() {
    if (!streaming_) {
        return common::model::EncodedFramePtr();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(config_.frame_interval_ms));
    const std::uint64_t capture_timestamp_ns = common::time::MonotonicNowNs();

    auto frame = std::make_shared<common::model::EncodedFrame>();
    frame->sequence = sequence_++;
    frame->type = common::model::StreamPayloadType::kVideo;
    frame->capture_timestamp_ns = capture_timestamp_ns;
    if (config_.null_payload_mode == "h264_test_pattern" && encoder_ != nullptr) {
        FillSyntheticYuyvFrame();
        frame->encode_start_timestamp_ns = common::time::MonotonicNowNs();
        if (!encoder_->EncodeYuyv422Frame(
                    synthetic_yuyv_frame_.data(),
                    synthetic_yuyv_frame_.size(),
                    &frame->payload,
                    &frame->is_keyframe)) {
            return common::model::EncodedFramePtr();
        }
        frame->encode_end_timestamp_ns = common::time::MonotonicNowNs();
    } else {
        frame->encode_start_timestamp_ns = capture_timestamp_ns;
        frame->is_keyframe = true;

        std::ostringstream stream;
        stream << "null-frame-" << frame->sequence;
        const std::string text = stream.str();
        frame->payload.assign(text.begin(), text.end());
        if (config_.null_payload_bytes > frame->payload.size()) {
            frame->payload.resize(config_.null_payload_bytes, static_cast<std::uint8_t>('A' + (frame->sequence % 26)));
        }
        frame->encode_end_timestamp_ns = common::time::MonotonicNowNs();
    }

    return frame;
}

void NullCaptureDevice::Stop() {
    streaming_ = false;
    if (encoder_ != nullptr) {
        encoder_->Shutdown();
        encoder_.reset();
    }
}

void NullCaptureDevice::Close() {
    opened_ = false;
}

std::string NullCaptureDevice::Describe() const {
    return "null-capture-device";
}

void NullCaptureDevice::FillSyntheticYuyvFrame() {
    if (synthetic_yuyv_frame_.empty()) {
        return;
    }

    const int width = config_.width;
    const int height = config_.height;
    const std::uint8_t phase = static_cast<std::uint8_t>((sequence_ * 7) % 256);

    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; column += 2) {
            const std::size_t offset = static_cast<std::size_t>(row * width + column) * 2;
            const std::uint8_t y0 = static_cast<std::uint8_t>((row + column + phase) % 256);
            const std::uint8_t y1 = static_cast<std::uint8_t>((row + column + 32 + phase) % 256);
            const std::uint8_t u = static_cast<std::uint8_t>((128 + row / 2 + phase) % 256);
            const std::uint8_t v = static_cast<std::uint8_t>((64 + column / 2 + phase) % 256);
            synthetic_yuyv_frame_[offset + 0] = y0;
            synthetic_yuyv_frame_[offset + 1] = u;
            synthetic_yuyv_frame_[offset + 2] = y1;
            synthetic_yuyv_frame_[offset + 3] = v;
        }
    }
}

}  // namespace capture
}  // namespace modules
}  // namespace sserver
