#ifndef H264ENCODER_H
#define H264ENCODER_H
#include "mediabase.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

class H264Encoder
{
public:
    H264Encoder();
    virtual ~H264Encoder();

    virtual int Init(const Properties &properties);

    virtual AVPacket *Encode(uint8_t *yuv, int size, int64_t pts, int *pkt_frame, RET_CODE *ret);

    inline uint8_t *get_sps_data() {
        return (uint8_t *)sps_.c_str();
    }
    inline int get_sps_size(){
        return sps_.size();
    }
    inline uint8_t *get_pps_data() {
        return (uint8_t *)pps_.c_str();
    }
    inline int get_pps_size(){
        return pps_.size();
    }
    inline int GetFps() {
        return fps_;
    }
    AVCodecContext *GetCodecContext() {
        return ctx_;
    }

private:
    int width_ = 0;
    int height_ = 0;
    int fps_ = 0;                                               // 帧率
    int b_frames_ = 0;                                          // 连续B帧的数量
    int bitrate_ = 0;                                           // 码率
    int gop_ = 0;                                               // gop
    bool annexb_  = false;
    int threads_ = 1;
    int pix_fmt_ = 0;
    //    std::string profile_;
    //    std::string level_id_;

    std::string sps_;
    std::string pps_;
    std::string codec_name_;                                    // 编码器名字
    AVCodec *codec_         = NULL;
    AVCodecContext  *ctx_   = NULL;
    AVDictionary *dict_     = NULL;                             // 编码器的选项设置

    AVFrame *frame_         = NULL;
};

#endif // H264ENCODER_H
