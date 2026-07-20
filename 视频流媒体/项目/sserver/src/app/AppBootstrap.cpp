#include "app/AppBootstrap.h"

#include <sstream>

#include "common/log/Logger.h"
#include "modules/capture/video/CaptureModule.h"
#include "modules/transport/TransportModule.h"

namespace sserver {
namespace app {

AppBootstrap::AppBootstrap(const config::AppConfig &config)
        : config_(config),
          application_(new core::Application(config_)),
          send_latency_recorder_(std::make_shared<common::metrics::LatencyRecorder>()),
          started_(false) {
}

AppBootstrap::~AppBootstrap() {
    Stop();
}

bool AppBootstrap::Initialize() {
    if (capture_module_ != nullptr) {
        return true;
    }

    transport_module_ = std::make_shared<modules::transport::TransportModule>(send_latency_recorder_);
    capture_module_ = std::make_shared<modules::capture::CaptureModule>();

    application_->RegisterModule(transport_module_);
    application_->RegisterModule(capture_module_);

    return application_->Initialize();
}

bool AppBootstrap::Start() {
    if (!Initialize()) {
        return false;
    }
    if (started_) {
        return true;
    }
    BindStreamingPipeline();
    started_ = application_->Start();
    if (started_) {
        PrintStartupSummary();
    } else {
        UnbindStreamingPipeline();
    }
    return started_;
}

void AppBootstrap::Stop() {
    if (application_ == nullptr) {
        return;
    }

    const bool was_started = started_;
    UnbindStreamingPipeline();
    application_->Stop();
    application_->Shutdown();
    if (was_started) {
        PrintShutdownSummary();
    }
    started_ = false;
}

core::Application &AppBootstrap::application() {
    return *application_;
}

std::shared_ptr<common::metrics::LatencyRecorder> AppBootstrap::send_latency_recorder() const {
    return send_latency_recorder_;
}

int AppBootstrap::bound_port() const {
    if (transport_module_ == nullptr) {
        return 0;
    }
    return transport_module_->bound_port();
}

void AppBootstrap::BindStreamingPipeline() {
    if (capture_module_ == nullptr || transport_module_ == nullptr) {
        return;
    }

    capture_module_->SetFrameHandler(
            [transport_module = transport_module_](common::model::EncodedFramePtr frame) {
                transport_module->Broadcast(frame);
            });
}

void AppBootstrap::UnbindStreamingPipeline() {
    if (capture_module_ == nullptr) {
        return;
    }
    capture_module_->SetFrameHandler(modules::capture::FrameHandler());
}

void AppBootstrap::PrintStartupSummary() const {
    std::ostringstream stream;
    stream << "application started: app=" << config_.app_name
           << ", capture_source=" << config_.capture.source
           << ", transport_backend=" << config_.transport.backend
           << ", listen_port=" << config_.transport.listen_port;
    common::log::Logger::Info(stream.str());
}

void AppBootstrap::PrintShutdownSummary() const {
    common::log::Logger::Info("application stopped");
}

}  // namespace app
}  // namespace sserver
