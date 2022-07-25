#ifndef THREADPOLL_H
#define THREADPOLL_H

#include <list>
#include<cstdio>
#include<exception>
#include<pthread.h>
#include "../lock/locker.h"

/* !!!!!! 未添加数据库连接池相关 */

template <typename T>
class threadpoll
{
public:
    threadpoll( int thread_number = 8, int max_request = 10000 );
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
};

template<typename T>
threadpoll<T>::threadpoll( int thread_number = 8, int max_request = 10000 ) 
: m_thread_number(thread_number),m_max_requests(max_request), m_threads(NULL), m_stop(false)
{
    if ( thread_number <= 0 || max_request <= 0 )
        throw std::exception();
    
    m_threads = new pthread_t[m_thread_number];
    if ( !m_threads )
        throw std::exception();
    
    for ( int i = 0; i < thread_number; ++i)
    {
        // create pthread
        if ( pthread_create( m_threads+i , NULL, worker, this ) != 0 )
        {
            delete[] m_threads;
            throw std::exception();
        }
        if ( pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template<typename T>
threadpoll<T>::~threadpoll()
{
    delete[] m_threads;
    m_stop = true;
}

template<typename T>
bool threadpoll<T>::append( T* request)
{
    m_queuelocker.lock();
    if ( m_workqueue.size() > m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template<typename T>
void *threadpoll<T>::worker( void* arg)
{
    threadpoll* poll = (threadpoll*) arg;
    poll->run();
    return poll;
}


template<typename T>
void threadpoll<T>::run()
{
    while ( !m_stop)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if ( m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if ( !request )
            continue;
        
        /* !!!缺少对于数据库的设置 */
        request->process();

    }
}


#endif