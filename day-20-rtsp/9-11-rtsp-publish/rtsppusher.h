#ifndef RTSPPUSHER_H
#define RTSPPUSHER_H

#include "mediabase.h"
#include "commonlooper.h"
#include "packetqueue.h"
#include "messagequeue.h"
extern "C" {
#include "libavformat/avformat.h"
#include "libavformat/avio.h"
#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"
}

// 继承CommonLooper，是因为RtspPusher会开一个线程去读取编码后的packet。
class RtspPusher : public CommonLooper
{
public:
    // 他这里的消息队列设计，是由外部传进来的
    RtspPusher(MessageQueue *msg_queue);
    virtual ~RtspPusher();

    RET_CODE Init(const Properties& properties);
    // 如果有视频成分
    RET_CODE ConfigVideoStream(const AVCodecContext *ctx);
    // 如果有音频成分
    RET_CODE ConfigAudioStream(const AVCodecContext *ctx);
    // 连接服务器，如果连接成功则启动线程
    RET_CODE Connect();
    virtual void Loop();

    RET_CODE Push(AVPacket *pkt, MediaType media_type);

    void DeInit();

    bool IsTimeout();
    void RestTiemout();
    int GetTimeout();
    int64_t GetBlockTime();

private:
    int64_t pre_debug_time_ = 0;                    // 定时打印队列信息的起始时间，默认0开始即可。
    int64_t debug_interval_ = 2000;                 // 定时打印队列状态信息的间隔，这里默认是2s。
    void debugQueue(int64_t interval);              // 按时间间隔打印packetqueue的状况
    // 监测队列的缓存情况
    void checkPacketQueueDuration();
    int sendPacket(AVPacket *pkt, MediaType media_type);

    // 整个输出流的上下文
    AVFormatContext *fmt_ctx_  = NULL;
    // 视频编码器上下文
    AVCodecContext *video_ctx_ = NULL;
    // 音频频编码器上下文
    AVCodecContext *audio_ctx_ = NULL;

    // 流成分
    AVStream *video_stream_ = NULL;
    int video_index_ = -1;
    AVStream *audio_stream_ = NULL;
    int audio_index_ = -1;

    std::string url_ = "";                          // 推流rtsp的地址
    std::string rtsp_transport_ = "";               // rtsp的传输方式，tcp或者udp

    double audio_frame_duration_ = 23.21995649;     // 默认23.2ms 44.1khz  1024*1000ms/44100=23.21995649ms
    double video_frame_duration_ = 40;              // 40ms 视频帧率为25的  ， 1000ms/25=40ms

    PacketQueue *queue_ = NULL;                     // 编码后的包数据队列，推流器从这里取数据进行推流，而上层不会再接触该队列，故推流器就是最高上层，再此new该队列即可。

    // 队列最大限制时长
    int max_queue_duration_ = 500;                  // 默认500ms或者100ms两三帧也行，看情况。

    // 处理超时
    int timeout_;
    int64_t pre_time_ = 0;                          // 记录调用ffmpeg api之前的时间，防止api卡死
    MessageQueue *msg_queue_ = NULL;                // 消息队列，这里由构造初始化，浅拷贝，所以应当由外部释放，析构不处理
};

#endif // RTSPPUSHER_H
