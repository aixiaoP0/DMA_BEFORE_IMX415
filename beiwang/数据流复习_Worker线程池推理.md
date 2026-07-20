# 第六阶段：Worker 线程池推理

**相关代码：** `thread_pool.cpp`（完整文件）、`yolov5s.h/cpp`、`post_process.h/cpp`

这是整个管线中**技术密度最高的阶段**。分 5 个子阶段来看。

---

## 6.1 线程池初始化：4 Worker × 独立 YOLO 实例

**相关代码：** `thread_pool.cpp:55-72`

```cpp
// 每个 Worker 线程持有独立的 YOLO 实例
for (size_t i = 0; i < num_threads; i++) {
    auto yolo = make_shared<Yolov5s>(model_path, i % 3);
    yolo_group.emplace_back(yolo);
}

// 启动 num_threads 个工作线程
for (size_t i = 0; i < num_threads; i++) {
    threads.emplace_back(&ThreadPool::worker, this, i);
}
```

**Worker 与 NPU 核心的映射**（`i % 3` 轮询分配）：

```
Worker 0 → model[0] → NPU Core 0
Worker 1 → model[1] → NPU Core 1
Worker 2 → model[2] → NPU Core 2
Worker 3 → model[0] → NPU Core 0  （轮询）
```

> **追问：** 为什么需要 4 个模型实例而不是 1 个加锁？
> → RKNN 上下文不是线程安全的。如果 4 个线程共享一个实例，推理任务会串行化，相当于单核性能。4 个独立实例使得 4 个 Worker 可以真正并行推理。

**CPU 绑核：**

```cpp
int target_cpu = 4 + (id % 4);  // Worker 0→CPU4, Worker 1→CPU5, ...
pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
```

RK3588 的大小核架构：CPU0-3 是小核（Cortex-A55），CPU4-7 是大核（Cortex-A76）。Worker 绑定大核确保 RGA 调用、后处理等准备阶段有足够算力。

---

## 6.2 第二条 RGA 路径：NV12 → RGB888 写入 NPU 内存（已移至 ReadVideo）

**注意：架构重构后，此步骤已从 Worker 移至 ReadVideo 线程。**

旧版 Worker 中通过 `importbuffer_virtualaddr(t.nv12_data)` 从 CPU 堆内存读取 NV12，经过 RGA 缩放+转换写入 NPU。重构后 ReadVideo 线程通过 `importbuffer_fd(DMABUF fd)` 直接从 DMABUF 读取 NV12，完成相同操作。

### ReadVideo 中的 Path C（替代原 Worker 的 RGA 操作）

**相关代码：** `main.cpp:249-274`（ReadVideo 线程）

```cpp
int target_worker = frame_temp.index % g_num_workers;
int npu_fd = gthreadpool.get_worker_input_fd(target_worker);

// RGA 直接从 DMABUF fd 读取 NV12，缩放+转换后直写 NPU 物理内存
rga_buffer_handle_t src_c = importbuffer_fd(buffer_info.dmabuf_fd, nv12_size);
rga_buffer_handle_t dst_c = importbuffer_fd(npu_fd, 640 * 640 * 3);

rga_buffer_t src = wrapbuffer_handle(src_c, 1280, 720, RK_FORMAT_YCbCr_420_SP);
rga_buffer_t dst = wrapbuffer_handle(dst_c, 640, 640, RK_FORMAT_RGB_888);

imresize(src, dst);  // 一次 RGA 调用完成缩放 + 色彩转换 + DMA 写入
```

### 对比：旧 Worker RGA vs 新 ReadVideo Path C

```
旧版 Worker:
  importbuffer_virtualaddr(NV12 heap)  → importbuffer_fd(NPU fd)
  ↑ RGA 读 CPU 内存（需 CPU 先 memcpy）  ↑ RGA 写 NPU 物理内存

新版 ReadVideo Path C:
  importbuffer_fd(DMABUF fd)           → importbuffer_fd(NPU fd)
  ↑ RGA 硬件直读 DMABUF                 ↑ RGA 直接写 NPU 物理内存
```

> **追问：** 为什么新版不需要 `importbuffer_virtualaddr` 了？
> → 旧版因为 NV12 数据已经被 `malloc+memcpy` 到 CPU 堆内存中，没有 DMABUF fd 可用。重构后 NV12 数据只存在于 DMABUF 中，RGA 通过 `importbuffer_fd` 硬件 DMA 直接读取，彻底跳过 CPU 内存。

---

## 6.3 NPU 零拷贝推理

### 零拷贝内存的建立（构造函数）

**相关代码：** `yolov5s.cpp:122-138`

```cpp
// 告诉驱动输入格式
input_attrs[0].type = RKNN_TENSOR_UINT8;
input_attrs[0].fmt = RKNN_TENSOR_NHWC;

// 核心：分配 NPU 侧物理内存
input_mem = rknn_create_mem(ctx, input_attrs[0].size_with_stride);
rknn_set_io_mem(ctx, input_mem, &input_attrs[0]);

// 输出端同样分配
for (int i = 0; i < io_num.n_output; i++) {
    output_mems[i] = rknn_create_mem(ctx, output_attrs[i].size);
    rknn_set_io_mem(ctx, output_mems[i], &output_attrs[i]);
}
```

`rknn_create_mem` 返回的 `rknn_tensor_mem` 结构体：

```
rknn_tensor_mem {
    void   *virt_addr;     // CPU 可访问的映射地址（输出时读检测结果）
    int     fd;            // 文件描述符（RGA 可通过 importbuffer_fd 写入）
    void   *logical_addr;  // NPU 侧逻辑地址
    size_t  size;          // 内存大小
}
```

**关键理解：** `virt_addr` 和 `fd` 指向同一块物理内存，但从不同接口访问：
- RGA 通过 `fd`（DMA 设备直写）
- CPU 通过 `virt_addr`（mmap 映射后读取）

### 零拷贝推理链路

```
                            ReadVideo Path C: RGA 直写
  [DMABUF fd] ───────────────────────────────────► input_mem (NPU 物理内存)
    (ISP 写入)   importbuffer_fd(DMABUF)            ↑ fd 供 RGA 写入
                 + imresize +                        │
                 importbuffer_fd(npu_fd)              │
                                                     │
                                               rknn_run(ctx, NULL)
                                                     │
                                                     ▼
                                             output_mems[i] (NPU 物理内存)
                                                     │
                                         virt_addr ◄─┘ CPU 直接读
                                                     │
                                           post_process (INT8 → float)
```

**推理调用：**

```cpp
// yolov5s.cpp:251
int ret = rknn_run(ctx, NULL);
```

RGA 写入 input_mem → NPU 直接读取 → 推理结果写入 output_mems。**没有任何一次 CPU memcpy**。

> **追问：** 不零拷贝会怎样？
> → 每次推理前需要 `memcpy` 输入数据到 NPU 内存，推理后再 `memcpy` 结果回 CPU 内存。两张额外的内存拷贝在 30FPS 下就是每秒 60 次拷贝。

---

## 6.4 INT8 后处理

**相关代码：** `yolov5s.cpp:254-272`、`post_process.h/cpp`

### 调用入口

```cpp
post_process(
    (int8_t*)output_mems[0]->virt_addr,   // 3 个输出分支
    (int8_t*)output_mems[1]->virt_addr,
    (int8_t*)output_mems[2]->virt_addr,
    model_height, model_width, BOX_THRESHOLD, NMS_THRESHOLD,
    scale_w, scale_h, qnt_zps, qnt_scales, group);
```

### 反量化

NPU 输出是 INT8 格式，需要转为浮点数才能做 NMS：

```cpp
float_val = (int8_val - zp) * scale;
```

### YOLOv5s 三分支输出

| 分支 | 下采样倍数 | 网格大小 | 锚框 | 负责目标 |
|------|-----------|---------|------|---------|
| output0 | 8x | 80x80 | `[10,13, 16,30, 33,23]` | 小目标 |
| output1 | 16x | 40x40 | `[30,61, 62,45, 59,119]` | 中目标 |
| output2 | 32x | 20x20 | `[116,90, 156,198, 373,326]` | 大目标 |

### NMS 流程

```
1. 遍历所有检测框，低于 BOX_THRESHOLD(0.5) 的丢弃
2. 按类别分组
3. 单个类别内按置信度排序
4. 从最高分开始，与其余框计算 IOU
5. IOU > NMS_THRESHOLD(0.5) 的被抑制（视为重复检测）
```

> **追问：** 为什么两个阈值都设置 0.5？
> → 0.5 是 COCO 数据集上的常用平衡值。太低产生很多误检（FP），太高漏检（FN）严重。实际部署时可根据场景调整。

---

## 6.5 绘制 + BGR→NV12（第三条路径）

**相关代码：** `yolov5s.cpp:282-304`、`thread_pool.cpp:199-217`

### 绘制检测框

```cpp
cv::rectangle(orig_img, Point(xmin, ymin), Point(xmax, ymax), Scalar(255,0,0), 3);
cv::putText(orig_img, label_name, Point(xmin+10, ymin+10), FONT_HERSHEY_SIMPLEX, 0.5, Scalar(0,0,0));
```

### 第三条 RGA 路径：BGR→NV12

```cpp
uint8_t* pixel_buffer = (uint8_t*)malloc(nv12_size);
BGR_to_NV12_with_rga(t.img.data, pixel_buffer, width, height);

res.nv12_data = pixel_buffer;  // 传递给 WriteVideo
res.processed_img = t.img;     // 引用计数浅拷贝
res.success = true;
```

**三条 RGA 路径总结：**

| 路径 | 位置 | 转换 | 来源 → 目标 | 用途 |
|------|------|------|------------|------|
| 第 1 条 (Path A) | ReadVideo | NV12→BGR888 | DMABUF fd → CPU Mat | OpenCV 画框 |
| 第 2 条 (Path C) | ReadVideo | NV12→RGB888 | DMABUF fd → NPU mem | NPU 输入（零拷贝核心） |
| 第 3 条 (Path D) | Worker | BGR888→NV12 | CPU Mat → CPU heap | MPP 编码 |

### Worker 的 NV12 内存所有权链

```
ReadVideo(Path C: RGA 直写 NPU 物理内存)      ← 输入 NV12 无需 malloc/free，RGA 硬件 DMA 完成
                                                         │
                                                 BGR→NV12 malloc (输出编码用)
                                                         │
                                                         ▼
                                                Aggregator(collect)
                                                         │
                                                         ▼
                                                WriteVideo(free)
```

Worker 内部只有一段 NV12 内存：
1. **输出的 NV12**（BGR→NV12 转换结果 `pixel_buffer`）→ 通过 ProcessResult 传出去，WriteVideo 中 `free`

> **重构收益：** 旧版 Worker 需要管理两段 NV12 内存（输入拷贝+输出新建），其中输入的 NV12 由 ReadVideo 的 `malloc+memcpy` 分配、Worker 中 `free`。重构后 ReadVideo 通过 RGA 硬件 DMA 直接将 NV12→RGB 写入 NPU，不再需要输入 NV12 的 malloc/free 循环。
