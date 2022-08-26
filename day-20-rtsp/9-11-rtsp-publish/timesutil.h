#ifndef TIMEUTIL_H
#define TIMEUTIL_H

#include <stdint.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#ifdef _MSC_VER	/* MSVC */
//#define snprintf _snprintf
#define strcasecmp stricmp
#define strncasecmp strnicmp
//#define vsnprintf _vsnprintf
#endif

#define GetSockError()	WSAGetLastError()
#define SetSockError(e)	WSASetLastError(e)
#define setsockopt(a,b,c,d,e)	(setsockopt)(a,b,c,(const char *)d,(int)e)
#define EWOULDBLOCK	WSAETIMEDOUT	/* we don't use nonblocking, but we do use timeouts */
#define sleep(n)	Sleep(n*1000)
#define msleep(n)	Sleep(n)
#define SET_RCVTIMEO(tv,s)	int tv = s*1000
#else /* !_WIN32 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/times.h>
#include <netdb.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#define GetSockError()	errno
#define SetSockError(e)	errno = e
#undef closesocket
#define closesocket(s)	close(s)
#define msleep(n)	usleep(n*1000)
#define SET_RCVTIMEO(tv,s)	struct timeval tv = {s,0}
#endif

#include<chrono>
using namespace std;
using namespace std::chrono;

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

// 获取当前的时间，windows以系统开始计算，linux以1970年1月1日0时0分0秒开始计算
// windows可能存在的误差情况，后续需要注意优化。
class TimesUtil
{
public:
    static inline int64_t GetTimeMillisecond()
    {
        #ifdef _WIN32
        // 返回从操作系统启动到当前所经过的毫秒数，存储的最大值是(2^32-1) ms约为49.71天，因此若系统运行时间超过49.71天时，这个数就会归0，例如你的电脑。
        // 如果是编写服务器端程序，此处一定要万分注意，避免引起意外的状况.
        // 我们这里也会有影响。例如rtsp推流时，pre_time_是一个很大的数，然后写头，开始判断是否超时，
        // 此时假设归0后cur_time=5，相减后取绝对值就会变得很大(必须取绝对值)，那么此时就会超时返回。所以可能本来是正常写头的，但是因为归0而造成超时返回了，

        // 这种情况解决方法是：在本程序可能会遇到超时的接口，可以再次调用该接口，以更新一次pre_time_，从而解决这种情况。

        // 关于GetTickCount， 详看https://blog.csdn.net/kevin_lp/article/details/89631034?spm=1001.2014.3001.5501博客。
        // 更精准的可以使用QueryPerformanceFrequency + QueryPerformanceCounter(后续建议使用)：https://blog.csdn.net/xp178171640/article/details/118306073
            return (int64_t)GetTickCount();
        #else
        // linux使用的时间函数比较简单，gettimeofday()用来来得到从1970年1月1日0时0分0秒到现在的秒数
            struct timeval tv;
            gettimeofday(&tv, NULL);
            return ((int64_t)tv.tv_sec * 1000 + (unsigned long long)tv.tv_usec / 1000);// 转成ms返回
        #endif

//        return duration_cast<chrono::milliseconds>(high_resolution_clock::now() - m_begin).count();

    }
//private:
//    static time_point<high_resolution_clock> m_begin;
};

#endif // TIMEUTIL_H
