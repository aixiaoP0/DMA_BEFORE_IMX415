#ifndef SSERVER_CORE_MODULESTATE_H
#define SSERVER_CORE_MODULESTATE_H

namespace sserver {
namespace core {

enum class ModuleState {
    kCreated,
    kInitialized,
    kRunning,
    kStopped,
    kShutdown,
    kFailed,
};

}  // namespace core
}  // namespace sserver

#endif  // SSERVER_CORE_MODULESTATE_H
