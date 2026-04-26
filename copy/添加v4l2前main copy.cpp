#include <iostream>
#include <opencv2/core/core.hpp> // OpenCV核心功能库
#include <opencv2/highgui/highgui.hpp> // OpenCV高级功能库，如视频处理

#include <thread>
#include <unistd.h>
#include <queue>
#include <mutex>
#include "SafeQueue.h"
#include "yolov5s.h"
#include <map>
#include "post_process.h"
#include "thread_pool.h"
using namespace std;
using namespace cv;

/*FrameData 结构体的本质是将 “图像数据” 和 “帧序号” 绑定为一个整体*/
struct FrameData
{
   cv::Mat frame; //OpenCV的Mat对象，用于存储图像
   int index;//帧索引 
};

#define model_path "/home/radxa/chapter1/model/yolov5s.rknn"

ThreadPool gthreadpool(model_path,12);//创建一个线程池，最大线程数为12
static int g_frame_start_id = 0;// 定义一个静态变量，用于记录帧的起始ID

// queue <FrameData> ReadFrameQueue;
SafeQueue<FrameData> SafeQueueRead(30);//安全队列，用于线程间通信
SafeQueue<FrameData> SafeQueueProcess(30);

//读取视频的线程函数
void Thread_ReadVideo(VideoCapture& cap,SafeQueue<FrameData>& img_queue,int& img_index,mutex& cap_mutex,bool& finished)//取地址符的作用&、 表示引用（reference)1. 避免对象的拷贝，提高效率2. 保证函数内外操作的是同一个对象3. 确保资源的正确管理
{
   while(true)
   {
      //定义一个 FrameData 类型的临时变量 frame_temp，用于暂存一帧视频的完整数据（图像帧 + 索引）
      FrameData frame_temp;
      {  
         std::lock_guard<mutex> lock(cap_mutex); //锁定互斥锁cap_mutex
         //从 VideoCapture 读取一帧图像到 frame_temp.frame；
         if(!cap.read(frame_temp.frame))//如果没有帧读取了，返回false，取！得真，break
         {
            printf("read %d frame.\n", img_index);
            printf("read end!\r\n");
            finished = true;
            break; // 如果读取失败，则退出循环
         }
      }
      //读取到图片
      img_index++;//更新帧索引 
      frame_temp.index = img_index;//为该帧分配索引
      img_queue.enqueue(frame_temp); //将帧数据加入队列
      if(img_index % 100 == 0)
      printf("read index %d finished!\r\n", img_index);
   }
}

std::map<int,cv::Mat> ProcessFrameBuffer;
mutex bufferMutex;

// 定义一个处理视频帧的线程函数
void Thread_ProcessVideo(SafeQueue<FrameData>& r_queue, SafeQueue<FrameData>& w_queue, bool& r_finished, bool& p_finished)
{
    FrameData frame_temp;
    int next_index;
    printf("next_index = %d\n", next_index);
    // 无限循环，直到处理结束
    while(true)
    {
        // 如果读取队列不为空，则取出一帧进行处理
        if(!r_queue.empty())
        {
            r_queue.dequeue(frame_temp);
            {
                // 使用互斥锁保护帧缓冲区,这里使用{}时buffermutex的作用域
                lock_guard<mutex> lock(bufferMutex);
                // 将读取到的帧数据加入到帧缓冲区
                ProcessFrameBuffer[frame_temp.index] = frame_temp.frame.clone();//map类数组
            }
            
            // 处理帧缓冲区中的帧
            while(!ProcessFrameBuffer.empty() && ProcessFrameBuffer.count(next_index))
            {   
                //count判断是否存在，find查找并返回
                cv::Mat img;
                auto it = ProcessFrameBuffer.find(next_index);
                if(it != ProcessFrameBuffer.end())
                {
                    img = it->second;

                    // 这里应该是处理图像的代码，但目前是空的（待完成）
                    gthreadpool.submit_task(img.clone(), g_frame_start_id++);
                    gthreadpool.get_result(img, next_index);
                    //---------------

                    // 将处理后的帧数据加入到写入队列
                    w_queue.enqueue({img, next_index});

                    img.release();

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
        else if(r_finished && r_queue.empty())
        {
            // 如果读取结束且读取队列为空，则打印处理结束信息并退出循环
            printf("process end!\r\n");
            p_finished = true;
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
void Thread_WriteVideo(VideoWriter& writer, SafeQueue<FrameData>& img_queue, bool& p_finished)
{
    Mat img_temp;
    FrameData frame_temp;
    auto start = std::chrono::high_resolution_clock::now();
    // 无限循环，直到写入结束
    while(true)
    {

        // 如果写入队列不为空且时间间隔超过30毫秒，则取出一帧进行写入
        if(!img_queue.empty())
        {
            auto end = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<chrono::milliseconds>(end - start);
            if(duration.count() > 30)//why30，怎么判断，仅用于控制视频写入的帧率（避免写入过快导致视频播放加速），不影响帧的顺序：
            {
                img_queue.dequeue(frame_temp);
                img_temp = frame_temp.frame;
                if(!img_temp.empty())
                {
                    start   = std::chrono::high_resolution_clock::now();
                    end     =  std::chrono::high_resolution_clock::now();
                    writer.write(img_temp);
                }
                // 每写入100帧打印一次信息
                if(frame_temp.index % 100 == 0)
                {
                    printf("write index %d finished!\r\n", frame_temp.index);
                }

            }
        }  
        else if(p_finished)
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

cv::VideoCapture cap(video_path);//是 OpenCV 库中用于读取视频流或捕获摄像头数据的核心类，视频输入

if(!cap.isOpened())
{
   printf("video open failed\r\n");
   return -1;
}

int width = cap.get(CAP_PROP_FRAME_WIDTH);
int heigth = cap.get(CAP_PROP_FRAME_HEIGHT);
double fps = cap.get(CAP_PROP_FPS);
int frame_num = cap.get(CAP_PROP_FRAME_COUNT);

//打印视频尺寸和帧率
printf("Video size:%d x %d, fps: %f, total frame = %d\n",width,heigth,fps,frame_num);

   int index = -1; //初始化帧索引,视频从第零帧开始
   int numThread = 1; //线程数量
   mutex cap_m; //初始化视频捕获互斥锁，读取

   bool read_finished = false; //初始化 完成标志
   bool process_finished = false;

   std::vector<thread> video_reader; //线程容器
   for(int i =0;i < numThread;i ++)
   {  
      video_reader.emplace_back(Thread_ReadVideo,ref(cap),ref(SafeQueueRead),ref(index),ref(cap_m),ref(read_finished)); //创建视频读取线程
   }
   cv::Size framesize(width,heigth); //创建视频帧尺寸

   cv::VideoWriter writer("/home/radxa/chapter1/output.avi",cv::VideoWriter::fourcc('I','4','2','0'),fps,framesize);//与这个cv::VideoCapture cap(video_path);对应

   std::thread video_process(Thread_ProcessVideo,ref(SafeQueueRead),ref(SafeQueueProcess),ref(read_finished),ref(process_finished));
   std::thread video_writer(Thread_WriteVideo,ref(writer),ref(SafeQueueProcess),ref(process_finished)); //创建视频写入线程
   for(thread&t : video_reader) //等待所有视频读取线程完成
   {
      t.join();//free thread0.jion();
   }
   video_process.join();//等待视频处理线程完成
   video_writer.join();//等待视频写入线程完成
   // 释放视频写入对象和视频捕获对象
    writer.release();
    cap.release();
    printf("code end!\n");
 return 0;   
} 
//pkg-config --cflags(编译阶段指定头文件的路径) --libs opencv4