# 初始化、推理与后处理详解

---

## 一、Yolov5s 构造函数：初始化 RKNN 模型

构造函数 `Yolov5s::Yolov5s(const char *model_path, int npu_index)` 位于 `yolov5s.cpp:27-139`，完成了**模型加载 → RKNN 上下文创建 → NPU 核心绑定 → 张量属性查询 → 零拷贝内存分配**全流程。

### 1.1 加载模型文件（辅助函数）

```cpp
this->model_data = load_model(model_path, &this->model_data_size);
```

**`load_model()`** (`yolov5s.cpp:170-193`) — 加载 .rknn 模型文件到内存：
- 以二进制只读模式 `fopen` 打开模型文件
- `fseek(fp, 0, SEEK_END)` + `ftell(fp)` 获取文件大小
- 调用 `load_data()` 读取全部二进制数据
- 返回 `unsigned char*` 缓冲区（适配二进制字节流）

**`load_data()`** (`yolov5s.cpp:203-239`) — 从指定偏移读取二进制数据：
- `fseek(fp, ofst, SEEK_SET)` 定位到指定偏移
- `malloc(size)` 分配堆内存
- `fread(data, 1, size, fp)` 读取模型数据
- 读取失败时自动 `free` 防止内存泄漏

> `unsigned char*` 存储模型二进制，因为 RKNN 模型是二进制文件，`unsigned char` 可无符号存储，避免符号位错误。

### 1.2 初始化 RKNN 上下文

```cpp
ret = rknn_init(&this->ctx, model_data, model_data_size, RKNN_FLAG_PRIOR_HIGH, NULL);
```

- `rknn_init` 创建 RKNN 推理上下文（类似文件描述符，管理模型加载/推理）
- `RKNN_FLAG_PRIOR_HIGH` — 高优先级标志，确保推理任务优先调度
- `ctx` 是后续所有 API 调用（`rknn_run`、`rknn_query`、`rknn_destroy`）的句柄

### 1.3 绑定 NPU 核心

```cpp
rknn_core_mask core_mask;
if(npu_index == 0)      core_mask = RKNN_NPU_CORE_0;
else if(npu_index == 1)  core_mask = RKNN_NPU_CORE_1;
else                     core_mask = RKNN_NPU_CORE_2;
rknn_set_core_mask(ctx, core_mask);
```

RK3588 有 3 个 NPU 核心（Core 0/1/2），通过 `rknn_set_core_mask` 指定。在项目中，4 个 Worker 线程通过 `i % 3` 轮询分配到 3 个核心：
- Worker 0 → Core 0
- Worker 1 → Core 1
- Worker 2 → Core 2
- Worker 3 → Core 0（复用）

这样实现了 NPU 核心的负载均衡。

### 1.4 查询张量属性（获取 `scale` / `zp` 的关键）

```cpp
// 查询 SDK 版本
rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &this->version, sizeof(this->version));

// 查询输入输出数量
rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &this->io_num, sizeof(this->io_num));

// 查询每个输入张量属性
for(int i = 0; i < io_num.n_input; i++) {
    input_attrs[i].index = i;
    rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
}

// 查询每个输出张量属性
for(int i = 0; i < io_num.n_output; i++) {
    output_attrs[i].index = i;
    rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
}
```

**`printf_tensor_attr()`** (`yolov5s.cpp:8-21`) — 辅助函数，打印张量属性信息（调试用），包含：
- `index` — 张量索引
- `name` — 张量名称
- `n_dims` / `dims[]` — 维度信息
- `size` — 数据大小
- `fmt` — 数据格式（NCHW/NHWC）
- `scale` / `zp` — **量化参数**（后处理反量化的关键！）

#### 张量属性的作用

| 属性 | 后续用途 |
|------|---------|
| `input_attrs[0].fmt` | 解析模型输入格式（NCHW → `dims[1,2,3]` 为 C,H,W；NHWC → `dims[1,2,3]` 为 H,W,C），赋值 `model_height/width/channel` |
| `input_attrs[0].size_with_stride` | `rknn_create_mem` 分配输入零拷贝内存时使用，保证硬件 stride 对齐 |
| `output_attrs[i].size` | `rknn_create_mem` 分配各输出张量的零拷贝内存 |
| `output_attrs[i].zp` / `.scale` | `inference_zero_copy()` 中收集到 `qnt_zps` / `qnt_scales`，传给 `post_process` 做反量化 |
| `input_attrs[0].type` / `.fmt` | 强制设为 `UINT8` / `NHWC`，告诉驱动输入数据格式，匹配 RGA 写入格式 |

### 1.5 零拷贝内存绑定（核心机制）

```cpp
// 输入：分配 NPU 物理内存，按 stride 对齐
input_attrs[0].type = RKNN_TENSOR_UINT8;
input_attrs[0].fmt  = RKNN_TENSOR_NHWC;
input_mem = rknn_create_mem(ctx, input_attrs[0].size_with_stride);
rknn_set_io_mem(ctx, input_mem, &input_attrs[0]);

// 输出：为每个输出张量分配零拷贝内存
output_mems.resize(io_num.n_output);
for(int i = 0; i < io_num.n_output; i++) {
    output_mems[i] = rknn_create_mem(ctx, output_attrs[i].size);
    rknn_set_io_mem(ctx, output_mems[i], &output_attrs[i]);
}
```

**零拷贝机制原理**：

`rknn_tensor_mem` 结构体包含三个关键字段：
```
rknn_tensor_mem {
    .virt_addr     → CPU 通过 mmap 访问（推理后读输出）
    .fd            → RGA 通过 importbuffer_fd 访问（推理前写输入）
    .logical_addr  → NPU 内部使用（rknn_run 读输入、写输出）
}
```

| 角色 | 访问方式 | 时机 |
|------|---------|------|
| RGA（硬件） | `importbuffer_fd(input_mem->fd)` | 推理前写入输入图像 |
| NPU（硬件） | `logical_addr` | `rknn_run` 时读取输入、写入输出 |
| CPU（软件） | `output_mems[i]->virt_addr` | 推理后读取输出数据 |

- **输入**：`rknn_create_mem(ctx, size_with_stride)` — `size_with_stride` 保证硬件 stride 对齐
- **输出**：`rknn_create_mem(ctx, size)` — `size` 即实际数据大小，无需对齐

> 对比通用模式：通用接口 `rknn_inputs_set` 内部 memcpy + `rknn_get_outputs` 内部 memcpy，零拷贝模式消除了这两次 CPU 拷贝。

---

## 二、推理函数 `inference_zero_copy()` 

(`yolov5s.cpp:248-273`)

```cpp
int Yolov5s::inference_zero_copy(int raw_w, int raw_h, detect_result_group_t *group)
{
    // 1. 执行 NPU 推理（RGA 已通过 Path C 写入 input_mem）
    int ret = rknn_run(ctx, NULL);

    // 2. 计算缩放比例（模型坐标 → 原图坐标）
    float scale_w = (float)model_width / raw_w;
    float scale_h = (float)model_height / raw_h;

    // 3. 收集各输出张量的量化参数
    vector<int32_t> qnt_zps;
    vector<float> qnt_scales;
    for(int i = 0; i < io_num.n_output; i++) {
        qnt_zps.emplace_back(output_attrs[i].zp);
        qnt_scales.emplace_back(output_attrs[i].scale);
    }

    // 4. 后处理：解析 3 个输出分支的 INT8 数据
    post_process((int8_t*)output_mems[0]->virt_addr,
                 (int8_t*)output_mems[1]->virt_addr,
                 (int8_t*)output_mems[2]->virt_addr,
                 model_height, model_width,
                 BOX_THRESHOLD, NMS_THRESHOLD,
                 scale_w, scale_h,
                 qnt_zps, qnt_scales, group);
    return 0;
}
```

流程要点：
1. **`rknn_run(ctx, NULL)`** — NPU 读取 `input_mem`（RGA 已写入）执行推理，输出写入 `output_mems[i]`
2. **量化参数收集** — `output_attrs[i].zp` 和 `.scale` 是在构造函数中通过 `rknn_query` 获取的，每个输出分支各有一套独立的量化参数
3. **CPU 直接读取** — 通过 `output_mems[i]->virt_addr` 获取 NPU 输出数据，零拷贝

---

## 三、后处理详解

### 3.1 整体架构

```
NPU INT8 输出 (3 个尺度)
       ↓
post_process() ─── 入口函数，编排 6 个阶段
       ├── process_youhua()  → 单尺度解码（×3 个尺度）
       ├── sort_descending() → 按置信度降序排序
       ├── nms()            → 非极大值抑制
       └── 结果格式化        → 坐标缩放 + 输出 group
```

### 3.2 `post_process()` 主函数

(`post_process.cpp:352-480`)

`post_process` 是整个后处理的**入口和编排器**，负责将 NPU 输出的 3 个尺度 INT8 量化数据转化为最终的检测结果。它编排 6 个阶段，每个阶段依赖前一阶段的输出：

```
NPU 3× INT8 输出
  ↓ 阶段1: 初始化（标签加载，仅一次）
  ↓ 阶段2: 创建结果容器
  ↓ 阶段3: process_youhua() × 3 尺度 → 原始检测数据
  ↓ 阶段4: 按置信度降序排序
  ↓ 阶段5: NMS 去重
  ↓ 阶段6: 坐标缩放 → detect_result_group_t
```

#### 阶段 1：初始化（仅首次调用）

```cpp
static int init = -1;
int ret;
if(init == -1) {
    ret = loadLableName(LABEL_PATH, labels, OBJ_CLASS_NUM);
    if(ret == 0) {
        for(string &s: labels)
            cout << "lable_name : " << s << endl;
    } else {
        cout << "lable_name failed "<< endl;
    }
    init = 0;
}
```

**设计要点**：

1. **`static int init`** — 函数内的 `static` 变量存储在全局/静态区，而非栈上。首次进入时 `init = -1`，执行初始化后设为 `0`。后续每次调用 `post_process` 直接跳过此块。这是 C 语言经典的一次性初始化模式，比全局变量更安全（作用域受限）。

2. **标签文件路径 `LABEL_PATH = "../model/coco_80_labels_list.txt"`** — 这是一个**相对路径**，相对于运行时工作目录（通常为 `build/`），因此实际指向 `build/../model/coco_80_labels_list.txt` → `model/coco_80_labels_list.txt`。

3. **`labels` 全局变量** (`post_process.cpp:19`)：
   ```cpp
   vector<string> labels;
   ```
   声明在文件作用域，所有函数可见。`loadLableName` 将 COCO 80 类名称（"person"、"car"、"bicycle" 等）逐行读入。后续阶段 6 通过 `labels[id]` 将类别 ID 转为可读名称。

**辅助函数详解**：

**`readlines()`** (`post_process.cpp:28-49`)：
```cpp
int readlines(const char *filepath, vector<string>& lable_vector, int maxlines)
```
- 使用 `ifstream` 打开文件，`getline(file, line)` 逐行读取
- `lable_vector.emplace_back(line)` — `emplace_back` 直接构造元素，避免 `push_back` 的临时对象拷贝
- 读到 `maxlines` 行后停止（防止标签文件意外过大）
- 返回实际读取行数

**`loadLableName()`** (`post_process.cpp:58-66`)：
```cpp
int loadLableName(const char* filepath, vector<string>& lable_vector, int maxlines)
```
- 封装 `readlines`，传参 `OBJ_CLASS_NUM`（80）作为最大行数
- 打印读取的标签数量用于调试

#### 阶段 2：创建结果容器

```cpp
vector<float> detect_boxes;  // 所有框坐标（xmin,ymin,w,h 连续存储）
vector<float> objProbs;      // 所有框置信度
vector<int>   classId;       // 所有框类别 ID
```

**数据结构设计**：

| 容器 | 元素意义 | 访问方式 |
|------|---------|---------|
| `detect_boxes` | 每 4 个 float 为一组：`xmin, ymin, width, height` | 第 n 个框 → `detect_boxes[4*n + 0~3]` |
| `objProbs` | 每个 float 对应一个框的置信度 | 第 n 个框 → `objProbs[n]` |
| `classId` | 每个 int 对应一个框的类别 ID | 第 n 个框 → `classId[n]` |

**为什么用三个独立向量而非一个结构体数组？**

三个向量是"平行数组"（parallel arrays）设计，索引 n 关联同一框的三项信息。这种设计在后续排序和 NMS 阶段更灵活：
- 排序时只需要交换 `Probarray.index`（排序后的原始索引），而不必移动 `detect_boxes` 和 `classId` 中的大量数据
- NMS 时只需要将 `indexArray[i]` 标记为 -1（伪删除），无需物理删除元素

> 三个向量通过**隐式索引**关联：第 n 个框的坐标在 `detect_boxes[4*n~4*n+3]`，置信度在 `objProbs[n]`，类别 ID 在 `classId[n]`。这种范式在 C/C++ 高性能计算中很常见（如 AoS vs SoA 中的 SoA 风格）。

#### 阶段 3：多尺度输出解析

```cpp
// output0: stride=8，检测小目标
int stride0 = 8;
int grid_h0 = model_height / stride0;  // 640/8 = 80
int grid_w0 = model_width  / stride0;  // 640/8 = 80
validCount0 = process_youhua(output0, (int*)anchor0, grid_h0, grid_w0,
    model_height, model_width, stride0, detect_boxes, objProbs, classId,
    box_threshold, qnt_zps[0], qnt_scales[0]);

// output1: stride=16，检测中目标
int stride1 = 16;
int grid_h1 = model_height / stride1;  // 640/16 = 40
int grid_w1 = model_width  / stride1;  // 640/16 = 40
validCount1 = process_youhua(output1, (int*)anchor1, grid_h1, grid_w1,
    model_height, model_width, stride1, detect_boxes, objProbs, classId,
    box_threshold, qnt_zps[1], qnt_scales[1]);

// output2: stride=32，检测大目标
int stride2 = 32;
int grid_h2 = model_height / stride2;  // 640/32 = 20
int grid_w2 = model_width  / stride2;  // 640/32 = 20
validCount2 = process_youhua(output2, (int*)anchor2, grid_h2, grid_w2,
    model_height, model_width, stride2, detect_boxes, objProbs, classId,
    box_threshold, qnt_zps[2], qnt_scales[2]);

int vC = validCount0 + validCount1 + validCount2;
```

**YOLOv5 的三输出结构**：

YOLOv5 使用 FPN+PAN 特征金字塔 neck，产生 3 个不同分辨率的特征图输出：

```
Backbone → P3 (1/8)  ───→ Output0 (stride=8,  80×80)  小目标
                ↓
           P4 (1/16) ───→ Output1 (stride=16, 40×40)  中目标
                ↓
           P5 (1/32) ───→ Output2 (stride=32, 20×20)  大目标
```

| 属性 | Output0 | Output1 | Output2 |
|------|:-------:|:-------:|:-------:|
| 步长 stride | 8 | 16 | 32 |
| 网格尺寸 (640 输入) | 80×80 | 40×40 | 20×20 |
| 网格大小（原图像素） | 8×8 | 16×16 | 32×32 |
| 锚框 | `[10,13,16,30,33,23]` | `[30,61,62,45,59,119]` | `[116,90,156,198,373,326]` |
| 检测偏好 | 小目标 | 中目标 | 大目标 |

**为什么需要多尺度？**

- 小目标在低层特征图（stride=8）上像素信息更丰富，80×80 网格能捕获精细空间位置
- 大目标在高层特征图（stride=32）上语义信息更强，20×20 网格感受野更大
- 三个尺度互补，覆盖不同大小的目标

**量化参数的尺度独立性**：

关键：每个输出尺度的 `zp` 和 `scale` 可能不同！

```cpp
process_youhua(output0, ..., qnt_zps[0], qnt_scales[0]);  // 尺度0的量化参数
process_youhua(output1, ..., qnt_zps[1], qnt_scales[1]);  // 尺度1的量化参数
process_youhua(output2, ..., qnt_zps[2], qnt_scales[2]);  // 尺度2的量化参数
```

这是因为 PTQ（Post-Training Quantization）量化校准过程中，每个输出分支的数据分布不同，算出的最优 `scale`/`zp` 也不同。必须一一对应反量化，否则结果错误。

**三个 `process_youhua` 的结果合并到同一组向量**：

三个尺度的有效检测框都追加到 `detect_boxes` / `objProbs` / `classId` 的末尾，所以这些向量中框的排列顺序是：
```
[output0框1, output0框2, ..., output1框1, output1框2, ..., output2框1, ...]
```
后续的排序和 NMS 会对这个**合并后的集合**统一处理。

#### 阶段 4：按置信度降序排序

```cpp
vector<Probarray> prob_arry;
for (int i = 0; i < vC; i++) {
    Probarray temp;
    temp.index = i;
    temp.conf = objProbs[i];
    prob_arry.emplace_back(temp);
}
sort_descending(prob_arry);

// 分离排序后的置信度和原始索引
vector<int> indexArray;
objProbs.clear();
indexArray.clear();
for(int i = 0; i < vC; i++) {
    objProbs.emplace_back(prob_arry[i].conf);
    indexArray.emplace_back(prob_arry[i].index);
}
```

**`Probarray` 结构体** (`post_process.cpp:12-16`)：
```cpp
struct Probarray {
    float conf;   // 置信度
    int index;    // 对应检测框的原始索引
};
```

**为什么需要这个排序步骤？**

NMS 算法的前提是"从最高置信度的框开始处理"，因此必须先排序。

**为什么排序后要分离 `conf` 和 `index`？**

排序改变了元素顺序。排序后 `objProbs[i]` 是第 i 高的置信度，但对应的框坐标仍在 `detect_boxes` 的**原位置**。`indexArray[i]` 记录了"第 i 高的框在原数组中的位置"，这是后续 NMS 和结果格式化中连接排序后顺序与原始数据的桥梁。

```
排序前:  objProbs = [0.3, 0.9, 0.6, 0.1]
排序后:  objProbs = [0.9, 0.6, 0.3, 0.1]
         indexArray = [1,   2,   0,   3]  ← 原始索引
         
detect_boxes 和 classId 保持原样不变！
第 i 高的框 → detect_boxes[4 * indexArray[i]] 获取坐标
           → classId[indexArray[i]] 获取类别
```

**辅助函数详解**：

**`sort_descending()`** (`post_process.cpp:137-145`)：
```cpp
static int sort_descending(vector<Probarray>& p_arr) {
    sort(p_arr.begin(), p_arr.end(),
         [](const Probarray& a, const Probarray& b) {
            return a.conf > b.conf;  // 降序
         });
    return 0;
}
```
- 使用 `std::sort` + **lambda 表达式**自定义比较器
- `a.conf > b.conf` 表示降序（高置信度在前）
- `std::sort` 底层是**内省排序**（introsort），最坏复杂度 O(n log n)

#### 阶段 5：非极大值抑制（NMS）

```cpp
std::set<int> class_set(begin(classId), end(classId));

for(const int& id : class_set) {
    printf("model detect class_id = %d, is %s\n", id, labels[id].c_str());
    nms(vC, detect_boxes, classId, indexArray, id, nms_threshold);
}
```

**为什么按类别分别执行 NMS？**

同一目标不会同时是两个类别（"person" 和 "car" 不可能重叠为同一物体）。如果不按类别区分，一个高置信度的 "person" 框可能会错误地抑制一个重叠的 "car" 框。NMS 应该在**同类别的框之间**进行。

**`std::set<int> class_set`** 的作用：
- `std::set` 自动**去重**且**升序排序**
- 从 `classId` 中提取所有唯一的类别 ID，如 `{0, 2, 5}` 表示当前帧检测到了 person、car、bus
- 循环遍历每个类别，分别调用 `nms()`

**`nms()` 函数详解** (`post_process.cpp:185-228`)：

```cpp
static int nms(int validCount, vector<float>& boxes, vector<int>& classIds,
                vector<int>& index, int current_class, float nms_thres)
```

核心算法：
```
for i = 0 to validCount-1:           // 遍历排序后的框
    n = index[i]                      // 原始索引
    if n == -1 or classIds[n] != current_class:
        continue                      // 跳过无效框或非当前类别
    
    for j = i+1 to validCount-1:     // 与后续框比较
        m = index[j]
        if m == -1 or classIds[j] != current_class:
            continue
        
        iou = calculateIOU(boxes[n], boxes[m])
        if iou > nms_thres:          // 重叠严重
            index[j] = -1            // 标记为抑制
```

**NMS 算法图解**：
```
排序后框: [A(0.9), B(0.6), C(0.5), D(0.4)]  类别: person
         ↓ 第1轮
保留 A(最高)，计算 A vs B, C, D 的 IOU
IOU(A,B) > 0.5 → B 被抑制(index = -1)
IOU(A,C) < 0.5 → C 保留
IOU(A,D) > 0.5 → D 被抑制(index = -1)
         ↓ 第2轮
保留 C(次高，未被抑制)，无后续同类框 → 结束
最终保留: A, C
```

**`calculateIOU()` 函数详解** (`post_process.cpp:159-173`)：

```cpp
static float calculateIOU(float xmin0, float ymin0, float xmax0, float ymax0,
                          float xmin1, float ymin1, float xmax1, float ymax1)
{
    // 交集宽高（无交集则为0）
    float w = fmax(0.f, fmin(xmax0, xmax1) - fmax(xmin0, xmin1) + 1.0);
    float h = fmax(0.f, fmin(ymax0, ymax1) - fmax(ymin0, ymin1) + 1.0);
    float inter_area = w * h;

    // 并集 = 框0面积 + 框1面积 - 交集面积
    float union_area = (xmax0 - xmin0 + 1.0) * (ymax0 - ymin0 + 1.0) +
                       (xmax1 - xmin1 + 1.0) * (ymax1 - ymin1 + 1.0) - inter_area;

    return union_area <= 0.f ? 0.f : (inter_area / union_area);
}
```

```
       ┌───────A────────┐
       │                │
       │   ┌───B───┐    │
       │   │ 交集  │    │
       │   └───────┘    │
       └────────────────┘

交集: fmin(xmax_A,xmax_B) - fmax(xmin_A,xmin_B) + 1
     × fmin(ymax_A,ymax_B) - fmax(ymin_A,ymin_B) + 1
IOU = 交集面积 / (A面积 + B面积 - 交集面积)
```

注意：`nms()` 函数内部在框比较时，`boxes` 存储的格式是 `xmin, ymin, width, height`，因此需要将 width/height 转换为 `xmax = xmin + width`、`ymax = ymin + height` 后再计算 IOU。

#### 阶段 6：结果格式化

```cpp
int count = 0;
group->box_count = 0;

for(int i = 0; i < vC; i++) {
    if(indexArray[i] == -1 || count >= OBJ_NUM_MAX_SIZE)
        continue;

    int n = indexArray[i];                    // 原始索引
    float box_conf = objProbs[i];             // 排序后的置信度

    float xmin = detect_boxes[4 * n + 0];
    float ymin = detect_boxes[4 * n + 1];
    float xmax = detect_boxes[4 * n + 2] + xmin;  // w → xmax
    float ymax = detect_boxes[4 * n + 3] + ymin;  // h → ymax
    int id = classId[n];

    // 坐标缩放 + 钳位
    group->result[count].box.left_top_x = (int)(clamp(xmin, 0, model_width) / scale_w);
    group->result[count].box.left_top_y = (int)(clamp(ymin, 0, model_height) / scale_h);
    group->result[count].box.right_bottom_x = (int)(clamp(xmax, 0, model_width) / scale_w);
    group->result[count].box.right_bottom_y = (int)(clamp(ymax, 0, model_height) / scale_h);
    group->result[count].box_conf = box_conf;

    const char *label_temp = labels[id].c_str();
    strncpy(group->result[count].label_name, label_temp, LABEL_NAME_MAX_SIZE);
    count++;
}
group->box_count = count;
```

**关键索引映射（面试重点）**：

这是代码中最容易混淆的地方，需要透彻理解 `i` 和 `n` 的区别：

| 变量 | 含义 | 用于索引 |
|------|------|---------|
| `i` | **排序后**的排名位置（循环变量） | `objProbs[i]`（排序后的置信度）、`indexArray[i]`（原始索引） |
| `n = indexArray[i]` | **原始存储**位置 | `detect_boxes[4*n]`（坐标）、`classId[n]`（类别） |

```
排序后状态:
    排名(i):   0     1     2     3
    objProbs: 0.9   0.6   0.5   0.4       ← 排序后的置信度
    indexArray: [1,   2,    0,    3]       ← 原始索引

    第0高的框(i=0): n = indexArray[0] = 1
      → detect_boxes[4] → xmin (原始位置1的框坐标)
      → classId[1] → 类别
      → objProbs[0] → 置信度 0.9

    第1高的框(i=1): n = indexArray[1] = 2
      → detect_boxes[8] → xmin (原始位置2的框坐标)
```

**坐标缩放**：

```cpp
group->result[count].box.left_top_x = (int)(clamp(xmin, 0, model_width) / scale_w);
```

- `xmin` 是**模型输入坐标系**下的坐标（如 640×640 空间中的值）
- `scale_w = model_width / raw_w`（如 640/1280=0.5）
- 除以 `scale_w` 还原到**原始图像坐标系**：`xmin_orig = xmin / (model_width/raw_w) = xmin * raw_w / model_width`
- `clamp()` 将坐标限制在 `[0, model_width]` 范围内，防止坐标越界

例如：模型检测到 xmin=100（在 640×640 空间中），原图宽 1280：
```
xmin_original = 100 / (640/1280) = 100 × 2 = 200 像素
```

**`clamp()` 函数** (`post_process.cpp:331-333`)：
```cpp
inline static int clamp(float val, int min, int max) {
    return val > min ? (val < max ? val : max) : min;
}
```
嵌套三元运算符：`val > min` → 再判断是否 `< max`，否则取 max；`val <= min` → 取 min。等价于 `std::clamp`（C++17），但手动实现避免依赖版本。

**输出结构体**：
```cpp
typedef struct _detect_result_group_t {
    int box_count;           // 最终有效框数
    detect_result_t result[OBJ_NUM_MAX_SIZE];  // 最多 64 个结果
} detect_result_group_t;

typedef struct _detect_result_t {
    char label_name[LABEL_NAME_MAX_SIZE];  // 类别名（如 "person"）
    float box_conf;                        // 置信度
    box_t box;                             // 坐标
} detect_result_t;
```

这个结构体最终返回给 `Yolov5s::inference_zero_copy()` 的调用者（Worker 线程），用于在图像上绘制检测框和标签。

### 3.3 `process_youhua()` 优化版单尺度解码器

(`post_process.cpp:247-322`)

这是后处理**最核心的函数**，也是最体现优化思维的部分。

#### 输入参数

| 参数 | 含义 |
|------|------|
| `input` | 当前尺度的 INT8 量化输出 |
| `anchor` | 锚框参数（3 组宽高） |
| `grid_h/grid_w` | 网格行数/列数 |
| `stride` | 下采样步长 |
| `box_threshold` | 置信度阈值 |
| `zp/scale` | 量化参数 |
| `boxes/objProbs/classId` | 输出（引用传递） |

#### 核心流程

**Step 1 — 阈值预转换（延迟解码的关键）**

```cpp
float thres = unsigmoid(box_threshold);      // 阈值 0.5 → logits 值
int8_t threa_i8 = qnt_float_to_int8(thres, zp, scale);  // logits → INT8 量化值
```

**Step 2 — 三重循环**：遍历 3 个锚框 × grid_h 行 × grid_w 列

**Step 3 — 第一道快筛**：INT8 域直接比较

```cpp
int8_t box_anchor_conf = input[a * BOX_NUM_SIZE * grid_len + 4 * grid_len + b * grid_w + c];
if (box_anchor_conf >= threa_i8) {  // 纯整数比较，极快
    // 进入后续处理
}
```

**Step 4 — 量化域内找最佳类别**（纯整数比较，无浮点运算）

```cpp
int8_t maxClassProb = *(box_p + 5 * grid_len);
int maxClassID = 0;
for (int k = 1; k < OBJ_CLASS_NUM; k++) {
    int8_t prob = *(box_p + (5 + k) * grid_len);
    if (prob > maxClassProb) { maxClassProb = prob; maxClassID = k; }
}
```

**Step 5 — 第二道严筛**：反量化 + Sigmoid 计算最终得分

```cpp
float obj_prob = sigmoid(deqnt_int8t_to_f32(box_anchor_conf, zp, scale));
float cls_prob = sigmoid(deqnt_int8t_to_f32(maxClassProb, zp, scale));
float final_score = obj_prob * cls_prob;  // 目标存在概率 × 类别概率
if (final_score >= box_threshold) {
    // 延迟解码坐标
}
```

**Step 6 — 坐标解码**（延迟执行，仅对精英框）

```cpp
float box_x = sigmoid(deqnt_int8t_to_f32(*(box_p + 0 * grid_len), zp, scale)) * 2.0f - 0.5f;
float box_y = sigmoid(deqnt_int8t_to_f32(*(box_p + 1 * grid_len), zp, scale)) * 2.0f - 0.5f;
float box_w = sigmoid(deqnt_int8t_to_f32(*(box_p + 2 * grid_len), zp, scale)) * 2.0f;
float box_h = sigmoid(deqnt_int8t_to_f32(*(box_p + 3 * grid_len), zp, scale)) * 2.0f;

// 网格坐标 → 像素坐标
box_x = (box_x + c) * (float)stride;
box_y = (box_y + b) * (float)stride;
// 锚框缩放还原
box_w = box_w * box_w * (float)anchor[a * 2];
box_h = box_h * box_h * (float)anchor[a * 2 + 1];
// 中心坐标 → 左上角坐标
box_x = box_x - (box_w / 2.0f);
box_y = box_y - (box_h / 2.0f);
```

---

### 3.4 辅助函数汇总

| 函数 | 所在文件 | 行号 | 作用 |
|------|---------|:----:|------|
| `printf_tensor_attr()` | `yolov5s.cpp` | 8-21 | 打印张量属性（调试用） |
| `load_model()` | `yolov5s.cpp` | 170-193 | 加载 .rknn 模型二进制文件 |
| `load_data()` | `yolov5s.cpp` | 203-239 | 从文件偏移读取二进制数据 |
| `readlines()` | `post_process.cpp` | 28-49 | 逐行读取文件到字符串向量 |
| `loadLableName()` | `post_process.cpp` | 58-66 | 加载类别标签名称 |
| `deqnt_int8t_to_f32()` | `post_process.cpp` | 75-79 | INT8 反量化 → float |
| `__limit_num()` | `post_process.cpp` | 88-93 | 数值限制 |
| `qnt_float_to_int8()` | `post_process.cpp` | 102-108 | float 量化 → INT8 |
| `sigmoid()` | `post_process.cpp` | 115-119 | Sigmoid 激活函数 |
| `unsigmoid()` | `post_process.cpp` | 126-130 | Sigmoid 逆函数 |
| `sort_descending()` | `post_process.cpp` | 137-145 | 按置信度降序排序 |
| `calculateIOU()` | `post_process.cpp` | 159-173 | 计算交并比 |
| `nms()` | `post_process.cpp` | 185-228 | 非极大值抑制 |
| `clamp()` | `post_process.cpp` | 331-333 | 数值钳位 |

---

## 四、INT8 输出 + scale + zp 还原真实概率

### 4.1 量化的必要性

NPU 硬件为了提速和降功耗，将浮点数（float32，4 字节）压缩为 INT8（1 字节），内存需求降低 75%，且硬件对 INT8 运算有专门的加速支持。

### 4.2 反量化公式

```
浮点数 = (INT8_值 - zp) × scale
```

其中：
- **`zp` (zero point)** — 量化零点，浮点数 0 对应的 INT8 值
- **`scale`** — 缩放因子，INT8 与 float 的"单位换算比例"

### 4.3 代码中的反量化实现

```cpp
static float deqnt_int8t_to_f32(int8_t int_num, int32_t zp, float scale)
{
    return ((float)int_num - (float)zp) * scale;
}
```

### 4.4 完整还原链路

```
NPU 输出 INT8 值
      ↓
(int8 - zp) × scale          ← 反量化为浮点数 logit
      ↓
sigmoid(logit)               ← 激活为 0~1 的概率
      ↓
Score = obj_prob × cls_prob  ← 融合最终置信度
```

### 4.5 优化技巧：延迟解码

常规做法是对所有 INT8 数据先反量化再比较，但 `process_youhua` 采用了**逆向思维优化**：

```
阈值 0.5
  → unsigmoid(0.5)          → logits 值
  → qnt_float_to_int8(logits) → INT8 阈值 (如 60)
  → if (box_anchor_conf >= 60)  ← 纯整数比较，极快
  → 仅对通过的做反量化+坐标解码   ← 延迟解码
```

优势：一张图片中大部分网格是背景，第一道 INT8 快筛可瞬间剔除 90%+ 的无用数据。

### 4.6 最终置信度公式（面试重点）

```cpp
final_score = obj_prob × cls_prob
```

即：**目标存在概率 × 最大类别概率**。两者在 YOLOv5 输出中分别存储，必须相乘才是真正的检测置信度。仅使用类别概率会导致高分的假阳性误检。

---

## 五、数据流总结

```
构造函数:
  load_model → rknn_init → rknn_set_core_mask
  → rknn_query(张量属性) → 解析 model_width/height
  → rknn_create_mem(输入+输出) → rknn_set_io_mem
       ↓
推理:
  RGA 写入 input_mem(fd) → rknn_run
  → CPU 读取 output_mems[i]->virt_addr
       ↓
后处理:
  post_process()
    ├── process_youhua(3 尺度) → INT8 反量化 + 置信度筛选 + 坐标解码
    ├── sort_descending()      → 按置信度排序
    ├── nms()                 → 去重
    └── 结果格式化             → 坐标缩放 + 输出 group
```

---

## 六、各小节背诵版本（速记版）

> 以下为每个小节的**精炼背诵版**，适合面试前快速回顾。每节控制在 3-8 个要点，黑体字为核心记忆锚点。

---

### 6.1 模型加载（load_model / load_data）

| 步骤 | 函数 | 一句话 |
|:----:|:----:|:-------|
| 1 | `fopen` + `fseek` + `ftell` | 打开文件，定位末尾得大小 |
| 2 | `load_data()` | 从指定偏移 `fread` 全部二进制，`malloc` 分配内存 |
| 3 | 返回 `unsigned char*` | **适配二进制字节流，避免符号位错误** |

---

### 6.2 RKNN 初始化 + NPU 核心

```
① rknn_init(ctx, model_data, size, RKNN_FLAG_PRIOR_HIGH, NULL)
   → 创建 RKNN 上下文（高优先级）
② rknn_set_core_mask(ctx, core_mask)
   → 绑定 NPU 核心（RK3588 有 3 个，4 Worker 通过 i%3 轮询）
```

---

### 6.3 张量属性查询（rknn_query）

| 查询 | 获取内容 | 后续用途 |
|:----|:---------|:---------|
| `RKNN_QUERY_IN_OUT_NUM` | 输入/输出数量 | 分配 `input_attrs` / `output_attrs` 容器 |
| `RKNN_QUERY_INPUT_ATTR` | `fmt`, `dims[]`, `size_with_stride` | **解析 model_h/w**、分配输入零拷贝内存 |
| `RKNN_QUERY_OUTPUT_ATTR` | `size`, **`zp`**, **`scale`** | 分配输出零拷贝内存、传给 `post_process` 反量化 |

**背诵口诀**："数量查 I/O，输入看格式+stride 对齐，输出拿 zp/scale 反量化"

---

### 6.4 零拷贝内存（rknn_tensor_mem）

```
rknn_tensor_mem {
    .virt_addr    → CPU 直读（推理后读输出）
    .fd           → RGA 直写（推理前写输入）
    .logical_addr → NPU 内部（rknn_run 读/写）
}
```

**同一块物理内存，三方通过不同接口访问，零拷贝。**

| 角色 | 接口 | 方向 |
|:----|:----|:----|
| RGA | `importbuffer_fd(fd)` → 写入 | 输入 |
| NPU | `logical_addr` → rknn_run | 输入+输出 |
| CPU | `virt_addr` → 直接读 | 输出 |

- **输入**：`rknn_create_mem(ctx, size_with_stride)` — stride 对齐保证硬件兼容
- **输出**：`rknn_create_mem(ctx, size)` — 实际数据大小即可
- 对比通用模式：省去 **2 次** CPU memcpy（`rknn_inputs_set` + `rknn_get_outputs`）

---

### 6.5 `inference_zero_copy()` 推理

**三步流程**：
1. **`rknn_run(ctx, NULL)`** — NPU 开始推理
2. **收集量化参数** — `output_attrs[i].zp` + `.scale` → `qnt_zps` / `qnt_scales`
3. **后处理** — 3 个 `output_mems[i]->virt_addr` + 量化参数 → `post_process`

**注意**：RGA 已在推理前通过 Path C 将输入图像直写到 `input_mem->fd`，所以推理时 NPU 直接读取。

---

### 6.6 `post_process()` 六阶段（面试高频）

| 阶段 | 做什么 | 口诀 |
|:---:|:-------|:-----|
| ① | `static init` 标签文件只加载一次 | **首次加载** |
| ② | 建 3 个向量存框/置信度/类别 | **建容器** |
| ③ | 3 次 `process_youhua` 解析 3 个尺度 | **三尺度解码** |
| ④ | `Probarray` 排序 + `indexArray` 记录原始索引 | **按置信度排序** |
| ⑤ | 按类别分别 NMS，`index[i] = -1` 标记抑制 | **NMS 去重** |
| ⑥ | 坐标缩放/钳位 → 输出 `detect_result_group_t` | **格式化输出** |

**六阶段连背**："①首次加载 → ②建容器 → ③三尺度解码 → ④按置信度排序 → ⑤NMS 去重 → ⑥格式化输出"

**面试核心追问点**：
- 阶段④：**`i` vs `n` 索引映射** — `i` = 排序后排名，`n = indexArray[i]` = 原始位置
- 阶段⑤：**为什么按类别 NMS？** — 不同类别的框可能重叠（如人和车），不区分会错误抑制
- 阶段⑥：**坐标缩放** — `coord_orig = coord_model / (model_w / raw_w)`

---

### 6.7 `process_youhua()` 延迟解码优化（面试亮点）

```
           用户阈值 0.5
               ↓
        unsigmoid(0.5) → logits
               ↓
        qnt_float_to_int8 → INT8 阈值（如 60）
               ↓
   ┌──────────────────────────────────────┐
   │ 第一道快筛: 纯 INT8 比较              │
   │ if (box_anchor_conf >= 60) → 继续     │ ← 排除 90%+ 背景网格
   │              else → 直接跳过          │
   └──────────────────────────────────────┘
               ↓
   第二道严筛: 反量化 + Sigmoid
   final_score = sigmoid(obj_conf) × sigmoid(cls_prob)
   if (final_score >= 0.5) → 解码坐标
               ↓
   延迟解码坐标: 
   box_x/y/w/h → 网格坐标 → 像素坐标 → 左上角格式
```

**关键词**："INT8 域快筛 → 延迟解码 → 仅精英框反量化"

---

### 6.8 INT8 量化和反量化

| 操作 | 公式 | 函数 |
|:----|:-----|:-----|
| **反量化** | `float = (int8 - zp) × scale` | `deqnt_int8t_to_f32()` |
| **量化** | `int8 = clamp((float / scale) + zp, -128, 127)` | `qnt_float_to_int8()` |

**完整还原链路**：
```
INT8 → (int8 - zp) × scale → float logit → sigmoid → 0~1 概率
```

**为什么用 INT8**？内存减 75%，NPU 硬件加速 8 位整数运算。

---

### 6.9 辅助函数速记

| 函数 | 一句话 |
|:-----|:-------|
| `sigmoid()` | `1/(1+e^(-x))`，任意实数 → [0,1] |
| `unsigmoid()` | sigmoid 逆运算，概率 → logits |
| `calculateIOU()` | 交集面积 / 并集面积 |
| `nms()` | 按类别遍历，IOU > 阈值 → 抑制 |
| `sort_descending()` | lambda 降序排序 |
| `clamp()` | 值限制在 [min, max] |
| `printf_tensor_attr()` | 打印张量维度/格式/量化参数 |
| `readlines()` | `getline` 逐行读文件到 `vector<string>` |
| `loadLableName()` | 封装 readlines，加载 COCO 80 类 |

---
