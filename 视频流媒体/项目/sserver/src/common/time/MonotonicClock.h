#ifndef SSERVER_COMMON_TIME_MONOTONICCLOCK_H
#define SSERVER_COMMON_TIME_MONOTONICCLOCK_H

#include <chrono>
#include <cstdint>

namespace sserver {
namespace common {
namespace time {

inline std::uint64_t MonotonicNowNs() {
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
}

}  // namespace time
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_TIME_MONOTONICCLOCK_H
