#include "videocapturer.h"
#include "dlog.h"
#include "timesutil.h"
#include "avpublishtime.h"


VideoCapturer::VideoCapturer()
{

}

VideoCapturer::~VideoCapturer()
{
    if(yuv_buf_) {
        delete [] yuv_buf_;
    }
    if(yuv_fp_) {
        fclose(yuv_fp_);
    }
}

/**
 * @brief 初始化采集模块的相关参数。
 * @param   "x"                 x起始位置，缺省为0
 *          "y"                 y起始位置，缺省为0
 *          "width"             宽度，缺省为屏幕宽带
 *          "height"            高度，缺省为屏幕高度
 *          "pixel_format"      像素格式，AVPixelFormat对应的值，缺省为AV_PIX_FMT_YUV420P
 *          "fps"               帧数，缺省为25
 *
 * @return success 0 fail return a negative number。
 */
RET_CODE VideoCapturer::Init(const Properties &properties)
{
    // 初始化采集的参数
    video_test_         = properties.GetProperty("video_test", 0);
    input_yuv_name_     = properties.GetProperty("input_yuv_name", "720x480_25fps_420p.yuv");
    x_                  = properties.GetProperty("x", 0);
    y_                  = properties.GetProperty("y", 0);
    width_              = properties.GetProperty("width", 1920);
    height_             = properties.GetProperty("height", 1080);
    pixel_format_       = properties.GetProperty("pixel_format", 0);
    fps_                = properties.GetProperty("fps", 25);
    frame_duration_     = 1000.0 / fps_;                                                // 单位是毫秒的

    // 打开文件
    if(openYuvFile(input_yuv_name_.c_str()) != 0)
    {
        LogError("openYuvFile %s failed", input_yuv_name_.c_str());
        return RET_FAIL;
    }

    return RET_OK;
}

/**
 * @brief 采集线程回调，将采集到的数据调用编码层传入的回调进行处理。
 * @return void。
 */
void VideoCapturer::Loop()
{
    LogInfo("into loop");

    // 1）一帧yuv占用的字节数量，宽高各自取余是：若分辨率是奇数，则需要增大一帧的buf。
    // 例如3x3.不取余则是：3x3x1.5=13.5，但是奇数分辨率在内存中仍会开辟成奇数+1的偶数的乘积大小，只是不占数据而已0~3的下标3不存数据(猜想FFmpeg可能就这么做的)。
    // 所以实际一帧的内存就是4x4x1.5=24，再用13.5的buf存就会溢出，所以必须取余各自增1后再乘以1.5.这样buf才是24，能存下最大一帧的字节数24.
    // 或者上面可以在Init初始化时，若x_、y_是奇数就直接返回，也是处理的一种方法。
    // 2）并且注意，这里乘以1.5是因为他用yuv420的格式了，重写时必须优化掉，不能写死为1.5。
    yuv_buf_size =(width_ + (width_ % 2)) * (height_ + (height_ % 2)) * 1.5;        // 一帧yuv420占用的字节数量，这里写死是yuv420；yuv422需要乘以2；yuv444需要乘以3.
    yuv_buf_ = new uint8_t[yuv_buf_size];

    yuv_total_duration_ = 0;
    yuv_start_time_ = TimesUtil::GetTimeMillisecond();                              // 采集模块的第一帧yuv的采集时间
    LogInfo("into loop while");

    while (true) {
        if(request_abort_) {
            break;
        }
        if(readYuvFile(yuv_buf_, yuv_buf_size) == 0)
        {
            // 打印采集首帧视频的时间戳，方便对比编码、推流时的时间戳，以获取延时，方便debug。
            if(!is_first_frame_) {
                is_first_frame_ = true;
                // 这里打印时间戳时是单例，后续推多路时，不能将获取时间戳写成单例，否则获取延时肯定是不对的
                LogInfo("%s:t%u", AVPublishTime::GetInstance()->getVInTag(),
                        AVPublishTime::GetInstance()->getCurrenTime());
            }
            if(callable_object_)
            {
                callable_object_(yuv_buf_, yuv_buf_size);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    request_abort_ = false;
    closeYuvFile();
}

void VideoCapturer::AddCallback(function<void (uint8_t *, int32_t)> callback)
{
    callable_object_ = callback;
}

/**
 * @brief 以只读方式打开一个文件。
 * @param file_name 输入文件名。
 * @return success 0 fail return a negative number。
 */
int VideoCapturer::openYuvFile(const char *file_name)
{
    yuv_fp_ = fopen(file_name, "rb");
    if(!yuv_fp_)
    {
        return -1;
    }
    return 0;
}

/**
 * @brief 以只读方式打开一个文件。
 * @param yuv_buf 传入传出，一帧缓存。音频该参数未使用，使用成员代替了。所以后续需要统一一下。
 * @param yuv_buf_size  要读取的yuv字节大小。
 * @return success 0 fail return other。
 *
 * 注意：这里看到，采集数据的时间：是使用帧间隔+直接系统时间模式去采集数据的(详看rtmp推流310.pdf)。
 * 假设首帧：yuv_start_time_=40,cur_time=80,yuv_total_duration_=0->可以读一帧->yuv_total_duration_=40;
 * 假设     yuv_start_time_=40,cur_time=110,yuv_total_duration_=40->可以读一帧->yuv_total_duration_=80;
 * 假设     yuv_start_time_=40,cur_time=130,yuv_total_duration_=80->可以读一帧->yuv_total_duration_=120;
 *
 * 假设     yuv_start_time_=40,cur_time=140,yuv_total_duration_=120->不可以读，只能等cur_time=160及以上才能读;
 *
 * 不是很理解采集时的3种方式，后续需要看回rtmp的视频。
 */
int VideoCapturer::readYuvFile(uint8_t *yuv_buf, int32_t yuv_buf_size)
{
    // dif 采集开始到目前的时间，代表还可以采集的时间戳.
    // 若已经采集到的数据大于dif，表示你采集数据过多了目前不能再采集，请稍后再采集；小于diff，则表示只要你还想采集，就可以采集。
    int64_t cur_time = TimesUtil::GetTimeMillisecond();
    int64_t dif = cur_time - yuv_start_time_;

    // 1 使用直接系统时间，判断是否可以采集数据
    //LogDebug("%lld, %lld\n", yuv_total_duration_, dif);
    if((int64_t)yuv_total_duration_ > dif)
        return 1;

    // 2 该读取数据了
    size_t ret = fread(yuv_buf, 1, yuv_buf_size, yuv_fp_);
    if(ret != yuv_buf_size)
    {
        // 从文件头部开始读取
        ret = fseek(yuv_fp_, 0, SEEK_SET);
        ret = fread(yuv_buf, 1, yuv_buf_size, yuv_fp_);
        if(ret != yuv_buf_size)
        {
            return -1;
        }
    }

    // 3 累计帧间隔
    // LogDebug("yuv_total_duration_:%lldms, %lldms", (int64_t)yuv_total_duration_, dif);
    yuv_total_duration_ += frame_duration_;

    return 0;
}

int VideoCapturer::closeYuvFile()
{
    if(yuv_fp_)
        fclose(yuv_fp_);
    return 0;
}

