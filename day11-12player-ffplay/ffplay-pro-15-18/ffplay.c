/*
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
/**
 * FFMPEG三件套之ffplay，基于4.2.1版本
 *
 * 本程序将ffmpeg的三大范例之ffplay.c移植到QT平台进行编译，以方便调试。
 * 该程序功能：基于ffmpeg库开发多媒体播放器。
 *
 */
/**
 * @file
 * simple media player based on the FFmpeg libraries
 */

#include "config.h"
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

#if CONFIG_AVFILTER
# include "libavfilter/avfilter.h"
# include "libavfilter/buffersink.h"
# include "libavfilter/buffersrc.h"
#endif

#include <SDL.h>
#include <SDL_thread.h>

#include "cmdutils.h"

#include <assert.h>

const char program_name[] = "ffplay";
const int program_birth_year = 2003;

// 15M
#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
// 外部时钟的最小最大帧数
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/*
 * Calculate actual buffer size keeping in mind not cause too frequent audio callbacks
 * 计算实际的缓冲区大小，记住不要引起太频繁的音频回调。
*/
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04  // 40ms
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1   // 100ms
/*
 * If a frame duration is longer than this, it will not be duplicated to compensate AV sync
 * 如果一个帧的持续时间超过这个值，它将不会被重复的去补偿音视频同步。实际上这个值可以按照情况去修改。
*/
//#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.040
/*
 * no AV correction is done if too big error
 * 误差太大则不作音视频校正
*/
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/*
 * external clock speed adjustment constants for realtime sources based on buffer fullness
 * 基于缓冲区大小的实时源的外部时钟速度调整常数
 */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
// 步调：1ms的改变量。
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20

/*
 * polls for possible required screen refresh at least this often, should be less than 1/fps
 * 至少在这种情况下，可能需要屏幕刷新的轮询应该小于1/fps。
 * REFRESH_RATE是用于当没有帧可放时的轮询时间间隔。一般小于1/fps即可，大多数是小于0.04=40ms即可。他这里选择10ms的轮询时间。
*/
#define REFRESH_RATE 0.01

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

static unsigned sws_flags = SWS_BICUBIC;

/**
 * ok
 * PacketQueue队列节点。
 */
typedef struct MyAVPacketList {
    AVPacket		pkt;            //解封装后的数据
    struct MyAVPacketList	*next;  //下一个节点
    int			serial;             //播放序列，和PacketQueue的serial作用相同。
} MyAVPacketList;

/**
 * ok
 * PacketQueue队列。
 */
typedef struct PacketQueue {
    MyAVPacketList	*first_pkt, *last_pkt;  // 队首，队尾指针
    int		nb_packets;                     // 包数量，也就是队列元素数量
    int		size;                           // 队列所有元素的数据大小总和。每个包的大小由：实际数据 + MyAVPacketList的大小
    int64_t		duration;                   // 队列所有元素的数据播放持续时间
    int		abort_request;                  // 用户退出请求标志
    int		serial;                         // 播放序列号，和MyAVPacketList的serial作用相同，但改变的时序稍微有点不同，可认为是一样的。
                                            // 到时候会通过MyAVPacketList里面的serial赋值给Decoder的pkt_serial，以判断是否直接舍弃该包，可看decoder_decode_frame()。
                                            // 这里的包队列的serial是最新的，如果解码时从包队列获取到的Decoder->pkt_serial不相等，表示需要舍弃掉。
                                            // 这里的serial在packet_queue_put_private插入flush_pkt包时会被自增。同样MyAVPacketList的serial也是在packet_queue_put_private被赋值。

    SDL_mutex	*mutex;                     // 用于维持PacketQueue的多线程安全(SDL_mutex可以按pthread_mutex_t理解）
    SDL_cond	*cond;                      // 用于读、写线程相互通知(SDL_cond可以按pthread_cond_t理解)
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE	3       // 图像帧缓存数量
#define SUBPICTURE_QUEUE_SIZE		16      // 字幕帧缓存数量
#define SAMPLE_QUEUE_SIZE           9       // 采样帧缓存数量
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))   // 取视频、音频、字幕的三者最大值

/**
 * 封装的音频参数结构体。
*/
typedef struct AudioParams {
    int			freq;                   // 采样率
    int			channels;               // 通道数
    int64_t		channel_layout;         // 通道布局，比如2.1声道，5.1声道等
    enum AVSampleFormat	fmt;            // 音频采样格式，比如AV_SAMPLE_FMT_S16表示为有符号16bit深度，交错排列模式。
    int			frame_size;             // 一个采样单元占用的字节数（比如2通道时，则左右通道各采样一次合成一个采样单元）
    int			bytes_per_sec;          // 一秒时间的字节数，比如采样率48Khz，2 channel，16bit，则一秒48000*2*2=192000
} AudioParams;

/**
 * 封装的时钟结构体。
 * 这里讲的系统时钟 是通过av_gettime_relative()获取到的时钟，单位为微妙。
*/
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

    // 指向packet_serial，即指向当前包队列的指针，用于过时的时钟检测。
    int *queue_serial;      /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

/**
 * ok
 * Common struct for handling all types of decoded data and allocated render buffers.
 * 用于处理所有类型的解码数据和分配的呈现缓冲区的通用结构。即用于缓存解码后的数据
*/
typedef struct Frame {
    AVFrame		*frame;         // 指向数据帧
    AVSubtitle	sub;            // 用于字幕，解码后的字幕帧。
    int		serial;             // 帧序列，在seek的操作时serial会变化
    double		pts;            // 时间戳，单位为秒，就是从解码后获得的AVFrame->pts赋值的，然后push到帧队列
    double		duration;       // 该帧持续时间，单位为秒
    int64_t		pos;            // 该帧在输入文件中的字节位置
    int		width;              // 图像宽度
    int		height;             // 图像高读
    int		format;             // 对于图像为(enum AVPixelFormat)，对于声音则为(enum AVSampleFormat)

    AVRational	sar;            // 图像的宽高比（16:9，4:3...），如果未知或未指定则为0/1
    int		uploaded;           // 用来记录该帧是否已经显示过。0：未显示；1已显示。
    int		flip_v;             // = 1则垂直翻转， = 0则正常播放
} Frame;

/**
 * ok
 * 这是一个循环队列，windex是指其中的首元素，rindex是指其中的尾部元素.
*/
typedef struct FrameQueue {
    Frame	queue[FRAME_QUEUE_SIZE];        // FRAME_QUEUE_SIZE  最大size, 数字太大时会占用大量的内存，需要注意该值的设置
    int		rindex;                         // 读索引。待播放时读取此帧进行播放，播放后此帧成为上一帧
    int		windex;                         // 写索引，代表从该位置可以写入帧，windex上一个位置不可以，上一位置是刚插入的帧
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
    AVPacket pkt;                   // 用于保存packet_pending=1解码器异常时的包，以便需要等解码器恢复正常再进行解码。注该包是正常的，只是解码器异常
    PacketQueue	*queue;             // 数据包队列
    AVCodecContext	*avctx;         // 解码器上下文
    int		pkt_serial;             // 包序列。解码结束后，会在decoder_decode_frame被标记为d->finished = d->pkt_serial;
    int		finished;               // =0，解码器处于工作状态；=非0，解码器处于空闲状态。解码结束后，会在decoder_decode_frame被标记为d->finished = d->pkt_serial;
    int		packet_pending;         // =0，解码器处于异常状态，需要考虑重置解码器；=1，解码器处于正常状态
    SDL_cond	*empty_queue_cond;  // 检查到packet队列空时发送 signal缓存read_thread读取数据
    int64_t		start_pts;          // 初始化时是stream的start time，未被初始化时是个未知值
    AVRational	start_pts_tb;       // 初始化时是stream的time_base，未被初始化时是个未知值
    int64_t		next_pts;           // 记录最近一次解码后的frame的pts，当解出来的部分帧没有有效的pts时则使用next_pts进行推算
    AVRational	next_pts_tb;        // next_pts的单位
    SDL_Thread	*decoder_tid;       // 线程句柄
} Decoder;

/**
 * ok
 * 播放器封装。
 */
typedef struct VideoState {
    SDL_Thread	*read_tid;      // 读线程句柄
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

    double			audio_clock;            // 当前音频帧的PTS + 当前帧Duration，表示下一音频帧的pts？
    int             audio_clock_serial;     // 播放序列，seek可改变此值

    // 以下4个参数 非audio master同步方式使用
    double			audio_diff_cum;         // used for AV difference average computation.代表本次的误差+历史的权重比误差的和。用于AV差值平均计算，初始值是0，由av_mallocz()时赋值。
    double			audio_diff_avg_coef;    // 一个系数值，经验得出的av系数值吗？？？
    double			audio_diff_threshold;   // (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec得出，即缓冲区所占秒数的最大阈值，一般是两帧音频的时长，大于时会进行音频校正
    int             audio_diff_avg_count;   // 用于统计次数，配合AUDIO_DIFF_AVG_NB宏使用后，决定了audio_diff_cum中的历史的权重比误差。
                                            // 非音频主时钟时，判断是否可以进行音视频同步。一般是20次。
    // end

    AVStream		*audio_st;              // 音频流
    PacketQueue		audioq;                 // 音频packet队列
    int             audio_hw_buf_size;      // SDL音频缓冲区的大小(字节为单位)，由audio_open调用后的返回值获取。

    // 指向待播放的一帧音频数据，指向的数据区将被拷入SDL音频缓冲区。若经过重采样则指向audio_buf1，
    // 否则指向frame中的音频
    uint8_t			*audio_buf;             // 指向需要重采样的数据。
    uint8_t			*audio_buf1;            // 指向重采样后的数据。
    unsigned int		audio_buf_size;     // 待播放的一帧音频数据(audio_buf指向)的大小。
                                            // 所以在使用时申请到的尺寸(audio_buf1_size)都会大于这个实际尺寸(audio_buf_size)。例如4736.而使用时实际只使用4096.

    unsigned int		audio_buf1_size;    // 申请到的音频缓冲区audio_buf1的实际尺寸。
    int			audio_buf_index;            // 更新拷贝位置 当前音频帧中已拷入SDL音频缓冲区
                                            // 的位置索引(指向第一个待拷贝字节)
    // 当前音频帧中尚未拷入SDL音频缓冲区的数据量:
    // audio_buf_size = audio_buf_index + audio_write_buf_size
    int			audio_write_buf_size;
    int			audio_volume;               // 音量，范围值由用户传进的[0,100]转成SDL的[0,128]
    int			muted;                      // =1静音，=0则正常
    struct AudioParams audio_src;           // 音频frame的参数
#if CONFIG_AVFILTER
    struct AudioParams audio_filter_src;
#endif
    struct AudioParams audio_tgt;           // SDL支持的音频参数(也是硬件支持的音频参数)，重采样转换：audio_src->audio_tgt
    struct SwrContext *swr_ctx;             // 音频重采样context
    int frame_drops_early;                  // 丢弃视频packet计数
    int frame_drops_late;                   // 丢弃视频frame计数

    enum ShowMode {
        SHOW_MODE_NONE = -1,                // 无显示
        SHOW_MODE_VIDEO = 0,                // 显示视频
        SHOW_MODE_WAVES,                    // 显示波浪，音频
        SHOW_MODE_RDFT,                     // 自适应滤波器
        SHOW_MODE_NB
    } show_mode;

    // 音频波形显示使用
    int16_t sample_array[SAMPLE_ARRAY_SIZE];    // 采样数组
    int sample_array_index;                     // 采样索引
    int last_i_start;                           // 上一开始
    RDFTContext *rdft;                          // 自适应滤波器上下文
    int rdft_bits;                              // 自适应比特率
    FFTSample *rdft_data;                       // 快速傅里叶采样

    int xpos;
    double last_vis_time;
    SDL_Texture *vis_texture;                   // 音频纹理Texture

    SDL_Texture *sub_texture;                   // 字幕显示
    SDL_Texture *vid_texture;                   // 视频显示

    int subtitle_stream;                        // 字幕流索引
    AVStream *subtitle_st;                      // 字幕流
    PacketQueue subtitleq;                      // 字幕packet队列

    double frame_timer;                         // 记录最后一帧播放的时刻
    double frame_last_returned_time;            // 上一次返回时间，配合frame_last_filter_delay使用
    double frame_last_filter_delay;             // 上一个过滤器延时，即每一次从过滤器中获取一帧的延时。

    int video_stream;                           // 视频流索引。在stream_component_open有赋值记录
    AVStream *video_st;                         // 视频流
    PacketQueue videoq;                         // 视频队列
    double max_frame_duration;                  // 一帧最大间隔，被初始化成10或者3600. above this, we consider the jump a timestamp discontinuity(在此之上，我们认为跳转是时间戳的不连续)
                                                // 3600的意思是0.04x90k=3600？个人认为不是这个意思，它只是单纯的代表一个秒数，并且最好尽可能大，例如这里是1小时的秒数。
                                                // 认为它是一个秒数的根据是：compute_target_delay里面，直接使用fabs(diff)与max_frame_duration比较了，感觉它这个名字起得不太好。
    struct SwsContext *img_convert_ctx;         // 视频尺寸格式变换
    struct SwsContext *sub_convert_ctx;         // 字幕尺寸格式变换
    int eof;                                    // 是否读取结束

    char *filename;                             // 文件名
    int width, height, xleft, ytop;             // 屏幕的宽、高，x起始坐标，y起始坐标
    int step;                                   // =1 步进播放模式, =0 其他模式

#if CONFIG_AVFILTER
    int vfilter_idx;
    AVFilterContext *in_video_filter;           // the first filter in the video chain
    AVFilterContext *out_video_filter;          // the last filter in the video chain
    AVFilterContext *in_audio_filter;           // the first filter in the audio chain
    AVFilterContext *out_audio_filter;          // the last filter in the audio chain
    AVFilterGraph *agraph;                      // audio filter graph
#endif
    // 保留最近的相应audio、video、subtitle流的steam index，在stream_component_open保存。估计是想在切换语言(粤/国)的时候使用
    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_cond *continue_read_thread;                 // 当读取数据队列满了后进入休眠时，可以通过该condition唤醒读线程
} VideoState;

/* options specified by the user */
static AVInputFormat *file_iformat;                 // 输入封装格式结构体，注AVFormatContext才是输入输出封装上下文
static const char *input_filename;                  // 从命令行拿到的输入文件名，可以是网络流
static const char *window_title;
static int default_width  = 640;
static int default_height = 480;
static int screen_width  = 0;
static int screen_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;    // 显示视频窗口的x坐标，默认在居中
static int screen_top = SDL_WINDOWPOS_CENTERED;     // 显示视频窗口的y坐标，默认居中
static int audio_disable;                           // 是否禁用音频，0启用(默认)；1禁用，禁用将不会播放声音。
static int video_disable;
static int subtitle_disable;

static const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};   // 用于保存目标流的下标。
                                                                // 目标流指目标流的分类。相关流指除我自己以外的流。
                                                                // 例如音频流可能分为国语与粤语，那么国语、粤语就是目标流。   再例如我是视频流，那么相关流就是指音频流、字幕流。
                                                                // 例如2_audio.mp4，可在命令行 -ast 0指定播放粤语，-ast 1指定播放国语。
                                                                // wanted_stream_spec就会保存着这个0或者1。
                                                                // 视频流、字幕流同理，命令行是 -vst n 和 -sst n，n代表数字。
                                                                // 实际我们只需要知道wanted_stream_spec存储的元素的意思指不同语言即可，什么是目标流相关流只是FFmpeg的描述而已。

static int seek_by_bytes = -1;                      // 是否按照字节来seek
static float seek_interval = 10;                    // 可以指定seek的间隔
static int display_disable;                         // 是否显示视频，0显示(默认)，1不显示。置1后， 会导致video_disable会被置1.
static int borderless;                              // 是否可以调整窗口大小。0可以；1不可以。
static int alwaysontop;                             // 是否顶置
static int startup_volume = 50;                     // 起始音量
static int show_status = 1;                         // 打印关于输入或输出格式的详细信息(tbr,tbn,tbc)，默认1代表打印
static int av_sync_type = AV_SYNC_AUDIO_MASTER;     // 默认音频时钟同步
static int64_t start_time = AV_NOPTS_VALUE;         // 指定开始播放的时间
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
double rdftspeed = 0.02;
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

// 一个特殊的packet，主要用来作为非连续的两端数据的“分界”标记：
// 1）插入 flush_pkt 触发PacketQueue其对应的serial，加1操作；
// 2）触发解码器清空自身缓存 avcodec_flush_buffers()，以备新序列的数据进行新解码
static AVPacket flush_pkt;

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

static SDL_Window *window;                      // 窗口的ID
static SDL_Renderer *renderer;
static SDL_RendererInfo renderer_info = {0};
static SDL_AudioDeviceID audio_dev;             // 打开音频设备后返回的设备ID。SDL_OpenAudio()始终返回1，SDL_OpenAudioDevice返回2以及2以上，这是为了兼容SDL不同版本。

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

/**
 * @brief 比较通道数和格式是否相等，通道数都为1时，会使用另一种比较方法。
 * @param fmt1 格式1。
 * @param channel_count1  通道数1。
 * @param fmt2 格式2。
 * @param channel_count2  通道数2。
 * @return 通道数、格式都相等则返回0，有一个不等则返回1.
 */
static inline
int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2)
{
    /*
     * If channel count == 1, planar and non-planar formats are the same。
     * 如果通道计数== 1，平面和非平面格式相同。
     *
     * 1）av_get_packed_sample_fmt：获取给定样本格式的包装替代形式，
     *      如果传入的sample_fmt已经是打包格式，则返回的格式与输入的格式相同。
     * 返回值：错误返回 给定样本格式的替代格式或AV_SAMPLE_FMT_NONE。
     *
     * 这样比是因为：当通道数都为1时，不管是包格式还是交错模式，返回都是包格式进行统一比较，即确保比较单位是一样的。
     * 如果通道数不是1的话，那就只能直接比较通道数和格式是否相等了。
    */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

/**
 * @brief 判断通道布局是否可用。判断依据，根据通道布局获取的通道数是否等于传入的通道数。
 * @param channel_layout 通道布局。
 * @param channels  用户传进的通道数。
 * @return 通道布局有效返回通道布局； 无效返回0.
 */
static inline
int64_t get_valid_channel_layout(int64_t channel_layout, int channels)
{
    // av_get_channel_layout_nb_channels: 返回通道布局中的通道数量。
    if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
        return channel_layout;
    else
        return 0;
}

/**
 * @brief 往包队列插入一个包。
 * @param q 包队列。可能是音视频、字幕包队列。
 * @param pkt 要插入的包。可能是数据包，也可能是个空包，用于刷掉帧队列剩余的帧。
 * @return 成功0 失败-1
*/
static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt)
{
    MyAVPacketList *pkt1;

    // 1. 如果已中止，则放入失败
    if (q->abort_request)
        return -1;

    // 2. 分配节点内存
    pkt1 = av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)  //内存不足，则放入失败
        return -1;

    // 3. 赋值，和判断是否插入的是flush_pkt包。
    // 没有做引用计数，那这里也说明av_read_frame不会释放替用户释放buffer。
    pkt1->pkt = *pkt; //拷贝AVPacket(浅拷贝，AVPacket.data等内存并没有拷贝)
    pkt1->next = NULL;
    if (pkt == &flush_pkt)//如果放入的是flush_pkt，需要增加队列的播放序列号，以区分不连续的两段数据。
    {
        q->serial++;
        printf("q->serial = %d\n", q->serial);
    }
    pkt1->serial = q->serial;   //用队列序列号标记节点序列号。和上面的包队列->serial作用一样，上面的不变，这里的也不变，上面的变，这里的也会变。
                                //这里看到，添加flush_pkt时的这个节点的serial也是自增。

    /*
     * 4. 队列操作：如果last_pkt为空，说明队列是空的，新增节点为队头；例如包队列只有一个包时，first_pkt、last_pkt指向同一个包。
     *      注意last_pkt不是指向NULL，和平时的设计不一样，不过内部的next是可能指向NULL。
     * 否则，队列有数据，则让原队尾的next为新增节点。 最后将队尾指向新增节点。
     *
     * 他这个队列的特点：1）first_pkt只操作一次，永远指向首包； 2）last_pkt永远指向尾包，不会指向NULL，但尾包last_pkt内部的next永远指向NULL。
     */
    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;

    // 5. 队列属性操作：增加节点数、cache大小、cache总时长, 用来控制队列的大小
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);// 每个包的大小由：实际数据 + MyAVPacketList的大小。
    q->duration += pkt1->pkt.duration;        // 如果是空包，在av_init_packet时duration被赋值为0.

    /* XXX: should duplicate packet data in DV case */
    // 6. 发出信号，表明当前队列中有数据了，通知等待中的读线程可以取数据了。这里的读线程应该指解码线程，以便读取包进行解码。
    SDL_CondSignal(q->cond);

    return 0;
}

/**
 * @brief 往包队列插入一个包。
 * @param q 包队列。可能是音视频、字幕包队列。
 * @param pkt 要插入的包。可能是数据包，也可能是个空包，用于刷掉帧队列剩余的帧。
 * @return 成功0 失败-1。 see packet_queue_put_private。
*/
static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    int ret;

    // 1. 调用packet_queue_put_private往包队列put一个包。
    SDL_LockMutex(q->mutex);
    ret = packet_queue_put_private(q, pkt);//主要实现
    SDL_UnlockMutex(q->mutex);

    // 2. 如果不是flush_pkt包并且放入失败的话，需要释放掉，因为av_read_frame不会帮你释放
    if (pkt != &flush_pkt && ret < 0)
        av_packet_unref(pkt);       //放入失败，释放AVPacket

    return ret;
}

/**
 * @brief 往包队列插入空包。插入空包说明码流数据读取完毕了，之前讲解码的时候说过刷空包是为了从解码器把所有帧都读出来。
 * @param q 包队列。可能是音视频、字幕包队列。
 * @param stream_index 对应流的下标。可能是音视频、字幕下标。
 * @return 成功0 失败-1。 see packet_queue_put、packet_queue_put_private.
*/
static int packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);// 以默认值初始化除了data、size外的字段。data、size必须由用户设置。
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

/**
 * @brief 初始化包队列，可能是视频包队列、音频包队列、字幕包队列。
 * @param 可能是视频包队列、音频包队列、字幕包队列。
 * @return 成功 0； 失败 负数。
*/
static int packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));          // 这里所有成员默认全部被置0.
    q->mutex = SDL_CreateMutex();
    if (!q->mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->cond = SDL_CreateCond();
    if (!q->cond) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    q->abort_request = 1;
    return 0;
}

/**
 * @brief 清空包队列中的数据，即清空PacketQueue里的MyAVPacketList链表成员。
 * @param q 包队列。
 * @return void。
 */
static void packet_queue_flush(PacketQueue *q)
{
    MyAVPacketList *pkt, *pkt1;// pkt指向当前包，pkt1指向当前包的下一个包

    SDL_LockMutex(q->mutex);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);// 取消对数据包引用的缓冲区的引用，并将其余的数据包字段重置为默认值。
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    SDL_UnlockMutex(q->mutex);
}

/**
 * @brief 清理包队列里面的链表、互斥锁、条件变量。
 * @param q 包队列。
 * @return void。
 */
static void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q); //先清除所有的节点
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->cond);
}

static void packet_queue_abort(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);

    q->abort_request = 1;       // 请求退出

    SDL_CondSignal(q->cond);    //释放一个条件信号

    SDL_UnlockMutex(q->mutex);
}

/**
 * @brief 启动包队列。往队列插入一个flush_pkt包，标记启动了包队列。由于在读线程中，stream_component_open是比av_read_frame读包进队列早的，
 *          所以解码线程是更快创建和flush_pkt是被首先放进对应的队列的，flush_pkt对应的serial加1。
 * @param q 包队列。
 * @return void。
 */
static void packet_queue_start(PacketQueue *q)
{
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt); //这里放入了一个flush_pkt
    SDL_UnlockMutex(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
/**
 * @brief packet_queue_get
 * @param q 包队列。
 * @param pkt 传入传出参数，即MyAVPacketList.pkt。
 * @param block 调用者是否需要在没节点可取的情况下阻塞等待。0非阻塞，1阻塞。
 * @param serial 传入传出参数，即MyAVPacketList.serial。
 * @return <0: aborted; =0: no packet; >0: has packet。
 *
 * 该队列的get的设计与rtsp-publish推流的PacketQueue的get类似。
 * packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial);
 */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial)
{
    MyAVPacketList *pkt1;
    int ret;

    // 1. 对包队列上锁
    SDL_LockMutex(q->mutex);

    for (;;) {
        // 2. 用户请求包队列中断退出
        if (q->abort_request) {
            ret = -1;
            break;
        }

        // 3. 从对头取出一个节点MyAVPacketList
        pkt1 = q->first_pkt;
        if (pkt1) {                     // 队列中有数据
            q->first_pkt = pkt1->next;  // 更新对头，此时队头为第二个节点
            if (!q->first_pkt)          // 如果第二个包是空的话，那么此时队列为空，last_pkt也应该被置为空，回到最初的状态。
                q->last_pkt = NULL;

            // 4. 更新相应的属性。 可以对比packet_queue_put_private，也是更新了这3个属性。
            q->nb_packets--;                            // 节点数减1
            q->size -= pkt1->pkt.size + sizeof(*pkt1);  // cache大小扣除一个节点
            q->duration -= pkt1->pkt.duration;          // 总时长扣除一个节点

            // 5. 真正获取AVPacket，这里发生一次AVPacket结构体拷贝，AVPacket的data只拷贝了指针
            *pkt = pkt1->pkt;
            if (serial) //如果需要输出serial，把serial输出.serial一般是解码队列里面的serial。
                *serial = pkt1->serial;

            // 6. 释放节点内存，因为在MyAVPacketList是malloc节点的内存。注只是释放节点，而不是释放AVPacket
            av_free(pkt1);
            ret = 1;
            break;

        } else if (!block) {    // 7. 队列中没有数据，且非阻塞调用
            ret = 0;
            break;
        } else {                // 8. 队列中没有数据，且阻塞调用，会阻塞在条件变量。
            //这里没有break。for循环的另一个作用是在条件变量被唤醒后，重复上述代码取出节点。
            SDL_CondWait(q->cond, q->mutex);
        }

    }// <== for (;;) end ==>

    // 9. 释放锁
    SDL_UnlockMutex(q->mutex);

    return ret;
}

/**
 * @brief 初始化解码器。
 * @param d 解码器。
 * @param avctx 解码器上下文。
 * @param queue 包队列。
 * @param empty_queue_cond 队列是否为空的条件变量。即解码时，与读线程共用，以此判断是否有数据进行解码。
 * @return void。
 */
static void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->avctx = avctx;                           // 解码器上下文
    d->queue = queue;                           // 绑定对应的packet queue
    d->empty_queue_cond = empty_queue_cond;     // 绑定read_thread线程的continue_read_thread
    d->start_pts = AV_NOPTS_VALUE;              // 起始设置为无效
    d->pkt_serial = -1;                         // 起始设置为-1
}

/**
 * @brief 解码视频帧，每次只获取一帧，获取到就会break。
 * @param d 播放器实例。
 * @param frame 传入传出，指向要获取视频帧的内存。
 * @param sub 字幕。视频时一般传NULL。
 * @return -1 用户中断； 0 解码结束； 1 正常解码。
 *
 * 注：avcodec_send_packet、avcodec_receive_frame代替了以前旧版本的解码函数avcodec_decode_video2。
 */
static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        AVPacket pkt;

        // 1. 流连续情况下获取解码后的帧
        // 1.1 先判断是否是同一播放序列的数据，不是的话不会进行receive。 解码队列中的包队列的serial(d->queue->serial)会在插入flush_pkt时自增
        // 但个人感觉这个if可以去掉，因为下面不是同一序列的pkt会被直接释放，不会送进解码。
        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request){
                    // 是否请求退出
                    return -1;
                }

                // 1.2. 获取解码帧。avcodec_receive_frame好像是要在avcodec_send_packet前接收的，可以看看以前的笔记。
                switch (d->avctx->codec_type) {
                case AVMEDIA_TYPE_VIDEO:
                    ret = avcodec_receive_frame(d->avctx, frame);    // 这里是拿到真正解码一帧的数据，并且best_effort_timestamp、pts均是这个函数内部赋值的
                    //printf("frame pts:%ld, dts:%ld\n", frame->pts, frame->pkt_dts);
                    if (ret >= 0) {
                        if (decoder_reorder_pts == -1) {
                            // 正常走这里的路径。使用各种启发式算法估计帧时间戳，单位为流的时基。
                            frame->pts = frame->best_effort_timestamp;
                        } else if (!decoder_reorder_pts) {
                            frame->pts = frame->pkt_dts;
                        }
                    }
                    break;

                case AVMEDIA_TYPE_AUDIO:
                    ret = avcodec_receive_frame(d->avctx, frame);
                    if (ret >= 0) {
                        AVRational tb = (AVRational){1, frame->sample_rate};    // 以采样率作为tb
                        if (frame->pts != AV_NOPTS_VALUE) {
                            /*
                             * 如果frame->pts正常则先将其从pkt_timebase转成{1, frame->sample_rate}。
                             * pkt_timebase实质就是stream->time_base。
                             * av_rescale_q公式：axbq/cq=ax(numb/denb)/(numc/denc);
                             * 而numb和numc通常是1，所以axbq/cq=ax(1/denb)/(1/denc)=(a/denb)*denc;
                             * 既有：a = a*denc / denb;  此时a的单位是采样频率。所以下面统计下一帧的next_pts时，
                             * 可以直接加上采样点个数nb_samples。因为此时frame->pts可以认为就是已经采样到的采样点总个数。
                             * 例如采样率=44100，nb_samples=1024，那么加上1024相当于加上1024/44.1k=0.023s。
                             * 例如采样率=8000，nb_samples=320，那么加上320相当于加上320/8k=0.04s。
                             * 更具体(以采样率=8k为例)：首帧frame->pts=8696，此时单位是采样频率，并且采样点按照320递增，那么下一帧frame->pts=8696+320=9016.
                             * 那么即使按照采样频率为单位，那么8696/8k=1.087;9016/8l=1.127。 1.127-1.087=0.04s，仍然是一帧的间隔，那么这样算就是没错的。
                             *
                             * 注意：有人可能在debug时出现这个疑惑：第一次进来获取到解码帧frame->pts=8696，它是代表这一帧或者这一秒的采样个数吗？
                             * 如果是的话，1s内不是最大采集8000个采样点吗？ 那按照pts是采样点个数的话，不是超过这个范围了吗？
                             *  答(个人理解)：首次进来frame->pts=8696，意思并不是这一帧或者这一秒的采样个数，而应该是此时读到的一个初始值，这个值可以认为是用户从网络将包解码后输入解码器。
                             *  解码器再根据其进行赋了一个初始值而已，所以它此时应该代表这个采样点个数的起始值，相当于我们传统的0初始值而已。
                             *  通过debug发现，频率=8k时，每帧的采样点个数是320左右。44.1k，采样点个数是1024左右。
                             * 实际上也类似视频，视频起始frame->pts值一般也是非0值，例如192000，单位是90k，那么起始大概是2.1333s。
                            */
                            frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                        }
                        else if (d->next_pts != AV_NOPTS_VALUE) {
                            // 如果frame->pts不正常则使用上一帧更新的next_pts和next_pts_tb。
                            // 转成{1, frame->sample_rate}。
                            frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                        }
                        if (frame->pts != AV_NOPTS_VALUE) {
                            // 记录下一帧的pts和tb。 根据当前帧的pts和nb_samples预估下一帧的pts。
                            // 首次进来之前next_pts、next_pts_tb是未赋值的。
                            d->next_pts = frame->pts + frame->nb_samples;
                            d->next_pts_tb = tb; // 设置timebase。
                        }
                    }
                    break;

                }// <== switch end ==>

                // 1.3. 检查解码是否已经结束，解码结束返回0
                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;        // 这里看到，解码结束后，会标记封装的解码器的finished=pkt_serial。
                    printf("avcodec_flush_buffers %s(%d)\n", __FUNCTION__, __LINE__);
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }

                // 1.4. 正常解码返回1
                if (ret >= 0)
                    return 1;

            } while (ret != AVERROR(EAGAIN));   // 1.5 没帧可读时ret返回EAGIN，需要继续送packet

        }// <== if(d->queue->serial == d->pkt_serial) end ==>

        // 若进了if，上面dowhile退出是因为解码器avcodec_receive_frame时返回了EAGAIN。

        // 2 获取一个packet，如果播放序列不一致(数据不连续)则过滤掉“过时”的packet
        do {
            // 2.1 如果没有数据可读则唤醒read_thread, 实际是continue_read_thread SDL_cond
            // 这里实际判断是大概，因为读线程此时可能上锁往队列扔包，但这的判断并未上锁，当然这里只是唤醒催读线程，不做也可以，但做了更好。
            if (d->queue->nb_packets == 0)
                SDL_CondSignal(d->empty_queue_cond);// 通知read_thread放入packet

            // 2.2 如果还有pending的packet则使用它。 packet_pending为1表示解码器异常导致有个包没成功解码，需要重新解码
            if (d->packet_pending) {
                av_packet_move_ref(&pkt, &d->pkt);// 将参2中的每个字段移动到参1并重置参2。类似unique_lock的所有权变动
                d->packet_pending = 0;
            } else {
                // 2.3 阻塞式读取packet。-1表示用户请求中断。
                if (packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0)
                    return -1;
            }
            if(d->queue->serial != d->pkt_serial) {
                // darren自己的代码
                printf("+++++%s(%d) discontinue:queue->serial: %d, pkt_serial: %d\n", __FUNCTION__, __LINE__, d->queue->serial, d->pkt_serial);
                av_packet_unref(&pkt); // fixed me? 释放要过滤的packet
            }
        } while (d->queue->serial != d->pkt_serial);// 如果不是同一播放序列(流不连续)则继续读取

        // 到下面，拿到的pkt肯定是与最新的d->queue->serial相等。注意：d->pkt_serial的值是由packet_queue_get()的参4传入时，由队列节点MyAVPacketList的serial更新的。
        // 这样你就理解为什么是判断d->queue->serial != d->pkt_serial，而不是判断pkt->serial，更何况pkt本身就没有serial这个成员，MyAVPacketList才有。

        // 3 若packet是flush_pkt则不送，会重置解码器，清空解码器里面的缓存帧(说明seek到来)，否则将packet送入解码器。
        if (pkt.data == flush_pkt.data) {// flush_pkt也是符合d->queue->serial=d->pkt_serial的，所以需要处理。
            // when seeking or when switching to a different stream
            // 遇到seek操作或者切换国/粤时，重置，相当于重新开始播放。
            avcodec_flush_buffers(d->avctx);    // 清空解码器里面的缓存帧
            d->finished = 0;                    // 重置为0
            d->next_pts = d->start_pts;         // 主要用在了audio
            d->next_pts_tb = d->start_pts_tb;   // 主要用在了audio
        } else {
            if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {// 主要是字幕相关，因为上面的switch (d->avctx->codec_type)是没有处理过字幕的内容。
                int got_frame = 0;
                ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &pkt);//错误时返回负值，否则返回所使用的字节数。
                if (ret < 0) {
                    // 小于0，标记为AVERROR(EAGAIN)并释放这个pkt，等下一次for循环再继续get pkt去解码。
                    // 否则大于0的情况下，还需要看got_frame的值。
                    ret = AVERROR(EAGAIN);
                } else {
                    if (got_frame && !pkt.data) {// 解字幕成功，但是该pkt.data为空，这是什么意思，pkt.data可能为空吗？答：看注释，是可能的，因为输入输出有延时所以这个函数内部会
                                                // 进行pkt.data置空，以冲刷解码器尾部的数据直到冲刷完成。所以会存在这种情况需要处理。
                        d->packet_pending = 1;              // 标记为1.
                        av_packet_move_ref(&d->pkt, &pkt);  // 此时d->pkt夺走该pkt的所有权。
                    }
                    // got_frame非0说明解字幕成功，为0说明没有字幕可以压缩。
                    // 但是ffplay做得更详细：pkt.data不为空说明此时可能解码器出问题，等下再处理，否则为空就真的说明没数据了，遇到EOF.
                    ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
                }
            } else {
                if (avcodec_send_packet(d->avctx, &pkt) == AVERROR(EAGAIN)) {
                    // 如果走进来这里，说明avcodec_send_packet、avcodec_receive_frame都返回EAGAIN，这是不正常的，需要保存这一个包到解码器中，等编码器恢复正常再拿来使用
                    av_log(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                    d->packet_pending = 1;
                    av_packet_move_ref(&d->pkt, &pkt);
                }
            }
            av_packet_unref(&pkt);	// 调用avcodec_send_packet后，pkt仍属于调用者，所以一定要自己去释放音视频数据
        }

    }//<== for end ==>

}

static void decoder_destroy(Decoder *d) {
    av_packet_unref(&d->pkt);
    avcodec_free_context(&d->avctx);
}

/**
 * @brief 取消对frame引用的所有缓冲区的引用，并重置frame字段。如果是字幕，会额外释放给字幕分配的内存。
 * @param vp 封装的帧结构体Frame。
 * @return void。
 */
static void frame_queue_unref_item(Frame *vp)
{
    av_frame_unref(vp->frame);	/* 释放数据，不是释放AVFrame本身 */
    avsubtitle_free(&vp->sub);  /* 释放给定字幕结构中所有已分配的数据 */
}

/**
 * @brief 初始化FrameQueue，视频和音频keep_last设置为1，字幕设置为0。
 * @param f 一个封装Frame的帧队列结构体，可能是视频帧、音频帧、字幕帧队列。
 * @param pktq 由播放器的包队列传入，用于保存在帧队列当中。同样可能是视频包、音频包、字幕包队列。
 * @param max_size 用户输入的帧队列最大长度，超过范围时被置为FRAME_QUEUE_SIZE。
 * @param keep_last 看结构体说明。
 * @return 成功 0； 失败 -1。
*/
static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last)
{
    int i;
    memset(f, 0, sizeof(FrameQueue));
    if (!(f->mutex = SDL_CreateMutex())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    if (!(f->cond = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    f->pktq = pktq;                                         // 帧队列里面保持着播放器里面的包队列，pktq是播放器内部的包队列
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);        // 表示用户输入的max_size的值在[负无穷,FRAME_QUEUE_SIZE]，但结合实际，负数和0应该是不存在
    f->keep_last = !!keep_last;                             // 连续关系非运算，相当于直接赋值。关系运算符结果不是0就是1.
    for (i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))        // 分配AVFrame结构体，个数为最大个数f->max_size
            return AVERROR(ENOMEM);
    return 0;
}

/**
 * @brief 释放AVFrame以及内部的数据缓冲区引用，和释放互斥锁和条件变量。 对比看frame_queue_init。
 * @param f 帧队列。
 * @return void。
 */
static void frame_queue_destory(FrameQueue *f)
{
    int i;
    // 1 释放AVFrame以及内部的数据缓冲区引用
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        // 1.1 释放对vp->frame中的数据缓冲区的引用，注意不是释放frame对象本身
        frame_queue_unref_item(vp);

        // 1.2 释放vp->frame对象
        av_frame_free(&vp->frame);
    }

    // 2 释放互斥锁和条件变量
    SDL_DestroyMutex(f->mutex);
    SDL_DestroyCond(f->cond);
}

static void frame_queue_signal(FrameQueue *f)
{
    SDL_LockMutex(f->mutex);
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

/* 获取last Frame：
 * 当rindex_shown=0时，和frame_queue_peek效果一样
 * 当rindex_shown=1时，读取的是已经显示过的frame
 */
static Frame *frame_queue_peek_last(FrameQueue *f)
{
    return &f->queue[f->rindex];    // 这时候才有意义
}

/* 获取队列当前Frame, 在调用该函数前先调用frame_queue_nb_remaining确保有frame可读 */
static Frame *frame_queue_peek(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

/* 获取当前Frame的下一Frame, 此时要确保queue里面至少有2个Frame */
// 不管你什么时候调用，返回来肯定不是 NULL
static Frame *frame_queue_peek_next(FrameQueue *f)
{
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}
/**
 * 总结上面frame_queue_peek_last、frame_queue_peek、frame_queue_peek_next三个函数的作用：
 * 1）当rindex_shown=0时，frame_queue_peek_last和frame_queue_peek效果一样，都表示获取当前帧(已显示还是未显示？留个疑问)。frame_queue_peek_next表示获取下一帧。
 *      回答疑问：表示未显示的帧，因为rindex_shown=0是第一次获取帧队列的数据，所以获取到的是未显示的帧。
 * 2）当rindex_shown=1时，frame_queue_peek_last读取的是已经显示过的frame；frame_queue_peek读取的是待显示的帧；frame_queue_peek_next表示待显示帧的下一帧。
 * 3）frame_queue_peek、frame_queue_peek_next除以f->max_size：因为帧队列有可能大于max_size的，所以必须确保使用的是已经开辟的帧内存。
 *      例如音频，max_size=9，queue帧队列是15，但是queue->frame是没有alloc内存的，所以必须除以max_size，确保合法。
 *
 * 想要debug第1）第2）点很简单，以视频为例，在video_refresh()函数中的lastvp = frame_queue_peek_last(&is->pictq);位置打个断点：
 *      第一次进来时，帧队列的rindex_shown=0，那么frame_queue_peek_last和frame_queue_peek获取到相同的未显示帧，显示完毕后，会调用frame_queue_next(&is->pictq);
 *          但是因为rindex_shown=0并且keep_last=1，所以会rindex不会改变，rindex_shown则会变成1.
 *      第二次进来时，帧队列的rindex_shown=1，所以frame_queue_peek_last获取到的是已经显示过的帧，而frame_queue_peek获取到的是待显示的帧。显示完毕后，
 *          会调用frame_queue_next(&is->pictq);此时rindex=0的帧被清空(av_frame本身不会释放)，然后rindex指向下一个。
 *      以此类推，所以当rindex_shown=1时(非第一次流程)，rindex都指向已经显示过的帧。
 *
 */

/**
 * @brief 获取队列中可写入的帧内存，用于写入。
 * @param f 帧队列。
 * @return 成功返回可写入的帧内存地址； 用户中断返回NULL。
 */
static Frame *frame_queue_peek_writable(FrameQueue *f)
{
    /* wait until we have space to put a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size >= f->max_size &&    // 当前帧数到达最大帧数，阻塞在条件变量，等待消费唤醒后才能再写入。
           !f->pktq->abort_request) {	/* 或者用户请求退出，1表示退出 */
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)			 /* 检查是不是要退出 */
        return NULL;

    return &f->queue[f->windex];        // windex的自增是在frame_queue_push内处理的。
}

static Frame *frame_queue_peek_readable(FrameQueue *f)
{
    /* wait until we have a readable a new frame */
    SDL_LockMutex(f->mutex);
    while (f->size - f->rindex_shown <= 0 &&
           !f->pktq->abort_request) {
        SDL_CondWait(f->cond, f->mutex);
    }
    SDL_UnlockMutex(f->mutex);

    if (f->pktq->abort_request)
        return NULL;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

/**
 * @brief 更新写下标和队列的帧数量。
 * @param f 帧队列。
 * @return void。
 */
static void frame_queue_push(FrameQueue *f)
{
    // 1. 如果写下标到末尾再+1，则回到开头。
    // 写下标不用加锁是因为，写下标只在写线程维护，同样读下标只在读线程维护，所以不用上锁。而下面的size则需要，因为读写线程都用到。
    if (++f->windex == f->max_size)
        f->windex = 0;

    // 2. 更新帧队列的帧数量，并且唤醒。
    SDL_LockMutex(f->mutex);
    f->size++;
    SDL_CondSignal(f->cond);    // 当_readable在等待时则可以唤醒
    SDL_UnlockMutex(f->mutex);
}

/**
 * @brief 释放当前frame，并更新读索引rindex与当前队列的总帧数。
 * @param f 帧队列。
 * @return void。
*/
static void frame_queue_next(FrameQueue *f)
{
    // 1. 当keep_last为1, rindex_show为0时不去更新rindex,也不释放当前frame
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1; // 第一次进来没有更新，对应的frame就没有释放
        return;
    }

    // 2. 释放当前frame，并更新读索引rindex与当前队列的总帧数。
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)// 如果此时rindex是队列的最尾部元素，则读索引更新为开头索引即0.
        f->rindex = 0;
    SDL_LockMutex(f->mutex);
    f->size--;                      // 当前队列总帧数减1.
    SDL_CondSignal(f->cond);
    SDL_UnlockMutex(f->mutex);
}

/*
 * return the number of undisplayed frames in the queue
 * 返回队列中未显示帧的数量
*/
static int frame_queue_nb_remaining(FrameQueue *f)
{
    return f->size - f->rindex_shown;	// 注意这里为什么要减去f->rindex_shown
                                        // rindex_shown为1时，队列中总是保留了最后一帧lastvp
                                        // 在计算队列当前Frame数量是不包含lastvp，故需要减去rindex_shown。
                                        // 关键：lastvp保存在队列的哪个位置？
                                        // 答：lastvp是待显示帧或者待显示帧的上一帧，当rindex_shown=0，lastvp也是未显示帧，
                                        // 当rindex_shown=1，lastvp是待显示帧的上一帧，此时rindex的位置就是lastvp的位置。
}

/* return last shown position */
static int64_t frame_queue_last_pos(FrameQueue *f)
{
    // 返回上一帧的pos位置，由于只有调用frame_queue_next了，rindex_shown才会置1，也就是说，只有出过队列，才会存在上一帧，
    // 没有出过队列那么rindex_shown=0，不会存在上一帧，所以返回-1.
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}

static void decoder_abort(Decoder *d, FrameQueue *fq)
{
    packet_queue_abort(d->queue);   // 终止packet队列，packetQueue的abort_request被置为1
    frame_queue_signal(fq);         // 唤醒Frame队列, 以便退出
    SDL_WaitThread(d->decoder_tid, NULL);   // 等待解码线程退出
    d->decoder_tid = NULL;          // 线程ID重置
    packet_queue_flush(d->queue);   // 情况packet队列，并释放数据
}

static inline void fill_rectangle(int x, int y, int w, int h)
{
    SDL_Rect rect;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    if (w && h)
        SDL_RenderFillRect(renderer, &rect);
}

/**
 * @brief 若传进来的帧属性与纹理的像素属性不一样，该纹理会释放掉并重新创建，并根据init_texture是否初始化纹理的像素数据。
 *          如果传进来的帧属性与纹理的像素属性一样，则什么也不做。
 *
 * @param texture 纹理。
 * @param new_format 根据FFmpeg得到的SDL像素格式。
 * @param new_width 帧的宽度。
 * @param new_height 帧的高度。
 * @param blendmode 根据FFmpeg得到的SDL的混合模式。
 * @param init_texture 是否重新初始化纹理的像素数据。一般传0即可。
 *
 * @return 成功0； 失败-1.
 */
static int realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height,
                           SDL_BlendMode blendmode, int init_texture)
{
    Uint32 format;
    int access, w, h;

    /*
     * 1）SDL_QueryTexture：用于检索纹理的基本设置，包括格式，访问，宽度和高度。
     * 2）SDL_LockTexture：用于锁住纹理的可写部分。参1：纹理。参2：传出的像素。参3：传出，一行像素数据中的字节数，包括行之间的填充。字节数通常由纹理的格式决定。
    */

    // 1. 若传进来的帧属性与纹理的像素属性不一样，该纹理会释放掉并重新创建。
    if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
        void *pixels;
        int pitch;
        if (*texture)
            SDL_DestroyTexture(*texture);
        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        if (init_texture) {
            if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)// 要访问纹理的像素数据，必须使用函数SDL_LockTexture()
                return -1;
            memset(pixels, 0, pitch * new_height);          // 初始化纹理的像素数据。pitch：一行像素数据中的字节数，包括行之间的填充。字节数通常由纹理的格式决定。
                                                            // 所以pitch * new_height就是这个纹理的总字节大小。
                                                            // 例如960x540，格式为YUV420p，一个像素包含一个y和0.25u和0.25v，那么一行像素数据中的字节数pitch=960(1+0.25+0.25)=1440
                                                            // 例如1920x1080，格式为ARGB，一个像素包含a、r、g、b各一个，那么一行像素数据中的字节数pitch=1920x4=7680
            SDL_UnlockTexture(*texture);
        }
        av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
    }

    return 0;
}

// 4.2.0 和4.0有区别
/**
 * @brief 将帧宽高按照sar最大适配到窗口。算法是这样的：
 *  1）如果用户不指定SDL显示屏幕的大小，则按照视频流分辨率的真实大小进行显示。例如测试时不加-x-y参数。
 *  2）若用户指定SDL显示屏幕的大小，按照视频流分辨率的真实大小的宽高比进行计算，先以用户的-x宽度为基准，若不符合则再以-y高度为基准，最终得出用户指定的屏幕显示宽高和起始坐标。
 *
 * 总结：总的思路就是，如何将视频流不一样的分辨率，输出到指定的屏幕上面显示。这也是我们面对的需求。
 *
 * @param rect      获取到的SDL屏幕起始位置和解码后的画面的宽高。
 * @param scr_xleft 窗口显示起始x位置,这里说的是内部显示的坐标, 不是窗口在整个屏幕的起始位置。
 * @param scr_ytop  窗口显示起始y位置。
 * @param scr_width 窗口宽度，可以是视频帧的原分辨率或者是用户指定的-x参数。
 * @param scr_height窗口高度，可以是视频帧的原分辨率或者是用户指定的-y参数。
 * @param pic_width 视频显示帧的真实宽度。
 * @param pic_height视频显示帧的真实高度。
 * @param pic_sar   显示帧宽高比，只是参考该传入参数。
 *
 * calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
 */
static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar)
{
    AVRational aspect_ratio = pic_sar; // 比率
    int64_t width, height, x, y;

    // 1. 判断传进来的宽高比是否有效，无效会被置为1:1
    // 例如(9,16)与(0,1)比较之后，返回1.
    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);// 如果aspect_ratio是负数或者为0,设置为1:1。一般实时流都会走这里。

    // 2. 重新计算宽高比，转成真正的播放比例。不过大多数执行到这后，aspect_ratio={1,1}。所以宽高比就是pic_width/pic_height,一般是1920/1080=16:9。
    // 他这里曾经提到过如果传进来的pic_sar不是1:1的，那么aspect_ratio宽高比将被重新计算，
    // 导致宽高也会不一样。在一些录制可能会存在问题，这里先留个疑问。
    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    // 3. 根据上面得出的宽高比，求出用户指定的屏幕显示宽高。
    /* XXX: we suppose the screen has a 1.0 pixel ratio(我们假设屏幕的像素比为1.0) */
    // 计算显示视频帧区域的宽高.
    // 下面可以用用户指定-x 720 -y 480去代入理解。或者不传-x -y参数去理解，不难。
    // 先以高度为基准
    height = scr_height;
    // &~1, 表示如果结果是奇数的话，减去1，取偶数宽度，~1二进制为1110
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;// 例如用户指定720x480窗口播放，那么height=480，第一码流是1920/1080=16:9。算出whidth=853.333&~1=852.
    if (width > scr_width) {
        // 当以高度为基准,发现计算出来的需要的窗口宽度不足时，调整为以窗口宽度为基准。不过需要注意：计算公式是不一样的，可以由宽高比相等的公式求出。x1/y1=x2/y2.
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;// 例如720x9/16=405&~1=404
    }

    // 4. 计算显示视频帧区域的起始坐标（在显示窗口内部的区域）
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;// 不论以高度还是宽度为基准，求出的x、y都会有一个是0.
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX((int)width,  1);
    rect->h = FFMAX((int)height, 1);
}

/**
 * @brief 根据FFmpeg的像素格式，获取SDL像素格式和混合模式。
 * @param format 传入，FFmpeg的像素格式。
 * @param sdl_pix_fmt 传入传出，SDL的像素格式。
 * @param sdl_blendmode  传入传出，SDL的混合模式。
 * @return void.
 */
static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode)
{
    int i;
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;

    // 1. 如果FFmpeg格式为下面四种，则获取SDL的混合模式并通过参数传出。如果不是，SDL的混合模式是SDL_BLENDMODE_NONE。
    if (format == AV_PIX_FMT_RGB32   ||
            format == AV_PIX_FMT_RGB32_1 ||
            format == AV_PIX_FMT_BGR32   ||
            format == AV_PIX_FMT_BGR32_1)
        *sdl_blendmode = SDL_BLENDMODE_BLEND;

    // 2. 遍历FFmpeg PIX_FMT to SDL_PIX的映射关系数组，如果在数组中找到该格式，
    //    说明FFmpeg和SDL都支持该格式，那么直接返回，由参数传出。找不到则为SDL_PIXELFORMAT_UNKNOWN。
    for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

/**
 * @brief 将帧的数据更新填充到纹理当中。
 * @param tex 纹理。
 * @param frame 要显示的一帧图片。
 * @param img_convert_ctx 图像尺寸转换上下文。
 * @return 成功0；失败返回-1.
*/
static int upload_texture(SDL_Texture **tex, AVFrame *frame, struct SwsContext **img_convert_ctx) {
    int ret = 0;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;

    // 1. 根据frame中的图像格式(FFmpeg像素格式)，获取对应的SDL像素格式和blendmode
    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);

    // 2. 判断是否需要重新开辟纹理。
    // 参数tex实际是&is->vid_texture。
    if (realloc_texture(tex,
                        sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt,/* 为空则使用ARGB8888 */
                        frame->width, frame->height, sdl_blendmode, 0) < 0)
        return -1;

    // 3. 根据sdl_pix_fmt从AVFrame中取数据填充纹理
    switch (sdl_pix_fmt) {
     // 3.1 frame格式是SDL不支持的格式，则需要进行图像格式转换，转换为目标格式AV_PIX_FMT_BGRA，对应SDL_PIXELFORMAT_BGRA32。
     //     这应该只发生在我们不使用avfilter的情况下。
    case SDL_PIXELFORMAT_UNKNOWN:
        /*
         * 1）sws_getCachedContext：检查上下文是否可以重用，否则重新分配一个新的上下文。
         * 如果context是NULL，只需调用sws_getContext()来获取一个新的上下文。否则，检查参数是否已经保存在上下文中，
         * 如果是这种情况，返回当前的上下文。否则，释放上下文并获得一个新的上下文新参数。
         *
         * 2）sws_scale：尺寸转换函数：
         * 参1：上下文。
         * 参2：包含指向源切片平面的指针的数组。
         * 参3：包含源图像每个平面的步长的数组。
         * 参4：要处理的切片在源图像中的位置，即切片第一行在图像中从0开始计算的数字。
         * 参5：源切片的高度，即切片中的行数。
         * 参6：数组，其中包含指向目标图像平面的指针，即输出指针。
         * 参7：该数组包含目标图像的每个平面的步长。由于这里是RGB，所以数组长度为4即可。即三种颜色加上对比度RGB+A。
        */
        *img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
                                                frame->width, frame->height, frame->format,
                                                frame->width, frame->height, AV_PIX_FMT_BGRA,   // 若SDL不支持，默认转成AV_PIX_FMT_BGRA
                                                sws_flags, NULL, NULL, NULL);
        if (*img_convert_ctx != NULL) {
            uint8_t *pixels[4]; // 之前取Texture的缓存。用于保存输出的帧数据缓存。例如帧格式是yuv或者rgb，那么数组大小是3，若是argb，那么大小是4，也就是帧的格式的分量最多是4个。所以这里取数组大小是4.
            int pitch[4];       // pitch：包含着pixels每个数组的长度。即每个分量的长度，例如yuv，y是1024，那么pitch[0]=1024.
            if (!SDL_LockTexture(*tex, NULL, (void **)pixels, pitch)) {
                sws_scale(*img_convert_ctx, (const uint8_t * const *)frame->data, frame->linesize,
                          0, frame->height, pixels, pitch);// 通过这个函数，就将frame的数据填充到纹理当中。
                SDL_UnlockTexture(*tex);
            }
        } else {
            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
            ret = -1;
        }
        break;

    // 3.2 frame格式对应SDL_PIXELFORMAT_IYUV，不用进行图像格式转换，对应的是FFmpeg的AV_PIX_FMT_YUV420P。
    //      直接调用SDL_UpdateYUVTexture()更新SDL texture。
    // 经常更新纹理使用SDL_LockTexture+SDL_UnlockTexture。不经常则可以直接使用SDL_UpdateTexture、SDL_UpdateYUVTexture等SDL_Updatexxx函数。
    case SDL_PIXELFORMAT_IYUV:
        if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {// yuv的3平面长度都是正数的情况下，大多数走这里
            ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
                    frame->data[1], frame->linesize[1],
                    frame->data[2], frame->linesize[2]);
        } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {// yuv的3平面长度都是负数的情况下，这里可以先忽略，后续再研究
            ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0],
                    frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                    frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
        } else {
            // 不支持混合的正负线宽
            av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
            return -1;
        }
        break;

    // 3.3 frame格式对应其他SDL像素格式，不用进行图像格式转换，直接调用SDL_UpdateTexture()更新SDL texture
    default:
        if (frame->linesize[0] < 0) {
            ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);// 这里可以先忽略，后续再研究
        } else {
            ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
        }
        break;
    }

    return ret;
}

/**
 * @brief 设置YUV转换模式。旧版本的SDL不支持，新版本2,0,8以上才支持。
 * @param frame 解码后的帧。
 * @return void。
 */
static void set_sdl_yuv_conversion_mode(AVFrame *frame)
{
#if SDL_VERSION_ATLEAST(2,0,8)
    SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
    if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 || frame->format == AV_PIX_FMT_UYVY422)) {
        if (frame->color_range == AVCOL_RANGE_JPEG)
            mode = SDL_YUV_CONVERSION_JPEG;
        else if (frame->colorspace == AVCOL_SPC_BT709)
            mode = SDL_YUV_CONVERSION_BT709;
        else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M || frame->colorspace == AVCOL_SPC_SMPTE240M)
            mode = SDL_YUV_CONVERSION_BT601;
    }

    // 最终调用这个函数来设置YUV转换模式
    SDL_SetYUVConversionMode(mode);
#endif
}

/**
 * @brief 将帧数据填充到纹理，以及纹理数据拷贝到渲染器。
 * @param is 播放器实例。
 * @return void.
*/
static void video_image_display(VideoState *is)
{
    Frame *vp;
    Frame *sp = NULL;
    SDL_Rect rect;

    // 1. 获取未显示的一帧
    // keep_last+rindex_shown保存上一帧的作用就出来了,我们是有调用frame_queue_next, 但最近出队列的帧并没有真正销毁
    // 所以这里可以读取出来显示.
    // 注意：在video_refresh我们称frame_queue_peek_last()获取到是已显示的帧，但是这里是调用frame_queue_peek_last获取到的是未显示的帧。
    // 因为在显示之前我们不是调用了frame_queue_next嘛。
    vp = frame_queue_peek_last(&is->pictq);

    // 2. 判断是否有字幕流，有则处理，没有则啥也不做。
    if (is->subtitle_st) {
        // 2.1 判断队列是否有字幕帧，有则处理，否则啥也不做。
        if (frame_queue_nb_remaining(&is->subpq) > 0) {
            sp = frame_queue_peek(&is->subpq);

            // 2.2 start_display_time是什么意思？留个疑问
            printf("tyycode, sp->pts: %lf, sp->sub.start_display_time: %u\n", sp->pts, sp->sub.start_display_time);
            if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {

                // 2.3 如果未显示，则处理，否则啥也不做。
                if (!sp->uploaded) {
                    uint8_t* pixels[4];
                    int pitch[4];
                    int i;
                    // 2.4 如果字幕帧没有分辨率，则以视频帧分辨率赋值。
                    if (!sp->width || !sp->height) {
                        sp->width = vp->width;
                        sp->height = vp->height;
                    }
                    // 2.5 不管字幕的纹理是否存在，都释放然后重新开辟。
                    if (realloc_texture(&is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
                        return;

                    // 2.6 将字幕帧的每个子矩形进行格式转换，转换后的数据保存在字幕纹理sub_texture中。
                    for (i = 0; i < sp->sub.num_rects; i++) {
                        AVSubtitleRect *sub_rect = sp->sub.rects[i];
                        // 重置sub_rect的x、y、w、h，并使用av_clip确保x、y、w、h在[0, xxx]范围。
                        // 实际上就是确保子矩形在字幕帧这个矩形的范围内。
                        sub_rect->x = av_clip(sub_rect->x, 0, sp->width );
                        sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                        sub_rect->w = av_clip(sub_rect->w, 0, sp->width  - sub_rect->x);
                        sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                        // 检查上下文是否可以重用，否则重新分配一个新的上下文。
                        // 从8位的调色板AV_PIX_FMT_PAL8格式转到AV_PIX_FMT_BGRA的格式。子矩形应该默认就是AV_PIX_FMT_PAL8的格式。
                        is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                                                                   sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                                                                   sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                                                                   0, NULL, NULL, NULL);
                        if (!is->sub_convert_ctx) {
                            av_log(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                            return;
                        }

                        // 此时，得到了输出格式为AV_PIX_FMT_BGRA字幕尺寸格式变换上下文，那么可以调用sws_scale进行转换了。
                        // 转换的意义在于使SDL支持这种格式进行渲染。
                        // 首先锁定字幕纹理中(相当于字幕帧矩形)的子矩形的区域。pixels、pitch分别是指锁定的子矩形的数据与一行字节数，可看函数声明。
                        if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)pixels, pitch)) {
                            sws_scale(is->sub_convert_ctx, (const uint8_t * const *)sub_rect->data, sub_rect->linesize,
                                      0, sub_rect->h, pixels, pitch);// 转完格式后，就将sub_rect->data的数据填充到纹理当中，等待渲染即可显示。
                            SDL_UnlockTexture(is->sub_texture);
                        }
                    }// <== for (i = 0; i < sp->sub.num_rects; i++) end ==>

                    sp->uploaded = 1;// 标记为已显示。
                }
            } else
                sp = NULL;
        }
    }

    // 3. 计算要视频帧宽度要在屏幕显示的大小。
    // 将帧宽高按照sar最大适配到窗口，并通过rect返回视频帧在窗口的显示位置和宽高
    calculate_display_rect(&rect, is->xleft, is->ytop, is->width, is->height,
                           vp->width, vp->height, vp->sar);
    //    rect.x = rect.w /2;   // 测试
    //    rect.w = rect.w /2;   // 缩放实际不是用sws， 缩放是sdl去做的

    // 4. 如果没有显示则将帧的数据填充到纹理当中。 但还没显示，显示需要在渲染器显示。
    if (!vp->uploaded) {
        // 把yuv数据更新到vid_texture
        if (upload_texture(&is->vid_texture, vp->frame, &is->img_convert_ctx) < 0)
            return;
        vp->uploaded = 1;       // 标记该帧已显示
        vp->flip_v = vp->frame->linesize[0] < 0;// 记录是正常播放还是垂直翻转播放，即frame->linesize[0]为负数时代表垂直翻转播放，
                                                // 到时理解upload_texture内部判断frame->linesize[0]为负，可以容易理解。
    }

    // 经过上面的处理，视频帧、字幕帧都已经转换为SDL支持的显示格式，并都保存在各自的纹理当中，剩下就是渲染显示了。

    // 5. 拷贝视频的纹理到渲染器。具体是设置yuv的转换格式以及拷贝视频纹理的像素数据到渲染器。
    /*
     * SDL_RenderCopyEx：将部分源纹理复制到当前渲染目标，并围绕给定的中心旋转角度。
     * 参1 renderer：渲染器。
     * 参2 texture：纹理。
     * 参3 srcrect：指向源矩形的指针，或者为NULL，表示整个纹理。
     * 参4 dstrect：指向目标矩形的指针，或者为NULL，表示整个渲染目标。
     * 参5 angle：表示将应用于参4的旋转的角度.
     * 参6 center：A pointer to a point indicating the point around which dstrect
     *      will be rotated (if NULL, rotation will be done aroud dstrect.w/2, dstrect.h/2)。
     * 参7 flip：一个SDL_RendererFlip值，说明应该在纹理上执行哪些翻转动作。
    */
    set_sdl_yuv_conversion_mode(vp->frame);// 根据帧的属性来设置yuv的转换格式
    SDL_RenderCopyEx(renderer, is->vid_texture, NULL, &rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : 0);  // 到这一步后，我们只需要执行SDL_RenderPresent就可以显示图片了
    set_sdl_yuv_conversion_mode(NULL);      // 重设为自动选择，传空表示自动选择yuv的转换格式(SDL_YUV_CONVERSION_AUTOMATIC)。

    // 6. 拷贝字幕的纹理到渲染器。
    if (sp) {// 有字幕的才会进来
#if USE_ONEPASS_SUBTITLE_RENDER
        SDL_RenderCopy(renderer, is->sub_texture, NULL, &rect);
#else
        int i;
        double xratio = (double)rect.w / (double)sp->width;
        double yratio = (double)rect.h / (double)sp->height;
        for (i = 0; i < sp->sub.num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect*)sp->sub.rects[i];
            SDL_Rect target = {.x = rect.x + sub_rect->x * xratio,
                               .y = rect.y + sub_rect->y * yratio,
                               .w = sub_rect->w * xratio,
                               .h = sub_rect->h * yratio};
            SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
        }
#endif
    }
}

static inline int compute_mod(int a, int b)
{
    return a < 0 ? a%b + b : a%b;
}

static void video_audio_display(VideoState *s)
{
    int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
    int ch, channels, h, h2;
    int64_t time_diff;
    int rdft_bits, nb_freq;

    for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++)
        ;
    nb_freq = 1 << (rdft_bits - 1);

    /* compute display index : center on currently output samples */
    channels = s->audio_tgt.channels;
    nb_display_channels = channels;
    if (!s->paused) {
        int data_used= s->show_mode == SHOW_MODE_WAVES ? s->width : (2*nb_freq);
        n = 2 * channels;
        delay = s->audio_write_buf_size;
        delay /= n;

        /* to be more precise, we take into account the time spent since
           the last buffer computation */
        if (audio_callback_time) {
            time_diff = av_gettime_relative() - audio_callback_time;
            delay -= (time_diff * s->audio_tgt.freq) / 1000000;
        }

        delay += 2 * data_used;
        if (delay < data_used)
            delay = data_used;

        i_start= x = compute_mod(s->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
        if (s->show_mode == SHOW_MODE_WAVES) {
            h = INT_MIN;
            for (i = 0; i < 1000; i += channels) {
                int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
                int a = s->sample_array[idx];
                int b = s->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
                int c = s->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
                int d = s->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
                int score = a - d;
                if (h < score && (b ^ c) < 0) {
                    h = score;
                    i_start = idx;
                }
            }
        }

        s->last_i_start = i_start;
    } else {
        i_start = s->last_i_start;
    }

    if (s->show_mode == SHOW_MODE_WAVES) {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

        /* total height for one channel */
        h = s->height / nb_display_channels;
        /* graph height / 2 */
        h2 = (h * 9) / 20;
        for (ch = 0; ch < nb_display_channels; ch++) {
            i = i_start + ch;
            y1 = s->ytop + ch * h + (h / 2); /* position of center line */
            for (x = 0; x < s->width; x++) {
                y = (s->sample_array[i] * h2) >> 15;
                if (y < 0) {
                    y = -y;
                    ys = y1 - y;
                } else {
                    ys = y1;
                }
                fill_rectangle(s->xleft + x, ys, 1, y);
                i += channels;
                if (i >= SAMPLE_ARRAY_SIZE)
                    i -= SAMPLE_ARRAY_SIZE;
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);

        for (ch = 1; ch < nb_display_channels; ch++) {
            y = s->ytop + ch * h;
            fill_rectangle(s->xleft, y, s->width, 1);
        }
    } else {
        if (realloc_texture(&s->vis_texture, SDL_PIXELFORMAT_ARGB8888, s->width, s->height, SDL_BLENDMODE_NONE, 1) < 0)
            return;

        nb_display_channels= FFMIN(nb_display_channels, 2);
        if (rdft_bits != s->rdft_bits) {
            av_rdft_end(s->rdft);
            av_free(s->rdft_data);
            s->rdft = av_rdft_init(rdft_bits, DFT_R2C);
            s->rdft_bits = rdft_bits;
            s->rdft_data = av_malloc_array(nb_freq, 4 *sizeof(*s->rdft_data));
        }
        if (!s->rdft || !s->rdft_data){
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
            s->show_mode = SHOW_MODE_WAVES;
        } else {
            FFTSample *data[2];
            SDL_Rect rect = {.x = s->xpos, .y = 0, .w = 1, .h = s->height};
            uint32_t *pixels;
            int pitch;
            for (ch = 0; ch < nb_display_channels; ch++) {
                data[ch] = s->rdft_data + 2 * nb_freq * ch;
                i = i_start + ch;
                for (x = 0; x < 2 * nb_freq; x++) {
                    double w = (x-nb_freq) * (1.0 / nb_freq);
                    data[ch][x] = s->sample_array[i] * (1.0 - w * w);
                    i += channels;
                    if (i >= SAMPLE_ARRAY_SIZE)
                        i -= SAMPLE_ARRAY_SIZE;
                }
                av_rdft_calc(s->rdft, data[ch]);
            }
            /* Least efficient way to do this, we should of course
             * directly access it but it is more than fast enough. */
            if (!SDL_LockTexture(s->vis_texture, &rect, (void **)&pixels, &pitch)) {
                pitch >>= 2;
                pixels += pitch * s->height;
                for (y = 0; y < s->height; y++) {
                    double w = 1 / sqrt(nb_freq);
                    int a = sqrt(w * sqrt(data[0][2 * y + 0] * data[0][2 * y + 0] + data[0][2 * y + 1] * data[0][2 * y + 1]));
                    int b = (nb_display_channels == 2 ) ? sqrt(w * hypot(data[1][2 * y + 0], data[1][2 * y + 1]))
                            : a;
                    a = FFMIN(a, 255);
                    b = FFMIN(b, 255);
                    pixels -= pitch;
                    *pixels = (a << 16) + (b << 8) + ((a+b) >> 1);
                }
                SDL_UnlockTexture(s->vis_texture);
            }
            SDL_RenderCopy(renderer, s->vis_texture, NULL, NULL);
        }
        if (!s->paused)
            s->xpos++;
        if (s->xpos >= s->width)
            s->xpos= s->xleft;
    }
}

/**
 * @brief
 * @param is 播放器的封装结构体。
 * @param stream_index 流下标。
 * @return void。
 */
static void stream_component_close(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    // 非法的流下标直接返回
    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;

    // 获取流的编解码器参数
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        decoder_abort(&is->auddec, &is->sampq);
        SDL_CloseAudioDevice(audio_dev);
        decoder_destroy(&is->auddec);
        swr_free(&is->swr_ctx);
        av_freep(&is->audio_buf1);
        is->audio_buf1_size = 0;
        is->audio_buf = NULL;

        if (is->rdft) {
            av_rdft_end(is->rdft);
            av_freep(&is->rdft_data);
            is->rdft = NULL;
            is->rdft_bits = 0;
        }
        break;
    case AVMEDIA_TYPE_VIDEO:
        decoder_abort(&is->viddec, &is->pictq);
        decoder_destroy(&is->viddec);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        decoder_abort(&is->subdec, &is->subpq);
        decoder_destroy(&is->subdec);
        break;
    default:
        break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        is->audio_st = NULL;
        is->audio_stream = -1;
        break;
    case AVMEDIA_TYPE_VIDEO:
        is->video_st = NULL;
        is->video_stream = -1;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        is->subtitle_st = NULL;
        is->subtitle_stream = -1;
        break;
    default:
        break;
    }
}

/**
 * @brief 清理播放器中的内容。stream_open失败或者程序结束do_exit时调用。
 * @param 播放器实例。
 * @return void。
 */
static void stream_close(VideoState *is)
{
    // 动态(线程/callback)的先停止退出
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;				// 播放器的请求退出

    SDL_WaitThread(is->read_tid, NULL);	// 等待数据读取线程退出

    /* close each stream */
    if (is->audio_stream >= 0)
        stream_component_close(is, is->audio_stream);
    if (is->video_stream >= 0)
        stream_component_close(is, is->video_stream);
    if (is->subtitle_stream >= 0)
        stream_component_close(is, is->subtitle_stream);

    avformat_close_input(&is->ic);

    /* 清理包队列 */
    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    /* 清理帧队列(free all pictures) */
    frame_queue_destory(&is->pictq);
    frame_queue_destory(&is->sampq);
    frame_queue_destory(&is->subpq);

    SDL_DestroyCond(is->continue_read_thread);

    sws_freeContext(is->img_convert_ctx);
    sws_freeContext(is->sub_convert_ctx);

    /* 由av_strdup(filename)开辟的内存，需要释放 */
    av_free(is->filename);

    if (is->vis_texture)
        SDL_DestroyTexture(is->vis_texture);
    if (is->vid_texture)
        SDL_DestroyTexture(is->vid_texture);
    if (is->sub_texture)
        SDL_DestroyTexture(is->sub_texture);

    /* 最后释放播放器本身 */
    av_free(is);
}

static void do_exit(VideoState *is)
{
    if (is) {
        stream_close(is);
    }
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    uninit_opts();
#if CONFIG_AVFILTER
    av_freep(&vfilters_list);
#endif
    avformat_network_deinit();
    if (show_status)
        printf("\n");
    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);
}

static void sigterm_handler(int sig)
{
    exit(123);
}

/**
 * @brief 通过视频流的分辨率，来计算出SDL屏幕的显示窗口的起始坐标、宽高，
 *          结果保存在局部变量rect，但是只用到宽高，最终结果保存在全局变量default_width、default_height。
 *
 * @param width  视频的真实宽度。
 * @param height 视频的真实高度。
 * @param sar 宽高比，这里只是参考值，内部calculate_display_rect会进行修改。
 * @return void。
 *
 * set_default_window_size(codecpar->width, codecpar->height, sar);
 */
static void set_default_window_size(int width, int height, AVRational sar)
{
    SDL_Rect rect;
    // screen_width和screen_height可以在ffplay启动时设置 -x screen_width -y screen_height获取指定 的宽高，
    // 如果没有指定，则max_height = height，即是视频帧的高度。
    int max_width  = screen_width  ? screen_width  : INT_MAX; // 确定是否指定窗口最大宽度
    int max_height = screen_height ? screen_height : INT_MAX; // 确定是否指定窗口最大高度
    if (max_width == INT_MAX && max_height == INT_MAX){
        max_height = height;                                  // 没有指定最大高度时，则使用传进来视频的高度作为最大高度
        //max_width  = width;                                   // tyycode，最好加，不然scr_width是一个很大的数值，虽然效果一样。
    }


    // 重点在calculate_display_rect()函数，计算完后，SDL显示屏幕的起始坐标、宽高保存在rect。
    calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
    default_width  = rect.w; // 更新全局变量，实际是渲染区域的宽高，即解码后的图片的宽高。
    default_height = rect.h;
}

static int video_open(VideoState *is)
{
    int w,h;

    w = screen_width ? screen_width : default_width;
    h = screen_height ? screen_height : default_height;

    if (!window_title)
        window_title = input_filename;
    SDL_SetWindowTitle(window, window_title);

    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, screen_left, screen_top);
    if (is_full_screen)
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    SDL_ShowWindow(window);// 最终显示窗口的函数调用

    is->width  = w;         // 保存屏幕要显示的宽度和高度
    is->height = h;

    return 0;
}

/* display the current picture, if any */
/**
 * @brief 显示一帧图片，内部实现很简单，并且到了这里，已经不涉及到了音视频同步的内容。
 * @param is 播放器实例。
 * @return void.
*/
static void video_display(VideoState *is)
{
    if (!is->width)
        video_open(is); //如果窗口未显示，则显示窗口

    // 1. 设置渲染器绘图颜色
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);

    // 2. 每次重新画图一帧都应该清除掉渲染器的内容
    SDL_RenderClear(renderer);

    // 3. 图形化显示仅有音轨的文件
    if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO)
        video_audio_display(is);
    else if (is->video_st)  // 4. 显示一帧视频画面。 video_image_display内部实际上就是做SDL_RenderCopy，即帧数据填充到纹理，以及纹理数据拷贝到渲染器。
        video_image_display(is);

    // 经过上面，视频帧、字幕帧的纹理数据都均拷贝到渲染器了，那么直接显示即可。

    // 5. 显示到渲染器(最终显示一帧图片的函数)。
    SDL_RenderPresent(renderer);
}

/**
 * @brief 在下次set_clock对时前，通过 get_clock 来查询时间，判断pts是否可用。
 * @param 时钟。
 * @return 序列号不一样：返回NAN ； 暂停：直接返回set_clock对时时的pts。 否则通过pts_drift返回校正后的pts。返回值的单位是秒。
 *
 * 关于set_clock_at对时和get_clock查询时间的代码，可以看ffplay播放器-11音视频同步基础.pdf。
 *
 * 获取到的实际上是:最后一帧的pts 加上 从处理最后一帧开始到现在的时间,具体参考set_clock_at 和get_clock的代码。
 * c->pts_drift=最后一帧的pts-从处理最后一帧时间
 * clock=c->pts_drift+现在的时候
 * get_clock(&is->vidclk) ==is->vidclk.pts, av_gettime_relative() / 1000000.0 -is->vidclk.last_updated  +is->vidclk.pts
 */
static double get_clock(Clock *c)
{
    if (*c->queue_serial != c->serial)
        return NAN; // 不是同一个播放序列，时钟是无效
    if (c->paused) {
        return c->pts;  // 暂停的时候返回的是pts
    } else {
        // 流正常播放一般都会走这里
        double time = av_gettime_relative() / 1000000.0;
        // 1）c->pts_drift + time：代表本次的pts，因为系统时钟time是一直变化的，需要利用它和set_clock对时得出的pts_drift重新计算。
        // 2）(time - c->last_updated)：代表此时系统时间和上一次系统时间的间隔。
        // 3）(1.0 - c->speed)：主要用来控制播放速度。例如倍速speed=2，假设上一次系统时间到本次系统时间间隔=1s，
        //      c->pts_drift + time=5s，本次time=6.04s，那么上一次time=5.04s，得出pts_drift=-40ms；
        //      所以正常倍速=1来说，本次显示的时间是6s，
        //      但是2倍速后，(time - c->last_updated) * (1.0 - c->speed) = 1*(-1)=-1。   6-(-1)=7s。可以看到即使是2倍速，pts也不一定是成倍增加的。
        //      看不懂也没关系，因为正常播放时，c->speed都是1。所以可以先不用考虑 c->pts_drift + time 后面的内容。
        //      ffpaly好像也没有提供变速的参数。
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

/**
 * @brief 设置时钟的pts、last_updated、pts_drift、serial。
 * @param c 播放器中的时钟。
 * @param pts 当前帧的pts，传入时是gcc的一种内建函数，宏为NAN。
 * @param serial 序列号，初始化时是-1.
 * @param time 当前的系统实际，单位秒。
 *
 * @return void。
 */
static void set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts          = pts;                  /* 当前帧的pts */
    c->last_updated = time;                 /* 最后更新的时间，实际上是当前的一个系统时间 */
    c->pts_drift	= c->pts - time;        /* 当前帧pts和系统时间的差值，正常播放情况下两者的差值应该是比较固定的，因为两者都是以时间为基准进行线性增长 */
    c->serial       = serial;
}

/**
 * @brief 设置时钟，具体看set_clock_at。
 * @param c 时钟，例如初始化时是播放器中的时钟。
 * @param pts 当前帧的pts，传入时是gcc的一种内建函数，宏为NAN。
 * @param serial 序列号，初始化时是-1.
 *
 * 例如调用：set_clock(c, NAN, -1);
 *
 * @return void。
 */
static void set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;// 获取当前时间，单位秒。
    set_clock_at(c, pts, serial, time);
}

/**
 * @brief 更新随着系统流逝的pts到时钟、更新时钟的serial以及设置时钟的速率。
 * @param c 时钟。
 * @param speed 要设置的速率。
 * @return void。
 */
static void set_clock_speed(Clock *c, double speed)
{
    set_clock(c, get_clock(c), c->serial);// 这里会更新时钟的pts以及serial，pts是随着系统流逝的pts。
    c->speed = speed;
}

/**
 * @brief 初始化播放器中的时钟。
 * @param c 播放器中的时钟变量，可能是视频时钟、音频时钟、外部时钟。
 * @param queue_serial 序列号，可能是播放器中的视频包队列的序列号、音频包队列的序列号、外部时钟的序列号。确实是没有字幕的序列号的传入，因为主时钟的设置不包含字幕。
 * @return void。
 */
static void init_clock(Clock *c, int *queue_serial)
{
    c->speed = 1.0;                     // 播放速率
    c->paused = 0;                      // 0是未暂停，1是暂停。
    c->queue_serial = queue_serial;     // 时钟指向播放器中的包队列的serial。

    // 浮点数的运算：当遇到正无穷负无穷时，会以inf、-inf表示。当根本不存在或者无法表示的值时，会以NAN表示。
    // see https://blog.csdn.net/yuanlulu/article/details/6236330。
    double tyyinf = 1/0.0;              // 正无穷
    double tyyfuinf = log (0);          // 负无穷
    double tyyNAN1 = sqrt (-1);         // -NAN，不过对于NAN正负应该是没有意义的。对-1开方是不存在的数学表达式
    double tyyNAN2 = NAN / 1;           // NAN，凡是与NAN计算的，都是NAN。

    set_clock(c, NAN, -1);              // 注意上面是包队列的serial(初始化时值是0)，时钟内部还有一个自己的serial(初始化时值是-1)。
}

static void sync_clock_to_slave(Clock *c, Clock *slave)
{
    // 1. 获取主时钟随着实时时间流逝的pts
    double clock = get_clock(c);
    // 2. 获取从时钟随着实时时间流逝的pts
    double slave_clock = get_clock(slave);

    // 3. 通过set_clock使用从时钟的pts设置主时钟的pts
    // 1）从时钟有效并且主时钟无效；这个条件应该是主时钟未设置需要进行设置。
    // 2）或者 从时钟随着实时时间流逝的pts 与 主时钟随着实时时间流逝的pts 相差太大，需要根据从时钟对主时钟进行重设。
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

/**
 * @brief 获取音视频的主时钟同步类型，并且主时钟同步类型虽然被设置，但是若输入流不含该流，这个函数会进行重设。
 * @param 播放器结构体。
 * @return 返回对应设置的主时钟同步类型。
 * 注意：1）只有视频时，同步类型应该设置AV_SYNC_VIDEO_MASTER；若设置音频会被重设为外部时钟，因为没有音频，所以这可能效果不太好。
 *      2）音视频都有或者只有音频时，同步类型应该设置AV_SYNC_AUDIO_MASTER。
 *      3）开发中尽量这样设置，避免给自己挖坑。
*/
static int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;	 /* 如果没有视频成分则使用 audio master */
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;	 /* 没有音频的时候那就用外部时钟 */
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is)
{
    double val;

    switch (get_master_sync_type(is)) {
    case AV_SYNC_VIDEO_MASTER:
        val = get_clock(&is->vidclk);
        break;
    case AV_SYNC_AUDIO_MASTER:
        val = get_clock(&is->audclk);
        break;
    default:
        val = get_clock(&is->extclk);// get_clock是获取随着实时时间流逝的pts，很简单。
        break;
    }
    return val;
}

/**
 * @brief 根据音视频包队列的包数量，进行调整外部时钟的速率。
 * @param is 播放器实例。
 * @return void。
*/
static void check_external_clock_speed(VideoState *is) {
    if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||  // 有视频流且视频包队列小于等于2个
            is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {// 或者有音频流且音频包队列小于等于2个。表示包队列包快不够了，减少时钟速率让你播慢点
                                                                                            // 即：只有有一个包队列快不足了，都要降慢时钟速率。
        // 视频、音频、外部时钟的初始化时的值：pts=NAN、pts_drift=NAN、serial=-1、last_updated=time(系统时间)、paused=0、speed = 1.0
        // 调用set_clock_speed只会时钟的更新3个值：时钟的pts、serial、speed。这里的FFMAX作用是确保速率在MIN值以上。而且由于speed=1.0，所以基本在0.900-1.0徘徊
        set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));

    } else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) && // (不存在视频流或者视频包队列大于10个)
               (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) { // 并且 (不存在音频流或者音频包队列大于10个) ：表示队列有很多包，提高时钟速率让你播快点
                                                                                               // 即：只有同时有足够多的包才加快时钟速率。
        // FFMIN的作用是确保速率在MAX以下。而且由于speed=1.0，所以基本在1.0-1.010徘徊
        set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));

    } else {// 包队列剩余3-10帧的路径，按照时钟的正常速率1.0去播放即可。
        double speed = is->extclk.speed;
        if (speed != 1.0)
            set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));

        // 假设speed=1.005，speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed) = 1.005 + 0.001*(-0.005)/0.005=1.005-0.001=1.004
        // 假设speed=0.910，0.910 + 0.001 * (0.090)/0.090=0.911
        // 上面两假设得出：
        //      1）(1.0 - speed) / fabs(1.0 - speed)表示取正负号，speed大于1.0表示负号，小于表示正号。
        //      2）且speed大于1.0时，调整速率，自动减去0.001；小于1.0时，加上0.001
    }
}

/* seek in the stream */
/**
 * @brief seek stream，记录有seek请求，在读线程轮询到时会进行处理。
 * @param is 播放器实例。
 * @param pos  具体seek到的位置。
 * @param rel  增量。
 * @param seek_by_bytes 是否按照字节方式seek。
 */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes)
{
    // 只有seek_req为0才进来，所以即使用户多次触发请求事件，也是按照一次处理。
    if (!is->seek_req) {
        is->seek_pos = pos; // 按时间单位是微秒，按字节单位是byte
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;        // 不按字节的方式去seek
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;     // 强制按字节的方式去seek
        is->seek_req = 1;                           // 请求seek， 在read_thread线程seek成功才将其置为0
        SDL_CondSignal(is->continue_read_thread);
    }
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState *is)
{
    // 如果当前是暂停 -> 恢复播放
    // 正常播放 -> 暂停
    if (is->paused) {// 当前是暂停，那这个时候进来这个函数就是要恢复播放
        /* 恢复暂停状态时也需要恢复时钟，需要更新vidclk */
        // 加上 暂停->恢复 经过的时间

        // 本if的内容可以全部注掉，与不注掉的区别是：注掉会drop一帧，并且会造成视频求delay时的diff值变大(会自动通过同步处理，所以不用担心)，不注掉则不会。
        // 但是注意必须全部注掉，若只注掉frame_timer的内容而不set_clock()，会导致暂停重新播放后画面卡主，原因是程序以为视频超前了，需要等待。
        // 反正大家可以根据自己的感受去做优化即可。
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }

        // 设置时钟的意义，暂停状态下读取的是单纯pts
        // 重新矫正video时钟
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }

    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    // 更新播放器的状态 + 3个时钟的状态：切换 pause/resume 两种状态
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
    printf("is->step = %d; stream_toggle_pause\n", is->step);
}

/**
 * @brief 触发暂停函数， detail see stream_toggle_pause()。
 * @param is 播放器实例。
 * @return void。
 */
static void toggle_pause(VideoState *is)
{
    stream_toggle_pause(is);
    is->step = 0;   // 逐帧的时候用
    printf("is->step = 0; toggle_pause\n");
}

/**
 * @brief 触发静音的函数。
 * @param is 播放器实例。
 * @return void。
 */
static void toggle_mute(VideoState *is)
{
    is->muted = !is->muted;
}

/**
 * @brief 音量改变的函数。
 * @param is 播放器实例。
 * @param sign 正数，表示增大音量，负数表示减少音量。
 * @param step 音量变化的步调。ffplay设为0.75。
 * @return void。
 */
static void update_volume(VideoState *is, int sign, double step)
{
    /*
     * ffplay音量的大小是根据对数函数和指数函数的性质去处理的，音量等级由对数去求，变化后的音量由指数函数求。
     * 1. 涉及以下cmath的数学函数：
     *  1)log(x)：以e为底的对数。
     *  2)lrint(double)：将传入的浮点数根据四舍五入的方法取整，返回长整型变量。
     *  3)pow(double a,double b)：以a为常数，b为自变量的指数函数，即a的b次方。
     * 下面我们进行详细推导：
     * 2. 首先是音量等级：
     * 2.1)is->audio_volume∈[0,128]，故is->audio_volume / (double)SDL_MIX_MAXVOLUME∈[0,1];
     * 2.2)故log(is->audio_volume / (double)SDL_MIX_MAXVOLUME)=loge[0,1]; 设其为y。
     * 2.3)那么有对数公式：y=loge[0,1];  根据对数底数大于1的图形，故y∈(-∞, 0];
     * 2.4）然后将2.3步骤乘以20，再除以log(10)，log(10)是个常数，约为log(10)=loge10 => e^x=10, x=2.302。
     * 换句话说，最终求出来的音量等级必定是个y∈(-∞, 0];的负数。
     * 音量等级总公式为：lev = (20loge(v/max)) / loge10;          lev代表音量等级；v代表音量；max表示音量最大值。
     *
     * 3. 然后是根据等级求变化后的音量：
     *  3.1)由pow得出指数函数的表达式：y=10.0^((volume_level + sign * step) / 20.0);
     *  3.2)根据指数函数底数大于1的图形有：
     *      3.2.1)当(volume_level + sign * step) / 20.0 >= 0时，y∈[1,+∞);
     *      3.2.2)当(volume_level + sign * step) / 20.0 < 0时， y∈(0,1);  注：指数函数的值域必定大于0。
     *  3.3)根据3.2)的两个情况，volume_level、sign、step到底取什么值才满足[1,+∞]或者(0,1)的范围呢？
     *      所以我们需要再具体分析(volume_level + sign * step)表达式：
     *          volume_level在第2点看到，范围必在(-∞, 0]的区间，而step是一个音量变化的宏，ffpaly设为0.75.
     *          所以sign就是一个代表正负数的变量。故有：
     *              3.3.1)当sign为正数且volume_level∈[-0.75,0]时 ，                      y∈[1,+∞);
     *              3.3.2)当sign为正数且volume_level∈(-∞,-0.75)时，                      y∈(0,1);
     *              3.3.3)当sign为负数(volume_level不需要关心，因为已经是负数，即(-∞, 0])时，  y∈(0,1);
     *
     * 所以知道了volume_level、sign、step的意义后，我们回到pow得出的公式：
     * 3.4)：y=10.0^((volume_level + sign * step) / 20.0);   y∈(0,1)或者y∈[1,+∞);即y∈(0,+∞);
     *      即pow(10.0, (volume_level + sign * step) / 20.0)的范围是y∈(0,+∞)。
     *
     * 3.5)最后SDL_MIX_MAXVOLUME乘以这个范围值，使用lrint进行取整后得到新的音量值。但是因为可能会溢出，所以使用av_clip确保在[0, SDL_MIX_MAXVOLUME]。
     *
     *
     * 4. 此时再回过头来想想，ffplay是如何靠sign的正负值就能做到音量的增加和减少呢？
     *  恭喜你，看完上面2、3其实根本很难看出，哈哈哈，因为上面是我一开始的推导，但是对你的理解也有一定效果，因为你知道我是如何推导出来的。
     *  正确的推导：
     *
     * y = 10.0^((volume_level + sign * step) / 20.0);
     * 其中：volume_level = 20*loge(v/m) / loge(10);
     * 根据换底公式：logαx = logβx/logβα有：
     * volume_level = 20*log10(v/m);
     * 代入得：
     * y = 10.0^((20*log10(v/m) + sign * step) / 20.0) = 10.0^( log10(v/m) + ((sign*step)/20.0) )。
     *
     * 所以最终的化简公式为：y = 10.0^( log10(v/m) + ((sign*step)/20.0) )。
     * 又因为：
     * y = 10.0^( log10(v/m) )时，y=v/m。
     * 第4步最好自己手动写出来感受，否则这样看很难看懂。
     *
     * 因为指数函数底数大于1时，是单调递增的，所以可以通过改变指数来进行增加或者减少音量。即(sign*step)/20.0)。
     * 这也就是，ffplay可以通过sign的正负值就能做到音量的增加和减少的原因。
     *
    */

    double volume_level = is->audio_volume ? (20 * log(is->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10)) : -1000.0;
    int new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
    is->audio_volume = av_clip(is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume, 0, SDL_MIX_MAXVOLUME);
    //printf("update_volume audio_volume:%d\n", is->audio_volume);
}

static void step_to_next_frame(VideoState *is)
{
    /* if the stream is paused unpause it, then step(如果流被暂停，则取消暂停，然后执行step) */
    if (is->paused)
        stream_toggle_pause(is);
    is->step = 1;
    printf("is->step = 1; step_to_next_frame\n");
}

/**
 * @brief 在last_duration的基础上，动态的计算真正 正在显示帧 还要显示的时长，以达到音视频同步的目的。
 * @param delay 该参数实际传递的是当前显示帧和待播放帧的间隔。
 * @param is 播放器实例。
 * @return 经音视频同步后，返回当前显示帧要持续播放的时间。
 *
 * 总结一些关键性需求问题：
 * 1）为何要同步？
 * 答：若不同步，视频和音频各自播放自己的内容，视频看起来是很乱的，就像平时视频与字幕不匹配一样，用户体验非常不好。
 * 2）音视频同步的思路是什么？
 * 答：以 只有音频和视频 并且 主时钟是音频 的情况下举例。  以音频为主时钟，那么无非就是存在两种情况，视频超前播放或者落后播放，那么就需要进行调整。
 *      当视频落后时，那么就进行drop帧处理，赶紧追上音频的播放；当视频超前时，那么就需要睡眠等待，直观效果就是增加了这一帧的显示时长，即重复播放。
 * 理解了这个问题后，下面的代码就简单很多了。
 *
 */
static double compute_target_delay(double delay, VideoState *is)
{
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */

    /* 1. 只有当前主Clock源不是video才往下走，否则直接返回delay */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame。
           通过重复帧或者删除帧来纠正延迟。
        */

        // 2. 求出从时钟与主时钟的pts差值。diff作用是：判断视频超前或者落后的大小，正数超前，负数落后。
        diff = get_clock(&is->vidclk) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess
        */
        // 3. 根据delay求出对应的音视频同步精度阈值。
        // sync_threshold作用是：根据视频落后或者超前的差距大小(即diff值)，调整对应的音视频同步幅度。
        // 如果落后或者超前差距大于这个大小，则进行同步调整，否则差距在-sync_threshold ~ +sync_threshold以内，不做处理。
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));// 确保在[40,100]ms的范围。

        // 4. diff在max_frame_duration内则往下走，否则diff超过max_frame_duration以上的pts，直接返回delay。
        // 依赖新的一帧的pts去调用update_video_pts去更新pts，以让diff恢复正常。如果diff一直处于>max_frame_duration的状态，
        // 那么是无法同步的，那就不管了，不过这种情况不怎么存在，一般是由于外部操作导致的。
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            /*
             * 5. 视频落后音频了。并且落后的diff值 <= 负的音视频同步的阈值。
             * -sync_threshold在[-100,-40]ms的范围，所以只有diff在[负无穷,-sync_threshold]的范围才会满足条件(可以画个图或者看音频同步pdf的图)。
             * 5.1 例如假设delay=0.05s，-sync_threshold=-0.05s，再假设diff=0.2s-1s=-0.8s，得出delay=0.05s+(-0.8s)=-0.75s，
             * 经过FFMAX处理，最终返回0.
             * 5.2 而若diff在 > -sync_threshold的落后情况，那么也是直接返回delay。例如这个例子的diff=0.96s-1s=-0.04s到0之前的值。
             * 注意diff=0以及正数是不行的，正数代表超前而不是落后了，0不满足fabs(diff)条件。
            */
            if (diff <= -sync_threshold) {
                delay = FFMAX(0, delay + diff); // 上一帧持续的时间往小的方向去调整
            }
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
                /* 6. 视频超前了 */
                // 这里的意思是当diff值太大时，需要使用diff去计算delay。
                // 例如，当传入的帧间隔delay过大，那么计算出来的sync_threshold也会很大，所以只有diff比这个阈值sync_threshold更大才能进来这个流程。
                // 以具体值为例，假设delay传进来=0.2s(200ms)，那么sync_threshold=0.1s(100ms)，所以只有diff大于100ms才能进来。
                // 假设diff=1s(1000ms)，那么计算后得到最终的delay=0.2s+1s=1.2s。最终音视频同步后，这一帧还需要显示1.2s才能显示下一帧。
                // 其中睡的0.2s是代表这一帧到下一帧还需要等待的时长，1s是要等待音频的时长，也就是音视频需要同步的时长。
                // 视频超前
                // ffpaly直接使用浮点数比较不太好，后续可以进行优化

                delay = delay + diff; // 上一帧持续时间往大的方向去调整
                av_log(NULL, AV_LOG_INFO, "video: delay=%0.3f A-V=%f\n", delay, -diff);//A-V：代表音频减去视频的差距
            }
            else if (diff >= sync_threshold) {
                /* 7. 同样是视频超前了，只不过处理的方式不一样，上面的超前值比较大 */
                // 上一帧持续时间往大的方向去调整
                // 例如delay=0.05s，diff=(160-100)ms=0.06s，sync_threshold=0.05s，那么不满足delay > AV_SYNC_FRAMEDUP_THRESHOLD的条件，
                // 按照delay = 2 * delay计算，delay=0.1s。实际上和delay = delay + diff=0.11s差了10ms，但是由于delay每次都是动态计算的，这种误差可以认为是可忽略的，
                // 不过有些人不喜欢这种误差情况，就把ffplay的AV_SYNC_FRAMEDUP_THRESHOLD=0.1的值改成0.04，尽量让它走上面的流程。
                // 总之，大家可以按照自己的实际情况和想法去测试，去达到最好的要求即可，尚且ffplay的作者也不知道这种同步处理是否是最好的。
                delay = 2 * delay;
            } else {
                // 8. 音视频同步精度在 -sync_threshold ~ +sync_threshold，不做处理。
                // 其他条件就是 delay = delay; 维持原来的delay, 依靠frame_timer+duration和当前时间进行对比

                // 例如get_clock(&is->vidclk) - get_master_clock(is)获取到的值是120ms与100ms，那么diff=20ms。delay=40ms传入，那么sync_threshold=40.
                // is->max_frame_duration一般是3600，那么由于20在阈值40内，所以会来到这个else流程。
            }
        }else if(fabs(diff) < is->max_frame_duration){
            // tyycode
            printf("fabs(diff) > max_frame_duration\n");
        }
    } else {
        // 9. 如果是以video为同步，则直接返回last_duration
    }

    av_log(NULL, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);

    return delay;
}

// 计算上一帧需要持续的duration，这里有校正算法
/**
 * @brief 计算一帧需要显示的duration。校正算法：下一帧减去当前帧的pts，：
 *          1）若异常则返回1/帧率的时长。
 *          2）正常则返回两帧的时间间隔。
 * @param is 播放器实例。
 * @param vp 当前帧(老师将其说成是上一帧，不过意思应该是一样的)。
 * @param nextvp 下一帧。
*/
static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp)
{
    if (vp->serial == nextvp->serial) { // 同一播放序列，序列连续的情况下
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration)         // duration 数值异常
                || duration <= 0    // pts值没有递增时
                || duration > is->max_frame_duration    // 超过了最大帧范围
                ) {
            return vp->duration;	 /* 异常时以帧时间为基准(1秒/帧率) */
        }
        else {
            return duration; //使用两帧pts差值计算duration，一般情况下也是走的这个分支
        }
    } else {        // 不同播放序列, 序列不连续则返回0
        return 0.0;
    }
}

/**
 * @brief 设置视频的pts。
 */
static void update_video_pts(VideoState *is, double pts, int64_t pos, int serial) {
    /* update current video pts */

    // 以sync=video为例：
    // 1）假设第一次，pts传进来为2，set_clock时time假设是102；那么pts_drift=2-102=-100;这个是update_video_pts函数调用set_clock设置视频时钟的步骤。
    // 2）然后调用sync_clock_to_slave。其步骤是：
    //      2.1 内部首先获取随时间流逝的pts，由于首次extclk是nan，所以内部获取的clock是nan；
    //      2.2 然后因为刚刚设了视频的时钟，并且1和2）的调用时间很短可以忽略不计，那么slave_clock为-100+102=2；
    //      2.3 然后就是调用set_clock()用从时钟设置主时钟，那么此时主时钟就和从时钟可以认为是一样的。
    //      即视频同步时，外部时钟的结构体被用来作为主时钟，但不是真正的主时钟，只是使用它的结构体记录值而已。
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);// 参1为外部时钟是因为，当设置了外部时钟为主时钟时，需要参考视频与音频，所以视频的时钟也要更新到外部时钟。
}

/* called to display each frame */
/* 非暂停或强制刷新的时候，循环调用video_refresh */
static void video_refresh(void *opaque, double *remaining_time)
{
    VideoState *is = opaque;
    double time;

    Frame *sp, *sp2;

    // 1. 没有暂停，音视频同步是外部时钟(只有视频时ffplay默认使用外部时钟)，并且是实时流时：
    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
        check_external_clock_speed(is);

    // 2. 没有禁用视频，显示模式不是视频，并且有音频流。这里估计是音频封面，留个疑问
    // debug 带有音、视频的实时流或者文件：show_mode=SHOW_MODE_VIDEO (0x0000)，都不会进来
    if (!display_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + rdftspeed < time) {
            video_display(is);
            is->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + rdftspeed - time);
    }

    // 3. 如果有视频流
    if (is->video_st) {
retry:
        // 3.1 帧队列是否为空
        if (frame_queue_nb_remaining(&is->pictq) == 0) {
            // nothing to do, no picture to display in the queue
            // 什么都不做，队列中没有图像可显示
        } else { // 重点是音视频同步
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            // 从队列取出上一个Frame
            lastvp = frame_queue_peek_last(&is->pictq); // 读取上一帧
            vp = frame_queue_peek(&is->pictq);          // 读取待显示帧
            // lastvp 上一帧(正在显示的帧)
            // vp 等待显示的帧

            // 3.2 vp->serial是否是当前系序列下的包。帧队列的serial由解码器的serial赋值，具体看queue_picture()。
            // 凡是serial是否最新，都应该与包队列的serial比较，因为seek后首先影响的是包队列。
            // 这里表明，一旦serial与最新的serial不相等，不仅包队列里面的旧的serial包要舍弃不能送进解码，已经解码后的旧的serial同样要舍弃，不能在帧队列保存了。
            if (vp->serial != is->videoq.serial) {
                // 如果不是最新的播放序列，则将其出队列，以尽快读取最新序列的帧
                frame_queue_next(&is->pictq);   // here
                goto retry;
            }

            // 更新上一帧的播放时间，因为上面serial不一样时，帧出队列，goto retry后lastvp会被更新
            if (lastvp->serial != vp->serial) {
                // 新的播放序列重置当前时间
                is->frame_timer = av_gettime_relative() / 1000000.0;
            }

            // 暂停状态会一直显示正在显示的帧，不做同步处理。
            if (is->paused)
            {
                goto display;
                printf("视频暂停is->paused");
            }

            /* compute nominal last_duration */
            //lastvp上一帧，vp当前帧 ，nextvp下一帧
            //last_duration：静态计算上一帧应显示的时长
            last_duration = vp_duration(is, lastvp, vp);

            // 经过compute__delay方法，动态计算出真正待显示帧vp需要等待的时间
            // 如果以video同步，则delay直接等于last_duration。
            // 如果以audio或外部时钟同步，则需要比对主时钟调整待显示帧vp要等待的时间。
            delay = compute_target_delay(last_duration, is); // 上一帧需要维持的时间
            //printf("last_duration: %lf, delay: %lf\n", last_duration, delay);


            time= av_gettime_relative()/1000000.0;
            // is->frame_timer 实际上就是上一帧lastvp的播放时间,
            // is->frame_timer + delay 是待显示帧vp该播放的时间
            if (time < is->frame_timer + delay) { //判断是否继续显示上一帧
                // 当前系统时刻还未到达上一帧的结束时刻，那么还应该继续显示上一帧。
                // 计算出最小等待时间
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);// remaining_time=REFRESH_RATE默认睡0.01s
                //printf("tyycode frame_timer:%lf, delay:%lf, sleep remaining_time(s): %lf\n", is->frame_timer, delay, *remaining_time);
                goto display;
            }

            /*
             到这一步，说明已经到了或过了该显示的时间，可以显示该帧。
             注意：
             1）即使time是已经过了该显示的时间，但不会影响该帧的显示，只不过会影响该帧的显示时长，上一帧显示久个几毫秒，该帧可能会显示短了几毫秒，
             它是不会影响到下一次的视频同步的。
                例如首次进来：pts=20ms，_last_pts=0；求出的last_duration=delay=40.假设time=10 0000ms，那么不会进入睡眠处理。
                _frame_timer=0+40=40,但是会被更新为系统时间，_frame_timer=10 0000ms._last_pts=20ms。

                第二次：假设pts=40ms，求出的last_duration=delay=20.假设time=10 0010ms，那么会睡10ms。
                _frame_timer=10 0000+20=10 0020，不会更新_frame_timer，_last_pts=40ms。

                第三次：假设pts=80ms，求出的last_duration=delay=40.  此时假设time已经超过播放时间，例如time=10 0061ms，那么不会进入睡眠处理，
                _frame_timer=10 0020+40=10 0060，不会更新_frame_timer，_last_pts=80ms。

                第四次：假设pts=120ms，求出的last_duration=delay=40.假设time=10 0080ms，那么会睡20ms。
                _frame_timer=10 0060+40=10 0100，不会更新_frame_timer，_last_pts=120ms。

                总结：
                    1）到第3次就可以看到，虽然time已经超过了时间，但是并未影响了第四次的显示，只不过会导致第二次多显示了1ms左右，
                    但是第三次也不一定显示短了几毫秒，因为第四次也有可能慢显示。
                    2）从第二次和第四次看到，time - _frame_timer是一个负值，需要在判断time - _frame_timer之前，更新一下time的值，确保是正数，
                    因为此时的时间肯定大于该帧要显示的时间了。
                    可以参考ffplay在睡眠之后使用goto，或者直接在_frame_timer += delay;后面更新(检查了一下应该是没问题的，暂时还没试过)。
                    3）为何要纠正为系统时间呢，首先作用就是第一次进来需要纠正，其他作用就是：后面可能由于操作导致没继续进来更新time，
                    time - _frame_timer，只有time是变量，_frame_timer是固定的，所以只可能time变太大才会纠正。例如ffplay使用goto睡眠后会返回到事件
                    处理，如果处理事件的时间过长，再进来本函数就会导致time过大，需要纠正。如果直接更新time应该不会存在这种问题。
                    归根结底，纠正还是必须的。
            */

            is->frame_timer += delay;   // 更新上一帧为当前帧的播放时间
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX) {// 0.1s
                is->frame_timer = time; //如果上一帧播放时间和系统时间差距太大，就纠正为系统时间
            }

            SDL_LockMutex(is->pictq.mutex);
            if (!isnan(vp->pts))
                update_video_pts(is, vp->pts, vp->pos, vp->serial); // 更新video时钟，以及更新从时钟(vidclk)到主时钟(extclk)
            SDL_UnlockMutex(is->pictq.mutex);

            /*
             * 因为上面compute_target_delay的sync_threshold同步阈值最差可能是[-0.1s,0.1s]，是可能存在一帧到两帧的落后情况。
             * 例如delay传入是0.1s，那么sync_threshold=0.1s，假设音视频的pts差距diff是在0.1s以内(当然这里只是假设不一定成立)，
             * 那么delay还是直接返回传入的delay=0.1s，不睡眠，is->frame_timer += delay直接更新并且不会纠正，
             * 那么若满足下面的if条件，主要条件是：若实时时间time > 本帧的pts + duration，本帧的pts就是frame_time，因为上面刚好加上了delay。
             * 即实时时间大于下一帧的pts，证明确实落后了一帧，需要drop帧，以追上播放速度。
            */
            // 丢帧逻辑(主时钟为视频不会进来)
            if (frame_queue_nb_remaining(&is->pictq) > 1) {//有nextvp才会检测是否该丢帧
                Frame *nextvp = frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp);
                // 非逐帧 并且 (强制刷新帧 或者 不强制刷新帧的情况下视频不是主时钟) 并且 视频确实落后一帧，
                // 那么进行drop帧
                if(!is->step        // 非逐帧模式才检测是否需要丢帧 is->step==1 为逐帧播放
                        && (framedrop>0 ||      // cpu解帧过慢
                            (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) // 非视频同步方式
                        && time > is->frame_timer + duration // 确实落后了一帧数据
                        ) {
                    printf("%s(%d) dif:%lfs, drop frame\n", __FUNCTION__, __LINE__, (is->frame_timer + duration) - time);
                    is->frame_drops_late++;             // 统计丢帧情况
                    frame_queue_next(&is->pictq);       // vp出队，这里实现真正的丢帧

                    //(这里不能直接while丢帧，因为很可能audio clock重新对时了，这样delay值需要重新计算)
                    goto retry; // 回到函数开始位置，继续重试
                }
            }

            // 来到这里，说明视频帧都是能播放的。但是需要判断字幕帧是否能播放，不能则会进行丢字幕帧或者清空正在播放的字幕帧。
            // 判断流程：如果发生seek或者落后视频1-2帧：若字幕帧显示过或者正在显示，立马进行清空；若字幕帧没显示过，则立马出队列drop帧。
            // 这里的代码让我觉得还是需要写一些demo去理解SDL_LockTexture、pixels、pitch的意义。例如如何处理字幕的格式，提取yuv各个分量的首地址等等。
            if (is->subtitle_st) {

                while (frame_queue_nb_remaining(&is->subpq) > 0) {// 字幕的keep_last、rindex_shown始终都是0，音视频则会变成1.
                    // 获取当前帧。
                    sp = frame_queue_peek(&is->subpq);

                    // 若队列存在2帧以上，那么获取下一帧。因为frame_queue_peek获取后rindex并未自增，所以sp、sp2是相邻的。
                    if (frame_queue_nb_remaining(&is->subpq) > 1)
                        sp2 = frame_queue_peek_next(&is->subpq);
                    else
                        sp2 = NULL;

                    if (sp->serial != is->subtitleq.serial  // 发生seek了
                            // 因为上面update_video_pts更新了vp即待显示帧的pts，所以is->vidclk.pts 就是表示待显示帧的pts。
                            // sp->sub.end_display_time表示字幕帧的显示时长。故sp->pts + ((float) sp->sub.end_display_time / 1000)表示当前字幕帧的结束时间戳。
                            || (is->vidclk.pts > (sp->pts + ((float) sp->sub.end_display_time / 1000)))// 表示：字幕帧落后视频帧一帧。
                            || (sp2 && is->vidclk.pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000))))// 同理表示：字幕帧落后视频帧两帧。
                    {
                        /*
                         * 字幕相关结构体解释：
                         * AVSubtitle：保存着字幕流相关的信息，可以认为是一个管理结构体。
                         * AVSubtitleRect：真正存储字幕的信息，每个AVSubtitleRect存储单独的字幕信息。从这个结构体看到，分别有data+linesize(pict将被舍弃)、text、ass指针，
                         *                  这是为了支持FFmpeg的3种字幕类型，具体谁有效，看type。例如type是text，那么text指针就保存着字幕的内容，其它两个指针没实际意义。
                         * AVSubtitleType：FFmpeg支持的字幕类型，3种，分别是：bitmap位图、text文本、ass。
                        */
                        if (sp->uploaded) {
                            int i;
                            /*
                             * 下面两个for循环的操作大概意思是(个人理解，具体需要后续写demo去验证)：
                             * 1）例如一行字幕有4个字。那么sp->sub.num_rects=4.
                             * 2）而每个字又单独是一幅图像，所以同样需要遍历处理。
                             * 3）一幅图像的清空：按照高度进行遍历，宽度作为清空的长度即可(为啥左移2目前不是很懂)。这一步猜出pitch是这个图像的offset偏移位置。
                             *                  每次处理完一次纹理pixels，都需要相加进行地址偏移，而不能使用宽度w进行地址偏移。宽度是针对于图像大小，pitch才是地址的真正偏移。
                             *                  换句话说，操作图像用宽度，操作地址偏移用pitch。
                             * 4）处理完这个字后，回到外层循环继续处理下一个字，以此类推...。
                            */

                            for (i = 0; i < sp->sub.num_rects; i++) {   // 遍历字幕信息AVSubtitleRect的个数。
                                AVSubtitleRect *sub_rect = sp->sub.rects[i];
                                uint8_t *pixels;                        // pixels、pitch的作用实际是提供给纹理用的，纹理又通过这个指针给调用者进行操作纹理中的数据。
                                int pitch, j;

                                // AVSubtitleRect可以转成SDL_Rect是因为结构体前4个成员的位置、数据类型一样。
                                if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *)sub_rect, (void **)&pixels, &pitch)) {
                                    for (j = 0; j < sub_rect->h; j++, pixels += pitch){
                                        memset(pixels, 0, sub_rect->w << 2);// 将纹理置空，相当于把画面的字幕清空(纹理相当于是一张纸)，宽度为啥要左移2(相当于乘以4)？留个疑问
                                        // 答：我们创建纹理时传进的参数分辨率的单位是像素(理解这一点非常重要)，一行像素占多少字节由格式图片格式决定。
                                        // 因为字幕的纹理创建时，格式为SDL_PIXELFORMAT_ARGB8888，所以一个像素占4个字节(一个像素包含ARGB)，那么一行像素占w*4，故左移2。
                                    }
                                    SDL_UnlockTexture(is->sub_texture);
                                }
                            }
                        }

                        frame_queue_next(&is->subpq);
                    } else {
                        break;
                    }

                }// <== while end ==>
            }// <== if (is->subtitle_st) end ==>

            frame_queue_next(&is->pictq);   // 当前vp帧出队列，vp变成last_vp，这样video_display就能播放待显示帧。
            is->force_refresh = 1;          /* 说明需要刷新视频帧 */

            if (is->step && !is->paused)
                stream_toggle_pause(is);    // 逐帧的时候那继续进入暂停状态。音频在逐帧模式下不需要处理，因为它会自动按照暂停模式下的流程处理。所以暂停、播放、逐帧都比较简单。

        }// <== else end ==>

display:
        /* display picture */
        if (!display_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)// is->pictq.rindex_shown会在上面的frame_queue_next被置为1.
            video_display(is); // 重点是显示。see here

    }//<== if (is->video_st) end ==>


    is->force_refresh = 0;
    show_status = 0;
    if (show_status) {
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
            aqsize = 0;
            vqsize = 0;
            sqsize = 0;
            if (is->audio_st)
                aqsize = is->audioq.size;
            if (is->video_st)
                vqsize = is->videoq.size;
            if (is->subtitle_st)
                sqsize = is->subtitleq.size;
            av_diff = 0;
            if (is->audio_st && is->video_st)
                av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
            else if (is->video_st)
                av_diff = get_master_clock(is) - get_clock(&is->vidclk);
            else if (is->audio_st)
                av_diff = get_master_clock(is) - get_clock(&is->audclk);
            av_log(NULL, AV_LOG_INFO,
                   "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"PRId64"/%"PRId64"   \r",
                   get_master_clock(is),
                   (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
                   av_diff,
                   is->frame_drops_early + is->frame_drops_late,
                   aqsize / 1024,
                   vqsize / 1024,
                   sqsize,
                   is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts : 0,
                   is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts : 0);
            fflush(stdout);
            last_time = cur_time;
        }
    }
}

/**
 * @brief 将AVFrame重新封装成Frame结构，然后push进帧队列。
 * @param is 播放器实例。
 * @param src_frame 解码后得到的AVFrame。
 * @param pts 利用frame->pts计算得到的pts，实际只是转成了double类型。
 * @param duration 利用帧率求出的时长，传入需要依赖滤波器计算出帧率，计算方式：1/帧率 = duration，单位秒。
 * @param pos 该帧在输入文件中的字节位置。
 * @param serial 解码器中的pkt_serial。
 *
 * @return 0 成功； -1 用户请求中断。
 *
 * queue_picture(is, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
 */
static int queue_picture(VideoState *is, AVFrame *src_frame, double pts,
                         double duration, int64_t pos, int serial)
{
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    // 1. 获取队列中可写的帧
    if (!(vp = frame_queue_peek_writable(&is->pictq))) // 检测队列是否有可写空间
        return -1;      // 请求退出则返回-1

    // 执行到这步说明已经获取到了可写入的Frame

    // 2. 使用传进来的参数对Frame的内部成员进行赋值。
    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;                           // 0 表示该帧还没显示

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;                              // 该帧在输入文件中的字节位置
    vp->serial = serial;                        // 帧队列里面的serial是由解码器里面的pkt_serial赋值，而pkt_serial由包队列的MyAVPacketList节点赋值。

    set_default_window_size(vp->width, vp->height, vp->sar);    // 更新解码后的实际要显示图片的宽高
    av_frame_move_ref(vp->frame, src_frame);    // 这里才是真正的push，将src中所有数据转移到dst中，并复位src。
                                                // 夺走所有权，但和C++有区别，这里可以认为frame会重新开辟一份内存并进行内容拷贝，原来的src_frame内容被重置但内存不会被释放。
                                                // 为啥这么想呢？因为我们看到video_thread调用完本函数后，明明已经push到队列了，但还是调用av_frame_unref(frame)释放src_frame。

    // 3. push到Frame帧队列，但frame_queue_push函数里面只是更新属性，
    // 真正的push应该是通过frame_queue_peek_writable获取地址，再av_frame_move_ref。
    frame_queue_push(&is->pictq);               // 更新写索引位置

    return 0;
}

/**
 * @brief 获取视频帧，如果frame->pts与主时钟的pts有误差，可能会被drop掉而不会再进去帧队列。
 * @param is 播放器实例。
 * @param frame 传入传出，指向要获取视频帧的内存。
 * @return -1 用户中断； 0 解码结束或者该帧被drop掉； 1该帧需要传出进行显示。
 */
static int get_video_frame(VideoState *is, AVFrame *frame)
{
    int got_picture;

    // 1. 获取解码后的视频帧。解码帧后。帧带有pts
    if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0) {
        return -1; // 返回-1意味着要退出解码线程, 所以要分析decoder_decode_frame什么情况下返回-1
    }

    // 2. 判断是否要drop掉该帧(视频同步不会进来)。该机制的目的是在放入帧队列前先drop掉过时的视频帧。
    //    注意返回值got_picture=0表示解码结束了，不会进来。
    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;    // 计算出秒为单位的pts，即pts*(num/den)

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);// 视频帧采样长宽比。这里留个疑问是啥意思。或者先不理。

        if (framedrop>0 || // 允许drop帧
                (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER))// 非视频同步模式。一般只有视频的话，默认外部时钟是主时钟，最好手动设为视频，这一点需要注意。
        {
            if (frame->pts != AV_NOPTS_VALUE) { // pts值有效
                double diff = dpts - get_master_clock(is);  // 计算出解码后一帧的pts与当前主时钟的差值。
                if (!isnan(diff) &&                         // 差值有效。isnan函数：返回1表示是NAN，返回0表示非NAN。
                        fabs(diff) < AV_NOSYNC_THRESHOLD && // 差值在可同步范围，可以drop掉进行校正，但是大于10s认为输入文件本身的时间戳有问题， 画面不能随便drop掉，比较单位是秒。
                        diff - is->frame_last_filter_delay < 0 &&       // 和过滤器有关系，不太懂，留个疑问
                        is->viddec.pkt_serial == is->vidclk.serial &&   // 同一序列的包，不太懂，留个疑问
                        is->videoq.nb_packets) { // packet队列至少有1帧数据，为啥要求至少队列有一个包才能drop，不太懂，留个疑问

                    is->frame_drops_early++;     // 记录已经丢帧的数量
                    printf("%s(%d) diff: %lfs, drop frame, drops frame_drops_early: %d\n", __FUNCTION__, __LINE__, diff, is->frame_drops_early);
                    av_frame_unref(frame);
                    got_picture = 0;

                }
            }
        }

    }// <== if(got_picture) end ==>

    return got_picture;
}

/**
 * @brief 判断是否需要添加复杂的过滤器，如果有会添加并且重排所有的过滤器。
 *          该函数会将输入输出的AVFilterContext进行Link,最后提交整个滤波图。
 *
 * @param graph 系统过滤器。
 * @param filtergraph 用户传进的复杂过滤器字符串。
 * @param source_ctx 输入源过滤器。
 * @param sink_ctx 输出源过滤器。
 *
 * @return >=0成功，负数失败。
 *
 * @note 该函数可以参考09-05-crop-flip复杂过滤器的实现，基本一样。
 */
#if CONFIG_AVFILTER
static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx)
{
    int ret, i;
    int nb_filters = graph->nb_filters;     // 不考虑filtergraph是否有值，先保存简单过滤器的个数
    AVFilterInOut *outputs = NULL, *inputs = NULL;

    // 1. 判断用户是否输入了复杂的过滤器字符串，如果有，则在简单过滤器结构的基础上，再添加复杂过滤的结构。
    if (filtergraph) {
        // 1.1 开辟输入输出的AVFilterInOut内存(09-05-crop-flip没开，开不开应该问题不大，不过还没测试过)
        outputs = avfilter_inout_alloc();
        inputs  = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name       = av_strdup("in");
        outputs->filter_ctx = source_ctx;       // 输入的AVFilterInOut->filter_ctx附属在输入源AVFilterContext上(看09-05的滤波过程图)
        outputs->pad_idx    = 0;
        outputs->next       = NULL;

        inputs->name        = av_strdup("out");
        inputs->filter_ctx  = sink_ctx;         // 输出的AVFilterInOut->filter_ctx附属在输出源AVFilterContext上
        inputs->pad_idx     = 0;
        inputs->next        = NULL;

        // 1.2 1）解析字符串；2）并且将滤波图的集合放在inputs、outputs中。 与avfilter_graph_parse2功能是一样的，参考(09-05-crop-flip)
        // 注意这里成功的话，根据复杂的字符串结构，输入输出的AVFilterContext会有Link操作，所以不需要再次调用avfilter_link
        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
            goto fail;
    }
    // 2. 否则将输入输出的AVFilterContext进行Link
    else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /*
     * Reorder the filters to ensure that inputs of the custom filters are merged first。
     * 3. 重新排序筛选器，以确保自定义筛选器的输入首先合并。
     * 因为我们这里是可能添加了一些复杂的过滤器，所以需要进行重拍。例如简单过滤器是2个，复杂过滤器1个。
     * 此时graph->nb_filters=3，原本的nb_filters=2，那么需要对3-2该过滤器进行重排。
     * 重排算法也很简单，就是将复杂过滤器往最前面放，例如上面例子，假设下标0、1是输入输出，2是复杂过滤器，那么
     * 换完0、1、2下标分别是：复杂过滤器、输出过滤器、输入过滤器。当然不一定准确，我们也不需要非得理解内部处理，
     * 只需要知道这样去做即可，当然你也可以深入，但感觉没必要浪费时间。
    */
    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    // 4. 提交整个滤波图
    ret = avfilter_graph_config(graph, NULL);

fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);

    return ret;
}

/**
 * @brief 配置视频相关的过滤器，若没有旋转角度，只会有输入输出过滤器。该函数使用了简单过滤器和复杂过滤器的混合，
 *          最后将输入输出filt_src、filt_out过滤器AVFilterContext保存在播放器实例中。
 * @param graph 系统过滤器。
 * @param is 播放器实例。
 * @param vfilters 用户传进的复杂过滤器字符串。
 * @param frame 解码后的一帧。该参数传入主要是使用它内部的一些字段进行初始化
 *                  输入AVFilterContext(没有其它作用了)。这是有必要的，参考09-02.
 *
 * @return 它是返回ffmpeg内部的ret，细看了，>=0成功，负数失败。
 *
 * @note 该函数可以参考09-02-video-watermark简单过滤器的实现，基本一样。
*/
static int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame)
{
    enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];// 求出数组元素个数
    char sws_flags_str[512] = "";
    char buffersrc_args[256];
    int ret;
    AVFilterContext *filt_src = NULL, *filt_out = NULL, *last_filter = NULL;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
    AVDictionaryEntry *e = NULL;
    int nb_pix_fmts = 0;
    int i, j;

    // 1. 找出SDL和FFmpeg都支持的pix_format，保存在局部数组pix_fmts中。
    // num_texture_formats是一个SDL_PixelFormatEnum数组。see http://wiki.libsdl.org/SDL_RendererInfo
    for (i = 0; i < renderer_info.num_texture_formats; i++) {// 遍历SDL支持的纹理格式，实际就是图片的pix_format
        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {// 遍历FFmpeg支持的pix_format(通过映射的关系来搭建桥梁判断)
            if (renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {// 如果SDL和FFmpeg都支持的pix_format，保存在局部数组pix_fmts中。
                pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                break;  // 找到一个先退出内层for，进行下一个查找。
            }
        }
    }
    pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;// 末尾补空

    // 2. 遍历字典sws_dict
    while ((e = av_dict_get(sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);// 追加字符串
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str)-1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);// scale_sws_opts是用于自动插入比例过滤器的SWS选项

    // 3. 使用简单方法创建输入滤波器AVFilterContext
    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format,
             is->video_st->time_base.num, is->video_st->time_base.den,
             codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
    if (fr.num && fr.den)
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

    if ((ret = avfilter_graph_create_filter(&filt_src,
                                            avfilter_get_by_name("buffer"),
                                            "ffplay_buffer", buffersrc_args, NULL,
                                            graph)) < 0)
        goto fail;

    // 4. 使用简单方法创建输出滤波器AVFilterContext
    ret = avfilter_graph_create_filter(&filt_out,
                                       avfilter_get_by_name("buffersink"),
                                       "ffplay_buffersink", NULL, NULL, graph);
    if (ret < 0)
        goto fail;

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts,  AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto fail;

    last_filter = filt_out;

    /*
     * Note: this macro adds a filter before the lastly added filter, so the
     * processing order of the filters is in reverse。
     * 意思是：AVFilterContext串每次是通过avfilter_link从末尾往头链起来的，顺序是倒转的。
    */
#define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
    \
    ret = avfilter_graph_create_filter(&filt_ctx,                            \
    avfilter_get_by_name(name),           \
    "ffplay_" name, arg, NULL, graph);    \
    if (ret < 0)                                                             \
    goto fail;                                                           \
    \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
    if (ret < 0)                                                             \
    goto fail;                                                           \
    \
    last_filter = filt_ctx;                                                  \
} while (0)

    // 5. 判断是否有旋转角度，如果有需要使用对应的过滤器进行处理；没有则不会添加
    // 额外的过滤器处理，只有输入输出过滤器。
    /*
     * 一 图片的相关旋转操作命令：
     * 1）垂直翻转：                      ffmpeg -i fan.jpg -vf vflip -y vflip.png
     * 2）水平翻转：                      ffmpeg -i fan.jpg -vf hflip -y hflip.png
     * 3）顺时针旋转60°(PI代表180°)：      ffmpeg -i fan.jpg -vf rotate=PI/3 -y rotate60.png
     * 4）顺时针旋转90°：                 ffmpeg -i fan.jpg -vf rotate=90*PI/180 -y rotate90.png
     * 5）逆时针旋转90°(负号代表逆时针，正号代表顺时针)：ffmpeg -i fan.jpg -vf rotate=-90*PI/180 -y rotate90-.png
     * 6）逆时针旋转90°：                  ffmpeg -i fan.jpg -vf transpose=2 -y transpose2.png
     * rotate、transpose的值具体使用ffmpeg -h filter=filtername去查看。
     * 注意1：上面的图片使用ffprobe去看不会有metadata元数据，所以自然不会有rotate与Side data里面的displaymatrix。只有视频才有。
     * 注意2：使用是rotate带有黑底的，例如上面的rotate60.png。图片的很好理解，都是以原图进行正常的旋转，没有难度。
     *
     *
     * 二 视频文件相关旋转的操作：
     * 1.1 使用rotate选项：
     * 1） ffmpeg -i 2_audio.mp4 -metadata:s:v rotate='90' -codec copy 2_audio_rotate90.mp4
     * 但是这个命令实际效果是：画面变成逆时针的90°操作。使用ffprobe一看：
     *     Metadata:
              rotate          : 270
              handler_name    : VideoHandler
           Side data:
              displaymatrix: rotation of 90.00 degrees
           Stream #0:1(und): Audio: aac (LC) (mp4a / 0x6134706D), 44100 Hz, stereo, fltp, 184 kb/s (default)
            Metadata:
              handler_name    : 粤语
    * 可以看到rotate是270°，但displaymatrix确实是转了90°。
    *
    * 2）ffmpeg -i 2_audio.mp4 -metadata:s:v rotate='270' -codec copy 2_audio_rotate270.mp4
    * 同样rotate='270'时，画面变成顺时针90°的操作。rotate=90，displaymatrix=rotation of -90.00 degrees。
    *
    * 3）ffmpeg -i 2_audio.mp4 -metadata:s:v rotate='180' -codec copy 2_audio_rotate180.mp4
    * 而180的画面是倒转的，这个可以理解。rotate=180，displaymatrix=rotation of -180.00 degrees。
    *
    * 2.1 使用transpose选项
    * 1）ffmpeg -i 2_audio.mp4  -vf transpose=1 -codec copy 2_audio_transpose90.mp4(顺时针90°)
    * 2）ffmpeg -i 2_audio.mp4  -vf transpose=2 2_audio_transpose-90.mp4(逆时针90°，不能加-codec copy，否则与transpose冲突)
    * 上面命令按预期正常顺时针的旋转了90°和逆时针旋转90°的画面，但是使用ffprobe看不到rotate或者displaymatrix对应的角度。
    *
    * 3.1 使用rotate+transpose选项
    * 1） ffmpeg -i 2_audio.mp4 -vf transpose=1 -metadata:s:v rotate='90' -vcodec libx264 2_audio_rotate90.mp4
    * 2）ffmpeg -i 2_audio.mp4 -vf transpose=1 -metadata:s:v rotate='180' -vcodec libx264 2_audio_rotate180.mp4
    * 3）ffmpeg -i 2_audio.mp4 -vf transpose=1 -metadata:s:v rotate='270' -vcodec libx264 2_audio_rotate270.mp4
    * 只要使用了transpose选项，rotate就会失效。例如运行上面三个命令，实际只顺时针旋转了90°，即transpose=1的效果，并且，只要存在transpose，它和2.1一样，
    *   使用ffprobe看不到rotate或者displaymatrix对应的角度，这种情况是我们不愿意看到的。所以经过分析，我们最终还是得回到只有rotate选项的情况。
    *
    * 目前我们先记着1.1的三种情况的结果就行，后续有空再深入研究旋转，并且实时流一般都会返回theta=0，不会有旋转的操作。
    */
    if (autorotate) {
        double theta  = get_rotation(is->video_st);

        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", "clock");// 转换过滤器，clock代表，顺时针旋转，等价于命令transpose=1。
                                              // 可用ffmpeg -h filter=transpose查看。查看所有filter：ffmpeg -filters
        } else if (fabs(theta - 180) < 1.0) {
            INSERT_FILT("hflip", NULL);// 镜像左右反转过滤器，
            INSERT_FILT("vflip", NULL);// 镜像上下反转过滤器，经过这个过滤器处理后，画面会反转，类似水中倒影。
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", "cclock");// 逆时针旋转，等价于命令transpose=2.
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);// 旋转角度过滤器
        }
    }

    // 上面看到，除了原过滤器没有Link，其余都是通过了INSERT_FILT进行了Link的。

    // 6. 判断是否需要添加复杂过滤器，并且将输入输出过滤器AVFilterContext进行Link，最后提交整个滤波图。
    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;

    // 7. 经过上面的过程处理后，得到输出的过滤器filt_out，并保存输入的过滤器filt_src
    is->in_video_filter  = filt_src;
    is->out_video_filter = filt_out;

fail:
    return ret;
}

/**
 * @brief 配置音频相关的过滤器。该函数使用了简单过滤器和复杂过滤器的混合，
 *          最后将输入输出filt_asrc、filt_asink过滤器AVFilterContext保存在播放器实例中，和视频类似。
 * @param is 播放器实例。
 * @param afilters 用户传进的复杂过滤器字符串。
 * @param force_output_format 是否强制转换音频格式。
 *
 * @return 它是返回ffmpeg内部的ret，细看了，>=0成功，负数失败。
 *
 * @note 该函数同样可以参考09-02-video-watermark简单过滤器的实现，基本一样。
*/
static int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format)
{
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };// 初始化过去的asink输出是s16格式。
    int sample_rates[2] = { 0, -1 };
    int64_t channel_layouts[2] = { 0, -1 };
    int channels[2] = { 0, -1 };
    AVFilterContext *filt_asrc = NULL, *filt_asink = NULL;
    char aresample_swr_opts[512] = "";
    AVDictionaryEntry *e = NULL;
    char asrc_args[256];
    int ret;

    // 1.  开辟系统管理avfilter的结构体
    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    is->agraph->nb_threads = filter_nbthreads;

    // 2. 遍历字典sws_dict
    while ((e = av_dict_get(swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);// av_strlcatf是追加字符串
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts)-1] = '\0';// 长度末尾补0，所以上面追加完字符串的冒号：能被去掉。
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    // 3. 使用简单方法创建输入滤波器AVFilterContext(获取输入源filter--->buffer). 输入源都是需要传asrc_args相关参数的。
    // 此时输入源的audio_filter_src是从AVCodecContext读取的。
    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   is->audio_filter_src.channels,
                   1, is->audio_filter_src.freq);
    if (is->audio_filter_src.channel_layout)
        snprintf(asrc_args + ret, sizeof(asrc_args) - ret,
                 ":channel_layout=0x%"PRIx64,  is->audio_filter_src.channel_layout);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, NULL, is->agraph);
    if (ret < 0)
        goto end;

    // 4. 使用简单方法创建输出滤波器AVFilterContext
    // 注：
    // 1）下面abuffer、abuffersink(视频是buffer、buffersink)的名字应该是固定的，在复杂字符串过滤器的get例子同理，不过前后需要补一些字符串内容。
    // 例如 mainsrc_ctx = avfilter_graph_get_filter(filter_graph, "Parsed_buffer_0");
    // 这个参2的名字一定是 "Parsed_" + "系统过滤器名字" + "_" + "本次字符串中系统过滤器的序号" 的格式吗？
    // 确实是，可以看filter_graph->filters->name变量
    // 2）而下面ffplay_abuffer、ffplay_abuffersink可以任意。
    ret = avfilter_graph_create_filter(&filt_asink,
                                       avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
                                       NULL, NULL, is->agraph);
    if (ret < 0)
        goto end;

    /*
     * 1）av_int_list_length： #define av_int_list_length(list, term)	av_int_list_length_for_size(sizeof(*(list)), list, term)。
     * 参1 list：指向列表的指针。
     * 参2 term：列表终结符，通常是0或者-1。
     * 返回值：返回列表的长度(即返回元素的长度，例如"abc"，那么返回3.)，以元素为单位，不计算终止符。
     * 该函数具体看av_int_list_length_for_size。
     *
     * 2）av_int_list_length_for_size：unsigned av_int_list_length_for_size	(unsigned elsize, const void *list, uint64_t term)。
     * 参1 elsize：每个列表元素的字节大小(只有1、2、4或8)。例如下面的传进来是sample_fmts,  AV_SAMPLE_FMT_NONE。
     *              那么elsize=sizeof(*(list))=8字节。  数组以指针的大小计算，64位机是8字节，32位机器是4字节。
     * 参2 list：指向列表的指针。
     * 参3 term：列表终结符，通常是0或者-1。
     * 返回值：返回列表的长度(即返回元素的个数，例如"abc"，那么返回3.)，以元素为单位，不计算终止符。
     *
     * 3）av_opt_set_bin：就是和av_opt_set这些一样，只是参数3后面不一样而已，没啥好说的。
     *
     * 4）所以av_opt_set_int_list：av_opt_set_int_list(obj, name, val, term, flags)。
     * 参1 obj：一个带有AVClass结构体成员的结构体，并且AVClass是该结构体首个成员，由于该结构体首地址和第一个成员(即AVClass)是一样的，
     * 所以看到FFmpeg描述av_opt_set_int_list的obj参数时，写成了"要设置选项的AVClass对象"，其实看回av_opt_set等函数族的参数描述---"第一个元素是AVClass指针的结构体。"
     * 两者意思是一样的，只是表达不一样。
     * 参2 name：要设置的key选项。
     * 参3 val：key选项的值。
     * 参4 term 终结符。
     * 参5：查找方式，暂未深入该参数。
     * 返回值：0成功，负数失败。
     *
     * 即总结av_opt_set_int_list函数作用：先检查val值是否合法，否则使用二进制的方式进行设置到AVClass中的option成员当中。
     * 一般支持av_opt_set的有AVFilterContext、AVFormatContext、AVCodecContext，他们内部的第一个成员必定是AVClass，
     * 并且AVClass的option成员指向对应的静态数组，以便查看用户设置的选项是否被支持。
     *
     * 注：INT_MAX、INT_MIN是 'signed int'可以保存的最小值和最大值。
     *
     * 5）关于av_opt_set函数族的使用和理解，可参考https://blog.csdn.net/qq_17368865/article/details/79101659。
     * 6）关于官方文档的参数和返回值的理解(包含源码)，可参考：http://ffmpeg.org/doxygen/trunk/group__opt__set__funcs.html#gac06fc2b2e32f67f067ed7aaec163447f。
    */

    // 初始化设置音频输出过滤器filt_asink为s16和all_channel_counts=1.
    if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts,  AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    // 5. 若指定强制输出音频格式，则先初始相关参数到输出过滤器filt_asink，以准备重采样.
    // 不过在这里还没有调audio_open，所以audio_tgt内部全是0，即这里都是初始化。
    // 通过FFmpeg文档，下面的初始化应该最终被设置到 BufferSinkContext。
    // see http://ffmpeg.org/doxygen/trunk/buffersink_8c_source.html
    if (force_output_format) {
        channel_layouts[0] = is->audio_tgt.channel_layout;
        channels       [0] = is->audio_tgt.channels;
        sample_rates   [0] = is->audio_tgt.freq;
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_counts" , channels       ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates"   , sample_rates   ,  -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
    }

    // 上面看到，原过滤器和输出过滤器是没有Link的。

    // 6. 判断是否需要添加复杂过滤器，并且将输入输出过滤器AVFilterContext进行Link，最后提交整个滤波图。
    //      不管视频还是音频，都是调用这个函数处理复杂过滤器字符串。
    if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0)
        goto end;

    // 7. 经过上面的过程处理后，得到输出的过滤器filt_asink，并保存输入的过滤器filt_asrc
    is->in_audio_filter  = filt_asrc;
    is->out_audio_filter = filt_asink;

end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    return ret;
}
#endif  /* CONFIG_AVFILTER */

// 音频解码线程
static int audio_thread(void *arg)
{
    VideoState *is = arg;
    AVFrame *frame = av_frame_alloc();  // 分配解码帧
    Frame *af;
#if CONFIG_AVFILTER
    int last_serial = -1;
    int64_t dec_channel_layout;
    int reconfigure;
#endif
    int got_frame = 0;  // 是否读取到帧
    AVRational tb;      // timebase
    int ret = 0;

    if (!frame)
        return AVERROR(ENOMEM);

    do {
        // 1. 读取解码帧
        if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0)
            goto the_end;

        // 2. 读取帧成功
        if (got_frame) {
            tb = (AVRational){1, frame->sample_rate};// 设置为sample_rate为timebase

#if CONFIG_AVFILTER

            // 3. 判断过滤器源格式、通道数、通道布局、频率以及解码器的serial是否一样，若全部一样记录为0，否则只要有一个不一样，记录为1.
            dec_channel_layout = get_valid_channel_layout(frame->channel_layout, frame->channels);
            reconfigure =
                    cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.channels, frame->format, frame->channels)
                            ||
                    is->audio_filter_src.channel_layout != dec_channel_layout ||
                    is->audio_filter_src.freq           != frame->sample_rate ||
                    is->auddec.pkt_serial               != last_serial;

            // 4. 如果不一样，更新当前帧的音频相关信息到源过滤器当中，并且重新 根据输入源 配置过滤器。
            if (reconfigure) {
                char buf1[1024], buf2[1024];
                av_get_channel_layout_string(buf1, sizeof(buf1), -1, is->audio_filter_src.channel_layout);
                av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);// 获取通道布局的字符串描述
                av_log(NULL, AV_LOG_DEBUG,
                       "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                       is->audio_filter_src.freq, is->audio_filter_src.channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                       frame->sample_rate, frame->channels, av_get_sample_fmt_name(frame->format), buf2, is->auddec.pkt_serial);

                is->audio_filter_src.fmt            = frame->format;
                is->audio_filter_src.channels       = frame->channels;
                is->audio_filter_src.channel_layout = dec_channel_layout;
                is->audio_filter_src.freq           = frame->sample_rate;
                last_serial                         = is->auddec.pkt_serial;// 一般开始是这里不等，因为last_serial初始值=-1

                if ((ret = configure_audio_filters(is, afilters, 1)) < 0)// 注意这里是强制了输出格式force_output_format=1的。
                    goto the_end;
            }

            // 5. 重新配置过滤器后，那么就可以往过滤器中输入解码一帧。
            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;

            // 6. 从输出过滤器读取过滤后的一帧音频。
            // while一般从这里的 av_buffersink_get_frame_flags 退出，第二次再读时，因为输出过滤器没有帧可读会返回AVERROR(EAGAIN)。
            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                // 6.1 从输出过滤器中更新时基
                tb = av_buffersink_get_time_base(is->out_audio_filter);
#endif
                // 6.2 获取可写Frame。如果没有可写帧，会阻塞等待，直到有可写帧；当用户中断包队列 返回NULL。
                if (!(af = frame_queue_peek_writable(&is->sampq)))  // 获取可写帧。留个疑问，视频好像没有获取可写帧？答：都有的。封装在queue_picture()。
                    goto the_end;

                // 6.3 设置Frame pts、pos、serial、duration
                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = frame->pkt_pos;
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d((AVRational){frame->nb_samples, frame->sample_rate});

                // 6.4 保存AVFrame到帧队列，和更新帧队列的 写坐标windex 以及 大小size。
                av_frame_move_ref(af->frame, frame);// frame数据拷贝到af->frame和重置frame，但frame的内存还是可以继续使用的。
                frame_queue_push(&is->sampq);       // 实际只更新帧队列的写坐标windex和大小size

#if CONFIG_AVFILTER
                /*
                 * 有什么用？留个疑问。
                 * 答(个人理解)：看上面decoder_decode_frame()的代码，只有包队列的serial==解码器的pkt_serial时才能获取到解码帧，
                 * 也就是说，在decoder_decode_frame()出来时，两者肯定是相等的，至于不等，那么就是从decoder_decode_frame()到这里的代码之间，用户进行了seek操作，
                 * 导致了包队列的serial != 解码器的pkt_serial。那么区别就是提前break掉，ret会按照最近一次>=0退出，
                 * 而不加这个语句就是av_buffersink_get_frame_flags时读到ret == AVERROR(EAGAIN)退出。
                 * 此时可以看到帧仍然是会被放进帧队列的。
                 * 想测试也不难，将break注掉，随便打印东西，seek的时候看视频是否正常即可。
                */
                if (is->audioq.serial != is->auddec.pkt_serial){
                    printf("tyycode+++++++++++++++++++++++++++++++++++++++++++++++++++++++++\n");
                    break;
                }

            }

            if (ret == AVERROR_EOF) // 检查解码是否已经结束，解码结束返回0。哪里返回0了？留个疑问
                is->auddec.finished = is->auddec.pkt_serial;
#endif
        }// <== if (got_frame) ==>

    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    /*
     * 上面为什么ret == AVERROR_EOF结束了还继续回到do while循环？留个疑问。
     * 答：1）这是因为，即使结束了，此时包队列可能还会有包，需要继续进行解码到帧队列当中。
     * 2）而此时读线程当中，同样会继续读取，到时会一直读到AVERROR_EOF，所以会刷一次空包(eof标记决定，所以只会刷一次)，然后continue继续判断帧队列是否播放完毕。
     * 3）然后这里的解码线程audio_thread最后读到空包后，同样应该是会被放在帧队列，然后解码线程就会继续调用decoder_decode_frame，由于没包，
     * 4）所以解码线程会阻塞在packet_queue_get，等待读线程显示完最后一帧。如果设置了自动退出，那么读线程直接退出，否则会一直处于for循环，但啥也不做，等待中断。
     * 5）最终的中断我看了一下，是由do_exit内的stream_close的request_abort=1进行中断的。而do_exit是有SDL的事件进行调用，我们实际需求可以看情况进行触发中断信号。
    */

the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&is->agraph);
#endif
    av_frame_free(&frame);
    return ret;
}

/**
 * @brief 创建解码线程, audio/video有各自独立的线程。
 * @param d 解码器。
 * @param fn 线程回调函数。
 * @param thread_name 线程回调函数名字。
 * @param arg 回调参数。这里是播放器实例 is。
 *
 * @return 成功0 失败AVERROR(ENOMEM)，或者说失败返回负数。
 */
static int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void* arg)
{
    packet_queue_start(d->queue);                               // 启用对应的packet 队列。在decoder_init时，解码器的queue指向播放器对应的队列。
    d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);    // 创建解码线程
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }

    return 0;
}

// 视频解码线程
static int video_thread(void *arg)
{
    VideoState *is = arg;
    AVFrame *frame = av_frame_alloc();  // 分配解码帧
    double pts;                         // pts
    double duration;                    // 帧持续时间
    int ret;

    //1 获取stream timebase
    AVRational tb = is->video_st->time_base; // 获取stream timebase
    //2 获取帧率，以便计算每帧picture的duration。
    // 估计优先选择流中的帧率(https://blog.csdn.net/weixin_44517656/article/details/110355462)，具体看av_guess_frame_rate。
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

#if CONFIG_AVFILTER
    AVFilterGraph *graph = NULL;
    AVFilterContext *filt_out = NULL, *filt_in = NULL;
    int last_w = 0;
    int last_h = 0;
    enum AVPixelFormat last_format = -2;
    int last_serial = -1;
    int last_vfilter_idx = 0;

#endif

    if (!frame)
        return AVERROR(ENOMEM);


    for (;;) {  // 循环取出视频解码的帧数据
        // 3 获取解码后的视频帧
        ret = get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;   //解码结束, 什么时候会结束
        if (!ret)           //没有解码得到画面, 什么情况下会得不到解后的帧。实际上get_video_frame解码到文件末尾也会返回0.
            continue;

#if CONFIG_AVFILTER
        if (last_w != frame->width
               || last_h != frame->height
               || last_format != frame->format
               || last_serial != is->viddec.pkt_serial  // 解码器的pkt_serial就是每个节点的serial，它会不断更新，具体看packet_queue_get。
               || last_vfilter_idx != is->vfilter_idx) {
            av_log(NULL, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d ---to--- size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *)av_x_if_null(av_get_pix_fmt_name(frame->format), "none"), is->viddec.pkt_serial);

            avfilter_graph_free(&graph);        // 即使graph也是安全的。
            // 1. 创建系统滤波
            graph = avfilter_graph_alloc();
            if (!graph) {
                ret = AVERROR(ENOMEM);
                goto the_end;
            }
            graph->nb_threads = filter_nbthreads;

            // 2. 配置相关过滤器保存在播放器实例中。内部使用了简单过滤器+复杂的字符串过滤器的组合。
            if ((ret = configure_video_filters(graph, is, vfilters_list ? vfilters_list[is->vfilter_idx] : NULL, frame)) < 0) {
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                goto the_end;
            }

            // 这里主要是从解码帧保存一些已知的内容，判断下一帧到来时，和之前的内容是否一致。
            filt_in  = is->in_video_filter; // 获取经过configure_video_filters处理后的输入输出过滤器
            filt_out = is->out_video_filter;
            last_w = frame->width;          // 保存上一次解码帧的分辨率，经过configure_video_filters前后，帧的分辨率应该是不会变的，例如是1920x1080，经过过滤器处理应该还是1920x1080。
            last_h = frame->height;
            last_format = frame->format;
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx; // 保存上一次过滤器的下标，主要用来改变configure_video_filters参3复杂过滤器的选项
            frame_rate = av_buffersink_get_frame_rate(filt_out);
        }

        // 4. 上面配置好过滤器后，现在就可以往输入过滤器添加解码后的一帧数据，进行过滤处理了。
        // 实际上过滤器的流程就是：配置好过滤器后，添加帧，输出处理后的帧，就是这么简单。
        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {// 一般只会执行一次while，第二次会在av_buffersink_get_frame_flags中break掉，进入下一次的for循环
            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            // 5. 获取处理后的一帧
            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);// 与av_buffersink_get_frame一样的。
            if (ret < 0) {
                if (ret == AVERROR_EOF){
                    is->viddec.finished = is->viddec.pkt_serial;
                }
                ret = 0;
                break;
            }

            // 6. 每一次从过滤器中获取一帧的延时，若大于不同步的阈值，不做处理，将其赋值为0
            // (感觉他起名不好，他认为是上一次的延时，我感觉这一次的延时更恰当和更好理解)
            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;

            // 更新时基，从过滤器中获取，一开始从流中获取
            tb = av_buffersink_get_time_base(filt_out);
#endif
            //printf("tyycode frame_rate:%d/%d, tb:%d/%d\n", frame_rate.num, frame_rate.den, tb.num, tb.den);// 对于实时流，帧率一般都是固定为25,时基固定为90k

            // 7. 计算帧持续时间和换算pts值为秒
            // 1/帧率 = duration 单位秒, 没有帧率时则设置为0, 有帧率时计算出帧间隔，单位转成double。依赖滤波器求出帧率，再求出帧时长
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational){frame_rate.den, frame_rate.num}) : 0);
            // 根据AVStream timebase计算出pts值, 单位为秒
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            // 8. 将解码后的视频帧插入队列
            ret = queue_picture(is, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
            // 9. 释放frame对应的数据
            av_frame_unref(frame);

#if CONFIG_AVFILTER
            if (is->videoq.serial != is->viddec.pkt_serial)
                break;

        }//<== while end ==>
#endif

        if (ret < 0) // 返回值小于0则退出线程
            goto the_end;

    }// <== for (;;) end ==>

the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&graph);
#endif
    av_frame_free(&frame);
    return 0;
}

static int subtitle_thread(void *arg)
{
    VideoState *is = arg;
    Frame *sp;
    int got_subtitle;
    double pts;

    for (;;) {
        // 1. 从字幕队列中获取可写入的一帧，准备用于存储解码后的字幕帧。
        if (!(sp = frame_queue_peek_writable(&is->subpq)))
            return 0;

        // 2. 开始解码字幕pkt，解码后的字幕保存在sp->sub中。
        // 音视频参2都是传Frame，只有字幕传NULL，参3则相反，音视频传NULL，字幕传AVSubtitle。
        if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub)) < 0)
            break;

        pts = 0;

        // 3. 解码成功且字幕格式要求是图形，则更新相关信息和将帧放在帧队列中。
        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double)AV_TIME_BASE;// 转成秒
            // 更新自定义字幕帧的相关信息
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            frame_queue_push(&is->subpq);
        } else if (got_subtitle) {// 4. 解码成功但sp->sub.format != 0的情况，则丢弃该字幕。
            avsubtitle_free(&sp->sub);
        }
    }
    return 0;
}

/* copy samples for viewing in editor window */
/**
 * @brief 循环将音频帧数据拷贝到is->sample_array数组当中。
 *          注意，想要debug该函数只能将视频禁用，但是画面好像并未显示出波形。
 *
 * @param is 播放器实例。
 * @param samples 音频帧数据。
 * @param samples_size 音频帧数据字节大小，并非采样点个数(see audio_decode_frame())。
 *
 * @return void。
 */
static void update_sample_display(VideoState *is, short *samples, int samples_size)
{
    int size, len;

    // 这里为什么要除以sizeof(short)呢？
    // 答：因为sample_array数组单位是int16_t，只是为了保持一致的单位去比较长度。
    // 内部定义：int16_t sample_array[SAMPLE_ARRAY_SIZE];
    // 1. 求出samples_size在int16_t数组需要占用的元素个数。例如samples_size=4字节，那么在int16_t数组占用两个元素。
    size = samples_size / sizeof(short);
    while (size > 0) {
        // 2. 求出int16_t剩余元素个数、虽然这样算，但是单位同样是int16_t单位。
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        // 3. 如果数组有足够大小，则拷贝全部size；否则拷贝数组剩余元素个数len，剩余大小求法：len*sizeof(short)。
        if (len > size)
            len = size;
        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));

        // 4. 因为现在samples可能没完全拷贝，所以现在先跳过已经拷贝的元素数据，同样数组下标跳过对应的元素个数。
        samples += len;
        is->sample_array_index += len;

        // 5. 如果数组下标大于数组大小，置为0.
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
            is->sample_array_index = 0;

        // 6. 去除本次拷贝的元素个数，以进行下一次的循环拷贝。
        size -= len;
    }
}

/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */
/**
 * @brief 如果sync_type是视频或外部主时钟，则返回所需样本数以获得更好的同步。
 *
 * @param is 播放器实例。
 * @param nb_samples    正常播放的采样数量。
 *
 * @return 若判断不需要同步，则直接返回传入的采样点个数；若同步则返回同步计算后的采样点个数。
 *
 * @note 在开发时知道怎么使用即可，下面的每个变量及其意义都了解了，但是它的数学公式(以及数学图形)还不太清楚，
 *          但是这个不太重要，因为开发时基本以音频为主时钟；以视频为主时钟的一般只有视频流，因为不含音频，
 *          所以不会涉及到这里的内容。 所以我们无需了解更深层次的含义。
 */
static int synchronize_audio(VideoState *is, int nb_samples)
{
    int wanted_nb_samples = nb_samples;

    // 1. 视频、外部是主时钟，需要调整帧的采样点个数
    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        // 1.1 求出音频时钟与主时钟的差值。正数音频超前，负数音频落后。单位秒。
        diff = get_clock(&is->audclk) - get_master_clock(is);

        // 1.2 误差在 AV_NOSYNC_THRESHOLD(10s) 范围再来看看要不要调整。
        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            //printf("1 audio_diff_cum: %lf, diff: %lf, audio_diff_avg_coef: %lf\n", is->audio_diff_cum, diff, is->audio_diff_avg_coef);
            /*
             * audio_diff_cum代表本次的误差(diff)，加上历史的权重比误差(is->audio_diff_avg_coef * is->audio_diff_cum)之和。
             * 并且AUDIO_DIFF_AVG_NB次数越多，历史的权重比误差占比越小。
            */
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            //printf("2 audio_diff_cum: %lf\n", is->audio_diff_cum);

            // 1.3 连续20次不同步才进行校正，不到也会直接返回原本的采样点个数。
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate(没有足够的度量来做出正确的估计) */
                is->audio_diff_avg_count++;
            } else {
                // 1.4 否则进行校正，但依然需要判断。
                /* estimate the A-V difference。估算A-V的差值。
                 * 计算后的avg_diff并不是一次过校正完本次diff的全部误差，而是先校正一部分，剩下的根据下一次的误差在校正。
                 * 例如假设AUDIO_DIFF_AVG_NB=4，前3次的diff分别是2s、2s、1s，本次的diff是2s，那么得出：audio_diff_cum=5.104s.
                 * 所以avg_diff=5.104x0.2=1.0208s，而不是校正完本次的2s。也就是平滑校正，这样做可以让下面的采样点变化尽量小，从而优化声音的变尖或者变粗。
                 * 因为同样的频率下，采样点变大或者变小了，会导致声音变形。
                */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
                //avg_diff = diff; // 这样也可以，但是没有上面平滑。

                // 1.5 如果avg_diff大于同步阈值，则进行调整，否则在[-audio_diff_threshold, +audio_diff_threshold]范围内不调整，与视频类似。
                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    /*
                     * 根据公式：采样点个数/采样频率=秒数，得出：采样点个数=秒数*采样频率。
                     * 例如diff超前了0.02s，采样频率是44.1k，那么求出采样点个数是：0.02s*44.1k=882.这个值代表超前的采样点个数。
                     * 然后wanted_nb_samples=原本采样点 + 求出的超前的采样点个数；例如1024+882.
                     * 上面也说到了，ffplay考虑到同样频率下，采样点变化过大或者过小的情况，会有一个变化区间[min_nb_samples，max_nb_samples]
                     * 所以本次同步后采样点个数变成：1024*110/100=1126.4=1126左右(取整)。
                     *
                     * 注意是乘以is->audio_src.freq变量的采样频率值，是从audio_decode_frame()每次在重采样参数改变后，从解码帧中获取的值，也就是上一次的帧的采样频率值。
                    */
                    wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    // av_clip 用来限制wanted_nb_samples最终落在 min_nb_samples~max_nb_samples
                    // nb_samples *（90%~110%）
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }

                /*
                 * (非音频才会打印，例如视频同步-sync video)
                 * 这里看打印看前三个值即可：
                 * 1）例如：diff=0.402868 adiff=0.388198 sample_diff=32 apts=2.280 audio_diff_threshold=0.064000
                 * 上面看到前3个值都是正数，说明此时是超前，采样点需要增加。
                 * 2）例如：diff=-1.154783 adiff=-1.137120 sample_diff=-32 apts=2.240 audio_diff_threshold=0.064000
                 * 上面看到前3个值都是负数，说明此时是落后，采样点需要减少。
                 *
                 * 3）总结：若超前，前3个打印值都是正数；若落后，前3个打印值都是负数。
                */
                av_log(NULL, AV_LOG_INFO, "diff=%f avgdiff=%f sample_diff=%d apts=%0.3f audio_diff_threshold=%f\n",
                       diff, avg_diff, wanted_nb_samples - nb_samples,
                       is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            // 大于 AV_NOSYNC_THRESHOLD 阈值，该干嘛就干嘛，不做处理了。
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum       = 0;   // 恢复正常后重置为0
        }
    }

    // 2. 否则是音频主时钟直接返回原采样点个数
    return wanted_nb_samples;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
/**
 * @brief 从音频帧队列读取一帧，判断是否需要重采样，但不论是否需要重采样，
 *          读取到的音频帧数据都保存在is->audio_buf中，大小由返回值返回。也就说，这个函数更多是履行重采样的作用。
 *
 * @param is 播放器实例。
 *
 * @return 成功：返回一帧音频数据的实际大小(字节)，无论是否进行了重采样。失败：返回-1.
 *                  返回-1的情况比较多，看代码。
 */
static int audio_decode_frame(VideoState *is)
{
    int data_size, resampled_data_size;
    int64_t dec_channel_layout;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    // 暂停直接返回-1，让音频回调补充0数据，播放静音。
    if (is->paused)
        return -1;

    /* 1. 获取一帧解码后的音频帧。
     * 1.1 首先判断音频帧队列是否有未显示的，如果有则获取并出队，但是若serial不是最新会重新读取(因为在解码线程看到，帧队列是有可能含有不是最新的serial的帧)；
     *      没有则判断，若经过的时间大于阈值的一半，直接返回-1，否则休眠1ms再判断。
     *
     * 注:
     * 留个疑问1：若休眠完后，刚好一帧数据被读走，在frame_queue_peek_readable会阻塞在条件变量吗？
     * 答：应该不存在，因为本场景只有这里读取音频帧而已，并未有其它地方竞争读取。
     * 留个疑问2：读取到的帧会因frame_queue_unref_item被清空掉数据吗？
     * 答：不会，音频帧队列同样是keep_last+rindex_shown机制，第一次不会进行释放，所以后面也不会。
    */
    do {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            /*
             * 1）(av_gettime_relative() - audio_callback_time)：此次SDL回调sdl_audio_callback到现在的时间。
             * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec：阈值，和audio_diff_threshold算法一样。除以2代表阈值的一半。乘以1000000LL代表需要单位相同才能比较。
             * if表示：若帧队列一直没数据并超过阈值的一半时间，则补充数据。若第一次帧队列为空，并且满足if，补充一次数据；若还是同样的SDL回调，并且第二次帧队列仍是空，if肯定满足，
             * 因为audio_callback_time一样，而实时时间增大，所以继续补充数据，直至补充完SDL的len。
             *
             * 2）更深层次的理解，is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec实际是一帧音频的播放时长，
             * 因为audio_hw_buf_size就是在audio_open打开时返回的音频缓存大小，单位是字节，通过采样点个数求出；而audio_tgt.bytes_per_sec就是通过ch*fmt*freq求出，单位也是字节。
             * 加上平时求音频一帧播放时长：采样点个数(samples)/采样频率(freq)；根据两者求出的播放时长是一样的，那么有公式：
             * audio_hw_buf_size / audio_tgt.bytes_per_sec = samples / freq; 代入audio_tgt.bytes_per_sec = ch*fmt*freq；
             * audio_hw_buf_size / ch*fmt*freq = samples / freq;化简后：
             * audio_hw_buf_size / ch*fmt = samples; 因为这里是采用s16的交错模式进行输出的，所以ch=1，fmt=2。最终得出：
             * audio_hw_buf_size / 2 = samples；
             * 根据s16格式，采样点个数和字节数就是2倍的关系，所以推断的公式是完全成立的。
             *
             * 3）换句话说，下面if意思就简单了，即：若帧队列一直没数据，并且每次调用audio_decode_frame都超过一帧的一半时长，那么补充默认的数据。
             *
             * 注：上面都是按照 一次SDL回调sdl_audio_callback 进行解释的。
            */
            if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2){
                return -1;
            }

            av_usleep (1000);
        }
#endif
        // 若队列头部可读，则由af指向可读帧
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return -1;

        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    // 2. 根据frame中指定的音频参数获取缓冲区的大小 af->frame->channels * af->frame->nb_samples * 2
    data_size = av_samples_get_buffer_size(NULL,
                                           af->frame->channels,
                                           af->frame->nb_samples,
                                           af->frame->format, 1);
    // 3. 获取声道布局。
    // 获取规则：若存在通道布局 且 通道布局获取的通道数和已有通道数相等，则获取该通道数；
    //          否则根据已有通道数来获取默认的通道布局。
    dec_channel_layout =
            (af->frame->channel_layout &&
             af->frame->channels == av_get_channel_layout_nb_channels(af->frame->channel_layout)) ?
                af->frame->channel_layout : av_get_default_channel_layout(af->frame->channels);

    // 4. 获取样本数校正值：若同步时钟是音频，则不调整样本数；否则根据同步需要调整样本数
    /*
     * 1）音频如何同步到视频？
     *  答：类似视频同步到音频(音频为主时钟)。视频为主时钟的话，无非就是音频超前或者音频落后。
     *      而音频超前了，只要多放几个采样点就能等待视频；音频落后了，那么就少播放几个采样点就能追上视频，这是视频为主时钟的主要思路。
     *      但是注意，音频的丢弃或者增长并不能随意增加，必须通过重采样进行，如果人为挑选某些采样点丢弃或者增加，会导致音频不连续，这与视频有区别。
     *      具体可以看pdf。
     * 2）音频同步到视频为啥在audio_decode_frame()函数做呢？
     *  答：参考视频，都是获取到一帧数据后，然后判断其是否能够显示，如果能则直接显示；否则进行相应的休眠处理。
     * 3）在2）的基础上，即音频同步到视频为啥在audio_decode_frame()函数做，并且要在重采样之前做呢？
     *  答：只是因为，想要重采样，就必须知道重采样想要采样点的个数，那么这个想要的采样点个数如何获取？就是根据音频超前或者落后，
     *      得出对应的采样点，这样我们就能够进行重采样。否则重采样在同步之前处理，想要的采样点个数是未知的。
     *      在同步和重采样后，返回的数据就能通过SDL回调进行显示了，进而做到音视频同步。
     *
    */
    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    // 5. 判断是否需要进行重采样。若需要这里先进行初始化。
    // is->audio_tgt是SDL可接受的音频帧数，是audio_open()中取得的参数
    // 在audio_open()函数中又有"is->audio_src = is->audio_tgt""
    // 此处表示：如果frame中的音频参数 == is->audio_src == is->audio_tgt，
    // 那音频重采样的过程就免了(因此时is->swr_ctr是NULL)
    // 否则使用frame(源)和is->audio_tgt(目标)中的音频参数来设置is->swr_ctx，
    // 并使用frame中的音频参数来赋值is->audio_src
    if (af->frame->format           != is->audio_src.fmt            || // 采样格式
            dec_channel_layout      != is->audio_src.channel_layout || // 通道布局
            af->frame->sample_rate  != is->audio_src.freq           || // 采样率
            // 第4个条件, 要改变样本数量, 那就是需要初始化重采样。
            // samples不同且swr_ctx没有初始化。 因为已经初始化可以直接重采样，和上面不一样，上面3个参数一旦改变，必须重新初始化。
            (wanted_nb_samples      != af->frame->nb_samples && !is->swr_ctx)
            )
    {
        swr_free(&is->swr_ctx);
        // 5.1 开辟重采样器以及设置参数。
        is->swr_ctx = swr_alloc_set_opts(NULL,
                                         is->audio_tgt.channel_layout,  // 目标输出
                                         is->audio_tgt.fmt,
                                         is->audio_tgt.freq,
                                         dec_channel_layout,            // 数据源
                                         af->frame->format,
                                         af->frame->sample_rate,
                                         0, NULL);
        // 5.2 重采样器初始化。
        if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                   af->frame->sample_rate, av_get_sample_fmt_name(af->frame->format), af->frame->channels,
                   is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
            swr_free(&is->swr_ctx);
            return -1;
        }

        /*
         * 更新源音频信息。留个疑问，这样不会导致上面判断是否重采样错误吗？
         * 即本来输出设备只支持输出一种格式，输入设备是另一种格式，赋值后因为源和帧的格式相同，导致无法重采样到和输出格式一样的格式。
         * 答：不会，首先要理解audio_src、audio_tgt的作用。
         * 1）audio_src的作用是：保存上一次帧的重采样3元祖，用于判断是否需要重新初始化重采样器，
         * 由于首次时is->audio_src = is->audio_tgt，所以从输出过滤器获取的frame应该也是一样的。因为在audio_thread线程的configure_audio_filters配置
         * 输出过滤器时，会被强制配置与audio_tgt一样的格式。
         *
         * 2）audio_tgt的作用是：比较简单，就是固定为重采样后的输出格式，该变量从audio_open调用后，不会被改变，它是SDL从硬件设备读取到的硬件支持参数。
         * 3）那么理解这里的代码就简单了：
         *      例如开始有is->audio_src = is->audio_tgt，不会进行重采样；
         *      假设frame的重采样参数改变，if条件满足，那么重采样器初始化，audio_src更新为frame的参数；
         *      假设frame的重采样参数再次改变，frame与上一次的frame(即audio_src)不一样，if条件满足，那么需要进行重新重采样器初始化，以此类推。。。
        */
        is->audio_src.channel_layout = dec_channel_layout;
        is->audio_src.channels       = af->frame->channels;
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = af->frame->format;
    }

    // 5.3 开始重采样前的参数计算
    if (is->swr_ctx) {
        // 5.3.1 获取重采样的输入数据
        // 重采样输入参数1：输入音频样本数是af->frame->nb_samples
        // 重采样输入参数2：输入音频缓冲区
        const uint8_t **in = (const uint8_t **)af->frame->extended_data; // data[0] data[1]

        // 5.3.2 获取重采样后的存储缓存，计算重采样输出的采样点个数、以及获取存储输出采样点个数的缓存总大小。
        // 具体算法详看，不难：https://blog.csdn.net/weixin_44517656/article/details/117849297
        // 重采样输出参数1：输出音频缓冲区尺寸
        uint8_t **out = &is->audio_buf1; //真正分配缓存audio_buf1，最终给到audio_buf使用
        // 重采样输出参数2：输出音频缓冲区
        int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate
                + 256;// 留个疑问，计算重采样输出的采样点个数为啥加上256？？？ see 下面if(len2 == out_count)的解释。
        int out_size  = av_samples_get_buffer_size(NULL, is->audio_tgt.channels,
                                                   out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }

        // 5.3.3 如果frame中的样本数经过校正，则条件成立，设置重采样补偿，当需要时FFmpeg会自己进行补偿。
        if (wanted_nb_samples != af->frame->nb_samples) {
            /*
             * 1）swr_set_compensation：激活重采样补偿(“软”补偿)。这个函数是在需要时在swr_next_pts()中内部调用。
             * 参1：重采样器。
             * 参2：每个样本PTS的增量(差距)。实际上就是校正前后求出的输出采样点个数之差。
             * 参3：需要补偿的样本数量。实际上就是校正后，输出的采样点数量。
             * 返回值：>=0成功，小于0返回错误码。
            */

            // 5.3.3.1 求出增量。
            // 算法也很简单，和上面求out_count是一样的：
            // 1）首先通过未经过修正的源采样点个数，求出原本正常的输出采样点个数：af->frame->nb_samples * is->audio_tgt.freq / af->frame->sample_rate;
            // 2）然后再通过经过校正后的采样点个数，求出输出采样点个数：wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate;
            // 3）然后2）-1），作减法合并表达式即可得到下面的公式。
            int sample_delta = (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq
                    / af->frame->sample_rate;
            // 5.3.3.2 求出要补偿的样本数量。注意，要补偿的样本数量指的是校正后的采样点个数，不要将其当成5.3.3.1。
            int compensation_distance = wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate;
            // swr_set_compensation
            if (swr_set_compensation(is->swr_ctx, sample_delta, compensation_distance) < 0) {
                av_log(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }

        // 5.4 为audio_buf1开辟内存。
        // 1）av_fast_realloc：在buffer不足的情况下，重新分配内存(内部应该会把旧的释放，具体看源码)，否则不做处理。
        // 2）av_fast_malloc：与av_fast_realloc一样，但FFMPEG官方说更安全高效，避免可能发生内存泄漏。see http://ffmpeg.org/pipermail/ffmpeg-cvslog/2011-May/036992.html。
        // 3）关于FFMPEG更多的开辟堆内存操作，see https://www.cnblogs.com/tocy/p/ffmpeg-libavutil-details.html。
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);// audio_buf1、audio_buf1_size初始值是NULL和0.
        if (!is->audio_buf1)
            return AVERROR(ENOMEM);

        // 5.5 真正调用音频重采样的函数：返回值是重采样后得到的音频数据中单个声道的样本数。
        // swr_convert函数可以详看：https://blog.csdn.net/weixin_44517656/article/details/117849297
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            /*
             * 这里看到ffpaly的做法，当重采样后返回的样本数和缓冲区相等，他认为缓冲区太小了。
             * 也就是说，他上面计算输出样本数的缓冲区大小时，加上256的目的就是为了扩大缓冲区，而不是增加输出的采样点个数。
             * 从而得出，上面计算out_count的意思是指：音频输出缓冲区大小而不是指输出采样点个数。而在swr_convert参3你可以认为是输出采样点个数，
             * 因为即使你这样传，它也不会每次按照最大的输出采样点个数给你返回，例如out_count=1024，可能实际返回给你921.
             * 这也就是我们可以对这个out_count进行调整增大的意思。
             *
             * 这里他重新初始化了重采样器，具体目的未知，后续可以自己研究。可以这样做：把swr_init注释掉，
             * 并添加对应打印或者getchar()出现时，让它停在这里。或者添加打印观察，看视频有什么异常。
            */
            av_log(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }

        // 5.7 保存采样后的数据以及重采样返回的一帧音频数据大小(以字节为单位)
        // 这里可以看到，audio_buf1是实际开辟了内存的，而audio_buf只是简单指向它。
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);// 获取重采样帧的字节大小，也可av_samples_get_buffer_size()获取
    } else {
        // 未经重采样，则将指针指向frame中的音频数据
        is->audio_buf = af->frame->data[0]; // s16交错模式data[0], fltp data[0] data[1]
        resampled_data_size = data_size;
    }

    // 6. 更新音频时钟audio_clock与audio_clock_serial。但是注意没有更新is->audclk这个变量的时钟，留个疑问。
    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))            // (double) af->frame->nb_samples / af->frame->sample_rate求出的就是一个音频帧所占时长
        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;// 帧的serial由is->auddec.pkt_serial获取。

#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif

    // 7. 返回一帧音频数据的实际大小，无论是否进行了重采样。
    return resampled_data_size;
}

/* prepare a new audio buffer */
/**
 * @brief sdl_audio_callback 从音频帧队列中拷贝数据到SDL的 stream 显示。
 * @param opaque    指向user的数据。
 * @param stream    拷贝PCM的地址。
 * @param len       需要拷贝的长度，由SDL传入。一般以512单位。1024时，因为ffpaly默认出错时返回SDL_AUDIO_MIN_BUFFER_SIZE=512的补充字节，所以若是这种情况：
 *                      首次audio_decode_frame返回错误，那么stream的前512字节被0填充；len=512>0继续循环，此时读到正常的音频数据，拷贝到stream下标为512-1023的地址。
 *                  也就说，出现错误时，会被对应的0进行补充，这样做是为了方便进行音视频校正。
 * @return void。
 */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len)
{
    VideoState *is = opaque;
    int audio_size, len1;

    // 在while调用audio_decode_frame可能产生延迟，当超过一定延时时需要进行处理。例如获取不到解码帧，需要补充数据。
    // audio_callback_time就是这个作用(作用之一，下面尾部还有一个另外的作用)，用于判断每次补充数据时，若此时帧队列没数据，
    // 则是否已经超过一定阈值，若超过需要人为进行补充数据。  至于补多少次，由 SDL的传入参数len 和 帧队列是否有数据 决定。
    audio_callback_time = av_gettime_relative();

    // 1. 循环读取，直到读取到SDL需要的足够的数据len，才会退出循环。
    while (len > 0) {
        /* 读取逻辑：
         * (1)如果is->audio_buf_index >= is->audio_buf_size，说明audio_buf消耗完了，
         * 则调用audio_decode_frame重新填充audio_buf。
         *
         * (2)如果is->audio_buf_index < is->audio_buf_size则说明上次拷贝还剩余一些数据，
         * 先拷贝到stream再调用audio_decode_frame
         *
         */
        // 1.1 数据不足，进行补充.
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_size = audio_decode_frame(is);
            if (audio_size < 0) {
                /* if error, just output silence */
                /*
                 * 当出现错误时，这里置audio_buf为空，不会拷贝数据到stream，不过因为memset stream为0，相当于拷贝了0数据，
                 * 所以SDL这次回调会有静音数据。但是时钟还是需要进行更新。
                 * 留个疑问，audio_decode_frame怎么才会返回负数？
                 *
                 * 注意：只有数据不足时，才有可能报错(因为只有消耗完数据才进来补充)。所以报错时将audio_buf置空，虽然audio_buf仍可能还有正常数据或者填充的0数据，
                 * 但是已经被拷贝到stream了，所以置空是安全的(例以len=1024，每次补充512，第一次512正常，第二次补充失败的例子理解)，这一点对理解audio_callback_time有作用。
                */
                is->audio_buf = NULL;
                // 这里不是相当于直接等于SDL_AUDIO_MIN_BUFFER_SIZE吗？ffplay又除以乘以搞得那么复杂？
                // ffpaly获取解码帧报错时，固定以512个字节填充。
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            } else {
                if (is->show_mode != SHOW_MODE_VIDEO)// 显示音频波形，禁用视频才会进来。main函数添加：display_disable = 1;可先忽略。
                    update_sample_display(is, (int16_t *)is->audio_buf, audio_size);

                is->audio_buf_size = audio_size; // 讲字节 多少字节。保存本次获取到的帧数据。
            }

            // 不管是否读取帧队列的数据成功，都要重置audio_buf_index。
            is->audio_buf_index = 0;
        }

        // 来到这里，audio_buf肯定是有数据的，要么是刚补充完，要么是还还有剩余的数据。

        // 1.2 计算本次循环要拷贝的长度，根据缓冲区剩余大小量力而行进行拷贝。len是SDL本次回调要拷贝的总数据量，
        //      len1是本次循环拷贝的数据量，只是充当一个临时变量。
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)  // 例如len1 = 4096 > len = 3000
            len1 = len;

        // 2. 拷贝数据到stream中，并会根据audio_volume决定如何输出audio_buf。
        /* 2.1 判断是否为静音，以及当前音量的大小，如果音量为最大则直接拷贝数据；否则进行混音。 */
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
        else {
            /*
             * SDL_MixAudioFormat：进行混音。
             * 参1：混音输出目的地。
             * 参2：要进行混音的输入源。
             * 参3：表示所需的音频格式。
             * 参4：要混音的长度。
             * 参5：混音大小，范围[0,128]。
            */
            memset(stream, 0, len1);
            // 调整音量
            /* 如果处于mute状态则直接使用stream填0数据,因为上面memset为0了，暂停时is->audio_buf = NULL */
            if (!is->muted && is->audio_buf)
                SDL_MixAudioFormat(stream, (uint8_t *)is->audio_buf + is->audio_buf_index,
                                   AUDIO_S16SYS, len1, is->audio_volume);
        }

        // 3. 更新还能拷贝到SDL硬件缓冲区的大小len及其指针，以便进入下一次拷贝。
        //      不过只有当拷贝不足len时，才会进行第二次while，进行下一次的拷贝。
        len -= len1;
        stream += len1;

        /* 4. 更新is->audio_buf_index，指向audio_buf中未被拷贝到stream的数据（剩余数据）的起始位置 */
        is->audio_buf_index += len1;

    }// <== while (len > 0) end ==>

    // 来到这里，说明本次回调已经从audio_buf拷贝到足够的数据到stream中了。

    // 5. 设置时钟。
    // 保存audio_buf还没写入SDL缓存的大小
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /*
     * Let's assume the audio driver that is used by SDL has two periods.
     * 让我们假设SDL使用的音频驱动程序有两个周期。
     *
     * 1）疑问1：为什么乘以2 * is->audio_hw_buf_size？
     *      答：SDL有两个缓冲区，分为SDL内部缓冲区和SDL外部缓冲区，内部缓冲区是正在播放，而外部缓冲区就是回调的stream，它们是环形buffer，交替使用。
     *      所以需要乘以2.
     *
     * 2）疑问2：为什么在求剩余的字节所占的秒数时，还要加上2 * is->audio_hw_buf_size？
     *      答：这是因为，audio_write_buf_size只是audio_buf的剩余数据，我们还需要加上SDL的2个缓冲区的数据，首先加上SDL外部缓冲区比较容易理解，
     *      因为获取到的这次SDL回调数据虽然拷贝到stream，但是函数并未返回，实际并未开始播放，所以这些数据仍然是被认为剩余的数据。
     *      而加上SDL内部缓冲区，虽然该内部缓冲区不一定剩余audio_hw_buf_size，因为是正在播放嘛，但是因为我们的目的是想求正在播放的帧的起始pts(看下面的目的)，
     *      我们认为它剩余的字节也是audio_hw_buf_size。所以也需要加上它。
     *      故需要加上2 * is->audio_hw_buf_size。
     *      所以：2 * is->audio_hw_buf_size + is->audio_write_buf_size：就是代表目前所有已经缓存的音频数据大小。
     *
     * 3）疑问3：为什么在set_clock_at的参4传audio_callback_time / 1000000.0，而不是传实时的av_gettime_relative() / 1000000.0？(本疑问建议先看目的)
     *      答：这是因为，SDL回调到来时，由于上面的while(主要是audio_decode_frame)会有延迟，若不能准确调用set_clock_at设置pts_drift的值的话，
     *      会导致调用get_clock时获取到不正确的实时音频pts，因为while有延时，而你在get_clock时并未算延时造成的结果，
     *      所以最终导致音频为主时钟时，视频同步出现问题。，故不能传av_gettime_relative()。
     * 举个例子：
     *      假设audio_clock=80ms，SDL内部、外部缓存各占40ms，audio_buf没有剩余数据即0ms，那么求出SDL内部实际正在播放的pts=80-(40+40)=0ms。
     *      然后再假设刚进来回调时audio_callback_time=100ms，此时假设因while延时10ms，所以实时时间为110ms。
     *
     *      1. 那么按照正确的方法set_clock_at时，参数pts=80ms-80ms=0，time=100ms：求出pts_drift=0-100=-100。
     *          那么再get_clock时，c->pts_drift + time=-100+110=10ms，正常来说，因为while延迟，音频内部正在播放的时间应该是从0变成10ms，
     *          而得到的音频时钟也是10ms，所以这个是正确的。
     *
     *      2. 但是当你错误的传av_gettime_relative()调用set_clock_at时，参数pts=80ms-80ms=0，time=110ms：求出pts_drift=0-110=-110。
     *          那么再get_clock时，c->pts_drift + time=-110+110=0ms，正常来说，因为while延迟，音频内部正在播放的时间应该是从0变成10ms，
     *          而得到的音频时钟仍然是0ms，所以这个肯定是错误的。此时再去进行同步，就会出现问题。
     *          对比视频，因为视频没有while这种延迟，所以每次拿到帧能播放时，就会直接在video_refresh->update_video_pts(is, vp->pts, vp->pos, vp->serial)->set_clock中，
     *          直接传av_gettime_relative进行设置这个实时时间。
     *
     * 这样经过2和3，就能计算出准确的pts_drift，所以即使while有延迟，在get_clock时也能根据pts_drift重新获取到正确的SDL内部播放时间，从而进行同步。
     *
     * 理解上面3个疑问后，就知道目的是想干嘛了。
     * 4）目的：想通过下一帧的pts(audio_clock)减去目前已经缓存的音频总秒数，这样就得出SDL内部实际正在播放的pts(更具体是想设置正确的pts_drift)。以便进行视频的同步。
     *      而不能直接以audio_clock作为视频同步的pts。
     *      举个求出SDL内部实际正在播放的pts例子：
     *      假设audio_clock=200ms，audio_hw_buf_size=8192，audio_write_buf_size剩余2000，bytes_per_sec=176400，
     *      那么所有缓冲区的剩余秒数=(2*8192+2000)/176400=18384/176400=0.104s=104ms。
     *      那么SDL内部实际正在播放的pts就是：200ms - 104ms = 96ms。
     *      这个就是下面算法和设置时钟的目的。
     *
    */

    double tyycodeAllBufBytesSize = (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size)
            / is->audio_tgt.bytes_per_sec;// 求出所有缓冲区所占字节数。SDL两级buf + audio_buf剩余数据。
    double tyycodeSDLStartPts = is->audio_clock - tyycodeAllBufBytesSize;// 求出SDL内部缓冲区的起始pts。
    if (!isnan(is->audio_clock)) {
        set_clock_at(&is->audclk,
                     is->audio_clock -
                     (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec,
                     is->audio_clock_serial,
                     audio_callback_time / 1000000.0);// 设置音频时钟。 audio_callback_time / 1000000.0代表SDL内部缓冲区的起始pts的那一刻的实时时间。
                                                      // 因为本回调刚进来，说明SDL外部缓冲区开始补充数据，而SDL内部缓冲区刚好在播放数据。
        sync_clock_to_slave(&is->extclk, &is->audclk);// 根据从时钟判断是否需要调整主时钟
    }
}

/**
 * @brief 使用SDL打开音频硬件设备，获取对应的硬件参数，保存到FFmpeg类型的结构体进行传出，方便上层调用，例如进行重采样就是用到这个硬件的音频相关参数。
 *          并且会设置相应的回调函数。
 *
 * @param opaque 用户参数。这里是播放器实例。
 * @param wanted_channel_layout 传入参数，表示期望的通道布局，硬件支持就使用这个，
 *          不支持时，在传出的audio_hw_params的通道布局会被硬件支持的布局代替。
 * @param wanted_nb_channels 和通道布局一样的意思。
 * @param wanted_sample_rate 和通道布局、通道一样的意思。
 * @param audio_hw_params 传入传出参数，FFmpeg类型的结构体，最终保存着硬件设备支持的音频相关参数。
 *
 * @return 成功返回对应的 SDL内部缓存的数据字节，一般是1024. 失败返回-1.
 */
static int audio_open(void *opaque, int64_t wanted_channel_layout,
                      int wanted_nb_channels, int wanted_sample_rate,
                      struct AudioParams *audio_hw_params)
{
    SDL_AudioSpec wanted_spec, spec;// 期望的SDL参数，实际从硬件中获取到的SDL音频参数。
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

    // 1. 若环境变量有设置，优先从环境变量取得声道数和声道布局
    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }

    // 2. 如果通道布局是空或者通道布局与通道数不匹配，则按照默认的通道数进行获取通道布局。
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        //#define AV_CH_LAYOUT_STEREO_DOWNMIX    (AV_CH_STEREO_LEFT|AV_CH_STEREO_RIGHT)
        //#define AV_CH_STEREO_LEFT            0x20000000
        //#define AV_CH_STEREO_RIGHT           0x40000000
        // AV_CH_LAYOUT_STEREO_DOWNMIX=00100000 00000000 00000000 00000000 | 01000000 00000000 00000000 00000000 = 01100000 00000000 00000000 00000000
        // ~AV_CH_LAYOUT_STEREO_DOWNMIX = 10011111 11111111 11111111 11111111
        // 例如wanted_channel_layout=4时， 00000000 00000000 00000000 00000100 & 10011111 11111111 11111111 11111111 = 4。
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }

    // 3. 根据channel_layout获取nb_channels，当传入参数wanted_nb_channels不匹配时，此处会作修正。和2一样。
    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;

    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }

    // 4. 从采样率数组中找到第一个小于传入参数wanted_sample_rate的值
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;

    // 音频采样格式有两大类型：planar和packed，假设一个双声道音频文件，一个左声道采样点记作L，一个右声道采样点记作R，则：
    // planar存储格式：(plane1)LLLLLLLL...LLLL (plane2)RRRRRRRR...RRRR
    // packed存储格式：(plane1)LRLRLRLR...........................LRLR
    // 在这两种采样类型下，又细分多种采样格式，如AV_SAMPLE_FMT_S16、AV_SAMPLE_FMT_S16P等，
    // 注意SDL2.0目前不支持planar格式
    // channel_layout是int64_t类型，表示音频声道布局，每bit代表一个特定的声道，参考channel_layout.h中的定义，一目了然
    // 数据量(bits/秒) = 采样率(Hz) * 采样深度(bit) * 声道数。采样深度应该是指一个采样点所占的字节数，转换成bit单位即可。
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence =   0;

    /*
     * 5. 计算出样本中的音频缓冲区大小，即下面的wanted_spec.samples。
     * 理解音频缓存大小的一个重要公式推导：
     * 假如你希望1s装载 m 次 AudioBuffer(采样点个数)，那么1s装载的音频数据就是m x AudioBuffer。而音频播放1s需要多少数据？
     * 有个公式：1s数据大小 = 采样率 x 声道数 x 每个样本大小。
     * 所以AudioBuffer = （采样率 x 声道数 x 每个样本大小）/ m。
     * 由于为了适配各种音频输出，不会选择立体声，而选择交错模式s16进行输出，所以声道数(通道数)必定是1，每个样本大小占2字节。
     * 这里的m在ffplay是SDL_AUDIO_MAX_CALLBACKS_PER_SEC = 30.
     * 得出AudioBuffer = 采样率x1x2/m = 2x(采样率/m);
     * 但是不能直接这样带进去算，需要调用av_log2求出对应的幂次方数，最终确定每次采样点的最大个数。
     * 因为(采样率/m)就是平均每次的采样个数。
     * 所以最终公式应该是：AudioBuffer = 2^n。n由(采样率/m)求出。
     *
     * 也就得出下面的表达式：2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC);
     * 注意这里求出的AudioBuffer是采样点个数，在调用SDL_OpenAudioDevice后，会被转成字节单位保存在spec.size中。
     * 例如这里求出AudioBuffer=512采样点，假设s16，1个通道。那么spec.size=512x2x1=1024。
     * 再例如：AudioBuffer=2048采样点，假设s16，2个通道。那么spec.size=2048x2x2=8192。
     * spec.size与wanted_spec.samples的转换公式是：spec.size = samples * byte_per_sample * channels;
     *
     * 一次读取多长的数据
     * SDL_AUDIO_MAX_CALLBACKS_PER_SEC一秒最多回调次数，避免频繁的回调。
     *  Audio buffer size in samples (power of 2，即2的幂次方)。
     */
    // av_log2应该是对齐成2的n次方吧。例如freq=8000，每秒30次，最终返回8.具体可以看源码是如何处理的。
    // 大概估计是8k/30=266.6，2*2^7<266.7<2*2^8.因为缓存要大于实际的，所以返回8。
    // 44.1k/30=1470，2*2^9<1470<2*2^10.因为缓存要大于实际的，所以返回10。
    int tyycode1 = av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC);
    int tyycode2 = 2 << tyycode1;// 2左移8次方，即2*2的8次幂=512.多乘以一个2是因为它本身底数就是2了。
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;

    // 6. 打开音频设备并创建音频处理线程(实际是回调)。下面while如果打开音频失败，会一直尝试不同的通道、通道布局、帧率进行打开，直到帧率为0.
    // 期望的参数是wanted_spec，实际得到的硬件参数是spec。
    // 1) SDL提供两种使音频设备取得音频数据方法：
    //    a. push，SDL以特定的频率调用回调函数，在回调函数中取得音频数据
    //    b. pull，用户程序以特定的频率调用SDL_QueueAudio()，向音频设备提供数据。此种情况wanted_spec.callback=NULL
    // 2) 音频设备打开后播放静音，不启动回调，调用SDL_PauseAudio(0)后启动回调，开始正常播放音频
    /*
     * SDL_OpenAudioDevice()：
     * 参1：设备名字。最合理应该传NULL，为NULL时，等价于SDL_OpenAudio()。
     * 参2：一般传0即可。
     * 参3：期望的参数。
     * 参4：实际获取到的硬件参数。
     * 参5：一些权限宏的配置。
     * 返回值：0失败。>=2成功。因为1被旧版本的SDL_OpenAudio()占用了。
    */
    while (!(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        av_log(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());// 返回0，报警告，估计硬件不支持。
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {// 一直匹配不同的帧率通道，通道布局，直到帧率为0，返回-1。
                av_log(NULL, AV_LOG_ERROR, "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }

    // 7. 检查打开音频设备的实际参数：采样格式
    if (spec.format != AUDIO_S16SYS) {
        av_log(NULL, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }

    // 8. 检查打开音频设备的实际参数：声道数
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);// 使用实际的硬件通道数获取通道布局。
        if (!wanted_channel_layout) {
            av_log(NULL, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    // 9. 利用SDL获取到音频硬件的实际参数后，赋值给FFmpeg类型的结构，即指针形参audio_hw_params进行传出。
    // wanted_spec是期望的参数，spec是实际的参数，wanted_spec和spec都是SDL中的结构。
    // 此处audio_hw_params是FFmpeg中的参数，输出参数供上级函数使用
    // audio_hw_params保存的参数，就是在做重采样的时候要转成的格式。
    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels =  spec.channels;
    /* audio_hw_params->frame_size这里只是计算一个采样点占用的字节数 */
    audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
    /* audio_hw_params->bytes_per_sec则是计算1s占用的字节数，会配合返回值spec.size，
     * 用来求算阈值audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec */
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }

    // 返回音频缓冲区大小，单位是字节，对比上面的wanted_spec.samples，单位是采样点。最终转换成字节结果是一样的。
    return spec.size;	/* SDL内部缓存的数据字节, samples * channels *byte_per_sample，在SDL_OpenAudioDevice打开时被改变。 */
}

/* open a given stream. Return 0 if OK */
/**
 * @brief stream_component_open
 * @param is
 * @param stream_index 流索引
 * @return Return 0 if OK
 */
static int stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *ic = is->ic;           // 从播放器获取输入封装上下文
    AVCodecContext *avctx;
    AVCodec *codec;
    const char *forced_codec_name = NULL;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
    int stream_lowres = lowres;             // 用于输入的解码分辨率，但是最终由流的编码器最低支持的分辨率决定该值

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    /* 1. 为解码器分配一个编解码器上下文结构体 */
    avctx = avcodec_alloc_context3(NULL);
    if (!avctx)
        return AVERROR(ENOMEM);

    /* 2. 将对应音视频码流中的编解码器信息，拷贝到新分配的编解码器上下文结构体 */
    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;

    // 3. 设置pkt_timebase
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    /* 4. 根据codec_id查找解码器 */
    codec = avcodec_find_decoder(avctx->codec_id);

    /* 5. 保存流下标last_audio_stream、last_subtitle_stream、last_video_stream，用于记录，方便进行其它操作。例如切换国语/粤语 */
    switch(avctx->codec_type){
    case AVMEDIA_TYPE_AUDIO   : is->last_audio_stream    = stream_index;
        forced_codec_name =    audio_codec_name; break;
    case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index;
        forced_codec_name = subtitle_codec_name; break;
    case AVMEDIA_TYPE_VIDEO   : is->last_video_stream    = stream_index;
        forced_codec_name =    video_codec_name; break;
    }

    /* 6. 如果有名字，又根据名字去找解码器。 但是感觉没必要啊？ */
    if (forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if (!codec) {
        if (forced_codec_name) av_log(NULL, AV_LOG_WARNING,
                                      "No codec could be found with name '%s'\n", forced_codec_name);
        else                   av_log(NULL, AV_LOG_WARNING,
                                      "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id; // 这里应该是防止4的avctx->codec_id找不到解码器，而6找到的情况下，需要给avctx->codec_id重新赋值。
    /* 7. 给解码器的以哪种分辨率进行解码。 会进行检查用户输入的最大低分辨率是否被解码器支持 */
    if (stream_lowres > codec->max_lowres) {// codec->max_lowres: 解码器支持的最大低分辨率值
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n", codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;

    // 用户是否设置了加快解码速度
    if (fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;// 允许不符合规范的加速技巧。估计是加快解码速度

    /* 8. 设置相关选项，然后解码器与解码器上下文关联。 */
    opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
    if (!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    // av_opt_set_int(avctx, "refcounted_frames", 1, 0);等同于avctx->refcounted_frames = 1;
    // 设置为1的时候表示解码出来的frames引用永久有效，需要手动释放
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {// key为空并且参4是AV_DICT_IGNORE_SUFFIX：代表遍历所有的字典条目。
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret =  AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    is->eof = 0;                                           // 这里赋值为0的意义是什么？应该只需要在读线程处理吧？这个函数就是读线程调用的。
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;// 丢弃无用的数据包，如0大小的数据包在avi
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
    {
        // 这里是音频filter的一些处理。
        AVFilterContext *sink;

        // 1）保存音频过滤器src的相关参数。avctx的信息是从第二步的流参数拷贝过来的。
        is->audio_filter_src.freq           = avctx->sample_rate;
        is->audio_filter_src.channels       = avctx->channels;
        is->audio_filter_src.channel_layout = get_valid_channel_layout(avctx->channel_layout, avctx->channels);
        is->audio_filter_src.fmt            = avctx->sample_fmt;

        // 2）初始化配置音频过滤器。
        // 注意这里不强制输出音频格式。因为未调用audio_open，此时音频的输出格式audio_tgt内部值全是0，并未读取到对应的参数。
        // 但是输入的音频格式的知道的，所以可以先指定。
        // 在解码线程才是真正的配置。
        if ((ret = configure_audio_filters(is, afilters, 0)) < 0)
            goto fail;

        // 3）获取音频的输出过滤器及其音频相关信息(实际上是获取输入的相关信息)。上面配置之后，out_audio_filter就是输出的过滤器。
        // 注意下面的内容只有 avfilter_graph_config 提交了系统滤波器才能获取到这些参数。
        // 并且通过debug发现，输入过滤器的值和下面的值的一样的，并且因为输出过滤器out_audio_filter是没有这些参数值的，
        // 所以猜测此时获取到的内容应该是从输入过滤器中读取的(其实看else的内容也可以确定是从输入获取的)。想更确定的可以去看看源码，但没必要，知道就行。
        sink = is->out_audio_filter;
        sample_rate    = av_buffersink_get_sample_rate(sink);
        nb_channels    = av_buffersink_get_channels(sink);
        channel_layout = av_buffersink_get_channel_layout(sink);
    }
#else
        //从avctx(即AVCodecContext)中获取音频格式参数
        sample_rate    = avctx->sample_rate;
        nb_channels    = avctx->channels;
        channel_layout = avctx->channel_layout;
#endif

        /* prepare audio output 准备音频输出 */
        // 4）调用audio_open打开音频，获取对应的硬件参数，保存到FFmpeg类型的结构体进行传出(audio_tgt)，
        //      返回值表示输出设备的缓冲区大小，内部SDL会启动相应的回调函数
        if ((ret = audio_open(is, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
            goto fail;

        // 5）.1 初始化音频的缓存相关数据
        is->audio_hw_buf_size = ret;    // 保存SDL的音频缓存大小
        is->audio_src = is->audio_tgt;  // 暂且将数据源参数等同于目标输出参数，注audio_filter_src仍然保存着流中原本的信息，与audio_src不是同一变量。
        //初始化audio_buf相关参数
        is->audio_buf_size  = 0;
        is->audio_buf_index = 0;

        /* 5）.2 init averaging filter 初始化averaging滤镜, 下面3个变量只有在非audio master时使用，audio_diff_cum也是。 */
        /*
         * 1）exp(x):返回以e为底的x次方，即e^x。注意和对数不一样，对数公式有：y=log(a)(x)，a是低，那么有：x=a^y。和这个函数不一样，不要代进去混淆了。
         * 2）log(x)：以e为底的对数。这个才可以代入对数公式。例如log(10)=y，那么e^y=10，y≈2.30258509。其中e=2.718281828...
         *
         * 3）log(0.01)=y，那么e^y=0.01，y≈-4.60517018(计算器计出来即可)。根据对数的图，这个结果没问题。
         * 那么log(0.01) / AUDIO_DIFF_AVG_NB = -4.60517018 / 20 ≈ -0.2302585092。
         * 那么exp(-0.2302585092) ≈ e^(-0.2302585092)=0.7943282347242815。可以使用pow(a,n)去验证。
        */
        double tyycodeLog1 = log(0.01);                         // -4.60517018
        double tyycodeLog2 = tyycodeLog1 / AUDIO_DIFF_AVG_NB;   // -0.2302585092
        double tyycodeLog3 = exp(tyycodeLog2);                  // 0.7943282347242815
        double tyycodeVerity = pow(2.718281828, -0.2302585092); // 验证，结果是接近的，说明没错。
        is->audio_diff_avg_coef  = exp(log(0.01) / AUDIO_DIFF_AVG_NB); // 0.794。转成数学公式是：e^(ln(0.01)/20)
        is->audio_diff_avg_count = 0;
        /* 由于我们没有精确的音频数据填充FIFO,故只有在大于该阈值时才进行校正音频同步 */
        is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;// 例如1024/8000*1*2=1024/16000=0.064

        // 5）.3 初始化播放器的音频流及其下标
        is->audio_stream = stream_index;            // 获取audio的stream索引
        is->audio_st = ic->streams[stream_index];   // 获取audio的stream指

        // 5）.4初始化解码器队列
        decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread);    // 音视频、字幕的队满队空都是使用同一个条件变量去做的。

        /* 6）判断is->ic->iformat->flags是否有这3个宏其中之一，若只要有一个，就代表不允许这样操作。
         * 若无，则不会进行初始化。例如flags=64=01000000 & 11100000 00000000 = 0，没有这些宏代表允许这样操作。
         * 并且若read_seek是空的话，才会初始化start_pts、start_pts_tb。
         *
         * AVFMT_NOBINSEARCH：Format不允许通过read_timestamp返回二进制搜索。
         * AVFMT_NOGENSEARCH：格式不允许退回到通用搜索。
         * AVFMT_NO_BYTE_SEEK：格式不允许按字节查找。
         * AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK = 11100000 00000000.
         *
         * is->ic->iformat->read_seek：回调函数，用于在流组件stream_index中查找相对于帧的给定时间戳。
         *
         * 上面想表达的意思基本就是：如果不支持二进制、不支持通用、不支持字节查找，那么只能通过pts查找了。这个应该是如何操作seek的处理。
        */
        int tyycodeFl1 = is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK);
        int tyycodeFl2 = !is->ic->iformat->read_seek;// 一般read_seek不为空
        if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !is->ic->iformat->read_seek) {
            is->auddec.start_pts = is->audio_st->start_time;
            is->auddec.start_pts_tb = is->audio_st->time_base;
        }

        // 7）启动音频解码线程
        if ((ret = decoder_start(&is->auddec, audio_thread, "audio_decoder", is)) < 0)
            goto out;

        // 8）需要开始播放，才会有声音，注释掉它是不会有声音的。参2传0代表开始播放，非0表示暂停。
        //  see http://wiki.libsdl.org/SDL_PauseAudioDevice
        SDL_PauseAudioDevice(audio_dev, 0);
        break;

    case AVMEDIA_TYPE_VIDEO:
        is->video_stream = stream_index;            // 获取video的stream索引
        is->video_st = ic->streams[stream_index];   // 获取video的stream
        // 初始化ffplay封装的视频解码器
        decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread);
        // 启动视频频解码线程
        if ((ret = decoder_start(&is->viddec, video_thread, "video_decoder", is)) < 0)
            goto out;
        is->queue_attachments_req = 1; // 使能请求mp3、aac等音频文件的封面
        break;

    case AVMEDIA_TYPE_SUBTITLE: // 视频是类似逻辑处理
        is->subtitle_stream = stream_index;
        is->subtitle_st = ic->streams[stream_index];

        decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread);
        if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder", is)) < 0)
            goto out;
        break;
    default:
        break;
    }
    goto out;

fail:
    avcodec_free_context(&avctx);
out:
    av_dict_free(&opts);

    return ret;
}

/**
 * @brief 这里是设置给ffmpeg内部，当ffmpeg内部当执行耗时操作时（一般是在执行while或者for循环的数据读取时）
 *          就会调用该函数
 * @param ctx
 * @return 若直接退出阻塞则返回1，等待读取则返回0
 */
static int decode_interrupt_cb(void *ctx)
{
    static int64_t s_pre_time = 0;
    int64_t cur_time = av_gettime_relative() / 1000;
    //    printf("decode_interrupt_cb interval:%lldms\n", cur_time - s_pre_time);
    s_pre_time = cur_time;
    VideoState *is = (VideoState *)ctx;
    return is->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    // 下面这样判断只是大概判断的，因为此时解码线程是有可能上锁对包队列进行操作的，所以是不准确的，甚至认为是不安全的，ffplay可能为了更快吧。
    return stream_id < 0 || // 没有该流
            queue->abort_request || // 请求退出
            (st->disposition & AV_DISPOSITION_ATTACHED_PIC) || // 是ATTACHED_PIC
            queue->nb_packets > MIN_FRAMES // packet数>25
            && (!queue->duration ||     // 满足PacketQueue总时长为0
                av_q2d(st->time_base) * queue->duration > 1.0); //或总时长超过1s，实际上就是有25帧。
}

/**
 * @brief 判断是实时流还是文件。
 * @param s 输入的上下文。
 * @return 1 实时流；0 文件。
 */
static int is_realtime(AVFormatContext *s)
{
    // 1. 根据输入的复用器的名字判断是否是网络流，一般是这个条件满足，例如s->iformat->name = "rtsp"。
    if(   !strcmp(s->iformat->name, "rtp")
          || !strcmp(s->iformat->name, "rtsp")
          || !strcmp(s->iformat->name, "sdp")
          )
        return 1;

    // 2. 根据是否打开了网络io和url前缀是否是对应的网络流协议来判断
    if(s->pb && (   !strncmp(s->url, "rtp:", 4)
                    || !strncmp(s->url, "udp:", 4)
                    )
            )
        return 1;
    return 0;
}

/* this thread gets the stream from the disk or the network */
/*
 * 数据都由这里读取
 * 主要功能是做解复用，从码流中分离音视频packet，并插入缓存队列
 */
static int read_thread(void *arg)
{
    VideoState *is = arg;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];      // 记录对应的音视频流的下标，不为-1，表示需要播放出来。例如nb_streams=3，其中音频流index占0、1(表示粤语国语)，2是视频的index。
                                        // 那么st_index[AVMEDIA_TYPE_VIDEO]=2,st_index[AVMEDIA_TYPE_AUDIO]=0，表示同时播放视频和音频，且音频为粤语。
                                        // 也就是说，st_index中最多只会有一个视频流或者音频流或者字幕流，它们可以同时出现，但不会出现多个音频流或者视频流或者字幕流的情况。

    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    SDL_mutex *wait_mutex = SDL_CreateMutex();
    int scan_all_pmts_set = 0;
    int64_t pkt_ts;

    // 一、准备流程
    if (!wait_mutex) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    // 初始化为-1,如果一直为-1说明没相应steam
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;

    is->eof = 0;    // =1是表明数据读取完毕

    // 1. 创建上下文结构体，这个结构体是最上层的结构体，表示输入上下文
    ic = avformat_alloc_context();
    if (!ic) {
        av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    /* 2. 设置中断回调函数，如果出错或者退出，就根据目前程序设置的状态选择继续check或者直接退出 */
    /* 当执行耗时操作时（一般是在执行while或者for循环的数据读取时），会调用interrupt_callback.callback
     * 回调函数中返回1则代表ffmpeg结束耗时操作退出当前函数的调用
     * 回调函数中返回0则代表ffmpeg内部继续执行耗时操作，直到完成既定的任务(比如读取到既定的数据包)
     */
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;

    /*
     * 特定选项处理
     * scan_all_pmts 是 mpegts的一个选项，这里在没有设定该选项的时候，强制设为1。
     *
     * av_dict_get：从字典获取一个条目，参1是字典，里面保存着返回的条目，参2是key，
     *                  参3一般传NULL，表示获取第一个匹配的key的字典，不为空代表找到的key的前面的key必须也要匹配。
     *                  参4是宏，具体看注释即可。
     * av_dict_set：设置一个key，是否覆盖和参4有关。看函数注释即可，不难。
    */
    if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {// 如果没设置scan_all_pmts，则进行设置该key
        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);// 设置时不会覆盖现有的条目，即有该key时，不会进行设置。
        scan_all_pmts_set = 1;// 强制设为1
    }

    // tyycode. avformat_open_input执行前打印所有内容，执行完后format_opts会被avformat_open_input内部置空。
    t = NULL;
    while ((t = av_dict_get(format_opts, "", t, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_INFO, "key: %s, value: %s.\n", t->key, t->value);
    }

    /* 3.打开文件，主要是探测协议类型，如果是网络文件则创建网络链接等 */
    // 注意：-fflags nobuffer最终是通过format_opts在这里设置的，
    //  而format_opts中的-fflags nobuffer，是通过parse_options调用到回调函数opt_defalut从命令行参数获取并进行设置的。
    // nobuffer debug相关可参考：https://blog.51cto.com/fengyuzaitu/3028132.
    // 字典相关可参考：https://www.jianshu.com/p/89f2da631e16?utm_medium=timeline(包含avformat_open_input参4支持的参数)
    // 关于延时选项可参考：https://www.it1352.com/2040727.html
    err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
    if (err < 0) {
        print_error(is->filename, err);
        ret = -1;
        goto fail;
    }
    // 执行完avformat_open_input后，会被重新置为NULL
    if (scan_all_pmts_set)
        av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

    // key为空并且参4是AV_DICT_IGNORE_SUFFIX：代表返回该字典的第一个条目。即判定该字典是否为空。
    // 由于avformat_open_input会调用完后会将format_opts有效的条目置空，所以此时还有条目，代表该条目是不合法的。
    if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        av_log(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    // videoState的ic指向分配的ic
    is->ic = ic;

    // 默认genpts是0，不产生缺失的pts。
    if (genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;// 生成缺失的pts，即使它需要解析未来的帧

    /*
     * 该函数将导致全局端数据被注入到下一个包中的每个流，以及在任何后续的seek之后。
     * 看源码可以看到，该函数很简单，就是将s的所有AVStream的inject_global_side_data字段设置为1.
     * see https://blog.csdn.net/chngu40648/article/details/100653452?spm=1001.2014.3001.5501
    */
    av_format_inject_global_side_data(ic);

    if (find_stream_info) {
        // 调用该函数后，会开辟一个二级指针返回，用于avformat_find_stream_info
        AVDictionary **opts = setup_find_stream_info_opts(ic, codec_opts);
        int orig_nb_streams = ic->nb_streams;

        /*
         * 4. 探测媒体类型，可得到当前文件的封装格式，音视频编码参数等信息
         * 调用该函数后得多的参数信息会比只调用avformat_open_input更为详细，
         * 其本质上是去做了decdoe packet获取信息的工作
         * codecpar, filled by libavformat on stream creation or
         * in avformat_find_stream_info()
         */
        err = avformat_find_stream_info(ic, opts);

        // 依次对每个流的选项的键值对进行释放
        for (i = 0; i < orig_nb_streams; i++)
            av_dict_free(&opts[i]);// av_dict_free：释放所有分配给AVDictionary结构的内存和所有键和值

        // 回收本身，即setup_find_stream_info_opts的返回值。
        // 看他注释，使用av_freep，参数需要加上&，这样能确保释放后被置空，而av_free(buf)不会置空，会导致悬空指针(free掉未置空)的产生。
        av_freep(&opts);

        if (err < 0) {
            av_log(NULL, AV_LOG_WARNING,
                   "%s: could not find codec parameters\n", is->filename);
            ret = -1;
            goto fail;
        }
    }

    if (ic->pb)
        // 如果由于错误或eof而无法读取，则为True
        // FIXME hack, ffplay maybe should not use avio_feof() to test for the end
        ic->pb->eof_reached = 0;

    // 初始化seek_by_bytes，表示是否按照字节数来进行seek
    if (seek_by_bytes < 0) {
        // 0100000000001000000000000000(67,141,632) & 001000000000(0x0200=512) = 0
        int flag = ic->iformat->flags & AVFMT_TS_DISCONT;   // 格式允许时间戳不连续。注意，muxers总是需要有效的(单调的)时间戳
        int cmp = strcmp("ogg", ic->iformat->name);         // 例如ic->iformat->name = "mov,mp4,m4a,3gp,3g2,mj2"
        seek_by_bytes = !!(flag) && cmp;                    // 首次执行运算后，应该是0字节。
    }
    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;// 一帧最大时长，为啥是10.0或者3600.0，一般值是3600.

    // 获取左上角的主题，但不是这里设置左上角的主题
    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value, input_filename);

    /* if seeking requested, we execute it */
    /* 5. 检测是否指定播放起始时间。 若命令行设置了(-ss 00:00:30)，会在opt_seek设置start_time，设完后单位变为AV_TIME_BASE */
    if (start_time != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            // 为啥还要加上ic->start_time，感觉不加也可以，留个疑问。
            // 答：因为ic->start_time代表首帧的开始时间，一般是0.如果不是0，需要加上首帧的时间。代表首帧开始时间+你想要跳过的时间=知道播放器的起始时间。很简单。
            timestamp += ic->start_time;

        // seek的指定的位置开始播放
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                   is->filename, (double)timestamp / AV_TIME_BASE);
        }
    }

    /* 是否为实时流媒体 */
    is->realtime = is_realtime(ic);

    /*
     * 打印关于输入或输出格式的详细信息。
     * 例如持续时间，比特率，流，容器，程序，元数据，侧数据，编解码器和时基。
    */
    if (show_status)
        av_dump_format(ic, 0, is->filename, 0);

    // 6. 查找AVStream
    // 6.1 根据用户指定来查找流。 若用户命令传参进来，wanted_stream_spec会保存用户指定的流下标，st_index用于此时的记录想要播放的下标。
    // 指定流基本是为了适配文件，因为文件可以有多个同样的流。
    for (i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];                  // 测试2_audio.mp4时，i=0、i=1时st是音频流，所以i=0不一定都是视频流(这里i=2才是视频流)，不能写死，需要动态判断。
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;                    // 选择哪些数据包可以随意丢弃，而不需要解复用。 默认AVDISCARD_ALL代表开始没有流舍弃所有。
        if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1){
            // avformat_match_stream_specifier：判断当前st是否与用户的wanted_stream_spec[type]匹配，具体如何判断需要看源码。
            // 可以这样测试：-ast 1，i=0看到第一次虽然st->codecpar->codec_type也是音频流，但是并不匹配，i=1时才会匹配。
            if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
                st_index[type] = i; // st_index和和wanted_stream_spec的区别：
                                    //通过debug发现基本是一样的，wanted_stream_spec保存用户指定的流下标(存字符串)，st_index用于此时的记录想要播放的下标(存数值)
        }
    }
    // 检测用户是否有输错流的种类(种类不是指视频、音频。而是指音频下有哪些种类，例如国语，粤语)。
    // 例如音频只有0和1两路(粤语和国语)，但你输入了 -ast 2，实际下标只有0和1，并没2。
    for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n",
                   wanted_stream_spec[i], av_get_media_type_string(i));// 报错例子: "Stream specifier 2 does not match any audio stream"
            //            st_index[i] = INT_MAX;
            st_index[i] = -1;               // 报错，最好将对应的流的种类也置为-1，增强代码健壮性。
        }
    }

    /*
      *  int av_find_best_stream(AVFormatContext *ic,
                        enum AVMediaType type,          // 要选择的流类型
                        int wanted_stream_nb,           // 目标流索引，-1时会参考相关流。
                        int related_stream,             // 相关流(参考流)索引。
                        AVCodec **decoder_ret,
                        int flags);
    */

    // 6.2 利用av_find_best_stream选择流。
    // 文件实时流都可以走这个流程，并且对应实时流，大多数之后走这个流程，因为实时流基本不存在多个同样的流，所以无法走上面指定的流播放。
    // 估计内部和for(int i = 0; i < _ic->nb_streams; i++)遍历去找的做法类似
    if (!video_disable)                     // 没有禁用视频才进来
        st_index[AVMEDIA_TYPE_VIDEO] =
                av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                    st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);// 若参3不为-1，则按照用户的选择流，为-1则自动选择；视频的相关流直接置为-1。
    if (!audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] =
                av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                    st_index[AVMEDIA_TYPE_AUDIO],
                                    st_index[AVMEDIA_TYPE_VIDEO], // 如果目标流参3是-1或者指定越界，则会参考相关流(即视频流)进行返回，一般默认返回第一个流或者最大的流，但不一定。
                                    NULL, 0);
    if (!video_disable && !subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] =
                av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                    st_index[AVMEDIA_TYPE_SUBTITLE],
                                    (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                         st_index[AVMEDIA_TYPE_AUDIO] :
                                         st_index[AVMEDIA_TYPE_VIDEO]),// 字幕：如果目标流参3是-1，则会参考相关流(优先音频流，再考虑视频流)进行返回，一般默认返回第一个流或者最大的流，但不一定。
                                    NULL, 0);

    // 通过上面的第6点，就能找到了对应的想要的音视频各自单独的流下标。

    // 这里应该还是一个默认值SHOW_MODE_NONE
    is->show_mode = show_mode;

    // 7. 从待处理流中获取相关参数，设置显示窗口的宽度、高度及宽高比
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        // 根据流和帧宽高比猜测视频帧的像素宽高比（像素的宽高比，注意不是图像的）
        // 为啥要猜呢？因为帧宽高比由编解码器设置，但流宽高比由解复用器设置，因此这两者可能是不相等的
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);// 实时流一般起始是{0, 1}，文件是{1,1}，不过后面会在set_default_window_size里面被重设
        if (codecpar->width) {
            // 设置显示窗口的大小和宽高比
            set_default_window_size(codecpar->width, codecpar->height, sar);
        }
    }

    /* open the streams */
    /* 8. 打开视频、音频解码器。在此会打开相应解码器，并创建相应的解码线程。 */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {// 如果有音频流则打开音频流
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) { // 如果有视频流则打开视频流
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }
    if (is->show_mode == SHOW_MODE_NONE) {
        //选择怎么显示，如果视频打开成功，就显示视频画面，否则，显示音频对应的频谱图
        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;
    }

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) { // 如果有字幕流则打开字幕流
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
               is->filename);
        ret = -1;
        goto fail;
    }

    // buffer是否需要无穷大 并且 是实时流。
    if (infinite_buffer < 0 && is->realtime)
        infinite_buffer = 1;    // 如果是实时流

    /*
     * 二、For循环流程
    */
    for (;;) {
        // 1 检测是否退出
        if (is->abort_request)
            break;

        // 2 检测是否暂停/继续，更新last_paused，以及网络流的状态。
        if (is->paused != is->last_paused) {
            is->last_paused = is->paused;
            if (is->paused)
                is->read_pause_return = av_read_pause(ic); // 网络流的时候有用
            else
                av_read_play(ic);
        }

        // 暂停 并且是 (rtsp或者是mmsh协议，那么睡眠并continue到for)
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            // 等待10ms，避免立马尝试下一个Packet
            SDL_Delay(10);
            continue;
        }
#endif
        // 3 检测是否seek(读线程的快进快退seek是从这里开始的)。
        if (is->seek_req) { // 是否有seek请求
            int64_t seek_target = is->seek_pos; // 目标位置
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;
            // 前进seek seek_rel>0
            //seek_min    = seek_target - is->seek_rel + 2;
            //seek_max    = INT64_MAX;
            // 后退seek seek_rel<0
            //seek_min = INT64_MIN;
            //seek_max = seek_target + |seek_rel| -2;
            //seek_rel =0  鼠标直接seek
            //seek_min = INT64_MIN;
            //seek_max = INT64_MAX;

            /*
             * FIXME the +-2 is due to rounding being not done in the correct direction in generation
             *  of the seek_pos/seek_rel variables. 修复由于四舍五入，没有在seek_pos/seek_rel变量的正确方向上进行.
            */
            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->url);
            } else {
                /* seek的时候，要把原先的数据清空，并重启解码器，put flush_pkt的目的是告知解码线程需要reset decoder.
                 * 其中：
                 * 清空packet队列：音视频流、字幕流都在本read_frame读线程处理。
                 * 清空帧队列在：音频 sdl_audio_callback->audio_decode_frame->do while (af->serial != is->audioq.serial)处理。
                 *             视频 video_refresh->if (vp->serial != is->videoq.serial)。
                 * 重置解码器在：音频 audio_thread->decoder_decode_frame->if (pkt.data == flush_pkt.data)时处理。
                 *             视频 video_thread->get_video_frame->decoder_decode_frame->if (pkt.data == flush_pkt.data)时处理。
                 *              实际重置解码器时，音视频是一样的，最后都调用decoder_decode_frame。
                 */
                if (is->audio_stream >= 0) { // 如果有音频流
                    packet_queue_flush(&is->audioq);    // 清空packet队列数据
                    // 放入flush pkt, 用来开起新的一个播放序列, 解码器读取到flush_pkt会重置解码器以及清空帧队列。
                    packet_queue_put(&is->audioq, &flush_pkt);
                }
                if (is->subtitle_stream >= 0) { // 如果有字幕流
                    packet_queue_flush(&is->subtitleq); // 和上同理
                    packet_queue_put(&is->subtitleq, &flush_pkt);
                }
                if (is->video_stream >= 0) {    // 如果有视频流
                    packet_queue_flush(&is->videoq);    // 和上同理
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                    set_clock(&is->extclk, NAN, 0);
                } else {
                    set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);// 时间戳方式seek，seek_target / (double)AV_TIME_BASE代表seek目标位置的时间点，单位微秒。
                }
            }
            is->seek_req = 0;               // 这里看到，如果用户多次触发seek请求，实际只会处理一次。
            is->queue_attachments_req = 1;
            is-> eof = 0;                   // 细节。0未读取完毕，1完毕。
            if (is->paused)
                step_to_next_frame(is); // 暂停状态下需要播放seek后的第一帧。
        }

        // 4 检测video是否为attached_pic
        if (is->queue_attachments_req) {
            // attached_pic 附带的图片。比如说一些MP3，AAC音频文件附带的专辑封面，所以需要注意的是音频文件不一定只存在音频流本身
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                AVPacket copy = { 0 };
                if ((ret = av_packet_ref(&copy, &is->video_st->attached_pic)) < 0)
                    goto fail;
                packet_queue_put(&is->videoq, &copy);
                packet_queue_put_nullpacket(&is->videoq, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }

        // 5 检测队列是否已经有足够数据。
        // 下面只是粗略判断是否为满，因为此时解码线程可能从队列中读取包，而这里是没有上锁的；与解码线程的判断队列为空一样，也是粗略判断。
        // 也就是说，队列为满或者为空时，是可粗略判断的，但是队列放入取出绝对不能。
        /* 缓存队列有足够的包，不需要继续读取数据 */
        // 下面if条件：不满足缓存无穷大的包数据 && (音视频、字幕队列大于设定的15M || 音视频、字幕队列都有足够的包) 的情况下，才认为已经读取到了足够的数据包
        if (infinite_buffer < 1 &&      // 不满足缓存无穷大的包数据
                (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE        // 音视频、字幕队列大于设定的15M
                 || (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&  // 音视频、字幕队列都有足够的包，才认为已经读取到了足够的数据包
                     stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
                     stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {

            /* wait 10 ms */
            SDL_LockMutex(wait_mutex);
            // 如果没有唤醒则超时10ms退出，比如在seek操作时这里会被唤醒。
            // 重点：ffplay的packetqueue的锁+条件变量的设计原理：
            // 1）这里额外添加了一个局部变量锁wait_mutex + continue_read_thread，是为了让锁的粒度更小，让读线程可以SDL_CondWaitTimeout超时退出。
            // 2）因为我们平时都是使用一把锁+一个条件变量，这里使用两把锁+两个条件变量。具体看代码目录下的测试代码testWaitMutex.cpp。
            // 3）ffplay的packetqueue的锁+条件变量的设计就是测试代码中的" 3. 解决的方法xxx"的思路。
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
            SDL_UnlockMutex(wait_mutex);
            continue;

        }

        // 6 检测码流是否已经播放结束。只有非暂停 并且 音视频都播放完毕 才认为播放结束。
        // auddec.finished == is->audioq.serial会在对应的解码器线程标记，例如音频的audio_thread
        if (!is->paused // 非暂停
                && // 这里的执行是因为码流读取完毕后 插入空包所致
                (!is->audio_st // 没有音频流，那肯定就是认为播放完毕
                 || (is->auddec.finished == is->audioq.serial // 或者音频播放完毕(is->auddec.finished相等is->audioq.serial 并且 帧队列帧数为0)
                     && frame_queue_nb_remaining(&is->sampq) == 0))
                && (!is->video_st // 没有视频流，那肯定就是认为播放完毕
                    || (is->viddec.finished == is->videoq.serial // 或者视频播放完毕
                        && frame_queue_nb_remaining(&is->pictq) == 0)))
        {
            if (loop != 1 /* 是否循环播放 */ && (!loop || --loop)) {// 这个if条件留个疑问
                // stream_seek不是ffmpeg的函数，是ffplay封装的，每次seek的时候会调用
                stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
            } else if (autoexit) {  // b 是否自动退出
                ret = AVERROR_EOF;
                goto fail;
            }
        }

        // 7.读取媒体数据，得到的是音视频分离后、解码前的数据
        ret = av_read_frame(ic, pkt); // 调用不会释放pkt的数据，需要我们自己去释放packet的数据
        // 8 检测数据是否读取完毕
        if (ret < 0) {
            // avio_feof：当且仅当在文件末尾或读取时发生错误时返回非零。
            // 真正读到文件末尾 或者 文件读取错误 并且 文件还未标记读取完毕，此时认为数据读取完毕。
            // 在这里，avio_feof的作用是判断是否读取错误，因为读到文件末尾由AVERROR_EOF判断了。
            if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof)
            {
                // 插入空包说明码流数据读取完毕了，之前讲解码的时候说过刷空包是为了从解码器把所有帧都读出来
                if (is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, is->video_stream);// 这里看到插入空包时，可以不需要关心其返回值。
                if (is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                if (is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                is->eof = 1;        // 标记文件读取完毕
            }
            if (ic->pb && ic->pb->error){
                // 读取错误，读数据线程直接退出
                break;
            }

            // 这里应该是AVERROR(EAGAIN)，代表此时读不到数据，需要睡眠一下再读。
            SDL_LockMutex(wait_mutex);
            SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);// 为啥读线程满时用wait_mutex这把锁，但是解码线程好像没有这把锁？
            SDL_UnlockMutex(wait_mutex);
            continue;		// 继续循环
        } else {
            // 成功读取一个pkt，eof标记为0
            is->eof = 0;
        }

        // 9 检测是否在播放范围内
        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = ic->streams[pkt->stream_index]->start_time; // 获取流的起始时间(不是上面ic里面的起始时间)。debug发现流里面的start_time，文件或者实时流起始都不是0.
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;      // 获取packet的时间戳。为空则使用dts。单位是AVStream->time_base。
        // 这里的duration是在命令行时用来指定播放长度
        int tyytest = duration == AV_NOPTS_VALUE;                       // 没有设置的话，一直是true.
        pkt_in_play_range = duration == AV_NOPTS_VALUE ||
                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
                <= ((double)duration / 1000000);// 两次start_time是不一样的注意，留个疑问。不过没有设置duration就不需要理会||后面的内容。
                                                // 答：pkt_ts、stream_start_time单位都是AVStream->time_base，所以需要除以该单位转成秒单位，
                                                // ic->start_time单位是AV_TIME_BASE，同样需要除以一百万转成秒，再计算
                                                // pkt_ts - stream_start_time表示该流经过的时长，而再减去start_time，是因为start_time才是真正的起始，stream_start_time可能是在
                                                // start_time后一点才记录的，所以需要减去，不过一般start_time是AV_NOPTS_VALUE相当于0.所以该流已经播放的时长按照
                                                // pkt_ts - stream_start_time计算也是没有太大问题的。

        // 10 将音视频数据分别送入相应的queue中
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {// 是视频流&&在播放范围&&不是音频的封面
            //printf("pkt pts:%ld, dts:%ld\n", pkt->pts, pkt->dts);
            packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_packet_unref(pkt);// // 不入队列则直接释放数据
        }

    }// <== for(;;) end ==>

    // 三 退出线程处理
    ret = 0;
fail:
    if (ic && !is->ic)
        avformat_close_input(&ic);

    if (ret != 0) {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    SDL_DestroyMutex(wait_mutex);
    return 0;
}

/**
 * @brief
 * @param filename 从命令行获取的输入文件名。
 * @param iformat 输入封装格式结构体。
 * @return
 */
static VideoState *stream_open(const char *filename, AVInputFormat *iformat)
{
    VideoState *is;

    // 1. 初始化播放器本身。带z(zero)的都会将这个结构体置0，相当于调用了memset.
    /* 1.1 分配结构体本身 */
    is = av_mallocz(sizeof(VideoState));
    if (!is)
        return NULL;

    /* 1.2 下面都是初始化播放器内部相关内容 */
    is->filename = av_strdup(filename);     // 为is->filename开辟内存并赋值
    if (!is->filename)
        goto fail;

    is->iformat = iformat;
    is->ytop    = 0;
    is->xleft   = 0;

    /* 初始化视频帧、音频帧、字幕帧队列 */
    // 注意帧视频音频队列的keep_last是传1的，字幕是0.
    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
        goto fail;
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    /* 初始化视频包、音频包、字幕包队列 */
    if (packet_queue_init(&is->videoq) < 0 ||
            packet_queue_init(&is->audioq) < 0 ||
            packet_queue_init(&is->subtitleq) < 0)
        goto fail;

    /* 创建读线程的条件变量 */
    if (!(is->continue_read_thread = SDL_CreateCond())) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        goto fail;
    }

    /*
     * 初始化时钟
     * 时钟序列->queue_serial，实际上指向的是包队列里面的serial，例如is->videoq.serial，默认初始化时是0。
     */
    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);
    is->audio_clock_serial = -1;

    /* 初始化音量 */
    if (startup_volume < 0)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
    if (startup_volume > 100)
        av_log(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);

    /* 首先确保音量是合法的。
     * av_clip宏作用：判断参1是否超过[参2，参3]的范围，若小于最小值则取参2；大于最大值则取参3.例如-1，则是0,101则是100.
    */
    startup_volume = av_clip(startup_volume, 0, 100);

    /*
     * 转成[0,128]范围的音量。
     *
     * 这需要两个音频缓冲区的播放音频格式和混合它们，执行添加，音量调整，和溢出剪辑。
     * 音频的取值范围为0 ~ 128，取值为::SDL_MIX_MAXVOLUME，为全音频音量。
     * 注意，这不会改变硬件容量。
     * 这是为了方便——你可以混合自己的音频数据。
     *
     * startup_volume范围是[0,100]，所以乘以128后再除以100，范围可以变成[0,128]
    */
    startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    is->audio_volume = startup_volume;
    is->muted = 0;                          // =1静音，=0则正常
    is->av_sync_type = av_sync_type;        // 音视频同步类型，默认音频同步！！！

    /* 创建读线程 */
    is->read_tid     = SDL_CreateThread(read_thread, "read_thread", is);
    if (!is->read_tid) {
        av_log(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
fail:
        stream_close(is);
        return NULL;
    }

    return is;
}

static void stream_cycle_channel(VideoState *is, int codec_type)
{
    AVFormatContext *ic = is->ic;
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = NULL;
    int nb_streams = is->ic->nb_streams;

    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        start_index = is->last_video_stream;
        old_index = is->video_stream;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = is->last_audio_stream;
        old_index = is->audio_stream;
    } else {
        start_index = is->last_subtitle_stream;
        old_index = is->subtitle_stream;
    }
    stream_index = start_index;

    if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1) {
        p = av_find_program_from_stream(ic, NULL, is->video_stream);
        if (p) {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++)
                if (p->stream_index[start_index] == stream_index)
                    break;
            if (start_index == nb_streams)
                start_index = -1;
            stream_index = start_index;
        }
    }

    for (;;) {
        if (++stream_index >= nb_streams)
        {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE)
            {
                stream_index = -1;
                is->last_subtitle_stream = -1;
                goto the_end;
            }
            if (start_index == -1)
                return;
            stream_index = 0;
        }
        if (stream_index == start_index)
            return;
        st = is->ic->streams[p ? p->stream_index[stream_index] : stream_index];
        if (st->codecpar->codec_type == codec_type) {
            /* check that parameters are OK */
            switch (codec_type) {
            case AVMEDIA_TYPE_AUDIO:
                if (st->codecpar->sample_rate != 0 &&
                        st->codecpar->channels != 0)
                    goto the_end;
                break;
            case AVMEDIA_TYPE_VIDEO:
            case AVMEDIA_TYPE_SUBTITLE:
                goto the_end;
            default:
                break;
            }
        }
    }
the_end:
    if (p && stream_index != -1)
        stream_index = p->stream_index[stream_index];
    av_log(NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
           av_get_media_type_string(codec_type),
           old_index,
           stream_index);

    stream_component_close(is, old_index);
    stream_component_open(is, stream_index);
}


static void toggle_full_screen(VideoState *is)
{
    is_full_screen = !is_full_screen;
    SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void toggle_audio_display(VideoState *is)
{
    int next = is->show_mode;
    do {
        next = (next + 1) % SHOW_MODE_NB;
    } while (next != is->show_mode && (next == SHOW_MODE_VIDEO && !is->video_st || next != SHOW_MODE_VIDEO && !is->audio_st));
    if (is->show_mode != next) {
        is->force_refresh = 1;
        is->show_mode = next;
    }
}

static void refresh_loop_wait_event(VideoState *is, SDL_Event *event) {
    double remaining_time = 0.0; /* 休眠等待，remaining_time的计算在video_refresh中 */

    /* 调用SDL_PeepEvents前先调用SDL_PumpEvents，将输入设备的事件抽到事件队列中 */
    SDL_PumpEvents();

    /*
     * SDL_PeepEvents check是否有事件，比如鼠标移入显示区等，有就
     * 从事件队列中拿一个事件，放到event中，如果没有事件，则进入循环中
     * SDL_PeekEvents用于读取事件，在调用该函数之前，必须调用SDL_PumpEvents搜集键盘等事件
     */
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
            /*
             * SDL_ShowCursor：切换光标是否显示。
             * 参数：1显示游标，0隐藏游标，-1查询当前游标状态。
             * 如果光标显示，返回1;如果光标隐藏，返回0。
            */
            SDL_ShowCursor(0);
            cursor_hidden = 1;
        }

        /*
         * remaining_time就是用来进行音视频同步的。
         * 在video_refresh函数中，根据当前帧显示时刻(display time)和实际时刻(actual time)
         * 计算需要sleep的时间，保证帧按时显示
         */
        if (remaining_time > 0.0) {//sleep控制画面输出的时机
            av_usleep((int64_t)(remaining_time * 1000000.0)); // remaining_time <= REFRESH_RATE
        }
        remaining_time = REFRESH_RATE;
        if (is->show_mode != SHOW_MODE_NONE &&  // 显示模式不等于SHOW_MODE_NONE
                (!is->paused                    // 非暂停状态
                 || is->force_refresh)          // 强制刷新状态
           ) {
            // 只有符合上面的3个条件才更新视频
            video_refresh(is, &remaining_time);
        }

        /* 从输入设备中搜集事件，推动这些事件进入事件队列，更新事件队列的状态，
         * 不过它还有一个作用是进行视频子系统的设备状态更新，如果不调用这个函数，
         * 所显示的视频会在大约10秒后丢失色彩。没有调用SDL_PumpEvents，将不会
         * 有任何的输入设备事件进入队列，这种情况下，SDL就无法响应任何的键盘等硬件输入。
        */
        SDL_PumpEvents();
    }
}

static void seek_chapter(VideoState *is, int incr)
{
    int64_t pos = get_master_clock(is) * AV_TIME_BASE;
    int i;

    if (!is->ic->nb_chapters)
        return;

    /* find the current chapter */
    for (i = 0; i < is->ic->nb_chapters; i++) {
        AVChapter *ch = is->ic->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);
    if (i >= is->ic->nb_chapters)
        return;

    av_log(NULL, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    stream_seek(is, av_rescale_q(is->ic->chapters[i]->start, is->ic->chapters[i]->time_base,
                                 AV_TIME_BASE_Q), 0, 0);
}

/* handle an event sent by the GUI */
static void event_loop(VideoState *cur_stream)
{
    SDL_Event event;
    double incr, pos, frac;

    int tyytestMoveNum = 0;
    for (;;) {
        double x;
        refresh_loop_wait_event(cur_stream, &event); //video是在这里显示的
#if 1
        //printf("type: %d\n", event.type);
        switch (event.type) {
        case SDL_KEYDOWN:	/* 键盘事件 */
            /*
             * keysym记录了按键的信息，其结构为：
                typedef struct SDL_Keysym
                {
                    SDL_Scancode scancode;  // 键盘硬件产生的扫描码
                    SDL_Keycode sym;        // SDL所定义的虚拟码
                    Uint16 mod;             // 修饰键
                    Uint32 unused;          // 未使用，可能有些版本是按键的Unicode码
                } SDL_Keysym;
            */
            // 如果用户设置了退出或者键盘按下Esc、q键，那么程序直接退出。
            if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                do_exit(cur_stream);
                break;
            }
            if (!cur_stream->width)
                continue;

            // 根据SDL的虚拟码进行处理键盘事件
            switch (event.key.keysym.sym) {
            case SDLK_f:
                toggle_full_screen(cur_stream);
                cur_stream->force_refresh = 1;
                break;
            case SDLK_p:
            case SDLK_SPACE: // 1. 按空格键触发暂停/恢复
                toggle_pause(cur_stream);
                break;
            case SDLK_m:    // 3. 静音
                toggle_mute(cur_stream);
                break;
            case SDLK_KP_MULTIPLY:
            case SDLK_0:    // 3. 增大音量
                update_volume(cur_stream, 1, SDL_VOLUME_STEP);
                break;
            case SDLK_KP_DIVIDE:
            case SDLK_9:    // 3. 减少音量
                update_volume(cur_stream, -1, SDL_VOLUME_STEP);
                break;
            case SDLK_s: // 2. Step to next frame
                step_to_next_frame(cur_stream);
                break;
            case SDLK_a:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                break;
            case SDLK_v:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                break;
            case SDLK_c:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_t:
                stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                break;
            case SDLK_w:
#if CONFIG_AVFILTER
                if (cur_stream->show_mode == SHOW_MODE_VIDEO && cur_stream->vfilter_idx < nb_vfilters - 1) {
                    if (++cur_stream->vfilter_idx >= nb_vfilters)
                        cur_stream->vfilter_idx = 0;
                } else {
                    cur_stream->vfilter_idx = 0;
                    toggle_audio_display(cur_stream);
                }
#else
                toggle_audio_display(cur_stream);
#endif
                break;
            case SDLK_PAGEUP:
                if (cur_stream->ic->nb_chapters <= 1) {
                    incr = 600.0;
                    goto do_seek;
                }
                seek_chapter(cur_stream, 1);
                break;
            case SDLK_PAGEDOWN:
                if (cur_stream->ic->nb_chapters <= 1) {
                    incr = -600.0;
                    goto do_seek;
                }
                seek_chapter(cur_stream, -1);
                break;
            case SDLK_LEFT:             // 4. 快进快退seek
                incr = seek_interval ? -seek_interval : -10.0;
                goto do_seek;
            case SDLK_RIGHT:
                incr = seek_interval ? seek_interval : 10.0;
                goto do_seek;
            case SDLK_UP:
                incr = 60.0;
                goto do_seek;
            case SDLK_DOWN:
                incr = -60.0;
do_seek:
                if (seek_by_bytes) {
                    pos = -1;
                    if (pos < 0 && cur_stream->video_stream >= 0)
                        pos = frame_queue_last_pos(&cur_stream->pictq);
                    if (pos < 0 && cur_stream->audio_stream >= 0)
                        pos = frame_queue_last_pos(&cur_stream->sampq);
                    if (pos < 0)
                        pos = avio_tell(cur_stream->ic->pb);
                    if (cur_stream->ic->bit_rate)
                        incr *= cur_stream->ic->bit_rate / 8.0;// cur_stream->ic->bit_rate / 8.0代表每秒的字节数，那么想要快进10s或者后退10s，只需要乘以它即可得到字节增量。
                    else
                        incr *= 180000.0;// 码率不存在默认按照每秒180k/bytes计算。
                    pos += incr;
                    stream_seek(cur_stream, pos, incr, 1);// 此时pos代表目标的字节位置，单位是字节。
                } else {
                    pos = get_master_clock(cur_stream);
                    if (isnan(pos))
                        pos = (double)cur_stream->seek_pos / AV_TIME_BASE;
                    pos += incr;    // 现在是秒的单位。start_time是首帧开始时间
                    if (cur_stream->ic->start_time != AV_NOPTS_VALUE && pos < cur_stream->ic->start_time / (double)AV_TIME_BASE)
                        pos = cur_stream->ic->start_time / (double)AV_TIME_BASE;// 如果后退的时间小于第一帧，则pos为第一帧的时间。
                    stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);// 此时pos代表目标的时间戳位置，单位是秒。
                }
                break;
            default:
                break;
            }// <== switch (event.key.keysym.sym) end ==>

            break;
        case SDL_MOUSEBUTTONDOWN:			/* 5. 鼠标按下事件 里面的双击左键 */
            if (exit_on_mousedown) {
                do_exit(cur_stream);
                break;
            }
            if (event.button.button == SDL_BUTTON_LEFT) {
                static int64_t last_mouse_left_click = 0;
                if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                    //连续鼠标左键点击2次显示窗口间隔小于0.5秒，则进行全屏或者恢复原始窗口
                    toggle_full_screen(cur_stream);
                    cur_stream->force_refresh = 1;
                    last_mouse_left_click = 0;
                } else {
                    last_mouse_left_click = av_gettime_relative();
                }
            }
            // 这里注意，鼠标按下时间同样会触发鼠标移动，因为没有break。
        case SDL_MOUSEMOTION:		/* 4. 快进快退seek 鼠标移动事件 */
            if (cursor_hidden) {
                SDL_ShowCursor(1);
                cursor_hidden = 0;
            }
            cursor_last_shown = av_gettime_relative();
            //printf("tyytestMoveNum: %d, type: %d\n", tyytestMoveNum++, event.type);
            //int va1 = SDL_MOUSEMOTION;      // 1024
            //int va2 = SDL_MOUSEBUTTONDOWN;  // 1025
            if (event.type == SDL_MOUSEBUTTONDOWN) {// 留个疑问，为什么最外层event.type进入SDL_MOUSEMOTION后，这里还能进入SDL_MOUSEBUTTONDOWN。
                                                    // 答：这是因为进入SDL_MOUSEMOTION，ffplay是依赖SDL_MOUSEBUTTONDOWN没有break进入的，
                                                    // 所以进入SDL_MOUSEMOTION后，这里还能进入SDL_MOUSEBUTTONDOWN。
                if (event.button.button == SDL_BUTTON_LEFT){
                    printf("tyyLEFT, x: %d\n", event.button.x);
                }
                if (event.button.button == SDL_BUTTON_RIGHT){
                    printf("tyyRIGHT, x: %d\n", event.button.x);
                }
                if (event.button.button == SDL_BUTTON_MIDDLE){
                    printf("tyyMIDDLE, x: %d\n", event.button.x);
                }

                if (event.button.button != SDL_BUTTON_RIGHT)
                    break;
                x = event.button.x;// 鼠标单击右键按下，该x坐标相对于正在播放的窗口，而不是电脑屏幕。
            } else {
                if (!(event.motion.state & SDL_BUTTON_RMASK))// 不存在SDL_BUTTON_RMASK=4，则直接break.SDL_BUTTON_RMASK代表什么事件暂未研究
                    break;// 一般单纯在正在播放的窗口触发移动事件，会从这里break。
                x = event.motion.x;
            }
            if (seek_by_bytes || cur_stream->ic->duration <= 0) {
                uint64_t size =  avio_size(cur_stream->ic->pb); // 整个文件的字节
                stream_seek(cur_stream, size*x/cur_stream->width, 0, 1);// 和时间戳的求法一样，参考时间戳。
            } else {
                int64_t ts;
                int ns, hh, mm, ss;
                int tns, thh, tmm, tss;
                tns  = cur_stream->ic->duration / 1000000LL;// 将视频总时长单位转成秒
                thh  = tns / 3600;                          // 获取总时长的小时的位数
                tmm  = (tns % 3600) / 60;                   // 获取总时长的分钟的位数
                tss  = (tns % 60);                          // 获取总时长秒的位数
                // 根据宽度作为x坐标轴，并划分总时长total后，那么x轴上的某一点a的时间点为：t=a/width*total.
                // 例如简单举个例子，宽度为1920，总时长也是1920，那么每个点刚好占1s，假设a的坐标为1000，那么t就是1000s。即t=1000/1920*1920.
                frac = x / cur_stream->width;               // 求出这一点在x轴的占比。实际我们也可以求 这一点在总时长的占比 来计算。原理都是一样的。
                ns   = frac * tns;                          // 求出播放到目标位置的时间戳，单位是秒。
                hh   = ns / 3600;                           // 然后依次获取对应的时、分、秒的位数。
                mm   = (ns % 3600) / 60;
                ss   = (ns % 60);
                av_log(NULL, AV_LOG_INFO,
                       "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac*100,
                       hh, mm, ss, thh, tmm, tss);
                ts = frac * cur_stream->ic->duration;
                if (cur_stream->ic->start_time != AV_NOPTS_VALUE)// 细节
                    ts += cur_stream->ic->start_time;
                stream_seek(cur_stream, ts, 0, 0);// rel增量是0时，代表鼠标事件的seek。
            }
            break;
        case SDL_WINDOWEVENT:		/* 窗口事件 */
            switch (event.window.event) {
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                screen_width  = cur_stream->width  = event.window.data1;
                screen_height = cur_stream->height = event.window.data2;
                if (cur_stream->vis_texture) {
                    SDL_DestroyTexture(cur_stream->vis_texture);
                    cur_stream->vis_texture = NULL;
                }
            case SDL_WINDOWEVENT_EXPOSED:
                cur_stream->force_refresh = 1;
            }
            break;
        case SDL_QUIT:
        case FF_QUIT_EVENT:	/* ffplay自定义事件,用于主动退出 */
            do_exit(cur_stream);
            break;
        default:
            break;

        }//<== switch (event.type) end ==>

#endif
    }

}

static int opt_frame_size(void *optctx, const char *opt, const char *arg)
{
    av_log(NULL, AV_LOG_WARNING, "Option -s is deprecated, use -video_size.\n");
    return opt_default(NULL, "video_size", arg);
}

static int opt_width(void *optctx, const char *opt, const char *arg)
{
    screen_width = parse_number_or_die(opt, arg, OPT_INT64, 1, INT_MAX);
    return 0;
}

static int opt_height(void *optctx, const char *opt, const char *arg)
{
    screen_height = parse_number_or_die(opt, arg, OPT_INT64, 1, INT_MAX);
    return 0;
}

static int opt_format(void *optctx, const char *opt, const char *arg)
{
    file_iformat = av_find_input_format(arg);
    if (!file_iformat) {
        av_log(NULL, AV_LOG_FATAL, "Unknown input format: %s\n", arg);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int opt_frame_pix_fmt(void *optctx, const char *opt, const char *arg)
{
    av_log(NULL, AV_LOG_WARNING, "Option -pix_fmt is deprecated, use -pixel_format.\n");
    return opt_default(NULL, "pixel_format", arg);
}

static int opt_sync(void *optctx, const char *opt, const char *arg)
{
    if (!strcmp(arg, "audio"))
        av_sync_type = AV_SYNC_AUDIO_MASTER;
    else if (!strcmp(arg, "video"))
        av_sync_type = AV_SYNC_VIDEO_MASTER;
    else if (!strcmp(arg, "ext"))
        av_sync_type = AV_SYNC_EXTERNAL_CLOCK;
    else {
        av_log(NULL, AV_LOG_ERROR, "Unknown value for %s: %s\n", opt, arg);
        exit(1);
    }
    return 0;
}

/**
 * @brief 将传进来的时间格式转成微秒，保存到全局变量start_time中。
 * @param optctx 一般是空。
 * @param opt key，例如调整开始时间时的"ss"选项。
 * @param arg key的value，时间格式是hh:mm:ss，例如00:00:30，调用函数后最终被转成微秒单位，例如3000000.
 * @return 0，no mean。
*/
static int opt_seek(void *optctx, const char *opt, const char *arg)
{
    start_time = parse_time_or_die(opt, arg, 1);
    return 0;
}

static int opt_duration(void *optctx, const char *opt, const char *arg)
{
    duration = parse_time_or_die(opt, arg, 1);
    return 0;
}

static int opt_show_mode(void *optctx, const char *opt, const char *arg)
{
    show_mode = !strcmp(arg, "video") ? SHOW_MODE_VIDEO :
                                        !strcmp(arg, "waves") ? SHOW_MODE_WAVES :
                                                                !strcmp(arg, "rdft" ) ? SHOW_MODE_RDFT  :
                                                                                        parse_number_or_die(opt, arg, OPT_INT, 0, SHOW_MODE_NB-1);
    return 0;
}

/**
 * @brief 将从命令行拿到的输入文件名赋值给静态变量input_filename。
 * @param optctx 一般是NULL。
 * @param filename 命令行获取的输入文件名。
 * @return void。
 */
static void opt_input_file(void *optctx, const char *filename)
{
    if (input_filename) {
        av_log(NULL, AV_LOG_FATAL,
               "Argument '%s' provided as input filename, but '%s' was already specified.\n",
               filename, input_filename);
        exit(1);
    }
    if (!strcmp(filename, "-"))
        filename = "pipe:";
    input_filename = filename;
}

static int opt_codec(void *optctx, const char *opt, const char *arg)
{
    const char *spec = strchr(opt, ':');
    if (!spec) {
        av_log(NULL, AV_LOG_ERROR,
               "No media specifier was specified in '%s' in option '%s'\n",
               arg, opt);
        return AVERROR(EINVAL);
    }
    spec++;
    switch (spec[0]) {
    case 'a' :    audio_codec_name = arg; break;
    case 's' : subtitle_codec_name = arg; break;
    case 'v' :    video_codec_name = arg; break;
    default:
        av_log(NULL, AV_LOG_ERROR,
               "Invalid media specifier '%s' in option '%s'\n", spec, opt);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int dummy;


/*
下面是OptionDef类型的选项数组。
// CMDUTILS_COMMON_OPTIONS共0-28=29个命令选项。从x开始到NULL是29-77=49个，49个中是包含了NULL的。
typedef struct OptionDef {
    const char *name;
    int flags;
     union {
        void *dst_ptr;
        int (*func_arg)(void *, const char *, const char *);
        size_t off;
    } u;
    const char *help;
    const char *argname;
} OptionDef;
注：
共用体赋值时，func_arg前面加一个点是C语言特有的语法，C++的严格语法不支持。
若在.cpp的文件测试时，需要 加上#ifdef __cplusplus extern "C" 这些内容，否则语法不通过。
*/
static const OptionDef options[] = {
    CMDUTILS_COMMON_OPTIONS
    { "x", HAS_ARG, { .func_arg = opt_width }, "force displayed width", "width" },
    { "y", HAS_ARG, { .func_arg = opt_height }, "force displayed height", "height" },
    { "s", HAS_ARG | OPT_VIDEO, { .func_arg = opt_frame_size }, "set frame size (WxH or abbreviation)", "size" },
    { "fs", OPT_BOOL, { &is_full_screen }, "force full screen" },
    { "an", OPT_BOOL, { &audio_disable }, "disable audio" },
    { "vn", OPT_BOOL, { &video_disable }, "disable video" },
    { "sn", OPT_BOOL, { &subtitle_disable }, "disable subtitling" },
    { "ast", OPT_STRING | HAS_ARG | OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_AUDIO] }, "select desired audio stream", "stream_specifier" },
    { "vst", OPT_STRING | HAS_ARG | OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_VIDEO] }, "select desired video stream", "stream_specifier" },
    { "sst", OPT_STRING | HAS_ARG | OPT_EXPERT, { &wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE] }, "select desired subtitle stream", "stream_specifier" },
    { "ss", HAS_ARG, { .func_arg = opt_seek }, "seek to a given position in seconds", "pos" },
    { "t", HAS_ARG, { .func_arg = opt_duration }, "play  \"duration\" seconds of audio/video", "duration" },
    { "bytes", OPT_INT | HAS_ARG, { &seek_by_bytes }, "seek by bytes 0=off 1=on -1=auto", "val" },
    { "seek_interval", OPT_FLOAT | HAS_ARG, { &seek_interval }, "set seek interval for left/right keys, in seconds", "seconds" },
    { "nodisp", OPT_BOOL, { &display_disable }, "disable graphical display" },
    { "noborder", OPT_BOOL, { &borderless }, "borderless window" },
    { "alwaysontop", OPT_BOOL, { &alwaysontop }, "window always on top" },
    { "volume", OPT_INT | HAS_ARG, { &startup_volume}, "set startup volume 0=min 100=max", "volume" },
    { "f", HAS_ARG, { .func_arg = opt_format }, "force format", "fmt" },
    { "pix_fmt", HAS_ARG | OPT_EXPERT | OPT_VIDEO, { .func_arg = opt_frame_pix_fmt }, "set pixel format", "format" },
    { "stats", OPT_BOOL | OPT_EXPERT, { &show_status }, "show status", "" },
    { "fast", OPT_BOOL | OPT_EXPERT, { &fast }, "non spec compliant optimizations", "" },
    { "genpts", OPT_BOOL | OPT_EXPERT, { &genpts }, "generate pts", "" },
    { "drp", OPT_INT | HAS_ARG | OPT_EXPERT, { &decoder_reorder_pts }, "let decoder reorder pts 0=off 1=on -1=auto", ""},
    { "lowres", OPT_INT | HAS_ARG | OPT_EXPERT, { &lowres }, "", "" },
    { "sync", HAS_ARG | OPT_EXPERT, { .func_arg = opt_sync }, "set audio-video sync. type (type=audio/video/ext)", "type" },
    { "autoexit", OPT_BOOL | OPT_EXPERT, { &autoexit }, "exit at the end", "" },
    { "exitonkeydown", OPT_BOOL | OPT_EXPERT, { &exit_on_keydown }, "exit on key down", "" },
    { "exitonmousedown", OPT_BOOL | OPT_EXPERT, { &exit_on_mousedown }, "exit on mouse down", "" },
    { "loop", OPT_INT | HAS_ARG | OPT_EXPERT, { &loop }, "set number of times the playback shall be looped", "loop count" },
    { "framedrop", OPT_BOOL | OPT_EXPERT, { &framedrop }, "drop frames when cpu is too slow", "" },
    { "infbuf", OPT_BOOL | OPT_EXPERT, { &infinite_buffer }, "don't limit the input buffer size (useful with realtime streams)", "" },
    { "window_title", OPT_STRING | HAS_ARG, { &window_title }, "set window title", "window title" },
    { "left", OPT_INT | HAS_ARG | OPT_EXPERT, { &screen_left }, "set the x position for the left of the window", "x pos" },
    { "top", OPT_INT | HAS_ARG | OPT_EXPERT, { &screen_top }, "set the y position for the top of the window", "y pos" },
    #if CONFIG_AVFILTER
    { "vf", OPT_EXPERT | HAS_ARG, { .func_arg = opt_add_vfilter }, "set video filters", "filter_graph" },
    { "af", OPT_STRING | HAS_ARG, { &afilters }, "set audio filters", "filter_graph" },
    #endif
    { "rdftspeed", OPT_INT | HAS_ARG| OPT_AUDIO | OPT_EXPERT, { &rdftspeed }, "rdft speed", "msecs" },
    { "showmode", HAS_ARG, { .func_arg = opt_show_mode}, "select show mode (0 = video, 1 = waves, 2 = RDFT)", "mode" },
    { "default", HAS_ARG | OPT_AUDIO | OPT_VIDEO | OPT_EXPERT, { .func_arg = opt_default }, "generic catch all option", "" },
    { "i", OPT_BOOL, { &dummy}, "read specified file", "input_file"},
    { "codec", HAS_ARG, { .func_arg = opt_codec}, "force decoder", "decoder_name" },
    { "acodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {    &audio_codec_name }, "force audio decoder",    "decoder_name" },
    { "scodec", HAS_ARG | OPT_STRING | OPT_EXPERT, { &subtitle_codec_name }, "force subtitle decoder", "decoder_name" },
    { "vcodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {    &video_codec_name }, "force video decoder",    "decoder_name" },
    { "autorotate", OPT_BOOL, { &autorotate }, "automatically rotate video", "" },
    { "find_stream_info", OPT_BOOL | OPT_INPUT | OPT_EXPERT, { &find_stream_info },
      "read and decode the streams to fill missing information with heuristics" },
    { "filter_threads", HAS_ARG | OPT_INT | OPT_EXPERT, { &filter_nbthreads }, "number of filter threads per graph" },
    { NULL, },
};

/**
 * @brief 简单显示一些内容，了解一下即可。
*/
static void show_usage(void)
{
    av_log(NULL, AV_LOG_INFO, "Simple media player\n");
    av_log(NULL, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
    av_log(NULL, AV_LOG_INFO, "\n");
}

void show_help_default(const char *opt, const char *arg)
{
    av_log_set_callback(log_callback_help);
    show_usage();
    show_help_options(options, "Main options:", 0, OPT_EXPERT, 0);
    show_help_options(options, "Advanced options:", OPT_EXPERT, 0, 0);
    printf("\n");
    show_help_children(avcodec_get_class(), AV_OPT_FLAG_DECODING_PARAM);
    show_help_children(avformat_get_class(), AV_OPT_FLAG_DECODING_PARAM);
#if !CONFIG_AVFILTER
    show_help_children(sws_get_class(), AV_OPT_FLAG_ENCODING_PARAM);
#else
    show_help_children(avfilter_get_class(), AV_OPT_FLAG_FILTERING_PARAM);
#endif
    printf("\nWhile playing:\n"
           "q, ESC              quit\n"
           "f                   toggle full screen\n"
           "p, SPC              pause\n"
           "m                   toggle mute\n"
           "9, 0                decrease and increase volume respectively\n"
           "/, *                decrease and increase volume respectively\n"
           "a                   cycle audio channel in the current program\n"
           "v                   cycle video channel\n"
           "t                   cycle subtitle channel in the current program\n"
           "c                   cycle program\n"
           "w                   cycle video filters or show modes\n"
           "s                   activate frame-step mode\n"
           "left/right          seek backward/forward 10 seconds or to custom interval if -seek_interval is set\n"
           "down/up             seek backward/forward 1 minute\n"
           "page down/page up   seek backward/forward 10 minutes\n"
           "right mouse click   seek to percentage in file corresponding to fraction of width\n"
           "left double-click   toggle full screen\n"
           );
}

/* Called from the main */
int main(int argc, char **argv)
{
    int flags;
    VideoState *is;                         // 播放器封装

    //    av_log_set_level(AV_LOG_TRACE);
    init_dynload();

    // 1. 对FFmpeg的初始化
    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options);
    /// av_log_set_level(AV_LOG_DEBUG);

    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    // 设置和图片伸缩有关的参数flags = "bicubic"
    init_opts();

    signal(SIGINT , sigterm_handler); /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

    show_banner(argc, argv, options);

    // 2. 对传递的参数进行初始化
    // 简单了解一下该函数即可，主要看ffplay的核心。毕竟FFmpeg的命令参数还是挺复杂的，没必要花太多时间，看完核心有空再看不迟。
    parse_options(NULL, argc, argv, options, opt_input_file);

    if (!input_filename) {
        show_usage();
        av_log(NULL, AV_LOG_FATAL, "An input file must be specified\n");
        av_log(NULL, AV_LOG_FATAL,
               "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        exit(1);
    }

    /* 是否显示视频 */
    //display_disable = 1;
    if (display_disable) {
        video_disable = 1;
    }

    // 3. SDL的初始化
    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;// flags = 00110001
    /* 是否运行音频 */
    //audio_disable = 1;
    if (audio_disable){
        flags &= ~SDL_INIT_AUDIO;// 00110001 & 1110 1111 = 0010 0001，即去掉音频标志位
    }
    else {
        /* Try to work around an occasional ALSA buffer underflow issue when the
         * period size is NPOT due to ALSA resampling by forcing the buffer size.
         * 尝试解决一个偶然的ALSA缓冲区下溢问题，周期大小是NPOT，由于ALSA重采样强迫缓冲区大小。
        */
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
            SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);
    }
    if (display_disable)
        flags &= ~SDL_INIT_VIDEO;// 同理，去掉视频标志位

    // SDL_Init函数参考：https://blog.csdn.net/qq_25333681/article/details/89787836
    if (SDL_Init (flags)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    /*
     * 从队列删除系统和用户的事件。
     *  该函数允许您设置处理某些事件的状态。
     * -如果state设置为::SDL_IGNORE，该事件将自动从事件队列中删除，不会过滤事件。
     *
     * -如果state设置为::SDL_ENABLE，则该事件将被处理正常。
     *
     * -如果state设置为::SDL_QUERY, SDL_EventState()将返回指定事件的当前处理状态。
    */
    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    av_init_packet(&flush_pkt);				// 初始化flush_packet
    flush_pkt.data = (uint8_t *)&flush_pkt; // 初始化为数据指向自己本身

    // 4. 创建窗口
    if (!display_disable) {
        int flags = SDL_WINDOW_HIDDEN;      // 窗口不可见
        if (alwaysontop)                    // alwaysontop是否置顶，不过设置了也没用，应该和版本是否支持有关系
#if SDL_VERSION_ATLEAST(2,0,5)
            flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
            av_log(NULL, AV_LOG_WARNING, "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif

        // 是否禁用窗口大小调整
        //borderless = 1;
        if (borderless)
            flags |= SDL_WINDOW_BORDERLESS; // 没有窗口装饰，即没有最外层
        else
            flags |= SDL_WINDOW_RESIZABLE;  // 窗口可以调整大小

        //  返回创建完成的窗口的ID。如果创建失败则返回0。
        window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width, default_height, flags);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");// 创建优先级提示，"1"或"linear" - 表示 线性滤波(OpenGL和Direct3D支持)
        if (window) {
            // 创建renderer(see details https://blog.csdn.net/leixiaohua1020/article/details/40723085)
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);//使用硬件加速，并且设置和显示器的刷新率同步
            if (!renderer) {
                av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
                renderer = SDL_CreateRenderer(window, -1, 0);
            }
            if (renderer) {
                if (!SDL_GetRendererInfo(renderer, &renderer_info))
                    av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
            }
        }

        // 窗口、渲染器、渲染器中可用纹理格式的数量其中一个失败，程序都退出
        if (!window || !renderer || !renderer_info.num_texture_formats) {
            av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
            do_exit(NULL);
        }
    }

    // 5. 通过stream_open函数，返回播放器实例，并且开启read_thread读取线程
    is = stream_open(input_filename, file_iformat);
    if (!is) {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        do_exit(NULL);
    }

    // 6. 事件响应
    event_loop(is);

    /* never returns */

    return 0;
}
