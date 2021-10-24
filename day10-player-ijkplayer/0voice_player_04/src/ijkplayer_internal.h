#ifndef IJKPLAYER_INTERNAL_H
#define IJKPLAYER_INTERNAL_H
#include <assert.h>
#include "ff_fferror.h"
#include "ff_ffplay.h"
//#include "ijkplayer.h"
#include "SDL.h"

struct IjkMediaPlayer {
    volatile int ref_count;
    SDL_mutex *mutex;
    FFPlayer *ffplayer;     // 播放器

    int (*msg_loop)(void*); // 消息循环
    void *user;             // 报错msg_loop的参数
    SDL_Thread *msg_thread;
    int mp_state;           // 涉及到播放器的逻辑的
    char *data_source;      // 要播放的url
    void *weak_thiz;

    int restart;
    int restart_from_beginning;
    int seek_req;
    long seek_msec;
};
#endif // IJKPLAYER_INTERNAL_H
