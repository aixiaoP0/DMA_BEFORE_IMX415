# YOLOv5s 零拷贝内存绑定

**相关代码：** `yolov5s.h`（声明）、`yolov5s.cpp:27-139`（构造绑定）、`yolov5s.cpp:248-273`（推理）

---

## 概述

Yolov5s 类封装了 RKNN 模型的完整生命周期：模型加载、NPU 核心绑定、零拷贝内存分配、推理执行、结果绘制。其中最核心的是**零拷贝内存绑定**——让 RGA 直接写入 NPU 物理内存，CPU 直接读取推理结果，中间不经过任何 memcpy。

---

## 1. rknn_tensor_mem：零拷贝的内存抽象

`rknn_create_mem` 返回的 `rknn_tensor_mem` 结构体是零拷贝的基石：

```c
typedef struct rknn_tensor_mem {
    void        *virt_addr;      // CPU 可访问的虚拟地址（mmap 映射）
    int          fd;             // 文件描述符（可用于 RGA importbuffer_fd）
    void        *logical_addr;   // NPU 侧逻辑地址（驱动内部使用）
    size_t       size;           // 内存大小（字节）
    uint32_t     flags;          // 标志位
    int          handle;         // 句柄（驱动内部）
} rknn_tensor_mem;
```

**关键：同一个物理内存块，从两个接口暴露：**

| 字段 | 给谁用 | 如何访问 | 用途 |
|------|--------|---------|------|
| `virt_addr` | CPU | `mmap` 映射到用户空间 | 推理后读取输出结果（INT8 后处理） |
| `fd` | RGA（DMA 设备） | `importbuffer_fd` | 推理前写入输入数据（NV12→RGB） |

这也就是说：RGA 通过 `fd` 写入这块内存，NPU 通过 `logical_addr` 读取，CPU 通过 `virt_addr` 读取——**三方访问同一块物理内存，没有一次数据搬运**。

---

## 2. 输入零拷贝绑定

**相关代码：** `yolov5s.cpp:122-129`

```cpp
// 第 1 步：覆盖格式声明
input_attrs[0].type = RKNN_TENSOR_UINT8;   // RGA 写入的是 UINT8
input_attrs[0].fmt = RKNN_TENSOR_NHWC;     // RGA 写入的是 NHWC 布局

// 第 2 步：在 NPU 侧分配物理内存
input_mem = rknn_create_mem(ctx, input_attrs[0].size_with_stride);

// 第 3 步：绑定到 RKNN 上下文
rknn_set_io_mem(ctx, input_mem, &input_attrs[0]);
```

### 2.1 为什么强制声明 UINT8 + NHWC？

模型训练时可能使用 FP32 或 FP16，但 NPU 推理时**输入必须是 INT8/UINT8**（硬件量化要求）。RGA 写入 `imresize` 的输出格式是 UINT8 RGB 数据，排列为 NHWC（N=1, H=640, W=640, C=3）。

如果不设置这两个字段，RKNN 驱动会使用模型文件中原有的 `type` 和 `fmt` 声明，可能与 RGA 实际写入的数据格式不匹配。

### 2.2 size_with_stride vs size

```cpp
// 输入分配使用 size_with_stride
rknn_create_mem(ctx, input_attrs[0].size_with_stride);
```

**为什么输入用 `size_with_stride` 而不是 `size`？**

NPU 硬件对输入张量的对齐有要求——每行的字节数可能大于 `width * channels`，多出来的部分叫 **stride padding**。RKNN 驱动在查询属性时，`dims` 字段给的是逻辑尺寸（如 `[1, 640, 640, 3]`），`size_with_stride` 给的是 NPU 硬件实际需要的对齐尺寸（每行可能 128 字节对齐）。

```
逻辑尺寸：         640 x 640 x 3 = 1,228,800 字节
硬件对齐尺寸：     stride 对齐后可能更大
size_with_stride： NPU 实际要求的缓冲区大小
```

使用 `size` 分配可能不够——RGA 写入时如果使用了 stride，会写到超过 `size` 的范围，造成缓冲区溢出。

### 2.3 RGA 如何写入 input_mem（在 Worker 中）

```cpp
// thread_pool.cpp:174
rga_buffer_handle_t dst_handle = importbuffer_fd(npu_fd, 640 * 640 * 3);
//                                           ↑ input_mem->fd
```

`my_get_input_fd()` 返回 `input_mem->fd`，RGA 通过 `importbuffer_fd` 拿到这个 fd，就知道目标内存是 NPU 侧物理内存。RGA 硬件 DMA 引擎直接写入，CPU 全程不参与数据搬运。

```
RGA imresize 完成后：
    [input_mem] ← RGB888 数据已由 RGA 写入
         │
    rknn_run(ctx, NULL)
         │
         ▼
    NPU 直接读取 input_mem → 推理计算
```

---

## 3. 输出零拷贝绑定

**相关代码：** `yolov5s.cpp:131-138`

```cpp
output_mems.resize(io_num.n_output);
for (int i = 0; i < io_num.n_output; i++) {
    output_mems[i] = rknn_create_mem(ctx, output_attrs[i].size);
    //                                    ↑ 输出使用 size，不是 size_with_stride
    rknn_set_io_mem(ctx, output_mems[i], &output_attrs[i]);
}
```

### 3.1 为什么输出用 size？

输出是 NPU 写得、CPU 读的，不需要硬件 stride 对齐——`size` 就是 NPU 实际输出的数据大小。

### 3.2 三分支输出

YOLOv5s 有三个输出分支：

| 索引 | output_mems[i] | 下采样 | 网格 | 张量形状 |
|------|---------------|--------|------|---------|
| 0 | `output_mems[0]` | 8x | 80x80 | `[1, 80x80, 85]` |
| 1 | `output_mems[1]` | 16x | 40x40 | `[1, 40x40, 85]` |
| 2 | `output_mems[2]` | 32x | 20x20 | `[1, 20x20, 85]` |

每个输出分支 `85` 维 = `[x, y, w, h, obj_conf, class_0_conf, ..., class_79_conf]`。

### 3.3 推理后 CPU 直接读取

```cpp
// yolov5s.cpp:267-271
post_process(
    (int8_t*)output_mems[0]->virt_addr,   // CPU 直接读 NPU 输出
    (int8_t*)output_mems[1]->virt_addr,
    (int8_t*)output_mems[2]->virt_addr,
    ...
);
```

**NPU 输出是 INT8 格式**——因为模型推理时做了 INT8 量化。CPU 读取 `virt_addr` 指向的内存后，`post_process` 中做反量化：`float_val = (int8_val - zp) * scale`。

---

## 4. 完整的零拷贝数据路径

```
┌─────────────────────────────────────────────────────────────────┐
│                         NPU 物理内存                             │
│                                                                 │
│  ┌──────────────────────────────────┐                           │
│  │        input_mem                 │  ← rknn_create_mem        │
│  │   (640x640x3, UINT8, NHWC)       │                           │
│  │                                  │                           │
│  │  ┌─ fd → importbuffer_fd ────────┼── RGA 写 NV12→RGB       │
│  │  └─ logical_addr ────────────────┼── NPU rknn_run 读        │
│  └──────────────────────────────────┘                           │
│                                                                 │
│  ┌──────────────────────────────────┐                           │
│  │     output_mems[0]               │  ← rknn_create_mem        │
│  │   (80x80x85, INT8)              │                           │
│  │  └─ virt_addr ───────────────────┼── CPU 读后处理            │
│  └──────────────────────────────────┘                           │
│                                                                 │
│  ┌──────────────────────────────────┐                           │
│  │     output_mems[1]               │  ← rknn_create_mem        │
│  │   (40x40x85, INT8)              │                           │
│  │  └─ virt_addr ───────────────────┼── CPU 读后处理            │
│  └──────────────────────────────────┘                           │
│                                                                 │
│  ┌──────────────────────────────────┐                           │
│  │     output_mems[2]               │  ← rknn_create_mem        │
│  │   (20x20x85, INT8)              │                           │
│  │  └─ virt_addr ───────────────────┼── CPU 读后处理            │
│  └──────────────────────────────────┘                           │
└─────────────────────────────────────────────────────────────────┘
         ▲                                        │
         │                                        ▼
    RGA 硬件 DMA 写入                        CPU 读取 virt_addr
    importbuffer_fd(npu_fd)                  post_process(INT8→float)
```

**数据搬运对比：**

```
无零拷贝方案：
  CPU 内存 ──memcpy──► NPU 内存 ──rknn_run──► NPU 内存 ──memcpy──► CPU 内存
        输入拷贝 (2.76MB)                       输出拷贝 (几百 KB)
         ↓                                        ↓
    每次推理多 2 次 CPU 拷贝，30fps 下每秒 60 次

零拷贝方案（本项目）：
  RGA DMA ──硬件直写──► NPU 内存 ──rknn_run──► NPU 内存 ◄──CPU virt_addr 直接读
        零 CPU 拷贝                              无拷贝，CPU 直接访问
```

---

## 5. 初始化的完整流程

```
Yolov5s(model_path, npu_index)
    │
    ├── load_model()              → 读 .rknn 文件到 model_data
    ├── rknn_init()               → 创建 RKNN 上下文 ctx（高优先级）
    ├── rknn_set_core_mask()      → 绑定到指定 NPU 核心（0/1/2）
    │
    ├── rknn_query 输入输出属性    → 获取 io_num、input_attrs、output_attrs
    │
    ├── 解析模型输入尺寸          → model_width=640, model_height=640, model_channel=3
    │
    ├── 强制 UINT8 + NHWC         → 告诉驱动输入格式
    ├── rknn_create_mem(input)    → 分配 NPU 输入物理内存
    ├── rknn_set_io_mem(input)    → 绑定输入到 ctx
    │
    ├── rknn_create_mem(output) × 3  → 分配 3 个输出物理内存
    ├── rknn_set_io_mem(output) × 3  → 绑定输出到 ctx
    │
    └── 构造完成
```

### 5.1 NPU 核心选择

```cpp
rknn_core_mask core_mask;
switch (npu_index) {
    case 0: core_mask = RKNN_NPU_CORE_0; break;
    case 1: core_mask = RKNN_NPU_CORE_1; break;
    default: core_mask = RKNN_NPU_CORE_2; break;
}
rknn_set_core_mask(ctx, core_mask);
```

NPU 核心是硬件单元，`rknn_set_core_mask` 相当于告诉硬件调度器"这个推理任务只能在这颗核心上运行"。三个核心可以并行处理三个不同的推理请求。

---

## 6. 析构：释放顺序

```cpp
Yolov5s::~Yolov5s() {
    // 1. 先释放输出内存
    for (auto mem : output_mems) {
        rknn_destroy_mem(ctx, mem);
    }
    // 2. 再销毁 RKNN 上下文
    rknn_destroy(ctx);
    // 3. 释放模型数据
    free(model_data);
    // 4. 最后释放输入内存
    if (input_mem) rknn_destroy_mem(ctx, input_mem);
}
```

**为什么先释放输出再释放输入？** 从 RKNN SDK 的语义来说，所有通过 `rknn_create_mem` 分配的内存都绑定到 `ctx`，理论上释放顺序可以任意。但先释放 ctx 再释放 mem 是未定义行为——`rknn_destroy_mem` 需要 ctx 参数来操作内核中的内存管理结构。所以必须先 destroy_mem 再 destroy ctx。

---

## 7. 和通用推理接口的对比

RKNN 提供了两套推理接口：

### 接口 A：通用模式（有拷贝）

```cpp
rknn_input inputs[1];
inputs[0].buf = img_data;           // CPU 内存中的图像数据
inputs[0].size = img_size;
inputs[0].type = RKNN_TENSOR_UINT8;
rknn_inputs_set(ctx, 1, inputs);     // ★ 内部会 memcpy 到 NPU 内存

rknn_run(ctx, NULL);

rknn_output outputs[3];
rknn_get_outputs(ctx, 3, outputs);   // ★ 内部会 memcpy 结果回 CPU
// outputs[i].buf 中的就是 CPU 可读的结果
```

### 接口 B：零拷贝模式（本项目使用）

```cpp
// 构造时：
input_mem = rknn_create_mem(ctx, size);   // NPU 物理内存
rknn_set_io_mem(ctx, input_mem, &attr);   // 绑定为输入
// 输出同理

// 推理时：不调 rknn_inputs_set / rknn_get_outputs
// RGA 直接写 input_mem→fd
// rknn_run(ctx, NULL) → NPU 直接读 input_mem
// CPU 直接读 output_mems[i]→virt_addr
```

### 对比

| 维度 | 通用模式 | 零拷贝模式（本项目） |
|------|---------|-------------------|
| 输入传递 | `rknn_inputs_set` 内部 memcpy | RGA `importbuffer_fd` 硬件直写 |
| 输出获取 | `rknn_get_outputs` 内部 memcpy | CPU 直接读 `virt_addr` |
| 额外内存 | 需要 CPU 侧临时 buffer | 无（RGA 直接写 NPU 内存） |
| 代码复杂度 | 简单 | 较复杂（需要管理 mem 生命周期） |
| 性能 | 每次推理多 2 次 memcpy | 零拷贝 |

---

## 8. 关键问答

**Q: 为什么 `rknn_run(ctx, NULL)` 不需要传入任何数据？**

因为输入输出内存已经通过 `rknn_set_io_mem` 绑定到 ctx 了。RGA 写入 `input_mem` 后，调用 `rknn_run`，NPU 硬件直接读取 `input_mem` 计算，结果写入 `output_mems`。不需要显式"传入数据"或"取出结果"。

**Q: 不零拷贝会怎样？**

每次推理前需要 `memcpy` 输入数据到 NPU 内存，推理后再 `memcpy` 结果回 CPU 内存。两张额外的内存拷贝在 30FPS 下就是每秒 60 次拷贝：
- 输入：1280x720 NV12→640x640 RGB 已由 RGA 完成（不在比较范围内）
- 但输入 memcpy 是把 RGA 结果从 CPU 内存拷到 NPU 内存：`640x640x3 ≈ 1.2MB`，每秒 30 次 ≈ 36MB/s
- 输出 memcpy 是把三个输出分支拷回 CPU 内存，数据量小得多但也是额外开销

**Q: 为什么输入用 `size_with_stride` 输出用 `size`？**

输入需要硬件 stride 对齐（NPU 要求每行 16/64 字节对齐），`size_with_stride` 是硬件实际需要的字节数。输出不需要 stride 对齐，`size` 就是实际数据大小。
