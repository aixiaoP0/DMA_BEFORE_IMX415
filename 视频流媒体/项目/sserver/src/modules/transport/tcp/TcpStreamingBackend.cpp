#include "modules/transport/tcp/TcpStreamingBackend.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>

#include "common/log/Logger.h"

namespace sserver {
namespace modules {
namespace transport {
namespace tcp {

TcpStreamingBackend::TcpStreamingBackend(const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder)
        : backend_name_("tcp"),
          latency_log_interval_frames_(120),
          listen_socket_fd_(-1),
          bound_port_(0),
          running_(false),
          send_latency_recorder_(send_latency_recorder),
          state_(core::ModuleState::kCreated) {
}

TcpStreamingBackend::~TcpStreamingBackend() {
    shutdown();
}

bool TcpStreamingBackend::initialize(const core::ApplicationContext &context) {
    config_ = context.config.transport;
    latency_log_interval_frames_ = context.config.runtime.latency_log_interval_frames;
    state_ = core::ModuleState::kInitialized;
    return true;
}

bool TcpStreamingBackend::start() {
    if (!config_.enabled) {
        state_ = core::ModuleState::kRunning;
        return true;
    }

    if (!OpenListenSocket()) {
        state_ = core::ModuleState::kFailed;
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread(&TcpStreamingBackend::AcceptLoop, this);
    state_ = core::ModuleState::kRunning;
    return true;
}

void TcpStreamingBackend::stop() {
    if (state_.load() != core::ModuleState::kRunning) {
        return;
    }

    running_ = false;
    CloseListenSocket();

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    std::vector<std::shared_ptr<TcpClientSession>> clients_copy;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_copy.swap(clients_);
    }
    for (std::size_t index = 0; index < clients_copy.size(); ++index) {
        clients_copy[index]->Stop();
    }

    state_ = core::ModuleState::kStopped;
}

void TcpStreamingBackend::shutdown() {
    stop();
    state_ = core::ModuleState::kShutdown;
}

core::ModuleState TcpStreamingBackend::state() const {
    return state_.load();
}

void TcpStreamingBackend::Broadcast(common::model::EncodedFramePtr frame) {
    if (!config_.enabled || !frame) {
        return;
    }

    std::vector<std::shared_ptr<TcpClientSession>> snapshot;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        snapshot = clients_;
    }

    for (std::size_t index = 0; index < snapshot.size(); ++index) {
        if (snapshot[index]->IsRunning()) {
            snapshot[index]->EnqueueFrame(frame);
        }
    }

    PruneClosedClients();
}

int TcpStreamingBackend::bound_port() const {
    return bound_port_;
}

TransportBackend TcpStreamingBackend::backend() const {
    return TransportBackend::kTcp;
}

const std::string &TcpStreamingBackend::backend_name() const {
    return backend_name_;
}

bool TcpStreamingBackend::OpenListenSocket() {
    listen_socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket_fd_ < 0) {
        common::log::Logger::Error("failed to create tcp listen socket");
        return false;
    }

    int reuse = 1;
    setsockopt(listen_socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    const int current_flags = fcntl(listen_socket_fd_, F_GETFL, 0);
    if (current_flags >= 0) {
        fcntl(listen_socket_fd_, F_SETFL, current_flags | O_NONBLOCK);
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(config_.listen_port));
    address.sin_addr.s_addr = inet_addr(config_.bind_address.c_str());

    if (bind(listen_socket_fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        common::log::Logger::Error("failed to bind tcp transport server");
        CloseListenSocket();
        return false;
    }

    if (listen(listen_socket_fd_, 16) < 0) {
        common::log::Logger::Error("failed to listen on tcp transport socket");
        CloseListenSocket();
        return false;
    }

    if (config_.listen_port == 0) {
        sockaddr_in bound_address{};
        socklen_t bound_length = sizeof(bound_address);
        if (getsockname(listen_socket_fd_, reinterpret_cast<sockaddr *>(&bound_address), &bound_length) == 0) {
            bound_port_ = ntohs(bound_address.sin_port);
            common::log::Logger::Info("tcp transport bound to ephemeral port " + std::to_string(bound_port_));
        }
    } else {
        bound_port_ = config_.listen_port;
    }

    return true;
}

void TcpStreamingBackend::CloseListenSocket() {
    if (listen_socket_fd_ >= 0) {
        close(listen_socket_fd_);
        listen_socket_fd_ = -1;
    }
}

void TcpStreamingBackend::AcceptLoop() {
    while (running_.load()) {
        sockaddr_in client_address{};
        socklen_t address_length = sizeof(client_address);
        const int client_fd = accept(listen_socket_fd_, reinterpret_cast<sockaddr *>(&client_address), &address_length);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(config_.accept_loop_interval_ms));
                continue;
            }
            common::log::Logger::Warn("accept failed on tcp transport socket");
            break;
        }

        std::shared_ptr<TcpClientSession> session(
                new TcpClientSession(client_fd, config_, send_latency_recorder_, latency_log_interval_frames_));
        if (!session->Start()) {
            session->Stop();
            continue;
        }

        common::log::Logger::Info("tcp client connected: " + session->remote_endpoint());
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_.push_back(session);
        }
        PruneClosedClients();
    }
}

void TcpStreamingBackend::PruneClosedClients() {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_.erase(
            std::remove_if(
                    clients_.begin(),
                    clients_.end(),
                    [](const std::shared_ptr<TcpClientSession> &client) {
                        return client == nullptr || !client->IsRunning();
                    }),
            clients_.end());
}

}  // namespace tcp
}  // namespace transport
}  // namespace modules
}  // namespace sserver
