/*
 * Various utilities for command line tools
 * Copyright (c) 2000-2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>

/* Include only the enabled headers since some compilers (namely, Sun
   Studio) will not omit unused inline functions and create undefined
   references to libraries that are not being built. */

#include "config.h"
#include "compat/va_copy.h"
#include "libavformat/avformat.h"
#include "libavfilter/avfilter.h"
#include "libavdevice/avdevice.h"
#include "libavresample/avresample.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "libpostproc/postprocess.h"
#include "libavutil/attributes.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/display.h"
#include "libavutil/mathematics.h"
#include "libavutil/imgutils.h"
#include "libavutil/libm.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/eval.h"
#include "libavutil/dict.h"
#include "libavutil/opt.h"
#include "libavutil/cpu.h"
#include "libavutil/ffversion.h"
#include "libavutil/version.h"
#include "cmdutils.h"
#if CONFIG_NETWORK
#include "libavformat/network.h"
#endif
#if HAVE_SYS_RESOURCE_H
#include <sys/time.h>
#include <sys/resource.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

static int init_report(const char *env);
// 这5个都是全局的字典
AVDictionary *sws_dict;
AVDictionary *swr_opts;
AVDictionary *format_opts, *codec_opts, *resample_opts;

static FILE *report_file;
static int report_file_level = AV_LOG_DEBUG;
int hide_banner = 0;

enum show_muxdemuxers {
    SHOW_DEFAULT,
    SHOW_DEMUXERS,
    SHOW_MUXERS,
};

void init_opts(void)
{
    av_dict_set(&sws_dict, "flags", "bicubic", 0);
}

void uninit_opts(void)
{
    av_dict_free(&swr_opts);
    av_dict_free(&sws_dict);
    av_dict_free(&format_opts);
    av_dict_free(&codec_opts);
    av_dict_free(&resample_opts);
}

void log_callback_help(void *ptr, int level, const char *fmt, va_list vl)
{
    vfprintf(stdout, fmt, vl);
}

/**
 * @brief ffplay报告日志的回调函数，一旦有调用到av_log的地方，该回调就会被触发，进行写入报告文件report_file。
 *
 * @param ptr 一个万能指针，由FFmpeg自己传入处理，不过debug看到一般都是NULL值。
 * @param level 日志水平，即调用av_log时的参数2，例如AV_LOG_FATAL，值是8，32是AV_LOG_INFO。
 * @param fmt av_log的参3，格式化列表，例如"Invalid %s specification for %s: %s\n"。
 * @param vl 实参的参数列表，例如is_duration ? "duration" : "date", context, timestr。
 *      下面是上面举例的完整语句：
        av_log(NULL, AV_LOG_FATAL, "Invalid %s specification for %s: %s\n", is_duration ? "duration" : "date", context, timestr);

 * @return void。
 */
static void log_callback_report(void *ptr, int level, const char *fmt, va_list vl)
{
    va_list vl2;
    char line[1024];
    static int print_prefix = 1;
    //static int print_prefix = 0;//改成0在日志文件，看前缀好像并未看到太大的区别

    // 关于av_log_default_callback、av_log_format_line可以不必太深入研究。
    va_copy(vl2, vl);
    av_log_default_callback(ptr, level, fmt, vl);
    av_log_format_line(ptr, level, fmt, vl2, line, sizeof(line), &print_prefix);    // 将日志信息保存到line数组中。
    va_end(vl2);

    // 只有level小于报告水平才会被写入到文件，FFmpeg在非负数情况下，level越小，信息越重要。
    if (report_file_level >= level) {
        fputs(line, report_file);// line写入文件，但实际是写到文件缓冲区，还需要调用fflush才能确保每次写入到文件。
        fflush(report_file);
    }
}

/**
 * 程序开头先设置动态库的搜索路径，将当前路径去除(SetDllDirectory函数传空字符串即可)，不做为dll的搜索路径，
 * 防止由于windows下的DLL搜索路径顺序缺陷造成的DLL劫持问题。
 */
void init_dynload(void)
{
#ifdef _WIN32
    /* Calling SetDllDirectory with the empty string (but not NULL) removes the
     * current working directory from the DLL search path as a security pre-caution. */
    // 意思是防止由于windows下的DLL搜索路径顺序造成的DLL劫持问题。
    // 具体看 https://blog.csdn.net/magictong/article/details/6931520。
    SetDllDirectory("");
#endif
}

static void (*program_exit)(int ret);

void register_exit(void (*cb)(int ret))
{
    program_exit = cb;
}

void exit_program(int ret)
{
    if (program_exit)
        program_exit(ret);

    exit(ret);
}

double parse_number_or_die(const char *context, const char *numstr, int type,
                           double min, double max)
{
    char *tail;
    const char *error;
    double d = av_strtod(numstr, &tail);
    if (*tail)
        error = "Expected number for %s but found: %s\n";
    else if (d < min || d > max)
        error = "The value for %s was %s which is not within %f - %f\n";
    else if (type == OPT_INT64 && (int64_t)d != d)
        error = "Expected int64 for %s but found %s\n";
    else if (type == OPT_INT && (int)d != d)
        error = "Expected int for %s but found %s\n";
    else
        return d;
    av_log(NULL, AV_LOG_FATAL, error, context, numstr, min, max);
    exit_program(1);
    return 0;
}

/**
 * @brief 将timestr时间格式字符串 转成 对应的微秒。
 * @param context key，例如"ss"。
 * @param timestr 时间，格式为"hh:mm:ss"，例如"00:00:30"。
 * @param is_duration Duration标志，它告诉如何解释timestr，如果timestr不是0则解释为持续时间，否则解释为日期，所以一般传1表示日期。
 * @return 返回对应的微秒。
 */
int64_t parse_time_or_die(const char *context, const char *timestr,
                          int is_duration)
{
    int64_t us;
    if (av_parse_time(&us, timestr, is_duration) < 0) {
        av_log(NULL, AV_LOG_FATAL, "Invalid %s specification for %s: %s\n",
               is_duration ? "duration" : "date", context, timestr);
        exit_program(1);
    }
    return us;
}

void show_help_options(const OptionDef *options, const char *msg, int req_flags,
                       int rej_flags, int alt_flags)
{
    const OptionDef *po;
    int first;

    first = 1;
    for (po = options; po->name; po++) {
        char buf[64];

        if (((po->flags & req_flags) != req_flags) ||
            (alt_flags && !(po->flags & alt_flags)) ||
            (po->flags & rej_flags))
            continue;

        if (first) {
            printf("%s\n", msg);
            first = 0;
        }
        av_strlcpy(buf, po->name, sizeof(buf));
        if (po->argname) {
            av_strlcat(buf, " ", sizeof(buf));
            av_strlcat(buf, po->argname, sizeof(buf));
        }
        printf("-%-17s  %s\n", buf, po->help);
    }
    printf("\n");
}

void show_help_children(const AVClass *class, int flags)
{
    const AVClass *child = NULL;
    if (class->option) {
        av_opt_show2(&class, NULL, flags, 0);
        printf("\n");
    }

    while (child = av_opt_child_class_next(class, child))
        show_help_children(child, flags);
}

/**
 * @brief 将用户输入的选项与OptionDef数组比较，看是否支持该选项。
 * @param po FFmpeg支持的选项。
 * @param name 用户输入的选项。
 * @return 若用户的选项被找到，则返回单个OptionDef对象，没找到while去到末尾是返回空的变量OptionDef，注并不是空值，而是将该变量置为空，是有内存的。
 */
static const OptionDef *find_option(const OptionDef *po, const char *name)
{
    // strchr()：在字符串str中寻找字符C第一次出现的位置，并返回其位置（地址指针而不是下标），若失败则返回NULL. 例如"hello",找e则返回e的指针，打印出来的话就是"ello"
    // 寻找":"，若找到则说明用户使用冒号的形式进行赋值，选项名字是在":"之前，例如"x:100"，那么选项名字是x，长度是0x1-0x0=1(地址是按照1字节进行递增的)。
    const char *p = strchr(name, ':');
    int len = p ? p - name : strlen(name);

    // 确认用户输入的选项名字后，在OptionDef匹配是否支持该选项。
    while (po->name) {
        // 与上strlen(po->name) == len是因为：例如name是"aa"，po->name是"aac"，那么不与的话，也是满足if，但选项却不完全一样，所以必须加上&&
        if (!strncmp(name, po->name, len) && strlen(po->name) == len){
            break;
        }
        po++;
    }
    return po;
}

/* _WIN32 means using the windows libc - cygwin doesn't define that
 * by default. HAVE_COMMANDLINETOARGVW is true on cygwin, while
 * it doesn't provide the actual command line via GetCommandLineW(). */
#if HAVE_COMMANDLINETOARGVW && defined(_WIN32)
#include <shellapi.h>
/* Will be leaked on exit */
static char** win32_argv_utf8 = NULL;
static int win32_argc = 0;

/**
 * Prepare command line arguments for executable.
 * For Windows - perform wide-char to UTF-8 conversion.
 * Input arguments should be main() function arguments.
 * @param argc_ptr Arguments number (including executable)
 * @param argv_ptr Arguments list.
 */
static void prepare_app_arguments(int *argc_ptr, char ***argv_ptr)
{
    char *argstr_flat;
    wchar_t **argv_w;
    int i, buffsize = 0, offset = 0;

    if (win32_argv_utf8) {
        *argc_ptr = win32_argc;
        *argv_ptr = win32_argv_utf8;
        return;
    }

    win32_argc = 0;
    argv_w = CommandLineToArgvW(GetCommandLineW(), &win32_argc);
    if (win32_argc <= 0 || !argv_w)
        return;

    /* determine the UTF-8 buffer size (including NULL-termination symbols) */
    for (i = 0; i < win32_argc; i++)
        buffsize += WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1,
                                        NULL, 0, NULL, NULL);

    win32_argv_utf8 = av_mallocz(sizeof(char *) * (win32_argc + 1) + buffsize);
    argstr_flat     = (char *)win32_argv_utf8 + sizeof(char *) * (win32_argc + 1);
    if (!win32_argv_utf8) {
        LocalFree(argv_w);
        return;
    }

    for (i = 0; i < win32_argc; i++) {
        win32_argv_utf8[i] = &argstr_flat[offset];
        offset += WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1,
                                      &argstr_flat[offset],
                                      buffsize - offset, NULL, NULL);
    }
    win32_argv_utf8[i] = NULL;
    LocalFree(argv_w);

    *argc_ptr = win32_argc;
    *argv_ptr = win32_argv_utf8;
}
#else
static inline void prepare_app_arguments(int *argc_ptr, char ***argv_ptr)
{
    /* nothing to do */
}
#endif /* HAVE_COMMANDLINETOARGVW */

static int write_option(void *optctx, const OptionDef *po, const char *opt,
                        const char *arg)
{
    /* new-style options contain an offset into optctx, old-style address of
     * a global var*/
    void *dst = po->flags & (OPT_OFFSET | OPT_SPEC) ?
                (uint8_t *)optctx + po->u.off : po->u.dst_ptr;
    int *dstcount;

    if (po->flags & OPT_SPEC) {
        SpecifierOpt **so = dst;
        char *p = strchr(opt, ':');
        char *str;

        dstcount = (int *)(so + 1);
        *so = grow_array(*so, sizeof(**so), dstcount, *dstcount + 1);
        str = av_strdup(p ? p + 1 : "");
        if (!str)
            return AVERROR(ENOMEM);
        (*so)[*dstcount - 1].specifier = str;
        dst = &(*so)[*dstcount - 1].u;
    }

    if (po->flags & OPT_STRING) {
        char *str;
        str = av_strdup(arg);
        av_freep(dst);
        if (!str)
            return AVERROR(ENOMEM);
        *(char **)dst = str;
    } else if (po->flags & OPT_BOOL || po->flags & OPT_INT) {
        *(int *)dst = parse_number_or_die(opt, arg, OPT_INT64, INT_MIN, INT_MAX);
    } else if (po->flags & OPT_INT64) {
        *(int64_t *)dst = parse_number_or_die(opt, arg, OPT_INT64, INT64_MIN, INT64_MAX);
    } else if (po->flags & OPT_TIME) {
        *(int64_t *)dst = parse_time_or_die(opt, arg, 1);
    } else if (po->flags & OPT_FLOAT) {
        *(float *)dst = parse_number_or_die(opt, arg, OPT_FLOAT, -INFINITY, INFINITY);
    } else if (po->flags & OPT_DOUBLE) {
        *(double *)dst = parse_number_or_die(opt, arg, OPT_DOUBLE, -INFINITY, INFINITY);
    } else if (po->u.func_arg) {
        int ret = po->u.func_arg(optctx, opt, arg);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Failed to set value '%s' for option '%s': %s\n",
                   arg, opt, av_err2str(ret));
            return ret;
        }
    }
    if (po->flags & OPT_EXIT)
        exit_program(0);

    return 0;
}

/**
 * @brief 。
 * @param optctx 参数上下文，一般传NULL即可。
 * @param argc 参数key。
 * @param argv 参数value。
 * @param options FFmpeg支持的参数选项，静态数组，类型是OptionDef。
 * @return
 */
int parse_option(void *optctx, const char *opt, const char *arg,
                 const OptionDef *options)
{
    const OptionDef *po;
    int ret;

    // 1. 判断用户的参数是否在选项数组options中，以此标记arg的值。
    po = find_option(options, opt);
    // 1.1 如果没找到并且选项遇到no，则跳过再次寻找。例如"-noi"
    if (!po->name && opt[0] == 'n' && opt[1] == 'o') {
        /* handle 'no' bool option */
        po = find_option(options, opt + 2);
        // 1.2 找到并且是布尔型，arg标志为0。
        if ((po->name && (po->flags & OPT_BOOL)))// OPT_BOOL指：OptionDef的共用体是一个布尔型变量，由于C没有布尔型，所以使用int代替。
            arg = "0";
    } // 1.3 找到并且共用体是布尔型(例如-i)，标志为1.
      // 注：没找到并且共用体是布尔型(例如-moi)应该是不会进来的，因为不满足flags是OPT_BOOL，
      //      此时flags应该是0，因为数组最末尾的变量被置为NULL了，相当于初始化。例如在命令行随机添加个不存在的选项即可
    else if (po->flags & OPT_BOOL)
        arg = "1";

    // 1.4 确认没找到的话，则按默认值"default"去找
    if (!po->name)
        po = find_option(options, "default");
    if (!po->name) {
        av_log(NULL, AV_LOG_ERROR, "Unrecognized option '%s'\n", opt);
        return AVERROR(EINVAL);
    }
    if (po->flags & HAS_ARG && !arg) {
        av_log(NULL, AV_LOG_ERROR, "Missing argument for option '%s'\n", opt);
        return AVERROR(EINVAL);
    }

    ret = write_option(optctx, po, opt, arg);
    if (ret < 0)
        return ret;

    return !!(po->flags & HAS_ARG);
}

/**
 * @brief 。
 * @param optctx 参数上下文，一般传NULL即可。
 * @param argc 参数个数。
 * @param argv 参数列表。
 * @param options FFmpeg支持的参数选项，静态数组。
 * @param parse_arg_function 解析参数函数，即opt_input_file。
 * @return void。
 *
 * 例如调用本函数：parse_options(NULL, argc, argv, options, opt_input_file);
 */
void parse_options(void *optctx, int argc, char **argv, const OptionDef *options,
                   void (*parse_arg_function)(void *, const char*))
{
    const char *opt;
    int optindex, handleoptions = 1, ret;

    /* perform system-dependent conversions for arguments list(对参数列表执行系统依赖的转换)，了解一下即可。 */
    prepare_app_arguments(&argc, &argv);

    /* parse options */
    optindex = 1;
    while (optindex < argc) {// 保证除了程序本身，至少有一个选项
        opt = argv[optindex++];

        if (handleoptions && opt[0] == '-' && opt[1] != '\0') {
            if (opt[1] == '-' && opt[2] == '\0') {// 这种情况："--x"
                handleoptions = 0;
                continue;
            }
            opt++;// 指向后一字符，例如opt="-noi"，opt++后，opt="noi"

            if ((ret = parse_option(optctx, opt, argv[optindex], options)) < 0)
                exit_program(1);
            optindex += ret;
        } else {
            if (parse_arg_function)
                parse_arg_function(optctx, opt);    // opt_input_file回调，赋值给静态变量input_filename。
        }
    }
}

int parse_optgroup(void *optctx, OptionGroup *g)
{
    int i, ret;

    av_log(NULL, AV_LOG_DEBUG, "Parsing a group of options: %s %s.\n",
           g->group_def->name, g->arg);

    for (i = 0; i < g->nb_opts; i++) {
        Option *o = &g->opts[i];

        if (g->group_def->flags &&
            !(g->group_def->flags & o->opt->flags)) {
            av_log(NULL, AV_LOG_ERROR, "Option %s (%s) cannot be applied to "
                   "%s %s -- you are trying to apply an input option to an "
                   "output file or vice versa. Move this option before the "
                   "file it belongs to.\n", o->key, o->opt->help,
                   g->group_def->name, g->arg);
            return AVERROR(EINVAL);
        }

        av_log(NULL, AV_LOG_DEBUG, "Applying option %s (%s) with argument %s.\n",
               o->key, o->opt->help, o->val);

        ret = write_option(optctx, o->opt, o->key, o->val);
        if (ret < 0)
            return ret;
    }

    av_log(NULL, AV_LOG_DEBUG, "Successfully parsed a group of options.\n");

    return 0;
}

/**
 * @brief 不管输入参数在FFmpeg是否支持，只要我要找的内容在用户输入有这个参数，那我就返回这个下标。
 * @param argc 参数个数。
 * @param argv 参数列表。
 * @param options FFmpeg支持的参数选项，静态数组。
 * @param optname 要寻找的参数。
 * @return 找到下标则返回对应下标，找不到则返回0.
 * 可以通过命令增加删减选项去debug该函数：
 *  -noi rtsp://admin:runone2016@192.168.1.186:554/Streaming/Channels/1 -tyy -loglevel 8 -fflags nobuffer -x 720 -y 480
 *
 * 实际上本函数不一定非得按照FFmpeg的做法，可以按照自己的实际需求进行修改。
 */
int locate_option(int argc, char **argv, const OptionDef *options,
                  const char *optname)
{
    const OptionDef *po;
    int i;

    for (i = 1; i < argc; i++) {
        const char *cur_opt = argv[i];
        printf("i: %d\n", i);
        if (*cur_opt++ != '-')      // 这里会一直过滤空格，直至找到"-"，例如命令：ffplay.exe -i a.mp4，跳过argv[0]，那么就从参数-i开始判断if，找到后，cur_opt指向选项的首个字符。
            continue;

        // 1. 判断每个参数是否被FFmpeg支持. 将optname="v"代进来，命令行不带"v"选项，那么肯定不满足2的if条件，不返回下标i。
        po = find_option(options, cur_opt);
        if (!po->name && cur_opt[0] == 'n' && cur_opt[1] == 'o')    // 这里的if意思是：若遇到选项以no开头的，则跳过no再次查找选项。例如添加选项-noi，同样是当成-i选项
            po = find_option(options, cur_opt + 2);

        // 2. 根据optname，匹配用户输入的参数的下标，若用户输入了optname参数，则返回对应的下标。
        // 1）如果po->name是空并且要找的选项等于用户输入的参数，那么返回用户输入参数的下标。
        //      即意思是：FFmpeg不支持的选项(!po->name的意思)，但是我要找的选项名字在用户输入的参数中找到了。
        // 2）或者po->name不是空并且要找的选项等于po->name，那么同样返回用户输入参数的下标。
        //      即意思是：用户输入的参数在FFmpeg支持的选项，并且要找的选项名字在用户输入的参数中找到了，此时cur_opt、po->name是一样的。
        // 理解关键和总结：
        //      1. 用户参数不在po的情况下，以用户输入参数判断并返回下标；用户参数在po的情况下，优先以po->name判断并返回下标，实际找到的时候cur_opt、po->name是一样的。
        //      2. cur_opt是用户输入的参数列表；po->name是FFmpeg支持的参数列表；optname是我们要寻找的参数名字。
        //      3. 感觉这个if有点别扭难理解，目的应该就是根据optname寻找用户输出参数的下标。
        //      4. 最好的解释是：不管输入参数在FFmpeg是否支持，只要我要找的内容在用户输入有这个参数，那我就返回这个下标。
        // 测试(!po->name && !strcmp(cur_opt, optname))：在parse_loglevel函数添加testidx那行的代码，然后命令行添加-tyy即可满足，这个条件感觉可以让程序支持自定义的参数选项。
        // 测试(po->name && !strcmp(optname, po->name))：例如我要寻找的optname是loglevel，那我在命令行添加-loglevel 8即可满足测试。
        if ((!po->name && !strcmp(cur_opt, optname)) || (po->name && !strcmp(optname, po->name)))
            return i;

        // 3. 没找到对应的po也会++，他应该默认按照当有非法参数时，非法参数的形式是选项+值的方式，例如：-abc 1。
        // po->flags & HAS_ARG代表：若选项是有参数的，则跳过该参数，例如-x 10，则会跳过10这个选项。
        if (!po->name || po->flags & HAS_ARG)
            i++;
    }
    return 0;
}

/**
 * @brief 就是dump参数，没什么参考意义。
 * @param a 用户输入的参数列表的其中一个参数。
 * @return void。
 */
static void dump_argument(const char *a)
{
    const unsigned char *p;

    // 1. 检测输入的参数的每个字符串是否是合法字符。合法的有：43-58；64-90；95；97-122。其余都是非法字符，直接退出for。
    // 所以遇到"\"反斜杠值是92，for就会停止。
    for (p = a; *p; p++)
        if (!((*p >= '+' && *p <= ':') || (*p >= '@' && *p <= 'Z') ||
              *p == '_' || (*p >= 'a' && *p <= 'z')))
            break;

    // 2. 判断p是否是因为遇到空而退出，若是直接写入a后返回即可。但是p此时指向反斜杠，p不是空，所以不会进入。
    // 例如此时p="\Users\DELL\Desktop\StreamMedia_Study\day11-12player-ffplay\build-ffplay-pro-Desktop_Qt_5_10_1_Min"
    if (!*p) {
        fputs(a, report_file);
        return;
    }

    fputc('"', report_file);    // 先写入"双引号的一个引号。

    // 3. 开始dump参数。
    for (p = a; *p; p++) {
        if (*p == '\\' || *p == '"' || *p == '$' || *p == '`')
            fprintf(report_file, "\\%c", *p);
        else if (*p < ' ' || *p > '~')
            fprintf(report_file, "\\x%02x", *p);
        else
            fputc(*p, report_file);
    }

    fputc('"', report_file);    // 最后写入另一个双引号。
}

/**
 * @brief 大概就是检查OptionDef数组是否带有OPT_PERFILE选项，
 *          若带有该选项，则必须同时包含(OPT_INPUT | OPT_OUTPUT)，否则断言。
 *          关于OPT_PERFILE的具体意思，后面可以再看看。
 * @param po FFmpeg支持的参数选项，静态数组。
 * @return void。
 *
 * #define av_assert0(cond) do {                                        \
    if (!(cond)) {                                                      \
        av_log(NULL, AV_LOG_PANIC, "Assertion %s failed at %s:%d\n",    \
               AV_STRINGIFY(cond), __FILE__, __LINE__);                 \
        abort();                                                        \
    }                                                                   \
} while (0)
 */
static void check_options(const OptionDef *po)
{
    while (po->name) {
        if (po->flags & OPT_PERFILE)
            av_assert0(po->flags & (OPT_INPUT | OPT_OUTPUT));
        po++;
    }
}

/**
 * @brief 解析日志相关以及控制控制台开始时输出内容多少的函数。
 * @param argc 参数个数。
 * @param argv 参数列表。
 * @param options FFmpeg支持的参数选项，静态数组。
 * @return void。
 */
void parse_loglevel(int argc, char **argv, const OptionDef *options)
{
    //int testidx = locate_option(argc, argv, options, "tyy");// tyy代码，用于测试
    // 1. 找到loglevel在用户输入的参数列表中的下标。
    int idx = locate_option(argc, argv, options, "loglevel");
    const char *env;

    // 检查OptionDef数组是否带有OPT_PERFILE选项
    check_options(options);

    // 2. 如果在用户输入的参数没找到loglevel参数，则判断用户是否输入了日志"v"的缩写参数。
    // 注意，虽然在OptionDef数组带有"v"，缩写，但是用户参数不输入"v"，返回依然是0，不要被误解了(see locate_option函数)。
    // locate_option会根据参4在用户与OptionDef数组进行查找是否含该参数，找到就返回下标。
    // 这段代码可以使用-noi a.mp4 -x 720 -y 480测试，即不带loglevel、v选项即可。
    if (!idx){
        idx = locate_option(argc, argv, options, "v");
    }

    // 3. 如果找到loglevel或者v参数，则设置日志水平。
    if (idx && argv[idx + 1]){
        opt_loglevel(NULL, "loglevel", argv[idx + 1]);
    }

    // 4. 如果用户输入了report参数或者存在FFREPORT环境变量，则初始化报告，可以通过在命令行添加"-report"选项即可，
    // 不加参数，设置FFREPORT环境变量效果一样。在构建目录生成对应的xxx.log，是控制台的打印信息。
    // getenv：想要获取的环境变量名。调用成功返回指向 value 的指针，失败返回 NULL。
    idx = locate_option(argc, argv, options, "report");
    if ((env = getenv("FFREPORT")) || idx) {

        init_report(env);

        // 这里是dump一下参数，在控制台和报告的日志可以看到打印。
        if (report_file) {
            int i;
            fprintf(report_file, "Command line:\n");
            for (i = 0; i < argc; i++) {
                dump_argument(argv[i]);
                fputc(i < argc - 1 ? ' ' : '\n', report_file);
            }
            fflush(report_file);
        }

    }

    // 5. 查找用户是否输入hide_banner参数，有则将全局变量hide_banner置1.
    // 若带了-hide_banner参数(不用值)，则在show_banner不会显示对应的版权、配置、动态库版本信息。
    idx = locate_option(argc, argv, options, "hide_banner");
    if (idx)
        hide_banner = 1;
}

static const AVOption *opt_find(void *obj, const char *name, const char *unit,
                            int opt_flags, int search_flags)
{
    const AVOption *o = av_opt_find(obj, name, unit, opt_flags, search_flags);
    if(o && !o->flags)
        return NULL;
    return o;
}

#define FLAGS (o->type == AV_OPT_TYPE_FLAGS && (arg[0]=='-' || arg[0]=='+')) ? AV_DICT_APPEND : 0
int opt_default(void *optctx, const char *opt, const char *arg)
{
    const AVOption *o;
    int consumed = 0;
    char opt_stripped[128];
    const char *p;
    const AVClass *cc = avcodec_get_class(), *fc = avformat_get_class();
#if CONFIG_AVRESAMPLE
    const AVClass *rc = avresample_get_class();
#endif
#if CONFIG_SWSCALE
    const AVClass *sc = sws_get_class();
#endif
#if CONFIG_SWRESAMPLE
    const AVClass *swr_class = swr_get_class();
#endif

    if (!strcmp(opt, "debug") || !strcmp(opt, "fdebug"))
        av_log_set_level(AV_LOG_DEBUG);

    if (!(p = strchr(opt, ':')))
        p = opt + strlen(opt);
    av_strlcpy(opt_stripped, opt, FFMIN(sizeof(opt_stripped), p - opt + 1));

    if ((o = opt_find(&cc, opt_stripped, NULL, 0,
                         AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ)) ||
        ((opt[0] == 'v' || opt[0] == 'a' || opt[0] == 's') &&
         (o = opt_find(&cc, opt + 1, NULL, 0, AV_OPT_SEARCH_FAKE_OBJ)))) {
        av_dict_set(&codec_opts, opt, arg, FLAGS);
        consumed = 1;
    }
    if ((o = opt_find(&fc, opt, NULL, 0,
                         AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ))) {
        int tyycodeFlags = FLAGS;// 该条目的类型，nobuffer时为0
        av_dict_set(&format_opts, opt, arg, FLAGS);
        if (consumed)
            av_log(NULL, AV_LOG_VERBOSE, "Routing option %s to both codec and muxer layer\n", opt);
        consumed = 1;
    }
#if CONFIG_SWSCALE
    if (!consumed && (o = opt_find(&sc, opt, NULL, 0,
                         AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ))) {
        struct SwsContext *sws = sws_alloc_context();
        int ret = av_opt_set(sws, opt, arg, 0);
        sws_freeContext(sws);
        if (!strcmp(opt, "srcw") || !strcmp(opt, "srch") ||
            !strcmp(opt, "dstw") || !strcmp(opt, "dsth") ||
            !strcmp(opt, "src_format") || !strcmp(opt, "dst_format")) {
            av_log(NULL, AV_LOG_ERROR, "Directly using swscale dimensions/format options is not supported, please use the -s or -pix_fmt options\n");
            return AVERROR(EINVAL);
        }
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error setting option %s.\n", opt);
            return ret;
        }

        av_dict_set(&sws_dict, opt, arg, FLAGS);

        consumed = 1;
    }
#else
    if (!consumed && !strcmp(opt, "sws_flags")) {
        av_log(NULL, AV_LOG_WARNING, "Ignoring %s %s, due to disabled swscale\n", opt, arg);
        consumed = 1;
    }
#endif
#if CONFIG_SWRESAMPLE
    if (!consumed && (o=opt_find(&swr_class, opt, NULL, 0,
                                    AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ))) {
        struct SwrContext *swr = swr_alloc();
        int ret = av_opt_set(swr, opt, arg, 0);
        swr_free(&swr);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error setting option %s.\n", opt);
            return ret;
        }
        av_dict_set(&swr_opts, opt, arg, FLAGS);
        consumed = 1;
    }
#endif
#if CONFIG_AVRESAMPLE
    if ((o=opt_find(&rc, opt, NULL, 0,
                       AV_OPT_SEARCH_CHILDREN | AV_OPT_SEARCH_FAKE_OBJ))) {
        av_dict_set(&resample_opts, opt, arg, FLAGS);
        consumed = 1;
    }
#endif

    if (consumed)
        return 0;
    return AVERROR_OPTION_NOT_FOUND;
}

/*
 * Check whether given option is a group separator.
 *
 * @return index of the group definition that matched or -1 if none
 */
static int match_group_separator(const OptionGroupDef *groups, int nb_groups,
                                 const char *opt)
{
    int i;

    for (i = 0; i < nb_groups; i++) {
        const OptionGroupDef *p = &groups[i];
        if (p->sep && !strcmp(p->sep, opt))
            return i;
    }

    return -1;
}

/*
 * Finish parsing an option group.
 *
 * @param group_idx which group definition should this group belong to
 * @param arg argument of the group delimiting option
 */
static void finish_group(OptionParseContext *octx, int group_idx,
                         const char *arg)
{
    OptionGroupList *l = &octx->groups[group_idx];
    OptionGroup *g;

    GROW_ARRAY(l->groups, l->nb_groups);
    g = &l->groups[l->nb_groups - 1];

    *g             = octx->cur_group;
    g->arg         = arg;
    g->group_def   = l->group_def;
    g->sws_dict    = sws_dict;
    g->swr_opts    = swr_opts;
    g->codec_opts  = codec_opts;
    g->format_opts = format_opts;
    g->resample_opts = resample_opts;

    codec_opts  = NULL;
    format_opts = NULL;
    resample_opts = NULL;
    sws_dict    = NULL;
    swr_opts    = NULL;
    init_opts();

    memset(&octx->cur_group, 0, sizeof(octx->cur_group));
}

/*
 * Add an option instance to currently parsed group.
 */
static void add_opt(OptionParseContext *octx, const OptionDef *opt,
                    const char *key, const char *val)
{
    int global = !(opt->flags & (OPT_PERFILE | OPT_SPEC | OPT_OFFSET));
    OptionGroup *g = global ? &octx->global_opts : &octx->cur_group;

    GROW_ARRAY(g->opts, g->nb_opts);
    g->opts[g->nb_opts - 1].opt = opt;
    g->opts[g->nb_opts - 1].key = key;
    g->opts[g->nb_opts - 1].val = val;
}

static void init_parse_context(OptionParseContext *octx,
                               const OptionGroupDef *groups, int nb_groups)
{
    static const OptionGroupDef global_group = { "global" };
    int i;

    memset(octx, 0, sizeof(*octx));

    octx->nb_groups = nb_groups;
    octx->groups    = av_mallocz_array(octx->nb_groups, sizeof(*octx->groups));
    if (!octx->groups)
        exit_program(1);

    for (i = 0; i < octx->nb_groups; i++)
        octx->groups[i].group_def = &groups[i];

    octx->global_opts.group_def = &global_group;
    octx->global_opts.arg       = "";

    init_opts();
}

void uninit_parse_context(OptionParseContext *octx)
{
    int i, j;

    for (i = 0; i < octx->nb_groups; i++) {
        OptionGroupList *l = &octx->groups[i];

        for (j = 0; j < l->nb_groups; j++) {
            av_freep(&l->groups[j].opts);
            av_dict_free(&l->groups[j].codec_opts);
            av_dict_free(&l->groups[j].format_opts);
            av_dict_free(&l->groups[j].resample_opts);

            av_dict_free(&l->groups[j].sws_dict);
            av_dict_free(&l->groups[j].swr_opts);
        }
        av_freep(&l->groups);
    }
    av_freep(&octx->groups);

    av_freep(&octx->cur_group.opts);
    av_freep(&octx->global_opts.opts);

    uninit_opts();
}

int split_commandline(OptionParseContext *octx, int argc, char *argv[],
                      const OptionDef *options,
                      const OptionGroupDef *groups, int nb_groups)
{
    int optindex = 1;
    int dashdash = -2;

    /* perform system-dependent conversions for arguments list */
    prepare_app_arguments(&argc, &argv);

    init_parse_context(octx, groups, nb_groups);
    av_log(NULL, AV_LOG_DEBUG, "Splitting the commandline.\n");

    while (optindex < argc) {
        const char *opt = argv[optindex++], *arg;
        const OptionDef *po;
        int ret;

        av_log(NULL, AV_LOG_DEBUG, "Reading option '%s' ...", opt);

        if (opt[0] == '-' && opt[1] == '-' && !opt[2]) {
            dashdash = optindex;
            continue;
        }
        /* unnamed group separators, e.g. output filename */
        if (opt[0] != '-' || !opt[1] || dashdash+1 == optindex) {
            finish_group(octx, 0, opt);
            av_log(NULL, AV_LOG_DEBUG, " matched as %s.\n", groups[0].name);
            continue;
        }
        opt++;

#define GET_ARG(arg)                                                           \
do {                                                                           \
    arg = argv[optindex++];                                                    \
    if (!arg) {                                                                \
        av_log(NULL, AV_LOG_ERROR, "Missing argument for option '%s'.\n", opt);\
        return AVERROR(EINVAL);                                                \
    }                                                                          \
} while (0)

        /* named group separators, e.g. -i */
        if ((ret = match_group_separator(groups, nb_groups, opt)) >= 0) {
            GET_ARG(arg);
            finish_group(octx, ret, arg);
            av_log(NULL, AV_LOG_DEBUG, " matched as %s with argument '%s'.\n",
                   groups[ret].name, arg);
            continue;
        }

        /* normal options */
        po = find_option(options, opt);
        if (po->name) {
            if (po->flags & OPT_EXIT) {
                /* optional argument, e.g. -h */
                arg = argv[optindex++];
            } else if (po->flags & HAS_ARG) {
                GET_ARG(arg);
            } else {
                arg = "1";
            }

            add_opt(octx, po, opt, arg);
            av_log(NULL, AV_LOG_DEBUG, " matched as option '%s' (%s) with "
                   "argument '%s'.\n", po->name, po->help, arg);
            continue;
        }

        /* AVOptions */
        if (argv[optindex]) {
            ret = opt_default(NULL, opt, argv[optindex]);
            if (ret >= 0) {
                av_log(NULL, AV_LOG_DEBUG, " matched as AVOption '%s' with "
                       "argument '%s'.\n", opt, argv[optindex]);
                optindex++;
                continue;
            } else if (ret != AVERROR_OPTION_NOT_FOUND) {
                av_log(NULL, AV_LOG_ERROR, "Error parsing option '%s' "
                       "with argument '%s'.\n", opt, argv[optindex]);
                return ret;
            }
        }

        /* boolean -nofoo options */
        if (opt[0] == 'n' && opt[1] == 'o' &&
            (po = find_option(options, opt + 2)) &&
            po->name && po->flags & OPT_BOOL) {
            add_opt(octx, po, opt, "0");
            av_log(NULL, AV_LOG_DEBUG, " matched as option '%s' (%s) with "
                   "argument 0.\n", po->name, po->help);
            continue;
        }

        av_log(NULL, AV_LOG_ERROR, "Unrecognized option '%s'.\n", opt);
        return AVERROR_OPTION_NOT_FOUND;
    }

    if (octx->cur_group.nb_opts || codec_opts || format_opts || resample_opts)
        av_log(NULL, AV_LOG_WARNING, "Trailing options were found on the "
               "commandline.\n");

    av_log(NULL, AV_LOG_DEBUG, "Finished splitting the commandline.\n");

    return 0;
}

int opt_cpuflags(void *optctx, const char *opt, const char *arg)
{
    int ret;
    unsigned flags = av_get_cpu_flags();

    if ((ret = av_parse_cpu_caps(&flags, arg)) < 0)
        return ret;

    av_force_cpu_flags(flags);
    return 0;
}

/**
 * @brief 对日志水平获取和设置的函数，简单了解一下即可。例如传参：NULL, "loglevel", argv[idx + 1]。 argv[idx + 1]是数值，例如8.
 * @param optctx 选项上下文，一般传参即可。
 * @param opt   日志选项，例如"loglevel"。
 * @param arg   日志水平，例如+8或者-8或者8。
 * @return  no mean。
 */
int opt_loglevel(void *optctx, const char *opt, const char *arg)
{
    const struct { const char *name; int level; } log_levels[] = {
        { "quiet"  , AV_LOG_QUIET   },          // 平静的
        { "panic"  , AV_LOG_PANIC   },          // 恐慌的
        { "fatal"  , AV_LOG_FATAL   },
        { "error"  , AV_LOG_ERROR   },
        { "warning", AV_LOG_WARNING },
        { "info"   , AV_LOG_INFO    },
        { "verbose", AV_LOG_VERBOSE },          // 详细的
        { "debug"  , AV_LOG_DEBUG   },
        { "trace"  , AV_LOG_TRACE   },          // 跟踪
    };
    const char *token;
    char *tail;
    int flags = av_log_get_flags();// 日志标志，可能是 AV_LOG_SKIP_REPEATED 或者 AV_LOG_PRINT_LEVEL，程序开头设置了。
    int level = av_log_get_level();// 获取当前日志水平。
    int cmd, i = 0;

    av_assert0(arg);                // 断言，arg不能是空
    while (*arg) {
        token = arg;                // 临时操作arg的指针
        if (*token == '+' || *token == '-') {// 支持正负号的输入，若用户输入正负号，cmd不为空
            cmd = *token++;
        } else {
            cmd = 0;
        }

        if (!i && !cmd) {
            flags = 0;  /* missing relative prefix, build absolute value(用户没有传入正负号的前缀，重置flags) */
        }

        if (!strncmp(token, "repeat", 6)) {
            if (cmd == '-') {
                flags |= AV_LOG_SKIP_REPEATED;
            } else {
                flags &= ~AV_LOG_SKIP_REPEATED;
            }
            arg = token + 6;
        } else if (!strncmp(token, "level", 5)) {
            if (cmd == '-') {
                flags &= ~AV_LOG_PRINT_LEVEL;
            } else {
                flags |= AV_LOG_PRINT_LEVEL;
            }
            arg = token + 5;
        } else {
            break;
        }
        i++;
    }

    if (!*arg) {
        goto end;
    } else if (*arg == '+') {// 跳过正负号，拿到数字
        arg++;
    } else if (!i) {
        flags = av_log_get_flags();  /* level value without prefix, reset flags(如果水平没有正负号的前缀，重置flags) */
    }

    for (i = 0; i < FF_ARRAY_ELEMS(log_levels); i++) {
        if (!strcmp(log_levels[i].name, arg)) {
            level = log_levels[i].level;
            goto end;
        }
    }

    level = strtol(arg, &tail, 10);
    if (*tail) {
        av_log(NULL, AV_LOG_FATAL, "Invalid loglevel \"%s\". "
               "Possible levels are numbers or:\n", arg);
        for (i = 0; i < FF_ARRAY_ELEMS(log_levels); i++)
            av_log(NULL, AV_LOG_FATAL, "\"%s\"\n", log_levels[i].name);
        exit_program(1);
    }

end:
    av_log_set_flags(flags);
    av_log_set_level(level);
    return 0;
}

/**
 * @brief 按照template的格式组成对应的字符串，保存在bp->str中。
 *
 * @param bp 一个字符串缓存区，可以认为类似C++的string或者当做一个自动改变大小的buffer数组即可。
 * @param template 文件名的模板，例如"%p-%t.log"，配合参数tm，处理后会变成"ffplay-20211028-105917.log"。
 * @param tm 获取的当前时间。
 * @return void。
 *
 * 以参2 = "%p-%t.log"为例理解该函数即可。
*/
static void expand_filename_template(AVBPrint *bp, const char *template,
                                     struct tm *tm)
{
    int c;

    while ((c = *(template++))) {
        if (c == '%') {
            if (!(c = *(template++)))// 如果%的下一个字符为空，则break
                break;
            switch (c) {
            case 'p':
                // 往输出缓冲区bp添加"ffplay"，program_name在ffplay.c中被赋值了。此时bp->str="ffplay"
                av_bprintf(bp, "%s", program_name);
                break;
            case 't':
                av_bprintf(bp, "%04d%02d%02d-%02d%02d%02d",
                           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                           tm->tm_hour, tm->tm_min, tm->tm_sec);// 此时bp->str="ffplay-20211028-154930"
                break;
            case '%':
                av_bprint_chars(bp, c, 1);
                break;
            }
        } else {
            // 这里的逻辑一般是对应%以及%后一个字符以外的字符，以"%p-%t.log"为例。第一次进入时：bp->str="ffplay-"
            // 后面执行完所以的%后，剩下".log"都会执行这里的逻辑，最后处理会变成"ffplay-20211028-105917.log"。
            av_bprint_chars(bp, c, 1);
        }
    }
}

/**
 * @brief 对FFmpeg报告文件的处理，即在控制台的输出的内容保存到日志文件中。
 * @param env 查找到"FFREPORT"的环境变量value值或者用户输入参数"-report"的value值。
 * @return 0 成功 负数 失败。
 */
static int init_report(const char *env)
{
    char *filename_template = NULL;
    char *key, *val;
    int ret, count = 0;
    time_t now;
    struct tm *tm;
    AVBPrint filename;

    // 1. 报告文件已经打开则返回0. report_file是一个全局静态的文件变量。
    if (report_file) /* already opened */
        return 0;

    time(&now);
    tm = localtime(&now);

    while (env && *env) {
        // 这里的if应该是提取-report输入时的参数，应该支持多个参数(例如下面的file、level)，这里就不测试了。
        if ((ret = av_opt_get_key_value(&env, "=", ":", 0, &key, &val)) < 0) {
            if (count)
                av_log(NULL, AV_LOG_ERROR,
                       "Failed to parse FFREPORT environment variable: %s\n",
                       av_err2str(ret));
            break;
        }

        if (*env){
            env++;
        }

        count++;
        if (!strcmp(key, "file")) {
            av_free(filename_template);// av_free释放NULL指针也是安全的。
            filename_template = val;
            val = NULL;
        } else if (!strcmp(key, "level")) {
            char *tail;
            // report_file_level全局变量，指报告文件的日志水平，默认debug。
            // strtol函数在数据库连接池使用过，它有很多类似的函数，不算难。参1是传入字符串，参2是参3设置好后，参1的非法字符串，一般不关心都是置NULL。参3是2-36进制的范围。
            // 返回值是遇到非法字符串前的合法字符的值。  具体可以看https://blog.csdn.net/zxx2096/article/details/81127858的例子，很简单。
            report_file_level = strtol(val, &tail, 10);
            if (*tail) {// tail不为空说明有非法字符串。
                av_log(NULL, AV_LOG_FATAL, "Invalid report file level\n");
                exit_program(1);// 执行预先注册的退出函数，没有注册则调用exit()。
            }
        } else {
            av_log(NULL, AV_LOG_ERROR, "Unknown key '%s' in FFREPORT\n", key);
        }
        av_free(val);
        av_free(key);
    }

    // 初始化AVBPrint，参2设置打印缓冲大小是0，但是参3可以自动根据需要，重新开辟buffer。具体看FFmpeg的函数注释。
    av_bprint_init(&filename, 0, AV_BPRINT_SIZE_AUTOMATIC);
    // 将"%p-%t.log"展开成对应的字符串，保存在filename->str中。
    expand_filename_template(&filename, av_x_if_null(filename_template, "%p-%t.log"), tm);
    av_free(filename_template);

    /*
     * 测试打印缓冲区是否完整(未被截断)。
     * 可能由于内存分配失败而被截断或size_max限制(如果需要比较size和size_max)。
     * true:代表开辟的内存>字符串大小，是正常的。 false：表示开辟的内存不足以存放str文件名，需要报错。
    */
    if (!av_bprint_is_complete(&filename)) {
        av_log(NULL, AV_LOG_ERROR, "Out of memory building report file name\n");
        return AVERROR(ENOMEM);
    }

    report_file = fopen(filename.str, "w");
    if (!report_file) {
        int ret = AVERROR(errno);
        av_log(NULL, AV_LOG_ERROR, "Failed to open report \"%s\": %s\n",
               filename.str, strerror(errno));
        return ret;
    }

    // 设置日志回调，通过日志回调将日志写入到报告文件中。回调函数要求是多线程安全的，即使本身不是多线程，但是编解码时是多线程的。
    av_log_set_callback(log_callback_report);
    av_log(NULL, AV_LOG_INFO,
           "%s started on %04d-%02d-%02d at %02d:%02d:%02d\n"
           "Report written to \"%s\"\n",
           program_name,
           tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
           tm->tm_hour, tm->tm_min, tm->tm_sec,
           filename.str);

    // 通过debug发现，av_bprint_finalize的作用是将AVBPrint.size多余的内存释放掉，变成实际长度+0的长度，例如文件长26字节，size原本开辟1008长度，执行完后，size变成27(26+1)。
    // av_bprint_finalize参2：如果不是NULL，用于返回缓冲区内容，如果内存分配失败，则为NULL;如果为NULL，缓冲区将被丢弃并释放，这里传NULL即可。
    av_bprint_finalize(&filename, NULL);

    return 0;
}

int opt_report(const char *opt)
{
    return init_report(NULL);
}

int opt_max_alloc(void *optctx, const char *opt, const char *arg)
{
    char *tail;
    size_t max;

    max = strtol(arg, &tail, 10);
    if (*tail) {
        av_log(NULL, AV_LOG_FATAL, "Invalid max_alloc \"%s\".\n", arg);
        exit_program(1);
    }
    av_max_alloc(max);
    return 0;
}

int opt_timelimit(void *optctx, const char *opt, const char *arg)
{
#if HAVE_SETRLIMIT
    int lim = parse_number_or_die(opt, arg, OPT_INT64, 0, INT_MAX);
    struct rlimit rl = { lim, lim + 1 };
    if (setrlimit(RLIMIT_CPU, &rl))
        perror("setrlimit");
#else
    av_log(NULL, AV_LOG_WARNING, "-%s not implemented on this OS\n", opt);
#endif
    return 0;
}

void print_error(const char *filename, int err)
{
    char errbuf[128];
    const char *errbuf_ptr = errbuf;

    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));
    av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, errbuf_ptr);
}

static int warned_cfg = 0;

#define INDENT        1
#define SHOW_VERSION  2
#define SHOW_CONFIG   4
#define SHOW_COPYRIGHT 8

#define PRINT_LIB_INFO(libname, LIBNAME, flags, level)                  \
    if (CONFIG_##LIBNAME) {                                             \
        const char *indent = flags & INDENT? "  " : "";                 \
        if (flags & SHOW_VERSION) {                                     \
            unsigned int version = libname##_version();                 \
            av_log(NULL, level,                                         \
                   "%slib%-11s %2d.%3d.%3d / %2d.%3d.%3d\n",            \
                   indent, #libname,                                    \
                   LIB##LIBNAME##_VERSION_MAJOR,                        \
                   LIB##LIBNAME##_VERSION_MINOR,                        \
                   LIB##LIBNAME##_VERSION_MICRO,                        \
                   AV_VERSION_MAJOR(version), AV_VERSION_MINOR(version),\
                   AV_VERSION_MICRO(version));                          \
        }                                                               \
        if (flags & SHOW_CONFIG) {                                      \
            const char *cfg = libname##_configuration();                \
            if (strcmp(FFMPEG_CONFIGURATION, cfg)) {                    \
                if (!warned_cfg) {                                      \
                    av_log(NULL, level,                                 \
                            "%sWARNING: library configuration mismatch\n", \
                            indent);                                    \
                    warned_cfg = 1;                                     \
                }                                                       \
                av_log(NULL, level, "%s%-11s configuration: %s\n",      \
                        indent, #libname, cfg);                         \
            }                                                           \
        }                                                               \
    }                                                                   \

static void print_all_libs_info(int flags, int level)
{
    PRINT_LIB_INFO(avutil,     AVUTIL,     flags, level);
    PRINT_LIB_INFO(avcodec,    AVCODEC,    flags, level);
    PRINT_LIB_INFO(avformat,   AVFORMAT,   flags, level);
    PRINT_LIB_INFO(avdevice,   AVDEVICE,   flags, level);
    PRINT_LIB_INFO(avfilter,   AVFILTER,   flags, level);
    PRINT_LIB_INFO(avresample, AVRESAMPLE, flags, level);
    PRINT_LIB_INFO(swscale,    SWSCALE,    flags, level);
    PRINT_LIB_INFO(swresample, SWRESAMPLE, flags, level);
    PRINT_LIB_INFO(postproc,   POSTPROC,   flags, level);
}

static void print_program_info(int flags, int level)
{
    const char *indent = flags & INDENT? "  " : "";

    av_log(NULL, level, "%s version " FFMPEG_VERSION, program_name);
    if (flags & SHOW_COPYRIGHT)
        av_log(NULL, level, " Copyright (c) %d-%d the FFmpeg developers",
               program_birth_year, CONFIG_THIS_YEAR);
    av_log(NULL, level, "\n");
    av_log(NULL, level, "%sbuilt with %s\n", indent, CC_IDENT);

    av_log(NULL, level, "%sconfiguration: " FFMPEG_CONFIGURATION "\n", indent);
}

static void print_buildconf(int flags, int level)
{
    const char *indent = flags & INDENT ? "  " : "";
    char str[] = { FFMPEG_CONFIGURATION };
    char *conflist, *remove_tilde, *splitconf;

    // Change all the ' --' strings to '~--' so that
    // they can be identified as tokens.
    while ((conflist = strstr(str, " --")) != NULL) {
        strncpy(conflist, "~--", 3);
    }

    // Compensate for the weirdness this would cause
    // when passing 'pkg-config --static'.
    while ((remove_tilde = strstr(str, "pkg-config~")) != NULL) {
        strncpy(remove_tilde, "pkg-config ", 11);
    }

    splitconf = strtok(str, "~");
    av_log(NULL, level, "\n%sconfiguration:\n", indent);
    while (splitconf != NULL) {
        av_log(NULL, level, "%s%s%s\n", indent, indent, splitconf);
        splitconf = strtok(NULL, "~");
    }
}

/**
 * @brief 在控制台输出对应的版权、配置、动态库版本信息。可以通过命令行输入 -hide_banner 隐藏。
 * @param argc 参数个数。
 * @param argv 参数列表。
 * @param options FFmpeg支持的参数选项，静态数组。
 * @return void。
 */
void show_banner(int argc, char **argv, const OptionDef *options)
{
    int idx = locate_option(argc, argv, options, "version");
    if (hide_banner || idx)
        return;

    // 显示对应的版权、配置、动态库版本信息。可以通过命令行输入 -hide_banner 隐藏。
    print_program_info (INDENT|SHOW_COPYRIGHT, AV_LOG_INFO);
    print_all_libs_info(INDENT|SHOW_CONFIG,  AV_LOG_INFO);
    print_all_libs_info(INDENT|SHOW_VERSION, AV_LOG_INFO);
}

int show_version(void *optctx, const char *opt, const char *arg)
{
    av_log_set_callback(log_callback_help);
    print_program_info (SHOW_COPYRIGHT, AV_LOG_INFO);
    print_all_libs_info(SHOW_VERSION, AV_LOG_INFO);

    return 0;
}

int show_buildconf(void *optctx, const char *opt, const char *arg)
{
    av_log_set_callback(log_callback_help);
    print_buildconf      (INDENT|0, AV_LOG_INFO);

    return 0;
}

int show_license(void *optctx, const char *opt, const char *arg)
{
#if CONFIG_NONFREE
    printf(
    "This version of %s has nonfree parts compiled in.\n"
    "Therefore it is not legally redistributable.\n",
    program_name );
#elif CONFIG_GPLV3
    printf(
    "%s is free software; you can redistribute it and/or modify\n"
    "it under the terms of the GNU General Public License as published by\n"
    "the Free Software Foundation; either version 3 of the License, or\n"
    "(at your option) any later version.\n"
    "\n"
    "%s is distributed in the hope that it will be useful,\n"
    "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
    "GNU General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU General Public License\n"
    "along with %s.  If not, see <http://www.gnu.org/licenses/>.\n",
    program_name, program_name, program_name );
#elif CONFIG_GPL
    printf(
    "%s is free software; you can redistribute it and/or modify\n"
    "it under the terms of the GNU General Public License as published by\n"
    "the Free Software Foundation; either version 2 of the License, or\n"
    "(at your option) any later version.\n"
    "\n"
    "%s is distributed in the hope that it will be useful,\n"
    "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
    "GNU General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU General Public License\n"
    "along with %s; if not, write to the Free Software\n"
    "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA\n",
    program_name, program_name, program_name );
#elif CONFIG_LGPLV3
    printf(
    "%s is free software; you can redistribute it and/or modify\n"
    "it under the terms of the GNU Lesser General Public License as published by\n"
    "the Free Software Foundation; either version 3 of the License, or\n"
    "(at your option) any later version.\n"
    "\n"
    "%s is distributed in the hope that it will be useful,\n"
    "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
    "GNU Lesser General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU Lesser General Public License\n"
    "along with %s.  If not, see <http://www.gnu.org/licenses/>.\n",
    program_name, program_name, program_name );
#else
    printf(
    "%s is free software; you can redistribute it and/or\n"
    "modify it under the terms of the GNU Lesser General Public\n"
    "License as published by the Free Software Foundation; either\n"
    "version 2.1 of the License, or (at your option) any later version.\n"
    "\n"
    "%s is distributed in the hope that it will be useful,\n"
    "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU\n"
    "Lesser General Public License for more details.\n"
    "\n"
    "You should have received a copy of the GNU Lesser General Public\n"
    "License along with %s; if not, write to the Free Software\n"
    "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA\n",
    program_name, program_name, program_name );
#endif

    return 0;
}

static int is_device(const AVClass *avclass)
{
    if (!avclass)
        return 0;
    return AV_IS_INPUT_DEVICE(avclass->category) || AV_IS_OUTPUT_DEVICE(avclass->category);
}

static int show_formats_devices(void *optctx, const char *opt, const char *arg, int device_only, int muxdemuxers)
{
    void *ifmt_opaque = NULL;
    const AVInputFormat *ifmt  = NULL;
    void *ofmt_opaque = NULL;
    const AVOutputFormat *ofmt = NULL;
    const char *last_name;
    int is_dev;

    printf("%s\n"
           " D. = Demuxing supported\n"
           " .E = Muxing supported\n"
           " --\n", device_only ? "Devices:" : "File formats:");
    last_name = "000";
    for (;;) {
        int decode = 0;
        int encode = 0;
        const char *name      = NULL;
        const char *long_name = NULL;

        if (muxdemuxers !=SHOW_DEMUXERS) {
            ofmt_opaque = NULL;
            while ((ofmt = av_muxer_iterate(&ofmt_opaque))) {
                is_dev = is_device(ofmt->priv_class);
                if (!is_dev && device_only)
                    continue;
                if ((!name || strcmp(ofmt->name, name) < 0) &&
                    strcmp(ofmt->name, last_name) > 0) {
                    name      = ofmt->name;
                    long_name = ofmt->long_name;
                    encode    = 1;
                }
            }
        }
        if (muxdemuxers != SHOW_MUXERS) {
            ifmt_opaque = NULL;
            while ((ifmt = av_demuxer_iterate(&ifmt_opaque))) {
                is_dev = is_device(ifmt->priv_class);
                if (!is_dev && device_only)
                    continue;
                if ((!name || strcmp(ifmt->name, name) < 0) &&
                    strcmp(ifmt->name, last_name) > 0) {
                    name      = ifmt->name;
                    long_name = ifmt->long_name;
                    encode    = 0;
                }
                if (name && strcmp(ifmt->name, name) == 0)
                    decode = 1;
            }
        }
        if (!name)
            break;
        last_name = name;

        printf(" %s%s %-15s %s\n",
               decode ? "D" : " ",
               encode ? "E" : " ",
               name,
            long_name ? long_name:" ");
    }
    return 0;
}

int show_formats(void *optctx, const char *opt, const char *arg)
{
    return show_formats_devices(optctx, opt, arg, 0, SHOW_DEFAULT);
}

int show_muxers(void *optctx, const char *opt, const char *arg)
{
    return show_formats_devices(optctx, opt, arg, 0, SHOW_MUXERS);
}

int show_demuxers(void *optctx, const char *opt, const char *arg)
{
    return show_formats_devices(optctx, opt, arg, 0, SHOW_DEMUXERS);
}

int show_devices(void *optctx, const char *opt, const char *arg)
{
    return show_formats_devices(optctx, opt, arg, 1, SHOW_DEFAULT);
}

#define PRINT_CODEC_SUPPORTED(codec, field, type, list_name, term, get_name) \
    if (codec->field) {                                                      \
        const type *p = codec->field;                                        \
                                                                             \
        printf("    Supported " list_name ":");                              \
        while (*p != term) {                                                 \
            get_name(*p);                                                    \
            printf(" %s", name);                                             \
            p++;                                                             \
        }                                                                    \
        printf("\n");                                                        \
    }                                                                        \

static void print_codec(const AVCodec *c)
{
    int encoder = av_codec_is_encoder(c);

    printf("%s %s [%s]:\n", encoder ? "Encoder" : "Decoder", c->name,
           c->long_name ? c->long_name : "");

    printf("    General capabilities: ");
    if (c->capabilities & AV_CODEC_CAP_DRAW_HORIZ_BAND)
        printf("horizband ");
    if (c->capabilities & AV_CODEC_CAP_DR1)
        printf("dr1 ");
    if (c->capabilities & AV_CODEC_CAP_TRUNCATED)
        printf("trunc ");
    if (c->capabilities & AV_CODEC_CAP_DELAY)
        printf("delay ");
    if (c->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME)
        printf("small ");
    if (c->capabilities & AV_CODEC_CAP_SUBFRAMES)
        printf("subframes ");
    if (c->capabilities & AV_CODEC_CAP_EXPERIMENTAL)
        printf("exp ");
    if (c->capabilities & AV_CODEC_CAP_CHANNEL_CONF)
        printf("chconf ");
    if (c->capabilities & AV_CODEC_CAP_PARAM_CHANGE)
        printf("paramchange ");
    if (c->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        printf("variable ");
    if (c->capabilities & (AV_CODEC_CAP_FRAME_THREADS |
                           AV_CODEC_CAP_SLICE_THREADS |
                           AV_CODEC_CAP_AUTO_THREADS))
        printf("threads ");
    if (c->capabilities & AV_CODEC_CAP_AVOID_PROBING)
        printf("avoidprobe ");
    if (c->capabilities & AV_CODEC_CAP_INTRA_ONLY)
        printf("intraonly ");
    if (c->capabilities & AV_CODEC_CAP_LOSSLESS)
        printf("lossless ");
    if (c->capabilities & AV_CODEC_CAP_HARDWARE)
        printf("hardware ");
    if (c->capabilities & AV_CODEC_CAP_HYBRID)
        printf("hybrid ");
    if (!c->capabilities)
        printf("none");
    printf("\n");

    if (c->type == AVMEDIA_TYPE_VIDEO ||
        c->type == AVMEDIA_TYPE_AUDIO) {
        printf("    Threading capabilities: ");
        switch (c->capabilities & (AV_CODEC_CAP_FRAME_THREADS |
                                   AV_CODEC_CAP_SLICE_THREADS |
                                   AV_CODEC_CAP_AUTO_THREADS)) {
        case AV_CODEC_CAP_FRAME_THREADS |
             AV_CODEC_CAP_SLICE_THREADS: printf("frame and slice"); break;
        case AV_CODEC_CAP_FRAME_THREADS: printf("frame");           break;
        case AV_CODEC_CAP_SLICE_THREADS: printf("slice");           break;
        case AV_CODEC_CAP_AUTO_THREADS : printf("auto");            break;
        default:                         printf("none");            break;
        }
        printf("\n");
    }
    if (avcodec_get_hw_config(c, 0)) {
        printf("    Supported hardware devices: ");
        for (int i = 0;; i++) {
            const AVCodecHWConfig *config = avcodec_get_hw_config(c, i);
            if (!config)
                break;
            printf("%s ", av_hwdevice_get_type_name(config->device_type));
        }
        printf("\n");
    }

    if (c->supported_framerates) {
        const AVRational *fps = c->supported_framerates;

        printf("    Supported framerates:");
        while (fps->num) {
            printf(" %d/%d", fps->num, fps->den);
            fps++;
        }
        printf("\n");
    }
    PRINT_CODEC_SUPPORTED(c, pix_fmts, enum AVPixelFormat, "pixel formats",
                          AV_PIX_FMT_NONE, GET_PIX_FMT_NAME);
    PRINT_CODEC_SUPPORTED(c, supported_samplerates, int, "sample rates", 0,
                          GET_SAMPLE_RATE_NAME);
    PRINT_CODEC_SUPPORTED(c, sample_fmts, enum AVSampleFormat, "sample formats",
                          AV_SAMPLE_FMT_NONE, GET_SAMPLE_FMT_NAME);
    PRINT_CODEC_SUPPORTED(c, channel_layouts, uint64_t, "channel layouts",
                          0, GET_CH_LAYOUT_DESC);

    if (c->priv_class) {
        show_help_children(c->priv_class,
                           AV_OPT_FLAG_ENCODING_PARAM |
                           AV_OPT_FLAG_DECODING_PARAM);
    }
}

static char get_media_type_char(enum AVMediaType type)
{
    switch (type) {
        case AVMEDIA_TYPE_VIDEO:    return 'V';
        case AVMEDIA_TYPE_AUDIO:    return 'A';
        case AVMEDIA_TYPE_DATA:     return 'D';
        case AVMEDIA_TYPE_SUBTITLE: return 'S';
        case AVMEDIA_TYPE_ATTACHMENT:return 'T';
        default:                    return '?';
    }
}

static const AVCodec *next_codec_for_id(enum AVCodecID id, const AVCodec *prev,
                                        int encoder)
{
    while ((prev = av_codec_next(prev))) {
        if (prev->id == id &&
            (encoder ? av_codec_is_encoder(prev) : av_codec_is_decoder(prev)))
            return prev;
    }
    return NULL;
}

static int compare_codec_desc(const void *a, const void *b)
{
    const AVCodecDescriptor * const *da = a;
    const AVCodecDescriptor * const *db = b;

    return (*da)->type != (*db)->type ? FFDIFFSIGN((*da)->type, (*db)->type) :
           strcmp((*da)->name, (*db)->name);
}

static unsigned get_codecs_sorted(const AVCodecDescriptor ***rcodecs)
{
    const AVCodecDescriptor *desc = NULL;
    const AVCodecDescriptor **codecs;
    unsigned nb_codecs = 0, i = 0;

    while ((desc = avcodec_descriptor_next(desc)))
        nb_codecs++;
    if (!(codecs = av_calloc(nb_codecs, sizeof(*codecs)))) {
        av_log(NULL, AV_LOG_ERROR, "Out of memory\n");
        exit_program(1);
    }
    desc = NULL;
    while ((desc = avcodec_descriptor_next(desc)))
        codecs[i++] = desc;
    av_assert0(i == nb_codecs);
    qsort(codecs, nb_codecs, sizeof(*codecs), compare_codec_desc);
    *rcodecs = codecs;
    return nb_codecs;
}

static void print_codecs_for_id(enum AVCodecID id, int encoder)
{
    const AVCodec *codec = NULL;

    printf(" (%s: ", encoder ? "encoders" : "decoders");

    while ((codec = next_codec_for_id(id, codec, encoder)))
        printf("%s ", codec->name);

    printf(")");
}

int show_codecs(void *optctx, const char *opt, const char *arg)
{
    const AVCodecDescriptor **codecs;
    unsigned i, nb_codecs = get_codecs_sorted(&codecs);

    printf("Codecs:\n"
           " D..... = Decoding supported\n"
           " .E.... = Encoding supported\n"
           " ..V... = Video codec\n"
           " ..A... = Audio codec\n"
           " ..S... = Subtitle codec\n"
           " ...I.. = Intra frame-only codec\n"
           " ....L. = Lossy compression\n"
           " .....S = Lossless compression\n"
           " -------\n");
    for (i = 0; i < nb_codecs; i++) {
        const AVCodecDescriptor *desc = codecs[i];
        const AVCodec *codec = NULL;

        if (strstr(desc->name, "_deprecated"))
            continue;

        printf(" ");
        printf(avcodec_find_decoder(desc->id) ? "D" : ".");
        printf(avcodec_find_encoder(desc->id) ? "E" : ".");

        printf("%c", get_media_type_char(desc->type));
        printf((desc->props & AV_CODEC_PROP_INTRA_ONLY) ? "I" : ".");
        printf((desc->props & AV_CODEC_PROP_LOSSY)      ? "L" : ".");
        printf((desc->props & AV_CODEC_PROP_LOSSLESS)   ? "S" : ".");

        printf(" %-20s %s", desc->name, desc->long_name ? desc->long_name : "");

        /* print decoders/encoders when there's more than one or their
         * names are different from codec name */
        while ((codec = next_codec_for_id(desc->id, codec, 0))) {
            if (strcmp(codec->name, desc->name)) {
                print_codecs_for_id(desc->id, 0);
                break;
            }
        }
        codec = NULL;
        while ((codec = next_codec_for_id(desc->id, codec, 1))) {
            if (strcmp(codec->name, desc->name)) {
                print_codecs_for_id(desc->id, 1);
                break;
            }
        }

        printf("\n");
    }
    av_free(codecs);
    return 0;
}

static void print_codecs(int encoder)
{
    const AVCodecDescriptor **codecs;
    unsigned i, nb_codecs = get_codecs_sorted(&codecs);

    printf("%s:\n"
           " V..... = Video\n"
           " A..... = Audio\n"
           " S..... = Subtitle\n"
           " .F.... = Frame-level multithreading\n"
           " ..S... = Slice-level multithreading\n"
           " ...X.. = Codec is experimental\n"
           " ....B. = Supports draw_horiz_band\n"
           " .....D = Supports direct rendering method 1\n"
           " ------\n",
           encoder ? "Encoders" : "Decoders");
    for (i = 0; i < nb_codecs; i++) {
        const AVCodecDescriptor *desc = codecs[i];
        const AVCodec *codec = NULL;

        while ((codec = next_codec_for_id(desc->id, codec, encoder))) {
            printf(" %c", get_media_type_char(desc->type));
            printf((codec->capabilities & AV_CODEC_CAP_FRAME_THREADS) ? "F" : ".");
            printf((codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) ? "S" : ".");
            printf((codec->capabilities & AV_CODEC_CAP_EXPERIMENTAL)  ? "X" : ".");
            printf((codec->capabilities & AV_CODEC_CAP_DRAW_HORIZ_BAND)?"B" : ".");
            printf((codec->capabilities & AV_CODEC_CAP_DR1)           ? "D" : ".");

            printf(" %-20s %s", codec->name, codec->long_name ? codec->long_name : "");
            if (strcmp(codec->name, desc->name))
                printf(" (codec %s)", desc->name);

            printf("\n");
        }
    }
    av_free(codecs);
}

int show_decoders(void *optctx, const char *opt, const char *arg)
{
    print_codecs(0);
    return 0;
}

int show_encoders(void *optctx, const char *opt, const char *arg)
{
    print_codecs(1);
    return 0;
}

int show_bsfs(void *optctx, const char *opt, const char *arg)
{
    const AVBitStreamFilter *bsf = NULL;
    void *opaque = NULL;

    printf("Bitstream filters:\n");
    while ((bsf = av_bsf_iterate(&opaque)))
        printf("%s\n", bsf->name);
    printf("\n");
    return 0;
}

int show_protocols(void *optctx, const char *opt, const char *arg)
{
    void *opaque = NULL;
    const char *name;

    printf("Supported file protocols:\n"
           "Input:\n");
    while ((name = avio_enum_protocols(&opaque, 0)))
        printf("  %s\n", name);
    printf("Output:\n");
    while ((name = avio_enum_protocols(&opaque, 1)))
        printf("  %s\n", name);
    return 0;
}

int show_filters(void *optctx, const char *opt, const char *arg)
{
#if CONFIG_AVFILTER
    const AVFilter *filter = NULL;
    char descr[64], *descr_cur;
    void *opaque = NULL;
    int i, j;
    const AVFilterPad *pad;

    printf("Filters:\n"
           "  T.. = Timeline support\n"
           "  .S. = Slice threading\n"
           "  ..C = Command support\n"
           "  A = Audio input/output\n"
           "  V = Video input/output\n"
           "  N = Dynamic number and/or type of input/output\n"
           "  | = Source or sink filter\n");
    while ((filter = av_filter_iterate(&opaque))) {
        descr_cur = descr;
        for (i = 0; i < 2; i++) {
            if (i) {
                *(descr_cur++) = '-';
                *(descr_cur++) = '>';
            }
            pad = i ? filter->outputs : filter->inputs;
            for (j = 0; pad && avfilter_pad_get_name(pad, j); j++) {
                if (descr_cur >= descr + sizeof(descr) - 4)
                    break;
                *(descr_cur++) = get_media_type_char(avfilter_pad_get_type(pad, j));
            }
            if (!j)
                *(descr_cur++) = ((!i && (filter->flags & AVFILTER_FLAG_DYNAMIC_INPUTS)) ||
                                  ( i && (filter->flags & AVFILTER_FLAG_DYNAMIC_OUTPUTS))) ? 'N' : '|';
        }
        *descr_cur = 0;
        printf(" %c%c%c %-17s %-10s %s\n",
               filter->flags & AVFILTER_FLAG_SUPPORT_TIMELINE ? 'T' : '.',
               filter->flags & AVFILTER_FLAG_SLICE_THREADS    ? 'S' : '.',
               filter->process_command                        ? 'C' : '.',
               filter->name, descr, filter->description);
    }
#else
    printf("No filters available: libavfilter disabled\n");
#endif
    return 0;
}

int show_colors(void *optctx, const char *opt, const char *arg)
{
    const char *name;
    const uint8_t *rgb;
    int i;

    printf("%-32s #RRGGBB\n", "name");

    for (i = 0; name = av_get_known_color_name(i, &rgb); i++)
        printf("%-32s #%02x%02x%02x\n", name, rgb[0], rgb[1], rgb[2]);

    return 0;
}

int show_pix_fmts(void *optctx, const char *opt, const char *arg)
{
    const AVPixFmtDescriptor *pix_desc = NULL;

    printf("Pixel formats:\n"
           "I.... = Supported Input  format for conversion\n"
           ".O... = Supported Output format for conversion\n"
           "..H.. = Hardware accelerated format\n"
           "...P. = Paletted format\n"
           "....B = Bitstream format\n"
           "FLAGS NAME            NB_COMPONENTS BITS_PER_PIXEL\n"
           "-----\n");

#if !CONFIG_SWSCALE
#   define sws_isSupportedInput(x)  0
#   define sws_isSupportedOutput(x) 0
#endif

    while ((pix_desc = av_pix_fmt_desc_next(pix_desc))) {
        enum AVPixelFormat av_unused pix_fmt = av_pix_fmt_desc_get_id(pix_desc);
        printf("%c%c%c%c%c %-16s       %d            %2d\n",
               sws_isSupportedInput (pix_fmt)              ? 'I' : '.',
               sws_isSupportedOutput(pix_fmt)              ? 'O' : '.',
               pix_desc->flags & AV_PIX_FMT_FLAG_HWACCEL   ? 'H' : '.',
               pix_desc->flags & AV_PIX_FMT_FLAG_PAL       ? 'P' : '.',
               pix_desc->flags & AV_PIX_FMT_FLAG_BITSTREAM ? 'B' : '.',
               pix_desc->name,
               pix_desc->nb_components,
               av_get_bits_per_pixel(pix_desc));
    }
    return 0;
}

int show_layouts(void *optctx, const char *opt, const char *arg)
{
    int i = 0;
    uint64_t layout, j;
    const char *name, *descr;

    printf("Individual channels:\n"
           "NAME           DESCRIPTION\n");
    for (i = 0; i < 63; i++) {
        name = av_get_channel_name((uint64_t)1 << i);
        if (!name)
            continue;
        descr = av_get_channel_description((uint64_t)1 << i);
        printf("%-14s %s\n", name, descr);
    }
    printf("\nStandard channel layouts:\n"
           "NAME           DECOMPOSITION\n");
    for (i = 0; !av_get_standard_channel_layout(i, &layout, &name); i++) {
        if (name) {
            printf("%-14s ", name);
            for (j = 1; j; j <<= 1)
                if ((layout & j))
                    printf("%s%s", (layout & (j - 1)) ? "+" : "", av_get_channel_name(j));
            printf("\n");
        }
    }
    return 0;
}

int show_sample_fmts(void *optctx, const char *opt, const char *arg)
{
    int i;
    char fmt_str[128];
    for (i = -1; i < AV_SAMPLE_FMT_NB; i++)
        printf("%s\n", av_get_sample_fmt_string(fmt_str, sizeof(fmt_str), i));
    return 0;
}

static void show_help_codec(const char *name, int encoder)
{
    const AVCodecDescriptor *desc;
    const AVCodec *codec;

    if (!name) {
        av_log(NULL, AV_LOG_ERROR, "No codec name specified.\n");
        return;
    }

    codec = encoder ? avcodec_find_encoder_by_name(name) :
                      avcodec_find_decoder_by_name(name);

    if (codec)
        print_codec(codec);
    else if ((desc = avcodec_descriptor_get_by_name(name))) {
        int printed = 0;

        while ((codec = next_codec_for_id(desc->id, codec, encoder))) {
            printed = 1;
            print_codec(codec);
        }

        if (!printed) {
            av_log(NULL, AV_LOG_ERROR, "Codec '%s' is known to FFmpeg, "
                   "but no %s for it are available. FFmpeg might need to be "
                   "recompiled with additional external libraries.\n",
                   name, encoder ? "encoders" : "decoders");
        }
    } else {
        av_log(NULL, AV_LOG_ERROR, "Codec '%s' is not recognized by FFmpeg.\n",
               name);
    }
}

static void show_help_demuxer(const char *name)
{
    const AVInputFormat *fmt = av_find_input_format(name);

    if (!fmt) {
        av_log(NULL, AV_LOG_ERROR, "Unknown format '%s'.\n", name);
        return;
    }

    printf("Demuxer %s [%s]:\n", fmt->name, fmt->long_name);

    if (fmt->extensions)
        printf("    Common extensions: %s.\n", fmt->extensions);

    if (fmt->priv_class)
        show_help_children(fmt->priv_class, AV_OPT_FLAG_DECODING_PARAM);
}

static void show_help_muxer(const char *name)
{
    const AVCodecDescriptor *desc;
    const AVOutputFormat *fmt = av_guess_format(name, NULL, NULL);

    if (!fmt) {
        av_log(NULL, AV_LOG_ERROR, "Unknown format '%s'.\n", name);
        return;
    }

    printf("Muxer %s [%s]:\n", fmt->name, fmt->long_name);

    if (fmt->extensions)
        printf("    Common extensions: %s.\n", fmt->extensions);
    if (fmt->mime_type)
        printf("    Mime type: %s.\n", fmt->mime_type);
    if (fmt->video_codec != AV_CODEC_ID_NONE &&
        (desc = avcodec_descriptor_get(fmt->video_codec))) {
        printf("    Default video codec: %s.\n", desc->name);
    }
    if (fmt->audio_codec != AV_CODEC_ID_NONE &&
        (desc = avcodec_descriptor_get(fmt->audio_codec))) {
        printf("    Default audio codec: %s.\n", desc->name);
    }
    if (fmt->subtitle_codec != AV_CODEC_ID_NONE &&
        (desc = avcodec_descriptor_get(fmt->subtitle_codec))) {
        printf("    Default subtitle codec: %s.\n", desc->name);
    }

    if (fmt->priv_class)
        show_help_children(fmt->priv_class, AV_OPT_FLAG_ENCODING_PARAM);
}

#if CONFIG_AVFILTER
static void show_help_filter(const char *name)
{
#if CONFIG_AVFILTER
    const AVFilter *f = avfilter_get_by_name(name);
    int i, count;

    if (!name) {
        av_log(NULL, AV_LOG_ERROR, "No filter name specified.\n");
        return;
    } else if (!f) {
        av_log(NULL, AV_LOG_ERROR, "Unknown filter '%s'.\n", name);
        return;
    }

    printf("Filter %s\n", f->name);
    if (f->description)
        printf("  %s\n", f->description);

    if (f->flags & AVFILTER_FLAG_SLICE_THREADS)
        printf("    slice threading supported\n");

    printf("    Inputs:\n");
    count = avfilter_pad_count(f->inputs);
    for (i = 0; i < count; i++) {
        printf("       #%d: %s (%s)\n", i, avfilter_pad_get_name(f->inputs, i),
               media_type_string(avfilter_pad_get_type(f->inputs, i)));
    }
    if (f->flags & AVFILTER_FLAG_DYNAMIC_INPUTS)
        printf("        dynamic (depending on the options)\n");
    else if (!count)
        printf("        none (source filter)\n");

    printf("    Outputs:\n");
    count = avfilter_pad_count(f->outputs);
    for (i = 0; i < count; i++) {
        printf("       #%d: %s (%s)\n", i, avfilter_pad_get_name(f->outputs, i),
               media_type_string(avfilter_pad_get_type(f->outputs, i)));
    }
    if (f->flags & AVFILTER_FLAG_DYNAMIC_OUTPUTS)
        printf("        dynamic (depending on the options)\n");
    else if (!count)
        printf("        none (sink filter)\n");

    if (f->priv_class)
        show_help_children(f->priv_class, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_FILTERING_PARAM |
                                          AV_OPT_FLAG_AUDIO_PARAM);
    if (f->flags & AVFILTER_FLAG_SUPPORT_TIMELINE)
        printf("This filter has support for timeline through the 'enable' option.\n");
#else
    av_log(NULL, AV_LOG_ERROR, "Build without libavfilter; "
           "can not to satisfy request\n");
#endif
}
#endif

static void show_help_bsf(const char *name)
{
    const AVBitStreamFilter *bsf = av_bsf_get_by_name(name);

    if (!name) {
        av_log(NULL, AV_LOG_ERROR, "No bitstream filter name specified.\n");
        return;
    } else if (!bsf) {
        av_log(NULL, AV_LOG_ERROR, "Unknown bit stream filter '%s'.\n", name);
        return;
    }

    printf("Bit stream filter %s\n", bsf->name);
    PRINT_CODEC_SUPPORTED(bsf, codec_ids, enum AVCodecID, "codecs",
                          AV_CODEC_ID_NONE, GET_CODEC_NAME);
    if (bsf->priv_class)
        show_help_children(bsf->priv_class, AV_OPT_FLAG_BSF_PARAM);
}

int show_help(void *optctx, const char *opt, const char *arg)
{
    char *topic, *par;
    av_log_set_callback(log_callback_help);

    topic = av_strdup(arg ? arg : "");
    if (!topic)
        return AVERROR(ENOMEM);
    par = strchr(topic, '=');
    if (par)
        *par++ = 0;

    if (!*topic) {
        show_help_default(topic, par);
    } else if (!strcmp(topic, "decoder")) {
        show_help_codec(par, 0);
    } else if (!strcmp(topic, "encoder")) {
        show_help_codec(par, 1);
    } else if (!strcmp(topic, "demuxer")) {
        show_help_demuxer(par);
    } else if (!strcmp(topic, "muxer")) {
        show_help_muxer(par);
#if CONFIG_AVFILTER
    } else if (!strcmp(topic, "filter")) {
        show_help_filter(par);
#endif
    } else if (!strcmp(topic, "bsf")) {
        show_help_bsf(par);
    } else {
        show_help_default(topic, par);
    }

    av_freep(&topic);
    return 0;
}

int read_yesno(void)
{
    int c = getchar();
    int yesno = (av_toupper(c) == 'Y');

    while (c != '\n' && c != EOF)
        c = getchar();

    return yesno;
}

FILE *get_preset_file(char *filename, size_t filename_size,
                      const char *preset_name, int is_path,
                      const char *codec_name)
{
    FILE *f = NULL;
    int i;
    const char *base[3] = { getenv("FFMPEG_DATADIR"),
                            getenv("HOME"),
                            FFMPEG_DATADIR, };

    if (is_path) {
        av_strlcpy(filename, preset_name, filename_size);
        f = fopen(filename, "r");
    } else {
#ifdef _WIN32
        char datadir[MAX_PATH], *ls;
        base[2] = NULL;

        if (GetModuleFileNameA(GetModuleHandleA(NULL), datadir, sizeof(datadir) - 1))
        {
            for (ls = datadir; ls < datadir + strlen(datadir); ls++)
                if (*ls == '\\') *ls = '/';

            if (ls = strrchr(datadir, '/'))
            {
                *ls = 0;
                strncat(datadir, "/ffpresets",  sizeof(datadir) - 1 - strlen(datadir));
                base[2] = datadir;
            }
        }
#endif
        for (i = 0; i < 3 && !f; i++) {
            if (!base[i])
                continue;
            snprintf(filename, filename_size, "%s%s/%s.ffpreset", base[i],
                     i != 1 ? "" : "/.ffmpeg", preset_name);
            f = fopen(filename, "r");
            if (!f && codec_name) {
                snprintf(filename, filename_size,
                         "%s%s/%s-%s.ffpreset",
                         base[i], i != 1 ? "" : "/.ffmpeg", codec_name,
                         preset_name);
                f = fopen(filename, "r");
            }
        }
    }

    return f;
}

int check_stream_specifier(AVFormatContext *s, AVStream *st, const char *spec)
{
    int ret = avformat_match_stream_specifier(s, st, spec);
    if (ret < 0)
        av_log(s, AV_LOG_ERROR, "Invalid stream specifier: %s.\n", spec);
    return ret;
}

/**
 * @brief 先不管。
 * @param opts 字典。
 * @param codec_id 编解码器id。例如h264、h265、vp8、aac等等。
 * @param s 输入上下文。
 * @param st 对应的流，可能是视频流、音频流、字幕流等等。
 * @param codec 编解码器。
 * @return 先不管。
 *
 * 调用：filter_codec_opts(codec_opts, s->streams[i]->codecpar->codec_id, s, s->streams[i], NULL);
 */
AVDictionary *filter_codec_opts(AVDictionary *opts, enum AVCodecID codec_id,
                                AVFormatContext *s, AVStream *st, AVCodec *codec)
{
    AVDictionary    *ret = NULL;
    AVDictionaryEntry *t = NULL;

    // 1. 判断是编码还是解码，s->oformat=NULL代表解码，说明不用编码进行输出。
    int            flags = s->oformat ? AV_OPT_FLAG_ENCODING_PARAM
                                      : AV_OPT_FLAG_DECODING_PARAM;
    char          prefix = 0;

    // 2. 获取AVCodecContext的AVClass。它可以与AV_OPT_SEARCH_FAKE_OBJ结合使用，用于检查选项
    const AVClass    *cc = avcodec_get_class();

    // 3. 如果编解码器传进来是一个空值，自动根据codec_id再次寻找
    if (!codec)
        codec            = s->oformat ? avcodec_find_encoder(codec_id)
                                      : avcodec_find_decoder(codec_id);

    // 4. 判断是哪一种流，进行标记flags。
    switch (st->codecpar->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        prefix  = 'v';
        flags  |= AV_OPT_FLAG_VIDEO_PARAM;
        break;
    case AVMEDIA_TYPE_AUDIO:
        prefix  = 'a';
        flags  |= AV_OPT_FLAG_AUDIO_PARAM;
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        prefix  = 's';
        flags  |= AV_OPT_FLAG_SUBTITLE_PARAM;
        break;
    }

    // 5. 处理，由于一般t返回空，不进while循环，所以目前暂不分析while内部
    while (t = av_dict_get(opts, "", t, AV_DICT_IGNORE_SUFFIX)) {
        char *p = strchr(t->key, ':');

        /* check stream specification in opt name */
        if (p)
            switch (check_stream_specifier(s, st, p + 1)) {
            case  1: *p = 0; break;
            case  0:         continue;
            default:         exit_program(1);
            }

        if (av_opt_find(&cc, t->key, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ) ||
            !codec ||
            (codec->priv_class &&
             av_opt_find(&codec->priv_class, t->key, NULL, flags,
                         AV_OPT_SEARCH_FAKE_OBJ)))
            av_dict_set(&ret, t->key, t->value, 0);
        else if (t->key[0] == prefix &&
                 av_opt_find(&cc, t->key + 1, NULL, flags,
                             AV_OPT_SEARCH_FAKE_OBJ))
            av_dict_set(&ret, t->key + 1, t->value, 0);

        if (p)
            *p = ':';
    }

    return ret;
}

/**
 * @brief 先不管。
 * @param s 输入的上下文。
 * @param codec_opts 字典选项，例如传入的是全局的编解码器选项。
 * @return 先不管。
 */
AVDictionary **setup_find_stream_info_opts(AVFormatContext *s,
                                           AVDictionary *codec_opts)
{
    int i;
    AVDictionary **opts;

    // 1. 没有可用的流直接返回
    if (!s->nb_streams)
        return NULL;

    // 2. 开辟多个AVDictionary *类型的指针，保存在二级指针opts中。
    opts = av_mallocz_array(s->nb_streams, sizeof(*opts));
    if (!opts) {
        av_log(NULL, AV_LOG_ERROR,
               "Could not alloc memory for stream options.\n");
        return NULL;
    }

    // 3. 设置对应流(视频流、音频流、字幕流)的选项。不过opts里面的一级指针一般得到的空值，后续遇到再详细分析
    for (i = 0; i < s->nb_streams; i++)
        opts[i] = filter_codec_opts(codec_opts, s->streams[i]->codecpar->codec_id,
                                    s, s->streams[i], NULL);

    // 二级指针opts不是空，但里面的一级指针是空值
    return opts;
}

void *grow_array(void *array, int elem_size, int *size, int new_size)
{
    if (new_size >= INT_MAX / elem_size) {
        av_log(NULL, AV_LOG_ERROR, "Array too big.\n");
        exit_program(1);
    }
    if (*size < new_size) {
        uint8_t *tmp = av_realloc_array(array, new_size, elem_size);
        if (!tmp) {
            av_log(NULL, AV_LOG_ERROR, "Could not alloc buffer.\n");
            exit_program(1);
        }
        memset(tmp + *size*elem_size, 0, (new_size-*size) * elem_size);
        *size = new_size;
        return tmp;
    }
    return array;
}

double get_rotation(AVStream *st)
{
    /*
     * av_stream_get_side_data()：从信息流中获取边信息。
     * 参1：流；参2：所需的边信息类型；参3：用于存储边信息大小的指针(可选)；
     * 存在返回数据指针，，否则为NULL。
     *
     * AV_PKT_DATA_DISPLAYMATRIX：这个边数据包含一个描述仿射的3x3变换矩阵转换，
     * 需要应用到解码的视频帧正确的显示。数据的详细描述请参见libavutil/display.h
    */
    uint8_t* displaymatrix = av_stream_get_side_data(st,
                                                     AV_PKT_DATA_DISPLAYMATRIX, NULL);
    if(displaymatrix){
        printf("displaymatrix: %d\n", *displaymatrix);
    }

    /*
     * av_display_rotation_get()：提取变换矩阵的旋转分量。
     * 参1：转换矩阵。
     * 返回转换旋转帧的角度(以度为单位)逆时针方向。角度将在[-180.0,180.0]范围内。如果矩阵是奇异的则返回NaN。
     * @note：浮点数本质上是不精确的，所以调用者是建议在使用前将返回值舍入到最接近的整数。
    */
    double theta = 0;
    if (displaymatrix)
        theta = -av_display_rotation_get((int32_t*) displaymatrix);

    theta -= 360*floor(theta/360 + 0.9/360);

    if (fabs(theta - 90*round(theta/90)) > 2)
        av_log(NULL, AV_LOG_WARNING, "Odd rotation angle.\n"
               "If you want to help, upload a sample "
               "of this file to ftp://upload.ffmpeg.org/incoming/ "
               "and contact the ffmpeg-devel mailing list. (ffmpeg-devel@ffmpeg.org)");// 上传样本，找ffmpeg帮忙

    return theta;
}

#if CONFIG_AVDEVICE
static int print_device_sources(AVInputFormat *fmt, AVDictionary *opts)
{
    int ret, i;
    AVDeviceInfoList *device_list = NULL;

    if (!fmt || !fmt->priv_class  || !AV_IS_INPUT_DEVICE(fmt->priv_class->category))
        return AVERROR(EINVAL);

    printf("Auto-detected sources for %s:\n", fmt->name);
    if (!fmt->get_device_list) {
        ret = AVERROR(ENOSYS);
        printf("Cannot list sources. Not implemented.\n");
        goto fail;
    }

    if ((ret = avdevice_list_input_sources(fmt, NULL, opts, &device_list)) < 0) {
        printf("Cannot list sources.\n");
        goto fail;
    }

    for (i = 0; i < device_list->nb_devices; i++) {
        printf("%s %s [%s]\n", device_list->default_device == i ? "*" : " ",
               device_list->devices[i]->device_name, device_list->devices[i]->device_description);
    }

  fail:
    avdevice_free_list_devices(&device_list);
    return ret;
}

static int print_device_sinks(AVOutputFormat *fmt, AVDictionary *opts)
{
    int ret, i;
    AVDeviceInfoList *device_list = NULL;

    if (!fmt || !fmt->priv_class  || !AV_IS_OUTPUT_DEVICE(fmt->priv_class->category))
        return AVERROR(EINVAL);

    printf("Auto-detected sinks for %s:\n", fmt->name);
    if (!fmt->get_device_list) {
        ret = AVERROR(ENOSYS);
        printf("Cannot list sinks. Not implemented.\n");
        goto fail;
    }

    if ((ret = avdevice_list_output_sinks(fmt, NULL, opts, &device_list)) < 0) {
        printf("Cannot list sinks.\n");
        goto fail;
    }

    for (i = 0; i < device_list->nb_devices; i++) {
        printf("%s %s [%s]\n", device_list->default_device == i ? "*" : " ",
               device_list->devices[i]->device_name, device_list->devices[i]->device_description);
    }

  fail:
    avdevice_free_list_devices(&device_list);
    return ret;
}

static int show_sinks_sources_parse_arg(const char *arg, char **dev, AVDictionary **opts)
{
    int ret;
    if (arg) {
        char *opts_str = NULL;
        av_assert0(dev && opts);
        *dev = av_strdup(arg);
        if (!*dev)
            return AVERROR(ENOMEM);
        if ((opts_str = strchr(*dev, ','))) {
            *(opts_str++) = '\0';
            if (opts_str[0] && ((ret = av_dict_parse_string(opts, opts_str, "=", ":", 0)) < 0)) {
                av_freep(dev);
                return ret;
            }
        }
    } else
        printf("\nDevice name is not provided.\n"
                "You can pass devicename[,opt1=val1[,opt2=val2...]] as an argument.\n\n");
    return 0;
}

int show_sources(void *optctx, const char *opt, const char *arg)
{
    AVInputFormat *fmt = NULL;
    char *dev = NULL;
    AVDictionary *opts = NULL;
    int ret = 0;
    int error_level = av_log_get_level();

    av_log_set_level(AV_LOG_ERROR);

    if ((ret = show_sinks_sources_parse_arg(arg, &dev, &opts)) < 0)
        goto fail;

    do {
        fmt = av_input_audio_device_next(fmt);
        if (fmt) {
            if (!strcmp(fmt->name, "lavfi"))
                continue; //it's pointless to probe lavfi
            if (dev && !av_match_name(dev, fmt->name))
                continue;
            print_device_sources(fmt, opts);
        }
    } while (fmt);
    do {
        fmt = av_input_video_device_next(fmt);
        if (fmt) {
            if (dev && !av_match_name(dev, fmt->name))
                continue;
            print_device_sources(fmt, opts);
        }
    } while (fmt);
  fail:
    av_dict_free(&opts);
    av_free(dev);
    av_log_set_level(error_level);
    return ret;
}

int show_sinks(void *optctx, const char *opt, const char *arg)
{
    AVOutputFormat *fmt = NULL;
    char *dev = NULL;
    AVDictionary *opts = NULL;
    int ret = 0;
    int error_level = av_log_get_level();

    av_log_set_level(AV_LOG_ERROR);

    if ((ret = show_sinks_sources_parse_arg(arg, &dev, &opts)) < 0)
        goto fail;

    do {
        fmt = av_output_audio_device_next(fmt);
        if (fmt) {
            if (dev && !av_match_name(dev, fmt->name))
                continue;
            print_device_sinks(fmt, opts);
        }
    } while (fmt);
    do {
        fmt = av_output_video_device_next(fmt);
        if (fmt) {
            if (dev && !av_match_name(dev, fmt->name))
                continue;
            print_device_sinks(fmt, opts);
        }
    } while (fmt);
  fail:
    av_dict_free(&opts);
    av_free(dev);
    av_log_set_level(error_level);
    return ret;
}

#endif
