# 架构对比：promise/future vs Per-Worker 独立任务队列

---

## 一、两套方案的架构总览

### 旧架构：全局共享队列 + promise/future

```
Aggregator 线程                              Worker 线程 (×4)
─────────────────                          ─────────────────
submit_task_async(index, img, nv12, ...)    while(run_flag) {
    │                                          │
    ├─ lock(task_mutx)              ←④ 全局锁     ├─ lock(task_mutx)      ←④ cv.wait() 抢锁
    ├─ tasks.push(move(t))          ←② 入队      ├─ tasks.pop()           ←② 出队
    ├─ unlock(task_mutx)                       ├─ unlock(task_mutx)
    ├─ task_cond.notify_one()                  │
    └─ return future ←① 返回 future            ├─ RGA NV12→NPU input    ←③ CPU 参与拷贝
                                               ├─ rknn_run()
                                               ├─ 后处理 + 画框
                                               ├─ BGR→NV12
                                               └─ t.promise.set_value(res) ←⑤ 无锁写结果
                                                                              ↑ 一对一信道
Aggregator 收集:                               
while(true) {
    this_thread::sleep_for(1ms);               ←⑥ sleep 轮询
    for (auto& [index, future] : tasks_inflight) {
        if (future.wait_for(0) == ready) {
            result = future.get();             ←⑦ 无锁读结果（atomic flag）
            按 nextWriteIndex 序输出
        }
    }
}
```

### 新架构：Per-Worker 独立任务队列 + 全局结果队列

```
ReadVideo 线程（Path C）                        Aggregator 线程
─────────────────                              ─────────────────
RGA 直写 NPU fd (已知 target Worker)           submit_to_worker(id, ...)       ← round-robin
                                                    │
                                                    ├─ lock(worker_ctx[id].mtx)
                                                    ├─ tasks.push(move(t))
                                                    ├─ unlock(...)
                                                    ├─ cv.notify_one()
                                                    │
                    ┌── queue[0] ── Worker0 ──→ lock(result_mtx) ──→ push → unlock
                    ├── queue[1] ── Worker1 ──→ lock(result_mtx) ──→ push → unlock
                    ├── queue[2] ── Worker2 ──→ lock(result_mtx) ──→ push → unlock
                    └── queue[3] ── Worker3 ──→ lock(result_mtx) ──→ push → unlock
                                                                ↓
                                                       result_queue (全局)
                                                                ↓
                                                    try_get_result(wr)    ← 非阻塞
                                                    result_buffer map 重排
                                                    nextWriteIndex 序输出
```

---

## 二、两方案核心差异速览

| 对比维度 | 旧架构 | 新架构 |
|:---------|:-------|:-------|
| **任务提交方式** | `submit_task_async()` → 全局队列，返回 future | `submit_to_worker(id, ...)` → 指定 Worker 私有队列 |
| **NV12→NPU 路径** | Path B: malloc + memcpy → Worker 中 RGA 缩放 | Path C: ReadVideo 中 RGA 直写 NPU fd |
| **NV12 拷贝次数** | 2 次（ReadVideo 1 次 + Worker 输出 1 次） | 1 次（仅 Worker 输出） |
| **流量控制** | 队列长度限流 + sleep(1ms) | `is_busy` atomic 按 Worker 丢帧 |
| **结果收集同步** | promise/future（零锁，atomic 状态机） | result_queue + mutex（有锁但无竞争） |
| **Aggregator 休眠** | sleep(1ms) 轮询 | try_get_result 即时返回，无 sleep |
| **锁竞争问题** | 全局队列 cv.wait() 的 futex 唤醒开销 | 任务端无竞争；结果端有锁但竞争极低 |
| **架构耦合度** | Task 内嵌 promise，Worker 持有引用 | Worker 松耦合，结果推入队列 |

---

## 三、旧架构的优点

### 3.1 结果收集端完美无锁

每个 `promise`/`future` 是 C++ 标准库的一对一同步信道：

```
promise.set_value(res)    ──→  shared_state (atomic ready flag)  ──→  future.get()
  (Worker i 写)                         │                           (Aggregator 读)
                                        │
                              ┌─────────┴──────────┐
                              │  原子操作，无需 mutex   │
                              │   无锁的无等待信道      │
                              └──────────────────────┘
```

每个 `shared_state` 只有 **1 个 Writer（Worker_i）+ 1 个 Reader（Aggregator）**，通过 atomic flag + memory ordering 实现线程安全，**无需 mutex**。这是 C++ 标准库提供的最高效的单向同步原语之一。

### 3.2 任务与结果天然绑定

```
submit_task_async() → future → 结果从同一个 future 取回
```

每个任务对应一个 future，数据流是线性、可追溯的。对于简单的生产者-消费者场景，这种"提交-领取"模式比"提交到队列-再从另一个队列收集"更直观。

### 3.3 无额外内存拷贝

Worker 通过 `t.promise.set_value(res)` 将结果写入 future 的 `shared_state`，Aggregator 通过 `future.get()` 读取——数据只移动一次，无中间队列的入队/出队开销。

---

## 四、旧架构的缺点

### 4.1 无法支持 Path C 零拷贝（核心问题）

旧架构的 `submit_task_async()` 提交到全局队列，Worker 从队列中自由竞争取任务。Aggregator **无法提前知道哪个 Worker 会处理这帧**：

```cpp
// 旧架构：Aggregator 不知道目标 Worker
auto future = gthreadpool.submit_task_async(index, img, nv12_data, ...);
// nv12_data 是 malloc 的堆内存，Worker 自己处理后 free
```

而 Path C 要求在**写入 NPU fd 前就确定目标 Worker**，因为每个 Worker 的 `input_mem->fd` 是不同的：

```cpp
// 新架构：必须先知道目标 Worker
int npu_fd = gthreadpool.get_worker_input_fd(worker_id);  // ← 必须先知道 worker_id
// RGA 直写到这个 npu_fd
rga_buffer_t dst = wrapbuffer_handle(dst_handle, 640, 640, RK_FORMAT_RGB_888);
imresize(src, dst);   // 写入 Worker_i 的 NPU 物理内存
```

如果不知道目标 Worker，写入的 `npu_fd` 可能和实际处理该帧的 Worker 不匹配 → **数据紊乱或 Crash**。所以旧架构的全局队列 + 自由竞争模式与 Path C 不兼容。

> **这是从旧架构到新架构的首要驱动力。** 不是"为了消除锁竞争而改为 per-worker 队列"，而是"为了 Path C 必须改成 per-worker 提交，顺便附带了一系列好处"。

### 4.2 `cv.wait()` 的 futex 唤醒开销

旧架构中 Worker 在空闲时阻塞在 `cv.wait()` 上：

```cpp
unique_lock<mutex> lock(task_mutx);
task_cond.wait(lock, [this]{ return !tasks.empty() || !run_flag; });
// pthread_cond_wait 内部：
//   ① unlock(mutex)           ← 释放锁
//   ② futex(FUTEX_WAIT)       ← 进入内核睡眠（上下文切换 ~1-5μs）
//   ...被 notify 唤醒后...
//   ③ lock(mutex)             ← 重锁（可能需要再次 futex）
```

每次唤醒必然涉及**用户态↔内核态切换**。即使无锁竞争，Worker 仍然要经过一次 futex 唤醒 + 重锁：

```
Aggregator:  push → unlock → notify_one()
                                         ↓
Worker(cv):  被唤醒 → futex 返回 → lock(mutex) → pop → unlock → 处理任务
              ↑ 至少 1-2 次内核交互，~1-5μs
```

而新架构中，Per-Worker 队列的 Worker 在 `cv.wait()` 上等待自己的私有队列。Aggregator `notify_one()` 只唤醒目标 Worker——不存在多余唤醒（无惊群效应）。但 futex 唤醒本身的 ~μs 级开销仍然存在，只是**只有目标 Worker 会醒来**，而不是所有 Worker 都醒来。

### 4.3 关于锁竞争频率的诚实分析

旧架构 4 个 Worker 从全局队列取任务时的锁竞争：

```
Worker0 推理耗时: 30ms
Worker1 推理耗时: 28ms
Worker2 推理耗时: 32ms
Worker3 推理耗时: 35ms
            ↑ 完成时刻天然错开，差距 ~5-10ms
```

**实际上，4 个 Worker 同时醒来抢锁的情况并不常见。** Worker 推理时间有波动（25-40ms），完成时刻错开。当 Aggregator 提交新帧时，大概率只有一个 Worker 空闲在 `cv.wait()` 上——其他 Worker 还在处理中。此时 `notify_one()` 唤醒这个唯一的空闲 Worker，不存在竞争。

锁竞争主要发生在**边界情况**：
- 帧率远低于推理速度时，多个 Worker 同时空闲
- 推理时间高度一致，多个 Worker 几乎同时完成
- 系统调度导致多个 Worker 被同时唤醒

所以旧架构的问题**不是"锁竞争频率高"**，而是：

| 问题 | 影响 |
|:-----|:------|
| **每次唤醒必有 futex** | cv.wait() 机制决定了每次任务分发至少经历 1 次 futex 系统调用（~μs 级），无论有无竞争 |
| **无法支持 Path C** | 不知道目标 Worker，无法让 ReadVideo 直写 NPU fd |
| **流量控制粗糙** | 只能全局限流，不能按 Worker 粒度丢帧 |
| **Task 携带大量数据** | nv12_data 指针 + promise，队列操作开销大 |

锁竞争本身不是瓶颈，但这些连带问题是。

### 4.4 流量控制粗糙

```cpp
// DMA_旧架构/thread_pool.cpp:98-101
while(tasks.size() > 10) {
    this_thread::sleep_for(chrono::milliseconds(1));
}
```

- **不能区分哪个 Worker 忙** — 如果只有 Worker0 处理慢，队列累积会导致所有新帧被限流
- **sleep(1ms) 延迟高** — 最坏情况 1ms 的空闲等待
- **无法按帧丢帧** — 只能整体阻塞，不能选择性丢弃特定帧

### 4.5 Task 结构臃肿

旧架构 Task 包含 `promise`（不可拷贝，只能 move）和 `nv12_data` 指针：

```cpp
typedef struct Task {
    int index;
    cv::Mat img;
    uint8_t* nv12_data;            // 1.38MB 堆内存的指针
    int nv12_size;
    int width;
    int height;
    std::promise<ProcessResult> promise;  // 不可拷贝
} Task;
```

`promise` 的存在意味着 Task **不能拷贝只能 move**，限制了队列操作的灵活性。且 Task 进入队列时携带的 `nv12_data` 指针指向堆内存，生命周期管理更复杂。

---

## 五、新架构的优点

### 5.1 支持 Path C 零拷贝（核心改进）

Path C 是 ReadVideo 线程中 RGA 直接将 NV12 缩放+转换后写入 NPU fd：

```
ReadVideo:
  ISP → DMABUF NV12
    ├─ Path A: RGA NV12→BGR (画框)
    └─ Path C: RGA importbuffer_fd(NPU fd) → imresize(NV12→RGB 640x640)
                 ↑ 直写 Worker_i 的物理内存，零 CPU 拷贝

Worker:
  rknn_run()  ← NPU 直接读取刚才写入的数据
```

这需要：
1. **ReadVideo 知道目标 Worker 的 `input_mem->fd`** → `get_worker_input_fd(worker_id)`
2. **ReadVideo 写入后通知对应 Worker 开始推理** → `submit_to_worker(worker_id, ...)`
3. **Worker 的 NPU 输入缓冲区不会被覆盖** → `is_busy` 保护

Per-Worker 队列 + 按 ID 提交是 Path C 的前提条件，而非副产品。

### 5.2 NV12 拷贝次数减半

| 架构 | NV12 拷贝路径 | 拷贝次数 |
|:-----|:-------------|:--------:|
| 旧架构 | ReadVideo: malloc+memcpy NV12(给 Worker) → Worker: malloc+BGR→NV12(给编码器) | **2 次** |
| 新架构 | Worker: malloc+BGR→NV12(给编码器) — ReadVideo 通过 Path C 零拷贝直写 NPU | **1 次** |

省去了一次 1.38MB（1280×720×1.5）的 NV12 整帧拷贝，在 30fps 下每秒省去 30 次 × 1.38MB ≈ **41MB/s 的内存带宽**。

### 5.3 按 Worker 粒度的流量控制

```cpp
// Aggregator 或 ReadVideo 中
for (int i = 0; i < 4; i++) {
    if (!gthreadpool.is_worker_busy(i)) {
        gthreadpool.submit_to_worker(i % 4, ...);
        break;
    }
}
// 如果 4 个 Worker 全忙 → 直接丢帧，不提交
```

`is_busy` 标志在 Worker 推理完成时置 `false`，粒度精确到 Worker 个体：

```
场景：Worker0 处理特别慢（~50ms），其他 Worker 正常（~25ms）
  
旧架构：
  Worker0 处理帧0 慢 → 队列累积帧4、8、12...
  队列 > 10 → sleep(1ms) → 所有 Worker 都被阻塞，包括空闲的 Worker1/2/3

新架构：
  Worker0 忙 → ReadVideo 跳过提交给 Worker0 的帧（帧0、4、8...）
  Worker1/2/3 继续正常处理自己的帧
  流水线整体吞吐不受单 Worker 波动影响
```

### 5.4 Task 极简化 + 无 sleep 轮询

Task 仅携带元数据：

```cpp
typedef struct Task {
    int index = 0;
    cv::Mat img;                    // BGR 图像头（引用计数共享，无像素拷贝）
    int width = 0;
    int height = 0;
} Task;
```

Aggregator 使用 `try_get_result` 即时返回：

```cpp
// 不 sleep
while(gthreadpool.try_get_result(wr)) {
    result_buffer[wr.index] = std::move(wr.result);
}
// 即使 result_queue 空，也不 sleep，直接继续处理新的输入帧
```

结果收集延迟从 ~500μs（旧架构 sleep 1ms 的中点）降为**几乎零延迟**。

### 5.5 锁竞争条件改善（附带收益）

Per-Worker 队列的锁行为：

```
Aggregator → queue[0] ← Worker0  (2 线程)
Aggregator → queue[1] ← Worker1  (2 线程)
Aggregator → queue[2] ← Worker2  (2 线程)
Aggregator → queue[3] ← Worker3  (2 线程)
```

每个队列只有 2 个线程访问。更重要的是**移除了 cv.wait() 在全局锁上的 futex 唤醒开销**——旧架构中 notify_one() 唤醒阻塞在全局 mutex 上的线程，涉及内核交互；新架构中 notify_one() 只唤醒目标 Worker，虽然 futex 开销仍然存在（Per-Worker cv 也需要 wait/notify），但**不会有其他 Worker 被误唤醒**。

**诚实对比**：

| 锁相关指标 | 旧架构（全局队列） | 新架构（Per-Worker） |
|:-----------|:-----------------|:-------------------|
| 竞争频率 | 低（Worker 完成时刻错开） | 低（同理） |
| 每次唤醒开销 | futex 系统调用（~1-5μs） | 同样有 futex 调用 |
| 惊群效应 | 可能唤醒多余 Worker | 只唤醒目标 Worker |
| 额外 Worker 被误唤醒 | 可能 | **不会** |
| 任务分发路径 | 全局锁 + cv + 1 步 copy | 单队列锁 + cv + 1 步 copy |

Per-Worker 队列在锁方面有改善，但**不是零锁，也不是零 futex**。真正的价值在于 `is_busy` 和 Path C 的支持。

---

## 六、新架构的缺点

### 6.1 结果收集端引入了锁

旧架构用 promise/future 实现了结果端零锁，新架构的 result_queue + mutex 是一把真正的锁：

```cpp
// Worker push 结果
{
    lock_guard<mutex> lock(result_mtx);
    result_queue.push({t.index, std::move(res)});
}

// Aggregator 弹出结果
bool ThreadPool::try_get_result(WorkerResult& wr) {
    lock_guard<mutex> lock(result_mtx);
    if(result_queue.empty()) return false;
    wr = std::move(result_queue.front());
    result_queue.pop();
    return true;
}
```

这把锁的实际开销：

```
无竞争时（绝大多数情况）：
  lock_guard 的 lock() → 1 次 CAS（用户态 atomic exchange，~10ns）→ 成功
  push() → 移动几个指针
  unlock() → atomic store（~5ns）
  总计 ≈ 30-50ns

有竞争时（极罕见，≈ 两个 Worker 同时完成）：
  CAS 失败 → futex(FUTEX_WAIT) 睡眠 → 被唤醒后重试
  额外开销 ≈ 2-5μs（一次 futex 的代价）
```

**关于竞争概率的诚实说明**：和旧架构的全局任务队列同理，4 个 Worker 推理完成时刻天然错开 ~5-10ms，同时 push result_queue 的概率极低。所以这把锁绝大多数时候无竞争，只是一个用户态原子操作。

> 这暴露了一个事实：**"Worker 完成时刻错开 → 竞争少"的逻辑在旧架构任务队列和新架构结果队列上都成立。** 所以锁竞争本身从来不是旧架构的主要问题。

### 6.2 架构不对称

| | 任务提交端 | 结果收集端 |
|:---|:---------|:----------|
| 旧架构 | 全局队列（有 cv.wait 的 futex 开销） | promise/future（零锁） |
| 新架构 | Per-Worker 队列（无跨 Worker 锁） | result_queue（有锁但无竞争） |

旧架构结果端零锁，新架构引入了锁。理论上可以用 Per-Worker 结果队列对称消除：

```cpp
struct WorkerContext {
    queue<Task> tasks;
    queue<ProcessResult> results;    // 对称！
    mutex task_mtx;
    mutex result_mtx;
    ...
};
```

但 `result_mtx` 的实际开销不到 50ns/帧，对称化带来的复杂度不值得。

### 6.3 结果收集多一层间接性

旧架构只需要遍历 `tasks_inflight` map：

```cpp
for (auto it = tasks_inflight.begin(); it != tasks_inflight.end(); ) {
    if (it->second.wait_for(0s) == future_status::ready) {
        ProcessResult result = it->second.get();
        it = tasks_inflight.erase(it);
    } else ++it;
}
```

新架构需要 queue + map 两层配合：

```cpp
// 步骤 A: 从 result_queue 收集到 buffer
WorkerResult wr;
while(gthreadpool.try_get_result(wr)) {
    result_buffer[wr.index] = std::move(wr.result);
}
// 步骤 B: 从 buffer 按序写出
while(result_buffer.count(nextWriteIndex)) {
    result_buffer.erase(nextWriteIndex++);
}
```

---

## 七、为什么选择新架构（核心决策理由）

### 7.1 首要理由：Path C 零拷贝

```
旧架构: NV12 → malloc+memcpy → Worker RGA 缩放 → NPU 推理
                                    ↑ CPU 参与拷贝
新架构: NV12 → RGA 直写 NPU fd → rknn_run
                  ↑ 零 CPU 拷贝
```

Path C 要求：
1. ReadVideo 在写入前知道目标 Worker 的 NPU fd
2. 写入后通知**同一个 Worker** 开始推理
3. 该 Worker 的输入缓冲区不能被其他帧覆盖

这三点决定了必须从**全局自由竞争**改为**按 ID 指定提交**，Per-Worker 队列是最自然的数据结构。

### 7.2 次要理由：`is_busy` 流量控制

一旦 Per-Worker 架构就绪，`is_busy` 就自然地融入：每个 Worker 处理完成后置 `is_busy=false`，ReadVideo 在提交前检查 `is_busy`——实现了按 Worker 粒度的丢帧保护。这是旧架构无法做到的。

### 7.3 附带收益汇总

| 收益 | 重要性 | 说明 |
|:-----|:------|:------|
| Path C 零拷贝 ✅ | **首要** | 省 1 次 1.38MB NV12 拷贝/帧，30fps 下省 41MB/s 带宽 |
| is_busy 按 Worker 丢帧 ✅ | **次要** | 单 Worker 慢速不影响整体流水线 |
| 无跨 Worker 锁竞争 ✅ | **附带** | Per-Worker 队列天然无竞争，但旧架构竞争也不严重 |
| 无 sleep 轮询 ✅ | **附带** | try_get_result 即时返回，vs 旧架构 sleep(1ms) |
| Task 极简化 ✅ | **附带** | 移除了 promise 和 nv12_data 指针 |

### 7.4 可接受的代价

| 代价 | 影响 | 接受理由 |
|:-----|:-----|:---------|
| 结果端 `result_mtx` | ~30-50ns/帧 | 相对于推理 ~30ms 完全不可感知 |
| 架构不对称 | 代码略复杂 | 对称修复（Per-Worker 结果队列）收益为零 |
| 结果多一层 queue | 多一步数据搬运 | 解耦 Worker 和 Aggregator |

---

## 八、`lock_guard<mutex>` 补充说明

### 8.1 基本用法

```cpp
{
    lock_guard<mutex> lock(result_mtx);  // 构造时 lock()
    result_queue.push(...);
}  // 析构时 unlock()
```

RAII（Resource Acquisition Is Initialization）风格——在作用域入口加锁，出口自动解锁，即使中间抛出异常也能正确释放。

### 8.2 底层实现

`std::mutex` 在 Linux glibc 下基于 `futex`（Fast Userspace Mutex）实现：

```
lock():
  ① CAS(state, 0→1)  ─── 用户态 atomic exchange（无 syscall）
      ├── 成功 → 拿到锁，返回（~10ns）
      └── 失败 → futex(FUTEX_WAIT) 进入睡眠（~1-5μs）

unlock():
  ① state.store(0)    ─── 用户态 atomic store
  ② futex(FUTEX_WAKE)  ─── 如有等待者则唤醒（~1μs）
```

**CAS（Compare-And-Swap）** 是一条 CPU 原子指令：

```
CAS(内存地址, 期望值, 新值):
  如果 内存地址的值 == 期望值:
      将内存地址的值替换为新值，返回 true
  否则:
      返回 false，什么都不做

// mutex::lock() 中的 CAS 语义：
state.compare_exchange_strong(0, 1)
// 期望 state 是 0（未锁定），如果是，设为 1（锁定），返回 true
// 如果 state 已经是 1（被他人持有），返回 false
```

CAS 失败后 `mutex` **不会在用户态无限自旋**，而是直接 futex 睡眠，避免 CPU 空转。这和纯自旋锁不同：

```cpp
// 纯自旋锁（会空转 CPU）：
while (state.exchange(1)) {   // 不停尝试，CPU 100%
    ;
}

// std::mutex（无竞争时 CAS 直接成功，有竞争时 futex 睡眠）：
if (state.compare_exchange_strong(0, 1)) {
    return;  // 拿到锁
}
futex(FUTEX_WAIT, ...);  // 睡眠，不浪费 CPU
```

### 8.3 新架构中的实际行为

`result_mtx` 的竞争场景：

```
Worker推理完成(~30ms) → lock(result_mtx) → CAS 成功 → push → unlock
Aggregator轮询         → lock(result_mtx) → CAS 成功 → try_pop → unlock
                      ↑ 多数时候无竞争，CAS 一次成功
                      
意外碰撞:
Worker lock() CAS 成功 → 写结果
Aggregator lock() CAS 失败 → futex(WAIT) → Worker unlock → futex(WAKE) → Aggregator 被唤醒 → 取结果
                      ↑ 极罕见，额外 ~2-5μs
```

**实际运行中，`result_mtx` 的 lock() 几乎总是在 CAS 第一次尝试时成功**，从不下潜到 futex。这就是为什么说它的开销 ≈ 30-50ns——纯粹的用户态原子操作。

---

## 九、面试口语化总结

> **问：为什么从 promise/future 换到 Per-Worker 队列？**
>
> **答：首要原因是 Path C 零拷贝需要提前确定目标 Worker。**
>
> 旧架构的 `submit_task_async()` 提交到全局队列，Worker 自由竞争取任务，Aggregator 无法知道谁最终处理这帧。但 Path C 要求 ReadVideo 将 NV12 直接写入指定 Worker 的 NPU 物理内存（`input_mem->fd`），必须在写入前就知道目标 Worker——所以必须改为 `submit_to_worker(worker_id, ...)` 按 ID 寻址。
>
> 一旦改为 Per-Worker 提交，自然演进到 Per-Worker 独立队列。`is_busy` 标志也自然地实现了按 Worker 粒度的丢帧——这是旧架构做不到的。
>
> **那旧架构的锁竞争严重吗？** 诚实地说，4 个 Worker 推理完成时刻错开 ~5-10ms，同时抢锁的概率并不高。旧架构真正的问题是 `cv.wait()` 的 futex 唤醒机制（每次 ~μs 级内核交互）和无法支持 Path C。Per-Worker 队列消除了惊群效应（只唤醒目标 Worker），但锁行为上的改善只是附带收益，不是决策驱动力。

> **问：新架构结果端不是多了一把锁吗？**
>
> **答：是，但它的实际开销约 30-50ns/帧，且几乎永远无竞争——Worker 完成时刻天然错开，和旧架构任务队列的逻辑一样。这 50ns 相对于 Worker 30ms 的推理时间占比约 0.00017%，完全不可感知。相当于用一把几乎零开销的锁，换来了 Path C 零拷贝和按 Worker 丢帧的能力。**

### 一句话总结

> **不是"为了消除锁竞争而改 Per-Worker 队列"，而是"为了 Path C 零拷贝必须按 ID 提交，Per-Worker 队列是自然选择，锁行为的改善只是附带收益"。**
