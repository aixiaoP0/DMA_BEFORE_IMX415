#ifndef SSERVER_APP_APPBOOTSTRAP_H
#define SSERVER_APP_APPBOOTSTRAP_H

#include <memory>

#include "common/metrics/LatencyRecorder.h"
#include "config/AppConfig.h"
#include "core/Application.h"

namespace sserver {
namespace modules {
namespace capture {
class CaptureModule;
}
namespace transport {
class TransportModule;
}
}  // namespace modules
namespace app {

class AppBootstrap {
public:
    explicit AppBootstrap(const config::AppConfig &config);
    ~AppBootstrap();

    bool Initialize();
    bool Start();
    void Stop();

    core::Application &application();
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder() const;
    int bound_port() const;

private:
    void BindStreamingPipeline();
    void UnbindStreamingPipeline();
    void PrintStartupSummary() const;
    void PrintShutdownSummary() const;

    config::AppConfig config_;
    std::unique_ptr<core::Application> application_;
    std::shared_ptr<common::metrics::LatencyRecorder> send_latency_recorder_;
    std::shared_ptr<modules::capture::CaptureModule> capture_module_;
    std::shared_ptr<modules::transport::TransportModule> transport_module_;
    bool started_;
};

}  // namespace app
}  // namespace sserver

#endif  // SSERVER_APP_APPBOOTSTRAP_H
