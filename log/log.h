#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"
using namespace std;

class Log
{
public:
    // c++ 11 以后，使用局部变量懒汉不用加锁,因为 c++ 11 以上，静态局部变量初始化是线程安全的
    static Log* get_instance()
    {
        static Log instance;
        return &instance;
    }

    static void* flush_log_thread(void *args)
    {
        Log::get_instance()->async_write_log();
    }

    // 可选择的参数有日志文件、日志缓冲大小、最大行数以及最长日纸条队列
    bool init( const char* file_name, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    void write_log( int level, const char* format , ...);

    void flush(void);

private:
    Log();
    virtual ~Log();
    Log(const Log&)  = delete;
    Log& operator=(const Log&) = delete;

    void *async_write_log()
    {
        string single_log;
        // 从阻塞队列中取出一条日志 string，写入文件
        while ( m_log_queue->pop(single_log) )
        {
            m_mutex.lock();
            fputs( single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
        
    }

private:
    char dir_name[128]; // 路径名
    char log_name[128]; // log 文件名
    int m_split_lines;  // 日志最大行数
    int m_log_buf_size; // 日志缓冲区大小
    long long m_count;  // 日志行数记录
    int m_today;        // 记录当前时间的是日期
    FILE *m_fp;         // log 文件指针
    char *m_buf;        // 缓冲
    block_queue<string> *m_log_queue;   // 阻塞队列
    bool m_is_async;    // 设置同步的标志位
    locker m_mutex;
};

#define LOG_DEBUG(format, ...) Log::get_instance()->write_log( 0 , format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) Log::get_instance()->write_log(1, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) Log::get_instance()->write_log(2, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) Log::get_instance()->write_log(3, format, ##__VA_ARGS__)


#endif