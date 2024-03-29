﻿/**
* 本队列是针对于多个输入，一个输出的场景。
* 其实也支持多个输入多个输出的场景，但是多个输出会将音视频包分开，从而无法组成数据帧，视频可能乱码，音频可能会杂音等等。
*/

#ifndef PACKETQUEUE_H
#define PACKETQUEUE_H
#include <mutex>
#include <condition_variable>
#include <queue>
#include "mediabase.h"
#include "dlog.h"

extern "C"
{
#include "libavcodec/avcodec.h"
}

/**
 * 在debug时，查看队列的时长应该这样查看：
 * 1）例如在某一处打了断点，先看video_is_pop_(以视频为例)，若为0，则队尾pts-队头pts+帧时长；若为1则是队尾pts-队头pts。
        如果想判断是否正确，那么需要查看成员stats_的内容，查看对应的包数，乘以帧时长，看看是否与求出的时长基本一致，若差几ms是没问题的，但是差一帧例如40ms
        以上，说明计算还是不太准确。
 * 2）若不想这么麻烦，直接调一下GetStats去计算判断即可。注意必须是依赖传出的参数判断，不能再debug的信息看了，因为此时其它线程可能在操作数据。

 例如自己的测试：
    video_back_pts_ - video_front_pts_ = 4330350 - 4139550 = 190800; 由于单位是90k，所以190800/90000=2.12s=2120ms
    而此时包数为53个，帧时长=40ms，53x40=2120ms
    由此看来，优化之后，运算是没有问题的
*/

// 是否使用自己的求队列时长的代码，已经优化
#define MyDurationCode

// 记录包队列的状态信息
typedef struct packet_queue_stats
{
    int     audio_nb_packets;           // 音频包数量
    int     video_nb_packets;           // 视频包数量
    int     audio_size;                 // 音频总大小 字节
    int     video_size;                 // 视频总大小 字节
    int64_t audio_duration;             // 音频持续时长
    int64_t video_duration;             // 视频持续时长
}PacketQueueStats;

// 自定义封装的AVPacket
typedef struct my_avpacket
{
    AVPacket *pkt;                      // 编码后的packet
    MediaType media_type;               // 包类型，例如视频、音频
}MyAVPacket;

class PacketQueue
{
public:

//    PacketQueue() {
//        if (audio_frame_duration_ < 0) {
//            audio_frame_duration_ = 0;
//        }
//        if (video_frame_duration_ < 0) {
//            video_frame_duration_ = 0;
//        }
//        memset(&stats_, 0, sizeof(PacketQueueStats));
//    };

    // 音频和视频的帧时长需要由外部赋值，否则可能会出现不匹配的问题。
    PacketQueue(double audio_frame_duration, double video_frame_duration)
        : audio_frame_duration_(audio_frame_duration), video_frame_duration_(video_frame_duration)
    {
        if (audio_frame_duration_ < 0) {
            audio_frame_duration_ = 0;
        }
        if (video_frame_duration_ < 0) {
            video_frame_duration_ = 0;
        }
        memset(&stats_, 0, sizeof(PacketQueueStats));
    }
    ~PacketQueue()
    {
        Drop(true, 0);
    }

    /**
    * @brief psuh一个包进队列，浅拷贝，与消息队列的深拷贝不一样。
    * @param pkt 编码后的包。
    * @param media_type 包类型。
    * @return 成功 0 失败 -1
    */
    int Push(AVPacket *pkt, MediaType media_type)
    {
        if (!pkt) {
            //LogError("pkt is null");
            return -1;
        }
        if (media_type != E_AUDIO_TYPE && media_type != E_VIDEO_TYPE) {
            //LogError("media_type: %d is unknown", media_type);
            return -1;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        int ret = pushPrivate(pkt, media_type);
        if (ret < 0) {
            //LogError("pushPrivate failed");
            return -1;
        }
        else {
            cond_.notify_one();
            return 0;
        }
    }

    /**
    * @brief 真正的psuh一个包进队列，浅拷贝。
    * @param pkt 编码后的包。
    * @param media_type 包类型。
    * @return 成功 0 失败 -1
    */
    int pushPrivate(AVPacket *pkt, MediaType media_type)
    {
        if (abort_request_) {
            //LogWarn("abort request");
            return -1;
        }
        // 只开辟管理AVPacket的结构体内存
        MyAVPacket *mypkt = (MyAVPacket *)malloc(sizeof(MyAVPacket));
        if (!mypkt) {
            //LogError("malloc MyAVPacket failed");
            return -1;
        }
        mypkt->pkt = pkt;
        mypkt->media_type = media_type;

        // 记录相关的统计信息
        if (E_AUDIO_TYPE == media_type) {
            stats_.audio_nb_packets++;      // 包数量
            stats_.audio_size += pkt->size;
            // 队列持续时长怎么统计，不是用pkt->duration(ffplay做法)，而是使用队尾的pts-对头的pts
            // 虽然这里audio_back_pts_ - audio_front_pts_时，时长会少了一个包的时长，例如队列只有第一帧此时audio_back_pts_ = audio_front_pts_，
            // 那么duration = audio_back_pts_ - audio_front_pts_ = 0，即有一帧数据但是时长确是0的情况，但是下面在Get时长时，会加上一帧的时长，所以不影响获取队列的时长。视频同理。
            // 即总结上面：这里统计是这样统计，但是下面获取队列信息的时候，会进行修正，返回的队列时长信息仍然是正确的。
            audio_back_pts_ = pkt->pts;
            if (audio_first_packet) {
                audio_first_packet = 0;
                audio_front_pts_ = pkt->pts;
            }
        }
        if (E_VIDEO_TYPE == media_type) {
            stats_.video_nb_packets++;      // 包数量
            stats_.video_size += pkt->size;
            // 队列持续时长怎么统计，不是用pkt->duration，而是使用队尾的pts-对头的pts
            video_back_pts_ = pkt->pts;
            if (video_first_packet) {
                video_first_packet = 0;
                video_front_pts_ = pkt->pts;
            }
            //LogInfo("video_back_pts_: %lld, video_front_pts_: %lld, stats_.video_nb_packets: %d",
            //video_back_pts_, video_front_pts_, stats_.video_nb_packets);
        }

        // 一定要push
        queue_.push(mypkt);
        return 0;
    }

    /**
    * @brief 从队列取出一个包。
    * @param pkt 传入传出，取出一个包。
    * @param media_type 取出的包类型。
    * @return -1 abort; 1 获取到消息。
    */
    int Pop(AVPacket **pkt, MediaType &media_type)
    {
        if (!pkt) {// 不能或上!(*pkt)，因为*pkt传进来就是个空的，它是一个传出参数
            //LogError("pkt is null");
            return -1;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (abort_request_) {
            //LogWarn("abort request");
            return -1;
        }

        if (queue_.empty()) {
            // return如果返回false，继续wait, 如果返回true退出wait
            cond_.wait(lock, [this] {
                return !queue_.empty() | abort_request_;
            });
        }

        // wiat带参2，上面流程出来后，队列可能是空或者不是空。
        // 当abort_request_=1，不管是空还是非空，必须中断；当abort_request_=0，证明队列必定是非空，所以只需要判断abort_request_即可，可以不再判断队列是否为空。
        // 但是可能是中断唤醒，因为条件变量会释放锁阻塞
        if (abort_request_) {
            //LogWarn("abort request");
            return -1;
        }

        // 下来后，这里必定是队列为非空，且abort_request_=0
        MyAVPacket *mypkt = queue_.front(); // 读取队列首部元素，这里还没有真正出队列
        *pkt = mypkt->pkt;
        media_type = mypkt->media_type;

        // 更新统计信息
        if (E_AUDIO_TYPE == media_type) {
            stats_.audio_nb_packets--;      // 包数量
            stats_.audio_size -= mypkt->pkt->size;
            // 持续时长怎么统计，不是用pkt->duration. 本来没有Pop操作的话，audio_front_pts_的值不会改变，那么下面获取相关信息时，队尾的pts-对头的pts，
            // 再加上audio_frame_duration_就能获取到正确的队列时长，但是一旦出现Pop改变了audio_front_pts_之后，由于audio_front_pts_是等于被删除节点的pts，
            // 即 mypkt->pkt->pts，所以就导致再按"队尾的pts - 对头的pts + audio_frame_duration_"求队列时长就会多出一帧的时长，这是不正确的。
            // 例如此时队列有两帧，对头pts=40，队尾pts=80，获取时长就是80-40+40=80(视频为例).当pop后，对头pts还是40，再算还是两帧的时长，但实际队列只剩下一帧。
            // 解决方法：添加标记位是否有pop操作，有则不能再加上audio_frame_duration_，这样就能保证获取信息时，对应的包数是正确的队列时长。
            // 一开始我尝试了其它错误的做法：
            // 1）例如在push时记录队头的下一个pts，但是由于Pop时无法更新队头的下一个pts，所以该方法还是行不通。
            // 2）在Pop时，如果包队列个数>=1，则去获取下一个包的pts，但是这个包的类型是不确定的(队列存放音视频包是无序的)，
            //		所以无法判断它就是该类型的下一个pts，所以该方法也是不行的。
            audio_front_pts_ = mypkt->pkt->pts;
            audio_is_pop_ = true;
        }
        if (E_VIDEO_TYPE == media_type) {
            stats_.video_nb_packets--;      // 包数量
            stats_.video_size -= mypkt->pkt->size;
            // 持续时长怎么统计，不是用pkt->duration
            video_front_pts_ = mypkt->pkt->pts;
            video_is_pop_ = true;
        }

        queue_.pop();
        free(mypkt);

        return 1;
    }

    /**
    * @brief 带超时时间的获取一个包。
    * @param pkt 传入传出，取出一个包。
    * @param media_type 取出的包类型。
    * @param timeout -1代表阻塞等待; 0; 代表非阻塞等待; >0 代表有超时的等待。
    * @return -1 abort;  0 超时返回，没有消息；1 超时时间内有消息.
    *
    * 后续可以将Pop内部的代码与PopWithTimeout内部代码写成一个函数，方便后续优化。
    */
    int PopWithTimeout(AVPacket **pkt, MediaType &media_type, int timeout)
    {
        // 小于0，直接调Pop阻塞等待即可。
        if (timeout < 0) {
            return Pop(pkt, media_type);
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (abort_request_) {
            //LogWarn("abort request");
            return -1;
        }

        // wait或者wait_for不带参2与pthread_cond_wait是一样的，但是带参2是不一样的，带参2相当于多了一个忙轮询。
        // 这也就是为啥wait或者wait_for加上参2后，使用if即可；而pthread_cond_wait需要使用while忙轮询去判断是否为空，不能使用if。
        // 这里的参2指lambda表达式，而非真正的参2延时时间。
        if (queue_.empty()) {
            cond_.wait_for(lock, std::chrono::milliseconds(timeout), [this] {
                return !queue_.empty() | abort_request_;
            });
        }
        if (abort_request_) {
            //LogWarn("abort request");
            return -1;
        }
        // 超时需要判断队列是否为空，因为当超过时间都没满足条件，wait_for也会返回，这样可能会导致队列是空的。
        if (queue_.empty()) {
            return 0;
        }


        // 真正干活
        MyAVPacket *mypkt = queue_.front(); // 读取队列首部元素，这里还没有真正出队列
        *pkt = mypkt->pkt;
        media_type = mypkt->media_type;

        // 更新统计信息
        if (E_AUDIO_TYPE == media_type) {
            stats_.audio_nb_packets--;      // 包数量
            stats_.audio_size -= mypkt->pkt->size;
            // 持续时长怎么统计，不是用pkt->duration
            audio_front_pts_ = mypkt->pkt->pts;
            audio_is_pop_ = true;
        }
        if (E_VIDEO_TYPE == media_type) {
            stats_.video_nb_packets--;      // 包数量
            stats_.video_size -= mypkt->pkt->size;
            // 持续时长怎么统计，不是用pkt->duration
            video_front_pts_ = mypkt->pkt->pts;
            video_is_pop_ = true;
        }

        queue_.pop();
        free(mypkt);

        return 1;
    }

    /**
    * @brief 判断包队列是否为空。
    * @return 为空 true 不为空 false
    */
    bool Empty()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    /**
    * @brief 中断，唤醒在等待的线程。
    * @return void
    */
    void Abort()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_request_ = true;
        cond_.notify_all();
    }

    /**
    * @brief drop掉包队列中的音视频数据，音频的drop暂不考虑支持。
    * Drop的主要思想是这样的：首先队列不为空的话，首先判断队头是否是I帧，不是则会一直drop掉非关键帧，
    * 当遇到关键帧，再去判断队列时长是否已经小于想要保留的时长。 小于则退出，否则连同该I帧也drop，然后回到上面的开始步骤继续drop非关键帧，以此类推...
    *
    * @param all：
    *            1）all为true:清空队列;清空队列时最好把一些状态信息也重置，防止污染下一次的使用
    *            2）all为false: 会一直drop数据，直到遇到I帧，会去判断是否drop够数据，满足则保留该关键帧，否则也会drop掉。
    * @param remain_max_duration 队列最大保留remain_max_duration时长;
    *
    * @return no mean.
    */
    int Drop(bool all, int64_t remain_max_duration)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty())
        {
            // 1 获取队列的对头包
            MyAVPacket *mypkt = queue_.front();
            // 2 判断是否需要drop掉，只有关键帧才能进duration <= remain_max_duration判断，
            //      否则非关键帧不管是否时长已经小于remain_max_duration，同样会被drop掉。
            if (!all && mypkt->media_type == E_VIDEO_TYPE && (mypkt->pkt->flags & AV_PKT_FLAG_KEY))// 只有关键帧才会进来，也就说每次drop完后，队列头可能是关键帧或者无数据
            {
                // 2.1 以pts为准，求出队列目前视频的时长
                // 例如以video_front_pts_=76, video_back_pts_分别是76 117 165 197四帧的pts为例，
                // duration=(41)+(48)+(32)+(最后一帧的时长由它的下一帧pts决定，所以队列中求出的duration实际只有3帧)=121ms.
                // see pushPrivate的打印
#ifndef MyDurationCode
                int64_t duration = video_back_pts_ - video_front_pts_;
#else
                // 我自己的代码
                int64_t duration;
                if (video_is_pop_) {
                    duration = video_back_pts_ - video_front_pts_;
                }
                else {
                    duration = video_back_pts_ - video_front_pts_ + video_frame_duration_;
                }
#endif
                // 2.2 也参考帧(包)持续时长 * 帧(包)数
                // 意思是：duration意外变成负数或者队列时长比队列实际包数的总时长的两倍还要大的话，则自动修正duration，修正为队列的包数的总时长
                // 这种情况几乎不怎么发生，只是一种比较极端情况的处理
                if (duration < 0 || duration > video_frame_duration_ * stats_.video_nb_packets * 2) {
                    duration = video_frame_duration_ * stats_.video_nb_packets;
                    LogInfo("++++++++++ adjust duration become: %lld, video_frame_duration_: %lld, video_nb_packets: %lld ++++++++++",
                        duration, video_frame_duration_, stats_.video_nb_packets);
                }

                LogInfo("video duration: %lld", duration);
                if (duration <= remain_max_duration) {
                    LogInfo("drop break, video duration: %lld", duration);
                    break;// 小于，说明可以break 退出while
                }

            }
            // 3 音频是否有drop的必要呢？后面看情况和需求再考虑，因为时钟一般以音频为准

            // 4 更新统计相关信息
            if (E_AUDIO_TYPE == mypkt->media_type)
            {
                stats_.audio_nb_packets--;      // 包数量
                stats_.audio_size -= mypkt->pkt->size;
                // 持续时长怎么统计，不是用pkt->duration
                audio_front_pts_ = mypkt->pkt->pts;
                audio_is_pop_ = true;
            }
            if (E_VIDEO_TYPE == mypkt->media_type)
            {
                stats_.video_nb_packets--;      // 包数量
                stats_.video_size -= mypkt->pkt->size;
                // 持续时长怎么统计，不是用pkt->duration
                video_front_pts_ = mypkt->pkt->pts;
                video_is_pop_ = true;
            }

            // 5 真正drop掉数据
            av_packet_free(&mypkt->pkt);        // 先释放AVPacket
            queue_.pop();
            free(mypkt);                        // 再释放MyAVPacket
        }

        if (all) {
            Clear();
        }

        return 0;
    }

    /**
    * @brief 自己写的，清空队列的一些信息，防止影响下一次的使用。
    * @return void。
    */
    void Clear() {
        memset(&stats_, 0, sizeof(PacketQueueStats));
        audio_front_pts_ = 0;
        audio_back_pts_ = 0;
        audio_first_packet = 1;
        video_front_pts_ = 0;
        video_back_pts_ = 0;
        video_first_packet = 1;

        audio_is_pop_ = false;
        video_is_pop_ = false;
    }

    /**
    * @brief 自己写的，清空队列。
    * @return void。
    */
    void queue_erase_all()
    {

        std::lock_guard<std::mutex> lock(mutex_);

        while (!queue_.empty()) {
            MyAVPacket *mypkt = queue_.front();
            av_packet_free(&mypkt->pkt);        // 先释放AVPacket
            queue_.pop();
            free(mypkt);                        // 再释放MyAVPacket
        }

        // 清空后，下面的内容应当被重置
        Clear();
    }

    /**
    * @brief 获取队列中目前的音频持续时间，这里会返回队列全部的时长，
    *          而不像上面的pushPrivate和Drop，它们内部使用的是n-1帧的时长去判断逻辑。
    *			这里会根据是否有Pop操作而统计队列的真实时长。
    *
    * @return 始终返回一个duration，若是负数或者太大的值，会自动调整到合适的值。
    */
    int64_t GetAudioDuration()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (stats_.audio_nb_packets <= 0) {
            return 0;
        }

        //以pts为准
#ifndef MyDurationCode
        int64_t duration = audio_back_pts_ - audio_front_pts_;
        // 也参考帧（包）持续 *帧(包)数
        if (duration < 0     // pts回绕
            || duration > audio_frame_duration_ * stats_.audio_nb_packets * 2) {
            duration = audio_frame_duration_ * stats_.audio_nb_packets;
        }
        else {
            // 上面的pushPrivate和Drop看到，audio_back_pts_ - audio_front_pts_实际是缺少最后一帧的时长，所以这里返回需要加上一帧时长
            duration += audio_frame_duration_;
        }
        return duration;
#else
        // 我自己的代码，这样更准确
        int64_t duration;
        if (audio_is_pop_) {
            duration = audio_back_pts_ - audio_front_pts_;	// 有Pop包时队列的真实长度，
                                                            // 这样和audio_frame_duration_ * stats_.audio_nb_packets * 2比较才更准确
            if (duration < 0 /*pts回绕*/ || duration > audio_frame_duration_ * stats_.audio_nb_packets * 2) { // duration为负数或者过大
                duration = audio_frame_duration_ * stats_.audio_nb_packets;
            }
        }
        else {
            duration = audio_back_pts_ - audio_front_pts_ + audio_frame_duration_;	// 无Pop包时队列的真实长度，
                                                                                    // 这样和audio_frame_duration_ * stats_.audio_nb_packets * 2比较才更准确
            // 也参考帧（包）持续 *帧(包)数
            if (duration < 0 /*pts回绕*/ || duration > audio_frame_duration_ * stats_.audio_nb_packets * 2) {
                duration = audio_frame_duration_ * stats_.audio_nb_packets;
            }
        }

        return duration;
#endif
    }

    /**
    * @brief 获取队列中目前的视频持续时间，这里会返回队列全部的时长，
    *          而不像上面的pushPrivate和Drop，它们内部使用的是n-1帧的时长去判断逻辑。
    *			这里会根据是否有Pop操作而统计队列的真实时长。
    *
    * @return 始终返回一个duration，若是负数或者太大的值，会自动调整到合适的值。
    */
    int64_t GetVideoDuration()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        //以pts为准
#ifndef MyDurationCode
        int64_t duration = video_back_pts_ - video_front_pts_;
        // 也参考帧（包）持续 *帧(包)数
        if (duration < 0     // pts回绕
            || duration > video_frame_duration_ * stats_.video_nb_packets * 2) {
            duration = video_frame_duration_ * stats_.video_nb_packets;
        }
        else {
            duration += video_frame_duration_;
        }
        return duration;
#else
        // 我自己的代码，这样更准确
        int64_t duration;
        if (video_is_pop_) {
            duration = video_back_pts_ - video_front_pts_;	// 有Pop包时队列的真实长度，
                                                            // 这样和video_frame_duration_ * stats_.video_nb_packets * 2比较才更准确
            if (duration < 0 /*pts回绕*/ || duration > video_frame_duration_ * stats_.video_nb_packets * 2) { // duration为负数或者过大
                duration = video_frame_duration_ * stats_.video_nb_packets;
            }
        }
        else {
            duration = video_back_pts_ - video_front_pts_ + video_frame_duration_;	// 无Pop包时队列的真实长度，
                                                                                    // 这样和video_frame_duration_ * stats_.video_nb_packets * 2比较才更准确
            if (duration < 0 /*pts回绕*/ || duration > video_frame_duration_ * stats_.video_nb_packets * 2) {
                duration = video_frame_duration_ * stats_.video_nb_packets;
            }
        }

        return duration;
#endif
    }

    /**
    * @brief 获取音频包数量。
    * @return 返回音频包数量
    */
    int GetAudioPackets()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_.audio_nb_packets;
    }
    /**
    * @brief 获取视频包数量。
    * @return 返回视频包数量
    */
    int GetVideoPackets()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stats_.video_nb_packets;
    }

    /**
    * @brief 获取队列的状态信息，统一获取时长与包数，比上面单纯获取时长或者包数更详细。这里的时长同样是队列全部帧的时长。
    * @param stats 传入传出，状态信息。
    * @return void。
    */
    void GetStats(PacketQueueStats *stats)
    {
        if (!stats) {
            //LogError("stats is null");
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        // 1 获取音频时长
        // 以pts为准
#ifndef MyDurationCode
        int64_t audio_duration = audio_back_pts_ - audio_front_pts_;
        // 也参考帧（包）持续 *帧(包)数
        if (audio_duration < 0     // pts回绕
            || audio_duration > audio_frame_duration_ * stats_.audio_nb_packets * 2) {
            audio_duration = audio_frame_duration_ * stats_.audio_nb_packets;
        }
        else {
            audio_duration += audio_frame_duration_;
        }
#else
        // 我自己的代码
        int64_t audio_duration;
        if (audio_is_pop_) {
            audio_duration = audio_back_pts_ - audio_front_pts_;
            if (audio_duration < 0 || audio_duration > audio_frame_duration_ * stats_.audio_nb_packets * 2) {
                audio_duration = audio_frame_duration_ * stats_.audio_nb_packets;
            }
        }
        else {
            audio_duration = audio_back_pts_ - audio_front_pts_ + audio_frame_duration_;
            if (audio_duration < 0 || audio_duration > audio_frame_duration_ * stats_.audio_nb_packets * 2) {
                audio_duration = audio_frame_duration_ * stats_.audio_nb_packets;
            }
        }

#endif

        // 2 获取视频时长
        // 以pts为准
#ifndef MyDurationCode
        int64_t video_duration = video_back_pts_ - video_front_pts_;
        // 也参考帧（包）持续 *帧(包)数
        if (video_duration < 0     // pts回绕
            || video_duration > video_frame_duration_ * stats_.video_nb_packets * 2) {
            video_duration = video_frame_duration_ * stats_.video_nb_packets;
        }
        else {
            video_duration += video_frame_duration_;
        }
#else
        // 我自己的代码
        int64_t video_duration;
        if (video_is_pop_) {
            video_duration = video_back_pts_ - video_front_pts_;
            if (video_duration < 0 || video_duration > video_frame_duration_ * stats_.video_nb_packets * 2) {
                video_duration = video_frame_duration_ * stats_.video_nb_packets;
            }
        }
        else {
            video_duration = video_back_pts_ - video_front_pts_ + video_frame_duration_;
            if (video_duration < 0 || video_duration > video_frame_duration_ * stats_.video_nb_packets * 2) {
                video_duration = video_frame_duration_ * stats_.video_nb_packets;
            }
        }

#endif
        stats->audio_duration = audio_duration;
        stats->video_duration = video_duration;

        // 3 获取音视频包数与其音视频的总字节大小
        stats->audio_nb_packets = stats_.audio_nb_packets;
        stats->video_nb_packets = stats_.video_nb_packets;
        stats->audio_size = stats_.audio_size;
        stats->video_size = stats_.video_size;
    }

private:
    std::mutex mutex_;
    std::condition_variable cond_;
    std::queue<MyAVPacket *> queue_;                    // queue_可能会残留MyAVPacket，应该要回收一下，到时看看怎么回收比较好

    bool abort_request_ = false;

    // 统计相关
    PacketQueueStats stats_;
    double audio_frame_duration_ = 23.21995649;         // 默认23.2ms 44.1khz  1024*1000ms/44100=23.21995649ms
    double video_frame_duration_ = 40;                  // 40ms 视频帧率为25的  ， 1000ms/25=40ms
                                                        // 用于统计队列pts的时长
    int64_t audio_front_pts_ = 0;
    int64_t audio_back_pts_ = 0;
    int     audio_first_packet = 1;						// 标记位，标识是否是首包，用于记录audio_front_pts_
    int64_t video_front_pts_ = 0;
    int64_t video_back_pts_ = 0;
    int     video_first_packet = 1;						// 标记位，标识是否是首包，用于记录video_front_pts_

    bool audio_is_pop_ = false;							// 标记位，标识音频是否有Pop的操作
    bool video_is_pop_ = false;
};
#endif // PACKETQUEUE_H


