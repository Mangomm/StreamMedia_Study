#include "h264encoder.h"
#include "dlog.h"


H264Encoder::H264Encoder()
{
}

H264Encoder::~H264Encoder()
{
    if(ctx_) {
        avcodec_free_context(&ctx_);
    }
    if(frame_) {
        av_frame_free(&frame_);
    }
}

/**
 * @brief Init 相关参数，查找编码器以及关联编码器上下文，保存sps、pps，与开辟帧内存。
 * @param   width       宽
 *          height      高
 *          fps         帧率
 *          b_frames    b帧连续数量
 *          bitrate     比特率
 *          gop         多少帧有一个I帧
 *          pix_fmt     像素格式
 * @return 成功 0 失败 -1
 */
int H264Encoder::Init(const Properties &properties)
{
    int ret = 0;

    // 获取初始化参数
    width_ = properties.GetProperty("width", 0);
    if(width_ == 0 || (width_ % 2) != 0) {
        LogError("width: %d", width_);
        return RET_ERR_NOT_SUPPORT;
    }
    height_ = properties.GetProperty("height", 0);
    if(height_ == 0 || (height_ % 2) != 0) {
        LogError("height: %d", height_);
        return RET_ERR_NOT_SUPPORT;
    }
    fps_        = properties.GetProperty("fps", 25);
    b_frames_   = properties.GetProperty("b_frames", 0);
    bitrate_    = properties.GetProperty("bitrate", 500*1024);
    gop_        = properties.GetProperty("gop", fps_);                      // 默认与帧率一样即可，gop过大会影响首帧秒开
    pix_fmt_    = properties.GetProperty("pix_fmt", AV_PIX_FMT_YUV420P);

    // 1 查找H264编码器 确定是否存在
    codec_name_ = properties.GetProperty("codec_name", "default");
    if(codec_name_ == "default") {
        LogInfo("use default encoder");
        codec_ = avcodec_find_encoder(AV_CODEC_ID_H264);
    } else {
        LogInfo("use %s encoder", codec_name_.c_str());
        codec_ = avcodec_find_encoder_by_name(codec_name_.c_str());
    }
    if(!codec_) {
        LogError("can't find encoder");
        return RET_FAIL;
    }

    // 2 分配编码器上下文
    ctx_ = avcodec_alloc_context3(codec_);
    if(!ctx_) {
        LogError("ctx_ h264 avcodec_alloc_context3 failed");
        return RET_FAIL;
    }
    // 2.1 设置参数
    // 宽高
    ctx_->width = width_;
    ctx_->height = height_;
    // 码率
    ctx_->bit_rate = bitrate_;
    // gop
    ctx_->gop_size = gop_;
    // 帧率
    ctx_->framerate.num = fps_;
    ctx_->framerate.den = 1;
    // time_base，与帧率互为相反数即可
    ctx_->time_base.num = 1;
    ctx_->time_base.den = fps_;
    // 像素格式
    ctx_->pix_fmt = (AVPixelFormat)pix_fmt_;
    // 编码类型
    ctx_->codec_type = AVMEDIA_TYPE_VIDEO;
    ctx_->max_b_frames = b_frames_;
    // 设置preset，tune，profile等参数
    av_dict_set(&dict_, "preset", "medium", 0);
    av_dict_set(&dict_, "tune", "zerolatency", 0);
    av_dict_set(&dict_, "profile", "high", 0);

    ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // 3 编码器与编码器上下文关联.
    // 注意区分与SDK的步骤：
    // 1）编码调用avcodec_open2，SDK中同样是在编码调用avformat_new_stream(this->m_outputContext, codec)关联。
    // 2）并且注意和打开网络流avio_open2函数不太一样，avio_open2也是在编码中使用。
    //      调用方式：avio_open2(&m_outputContext->pb, outUrl.c_str(), AVIO_FLAG_READ_WRITE, nullptr, &opts);
    // 不过实际这些带codec的在编码或者解码都是可以用的，这里只不过和之前的流程区分一下
    ret = avcodec_open2(ctx_, codec_, &dict_);
    if(ret < 0) {
        char buf[1024] = { 0 };
        av_strerror(ret, buf, sizeof(buf) - 1);
        LogError("avcodec_open2 failed:%s", buf);
        return RET_FAIL;
    }
    // 3.1 从上下文的extradata读取sps pps  其中sps、pps都是带起始码的
    if(ctx_->extradata) {
        LogInfo("extradata_size:%d", ctx_->extradata_size);
        // 第一个为sps 7
        // 第二个为pps 8

        uint8_t *sps = ctx_->extradata + 4;                     // 直接跳到数据，写死是4字节的起始码
        int sps_len = 0;
        uint8_t *pps = NULL;
        int pps_len = 0;
        // 寻找pps的数据位置
        uint8_t *data = ctx_->extradata + 4;
        for (int i = 0; i < ctx_->extradata_size - 4; ++i)
        {
            if (0 == data[i] && 0 == data[i + 1] && 0 == data[i + 2] && 1 == data[i + 3])
            {
                pps = &data[i+4];
                break;
            }
        }

        sps_len = int(pps - sps) - 4;                           // 4是00 00 00 01占用的字节
        pps_len = ctx_->extradata_size - 4*2 - sps_len;         // 总大小减去两个起始码与sps长度就是pps的长度
        sps_.append(sps, sps + sps_len);                        // 拷贝数据
        pps_.append(pps, pps + pps_len);
    }

    // 4 开辟帧及其帧内部的缓存
    frame_ = av_frame_alloc();
    frame_->width = width_;
    frame_->height = height_;
    frame_->format = ctx_->pix_fmt;
    ret = av_frame_get_buffer(frame_, 0);

    return RET_OK;
}

/**
 * @brief 编码一帧进行返回，编码前会为采集到的frame打上时间戳。
 * @param yuv           输入帧，用于编码。
 * @param size          输入帧大小。
 * @param pts           时间戳，用于给采集到的帧打上时间戳，即编码前的pts。后续与编码后的时间戳进行对比，是很重点。

 * @param pkt_frame     方便查看receive_packet还是send_frame的错误。0 receive_packet报错; 1 send_frame报错。
 * @param ret           只有RET_OK才不需要做异常处理。
 * @return              每次返回一个AVPacket或者NULL，返回值的判断具体需要看ret。
 *
 * 后续可以参考之前的编码音频去优化本函数。
 */
AVPacket *H264Encoder::Encode(uint8_t *yuv, int size, int64_t pts, int *pkt_frame,RET_CODE *ret)
{
    int ret1 = 0;
    *ret = RET_OK;
    *pkt_frame = -1;

    // 发送帧去编码
    if(yuv) {
        int need_size = 0;
        /* 依据src，开辟对应的缓存到data数组，成功返回src需要的大小，失败返回负数 */
        need_size = av_image_fill_arrays(frame_->data, frame_->linesize, yuv,
                                         (AVPixelFormat)frame_->format,
                                         frame_->width, frame_->height, 1);
        if(need_size != size)  {// 不等于直接返回错误
            LogError("need_size:%d != size:%d", need_size, size);
            *ret = RET_FAIL;
            return NULL;
        }

        frame_->pts = pts;
        frame_->pict_type = AV_PICTURE_TYPE_NONE;
        ret1 = avcodec_send_frame(ctx_, frame_);
    } else {
        // 冲刷
        ret1 = avcodec_send_frame(ctx_, NULL);
    }
    if(ret1 < 0) {  // <0 不能正常处理该frame
        char buf[1024] = { 0 };
        av_strerror(ret1, buf, sizeof(buf) - 1);
        LogError("avcodec_send_frame failed: %s", buf);
        *pkt_frame = 1;
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

    // 接收编码后的一个AVPacket数据，并返回该AVPacket
    AVPacket *packet = av_packet_alloc();
    ret1 = avcodec_receive_packet(ctx_, packet);
    if(ret1 < 0) {
        LogError("AAC: avcodec_receive_packet ret:%d", ret1);
        av_packet_free(&packet);
        *pkt_frame = 0;
        if(ret1 == AVERROR(EAGAIN)) {               // 需要继续发送frame我们才有packet读取
            *ret = RET_ERR_EAGAIN;
            return NULL;
        }else if(ret1 == AVERROR_EOF) {
            *ret = RET_ERR_EOF;                     // 结尾，不能在读取出来packet来了
            return NULL;
        } else {
            *ret = RET_FAIL;                        // 真正报错，这个encoder编码器就只能销毁了
            return NULL;
        }
    }else {
        *ret = RET_OK;
        return packet;
    }
}
