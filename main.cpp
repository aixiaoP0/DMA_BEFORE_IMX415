#include <iostream>
#include <opencv2/core/core.hpp>        // OpenCV核心功能库
#include <opencv2/highgui/highgui.hpp>  // OpenCV高级功能库（视频处理等）
#include <pthread.h>                    // POSIX线程库（设置线程名）
#include "mpp_decoder.h"                // MPP硬件解码库
#include <chrono>                       // 时间相关操作
#include <thread>                       // C++线程库
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
    MppFrame mpp_frame = NULL;   // MPP硬件解码帧对象
    int index;                   // 帧索引（保证帧序）
    int dmabuf_fd = -1;          // 【修改】记录当前帧所在的 DMABUF fd
    // 编码/推流用：RGA转换后的NV12数据
    uint8_t* nv12_data = nullptr;
    int data_size = 0;           // NV12数据长度
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
#define model_path "/home/radxa/chapter1/model/yolov5s.rknn"

// 全局线程池：加载YOLOv5s模型，初始化4个推理线程
ThreadPool gthreadpool(model_path,4);
// 全局帧索引起始值
static int g_frame_start_id = 0;

// 线程安全队列：读线程->处理线程 | 处理线程->写线程
SafeQueue<FrameData> g_SafeQueueRead(100);//安全队列，用于线程间通信
SafeQueue<FrameData> g_SafeQueueWrite(100);
std::atomic<bool> g_readFinish(false);
std::atomic<bool> g_processFinish(false);

/**
 * @brief 视频读取线程函数：从V4L2摄像头采集数据，MPP解码后入队
 * @param state 应用状态（DMABUF/V4L2缓冲区）
 * @param fd 摄像头设备文件描述符
 * @param g_SafeQueueRead 解码后帧队列（输出）
 * @param img_index 帧索引（自增）
 * @param cap_mutex 互斥锁（未实际使用，预留）
 * @param finished 退出标志
 */
void Thread_ReadVideo(app_state_t& state,int& fd,SafeQueue<FrameData>& g_SafeQueueWrite,int& img_index,mutex& cap_mutex, std::atomic<bool>& finished){
    // 设置线程名（便于调试）
    pthread_setname_np(pthread_self(), "ReadVideo");
    int ret;
    dmabuf_error_t dmabuf_ret;
    // 初始化MPP解码器（分辨率与V4L2配置一致：1280x720）
    MppDecoder decoder;
    if (decoder.init(1280, 720) != 0) {
    printf("Failed to init MPP decoder\n");
    }

    // 预分配BGR帧缓存（避免循环内频繁malloc）
    cv::Mat bgr_frame(720, 1280, CV_8UC3);
    
   while(true)
   {
    //定义一个 FrameData 类型的临时变量 frame_temp，用于暂存一帧视频的完整数据
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
        // “超时后继续循环等待”，更符合实际采集场景
        continue; 
    }
    // 从V4L2出队缓冲区（获取摄像头数据）
    struct v4l2_buffer buf;
    memset(&buf,0,sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_DMABUF;

    ret = ioctl(fd,VIDIOC_DQBUF,&buf);
     if(ret < 0)
    {
        perror("dequeue buf");
        break;
    }
    // 获取DMABUF缓冲区信息（FD、映射地址、大小）
    dmabuf_buffer_t *dmabuf_buffer = &state.dmabuf_buffer[buf.index];
    buffer_info.dmabuf_fd = my_dmabuf_get_fd(dmabuf_buffer);
    buffer_info.mapped_addr = my_dmabuf_get_mapped_addr(dmabuf_buffer);
    buffer_info.size = my_dmabuf_get_size(dmabuf_buffer);

    // 同步DMABUF到CPU可访问（开始读取）
    dmabuf_ret = my_dmabuf_sync_start(my_dmabuf_get_fd(dmabuf_buffer));
        if (dmabuf_ret != DMABUF_SUCCESS) {
            printf("Failed to sync start buffer %d\n", buf.index);
        } 
        else {
    // 获取MJPEG数据指针和长度
    void* mjpeg_data = buffer_info.mapped_addr;
    size_t mjpeg_size = buf.bytesused;

    // MPP解码MJPEG为NV12格式的硬件帧
    MppFrame frame = decoder.decode_by_fd(buffer_info.dmabuf_fd, mjpeg_size);
    //frame此时为NV12
    if (frame) {
        // 获取解码后帧分辨率（含步长对齐）
        int width = mpp_frame_get_width(frame);
        int height = mpp_frame_get_height(frame);
        
        // 确保 Mat 大小匹配
        if (bgr_frame.cols != width || bgr_frame.rows != height) {
            bgr_frame.create(height, width, CV_8UC3);
        }
        // === 画图推流数据 (BGR 1280x720) ===
        /// RGA转换：NV12（MPP帧）-> BGR888（OpenCV Mat）
        NV12_to_BGR_with_rga_by_dma(frame, bgr_frame);
        // 深度拷贝给 frame_temp.frame，因为 bgr_frame 在下一轮会被覆盖
        frame_temp.frame = bgr_frame.clone(); 
        //  必须在这里赋值并自增！确保进入队列的帧序号绝对连续
        frame_temp.index = img_index++;
        //  同时将 MPP 硬件解码帧直接甩给线程池，在线程池中实现格式转换
        frame_temp.mpp_frame = frame; 

        g_SafeQueueWrite.enqueue(frame_temp);
    } else {
        printf("MPP Decode failed, frame dropped\n");
    }

    
}

    // 同步DMABUF（结束读取）
    dmabuf_ret = my_dmabuf_sync_stop(my_dmabuf_get_fd(dmabuf_buffer));
    if (dmabuf_ret != DMABUF_SUCCESS) {
        printf("Failed to sync stop buffer %d\n", buf.index);
    }

    // 将空缓冲区重新入队V4L2，继续采集
    buf.length = state.buffer_infos[buf.index].size;
    buf.m.fd = state.buffer_infos[buf.index].dmabuf_fd;
    if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        perror("queue buf");
        finished = true;
        break;
    }
   }

   // 退出前销毁解码器
   decoder.deinit();
}


// 聚合线程：既提交多帧到线程池并行处理，也按顺序收集结果
void aggregatorThreadFunc(ThreadPool &gthreadpool)
{   
    pthread_setname_np(pthread_self(), "Aggregator");
    int nextWriteIndex = 0;
    // 存储"帧下标 -> future"的映射，实现无阻塞并行提交与按序收集 tasks_inflight:进行中的任务
    map<int,future<ProcessResult>> tasks_inflight;
    
    // 无限循环，直到处理结束
    while(true)
    {   
         
        bool is_idle = true; // 空闲标志

        // 步骤A：批量尝试从 g_SafeQueueRead 获取新帧并提交到线程池    
        FrameData inputFD;
        // 如果读取队列不为空，则取出一帧进行处理 && 控制并行度，避免内存溢出
        while(!g_SafeQueueRead.empty() && tasks_inflight.size() <20)
        {
            if(g_SafeQueueRead.dequeue(inputFD))
            {   
               // 异步提交推理任务，保存future对象
                auto future = gthreadpool.submit_task_async(inputFD.index, inputFD.frame,inputFD.mpp_frame);
                // 将 (index -> future) 存到映射
                tasks_inflight[inputFD.index] = std::move(future);
            }
            is_idle = false; // 只要发了任务，就不算闲着！
        }
            // 步骤B：检查是否有"下一个待写帧(nextWriteIndex)"已经推理完成
            // 如果完成，就把其结果按顺序放到 g_writeQueue
            
        auto it = tasks_inflight.find(nextWriteIndex);
        while(it!=tasks_inflight.end())
            {    
                // 非阻塞检查任务是否完成
                auto status = it->second.wait_for(std::chrono::milliseconds(0));
                if(status == std::future_status::ready)
                {
                    //获取推理结果
                    ProcessResult result = it->second.get();

                     // 封装输出帧数据（传递NV12数据，转移内存所有权）
                    FrameData outputFD;
                    outputFD.index = nextWriteIndex;
                    outputFD.frame = result.processed_img.clone();// 预留：本地保存视频用
                    outputFD.nv12_data = result.nv12_data; 
                    outputFD.data_size = result.data_size;
                    
                    // 结果入队，供写线程编码/推流
                    g_SafeQueueWrite.enqueue(outputFD);
                    // 移除映射并递增下一个待写index
                    tasks_inflight.erase(it);
                    cout<<"当前已经处理完成了："<<nextWriteIndex<<"帧图片，剩余任务："<<tasks_inflight.size()<<endl;
                    nextWriteIndex++;
                    // 继续尝试下一个
                    it = tasks_inflight.find(nextWriteIndex);
                    is_idle = false; // 只要收了任务，就不算闲着！
                }
                else
                {
                    // 下一个还没完成，就先退出等待，后面再检测
                    break;
                }
            }
        // 步骤C：判断退出条件
        //   若读完了 && 读队列空了 && 当前映射也空了，就说明都处理完了
        if(g_readFinish && g_SafeQueueRead.empty()  && tasks_inflight.empty())
        {
            cout<< "处理线程已结束" <<endl;
            break;
        }
        // 如果当前任务已满，适当增加睡眠时间避免CPU空转
        if(is_idle) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
    }
    // 设置处理完成标志
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
        
        // 处理完成 写入完成
        if(!g_SafeQueueWrite.empty())
        {
            FrameData output_FD;
            if(!g_SafeQueueWrite.dequeue(output_FD))
            {
                //即使没取到也要尝试继续，直到所有任务结束
                continue; //在 while(true) 循环中，触发后会跳过当前循环剩余代码，直接回到循环开头
            }
            
            if(output_FD.nv12_data != nullptr)
            {
                // A.写入视频版本
                //  writer.write(output_FD.frame);

                // 调用MPP编码+推流接口
                process_frame(output_FD.nv12_data, output_FD.data_size);

                // 释放NV12内存（所有权转移：Worker->Aggregator->Writer）
                free(output_FD.nv12_data);
                output_FD.nv12_data = nullptr;

                // FPS统计：每30帧打印一次
                fps_frame_counter++;
                if (fps_frame_counter == 30) { 
                    auto fps_end_time = std::chrono::high_resolution_clock::now();
                    // 计算 30 帧的总耗时（毫秒）
                    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(fps_end_time - fps_start_time).count();
                    
                    // 真实 FPS = (处理的帧数 * 1000) / 总耗时(毫秒)
                    float real_fps = (fps_frame_counter * 1000.0f) / elapsed_ms;
                    
                    // 终端打印为高亮绿色，方便观察
                    std::cout << "\033[1;32m[Performance] End-to-End Real FPS: " << real_fps << " \033[0m" << std::endl;
                    
                    // 重置计数器和起始时间，准备下一个周期的计算
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

    // 1. 打开摄像头设备（非阻塞模式，配合poll）
    int fd = open("/dev/video0", O_RDWR | O_NONBLOCK);
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
    printf("Device %s video capture.\n", (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ? "supports" : "does not support");
    printf("Device %s video output.\n", (cap.capabilities & V4L2_CAP_VIDEO_OUTPUT) ? "supports" : "does not support");

    // 3. 设置摄像头格式：MJPEG、1280x720
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    ret = ioctl(fd, VIDIOC_S_FMT, &fmt);
    if (ret == -1) {
        perror("set video format");
        return -1;
    }
    struct v4l2_pix_format pix_fmt = fmt.fmt.pix;
    
    // 4. 初始化DMABUF堆（用于摄像头数据传输）
    dmabuf_ret = my_dma_heap_init(&state.dmabuf_heap);
    if (dmabuf_ret != DMABUF_SUCCESS) {
        printf("Failed to initialize DMABUF heap: \n");
        return -1;
    }

    // 5. 向V4L2申请DMABUF缓冲区（数量=4）
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    int BUFFER_NUM = 4;
    req.count = BUFFER_NUM;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
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

        // 分配DMABUF缓冲区
        dmabuf_ret = my_dmabuf_buffer_alloc(&state.dmabuf_heap, &state.dmabuf_buffer[i],
                                            pix_fmt.sizeimage, buffer_name);
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

    // 7. 将DMABUF缓冲区入队V4L2，准备采集
    for (int i = 0; i < state.num_buffers; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.index = state.buffer_infos[i].index;
        buf.memory = V4L2_MEMORY_DMABUF;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.m.fd = state.buffer_infos[i].dmabuf_fd;
        buf.length = state.buffer_infos[i].size;

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            perror("queue buffer");
            return -1;
        }
    }

    // 8. 启动摄像头流采集
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
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
    std::string rtmpPath = "rtmp://192.168.15.214:1935/live/cv";  // RTMP推流地址
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
    cv::VideoWriter writer("/home/radxa/DMA/output.avi", cv::VideoWriter::fourcc('I', '4', '2', '0'), fps, framesize);
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