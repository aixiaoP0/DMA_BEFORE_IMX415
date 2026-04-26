#include "thread_pool.h"
#include <sched.h> // 线程绑核所需头文件
#include <pthread.h>

#include "dmabuf.h"
/**
 * @brief 线程池构造函数
 * @param model_path 模型文件路径
 * @param num_threads 工作线程数量
 */
ThreadPool::ThreadPool(const char * model_path,int num_threads)
{
    printf("初始化线程池\r\n"); 
    this->run_flag =true; //设置线程池的运行状态为 true
    init(model_path,num_threads); // 调初始化线程池核心逻辑
}
// 外部声明的全局变量（图像 stride 相关）
extern int g_hor_stride;
extern int g_ver_stride;
/**
 * @brief 利用RGA硬件将BGR格式图像转换为NV12格式
 * @param bgr 输入BGR图像数据指针
 * @param nv12 输出NV12图像数据指针
 * @param width 图像宽度
 * @param height 图像高度
 */
extern void BGR_to_NV12_with_rga(uint8_t* bgr, uint8_t* nv12, int width, int height);
/**
 * @brief 线程池析构函数
 * @details 停止线程池运行，唤醒所有阻塞线程，等待线程退出并释放资源
 */
ThreadPool::~ThreadPool()
{
    printf("析构线程池\r\n"); 
    this->run_flag = false; //设置线程池的运行状态为 flase
    task_cond.notify_all();//唤醒所有等待任务（被困在cond）的线程的

    //等待所有工作线程退出
    for(thread &t: threads)
    {
        if(t.joinable())
        {
            t.join();
        }
    }
    // 可在此处补充模型、内存池等资源的释放逻辑
    std::cout << "ThreadPoll destroyed. \n";
}
/**
 * @brief 线程池初始化核心函数
 * @param model_path 模型文件路径
 * @param num_threads 工作线程数量（最小为1）
 * @return 0:成功 其他:失败（当前默认返回0）
 */
int ThreadPool::init(const char *model_path,int num_threads)
{   
    // 线程数合法性校验：最小保证1个工作线程
    if(!num_threads)
    {num_threads =1;}
    // 为每个线程初始化独立的YOLO模型实例（按ID轮询使用3个模型实例）
    for(size_t i=0;i<num_threads; i++ )
    {
        std::shared_ptr<Yolov5s> yolo = std::make_shared<Yolov5s>(model_path, i%3);
        yolo_group.emplace_back(yolo);
    }
    //启动 num_threads 个工作线程
    for(size_t i=0;i<num_threads; i++ )
    {
        threads.emplace_back(&ThreadPool::worker, this , i);
    }
    printf("Thread Pool Init OK\r\n");
    return 0;
}
/**
 * @brief 异步提交处理任务到线程池
 * @param index 任务索引
 * @param img 待处理的OpenCV图像（CV::Mat）
 * @param mpp_frame MPP编码帧（硬件解码输出）
 * @return std::future<ProcessResult> 任务结果的异步获取对象
 */
std::future<ProcessResult> ThreadPool::submit_task_async(int index,const cv::Mat& img,MppFrame mpp_frame)
{
    Task t;
    t.img = img;
    t.index = index;
    t.mpp_frame = mpp_frame;
    // 获取任务结果的future对象（与promise绑定）
    std::future<ProcessResult> res_future = t.promise.get_future();
    // 任务队列限流：超过10个任务时等待，避免队列溢出
    while(tasks.size() > 10)
    {
        this_thread::sleep_for(chrono::milliseconds(1));
    }

    // 临界区：任务入队（必须加锁，避免多线程竞争）
    {
    // 【重要】必须加锁！！否则和 worker 线程冲突会 Crash
    unique_lock<mutex> lock(task_mutx);
    // 入队 (使用 std::move，因为 Task 包含不可拷贝的 promise)
    tasks.push(move(t)); 
    }
    task_cond.notify_one();// 唤醒一个空闲的工作线程处理新任务
    return res_future;
}
/**
 * @brief 工作线程核心处理逻辑
 * @param id 工作线程唯一ID（从0开始）
 * @details 1. 绑定线程到RK3588大核（CPU4-7）提升性能
 *          2. 循环等待并处理任务队列中的任务
 *          3. 支持零拷贝推理、RGA硬件转换、异常安全处理
 */
void ThreadPool::worker(int id)
{   
    // 设置线程名称（方便系统调试工具识别）
    pthread_setname_np(pthread_self(), "worker");

    // --- RK3588 专属绑核逻辑（提升NPU推理性能）---
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    // 大核分布：CPU4/5/6/7，按线程ID轮询绑定
    int target_cpu = 4 + (id % 4); 

    CPU_SET(target_cpu, &cpuset);
    
    // // 只有在绑核成功时才打印，方便调试
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0) {
        // printf("Worker %d 成功绑定到 CPU %d\n", id, target_cpu);
    }
    std::shared_ptr<Yolov5s> yolo = this->yolo_group[id];
    std::cout << "work 线程启动 id = " << id << endl;

    while(run_flag)
    {   
        Task t;

        // 临界区：获取任务（锁作用域最小化，减少阻塞）
        {  
            unique_lock<mutex> lock(task_mutx);
            ////等待任务变量，直到有任务或线程池停止运行
            task_cond.wait(lock,[this]{return( !tasks.empty() || !run_flag);});
            // 线程池停止且无剩余任务：退出循环
            if(!run_flag && tasks.empty())
            {
                printf("%d 下班",id);
                break;
            }
            // 添加防止虚假唤醒（条件满足但队列为空）
            if(tasks.empty()) continue; 
            //取出队列头部任务
            t = move(tasks.front());
            tasks.pop();
        }
        //任务处理逻辑（无锁区执行，提升并发效率）
        ProcessResult res;
        try{
            printf("worker %d get task !\r\n",id);
            // --- 全链路零拷贝推理（硬件加速核心逻辑）---
            if (t.mpp_frame != NULL) {
                // a. 拿到属于这个专属 worker 的 RKNN 内部内存 fd
                int npu_fd = yolo->my_get_input_fd();
                
                // b. 让 RGA 硬件把画面缩放并直接写入这个 NPU fd！
                MppFrame_to_ModelInput_by_dma_dst_fd(t.mpp_frame, npu_fd, 640, 640);
                
                // c. NPU 直接开跑
                yolo->inference_zero_copy(1280, 720, &res.detection_results);
                
                // d. 【极其重要】硬件用完，在这里释放上游传来的 MPP 帧！
                mpp_frame_deinit(&t.mpp_frame);
            }
            // 绘制推理结果到原始图像
            yolo->draw_result(t.img,&res.detection_results);
    
        //===================填充结果====================
        
        int width = t.img.cols;
        int height = t.img.rows;
        int nv12_size = width * height * 3 / 2;
        uint8_t* pixel_buffer = (uint8_t *)malloc(nv12_size);
        if(pixel_buffer) 
        {
            // RGA硬件执行格式转换（线程隔离，RGA handle安全）
            BGR_to_NV12_with_rga(t.img.data, pixel_buffer, width, height);
            // 填充任务结果
            res.processed_img = t.img;      // 带推理结果的原始图像（本地保存用）

            res.nv12_data = pixel_buffer; // 指向新分配的内存
            res.data_size = nv12_size;    // NV12数据长度
            res.success = true;
        } else {
            // 内存分配失败处理
            res.success = false;
            res.error_msg = "Malloc failed in worker";
            // 释放内存，防止内存泄漏
            if(res.nv12_data) { free(res.nv12_data); res.nv12_data = nullptr; }
            mpp_frame_deinit(&t.mpp_frame);
        }
        }
         catch(const std::exception& e)
        {
            // 异常捕获：保证线程不崩溃，正确释放资源
            res.error_msg = e.what();
            res.success = false;
            // 异常时也要释放 buffer
             if(res.nv12_data) { free(res.nv12_data); res.nv12_data = nullptr; }
        }
        // 向调用方返回任务结果（触发future的get()返回）
        t.promise.set_value(res);
    }
     // 线程退出日志：打印剩余任务数（调试用）
    std::cout << "worker " << id << " exited,remaining tasks : " << tasks.size() << std::endl;
}