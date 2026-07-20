要搞懂 **int8输出 + scale + zp 还原真实概率** 的过程，核心是理解**量化/反量化的数学逻辑** —— 模型训练时用浮点数计算，但为了适配NPU硬件（提速、降功耗），会把浮点数张量压缩成int8整数存储/计算，推理后需要把int8整数还原回有物理意义的浮点数（如置信度、坐标偏移量）。

### 一、先明确核心概念

在RKNN（瑞芯微NPU）的量化体系中，针对**输出张量**的关键参数：

| 参数              | 含义                                           |
| --------------- | -------------------------------------------- |
| int8输出          | 模型推理后输出的量化整数（范围通常是 `-128 ~ 127` 或 `0 ~ 255`） |
| zp (zero point) | 量化零点：浮点数0对应的int8整数值（核心偏移量）                   |
| scale           | 量化缩放因子：int8整数与浮点数的“单位换算比例”                   |

### 二、反量化核心公式（通用）

RKNN对输出张量的反量化公式是： &#x20;

```plain&#x20;text
浮点数结果 = (int8量化值 - zp) × scale
```

这个公式是所有还原的基础，不同场景（置信度、坐标）只是在此基础上做二次处理。

### 三、结合YOLOv5场景拆解还原过程

YOLOv5的输出张量包含「目标框坐标、置信度、类别概率」，NPU输出的int8值需要先反量化为浮点数，再做后续解析，步骤如下：

#### 步骤1：从代码中获取scale和zp

在你提供的代码里，推理前会先收集每个输出张量的scale和zp：

```c++
vector<int32_t> qnt_zps;   // 存储每个输出层的zp
vector<float> qnt_scales;  // 存储每个输出层的scale
for(int i = 0; i< io_num.n_output; i++) {
    qnt_zps.emplace_back(output_attrs[i].zp);       // 从张量属性中取zp
    qnt_scales.emplace_back(output_attrs[i].scale); // 从张量属性中取scale
}
```

这些参数是模型量化时固化的（比如通过PTQ/QAT量化工具生成），存在.rknn模型文件中，通过`rknn_query`接口可读取。

#### 步骤2：对int8输出逐值反量化

以YOLOv5的某一个输出特征层为例（比如outputs\[0].buf是int8数组）：

```c++
// 伪代码：单值反量化示例
int8_t quant_val = outputs[0].buf[offset]; // 取某一个int8量化值
int32_t zp = qnt_zps[0];                  // 该输出层的零点
float scale = qnt_scales[0];              // 该输出层的缩放因子
float float_val = (quant_val - zp) * scale; // 还原为浮点数
```

#### 步骤3：浮点数转换为“真实概率”（YOLOv5专属）

YOLOv5的输出张量值本身不是直接的概率，需要经过**Sigmoid/Softmax激活** 转换为0\~1的概率：

1. **置信度还原**：

反量化后的浮点数是「未激活的置信度logit值」，需通过Sigmoid函数计算真实置信度： &#x20;

```plain&#x20;text
真实置信度 = 1 / (1 + exp(-float_val))
```

2. **类别概率还原**：

类别维度的logit值需通过Softmax归一化，得到每个类别的概率（总和为1）： &#x20;

```plain&#x20;text
类别i概率 = exp(float_val_i) / Σ(exp(float_val_j))  （j遍历所有类别）
```

3. **坐标还原（补充）**：

目标框坐标的反量化结果还需结合锚框（Anchor）、网格尺寸做解码（如`x = (grid_x + sigmoid(x_logit)) × stride`），最终得到像素坐标，再缩放回原始图像尺寸。

### 四、代码中的落地（post\_process函数隐含逻辑）

你代码中`post_process`函数的入参是`(int8_t*)outputs[0].buf, ..., qnt_zps, qnt_scales`，其内部必然包含以下逻辑（虽未贴出post\_process实现，但可推断）：

```c++
// post_process内部伪代码
for (每个输出层) {
    for (每个网格/每个锚框) {
        // 1. 反量化：int8 → 浮点数
        float x_logit = (int8_output[x_offset] - zp) * scale;
        float conf_logit = (int8_output[conf_offset] - zp) * scale;
        
        // 2. 激活函数：转概率
        float conf = sigmoid(conf_logit); // 置信度概率（0~1）
        float cls_prob = softmax(cls_logits); // 类别概率（0~1）
        
        // 3. 最终置信度 = 目标框置信度 × 类别概率
        float final_prob = conf * cls_prob; 
    }
}
```

### 五、关键注意点

1. **scale/zp的层独立性**：

YOLOv5有3个输出特征层（大/中/小目标），每个层的scale和zp可能不同（代码中用数组存储就是这个原因），必须一一对应反量化。

2. **量化范围匹配**： &#x20;

   * 如果int8是`对称量化`（范围-128\~127），zp通常为0； &#x20;

   * 如果是`非对称量化`（范围0\~255），zp是偏移基准（比如128）；

   公式无需修改，`(quant_val - zp)`会自动适配两种场景。

3. **精度损耗**：

量化过程会损失少量精度，但scale/zp是量化时通过“最小化量化误差”计算出的最优值，能保证还原后结果接近原始浮点数推理。

### 总结

还原流程的核心链路： &#x20;

`NPU输出int8值` → `(int8 - zp) × scale 反量化为浮点数logit` → `Sigmoid/Softmax激活为0~1的概率` → `（可选）结合业务逻辑计算最终概率` &#x20;

scale和zp的本质是“量化时的压缩系数”，反量化就是用这两个系数把int8整数“解压缩”回有物理意义的浮点数，再通过激活函数得到最终的真实概率。
