# 仓库指南

## 项目结构与模块组织

本目录包含低延时视频流媒体相关资料与客户端工程。主要代码位于 `项目/sclient/`，这是一个 C++14 + CMake 的视频接收、解码和渲染客户端。`低延时视频流媒体系统/` 保存设计文档和架构图，压缩包用于归档参考，通常不要直接修改。

`项目/sclient/` 的核心结构：

- `src/app/`：程序入口和命令行参数解析。
- `src/common/`：日志、协议、网络格式、媒体帧、延迟统计等通用组件。
- `src/modules/`：网络接收、FFmpeg 解码、OpenGL/ImGui 渲染等业务模块。
- `tests/`：单元、集成、冒烟和 benchmark 测试。
- `docs/`：架构和模块文档索引。
- `scripts/`：TCP、UDP、RTP 等联调脚本。
- `third_party/`：glad、imgui、glm 等第三方源码。

## 构建、测试与开发命令

在 `项目/sclient/` 下运行：

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/sclient --help
./build/rtp_decode_smoke
```

`cmake -S . -B build` 配置工程并生成 `compile_commands.json`。`cmake --build build -j` 编译客户端和测试程序。`ctest` 运行已注册测试。`sclient --help` 检查命令行入口，`rtp_decode_smoke` 可在无服务端时做快速解码验证。

依赖示例：

```sh
sudo apt install -y build-essential cmake pkg-config libavcodec-dev libavutil-dev libglfw3-dev libgl-dev
```

## 代码风格与命名约定

使用 C++14，保持 `CMAKE_CXX_EXTENSIONS OFF` 的可移植写法。类和结构体使用 PascalCase，例如 `StreamClient`、`LatencyStats`；函数和局部变量使用 lowerCamelCase；头文件与实现文件成对放在同一模块目录。新增模块优先放入 `src/common/` 或 `src/modules/` 的现有边界内，不要绕过公共协议和帧类型重复定义数据结构。

## 测试指南

测试文件按类型放入 `tests/unit/`、`tests/integration/`、`tests/smoke/` 或 `tests/benchmark/`，命名采用 `FeatureTest.cpp` 或 `FeatureBenchmark.cpp`。新增测试需在 `CMakeLists.txt` 中创建目标并通过 `add_test` 注册。网络协议、抖动缓冲、CLI、解码链路等修改应至少覆盖对应单元或集成测试。

## 提交与 Pull Request 规范

当前目录未发现独立 Git 历史。提交说明应简短、聚焦变更，例如 `fix RTP timestamp parsing` 或 `add UDP jitter benchmark`。Pull Request 应说明变更模块、构建结果、`ctest` 输出摘要、联调用到的协议模式，以及是否影响命令行参数、渲染行为或服务端兼容性。

## 配置与安全建议

不要提交私有推流地址、设备 IP、凭据或本地联调端口配置。`build/`、临时日志、抓包文件和大型录制样本应保持在版本控制之外；需要共享样本时，优先放入明确命名的测试资源目录并说明来源。
