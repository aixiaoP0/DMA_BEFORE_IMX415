#include "modules/transport/TransportModule.h"

#include <utility>

#include "common/log/Logger.h"

namespace sserver {
namespace modules {
namespace transport {

TransportModule::TransportModule(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder)
        : transport_(std::make_unique<Transport>(send_latency_recorder)) {
}

TransportModule::~TransportModule() {
    shutdown();
}

std::string TransportModule::name() const {
    return "TransportModule";
}

bool TransportModule::initialize(const core::ApplicationContext &context) {
    std::string error_message;
    if (!transport_->initialize(context, &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to initialize transport: " + error_message);
        }
        return false;
    }
    return true;
}

bool TransportModule::start() {
    std::string error_message;
    if (!transport_->start(&error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to start transport: " + error_message);
        }
        return false;
    }
    return true;
}

void TransportModule::stop() {
    transport_->stop();
}

void TransportModule::shutdown() {
    transport_->shutdown();
}

core::ModuleState TransportModule::state() const {
    return transport_->state();
}

void TransportModule::Broadcast(common::model::EncodedFramePtr frame) {
    transport_->Broadcast(frame);
}

int TransportModule::bound_port() const {
    return transport_->bound_port();
}

std::string TransportModule::backend_name() const {
    return transport_->backend_name();
}

}  // namespace transport
}  // namespace modules
}  // namespace sserver
