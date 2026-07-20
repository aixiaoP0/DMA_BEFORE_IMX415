#include "modules/transport/Transport.h"

#include <utility>

#include "modules/transport/ITransportBackend.h"
#include "modules/transport/rtp/RtpStreamingBackend.h"
#include "modules/transport/tcp/TcpStreamingBackend.h"
#include "modules/transport/udp/UdpStreamingBackend.h"

namespace sserver {
namespace modules {
namespace transport {

std::unique_ptr<ITransportBackend> TransportBackendFactory::Create(
        const TransportBackendSelection &selection,
        const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder,
        std::string *error_message) {
    switch (selection.backend) {
        case TransportBackend::kTcp:
            return std::unique_ptr<ITransportBackend>(new tcp::TcpStreamingBackend(send_latency_recorder));
        case TransportBackend::kUdp:
            return std::unique_ptr<ITransportBackend>(new udp::UdpStreamingBackend(send_latency_recorder));
        case TransportBackend::kRtp:
            return std::unique_ptr<ITransportBackend>(new rtp::RtpStreamingBackend(send_latency_recorder));
        case TransportBackend::kAuto:
        default:
            if (error_message != nullptr) {
                *error_message = "requested transport backend is not supported";
            }
            return nullptr;
    }
}

struct Transport::TransportImpl {
    TransportBackend requested_backend = TransportBackend::kAuto;
    std::string requested_backend_name = "auto";
    TransportBackend active_backend = TransportBackend::kTcp;
    std::string active_backend_name = "tcp";
    std::unique_ptr<ITransportBackend> backend;
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder;
    core::ModuleState state = core::ModuleState::kCreated;
};

Transport::Transport(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder)
        : impl_(std::make_unique<TransportImpl>()) {
    impl_->send_latency_recorder = send_latency_recorder;
}

Transport::~Transport() {
    shutdown();
}

bool Transport::initialize(const core::ApplicationContext &context, std::string *error_message) {
    shutdown();

    TransportBackendSelection selection;
    if (!ResolveTransportBackendSelection(context.config.transport, &selection, error_message)) {
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    impl_->requested_backend = selection.backend;
    impl_->requested_backend_name = selection.backend_name;
    impl_->backend = TransportBackendFactory::Create(selection, impl_->send_latency_recorder, error_message);
    if (!impl_->backend) {
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    if (!impl_->backend->initialize(context)) {
        if (error_message != nullptr && error_message->empty()) {
            *error_message = "transport backend failed to initialize";
        }
        impl_->backend.reset();
        impl_->state = core::ModuleState::kFailed;
        return false;
    }

    impl_->active_backend = impl_->backend->backend();
    impl_->active_backend_name = impl_->backend->backend_name();
    impl_->state = impl_->backend->state();
    return true;
}

bool Transport::start(std::string *error_message) {
    if (!impl_->backend) {
        impl_->state = core::ModuleState::kFailed;
        if (error_message != nullptr) {
            *error_message = "transport is not initialized";
        }
        return false;
    }

    if (!impl_->backend->start()) {
        if (error_message != nullptr && error_message->empty()) {
            *error_message = "transport backend failed to start";
        }
        impl_->state = impl_->backend->state();
        return false;
    }

    impl_->state = impl_->backend->state();
    return true;
}

void Transport::stop() {
    if (impl_->backend) {
        impl_->backend->stop();
        impl_->state = impl_->backend->state();
        return;
    }
    impl_->state = core::ModuleState::kStopped;
}

void Transport::shutdown() {
    if (impl_->backend) {
        impl_->backend->shutdown();
        impl_->backend.reset();
    }

    impl_->requested_backend = TransportBackend::kAuto;
    impl_->requested_backend_name = "auto";
    impl_->active_backend = TransportBackend::kTcp;
    impl_->active_backend_name = "tcp";
    impl_->state = core::ModuleState::kShutdown;
}

core::ModuleState Transport::state() const {
    if (impl_->backend) {
        return impl_->backend->state();
    }
    return impl_->state;
}

void Transport::Broadcast(common::model::EncodedFramePtr frame) {
    if (impl_->backend) {
        impl_->backend->Broadcast(frame);
    }
}

int Transport::bound_port() const {
    if (!impl_->backend) {
        return 0;
    }
    return impl_->backend->bound_port();
}

TransportBackend Transport::backend() const {
    return impl_->active_backend;
}

const std::string &Transport::backend_name() const {
    return impl_->active_backend_name;
}

}  // namespace transport
}  // namespace modules
}  // namespace sserver
