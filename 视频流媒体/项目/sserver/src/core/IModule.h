#ifndef SSERVER_CORE_IMODULE_H
#define SSERVER_CORE_IMODULE_H

#include <string>

#include "core/ApplicationContext.h"
#include "core/ModuleState.h"

namespace sserver {
namespace core {

class IModule {
public:
    virtual ~IModule() {}

    virtual std::string name() const = 0;
    virtual bool initialize(const ApplicationContext &context) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void shutdown() = 0;
    virtual ModuleState state() const = 0;
};

}  // namespace core
}  // namespace sserver

#endif  // SSERVER_CORE_IMODULE_H
