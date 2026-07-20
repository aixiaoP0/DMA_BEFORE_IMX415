## 阶段三详解：V4L2 + DMABUF 视频采集

这段代码覆盖 `main()` 中第 460-591 行的初始化流程和 `Thread_ReadVideo()` 中第 178-295 行的采集循环。核心逻辑分为 **初始化** 和 **运行时循环** 两部分。

---

### 3.1 初始化流程（6 步）

```
打开设备 → 查能力 → 设格式 → 开DMABUF堆 → 申请缓冲 → 入队+启动
```

#### 第一步：打开设备 `main.cpp:475-480`

```cpp
int fd = open("/dev/video11", O_RDWR | O_NONBLOCK);
```

- `/dev/video11` 是 RK3588 ISP 主通道（mainpath），接收 ISP 处理后的 YUV 数据
- `O_NONBLOCK` 配合后面的 `poll()` 实现超时机制，避免阻塞式 `read()` 卡死

#### 第二步：VIDIOC_S_FMT 格式协商 `main.cpp:494-509`

这是最关键的一步，**告诉 ISP 驱动你要什么格式的数据**：

```cpp
fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;  // 多平面
fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;  // YUV420SP
```

**为什么必须用 MPLANE？** Rockchip ISP 驱动内部使用 `vb2_queue` 框架，MPLANE 是硬件要求的接口。单平面 API（`VIDEO_CAPTURE`）在 USB UVC 设备上普遍支持，但 ISP 需要 `pix_mp` 来传递每个平面的 stride、sizeimage 等信息。

**V4L2 格式协商的"双向性"**：应用程序 `S_FMT` 设置格式，但驱动可能调整（比如对齐 stride）。所以 `S_FMT` 后要读取回 `fmt.fmt.pix_mp` 中的实际值，用 `plane_fmt[0].sizeimage` 而不是自己计算 `W*H*3/2`。

`✶ Insight ─────────────────────────────────────`
`sizeimage` 不一定是 `W*H*3/2`。ISP 会对每行做 stride 对齐（比如 16 字节对齐），实际每行可能比 width 多几个 padding byte。`plane_fmt[0].sizeimage` 是驱动告诉你的"真实大小"，用这个值分配 DMA 内存才能保证硬件不会越界写入。
`─────────────────────────────────────────────────`

#### 第三步：DMABUF 堆初始化 `main.cpp:511-516`

```cpp
dmabuf_ret = my_dma_heap_init(&state.dmabuf_heap);
```

`dmabuf.c:33-38` 中定义了一组候选堆节点：

```c
"/dev/dma_heap/cma"         // 首选：连续物理内存
"/dev/dma_heap/linux,cma"   // 兼容：某些内核的别名
"/dev/dma_heap/reserved"    // 备选：预留区
"/dev/dma_heap/system"      // 兜底：非连续
```

**CMA（Contiguous Memory Allocator）** 是 Linux 内核的连续物理内存分配器。嵌入式系统中，摄像头、ISP、RGA、NPU 这些硬件模块需要通过**物理连续**内存传输数据，普通 `malloc` 返回的是虚拟连续但物理可能不连续的内存，硬件无法直接访问。

`my_dma_heap_init()` 做的事情很简单——遍历这些节点，以 `O_RDWR` 打开第一个可用的，保存文件描述符。

#### 第四步：VIDIOC_REQBUFS 申请缓冲区 `main.cpp:518-530`

```cpp
req.count = 4;
req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
req.memory = V4L2_MEMORY_DMABUF;
ioctl(fd, VIDIOC_REQBUFS, &req);
```

向 V4L2 驱动声明：**我要用 DMABUF 模式，给我预留 4 个缓冲区位置**。注意——此时 V4L2 只是"登记"了要使用 DMABUF 模式，实际的内存需要你自己分配。

三种内存模型的对比：

| 模式                  | 谁分配 | 谁映射 | 适用场景                              |
| --------------------- | ------ | ------ | ------------------------------------- |
| `V4L2_MEMORY_MMAP`    | 驱动   | 驱动   | 简单场景，但无法跨硬件共享            |
| `V4L2_MEMORY_USERPTR` | 应用   | 应用   | 灵活但物理不连续，硬件无法访问        |
| `V4L2_MEMORY_DMABUF`  | 应用   | 应用   | **物理连续，可跨硬件传递 fd，零拷贝** |

**为什么选 DMABUF？** 因为数据最终要传给 RGA 和 NPU。如果用 MMAP，数据在 V4L2 内核缓冲区，RGA 读不到；如果用 USERPTR，用户态内存物理不连续，RGA 硬件 DMA 会出错。DMABUF `fd` 是"硬件通用的号码牌"，V4L2、RGA、MPP 都能识别。

#### 第五步：分配 + 映射 DMABUF `main.cpp:532-561`

对每个 buffer 执行三操作：

```cpp
// 5a. 分配连续物理内存
my_dmabuf_buffer_alloc(&heap, &buffer, sizeimage, "v4l2_buffer_0");
//    内部：ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc)
//    → 内核分配连续的物理内存块，返回一个 fd（"号码牌"）

// 5b. 映射到 CPU 空间（可选，当需要 CPU 读数据时才需要）
my_dmabuf_buffer_map(&buffer);
//    内部：mmap(0, size, PROT_RW, MAP_SHARED, fd, 0)
//    → 返回 mapped_addr，CPU 可以通过这个指针读写

// 5c. 保存信息
state.buffer_infos[i].dmabuf_fd = buffer.fd;       // 给硬件用
state.buffer_infos[i].mapped_addr = buffer.mapped_addr;  // 给 CPU 用
state.buffer_infos[i].size = buffer.size;
```

`✶ Insight ─────────────────────────────────────`
注意 `dmabuf_buffer_t` 中有两个关键成员：`fd` 和 `mapped_addr`。`fd` 是给硬件用的——把 fd 传给 RGA（`importbuffer_fd`），RGA 硬件可以直接从这块物理内存读数据。`mapped_addr` 是给 CPU 用的——通过这个指针，CPU 可以 memcpy 数据。两者指向同一块物理内存，只是访问方式不同。
整个零拷贝的哲学就是：**能走硬件的绝对不走 CPU**。
`─────────────────────────────────────────────────`

#### 第六步：QBUF 入队 + STREAMON 启动 `main.cpp:563-591`

```cpp
// 把所有 4 个 buffer 排入 V4L2 的待采集队列
for (int i = 0; i < 4; i++) {
    buf.m.planes[0].m.fd = state.buffer_infos[i].dmabuf_fd;  // DMABUF fd
    buf.m.planes[0].length = state.buffer_infos[i].size;
    ioctl(fd, VIDIOC_QBUF, &buf);  // "驱动，这个空 buffer 给你用"
}

// 通知驱动开始采集
v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
ioctl(fd, VIDIOC_STREAMON, &type);  // "开始！"
```

此时 ISP 开始收到 IMX415 的数据并持续写入这些 DMABUF 缓冲区。

---

### 3.2 运行时采集循环

采集循环在 `Thread_ReadVideo()` 中 `main.cpp:178-295`。每一帧的完整路径：

```
poll(5s) → DQBUF → sync_start → 读数据 → sync_stop → QBUF（回收）
```

#### 3.2.1 poll 等待 `main.cpp:183-196`

```cpp
poll_fds[0].fd = fd;
poll_fds[0].events = POLLIN;
int poll_ret = poll(poll_fds, 1, 5000);
```

`poll()` 挂起线程等待内核通知"有一帧准备好了"。**为什么不用阻塞式 DQBUF？** 因为阻塞 DQBUF 无法设置超时，如果没有帧（比如摄像头掉线），线程会永远卡死。poll + 5 秒超时给了容错能力。

#### 3.2.2 DQBUF 出队 `main.cpp:197-213`

```cpp
struct v4l2_buffer buf;
struct v4l2_plane planes[1];
buf.m.planes = planes;          // ★ MPLANE 关键绑定
buf.length = 1;
ioctl(fd, VIDIOC_DQBUF, &buf);  // "驱动，给我一帧"
```

DQBUF 返回时，`buf.index` 告诉你哪个 buffer 被填满了。ISP 已经把一帧 NV12 数据写入了这个 buffer 对应的物理内存。

**MPLANE 的易错点：** `length` 这里填的是**平面数量**（NV12 是单平面，所以是 1），**不是数据长度**！这在面试中经常被问。

#### 3.2.3 缓存同步 `main.cpp:220-221, 268-272`

```cpp
// CPU 读取前：刷 inval CPU 缓存，确保看到硬件写入的最新数据
my_dmabuf_sync_start(fd);   // DMA_BUF_SYNC_START

// → 读数据（RGA 转换 + memcpy）...

// CPU 读取后：刷 clean CPU 缓存，确保硬件看到我们改过的数据
my_dmabuf_sync_stop(fd);    // DMA_BUF_SYNC_END
```

**为什么需要 sync？** CPU 有 L1/L2 缓存。CPU 读内存时，如果缓存中有旧数据，就读不到硬件刚写入的最新数据。CPU 写内存时，数据可能还在缓存里没刷回物理内存，硬件也看不到。

`sync_start` 操作：invaildate（使无效）CPU 缓存中该区域的副本，强制从物理内存重新读取。
`sync_stop` 操作：clean（刷回）CPU 缓存到物理内存，确保硬件看到 CPU 的修改。

**面试追问：** 如果只从 DMABUF 读数据（比如 RGA 从 fd 读），不改写它，还需要 sync_stop 吗？→ 不需要。sync_start 保证读到最新数据就够了。sync_stop 是用于"CPU 改了数据后刷给硬件看"。

#### 3.2.4 QBUF 回收 `main.cpp:276-290`

```cpp
buf.m.planes[0].m.fd = state.buffer_infos[buf.index].dmabuf_fd;
buf.m.planes[0].length = state.buffer_infos[buf.index].size;
ioctl(fd, VIDIOC_QBUF, &buf);  // "用完了，还给你"
```

关键时机点：**QBUF 之后，DMABUF 内的数据可能被 ISP 覆盖**。这就是为什么在 QBUF **之前**必须完成数据读取或拷贝。

---

### 3.3 为什么选 4 个 buffer？

```
时间轴：
Buffer 0: [ISP写入] → [RGA读取] → [QBUF回收]
Buffer 1:              [ISP写入] → [RGA读取] → [QBUF回收]
Buffer 2:                           [ISP写入] → [RGA读取] → [QBUF回收]
Buffer 3:                                        [ISP写入] → [RGA读取] → [QBUF回收]
Buffer 0:                                                       [ISP再次写入] → ...
```

3 个 buffer 可以保证流水线不中断（一个正在被 ISP 写入、一个正在被 RGA 读取、一个在队列中等候）。4 个留一个缓冲余量。**如果 buffer 太少，ISP 可能因为没有空闲 buffer 而丢帧**。

---

### 面试追问速查表

| 问题                          | 要点                                                        |
| ----------------------------- | ----------------------------------------------------------- |
| 为什么用 DMABUF 而不是 MMAP？ | 跨硬件共享 fd，零拷贝，物理连续                             |
| MPLANE 和单平面的区别？       | MPLANE 用 `pix_mp` + `planes[]`，ISP 驱动强制要求           |
| `buf.length` 填什么？         | 平面数量（1），不是数据长度                                 |
| `sizeimage` 怎么来的？        | `S_FMT` 后读取 `plane_fmt[0].sizeimage`，已包含 stride 对齐 |
| sync_start/stop 做了什么？    | 刷 CPU 缓存一致性，硬件→CPU 或 CPU→硬件                     |
| 为什么是 4 个 buffer？        | 硬件流水线深度 + 1 个缓冲                                   |
| poll 超时了怎么办？           | 继续循环等待，可能设备异常但不想崩溃                        |

接下来想继续讲哪个阶段？阶段四（RGA 格式转换）还是阶段六（线程池调度）？