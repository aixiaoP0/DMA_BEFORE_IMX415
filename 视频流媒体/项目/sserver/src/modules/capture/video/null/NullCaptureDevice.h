#ifndef SSERVER_MODULES_CAPTURE_VIDEO_NULL_NULLCAPTUREDEVICE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_NULL_NULLCAPTUREDEVICE_H

#include <cstdint>
#include <memory>
#include <vector>

#include "config/AppConfig.h"
#include "modules/capture/video/ICaptureDevice.h"
#include "modules/encoding/video/VideoEncoder.h"

namespace sserver {
namespace modules {
namespace capture {

class NullCaptureDevice : public ICaptureDevice {
public:
    NullCaptureDevice(const config::CaptureConfig &config, const config::CodecConfig &codec_config);

    bool Open() override;
    bool Start() override;
    common::model::EncodedFramePtr CaptureFrame() override;
    void Stop() override;
    void Close() override;
    std::string Describe() const override;

private:
    void FillSyntheticYuyvFrame();

    config::CaptureConfig config_;
    config::CodecConfig codec_config_;
    bool opened_;
    bool streaming_;
    std::uint64_t sequence_;
    std::unique_ptr<modules::encoding::VideoEncoder> encoder_;
    std::vector<std::uint8_t> synthetic_yuyv_frame_;
};

}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_NULL_NULLCAPTUREDEVICE_H
