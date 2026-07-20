# 第七阶段：MPP 编码与 RTMP 推流

**相关代码：** `streamer.h/c`（入口）、`mpp.h/c`（编码器）、`rtmp.h/c`（推流协议）

---

## 7.1 整体架构

```
Worker (BGR→NV12)
    │
    ▼
[SafeQueue]  (g_SafeQueueWrite)
    │
    ▼
WriteVideo 线程
    │  process_frame()
    ▼
streamer.c (中介层)
    │
    ├──► mpp.c   — Rockchip MPP 硬件编码 H.264
    │       │        输入: NV12 → 输出: H.264 Annex-B (Start Code)
    │       │        编码后回调 write_frame()
    │       ▼
    └──► rtmp.c  — FFmpeg FLV 封装 + RTMP 推流
                   输入: H.264 → 输出: RTMP 流
```

三个模块的职责划分：

| 模块 | 文件 | 职责 |
|------|------|------|
| `streamer` | `streamer.h/c` | 中介层，协调 MPP 和 RTMP 的初始化与调用 |
| `mpp` | `mpp.h/c` | Rockchip MPP 硬件编码器，NV12 → H.264 |
| `rtmp` | `rtmp.h/c` | 基于 FFmpeg 的 FLV 封装 + RTMP 推流 |

---

## 7.2 WriteVideo 线程入口

**相关代码：** `main.cpp:387-452`

```cpp
void Thread_WriteVideo(VideoWriter& writer) {
    while (true) {
        if (g_processFinish && g_SafeQueueWrite.empty()) break;

        if (!g_SafeQueueWrite.empty()) {
            FrameData output_FD;
            g_SafeQueueWrite.dequeue(output_FD);

            if (output_FD.nv12_data != nullptr) {
                process_frame(output_FD.nv12_data, output_FD.data_size);
                free(output_FD.nv12_data);  // 释放 NV12 内存（所有权终点）
                output_FD.nv12_data = nullptr;

                // FPS 统计：每 30 帧打印一次
                fps_frame_counter++;
                if (fps_frame_counter == 30) {
                    // 计算并打印端到端真实 FPS
                    float real_fps = (fps_frame_counter * 1000.0f) / elapsed_ms;
                    cout << "[Performance] End-to-End Real FPS: " << real_fps << endl;
                    fps_frame_counter = 0;
                }
            }
        } else {
            this_thread::sleep_for(chrono::milliseconds(2));
        }
    }
}
```

关键点：

- **NV12 内存的最终释放**：这一帧 NV12 的生命周期终点——ReadVideo `malloc` → Worker `malloc` → WriteVideo `free`
- **端到端 FPS 统计**：从 ReadVideo 采集到 WriteVideo 编码推流完成，统计的是整条管线的真实吞吐，非某一段的局部帧率

---

## 7.3 MPP 硬件编码器

**相关代码：** `mpp.h:85-172`（结构体）、`mpp.c:109-326`（初始化）、`mpp.c:369-470`（编码）

### MPP 是什么

MPP（Media Process Platform）是 Rockchip 的媒体处理硬件平台，提供硬件加速的视频编码/解码/转码能力。本项目中只用了**编码器**功能——把 NV12 原始帧压缩为 H.264 码流。

### 初始化流程

**相关代码：** `mpp.c:109-326`

```
alloc_mpp_context()     → 分配 MppContext 结构体，绑定函数指针
    │
init_mpp()              → 核心初始化
    ├─ MPP_ALIGN 步长   → hor_stride/ver_stride 对齐到 16 的倍数
    ├─ mpp_buffer_group_get_internal → 创建 DRM 缓冲区组
    ├─ mpp_buffer_get(frm_buf)       → 分配输入帧缓冲区（存放待编码 NV12）
    ├─ mpp_buffer_get(pkt_buf)       → 分配输出包缓冲区（存放编码后 H.264）
    ├─ mpp_create + mpp_init         → 创建并初始化 MPP 编码上下文
    ├─ mpp_enc_cfg_init              → 创建编码器配置
    └─ mpp_enc_cfg_set_s32           → 设置各项编码参数
```

初始化参数通过 `mpp_enc_cfg_set_s32` 以字符串 key-value 形式设置：

```
prep:width/height      → 1280x720
prep:hor_stride        → MPP_ALIGN(1280, 16) = 1280
prep:ver_stride        → MPP_ALIGN(720, 16)  = 720
prep:format            → MPP_FMT_YUV420SP (NV12)
rc:mode                → MPP_ENC_RC_MODE_CBR (固定码率)
rc:bps_target          → 码率 (如 2Mbps)
rc:fps_in/out_num      → 帧率 (30)
codec:type             → MPP_VIDEO_CodingAVC (H.264)
h264:profile           → 100 (High Profile)
h264:level             → 31 (720p@30fps)
h264:cabac_en          → 1 (CABAC 熵编码)
rc:gop                 → fps * 2 (60, 即 2 秒一个 I 帧)
```

### 编码流程

**相关代码：** `mpp.c:369-470`

```cpp
static int process_image(uint8_t* pr, int size, struct MppContext* p) {
    // 1. 拷贝 NV12 数据到 MPP 输入缓冲区
    void *buf = mpp_buffer_get_ptr(p->frm_buf);
    memcpy(buf, pr, size);        // CPU 拷贝，但很快（几分钟一次而已）

    // 2. 初始化 MppFrame，设置帧参数
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, p->width);
    mpp_frame_set_height(frame, p->height);
    mpp_frame_set_buffer(frame, p->frm_buf);

    // 3. 提交帧到硬件编码器
    p->mpi->encode_put_frame(p->ctx, frame);

    // 4. 循环获取编码后的包（直到 eoi=1）
    do {
        p->mpi->encode_get_packet(p->ctx, &packet);
        void *ptr = mpp_packet_get_pos(packet);
        size_t len = mpp_packet_get_length(packet);

        // 回调：把编码后的数据发给 RTMP
        if (p->write_frame)
            p->write_frame((uint8_t*)ptr, len);

        mpp_packet_deinit(&packet);
    } while (!eoi);
}
```

编码流程要点：

| 步骤 | 操作 | 说明 |
|------|------|------|
| 1 | `memcpy` 到 MPP 缓冲区 | NV12 数据从堆内存拷贝到 MPP 内部 DRM buffer |
| 2 | `encode_put_frame` | 提交给硬件编码器，非阻塞 |
| 3 | `encode_get_packet` | 循环获取编码后的包，每次返回一个 NALU |
| 4 | `mpp_packet_is_partition` | 判断是否分区编码（一帧可能分多个包输出） |
| 5 | `eoi` 标志 | End-Of-Image，eoi=1 表示一帧编码完毕 |

> **低延迟分区编码：** 一帧可能分成多个包输出（Slice），每个包 MPP 编码完成后立即回调发送，不必等整帧编码完。这就是 `do...while(!eoi)` 循环的意义。

### 编码参数详解

**CBR（固定码率）：**
```
rc:mode = MPP_ENC_RC_MODE_CBR
```
码率恒定适用于直播场景，带宽可预测。代价是画面剧烈变化时质量会下降（码率预算不够）。

**GOP（Group of Pictures，图像组）：**
```
rc:gop = fps * 2 = 60
```
每 60 帧一个 I 帧（关键帧），即每 2 秒一个 I 帧。I 帧可独立解码，P 帧依赖前面的帧。推流场景中 RTMP 通常要求**首个帧必须是 I 帧**。

**Profile 与 Level：**
```
h264:profile = 100    → High Profile（支持 CABAC + 8x8 变换）
h264:level   = 31     → Level 3.1（最大 1280x720@30fps）
```

---

## 7.4 RTMP 推流

**相关代码：** `rtmp.c`

### 初始化：建立与 RTMP 服务器的连接

```cpp
int init_rtmp_streamer(char* stream, RtmpContext* config) {
    avformat_network_init();
    avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", stream);
    //                    输出格式固定为 FLV ↑

    out_stream = avformat_new_stream(ofmt_ctx, NULL);
    // 配置 codec context 参数
    o_codec_ctx->codec_id = AV_CODEC_ID_H264;
    o_codec_ctx->pix_fmt = AV_PIX_FMT_NV12;
    o_codec_ctx->time_base = av_make_q(1, fps);

    // 关键：将 MPP 的 SPS/PPS 转为 AVCC 格式
    sps_pps_to_avcc(config->extradata, config->extradata_size,
                    &avcc_data, &avcc_len);
    o_codec_ctx->extradata = avcc_data;

    // 打开网络连接并写入 FLV 头
    avio_open(&ofmt_ctx->pb, stream, AVIO_FLAG_WRITE);
    avformat_write_header(ofmt_ctx, NULL);
}
```

### SPS/PPS → AVCC 格式转换

**相关代码：** `rtmp.c:28-79`

FFmpeg 要求的 extradata 格式是 **AVCC（AVC Configuration Box）**，但 MPP 输出的是 **Annex-B 格式**（Start Code `0x00000001` 分隔）。`sps_pps_to_avcc` 负责转换：

```
Annex-B 格式（MPP 输出）:
  [0x00 0x00 0x00 0x01] [SPS NALU] [0x00 0x00 0x00 0x01] [PPS NALU]

AVCC 格式（FFmpeg 要求）:
  [version=1] [profile] [compatibility] [level] [6 bits reserved | 2 bits nalu size]
  [sps_count=1] [sps_len_2bytes] [SPS data] [pps_count=1] [pps_len_2bytes] [PPS data]
```

### 帧发送：Annex-B → AVCC 长度前缀

**相关代码：** `rtmp.c:85-194`

MPP 输出的每帧 H.264 数据也是 Annex-B 格式（包含多个 NALU，如 SEI + IDR + Slice）：

```
[0x00 0x00 0x00 0x01] [SEI] [0x00 0x00 0x00 0x01] [IDR] [0x00 0x00 0x00 0x01] [SLICE]
```

`write_frame` 需要将其转为 AVCC 长度前缀格式：

```
[4字节: SEI 长度] [SEI 数据] [4字节: IDR 长度] [IDR 数据] [4字节: SLICE 长度] [SLICE 数据]
```

转换逻辑：

```cpp
// 1. 扫描所有 NALU，记录偏移和长度
int nalu_count = 0;
while (scan_idx < size - 4 && nalu_count < 16) {
    if (data[scan_idx] == 0 && data[scan_idx+1] == 0 && data[scan_idx+2] == 1) {
        // 找到起始码，记录 NALU 起始位置
        nalu_offsets[nalu_count] = scan_idx + sc_len;
        nalu_count++;
    }
    scan_idx++;
}

// 2. 替换起始码为 4 字节长度头（大端序）
for (int i = 0; i < nalu_count; i++) {
    uint32_t len = nalu_sizes[i];
    *p++ = (len >> 24) & 0xFF;   // 大端序 4 字节长度
    *p++ = (len >> 16) & 0xFF;
    *p++ = (len >> 8) & 0xFF;
    *p++ = len & 0xFF;
    memcpy(p, data + nalu_offsets[i], len);  // 复制 NALU 数据（不含起始码）
    p += len;
}
```

### 时间戳处理

```cpp
AVRational in_time_base = {1, 30};   // 30 fps → 每帧 1/30 秒
av_pkt.pts = frame_count;
av_pkt.dts = frame_count;
av_pkt.duration = 1;
av_packet_rescale_ts(&av_pkt, in_time_base, out_stream->time_base);
```

不使用 `pts = frame_count * 1000 / 30` 这种手动计算，而是通过 `av_packet_rescale_ts` 让 FFmpeg 自动从输入时间基转换到 FLV 的时间基（{1, 1000}）。

B 帧数设为 0（`max_b_frames = 0`），所以 PTS = DTS（无需重排）。

---

## 7.5 三模块交互时序

```
WriteVideo 线程
    │
    │ process_frame(nv12_data, size)
    ▼
streamer.c:process_frame()
    │
    │ mpp_ctx->process_image(nv12_data, size, mpp_ctx)
    ▼
mpp.c:process_image()
    │
    ├── memcpy → MPP 内部 frm_buf
    ├── encode_put_frame (硬件编码)
    ├── encode_get_packet (取编码后 H.264)
    │
    │ p->write_frame(ptr, len)   ← 回调
    ▼
rtmp.c:write_frame()
    │
    ├── 扫描 NALU → 转换 Annex-B 为 AVCC 长度前缀
    ├── av_packet_rescale_ts → 时间戳转换
    └── av_interleaved_write_frame → 发送到 RTMP 服务器
    ▼
RTMP Server (192.168.15.214:1935/live/cv)
```

---

## 7.6 CPU 占用率对比

| 编码方式 | CPU 占用率 | 说明 |
|---------|-----------|------|
| MPP 硬件编码 | ~5% | 专用硬件模块，几乎不占 CPU |
| x264 软件编码 | 50-80% | 纯 CPU 编码，720p@30fps 可占满 2-3 个大核 |

MPP 硬件编码的代价是平台绑定——换到非 Rockchip 平台（如树莓派）需要改用其硬件编码器或软件编码。

---

## 7.7 MPP 输出验证

```cpp
if (save_count < 5) {
    snprintf(filename, sizeof(filename), "encode_frame %d.h264", save_count);
    FILE *fp = fopen(filename, "wb");
    fwrite(ptr, 1, len, fp);
    fclose(fp);
}
```

前 5 帧编码后的 H.264 数据被保存到 `encode_frame 0.h264` 等文件，可用于离线分析编码质量。

---

## 阶段总结

Stage 7 是管线的出口——从 NV12 原始帧到网络上的 H.264 RTMP 流。核心设计模式是 **MPP 编码 + 回调推流**：

```
NV12 → [MPP 硬件] → H.264 Annex-B → [write_frame 回调] → AVCC → FLV → RTMP
        ~5% CPU            ↓                              ↓
                      encode_frame*.h264            FFmpeg avformat
```
