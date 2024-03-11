#ifndef LOG_H
#define LOG_H

#include <syslog.h>
#include <cstdarg>
// .h 只定义两个函数
// 未封装、且只能写到 stdout

void set_loglevel( int log_level = LOG_DEBUG );
void log( int log_level, const char* file_name, int line_num, const char* format, ... );

#endif
