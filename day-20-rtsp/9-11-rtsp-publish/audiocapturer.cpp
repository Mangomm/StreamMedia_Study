#include "audiocapturer.h"
#include "dlog.h"
#include "timesutil.h"
#include "avpublishtime.h"
extern "C" {
#include <libavcodec/avcodec.h>
}


AudioCapturer::AudioCapturer(): CommonLooper()
{

}

AudioCapturer::~AudioCapturer()
{
    if(pcm_buf_) {
        delete [] pcm_buf_;
    }
    if(pcm_fp_) {
        fclose(pcm_fp_);
    }
}

/**
 * @brief 初始化参数。
 * @param properties 参数属性容器，由上层传入。
 * @return success 0 fail return a negative number。
 */
RET_CODE AudioCapturer::Init(const Properties properties)
{
    // 从上层获取参数并保存
    audio_test_         = properties.GetProperty("audio_test", 0);
    input_pcm_name_     = properties.GetProperty("input_pcm_name", "buweishui_48000_2_s16le.pcm");
    sample_rate_        = properties.GetProperty("sample_rate", 48000);
    channels_           = properties.GetProperty("channels", 2);
    byte_per_sample_    = properties.GetProperty("byte_per_sample", 2);         // 单个采样点所占用字节数，默认s16故是2字节，前3个为音频3要素

    nb_samples_         = properties.GetProperty("nb_samples", 1024);
    format_             = properties.GetProperty("format", AV_SAMPLE_FMT_S16);

    // 计算一帧所占大小，必须是根据传入参数去计算
    pcm_buf_size_       = byte_per_sample_ * channels_ *  nb_samples_;
    pcm_buf_ = new uint8_t[pcm_buf_size_];
    if(!pcm_buf_)
    {
        return RET_ERR_OUTOFMEMORY;
    }

    if(openPcmFile(input_pcm_name_.c_str()) < 0)
    {
        LogError("openPcmFile %s failed", input_pcm_name_.c_str());
        return RET_FAIL;
    }
    // 必须是根据传入参数去计算
    frame_duration_ = 1.0 * nb_samples_ / sample_rate_ * 1000;  // 得到一帧的毫秒时间，1000先乘或者后乘结果都一样，基本到小数点20位后才可能不太准

    return RET_OK;
}

/**
 * @brief 采集线程回调，将采集到的数据调用编码层传入的回调进行处理。
 * @return void。
 */
void AudioCapturer::Loop()
{
    LogInfo("into loop");
    pcm_total_duration_ = 0;
    pcm_start_time_ = TimesUtil::GetTimeMillisecond();      // 初始化时间基，记录采集到首帧时的时间，单位ms。

    while(true) {
        if(request_abort_) {
            break;                                          // 请求退出
        }

        if(readPcmFile(pcm_buf_, pcm_buf_size_) == 0) {
            // 打印采集首帧视频的时间戳，方便对比编码、推流时的时间戳，以获取延时，方便debug。
            if(!is_first_time_) {
                is_first_time_ = true;
                // 这里打印时间戳时是单例，后续推多路时，不能将获取时间戳写成单例，否则获取延时肯定是不对的
                LogInfo("%s:t%u", AVPublishTime::GetInstance()->getAInTag(),
                        AVPublishTime::GetInstance()->getCurrenTime());
            }
            // 将数据上交给编码层处理
            if(callback_get_pcm_) {
                callback_get_pcm_(pcm_buf_, pcm_buf_size_);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    request_abort_ = false;
    closePcmFile();
}

void AudioCapturer::AddCallback(function<void (uint8_t *, int32_t)> callback)
{
    callback_get_pcm_ = callback;
}

/**
 * @brief 以只读方式打开一个文件。
 * @return success 0 fail return a negative number。
 */
int AudioCapturer::openPcmFile(const char *file_name)
{
    pcm_fp_ = fopen(file_name, "rb");
    if(!pcm_fp_)
    {
        return -1;
    }
    return 0;
}

/**
 * @brief 以只读方式打开一个文件。
 * @param pcm_buf 未用到，使用类成员代替了。
 * @param pcm_buf_size  要读取的pcm字节大小。
 * @return success 0 fail return other。
 */
int AudioCapturer::readPcmFile(uint8_t *pcm_buf, int32_t pcm_buf_size)
{
    int64_t cur_time = TimesUtil::GetTimeMillisecond();     // 单位毫秒
    int64_t dif = cur_time - pcm_start_time_;               // 目前经过的时间

    if(((int64_t)pcm_total_duration_) > dif) {              // 还没有到读取新一帧的时间，音频采集的总时长处理与5-rtp的发送aac的rtp包不一样。
        return 1;                                           // 后者有减50以多发几帧到客户端的操作，原因是前者若提前采集音频，会导致前后音频帧存在重复的情况，
     }                                                      // 给人以为卡顿；后者可以是因为发送的数据已经都是完整的帧，客户端只需要缓存接收进行播放即可

    // 读取数据
    size_t ret = fread(pcm_buf_, 1, pcm_buf_size, pcm_fp_);
    if(ret != pcm_buf_size) {                               // 可能读到尾部或者真的读取数据失败
        ret = fseek(pcm_fp_, 0, SEEK_SET);                  // seek到文件起始
        ret = fread(pcm_buf_, 1, pcm_buf_size, pcm_fp_);    // 再读一次，如果再失败说明真的失败，返回失败，否则说明上面读到文件尾，
        if(ret != pcm_buf_size) {                           // 这样会导致舍弃掉最后的部分数据，但是可以忽略，并且这也是比较好的处理方法
            return -1;                                      // 出错
        }
    }

    pcm_total_duration_ += frame_duration_;                 // 统计采集到的帧总时长
    return 0;
}

/**
 * @brief 关闭一个已经打开的文件。
 * @return no mean。
 */
int AudioCapturer::closePcmFile()
{
    if(pcm_fp_)
        fclose(pcm_fp_);
    return 0;
}
