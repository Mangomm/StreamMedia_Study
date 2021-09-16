#include "commonlooper.h"
#include "dlog.h"

/**
 * @brief 线程回调函数，内部调用真正的线程回调函数。
 * @param p 回调函数参数。
 * @return no mean.
 */
void* CommonLooper::trampoline(void *p)
{
    LogInfo("into");
    ((CommonLooper*)p)->SetRunning(true);
    ((CommonLooper*)p)->Loop();
     ((CommonLooper*)p)->SetRunning(false);
    LogInfo("leave");

    return NULL;
}

CommonLooper::CommonLooper():
    request_abort_(false),
    running_(false)
{

}

CommonLooper::~CommonLooper()
{
    Stop();
}

/**
 * @brief 创建一个线程并启动。
 * @return success 0 fail -1.
 */
RET_CODE CommonLooper::Start()
{
    LogInfo("into");
    worker_ = new std::thread(trampoline, this);
    if(!worker_) {
        LogError("new std::this_thread failed");
        return RET_FAIL;
    }

    return RET_OK;
}

/**
 * @brief 使用join停止一个线程。
 * @return void.
 */
void CommonLooper::Stop()
{
    request_abort_ = true;
    //running_ = false;     // running_统一在trampoline管理即可。
    if(worker_) {
        worker_->join();
        delete worker_;
        worker_ = NULL;
    }
}

bool CommonLooper::Running()
{
    return running_;
}

void CommonLooper::SetRunning(bool running)
{
    running_ = running;
}


