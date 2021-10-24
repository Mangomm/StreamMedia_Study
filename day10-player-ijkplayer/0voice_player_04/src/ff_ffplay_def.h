#ifndef FF_FFPLAY_DEF_H
#define FF_FFPLAY_DEF_H

// ffplay.c
#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>

#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libavutil/time.h"
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"
#include "SDL.h"

#include "ff_ffmsg_queue.h"

#ifdef __cplusplus //如果 __cplusplus被定义 ，__cplusplus只有C++中才有 成立则表示为C++编译器
                    //使用__cplusplus来判定是否需要将  extern "C"{}加入到编译中来。实现同一段代可以使用C、C++编译器编译
extern "C"
{
#endif
#define FFP_MERGE 1

//ijkplayer  begin
#define DEFAULT_HIGH_WATER_MARK_IN_BYTES        (256 * 1024)

/*
 * START: buffering after prepared/seeked
 * NEXT:  buffering for the second time after START
 * MAX:   ...
 */
#define DEFAULT_FIRST_HIGH_WATER_MARK_IN_MS     (100)
#define DEFAULT_NEXT_HIGH_WATER_MARK_IN_MS      (1 * 1000)
#define DEFAULT_LAST_HIGH_WATER_MARK_IN_MS      (5 * 1000)

#define BUFFERING_CHECK_PER_BYTES               (512)
#define BUFFERING_CHECK_PER_MILLISECONDS        (500)
#define FAST_BUFFERING_CHECK_PER_MILLISECONDS   (50)
#define MAX_RETRY_CONVERT_IMAGE                 (3)
//ijkplayer end

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04  // 40ms
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1   // 100ms
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.040
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

static unsigned sws_flags = SWS_BICUBIC;

//ijkplayer begin
#define HD_IMAGE 2  // 640*360
#define SD_IMAGE 1  // 320*180
#define LD_IMAGE 0  // 160*90
#define MAX_DEVIATION 1200000   // 1200ms

typedef struct GetImgInfo {
    char *img_path;
    int64_t start_time;
    int64_t end_time;
    int64_t frame_interval;
    int num;
    int count;
    int width;
    int height;
    AVCodecContext *frame_img_codec_ctx;
    struct SwsContext *frame_img_convert_ctx;
} GetImgInfo;
//ijkplayer end

typedef struct MyAVPacketList {
    AVPacket		pkt;    //解封装后的数据
    struct MyAVPacketList	*next;  //下一个节点
    int			serial;     //播放序列
} MyAVPacketList;

typedef struct PacketQueue {
    MyAVPacketList	*first_pkt, *last_pkt;  // 队首，队尾指针
    int		nb_packets;   // 包数量，也就是队列元素数量
    int		size;         // 队列所有元素的数据大小总和
    int64_t		duration; // 队列所有元素的数据播放持续时间
    int		abort_request; // 用户退出请求标志
    int		serial;         // 播放序列号，和MyAVPacketList的serial作用相同，但改变的时序稍微有点不同
    SDL_mutex	*mutex;     // 用于维持PacketQueue的多线程安全(SDL_mutex可以按pthread_mutex_t理解）
    SDL_cond	*cond;      // 用于读、写线程相互通知(SDL_cond可以按pthread_cond_t理解)
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE	3       // 图像帧缓存数量
#define SUBPICTURE_QUEUE_SIZE		16      // 字幕帧缓存数量
#define SAMPLE_QUEUE_SIZE           9       // 采样帧缓存数量
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct AudioParams {
    int			freq;                   // 采样率
    int			channels;               // 通道数
    int64_t		channel_layout;         // 通道布局，比如2.1声道，5.1声道等
    enum AVSampleFormat	fmt;            // 音频采样格式，比如AV_SAMPLE_FMT_S16表示为有符号16bit深度，交错排列模式。
    int			frame_size;             // 一个采样单元占用的字节数（比如2通道时，则左右通道各采样一次合成一个采样单元）
    int			bytes_per_sec;          // 一秒时间的字节数，比如采样率48Khz，2 channel，16bit，则一秒48000*2*16/8=192000
} AudioParams;

#define VIDEO_PICTURE_QUEUE_SIZE_MIN        (3)
#define VIDEO_PICTURE_QUEUE_SIZE_MAX        (16)
#define VIDEO_PICTURE_QUEUE_SIZE_DEFAULT    (VIDEO_PICTURE_QUEUE_SIZE_MIN)
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE_MAX, SUBPICTURE_QUEUE_SIZE))

#define VIDEO_MAX_FPS_DEFAULT 30

// 这里讲的系统时钟 是通过av_gettime_relative()获取到的时钟，单位为微妙
typedef struct Clock {
    double	pts;            // 时钟基础, 当前帧(待播放)显示时间戳，播放后，当前帧变成上一帧
    // 当前pts与当前系统时钟的差值, audio、video对于该值是独立的
    double	pts_drift;      // clock base minus time at which we updated the clock
    // 当前时钟(如视频时钟)最后一次更新时间，也可称当前时钟时间
    double	last_updated;   // 最后一次更新的系统时钟
    double	speed;          // 时钟速度控制，用于控制播放速度
    // 播放序列，所谓播放序列就是一段连续的播放动作，一个seek操作会启动一段新的播放序列
    int	serial;             // clock is based on a packet with this serial
    int	paused;             // = 1 说明是暂停状态
    // 指向packet_serial
    int *queue_serial;      /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

/* Common struct for handling all types of decoded data and allocated render buffers. */
// 用于缓存解码后的数据
typedef struct Frame {
    AVFrame		*frame;         // 指向数据帧
    AVSubtitle	sub;            // 用于字幕
    int		serial;             // 帧序列，在seek的操作时serial会变化
    double		pts;            // 时间戳，单位为秒
    double		duration;       // 该帧持续时间，单位为秒
    int64_t		pos;            // 该帧在输入文件中的字节位置
#ifdef FFP_MERGE
    SDL_Texture *bmp;
#else
    SDL_VoutOverlay *bmp;
#endif
    int     allocated;          // 是否分配
    int		width;              // 图像宽度
    int		height;             // 图像高读
    int		format;             // 对于图像为(enum AVPixelFormat)，
    // 对于声音则为(enum AVSampleFormat)
    AVRational	sar;            // 图像的宽高比（16:9，4:3...），如果未知或未指定则为0/1
    int		uploaded;           // 用来记录该帧是否已经显示过？
    int		flip_v;             // =1则垂直翻转， = 0则正常播放
} Frame;

/* 这是一个循环队列，windex是指其中的首元素，rindex是指其中的尾部元素. */
typedef struct FrameQueue {
    Frame	queue[FRAME_QUEUE_SIZE];        // FRAME_QUEUE_SIZE  最大size, 数字太大时会占用大量的内存，需要注意该值的设置
    int		rindex;                         // 读索引。待播放时读取此帧进行播放，播放后此帧成为上一帧
    int		windex;                         // 写索引
    int		size;                           // 当前总帧数
    int		max_size;                       // 可存储最大帧数
    int		keep_last;                      // = 1说明要在队列里面保持最后一帧的数据不释放，只在销毁队列的时候才将其真正释放
    int		rindex_shown;                   // 初始化为0，配合keep_last=1使用
    SDL_mutex	*mutex;                     // 互斥量
    SDL_cond	*cond;                      // 条件变量
    PacketQueue	*pktq;                      // 数据包缓冲队列
} FrameQueue;

/**
 *音视频同步方式，缺省以音频为基准
 */
enum {
    AV_SYNC_AUDIO_MASTER,                   // 以音频为基准
    AV_SYNC_VIDEO_MASTER,                   // 以视频为基准
    AV_SYNC_EXTERNAL_CLOCK,                 // 以外部时钟为基准，synchronize to an external clock */
};

/**
 * 解码器封装
 */
typedef struct Decoder {
    AVPacket pkt;
    AVPacket pkt_temp;
    PacketQueue	*queue;         // 数据包队列
    AVCodecContext	*avctx;     // 解码器上下文
    int		pkt_serial;         // 包序列
    int		finished;           // =0，解码器处于工作状态；=非0，解码器处于空闲状态
    int		packet_pending;     // =0，解码器处于异常状态，需要考虑重置解码器；=1，解码器处于正常状态
    SDL_cond	*empty_queue_cond;  // 检查到packet队列空时发送 signal缓存read_thread读取数据
    int64_t		start_pts;          // 初始化时是stream的start time
    AVRational	start_pts_tb;       // 初始化时是stream的time_base
    int64_t		next_pts;           // 记录最近一次解码后的frame的pts，当解出来的部分帧没有有效的pts时则使用next_pts进行推算
    AVRational	next_pts_tb;        // next_pts的单位
    SDL_Thread	*decoder_tid;       // 线程句柄
//    SDL_Thread _decoder_tid;

    //    SDL_Profiler decode_profiler;
    //        Uint64 first_frame_decoded_time;
    //        int    first_frame_decoded;
} Decoder;

enum ShowMode {
    SHOW_MODE_NONE = -1,    // 无显示
    SHOW_MODE_VIDEO = 0,    // 显示视频
    SHOW_MODE_WAVES,        // 显示波浪，音频
    SHOW_MODE_RDFT,         // 自适应滤波器
    SHOW_MODE_NB
};

typedef struct VideoState {
    SDL_Thread	*read_tid;      // 读线程句柄
//    SDL_Thread _read_tid;
    AVInputFormat	*iformat;   // 指向demuxer
    int		abort_request;      // =1时请求退出播放
    int		force_refresh;      // =1时需要刷新画面，请求立即刷新画面的意思
    int		paused;             // =1时暂停，=0时播放
    int		last_paused;        // 暂存“暂停”/“播放”状态
    int		queue_attachments_req;
    int		seek_req;           // 标识一次seek请求
    int		seek_flags;         // seek标志，诸如AVSEEK_FLAG_BYTE等
    int64_t		seek_pos;       // 请求seek的目标位置(当前位置+增量)
    int64_t		seek_rel;       // 本次seek的位置增量
    int		read_pause_return;
    AVFormatContext *ic;        // iformat的上下文
    int		realtime;           // =1为实时流

    Clock	audclk;             // 音频时钟
    Clock	vidclk;             // 视频时钟
    Clock	extclk;             // 外部时钟

    FrameQueue	pictq;          // 视频Frame队列
    FrameQueue	subpq;          // 字幕Frame队列
    FrameQueue	sampq;          // 采样Frame队列

    Decoder auddec;             // 音频解码器
    Decoder viddec;             // 视频解码器
    Decoder subdec;             // 字幕解码器

    int audio_stream ;          // 音频流索引

    int av_sync_type;           // 音视频同步类型, 默认audio master

    double			audio_clock;            // 当前音频帧的PTS+当前帧Duration
    int             audio_clock_serial;     // 播放序列，seek可改变此值
    // 以下4个参数 非audio master同步方式使用
    double			audio_diff_cum;         // used for AV difference average computation
    double			audio_diff_avg_coef;
    double			audio_diff_threshold;
    int			audio_diff_avg_count;
    // end

    AVStream		*audio_st;              // 音频流
    PacketQueue		audioq;                 // 音频packet队列
    int			audio_hw_buf_size;          // SDL音频缓冲区的大小(字节为单位)
    // 指向待播放的一帧音频数据，指向的数据区将被拷入SDL音频缓冲区。若经过重采样则指向audio_buf1，
    // 否则指向frame中的音频
    uint8_t			*audio_buf;             // 指向需要重采样的数据
    uint8_t			*audio_buf1;            // 指向重采样后的数据
    unsigned int		audio_buf_size;     // 待播放的一帧音频数据(audio_buf指向)的大小
    unsigned int		audio_buf1_size;    // 申请到的音频缓冲区audio_buf1的实际尺寸
    int			audio_buf_index;            // 更新拷贝位置 当前音频帧中已拷入SDL音频缓冲区
    // 的位置索引(指向第一个待拷贝字节)
    // 当前音频帧中尚未拷入SDL音频缓冲区的数据量:
    // audio_buf_size = audio_buf_index + audio_write_buf_size
    int			audio_write_buf_size;
    int			audio_volume;               // 音量
    int			muted;                      // =1静音，=0则正常
    struct AudioParams audio_src;           // 音频frame的参数
#if CONFIG_AVFILTER
    struct AudioParams audio_filter_src;
#endif
    struct AudioParams audio_tgt;       // SDL支持的音频参数，重采样转换：audio_src->audio_tgt
    struct SwrContext *swr_ctx;         // 音频重采样context
    int frame_drops_early;              // 丢弃视频packet计数
    int frame_drops_late;               // 丢弃视频frame计数

    enum ShowMode show_mode;

    // 音频波形显示使用
    int16_t sample_array[SAMPLE_ARRAY_SIZE];    // 采样数组
    int sample_array_index;                     // 采样索引
    int last_i_start;                           // 上一开始
    RDFTContext *rdft;                          // 自适应滤波器上下文
    int rdft_bits;                              // 自使用比特率
    FFTSample *rdft_data;                       // 快速傅里叶采样

    int xpos;
    double last_vis_time;
    SDL_Texture *vis_texture;       // 音频Texture

    SDL_Texture *sub_texture;       // 字幕显示
    SDL_Texture *vid_texture;       // 视频显示

    int subtitle_stream;            // 字幕流索引
    AVStream *subtitle_st;          // 字幕流
    PacketQueue subtitleq;          // 字幕packet队列

    double frame_timer;             // 记录最后一帧播放的时刻
    double frame_last_returned_time;    // 上一次返回时间
    double frame_last_filter_delay;     // 上一个过滤器延时

    int video_stream;               // 视频流索引
    AVStream *video_st;             // 视频流
    PacketQueue videoq;             // 视频队列
    double max_frame_duration;      // 一帧最大间隔. above this, we consider the jump a timestamp discontinuity
    struct SwsContext *img_convert_ctx; // 视频尺寸格式变换
    struct SwsContext *sub_convert_ctx; // 字幕尺寸格式变换
    int eof;            // 是否读取结束

    char *filename;     // 文件名
    int width, height, xleft, ytop; // 宽、高，x起始坐标，y起始坐标
    int step;           // =1 步进播放模式, =0 其他模式

#if CONFIG_AVFILTER
    int vfilter_idx;
    AVFilterContext *in_video_filter;   // the first filter in the video chain
    AVFilterContext *out_video_filter;  // the last filter in the video chain
    AVFilterContext *in_audio_filter;   // the first filter in the audio chain
    AVFilterContext *out_audio_filter;  // the last filter in the audio chain
    AVFilterGraph *agraph;              // audio filter graph
#endif
    // 保留最近的相应audio、video、subtitle流的steam index
    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_cond *continue_read_thread; // 当读取数据队列满了后进入休眠时，可以通过该condition唤醒读线程

    // ijkplayer
    /* extra fields */
    SDL_mutex  *play_mutex; // only guard state, do not block any long operation
    SDL_Thread *video_refresh_tid;
//    SDL_Thread _video_refresh_tid;

    int buffering_on;
    int pause_req;

    int dropping_frame;
    int is_video_high_fps; // above 30fps
    int is_video_high_res; // above 1080p

    PacketQueue *buffer_indicator_queue;

    volatile int latest_video_seek_load_serial;
    volatile int latest_audio_seek_load_serial;
    volatile int64_t latest_seek_load_start_at;

    int drop_aframe_count;
    int drop_vframe_count;
    int64_t accurate_seek_start_time;
    volatile int64_t accurate_seek_vframe_pts;
    volatile int64_t accurate_seek_aframe_pts;
    int audio_accurate_seek_req;
    int video_accurate_seek_req;
    SDL_mutex *accurate_seek_mutex;
    SDL_cond  *video_accurate_seek_cond;
    SDL_cond  *audio_accurate_seek_cond;
    volatile int initialized_decoder;
    int seek_buffering;

} VideoState;

/* options specified by the user */
static AVInputFormat *file_iformat; //
static const char *input_filename;
static const char *window_title;
static int default_width  = 640;
static int default_height = 480;
static int screen_width  = 0;
static int screen_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;  // 显示视频窗口的x坐标，默认在居中
static int screen_top = SDL_WINDOWPOS_CENTERED;   // 显示视频窗口的y坐标，默认居中
static int audio_disable;
static int video_disable;
static int subtitle_disable;
static const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
static int seek_by_bytes = -1;
static float seek_interval = 10;  // 可以指定seek的间隔
static int display_disable;
static int borderless;
static int alwaysontop; // 是否顶置
static int startup_volume = 20;  // 起始音量
static int show_status = 1;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static int fast = 0;
static int genpts = 0;
static int lowres = 0;
static int decoder_reorder_pts = -1;
static int autoexit;
static int exit_on_keydown;
static int exit_on_mousedown;
static int loop = 1;        // 设置循环次数
static int framedrop = -1;
static int infinite_buffer = -1;
static enum ShowMode show_mode = SHOW_MODE_NONE;
static const char *audio_codec_name;
static const char *subtitle_codec_name;
static const char *video_codec_name;
//double rdftspeed = 0.02;
static int64_t cursor_last_shown;
static int cursor_hidden = 0;
#if CONFIG_AVFILTER
static const char **vfilters_list = NULL;
static int nb_vfilters = 0;
static char *afilters = NULL;
#endif
static int autorotate = 1;
static int find_stream_info = 1;
static int filter_nbthreads = 0;  // filter线程数量

/* current context */
static int is_full_screen;
static int64_t audio_callback_time;

static AVPacket flush_pkt;

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_RendererInfo renderer_info = {0};
static SDL_AudioDeviceID audio_dev;

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {  // FFmpeg PIX_FMT to SDL_PIX的映射关系
{ AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
{ AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
{ AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
{ AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
{ AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
{ AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
{ AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
{ AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
{ AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
{ AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
{ AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
{ AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
{ AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
{ AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
{ AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
{ AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
{ AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
{ AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
{ AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
{ AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN },
};

#if CONFIG_AVFILTER
static int opt_add_vfilter(void *optctx, const char *opt, const char *arg)
{
    GROW_ARRAY(vfilters_list, nb_vfilters);
    vfilters_list[nb_vfilters - 1] = arg;
    return 0;
}
#endif

typedef struct FFPlayer {
    const AVClass *av_class;

    /* ffplay context */
    VideoState *is;

    /* format/codec options */
    AVDictionary *format_opts;  // 类似ffplay命令行获取到的
    AVDictionary *codec_opts;
    AVDictionary *sws_dict;
    AVDictionary *player_opts;
    AVDictionary *swr_opts;
    AVDictionary *swr_preset_opts;

    /* ffplay options specified by the user */
#ifdef FFP_MERGE
    AVInputFormat *file_iformat;
#endif
    char *input_filename;
#ifdef FFP_MERGE
    const char *window_title;
    int fs_screen_width;
    int fs_screen_height;
    int default_width;
    int default_height;
    int screen_width;
    int screen_height;
#endif
    int audio_disable;  // 类似ffplay命令行获取到的
    int video_disable;
    int subtitle_disable;
    const char* wanted_stream_spec[AVMEDIA_TYPE_NB];
    int seek_by_bytes;
    int display_disable;
    int show_status;
    int av_sync_type;
    int64_t start_time;
    int64_t duration;
    int fast;
    int genpts;
    int lowres;
    int decoder_reorder_pts;
    int autoexit;
#ifdef FFP_MERGE
    int exit_on_keydown;
    int exit_on_mousedown;
#endif
    int loop;
    int framedrop;
    int64_t seek_at_start;
    int subtitle;
    int infinite_buffer;
    enum ShowMode show_mode;
    char *audio_codec_name;
    char *subtitle_codec_name;
    char *video_codec_name;
    double rdftspeed;
#ifdef FFP_MERGE
    int64_t cursor_last_shown;
    int cursor_hidden;
#endif
#if CONFIG_AVFILTER
    const char **vfilters_list;
    int nb_vfilters;
    char *afilters;
    char *vfilter0;
#endif
    int autorotate;
    int find_stream_info;
    unsigned sws_flags;

    /* current context */
#ifdef FFP_MERGE
    int is_full_screen;
#endif
    int64_t audio_callback_time;
#ifdef FFP_MERGE
    SDL_Surface *screen;
#endif

    /* extra fields */
//    SDL_Aout *aout;     // 音频输出 不同的系统
//    SDL_Vout *vout;     // 视频输出
//    struct IJKFF_Pipeline *pipeline;
//    struct IJKFF_Pipenode *node_vdec;
    int sar_num;
    int sar_den;

    char *video_codec_info;
    char *audio_codec_info;
    char *subtitle_codec_info;
    Uint32 overlay_format;

    int last_error;
    int prepared;
    int auto_resume;
    int error;
    int error_count;
    int start_on_prepared;
    int first_video_frame_rendered;
    int first_audio_frame_rendered;
    int sync_av_start;

    MessageQueue msg_queue; // 消息队列

    int64_t playable_duration_ms;

    int packet_buffering;
    int pictq_size;
    int max_fps;
    int startup_volume;

    int videotoolbox;
    int vtb_max_frame_width;
    int vtb_async;
    int vtb_wait_async;
    int vtb_handle_resolution_change;

    int mediacodec_all_videos;
    int mediacodec_avc;
    int mediacodec_hevc;
    int mediacodec_mpeg2;
    int mediacodec_mpeg4;
    int mediacodec_handle_resolution_change;
    int mediacodec_auto_rotate;

    int opensles;
    int soundtouch_enable;

    char *iformat_name;

    int no_time_adjust;
    double preset_5_1_center_mix_level;

//    struct IjkMediaMeta *meta;

//    SDL_SpeedSampler vfps_sampler;
//    SDL_SpeedSampler vdps_sampler;

    /* filters */
//    SDL_mutex  *vf_mutex;
//    SDL_mutex  *af_mutex;
    int         vf_changed;
    int         af_changed;
    float       pf_playback_rate;
    int         pf_playback_rate_changed;
    float       pf_playback_volume;
    int         pf_playback_volume_changed;

    void               *inject_opaque;
    void               *ijkio_inject_opaque;
//    FFStatistic         stat;
//    FFDemuxCacheControl dcc;

//    AVApplicationContext *app_ctx;
//    IjkIOManagerContext *ijkio_manager_ctx;

    int enable_accurate_seek;
    int accurate_seek_timeout;
    int mediacodec_sync;
    int skip_calc_frame_rate;
    int get_frame_mode;
    GetImgInfo *get_img_info;
    int async_init_decoder;
    char *video_mime_type;
    char *mediacodec_default_name;
    int ijkmeta_delay_init;
    int render_wait_start;
    int is_manifest;
//    LasPlayerStatistic las_player_statistic;
} FFPlayer;

#define fftime_to_milliseconds(ts) (av_rescale(ts, 1000, AV_TIME_BASE))
#define milliseconds_to_fftime(ms) (av_rescale(ms, AV_TIME_BASE, 1000))

inline static void ffp_reset_internal(FFPlayer *ffp)
{
    /* ffp->is closed in stream_close() */
    av_opt_free(ffp);

    /* format/codec options */
    av_dict_free(&ffp->format_opts);
    av_dict_free(&ffp->codec_opts);
    av_dict_free(&ffp->sws_dict);
    av_dict_free(&ffp->player_opts);
    av_dict_free(&ffp->swr_opts);
    av_dict_free(&ffp->swr_preset_opts);

    /* ffplay options specified by the user */
    av_freep(&ffp->input_filename);
    ffp->audio_disable          = 0;
    ffp->video_disable          = 0;
    memset(ffp->wanted_stream_spec, 0, sizeof(ffp->wanted_stream_spec));
    ffp->seek_by_bytes          = -1;
    ffp->display_disable        = 0;
    ffp->show_status            = 0;
    ffp->av_sync_type           = AV_SYNC_AUDIO_MASTER;
    ffp->start_time             = AV_NOPTS_VALUE;
    ffp->duration               = AV_NOPTS_VALUE;
    ffp->fast                   = 1;
    ffp->genpts                 = 0;
    ffp->lowres                 = 0;
    ffp->decoder_reorder_pts    = -1;
    ffp->autoexit               = 0;
    ffp->loop                   = 1;
    ffp->framedrop              = 0; // option
    ffp->seek_at_start          = 0;
    ffp->infinite_buffer        = -1;
    ffp->show_mode              = SHOW_MODE_NONE;
    av_freep(&ffp->audio_codec_name);
    av_freep(&ffp->video_codec_name);
    ffp->rdftspeed              = 0.02;
#if CONFIG_AVFILTER
    av_freep(&ffp->vfilters_list);
    ffp->nb_vfilters            = 0;
    ffp->afilters               = NULL;
    ffp->vfilter0               = NULL;
#endif
    ffp->autorotate             = 1;
    ffp->find_stream_info       = 1;

    ffp->sws_flags              = SWS_FAST_BILINEAR;

    /* current context */
    ffp->audio_callback_time    = 0;

    /* extra fields */
//    ffp->aout                   = NULL; /* reset outside */
//    ffp->vout                   = NULL; /* reset outside */
//    ffp->pipeline               = NULL;
//    ffp->node_vdec              = NULL;
    ffp->sar_num                = 0;
    ffp->sar_den                = 0;

    av_freep(&ffp->video_codec_info);
    av_freep(&ffp->audio_codec_info);
    av_freep(&ffp->subtitle_codec_info);
//    ffp->overlay_format         = SDL_FCC_RV32;

    ffp->last_error             = 0;
    ffp->prepared               = 0;
    ffp->auto_resume            = 0;
    ffp->error                  = 0;
    ffp->error_count            = 0;
    ffp->start_on_prepared      = 1;
    ffp->first_video_frame_rendered = 0;
    ffp->sync_av_start          = 1;
    ffp->enable_accurate_seek   = 0;
//    ffp->accurate_seek_timeout  = MAX_ACCURATE_SEEK_TIMEOUT;

    ffp->playable_duration_ms           = 0;

    ffp->packet_buffering               = 1;
    ffp->pictq_size                     = VIDEO_PICTURE_QUEUE_SIZE_DEFAULT; // option
    ffp->max_fps                        = 31; // option

    ffp->videotoolbox                   = 0; // option
    ffp->vtb_max_frame_width            = 0; // option
    ffp->vtb_async                      = 0; // option
    ffp->vtb_handle_resolution_change   = 0; // option
    ffp->vtb_wait_async                 = 0; // option

    ffp->mediacodec_all_videos          = 0; // option
    ffp->mediacodec_avc                 = 0; // option
    ffp->mediacodec_hevc                = 0; // option
    ffp->mediacodec_mpeg2               = 0; // option
    ffp->mediacodec_handle_resolution_change = 0; // option
    ffp->mediacodec_auto_rotate         = 0; // option

    ffp->opensles                       = 0; // option
    ffp->soundtouch_enable              = 0; // option

    ffp->iformat_name                   = NULL; // option

    ffp->no_time_adjust                 = 0; // option
    ffp->async_init_decoder             = 0; // option
    ffp->video_mime_type                = NULL; // option
    ffp->mediacodec_default_name        = NULL; // option
    ffp->ijkmeta_delay_init             = 0; // option
    ffp->render_wait_start              = 0;
    ffp->is_manifest                    = 0;

//    ijkmeta_reset(ffp->meta);

//    SDL_SpeedSamplerReset(&ffp->vfps_sampler);
//    SDL_SpeedSamplerReset(&ffp->vdps_sampler);

    /* filters */
    ffp->vf_changed                     = 0;
    ffp->af_changed                     = 0;
    ffp->pf_playback_rate               = 1.0f;
    ffp->pf_playback_rate_changed       = 0;
    ffp->pf_playback_volume             = 1.0f;
    ffp->pf_playback_volume_changed     = 0;

//    av_application_closep(&ffp->app_ctx);
//    ijkio_manager_destroyp(&ffp->ijkio_manager_ctx);

//    msg_queue_flush(&ffp->msg_queue);

    ffp->inject_opaque = NULL;
    ffp->ijkio_inject_opaque = NULL;
//    ffp_reset_statistic(&ffp->stat);
//    ffp_reset_demux_cache_control(&ffp->dcc);
}

inline static void ffp_notify_msg1(FFPlayer *ffp, int what) {
//    msg_queue_put_simple3(&ffp->msg_queue, what, 0, 0);
}

inline static void ffp_notify_msg2(FFPlayer *ffp, int what, int arg1) {
//    msg_queue_put_simple3(&ffp->msg_queue, what, arg1, 0);
}

inline static void ffp_notify_msg3(FFPlayer *ffp, int what, int arg1, int arg2) {
//    msg_queue_put_simple3(&ffp->msg_queue, what, arg1, arg2);
}

inline static void ffp_notify_msg4(FFPlayer *ffp, int what, int arg1, int arg2, void *obj, int obj_len) {
//    msg_queue_put_simple4(&ffp->msg_queue, what, arg1, arg2, obj, obj_len);
}

inline static void ffp_remove_msg(FFPlayer *ffp, int what) {
//    msg_queue_remove(&ffp->msg_queue, what);
}

inline static const char *ffp_get_error_string(int error) {
    switch (error) {
        case AVERROR(ENOMEM):       return "AVERROR(ENOMEM)";       // 12
        case AVERROR(EINVAL):       return "AVERROR(EINVAL)";       // 22
        case AVERROR(EAGAIN):       return "AVERROR(EAGAIN)";       // 35
        case AVERROR(ETIMEDOUT):    return "AVERROR(ETIMEDOUT)";    // 60
        case AVERROR_EOF:           return "AVERROR_EOF";
        case AVERROR_EXIT:          return "AVERROR_EXIT";
    }
    return "unknown";
}

#define AVCODEC_MODULE_NAME    "avcodec"
#define MEDIACODEC_MODULE_NAME "MediaCodec"
#ifdef __cplusplus
}
#endif
#endif // FF_FFPLAY_DEF_H
