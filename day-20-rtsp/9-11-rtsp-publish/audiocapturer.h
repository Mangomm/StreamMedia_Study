#ifndef AUDIOCAPTURER_H
#define AUDIOCAPTURER_H

#include <functional>
#include "commonlooper.h"
#include "mediabase.h"
using std::function;

class AudioCapturer : public CommonLooper
{

public:
    AudioCapturer();
    virtual ~AudioCapturer();
    RET_CODE Init(const Properties properties);

    virtual void Loop();
    void AddCallback(function<void(uint8_t*, int32_t)> callback);
    // void AddCallback(std::function<void(uint8_t *, int32_t)> callback);

private:
    // PCM file只是用来测试, 写死为s16格式 2通道 采样率48Khz
    // 1帧1024采样点持续的时间21.333333333333333333333333333333ms
    int openPcmFile(const char *file_name);
    int readPcmFile(uint8_t *pcm_buf, int32_t pcm_buf_size);
    int closePcmFile();

    // 实际上下面的初始化最终还是由Init时决定，若没有在init Get对应的值，才会到这取这些初始值。
    int audio_test_ = 0;                                            // 该字段目前意义不大，只是表示一种模式，例如测试模式
    std::string input_pcm_name_;                                    // 输入pcm测试文件的名字
    FILE *pcm_fp_ = NULL;                                           // 输入pcm的测试文件
    int64_t pcm_start_time_ = 0;                                    // 记录采集到首帧时的时间，单位ms。
    double pcm_total_duration_ = 0;                                 // 推流时长的统计
    //double frame_duration_ = 23.2;                                // 一帧时长，23.2表示默认是44100hz.
    double frame_duration_ = 21.3;                                  // 一帧时长

    std::function<void(uint8_t *, int32_t)> callback_get_pcm_;      // 采集到数据后，用于传给编码层的回调函数，由上层赋值。

    uint8_t *pcm_buf_;                                              // 存在一帧音频的缓存
    int32_t pcm_buf_size_;                                          // 一帧音频最大字节大小

    int channels_ = 2;
    int sample_rate_ = 48000;
    int byte_per_sample_ = 2;                                       // 音频3要素

    int nb_samples_ = 1024;
    int format_ = 1;                                                // 目前固定s16先

    bool is_first_time_ = false;
};

#endif // AUDIOCAPTURER_H
