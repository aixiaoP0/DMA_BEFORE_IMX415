# 线程间同步：SafeQueue 有界阻塞队列

**相关代码：** `SafeQueue.h`（完整文件，约 70 行）

---

## 概述

SafeQueue 是一个**有界、线程安全、基于条件变量的阻塞队列模板**，连接管线中的三个线程：

```
ReadVideo ──enqueue──► g_SafeQueueRead(100) ──dequeue──► Aggregator
                                                              │
                                                          submit_task_async
                                                              │
                                                              ▼
Aggregator ──enqueue──► g_SafeQueueWrite(100) ──dequeue──► WriteVideo
```

| 队列 | 容量 | 生产者 | 消费者 | 传递数据 |
|------|------|--------|--------|---------|
| `g_SafeQueueRead` | 100 | ReadVideo | Aggregator | `FrameData`（BGR + NV12） |
| `g_SafeQueueWrite` | 100 | Aggregator | WriteVideo | `FrameData`（绘制后图像 + NV12） |

---

## 核心设计

```cpp
template<typename T>
class SafeQueue {
public:
    SafeQueue(size_t maxSize_in) : maxSize(maxSize_in) {}

    void enqueue(const T& t);
    bool dequeue(T& t);
    void stop();
    bool empty();
    size_t size();

private:
    bool stop_flag = false;
    queue<T> q;
    mutable mutex m;
    condition_variable cond_not_empty;
    condition_variable cond_not_full;
    size_t maxSize;
};
```

**关键设计决策：**

| 元素 | 选择 | 原因 |
|------|------|------|
| 两个条件变量 | `cond_not_empty` + `cond_not_full` | 通用 MPMC 设计，生产者和消费者分开等待，避免无效唤醒 |
| `maxSize` 有界 | 固定容量 | 防止生产者快于消费者时内存无限增长 |
| `mutable mutex` | `mutable` 修饰 | `empty()` 和 `size()` 是 const 方法，但需要加锁 |
| 模板 | `template<typename T>` | ReadQueue 和 WriteQueue 都使用，传递不同数据 |

---

## enqueue：带阻塞的生产者

**相关代码：** `SafeQueue.h:21-29`

```cpp
void enqueue(const T& t) {
    unique_lock<mutex> lock(m);
    cond_not_full.wait(lock, [this]{ return q.size() < maxSize; });

    q.push(t);
    cond_not_empty.notify_all();
}
```

### 执行流程

```
1. 加锁
2. 检查队列是否已满
   ├── 已满 (size >= maxSize) → 阻塞等待，直到有消费者 dequeue 唤醒
   └── 未满 → 继续
3. 入队
4. 通知消费者：队列非空，可以取了
5. 解锁（unique_lock 析构时自动释放）
```

### 虚假唤醒防护

```cpp
cond_not_full.wait(lock, [this]{ return q.size() < maxSize; });
// 等价于：
while (q.size() >= maxSize) {
    cond_not_full.wait(lock);
}
```

条件变量的 `wait` 可能被**虚假唤醒**（即使没有 `notify`，操作系统也可能让线程返回）。predicate 形式的 `wait` 会在每次被唤醒后重新检查条件——条件不满足就继续等，等价于 `while` 循环。

### 为什么 notify_all 而不是 notify_one？

```cpp
cond_not_empty.notify_all();
```

虽然每次 enqueue 只增加一个元素，但如果有多个消费者都在等待，`notify_one` 只能唤醒其中一个。被唤醒的消费者处理完数据后，队列可能仍有数据——但因为只唤醒了"一个人"，其他人还在睡。`notify_all` 确保所有等待消费的线程都有机会检查队列。

---

## dequeue：带阻塞的消费者

**相关代码：** `SafeQueue.h:32-43`

```cpp
bool dequeue(T& t) {
    unique_lock<mutex> lock(m);
    cond_not_empty.wait(lock, [this]{ return !q.empty(); });

    if (stop_flag && q.empty()) {
        return false;  // 收到停止信号，让调用方退出
    }

    t = q.front();
    q.pop();
    cond_not_full.notify_all();
    return true;
}
```

### 执行流程

```
1. 加锁
2. 检查队列是否为空
   ├── 为空 → 阻塞等待，直到有生产者 enqueue 唤醒
   └── 非空 → 继续
3. 检查停止标志
   ├── stop_flag && q.empty() → 返回 false（通知调用方退出）
   └── 否则 → 继续
4. 取出队首元素
5. 出队
6. 通知生产者：队列有空位了，可以继续入队
7. 返回 true
```

### 返回值语义

| 返回值 | 含义 |
|--------|------|
| `true` | 出队成功，`t` 中为有效数据 |
| `false` | 队列已停止且无数据，调用方应退出循环 |

这样 Aggregator 和 WriteVideo 可以直接用返回值做退出检测：

```cpp
// 典型的消费者循环
while (true) {
    FrameData fd;
    if (!g_SafeQueueRead.dequeue(fd)) {
        break;  // 队列已 stop，退出线程
    }
    // 处理 fd...
}
```

---

## stop：线程退出信号

**相关代码：** `SafeQueue.h:46-51`

```cpp
void stop() {
    unique_lock<mutex> lock(m);
    stop_flag = true;
    cond_not_empty.notify_all();
    cond_not_full.notify_all();
}
```

### 作用机制

`stop()` 被调用时：

```
stop_flag = true
notify_all（唤醒所有阻塞的线程）
    │
    ├── 生产者（enqueue 中等待 cond_not_full）被唤醒
    │   → 检查 predicate: q.size() < maxSize
    │   → 条件满足（队列不可能是满的 + stop_flag 不影响满判断）
    │   → 入队（可能还有残余数据）
    │
    └── 消费者（dequeue 中等待 cond_not_empty）被唤醒
        → 检查 predicate: !q.empty()
        ├── 队列有数据 → 正常出队，最后检查 stop_flag 发现 stop=true 但 q 不空
        │   → 返回 true（不丢数据）
        └── 队列为空 → wait() 返回
            → 检查 if (stop_flag && q.empty())
            → return false（通知退出）
```

**重要语义：`stop()` 不丢数据。** 如果队列中还有数据，`dequeue` 仍会返回 `true` 处理完所有残留帧，只有当队列彻底清空后才返回 `false`。

### 调用时机

**相关代码：** `main.cpp:649-650`

```cpp
// 在所有线程 join 之后调用
g_SafeQueueRead.stop();
g_SafeQueueWrite.stop();
```

作为**兜底安全措施**——如果某个线程因异常没有正常退出、仍阻塞在 `dequeue` 上，`stop()` 确保它被唤醒并退出。

---

## 完整时序：enqueue ↔ dequeue 协作

```
时间 →  Thread A (生产者)                  Thread B (消费者)
         │                                   │
         │  lock(m)                          │
         │  q.size()=100(max)                │
         │  cond_not_full.wait() 阻塞 ───────┤
         │                                   │  lock(m)
         │                                   │  q.size()=100
         │                                   │  q.front() → q.pop()
         │                                   │  cond_not_full.notify_all()
         │                                   │  unlock
         │                                   │  return t
         │  ← 被唤醒 ────────────────────────┤
         │  predicate: q.size()=99 < 100 ✓   │
         │  q.push(t)                        │
         │  cond_not_empty.notify_all()      │
         │  unlock                           │
         │                                   │
         │                                   │ 下一次 dequeue()
         │                                   │  lock(m)
         │                                   │  q.size()=1
         │                                   │  q.front() → q.pop()
         │                                   │  cond_not_full.notify_all()
         │                                   │  unlock
```

---

## 为什么不用 std::queue + mutex + busy-wait？

```cpp
// 错误示范：不要这样做
while (true) {
    lock_guard<mutex> lock(m);
    if (!q.empty()) {
        t = q.front(); q.pop();
        break;
    }
    // 没有数据？继续循环（忙等待）
}
```

| 方案 | CPU 占用 | 延迟 |
|------|---------|------|
| `mutex` + busy-wait | 100% 核心（空转） | 低 |
| `mutex` + `sleep(1ms)` | ~0%（大部分在休眠） | 高（1ms 延迟） |
| **条件变量（本方案）** | ~0%（内核调度睡眠） | **接近 0**（事件驱动即时唤醒） |

条件变量的优势在于**事件驱动**——线程在等待时进入睡眠状态（不占 CPU），被 `notify` 唤醒后立即执行，不引入额外延迟。

---

## 参数化对比：条件变量的 wait 行为

```cpp
// 方式 A：无 predicate（不推荐）
cond.wait(lock);
// 被唤醒后不检查条件，直接继续
// 可能发生虚假唤醒

// 方式 B：有 predicate（推荐，本项目使用）
cond.wait(lock, [this]{ return !q.empty(); });
// 等价于：
while (!q.empty()) {
    cond.wait(lock);
}
// 被唤醒后自动检查条件，不满足继续等
```

本项目中两个条件变量都使用了 predicate 形式：

| 条件变量 | predicate | 等待的原因 |
|---------|-----------|-----------|
| `cond_not_full.wait(lock, [this]{ return q.size() < maxSize; })` | 队列未满 | enqueue 时队列满了 |
| `cond_not_empty.wait(lock, [this]{ return !q.empty(); })` | 队列非空 | dequeue 时队列空了 |

---

---

## 双条件变量还是单条件变量？

### 两个 CV 解决了什么问题？

SafeQueue 用两个条件变量（`cond_not_empty` + `cond_not_full`）分别等待"队列有数据"和"队列有空位"。单条件变量的有界队列存在惊群问题：

```
单 CV 场景：队列满（size=100），Thread A 等入队，Thread B/C 等出队

消费者 Thread C 取走一个元素:
  cv.notify_all()
       │
       ▼
  Thread A (生产者)  ← 该醒 → size=99 < 100 ✓ 入队成功
  Thread B (消费者)  ← 白醒 → empty? false → 继续等
  Thread C (消费者)  ← 白醒 → empty? false → 继续等
                                 ↑
                    3 人醒，只有 1 个有事干，2 个空转一圈又睡回去
```

双 CV 把等待者按角色分开：

```
队列满时：
  cond_not_full 等待者:  Thread A (生产者)
  cond_not_empty 等待者:  Thread B, C (消费者)

消费者取走元素后调用 cond_not_full.notify_all()：
  只唤醒 Thread A（生产者），Thread B/C 继续睡
  → 零无效唤醒
```

### 但这个项目里实际用不上

看两个 SafeQueue 的生产者-消费者关系：

```
g_SafeQueueRead  = ReadVideo(1) → Aggregator(1)  → SPSC
g_SafeQueueWrite = Aggregator(1) → WriteVideo(1) → SPSC
```

**两个队列都是单一生产者、单一消费者（SPSC）。** 在这个前提下：

- 无论一个 CV 还是两个 CV，`notify_all` 唤醒的都是唯一那个等待者
- 根本没有"多个人抢着醒"的问题

单 CV + `notify_one` 在 SPSC 场景下效果完全相同：

```cpp
// SPSC 场景：单 CV 就够了
void enqueue(const T& t) {
    unique_lock<mutex> lock(m);
    cv.wait(lock, [this]{ return q.size() < maxSize; });
    q.push(t);
    cv.notify_one();     // 唯一消费者醒来，没有浪费
}

bool dequeue(T& t) {
    unique_lock<mutex> lock(m);
    cv.wait(lock, [this]{ return !q.empty(); });
    t = q.front(); q.pop();
    cv.notify_one();     // 唯一生产者醒来，没有浪费
    return true;
}
```

**双条件变量在这个项目中没有性能收益。** 它只是一个按通用 MPMC 模板编写的设计，恰好被用于 SPSC 场景。

### 对比：项目中的单 CV 场景

线程池的 per-Worker 队列只用了一个条件变量：

```cpp
struct WorkerContext {
    std::queue<Task> tasks;
    mutex mtx;
    condition_variable cv;  // 单 CV
};
```

这里**生产者从不阻塞**（per-worker 队列无界），只有消费者（Worker）需要等待任务到达。所以一个 CV 就够了——只用来唤醒 Worker：

| 对比 | SafeQueue | Per-Worker 队列 |
|------|-----------|----------------|
| 生产者-消费者 | 1:1（SPSC） | 1:1（SPSC） |
| 是否有界 | 有界（maxSize=100） | 无界 |
| 生产者是否可能阻塞 | 是（队列满时） | 否 |
| 需要几个条件变量 | 2（通用设计） | 1（足够） |

### 所以双 CV 到底有用吗？

**在通用 MPMC 场景下有用**——如果未来管线需要多个摄像头同时入队、多个后端同时出队，双 CV 可以避免惊群。

**在当前 SPSC 场景下没有用**——单 CV + `notify_one` 效果完全一样。

> **面试回答：** "我们的 SafeQueue 是按通用有界队列模板写的，用了双条件变量做生产者和消费者的分离等待。但在实际管线中，ReadQueue 和 WriteQueue 都是 SPSC（单一的采集线程和单一的调度/写入线程），所以双条件变量的优势没有发挥出来。如果后续扩展到多路摄像头，多生产者入队同一队列时，这个设计就有意义了。"

---

## 总结

SafeQueue 的设计模式：

```
                      ┌──────────────────┐
                      │     SafeQueue    │
                      │                  │
    enqueue(T) ──────►│  q.push(T)       │◄────── dequeue(T&)
                      │  cond_not_empty  │
    cond_not_full ◄───│  .notify_all()   │───► cond_not_empty
                      │                  │     .wait()
    cond_not_full     │  cond_not_full   │
    .wait() ◄─────────│  .notify_all() ◄─┼─────── dequeue()
                      │                  │
                      │  maxSize = 100   │
                      │  stop_flag       │
                      └──────────────────┘
```

三个关键设计原则：

1. **有界阻塞** — `maxSize` 防止内存无限增长，读写速率自动平衡
2. **事件驱动** — 条件变量替代忙等待，CPU 占用接近 0
3. **安全退出** — `stop()` + `stop_flag` 支持线程优雅退出，不丢数据
