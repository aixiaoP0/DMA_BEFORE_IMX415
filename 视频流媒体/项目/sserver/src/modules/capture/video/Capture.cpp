#include "modules/capture/video/Capture.h"

#include <utility>

#include "common/log/Logger.h"
#include "modules/capture/video/ICaptureDevice.h"
#include "modules/capture/video/null/NullCaptureDevice.h"
#include "modules/capture/video/v4l2/V4L2CaptureDevice.h"

namespace sserver {
namespace modules {
namespace capture {

struct Capture::CaptureImpl {
    config::CaptureConfig capture_config;
    config::CodecConfig codec_config;
    CaptureBackend requested_backend = CaptureBackend::kAuto;
    std::string requested_backend_name = "auto";
    CaptureBackend active_backend = CaptureBackend::kNull;
    std::string active_backend_name = "null";
    std::unique_ptr<ICaptureDevice> device;
    core::ModuleState state = core::ModuleState::kCreated;
};

std::unique_ptr<ICaptureDevice> CaptureBackendFactory::Create(
        const CaptureBackendSelection &selection,
        const config::CaptureConfig &capture_config,
        const config::CodecConfig &codec_config,
        std::string *error_message) {
    switch (selection.backend) {
        case CaptureBackend::kNull:
            return std::unique_ptr<ICaptureDevice>(new NullCaptureDevice(capture_config, codec_config));
        case CaptureBackend::kV4L2:
            return std::unique_ptr<ICaptureDevice>(new v4l2::V4L2CaptureDevice(capture_config, codec_config));
        case CaptureBackend::kAuto:
        default:
            if (error_message != nullptr) {
                *error_message = "requested capture backend is not supported";
            }
            return nullptr;
    }
}

Capture::Capture()
        : impl_(std::make_unique<CaptureImpl>()) {
}

Capture::~Capture() {
    shutdown();
}

bool Capture::initialize(const core::ApplicationContext &context, std::string *error_message) {
    shutdown();

    CaptureBackendSelection selection;
    if (!ResolveCaptureBackendSelection(context.config.capture, &selection, error_message)) {
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    impl_->capture_config = context.config.capture;
    impl_->codec_config = context.config.codec;
    impl_->requested_backend = selection.backend;
    impl_->requested_backend_name = selection.backend_name;
    impl_->device = CaptureBackendFactory::Create(selection, context.config.capture, context.config.codec, error_message);
    if (!impl_->device) {
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    impl_->active_backend = selection.backend;
    impl_->active_backend_name = selection.backend_name;
    impl_->state = core::ModuleState::kInitialized;
    return true;
}

bool Capture::start(std::string *error_message) {
    if (!impl_->capture_config.enabled) {
        impl_->state = core::ModuleState::kRunning;
        return true;
    }

    if (!impl_->device) {
        impl_->state = core::ModuleState::kFailed;
        if (error_message != nullptr) {
            *error_message = "capture is not initialized";
        }
        return false;
    }

    if (!impl_->device->Open()) {
        impl_->state = core::ModuleState::kFailed;
        if (error_message != nullptr && error_message->empty()) {
            *error_message = "failed to open capture device";
        }
        return false;
    }

    if (!impl_->device->Start()) {
        impl_->device->Close();
        impl_->state = core::ModuleState::kFailed;
        if (error_message != nullptr && error_message->empty()) {
            *error_message = "failed to start capture device";
        }
        return false;
    }

    impl_->state = core::ModuleState::kRunning;
    return true;
}

void Capture::stop() {
    if (!impl_->device) {
        impl_->state = core::ModuleState::kStopped;
        return;
    }

    impl_->device->Stop();
    impl_->device->Close();
    impl_->state = core::ModuleState::kStopped;
}

void Capture::shutdown() {
    if (impl_->device) {
        impl_->device->Stop();
        impl_->device->Close();
        impl_->device.reset();
    }

    impl_->requested_backend = CaptureBackend::kAuto;
    impl_->requested_backend_name = "auto";
    impl_->active_backend = CaptureBackend::kNull;
    impl_->active_backend_name = "null";
    impl_->state = core::ModuleState::kShutdown;
}

core::ModuleState Capture::state() const {
    return impl_->state;
}

common::model::EncodedFramePtr Capture::CaptureFrame() {
    if (!impl_->device) {
        return common::model::EncodedFramePtr();
    }
    return impl_->device->CaptureFrame();
}

CaptureBackend Capture::backend() const {
    return impl_->active_backend;
}

const std::string &Capture::backend_name() const {
    return impl_->active_backend_name;
}

std::string Capture::Describe() const {
    if (!impl_->device) {
        return "capture(uninitialized)";
    }
    return impl_->device->Describe();
}

bool Capture::SupportsRawCapture() const {
    return impl_->device && impl_->device->SupportsRawCapture();
}

RawCaptureFramePtr Capture::CaptureRawFrame() {
    if (!impl_->device) {
        return nullptr;
    }
    return impl_->device->CaptureRawFrame();
}

common::model::EncodedFramePtr Capture::EncodeRawFrame(RawCaptureFramePtr raw) {
    if (!impl_->device) {
        return nullptr;
    }
    return impl_->device->EncodeRawFrame(raw);
}

}  // namespace capture
}  // namespace modules
}  // namespace sserver
