#ifndef SSERVER_CORE_APPLICATION_H
#define SSERVER_CORE_APPLICATION_H

#include <memory>
#include <vector>

#include "core/ApplicationContext.h"
#include "core/IModule.h"

namespace sserver {
namespace core {

class Application {
public:
    explicit Application(const config::AppConfig &config);

    void RegisterModule(const std::shared_ptr<IModule> &module);

    bool Initialize();
    bool Start();
    void Stop();
    void Shutdown();

    const ApplicationContext &context() const;

private:
    bool InitializeAllModules();
    bool StartAllModules();
    void StopAllModules();
    void ShutdownAllModules();

    ApplicationContext context_;
    std::vector<std::shared_ptr<IModule>> modules_;
    bool initialized_;
    bool running_;
};

}  // namespace core
}  // namespace sserver

#endif  // SSERVER_CORE_APPLICATION_H
