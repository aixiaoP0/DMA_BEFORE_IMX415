#ifndef SSERVER_MODULES_CAPTURE_VIDEO_CAPTUREMODULE_H
#define SSERVER_MODULES_CAPTURE_VIDEO_CAPTUREMODULE_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "common/concurrency/ThreadSafeQueue.h"
#include "common/model/EncodedFrame.h"
#include "core/IModule.h"
#include "modules/capture/video/Capture.h"
#include "modules/capture/video/ICaptureDevice.h"

namespace sserver {
namespace modules {
namespace capture {

using FrameHandler = std::function<void(common::model::EncodedFramePtr)>;

class CaptureModule : public core::IModule {
public:
    CaptureModule();
    ~CaptureModule() override;

    std::string name() const override;
    bool initialize(const core::ApplicationContext &context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;
    core::ModuleState state() const override;

    void SetFrameHandler(FrameHandler handler);

private:
    void CaptureLoop();
    void CapturePump();
    void EncodePump();

private:
    std::unique_ptr<Capture> capture_;
    std::thread worker_thread_;
    std::thread encode_thread_;
    std::atomic_bool running_;
    std::atomic<core::ModuleState> state_;
    std::mutex handler_mutex_;
    FrameHandler frame_handler_;
    common::concurrency::ThreadSafeQueue<RawCaptureFramePtr> raw_queue_;
    bool use_raw_capture_;
};

}  // namespace capture
}  // namespace modules
}  // namespace sserver

#endif  // SSERVER_MODULES_CAPTURE_VIDEO_CAPTUREMODULE_H
