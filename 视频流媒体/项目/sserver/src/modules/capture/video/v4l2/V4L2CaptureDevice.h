#ifndef SSERVER_MODULES_CAPTURE_VIDEO_V4L2_V4L2CAPTUREDEVICE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_V4L2_V4L2CAPTUREDEVICE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <linux/videodev2.h>

#include "config/AppConfig.h"
#include "modules/capture/video/ICaptureDevice.h"
#include "modules/encoding/video/VideoEncoder.h"

namespace sserver {
namespace modules {
namespace capture {
namespace v4l2 {

struct MappedBuffer {
    void *start = nullptr;
    std::size_t length = 0;
};

class V4L2CaptureDevice : public ICaptureDevice {
public:
    V4L2CaptureDevice(const config::CaptureConfig &capture_config, const config::CodecConfig &codec_config);
    ~V4L2CaptureDevice() override;

    bool Open() override;
    bool Start() override;
    common::model::EncodedFramePtr CaptureFrame() override;
    void Stop() override;
    void Close() override;
    std::string Describe() const override;

    bool SupportsRawCapture() const override;
    RawCaptureFramePtr CaptureRawFrame() override;
    common::model::EncodedFramePtr EncodeRawFrame(RawCaptureFramePtr raw) override;

private:
    int IoctlWithRetry(unsigned long request, void *arg);
    bool QueryCapabilities();
    bool ConfigureFormat();
    bool ConfigureFrameRate();
    bool InitializeMemoryMapping();
    bool QueueCaptureBuffers();
    bool StartStreaming();
    void StopStreaming();
    void ReleaseMappedBuffers();

private:
    config::CaptureConfig capture_config_;
    config::CodecConfig codec_config_;
    std::unique_ptr<modules::encoding::VideoEncoder> encoder_;
    int device_fd_;
    bool opened_;
    bool streaming_;
    bool logged_timestamp_source_;
    std::vector<MappedBuffer> buffers_;
    std::uint64_t sequence_;
};

}  // namespace v4l2
}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_V4L2_V4L2CAPTUREDEVICE_H
