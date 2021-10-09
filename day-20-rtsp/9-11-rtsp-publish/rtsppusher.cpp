#include "rtsppusher.h"
#include "dlog.h"
#include "timesutil.h"


RtspPusher::RtspPusher( MessageQueue *msg_queue)
    : msg_queue_(msg_queue)
{
    LogInfo("RtspPusher create");
}

RtspPusher::~RtspPusher()
{
    DeInit();       // 释放资源
}

/**
 * @brief 中断回调函数，防止FFmpeg的某些api可能卡死的情况。
 * @param ctx rtsp推流对象。
 * @return false: 继续阻塞;  true: 退出阻塞
 */
static int decode_interrupt_cb(void *ctx)
{
    RtspPusher *rtsp_puser = (RtspPusher *)ctx;
    if(rtsp_puser->IsTimeout()) {
        LogWarn("decode_interrupt_cb timeout: %d ms", rtsp_puser->GetTimeout());
        return 1;
    }

    //    LogInfo("block time:%lld", rtsp_puser->GetBlockTime());
    return 0;
}

/**
 * @brief   设置相关参数，主要是分配AVFormatContext。
 * @param   url_                    推流url。
 *          rtsp_transport_         rtsp的传输方式。
 *          audio_frame_duration_   音频一帧的时长。
 *          video_frame_duration_   视频一帧的时长。
 *          timeout_                超时时长。
 *          max_queue_duration_     最大队列的包的保留时长。
 * @return  成功 0 失败 other
 */
RET_CODE RtspPusher::Init(const Properties &properties)
{
    url_                    = properties.GetProperty("url", "");
    rtsp_transport_         = properties.GetProperty("rtsp_transport", "");
    audio_frame_duration_   = properties.GetProperty("audio_frame_duration", 0);
    video_frame_duration_   = properties.GetProperty("video_frame_duration", 0);
    timeout_                = properties.GetProperty("timeout", 5000);    // 默认为5秒
    max_queue_duration_     = properties.GetProperty("max_queue_duration", 500);
    if(url_ == "") {
        LogError("url is null");
        return RET_FAIL;
    }
    if(rtsp_transport_ == "") {
        LogError("rtsp_transport is null, use udp or tcp");
        return RET_FAIL;
    }

    int ret = 0;
    char str_error[512] = {0};

    // 1 初始化网络库
    ret = avformat_network_init();
    if(ret < 0) {
        av_strerror(ret, str_error, sizeof(str_error) -1);
        LogError("avformat_network_init failed:%s", str_error);
        return RET_FAIL;
    }
    // 2 分配AVFormatContext
    ret = avformat_alloc_output_context2(&fmt_ctx_, NULL, "rtsp", url_.c_str());        // 一般推rtmp，参3写"flv"即可。
    if(ret < 0) {
        av_strerror(ret, str_error, sizeof(str_error) -1);
        LogError("avformat_alloc_output_context2 failed:%s", str_error);
        return RET_FAIL;
    }
    // 3 设置参数
    // av_opt_set和编码器的av_dict_set设置参数应该是差不多的，目前还没发现两者的区别.例如下面可以写成这样：
    // char key2[] = "rtsp_transport";
    // char val2[] = "tcp";     // 即rtsp_transport_.c_str()
    // av_dict_set(&opts, key2, val2, 0);
    ret = av_opt_set(fmt_ctx_->priv_data, "rtsp_transport", rtsp_transport_.c_str(), 0);    // 参1是一个对象，参2是参1的一个成员
    if(ret < 0) {
        av_strerror(ret, str_error, sizeof(str_error) -1);
        LogError("av_opt_set failed:%s", str_error);
        return RET_FAIL;
    }
    // 设置超时回调，防止卡死
    fmt_ctx_->interrupt_callback.callback = decode_interrupt_cb;
    fmt_ctx_->interrupt_callback.opaque = this;

    // 4 创建队列
    queue_ = new PacketQueue(audio_frame_duration_, video_frame_duration_);
    if(!queue_) {
        LogError("new PacketQueue failed");
        return RET_ERR_OUTOFMEMORY;
    }

    return RET_OK;
}

/**
 * @brief 回收rtsp推流类的内存，看了一下，就这3个成员需要回收，没啥问题，并且这个函数重复调用没有问题。
 * @return void。
 */
void RtspPusher::DeInit()
{
    if(queue_) {
        queue_->Abort();
    }
    // 1
    Stop();
    // 2
    if(fmt_ctx_) {
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = NULL;
    }
    // 3 注意上面要先中断，并且其它类有些回收是需要注意顺序的，这里不用考虑顺序。
    if(queue_) {
        delete queue_;
        queue_ = NULL;
    }
}

/**
 * @brief push一个包进队列，内部就是调用队列的Push，详看queue_->Push。
 * @param pkt 数据包。
 * @param media_type 数据包类型。
 * @return success 0 fail -1.
 */
RET_CODE RtspPusher::Push(AVPacket *pkt, MediaType media_type)
{
    int ret = queue_->Push(pkt, media_type);
    if(ret < 0) {
        return RET_FAIL;
    } else {
        return RET_OK;
    }
}

/**
 * @brief 连接服务器，写输出头，连接成功后，会创建一个线程进行写帧推流。
 *          不过他没有类似SDK这样调用avio_open2去打开网络io，有兴趣的可以看源码分析。
 * @return success 0 fail -1.
 */
RET_CODE RtspPusher::Connect()
{
    if(!audio_stream_ && !video_stream_) {
        return RET_FAIL;
    }

    LogInfo("connect to: %s", url_.c_str());
    // 连接服务器
    RestTiemout();                                          // 每次调用FFmpeg有可能卡死的接口都应该更新该值。if条件应加多一个重连该接口条件，解决GetTickCount归0问题。
    int ret = avformat_write_header(fmt_ctx_, NULL);
    if(ret < 0) {
        char str_error[512] = {0};
        av_strerror(ret, str_error, sizeof(str_error) - 1);
        LogError("av_opt_set failed: %s", str_error);
        return RET_FAIL;
    }

    LogInfo("avformat_write_header ok");
    return this->Start();                                       // 启动线程
}

/**
 * @brief rtsp的推流线程回调，内部从编码后的packet队列中去取数据进行推流，
 *          会定时debug包队列，并且会检测包队列的时长，如果时长大于最大时长，会进行drop包，以减少时长。
 *
 * @return void。
 */
void RtspPusher::Loop()
{
    LogInfo("Loop into");
    int ret = 0;
    AVPacket *pkt = NULL;
    MediaType media_type;

    LogInfo("sleep_for into");
    // 人为制造延迟，目的是想看长时间不去Pop包推流，debugQueue出来的队列情况会是什么情况，结果：过了10s后，可以看到下面的debugQueue会对视频进行drop挺多帧的。
    // 后面可以删掉这个延时
    std::this_thread::sleep_for(std::chrono::seconds(10));
    LogInfo("sleep_for leave");

    while (true)
    {
        if(request_abort_) {
            LogInfo("abort request");
            break;
        }

        debugQueue(debug_interval_);                            // 定时打印一下队列的状态信息
        checkPacketQueueDuration();                             // 可以每隔一秒check一次，看是否需要drop包。

        ret = queue_->PopWithTimeout(&pkt, media_type, 1000);   // 获取一个packet，记住这里每次从队列取完一个包后，都是需要free掉，因为编码后的包都在这个队列中处理。
        if(1 == ret) // 1代表 读取到消息
        {
            if(request_abort_) {
                LogInfo("abort request");
                av_packet_free(&pkt);
                break;
            }

            // 下面步骤虽然是一样，但是分开写更方便调试，例如对比编码前后与推流时的pts。
            switch (media_type)
            {
            case E_VIDEO_TYPE:
                ret = sendPacket(pkt, media_type);
                if(ret < 0) {
                    LogError("send video Packet failed");
                }
                av_packet_free(&pkt);
                break;
            case E_AUDIO_TYPE:
                ret = sendPacket(pkt, media_type);
                if(ret < 0) {
                    LogError("send audio Packet failed");
                }
                av_packet_free(&pkt);
                break;
            default:
                break;
            }
        }

    }// <===while

    // 如果这里不加av_write_trailer的话，在添加循环推多路流时，在第一路结束后，第二路开始init的时候(同一路)，服务器会返回406错误，
    // 原因是RtspPusher::Loop结束的时候没有write_trailer。添加后就不会出现该问题。
    RestTiemout();
    ret = av_write_trailer(fmt_ctx_);
    if(ret < 0) {
        char str_error[512] = {0};
        av_strerror(ret, str_error, sizeof(str_error) -1);
        LogError("av_write_trailer failed:%s", str_error);
        return;
    }
    LogInfo("av_write_trailer ok");
}

/**
 * @brief 判断接口调用是否超时，防止卡死，添加绝对值处理是防止windows特殊情况的发生。
 * @return 超时返回true； 没超时返回fasle
*/
bool RtspPusher::IsTimeout()
{
    //if(TimesUtil::GetTimeMillisecond() - pre_time_ > timeout_) {
    // 我加的fabs绝对值，防止windows下GetTickCount归0造成更大的误差.
    // 不是说加了fabs就没误差，只是出错情况变小了。不加fabs，也会可能卡死，例如pre_time_很大，GetTimeMillisecond()很小，
    // 而假设fabs这种情况虽然判断为超时，但总比卡死好，这种情况让接口再次调用一下能够解决。
    if(fabs(TimesUtil::GetTimeMillisecond() - pre_time_) > timeout_) {
        return true;    // 超时
    }
    return false;
}

/**
 * @brief 重置调用接口前的时间，每次调用FFmpeg有可能超时的接口时，这个函数都应当被调用。
 *          以更新pre_time_。
*/
void RtspPusher::RestTiemout()
{
    pre_time_ = TimesUtil::GetTimeMillisecond();        // 重置为当前时间
}

/**
 * @brief 获取用户设置的超时时长。
*/
int RtspPusher::GetTimeout()
{
    return timeout_;
}

/**
 * @brief 获取剩余要阻塞的时长。该函数作用不太大，更多是为了debug。
*/
int64_t RtspPusher::GetBlockTime()
{
    return TimesUtil::GetTimeMillisecond() - pre_time_;
}

/**
 * @brief 定时打印队列中的状态信息。
 * @param interval  定时打印的间隔时间。
 * @return void。
 */
void RtspPusher::debugQueue(int64_t interval)
{
    int64_t cur_time = TimesUtil::GetTimeMillisecond();
    if(cur_time - pre_debug_time_ > interval) {
        // 打印信息
        PacketQueueStats stats;             // debug时，应该看这个变量，不应再看queue_的内容，因为释放锁后，其它线程可能在操作队列
        queue_->GetStats(&stats);
        LogInfo("duration:a-%lldms, v-%lldms", stats.audio_duration, stats.video_duration);
        pre_debug_time_ = cur_time;         // 更新定时打印的起始时间
    }
}

/**
 * @brief 检测包队列时长是否超过最大时长，若超过则进行drop包处理，并且会生成消息到消息队列，通知别人处理。
 * @return void。
 */
void RtspPusher::checkPacketQueueDuration()
{
    PacketQueueStats stats;
    queue_->GetStats(&stats);
    if(stats.audio_duration > max_queue_duration_ || stats.video_duration > max_queue_duration_) {
        // 这里生成消息到消息队列有啥作用吗？他的意思是：可以交由上层去drop或者这里直接drop，这里选择直接drop了。
        msg_queue_->notify_msg3(MSG_RTSP_QUEUE_DURATION, stats.audio_duration, stats.video_duration);
        LogWarn("drop packet -> a: %lld, v: %lld, max: %d", stats.audio_duration, stats.video_duration, max_queue_duration_);
        queue_->Drop(false, max_queue_duration_);       // 从队列头部开始drop
    }
}

/**
 * @brief 写帧推流，但是在写帧之前内部会进行pts的单位转换，转成容器的时基进行推流。
 * @param pkt 编码后的数据包。
 * @param media_type 数据包类型。
 * @return success 0 failed -1.
 */
int RtspPusher::sendPacket(AVPacket *pkt, MediaType media_type)
{
    AVRational src_time_base = {1, 1000};                               // 我们采集、编码 时间戳单位都是ms，所以时基单位写成{1,1000}即可。
    AVRational dst_time_base;                                           // 目的时基单位，即容器时基。

    // 1 时基转换。 写帧之前需要将包的pts进行转换，从编码后的时基单位转成容器的时基单位。
    if(E_VIDEO_TYPE == media_type) {
        pkt->stream_index = video_index_;
        dst_time_base = video_stream_->time_base;                       // 容器的时基单位保存在流的time_base中。例如rtsp的{1,90000}
    } else if(E_AUDIO_TYPE == media_type) {
        pkt->stream_index = audio_index_;
        dst_time_base = audio_stream_->time_base;                       // 音频的audio_stream_->time_base一般是{1,采样率}例如{1,48000}
    } else {
        LogError("unknown mediatype:%d", media_type);
        return -1;
    }
    pkt->pts = av_rescale_q(pkt->pts, src_time_base, dst_time_base);    // 将编码后的包的pts的时基转成容器的时基单位。(pts*1/1000)/(1/90000)=pts*90000/1000=pts*90
    pkt->duration = 0;

    // 2 开始写帧，进行推流。
    RestTiemout();
    int ret = av_write_frame(fmt_ctx_, pkt);
    if(ret < 0) {
        msg_queue_->notify_msg2(MSG_RTSP_ERROR, ret);                   // 服务器断开时，这里就会报错例如Broken Pipe.
        char str_error[512] = {0};
        av_strerror(ret, str_error, sizeof(str_error) -1);
        LogError("av_write_frame failed: %s", str_error);               // 出错没有回调给PushWork？？？ 没有？？？
        return -1;
    }

    return 0;
}

/**
 * @brief   配置视频流信息，从传入的编码器上下文中拷贝该编码器上下文到流中，然后保存该编码器上下文，也会把new出来的流保存。
 * @param   传入的编码器上下文，用于初始化流信息。
 * @return  成功 0 失败 -1
 */
RET_CODE RtspPusher::ConfigVideoStream(const AVCodecContext *ctx)
{
    if(!fmt_ctx_) {
        LogError("fmt_ctx is null");
        return RET_FAIL;
    }
    if(!ctx) {
        LogError("ctx is null");
        return RET_FAIL;
    }
    // 添加视频流
    AVStream *vs = avformat_new_stream(fmt_ctx_, NULL);
    if(!vs) {
        LogError("avformat_new_stream failed");
        return RET_FAIL;
    }
    vs->codecpar->codec_tag = 0;

    // 从编码器上下文拷贝信息
    avcodec_parameters_from_context(vs->codecpar, ctx);         // 这个东西必须在打开io后拷贝，不然可能视频是黑屏的。
    video_ctx_ = (AVCodecContext *) ctx;
    // 保存流信息
    video_stream_ = vs;
    video_index_ = vs->index;                                   // 整个索引非常重要 fmt_ctx_根据index判别 音视频包

    return RET_OK;
}

/**
 * @brief   配置音频流信息，从传入的编码器上下文中拷贝该编码器上下文到流中，然后保存该编码器上下文，也会把new出来的流保存。
 * @param   传入的编码器上下文，用于初始化流信息。
 * @return  成功 0 失败 -1
 */
RET_CODE RtspPusher::ConfigAudioStream(const AVCodecContext *ctx)
{
    if(!fmt_ctx_) {
        LogError("fmt_ctx is null");
        return RET_FAIL;
    }
    if(!ctx) {
        LogError("ctx is null");
        return RET_FAIL;
    }
    // 添加视频流
    AVStream *as = avformat_new_stream(fmt_ctx_, NULL);
    if(!as) {
        LogError("avformat_new_stream failed");
        return RET_FAIL;
    }
    as->codecpar->codec_tag = 0;
    // 从编码器上下文拷贝信息
    avcodec_parameters_from_context(as->codecpar, ctx);             // 这个东西必须在打开io后拷贝，不然可能视频是黑屏的。
    audio_ctx_ = (AVCodecContext *) ctx;
    // 保存流信息
    audio_stream_ = as;
    audio_index_ = as->index;                                       // 整个索引非常重要 fmt_ctx_根据index判别 音视频包

    return RET_OK;
}















