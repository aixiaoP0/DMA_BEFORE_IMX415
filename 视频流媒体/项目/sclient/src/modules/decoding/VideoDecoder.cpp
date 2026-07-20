#include "VideoDecoder.h"

#include <utility>

#include "modules/decoding/VideoDecoderBackend.h"

namespace sclient {

std::unique_ptr<VideoDecoderBackend> CreateSoftwareVideoDecoderBackend();

namespace {

std::unique_ptr<VideoDecoderBackend> CreateVideoDecoderBackend(
        DecodeBackend requested_backend,
        std::string *error_message) {
    switch (requested_backend) {
        case DecodeBackend::kAuto:
        case DecodeBackend::kSoftware:
            return CreateSoftwareVideoDecoderBackend();
        default:
            if (error_message != nullptr) {
                *error_message = "requested decode backend is not supported";
            }
            return nullptr;
    }
}

}  // namespace

struct VideoDecoderImpl {
    DecodeBackend requested_backend = DecodeBackend::kAuto;
    DecodeBackend active_backend = DecodeBackend::kSoftware;
    std::string active_backend_name = "software";
    std::unique_ptr<VideoDecoderBackend> backend;
};

VideoDecoder::VideoDecoder()
        : impl_(std::make_unique<VideoDecoderImpl>()) {
}

VideoDecoder::~VideoDecoder() {
    Shutdown();
}

bool VideoDecoder::Initialize(
        std::string *error_message) {
    return Initialize(DecodeBackend::kAuto, error_message);
}

bool VideoDecoder::Initialize(
        DecodeBackend requested_backend,
        std::string *error_message) {
    Shutdown();

    impl_->requested_backend = requested_backend;
    impl_->backend = CreateVideoDecoderBackend(requested_backend, error_message);
    if (!impl_->backend) {
        return false;
    }

    if (!impl_->backend->Initialize(error_message)) {
        impl_->backend.reset();
        return false;
    }

    impl_->active_backend = DecodeBackend::kSoftware;
    impl_->active_backend_name = impl_->backend->backend_name();
    return true;
}

bool VideoDecoder::Decode(
        const std::uint8_t *data,
        std::size_t size,
        DecodedFrame *decoded_frame,
        std::string *error_message) {
    if (!impl_->backend) {
        if (error_message != nullptr) {
            *error_message = "decoder is not initialized";
        }
        return false;
    }
    return impl_->backend->Decode(data, size, decoded_frame, error_message);
}

void VideoDecoder::Shutdown() {
    if (impl_->backend) {
        impl_->backend->Shutdown();
        impl_->backend.reset();
    }

    impl_->requested_backend = DecodeBackend::kAuto;
    impl_->active_backend = DecodeBackend::kSoftware;
    impl_->active_backend_name = "software";
}

}  // namespace sclient
