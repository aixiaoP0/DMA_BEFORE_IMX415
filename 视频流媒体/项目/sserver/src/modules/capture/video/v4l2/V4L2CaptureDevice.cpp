#include "modules/capture/video/v4l2/V4L2CaptureDevice.h"

#include <arpa/inet.h>
#include <asm/types.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/log/Logger.h"
#include "common/time/MonotonicClock.h"

namespace sserver {
namespace modules {
namespace capture {
namespace v4l2 {

namespace {

std::uint64_t TimevalToNs(const timeval &timestamp) {
    return static_cast<std::uint64_t>(timestamp.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(timestamp.tv_usec) * 1000ULL;
}

}  // namespace

V4L2CaptureDevice::V4L2CaptureDevice(
        const config::CaptureConfig &capture_config,
        const config::CodecConfig &codec_config)
        : capture_config_(capture_config),
          codec_config_(codec_config),
          device_fd_(-1),
          opened_(false),
          streaming_(false),
          logged_timestamp_source_(false),
          sequence_(0) {
}

V4L2CaptureDevice::~V4L2CaptureDevice() {
    Stop();
    Close();
}

bool V4L2CaptureDevice::Open() {
    struct stat device_stat{};
    if (stat(capture_config_.device.c_str(), &device_stat) < 0) {
        common::log::Logger::Error("cannot identify video device: " + capture_config_.device);
        return false;
    }

    if (!S_ISCHR(device_stat.st_mode)) {
        common::log::Logger::Error("configured capture device is not a character device: " + capture_config_.device);
        return false;
    }

    device_fd_ = open(capture_config_.device.c_str(), O_RDWR, 0);
    if (device_fd_ < 0) {
        common::log::Logger::Error("failed to open capture device: " + capture_config_.device);
        return false;
    }

    opened_ = true;
    return true;
}

// 启动 V4L2 采集设备：依次完成能力查询、格式配置、帧率设置、内存映射、缓冲区入队
bool V4L2CaptureDevice::Start() {
    if (!opened_) {
        return false;
    }

    if (!QueryCapabilities() || !ConfigureFormat() || !ConfigureFrameRate() || !InitializeMemoryMapping() ||
        !QueueCaptureBuffers()) {
        ReleaseMappedBuffers();
        return false;
    }

    encoder_ = std::make_unique<modules::encoding::VideoEncoder>();

    std::string error_message;
    if (!encoder_->Initialize(
                capture_config_.width,
                capture_config_.height,
                capture_config_.fps,
                codec_config_,
                &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to initialize video encoder: " + error_message);
        }
        ReleaseMappedBuffers();
        encoder_.reset();
        return false;
    }

    if (!StartStreaming()) {
        encoder_->Shutdown();
        encoder_.reset();
        ReleaseMappedBuffers();
        return false;
    }

    streaming_ = true;
    return true;
}

common::model::EncodedFramePtr V4L2CaptureDevice::CaptureFrame() {
    if (!streaming_) {
        return common::model::EncodedFramePtr();
    }

    struct v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;

    if (IoctlWithRetry(VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN) {
            return common::model::EncodedFramePtr();
        }
        common::log::Logger::Warn("VIDIOC_DQBUF failed");
        return common::model::EncodedFramePtr();
    }

    std::uint64_t capture_timestamp_ns = common::time::MonotonicNowNs();
    const bool has_monotonic_timestamp =
            (buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    if (has_monotonic_timestamp) {
        capture_timestamp_ns = TimevalToNs(buffer.timestamp);
    }
    if (!logged_timestamp_source_) {
        if (has_monotonic_timestamp) {
            common::log::Logger::Info("v4l2 capture is using hardware monotonic buffer timestamps");
        } else {
            common::log::Logger::Warn("v4l2 capture did not expose monotonic buffer timestamps, falling back to dequeue timestamps");
        }
        logged_timestamp_source_ = true;
    }

    std::vector<std::uint8_t> encoded_payload;
    bool is_keyframe = false;
    const void *start = buffers_[buffer.index].start;
    const std::size_t length = buffer.bytesused > 0 ? buffer.bytesused : buffers_[buffer.index].length;
    const std::uint64_t encode_start_timestamp_ns = common::time::MonotonicNowNs();
    std::string error_message;
    if (encoder_ == nullptr ||
        !encoder_->EncodeYuyv422Frame(
                static_cast<const std::uint8_t *>(start),
                length,
                &encoded_payload,
                &is_keyframe,
                &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Warn("video encoder dropped frame: " + error_message);
        }
        IoctlWithRetry(VIDIOC_QBUF, &buffer);
        return common::model::EncodedFramePtr();
    }
    const std::uint64_t encode_end_timestamp_ns = common::time::MonotonicNowNs();

    if (IoctlWithRetry(VIDIOC_QBUF, &buffer) < 0) {
        common::log::Logger::Warn("VIDIOC_QBUF failed");
        return common::model::EncodedFramePtr();
    }

    auto frame = std::make_shared<common::model::EncodedFrame>();
    frame->sequence = sequence_++;
    frame->type = common::model::StreamPayloadType::kVideo;
    frame->capture_timestamp_ns = capture_timestamp_ns;
    frame->encode_start_timestamp_ns = encode_start_timestamp_ns;
    frame->encode_end_timestamp_ns = encode_end_timestamp_ns;
    frame->is_keyframe = is_keyframe;
    frame->payload.swap(encoded_payload);
    return frame;
}

bool V4L2CaptureDevice::SupportsRawCapture() const {
    return true;
}

RawCaptureFramePtr V4L2CaptureDevice::CaptureRawFrame() {
    if (!streaming_) {
        return nullptr;
    }

    struct v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;

    if (IoctlWithRetry(VIDIOC_DQBUF, &buffer) < 0) {
        if (errno == EAGAIN) {
            return nullptr;
        }
        common::log::Logger::Warn("VIDIOC_DQBUF failed");
        return nullptr;
    }

    auto raw = std::make_shared<RawCaptureFrame>();
    raw->capture_timestamp_ns = common::time::MonotonicNowNs();
    const bool has_monotonic_timestamp =
            (buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
    if (has_monotonic_timestamp) {
        raw->capture_timestamp_ns = TimevalToNs(buffer.timestamp);
    }
    if (!logged_timestamp_source_) {
        if (has_monotonic_timestamp) {
            common::log::Logger::Info("v4l2 capture is using hardware monotonic buffer timestamps");
        } else {
            common::log::Logger::Warn("v4l2 capture did not expose monotonic buffer timestamps, falling back to dequeue timestamps");
        }
        logged_timestamp_source_ = true;
    }

    const void *start = buffers_[buffer.index].start;
    const std::size_t length = buffer.bytesused > 0 ? buffer.bytesused : buffers_[buffer.index].length;
    raw->data.assign(
            static_cast<const std::uint8_t *>(start),
            static_cast<const std::uint8_t *>(start) + length);
    raw->bytes_used = length;

    IoctlWithRetry(VIDIOC_QBUF, &buffer);
    return raw;
}

common::model::EncodedFramePtr V4L2CaptureDevice::EncodeRawFrame(RawCaptureFramePtr raw) {
    if (!raw || encoder_ == nullptr) {
        return nullptr;
    }

    std::vector<std::uint8_t> encoded_payload;
    bool is_keyframe = false;
    const std::uint64_t encode_start_timestamp_ns = common::time::MonotonicNowNs();
    std::string error_message;
    if (!encoder_->EncodeYuyv422Frame(
                raw->data.data(),
                raw->bytes_used,
                &encoded_payload,
                &is_keyframe,
                &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Warn("video encoder dropped frame: " + error_message);
        }
        return nullptr;
    }
    const std::uint64_t encode_end_timestamp_ns = common::time::MonotonicNowNs();

    auto frame = std::make_shared<common::model::EncodedFrame>();
    frame->sequence = sequence_++;
    frame->type = common::model::StreamPayloadType::kVideo;
    frame->capture_timestamp_ns = raw->capture_timestamp_ns;
    frame->encode_start_timestamp_ns = encode_start_timestamp_ns;
    frame->encode_end_timestamp_ns = encode_end_timestamp_ns;
    frame->is_keyframe = is_keyframe;
    frame->payload.swap(encoded_payload);
    return frame;
}

void V4L2CaptureDevice::Stop() {
    if (!opened_) {
        return;
    }

    if (streaming_) {
        StopStreaming();
        streaming_ = false;
    }

    if (encoder_ != nullptr) {
        encoder_->Shutdown();
        encoder_.reset();
    }
    ReleaseMappedBuffers();
}

void V4L2CaptureDevice::Close() {
    if (device_fd_ >= 0) {
        close(device_fd_);
        device_fd_ = -1;
    }
    opened_ = false;
}

std::string V4L2CaptureDevice::Describe() const {
    return "v4l2-capture-device(" + capture_config_.device + ")";
}

int V4L2CaptureDevice::IoctlWithRetry(unsigned long request, void *arg) {
    int result = -1;
    do {
        result = ioctl(device_fd_, request, arg);
    } while (result < 0 && errno == EINTR);
    return result;
}

bool V4L2CaptureDevice::QueryCapabilities() {
    struct v4l2_capability capability{};
    if (IoctlWithRetry(VIDIOC_QUERYCAP, &capability) < 0) {
        common::log::Logger::Error("VIDIOC_QUERYCAP failed");
        return false;
    }

    if ((capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        common::log::Logger::Error("device does not support video capture");
        return false;
    }

    if ((capability.capabilities & V4L2_CAP_STREAMING) == 0) {
        common::log::Logger::Error("device does not support streaming I/O");
        return false;
    }

    return true;
}

bool V4L2CaptureDevice::ConfigureFormat() {
    struct v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = capture_config_.width;
    format.fmt.pix.height = capture_config_.height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    format.fmt.pix.field = V4L2_FIELD_INTERLACED;

    if (IoctlWithRetry(VIDIOC_S_FMT, &format) < 0) {
        common::log::Logger::Error("VIDIOC_S_FMT failed");
        return false;
    }

    return true;
}

bool V4L2CaptureDevice::ConfigureFrameRate() {
    struct v4l2_streamparm stream_parameters{};
    stream_parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (IoctlWithRetry(VIDIOC_G_PARM, &stream_parameters) < 0) {
        common::log::Logger::Warn("VIDIOC_G_PARM failed, skipping frame rate configuration");
        return true;
    }

    if ((stream_parameters.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) == 0) {
        common::log::Logger::Warn("capture device does not expose V4L2_CAP_TIMEPERFRAME");
        return true;
    }

    stream_parameters.parm.capture.timeperframe.numerator = 1;
    stream_parameters.parm.capture.timeperframe.denominator = static_cast<unsigned int>(capture_config_.fps);
    if (IoctlWithRetry(VIDIOC_S_PARM, &stream_parameters) < 0) {
        common::log::Logger::Warn("VIDIOC_S_PARM failed, continuing with driver default fps");
        return true;
    }

    return true;
}

bool V4L2CaptureDevice::InitializeMemoryMapping() {
    struct v4l2_requestbuffers request{};
    request.count = static_cast<unsigned int>(capture_config_.device_buffer_count);
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;

    if (IoctlWithRetry(VIDIOC_REQBUFS, &request) < 0) {
        common::log::Logger::Error("VIDIOC_REQBUFS failed");
        return false;
    }

    if (request.count < 2) {
        common::log::Logger::Error("insufficient capture buffers");
        return false;
    }

    buffers_.clear();
    buffers_.resize(request.count);

    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        struct v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = static_cast<unsigned int>(index);

        if (IoctlWithRetry(VIDIOC_QUERYBUF, &buffer) < 0) {
            common::log::Logger::Error("VIDIOC_QUERYBUF failed");
            return false;
        }

        buffers_[index].length = buffer.length;
        buffers_[index].start = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, device_fd_, buffer.m.offset);
        if (buffers_[index].start == MAP_FAILED) {
            buffers_[index].start = nullptr;
            common::log::Logger::Error("mmap failed for capture buffer");
            return false;
        }
    }

    return true;
}

bool V4L2CaptureDevice::QueueCaptureBuffers() {
    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        struct v4l2_buffer buffer{};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = static_cast<unsigned int>(index);

        if (IoctlWithRetry(VIDIOC_QBUF, &buffer) < 0) {
            common::log::Logger::Error("VIDIOC_QBUF failed while queueing capture buffer");
            return false;
        }
    }
    return true;
}

bool V4L2CaptureDevice::StartStreaming() {
    enum v4l2_buf_type buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (IoctlWithRetry(VIDIOC_STREAMON, &buffer_type) < 0) {
        common::log::Logger::Error("VIDIOC_STREAMON failed");
        return false;
    }
    return true;
}

void V4L2CaptureDevice::StopStreaming() {
    enum v4l2_buf_type buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    IoctlWithRetry(VIDIOC_STREAMOFF, &buffer_type);
}

void V4L2CaptureDevice::ReleaseMappedBuffers() {
    for (std::size_t index = 0; index < buffers_.size(); ++index) {
        if (buffers_[index].start != nullptr) {
            munmap(buffers_[index].start, buffers_[index].length);
            buffers_[index].start = nullptr;
            buffers_[index].length = 0;
        }
    }
    buffers_.clear();
}

}  // namespace v4l2
}  // namespace capture
}  // namespace modules
}  // namespace sserver
