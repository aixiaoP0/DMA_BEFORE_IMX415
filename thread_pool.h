#ifndef __THREAD_POOL_H
#define __THREAD_POOL_H
#include "mpp_decoder.h"
#include "yolov5s.h"
#include "dmabuf.h"

// 标准库头文件
#include <iostream>
#include <thread>
#include <map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <utility>
#include <exception>
#include <future>

// OpenCV 头文件
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>

// 命名空间声明
using namespace std;
using namespace cv;

/**
 * @brief 处理结果结构体
 * @details 存储单帧图像的处理结果、NV12数据、检测结果及状态信息
 */
typedef struct ProcessResult {
    cv::Mat processed_img;          // 处理后的图像数据
    uint8_t* nv12_data;             // 转换后的 NV12 格式图像数据
    int data_size;                  // NV12 数据的字节长度
    detect_result_group_t detection_results; // 目标检测结果组
    bool success = false;           // 处理是否成功的标志位
    std::string error_msg;          // 处理失败时的错误信息
} ProcessResult;

/**
 * @brief 任务结构体
 * @details 存储待处理的单帧任务数据，包含图像、硬件帧及结果回传的Promise
 */
typedef struct Task {
    int index;                      // 任务索引（帧序号）
    cv::Mat img;                    // 待处理的原始图像数据
    MppFrame mpp_frame = NULL;      // 硬件解码帧（无需外部申请DMA内存）
    std::promise<ProcessResult> promise; // 用于异步回传处理结果的Promise对象 promise篮子，回传结果
} Task;

/**
 * @brief 线程池类
 * @details 实现基于多线程的图像处理任务调度，封装YOLO模型实例与线程管理逻辑
 */
class ThreadPool
{
public:
    /**
     * @brief 构造函数
     * @param model_path YOLO模型文件路径
     * @param num_threads 线程池初始化的线程数量
     */
    ThreadPool(const char *model_path, int num_threads);

    /**
     * @brief 析构函数
     * @details 负责释放线程池资源，停止所有工作线程
     */
    ~ThreadPool();

    /**
     * @brief 异步提交处理任务
     * @param index 任务索引（帧序号）
     * @param img 待处理的原始图像
     * @param mpp_frame 硬件解码帧
     * @return std::future<ProcessResult> 可获取处理结果的Future对象
     */
    std::future<ProcessResult> submit_task_async(int index, const cv::Mat& img, MppFrame mpp_frame);

private:
    // YOLO模型实例组（多线程各自持有独立实例避免竞争）
    vector<shared_ptr<Yolov5s>> yolo_group;

    /**
     * @brief 线程池初始化函数
     * @param model_path YOLO模型文件路径
     * @param num_threads 初始化的线程数量
     * @return 0表示初始化成功，非0表示失败
     */
    int init(const char *model_path, int num_threads);

    std::queue<Task> tasks;         // 待处理任务队列（存储Task结构体）
    mutex task_mutx;                // 任务队列的互斥锁（保护队列操作线程安全）
    condition_variable task_cond;   // 任务条件变量（用于线程间任务通知）

    std::vector<thread> threads;    // 工作线程容器
    bool run_flag;                  // 线程池运行标志（控制工作线程启停）

    /**
     * @brief 工作线程处理函数
     * @details 每个工作线程的主循环，持续从任务队列取任务并执行处理
     * @param id 工作线程的唯一标识ID
     */
    void worker(int id); 
};

#endif 