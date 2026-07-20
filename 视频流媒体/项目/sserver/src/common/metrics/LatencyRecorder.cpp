#include "common/metrics/LatencyRecorder.h"

#include <algorithm>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace sserver {
namespace common {
namespace metrics {

namespace {

template <typename Container>
double PercentileFromSorted(const Container &values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }

    const double index = percentile * static_cast<double>(values.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(index);
    const std::size_t upper = std::min(lower + 1, values.size() - 1);
    const double fraction = index - static_cast<double>(lower);
    return values[lower] + (values[upper] - values[lower]) * fraction;
}

}  // namespace

LatencyRecorder::LatencyRecorder(std::size_t max_samples)
        : max_samples_(max_samples) {
}

void LatencyRecorder::RecordNs(std::uint64_t latency_ns) {
    const double latency_ms = static_cast<double>(latency_ns) / 1000000.0;
    std::lock_guard<std::mutex> lock(mutex_);
    if (samples_ms_.size() >= max_samples_) {
        samples_ms_.pop_front();
    }
    samples_ms_.push_back(latency_ms);
}

LatencySummary LatencyRecorder::Snapshot() const {
    std::deque<double> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = samples_ms_;
    }

    LatencySummary summary;
    summary.count = snapshot.size();
    if (snapshot.empty()) {
        return summary;
    }

    std::sort(snapshot.begin(), snapshot.end());
    summary.min_ms = snapshot.front();
    summary.max_ms = snapshot.back();
    summary.avg_ms = std::accumulate(snapshot.begin(), snapshot.end(), 0.0) / static_cast<double>(snapshot.size());
    summary.p50_ms = PercentileFromSorted(snapshot, 0.50);
    summary.p95_ms = PercentileFromSorted(snapshot, 0.95);
    summary.p99_ms = PercentileFromSorted(snapshot, 0.99);
    return summary;
}

std::string LatencyRecorder::Format(const std::string &name) const {
    const LatencySummary summary = Snapshot();
    std::ostringstream stream;
    stream << name
           << " count=" << summary.count
           << " min=" << std::fixed << std::setprecision(2) << summary.min_ms << "ms"
           << " avg=" << summary.avg_ms << "ms"
           << " p50=" << summary.p50_ms << "ms"
           << " p95=" << summary.p95_ms << "ms"
           << " p99=" << summary.p99_ms << "ms"
           << " max=" << summary.max_ms << "ms";
    return stream.str();
}

}  // namespace metrics
}  // namespace common
}  // namespace sserver
