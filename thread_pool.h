#ifndef __THREAD_POOL_H
#define __THREAD_POOL_H

#include "yolov5s.h"
#include "dmabuf.h"

#include <iostream>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <exception>
#include <utility>
#include <memory>
#include <vector>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>

using namespace std;
using namespace cv;

/**
 * @brief 处理结果结构体
 * @details 存储单帧图像的处理结果、NV12数据、检测结果及状态信息
 */
typedef struct ProcessResult {
    cv::Mat processed_img;          // 处理后的图像数据
    uint8_t* nv12_data = nullptr;   // 转换后的 NV12 格式图像数据
    int data_size = 0;              // NV12 数据的字节长度
    detect_result_group_t detection_results; // 目标检测结果组
    bool success = false;           // 处理是否成功的标志位
    std::string error_msg;          // 处理失败时的错误信息
} ProcessResult;

/**
 * @brief 任务结构体（简化版）
 * @details NV12 已由 ReadVideo 直写 NPU 内存，Task 仅携带 BGR 图像和帧信息
 */
typedef struct Task {
    int index = 0;                  // 帧序号
    cv::Mat img;                    // BGR图像（OpenCV画框用）
    int width = 0;                  // 原始图像宽度
    int height = 0;                 // 原始图像高度
} Task;

/**
 * @brief Worker处理结果（用于结果收集队列）
 */
typedef struct WorkerResult {
    int index;                      // 帧序号
    ProcessResult result;           // 推理结果
} WorkerResult;

/**
 * @brief 线程池类
 * @details 每Worker独立任务队列 + 统一结果收集队列，支持指定Worker提交
 *          移除future/promise机制，Worker完成后结果入公共队列供Aggregator收集
 */
class ThreadPool
{
public:
    ThreadPool(const char *model_path, int num_threads);
    ~ThreadPool();

    /**
     * @brief 提交任务到指定Worker（无future，异步非阻塞）
     * @param worker_id 目标Worker ID (0 ~ num_threads-1)
     * @param index 帧序号
     * @param img BGR图像
     * @param width 原始图像宽度
     * @param height 原始图像高度
     */
    void submit_to_worker(int worker_id, int index, const cv::Mat& img,
                          int width, int height);

    /**
     * @brief 获取指定Worker的NPU输入缓冲区fd（供ReadVideo RGA直写）
     * @param worker_id Worker ID
     * @return NPU输入内存的DMABUF fd
     */
    int get_worker_input_fd(int worker_id);

    /**
     * @brief 非阻塞获取已完成的结果
     * @param wr 输出参数，成功时填充WorkerResult
     * @return true=有结果已取出, false=暂无结果
     */
    bool try_get_result(WorkerResult& wr);

    /**
     * @brief 当前进行中的任务数（已提交未完成）
     */
    int in_flight_count() { return tasks_in_flight.load(); }

    // NPU 缓冲区状态查询/设置（用于丢帧保护）
    bool is_worker_busy(int worker_id) { return worker_ctx[worker_id].is_busy.load(); }
    void set_worker_busy(int worker_id, bool state) { worker_ctx[worker_id].is_busy.store(state); }

private:
    // 每Worker独立上下文（队列+锁+条件变量）
    struct WorkerContext {
        std::queue<Task> tasks;
        mutex mtx;
        condition_variable cv;
        std::atomic<bool> is_busy{false};  // NPU 输入缓冲区被占用标志
    };

    int init(const char *model_path, int num_threads);

    // YOLO模型实例组（每Worker独立持有）
    vector<shared_ptr<Yolov5s>> yolo_group;

    // 每Worker独立任务队列（unique_ptr数组避免mutex不可拷贝问题）
    std::unique_ptr<WorkerContext[]> worker_ctx;

    // 工作线程容器
    std::vector<thread> threads;

    // 线程池运行标志
    bool run_flag = true;

    // 结果收集队列（所有Worker共享，加锁保护）
    std::queue<WorkerResult> result_queue;
    mutex result_mtx;
    atomic<int> tasks_in_flight{0};

    // 工作线程处理函数
    void worker(int id);
};

#endif
