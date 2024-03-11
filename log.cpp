#include <stdio.h>
#include <time.h>
#include <string.h>
#include "log.h"

static int level = LOG_INFO; // log 默认是 info 级别的
static int LOG_BUFFER_SIZE = 2048; // log 缓存大小
static const char* loglevels[] =
{
    "emerge!", "alert!", "critical!", "error!", "warn!", "notice:", "info:", "debug:"
};

void set_loglevel( int log_level )
{
    level = log_level;
}

// 日志级别、文件名、文件行数、参数
void log( int log_level,  const char* file_name, int line_num, const char* format, ... )
{
    // 传入的级别 小于 设置的级别才能输出
    if ( log_level > level )
    {
        return;
    }

    // time() 函数返回值和指针参数都可以作为时间返回，是一样的
    time_t tmp = time( NULL );

    // tm 结构体包含: tm_sec, tm_min, tm_hour 等参数
    struct tm* cur_time = localtime( &tmp ); // 转化 time_t 到本地时间

    // 转换失败返回
    if ( ! cur_time )
    {
        return;
    }

    char arg_buffer[ LOG_BUFFER_SIZE ];
    memset( arg_buffer, '\0', LOG_BUFFER_SIZE );
    strftime( arg_buffer, LOG_BUFFER_SIZE - 1, "[ %x %X ] ", cur_time );

    printf( "%s", arg_buffer ); // 时间: 日期 + 时间
    printf( "%s:%04d ", file_name, line_num ); // 文件名 + 行数
    printf( "%s ", loglevels[ log_level - LOG_EMERG ] ); // 日志类型对应的表述，但是 LOG_EMERG 是哪来的？

    va_list arg_list; // valist in stdarg.h
    va_start( arg_list, format ); // 从 format 开始 va_list
    memset( arg_buffer, '\0', LOG_BUFFER_SIZE );

    vsnprintf( arg_buffer, LOG_BUFFER_SIZE - 1, format, arg_list ); // 从 arg_list 读到 arg_buffer
    printf( "%s\n", arg_buffer ); // 输出到 stdout
    fflush( stdout );
    va_end( arg_list );
}
