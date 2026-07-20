# 项目协作指南

## 项目范围

本目录是低延时视频流媒体系统的主要代码区，包含服务端 `sserver/` 和客户端 `sclient/` 两个 CMake 子项目。上层目录中的 `低延时视频流媒体系统/` 保存系统总体介绍、架构图和设计说明；`音视频知识点/` 保存背景知识。修改代码前，优先参考 `../低延时视频流媒体系统/低延时视频流媒体系统.md`、各子项目 `README.md`、`docs/README_INDEX.md` 和 `docs/src_architecture_overview.md`。

## 目录结构

- `sserver/`：服务端，负责采集、编码和发送。核心代码在 `src/app/`、`src/core/`、`src/config/`、`src/common/`、`src/modules/`。
- `sserver/config/`：TCP、UDP、RTP、benchmark、integration 等运行配置。
- `sserver/scripts/`：构建、测试和 TCP/UDP/RTP 联调脚本。
- `sclient/`：客户端，负责接收、解码和 OpenGL/ImGui 渲染。
- `sclient/src/modules/network/`：TCP、UDP、RTP 接收链路、NACK/FEC、jitter buffer。
- `sclient/src/modules/decoding/`：FFmpeg H.264 解码。
- `sclient/src/modules/rendering/`：OpenGL 渲染与 HUD。
- `tests/`：两端均按 `unit/`、`integration/`、`smoke/`、`benchmark/` 分层组织；服务端额外包含 `latency/`。

## 构建与运行

服务端：

```sh
cd sserver
cmake -S . -B build
cmake --build build -j
./build/stream_server --config config/default.conf
```

客户端：

```sh
cd sclient
cmake -S . -B build
cmake --build build -j
./build/sclient --help
```

常用联调脚本：

```sh
cd sserver && ./scripts/tcp.sh
cd sclient && ./scripts/tcp.sh
cd sserver && ./scripts/udp.sh
cd sclient && ./scripts/udp.sh
cd sserver && ./scripts/rtp.sh v4l2
cd sclient && ./scripts/rtp.sh v4l2
```

## 测试要求

修改后至少运行对应子项目的 CTest：

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build --output-on-failure -L unit
ctest --test-dir build --output-on-failure -L integration
ctest --test-dir build --output-on-failure -L smoke
```

涉及端到端链路、协议、缓冲队列或延迟统计的修改，应补充或更新相应测试。真实摄像头相关用例依赖 V4L2 设备；没有硬件时优先使用 null capture、loopback、smoke 或单元测试。

## 代码风格

项目使用 C++14，`CMAKE_CXX_EXTENSIONS OFF`。保持现有分层：`common` 不依赖业务模块，`app` 负责装配，`modules` 实现业务能力。新增模块优先沿用“门面 + Backend + 工厂”模式，避免跨层直接耦合。类名使用 PascalCase，函数和局部变量沿用现有 lowerCamelCase 风格，头文件与实现文件尽量放在同一模块目录。

## 提交注意事项

提交说明应简短聚焦，例如 `fix RTP FU-A reassembly` 或 `add UDP jitter benchmark`。PR 需说明修改模块、构建结果、`ctest` 结果、联调协议模式，以及是否影响配置项、CLI 参数、网络协议或 HUD 指标。

## 安全与生成文件

不要提交私有 IP、推流地址、设备凭据、抓包文件、大型录制样本、core dump 或 `build/` 生成产物。需要共享测试资源时，应放入明确命名的测试资源目录，并说明来源和用途。
