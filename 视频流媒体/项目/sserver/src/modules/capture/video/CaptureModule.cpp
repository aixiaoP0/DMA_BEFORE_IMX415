#include "modules/capture/video/CaptureModule.h"

#include <chrono>

#include "common/log/Logger.h"

namespace sserver {
namespace modules {
namespace capture {

CaptureModule::CaptureModule()
        : capture_(std::make_unique<Capture>()),
          running_(false),
          state_(core::ModuleState::kCreated),
          use_raw_capture_(false) {
}

CaptureModule::~CaptureModule() {
    shutdown();
}

std::string CaptureModule::name() const {
    return "CaptureModule";
}

bool CaptureModule::initialize(const core::ApplicationContext &context) {
    std::string error_message;
    if (!capture_->initialize(context, &error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to initialize capture: " + error_message);
        }
        state_ = core::ModuleState::kFailed;
        return false;
    }
    state_ = core::ModuleState::kInitialized;
    return true;
}

bool CaptureModule::start() {
    std::string error_message;
    if (!capture_->start(&error_message)) {
        if (!error_message.empty()) {
            common::log::Logger::Error("failed to start capture: " + error_message);
        }
        state_ = core::ModuleState::kFailed;
        return false;
    }

    running_ = true;
    use_raw_capture_ = capture_->SupportsRawCapture();
    if (use_raw_capture_) {
        worker_thread_ = std::thread(&CaptureModule::CapturePump, this);
        encode_thread_ = std::thread(&CaptureModule::EncodePump, this);
    } else {
        worker_thread_ = std::thread(&CaptureModule::CaptureLoop, this);
    }
    state_ = core::ModuleState::kRunning;
    common::log::Logger::Info("capture module started with " + capture_->Describe() +
                              (use_raw_capture_ ? " (dual-thread raw capture)" : " (single-thread)"));
    return true;
}

void CaptureModule::stop() {
    if (state_.load() != core::ModuleState::kRunning) {
        return;
    }

    running_ = false;
    raw_queue_.NotifyAll();

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    if (encode_thread_.joinable()) {
        encode_thread_.join();
    }
    capture_->stop();
    state_ = core::ModuleState::kStopped;
}

void CaptureModule::shutdown() {
    stop();
    capture_->shutdown();
    state_ = core::ModuleState::kShutdown;
}

core::ModuleState CaptureModule::state() const {
    return state_.load();
}

void CaptureModule::SetFrameHandler(FrameHandler handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    frame_handler_ = handler;
}

// 单线程路径：采集与编码在同一线程中完成（适用于不支持原始帧采集的设备）
void CaptureModule::CaptureLoop() {
    while (running_.load()) {
        common::model::EncodedFramePtr frame = capture_->CaptureFrame();
        if (!frame) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        FrameHandler handler_copy;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler_copy = frame_handler_;
        }
        if (handler_copy) {
            handler_copy(frame);
        }
    }
}

// 采集泵：从设备取出原始帧 → 拷贝 → 归还缓冲区 → 入队等待编码
void CaptureModule::CapturePump() {
    while (running_.load()) {
        RawCaptureFramePtr raw = capture_->CaptureRawFrame();
        if (!raw) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        raw_queue_.PushDropOldest(raw, 3);
    }
    raw_queue_.NotifyAll();
}

// 编码泵：从队列取出原始帧 → 编码 → 分发给下游
void CaptureModule::EncodePump() {
    while (running_.load()) {
        RawCaptureFramePtr raw;
        if (!raw_queue_.WaitPopFor(&raw, std::chrono::milliseconds(10))) {
            continue;
        }
        if (!raw) {
            continue;
        }

        common::model::EncodedFramePtr frame = capture_->EncodeRawFrame(raw);
        if (!frame) {
            continue;
        }

        FrameHandler handler_copy;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler_copy = frame_handler_;
        }
        if (handler_copy) {
            handler_copy(frame);
        }
    }
}

}  // namespace capture
}  // namespace modules
}  // namespace sserver
