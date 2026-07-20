#ifndef SSERVER_MODULES_TRANSPORT_TRANSPORT_H
#define SSERVER_MODULES_TRANSPORT_TRANSPORT_H

#include <memory>
#include <string>

#include "common/metrics/LatencyRecorder.h"
#include "common/model/EncodedFrame.h"
#include "config/AppConfig.h"
#include "core/ApplicationContext.h"
#include "core/ModuleState.h"

namespace sserver {
namespace modules {
namespace transport {

class ITransportBackend;

enum class TransportBackend {
    kAuto,
    kTcp,
    kUdp,
    kRtp,
};

struct TransportBackendSelection {
    TransportBackend backend = TransportBackend::kAuto;
    std::string backend_name = "auto";
};

inline bool ResolveTransportBackendSelection(
        const config::TransportConfig &config,
        TransportBackendSelection *selection,
        std::string *error_message) {
    if (selection == nullptr) {
        if (error_message != nullptr) {
            *error_message = "transport backend selection output is null";
        }
        return false;
    }

    if (config.backend == "tcp") {
        selection->backend = TransportBackend::kTcp;
        selection->backend_name = "tcp";
        return true;
    }

    if (config.backend == "udp") {
        selection->backend = TransportBackend::kUdp;
        selection->backend_name = "udp";
        return true;
    }

    if (config.backend == "rtp") {
        selection->backend = TransportBackend::kRtp;
        selection->backend_name = "rtp";
        return true;
    }

    if (error_message != nullptr) {
        *error_message = "transport.backend must be one of 'tcp', 'udp' or 'rtp'";
    }
    return false;
}

class TransportBackendFactory {
public:
    static std::unique_ptr<ITransportBackend> Create(
            const TransportBackendSelection &selection,
            const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder,
            std::string *error_message);
};

class Transport {
public:
    explicit Transport(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder);
    ~Transport();

    bool initialize(const core::ApplicationContext &context, std::string *error_message);
    bool start(std::string *error_message);
    void stop();
    void shutdown();
    core::ModuleState state() const;

    void Broadcast(common::model::EncodedFramePtr frame);
    int bound_port() const;
    TransportBackend backend() const;
    const std::string &backend_name() const;

private:
    struct TransportImpl;
    std::unique_ptr<TransportImpl> impl_;
};

}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_TRANSPORT_H
