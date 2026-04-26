#include <iostream>
#include <opencv2/core/core.hpp> // OpenCV核心功能库
#include <opencv2/highgui/highgui.hpp> // OpenCV高级功能库，如视频处理
#include <thread>
#include <unistd.h>
#include <queue>
#include <mutex>
using namespace std;
using namespace cv;
//线程0的函数，模拟一个周期性的任务
// void thread0_function(int num)
// {
//    while(true)
//    {
//       printf("hello from %d\r\n",num);
//       std::this_thread::sleep_for(std::chrono::milliseconds(1000));//休眠1s
//    }
// }

//线程1的函数，模仿另一个周期性任务
// void thread1_function(int num)
// {
//    while(true)
//    {
//       printf("%d",num);
//       std::this_thread::sleep_for(std::chrono::milliseconds(1000));
//    }
// }
// //死锁问题举例，t1使用m1 3s，t2使用m2 4s，t1 3s后 使用m2 结果被迫挂起，然后t2 4s后 使用m1，出现死锁
// //线程0的锁版本
// void thread0_lock_function(int num,mutex& m1,mutex& m2)
// {
//    while (true)
//    {
//       printf("%d",num);
//       {
//       lock_guard<mutex> lock(m1);//锁定互斥锁m1
//       sleep(3);
//       {
//          lock_guard<mutex> lock(m2);
//          sleep(2);
//       }
//       }
//    }
// }
// //线程1的锁版本
// void thread1_lock_function(int num,mutex& m1,mutex& m2)
// {
//    while (true)
//    {
//       printf("%d",num);
//       {
//       lock_guard<mutex> lock(m1);//锁定互斥锁m1
//       sleep(4);
//       {
//          lock_guard<mutex> lock(m2);
//          sleep(4);
//       }
//       }
//    }
// }

/*FrameData 结构体的本质是将 “图像数据” 和 “帧序号” 绑定为一个整体
cv::Mat 是 OpenCV 中用于存储图像数据的核心类（矩阵容器），可表示任意尺寸、任意通道的图像（如 RGB 彩色图、灰度图等）。这里的 frame 成员用于存储一帧视频的像素数据（即图像本身）。
*/
struct FrameData
{
   //存储单帧数据，包含cv::Mat frame（opencv图像矩阵），index帧的序号
   cv::Mat frame;
   int index;//帧索引 
};

//队列，用于缓存读取到的帧数据
queue <FrameData> ReadFrameQueue;

/*取地址符的作用
&、 表示引用（reference)
1. 避免对象的拷贝，提高效率
cv::VideoCapture 是一个封装了视频源（文件、摄像头等）的复杂对象，内部包含硬件资源句柄、缓冲区、状态信息等。若按默认的值传递（即不加 &），函数会创建一个 VideoCapture 对象的完整副本：
2. 保证函数内外操作的是同一个对象
// 错误：值传递，函数内操作的是副本
void readVideo(cv::VideoCapture cap) {
    cap.read(frame);  // 操作的是副本，外部原对象的状态不变
}
// 正确：引用传递，操作原对象
void readVideo(cv::VideoCapture& cap) {
    cap.read(frame);  // 直接操作外部传入的 cap，状态会同步更新
}
3. 确保资源的正确管理
VideoCapture 关联的视频源（如摄像头、文件句柄）是稀缺资源，系统通常限制同一资源被多个对象同时占用。若通过值传递拷贝 VideoCapture 对象，可能导致：
资源被重复释放（析构时多个对象尝试释放同一资源，引发程序崩溃）；
资源冲突（如两个对象同时操作同一摄像头，导致读取异常）。
引用传递避免了对象拷贝，确保整个程序中只有一个 VideoCapture 对象管理该资源，符合资源独占的原则。
*/

void Thread_ReadVideo(VideoCapture& cap,queue<FrameData>& img_queue,int& img_index,mutex& cap_mutex,mutex& queue_mutex,bool& finished)
{
   while(true)
   {
      //定义一个 FrameData 类型的临时变量 frame_temp，用于暂存一帧视频的完整数据（图像帧 + 索引）
      FrameData frame_temp;
      {  std::lock_guard<mutex> lock(cap_mutex); 
         //从 VideoCapture 读取一帧图像到 frame_temp.frame；
         if(!cap.read(frame_temp.frame))
         {
            break;
         }
      }
      img_index++;//读取到图片，更新帧索引
      
      frame_temp.index = img_index;//为该帧分配索引
      {
      std::lock_guard<mutex> lock(queue_mutex);
      img_queue.push(frame_temp);//将封装好的 frame_temp 存入队列
      }
   }
   finished = true;
   printf("read end!\r\n");
}
void Thread_WriteVideo(VideoWriter& writer, queue<FrameData>& img_queue, bool& finished)
{
   Mat img_temp;
   FrameData frame_temp;
   while (true)
   {
      if(!img_queue.empty())
      {
         frame_temp = img_queue.front();
         img_queue.pop();
         img_temp = frame_temp.frame;
         if(!img_temp.empty())
         {
            writer.write(img_temp);
         }
      }
      else if(finished)
      {
         printf("write end!\r\n");
         break;
      }
   }
}

int main(void)
{
//1.测试图像
//  char img_path[] = "/home/radxa/chapter1/cv.jpg";
//  cv::Mat img0,img1,img2; // int i; //cv:: 
//  if(img0.empty())
//  {
//     waitKey(6000);
//     return -1;
//  }
//  img0 = cv::imread(img_path,IMREAD_UNCHANGED); //读取原图
//  img1 = imread(img_path,IMREAD_GRAYSCALE);//包含亮度信息，没有颜色信息
//  img2 = imread(img_path, IMREAD_COLOR);// 加载这个图象保留其颜色信息
// //  imshow("img0",img0);//创建一个标题为img0的窗口，并在窗口显示img0对应的图像
//  waitKey(); 
// imwrite("img1.png",img1);
// std::cout << "hellop0" <<std::endl;

//2.测试视频,定义
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

// std::thread thread0(thread0_function,0);
// while (1)
// {
//    printf("hello from main\r\n");
//    sleep(1);
// }

   int index = -1;
   int numThread = 1;
   mutex cap_m;
   mutex queue_m;
   bool finished;
//动态数组容器，具有动态扩容，随机访问，连续内存存储的特性
// int i_arr[20]; 
// std::vector<int> i_arr;
// i_arr.empty();
   std::vector<thread> video_reader;
   for(int i =0;i < numThread;i ++)
   {  //在容器尾“放置”一个新元素，此处为thread对象;直接在容器中构造元素，减少复制，移动的开销；
      //主要是与push_back的区别
      video_reader.emplace_back(Thread_ReadVideo,ref(cap),ref(ReadFrameQueue),ref(index),ref(cap_m),ref(queue_m),ref(finished));
   }

   cv::Size framesize(width,heigth);

   cv::VideoWriter writer("/home/radxa/chapter1/output.avi",cv::VideoWriter::fourcc('I','4','2','0'),fps,framesize);

   thread video_writer(Thread_WriteVideo,ref(writer),ref(ReadFrameQueue),ref(finished));
   for(thread&t : video_reader)
   {
      t.join();//free thread0.jion();
   }
   video_writer.join();
 return 0;   
} 
//pkg-config --cflags(编译阶段指定头文件的路径) --libs opencv4