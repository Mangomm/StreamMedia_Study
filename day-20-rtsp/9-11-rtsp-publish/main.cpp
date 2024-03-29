﻿#include <iostream>
#include "dlog.h"
#include "pushwork.h"
#include "messagequeue.h"
using namespace std;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/audio_fifo.h>
}

/*
 * flv解码成yuv并指定分辨率：
 *      ffmpeg.exe -i source.200kbps.768x320.flv -vcodec rawvideo -acodec rawaudio -pix_fmt yuv420p -s 768x480 720x480_25fps_420p.yuv
 *  或者 ffmpeg -i test_1280x720.flv -t 5 -r 25 -pix_fmt yuv420p yuv420p_1280x720.yuv
 *
 * 去掉音频: ffmpeg.exe -i source.200kbps.768x320.flv -vcodec rawvideo -an -pix_fmt yuv420p -s 768x480 720x480_25fps_420p.yuv
 * 去掉视频：ffmpeg.exe -i source.200kbps.768x320.flv -vn -f s16le -ar 48000 -ac 2 buweishui_48000_2_s16le.pcm
 * 测试视频是否解码成功： ffplay -f rawvideo -video_size 768x480 720x480_25fps_420p.yuv
 * 测试音频是否解码成功： ffplay -ar 48000 -ac 2 -f s16le -i buweishui_48000_2_s16le.pcm
 *
 * 命令推rtsp：ffmpeg -re -s 768*480 -pix_fmt yuv420p -i 720x480_25fps_420p.yuv -vcodec libx264 -f rtsp rtsp://192.168.2.38/live/livestream
 *
*/

//减少缓冲 ffplay.exe -i rtmp://xxxxxxx -fflags nobuffer
// 减少码流分析时间 ffplay.exe -i rtmp://xxxxxxx -analyzeduration 1000000 单位为微秒
// ffplay -i rtsp://192.168.2.132/live/livestream -fflags nobuffer -analyzeduration 1000000 -rtsp_transport udp

//#define RTSP_URL "rtsp://111.229.231.225/live/livestream"
//#define RTSP_URL "rtsp://192.168.2.132/live/livestream"
#define RTSP_URL "rtsp://192.168.2.38/live/livestream"
// ffmpeg -re -i  rtsp_test_hd.flv  -vcodec copy -acodec copy  -f flv -y rtsp://111.229.231.225/live/livestream
// ffmpeg -re -i  rtsp_test_hd.flv  -vcodec copy -acodec copy  -f flv -y rtsp://192.168.1.12/live/livestream
// ffmpeg -re -i  1920x832_25fps.flv  -vcodec copy -acodec copy  -f flv -y rtsp://111.229.231.225/live/livestream


int main()
{
    cout << "Hello World!" << endl;

    init_logger("rtsp_push.log", S_INFO);

    MessageQueue *msg_queue_ = new MessageQueue();

    //    for(int i = 0; i < 5; i++)
    {
        //        LogInfo("test pushwork:%d", i);


        if(!msg_queue_) {
            LogError("new MessageQueue() failed");
            return -1;
        }

        PushWork push_work(msg_queue_);
        Properties properties;
        // 音频test模式
        properties.SetProperty("audio_test", 1);                    // 音频测试模式，这个配置应该是为后面切换到不同的播放模式
        properties.SetProperty("input_pcm_name", "buweishui_48000_2_s16le.pcm");
        // 麦克风采样属性(采集部分)
        properties.SetProperty("mic_sample_fmt", AV_SAMPLE_FMT_S16);
        properties.SetProperty("mic_sample_rate", 48000);
        properties.SetProperty("mic_channels", 2);
        // 音频编码属性(编码部分)
        properties.SetProperty("audio_sample_rate", 48000);
        properties.SetProperty("audio_bitrate", 64 * 1024);
        properties.SetProperty("audio_channels", 2);

        //视频test模式
        properties.SetProperty("video_test", 1);
        properties.SetProperty("input_yuv_name", "720x480_25fps_420p.yuv");
        //properties.SetProperty("input_yuv_name", "yuv420p_1280x720.yuv");
        //properties.SetProperty("input_yuv_name", "test-cmd.yuv");//1280x720
        // 桌面录制属性(采集部分)
        properties.SetProperty("desktop_x", 0);
        properties.SetProperty("desktop_y", 0);
        properties.SetProperty("desktop_width", 768);               // 测试模式时和yuv文件的宽度一致，记住本测试文件是768，而不是720
        properties.SetProperty("desktop_height", 480);              // 测试模式时和yuv文件的高度一致
        // properties.SetProperty("desktop_pixel_format", AV_PIX_FMT_YUV420P);
        properties.SetProperty("desktop_fps", 25);                  // 测试模式时和yuv文件的帧率一致
        // 视频编码属性(编码部分)
        properties.SetProperty("video_bitrate", 512 * 1024);        // 设置码率

        // 配置rtsp
        //1.url
        //2.udp
        properties.SetProperty("rtsp_url", RTSP_URL);
        properties.SetProperty("rtsp_transport", "udp");            // udp or tcp
        properties.SetProperty("rtsp_timeout", 5000);               // connect server timeout
        properties.SetProperty("rtsp_max_queue_duration", 1000);

        if(push_work.Init(properties) != RET_OK) {
            LogError("PushWork init failed");
            return -1;
        }

        int count = 0;
        AVMessage msg;
        int ret = 0;
        while (true)
        {
        // std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            ret = msg_queue_->msg_queue_get(&msg, 1000);
            if(1 == ret) {
                switch (msg.what) {
                case MSG_RTSP_ERROR:
                    LogError("MSG_RTSP_ERROR error:%d", msg.arg1);
                    break;
                case MSG_RTSP_QUEUE_DURATION:
                    LogError("MSG_RTSP_QUEUE_DURATION a:%d, v:%d", msg.arg1, msg.arg2);
                    break;
                default:
                    break;
                }
            }
            LogInfo("count:%d, ret:%d", count, ret);
            if(count++ > 100) {
                printf("main break\n");
                break;
            }
        }


        msg_queue_->msg_queue_abort();
    }

    delete msg_queue_;

    LogInfo("main finish");
    return 0;
}
