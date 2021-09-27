#include "aacencoder.h"
#include "dlog.h"


AACEncoder::AACEncoder()
{

}

AACEncoder::~AACEncoder()
{
    if(ctx_) {
        avcodec_free_context(&ctx_);
    }

    //编码器codec_应该是不需要我们处理的，因为没有alloc
}

/**
 * @brief 初始化相关参数和关联好编码器与编码器上下文。
 *
 * @param "sample_rate"     采样率，默认48000
 *        "channels"        通道数量 ，默认2
 *        "channel_layout"  通道布局，默认根据channels获取缺省的
 *        "bitrate"         比特率，默认128*1024
 *
 * @return 成功 0 失败 other
 */
RET_CODE AACEncoder::Init(const Properties &properties)
{
    // 获取上层(pushwork)传入的参数
    sample_rate_    = properties.GetProperty("sample_rate", 48000);
    channels_       = properties.GetProperty("channels", 2);
    channel_layout_ = properties.GetProperty("channel_layout",
                                             (int)av_get_default_channel_layout(channels_));
    bitrate_        = properties.GetProperty("bitrate", 128 * 1024);

    // 1 查找编码器
    codec_ = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if(!codec_) {
        LogError("AAC: No encoder found");
        return RET_ERR_MISMATCH_CODE;
    }
    // 2 分配编码器上下文
    ctx_ = avcodec_alloc_context3(codec_);
    if(!ctx_) {
        LogError("AAC: avcodec_alloc_context3 failed");
        return RET_ERR_OUTOFMEMORY;
    }
    // 2.1 设置参数
    ctx_->channels      = channels_;
    ctx_->channel_layout= channel_layout_;
    ctx_->sample_fmt    = AV_SAMPLE_FMT_FLTP;                   // 默认aac编码需要planar格式PCM， 如果是fdk-aac
    ctx_->sample_rate   = sample_rate_;
    ctx_->bit_rate      = bitrate_;
    ctx_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;   //allow experimental codecs

    // 3 编码器与编码器上下文关联.
    // 注意区分与SDK的步骤：
    // 1）编码调用avcodec_open2，SDK中同样是在编码调用avformat_new_stream(this->m_outputContext, codec)关联。
    // 2）并且注意和打开网络流avio_open2函数不太一样，avio_open2也是在编码中使用。
    //      调用方式：avio_open2(&m_outputContext->pb, outUrl.c_str(), AVIO_FLAG_READ_WRITE, nullptr, &opts);
    // 不过实际这些带codec的在编码或者解码都是可以用的，这里只不过和之前的流程区分一下
    if(avcodec_open2(ctx_, codec_, NULL) < 0) {
        LogError("AAC: can't avcodec_open2");
        avcodec_free_context(&ctx_);
        return RET_FAIL;
    }

    return RET_OK;
}

/**
 * @brief 编码一帧进行返回，编码前会为采集到的frame打上时间戳。
 * @param frame         输入帧，用于编码。
 * @param pts           时间戳，用于给采集到的帧打上时间戳，即编码前的pts。后续与编码后的时间戳进行对比，是很重点。
 * @param flush         是否flush，将编码器剩余的帧冲刷。
 * @param pkt_frame     方便查看receive_packet还是send_frame的错误。0 receive_packet报错; 1 send_frame报错。
 * @param ret           只有RET_OK才不需要做异常处理。
 * @return              每次返回一个AVPacket或者NULL，返回值的判断具体需要看ret。
 *
 * 后续可以参考之前的编码音频去优化本函数。
 */
AVPacket *AACEncoder::Encode(AVFrame *frame, const int64_t pts, int flush, int *pkt_frame, RET_CODE *ret)
{
    int ret1 = 0;
    *pkt_frame = 0;
    if(!ctx_) {
        *ret = RET_FAIL;
        LogError("AAC: no context");
        return NULL;
    }

    // 1 发送帧去编码
    if(frame) {

        frame->pts = pts;                               // 打上编码前的时间戳

        ret1 = avcodec_send_frame(ctx_, frame);
        //av_frame_unref(frame);
        if(ret1 < 0) {                                  // <0 不能正常处理该frame
            char buf[1024] = { 0 };
            av_strerror(ret1, buf, sizeof(buf) - 1);
            LogError("avcodec_send_frame failed:%s", buf);
            *pkt_frame = 1;                             // 标记avcodec_send_frame报错
            if(ret1 == AVERROR(EAGAIN)) {               // 你赶紧读取packet，我frame send不进去了
                *ret = RET_ERR_EAGAIN;
                return NULL;
            } else if(ret1 == AVERROR_EOF) {
                *ret = RET_ERR_EOF;
                return NULL;
            } else {
                *ret = RET_FAIL;                        // 真正报错，这个encoder就只能销毁了
                return NULL;
            }
        }
    }

    // 2 冲刷，不过听课时没啥用，后期参考之前的音频编码的冲刷方式优化处理。
    if(flush) {     // 只能调用一次
        avcodec_flush_buffers(ctx_);
    }

    // 3 接收编码后的一个AVPacket数据，并返回该AVPacket
    AVPacket *packet = av_packet_alloc();
    ret1 = avcodec_receive_packet(ctx_, packet);
    if(ret1 < 0) {
        LogError("AAC: avcodec_receive_packet ret:%d", ret1);
        av_packet_free(&packet);
        *pkt_frame = 0;
        if(ret1 == AVERROR(EAGAIN)) {                       // 需要继续发送 frame 我们才有packet读取
            *ret = RET_ERR_EAGAIN;
            return NULL;
        }else if(ret1 == AVERROR_EOF) {
            *ret = RET_ERR_EOF;                             // 不能在读取出来packet来了，读到文件尾部了，实时视频一般不会
            return NULL;
        } else {
            *ret = RET_FAIL;                                // 真正报错，这个encoder就只能销毁了
            return NULL;
        }
    }else {
        *ret = RET_OK;
        return packet;
    }
}

/**
 * @brief 采集到的数据是不带adts头部的，需要为aac条件adts头。
 * @param adts_header 7字节的adts头部。
 * @param aac_length aac裸流的长度，用于给adts的内部字段赋值。
 * @return success 0 fail return a negative num.
*/
RET_CODE AACEncoder::GetAdtsHeader(uint8_t *adts_header, int aac_length)
{
    uint8_t freqIdx = 0;    //0: 96000 Hz  3: 48000 Hz 4: 44100 Hz

    switch (ctx_->sample_rate)
    {
    case 96000: freqIdx = 0; break;
    case 88200: freqIdx = 1; break;
    case 64000: freqIdx = 2; break;
    case 48000: freqIdx = 3; break;
    case 44100: freqIdx = 4; break;
    case 32000: freqIdx = 5; break;
    case 24000: freqIdx = 6; break;
    case 22050: freqIdx = 7; break;
    case 16000: freqIdx = 8; break;
    case 12000: freqIdx = 9; break;
    case 11025: freqIdx = 10; break;
    case 8000: freqIdx = 11; break;
    case 7350: freqIdx = 12; break;
    default:
        LogError("can't support sample_rate:%d");
        freqIdx = 4;
        return RET_FAIL;
    }

    uint8_t ch_cfg = ctx_->channels;
    uint32_t frame_length = aac_length + 7;
    adts_header[0] = 0xFF;
    adts_header[1] = 0xF1;
    adts_header[2] = ((ctx_->profile) << 6) + (freqIdx << 2) + (ch_cfg >> 2);
    adts_header[3] = (((ch_cfg & 3) << 6) + (frame_length  >> 11));
    adts_header[4] = ((frame_length & 0x7FF) >> 3);
    adts_header[5] = (((frame_length & 7) << 5) + 0x1F);
    adts_header[6] = 0xFC;

    return RET_OK;
}















