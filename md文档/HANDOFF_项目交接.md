# DMA 项目交接文档

更新时间：2026-07-20  
当前目标：跑通新架构 IMX415 摄像头实时视频分析管线，并融合 `视频流媒体/项目/sclient` 的 TCP/UDP/RTP 客户端链路。

## 1. 当前仓库默认规则

`AGENTS.md` 已调整为：

> 默认以新架构（当前运行版本）为基准回答问题，除非用户明确说明“基于旧架构”或“面试基准版本”。

后续讨论、排错、文档更新默认都按根目录 /home/radxa/Dev/DMA/` 的新架构代码处理。

## 2. 当前主项目架构

主项目路径：`E:\DMA`

当前新架构数据流：

```text
IMX415 / V4L2 / ISP
  -> DMABUF NV12
  -> ReadVideo
      Path A: RGA NV12 -> BGR，用于画框
      Path C: RGA NV12 -> RGB 640x640，直写 Worker 独享 NPU input fd
  -> g_SafeQueueRead
  -> Aggregator
      submit_to_worker(worker_id)
      try_get_result()
      result_buffer 按帧序重排
  -> Worker[i]
      rknn_run
      后处理 + 画框 + OSD 时间戳
      RGA BGR -> NV12
  -> g_SafeQueueWrite
  -> WriteVideo
      MPP H.264 编码
      RTMP / TCP / UDP / RTP 分发
```

关键新架构特征：

- `ReadVideo` 中通过 Path C 把 V4L2 DMABUF 直接 RGA 缩放转换到 NPU 输入物理内存。
- `ThreadPool` 使用 per-worker 独立队列。
- `is_busy` 用于保护每个 Worker 独享的 NPU 输入缓冲区，避免 RGA 在 NPU 读取期间覆盖输入。
- `Aggregator` 使用 `try_get_result()` 非阻塞收集结果，并用 `result_buffer` 做按序写出。
- 当前 `skip_inference=true` 的帧在 Aggregator 中创建空结果推进序号，最终 `WriteVideo` 会跳过该帧，不是“保留画面但不画框”。

## 3. 已完成的融合改动

### 3.1 新增 H.264 分发器

新增文件：

- `h264_distributor.h`
- `h264_distributor.c`

作用：

- 在 MPP 编码完成后，复用同一份 Annex-B H.264 数据。
- 支持三种客户端链路：
  - TCP：默认监听 `9999`
  - 自定义 UDP：默认监听 `10000`
  - RTP/UDP：默认向指定客户端发送，端口默认 `10002`
- 不重复编码。
- 不进入 V4L2、RGA、NPU、MPP 编码前的关键路径。
- RTMP 初始化失败时，不阻断 TCP/UDP/RTP 分发。

运行时环境变量：

```bash
DMA_STREAM_TCP_PORT=9999      # 默认 9999，设为 0 可关闭 TCP
DMA_STREAM_UDP_PORT=10000     # 默认 10000，设为 0 可关闭自定义 UDP
DMA_STREAM_RTP_HOST=<客户端IP> # 为空则 RTP 不发送
DMA_STREAM_RTP_PORT=10002     # 默认 10002
```

### 3.2 接入点

修改文件：

- `CMakeLists.txt`
  - `add_executable(cv ...)` 中加入 `h264_distributor.c`

- `streamer.c`
  - 引入 `h264_distributor.h`
  - 将 `mpp_ctx->write_frame` 从 `write_frame` 改为 `streamer_write_frame`
  - `streamer_write_frame()` 中先调用 `h264_distributor_write(data, size)`，再按需调用 RTMP 的 `write_frame(data, size)`
  - `init_streamer()` 中启动分发器，并把 `MPP_ENC_GET_HDR_SYNC` 得到的 SPS/PPS 交给分发器
  - RTMP 初始化失败只设置 `rtmp_enabled=0`，不再直接 `return -1`
  - `close_streamer()` 中关闭分发器

### 3.3 客户端联调脚本

网上项目路径：`E:\DMA\视频流媒体\项目\sclient`

新增脚本：

- `scripts/dma_tcp.sh`
- `scripts/dma_udp.sh`
- `scripts/dma_rtp.sh`

用途：

```bash
./scripts/dma_tcp.sh <板端IP>
./scripts/dma_udp.sh <板端IP>
./scripts/dma_rtp.sh 0.0.0.0
```

## 4. 已修复的问题

### 4.1 无摄像头早退时 Segmentation fault

现象：

未连接摄像头时，板端运行：

```bash
./cv
```

原日志：

```text
open video device: No such file or directory
析构线程池
...
Segmentation fault
```

原因：

`Yolov5s::~Yolov5s()` 中释放顺序错误：

```cpp
rknn_destroy(ctx);
rknn_destroy_mem(ctx, input_mem);
```

`input_mem` 和 `output_mems` 都由该 RKNN ctx 创建，必须在 `rknn_destroy(ctx)` 前释放。

已修改：

- 先 `rknn_destroy_mem(ctx, input_mem)`
- 再释放 `output_mems`
- 最后 `rknn_destroy(ctx)`

修改文件：

- `yolov5s.cpp`

修复后无摄像头测试结果：

```text
open video device: No such file or directory
析构线程池
Worker ... 退出
ThreadPool destroyed.
```

没有再出现 `Segmentation fault`。

## 5. 当前板端测试状态

已验证：

- `make -j4` 成功。
- 程序能启动。
- 4 个 RKNN/YOLOv5s 实例初始化成功。
- NPU 输入/输出零拷贝内存绑定成功。
- 4 个 Worker 能启动并正常退出。
- 未连接摄像头时，`open /dev/video11` 失败是预期行为。
- 无摄像头早退路径已不再段错误。

尚未验证：

- IMX415 接入后 `/dev/video11` 是否存在。
- `VIDIOC_QUERYCAP` / `VIDIOC_S_FMT` / `VIDIOC_STREAMON` 是否成功。
- V4L2 DMABUF 采帧是否成功。
- RGA Path A：NV12 -> BGR 是否成功。
- RGA Path C：NV12 -> RGB 640x640 -> NPU fd 是否成功。
- Worker 推理和 BGR -> NV12 是否稳定。
- MPP 编码是否持续输出 H.264。
- RTMP 推流是否正常。
- TCP/UDP/RTP 客户端是否能收到并解码。

## 6. 板端运行步骤

板端路径示例：

```bash
cd ~/Dev/DMA
mkdir -p build
cd build
cmake ..
make -j4
```

启动 3A 服务：

```bash
rkaiq_3A_server -d /dev/video11 &
```

只测 TCP/UDP，可直接运行：

```bash
./cv
```

如果要测 RTP，需要在运行 `./cv` 前指定客户端 IP：

```bash
export DMA_STREAM_RTP_HOST=<虚拟机或客户端IP>
export DMA_STREAM_RTP_PORT=10002
./cv
```

如需关闭某条链路：

```bash
export DMA_STREAM_TCP_PORT=0
export DMA_STREAM_UDP_PORT=0
```

## 7. 客户端运行步骤

客户端项目路径：

```bash
cd "E:/DMA/视频流媒体/项目/sclient"
cmake -S . -B build
cmake --build build -j
```

TCP：

```bash
./scripts/dma_tcp.sh <板端IP>
```

UDP：

```bash
./scripts/dma_udp.sh <板端IP>
```

RTP：

先启动客户端监听：

```bash
./scripts/dma_rtp.sh 0.0.0.0
```

再在板端运行：

```bash
export DMA_STREAM_RTP_HOST=<客户端IP>
export DMA_STREAM_RTP_PORT=10002
./cv
```

## 8. 需要优先注意的风险点

### 8.1 当前三协议分发是最小可跑通版本

目前 `h264_distributor.c` 是为了先打通链路：

- TCP 只维护单客户端。
- UDP 依赖客户端 keepalive 来记录回包地址。
- UDP 暂未实现服务端侧 NACK 重传缓存和 FEC 奇偶校验包。
- RTP 实现了 H.264 单 NALU 和 FU-A 分片发送，但 RTP 元数据扩展暂未开启。
- RTP timestamp 当前按 30fps 固定步进 `3000`，如果后续 fps 改动，需要同步改成 `90000 / fps`。

面试表达时应说：

> 当前融合版本先实现编码后 H.264 Access Unit 的多协议分发，保证不重复编码、不阻塞采集和推理主链路。后续再完善 UDP NACK/FEC、RTP 扩展头和多客户端管理。

### 8.2 当前分发器仍在 MPP 回调线程内执行 send

虽然使用了 non-blocking send，慢客户端会被断开或丢包，但严格来说还不是最终理想架构。

最终更严谨的架构应是：

```text
MPP H.264 输出
  -> EncodedFrameDispatcher
      -> RTMP bounded queue -> RTMP sender thread
      -> TCP bounded queue  -> TCP sender thread
      -> UDP bounded queue  -> UDP sender thread
      -> RTP bounded queue  -> RTP sender thread
```

这样慢客户端不会影响 MPP 回调。

### 8.3 RTMP 仍有硬编码地址

`main.cpp` 中仍有：

```cpp
std::string rtmpPath = "rtmp://192.168.15.214:1935/live/cv";
```

当前已把 RTMP 初始化失败改成不阻断三协议，但后续建议改成命令行参数或环境变量。

### 8.4 `/dev/video11` 是硬编码

`main.cpp` 中：

```cpp
open("/dev/video11", O_RDWR | O_NONBLOCK);
```

接摄像头后如果设备节点不是 `/dev/video11`，会继续报：

```text
open video device: No such file or directory
```

建议先在板端执行：

```bash
ls -l /dev/video*
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video11 --all
```

## 9. 下一步建议顺序

1. 接 IMX415，确认 `/dev/video11` 存在。
2. 启动 `rkaiq_3A_server -d /dev/video11 &`。
3. 跑 `./cv`，确认进入 V4L2 初始化和 STREAMON。
4. 如果卡在 V4L2，优先看设备节点和格式。
5. 如果进入采帧但报 RGA 错，优先看 stride、format、DMABUF fd。
6. 如果能推理但无客户端画面，先测 TCP，再测 UDP，最后测 RTP。
7. 若 TCP 客户端无画面，抓第一帧 H.264 是否包含 SPS/PPS。
8. 等基本链路跑通后，再重构分发器为“有界队列 + 独立发送线程”的最终架构。

## 10. 面试口径建议

融合后的描述：

> 原系统已经完成 IMX415 采集、RGA 预处理、RKNN/NPU 推理、MPP H.264 编码和 RTMP 推流。我在编码输出后增加了 H.264 分发层，复用同一份 MPP 编码后的 Annex-B H.264 Access Unit，支持自定义 TCP、自定义 UDP 和 RTP/UDP 三种链路。客户端侧复用低延时视频项目中的网络接收、FFmpeg 解码、OpenGL/ImGui 渲染和延迟统计模块。这样不会重复编码，也不会把协议处理放到采集/RGA/NPU 关键路径里。

关于“是不是只是调用 API”：

> FFmpeg 解码、OpenGL 渲染确实使用成熟库 API，但项目价值不在重复造解码器，而在编码帧边界、H.264 Annex-B/RTP FU-A 打包、UDP 分片重组、jitter buffer、NACK/FEC 设计、有界队列背压和端到端延迟统计这些实时流媒体工程问题上。

关于“三协议会不会增加一帧耗时”：

> 单协议模式下只启用对应链路，额外开销很小；多协议同时分发时会增加网络发送和打包开销，但不重复编码。工程上应把各协议出口放到独立有界队列和发送线程，慢客户端只影响自己的队列，不反向阻塞 V4L2、NPU 和 MPP 主链路。

