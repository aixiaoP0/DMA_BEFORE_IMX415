#include "modules/rendering/VideoRenderer.h"

#include <utility>

#include "modules/rendering/VideoRendererBackend.h"
#include "modules/rendering/opengl/OpenGlVideoRendererBackend.h"

namespace sclient {

namespace {

std::unique_ptr<VideoRendererBackend> CreateVideoRendererBackend(
        RenderBackend requested_backend,
        std::string *error_message) {
    switch (requested_backend) {
        case RenderBackend::kAuto:
        case RenderBackend::kOpenGl:
            return CreateOpenGlVideoRendererBackend();
        default:
            if (error_message != nullptr) {
                *error_message = "requested renderer backend is not supported";
            }
            return nullptr;
    }
}

}  // namespace

struct VideoRendererImpl {
    RenderBackend requested_backend = RenderBackend::kAuto;
    RenderBackend active_backend = RenderBackend::kOpenGl;
    std::string active_backend_name = "opengl";
    std::unique_ptr<VideoRendererBackend> backend;
};

VideoRenderer::VideoRenderer()
        : impl_(std::make_unique<VideoRendererImpl>()) {
}

VideoRenderer::~VideoRenderer() {
    Shutdown();
}

bool VideoRenderer::Initialize(
        const std::string &window_title,
        RenderBackend requested_backend,
        bool enable_vsync,
        std::string *error_message,
        std::string *info_message) {
    Shutdown();

    impl_->requested_backend = requested_backend;
    impl_->backend = CreateVideoRendererBackend(requested_backend, error_message);
    if (!impl_->backend) {
        return false;
    }

    if (!impl_->backend->Initialize(window_title, enable_vsync, error_message)) {
        impl_->backend.reset();
        return false;
    }

    impl_->active_backend = impl_->backend->backend();
    impl_->active_backend_name = impl_->backend->backend_name();

    if (info_message != nullptr) {
        *info_message = std::string("renderer=") +
                (impl_->active_backend_name == "opengl" ? "opengl(glfw+glad+imgui)" : impl_->active_backend_name) +
                " vsync=" + (enable_vsync ? "on" : "off");
    }
    return true;
}

int VideoRenderer::PollKey(int delay_ms) const {
    if (!impl_->backend) {
        return 27;
    }
    return impl_->backend->PollKey(delay_ms);
}

RenderBackend VideoRenderer::backend() const {
    return impl_->active_backend;
}

const std::string &VideoRenderer::backend_name() const {
    return impl_->active_backend_name;
}

bool VideoRenderer::SupportsNativeFrame(const DecodedFrame &frame) const {
    return impl_->backend != nullptr && impl_->backend->SupportsNativeFrame(frame);
}

bool VideoRenderer::Render(const DecodedFrame &frame, const RenderFrameInfo &frame_info, std::string *error_message) {
    if (!impl_->backend) {
        if (error_message != nullptr) {
            *error_message = "renderer is not initialized";
        }
        return false;
    }
    return impl_->backend->Render(frame, frame_info, error_message);
}

void VideoRenderer::UpdateWindowTitle(const std::string &title) {
    if (impl_->backend) {
        impl_->backend->UpdateWindowTitle(title);
    }
}

void VideoRenderer::ToggleFullscreen() {
    if (impl_->backend) {
        impl_->backend->ToggleFullscreen();
    }
}

bool VideoRenderer::SaveScreenshot(const std::string &path, std::string *error_message) {
    if (!impl_->backend) {
        if (error_message != nullptr) {
            *error_message = "renderer is not initialized";
        }
        return false;
    }
    return impl_->backend->SaveScreenshot(path, error_message);
}

void VideoRenderer::Shutdown() {
    if (impl_->backend) {
        impl_->backend->Shutdown();
        impl_->backend.reset();
    }

    impl_->requested_backend = RenderBackend::kAuto;
    impl_->active_backend = RenderBackend::kOpenGl;
    impl_->active_backend_name = "opengl";
}

}  // namespace sclient
