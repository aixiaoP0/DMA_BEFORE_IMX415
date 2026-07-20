#include "modules/encoding/video/VideoEncoder.h"

#include <utility>

#include "modules/encoding/video/VideoEncoderBackend.h"
#include "modules/encoding/video/x264/X264VideoEncoderBackend.h"

namespace sserver {
namespace modules {
namespace encoding {

struct VideoEncoder::VideoEncoderImpl {
    EncodeBackend requested_backend = EncodeBackend::kAuto;
    std::string requested_backend_name = "auto";
    EncodeBackend active_backend = EncodeBackend::kX264;
    std::string active_backend_name = "x264";
    std::unique_ptr<VideoEncoderBackend> backend;
};

std::unique_ptr<VideoEncoderBackend> VideoEncoderBackendFactory::Create(
        const VideoEncoderBackendSelection &selection,
        std::string *error_message) {
    switch (selection.backend) {
        case EncodeBackend::kX264:
            return CreateX264VideoEncoderBackend();
        case EncodeBackend::kAuto:
        default:
            if (error_message != nullptr) {
                *error_message = "requested encode backend is not supported";
            }
            return nullptr;
    }
}

VideoEncoder::VideoEncoder()
        : impl_(std::make_unique<VideoEncoderImpl>()) {
}

VideoEncoder::~VideoEncoder() {
    Shutdown();
}

bool VideoEncoder::Initialize(int width, int height, int fps, const config::CodecConfig &config) {
    return Initialize(width, height, fps, config, nullptr);
}

bool VideoEncoder::Initialize(
        int width,
        int height,
        int fps,
        const config::CodecConfig &config,
        std::string *error_message) {
    Shutdown();

    VideoEncoderBackendSelection selection;
    if (!ResolveVideoEncoderBackendSelection(config, &selection, error_message)) {
        return false;
    }

    impl_->requested_backend = selection.backend;
    impl_->requested_backend_name = selection.backend_name;
    impl_->backend = VideoEncoderBackendFactory::Create(selection, error_message);
    if (!impl_->backend) {
        return false;
    }

    if (!impl_->backend->Initialize(width, height, fps, config, error_message)) {
        impl_->backend.reset();
        return false;
    }

    impl_->active_backend = impl_->backend->backend();
    impl_->active_backend_name = impl_->backend->backend_name();
    return true;
}

bool VideoEncoder::EncodeYuyv422Frame(
        const std::uint8_t *input,
        std::size_t input_length,
        std::vector<std::uint8_t> *output,
        bool *is_keyframe) {
    return EncodeYuyv422Frame(input, input_length, output, is_keyframe, nullptr);
}

bool VideoEncoder::EncodeYuyv422Frame(
        const std::uint8_t *input,
        std::size_t input_length,
        std::vector<std::uint8_t> *output,
        bool *is_keyframe,
        std::string *error_message) {
    if (!impl_->backend) {
        if (error_message != nullptr) {
            *error_message = "video encoder is not initialized";
        }
        return false;
    }

    return impl_->backend->EncodeYuyv422Frame(input, input_length, output, is_keyframe, error_message);
}

void VideoEncoder::Shutdown() {
    if (impl_->backend) {
        impl_->backend->Shutdown();
        impl_->backend.reset();
    }

    impl_->requested_backend = EncodeBackend::kAuto;
    impl_->requested_backend_name = "auto";
    impl_->active_backend = EncodeBackend::kX264;
    impl_->active_backend_name = "x264";
}

EncodeBackend VideoEncoder::backend() const {
    return impl_->active_backend;
}

const std::string &VideoEncoder::backend_name() const {
    return impl_->active_backend_name;
}

std::unique_ptr<IVideoEncoder> CodecFactory::Create(const config::CodecConfig &config) {
    VideoEncoderBackendSelection selection;
    if (!ResolveVideoEncoderBackendSelection(config, &selection, nullptr)) {
        return std::unique_ptr<IVideoEncoder>();
    }
    return std::unique_ptr<IVideoEncoder>(new VideoEncoder());
}

}  // namespace encoding
}  // namespace modules
}  // namespace sserver
