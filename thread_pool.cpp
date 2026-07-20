#include "thread_pool.h"
#include <sched.h>     // CPU_ZERO/CPU_SET
#include <pthread.h>   // pthread_setname_np, pthread_setaffinity_np
#include <chrono>      // system_clock（OSD时间水印用）
#include <ctime>       // tm, localtime（时间格式化）
#include <cstdio>      // snprintf（时间戳字符串格式化）

// 外部全局变量：RGA stride 对齐
extern int g_hor_stride;
extern int g_ver_stride;

// 外部函数：BGR→NV12 格式转换（RGA硬件加速）
extern void BGR_to_NV12_with_rga(uint8_t* bgr, uint8_t* nv12, int width, int height);

/**
 * @brief 线程池构造函数
 * @param model_path RKNN模型文件路径
 * @param num_threads 工作线程数量
 */
ThreadPool::ThreadPool(const char *model_path, int num_threads)
{
    printf("初始化线程池\r\n");
    this->run_flag = true;
    init(model_path, num_threads);
}

/**
 * @brief 线程池析构函数
 * @details 停止所有Worker，唤醒阻塞线程，等待退出
 */
ThreadPool::~ThreadPool()
{
    printf("析构线程池\r\n");
    this->run_flag = false;

    // 唤醒所有Worker（各自的条件变量）
    for(size_t i = 0; i < threads.size(); i++) {
        worker_ctx[i].cv.notify_all();
    }

    // 等待所有工作线程退出
    for(thread &t : threads) {
        if(t.joinable()) {
            t.join();
        }
    }
    std::cout << "ThreadPool destroyed.\n";
}

/**
 * @brief 线程池初始化
 * @param model_path RKNN模型文件路径
 * @param num_threads 工作线程数量
 * @return 0:成功
 */
int ThreadPool::init(const char *model_path, int num_threads)
{
    if(!num_threads) num_threads = 1;

    // 初始化每Worker独立任务队列（unique_ptr new[] 默认构造所有元素）
    worker_ctx.reset(new WorkerContext[num_threads]);
    // worker_ctx = std::make_unique<WorkerContext[]>(num_threads);
    // 为每个Worker创建独立的YOLOv5s模型实例（NPU核心i%3轮询）
    for(size_t i = 0; i < num_threads; i++) {
        std::shared_ptr<Yolov5s> yolo = std::make_shared<Yolov5s>(model_path, i % 3);
        yolo_group.emplace_back(yolo);
    }

    // 启动num_threads个工作线程
    for(size_t i = 0; i < num_threads; i++) {
        threads.emplace_back(&ThreadPool::worker, this, i);
    }
    printf("Thread Pool Init OK, workers = %zu\n", num_threads);
    return 0;
}

/**
 * @brief 提交任务到指定Worker（异步非阻塞）
 * @param worker_id 目标Worker ID
 * @param index 帧序号
 * @param img BGR图像（画框用）
 * @param width 原始图像宽度
 * @param height 原始图像高度
 */
void ThreadPool::submit_to_worker(int worker_id, int index, const cv::Mat& img,
                                   int width, int height)
{
    Task t;
    t.index  = index;
    t.img    = img;
    t.width  = width;
    t.height = height;

    tasks_in_flight++;

    // 入队到指定Worker的私有队列
    {
        lock_guard<mutex> lock(worker_ctx[worker_id].mtx);
        worker_ctx[worker_id].tasks.push(std::move(t));
    }
    worker_ctx[worker_id].cv.notify_one();
}

/**
 * @brief 获取指定Worker的NPU输入缓冲区DMABUF fd
 * @param worker_id Worker ID
 * @return NPU输入内存的fd，供ReadVideo中RGA直写
 */
int ThreadPool::get_worker_input_fd(int worker_id)
{
    return yolo_group[worker_id]->my_get_input_fd();
}

/**
 * @brief 非阻塞获取已完成的推理结果
 * @param wr 输出参数，成功时填充WorkerResult
 * @return true=有结果, false=队列空
 */
bool ThreadPool::try_get_result(WorkerResult& wr)
{
    lock_guard<mutex> lock(result_mtx);
    if(result_queue.empty()) return false;
    wr = std::move(result_queue.front());
    result_queue.pop();
    return true;
}

/**
 * @brief 工作线程主循环
 * @param id Worker唯一标识（0 ~ num_threads-1）
 * @details
 *   1. 绑核到RK3588大核（CPU4-7）
 *   2. 从自己的私有队列取任务
 *   3. NPU输入已在ReadVideo中被RGA直写完，直接rknn_run
 *   4. 推理后处理 + 画框 + BGR→NV12
 *   5. 结果入公共队列供Aggregator收集
 */
void ThreadPool::worker(int id)
{
    // 设置线程名
    pthread_setname_np(pthread_self(), "worker");

    // === RK3588 绑核：CPU4-7（A76大核） ===
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    int target_cpu = 4 + (id % 4);
    CPU_SET(target_cpu, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    std::shared_ptr<Yolov5s> yolo = this->yolo_group[id];
    std::cout << "Worker " << id << " 启动 (CPU" << target_cpu << ", NPU Core" << (id % 3) << ")" << endl;

    while(run_flag)
    {
        Task t;

        // === 从自己的私有队列取任务 ===
        {
            unique_lock<mutex> lock(worker_ctx[id].mtx);
            worker_ctx[id].cv.wait(lock, [this, id] {
                return !worker_ctx[id].tasks.empty() || !run_flag;
            });
            if(!run_flag && worker_ctx[id].tasks.empty()) break;
            if(worker_ctx[id].tasks.empty()) continue;

            t = std::move(worker_ctx[id].tasks.front());
            worker_ctx[id].tasks.pop();
        }

        ProcessResult res;
        try {
            // === NPU 推理 ===
            // 输入数据已由 ReadVideo 通过 RGA 直写 NPU 物理内存，此处直接 rknn_run
            yolo->inference_zero_copy(t.width, t.height, &res.detection_results);

            // === 绘制检测框到 BGR 原图 ===
            yolo->draw_result(t.img, &res.detection_results);

            // === OSD 时间水印：将当前系统时间叠加到画面左上角 ===
            // 目的：在播放器画面上直接看到时间戳，通过对比"板端当前时间"与"画面显示时间"
            //       即可人眼估算端到端推流延迟（方法：看秒表 vs 画面上的 HH:MM:SS.mmm）
            // 注意：这里使用 system_clock 而非 high_resolution_clock，
            //       因为我们要的是"人类可读的挂钟时间"而非单调递增计时
            {
                auto now = std::chrono::system_clock::now();
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()) % 1000;
                auto now_t = std::chrono::system_clock::to_time_t(now);
                std::tm* tm = std::localtime(&now_t);
                char timestamp[32];
                snprintf(timestamp, sizeof(timestamp), "%02d:%02d:%02d.%03ld",
                         tm->tm_hour, tm->tm_min, tm->tm_sec,
                         static_cast<long>(now_ms.count()));

                // 在画面左上角 (10, 40) 位置绘制白色时间戳，粗细为2
                // 绿色文字便于在白/黑背景上都清晰可辨
                cv::putText(t.img, timestamp, cv::Point(10, 40),
                            cv::FONT_HERSHEY_SIMPLEX, 1.0,
                            cv::Scalar(0, 255, 0), 2);
            }

            // === BGR → NV12（供MPP编码器用） ===
            int nv12_size = t.width * t.height * 3 / 2;
            uint8_t* pixel_buffer = (uint8_t*)malloc(nv12_size);
            if(pixel_buffer) {
                BGR_to_NV12_with_rga(t.img.data, pixel_buffer, t.width, t.height);
                res.processed_img = t.img;
                res.nv12_data = pixel_buffer;
                res.data_size = nv12_size;
                res.success = true;
            } else {
                res.success = false;
                res.error_msg = "Malloc failed in worker";
            }
        }
        catch(const std::exception& e) {
            res.error_msg = e.what();
            res.success = false;
            if(res.nv12_data) { free(res.nv12_data); res.nv12_data = nullptr; }
        }

        // === 结果放入公共收集队列 ===
        {
            lock_guard<mutex> lock(result_mtx);
            result_queue.push({t.index, std::move(res)});
        }
        tasks_in_flight--;

        // NPU 输入缓冲区已使用完毕，允许 ReadVideo 写入新帧
        worker_ctx[id].is_busy.store(false);
    }

    std::cout << "Worker " << id << " 退出" << std::endl;
}
