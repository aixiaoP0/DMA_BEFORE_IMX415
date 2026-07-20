# 面试回答：RTMP 推流初始化、H.264/H.265 区别、协议选择

> 以下回答**已对照实际代码修正**，去除了 AI 生成的模糊表述和不一致之处。

---

## 你是如何初始化 RTMP 推流器的？

**回答：**

我的 `init_rtmp_streamer` 实现在 `rtmp.c:202-255`，核心流程如下：

**第一阶段：网络与 FLV 输出上下文初始化**

```c
avformat_network_init();                                          // 初始化 FFmpeg 网络模块
avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", stream);  // 分配 FLV 封装上下文
out_stream = avformat_new_stream(ofmt_ctx, NULL);                // 创建视频流通道
```

这里直接指定 `"flv"` 作为封装格式，因为 RTMP 协议底层用 FLV 承载视频流。`ofmt_ctx` 和 `out_stream` 是全局 static 变量（`rtmp.c:11-12`），后续 `write_frame` 直接使用。

**第二阶段：临时编码器上下文与参数映射**

```c
AVCodecContext *o_codec_ctx = avcodec_alloc_context3(NULL);
o_codec_ctx->codec_id      = config->codec_id;      // AV_CODEC_ID_H264
o_codec_ctx->codec_type    = AVMEDIA_TYPE_VIDEO;
o_codec_ctx->codec_tag     = 0;                     // FLV 兼容性：强制设 0
o_codec_ctx->flags        |= AV_CODEC_FLAG_GLOBAL_HEADER;
o_codec_ctx->time_base     = av_make_q(1, config->fps);
o_codec_ctx->framerate     = av_make_q(config->fps, 1);
o_codec_ctx->pix_fmt       = config->pix_fmt;       // AV_PIX_FMT_NV12
o_codec_ctx->width         = config->width;          // 1280
o_codec_ctx->height        = config->height;         // 720
o_codec_ctx->gop_size      = config->fps * 2;        // 2 秒一个 I 帧
o_codec_ctx->max_b_frames  = config->max_b_frames;   // 0，禁用 B 帧
o_codec_ctx->profile       = config->profile;        // FF_PROFILE_H264_HIGH
o_codec_ctx->level         = config->level;          // 31（720p@30fps）
```

这里使用**临时**编码器上下文的原因是：我们不自己编码（编码由 MPP 硬件完成），但 FFmpeg 的 `avformat_write_header` 需要 `AVCodecParameters` 来写 FLV 头。所以我们用这个临时 `AVCodecContext` 作为参数容器，最后通过 `avcodec_parameters_from_context` 将参数拷贝到 stream 的 `codecpar` 中，即可释放这个临时上下文。

关键细节：
- **`codec_tag = 0`**：FLV 封装规定 codec_tag 必须为 0，否则 FFmpeg 会报错
- **`AV_CODEC_FLAG_GLOBAL_HEADER`**：告诉 FFmpeg 码流级头信息（SPS/PPS）存在 extradata 中，而非每帧前面
- **`max_b_frames = 0`**：禁用 B 帧，PTS = DTS，降低延迟，避免 B 帧重排序带来的额外延迟

**第三阶段：SPS/PPS → AVCC 格式转换**

```c
uint8_t* avcc_data = NULL;
int avcc_len = 0;
sps_pps_to_avcc(config->extradata, config->extradata_size, &avcc_data, &avcc_len);
o_codec_ctx->extradata      = avcc_data;
o_codec_ctx->extradata_size = avcc_len;
```

MPP 编码器输出的是 **Annex B** 格式的 SPS/PPS（起始码 `00 00 00 01` 分隔），但 FLV 封装要求 **AVCC 格式**（4 字节长度前缀）。转换函数 `sps_pps_to_avcc`（`rtmp.c:28-79`）做的是：

```
Annex B:  [00 00 00 01] SPS数据 [00 00 00 01] PPS数据
                          ↓
AVCC:     0x01 + profile + level等5字节 + 0xFF + 0xE1 + 2字节SPS长度 + SPS数据 + 0x01 + 2字节PPS长度 + PPS数据
```

如果不做这个转换，播放器收到 FLV 流后无法解析 SPS/PPS，必然黑屏。

**第四阶段：参数交接、建联与写头**

```c
avcodec_parameters_from_context(out_stream->codecpar, o_codec_ctx);
out_stream->codecpar->codec_tag = 0;
avio_open(&ofmt_ctx->pb, stream, AVIO_FLAG_WRITE);        // TCP 连接 RTMP 服务器
avformat_write_header(ofmt_ctx, NULL);                     // FLV 握手 + 写头
avcodec_free_context(&o_codec_ctx);                         // 释放临时上下文
```

`avio_open` 建立真正的 TCP 连接（RTMP 基于 TCP），`avformat_write_header` 完成 RTMP 握手并写入 FLV 的 `flv header` + `metadata tag`。之后就可以通过 `av_interleaved_write_frame` 持续推流了。

---

## H.264 / H.265 的区别是什么？

**回答：**

一句话概括：**H.265（HEVC）用更高的计算复杂度换取约 50% 的码率节省。**

我从三个维度具体说明：

### 1. 压缩效率

在同等主观画质下，H.265 的码率比 H.264 节省约 **40%~50%**。对于 1080p 流，H.264 可能需要 4 Mbps 才能清晰，H.265 约 2 Mbps 即可达到相近画质。

### 2. 核心算法差异——块结构

这是两者最根本的原理差异：

| 特性 | H.264 | H.265 |
|------|-------|-------|
| 基本单元 | 固定 16×16 宏块 (Macroblock) | 编码树单元 CTU，最大 64×64，递归四叉树分割 |
| 分割灵活性 | 16×16 可细分到 4×4 | 64×64 递归分割到 8×8，支持非对称划分 (AMP) |
| 帧内预测方向 | 9 种 (luma 4×4) / 4 种 (16×16) | 35 种 |
| 运动补偿精度 | 1/4 像素 (luma) | 1/4 像素 (luma)，1/8 像素 (chroma) |
| 变换 | 4×4 / 8×8 DCT-like | 4×4 ~ 32×8 DCT，4×4 DST |

简单理解：如果是大面积的蓝天或墙面，H.265 用一个 64×64 的大块就能编码，而 H.264 需要切成 16 个 16×16 宏块，块分割信息本身就多。这是 H.265 节省码率的主要来源。

### 3. 工程代价与项目适配

**在本项目（RK3588 嵌入式平台）的语境下：**

- **MPP 硬件编码器同时支持 H.264 和 H.265**：RK3588 的 VEPU（Video Encoder Processing Unit）硬编支持两种格式
- **本项目选择了 H.264**，原因有二：
  1. **720p 分辨率下 H.264 已足够**：IMX415 输出 1280×720，码率 2 Mbps 的 H.264 画质已满足视频分析需求，H.265 的码率优势在 1080p 以上才明显
  2. **兼容性**：RTMP/FLV 对 H.265 的支持仍然是"非标准"扩展（FLV 标准只定义了 H.264 = codec_id 7），部分 CDN 和播放器可能不支持 FLV over RTMP 传输 H.265
- 如果未来升级到 4K，会考虑切换到 H.265（RK3588 的 MPP 硬编支持 4K H.265）

### 面试加分项：MPP 中的配置

在本项目中，切换 H.264/H.265 只需改 `streamer.c:55` 一行：

```c
// H.264
g_streamer_ctx.mpp_ctx->type = MPP_VIDEO_CodingAVC;

// H.265
g_streamer_ctx.mpp_ctx->type = MPP_VIDEO_CodingHEVC;
```

MPP 内部通过 `mpp_init(ctx, MPP_CTX_ENC, type)` 根据 type 选择不同的硬件编码通路。这体现了硬件抽象层的设计——上层业务代码不需要关心编码细节。

---

## 你选择的推流协议是什么？为什么？对延迟和 H.264 打包方式有什么影响？

**回答：**

我选择了 **RTMP（Real-Time Messaging Protocol，基于 TCP）**。

### 为什么选 RTMP？

**1. 生态成熟度——直播行业的"工业标准"**

RTMP 由 Macromedia（现 Adobe）在 2005 年提出，虽然技术陈旧，但生态极其完善：
- 服务器端：Nginx-RTMP、SRS、MediaServer 等开源方案成熟稳定
- 云厂商：阿里云、腾讯云、AWS 的 CDN 直播服务都原生支持 RTMP 推流
- 播放端：通过 HTTP-FLV 或 HLS 转封装，几乎所有播放器都能拉流播放

对于嵌入式项目来说，"能跑通"比"用最新技术"更重要。RTMP 是兼容性风险最小的选择。

**2. FFmpeg 封装支持完善**

FFmpeg 的 FLV muxer 对 RTMP 的支持非常成熟，所有打包逻辑（Annex B → AVCC 转换、时间戳 rescale、FLV tag 封装）都在 FFmpeg 内部完成，我们只需要填充 AVPacket 然后调用 `av_interleaved_write_frame`。开发成本低，稳定性高。

**3. 为什么不选其他协议？**

| 协议 | 不选的原因 |
|------|-----------|
| **RTSP** | 基于 RTP/UDP，适合低延迟点对点，但不适合经过 CDN 分发。FFmpeg 的 RTSP muxer 不如 RTMP 健壮 |
| **WebRTC** | 延迟最低（~200ms），但信令服务器复杂，且 SDP 协商在嵌入式平台上不可控因素多。适合视频会议，不适合单向直播推流 |
| **SRT/HLS** | HLS 延迟太高（~6-10 秒）；SRT 较新，生态不如 RTMP 成熟 |

### 延迟分析：TCP/RTMP 传输延迟 vs 端到端延迟

需要先区分两个概念：**协议传输延迟**和**端到端总延迟**。

**TCP/RTMP 协议传输延迟**非常小：
- TCP 在局域网环境下的传输延迟 < 1 毫秒，公网通常 10~50 毫秒
- FFmpeg 推流端（MPP 编码完成 → av_interleaved_write_frame 发出）延迟可以做到 **200ms 以下**
- 本项目中禁用了 B 帧、用 CBR 模式，MPP 硬件编码的管线延迟约 30~60 毫秒

**端到端总延迟**（1~3 秒）主要来自**播放器缓冲策略**：
- 播放器为了对抗网络抖动、保证流畅播放，会缓冲 500ms ~ 几秒的数据
- 这不是 RTMP/TCP 协议的限制，而是播放器行为
- 用 FFplay 开 `-fflags nobuffer` 可将端到端延迟压到 500ms 以内

TCP 重传的影响也需要理性看待：**只有发生丢包时才触发重传**，正常稳定网络下不引入额外延迟。"TCP 重传导致延迟高"是极端场景下的表现，不能作为 RTMP 的固有缺点。

对于本项目——**实时视频分析 + 监控展示**——端到端 1~3 秒的体验延迟完全可以接受，不是毫秒级交互场景。

### RTMP 对 H.264 打包方式的影响

这是最容易被忽略但最重要的工程细节：

**RTMP 协议要求 FLV 封装**

FLV 规范对 H.264 码流的要求：
1. **头信息**：SPS/PPS 必须作为 AVCC 格式的 extradata，在 FLV 头的 `metadata` 之后、`Video Tag` 之前发送（在 RTMP 中对应 `onMetaData` 之后的 `AVCDecoderConfigurationRecord`）
2. **帧数据**：每个 NALU 前必须是 **4 字节大端长度前缀**（length prefix），而非 H.264 原生的 **起始码**（start code `00 00 00 01`）

这就是为什么我的 `write_frame`（`rtmp.c:85-195`）要做这个转换：

```
MPP 输出 (Annex B):    [00 00 00 01] NALU数据...
                           ↓
写入 AVPacket (AVCC):   [4字节大端长度][NALU数据][4字节大端长度][NALU数据]...
```

不转换的后果：播放器（VLC/FFplay）解析 FLV 时，找不到 NALU 边界，画面完全花屏或黑屏。

---

## 补充：在 MPP 编码循环中，"一帧多包"是如何处理的？

> 这个问题在原文档中提及但未展开，这里做补充说明。

MPP 硬件编码时，如果码率较高或启用了 slice split，硬件可能将一帧图像分多个分区（partition）编码。在 `process_image`（`mpp.c:369-470`）中，处理方式是：

```c
do {
    ret = p->mpi->encode_get_packet(p->ctx, &packet);
    if (packet) {
        // 回调发给 RTMP
        p->write_frame(ptr, len);

        // 关键：判断是否为分区编码
        if (mpp_packet_is_partition(packet)) {
            eoi = mpp_packet_is_eoi(packet);  // EOI=1 表示该帧最后一个包
        }
        mpp_packet_deinit(&packet);
        p->frame_count += eoi;
    }
} while (!eoi);
```

- **非分区模式**（本项目）：`mpp_packet_is_partition` 返回 false，`eoi` 保持为初始值 1，循环只执行一次
- **分区模式**：硬件返回多个包，直到 `mpp_packet_is_eoi(packet)` 返回 true（EOI = End Of Image）才结束循环

---

**代码对照索引：**

| 函数 | 文件 | 行号 |
|------|------|------|
| `init_rtmp_streamer` | `rtmp.c` | 202-255 |
| `sps_pps_to_avcc` | `rtmp.c` | 28-79 |
| `write_frame` | `rtmp.c` | 85-195 |
| `init_streamer` | `streamer.c` | 30-102 |
| `process_image` (MPP 编码+推流) | `mpp.c` | 369-470 |
| `init_mpp` (MPP 初始化配置) | `mpp.c` | 109-326 |

---

---

# 面试回答版本（口语化，适合面试现场讲述）

以下采用与上方技术文档相同的结论，但转换为**面试现场口语风格**，参考 `回答集.md` 的叙述方式——先讲背景/挑战，再讲方案，展示思考过程，适合面试时直接使用。

---

## 面试题 1：你是如何初始化 RTMP 推流器的？

面试官你好。我的 `init_rtmp_streamer` 实现在 `rtmp.c`，入口参数就两个：RTMP 服务器地址和一个配置结构体。配置结构体里包含分辨率、帧率、像素格式、H.264 的 profile/level，以及最重要的——MPP 编码器产出的 SPS/PPS 头信息。

整个初始化的核心挑战其实就一个：**MPP 硬件编码器不懂 FLV 封装，FFmpeg 的 FLV muxer 又不懂硬件编码的裸流格式，我需要做一层适配。**

我的做法分四步走：

第一步，搭输出框架。`avformat_network_init()`初始化FFmpeg网络模块，进行调用 `avformat_alloc_output_context2` 创建一个 FLV 封装上下文，然后 `avformat_new_stream` 开一条视频流通道。这里直接指定 `"flv"` 格式，因为 RTMP 协议底层就是用 FLV 来承载音视频的。

第二步，建参数映射。我申请了一个临时的 `AVCodecContext`，把分辨率、帧率、像素格式、profile/level 这些参数一股脑全填进去。这里有个坑：必须把 `codec_tag` 设为 0，否则 FFmpeg 校验 FLV 兼容性时会报 tag 校验失败。另外我把 `max_b_frames` 设成 0，禁了 B 帧，这样 PTS 永远等于 DTS，省掉了帧重排序的延迟。还有 `AV_CODEC_FLAG_GLOBAL_HEADER` 这个 flag，告诉 FFmpeg SPS/PPS 在 extradata 里而不是嵌在每帧前面。

第三步，也是最容易踩坑的一步——SPS/PPS 格式转换。MPP 吐出来的 SPS/PPS 是 H.264 原生的 Annex B 格式，也就是起始码 `00 00 00 01` 分隔的。但 FLV 封装不认这个，它要的是 AVCC 格式——4 字节长度前缀。所以我写了个 `sps_pps_to_avcc` 函数，从 MPP 的头信息里把 SPS 和 PPS 提取出来，按照 AVCC 规范重新打包：先是 0x01 标记版本号，然后是 profile/level/compat 这些配置字节，接着是 SPS 长度前缀加 SPS 数据，再是 PPS 长度前缀加 PPS 数据。打好的 AVCC 数据赋值给临时编码器上下文的 extradata。

第四步，建联和握手。用 `avcodec_parameters_from_context` 把临时上下文的参数正式拷到 stream 的 codecpar 里，然后 `avio_open` 去连 RTMP 服务器的 TCP 端口，最后 `avformat_write_header` 做 FLV 握手、写入 FLV 头和 metadata。大功告成后，释放临时上下文，后面就可以一直调 `av_interleaved_write_frame` 发帧了。

这里面有个设计决策我说一下：**为什么用临时编码器上下文？** 因为整个项目里 MPP 硬件负责真正编码，我们根本不需要 FFmpeg 的编码器。但 FFmpeg 的 `avformat_write_header` 写 FLV 头时需要 `AVCodecParameters`，这个参数结构体没有直接构造的 API，只能从 `AVCodecContext` 转过去。所以临时上下文纯粹就是个"参数容器"，用完即焚。

---

## 面试题 2：能否详细描述一下你调用 MPP 进行 H.264 编码的主要流程？

面试官你好。MPP 编码这部分的实现在 `mpp.c`，我把整个流程概括为"初始化→配置→拿头→编码循环→清理"五个阶段，但我不打算背流水账，我想重点说几个我在开发中真正遇到问题的地方。

先简单串一下流程：

**初始化和配置**在 `init_mpp` 函数里。先算步长和帧大小，用 `MPP_ALIGN` 对齐到 16 的倍数——这个对齐是硬件要求，不对齐硬件不干活。然后 `mpp_buffer_group_get_internal` 创建 DRM 缓冲区组，`mpp_buffer_get` 分别为输入帧和输出包分配硬件缓冲区。接着 `mpp_create` 创建编码器实例，`mpp_init` 初始化为编码模式。配置层用的是 MPP 的 cfg 字符串接口，比如 `"prep:width"`、`"rc:mode"` 这种 key-value 方式设进去，最后 `MPP_ENC_SET_CFG` 一次性激活。

具体参数上，我设了 CBR 固定码率模式，码率范围是目标码率的 ±1/16，GOP 长度是帧率的 2 倍也就是 2 秒一个 I 帧。H.264 专属参数我开了 High profile、Level 3.1、CABAC 熵编码、8x8 变换。

**拿头信息**是 `get_head` 函数，通过 `MPP_ENC_GET_HDR_SYNC` 这个 control 命令让硬件生成 SPS/PPS。这里有个细节——MPP 产出的头信息是 Annex B 格式，我需要把原始数据 malloc 一份存起来，后面 `init_rtmp_streamer` 会用这个做 AVCC 转换。

**编码循环**在 `process_image` 里。每次进来先把 NV12 帧数据 memcpy 到硬件输入缓冲区——这是整个编码环节**唯一的一次 CPU 拷贝**。然后用 `mpp_frame_init` 创建 MppFrame，设好宽高格式后关联缓冲区，通过 `encode_put_frame` 提交给硬件。输出端用 `encode_get_packet` 在 `do { } while (!eoi)` 循环里取包。

这里我想重点说一下**"一帧多包"**的问题。一开始我以为 MPP 编码就是"塞一帧进、取一包出"，后来调试发现某些帧会吐出多个包。原因是有个 `mpp_packet_is_partition` 的分区标志：高码率或启用了 slice split 时，硬件会把一帧拆成多个分区编码。所以我的输出循环必须用 `mpp_packet_is_eoi` 判断是不是当前帧的最后一个包——EOI 是 End Of Image 的缩写。如果是分区模式，eoi=0 说明还有后续分区，继续取；eoi=1 说明这一帧收齐了。如果不是分区模式，eoi 初始值就是 1，循环只执行一次。这个 do-while 比 while 好的地方在于确保至少尝试一次取包，避免空循环。

取出来的包通过函数指针回调解耦地发给 `write_frame`，`write_frame` 从 MPP 裸指针拿到数据后再做 Annex B 到 AVCC 的转换，然后推给 RTMP。

**清理**就是先 `reset` 再 `destroy` 最后 `buffer_put`，顺序不能错——因为硬件 DMA 可能还在跑，先 reset 让硬件停稳，再释放资源。

---

## 面试题 3：MPP 编码输出的 H.264 裸流，你是如何处理并喂给 FFmpeg 进行推流的？

面试官你好。这个问题其实和刚才的 RTMP 初始化是同一件事的两个面——一个偏编码侧，一个偏封装侧。

MPP 编码完输出的是 H.264 裸流，格式是 **Annex B**，也就是用 `00 00 00 01` 或 `00 00 01` 起始码来分隔每个 NALU。但 RTMP 协议的 FLV 封装不认这个，它要的是 **AVCC 格式**——每个 NALU 前面是 4 字节大端的长度前缀，没有起始码。

所以我的 `write_frame` 函数做的就是这件事：**把 Annex B 转成 AVCC**，包装进 FFmpeg 的 AVPacket 里发出去。

具体怎么做呢？一帧数据进来，我先从头扫描一遍，找到所有的起始码位置。每找到一个起始码，就记下它后面的 NALU 起始偏移和 NALU 的长度——长度由相邻两个起始码的差值算出，最后一个 NALU 的长度由帧总长度减去它的偏移得到。为了简单，我假设一帧最多 16 个 NALU，用静态数组存。

扫描完之后，我 `av_malloc` 一块新内存，新内存大小就是所有 NALU 的"4 字节长度头 + NALU 数据"之和。然后逐个 NALU 处理：先写入 4 字节大端长度前缀，再 memcpy NALU 数据。同时在拷贝中检查 NALU 类型——如果发现是 IDR 帧（type == 5），就给 AVPacket 打上 `AV_PKT_FLAG_KEY` 标记。

时间戳处理也容易踩坑。H.264 裸流本身没有时间概念，时间戳是我们手动给的。我的做法是用帧号作为 PTS 和 DTS，然后用 `av_packet_rescale_ts` 把时间基从 `{1, 30}` 转换到 stream 的 time_base。这里要注意因为禁用了 B 帧，PTS 始终等于 DTS，所以不需要复杂的帧重排序。

最后调 `av_interleaved_write_frame` 发出去，然后 `av_free` 释放刚才申请的内存。**这个 av_free 特别重要**——如果不释放，推流跑起来几十秒就会因为内存爆炸挂掉，因为每秒 30 帧每帧都要 alloc 一次。

---

## 面试题 4：H.264 和 H.265 的区别是什么？

面试官你好。做视频工程的人应该都知道一句话：**H.265 是用更高的算力换更低的带宽。** 在同等画质下，H.265 的码率能比 H.264 省 40% 到 50%。

但我不想只谈这个数字，我想谈三层的理解。

**第一层是算力——"没有免费的午餐"。** H.265 为什么能省码率？核心在于它的块结构更灵活。H.264 用固定的 16×16 宏块，而 H.265 引入了 CTU——编码树单元，最大能到 64×64，而且是递归四叉树分割。画面里如果有大片蓝天或白墙，H.265 用一个 64×64 的大块就表示完了，H.264 却要切成 16 个 16×16 的碎块，块分割信息本身的编码开销就多出来不少。但灵活性的代价就是计算复杂度翻了几倍——模式决策树更大了。

**第二层是工程选择——回到我们项目的语境。** 我用的 RK3588 芯片，它的 MPP 硬件编码器（VEPU 模块）其实同时支持 H.264 和 H.265。切换代码只需要一行——把 `type` 从 `MPP_VIDEO_CodingAVC` 改成 `MPP_VIDEO_CodingHEVC`。但我最终选了 H.264。为什么？原因有两个：

一是 **720p 分辨率下 H.264 已经足够**。IMX415 摄像头输出就是 1280×720，码率配 2 Mbps 的 H.264，画质已经满足视频分析的需求。H.265 的码率优势要到 1080p 甚至 4K 才能充分发挥，在 720p 这个分辨率上收益不明显。

二是 **RTMP/FLV 对 H.265 的支持是非标准扩展**。FLV 规范只定义了 H.264 的 codec_id 是 7，H.265 没有官方的 codec_id 分配。虽然现在很多 CDN 扩展支持了，但仍有部分播放器和中间件不兼容。对于嵌入式项目来说，"兼容性"比"省那几百 K 码率"重要得多，我不想在对接上出问题。

**第三层是未来的扩展性。** 如果项目之后升级到 4K 分辨率，我会考虑切换到 H.265——RK3588 的 MPP 硬编支持 4K H.265，而且 4K 下 H.265 的码率节省就非常可观了。到那时只需要改一行配置代码，上层逻辑完全不用动，这就是硬件抽象层设计带来的好处。

---

## 面试题 5：你选择的推流协议是什么？为什么？对延迟和 H.264 打包方式有什么影响？

面试官你好。我选的是 **RTMP**——基于 TCP 的传统推流协议。

**关于为什么选它，我其实做过对比分析。**

首先是 RTSP。RTSP 走 RTP/UDP，延迟能做到 200 毫秒左右，但它的问题是：必须经过 CDN 分发时很麻烦，很多 CDN 对 RTSP 的支持不好甚至不支持。而且 FFmpeg 的 RTSP muxer 远不如 RTMP 健壮，我在 RK3588 这种嵌入式平台上折腾不起稀奇古怪的兼容性问题。另外 RTSP 控制流和数据流分离，信令逻辑比 RTMP 复杂。

然后是 WebRTC。延迟确实低，100 到 200 毫秒级别，但 WebRTC 需要部署信令服务器做 SDP 协商，而且 NAT 穿透在嵌入式网络环境下不可控因素太多。它是为"双向视频通话"设计的，我们这个是"单向直播推流"，用 WebRTC 有点像用装甲车送快递——太重了。

再说 HLS。基于 HTTP 的切片方案，最大的问题是延迟——最少也要 6 到 10 秒，因为要等一个 ts 切片够时长才能发。而且 HLS 本身是拉流协议，不是推流协议，实时性根本满足不了视频分析的要求。

所以对比下来，RTMP 虽然技术陈旧——2005 年出的，但生态是最好的。Nginx-RTMP 和 SRS 等开源服务器都很成熟，阿里云、腾讯云、AWS 的 CDN 都原生支持 RTMP 推流，播放端通过 HTTP-FLV 转一下就能播。对嵌入式项目来说，"能稳定跑通"比"技术时髦"重要得多。

**关于延迟**，我要先把一个重要概念说清楚：**"端到端延迟"和"协议传输延迟"是两码事。**

TCP 协议本身的网络传输延迟其实非常小——局域网环境下不到 1 毫秒，公网环境也就 10 到 50 毫秒。所以 **FFmpeg 推流端延迟（从 MPP 编码完成、数据交到 write_frame、到通过 av_interleaved_write_frame 发出）完全可以做到 200 毫秒以下**，甚至更低。这个环节的瓶颈不在 TCP，而在编码方式——我们禁用了 B 帧、用 CBR 固定码率，MPP 硬件编码的管线延迟也就一两帧的时间，约 30~60 毫秒。

那经常说的"RTMP 延迟 1 到 3 秒"到底是什么？其实是**端到端延迟**，也就是从摄像头采集到播放器显示的总时间。这里面最大的贡献者是**播放器的缓冲策略**。为了对抗网络抖动保证播放流畅，播放器通常会缓冲 500 毫秒到几秒的数据。这个缓冲是播放器行为，不是 RTMP 协议本身的限制。实际上如果你用 FFplay 开 `-fflags nobuffer -analyzeduration 0` 或者用 VLC 把缓存调到最低，RTMP 流的端到端延迟也可以压到 500 毫秒以内。

所以对于延迟这件事的正确理解应该是：RTMP（TCP）的推流端延迟很低，200ms 以下是完全可以做到的。影响端到端延迟的瓶颈在播放器缓冲策略和编码的 GOP/帧结构上。我们项目里推流端确实做到了毫秒级发出，播放器端 1~3 秒的体验延迟属于监控场景的正常可接受范围。

另外，关于 TCP 重传对延迟的影响也需要澄清：只有发生**丢包**时重传才会引入额外延迟，正常的稳定网络下 TCP 的吞吐延迟远低于 1 毫秒。所以"TCP 重传导致延迟高"这个说法只有在网络很差的时候才成立，不能作为 RTMP 的固有缺点。

**关于 H.264 打包方式的影响**，这才是 RTMP 最大的工程技术点。RTMP 协议要求用 FLV 封装，而 FLV 规范对 H.264 码流有两个硬性要求：第一，SPS/PPS 必须包装成 AVCC 格式的 extradata，在 FLV 头的 metadata tag 之前就发出去；第二，每一帧的 NALU 数据不能用起始码分隔，必须改用 4 字节大端长度前缀。

这就是为什么我的整个推流链路里最核心、最容易出 bug 的地方就是 **Annex B 到 AVCC 的格式转换**。我的 `write_frame` 函数里有两次完整的内存扫描和拷贝：第一次扫描找 NALU 边界，第二次用 4 字节长度头替换起始码后组装新数据。如果这一步出了问题——比如长度算错了一个字节——远端播放器看到的就不是花屏，而是直接解码失败或者画面全是马赛克。这一点我在调试过程中确实踩过坑，反复核对过二进制数据才搞定。

---

# 附录：213.txt 原始笔记内容（经代码验证修正）

> 以下内容源自 `213.txt` 笔记，已对照 `rtmp.c` 和 `mpp.c` 实际代码修正了错别字和表述问题，补充了缺失的 MPP 编码流程答案。

---

## 你是如何初始化 RTMP 推流器的？（213.txt 修正版）

首先进行网络初始化（`avformat_network_init`），创建输出格式上下文，这里指定 FLV 格式（`avformat_alloc_output_context2`）。然后为输出上下文创建一个视频流（`avformat_new_stream`），后续所有视频参数都绑定到这个流。

然后初始化临时编码器上下文并配置参数，例如分辨率、帧率、像素格式、profile/level 等。这里要注意的是编码器上下文参数中的 extradata：从 MPP 获取的 extradata 是 Annex B 格式（带 `00 00 00 01` 起始码），而 FLV 封装要求的是 AVCC 格式，所以这里要进行格式转换。具体做法是从 MPP 的头信息里把 SPS 和 PPS 提取出来，按照 AVCC 规范重新打包——先是 `0x01` 标记版本号，然后是 profile/level/compat 等配置字节，接着是 SPS 长度前缀加 SPS 数据，再是 PPS 长度前缀加 PPS 数据。打包好的数据赋值给编码器上下文的 extradata。

然后将编码器上下文参数复制到视频流的 codecpar（`avcodec_parameters_from_context`），这里要强制设置 `codec_tag` 为 0，避免参数复制后被自动修改，保证 FLV 封装兼容性。最后调用 `avio_open` 打开 RTMP 地址的写连接，并调用 `avformat_write_header` 写入推流的 FLV 头信息和 metadata。

---

## 能否详细描述一下你调用 MPP 进行 H.264 编码的主要流程？（213.txt 修正版）

调用 MPP 进行编码，因为 MPP 编码为硬件编码，我们需要为编码的输入帧和输出码流分配内存缓冲区，所以先计算编码器所需要的内存尺寸参数 `frame_size`，进行预处理，也就是硬件对齐。

然后我们申请对应大小的缓冲区组、输入帧缓冲区和输出包缓冲区；然后创建 MPP 上下文并进行初始化，指定上下文类型为编码器，编码格式为 H.264。

接着初始化编码设置，主要有三个方面：

1. **基础图像属性**：告诉编码器进来的是多大分辨率、什么格式的图像
2. **码率控制**：设了 CBR 固定码率模式，码率范围是目标码率的 ±1/16，GOP 长度是帧率的 2 倍也就是 2 秒一个 I 帧
3. **H.264 专属参数**：High profile、Level 3.1、CABAC 熵编码、8×8 变换

然后因为我们做的是 H.264 编码，下游进行 RTMP 推流，须先拿到 SPS 和 PPS（头信息）。通过 `MPP_ENC_GET_HDR_SYNC` 这个 control 命令让硬件生成 SPS/PPS。这里 MPP 产出的头信息是 Annex B 格式，我们需要对其进行 AVCC 转换，再发送给下游。

**编码循环部分**，可以分为输入和输出两部分。

首先是**输入**：把外部采集到的原始 YUV 数据拷贝到我们第一步申请好的硬件输入缓冲区中。接着，用一个 MppFrame 结构体把这块内存包装起来，贴上宽、高、格式等标签，并把它推入编码器的输入队列。

然后是**输出**：输出侧可能出现**一帧多包**的情况（高码率或启用了 slice split 时，硬件会把一帧拆成多个分区编码）。所以输出循环必须用 `mpp_packet_is_eoi` 判断是不是当前帧的最后一个包——EOI 是 End Of Image 的缩写。如果是分区模式，`eoi = 0` 说明还有后续分区，继续取；`eoi = 1` 说明这一帧收齐了。如果不是分区模式，`eoi` 初始值就是 1，循环只执行一次。这个 do-while 比 while 好的地方在于确保至少尝试一次取包，避免空循环。

循环结束后我们就拿到了 Annex B 格式的 H.264 裸流数据。

---
