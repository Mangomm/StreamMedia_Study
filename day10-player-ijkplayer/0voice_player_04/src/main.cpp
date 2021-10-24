#include "mainwid.h"
#include <QApplication>
#include <QFontDatabase>
#include <QDebug>

#include "slog.h"

static void _test_log()
{
    LOG_ERROR("this is error log, %s, %d", "test", 123123);
    LOG_WARN("this is warn log, %s, %d", "test", 123123);
    LOG_INFO("this is info log, %s, %d", "test", 123123);
    LOG_DEBUG("this is debug log, %s, %d", "test", 123123);
    LOG_TRACE("this is trace log, %s, %d", "test", 123123);
}
#undef main
int main(int argc, char *argv[])
{
    if (init_logger("./", S_DEBUG) != TRUE) {
        printf("init logger failed\n");
        return -1;
    }

    printf("test begin...\n");
    _test_log();
    QApplication a(argc, argv);
    
    //使用第三方字库，用来作为UI图片 ://res/fa-solid-900.ttf
    QFontDatabase::addApplicationFont("://res/fontawesome-webfont.ttf");
    //QFontDatabase::addApplicationFont("://res/fa-solid-900.ttf");

    MainWid w;
    if (w.Init() == false)
    {
        return -1;
    }
    w.show();

    int ret = a.exec();

    return ret;
}

