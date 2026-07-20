#ifndef SSERVER_MODULES_TRANSPORT_TCP_TCPCLIENTSESSION_H
#define SSERVER_MODULES_TRANSPORT_TCP_TCPCLIENTSESSION_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/concurrency/ThreadSafeQueue.h"
#include "common/metrics/LatencyRecorder.h"
#include "common/model/EncodedFrame.h"
#include "config/AppConfig.h"

namespace sserver {
namespace modules {
namespace transport {
namespace tcp {

class TcpClientSession {
public:
    TcpClientSession(
            int socket_fd,
            const config::TransportConfig &config,
            const std::shared_ptr<common::metrics::LatencyRecorder> &send_latency_recorder,
            int latency_log_interval_frames);
    ~TcpClientSession();

    bool Start();
    void Stop();
    bool IsRunning() const;
    void EnqueueFrame(common::model::EncodedFramePtr frame);
    std::string remote_endpoint() const;

private:
    struct QueuedFrame {
        common::model::EncodedFramePtr frame;
        std::uint64_t enqueue_timestamp_ns = 0;
    };

    void ReceiveLoop();
    void SendLoop();
    bool ReceiveAll(char *buffer, std::size_t length);
    bool SendFrame(common::model::EncodedFramePtr frame, std::uint64_t send_start_timestamp_ns);
    bool SendMessageParts(const char *header, std::size_t header_length,
                          const char *metadata, std::size_t metadata_length,
                          const char *payload, std::size_t payload_length);
    void CloseSocket();
    std::string BuildRemoteEndpoint() const;

private:
    int socket_fd_;
    config::TransportConfig config_;
    std::atomic_bool running_;
    std::thread receive_thread_;
    std::thread send_thread_;
    std::mutex receive_mutex_;
    std::mutex send_mutex_;
    common::concurrency::ThreadSafeQueue<QueuedFrame> outbound_frames_;
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder_;
    common::metrics::LatencyRecorder queue_wait_latency_recorder_;
    common::metrics::LatencyRecorder send_time_latency_recorder_;
    int latency_log_interval_frames_;
    std::uint64_t sent_frames_;
    std::uint64_t overflow_dropped_frames_;
    std::uint64_t stale_dropped_frames_;
    std::uint64_t dropped_incoming_non_keyframes_;
    std::uint64_t backpressure_events_;
    std::size_t max_queue_depth_;
    std::string remote_endpoint_;
};

}  // namespace tcp
}  // namespace transport
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_TRANSPORT_TCP_TCPCLIENTSESSION_H
