#ifndef _SAFEQUEUE_H_
#define _SAFEQUEUE_H_

 #include <iostream>//标准输入输出库
 #include <queue>//标准队列库

 #include <mutex>//互斥锁库
 #include <condition_variable>//条件变量库

 using namespace std;//使用标准命名空间

 //定义一个模板类SafeQueue
 template<typename T>
 class SafeQueue{
    public:
         //SafeQueue(){};//构造函数
         SafeQueue(size_t maxSize_in): maxSize(maxSize_in){};
         ~SafeQueue(){};//析构函数

   //将变量t插入队列
   void enqueue(const T& t) //T是一个占位符，可以代表任意数据类型
   {
      //std::lock_guard <mutex> lock(m); 条件要配合unique_lock使用
      std::unique_lock<mutex> lock(m);
      cond_not_full.wait(lock,[this]{return q.size() < maxSize;});//若条件满足，则执行后续逻辑
      
      q.push(t); //将元素t压入队列q
      cond_not_empty.notify_all(); //通知一个等待的线程，通常用于唤醒在等待队列非空的线程（比如出队操作的线程）
   }

   //从队列中移除元素，并将其赋值给t
   bool dequeue(T& t)
   {
      std::unique_lock<mutex> lock(m);
      cond_not_empty.wait(lock,[this]{return !q.empty();});//等待条件变量c，直到队列不为空
      if(stop_flag && q.empty()) {
            // 若收到停止信号且队列也空了，就返回 false
            return false;
        }
      t = q.front();//将队列前端元素赋值给t
      q.pop();//将队列前端元素出队
      cond_not_full.notify_all();
      return true;
   }
   // 调用此函数让所有阻塞线程退出
    void stop() {
        std::unique_lock<std::mutex> lock(m);
        stop_flag = true;
        cond_not_empty.notify_all();
        cond_not_full.notify_all();
    }
   //检查队列是否为空
   bool empty()
   {
      std::lock_guard<mutex> lock(m);//使用lock_guard自动锁定互斥锁m
      return q.empty();//返回队列是否为空的状态
   }
   // 返回队列当前元素数量
    size_t size()
    {
        unique_lock<mutex> lock(m);
        return q.size();
    }
    private:
    bool stop_flag = false;
    queue<T> q;//队列，是private，enqueue和dequeue都能“看”的到的
    mutable mutex m;//互斥锁，用于同步访问队列
    
   std::condition_variable cond_not_empty;
   std::condition_variable cond_not_full;
    size_t maxSize;
 };

#endif 