#include "core/Application.h"

#include "common/log/Logger.h"

namespace sserver {
namespace core {

Application::Application(const config::AppConfig &config)
        : initialized_(false),
          running_(false) {
    context_.config = config;
}

void Application::RegisterModule(const std::shared_ptr<IModule> &module) {
    modules_.push_back(module);
}

bool Application::Initialize() {
    if (initialized_) {
        return true;
    }
    initialized_ = InitializeAllModules();
    return initialized_;
}

bool Application::Start() {
    if (!initialized_ && !Initialize()) {
        return false;
    }
    if (running_) {
        return true;
    }
    running_ = StartAllModules();
    return running_;
}

void Application::Stop() {
    if (!running_) {
        return;
    }
    StopAllModules();
    running_ = false;
}

void Application::Shutdown() {
    Stop();
    if (!initialized_) {
        return;
    }
    ShutdownAllModules();
    initialized_ = false;
}

const ApplicationContext &Application::context() const {
    return context_;
}

bool Application::InitializeAllModules() {
    for (std::size_t index = 0; index < modules_.size(); ++index) {
        const std::shared_ptr<IModule> &module = modules_[index];
        if (!module->initialize(context_)) {
            common::log::Logger::Error("failed to initialize module: " + module->name());
            for (std::size_t rollback = index; rollback > 0; --rollback) {
                modules_[rollback - 1]->shutdown();
            }
            return false;
        }
    }
    return true;
}

bool Application::StartAllModules() {
    for (std::size_t index = 0; index < modules_.size(); ++index) {
        const std::shared_ptr<IModule> &module = modules_[index];
        if (!module->start()) {
            common::log::Logger::Error("failed to start module: " + module->name());
            for (std::size_t rollback = index; rollback > 0; --rollback) {
                modules_[rollback - 1]->stop();
            }
            return false;
        }
    }
    return true;
}

void Application::StopAllModules() {
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
        (*it)->stop();
    }
}

void Application::ShutdownAllModules() {
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
        (*it)->shutdown();
    }
}

}  // namespace core
}  // namespace sserver
