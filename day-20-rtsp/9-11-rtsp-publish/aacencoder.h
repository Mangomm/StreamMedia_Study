#ifndef AACENCODER_H
#define AACENCODER_H

extern "C" {
#include <libavcodec/avcodec.h>
}
#include "mediabase.h"

class AACEncoder
{
public:
    AACEncoder();
    virtual ~AACEncoder();

    RET_CODE Init(const Properties &properties);

    virtual AVPacket *Encode(AVFrame *frame, const int64_t pts, int flush, int *pkt_frame, RET_CODE *ret);

    RET_CODE GetAdtsHeader(uint8_t *adts_header, int aac_length);

    // 获取编码器内部的一些信息，注意最好别从本类的成员去返回，应该从编码器内部返回，因为都已经初始化编码器完毕并且在使用了
    virtual int GetFormat() {
        return ctx_->sample_fmt;
    }
    virtual int GetChannels() {
        return ctx_->channels;
    }
    virtual int GetChannelLayout() {
        return ctx_->channel_layout;
    }
    virtual int GetFrameSamples() {         // 一帧的采样点数量，只是说的一个通道，例如1024
        return ctx_->frame_size;
    }
    virtual int GetSampleRate() {           // 一秒的采样点数量，只是说的一个通道，即采用频率，例如48khz
        return ctx_->sample_rate;
    }
    // 一帧占用的字节数
    virtual int GetFrameBytes() {
        return av_get_bytes_per_sample(ctx_->sample_fmt) * ctx_->channels * ctx_->frame_size;// frame_size是一帧的采样点数
    }

    AVCodecContext *GetCodecContext() {
        return ctx_;
    }

//    virtual RET_CODE EncodeInput(const AVFrame *frame);
//    virtual RET_CODE EncodeOutput(AVPacket *pkt);

private:
    int sample_rate_    = 48000;
    int channels_       = 2;
    int channel_layout_ = AV_CH_LAYOUT_STEREO;
    int bitrate_        = 128*1024;    // 码率128k，刚好128*8=1024=1M带宽

    AVCodec *codec_         = NULL;
    AVCodecContext  *ctx_   = NULL;

};

#endif // AACENCODER_H
