# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

Rockchip 平台（Radxa RK3588/RK356X）上的嵌入式实时视频分析管道，实现 IMX415 摄像头 → ISP 硬件处理 → YOLOv5s NPU 推理 → RTMP 推流全流程。

## 重要说明：两套架构并存

本仓库包含 **两套架构版本**的代码和文档，用于面试准备中的对比讲解：

### 回答默认规则

> **⚠️ 默认以旧架构为基准回答问题**，除非用户明确说明"基于新架构"。

### 两架构文件位置

| 架构版本 | 代码位置 | 文档位置 | 说明 |
|---------|---------|---------|------|
| **新架构（当前运行版本）** | `E:\DMA/`（根目录） | `E:\DMA\beiwang/` | Path C RGA 直写 NPU、submit_to_worker、result_queue、is_busy、per-worker 独立队列 |
| **旧架构（面试基准版本）** | `E:\DMA\DMA_旧架构/` | `E:\DMA\DMA_旧架构\beiwang/` | Path B malloc+memcpy、future/promise、SharedQueue、Worker 内 NV12→RGB |

### 旧架构核心差异（面试重点）

旧架构在 `DMA_旧架构/` 中保留了**重构前的面试基准版本**，与新架构的关键差异：

```
旧架构的 ReadVideo 线程:
  Path A: RGA NV12→BGR（画框）
  Path B: malloc + memcpy NV12 拷贝 → FrameData.nv12_data
           ↑ 这就是面试中讨论的那次"唯一不可避开的 CPU 拷贝"

旧架构的线程池:
  submit_task_async() → future<ProcessResult> ← 使用 future/promise 异步回传结果
  SharedQueue<Task> 全局共享任务队列          ← 所有 Worker 竞争取任务
  Worker 内部: NV12→RGB（importbuffer_virtualaddr + imresize）
  
旧架构的 Aggregator:
  future.get() 阻塞等待结果 ← 可能 head-of-line blocking
```

| 对比项 | 旧架构（面试基准） | 新架构（当前代码） |
|--------|------------------|------------------|
| NV12→NPU 输入 | Path B: malloc+memcpy 拷贝到堆，Worker 中 RGA 缩放+转换 | Path C: ReadVideo 中 RGA 直接从 DMABUF fd 缩放+转换→直写 NPU fd |
| 任务提交 | `submit_task_async()` + `future<ProcessResult>` | `submit_to_worker(worker_id, ...)` + 独立 per-worker 队列 |
| 结果收集 | `future.get()` 阻塞等待 | `try_get_result()` 非阻塞轮询 + `result_buffer map` 按帧序重排 |
| 任务队列 | 全局 `SharedQueue<Task>`（所有 Worker 竞争） | 每个 Worker 独立 `queue<Task>`（无竞争） |
| 帧保序 | future 阻塞 + nextWriteIndex | map 缓存结果 + nextWriteIndex 顺序写出 |
| 流量控制 | 无主动丢帧 | `is_busy` atomic 标志 + `skip_inference` 丢帧 |
| NV12 拷贝次数 | 2 次（ReadVideo 1次 + Worker 1次） | 1 次（Worker 输出 NV12 给编码器） |
| page-fault | ReadVideo 每帧 malloc 触发（稳态后 Glibc 保护） | ReadVideo 零 malloc（RGA 直写），仅 Worker 输出触发 |

## 构建命令

```bash
# 编译（在板端 aarch64 Linux 上执行）
cd build && cmake .. && make -j4

# 运行
./build/cv
```

构建依赖（板端需安装）：OpenCV、FFmpeg（libavcodec/libavformat/libavutil/libswscale/libavdevice）、Rockchip MPP、V4L2。第三方库在 `3rdparty/` 中包含：librknn_api（NPU SDK）、rga（图像处理库）。

## 代码架构

### 流水线管线（新架构）

```
IMX415 → MIPI CSI-2 → ISP (RKAIQ) → [DMABUF NV12] ───────────────────────────────────────────────────┐
                                                                                                        │
ReadVideo 线程（新架构 ≈ Path C, 旧架构 ≈ Path B）:                                                     │
  新架构:                                                                                                │
  ┌─ 路径 A: RGA importbuffer_fd(DMABUF fd) → imcvtcolor(NV12→BGR) → cv::Mat (画框用)                    │
  ├─ 路径 C: RGA importbuffer_fd(DMABUF fd) → imresize(NV12→RGB 640x640) → importbuffer_fd(NPU fd)     │
  │          (RGA 硬件直写 NPU 物理内存, 零拷贝)                                                         │
  │                                                                                                     │
  旧架构:                                                                                                │
  ├─ 路径 A: RGA importbuffer_fd(DMABUF fd) → imcvtcolor(NV12→BGR) → cv::Mat (画框用)                    │
  └─ 路径 B: malloc + memcpy NV12 拷贝 → FrameData.nv12_data (Worker 推理用)                             │
                                                                                                        ▼
[SafeQueue g_SafeQueueRead (maxSize=100)] → Aggregator
                                                                                                        │
新架构 Aggregator: submit_to_worker(i%4, ...) → per-worker 独立队列 → try_get_result() → result_buffer  │
旧架构 Aggregator: submit_task_async() → future<ProcessResult> → future.get() 阻塞等待                    │
                                                                                                        │
Worker[i] 线程 (CPU4-7 大核, NPU Core i%3):                                                              │
  新架构: ReadVideo 已通过 Path C 写入 NPU 物理内存，直接 rknn_run                                       │
  旧架构: importbuffer_virtualaddr(NV12拷贝) → imresize(NV12→RGB 640x640) → importbuffer_fd(NPU fd)     │
  ├─ rknn_run (NPU 读取 input_mem 推理, 零拷贝)                                                          │
  ├─ CPU 读取 output_mems[i]->virt_addr (INT8 反量化 + NMS)                                             │
  ├─ cv::rectangle + cv::putText 绘制检测框                                                              │
  └─ 路径 D: RGA BGR→NV12 (编码器输入)                                                                    │
                                                                                                        ▼
[SafeQueue g_SafeQueueWrite (maxSize=100)] → WriteVideo 线程 → MPP 编码 H.264 → RTMP 推流
```

**NV12 拷贝对比：**

| 架构 | NV12 拷贝路径 | 拷贝次数 |
|------|-------------|---------|
| 旧架构 | ReadVideo: malloc+memcpy(NV12, 1.38MB) 给 Worker 推理 → Worker: malloc+BGR→NV12(1.38MB) 给编码器 | **2 次** |
| 新架构 | Worker: malloc+BGR→NV12(1.38MB) 给编码器（ReadVideo 通过 Path C 零拷贝直写 NPU） | **1 次** |

旧架构中 ReadVideo→Worker 的内存拷贝是必要的，因为 DMABUF 缓冲区 QBUF 后立即被 ISP 覆盖。

### 线程模型（3 控制线程 + 4 Worker 线程池）

| 线程 | 函数 | 职责 |
|------|------|------|
| ReadVideo | `Thread_ReadVideo()` | V4L2 poll 采集 NV12（ISP 已解码）→ DMABUF → RGA NV12→BGR + Path C（新）或 malloc NV12（旧）→ 入读队列 |
| Aggregator | `aggregatorThreadFunc()` | 从读队列取帧 → 提交推理 → 按帧序收集结果 → NV12 入写队列 |
| Worker[i] | `worker()` | rknn_run 推理 → CPU 后处理(反量化+NMS) → 绘制 → RGA BGR→NV12 |
| WriteVideo | `Thread_WriteVideo()` | 取处理后帧 → MPP 编码 H.264 → RTMP 推流 → 每 30 帧打印 FPS |

### 旧架构主要文件（DMA_旧架构/）

```
DMA_旧架构/
├── main.cpp              — 旧架构 ReadVideo: 含 Path B (malloc+memcpy NV12), SharedQueue
├── thread_pool.h/cpp     — 旧架构: submit_task_async(), future<ProcessResult>, 全局 SharedQueue
├── SafeQueue.h           — 旧架构线程安全队列（面试重点：双条件变量实现）
├── mpp_decoder.h/cpp     — MPP 硬件解码器（USB 摄像头时期遗留，IMX415 改用 ISP 后废弃）
├── yolov5s.h/cpp         — YOLOv5s 推理封装（通用零拷贝接口，新旧架构共用基本一致）
├── post_process.h/cpp    — YOLOv5 后处理（INT8 反量化、NMS）
├── dmabuf.h/c            — DMABUF 内存管理（新旧架构共用）
├── rtmp.h/c              — RTMP 推流（新旧架构共用）
├── streamer.h/c          — MPP 编码 + 推流整合（新旧架构共用）
└── beiwang/              — 旧架构面试复习文档（面试基准版本）
    ├── 面试_零拷贝数据通路.md    ← 面试重点：旧架构的零拷贝解释
    ├── 面试_NPU初始化位置设计决策.md
    ├── 面试_CMA池与缺页中断.md
    ├── 面试问题.md
    ├── 数据流复习_*.md           ← 8 个阶段文档
    └── ...
```

### 新架构主要文件（E:\DMA/ 根目录）

| 文件 | 说明 |
|------|------|
| `main.cpp` | 新架构 ReadVideo: Path C (RGA 直写 NPU fd), is_busy 丢帧, per-worker 队列 |
| `thread_pool.h/cpp` | 新架构: submit_to_worker(), result_queue, per-worker queue, is_busy atomic |
| `SafeQueue.h` | 线程安全队列（g_SafeQueueRead / g_SafeQueueWrite 使用） |
| `dmabuf.h/c` | DMABUF 内存管理（新旧一致） |
| `mpp_decoder.h/cpp` | 已废弃保留（IMX415 ISP 直接输出 NV12，无需 MPP 解码） |
| `yolov5s.h/cpp` | YOLOv5s 零拷贝推理封装 |
| `beiwang/` | 面试复习文档（新架构文档 + 面试专项） |

### 核心模块文件（根目录，新架构）

| 文件 | 说明 |
|------|------|
| `main.cpp` | V4L2 摄像头初始化、DMABUF 缓冲区池、ReadVideo（Path A + Path C）、Aggregator、WriteVideo、线程生命周期管理 |
| `dmabuf.h/c` | DMABUF 堆/缓冲区管理封装（分配/映射/同步/清理），通过 `/dev/dma_heap/cma` 从 CMA 分配器获取物理连续内存 |
| `yolov5s.h/cpp` | Yolov5s 类：封装 RKNN 模型加载、NPU 核心选择、零拷贝推理（`inference_zero_copy()`）、结果绘制。核心机制：`rknn_create_mem` 分配 NPU 物理内存 → `rknn_set_io_mem` 绑定 IO → RGA 通过 `input_mem->fd` 直写 → CPU 通过 `output_mems[i]->virt_addr` 直读 |
| `post_process.h/cpp` | YOLOv5 后处理：INT8 反量化（`float_val = (int8_val - zp) * scale`）、置信度过滤、NMS、坐标还原，3 输出分支解析 |
| `thread_pool.h/cpp` | **新架构**：submit_to_worker() + per-worker 独立队列 + result_queue + is_busy atomic 丢帧 |
| `SafeQueue.h` | 有界线程安全队列模板：双条件变量，predicate wait，stop_flag 优雅退出 |
| `streamer.h/c` + `rtmp.h/c` | RTMP 推流：MPP 编码 H.264 → Annex-B → AVCC 格式转换 → RTMP 推送 |
| `v4l2_common.h/c` | V4L2 设备操作共享代码 |

### 面试准备文档

**`beiwang/`（根目录，最新版）**：

| 文档 | 内容 |
|------|------|
| `面试_零拷贝数据通路.md` | 零拷贝数据路径描述、malloc+memcpy 是否违反零拷贝原则、新旧架构对比 |
| `面试_NPU初始化位置设计决策.md` | 为何在 yolov5s 类内部初始化 NPU 而非 main 函数（RAII、高内聚、解耦） |
| `面试_CMA池与缺页中断.md` | CMA 池体现位置、缺页中断原理、perf stat 统计方法、"缓存命中率98%"表述辨析、Glibc 基础知识 |
| `面试问题.md` | 20+ 道常见面试题整理（Pipeline、V4L2、RGA、NPU、性能等） |
| `数据流复习_*.md` | 8 个管线阶段详细文档 + 目录索引 |
| `线程同步_SafeQueue.md` | SafeQueue 实现原理（双条件变量、虚假唤醒、stop机制） |
| `线程池_ThreadPool.md` | ThreadPool 实现（per-worker 队列、submit_to_worker、result_queue、is_busy） |
| `YOLOv5s零拷贝内存绑定.md` | rknn_tensor_mem 零拷贝机制详解（virt_addr、fd、logical_addr） |

**`DMA_旧架构/beiwang/`（旧版，面试基准）**：
- 旧架构的面试复习文档，用于面试中讲解**旧架构设计理由**（malloc+memcpy 是解耦的代价、Glibc 动态阈值等）

### 零拷贝优化路径（新架构）

**RGA + RKNN 四条硬件加速路径：**
1. **路径 A**（ReadVideo）：`importbuffer_fd(DMABUF fd)` → `imcvtcolor(NV12→BGR)` — RGA 直接从 DMABUF 读取，零 CPU 拷贝
2. **路径 C**（ReadVideo）：`importbuffer_fd(DMABUF fd)` → `imresize(NV12→RGB 640x640)` → `importbuffer_fd(NPU fd)` — RGA 缩放+转换后直写 NPU 物理内存（**新架构独有**）
3. **路径 D**（Worker）：RGA `imcvtcolor(BGR→NV12)` — 绘制后转换送 MPP 编码器

**旧架构零拷贝路径差异：**
- 无 Path C（ReadVideo 不直接写 NPU）
- Worker 中 `importbuffer_virtualaddr(NV12拷贝)` → `imresize(NV12→RGB 640x640)` → `importbuffer_fd(NPU fd)`
- 多一次 Path B：`malloc + memcpy` NV12 拷贝

**rknn_tensor_mem 零拷贝核心机制：**
```
rknn_create_mem → rknn_tensor_mem {
    .virt_addr     → CPU 通过 mmap 访问（推理后读输出）
    .fd            → RGA 通过 importbuffer_fd 访问（推理前写输入）
    .logical_addr  → NPU 内部使用（rknn_run 读输入、写输出）
}
// 同一块物理内存，三方通过不同接口访问，零拷贝
```
- **输入**：`rknn_create_mem(ctx, size_with_stride)` — `size_with_stride` 保证硬件 stride 对齐
- **输出**：`rknn_create_mem(ctx, size)` — `size` 即实际数据大小，无需对齐
- **格式覆盖**：`input_attrs.type = RKNN_TENSOR_UINT8; input_attrs.fmt = RKNN_TENSOR_NHWC` — 匹配 RGA 写入格式

**vs 通用 RKNN 接口：**
| 操作 | 通用模式 | 本项目零拷贝模式 |
|------|---------|-------------------|
| 输入传递 | `rknn_inputs_set` 内部 memcpy | RGA `importbuffer_fd` 硬件直写 |
| 输出获取 | `rknn_get_outputs` 内部 memcpy | CPU 直接读 `virt_addr` |
| 额外内存 | CPU 侧临时 buffer | 无 |

**NPU 输出的是检测元数据（INT8），不是像素。** 所以不存在"NPU→编码器直接通路"——必须走 CPU 解析（反量化 + NMS）。

### 关键数据结构

- `FrameData` — 绑定 `cv::Mat`（BGR）、`uint8_t* nv12_data`（ISP 输出拷贝）、帧索引
- `app_state_t` — 管理 DMABUF 堆、缓冲区数组和 V4L2 缓冲区信息
- `ProcessResult` — 异步推理结果，包含绘制后图像 `processed_img`、NV12 数据 `nv12_data`、检测结果 `detection_results`、成功标志 `success`/错误信息 `error_msg`
- `detect_result_group_t` — 单帧全部检测目标集合（最多 64 个），包含 `detect_result_t` 数组（类别、置信度、坐标框）
- `Task` — 线程池任务单元：`index`（帧序号）、`img`（BGR 图像）、`nv12_data`/`nv12_size`、`promise<ProcessResult>`（异步回传）
- `rknn_tensor_mem` — NPU 物理内存描述符：`virt_addr`（CPU）、`fd`（RGA DMA）、`logical_addr`（NPU）、`size`

### 关键设计决策

| 决策 | 选择 | 原因 |
|------|------|------|
| NPU 内存管理 | `rknn_create_mem` 零拷贝 | 避免每次推理 memcpy 输入/输出，30fps 下每秒省 60 次拷贝 |
| YOLO 实例分配 | 每 Worker 独享（4 实例） | RKNN 上下文非线程安全，共享会串行化推理 |
| NPU 核心分配 | `i % 3` 轮询 Core 0/1/2 | 3 个 NPU 核心分配给 4 个 Worker，Core 0 复用 |
| Worker 绑核 | CPU4-7（A76 大核） | 避免调度到 A55 小核，减少调度延迟和迁移开销 |
| 队列同步 | 条件变量（2 CV） | 事件驱动，CPU 占用≈0%，远优于 busy-wait（100%）或 sleep（高延迟） |
| 帧保序（新架构） | `map<index, ProcessResult>` + `nextWriteIndex` | Head-of-Line blocking 可控（map 缓存乱序到达的结果） |
| 帧保序（旧架构） | `future<ProcessResult>` + `nextWriteIndex` | `future.get()` 阻塞等待，天然的 FIFO 保序 |
| 任务提交（新架构） | `submit_to_worker(id, ...)` + per-worker 队列 | Round-robin 分发，无锁竞争 |
| 任务提交（旧架构） | `submit_task_async()` + `future` + 全局 SharedQueue | 所有 Worker 竞争取任务，存在锁竞争 |
| NV12→NPU（新架构） | ReadVideo 中 Path C：RGA 直写 | 零 CPU 拷贝，但需要 is_busy 协调 |
| NV12→NPU（旧架构） | Worker 中 RGA：`importbuffer_virtualaddr` → imresize | 从堆拷贝搬运，有 CPU 介入，但架构解耦好 |
| H.264 格式转换 | Annex-B → AVCC（4 字节长度前缀） | FFmpeg/RTMP 要求 AVCC 格式 |
| FPS 统计 | 每 30 帧，`high_resolution_clock` | 端到端真实 FPS，包含编码推流延迟，打印到 stderr |

## 内存管理与 Glibc 相关知识（面试重点）

### CMA 预分配池

项目通过 `/dev/dma_heap/cma` 从内核 CMA（Contiguous Memory Allocator）分配器申请 4 块物理连续内存，作为 ISP 硬件写入的循环缓冲池：

```cpp
// main.cpp:506-554（新旧架构共用）
my_dma_heap_init(&state.dmabuf_heap);         // 打开 CMA 堆
my_dmabuf_buffer_alloc(&state.dmabuf_heap, ...); // 分配 4 × 1.38MB
```

- **特性**：物理连续、预分配（初始化时锁定物理页）、固定池化（4 块循环周转）
- **面试价值**：证明你对嵌入式 Linux DMA 内存模型有理解，是"预分配策略"和"零缺页"的硬件侧保障

### 缺页中断（Page Fault）分析

`perf stat -e page-faults` 用于量化验证内存池化对实时性的收益：

| 阶段 | 缺页行为 | 原因 |
|------|---------|------|
| 启动预热 | 触发数百次缺页 | 首次 malloc 1.38MB → mmap → 首次访问分配物理页 |
| 稳态运行 | 趋近于零 | CMA 物理页已锁定 + Glibc 动态阈值保护后堆分配 |

**简历表述注意**：page-faults 不能推导"缓存命中率"，这是两个不同的 PMU 事件。应改为"缺页中断减少约 95%"（通过 `perf stat -e page-faults` 验证）。

### Glibc Ptmalloc 动态 mmap 阈值机制

旧架构中，`malloc(1.38MB)` 每次触发 mmap/free 触发 munmap 的问题由 Glibc 自动优化：

```
默认阈值 = 128KB
↓ 第一次 free(1.38MB) → 阈值升至 1.38MB
↓ 第二次 free(1.38MB) → 阈值升至 2.76MB
↓ 第三次 malloc(1.38MB) → 1.38MB < 2.76MB → 走 sbrk 堆分配！
↓ 后续 free → 不调用 munmap → 物理页映射保留
↓ 后续 malloc → 从 bins 中复用 → 零缺页
```

**面试关键点**：
- 动态 mmap 阈值是**全局变量**，提升后对所有 < 阈值的大块分配有效
- < 128KB 的小 `malloc` 本就使用 `sbrk` 堆，不受阈值影响
- 阈值提升需要经过 ~2 次 mmap-free 周期，不是瞬时的

### 内存管理面试问答速查

| 面试问题 | 一句话回答 |
|---------|-----------|
| "CMA 池体现在哪" | V4L2 采集前端，4 块从 `/dev/dma_heap/cma` 分配的物理连续内存 |
| "为什么统计缺页率" | 验证 CMA 预分配 + Glibc 阈值保护对实时性的收益 |
| "缺页中断怎么测" | `perf stat -e page-faults` 或 `perf stat -e minor-faults,major-faults` |
| "Glibc 动态阈值怎么工作" | 频繁 free mmap 大块 → 阈值翻倍 → 后续转 sbrk 堆 → 零缺页 |
| "为什么不用手动内存池" | 避免过早优化，Glibc 当前瓶颈可控；若升级 4K/多路会考虑无锁 RingBuffer |
| "不同大小 malloc 怎么办" | <128KB 本来就用堆，无缺页问题；阈值全局提升后所有 < 阈值的分配受益 |

## 硬件平台依赖

- NPU: Rockchip RKNN API (`rknn_api.h`)，通过 `rknn_set_core_mask()` 选择 NPU 核心
- 视频编码: Rockchip MPP（仅编码器，`rockchip/mpp_*.h`），路径硬编码在 `CMakeLists.txt` 中
- 图像处理: Rockchip RGA（`im2d.h`），`3rdparty/rga/RK3588/` 提供库和头文件
- 摄像头: IMX415 MIPI + ISP（RKAIQ 3A 服务），V4L2 MPLANE NV12 格式 @ 1280x720，DMABUF 内存模式
- 运行时需启动 `rkaiq_3A_server -d /dev/video11 &` 后台服务

### 配置文件

- `.vscode/launch.json` — GDB 调试配置，`program` 指向 `build/cv`
- `.vscode/c_cpp_properties.json` — IntelliSense 包含路径和编译器设置
