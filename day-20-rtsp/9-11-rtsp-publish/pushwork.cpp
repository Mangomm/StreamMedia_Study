#include <functional>
#include "pushwork.h"
#include "dlog.h"
#include "avpublishtime.h"

PushWork::PushWork(MessageQueue *msg_queue)
    : msg_queue_(msg_queue)
{

}

/**
 * @brief 回收PushWork内的资源，可以看到，类内成员有10个需要回收的，并且按照在init时的开辟处理，使用对应的接口进行回收。
 * @note 其实更建议将这里的内容写到DeInit，这样对接口调用更友好，然后这样只需要再调一次DeInit即可。
 */
PushWork::~PushWork()
{
    // 这里释放资源：
    // 从源头开始释放资源
    // 先释放音频、视频捕获
    // 注意：不是继承关系，所以不用先析构派生再析构基类的做法，这里是同类内的成员析构，不要与这里混淆了。
    if(audio_capturer_) {
        delete audio_capturer_;
        audio_capturer_ = NULL;
    }
    if(video_capturer_) {
        delete video_capturer_;
        video_capturer_ = NULL;
    }
    if(audio_encoder_) {
        delete audio_encoder_;
        audio_encoder_ = NULL;
    }
    if(video_encoder_) {
        delete video_encoder_;
        video_encoder_ = NULL;
    }

    if(fltp_buf_) {// 音频采集线程会使用，所以停了采集线程就可以回收这个buf。
        av_free(fltp_buf_);
    }
    if(pcm_s16le_fp_){// 音频采集线程会使用，所以停了采集线程就可以回收这个描述符。
        fclose(pcm_s16le_fp_);
    }
    if(aac_fp_){// 音频采集线程会使用，所以停了采集线程就可以回收这个描述符。
        fclose(aac_fp_);
    }
    if(yuv_fp_){
        fclose(yuv_fp_);
    }
    if(h264_fp_){
        fclose(h264_fp_);// 视频采集线程会使用，所以停了采集线程就可以回收这个描述符。
    }
    // 音频采集线程会使用，所以停了音频采集线程就可以回收这个buf。虽然音频编码器也用到这个frame，但是采集线程停止后，编码器也会停止使用，所以这个停了采集线程就可以回收。
    // 但是建议还是在音频编码器后回收，毕竟更安全，没必要给自己找麻烦。
    if(audio_frame_) {
        av_frame_free(&audio_frame_);
    }

    if(rtsp_pusher_) {
        delete rtsp_pusher_;
        rtsp_pusher_ = NULL;
    }
    LogInfo("~PushWork()");
}

/**
 * @brief 注意，PushWork的初始化步骤，编码、推流的初始化必须在采集的初始化之前，确保采集到数据后能立马推流，这样时间点更准确；
 *          否则，采集到数据再初始化，由于编码器的初始化时间可能比较长，导致监测的时间点不正常。
 *
 * PushWork的基本流程：
 * 1）音视频的编码器流程：avcodec_find_encoder(avcodec_find_encoder_by_name)->avcodec_alloc_context3->avcodec_open2。
 * 2）rtsp推流器的流程：avformat_network_init->avformat_alloc_output_context2->av_opt_set、interrupt_callback.callback->avformat_new_stream->
 *                      avcodec_parameters_from_context->avformat_write_header。
 *                   一般avformat_network_init都会放在最开头的地方处理，并且处理一次就可以(DH中的FFmpeg处理就是这样)，当然多次也没问题。
 * 3）捕获音视频的流程：没啥流程，就是开启线程，将捕获到的数据，在回调函数中编码后放进队列。
 *
 * @note HK、DH的SDK的FFmpeg流程：
 *      avformat_network_init->avformat_alloc_output_context2->avcodec_find_decoder->avformat_new_stream->av_dict_set、avio_open2->
 *          avcodec_parameters_from_context->avformat_write_header。
 * 可以看到，流程是差不多的，但是SDK的编码器的处理比较单一，并且new stream后，直接复用了stream->codec的编码器，而没有像上面再avcodec_alloc_context3开辟和打开。
 *
 * @param properties 包含音视频采集模块、音视频编码模块、rtsp推流器模块的参数。
 *
 * @return 成功 0，失败 other。
 */
RET_CODE PushWork::Init(const Properties &properties)
{
    int ret = 0;
    // 音频test模式
    audio_test_         = properties.GetProperty("audio_test", 0);
    input_pcm_name_     = properties.GetProperty("input_pcm_name", "input_48k_2ch_s16.pcm");

    // 麦克风采样属性
    mic_sample_rate_    = properties.GetProperty("mic_sample_rate", 48000);
    mic_sample_fmt_     = properties.GetProperty("mic_sample_fmt", AV_SAMPLE_FMT_S16);
    mic_channels_       = properties.GetProperty("mic_channels", 2);

    // 音频编码参数
    audio_sample_rate_  = properties.GetProperty("audio_sample_rate", mic_sample_rate_);
    audio_bitrate_      = properties.GetProperty("audio_bitrate", 128*1024);
    audio_channels_     = properties.GetProperty("audio_channels", mic_channels_);
    audio_ch_layout_    = av_get_default_channel_layout(audio_channels_);                   // 由audio_channels_决定


    // 视频test模式
    video_test_         = properties.GetProperty("video_test", 0);
    input_yuv_name_     = properties.GetProperty("input_yuv_name", "input_1280_720_420p.yuv");

    // 桌面录制属性
    desktop_x_          = properties.GetProperty("desktop_x", 0);
    desktop_y_          = properties.GetProperty("desktop_y", 0);
    desktop_width_      = properties.GetProperty("desktop_width", 1920);
    desktop_height_     = properties.GetProperty("desktop_height", 1080);
    desktop_format_     = properties.GetProperty("desktop_pixel_format", AV_PIX_FMT_YUV420P);
    desktop_fps_        = properties.GetProperty("desktop_fps", 25);

    // 视频编码属性
    video_width_        = properties.GetProperty("video_width", desktop_width_);            // 宽
    video_height_       = properties.GetProperty("video_height", desktop_height_);          // 高
    video_fps_          = properties.GetProperty("video_fps", desktop_fps_);                // 帧率
    video_gop_          = properties.GetProperty("video_gop", video_fps_);
    video_bitrate_      = properties.GetProperty("video_bitrate", 1024*1024);               // 先默认1M fixedme
    video_b_frames_     = properties.GetProperty("video_b_frames", 0);                      // b帧数量

    // rtsp推流属性
    rtsp_url_                   = properties.GetProperty("rtsp_url", "");
    rtsp_transport_             = properties.GetProperty("rtsp_transport", "");
    rtsp_timeout_               = properties.GetProperty("rtsp_timeout", 5000);
    rtsp_max_queue_duration_    = properties.GetProperty("rtsp_max_queue_duration", 500);

    // 初始化publish time，即记录start_time_，但放这里不会有误差吗？个人感觉放在音视频采集Start前更好。
    AVPublishTime::GetInstance()->Rest();                                                   // 推流打时间戳的问题

    // 1 初始化音视频编码器

    // 设置音频编码器，先音频捕获初始化(上面是获取到对应的音视频编码属性，这里是设置)
    audio_encoder_ = new AACEncoder();
    if(!audio_encoder_)
    {
        LogError("new AACEncoder() failed");
        return RET_FAIL;
    }
    Properties  aud_codec_properties;
    aud_codec_properties.SetProperty("sample_rate", audio_sample_rate_);
    aud_codec_properties.SetProperty("channels", audio_channels_);
    aud_codec_properties.SetProperty("bitrate", audio_bitrate_);                            // 这里没有去设置采样格式
    // 需要什么样的采样格式是从编码器读取出来的
    if(audio_encoder_->Init(aud_codec_properties) != RET_OK)
    {
        LogError("AACEncoder Init failed");
        return RET_FAIL;
    }

    int frame_bytes2 = 0;
    // 默认读取出来的数据是s16的，编码器需要的是fltp, 需要做重采样
    // 手动把s16转成fltp
    fltp_buf_size_ = av_samples_get_buffer_size(NULL, audio_encoder_->GetChannels(),
                                                audio_encoder_->GetFrameSamples(),
                                                (enum AVSampleFormat)audio_encoder_->GetFormat(), 1);
    fltp_buf_ = (uint8_t *)av_malloc(fltp_buf_size_);
    if(!fltp_buf_) {
        LogError("fltp_buf_ av_malloc failed");
        return RET_ERR_OUTOFMEMORY;
    }

    audio_frame_ = av_frame_alloc();
    audio_frame_->format = audio_encoder_->GetFormat();
    audio_frame_->format = AV_SAMPLE_FMT_FLTP;
    audio_frame_->nb_samples = audio_encoder_->GetFrameSamples();
    audio_frame_->channels = audio_encoder_->GetChannels();
    audio_frame_->channel_layout = audio_encoder_->GetChannelLayout();
    frame_bytes2  = audio_encoder_->GetFrameBytes();
    if(fltp_buf_size_ != frame_bytes2) {
        LogError("frame_bytes1: %d != frame_bytes2: %d", fltp_buf_size_, frame_bytes2);
        return RET_FAIL;
    }
    ret = av_frame_get_buffer(audio_frame_, 0);
    if(ret < 0) {
        LogError("audio_frame_ av_frame_get_buffer failed");
        return RET_FAIL;
    }

    // 初始化视频编码器
    video_encoder_ = new H264Encoder();
    Properties  vid_codec_properties;
    vid_codec_properties.SetProperty("width", video_width_);
    vid_codec_properties.SetProperty("height", video_height_);
    vid_codec_properties.SetProperty("fps", video_fps_);            // 帧率
    vid_codec_properties.SetProperty("b_frames", video_b_frames_);
    vid_codec_properties.SetProperty("bitrate", video_bitrate_);    // 码率
    vid_codec_properties.SetProperty("gop", video_gop_);            // gop
    if(video_encoder_->Init(vid_codec_properties) != RET_OK)
    {
        LogError("H264Encoder Init failed");
        return RET_FAIL;
    }

    // 2 初始化rtsp推流器。在音视频编码器初始化完， 音视频捕获前
    rtsp_pusher_ = new RtspPusher(msg_queue_);
    if(!rtsp_pusher_) {
        LogError("new RTSPPusher() failed");
        return RET_FAIL;
    }
    Properties  rtsp_properties;
    rtsp_properties.SetProperty("url", rtsp_url_);
    rtsp_properties.SetProperty("timeout", rtsp_timeout_);
    rtsp_properties.SetProperty("rtsp_transport", rtsp_transport_);
    rtsp_properties.SetProperty("max_queue_duration", rtsp_max_queue_duration_);
    if(audio_encoder_) {
        rtsp_properties.SetProperty("audio_frame_duration", audio_encoder_->GetFrameSamples()*1000/audio_encoder_->GetSampleRate());    // 设置音频一帧的时长
    }
    if(video_encoder_) {
        rtsp_properties.SetProperty("video_frame_duration", 1000/video_encoder_->GetFps());                                             // 设置视频一帧的时长
    }

    if(rtsp_pusher_->Init(rtsp_properties) != RET_OK) {// 里面主要是分配AVFormatContext。
        LogError("rtsp_pusher_->Init failed");
        return RET_FAIL;
    }

    // 创建音频流、音视频流
    if(video_encoder_) {
        if(rtsp_pusher_->ConfigVideoStream(video_encoder_->GetCodecContext()) != RET_OK) {
            LogError("rtsp_pusher ConfigVideoSteam failed");
            return RET_FAIL;
        }
    }
    if(audio_encoder_) {
        if(rtsp_pusher_->ConfigAudioStream(audio_encoder_->GetCodecContext()) != RET_OK) {
            LogError("rtsp_pusher ConfigAudioStream failed");
            return RET_FAIL;
        }
    }
    if(rtsp_pusher_->Connect() != RET_OK) {// 这里连接服务器后，rtsp推流器会开启一个线程，不断从packet_queue取数据，没数据时会休眠
        LogError("rtsp_pusher Connect() failed");
        return RET_FAIL;
    }

//    AVPublishTime::GetInstance()->Rest();                                                   // 推流打时间戳的问题

    // 3 设置音视频捕获
    // 设置音频捕获
    audio_capturer_ = new AudioCapturer();
    Properties aud_cap_properties;
    aud_cap_properties.SetProperty("audio_test", 1);
    aud_cap_properties.SetProperty("input_pcm_name", input_pcm_name_);
    aud_cap_properties.SetProperty("channels", mic_channels_);
    aud_cap_properties.SetProperty("nb_samples", 1024);     // 由编码器提供 // fix me
    aud_cap_properties.SetProperty("format", mic_sample_fmt_);
    aud_cap_properties.SetProperty("byte_per_sample", 2);   // fix me，默认读出来的是交错的s16，故固定为2字节。
    if(audio_capturer_->Init(aud_cap_properties) != RET_OK)
    {
        LogError("AudioCapturer Init failed");
        return RET_FAIL;
    }

    // 设置音频回调采集，但是此时还没执行。function+bind实现调用类内函数，std::placeholders::_1、2代表两个参数占位符
    audio_capturer_->AddCallback(std::bind(&PushWork::PcmCallback, this, std::placeholders::_1,
                                           std::placeholders::_2));
    // 这里才是真正的开始采集音频数据
    if(audio_capturer_->Start()!= RET_OK) {
        LogError("AudioCapturer Start failed");
        return RET_FAIL;
    }

    // 设置视频捕获
    video_capturer_ = new VideoCapturer();
    Properties  vid_cap_properties;
    vid_cap_properties.SetProperty("video_test", 1);
    vid_cap_properties.SetProperty("input_yuv_name", input_yuv_name_);
    vid_cap_properties.SetProperty("width", desktop_width_);
    vid_cap_properties.SetProperty("height", desktop_height_);
    if(video_capturer_->Init(vid_cap_properties) != RET_OK)
    {
        LogError("VideoCapturer Init failed");
        return RET_FAIL;
    }
    //    video_nalu_buf = new uint8_t[VIDEO_NALU_BUF_MAX_SIZE];

    video_capturer_->AddCallback(std::bind(&PushWork::YuvCallback, this,
                                           std::placeholders::_1,
                                           std::placeholders::_2));
    if(video_capturer_->Start()!= RET_OK) {
        LogError("VideoCapturer Start failed");
        return RET_FAIL;
    }

    return RET_OK;
}

/**
 * @brief 回收音视频采集器。DeInit目前这样写并没意义并且暂未被调用，后续可以将析构的内容弄到这里。
 * @return no mean.
 */
RET_CODE PushWork::DeInit()
{
    if(audio_capturer_) {
        audio_capturer_->Stop();
        delete audio_capturer_;
        audio_capturer_ = NULL;
    }
    if(video_capturer_){
        video_capturer_->Stop();
        delete video_capturer_;
        video_capturer_ = NULL;
    }
    return RET_OK;
}

/**
 * @brief 只支持2通道 s16交错模式 -> float planar格式。
 * @param s16le s16的pcm数据。
 * @param fltp 输出参数，转换fltp32后的输出缓冲区。
 * @param nb_samples 采样点个数。
 * @return void。
 */
void s16le_convert_to_fltp(short *s16le, float *fltp, int nb_samples) {
    float *fltp_l = fltp;   // -1~1
    float *fltp_r = fltp + nb_samples;
    for(int i = 0; i < nb_samples; i++) {
        fltp_l[i] = s16le[i*2] / 32768.0;     // 0 2 4，除以32768的原因是：需要将s16两字节的内容转成浮点数比例，有符号的两字节是：0x7fff=32767,0~32767共32768个，故除以它。
        fltp_r[i] = s16le[i*2+1] / 32768.0;   // 1 3 5
    }
}

/**
 * @brief 音频回调，将读取出来的s16数据转成fltp32，并编码成aac后push到packet_queue队列中。
 * @param pcm 读出来的pcm数据。
 * @param size pcm数据的大小。
 * @return void。
 */
void PushWork::PcmCallback(uint8_t *pcm, int32_t size)
{
    int ret = 0;
    if(!pcm_s16le_fp_)
    {
        pcm_s16le_fp_ = fopen("push_dump_s16le.pcm", "wb");
    }
    if(pcm_s16le_fp_)
    {
        // ffplay -ar 48000 -channels 2 -f s16le  -i push_dump_s16le.pcm
        fwrite(pcm, 1, size, pcm_s16le_fp_);
        fflush(pcm_s16le_fp_);// 冲刷文件描述符
    }

    // 这里就约定好，音频捕获的时候，采样点数和编码器需要的点数是一样的
    s16le_convert_to_fltp((short *)pcm, (float *)fltp_buf_, audio_frame_->nb_samples);
    ret = av_frame_make_writable(audio_frame_);
    if(ret < 0) {
        LogError("av_frame_make_writable failed");
        return;
    }
    // 将fltp_buf_写入frame
    ret = av_samples_fill_arrays(audio_frame_->data,
                                 audio_frame_->linesize,
                                 fltp_buf_,
                                 audio_frame_->channels,
                                 audio_frame_->nb_samples,
                                 (AVSampleFormat)audio_frame_->format,
                                 0);
    if(ret < 0) {
        LogError("av_samples_fill_arrays failed");
        return;
    }

    // 获取从开始到目前的pts总时长，对比上面的AVPublishTime::GetInstance()->Rest()
    int64_t pts = (int64_t)AVPublishTime::GetInstance()->get_audio_pts();
    int pkt_frame = 0;
    RET_CODE encode_ret = RET_OK;
    AVPacket *packet = audio_encoder_->Encode(audio_frame_, pts, 0, &pkt_frame, &encode_ret);// 他这里打时间戳pts是帧间隔+系统时间去打。当误差过大就会使用系统时间
    // dump编码后的音频数据，方便出问题时排查
    if(encode_ret == RET_OK && packet) {
        if(!aac_fp_) {
            aac_fp_ = fopen("push_dump.aac", "wb");
            if(!aac_fp_) {
                LogError("fopen push_dump.aac failed");
                return;
            }
        }
        if(aac_fp_) {
            uint8_t adts_header[7];
            if(audio_encoder_->GetAdtsHeader(adts_header, packet->size) != RET_OK) {
                LogError("GetAdtsHeader failed");
                return;
            }
            fwrite(adts_header, 1, 7, aac_fp_);
            fwrite(packet->data, 1, packet->size, aac_fp_);
        }
    }

    // 将编码后的音频数据包放进packet_queue队列
    //    LogInfo("PcmCallback pts: %ld", pts);
    if(packet) {
    //    LogInfo("PcmCallback packet->pts: %ld", packet->pts);
        rtsp_pusher_->Push(packet, E_AUDIO_TYPE);
    }else {
        LogInfo("audio_encoder_ packet is null");
    }
}

/**
 * @brief 视频回调，将读取出来的yuv数据编码成h264后，push到packet_queue队列中。
 * @param yuv 读出来的yuv数据。
 * @param size yuv数据的大小。
 * @return void。
 */
void PushWork::YuvCallback(uint8_t *yuv, int32_t size)
{
    // yuv视频数据不需要类似音频s16转fltp的做法，直接编码即可。
    if(!yuv_fp_)
    {
        yuv_fp_ = fopen("push_dump.yuv", "wb");
    }
    if(yuv_fp_)
    {
        // ffplay -f rawvideo -video_size 768x480 push_dump.yuv
        fwrite(yuv, 1, size, yuv_fp_);
        fflush(yuv_fp_);// 冲刷文件描述符
    }


    // LogInfo("YuvCallback size: %d", size);
    int64_t pts = (int64_t)AVPublishTime::GetInstance()->get_video_pts();
    int pkt_frame = 0;
    RET_CODE encode_ret = RET_OK;
    AVPacket *packet = video_encoder_->Encode(yuv, size, pts,  &pkt_frame, &encode_ret);
    if(encode_ret == RET_OK && packet) {
        if(!h264_fp_) {
            h264_fp_ = fopen("push_dump.h264", "wb");
            if(!h264_fp_) {
                LogError("fopen push_dump.h264 failed");
                return;
            }
            // 写入sps 和 pps(只需要开头写一次)
            uint8_t start_code[] = {0, 0, 0, 1};
            fwrite(start_code, 1, 4, h264_fp_);
            fwrite(video_encoder_->get_sps_data(), 1, video_encoder_->get_sps_size(), h264_fp_);
            fwrite(start_code, 1, 4, h264_fp_);
            fwrite(video_encoder_->get_pps_data(), 1, video_encoder_->get_pps_size(), h264_fp_);
        }

        fwrite(packet->data, 1,  packet->size, h264_fp_);
        fflush(h264_fp_);
    }else{
        LogError("============encode_ret: %d, size: %d==============", encode_ret, size);
    }

    // 将编码后的视频数据包放进packet_queue队列。并且看到，队列中的音视频包不一定是音频-视频-音频-视频...的顺序存放，它是不确定的，看两个采集线程的读取速度。
    //    LogInfo("YuvCallback pts: %ld", pts);
    if(packet) {
    //    LogInfo("YuvCallback packet->pts: %ld", packet->pts);
        rtsp_pusher_->Push(packet, E_VIDEO_TYPE);
    }else {
        LogInfo("video_encoder_ packet is null");
    }
}
