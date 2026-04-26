#include <iostream> // 标准输入输出流库
#include <opencv2/core/core.hpp> // OpenCV核心功能库
#include <opencv2/highgui/highgui.hpp> // OpenCV高级功能库，如视频处理

#include <thread> // C++11线程库
#include <unistd.h> // Unix标准函数定义，如sleep函数
#include <queue> // 标准队列库
#include <mutex> // 互斥锁库

#include "SafeQueue.h" // 自定义的安全队列头文件
using namespace std;
using namespace cv; // 使用cv命名空间，简化代码

// 线程0的函数，模拟一个周期性的任务
void thread0_function(int num)
{
    while(true)
    {
        printf("约会A, thread %d\r\n", num); // 打印信息
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // 休眠1秒
    }
}

// 线程1的函数，模拟另一个周期性的任务
void thread1_function(int num)
{
    while(true)
    {
        printf("约会B, thread %d\r\n", num); // 打印信息
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // 休眠1秒
    }
}

// 线程2的函数，模拟第三个周期性的任务
void thread2_function(int num)
{
    while(true)
    {
        printf("约会C, thread %d\r\n", num); // 打印信息
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // 休眠1秒
    }
}

// 线程0的另一个版本，包含互斥锁的使用
void thread0_function(int num, mutex& m1, mutex&m2)
{
    while(true)
    {
        printf("约会A, thread %d\r\n", num); // 打印信息
        {
            lock_guard<mutex> lock(m1); // 锁定互斥锁m1
            sleep(3); // 休眠3秒
            {
                lock_guard<mutex> lock(m2); // 锁定互斥锁m2
                sleep(2); // 休眠2秒
            }
        }
        
    }
}

// 线程1的另一个版本，也包含互斥锁的使用
void thread1_function(int num,mutex& m1, mutex&m2)
{
    while(true)
    {
        printf("约会B, thread %d\r\n", num); // 打印信息
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // 休眠1秒
        {
            lock_guard<mutex> lock(m1); // 锁定互斥锁m1
            sleep(4); // 休眠4秒
            {
                lock_guard<mutex> lock(m2); // 锁定互斥锁m2
                sleep(4); // 休眠4秒
            }
        }
        
    }
}


// 定义帧数据结构
struct  FrameData
{
    cv::Mat frame; // OpenCV的Mat对象，用于存储图像
    int index;      // 帧索引
};

// 定义安全队列，用于线程间通信
SafeQueue<FrameData> SafeQueueRead;

// 读取视频的线程函数
void Thread_ReadVideo(VideoCapture& cap, SafeQueue<FrameData>& img_queue, int& img_index, mutex& cap_mutex, bool& finished)
{
    while(true)
    {
        FrameData frame_temp;
        {
            std::lock_guard<mutex> lock(cap_mutex); // 锁定互斥锁cap_mutex
            if(!cap.read(frame_temp.frame)) // 读取视频帧
            {
                break;
            }
        }
        
        // 读取到图片
        img_index++;
        frame_temp.index = img_index;
        img_queue.enqueue(frame_temp); // 将帧数据加入队列
    }
    finished = true; // 设置读取完成标志
    printf("read end!\r\n"); // 打印读取结束信息
}

// 写入视频的线程函数
void Thread_WriteVideo(VideoWriter& writer, SafeQueue<FrameData>& img_queue, bool& finished)
{
    Mat img_temp;
    FrameData frame_temp;
    auto start = std::chrono::high_resolution_clock::now(); // 记录开始时间
    while(true)
    {
        auto end = std::chrono::high_resolution_clock::now(); // 记录当前时间
        auto duration = std::chrono::duration_cast<chrono::milliseconds>(end - start); // 计算持续时间
        if(!img_queue.empty()) // 如果队列不为空
        {
            if(duration.count() > 30) // 如果持续时间超过30毫秒
            {
                img_queue.dequeue(frame_temp); // 从队列中取出帧数据
                img_temp = frame_temp.frame; // 获取帧图像
                if(!img_temp.empty()) // 如果图像不为空
                {
                    start = std::chrono::high_resolution_clock::now(); // 重置开始时间
                    end =  std::chrono::high_resolution_clock::now(); // 记录当前时间
                    writer.write(img_temp); // 写入帧到视频文件
                }
            }
        }  
        else if(finished) // 如果读取完成
        {
            printf("write end\r\n"); // 打印写入结束信息
            break;
        }

    }
}


int main(void)
{
    // //1、测试图像
    // char img_path[] = "cv.jpg";
    // Mat img0, img1, img2;
    // //读取原图
    // img0 = imread(img_path, IMREAD_UNCHANGED);
    // if(img0.empty())
    // {
    //     waitKey(6000);
    //     return -1;
    // }
    // //包含亮度信息，没有颜色信息
    // img1 = imread(img_path, IMREAD_GRAYSCALE);
    // if(img1.empty())
    // {
    //     waitKey(6000);
    //     return -1;
    // }
    // //加载这个图象保留其颜色信息
    // img2 = imread(img_path, IMREAD_COLOR);
    // if(img2.empty())
    // {
    //     waitKey(6000);
    //     return -1;
    // }

    // // imshow("img0", img0);
    // // imshow("img1", img1);
    // // imshow("img2", img2);

    // waitKey();

    // imwrite("img0.png", img0);
    // imwrite("img1.png", img1);
    // imwrite("img2.png", img2);

    //cout << "Hello World!" <<endl;
    //2、测试视频
    // 测试视频
    char video_path[] = "/home/orangepi/work/lesson/chapter1/test.mp4"; // 视频文件路径
    VideoCapture cap(video_path); // 创建视频捕获对象
    if(!cap.isOpened()) // 检查视频是否成功打开
    {
        printf("video open failed\r\n"); // 打印打开失败信息
        return -1;
    }
    int width = cap.get(CAP_PROP_FRAME_WIDTH); // 获取视频宽度
    int height = cap.get(CAP_PROP_FRAME_HEIGHT); // 获取视频高度
    double fps = cap.get(CAP_PROP_FPS); // 获取视频帧率
    int frame_num = cap.get(CAP_PROP_FRAME_COUNT); // 获取视频总帧数

    printf("Video size: %d x %d, fps: %f, total frame = %d\n", width, height, fps, frame_num); // 打印视频信息

    mutex m1; // 创建互斥锁m1
    mutex m2; // 创建互斥锁m2

    std::thread thread0(thread0_function, 0, ref(m1), ref(m2)); // 创建线程0
    std::thread thread1(thread1_function, 1, ref(m1), ref(m2)); // 创建线程1
    std::thread thread2(thread2_function, 2); // 创建线程2，不使用互斥锁
    
    while(1)
    {
        printf("约会main\r\n"); // 打印主线程信息
        sleep(1); // 休眠1秒
    }
    
    //free() // 释放资源（注释掉了，因为C++会自动管理资源）
    thread0.join(); // 等待线程0完成
    thread1.join(); // 等待线程1完成
    thread2.join(); // 等待线程2完成

    int index = -1; // 初始化帧索引
    mutex cap_m; // 创建视频捕获互斥锁

    bool finished; // 初始化完成标志

    int numThread = 3; // 线程数量
    std::vector<thread> video_readers; // 线程容器
    for(int i = 0;i < numThread;i ++)
    {
        video_readers.emplace_back(Thread_ReadVideo, ref(cap), ref(SafeQueueRead), ref(index), ref(cap_m),ref(finished)); // 创建视频读取线程
    }

    cv::Size framesize(width, height); // 创建视频帧尺寸
    cv::VideoWriter writer("/home/orangepi/work/lesson/chapter1/output.avi", cv::VideoWriter::fourcc('I', '4', '2', '0'), fps, framesize); // 创建视频写入对象

    thread video_writer(Thread_WriteVideo, ref(writer), ref(SafeQueueRead), ref(finished)); // 创建视频写入线程

    for(thread&t : video_readers) // 等待所有视频读取线程完成
    {
        t.join();
    }

    video_writer.join(); // 等待视频写入线程完成

    return 0;
}