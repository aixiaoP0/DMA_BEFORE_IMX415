# 线程池：ThreadPool

**相关代码：** `thread_pool.h`（声明）、`thread_pool.cpp`（实现）

---

## 概述

ThreadPool 是 YOLOv5s 推理的**并行调度引擎**。它管理 4 个工作线程，每个线程持有独立的 YOLO 模型实例，通过**每 Worker 独立任务队列** + 条件变量实现无锁化生产-消费模式。

```
Aggregator                        ThreadPool
    │                                 │
    │  submit_to_worker(index%4,      │
    │    img, w, h)                   │
    │                                 │
    │  (no future) ───────────────────┤
    │                                 ├── Worker 0 私有队列 (CPU4, NPU Core 0)
    │                                 ├── Worker 1 私有队列 (CPU5, NPU Core 1)
    │                                 ├── Worker 2 私有队列 (CPU6, NPU Core 2)
    │                                 └── Worker 3 私有队列 (CPU7, NPU Core 0)
    │                                 │
    │  try_get_result() ◄──── result_queue (公共收集队列)
    │  result_buffer[index] = result  │
```

---

## 核心数据结构

### Task（任务单元 — 简化版）

```cpp
typedef struct Task {
    int index = 0;                  // 帧序号（保序用）
    cv::Mat img;                    // BGR 图像（画框用）
    int width = 0, height = 0;      // 原始分辨率
} Task;
```

**重构变化：** 旧版 Task 包含 `nv12_data`、`nv12_size`、`promise` 三个字段。重构后：
- **NV12 输入已由 ReadVideo 的 Path C（RGA 硬件 DMA）直写 NPU 物理内存**，不需要在 Task 中传递
- **Future/Promise 已移除**，改用 `result_queue` 收集结果，消除每次 submit 的堆分配和原子同步

### WorkerResult（结果队列单元 — 新增）

```cpp
typedef struct WorkerResult {
    int index;                      // 帧序号
    ProcessResult result;           // 推理结果
} WorkerResult;
```

Worker 完成后将 `{index, ProcessResult}` 推入公共结果队列，Aggregator 通过 `try_get_result()` 批量 drain。

### ProcessResult（处理结果）

```cpp
typedef struct ProcessResult {
    cv::Mat processed_img;              // 绘制后的图像
    uint8_t* nv12_data;                 // 编码用的 NV12 数据
    int data_size;
    detect_result_group_t detection_results; // 检测结果
    bool success = false;
    std::string error_msg;
} ProcessResult;
```

### 线程池内部成员

```cpp
class ThreadPool {
    // 每 Worker 独立上下文（替代旧版单队列）
    struct WorkerContext {
        std::queue<Task> tasks;              // 私有任务队列
        mutex mtx;                           // 队列锁
        condition_variable cv;               // 任务通知
        std::atomic<bool> is_busy{false};    // NPU 缓冲区占用标志
    };
    std::unique_ptr<WorkerContext[]> worker_ctx; // unique_ptr 数组规避 mutex 不可拷贝限制

    vector<shared_ptr<Yolov5s>> yolo_group;  // 每个 Worker 独享的 YOLO 实例
    vector<thread> threads;                   // Worker 线程容器
    bool run_flag;                            // 线程池运行标志

    // 结果收集队列（Aggregator 通过 try_get_result 批量 drain）
    std::queue<WorkerResult> result_queue;
    mutex result_mtx;
    atomic<int> tasks_in_flight{0};           // 在途任务计数
};
```

**与旧版的三个关键差异：**

| 差异 | 旧版 | 新版 |
|------|------|------|
| 任务队列 | 单 `queue<Task>` + 全局锁 | 每 Worker 独立 `queue<Task>` + 各自锁，无竞争 |
| 结果传递 | `promise/future`（堆分配 + 原子同步） | `result_queue` + `mutex`（轻量队列） |
| NPU 缓冲区保护 | 无 | `atomic<bool> is_busy` 防止内存踩踏 |

---

## 初始化流程

**相关代码：** `thread_pool.cpp:55-73`

```cpp
int ThreadPool::init(const char *model_path, int num_threads) {
    // 1. 创建 YOLO 实例（每个 Worker 一个）
    for (size_t i = 0; i < num_threads; i++) {
        auto yolo = make_shared<Yolov5s>(model_path, i % 3);
        yolo_group.emplace_back(yolo);
    }

    // 2. 启动 Worker 线程
    for (size_t i = 0; i < num_threads; i++) {
        threads.emplace_back(&ThreadPool::worker, this, i);
    }
}
```

### 为什么 4 个线程、3 个 NPU 核心？

| Worker ID | NPU 核心 | YOLO 实例 | 绑定的 CPU |
|-----------|---------|-----------|-----------|
| 0 | Core 0 | yolo_group[0] | CPU4 (大核) |
| 1 | Core 1 | yolo_group[1] | CPU5 (大核) |
| 2 | Core 2 | yolo_group[2] | CPU6 (大核) |
| 3 | Core 0 | yolo_group[0] (轮询复用) | CPU7 (大核) |

RK3588 有 3 个 NPU 核心，但线程池有 4 个 Worker。`i % 3` 让 Worker 3 复用最空闲的 Core 0。这样的设计背景是：NPU 推理是硬件操作，不占 CPU，所以 4 个线程共享 3 个 NPU 核心不会显著竞争——大部分时间 Worker 在等待 NPU 完成，而 NPU 本身有硬件调度器。

### YOLO 实例为什么不能共享？

```cpp
// 每个 Worker 拿自己的实例
std::shared_ptr<Yolov5s> yolo = this->yolo_group[id];
```

RKNN 上下文（`rknn_context`）**不是线程安全的**。如果 4 个线程共享一个 YOLO 实例，`rknn_run` 会串行化——相当于退化为单核推理。每个 Worker 持有独立实例才能实现真正的并行。

---

## 任务提交：submit_to_worker

**相关代码：** `thread_pool.cpp:82-99`

```cpp
void ThreadPool::submit_to_worker(int worker_id, int index, const cv::Mat& img,
                                   int width, int height)
{
    Task t;
    t.index  = index;
    t.img    = img;
    t.width  = width;
    t.height = height;

    tasks_in_flight++;

    // 直接入队目标 Worker 的私有队列（只锁该队列的 mutex）
    {
        lock_guard<mutex> lock(worker_ctx[worker_id].mtx);
        worker_ctx[worker_id].tasks.push(std::move(t));
    }
    worker_ctx[worker_id].cv.notify_one();
}
```

### 提交流程

```
1. 构建 Task → 填充 index / img / width / height（不再传递 nv12_data）
2. tasks_in_flight++ ← 在途任务计数
3. 直接入队目标 Worker 的私有队列 ← 只锁该队列的 mutex
4. 通知特定 Worker ← worker_ctx[worker_id].cv.notify_one()
5. 无返回值 ← Aggregator 通过 try_get_result() 收集结果
```

### 无 Future 的设计动机

| 对比维度 | 旧版 std::future | 新版 result_queue |
|---------|-------------|--------------|
| 堆分配 | 每次 submit 一次 promise 状态分配 | 零额外分配 |
| 同步开销 | std::promise::set_value 含原子 CAS | mutex lock（已有队列锁） |
| 轮询开销 | wait_for(0) 系统调用 | 检查队列 front |
| 批次收集 | 必须逐帧检查 | 一次 drain 全部 |

### 限流机制

重构后 ThreadPool 不再需要内部限流（`tasks.size() > 10` sleep），因为：

1. **Aggregator 限流**：`tasks_inflight.size() < 20` 已在 Aggregator 侧控制
2. ****is_busy 保护**：ReadVideo 中检测 Worker 繁忙时直接标记 `skip_inference=true`，不会继续提交
3. **4 独立队列**：每个 Worker 队列只处理自己的帧，不会出现单队列堆积

```
g_SafeQueueRead  ──►  Aggregator  ──►  ThreadPool
    (limit 100)           │                │
                    tasks_inflight      每 Worker 独立队列
                    (limit 20)          (无需单独限流)
```

---

## Worker 主循环

**相关代码：** `thread_pool.cpp:135-208`

### 结构概览

```
worker(id)
    │
    ├── 线程命名 + CPU 绑核（CPU4-7 A76 大核）
    │
    ├── while (run_flag)
    │   │
    │   ├── [加锁] 从自己的私有队列取任务
    │   │   ├── 无任务且 run_flag=false → break（线程退出）
    │   │   └── 有任务 → move 取出
    │   │
    │   ├── [无锁] 执行推理
    │   │   ├── rknn_run 零拷贝推理 ← NPU 输入已由 ReadVideo 填好
    │   │   ├── draw_result 画框
    │   │   └── BGR→NV12 转换（编码用）
    │   │
    │   ├── catch 异常 → 标记失败 + 释放内存
    │   │
    │   ├── result_queue.push({t.index, res}) → Aggregator 收集
    │   └── worker_ctx[id].is_busy = false → 解锁 NPU 缓冲区
    │
    └── 线程退出日志
```

**重构变化：**
1. **NV12→RGB RGA 已移除**（移至 ReadVideo Path C）
2. **`free(t.nv12_data)` 已移除**（不再需要输入 NV12 拷贝）
3. **`promise.set_value()` 替换为 `result_queue.push()`**（无堆分配）
4. **新增 `is_busy.store(false)`**：通知 ReadVideo 可以写入新帧到 NPU 缓冲区

### 绑核逻辑

```cpp
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
int target_cpu = 4 + (id % 4);  // Worker 0→CPU4, Worker 1→CPU5, ...
CPU_SET(target_cpu, &cpuset);
pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
```

RK3588 的 CPU 架构：

| CPU 核心 | 架构 | 用途 |
|---------|------|------|
| CPU0-3 | Cortex-A55（小核） | 操作系统、中断处理、IO |
| CPU4-7 | Cortex-A76（大核） | **Worker 线程**、计算密集型任务 |

小核省电但性能有限，大核性能强。Worker 的 RGA 调用、后处理计算都需要 CPU 参与，绑定大核能减少调度延迟和迁移开销。

### 取任务：每 Worker 独立队列 + 条件变量等待

```cpp
{
    unique_lock<mutex> lock(worker_ctx[id].mtx);
    worker_ctx[id].cv.wait(lock, [this, id]{
        return (!worker_ctx[id].tasks.empty() || !run_flag);
    });

    if (!run_flag && worker_ctx[id].tasks.empty()) {
        break;  // 线程池析构时优雅退出
    }

    if (worker_ctx[id].tasks.empty()) continue;  // 虚假唤醒防护

    t = move(worker_ctx[id].tasks.front());
    worker_ctx[id].tasks.pop();
}
// 锁在此 release，后续处理无需加锁
```

**为什么把锁限制在这个小作用域？**

```cpp
{  // ← 最小化锁范围
    unique_lock<mutex> lock(worker_ctx[id].mtx);
    t = move(worker_ctx[id].tasks.front());
    worker_ctx[id].tasks.pop();
}
// ← 锁在此释放
// 后续推理完全不持有锁，其他 Worker 可以并发入队/出队
```

只保护队列操作，不保护推理执行——这是线程池的性能关键。

### Worker 中的虚假唤醒：为什么需要两重检查？

```cpp
// 第一重：predicate 检查（条件变量内置）
worker_ctx[id].cv.wait(lock, [this, id]{
    return (!worker_ctx[id].tasks.empty() || !run_flag);
});

// 第二重：显式检查（兜底）
if (worker_ctx[id].tasks.empty()) continue;
```

两重检查风格不同但目的一致——防止 `wait` 返回时队列仍为空（虚假唤醒）。predicate 形式已经做了等效的 `while` 循环，但加上显式检查是防御式编程的风格。

### 结果回传与缓冲区解锁

```cpp
// 结果放入公共收集队列
{
    lock_guard<mutex> lock(result_mtx);
    result_queue.push({t.index, std::move(res)});
}
tasks_in_flight--;

// NPU 输入缓冲区已使用完毕，允许 ReadVideo 写入新帧
worker_ctx[id].is_busy.store(false);
```

关键行为：
- **`result_queue.push`**：将 `{index, ProcessResult}` 入公共队列，Aggregator 通过 `try_get_result()` 收集
- **`tasks_in_flight--`**：递减在途任务计数
- **`is_busy.store(false)`**：释放 NPU 缓冲区锁，ReadVideo 的下一轮 round-robin 可以再次使用该 Worker

---

## 线程池析构

**相关代码：** `thread_pool.cpp:28-45`

```cpp
ThreadPool::~ThreadPool() {
    this->run_flag = false;

    // 逐一唤醒所有 Worker（各 Worker 自己的条件变量）
    for(size_t i = 0; i < threads.size(); i++) {
        worker_ctx[i].cv.notify_all();
    }

    // 等待所有 Worker 退出
    for (thread &t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
}
```

**为什么用 `notify_all` 而不是 `notify_one`？** 当线程池析构时，可能有多个 Worker 都在等待任务，每个都需要被唤醒来检查 `run_flag`。`notify_one` 只能唤醒一个，其他 Worker 会一直阻塞——`~ThreadPool()` 的 `join()` 就永远等不到了。

**为什么用索引循环而不是 range-for？** `worker_ctx` 的类型是 `std::unique_ptr<WorkerContext[]>`，不支持 range-for 迭代，只能用索引循环。

### Worker 的退出判断

```cpp
// Worker 中
while (run_flag) {
    // ...
    worker_ctx[id].cv.wait(lock, [this, id]{
        return (!worker_ctx[id].tasks.empty() || !run_flag);
    });

    if (!run_flag && worker_ctx[id].tasks.empty()) {
        break;  // 析构了 + 无残留任务 → 退出
    }
    // 有任务 → 继续处理
}
```

这里的设计很精细：`!run_flag && tasks.empty()` 时才退出——如果线程池析构时队列中还有未处理的任务，Worker 不会立即退出，而是继续处理完残留任务后再退出。

---

## 完整时序：从提交到结果（新版）

```
Aggregator                          ThreadPool                  Worker
    │                                   │                          │
    │ submit_to_worker(id=1, ...)       │                          │
    │                                   │                          │
    │  lock(worker_ctx[1].mtx)          │                          │
    │  worker_ctx[1].tasks.push(move(t))│                          │
    │  unlock                           │                          │
    │  worker_ctx[1].cv.notify_one() ───┼─────────────────────────►│
    │                                   │                          │
    │ (无返回值，无 future 分配)         │   cv.wait()               │
    │                                   │   lock(worker_ctx)       │
    │                                   │   t = move(front)        │
    │                                   │   pop()                  │
    │                                   │   unlock                 │
    │                                   │                          │
    │  try_get_result() ◄───────────────┤   rknn_run 推理           │
    │                                   │   draw_result            │
    │  result_buffer[wr.index]          │   BGR→NV12               │
    │                                   │   is_busy = false        │
    │                                   │                          │
    │  find(nextWriteIndex)             │   lock(result_mtx)       │
    │  → 按序写出                       │   result_queue.push()    │
    │                                   │   unlock                 │
```

---

## 对比通用线程池

本项目中的 ThreadPool 是为 YOLOv5s 推理**定制**的，并非通用线程池：

| 特性 | 本项目 ThreadPool | 通用线程池 |
|------|------------------|-----------|
| Worker 绑核 | 固定 CPU4-7 | 通常不绑核 |
| 任务类型 | 固定：YOLOv5s 推理 + 绘制 | 任意 callable |
| YOLO 实例 | 每个 Worker 独享 | 不涉及 |
| 返回值 | 固定 `ProcessResult` | 模板化 |
| 限流 | 通过 `is_busy` + Aggregator 控制 | 通常依赖上游控制 |
| 结果收集 | `result_queue` + `try_get_result()` 批量 drain | 通常使用 `future` |
| 退出 | `run_flag` 原子变量 + 每个 Worker `cv.notify_all()` | 类似 |
| 内存保护 | `is_busy` 原子锁防止 NPU 缓冲区踩踏 | 不涉及 |

**为什么不直接用 `std::async` 或通用线程池？**

1. **需要控绑定** — Worker 必须绑定到指定 CPU 和 NPU 核心，通用线程池不支持
2. **需要独享 YOLO 实例** — 每个 Worker 需要自己的 `rknn_context`，不能共享
3. **需要预测执行** — RGA handle 等资源在线程内创建和销毁，线程复用减少开销

---

## 总结

ThreadPool 的设计模式：

```
生产者 (Aggregator)
    │  submit_to_worker(target, index, img, w, h)
    │
    ┌─────┼─────┐
    │ W0  │ W1  │ W2  W3  ← 每 Worker 独立队列
    │队列 │队列 │队列 队列 ← 各自 mutex + cv
    └─────┼─────┘
    │  worker() 循环
    ▼
┌────┬────┬────┬────┐
│ W0 │ W1 │ W2 │ W3 │  ← 各持独立 YOLO 实例
│NPU0│NPU1│NPU2│NPU0│  ← 轮询 3 个 NPU 核心
│CPU4│CPU5│CPU6│CPU7│  ← 绑定 A76 大核
│is_busy │is_busy│...│  ← 原子锁防踩踏
└────┴────┴────┴────┘
    │
    ▼
结果收集队列 (result_queue)
    │  try_get_result() 批量 drain
    ▼
消费者 (Aggregator)
    │  result_buffer[index] 重排 → nextWriteIndex 按序写出
```

四个核心设计决策：

1. **独享 YOLO 实例** — 每个 Worker `yolo_group[id]`，避免 RKNN 上下文竞争
2. **每 Worker 独立队列** — 4 个队列各自加锁，消除旧版单队列锁竞争
3. **无 Future 结果收集** — `result_queue` + `try_get_result()` 批量 drain，消除堆分配和轮询
4. **`is_busy` 原子锁** — 硬件并发内存踩踏保护 + 主动丢帧策略
