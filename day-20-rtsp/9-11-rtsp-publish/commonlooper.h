#ifndef COMMONLOOPER_H
#define COMMONLOOPER_H

#include <thread>
#include "mediabase.h"

class CommonLooper
{
public:
    CommonLooper();
    virtual  ~CommonLooper();
    virtual RET_CODE Start();               // 开启线程
    virtual void Stop();                    // 停止线程
    virtual bool Running();                 // 获取线程是否在运行
    virtual void SetRunning(bool running);  // 设置线程状态
    virtual void Loop() = 0;                // 由派生实现的函数，真正的回调函数
private:
    static void *trampoline(void *p);       // thread的回调函数，作为中转，内部调用Loop。
protected:
    std::thread *worker_ = NULL;            // 线程
    bool request_abort_ = false;            // 请求退出线程的标志，目前并未使用
    bool running_ = false;                  // 线程是否在运行

};

#endif // COMMONLOOPER_H
