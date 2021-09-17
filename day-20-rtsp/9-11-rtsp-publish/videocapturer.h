#ifndef VIDEOCAPTURER_H
#define VIDEOCAPTURER_H

#include <functional>
#include "commonlooper.h"
#include "mediabase.h"
using std::function;


class VideoCapturer: public CommonLooper
{
public:
    VideoCapturer();
    virtual ~VideoCapturer();

    /**
     * @brief Init
     * @param "x", x起始位置，缺省为0
     *          "y", y起始位置，缺省为0
     *          "width", 宽度，缺省为屏幕宽带
     *          "height", 高度，缺省为屏幕高度
     *          "format", 像素格式，AVPixelFormat对应的值，缺省为AV_PIX_FMT_YUV420P
     *          "fps", 帧数，缺省为25
     * @return
     */
    RET_CODE Init(const Properties& properties);
    virtual void Loop();
    void AddCallback(function<void(uint8_t*, int32_t)> callback);       // 设置编码回调函数

private:

    int video_test_ = 0;                                                // 一种模式。这里为测试模式。
    std::string input_yuv_name_;                                        // 输入yuv文件名字，用于测试
    int x_;                                                             // 采集的左上角起始坐标
    int y_;
    int width_ = 0;
    int height_ = 0;                                                    // (width_, height_)右下角坐标
    int pixel_format_ = 0;                                              // 视频格式
    int fps_;
    double frame_duration_ = 40;                                        // 帧间隔默认40ms

    // 本地文件测试
    int openYuvFile(const char *file_name);
    int readYuvFile(uint8_t *yuv_buf, int32_t yuv_buf_size);
    int closeYuvFile();

    int64_t yuv_start_time_ = 0;                                        // 起始时间
    double yuv_total_duration_ = 0;                                     // YUV读取累计的时间
    FILE *yuv_fp_ = NULL;
    uint8_t *yuv_buf_ = NULL;
    int yuv_buf_size = 0;


    function<void(uint8_t*, int32_t)> callable_object_ = NULL;          // 保存上层回调，采集到的数据，交由该回调处理，一般是编码。

    bool is_first_frame_ = false;                                       // 采集的帧是否是首帧，对于首帧的时间戳打印，对比延时很重要
};

#endif // VIDEOCAPTURER_H
