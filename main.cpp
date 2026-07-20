#include <iostream>
#include <opencv2/core/core.hpp>        // OpenCV核心功能库
#include <opencv2/highgui/highgui.hpp>  // OpenCV高级功能库（视频处理等）
#include <pthread.h>                    // POSIX线程库（设置线程名）
// ISP 直接输出 NV12，不再需要 MPP 解码器
// #include "mpp_decoder.h"
#include <chrono>                       // 时间相关操作
#include <thread>                       // C++线程库
#include <csignal>                      // SIGINT/SIGTERM优雅退出
#include <unistd.h>                     // 系统调用（如close、usleep）
#include <queue>                        // 标准队列
#include <mutex>                        // 互斥锁
#include "SafeQueue.h"                  // 线程安全队列
#include "yolov5s.h"                    // YOLOv5s推理相关
#include <map>                          // 映射容器
#include "post_process.h"               // 推理后处理
#include "thread_pool.h"                // 线程池
#include "streamer.h"                   // 推流相关

// V4L2摄像头相关头文件
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include "sys/mman.h"
#include <sys/poll.h>
#include "dmabuf.h"                     // DMABUF内存管理

// RGA图像格式转换头文件（注意包含顺序）
#include "rga.h"
#include "drmrga.h"
#include "im2d.h"
#include "RgaUtils.h"


using namespace std;
using namespace cv;
//v4l2的头文件
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>
#include "sys/mman.h"
#include <sys/poll.h>
#include "dmabuf.h"

#include "v4l2_common.h"
//rga头文件包含顺序
#include "rga.h"
#include "drmrga.h"
#include "im2d.h"
#include "RgaUtils.h"

// 向上取整宏：将x按a对齐
#define ALIGN(x, a) (((x)+(a)-1)&~((a)-1))

// 全局变量：RGA转换所需的宽/高步长（按16字节对齐）
int g_hor_stride = 0;
int g_ver_stride =0;
/**
 * @brief 基于RGA实现BGR888到NV12格式转换
 * @param bgr 输入BGR格式图像数据指针
 * @param nv12 输出NV12格式图像数据指针
 * @param width 图像宽度
 * @param height 图像高度
 * @note NV12是编码/推流所需格式，BGR是后处理输出格式
 */
void BGR_to_NV12_with_rga(uint8_t* bgr,uint8_t* nv12,int width,int height){
   
    // RGA句柄：用于关联普通内存到RGA可访问空间
    rga_buffer_handle_t bgr_handel , yuv_handel;

    //清空 NV12 缓冲区，为后续格式转换提供一个干净、可控的初始状态
    memset(nv12,0x00,width*height*3/2);

    //导入普通内存到RGA，获取操作句柄（RGA仅能通过句柄访问内存）
    bgr_handel = importbuffer_virtualaddr(bgr,width*height*3);
    yuv_handel = importbuffer_virtualaddr(nv12,g_hor_stride*g_ver_stride*3/2);
    if(bgr_handel == 0 || yuv_handel ==0){
        printf("import va failed\n");
    }

    //定义rga缓冲区
    rga_buffer_t bgr_src = wrapbuffer_handle(bgr_handel,width,height,RK_FORMAT_RGB_888);
    rga_buffer_t yuv_src = wrapbuffer_handle(yuv_handel,g_hor_stride,g_ver_stride,RK_FORMAT_YCrCb_420_SP);
    // 执行转换
    int ret = imcheck(bgr_src, yuv_src, {}, {});
    if(ret != IM_STATUS_NOERROR)
    {
        printf("%d, imcheck error! %s\n", __LINE__,  imStrError((IM_STATUS)ret));
    }
    
    ret = imcvtcolor(bgr_src, yuv_src, RK_FORMAT_RGB_888, RK_FORMAT_YCrCb_420_SP);
    if(ret == IM_STATUS_SUCCESS)
    {
        printf("BGR888 TO NV12 OK!\n");
    }   
    else
    {
        printf("%d, cvtColor error! %s\n", __LINE__,  imStrError((IM_STATUS)ret));
    }
    //释放句柄，避免内存泄漏
    if(bgr_handel){
        releasebuffer_handle(bgr_handel);
    }
    if(yuv_handel){
        releasebuffer_handle(yuv_handel);
    }
}

/**
 * @brief 帧数据结构体：绑定图像数据、帧序号、硬件帧对象、编码数据
 */
struct FrameData
{
    cv::Mat frame;               // OpenCV图像容器（BGR格式）
    int index;                   // 帧索引（保证帧序）
    bool skip_inference = false; // true=当前帧因NPU缓冲区忙被跳过推理
    int dmabuf_fd = -1;          // DMABUF fd（ISP输出的NV12数据）
    // ISP输出的NV12数据拷贝（读线程中从DMABUF映射区拷贝，避免QBUF回收后失效）
    uint8_t* nv12_data = nullptr;
    int data_size = 0;           // NV12数据长度
    int width = 0;               // 图像宽度
    int height = 0;              // 图像高度
};
/**
 * @brief 应用状态结构体
 * @param dmabuf_heap DMABUF 堆管理器结构体
 * @param dmabuf_buffer  DMABUF缓冲区结构体
 * @param v4l2_buffer_info_t V4L2缓冲区信息
 * @param num_buffers DMABUF数量
 * @param initialized
 */
struct app_state_t{
   dmabuf_heap_t dmabuf_heap;         // DMABUF堆管理器
    dmabuf_buffer_t* dmabuf_buffer;  // DMABUF缓冲区
    v4l2_buffer_info_t* buffer_infos;// V4L2缓冲区信息
    int num_buffers;                 // 缓冲区数量
    bool initialized;                // 初始化标志
};

// 模型路径常量
#define model_path "/home/radxa/Dev/DMA/model/yolov5s.rknn"

// 管线并行度配置：3块V4L2采集缓冲区，对应3个独立NPU Worker
static constexpr int kCaptureBufferCount = 3;
static constexpr int kWorkerCount = 3;

// 全局线程池：加载YOLOv5s模型，初始化3个推理线程
ThreadPool gthreadpool(model_path, kWorkerCount);
// 全局帧索引起始值
static int g_frame_start_id = 0;

// 线程安全队列：读线程->处理线程 | 处理线程->写线程
SafeQueue<FrameData> g_SafeQueueRead(100);//安全队列，用于线程间通信
SafeQueue<FrameData> g_SafeQueueWrite(100);
std::atomic<bool> g_readFinish(false);
std::atomic<bool> g_processFinish(false);
static volatile std::sig_atomic_t g_stopRequested = 0;

static void handleStopSignal(int)
{
    g_stopRequested = 1;
}

/**
 * @brief 视频读取线程函数：从V4L2摄像头（IMX415 ISP）采集NV12数据，RGA转BGR后入队
 * @param state 应用状态（DMABUF/V4L2缓冲区）
 * @param fd 摄像头设备文件描述符
 * @param g_SafeQueueRead 解码后帧队列（输出，传递给聚合线程）
 * @param img_index 帧索引（自增）
 * @param cap_mutex 互斥锁（预留）
 * @param finished 退出标志
 */
void Thread_ReadVideo(app_state_t& state,int& fd,SafeQueue<FrameData>& g_SafeQueueRead,int& img_index,mutex& cap_mutex, std::atomic<bool>& finished){
    // 设置线程名（便于调试）
    pthread_setname_np(pthread_self(), "ReadVideo");
    int ret;
    dmabuf_error_t dmabuf_ret;

    // 预分配BGR帧缓存（避免循环内频繁malloc）
    cv::Mat bgr_frame(720, 1280, CV_8UC3);

   while(!g_stopRequested)
   {
    // 定义一个 FrameData 类型的临时变量 frame_temp，用于暂存一帧视频的完整数据
    FrameData frame_temp;           // 临时帧数据
    v4l2_buffer_info_t buffer_info; // V4L2缓冲区信息
    struct pollfd poll_fds[1];      // poll监听结构体

    // 配置poll：监听摄像头fd的可读事件，超时5秒
    poll_fds[0].fd = fd;
    poll_fds[0].events = POLLIN;
    int poll_ret = poll(poll_fds, 1, 5000);
    if (poll_ret < 0) {
        perror("poll failed");
        break;
    } else if (poll_ret == 0) {
        // 显式处理超时：打印超时提醒，可选择继续等待或退出
        printf("poll timeout: no frame received in 5 seconds\n");
        continue;
    }
    // 从V4L2出队缓冲区（获取摄像头数据）—— 使用MPLANE多平面API
    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];   // NV12为单平面格式
    memset(&buf, 0, sizeof(buf));
    memset(&planes, 0, sizeof(planes));

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.m.planes = planes;          // 绑定planes数组
    buf.length = 1;                 // plane数量

    ret = ioctl(fd, VIDIOC_DQBUF, &buf);
     if(ret < 0)
    {
        perror("dequeue buf");
        break;
    }
    // 获取DMABUF缓冲区信息（FD、映射地址、大小）
    dmabuf_buffer_t *dmabuf_buffer = &state.dmabuf_buffer[buf.index];
    buffer_info.index = buf.index;
    buffer_info.dmabuf_fd = my_dmabuf_get_fd(dmabuf_buffer);
    buffer_info.mapped_addr = my_dmabuf_get_mapped_addr(dmabuf_buffer);
    buffer_info.size = my_dmabuf_get_size(dmabuf_buffer);

    // 同步DMABUF到CPU可访问（开始读取）
    dmabuf_ret = my_dmabuf_sync_start(my_dmabuf_get_fd(dmabuf_buffer));
        if (dmabuf_ret != DMABUF_SUCCESS) {
            printf("Failed to sync start buffer %d\n", buf.index);
        }
        else {
    // ★★★ ISP 已直接将数据解码为 NV12，无需 MPP 解码 ★★★
    // 直接从 DMABUF fd 获取 NV12 数据

    int width = 1280;
    int height = 720;
    int nv12_size = width * height * 3 / 2;

    // === [分支1] RGA转换：NV12 (DMABUF fd) → BGR888 (OpenCV Mat，用于画框) ===
    rga_buffer_handle_t src_handle = importbuffer_fd(buffer_info.dmabuf_fd, nv12_size);
    rga_buffer_handle_t dst_handle = importbuffer_virtualaddr(bgr_frame.data, width * height * 3);

    if (src_handle && dst_handle) {
        rga_buffer_t src = wrapbuffer_handle(src_handle, width, height, RK_FORMAT_YCbCr_420_SP);
        rga_buffer_t dst = wrapbuffer_handle(dst_handle, width, height, RK_FORMAT_BGR_888);

        int rga_ret = imcvtcolor(src, dst, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888);
        if (rga_ret == IM_STATUS_SUCCESS) {
            frame_temp.frame = bgr_frame.clone();
            frame_temp.index = img_index++;
            frame_temp.width = width;
            frame_temp.height = height;

            // === [Path C] RGA: NV12 (DMABUF fd) → RGB 640x640 → NPU 物理内存 (零拷贝) ===
            int target_worker = frame_temp.index % kWorkerCount;

            if (gthreadpool.is_worker_busy(target_worker)) {
                // Worker 还在用 NPU 内存做推理 → 丢弃本帧（跳过推理）
                printf("⚠️ Drop Frame %d: Worker %d busy\n",
                       frame_temp.index, target_worker);
                frame_temp.skip_inference = true;
            } else {
                gthreadpool.set_worker_busy(target_worker, true);
                int npu_fd = gthreadpool.get_worker_input_fd(target_worker);
                rga_buffer_handle_t src_handle_c = importbuffer_fd(buffer_info.dmabuf_fd, nv12_size);
                rga_buffer_handle_t dst_handle_c = importbuffer_fd(npu_fd, 640 * 640 * 3);
                if (src_handle_c && dst_handle_c) {
                    rga_buffer_t src_c = wrapbuffer_handle(src_handle_c, width, height, RK_FORMAT_YCbCr_420_SP);
                    rga_buffer_t dst_c = wrapbuffer_handle(dst_handle_c, 640, 640, RK_FORMAT_RGB_888);
                    int rga_ret2 = imresize(src_c, dst_c);
                    if (rga_ret2 != IM_STATUS_SUCCESS) {
                        printf("Path C RGA NV12->NPU Resize Failed: %s\n", imStrError((IM_STATUS)rga_ret2));
                    }
                    releasebuffer_handle(src_handle_c);
                    releasebuffer_handle(dst_handle_c);
                } else {
                    printf("Path C RGA importbuffer failed\n");
                }
            }

            g_SafeQueueRead.enqueue(frame_temp);
        } else {
            printf("Path A RGA NV12->BGR Convert Failed: %s\n", imStrError((IM_STATUS)rga_ret));
        }

        releasebuffer_handle(src_handle);
        releasebuffer_handle(dst_handle);
    } else {
        printf("RGA importbuffer failed\n");
    }

}

    // 同步DMABUF（结束读取）
    dmabuf_ret = my_dmabuf_sync_stop(my_dmabuf_get_fd(dmabuf_buffer));
    if (dmabuf_ret != DMABUF_SUCCESS) {
        printf("Failed to sync stop buffer %d\n", buf.index);
    }

    // 将空缓冲区重新入队V4L2，继续采集（使用MPLANE）
    memset(&buf, 0, sizeof(buf));
    memset(&planes, 0, sizeof(planes));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.m.planes = planes;
        buf.length = 1;
    buf.index = buffer_info.index;
    buf.m.planes[0].m.fd = state.buffer_infos[buf.index].dmabuf_fd;
        buf.m.planes[0].length = state.buffer_infos[buf.index].size;

    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        perror("queue buf");
        finished = true;
        break;
    }
   }

   finished = true;
   // 退出前清理（无MPP解码器需要销毁）
   printf("ReadVideo thread exit\n");
}


// 聚合线程：round-robin提交到指定Worker + 按帧序收集结果
void aggregatorThreadFunc(ThreadPool &gthreadpool)
{
    pthread_setname_np(pthread_self(), "Aggregator");
    int nextWriteIndex = 0;
    // 存储已完成的推理结果（按帧索引排序，保证输出有序）
    map<int, ProcessResult> result_buffer;

    while(true)
    {
        bool is_idle = true;

        // 步骤A：批量从读队列取帧，round-robin提交到指定Worker
        FrameData inputFD;
        while(!g_SafeQueueRead.empty() && gthreadpool.in_flight_count() < 20)
        {
            if(g_SafeQueueRead.dequeue(inputFD))
            {
                if (inputFD.skip_inference) {
                    // 丢帧：创建空白结果推进帧序，保证保序逻辑不卡死
                    ProcessResult blank;
                    blank.success = false;
                    result_buffer[inputFD.index] = std::move(blank);
                } else {
                    gthreadpool.submit_to_worker(
                        inputFD.index % kWorkerCount, inputFD.index, inputFD.frame,
                        inputFD.width, inputFD.height);
                }
            }
            is_idle = false;
        }

        // 步骤B：收集所有已完成的结果到buffer
        WorkerResult wr;
        while(gthreadpool.try_get_result(wr)) {
            result_buffer[wr.index] = std::move(wr.result);
            is_idle = false;
        }

        // 步骤C：按顺序将可写帧写入输出队列
        auto it = result_buffer.find(nextWriteIndex);
        while(it != result_buffer.end())
        {
            FrameData outputFD;
            outputFD.index = nextWriteIndex;
            outputFD.frame = it->second.processed_img;
            outputFD.nv12_data = it->second.nv12_data;
            outputFD.data_size = it->second.data_size;

            g_SafeQueueWrite.enqueue(outputFD);

            result_buffer.erase(it);
            cout << "已处理帧: " << nextWriteIndex
                 << ", 剩余任务: " << gthreadpool.in_flight_count() << endl;
            nextWriteIndex++;
            it = result_buffer.find(nextWriteIndex);
            is_idle = false;
        }

        // 步骤D：判断退出条件
        if(g_readFinish && g_SafeQueueRead.empty() &&
           gthreadpool.in_flight_count() == 0 && result_buffer.empty())
        {
            cout << "处理线程已结束" << endl;
            break;
        }
        if(is_idle) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    g_processFinish = true;
    std::cerr << "[AggregatorThread] finished.\n";
}

/**
 * @brief 视频写入/推流线程函数：编码NV12数据并推流，统计真实FPS
 * @param writer OpenCV视频写入器（预留：本地保存视频）
 */
void Thread_WriteVideo(VideoWriter& writer)
{   
    pthread_setname_np(pthread_self(), "WriteVideo");

    // FPS统计变量：每30帧计算一次真实帧率
    int fps_frame_counter = 0;
    auto fps_start_time = std::chrono::high_resolution_clock::now();

    // 无限循环，直到写入结束
    while(true)
    {
        // 如果写入队列不为空且时间间隔超过30毫秒，则取出一帧进行写入
        if(g_processFinish && g_SafeQueueWrite.empty())
        {
            break;
        }
        
        // 直接尝试出队，避免 empty→dequeue 双重检 + continue 跳过 sleep
        FrameData output_FD;
        if(g_SafeQueueWrite.dequeue(output_FD))
        {
            // 本地录制带检测框和OSD时间戳的BGR画面
            // if(writer.isOpened() && !output_FD.frame.empty())
            // {
            //     writer.write(output_FD.frame);
            // }

            if(output_FD.nv12_data != nullptr)
            {
                // 调用MPP编码+推流接口
                process_frame(output_FD.nv12_data, output_FD.data_size);

                // 释放NV12内存（所有权转移：Worker->Aggregator->Writer）
                free(output_FD.nv12_data);
                output_FD.nv12_data = nullptr;

                // FPS统计：每30帧打印一次
                fps_frame_counter++;
                if (fps_frame_counter == 30) {
                    auto fps_end_time = std::chrono::high_resolution_clock::now();
                    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(fps_end_time - fps_start_time).count();
                    float real_fps = (fps_frame_counter * 1000.0f) / elapsed_ms;
                    std::cout << "\033[1;32m[Performance] End-to-End Real FPS: " << real_fps << " \033[0m" << std::endl;
                    fps_frame_counter = 0;
                    fps_start_time = std::chrono::high_resolution_clock::now();
                }
            }
        }
        else
        {
            // 队列为空时休眠，降低CPU占用
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    std::cerr << "[WriteThread] finished.\n";
}



/**
 * @brief 主函数：初始化摄像头、DMABUF、线程池，启动各线程并管理资源
 * @return 0:成功 非0:失败
 */
int main(void) {
    pthread_setname_np(pthread_self(), "main");
    std::signal(SIGINT, handleStopSignal);
    std::signal(SIGTERM, handleStopSignal);

    // 基础配置：摄像头分辨率、帧率、最大处理帧数
    int width = 1280;
    int height = 720;
    double fps = 30;
    int frame_num = 2000;

    // 初始化应用状态
    app_state_t state = {0};
    memset(&state, 0, sizeof(app_state_t));
    int ret;
    dmabuf_error_t dmabuf_ret;

    // 1. 打开摄像头设备（ISP主通道，非阻塞模式）
    int fd = open("/dev/video11", O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        perror("open video device");
        return -1;
    }

    // 2. 查询摄像头能力
    struct v4l2_capability cap;
    ret = ioctl(fd, VIDIOC_QUERYCAP, &cap);
    if (ret == -1) {
        perror("get video capability");
        return -1;
    }
    printf("Driver Caps: \n Driver: %s\n BUS: %s\n Version: %d.%d\n Capabilities: %08x\n ",
           cap.driver, cap.bus_info, (cap.version >> 16) & 0xff, (cap.version >> 24) & 0xff, cap.capabilities);

    // V4L2_CAP_DEVICE_CAPS置位时，应从device_caps读取该设备节点的实际能力。
    // RKISP采集节点使用VIDEO_CAPTURE_MPLANE，不能只检查单平面采集标志。
    uint32_t device_caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                               ? cap.device_caps
                               : cap.capabilities;
    bool supports_capture = device_caps &
                            (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE);
    printf(" Device Caps: %08x\n", device_caps);
    printf("Device %s video capture%s.\n",
           supports_capture ? "supports" : "does not support",
           (device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ? " (multi-plane)" : "");
    printf("Device %s streaming I/O.\n",
           (device_caps & V4L2_CAP_STREAMING) ? "supports" : "does not support");

    if (!supports_capture) {
        fprintf(stderr, "Selected device is not a V4L2 capture device.\n");
        return -1;
    }

    // 3. 设置摄像头格式：NV12、多平面（MPLANE）、1280x720
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;

    ret = ioctl(fd, VIDIOC_S_FMT, &fmt);
    if (ret == -1) {
        perror("set video format");
        return -1;
    }
    // 从多平面格式中获取实际的缓冲区大小（含ISP对齐 stride）
    int sizeimage = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    
    // 4. 初始化DMABUF堆（用于摄像头数据传输）
    dmabuf_ret = my_dma_heap_init(&state.dmabuf_heap);
    if (dmabuf_ret != DMABUF_SUCCESS) {
        printf("Failed to initialize DMABUF heap: \n");
        return -1;
    }

    // 5. 向V4L2申请DMABUF缓冲区（数量由kCaptureBufferCount统一配置，MPLANE模式）
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    const int BUFFER_NUM = kCaptureBufferCount;
    req.count = BUFFER_NUM;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;

    ret = ioctl(fd, VIDIOC_REQBUFS, &req);
    if (ret < 0) {
        perror("request buffer");
        return -1;
    }

    // 6. 分配并映射DMABUF缓冲区
    state.num_buffers = BUFFER_NUM;
    state.dmabuf_buffer = (dmabuf_buffer_t*)calloc(state.num_buffers, sizeof(dmabuf_buffer_t));
    state.buffer_infos = (v4l2_buffer_info_t*)calloc(state.num_buffers, sizeof(v4l2_buffer_info_t));
    for (int i = 0; i < BUFFER_NUM; i++) {
        char buffer_name[32];
        snprintf(buffer_name, sizeof(buffer_name), "v4l2_buffer_%d", i);

        // 分配DMABUF缓冲区（使用MPLANE的 plane_fmt sizeimage）
        dmabuf_ret = my_dmabuf_buffer_alloc(&state.dmabuf_heap, &state.dmabuf_buffer[i],
                                            sizeimage, buffer_name);
        if (dmabuf_ret != DMABUF_SUCCESS) {
            printf("Failed to allocate DMABUF %d\n", i);
            return -1;
        }

        // 映射DMABUF到CPU地址空间
        dmabuf_ret = my_dmabuf_buffer_map(&state.dmabuf_buffer[i]);
        if (dmabuf_ret != DMABUF_SUCCESS) {
            printf("Failed to map DMABUF %d\n", i);
            return -1;
        }

        // 保存缓冲区信息
        state.buffer_infos[i].index = i;
        state.buffer_infos[i].dmabuf_fd = my_dmabuf_get_fd(&state.dmabuf_buffer[i]);
        state.buffer_infos[i].mapped_addr = my_dmabuf_get_mapped_addr(&state.dmabuf_buffer[i]);
        state.buffer_infos[i].size = my_dmabuf_get_size(&state.dmabuf_buffer[i]);
    }
    state.initialized = true;

    // 7. 将DMABUF缓冲区入队V4L2，准备采集（MPLANE模式）
    for (int i = 0; i < state.num_buffers; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[1];

        memset(&buf, 0, sizeof(buf));
        memset(&planes, 0, sizeof(planes));

        buf.index = state.buffer_infos[i].index;
        buf.memory = V4L2_MEMORY_DMABUF;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.m.planes = planes;
        buf.length = 1;
        buf.m.planes[0].m.fd = state.buffer_infos[i].dmabuf_fd;
        buf.m.planes[0].length = state.buffer_infos[i].size;

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("queue buffer");
            return -1;
        }
    }

    // 8. 启动摄像头流采集
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ret = ioctl(fd, VIDIOC_STREAMON, &type);
    if (ret < 0) {
        perror("stream on");
        return -1;
    }

    // 9. 初始化编码/推流参数（宽高按16对齐）
    g_hor_stride = ALIGN(width, 16);
    g_ver_stride = ALIGN(height, 16);
    int fourcc = cv::VideoWriter::fourcc('H', '2', '6', '4');
    int bitrate = width * height / 8 * (fps / 1);
    std::string rtmpPath = "rtmp://192.168.137.99:1935/live/cv";  // RTMP推流地址
    printf("Video size:%d x %d, fps: %f, total frame = %d\n", width, height, fps, frame_num);

    // 10. 初始化推流模块（MPP编码+RTMP）
    init_streamer(width, height, fps, bitrate, rtmpPath.c_str());

    // 11. 启动线程：读线程、处理线程、写线程
    int index = 0;          // 帧索引初始值
    mutex cap_m;            // 预留互斥锁

    // 读线程：采集+解码
    std::thread video_reader(Thread_ReadVideo, ref(state), ref(fd), ref(g_SafeQueueRead),
                             ref(index), ref(cap_m), ref(g_readFinish));
    // 处理线程：推理+格式转换
    std::thread video_process(aggregatorThreadFunc, ref(gthreadpool));
    // 写线程：编码+推流（1个线程）
    cv::Size framesize(width, height);
    const std::string local_record_path = "/home/radxa/Dev/DMA/output.avi";
    cv::VideoWriter writer(local_record_path,
                           cv::VideoWriter::fourcc('I', '4', '2', '0'),
                           fps, framesize);
    if (writer.isOpened()) {
        std::cout << "本地录制已开启: " << local_record_path << std::endl;
    } else {
        std::cerr << "本地录制初始化失败: " << local_record_path
                  << "，网络推流将继续运行" << std::endl;
    }
    std::vector<thread> video_writer;
    video_writer.emplace_back(Thread_WriteVideo, ref(writer));

    // 12. 等待线程退出
    video_reader.join();          // 等待读线程完成
    for (thread& t : video_writer) {  // 等待写线程完成
        t.join();
    }
    video_process.join();         // 等待处理线程完成

    // 13. 停止摄像头流采集
    ret = ioctl(fd, VIDIOC_STREAMOFF, &type);
    if (ret < 0) {
        perror("stream off");
        return -1;
    }

    // 14. 释放DMABUF资源
    if (state.dmabuf_buffer) {
        for (int i = 0; i < state.num_buffers; i++) {
            my_dmabuf_buffer_cleanup(&state.dmabuf_buffer[i]);
        }
        free(state.dmabuf_buffer);
        state.dmabuf_buffer = NULL;
    }
    if (state.buffer_infos) {
        free(state.buffer_infos);
        state.buffer_infos = NULL;
    }
    my_dmabuf_heap_cleanup(&state.dmabuf_heap);

    // 15. 关闭设备，清理队列
    close(fd);
    g_SafeQueueRead.stop();
    g_SafeQueueWrite.stop();

    // 16. 释放视频写入器
    writer.release();
    printf("code end!\n");

    return 0;
}
