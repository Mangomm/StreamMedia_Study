#include "ijkplayer_qt.h"
#include "ff_ffmsg_queue.h"
#include "ijkplayer_internal.h"

#include "slog.h"
IjkMediaPlayer *ijkmp_qt_create(int(*msg_loop)(void*), void *user)
{
    IjkMediaPlayer *mp = ijkmp_create(msg_loop, user);
    if (!mp)
        goto fail;

    //    mp->ffplayer->vout = SDL_VoutAndroid_CreateForAndroidSurface();
    //    if (!mp->ffplayer->vout)
    //        goto fail;

    //    mp->ffplayer->pipeline = ffpipeline_create_from_android(mp->ffplayer);
    //    if (!mp->ffplayer->pipeline)
    //        goto fail;

    //    ffpipeline_set_vout(mp->ffplayer->pipeline, mp->ffplayer->vout);

    return mp;

fail:
    //    ijkmp_dec_ref_p(&mp);
    return NULL;
}



static int message_loop_n(void *user)
{
    IjkMediaPlayer *mp = (IjkMediaPlayer *)user;

    IjkPlayerQt *ijkplayerqt = (IjkPlayerQt *)mp->user;

    while (1) {
        AVMessage msg;

        int retval = ijkmp_get_msg(mp, &msg, 1);
        if (retval < 0)
            break;

        if (retval < 0)
            break;

        // block-get should never return 0
        assert(retval > 0);

        switch (msg.what) {
        case FFP_MSG_FLUSH:
            LOG_TRACE("FFP_MSG_FLUSH:\n");
            //            post_event(env, weak_thiz, MEDIA_NOP, 0, 0);
            break;
        case FFP_MSG_PLAYBACK_STATE_CHANGED:
            LOG_TRACE("FFP_MSG_PLAYBACK_STATE_CHANGED:%d\n", mp->mp_state);
            break;
        default:
            LOG_TRACE("default  msg.what:%d\n", msg.what);
            break;
        }
    }

    return 0;
}
static int message_loop(void *arg)
{
    message_loop_n(arg);


    return 0;
}


IjkPlayerQt::IjkPlayerQt(QObject *parent) :
    QObject(parent),
    mp_(NULL)
{

}

int IjkPlayerQt::Init()
{
    mp_ = ijkmp_qt_create(message_loop_n, (void *)this);
    if(!mp_)
    {
        return -1;
    }
    return 0;
}

void IjkPlayerQt::OnIjkMediaPlayer_prepareAsync()
{
    int retval = 0;


    retval = ijkmp_prepare_async(mp_);
}

void IjkPlayerQt::OnIjkMediaPlayer_setDataSource(const char *url)
{
    int retval = 0;

    retval = ijkmp_set_data_source(mp_, url);
}

void IjkPlayerQt::OnIjkMediaPlayer_stop()
{
    int retval = ijkmp_stop(mp_);
}
