#ifndef AVTIMEBASE_H
#define AVTIMEBASE_H
#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <time.h>
#else
#include <sys/time.h>
#endif
#include "dlog.h"

// 单例AVPublishTime，这个类不是安全的单例类
// 思想主要是：记录一个start_time_，每次通过当前的系统时间减去这个start_time_得到一个差值，
// 再与帧间隔的总时长audio_pre_pts_(video_pre_pts_)进行比较，是否误差过大，从而进行校正。
class AVPublishTime
{
public:

    typedef enum PTS_STRATEGY
    {
        PTS_RECTIFY = 0,        // 缺省类型，pts的间隔尽量保持帧间隔
        PTS_REAL_TIME           // 实时pts
    }PTS_STRATEGY;

public:

    // 这种GetInstance写法肯定是不适合多线程的，后续可写成双重if判断+锁，才能符合多线程安全和加快效率。
    static AVPublishTime* GetInstance()
    {
        if (s_publish_time == NULL){
            s_publish_time = new AVPublishTime();
        }
        return s_publish_time;
    }

    AVPublishTime() {
        start_time_ = getCurrentTimeMsec();
    }

    // 更新开始时间
    void Rest() {
        start_time_ = getCurrentTimeMsec();
    }

    // 设置帧时长，并且更新帧时长的误差阈值
    void set_audio_frame_duration(const double frame_duration) {
        audio_frame_duration_ = frame_duration;
        audio_frame_threshold_ = (uint32_t)(frame_duration / 2);// 帧时长的误差阈值，以除以2为阈值
    }
    void set_video_frame_duration(const double frame_duration) {
        video_frame_duration_ = frame_duration;
        video_frame_threshold_ = (uint32_t)(frame_duration / 2);
    }

    // 获取从开始时间，到当前时间的一个差值，即获取音频pts帧间隔的总时长。
    uint32_t get_audio_pts() {
        int64_t pts = getCurrentTimeMsec() - start_time_;       // 当前时间与开始时间的差值

        if(PTS_RECTIFY == audio_pts_strategy_) {                // 缺省策略，目前都是这个策略
            // abs函数看https://blog.csdn.net/u010900851/article/details/9047249。
            uint32_t diff = (uint32_t)abs(pts - (long long)(audio_pre_pts_ + audio_frame_duration_));
            if(diff < audio_frame_threshold_) {
                // 误差在阈值范围内, 保持帧间隔
                audio_pre_pts_ += audio_frame_duration_;        // 帧间隔累加，浮点数，audio_pre_pts_是总的帧间隔时长，用于与当前时间减去start_time_比较，是否误差过大需要调整
                LogDebug("get_audio_pts1: %u RECTIFY: %0.0lf", diff, audio_pre_pts_);
                // 取整返回，取余0xffffffff(共四字节)是因为：他想pts是以32位4字节处理，确保不越界4个字节，当超过这个范围，将取余，值被改变，说明是不对的
                // 感觉取余8字节不是更好吗，这个取余感觉去掉问题不是特别大。
                return (uint32_t)(((int64_t)audio_pre_pts_) % 0xffffffff);
            }
            audio_pre_pts_ = (double)pts;                       // 误差超过半帧，重新调整pts
            LogDebug("get_audio_pts2:%u, RECTIFY:%0.0lf", diff, audio_pre_pts_);
            return (uint32_t)(pts % 0xffffffff);
        }else {
            audio_pre_pts_ = (double)pts;                       // 直接以实时的时间差值作为总的帧间隔时长
            LogDebug("get_audio_pts REAL_TIME: %0.0lf", audio_pre_pts_);
            return (uint32_t)(pts % 0xffffffff);
        }
    }

    // 获取从开始时间，到当前时间的一个差值，即获取视频pts帧间隔的总时长。
    uint32_t get_video_pts() {
        int64_t pts = getCurrentTimeMsec() - start_time_;
        if(PTS_RECTIFY == video_pts_strategy_) {
            uint32_t diff =(uint32_t)abs(pts - (long long)(video_pre_pts_ + video_frame_duration_));
            if(diff < video_frame_threshold_) {
                // 误差在阈值范围内, 保持帧间隔
                video_pre_pts_ += video_frame_duration_;
                LogDebug("get_video_pts1: %u RECTIFY: %0.0lf", diff, video_pre_pts_);
                return (uint32_t)(((int64_t)video_pre_pts_) % 0xffffffff);
            }
            video_pre_pts_ = (double)pts;                       // 误差超过半帧，重新调整pts
            LogDebug("get_video_pts2: %u RECTIFY: %0.0lf", diff, video_pre_pts_);
            return (uint32_t)(pts % 0xffffffff);
        }else {
            video_pre_pts_ = (double)pts;                       // 直接以实时的时间差值作为总的帧间隔时长
            LogDebug("get_video_pts REAL_TIME: %0.0lf", video_pre_pts_);
            return (uint32_t)(pts % 0xffffffff);
        }
    }


    // 设置音视频的pts策略
    void set_audio_pts_strategy(PTS_STRATEGY pts_strategy){
        audio_pts_strategy_ = pts_strategy;
    }
    void set_video_pts_strategy(PTS_STRATEGY pts_strategy){
        video_pts_strategy_ = pts_strategy;
    }

    // 获取当前时间与start_time_的差值
    uint32_t getCurrenTime() {
        int64_t t = getCurrentTimeMsec() - start_time_;
        return (uint32_t)(t % 0xffffffff);

    }


    // 各个关键点的时间戳
    inline const char *getKeyTimeTag() {
        return "keytime";
    }
    // rtmp位置关键点
    inline const char *getRtmpTag() {
        return "keytime:rtmp_publish";
    }
    // 发送metadata
    inline const char *getMetadataTag() {
        return "keytime:metadata";
    }
    // aac sequence header
    inline const char *getAacHeaderTag() {
        return "keytime:aacheader";
    }
    // aac raw data
    inline const char *getAacDataTag() {
        return "keytime:aacdata";
    }
    // avc sequence header
    inline const char *getAvcHeaderTag() {
        return "keytime:avcheader";
    }

    // 第一个i帧
    inline const char *getAvcIFrameTag() {
        return "keytime:avciframe";
    }
    // 第一个非i帧
    inline const char *getAvcFrameTag() {
        return "keytime:avcframe";
    }
    // 音视频解码
    inline const char *getAcodecTag() {
        return "keytime:acodec";
    }
    inline const char *getVcodecTag() {
        return "keytime:vcodec";
    }
    // 音视频捕获
    inline const char *getAInTag() {
        return "keytime:ain";
    }
    inline const char *getVInTag() {
        return "keytime:vin";
    }

private:

    // 获取从公元1970年1月1日0时0分0秒 算起至今的UTC时间所经过的秒数，单位是微秒。若不是从1970算起的时间，也会转成这样的时间。
    int64_t getCurrentTimeMsec() {
#ifdef _WIN32
        struct timeval tv;
        time_t clock;
        struct tm tm;

        // 获取当地的当前系统日期和时间。
        SYSTEMTIME wtm;
        GetLocalTime(&wtm);

        // 转换在tm中
        tm.tm_year = wtm.wYear - 1900;
        tm.tm_mon = wtm.wMonth - 1;
        tm.tm_mday = wtm.wDay;
        tm.tm_hour = wtm.wHour;
        tm.tm_min = wtm.wMinute;
        tm.tm_sec = wtm.wSecond;
        tm.tm_isdst = -1;

        // mktime用来将参数tm结构数据 转换成 从公元1970年1月1日0时0分0秒 算起至今的UTC时间所经过的秒数。返回经过的秒数。
        clock = mktime(&tm);
        tv.tv_sec = clock;
        tv.tv_usec = wtm.wMilliseconds * 1000;
        return ((unsigned long long)tv.tv_sec * 1000 + (long)tv.tv_usec / 1000);
#else
        // linux不需要转，本身就是1970开始算的时间。
        struct timeval tv;
        gettimeofday(&tv,NULL);
        return ((unsigned long long)tv.tv_sec * 1000 + (long)tv.tv_usec / 1000);
#endif
    }

    int64_t start_time_                 = 0;                                    // 记录当前的时间，单位毫秒。

    PTS_STRATEGY audio_pts_strategy_    = PTS_RECTIFY;
    double audio_frame_duration_        = 21.3333;                              // 帧时长，默认按aac 1024 个采样点, 48khz计算
    uint32_t audio_frame_threshold_     = (uint32_t)(audio_frame_duration_ / 2);
    double audio_pre_pts_ = 0;                                                  // 统计总的音频帧时长

    PTS_STRATEGY video_pts_strategy_    = PTS_RECTIFY;
    double video_frame_duration_        = 40;                                   // 默认是25帧计算
    uint32_t video_frame_threshold_     = (uint32_t)(video_frame_duration_ / 2);
    double video_pre_pts_               = 0;                                    // 统计总的视频帧时长


    static AVPublishTime *s_publish_time;                                       // 单例成员
};






// 用来debug rtmp拉流的关键时间点
// 不难，参考上面的
class AVPlayTime
{
public:
    static AVPlayTime* GetInstance() {
        if ( s_play_time == NULL ){
            s_play_time = new AVPlayTime();
        }
        return s_play_time;
    }

    AVPlayTime() {
        start_time_ = getCurrentTimeMsec();
    }

    void Rest() {
        start_time_ = getCurrentTimeMsec();
    }
    // 各个关键点的时间戳
    inline const char *getKeyTimeTag() {
        return "keytime";
    }
    // rtmp位置关键点
    inline const char *getRtmpTag() {
        return "keytime:rtmp_pull";
    }
    // 获取到metadata
    inline const char *getMetadataTag() {
        return "metadata";
    }
    // aac sequence header
    inline const char *getAacHeaderTag() {
        return "aacheader";
    }
    // aac raw data
    inline const char *getAacDataTag() {
        return "aacdata";
    }
    // avc sequence header
    inline const char *getAvcHeaderTag() {
        return "avcheader";
    }

    // 第一个i帧
    inline const char *getAvcIFrameTag() {
        return "avciframe";
    }
    // 第一个非i帧
    inline const char *getAvcFrameTag() {
        return "avcframe";
    }
    // 音视频解码
    inline const char *getAcodecTag() {
        return "keytime:acodec";
    }
    inline const char *getVcodecTag() {
        return "keytime:vcodec";
    }
    // 音视频输出
    inline const char *getAoutTag() {
        return "keytime:aout";
    }
    inline const char *getVoutTag() {
        return "keytime:vout";
    }

    // 返回毫秒
    uint32_t getCurrenTime() {
        int64_t t = getCurrentTimeMsec() - start_time_;

        return (uint32_t)(t%0xffffffff);

    }

private:
    int64_t getCurrentTimeMsec() {
#ifdef _WIN32
        struct timeval tv;
        time_t clock;
        struct tm tm;
        SYSTEMTIME wtm;
        GetLocalTime(&wtm);
        tm.tm_year = wtm.wYear - 1900;
        tm.tm_mon = wtm.wMonth - 1;
        tm.tm_mday = wtm.wDay;
        tm.tm_hour = wtm.wHour;
        tm.tm_min = wtm.wMinute;
        tm.tm_sec = wtm.wSecond;
        tm.tm_isdst = -1;
        clock = mktime(&tm);
        tv.tv_sec = clock;
        tv.tv_usec = wtm.wMilliseconds * 1000;
        return ((unsigned long long)tv.tv_sec * 1000 + ( long)tv.tv_usec / 1000);
#else
        struct timeval tv;
        gettimeofday(&tv,NULL);
        return ((unsigned long long)tv.tv_sec * 1000 + (long)tv.tv_usec / 1000);
#endif
    }

    int64_t start_time_ = 0;

    static AVPlayTime * s_play_time;
};


#endif // AVTIMEBASE_H
