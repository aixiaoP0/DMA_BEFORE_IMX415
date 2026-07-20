# C++ 并发与内存管理语法解析（基于新架构代码）

本文档以本项目代码为基础，系统讲解 C++ 中与多线程编程相关的核心语法。每个知识点都附带**代码中实际出现的位置**和**逐行拆解**。

---

## 一、互斥锁：`std::mutex` / `std::lock_guard` / `std::unique_lock`

### 1.1 为什么需要锁？

本项目是多线程架构：ReadVideo、Aggregator、Workers、WriteVideo 四个角色同时运行，多个线程可能同时访问同一个队列。例如 Worker 在 `pop` 任务的同时 Aggregator 在 `push` 任务——没有锁保护，队列的内部指针和计数会错乱，导致 crash 或数据丢失。

### 1.2 `std::lock_guard<mutex>` — 最简单的 RAII 锁

**代码位置：** `thread_pool.cpp:95-97`

```cpp
{
    lock_guard<mutex> lock(worker_ctx[worker_id].mtx);
    worker_ctx[worker_id].tasks.push(std::move(t));
}
```

**逐行拆解：**

| 代码 | 含义 |
|------|------|
| `{ ... }` | 花括号限定锁的作用域。出了这个花括号，lock_guard 析构，自动释放锁 |
| `lock_guard<mutex> lock(mtx)` | 构造 lock_guard 时自动调用 `mtx.lock()` |
| `}` | lock_guard 析构时自动调用 `mtx.unlock()`，**即使在 push 过程中抛异常** |

**与 `mtx.lock()` / `mtx.unlock()` 手动的对比：**

```cpp
// ❌ 手动 lock/unlock — 如果中间抛异常，锁永远不会释放
mtx.lock();
tasks.push(t);
mtx.unlock();  // 如果 push 抛异常，这行不会执行

// ✅ lock_guard — RAII，异常安全
{ lock_guard<mutex> lock(mtx); tasks.push(t); }
// 无论什么方式离开花括号，锁都会被释放
```

这就是 **RAII**（Resource Acquisition Is Initialization）——C++ 核心设计哲学：资源的获取在构造函数中完成，释放在析构函数中保证执行。

**代码中所有 `lock_guard` 的使用位置：**

| 位置 | 保护内容 |
|------|---------|
| `thread_pool.cpp:95` | `submit_to_worker` — 入队到 Worker 私有队列 |
| `thread_pool.cpp:118` | `try_get_result` — 取结果队列 |
| `SafeQueue.h:55` | `empty()` — 检查队列是否为空 |
| `thread_pool.cpp:198` | Worker 完成后结果入公共队列 |

### 1.3 `std::unique_lock<mutex>` — 更灵活的锁

**代码位置：** `SafeQueue.h:24`, `thread_pool.cpp:156`

`unique_lock` 比 `lock_guard` 多了两个能力：

1. **可以和条件变量配合使用** — `lock_guard` 不支持 `wait()`，因为 `wait` 需要临时解锁再重新锁定
2. **可以在作用域内提前解锁** — 调用 `lock.unlock()` 提前释放，不必等到作用域结束

```cpp
// SafeQueue.h:24 — 入队操作
std::unique_lock<mutex> lock(m);
cond_not_full.wait(lock, [this]{ return q.size() < maxSize; });
q.push(t);
```

```cpp
// thread_pool.cpp:156-165 — Worker 取任务
unique_lock<mutex> lock(worker_ctx[id].mtx);
worker_ctx[id].cv.wait(lock, [this, id] {
    return !worker_ctx[id].tasks.empty() || !run_flag;
});
t = std::move(worker_ctx[id].tasks.front());
```

**`lock_guard` vs `unique_lock` 选择原则：**

| 场景 | 用哪个 | 原因 |
|------|--------|------|
| 仅需要上锁/解锁，不涉及条件变量 | `lock_guard` | 更轻量，语义明确 |
| 需要条件变量 `wait` | `unique_lock` | `lock_guard` 不提供 `wait` 接口 |
| 需要在临界区中间提前解锁 | `unique_lock` | 可调用 `unlock()` |

> **面试追问：既然 `lock_guard` 更轻量，为什么条件变量不用它？**
> 因为 `condition_variable::wait` 需要先解锁（让其他线程能拿到锁去修改条件），等待被唤醒后再重新锁定。这个"解锁→等待→重新锁定"的三步序列只有 `unique_lock` 支持，`lock_guard` 没有提供操作底层 mutex 的接口。

---

## 二、条件变量：`std::condition_variable`

### 2.1 为什么需要条件变量？

如果线程只是用锁 + 循环来等待条件：

```cpp
// ❌ 自旋等待 — CPU 占用 100%
while(queue.empty()) { /* 空转 */ }

// ❌ sleep 轮询 — 有延迟且 CPU 空转依然存在
while(queue.empty()) { std::this_thread::sleep_for(1ms); }
```

条件变量的作用是：让线程**阻塞等待**某个条件变为 true，条件不满足时线程进入休眠（不占 CPU），条件满足时被唤醒。

### 2.2 SafeQueue 中的双条件变量

**代码位置：** `SafeQueue.h:21-44`

本项目中的线程安全队列使用了**两个**条件变量——这是一种比单条件变量更高性能的设计：

```cpp
// SafeQueue.h — 核心成员
condition_variable cond_not_empty;  // 等待队列不为空（dequeue 用）
condition_variable cond_not_full;   // 等待队列不满（enqueue 用）
mutex m;
```

**入队（生产者）：**

```cpp
void enqueue(const T& t) {
    unique_lock<mutex> lock(m);
    cond_not_full.wait(lock, [this]{ return q.size() < maxSize; });
    // ↑ 队列满了？阻塞在这里，直到有人 dequeue 后唤醒我
    
    q.push(t);
    cond_not_empty.notify_all();
    // ↑ 通知所有在等"队列非空"的消费者
}
```

**出队（消费者）：**

```cpp
bool dequeue(T& t) {
    unique_lock<mutex> lock(m);
    cond_not_empty.wait(lock, [this]{ return !q.empty(); });
    // ↑ 队列空了？阻塞在这里，直到有人 enqueue 后唤醒我
    
    t = q.front();
    q.pop();
    cond_not_full.notify_all();
    // ↑ 通知所有在等"队列不满"的生产者
}
```

**为什么用两个条件变量而不是一个？**

| 方案 | 行为 |
|------|------|
| 单 `cond_var` | `notify_all()` 唤醒**所有**等待线程，包括不该被唤醒的那一方——被唤醒的线程发现条件不满足，继续 wait |
| 双 `cond_var` | `cond_not_empty` 只唤醒消费者，`cond_not_full` 只唤醒生产者，**无无效唤醒** |

这就是**双条件变量模式**——比单条件变量更高效，比无锁队列更简单。

### 2.3 虚假唤醒与 predicate wait

```cpp
cond_not_full.wait(lock, [this]{ return q.size() < maxSize; });
```

这个写法等价于：

```cpp
while (q.size() >= maxSize) {
    cond_not_full.wait(lock);  // 可能被虚假唤醒！
}
```

**为什么需要 `while` 循环或 predicate？** 因为条件变量存在"虚假唤醒"（spurious wakeup）——即使没有 `notify`，`wait` 也可能返回。如果没有循环检查条件，虚假唤醒后就会在队列满时错误地 push 数据。

> **标准规定：** POSIX 和 C++ 标准都允许条件变量发生虚假唤醒。所以永远要用 `while` 或 predicate 包装 `wait` 调用。

### 2.4 Worker 中的条件变量

**代码位置：** `thread_pool.cpp:157-158`

```cpp
worker_ctx[id].cv.wait(lock, [this, id] {
    return !worker_ctx[id].tasks.empty() || !run_flag;
});
```

这里的 waiting predicate 做了两件事：
- **正常情况**：等待 `tasks` 不为空（有任务来了）
- **退出情况**：等待 `run_flag` 变为 false（线程池要关了）

### 2.5 `notify_one` vs `notify_all`

| 函数 | 唤醒数量 | 适用场景 |
|------|---------|---------|
| `notify_one()` | 唤醒一个等待线程 | 只唤醒一个 Worker 取任务（`submit_to_worker` 中一个 Worker 的私有队列来任务了） |
| `notify_all()` | 唤醒所有等待线程 | 队列满了/空了需要通知所有等待的线程（SafeQueue 中同一条件可能有多方等待） |

本项目中的选择：

| 位置 | 用哪个 | 原因 |
|------|--------|------|
| `SafeQueue::enqueue` | `notify_all` | 可能有多个 dequeue 线程在等 |
| `SafeQueue::dequeue` | `notify_all` | 可能有多个 enqueue 线程在等 |
| `submit_to_worker` | `notify_one` | 每个 Worker 只有 1 个线程等在自己的私有队列上 |

---

## 三、生产者-消费者模型

### 3.1 贯穿整条管线的模式

本项目的整个数据流就是一条"生产者-消费者"链：

```
ReadVideo ──(生产帧)──→ SafeQueueRead ──(消费/生产)──→ Worker[i] ──(生产结果)──→ result_queue ──(消费)──→ SafeQueueWrite ──(消费)──→ WriteVideo
  生产者        有界队列       消费者兼生产者    独立队列       消费者兼生产者   收集队列     消费者兼生产者  有界队列        消费者
```

### 3.2 三个独立的生产者-消费者环节

#### 环节一：ReadVideo → SafeQueueRead → Aggregator

- **生产者**：ReadVideo 线程 → 每帧 `enqueue(frame_temp)`
- **有界队列**：`SafeQueue<FrameData> g_SafeQueueRead(100)` — 容量 100，满了就阻塞生产者
- **消费者**：Aggregator 线程 → `dequeue(inputFD)`

这是标准的"单一生产者、单一消费者"（SPSC）场景，但 SafeQueue 的设计可以处理多生产者多消费者（MPMC）。

#### 环节二：Aggregator → per-worker 队列 → Worker[i]

- **生产者**：Aggregator 线程 → `submit_to_worker(id, ...)` → 入队到 `worker_ctx[id].tasks`
- **队列**：`std::queue<Task>` + `mutex` + `condition_variable` — 每个 Worker 私有
- **消费者**：Worker[i] 线程 → 从自己的 `tasks` 出队

这是"**单一生产者、单一消费者**"（SPSC）——Aggregator 是唯一的提交者，每个 Worker 只消费自己的队列。没有锁竞争（`mutex` 只是为了防止 CPU 缓存行问题，不构成竞争热点）。

#### 环节三：Worker[i] → result_queue → Aggregator

- **生产者**：各 Worker → `result_queue.push({t.index, std::move(res)})`（需要加锁）
- **队列**：`std::queue<WorkerResult>` + `mutex` 保护
- **消费者**：Aggregator → `try_get_result(wr)` 批量 drain

这是"**多生产者、单一消费者**"（MPSC）——所有 Worker 向同一个队列写结果，但只有 Aggregator 在读取。需要注意 `result_mtx` 的竞争。

### 3.3 有界队列 vs 无界队列

| 类型 | 本项目使用 | 特性 |
|------|-----------|------|
| 有界队列 | `SafeQueue`（容量 100） | 生产者满时阻塞，天然反压 |
| 无界队列 | per-worker `queue<Task>` | 生产者不会等待，但可能堆积 |

`SafeQueue` 的有界设计体现了一个关键工程决策：**视频管线中生产速度 > 消费速度时，必须阻塞生产者（ReadVideo）来防止内存无限增长**。如果生产端不阻塞，`SafeQueue` 的缓冲 100 帧后会溢出。

> **追问：per-worker `queue<Task>` 为什么不做有界？**
> 因为每个 Worker 独立队列中的任务数最多 = `in_flight_count` / `g_num_workers` ≈ 5 帧。4 个 Worker 各 5 帧 = 20 帧在途，已经由 `in_flight_count < 20` 在 Aggregator 层面限流了。

---

## 四、原子操作：`std::atomic`

### 4.1 为什么需要 atomic？

```cpp
// ❌ 非原子操作 — 可能读到中间值
bool is_busy = false;  // 线程1写，线程2读
// 线程1: is_busy = true;  → CPU 可能只写入了 1 个字节，线程2读到部分更新的值
// 线程2: if (is_busy) → 读到的是垃圾值
```

`std::atomic` 保证：**读或写操作是原子的（不可分割的），且对内存序有明确的约定**。

### 4.2 代码中的 atomic 变量

```cpp
// main.cpp
std::atomic<bool> g_readFinish(false);     // ReadVideo 线程完成
std::atomic<bool> g_processFinish(false);  // Aggregator 线程完成

// thread_pool.h
std::atomic<bool> is_busy{false};          // NPU 缓冲区是否被占用
struct WorkerContext { ... atomic<bool> is_busy{false}; };

// thread_pool.cpp
atomic<int> tasks_in_flight{0};            // 进行中的任务数
```

### 4.3 为什么这些 flag 用 atomic 而不是 mutex？

**只读/只写一个 bool 或 int，用 mutex 太重量级了。** `atomic` 在 x86 上通常编译为一条 `LOCK CMPXCHG` 指令（几纳秒），而 mutex 涉及系统调用或用户态 futex（几十到几百纳秒）。

```cpp
// atomic 版本 — 一条 CPU 指令
is_busy.store(true);

// mutex 版本 — 杀鸡用牛刀
{ lock_guard<mutex> lock(busy_mtx); is_busy = true; }
```

### 4.4 `load()` 和 `store()` 显式调用

```cpp
// thread_pool.h:97-98
bool is_worker_busy(int worker_id) { return worker_ctx[worker_id].is_busy.load(); }
void set_worker_busy(int worker_id, bool state) { worker_ctx[worker_id].is_busy.store(state); }
```

`atomic` 禁用了隐式的拷贝赋值和拷贝构造，强制开发者用 `load()` 和 `store()` 读写。这实际上是设计上的优点——**让你意识到这里正在进行跨线程访问**。

### 4.5 内存序

`load()` 和 `store()` 默认使用 `std::memory_order_seq_cst`（顺序一致性），这是最严格的内存序，确保所有线程观察到一致的执行顺序。

```
线程1:                    线程2:
is_busy.store(true)       if (is_busy.load())
                              // 一定能看到 store 的结果
```

如果出于性能考虑可以放宽到 `memory_order_acquire/release`，但本项目默认使用最安全的顺序一致性。

### 4.6 `tasks_in_flight` 的原子增减

```cpp
// thread_pool.cpp
tasks_in_flight++;          // submit_to_worker 中
tasks_in_flight--;          // worker 完成后
```

`++` 和 `--` 操作符也支持 `atomic`，内部使用原子 RMW（Read-Modify-Write）指令，保证多线程安全。

---

## 五、智能指针：`std::shared_ptr` / `std::unique_ptr`

### 5.1 Raw Pointer 的问题

```cpp
// ❌ 裸指针 — 谁负责释放？不确定
Yolov5s* yolo = new Yolov5s(model_path, i%3);
delete yolo;  // 忘了 delete？double delete？内存泄漏！
```

智能指针是 C++ 的"带自动析构的指针"——**资源的所有权被编码在类型系统中，由 RAII 自动管理生命周期**。

### 5.2 `std::shared_ptr` — 共享所有权

**代码位置：** `thread_pool.h:112`, `thread_pool.cpp:62`

```cpp
// thread_pool.h — 成员声明
vector<shared_ptr<Yolov5s>> yolo_group;

// thread_pool.cpp — 初始化
for(size_t i = 0; i < num_threads; i++) {
    std::shared_ptr<Yolov5s> yolo = std::make_shared<Yolov5s>(model_path, i % 3);
    yolo_group.emplace_back(yolo);
}
```

**为什么用 `shared_ptr` 而不是 `unique_ptr`？**

因为 `Yolov5s` 实例在代码中除了被 Worker 持有外，它的 NPU 内存 fd 还需要被 ReadVideo 线程通过 `get_worker_input_fd()` 访问：

```
yolo_group → shared_ptr<Yolov5s>[0] ← Worker 0 持有（推理）
           → shared_ptr<Yolov5s>[1] ← Worker 1 持有
           → shared_ptr<Yolov5s>[2] ← Worker 2 持有
           → shared_ptr<Yolov5s>[3] ← Worker 3 持有
           ThreadPool 作为容器持有所有 shared_ptr

get_worker_input_fd(0) → 返回 yolo_group[0]->input_mem->fd
                         ↑ ReadVideo 线程通过这个 fd 做 RGA 直写
```

如果用 `unique_ptr`，`yolo_group` 就独占所有权，无法安全地传递给其他上下文。虽然这里实际上 ThreadPool 是唯一的生命周期管理者，`shared_ptr` 给了更多灵活性——如果将来需要把 yolo 实例传递给其他模块，引用计数会自动管理。

**`std::make_shared` 优于 `new`：**

```cpp
// ✅ 推荐 — make_shared
auto yolo = std::make_shared<Yolov5s>(model_path, i%3);

// ❌ 不推荐 — new + shared_ptr 构造（两次内存分配）
std::shared_ptr<Yolov5s> yolo(new Yolov5s(model_path, i%3));
```

`make_shared` 一次性分配控制块和对象的内存，而 `new` + `shared_ptr` 构造需要两次独立分配。

### 5.3 `std::unique_ptr` — 唯一所有权

**代码位置：** `thread_pool.h:115`, `thread_pool.cpp:58`

```cpp
// thread_pool.h — 成员声明
std::unique_ptr<WorkerContext[]> worker_ctx;

// thread_pool.cpp — 初始化
worker_ctx.reset(new WorkerContext[num_threads]);
```

**为什么 `WorkerContext` 用 `unique_ptr` 而不是 `vector`？**

因为 `WorkerContext` 内部有 `mutex`，而 `std::mutex` **不可拷贝、不可移动**。`vector` 要求元素可移动，而 `mutex` 的存在使 `WorkerContext` 无法移动。

```cpp
struct WorkerContext {
    std::queue<Task> tasks;
    mutex mtx;           // ← mutex 不能 move！
    condition_variable cv;  // ← cv 也不能 move！
};

// ❌ 这会编译失败 — vector 需要移动元素
vector<WorkerContext> ctx(num_threads);

// ✅ 用 unique_ptr 数组 — 在堆上连续分配，通过指针访问，不要求元素可移动
std::unique_ptr<WorkerContext[]> worker_ctx;
worker_ctx.reset(new WorkerContext[num_threads]);
// 效果等价于栈上的 WorkerContext ctx[N]，但分配在堆上
```

**`unique_ptr` 没有拷贝语义，只有移动语义：**

```cpp
auto p1 = std::make_unique<WorkerContext[]>(4);
// auto p2 = p1;     // ❌ 编译错误 — unique_ptr 不能拷贝
auto p2 = std::move(p1);  // ✅ 转移所有权，p1 变为 nullptr
```

### 5.4 智能指针与裸指针的混用

```cpp
// Yolov5s 类的公共接口返回裸指针（不是智能指针）
int Yolov5s::my_get_input_fd() { return input_mem->fd; }
```

这里 `input_mem` 是 `rknn_tensor_mem*` 裸指针（由 `rknn_create_mem` 分配），返回 `input_mem->fd` 是为了让 RGA 拿到 NPU 内存的 DMABUF fd。**fd 是整数，不存在生命周期问题。** 返回裸指针/整数值是安全的——ReadVideo 只使用 fd 这个数值，不操作 `input_mem` 指针指向的内存。

**智能指针的使用原则：**

| 规则 | 说明 |
|------|------|
| 对象的所有权归属清晰时用 `unique_ptr` | `WorkerContext` 归 ThreadPool 唯一所有 |
| 对象有多个所有者时用 `shared_ptr` | YOLO 实例被 ThreadPool 持有，fd 被外部读取 |
| 只借用不持有用裸指针或引用 | `get_worker_input_fd` 返回 int（不是指针） |
| 永远不用 `new`/`delete` | 用 `make_shared`/`make_unique` |

---

## 六、移动语义与 `std::move`

### 6.1 为什么需要 move？

```cpp
// Task 结构体包含 cv::Mat（内部有指针指向堆内存）
struct Task {
    cv::Mat img;  // Mat 的拷贝是深拷贝（复制像素数据）
};

// 拷贝版本：把 t 复制 3MB 像素数据
tasks.push(t);  // 调用 cv::Mat 拷贝构造，深拷贝

// 移动版本：把 t 的资源指针转移给队列中的新元素
tasks.push(std::move(t));  // t.img 变为空 Mat，数据指针被转移
```

**拷贝是昂贵的，移动是廉价的。**

### 6.2 代码中的 move 使用

```cpp
// thread_pool.cpp:96 — Task 不可拷贝（包含 promise）
tasks.push(std::move(t));

// thread_pool.cpp:163 — Worker 取任务时转移所有权
t = std::move(worker_ctx[id].tasks.front());

// thread_pool.cpp:120 — 结果队列转移
wr = std::move(result_queue.front());

// main.cpp:341 — 空白结果转移
result_buffer[inputFD.index] = std::move(blank);
```

注意 `thread_pool.h:40` 中的 Task 结构体**已经简化，没有 promise 成员**（新架构移除了 future/promise），所以 `std::queue<Task>` 可以正常使用 `push(std::move(t))`。

**移动后的对象处于"有效但未指定"的状态**——典型实现中，被移动的 `cv::Mat` 会变为空 Mat（`data == nullptr`），被移动的 `string` 会变为空字符串。项目中利用这一点：移动后将原对象视为不再使用，不执行额外清理。

### 6.3 `std::move` 只是一种类型转换

```cpp
// std::move 的实际作用——把左值引用转换为右值引用
static_cast<typename remove_reference<T>::type&&>(t);
```

它**不移动任何东西**，只是让编译器的重载决议选择"移动版本"的构造函数/赋值运算符。

---

## 七、`std::thread` 与线程管理

### 7.1 线程创建

**代码位置：** `main.cpp:601-611`

```cpp
std::thread video_reader(Thread_ReadVideo, ref(state), ref(fd),
                          ref(g_SafeQueueRead), ref(index),
                          ref(cap_m), ref(g_readFinish));
std::thread video_process(aggregatorThreadFunc, ref(gthreadpool));
std::thread video_writer.emplace_back(Thread_WriteVideo, ref(writer));
```

`std::thread` 的构造函数接受一个可调用对象 + 参数列表。参数默认以**值传递**方式拷贝到线程内部——这意味着：

```cpp
// ❌ 这样写不通过 — thread 默认拷贝参数
thread t(func, state);  // 试图拷贝 state（可能是大对象或不可拷贝）

// ✅ 用 std::ref 包装为引用传递
thread t(func, ref(state));  // 传递引用包装器，内部存储引用
```

**为什么线程参数默认拷贝？** 线程可能比创建它的函数活得久，如果传引用而原函数退出了，引用悬空。用 `ref` 表明开发者明确知道线程生命周期内引用有效。

### 7.2 `join()` — 等待线程结束

```cpp
video_reader.join();       // 阻塞直到 ReadVideo 退出
for (thread& t : video_writer) { t.join(); }
video_process.join();      // 主线程阻塞，等待处理线程退出
```

`join()` 是线程同步的最基本形式——**确保线程退出后才能释放其持有的资源**。如果主函数直接 return 而不 join，程序会调用 `std::terminate()` 终止进程。

### 7.3 POSIX 线程扩展

```cpp
// main.cpp — 设置线程名（便于 gdb/perf top 识别）
pthread_setname_np(pthread_self(), "ReadVideo");

// thread_pool.cpp — 绑核到特定 CPU
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(target_cpu, &cpuset);
pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
```

这些是 POSIX 线程的扩展，`std::thread` 没有直接提供名字设置和 CPU 亲和性绑定，所以项目中用 `pthread_self()` 获取底层 pthread_t 句柄后调用。

---

## 八、模板：`SafeQueue<T>`

**代码位置：** `SafeQueue.h`

```cpp
template<typename T>
class SafeQueue {
    void enqueue(const T& t);
    bool dequeue(T& t);
    bool empty();
    void stop();
private:
    queue<T> q;
    mutable mutex m;
    condition_variable cond_not_empty;
    condition_variable cond_not_full;
    size_t maxSize;
    bool stop_flag = false;
};
```

**模板让队列可以适配任意类型：**

```cpp
SafeQueue<FrameData> g_SafeQueueRead(100);   // 存帧数据的队列
SafeQueue<FrameData> g_SafeQueueWrite(100);  // 同样类型但用途不同
```

`template<typename T>` 声明了一个**类型参数**——实例化时编译器会为 `T=FrameData` 生成一份 `SafeQueue` 的完整代码。

### `mutable mutex m` 的 `mutable` 关键字

```cpp
mutable mutex m;  // mutable 允许在 const 成员函数中修改
bool empty() const { lock_guard<mutex> lock(m); return q.empty(); }
```

`empty()` 被声明为 `const`（不修改对象状态），但它需要 `lock(m)`——而 `lock` 会修改 `m` 的内部状态（上锁/解锁）。`mutable` 告诉编译器：`m` 虽然是成员变量，但在 `const` 函数中可以修改它。这是 C++ 中为互斥锁设计的惯用模式。

---

## 九、Lambda 表达式

### 9.1 Lambda 是什么

Lambda 表达式（匿名函数）是 C++11 引入的语法，允许在代码中**内联定义一个可调用对象**，无需单独写一个函数或函数对象类。

```cpp
// 完整语法：
[capture](parameters) -> return_type { body }

// 最简形式：
[]{ /* 无参数、无捕获、无返回值 */ }

// 项目中实际使用的形式：
[this]{ return q.size() < maxSize; }
```

Lambda 的核心价值在于：**把一段逻辑直接写在需要它的地方，而不是跳转到别处定义的函数**。这在配合条件变量 `wait` 和 STL 算法时极其有用。

### 9.2 代码中的 Lambda 表达式

本项目中共有 **4 处** lambda 使用，分属两种模式。

#### 模式一：配合条件变量的 predicate（`SafeQueue.h:25/35`, `thread_pool.cpp:157`）

**代码位置：** `SafeQueue.h:25`（入队时的"队列不满"判断）

```cpp
void enqueue(const T& t) {
    std::unique_lock<mutex> lock(m);
    cond_not_full.wait(lock, [this]{ return q.size() < maxSize; });
    q.push(t);
    cond_not_empty.notify_all();
}
```

**捕获列表解析：** `[this]`

| 捕获方式 | 含义 | 等价写法 |
|----------|------|---------|
| `[this]` | 以引用捕获当前对象的 `this` 指针 | 可以访问 `q`、`maxSize` 等成员变量 |

`cond_not_full.wait(lock, [this]{ return q.size() < maxSize; })` 等价于：

```cpp
cond_not_full.wait(lock, [this]() -> bool { return q.size() < maxSize; });
```

编译器自动推导返回类型为 `bool`。

**代码位置：** `thread_pool.cpp:157-158`（Worker 取任务时的"队列非空或停止"判断）

```cpp
worker_ctx[id].cv.wait(lock, [this, id] {
    return !worker_ctx[id].tasks.empty() || !run_flag;
});
```

**捕获列表解析：** `[this, id]`

| 捕获方式 | 含义 |
|----------|------|
| `[this]` | 引用捕获 `this`，访问 `worker_ctx` 和 `run_flag` 成员 |
| `[id]` | **值捕获** `id`（Worker 编号的副本，int 类型，4 字节） |

为什么 `id` 用值捕获而不是引用捕获？

```cpp
// ✅ 正确 — id 是值捕获，lambda 内部持有一份副本
[this, id]{ return !worker_ctx[id].tasks.empty() || !run_flag; }

// ❌ 危险 — 如果捕获取引用，id 在线程退出时可能悬空
[this, &id]{ ... }  // id 是栈上的局部变量，lambda 延迟执行时地址已失效
```

> **原则：** 对于基本类型（int、bool、指针），**总是用值捕获**。对于 `this`，只能用引用捕获（没法值捕获"对象本身"）。

#### 模式二：STL 算法中的比较器（`post_process.cpp:140`）

**代码位置：** `DMA_旧架构/post_process.cpp:140`

```cpp
sort(p_arr.begin(), p_arr.end(),
     [](const Probarray& a, const Probarray& b) {
         return a.conf > b.conf;  // 降序排序
     });
```

**捕获列表解析：** `[]` — 空捕获，不访问任何外部变量。

这个 lambda 是一个纯函数——只依赖参数，不依赖上下文。这是最简单、最安全的 lambda 形式。

**如果没有 lambda：**

```cpp
// 需要单独定义一个比较函数或函数对象
bool compareByConf(const Probarray& a, const Probarray& b) {
    return a.conf > b.conf;
}
sort(p_arr.begin(), p_arr.end(), compareByConf);
```

lambda 版本更紧凑——比较逻辑直接写在 `sort` 调用中，一目了然。

### 9.3 捕获方式详解

| 捕获写法 | 含义 |
|----------|------|
| `[]` | 不捕获任何变量 |
| `[x]` | 值捕获 `x`（lambda 内持有副本） |
| `[&x]` | 引用捕获 `x` |
| `[this]` | 引用捕获当前对象的 `this` 指针 |
| `[=]` | 所有用到的自动变量均以值捕获 |
| `[&]` | 所有用到的自动变量均以引用捕获 |
| `[this, id]` | 混合捕获：`this` 引用捕获，`id` 值捕获 |

**代码中的捕获选择：**

| 位置 | 捕获 | 原因 |
|------|------|------|
| `SafeQueue.h:25` | `[this]` | 需要访问成员 `q`、`maxSize` |
| `SafeQueue.h:35` | `[this]` | 需要访问成员 `q` |
| `thread_pool.cpp:157` | `[this, id]` | 访问 `worker_ctx`、`run_flag`；`id` 是局部变量 |
| `post_process.cpp:140` | `[]` | 纯函数，只用参数 |

### 9.4 Lambda 与 `std::function` 的区别

```cpp
// Lambda — 编译期生成匿名 functor，类型唯一，一般比 function 更高效
auto pred = [this]{ return q.size() < maxSize; };

// std::function — 类型擦除包装，可以存任何可调用对象，有虚函数开销
std::function<bool()> pred = [this]{ return q.size() < maxSize; };
```

本项目中的 lambda **全部直接传递给模板参数**（`wait` 的模板参数、`sort` 的模板参数），编译器内联展开，**零运行时开销**。

### 9.5 Lambda 与条件变量的配合深究

`condition_variable::wait(lock, predicate)` 的内部行为：

```cpp
// 标准库中的 wait 实现大致如下：
template<typename Predicate>
void wait(unique_lock<mutex>& lock, Predicate pred) {
    while (!pred()) {  // ← 这里调用 lambda
        wait(lock);    // 条件不满足，阻塞等待唤醒
    }
}
```

lambda 被作为模板参数 `Predicate` 传入，在 `wait` 内部：

1. **进入 `wait` 时**：调用 `pred()`，返回 false → 阻塞解锁
2. **被 `notify_one/notify_all` 唤醒时**：重新锁定，再次调用 `pred()` 检查条件
3. **条件满足** → `pred()` 返回 true → `wait` 返回，线程继续执行

这个过程中，lambda 捕获的变量（`this`、`id`）被安全地保持在 lambda 对象内部，随 lambda 对象一起传递。

### 9.6 Lambda 的生命周期与安全性

```cpp
// Safe 的 lambda — 生命周期 ≤ 对象生命周期
void SafeQueue::enqueue(const T& t) {
    unique_lock<mutex> lock(m);
    cond_not_full.wait(lock, [this]{ ... });  // this 指向的 SafeQueue 一定存活
    // 因为 enqueue 是 SafeQueue 成员函数，this 自然有效
}

// 也 Safe 的 lambda — 值捕获的 id 是独立副本
thread_pool.cpp:157  // [this, id] — id 是 int 的副本，this 在成员函数内有效
```

**危险场景**：如果 lambda 被存储并在创建它的作用域之外执行：

```cpp
// ❌ 危险 — 引用捕获的局部变量已销毁
auto get_lambda() {
    int x = 42;
    return [&x]{ return x; };  // x 已销毁，返回悬空引用
}
```

本项目中所有 lambda 都是在被创建的那个函数内部立即使用（传给 `wait` 或 `sort`），不会逃逸出作用域，所以不存在生命周期问题。

---

## 十、`std::this_thread::sleep_for` 与空闲等待

**代码位置：** `main.cpp:385-387`

```cpp
if (is_idle) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
```

**为什么 Aggregator 空闲时要 sleep？**

Aggregator 在没有新帧可提交、也没有新结果可收集时是"空闲"的。但循环依然在高速运行：

- 没有 sleep：每秒执行几千万次循环，CPU 占用 100%
- sleep(1ms)：每秒最多执行 1000 次循环，CPU 占用 ≈ 0%

这在运行时不是一个问题——因为只要管线繁忙，`is_idle` 为 false，不会进入 sleep。sleep 只在管线的"间隙期"触发。

---

## 十一、语法特性总结

| 特性 | 头文件 | 本项目用途 |
|------|--------|-----------|
| `std::mutex` | `<mutex>` | 队列操作互斥 |
| `std::lock_guard` | `<mutex>` | RAII 锁定（简单场景） |
| `std::unique_lock` | `<mutex>` | RAII 锁定（配合条件变量） |
| `std::condition_variable` | `<condition_variable>` | 线程间事件通知 |
| `std::atomic<T>` | `<atomic>` | 无锁 flag 和计数器 |
| `std::thread` | `<thread>` | 工作线程创建与管理 |
| `std::ref` | `<functional>` | 传递引用到线程 |
| `std::shared_ptr<T>` | `<memory>` | YOLO 实例共享所有权 |
| `std::unique_ptr<T>` | `<memory>` | WorkerContext 独占所有权 |
| `std::make_shared` | `<memory>` | 安全高效的 shared_ptr 构造 |
| `std::move` | `<utility>` | 移动语义避免拷贝 |
| `std::queue<T>` | `<queue>` | FIFO 队列容器 |
| `std::map<K,V>` | `<map>` | 帧序保序缓存 |
| `mutable` | 关键字 | 允许 const 函数修改 mutex |
| `template<typename T>` | 关键字 | SafeQueue 类型参数化 |
| `this_thread::sleep_for` | `<thread>` | 空闲等待降低 CPU |
| `std::chrono` | `<chrono>` | 高精度时间测量 |
