#include <iostream>
#include <opencv2/core/core.hpp> 
#include <opencv2/highgui/highgui.hpp>

#include <thread>
#include <unistd.h>
#include <queue>
#include <mutex>

#include <map>

#include "SafeQueue.h"
// #include "thread_pool.h"
// #include "yolov5s.h"

using namespace std;
using namespace cv;

struct  FrameData
{
    cv::Mat frame;
    int index;      //帧索引
};


// 创建一个线程池，最大线程数为12
// ThreadPool gthreadpool(12);

// 定义一个静态变量，用于记录帧的起始ID
static int g_frame_start_id = 0;

// 定义两个线程安全的队列，用于在线程间传递FrameData对象
SafeQueue<FrameData> SafeQueueRead;
SafeQueue<FrameData> SafeQueueWrite;

// 定义一个读取视频帧的线程函数
void Thread_ReadVideo(VideoCapture& cap, SafeQueue<FrameData>& img_queue, int& img_index, mutex& cap_mutex, bool& finished)
{
    // 无限循环，直到读取视频结束
    while(true)
    {
        FrameData frame_temp; // 创建一个FrameData对象用于存储帧数据
        {
            // 使用互斥锁保护视频捕获对象，确保线程安全
            std::lock_guard<mutex> lock(cap_mutex);
            // 尝试读取一帧视频
            if(!cap.read(frame_temp.frame))
            {
                break; // 如果读取失败，则退出循环
            }
        }
        
        // 读取到图片后，增加帧索引
        img_index++;
        frame_temp.index = img_index;
        // 将帧数据加入到队列中
        img_queue.enqueue(frame_temp);
    }
    // 设置读取结束标志
    finished = true;
    // 打印读取结束的信息
    printf("read end!\r\n");
}

// 定义一个用于处理帧的缓冲区，使用互斥锁保护
std::map<int, cv::Mat> ProcessFrameBuffer;
mutex bufferMutex;

// 定义一个处理视频帧的线程函数
void Thread_ProcessVideo(SafeQueue<FrameData>& r_queue, SafeQueue<FrameData>& w_queue, bool& finished)
{
    FrameData frame_temp;
    int next_index;
    // 无限循环，直到处理结束
    while(true)
    {
        // 如果读取队列不为空，则取出一帧进行处理
        if(!r_queue.empty())
        {
            r_queue.dequeue(frame_temp);
            {
                // 使用互斥锁保护帧缓冲区
                lock_guard<mutex> lock(bufferMutex);
                // 将读取到的帧数据加入到帧缓冲区
                ProcessFrameBuffer[frame_temp.index] = frame_temp.frame.clone();
            }
            
            // 处理帧缓冲区中的帧
            while(!ProcessFrameBuffer.empty() && ProcessFrameBuffer.count(next_index))
            {
                cv::Mat img;
                auto it = ProcessFrameBuffer.find(next_index);
                if(it != ProcessFrameBuffer.end())
                {
                    img = it->second;

                    // 这里应该是处理图像的代码，但目前是空的（待完成）
                    // gthreadpool.submit_task(img.clone(), g_frame_start_id++);

                    // gthreadpool.get_result(img, next_index);
                    //---------------

                    // 将处理后的帧数据加入到写入队列
                    w_queue.enqueue({img, next_index});
                    // 从帧缓冲区移除处理过的帧
                    ProcessFrameBuffer.erase(it);
                    // 每处理100帧打印一次信息
                    if(next_index % 100 == 0)
                    {
                        printf("process index %d finished!\r\n", next_index);
                    }
                    next_index++;
                }
            }
        }
        else if(finished && r_queue.empty())
        {
            // 如果读取结束且读取队列为空，则打印处理结束信息并退出循环
            printf("process end!\r\n");
            break;
        }
        else
        {
            // 如果读取队列为空，则等待一段时间
            this_thread::sleep_for(chrono::milliseconds(5));
        }
    }
}

// 定义一个写入视频帧的线程函数
void Thread_WriteVideo(VideoWriter& writer, SafeQueue<FrameData>& img_queue, bool& finished)
{
    Mat img_temp;
    FrameData frame_temp;
    auto start = std::chrono::high_resolution_clock::now();
    // 无限循环，直到写入结束
    while(true)
    {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<chrono::milliseconds>(end - start);
        // 如果写入队列不为空且时间间隔超过30毫秒，则取出一帧进行写入
        if(!img_queue.empty())
        {
            if(duration.count() > 30)
            {
                img_queue.dequeue(frame_temp);
                img_temp = frame_temp.frame;
                if(!img_temp.empty())
                {
                    start = std::chrono::high_resolution_clock::now();
                    end =  std::chrono::high_resolution_clock::now();
                    writer.write(img_temp);
                }
                // 每写入100帧打印一次信息
                if(frame_temp.index % 100 == 0)
                {
                    printf("write index %d finished!\r\n", frame_temp.index);
                }

            }
        }  
        else if(finished)
        {
            // 如果写入结束标志被设置，则打印写入结束信息并退出循环
            printf("write end\r\n");
            break;

        }

    }
}

int main(void)
{
    char video_path[] = "/home/radxa/chapter1/test.mp4";
    VideoCapture cap(video_path);
    if(!cap.isOpened())
    {
        printf("video open failed\r\n");
        return -1;
    }
    int width = cap.get(CAP_PROP_FRAME_WIDTH);
    int height = cap.get(CAP_PROP_FRAME_HEIGHT);
    double fps = cap.get(CAP_PROP_FPS);
    int frame_num = cap.get(CAP_PROP_FRAME_COUNT);

    printf("Video size: %d x %d, fps: %f, total frame = %d\n", width, height, fps, frame_num); // 打印视频尺寸和帧率

    //测试图像
    // char img_name[] = "/home/radxa/chapter1/cv.jpg";
    // cv::Mat img_temp = cv::imread(img_name,IMREAD_COLOR);
    //测试视频的第一帧
    // Mat img_temp;
    // cap.read(img_temp);
    // if(img_temp.empty())
    // {
    //     printf("read video empty.\n");
    // }
    // Yolov5s yolov5s("/home/orangepi/work/lesson/chapter3/model/yolov5s.rknn", 1);
    // yolov5s.inference_image(img_temp);
    // while(1);
    
    int index = -1;
    mutex cap_m;
    bool finished;

    int numThread = 3;
    std::vector<thread> video_readers;
    for(int i = 0;i < numThread;i ++)
    {
        video_readers.emplace_back(Thread_ReadVideo, ref(cap), ref(SafeQueueRead), ref(index), ref(cap_m),ref(finished));
    }
    
    cv::Size framesize(width, height);
    cv::VideoWriter writer("/home/radxa/chapter1/output.avi", cv::VideoWriter::fourcc('I', '4', '2', '0'), fps, framesize);
    thread video_process(Thread_ProcessVideo, ref(SafeQueueRead), ref(SafeQueueWrite), ref(finished));
    thread video_writer(Thread_WriteVideo, ref(writer), ref(SafeQueueWrite), ref(finished));

    for(thread&t : video_readers)
    {
        t.join();
    }

    video_process.join();
    video_writer.join();

    return 0;
}