## post\_process

```c++
int post_process(int8_t *output0,int8_t *output1,int8_t *output2,int model_height,int model_width,float box_threshold,float nms_threshold,float scale_w,float scale_h,std::vector<int32_t>& qnt_zps,std::vector<float>& qnt_scales,detect_result_group_t* group)
```

`post_process` 函数是目标检测模型（如 YOLO 系列）推理后的**核心结果处理入口**，负责将模型输出的 3 个尺度量化数据（int8 类型），转化为最终可使用的检测结果（包含边界框坐标、类别名称、置信度）。整体流程按执行顺序可拆解为 **6 大核心阶段**，每个阶段的逻辑和作用如下：

### 阶段 1：初始化（仅首次调用执行）

核心目标：加载类别标签（如 COCO 80 类），避免重复IO操作，提升效率。

1. **判断初始化状态**：通过静态变量 `init` 标记（初始值 `-1`），仅首次调用进入初始化；

2. **加载类别文件**：调用 `loadLableName` 函数，读取 `LABEL_PATH`（`../model/coco_80_labels_list.txt`）中的类别名称，存入全局向量 `labels`；

3) **验证加载结果**：

   * 若加载成功（`ret == 0`），打印所有类别名称（如 "person"、"car" 等）；

   * 若加载失败（如文件路径错误），打印错误提示；

4) **标记初始化完成**：将 `init` 设为 `0`，后续调用跳过此阶段。

### 阶段 2：初始化结果存储容器

核心目标：创建临时向量，存储 3 个尺度解析出的原始检测数据（避免内存碎片化）。

* `detect_boxes`：存储边界框参数（每个框占 4 个元素：xmin、ymin、width、height）；

* `objProbs`：存储每个边界框的**类别置信度**（float 类型，0\~1 之间）；

* `classId`：存储每个边界框对应的**类别 ID**（如 0 对应 "person"，2 对应 "car"）。

### 阶段 3：多尺度输出解析（核心逻辑）

模型输出 3 个不同尺度的量化数据（`output0`/`output1`/`output2`），对应不同检测步长（`stride=8`/`16`/`32`），分别解析每个尺度的检测结果（3 个尺度逻辑一致，仅参数不同）。

以 `output0`（stride=8，检测小目标）为例，单尺度解析流程：

1. **计算尺度参数**：

   * 步长 `stride0 = 8`（模型下采样倍数，步长越小，检测小目标越灵敏）；

   * 网格尺寸 `grid_h0 = model_height / stride0`、`grid_w0 = model_width / stride0`（如模型输入 640×640 时，grid 为 80×80）；

2. **调用 `process` 函数解析**：

   * 传入参数：当前尺度的量化数据（`output0`）、锚框（`anchor0`）、网格尺寸、步长、置信度阈值（`box_threshold`）、量化参数（`qnt_zps[0]`/`qnt_scales[0]`）；

   * `process` 函数核心作用：

     * 筛选置信度≥阈值的有效框（排除低置信度噪声）；

     * 将量化的坐标/置信度反量化为浮点数，并还原为图像绝对坐标；

     * 记录每个有效框的边界框、置信度、类别 ID，存入 `detect_boxes`/`objProbs`/`classId`；

3) **重复解析其他尺度**：

   * `output1`：步长 `16`，网格尺寸 `model_height/16 × model_width/16`，使用锚框 `anchor1`；

   * `output2`：步长 `32`，网格尺寸 `model_height/32 × model_width/32`，使用锚框 `anchor2`；

4) **统计总有效框数量**：`vC = validCount0 + validCount1 + validCount2`（3 个尺度有效框之和）。

### 阶段 4：按置信度降序排序

核心目标：为后续非极大值抑制（NMS）做准备，确保优先保留高置信度的框。

1. **构建“置信度-索引”映射**：

   * 创建 `Probarray` 类型向量 `prob_arry`（每个元素包含 `conf` 置信度和 `index` 原始索引）；

   * 遍历 `objProbs`，将每个框的置信度和对应索引存入 `prob_arry`；

2. **降序排序**：调用 `sort_descending` 函数，按 `conf` 从大到小排序（高置信度框在前）；

3) **重构置信度和索引**：

   * 清空原 `objProbs` 和新向量 `indexArray`；

   * 遍历排序后的 `prob_arry`，将置信度重新存入 `objProbs`，原始索引存入 `indexArray`（关联排序后置信度与原始边界框/类别）。

### 阶段 5：非极大值抑制（NMS）

核心目标：消除同一目标的重叠框，保留最优检测结果（避免重复标注）。

1. **获取所有unique类别**：通过 `std::set<int> class_set` 对 `classId` 去重，得到当前检测到的所有类别（如仅检测到“person”和“dog”）；

2. **按类别执行NMS**：对每个类别（如“person”）单独执行 NMS：

   * 调用 `nms` 函数，传入总有效框数 `vC`、边界框 `detect_boxes`、类别ID `classId`、索引 `indexArray`、当前类别 `id`、NMS 阈值 `nms_threshold`；

   * `nms` 函数核心逻辑：

     * 遍历高置信度框，计算其与后续框的交并比（IOU）；

     * 若 IOU > 阈值（如 0.5），说明两框重叠严重，将后续框的索引设为 `-1`（标记为无效）；

3) **输出结果**：同一目标仅保留 IOU 最小、置信度最高的框。

### 阶段 6：结果格式化与输出

核心目标：将 NMS 后的有效结果，转换为最终的 `detect_result_group_t` 格式（包含坐标缩放、类别名称、置信度），供上层调用（如绘制框、打印结果）。

1. **初始化计数与结果容器**：

   * `count` 记录最终有效框数量；

   * 置空 `group->box_count`（结果组的框数量）；

2. **遍历筛选有效框**：遍历 `indexArray`，跳过索引为 `-1`（NMS 标记无效）或 `count ≥ OBJ_NUM_MAX_SIZE`（超过最大框数量限制）的框；

3) **坐标缩放与修正**：

   * 通过 `indexArray[i]` 拿到原始索引 `n`，从 `detect_boxes` 中提取 xmin/ymin/width/height；

   * 计算 xmax = xmin + width、ymax = ymin + height；

   * 调用 `clamp` 函数将坐标限制在图像范围内（避免超出 0\~model\_width/height）；

   * 按 `scale_w`（图像宽度缩放比）和 `scale_h`（图像高度缩放比），将模型输入尺寸的坐标转换为原始图像尺寸（如模型输入 640×640，原始图像 1280×720，`scale_w=640/1280=0.5`）；

4) **填充结果**：

   * 将缩放后的坐标（left\_top\_x/left\_top\_y/right\_bottom\_x/right\_bottom\_y）存入 `group->result[count].box`；

   * 存入置信度 `box_conf` 和类别名称（通过 `labels[id].c_str()` 拿到名称，用 `strncpy` 存入 `label_name`）；

   * `count` 自增；

5. **最终结果**：将 `count` 赋值给 `group->box_count`，函数返回 `0`（成功）。

### 总结：整体流程梳理

```plain&#x20;text
graph TD
    A[初始化：加载类别标签] --> B[创建临时容器：存储边界框/置信度/类别ID]
    B --> C[多尺度解析：output0/1/2 → 有效框筛选+坐标还原]
    C --> D[置信度排序：高置信度框在前]
    D --> E[NMS：消除重叠框，保留最优框]
    E --> F[结果格式化：坐标缩放+填充类别/置信度 → 输出group]
```



该函数的核心价值是**打通“模型量化输出”到“可用检测结果”的链路**，通过多尺度解析覆盖不同大小目标、NMS 消除冗余框，最终输出精准、无重复的检测结果。

## Process

`process` 函数是目标检测后处理的**核心子模块**，负责将**单个尺度**的模型量化输出（int8 数据）解析为「边界框、类别置信度、类别 ID」三大核心信息，并筛选出置信度达标的有效结果。其本质是“模型输出解码 + 有效结果筛选”，流程可拆解为 **7 个关键步骤**，每个步骤的逻辑和作用如下：

### 先明确输入参数的核心意义

在理解流程前，需先理清关键输入参数的作用（避免后续逻辑混淆）：

| 参数名                      | 类型             | 核心作用                                                      |
| ------------------------ | -------------- | --------------------------------------------------------- |
| `input`                  | int8\_t\*      | 单个尺度的模型量化输出（如 output0/output1/output2），存储坐标、置信度的量化值       |
| `anchor`                 | int\*          | 当前尺度的锚框（先验框）参数（如 anchor0 的 \[10,13,16,30,33,23]），用于还原目标尺寸 |
| `grid_h/grid_w`          | int            | 当前尺度的网格行数/列数（= 模型高度/宽度 ÷ 步长），对应模型输出的空间维度                  |
| `stride`                 | int            | 当前尺度的检测步长（如 8/16/32），用于将“网格坐标”转换为“图像像素坐标”                 |
| `box_threshold`          | float          | 边界框置信度阈值（如 0.5），低于此值的框会被过滤                                |
| `zp/scale`               | int32\_t/float | 量化参数（零点/缩放因子），用于将 int8 量化值反量化为原始浮点数                       |
| `boxes/objProbs/classId` | vector&        | 输出参数（引用传递），分别存储有效框的边界框、置信度、类别 ID                          |

### 步骤 1：初始化与参数计算

核心目标：准备基础计算参数，避免循环内重复计算，提升效率。

1. **统计有效框数量**：初始化 `validCount = 0`（记录当前尺度筛选出的有效框总数）；

2. **计算网格总数量**：`grid_len = grid_h * grid_w`（当前尺度的网格单元格总数，如 80×80=6400）；

3) **阈值转换（关键！）**：将“浮点数置信度阈值”转为“int8 量化阈值”，用于直接与模型输出的 int8 数据比较（避免循环内频繁类型转换）：

   * 第一步：`unsigmoid(box_threshold)` → 将 0\~1 的置信度阈值（如 0.5）转为模型输出的“原始 logits 值”（因为模型输出是未经过 sigmoid 的量化 logits，而非直接的概率）；

   * 第二步：`qnt_float_to_int8(...)` → 将原始 logits 值量化为 int8 类型（`threa_i8`），后续直接用 `threa_i8` 与 `input` 中的量化置信度比较。

### 步骤 2：三重循环遍历所有检测单元

核心目标：遍历当前尺度下“锚框 + 网格”的所有可能检测单元，不遗漏任何潜在目标。 &#x20;

循环结构为 **“锚框循环 → 网格行循环 → 网格列循环”**，对应模型输出的三维结构（3 个锚框 × grid\_h 行 × grid\_w 列）：

* 外层循环（`a=0~2`）：遍历 3 个锚框（每个锚框对应不同尺寸的先验目标，覆盖不同大小的潜在目标）；

* 中层循环（`b=0~grid_h-1`）：遍历网格的每一行（对应图像的垂直方向）；

* 内层循环（`c=0~grid_w-1`）：遍历网格的每一列（对应图像的水平方向）；

每个循环组合（`a,b,c`）对应一个“检测单元”（如“锚框 0 + 第 5 行网格 + 第 10 列网格”），负责检测该单元范围内的目标。

### 步骤 3：筛选置信度达标的检测单元

核心目标：排除连“物体”都不存在的背景网格，初筛掉绝大多数的无效计算。 &#x20;

1. **提取当前单元的量化置信度**：从 `input` 中定位并读取当前检测单元的“边界框置信度”（int8 类型）：

   * 定位逻辑：`input[ a * BOX_NUM_SIZE * grid_len + 4 * grid_len + b * grid_w + c ]`

   （`BOX_NUM_SIZE` 是每个锚框的输出维度数，`4 * grid_len` 对应坐标输出后的置信度偏移，最终定位到当前单元的置信度）；

2. **阈值快筛比较：**&#x82E5; `box_anchor_conf >= threa_i8`（置信度达标），才准许进入后续逻辑解析该单元的类别；否则直接跳过该单元（跳过原本庞大的坐标浮点计算）。

**步骤 4：寻找最优类别（量化域内极速查找）** 核心目标：从所有类别中，找到当前检测单元最可能的类别概率（P\_class）。

* **初始化类别置信度**：提取当前单元第一个类别的量化置信度作为初始最大值 `maxClassProb`，记录初始类别 ID 为 0；

* **遍历所有类别**：循环 `k=1~OBJ_CLASS_NUM-1`，提取第 k 类的量化置信度 `prob = *(box_p + (5 + k) * grid_len)`；

* **更新最优类别**：若 `prob > maxClassProb`，则更新 `maxClassProb` 为当前 `prob`，`maxClassID` 为 k；

* **量化域极速对比（优化点）**：此处的比较全程在 int8 量化域内完成，速度极快，完全没有触发反量化（Dequantization）开销。

**步骤 5：计算最终真实置信度（Score融合）** 核心目标：严格对齐 YOLO 算法的理论评分公式，彻底消除“高分假阳性（误检）”。

* **反量化与激活**：将第一道快筛拿到的目标存在概率（`box_anchor_conf`）和最优类别概率（`maxClassProb`）分别进行 `deqnt_int8t_to_f32` 反量化，并经过 `sigmoid` 激活转为 0\~1 的浮点数；

`sigmoid( deqnt_int8t_to_f32(maxClassProb, zp, scale) )`。

* **融合计算得分**：通过公式 `final_score = obj_prob * cls_prob`（目标存在概率 × 类别概率）计算出真正的最终置信度；

* **修正算法漏洞**：一个框只有在“确实有个物体”且“很大概率是某类”时，才能获得高分。原版逻辑仅使用类别概率作为最终得分，会导致严重的得分注水。

### 步骤 4：解析并还原边界框坐标

核心目标：将最消耗 CPU 算力的坐标解算（大量浮点乘加运算）推迟到最后，仅对真正的“精英框”执行。将模型输出的“量化网格相对坐标”，转换为“图像绝对像素坐标”（标准边界框格式：xmin, ymin, width, height）。 &#x20;

这是函数最核心的“解码”逻辑，分 4 小步：

#### 4.1 反量化 + sigmoid 还原原始坐标值

模型输出的坐标是量化后的 int8 数据，需先反量化为浮点数，再通过 sigmoid 压缩到 0\~1 范围（模型训练时的输出约束）：

> **第二道严筛**：判断 `final_score >= box_threshold`，只有最终得分真正达标，才正式开始解码边界框坐标；

* `box_x = sigmoid( deqnt_int8t_to_f32( *(box_p + 0 * grid_len), zp, scale ) ) * 2 - 0.5` &#x20;

* `box_y = sigmoid( deqnt_int8t_to_f32( *(box_p + 1 * grid_len), zp, scale ) ) * 2 - 0.5` &#x20;

* `box_w = sigmoid( deqnt_int8t_to_f32( *(box_p + 2 * grid_len), zp, scale ) ) * 2.0` &#x20;

* `box_h = sigmoid( deqnt_int8t_to_f32( *(box_p + 3 * grid_len), zp, scale ) ) * 2.0`

（`*2-0.5` 和 `*2.0` 是 YOLO 模型的坐标解码公式，将 sigmoid 后的 0\~1 值扩展为 -0.5\~1.5，适配网格偏移计算）

#### 4.2 转换为图像绝对中心坐标

将“网格内相对偏移”转换为“图像像素坐标”（基于当前网格的位置和步长）：

* `box_x = (box_x + c) * (float)stride` → `c` 是网格列索引，`stride` 是步长（如 8），结果是目标中心的 x 像素坐标；

* `box_y = (box_y + b) * (float)stride` → `b` 是网格行索引，结果是目标中心的 y 像素坐标。

#### 4.3 还原目标真实宽高

模型输出的宽高是“相对于锚框的缩放比例”，需结合锚框尺寸还原真实宽高：

* `box_w = box_w * box_w * (float)anchor[a*2]` → `anchor[a*2]` 是第 `a` 个锚框的预设宽度，`box_w²` 是模型学习到的缩放因子；

* `box_h = box_h * box_h * (float)anchor[a*2 + 1]` → `anchor[a*2+1]` 是第 `a` 个锚框的预设高度。

#### 4.4 转换为左上角坐标（标准格式）

目标检测常用“左上角坐标 + 宽高”格式，需将中心坐标转换：

* `box_x = box_x - (box_w / 2.0)` → 左上角 x 坐标（xmin）；

* `box_y = box_y - (box_h / 2.0)` → 左上角 y 坐标（ymin）。

### 步骤 6：存储有效结果

核心目标：将当前检测单元的“边界框、类别置信度、类别 ID”存入输出向量，供后续 NMS 处理。

* 边界框：`boxes.emplace_back(box_x)`、`boxes.emplace_back(box_y)`、`boxes.emplace_back(box_w)`、`boxes.emplace_back(box_h)`（每个框占 4 个元素）；

* 类别置信度：`objProbs.emplace_back(...)`（存入步骤 5 还原的浮点数概率）；

* 类别 ID：`classId.emplace_back(maxClassID)`（存入最优类别 ID）；

* 有效框计数：`validCount++`（当前尺度的有效框数量 +1）。

### 步骤 7：返回当前尺度的有效框数量

循环结束后，返回 `validCount`（当前尺度解析出的有效框总数），供上层函数（`post_process`）统计 3 个尺度的总有效框数量。

### <span style="color: rgb(143,149,158); background-color: inherit">关键逻辑补充说明</span>

1. **<span style="color: rgb(143,149,158); background-color: inherit">量化 / 反量化对齐</span>**<span style="color: rgb(143,149,158); background-color: inherit">：模型输出是 int8 量化值，因此需将浮点型置信度阈值转换为 int8 后再比较，避免精度损失；</span>

2. **<span style="color: rgb(143,149,158); background-color: inherit">坐标还原逻辑</span>**<span style="color: rgb(143,149,158); background-color: inherit">：</span>

   * <span style="color: rgb(143,149,158); background-color: inherit">模型输出的是 “网格内相对偏移”，需叠加网格索引（c/b）并乘以步长，转换为图像绝对坐标；</span>

   * <span style="color: rgb(143,149,158); background-color: inherit">宽高需基于锚框缩放（box_w² 是 YOLO 系列的宽高还原公式）；</span>

3. **<span style="color: rgb(143,149,158); background-color: inherit">效率优化</span>**<span style="color: rgb(143,149,158); background-color: inherit">：通过三层循环（锚框→网格行→网格列）遍历所有检测框，仅保留满足置信度阈值的框，减少后续 NMS 计算量；</span>

4. **<span style="color: rgb(143,149,158); background-color: inherit">类别筛选</span>**<span style="color: rgb(143,149,158); background-color: inherit">：对每个框遍历所有类别，取置信度最高的类别作为最终类别 ID。</span>

### 总结：process 函数的核心价值

该函数是**单尺度模型输出的“解码器”**，核心解决了 3 个关键问题：

1. **量化数据解码**：通过 `deqnt_int8t_to_f32` 将 int8 量化数据还原为浮点数；

2. **坐标格式转换**：将模型输出的“网格相对坐标”转为“图像绝对坐标”，符合目标检测的标准格式；

3) **有效结果筛选**：通过置信度阈值过滤低质量框，减少后续 NMS 的计算量。

最终，3 个尺度的 `process` 调用完成后，会生成全尺度的“边界框 + 置信度 + 类别 ID”列表，为后续的“置信度排序”和“NMS”提供原始数据。

```json
flowchart TD
    A["开始"] --> B["初始化：validCount=0，计算grid_len=grid_h×grid_w"]
    B --> C["将box_threshold反Sigmoid转换为logits阈值thres"]
    C --> D["将thres量化为int8类型threa_i8（与模型输出对齐）"]
    D --> E["遍历锚框（a=0→2）"]
    E --> F["遍历网格行（b=0→grid_h-1）"]
    F --> G["遍历网格列（c=0→grid_w-1）"]
    
    %% 置信度筛选
    G --> H["读取当前网格的框置信度box_anchor_conf（int8）"]
    H --> I{"box_anchor_conf ≥ threa_i8?"}
    I -- 否 --> G["继续下一列"]
    I -- 是 --> J["计算当前框偏移量box_ofst，获取框参数指针box_p"]
    
    %% 反量化+Sigmoid计算框坐标
    J --> K["反量化box_p[0/1/2/3]，并Sigmoid处理，得到box_x/y/w/h（网格相对值）"]
    K --> L["box_x = (box_x + c) × stride（转换为图像绝对中心x）"]
    L --> M["box_y = (box_y + b) × stride（转换为图像绝对中心y）"]
    M --> N["box_w = (box_w²) × anchor[a×2]（还原真实宽度）"]
    N --> O["box_h = (box_h²) × anchor[a×2+1]（还原真实高度）"]
    O --> P["box_x = box_x - box_w/2（转左上角x）；box_y = box_y - box_h/2（转左上角y）"]
    
    %% 类别置信度筛选
    P --> Q["初始化maxClassProb=-128，maxClassID=0"]
    Q --> R["遍历所有类别（k=0→OBJ_CLASS_NUM-1）"]
    R --> S["读取当前类别置信度prob（int8）"]
    S --> T{"prob > maxClassProb?"}
    T -- 是 --> U["更新maxClassProb=prob，maxClassID=k"]
    T -- 否 --> R["继续下一类"]
    R -- 遍历结束 --> V["反量化maxClassProb并Sigmoid，得到最终置信度"]
    
    %% 结果存储
    V --> W["将box_x/y/w/h存入boxes向量"]
    W --> X["将类别置信度存入objProbs向量"]
    X --> Y["将maxClassID存入classId向量"]
    Y --> Z["validCount += 1"]
    Z --> G["继续下一列"]
    
    %% 循环结束
    G -- 列遍历结束 --> F["继续下一行"]
    F -- 行遍历结束 --> E["继续下一锚框"]
    E -- 锚框遍历结束 --> AA["返回validCount（有效框数量）"]
    AA --> AB["结束"]
```

