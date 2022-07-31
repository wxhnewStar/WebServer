#ifndef THREADPOLL_H
#define THREADPOLL_H

#include <list>
#include<cstdio>
#include<exception>
#include<pthread.h>
#include "../lock/locker.h"

#include "../sqlpoll/sql_connection_poll.h"

template <typename T>
class threadpoll
{
public:
    threadpoll( connection_poll* conn_poll, int thread_number = 8, int max_request = 10000 );
    ~threadpoll();
    bool append( T* request);

private:
    /* 工作线程运行的函数，它将不断从工作队列中取出任务并执行之 */
    static void *worker( void *arg );
    void run();
    
private:
    int m_thread_number;        // 线程池中的线程数
    int m_max_requests;         // 请求队列中允许的最大请求数
    pthread_t* m_threads;       // 描述进程池的数组，其大小为 m_thread_number
    std::list<T*> m_workqueue;  // 请求队列
    locker m_queuelocker;       // 保护请求队列的互斥锁
    sem m_queuestat;            // 记录请求的信号量
    bool m_stop;                // 是否结束进程
    connection_poll* m_conn_poll;   // 数据库连接池
};



#endif