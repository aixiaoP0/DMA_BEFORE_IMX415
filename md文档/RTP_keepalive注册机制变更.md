# RTP keepalive 注册机制变更记录

更新时间：2026-07-20

## 1. 变更目标

原RTP链路要求板端通过环境变量预先指定客户端地址：

```text
DMA_STREAM_RTP_HOST=<客户端IP>
DMA_STREAM_RTP_PORT=10002
```

在VMware NAT网络中，虚拟机可以主动访问开发板，但开发板不能直接访问虚拟机的NAT私网地址。因此固定目标模式无法完成RTP联调。

本次改为客户端主动注册：

```text
sclient的RTP接收socket
  -> 向开发板UDP 10002发送keepalive
  -> VMware建立同一socket的NAT映射
  -> 板端recvfrom获取NAT后的来源IP和端口
  -> 板端向该来源地址发送RTP/H.264
  -> NAT把RTP数据交回原客户端socket
```

keepalive只负责发现客户端地址。视频数据仍然使用RTP头、H.264单NALU和FU-A分片，没有改成自定义UDP视频格式。

## 2. 注册协议

注册包复用项目已有的12字节 `MessageHeader`：

```text
head_id        = "CCTC"
message_type   = 0（kKeepAlive）
sub_type       = 0
payload_length = 0
```

客户端启动时立即发送一次，之后默认每500毫秒发送一次。板端每次收到合法keepalive都会：

1. 通过 `recvfrom()` 获取来源 `sockaddr_in`。
2. 保存来源IP和来源端口。
3. 更新最后活跃时间。
4. 将该地址作为RTP动态目标。

板端默认5秒未收到keepalive即令动态注册失效，并停止向该动态地址发送。超时时间可配置。

## 3. 板端改动

修改文件：

- `h264_distributor.c`
- `h264_distributor.h`

主要变化：

- RTP端口通过 `bind(0.0.0.0:10002)` 同时承担注册接收和RTP发送。
- 新增独立 `rtp_registration_thread`，非阻塞接收注册包，不进入MPP编码回调。
- 新增动态客户端地址、最后活跃时间和有效标志。
- 每帧RTP封包前只获取一次目标地址，单NALU/FU-A分片复用该地址。
- 动态注册地址优先于固定地址。
- `DMA_STREAM_RTP_HOST` 保留为可选固定目标备用模式。
- 关闭分发器时关闭RTP socket并回收注册线程。

运行时配置：

```bash
DMA_STREAM_RTP_PORT=10002
DMA_STREAM_RTP_CLIENT_TIMEOUT_MS=5000
DMA_STREAM_RTP_HOST=<可选固定目标IP>
```

- `DMA_STREAM_RTP_PORT=0`：关闭RTP。
- 不设置 `DMA_STREAM_RTP_HOST`：仅使用客户端注册模式，推荐用于VMware NAT联调。
- 设置 `DMA_STREAM_RTP_HOST`：没有有效动态客户端时使用该固定地址。

板端预期日志：

```text
[h264-distributor] RTP waiting for client registration on 0.0.0.0:10002
[h264-distributor] RTP client registered from <NAT后的IP>:<NAT后的端口>
```

客户端停止5秒后：

```text
[h264-distributor] RTP client registration expired
```

## 4. 客户端改动

修改文件：

- `src/modules/network/types/ClientConfig.h`
- `src/app/cli/CliOptions.cpp`
- `src/modules/network/StreamClient.cpp`
- `scripts/dma_rtp.sh`
- `tests/unit/CliOptionsTest.cpp`
- `tests/integration/RtpReceiveIntegrationTest.cpp`

新增配置：

```text
--rtp-server-host <开发板IP>
--rtp-server-port <注册端口，默认10002>
```

RTP模式下参数含义：

```text
--host             客户端本地bind地址
--port             客户端本地RTP接收端口
--rtp-server-host  板端注册地址
--rtp-server-port  板端注册端口
```

客户端执行顺序：

1. 创建一个UDP socket。
2. 将该socket绑定到本地RTP接收地址。
3. 将同一socket连接到板端注册地址。
4. 使用该socket发送初始keepalive。
5. 启动keepalive线程定期续约。
6. 使用同一socket接收板端返回的RTP包。

“同一socket”是NAT联调成功的关键，保证注册包来源端口与RTP接收映射一致。

未设置 `--rtp-server-host` 时，客户端仍保持原来的被动RTP监听模式，已有SDP、单元测试和本地回环场景不受影响。

## 5. 编译

板端：

```bash
cd ~/Dev/DMA/build
cmake ..
make -j4
```

虚拟机客户端：

```bash
cd ~/liumeiti/项目/sclient
cmake -S . -B build
cmake --build build -j4
```

## 6. 联调步骤

开发板IP示例：

```text
192.168.137.99
```

先在板端启动：

```bash
cd ~/Dev/DMA/build
unset DMA_STREAM_RTP_HOST
export DMA_STREAM_RTP_PORT=10002
export DMA_STREAM_RTP_CLIENT_TIMEOUT_MS=5000
./cv
```

确认板端端口：

```bash
ss -lunp | grep ':10002'
```

然后在虚拟机运行：

```bash
cd ~/liumeiti/项目/sclient
./scripts/dma_rtp.sh 192.168.137.99
```

脚本完整参数：

```text
./scripts/dma_rtp.sh <开发板IP> [本地bind地址]
```

例如：

```bash
./scripts/dma_rtp.sh 192.168.137.99
./scripts/dma_rtp.sh 192.168.137.99 0.0.0.0
```

## 7. 成功判定

板端：

- 打印RTP注册监听日志。
- 打印RTP客户端注册地址。
- 没有RTP端口绑定失败。
- 客户端运行期间不会打印注册超时。

客户端：

- HUD显示 `Connected`。
- HUD显示 `Transport: rtp`。
- 分辨率为1280×720。
- 画面持续刷新。
- 没有持续的RTP解析、FU-A重组或FFmpeg解码错误。

## 8. 验证记录

- 板端 `cv` 编译成功。
- 客户端CLI新增参数测试通过。
- 原有RTP单NALU、FU-A、丢包重同步和时间戳扩展测试通过。
- 新增RTP注册回环测试通过。
- 新测试确认keepalive来源端口与客户端RTP接收端口一致。

当前共享板端环境缺少客户端完整GUI构建所需的 `glfw3` 和OpenGL开发包，因此完整 `sclient` GUI需要在已经成功构建过客户端的虚拟机上重新编译验证。

## 9. 当前边界

- 当前只保存一个动态RTP客户端，后注册者会替换先注册者。
- keepalive是项目控制协议，不是RTCP。
- 当前未实现RTCP Sender Report、Receiver Report或RTP NACK。
- 注册包暂未加入鉴权，适合可信局域网联调。
- RTP时间戳仍按30 FPS固定步进3000；采集帧率调整后需要同步修改。

