# core

## 1. 模块定位

应用生命周期与模块编排层。定义模块接口、模块状态、应用上下文，实现模块的初始化/启动/停止/销毁顺序控制。

## 2. 核心职责

- 定义 `IModule` 接口，所有业务模块必须实现此接口
- 定义 `ModuleState` 枚举，描述模块生命周期状态
- 定义 `ApplicationContext` 结构体，作为模块间共享上下文的载体
- 实现 `Application` 类，按注册顺序初始化/启动模块，按逆序停止/销毁模块
- 支持初始化/启动失败时的自动回滚

## 3. 主要文件说明

| 文件 | 作用 |
|---|---|
| IModule.h | 定义 `IModule` 纯虚接口，所有模块必须实现 `name()`、`initialize()`、`start()`、`stop()`、`shutdown()`、`state()` |
| ModuleState.h | 定义 `ModuleState` 枚举：`kCreated`、`kInitialized`、`kRunning`、`kStopped`、`kShutdown`、`kFailed` |
| ApplicationContext.h | 定义 `ApplicationContext` 结构体，当前仅包含 `AppConfig`，作为模块初始化时的共享上下文 |
| Application.h | 定义 `Application` 类，管理模块注册和生命周期 |
| Application.cpp | 实现模块的按序初始化、按序启动、逆序停止、逆序销毁，以及失败时的回滚逻辑 |

## 4. 核心类 / 函数说明

### IModule

作用：
- 所有业务模块的统一接口
- 定义模块生命周期六个阶段：`initialize` → `start` → `stop` → `shutdown`

关键函数：
- `name()`：返回模块名称，用于日志输出
- `initialize(const ApplicationContext &context)`：初始化模块，从上下文获取配置
- `start()`：启动模块（如启动线程、打开设备、开始监听）
- `stop()`：停止模块运行（如停止线程、关闭连接）
- `shutdown()`：释放模块资源
- `state()`：返回当前模块状态

### ModuleState

```
kCreated → kInitialized → kRunning → kStopped → kShutdown
                ↓               ↓
             kFailed          kFailed
```

### Application

作用：
- 持有模块列表和应用上下文
- 按注册顺序初始化和启动模块
- 按逆序停止和销毁模块

关键函数：
- `RegisterModule()`：注册模块到列表末尾
- `Initialize()`：按注册顺序依次调用 `module->initialize(context_)`，失败时回滚已初始化的模块
- `Start()`：按注册顺序依次调用 `module->start()`，失败时回滚已启动的模块
- `Stop()`：按逆序调用 `module->stop()`
- `Shutdown()`：先 `Stop()`，再按逆序调用 `module->shutdown()`

## 5. 数据流说明

输入：
- `AppConfig`（通过构造函数传入，存入 `ApplicationContext`）
- `IModule` 实例（通过 `RegisterModule()` 注册）

处理：
- 将 `AppConfig` 包装为 `ApplicationContext`
- 按注册顺序依次初始化和启动模块
- 失败时逆序回滚

输出：
- 各模块按序进入运行状态
- `ApplicationContext` 传递给每个模块的 `initialize()` 方法

## 6. 与其他模块的关系

- 被 `app/AppBootstrap` 创建和使用
- `IModule` 接口被 `CaptureModule` 和 `TransportModule` 实现
- `ApplicationContext` 持有 `config/AppConfig`
- 使用 `common/log/Logger` 输出模块初始化/启动失败日志

## 7. 线程模型 / 阑队列模型

本模块当前不直接管理线程。`Application` 的所有操作都是同步调用，线程管理由各 `IModule` 实现自行负责。

注意：模块的注册顺序决定了初始化和启动顺序。当前注册顺序为：`TransportModule` → `CaptureModule`（先启动传输，再启动采集）。

## 8. 配置参数

本模块不直接读取配置参数。`ApplicationContext` 将完整的 `AppConfig` 传递给各模块，由模块自行提取所需配置。

## 9. 调试建议

- 如果模块初始化失败，日志会输出 "failed to initialize module: <name>"，可定位到具体模块
- 如果模块启动失败，日志会输出 "failed to start module: <name>"
- 模块的回滚逻辑是自动的：初始化失败会 `shutdown()` 已初始化的模块，启动失败会 `stop()` 已启动的模块
- 可以在 `Application::InitializeAllModules()` 和 `StartAllModules()` 中打断点观察模块初始化/启动顺序

## 10. 后续扩展方向

- 支持模块依赖声明和拓扑排序（当前依赖注册顺序）
- 支持模块健康检查和自动重启
- 支持动态加载/卸载模块
- 将 `ApplicationContext` 扩展为服务定位器，支持模块间通过上下文查找服务
