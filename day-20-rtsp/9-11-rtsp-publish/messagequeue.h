#ifndef MESSAGEQUEUE_H
#define MESSAGEQUEUE_H

#include <mutex>
#include <condition_variable>
#include <list>
#include "dlog.h"
extern "C"
{
#include "libavcodec/avcodec.h"
}

#define MSG_FLUSH                   1
#define MSG_RTSP_ERROR              100
#define MSG_RTSP_QUEUE_DURATION     101

// 消息处理结构体，类似做法ijkplayer的消息控制
typedef struct AVMessage
{
    int what;                   // 消息类型
    int arg1;
    int arg2;
    void *obj;                  // 如果2个参数不够用，则传入结构体
    void (*free_l)(void *obj);
}AVMessage;
static void msg_obj_free_l(void *obj)
{
    av_free(obj);
}

class MessageQueue
{
public:
    MessageQueue() {}
    ~MessageQueue()
    {
        msg_queue_flush();
    }

    inline void msg_init_msg(AVMessage *msg)
    {
        memset(msg, 0, sizeof(AVMessage));
    }

    /**
     * @brief 对一个消息进行深拷贝后放进队列。 detail see msg_queue_put_private.
     * @param msg 传入的消息。
     * @return success 0, error or abort return -1.
    */
    int msg_queue_put(AVMessage *msg)
    {
        LogInfo("msg_queue_put");
        std::lock_guard<std::mutex> lock(mutex_);
        int ret = msg_queue_put_private(msg);
        if(-1 == ret) {
            return -1;
        }

        // 正常插入队列了才会notify，让消费者去取
        cond_.notify_one();

        return 0;
    }

    /**
     * @brief 获取一个消息。 队列有消息直接返回，没消息依据参数 timeout 去处理。
     * @param msg 消息，传入传出参数。
     * @param timeout -1代表阻塞等待; 0; 代表非阻塞等待; >0 代表有超时的等待; -2 代表参数异常。
     * @return -2 代表未知错误； -1代表abort; 0 代表没有消息;  1代表读取到了消息。
    */
    int msg_queue_get(AVMessage *msg, int timeout)
    {
        // 传入传出的消息为空直接报错。
        if(!msg) {
            return -2;
        }

        // 处理获取消息的过程
        std::unique_lock<std::mutex> lock(mutex_);
        AVMessage *msg1;
        int ret;
        for(;;) {
            // 1 阻塞过程中可能会出现中断请求，所以必须判断
            if(abort_request_) {
                ret = -1;
                break;
            }

            // 2 队列不为空，直接返回消息
            if(!queue_.empty()) {
                msg1= queue_.front();
                *msg = *msg1;
                queue_.pop_front();
                av_free(msg1);      // 因为push时是深拷贝，所以需要释放msg1
                ret = 1;
                break;
            }
            // 3 队列为空且是非阻塞，同样直接返回
            else if(0 == timeout) {
                ret = 0;            // 没有消息
                break;
            }
            // 4 小于0，会一直阻塞，直到队列不为空或者abort请求才退出wait，然后继续回到for循环进行判断。
            else if(timeout < 0){
                cond_.wait(lock, [this] {
                    return !queue_.empty() | abort_request_;
                });
            }
            // 5 大于0，超时返回，没超时会继续回到for循环进行判断。
            else if(timeout > 0) {
                //LogInfo("wait_for into");
                cond_.wait_for(lock, std::chrono::milliseconds(timeout), [this] {
                    //LogInfo("wait_for leave");
                    return !queue_.empty() | abort_request_;        // 直接写return true;是错误的
                });

                // 上面wait_for出来可能存在3种情况：1）队列不为空；2）中断请求；3）因为1）与2）不满足而超时返回。
                // 这里需要判断empty，不能让其回到for中，不然下一次还是会回到timeout > 0的逻辑再次处理，这样是不对的。abort_request_可以放到for中处理。
                // 有人问那超时的处理呢？实际判断empty时就已经处理了，因为超时可能是因为1）与2）条件不满足导致的，所以处理1、2就相当于处理了超时。
                // 或者你可以使用std::cv_status::timeout配合返回值去写，不过没啥作用，只是方便看代码逻辑，而且一般C++的条件变量函数 wait、wait_for都不需要用到返回值。
                if(queue_.empty()) {
                    ret = 0;
                    break;
                }
            }
            else {
                // UNKOWN
                ret = -2;
                break;
            }
        }

        return ret;
    }

    /**
     * @brief 把队列里面what类型的消息全部删除。list.remove每次只能删除一次。
     * @param what 消息类型。
     * @return void。
    */
    void msg_queue_remove(int what)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while(!abort_request_ && !queue_.empty()) {
            std::list<AVMessage *>::iterator it;
            AVMessage *msg = NULL;
            for(it = queue_.begin(); it != queue_.end(); it++) {
                if((*it) && (*it)->what == what) {
                    msg = *it;
                    break;
                }
            }
            if(msg) {
                if(msg->obj && msg->free_l) {
                    msg->free_l(msg->obj);
                }
                av_free(msg);
                queue_.remove(msg); // 每次从队列中删除一个与msg相同值的元素。注意这里是地址值，不过普通值应该也没问题。它不会把msg地址释放，需要用户自行处理该内存，它只会移除队列。
            } else {
                // 空消息不用清掉，因为空消息没有what，说明它不等于参数what，所以不用清掉。看自己需要是否处理掉也可以。
                break;
            }
        }
    }

    /**
     * @brief 自己写的，和msg_queue_remove作用一样，把队列里面what类型的消息全部删除，比上面效率应该要好。
     * @param what 消息类型。
     * @return void。
    */
    void msg_queue_erase(int what)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(abort_request_){         // 1中断，判断一次即可，不用像上面每次都判断，因为已经上锁了，整个函数都是原子操作，本次调用要么是0，要么是1.
            return;
        }
        for(auto it = queue_.begin(); it != queue_.end();){
            auto msg = (*it);
            if(msg && msg->what == what){
                if(msg->obj && msg->free_l){
                    msg->free_l(msg->obj);
                }
                av_free(msg);
                it = queue_.erase(it);
            }else{
                it++;
            }
        }
    }


    /**
     * @brief 只初始化消息类型，并把该消息深拷贝放进队列。
     * @param what 消息类型。
     * @return void。
    */
    void notify_msg1(int what)
    {
        AVMessage msg;
        msg_init_msg(&msg);
        msg.what = what;
        msg_queue_put(&msg);
    }

    /**
     * @brief 初始化消息类型与一个参数，并把该消息深拷贝放进队列。
     * @param what 消息类型。
     * @param arg1 消息参数1。
     * @return void。
    */
    void notify_msg2(int what, int arg1)
    {
        AVMessage msg;
        msg_init_msg(&msg);
        msg.what = what;
        msg.arg1 = arg1;
        msg_queue_put(&msg);
    }

    /**
     * @brief 初始化消息类型与两个参数，并把该消息深拷贝放进队列。
     * @param what 消息类型。
     * @param arg1 消息参数1。
     * @param arg2 消息参数2。
     * @return void。
    */
    void notify_msg3(int what, int arg1, int arg2)
    {
        AVMessage msg;
        msg_init_msg(&msg);
        msg.what = what;
        msg.arg1 = arg1;
        msg.arg2 = arg2;
        msg_queue_put(&msg);
    }

    /**
     * @brief 初始化消息类型与所有参数，并把该消息深拷贝放进队列。
     * @param what 消息类型。
     * @param arg1 消息参数1。
     * @param arg2 消息参数2。
     * @param obj  当arg1、arg2不够用，使用obj传入多个参数。
     * @param obj_len obj_len。
     * @return void。
    */
    void notify_msg4(int what, int arg1, int arg2, void *obj, int obj_len)
    {
        AVMessage msg;
        msg_init_msg(&msg);
        msg.what = what;
        msg.arg1 = arg1;
        msg.arg2 = arg2;
        msg.obj = av_malloc(obj_len);
        msg.free_l = msg_obj_free_l;
        memcpy(msg.obj, obj, obj_len);
        msg_queue_put(&msg);
    }

    /**
     * @brief 置1，表示中断请求。
    */
    void msg_queue_abort()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_request_ = 1;
    }

    /**
     * @brief 从队列头部开始，将队列所有内容清空，即冲刷。
    */
    void msg_queue_flush()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) {
            AVMessage *msg = queue_.front();
            if(msg->obj && msg->free_l) {
                msg->free_l(msg->obj);
            }
            queue_.pop_front();
            av_free(msg);
        }
    }

    /**
     * @brief detail see msg_queue_flush()。
    */
    void msg_queue_destroy(MessageQueue *q)
    {
        msg_queue_flush();
    }

private:

    /**
     * @brief 对一个消息进行深拷贝后放进队列。   参考ffplay， private后缀的都是真正干活的。
     * @param msg 传入的消息。
     * @return success 0, error or abort return -1.
    */
    int msg_queue_put_private(AVMessage *msg)
    {
        // 若中断，则返回
        if(abort_request_) {
            return -1;
        }

        // 深拷贝传入的消息
        AVMessage *msg1 = (AVMessage *)av_malloc(sizeof(AVMessage));
        if(!msg1) {
            return -1;
        }
        *msg1 = *msg;
        queue_.push_back(msg1);

        return 0;
    }

private:
    int abort_request_ = 0;                 /* 是否中断，0不中断，1中断 */
    std::mutex mutex_;                      /* 用于锁住queue_ */
    std::condition_variable cond_;          /* 条件变量 */
    std::list<AVMessage *> queue_;          /* 消息队列，使用list不用vector是因为中间需要插入和删除，所以不使用vec */
};

#endif // MESSAGEQUEUE_H
