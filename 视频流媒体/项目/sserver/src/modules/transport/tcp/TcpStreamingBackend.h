#ifndef SSERVER_MODULES_TRANSPORT_TCP_TCPSTREAMINGBACKEND_H
#define SSERVER_MODULES_TRANSPORT_TCP_TCPSTREAMINGBACKEND_H

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/metrics/LatencyRecorder.h"
#include "config/AppConfig.h"
#include "modules/transport/ITransportBackend.h"
#include "modules/transport/tcp/TcpClientSession.h"

namespace sserver {
namespace modules {
namespace transport {
namespace tcp {

class TcpStreamingBackend : public ITransportBackend {
public:
    explicit TcpStreamingBackend(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder);
    ~TcpStreamingBackend() override;

    bool initialize(const core::ApplicationContext &context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;
    core::ModuleState state() const override;
    void Broadcast(common::model::EncodedFramePtr frame) override;
    int bound_port() const override;
    TransportBackend backend() const override;
    const std::string &backend_name() const override;

private:
    bool OpenListenSocket();
    void CloseListenSocket();
    void AcceptLoop();
    void PruneClosedClients();

private:
    config::TransportConfig config_;
    std::string backend_name_;
    int latency_log_interval_frames_;
    int listen_socket_fd_;
    int bound_port_;
    std::atomic_bool running_;
    std::thread accept_thread_;
    std::mutex clients_mutex_;
    std::vector<std::shared_ptr<TcpClientSession>> clients_;
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder_;
    std::atomic<core::ModuleState> state_;
};

}  // namespace tcp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_TCP_TCPSTREAMINGBACKEND_H
