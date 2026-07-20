# 第四阶段：RGA 格式转换（旧架构 — 面试基准版本）

旧架构中，RGA 的使用分布在 **三个位置**、**两个线程** 中，共涉及 **三条 RGA 硬件路径**。

---

## 旧架构 RGA 三条路径总览

旧架构中每帧数据经过 RGA 硬件三次：

| 路径 | 所在线程 | 功能 | 输入 → 输出 |
|------|---------|------|-------------|
| **Path A** | ReadVideo | NV12→BGR（画框用） | DMABUF fd → CPU 内存 `cv::Mat` |
| **Path B'** | Worker | NV12→RGB 640×640（NPU 推理输入） | CPU 堆 NV12 拷贝 → NPU 物理内存 fd |
| **Path D** | Worker | BGR→NV12（编码器输入） | CPU 内存 `cv::Mat` → CPU 堆 NV12 缓冲区 |

---

## 路径一：Path A — ReadVideo 中的 RGA NV12→BGR

**代码位置：** `DMA_旧架构/main.cpp:234-267`

```cpp
// === RGA转换：NV12 (DMABUF fd) → BGR888 (OpenCV Mat) ===
rga_buffer_handle_t src_handle = importbuffer_fd(buffer_info.dmabuf_fd, nv12_size);
rga_buffer_handle_t dst_handle = importbuffer_virtualaddr(bgr_frame.data, width * height * 3);

if (src_handle && dst_handle) {
    rga_buffer_t src = wrapbuffer_handle(src_handle, width, height, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_handle(dst_handle, width, height, RK_FORMAT_BGR_888);

    int rga_ret = imcvtcolor(src, dst, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888);
    ...
    releasebuffer_handle(src_handle);
    releasebuffer_handle(dst_handle);
}
```

**调用模式：** `importbuffer_fd`(DMABUF fd) → `importbuffer_virtualaddr`(CPU Mat)

| 角色 | API | 参数 | 来源 |
|------|-----|------|------|
| 源图像 | `importbuffer_fd` | `buffer_info.dmabuf_fd` | ISP 写入的 DMABUF 物理内存 |
| 目标图像 | `importbuffer_virtualaddr` | `bgr_frame.data` | 预分配的 CPU 堆内存 |

这里 RGA 从 DMABUF fd 硬件 DMA 直读 NV12，转换后通过 DMA 直写 `bgr_frame.data`——整个过程 CPU 不接触像素数据。

---

## 路径二：Path B' — Worker 中的 RGA NV12→RGB 640×640

**代码位置：** `DMA_旧架构/thread_pool.cpp:167-198`

```cpp
// a. 取 NPU 内存 fd
int npu_fd = yolo->my_get_input_fd();

// b. RGA：将 NV12 堆拷贝 → 缩放+转换 → 直写 NPU 物理内存
rga_buffer_handle_t src_handle = importbuffer_virtualaddr(
    t.nv12_data, t.width * t.height * 3 / 2);       // ← 源：CPU 堆上 malloc 的 NV12 拷贝
rga_buffer_handle_t dst_handle = importbuffer_fd(
    npu_fd, 640 * 640 * 3);                          // ← 目标：NPU 物理内存 (rknn_tensor_mem.fd)

if (src_handle && dst_handle) {
    rga_buffer_t src = wrapbuffer_handle(
        src_handle, t.width, t.height, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_handle(
        dst_handle, 640, 640, RK_FORMAT_RGB_888);

    int rga_ret = imresize(src, dst);  // 缩放 + 色彩转换，一步完成
    ...
    releasebuffer_handle(src_handle);
    releasebuffer_handle(dst_handle);
}

// c. NPU 直接推理（零拷贝读取已写入的内存）
yolo->inference_zero_copy(1280, 720, &res.detection_results);
```

**调用模式：** `importbuffer_virtualaddr`(堆 NV12) → `importbuffer_fd`(NPU fd)

### 这条路径的数据流程

旧架构中，NV12 数据经历了两段独立的内存生命周期：

```
ISP → DMABUF ──→ QBUF 回收 ──→ ISP 写入下一帧
         │
         └──→ sync_start → memcpy → sync_stop → malloc 堆拷贝 (FrameData.nv12_data)
                                                    │
                                               (跨线程传递)
                                                    │
                                              Worker 线程:
                                              importbuffer_virtualaddr(nv12_data)
                                                    │
                                              RGA imresize(NV12→RGB 640×640)
                                                    │
                                              importbuffer_fd(npu_fd)  ← 最终写入 NPU
```

这条路径的三处代价：

1. **ReadVideo 中 `malloc + memcpy`** — 不得不把 DMABUF 中的 NV12 拷贝到堆上（因为 QBUF 后 DMABUF 立即被 ISP 覆盖）
2. **Worker 中 `importbuffer_virtualaddr`** — RGA 通过 IOMMU 从 CPU 堆内存 DMA 读取，不是设备间直通
3. **CPU 缓存一致性开销** — `importbuffer_virtualaddr` 涉及 CPU cache 的隐式刷写

> **面试回答模板：**
> 问："旧架构中 NV12 有几处拷贝？"
> 答："两处不可避免的拷贝 — ① ReadVideo 中 malloc+memcpy 把 NV12 从 DMABUF 搬到堆（保护数据不因 QBUF 而失效），② Worker 内部 RGA 从堆读取时通过 IOMMU 搬运到 NPU。后者虽然由 RGA 硬件完成、不占 CPU，但数据路径不是设备间直通。"

---

## 路径三：Path D — Worker 中的 RGA BGR→NV12

**代码位置：** `DMA_旧架构/thread_pool.cpp:208-216`，调用 `DMA_旧架构/main.cpp:73-114` 中的 `BGR_to_NV12_with_rga()`

```cpp
// thread_pool.cpp
int nv12_size = width * height * 3 / 2;
uint8_t* pixel_buffer = (uint8_t*)malloc(nv12_size);
if (pixel_buffer) {
    BGR_to_NV12_with_rga(t.img.data, pixel_buffer, width, height);
    res.nv12_data = pixel_buffer;
    res.data_size = nv12_size;
}
```

```cpp
// main.cpp — BGR_to_NV12_with_rga 函数实现
void BGR_to_NV12_with_rga(uint8_t* bgr, uint8_t* nv12, int width, int height) {
    memset(nv12, 0x00, width * height * 3 / 2);

    bgr_handel = importbuffer_virtualaddr(bgr, width * height * 3);
    yuv_handel = importbuffer_virtualaddr(nv12, g_hor_stride * g_ver_stride * 3/2);

    rga_buffer_t bgr_src = wrapbuffer_handle(bgr_handel, width, height, RK_FORMAT_RGB_888);
    rga_buffer_t yuv_src = wrapbuffer_handle(yuv_handel, g_hor_stride, g_ver_stride, RK_FORMAT_YCrCb_420_SP);

    imcvtcolor(bgr_src, yuv_src, RK_FORMAT_RGB_888, RK_FORMAT_YCrCb_420_SP);

    releasebuffer_handle(bgr_handel);
    releasebuffer_handle(yuv_handel);
}
```

**调用模式：** `importbuffer_virtualaddr`(BGR Mat) → `importbuffer_virtualaddr`(NV12 堆)

BGR 和 NV12 都在 CPU 堆内存上，RGA 通过 IOMMU 从源地址 DMA 读取，转换后 DMA 写入目标地址。

### stride 对齐问题

此函数使用全局变量 `g_hor_stride` / `g_ver_stride`（16 字节对齐的宽高）：

```cpp
// main.cpp 初始化
g_hor_stride = ALIGN(width, 16);   // 1280 → 1280 (已对齐)
g_ver_stride = ALIGN(height, 16);  // 720  → 720  (已对齐)
```

`#define ALIGN(x, a) (((x)+(a)-1)&~((a)-1))`

RGA 硬件要求图像行 stride 按 16 字节对齐，否则可能输出错位。1280×720 正好满足对齐条件，所以这里 `g_hor_stride == width`、`g_ver_stride == height`，没有 padding 开销。

> **面试提示：** 如果你被问到"没有对齐问题的 1280×720 是特例吗"，可以回答："1280 和 720 都是 16 的倍数，所以这里不需要 padding。但如果分辨率不是 16 的倍数（比如 1920×1080，1080 不是 16 的倍数），RGA 的行 stride 会产生对齐空隙，`ALIGN` 宏确保此时 NV12 缓冲区大小包含 padding 字节，防止硬件读越界。"

---

## 三条路径对比汇总

| 对比项 | Path A (ReadVideo) | Path B' (Worker) | Path D (Worker) |
|--------|-------------------|-----------------|-----------------|
| 操作 | NV12→BGR | NV12→RGB 640×640 | BGR→NV12 |
| 源 | DMABUF fd（物理内存） | CPU 堆 NV12 拷贝 | CPU 堆 BGR Mat |
| 目标 | CPU 堆 BGR Mat | NPU 物理内存 fd | CPU 堆 NV12 缓冲区 |
| 源 API | `importbuffer_fd` | `importbuffer_virtualaddr` | `importbuffer_virtualaddr` |
| 目标 API | `importbuffer_virtualaddr` | `importbuffer_fd` | `importbuffer_virtualaddr` |
| RGA 指令 | `imcvtcolor` | `imresize` | `imcvtcolor` |
| 数据路径 | 物理内存→CPU 内存 | CPU 堆→NPU 物理内存 | CPU 内存→CPU 堆 |

三条路径的 handle 组合各不相同，反映了 RGA 的灵活性：

| 路径 | src handle | dst handle | 数据方向 |
|------|-----------|-----------|---------|
| Path A | `importbuffer_fd` | `importbuffer_virtualaddr` | 物理→CPU |
| Path B' | `importbuffer_virtualaddr` | `importbuffer_fd` | CPU→NPU |
| Path D | `importbuffer_virtualaddr` | `importbuffer_virtualaddr` | CPU→CPU |

---

## 设计决策：为什么 ReadVideo 不直接写 NPU？

旧架构中 ReadVideo 只做了一件事：采集帧 + RGA 转 BGR + 塞入队列。它不关心推理的内部细节——不知道 NPU 内存 fd、不知道模型输入尺寸、不知道哪个 Worker 会处理哪一帧。这种**职责分离**在工程上是合理的：

- ReadVideo 不持有 YOLO 实例引用（解耦）
- Worker 内部完成所有"与推理相关的操作"（内聚）
- 通过 `FrameData.nv12_data` 传递裸数据，接口简单

代价则是路径 B' 中 NV12 数据从堆搬运到 NPU 的开销——但这是一种有意识的设计选择：为了更清晰的模块边界，接受一次 RGA 搬运的代价。

> **工程权衡：** ReadVideo 专心做采集和分发，Worker 专心做推理和相关处理。如果让 ReadVideo 直接写 NPU，ReadVideo 就需要持有 YOLO 实例的 NPU 内存 fd，耦合度上升，且 ReadVideo 需要了解模型输入尺寸（640×640）等推理细节。

---

## 旧架构 RGA 相关的关键知识点

1. **`importbuffer_virtualaddr` vs `importbuffer_fd`** — 前者走 IOMMU 从 CPU 内存 DMA 读取，后者走硬件设备间直通 DMA。面试中问"零拷贝实现细节"时，区分这两个 API 是加分项。

2. **`imresize` 同时完成缩放+色彩转换** — 旧架构中 Path B' 的 `imresize` 把 1280×720 NV12 一步变为 640×640 RGB。RGA 硬件内部同时执行：① 双线性插值缩放 ② YUV→RGB 矩阵变换 ③ DMA 写入目标内存。一步替代了软件中 `resize + cvtColor` 两个操作。

3. **每帧 RGA handle 操作次数** — 旧架构中每帧需要 5 次 `importbuffer_xxx` + 5 次 `releasebuffer_handle`（Path A 2 次 + Path B' 2 次 + Path D 1 次）。RGA 驱动内部对 handle 操作有锁保护，频繁创建/销毁有累积开销。

4. **为什么 `BGR_to_NV12` 中要用 `memset` 清空缓冲区？** — NV12 格式中 Y 平面和 UV 平面的交错可能导致残留数据被 MPP 编码器误读。清零是一个防御性操作，确保未覆盖的边界像素（宽高不对齐时的 padding 区域）为黑色。

5. **Path B' 中 `imresize` 的输入是 1280×720 而非 640×640** — 注意这里 `imresize` 同时做了缩放和色彩空间转换：输入 NV12 1280×720，输出 RGB 640×640。分辨率降低约 4 倍，叠加格式转换，一步完成。
