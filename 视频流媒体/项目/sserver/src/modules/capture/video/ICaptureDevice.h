#ifndef SSERVER_MODULES_CAPTURE_VIDEO_ICAPTUREDEVICE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_ICAPTUREDEVICE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/model/EncodedFrame.h"

namespace sserver {
namespace modules {
namespace capture {

struct RawCaptureFrame {
    std::vector<std::uint8_t> data;
    std::uint64_t capture_timestamp_ns = 0;
    std::size_t bytes_used = 0;
};

using RawCaptureFramePtr = std::shared_ptr<RawCaptureFrame>;

class ICaptureDevice {
public:
    virtual ~ICaptureDevice() = default;

    virtual bool Open() = 0;
    virtual bool Start() = 0;
    virtual common::model::EncodedFramePtr CaptureFrame() = 0;
    virtual void Stop() = 0;
    virtual void Close() = 0;
    virtual std::string Describe() const = 0;

    virtual bool SupportsRawCapture() const { return false; }
    virtual RawCaptureFramePtr CaptureRawFrame() { return nullptr; }
    virtual common::model::EncodedFramePtr EncodeRawFrame(RawCaptureFramePtr /* raw */) { return nullptr; }
};

}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_ICAPTUREDEVICE_H
