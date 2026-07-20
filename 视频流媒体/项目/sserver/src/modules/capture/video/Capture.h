#ifndef SSERVER_MODULES_CAPTURE_VIDEO_CAPTURE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_CAPTURE_H

#include <memory>
#include <string>

#include "common/model/EncodedFrame.h"
#include "config/AppConfig.h"
#include "core/ApplicationContext.h"
#include "core/ModuleState.h"
#include "modules/capture/video/ICaptureDevice.h"

namespace sserver {
namespace modules {
namespace capture {

enum class CaptureBackend {
    kAuto,
    kNull,
    kV4L2,
};

struct CaptureBackendSelection {
    CaptureBackend backend = CaptureBackend::kAuto;
    std::string backend_name = "auto";
};

inline bool ResolveCaptureBackendSelection(
        const config::CaptureConfig &config,
        CaptureBackendSelection *selection,
        std::string *error_message) {
    if (selection == nullptr) {
        if (error_message != nullptr) {
            *error_message = "capture backend selection output is null";
        }
        return false;
    }

    if (config.source == "null") {
        selection->backend = CaptureBackend::kNull;
        selection->backend_name = "null";
        return true;
    }

    if (config.source == "v4l2") {
        selection->backend = CaptureBackend::kV4L2;
        selection->backend_name = "v4l2";
        return true;
    }

    if (error_message != nullptr) {
        *error_message = "capture.source must be either 'v4l2' or 'null'";
    }
    return false;
}

class CaptureBackendFactory {
public:
    static std::unique_ptr<ICaptureDevice> Create(
            const CaptureBackendSelection &selection,
            const config::CaptureConfig &capture_config,
            const config::CodecConfig &codec_config,
            std::string *error_message);
};

class Capture {
public:
    Capture();
    ~Capture();

    bool initialize(const core::ApplicationContext &context, std::string *error_message);
    bool start(std::string *error_message);
    void stop();
    void shutdown();
    core::ModuleState state() const;

    common::model::EncodedFramePtr CaptureFrame();
    CaptureBackend backend() const;
    const std::string &backend_name() const;
    std::string Describe() const;

    bool SupportsRawCapture() const;
    RawCaptureFramePtr CaptureRawFrame();
    common::model::EncodedFramePtr EncodeRawFrame(RawCaptureFramePtr raw);

private:
    struct CaptureImpl;
    std::unique_ptr<CaptureImpl> impl_;
};

}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_CAPTURE_H
