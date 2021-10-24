#ifndef IJKPLAYER_QT_H
#define IJKPLAYER_QT_H
#include <QObject>

#include "ijkplayer.h"

typedef struct MediaPlayInfo
{
    char *data_source;      // 要播放的url
    int restart;
    int restart_from_beginning;
    int seek_req;
    long seek_msec;
}MediaPlayInfo;

class IjkPlayerQt: public QObject
{
    Q_OBJECT

public:
    explicit IjkPlayerQt(QObject *parent = nullptr);
    int Init();
    IjkMediaPlayer *GetMediaPlayer() {
        return mp_;
    }

    // 槽响应
    void OnIjkMediaPlayer_prepareAsync();
    void OnIjkMediaPlayer_setDataSource(const char *url);
    void OnIjkMediaPlayer_stop();
private:
    IjkMediaPlayer *mp_;            // 对应的播放器
};

#endif // IJKPLAYER_QT_H
