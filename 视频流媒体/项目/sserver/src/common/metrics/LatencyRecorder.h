#ifndef SSERVER_COMMON_METRICS_LATENCYRECORDER_H
#define SSERVER_COMMON_METRICS_LATENCYRECORDER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace sserver {
namespace common {
namespace metrics {

struct LatencySummary {
    std::size_t count = 0;
    double min_ms = 0.0;
    double avg_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
};

class LatencyRecorder {
public:
    explicit LatencyRecorder(std::size_t max_samples = 4096);

    void RecordNs(std::uint64_t latency_ns);
    LatencySummary Snapshot() const;
    std::string Format(const std::string &name) const;

private:
    const std::size_t max_samples_;
    mutable std::mutex mutex_;
    std::deque<double> samples_ms_;
};

}  // namespace metrics
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_METRICS_LATENCYRECORDER_H
