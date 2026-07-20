# 第四阶段：RGA 格式转换（ReadVideo 线程）

数据流经过 V4L2 DQBUF 拿到 DMABUF 后，ReadVideo 线程要同时做两件事：
1. **给画框用** — NV12 → BGR888（RGA 硬件转换，Path A）
2. **给推理用** — NV12 → RGB 640×640 直写 NPU 物理内存（RGA 硬件转换，Path C）

**相关代码：** `main.cpp:169-296`（`Thread_ReadVideo` 完整函数）
**核心片段：** `main.cpp:234-274`（RGA 转换 + Path C + 入队）

---

## 4.1 两条 RGA 路径的对比

ReadVideo 线程中 DQBUF 拿到 DMABUF 后，同时产生两条 RGA 硬件路径：

| 路径 | 用途 | 输入 → 输出 | 硬件 |
|------|------|------------|------|
| **Path A: RGA NV12→BGR** | OpenCV 画框 | DMABUF fd → `cv::Mat` (CPU 内存) | **RGA 硬件转换** |
| **Path C: RGA NV12→RGB→NPU** | NPU 推理输入 | DMABUF fd → NPU 物理内存 `fd` | **RGA 硬件 DMA 直写** |

在代码中它们前后相邻（`main.cpp:234-274`）：

```cpp
// Path A: RGA NV12→BGR → clone → 入队
// Path C: RGA imresize(NV12→RGB 640x640) → importbuffer_fd(npu_fd) → 直写 NPU
```

> **设计要点：** Path C 是整个零拷贝架构的核心——它将原本在 Worker 中执行的 NV12→RGB 缩放+转换搬到了 ReadVideo，且源从 CPU heap 拷贝改为直接从 DMABUF fd 读取，目标从 `importbuffer_virtualaddr` 改为 `importbuffer_fd(npu_fd)`，实现了 ISP→RGA→NPU 的全链路硬件 DMA 直传。

---

## 4.2 RGA API 调用模式

RGA 是 Rockchip 的 2D 硬件加速器，操作模式遵循固定的四步序列：

```
importbuffer_xxx()    → 创建 handle（句柄），绑定内存来源
wrapbuffer_handle()   → 将 handle 包装成 rga_buffer_t（描述图像格式）
imXXX()               → 执行硬件操作（imcvtcolor / imresize 等）
releasebuffer_handle()→ 释放 handle
```

**两种导入方式对比：**

| API | 参数 | 数据路径 | CPU 参与 |
|-----|------|---------|---------|
| `importbuffer_fd(fd, size)` | DMABUF 文件描述符 | 物理内存 → RGA 硬件直读 | **完全不需要** |
| `importbuffer_virtualaddr(ptr, size)` | CPU 虚拟地址指针 | CPU 内存 → DMA 读取 | CPU 缓存被 DMA 读取 |

> **关键理解：** 两个 API 对应完全不同的内存传输路径。`importbuffer_fd` 是设备间直通（零拷贝），`importbuffer_virtualaddr` 是 CPU→设备的单向传递（有一层 IOMMU 转换）。

---

## 4.3 逐行拆解：ReadVideo 中的 RGA 调用

### 前置准备：预分配 BGR 帧缓存

```cpp
cv::Mat bgr_frame(720, 1280, CV_8UC3);  // while(true) 外部预分配
```
`bgr_frame` 在循环外声明，每次循环 RGA 直接覆写它的 `data` 指针指向的内存。`clone()` 是必须的——没有 clone，入队后下一轮循环就把数据覆盖了。

### RGA 转换四步

**第一步：创建 handle**

```cpp
rga_buffer_handle_t src_handle = importbuffer_fd(buffer_info.dmabuf_fd, nv12_size);
//                                    ↑ 从 DMABUF fd 导入，RGA 硬件直接读

rga_buffer_handle_t dst_handle = importbuffer_virtualaddr(bgr_frame.data, width * height * 3);
//                                    ↑ 写到 CPU 内存中的 bgr_frame
```

- `src`: DMABUF fd → RGA 拿到 fd 后通过硬件 DMA 直接读取 ISP 写入的 NV12，**零 CPU 拷贝、零系统调用（除了首次 import）**
- `dst`: CPU 虚拟地址 → RGA 通过 DMA 把转换后的 BGR 数据直接写入 `bgr_frame.data`

**第二步：包装成 rga_buffer_t**

```cpp
rga_buffer_t src = wrapbuffer_handle(src_handle, width, height, RK_FORMAT_YCbCr_420_SP);
rga_buffer_t dst = wrapbuffer_handle(dst_handle, width, height, RK_FORMAT_BGR_888);
```

`wrapbuffer_handle` 不拷贝数据，只是把 handle 和格式信息打包成一个 RGA 操作描述符。

**第三步：执行色彩空间转换**

```cpp
int rga_ret = imcvtcolor(src, dst, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888);
```

`imcvtcolor` 是 RGA 的色彩空间转换指令。硬件内部执行 CSC（Color Space Conversion）矩阵运算，将 YUV 转为 RGB。耗时约 **3ms**（纯 CPU 实现需要 ~30ms）。

**第四步：释放 handle**

```cpp
releasebuffer_handle(src_handle);
releasebuffer_handle(dst_handle);
```

RGA 内部引用计数管理，释放后 handle 失效。

---

## 4.4 RGA 失败路径分析

```cpp
if (rga_ret == IM_STATUS_SUCCESS) {
    // ... 入队 ...
} else {
    printf("RGA NV12->BGR Convert Failed: %s\n", imStrError((IM_STATUS)rga_ret));
}
```

RGA 失败时的行为：
- 只打印错误，**不退出线程**
- 失败的帧直接跳过，下一帧可能正常
- 整个管线不会因为单帧 RGA 失败而崩溃

> **潜在问题：** 失败时没有提前 `releasebuffer_handle`？——不需要担心，`releasebuffer_handle` 在第 262-263 行统一执行，在 `if-else` 结构之外。如果 `src_handle` 或 `dst_handle` 本身为 NULL（第 264 行 else 分支），`releasebuffer_handle` 对无效句柄是空操作，安全。

---

## 4.5 Path C 的时序窗口 — 无 CPU 同步需要的硬件 DMA

```
DQBUF → [RGA 硬件读取 DMABUF fd] → imresize + 直写 NPU 内存 → QBUF
         ^^^^^^^^^^^^^^^^^^^^^^^^^^
         RGA 硬件 DMA 直接读取，无需 CPU sync_start/sync_stop
```

Path C 使用 `importbuffer_fd`（硬件 DMA 直读），它不需要 `dma_buf_sync`：

| 操作 | 作用 | 是否必需 |
|------|------|---------|
| `sync_start` | CPU cache invalidation | **不需要** — RGA 硬件 DMA 直接从物理内存读，不走 CPU cache |
| `sync_stop` | cache clean 刷回物理内存 | **不需要** — RGA 不写 CPU cache |
| RGA `imresize` | 缩放+转换+DMA 直写 NPU | **唯一需要的操作** — 全部由 RGA 硬件完成 |

> **关键理解：** 旧版中 `sync_start/sync_stop` 保护的是 CPU 通过 `mapped_addr` 读取 DMABUF 的那次 `memcpy`。去掉 memcpy、改为纯 RGA `importbuffer_fd` 路径后，整个 `sync_start/sync_stop` 都不再需要——RGA 硬件 DMA 直接从物理内存读取，不走 CPU cache 路径。

---

## 4.6 RGA 转换成功后的数据打包

```cpp
frame_temp.frame = bgr_frame.clone();    // 深度拷贝 BGR
frame_temp.index = img_index++;          // 递增帧序号
frame_temp.width = width;                // 1280
frame_temp.height = height;              // 720

// Path C 已完成 RGA 直写 NPU 物理内存，无需再 malloc+memcpy NV12
// NV12 数据在 DMABUF 中被 RGA 硬件 DMA 读取，直接送入 NPU

g_SafeQueueRead.enqueue(frame_temp);     // 入读队列
```

**打包顺序（重构后简化）**：
1. 先 clone BGR（Path A 的 RGA 转换结果）
2. Path C 的 RGA 已将 NV12→RGB 直写 NPU 物理内存，无需打包 NV12 数据
3. 最后入队

**为什么 `clone()` 仍然需要？** `frame_temp.frame = bgr_frame.clone()` 分配新内存并拷贝像素数据。不 clone 的话，`frame_temp.frame` 只是浅拷贝——`cv::Mat` 的赋值运算符只复制头和指针，实际数据在 `bgr_frame` 中。下一轮循环 RGA 会覆盖 `bgr_frame.data`，之前入队的帧数据就花了。

---

## 4.7 对比：USB 摄像头版本 vs ISP 直接输出（旧版 vs 新版）

| 维度 | 旧版 ISP 直接输出（方案 A） | 新版 ISP 直接输出（方案 B — 当前） |
|------|---------------------------|---------------------------------|
| NV12 来源 | DMABUF（V4L2 周转缓冲区，QBUF 即回收） | DMABUF（V4L2 周转缓冲区，QBUF 即回收） |
| 数据传递 | `malloc+memcpy` 拷贝 NV12 | **RGA 硬件 DMA 直写 NPU 内存** |
| RGA 来源 | `importbuffer_fd(dmabuf_fd)`（Path A）+ `importbuffer_virtualaddr(heap)`（Path B Worker） | `importbuffer_fd(dmabuf_fd)`（Path A+B 都在 ReadVideo） |
| BGR 帧 | ISP 输出后 RGA 转换 | ISP 输出后 RGA 转换（不变） |
| NPU 输入 | CPU 内存→RGA `importbuffer_virtualaddr` 写入 NPU | **DMABUF→RGA `importbuffer_fd` 直接写入 NPU** |

---

## 总结

ReadVideo 线程本质上是**数据分发中心**——从 ISP 拿到一帧数据后，同时启动两条硬件路径：

```
DMABUF (NV12, ISP 写入)
    │
    ├─→ [Path A: RGA imcvtcolor] ─→ BGR Mat (clone) ─→ FrameData.frame       → 画框用
    │
    └─→ [Path C: RGA imresize + importbuffer_fd(NPU_fd)] ─→ NPU 物理内存       → 推理用
                                                                                ↓
                                                                        g_SafeQueueRead.enqueue()
```

> **重构收益：** 砍掉了 ReadVideo 中的 `malloc+memcpy` 和 Worker 中的 `importbuffer_virtualaddr`。NV12 数据只存在于 DMABUF 中，被 RGA 硬件 DMA 读取后直写 NPU 物理内存。不再经过 CPU 堆内存中转。
