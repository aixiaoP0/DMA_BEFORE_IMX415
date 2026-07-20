#ifndef SSERVER_COMMON_CONCURRENCY_THREADSAFEQUEUE_H
#define SSERVER_COMMON_CONCURRENCY_THREADSAFEQUEUE_H

#include <cstddef>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace sserver {
namespace common {
namespace concurrency {

// 线程安全的双端队列，支持阻塞等待弹出、丢弃最旧元素、按条件选择性丢弃等策略
template <typename T>
class ThreadSafeQueue {
public:
    void Push(const T &value) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(value);
        }
        condition_variable_.notify_one();
    }

    void PushDropOldest(const T &value, std::size_t max_size) {
        PushDropOldestCountDropped(value, max_size);
    }

    std::size_t PushDropOldestCountDropped(const T &value, std::size_t max_size) {
        std::size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            while (queue_.size() >= max_size && !queue_.empty()) {
                queue_.pop_front();
                ++dropped;
            }
            queue_.push_back(value);
        }
        condition_variable_.notify_one();
        return dropped;
    }

    template <typename Predicate>
    std::size_t PushDropSelective(const T &value, std::size_t max_size, Predicate predicate) {
        std::size_t dropped = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            while (queue_.size() >= max_size && !queue_.empty()) {
                typename std::deque<T>::iterator candidate = std::find_if(queue_.begin(), queue_.end(), predicate);
                if (candidate != queue_.end()) {
                    queue_.erase(candidate);
                } else {
                    queue_.pop_front();
                }
                ++dropped;
            }
            queue_.push_back(value);
        }
        condition_variable_.notify_one();
        return dropped;
    }

    bool WaitPopFor(T *value, const std::chrono::milliseconds &timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!condition_variable_.wait_for(lock, timeout, [this]() { return !queue_.empty(); })) {
            return false;
        }
        *value = queue_.front();
        queue_.pop_front();
        return true;
    }

    void NotifyAll() {
        condition_variable_.notify_all();
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

    bool TryPop(T *value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        *value = queue_.front();
        queue_.pop_front();
        return true;
    }

    bool Empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    template <typename Predicate>
    bool AnyMatching(Predicate predicate) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::find_if(queue_.begin(), queue_.end(), predicate) != queue_.end();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_variable_;
    std::deque<T> queue_;
};

}  // namespace concurrency
}  // namespace common
}  // namespace sserver

#endif  // SSERVER_COMMON_CONCURRENCY_THREADSAFEQUEUE_H
