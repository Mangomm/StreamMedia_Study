之前版本统称为V1.0.0，

V1.1.0：
	1）看到视频采集videocapturer类，已看完9个文件。
	
看完的目前为止不怎么理解透彻的有：
	1）主要是在采集模块时，不是很理解采集时的3种采集时间戳方式(帧间隔、直接系统时间模式、帧间隔+直接系统时间模式)，后续需要看回rtmp的视频去理解。
存在并发现的bug：
	1）在公司推流rtsp时，发现视频是花屏的，dump处理的视频也是花屏，音频都是正常。但是在自己电脑推流音视频都没问题。
	怀疑是ZLM版本的问题，不过在公司用FFmpeg单独推流rtsp，视频又能正常播放，但是没有进行视频同步，单独推视频正常这一点不能证明什么。
	后续打算看完代码再解决这个花屏问题吧。
	
	2）但是将上面的h264同样的命令解码后，放在自己的电脑去推流又没有问题。
	
	3）目前初步判断公司电脑中，dump yuv的时候出现问题了。(可是不是同样的代码吗？还是留在最后解决吧)
	
V1.1.1：
	看完packetqueue.h。
看完的目前为止不怎么理解透彻的有：
	1）算队列中的时长时，audio_front_pts_更新为出队列的pts，即audio_front_pts_ = mypkt->pkt->pts; 视频同理。
	后续需要研究一下pushPrivate的打印”LogInfo("video_back_pts_: %lld, video_front_pts_: %lld, stats_.video_nb_packets: %d",video_back_pts_, video_front_pts_, stats_.video_nb_packets);“显示的内容，
	搞懂求出的队列的duration实际是n-1的时长，而为什么不是n帧的时长。这可能涉及到编码前后打时间戳的内容。
	研究上面pushPrivate的内容，实际就是研究Drop的"int64_t duration = video_back_pts_ - video_front_pts_;"，这样packetqueue.h队列你就理解透彻了。
	
V1.1.2：
	看完rtsppusher.
	1）在rtsppusher推流的超时处理并且是windows平台时，会使用到GetTickCount函数，该函数可能会遇到归0问题。
			后续优化的话：在本程序可能会遇到超时的接口，可以再次调用该接口，以更新一次pre_time_，从而解决这种情况。
	
解决的问题：
	1）解决了V1.1.1的疑问。在debugQueue函数发现，用老师的代码看到获取的stats，队列的包数是0，但是获取到的队列视频或者音频时长仍然是40ms-21ms的打印，
		说明是不匹配的，或者有可能是统计Push或者Pop统计包数等相关信息不太准确，后期还是需要优化一下；而我的是0，说明应该问题不大。
		代码已经对Drop、GetAudioDuration、GetVideoDuration、GetStats的duration进行处理了，可以使用宏MyDurationCode去改变，具体看代码。
	视频可以看回9-11综合优化，大概从15min开始。
	即：该问题虽然解决，代码用着我的，但是后期还是需要去优化队列的统计相关内容，大概想了一下，应该是在Drop与Pop时，更新video_front_pts_值的时候有点问题，其值应该等于drop掉后的队列的包头pts，而不是等于drop掉的包的pts，需要导师验证，没太大问题后期再优化吧。