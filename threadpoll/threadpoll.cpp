#include "threadpoll.h"

template<typename T>
threadpoll<T>::threadpoll( connection_poll* conn_poll,  int thread_number = 8, int max_request = 10000 ) 
: m_thread_number(thread_number),m_max_requests(max_request), m_threads(NULL), m_stop(false),m_conn_poll(conn_poll)
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

// 将新的连接添加到线程池的任务队列中
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
        
        // 从连接池中取出一个连接赋给 “新获得线程的请求”
        connection_RAII mysql_conn(&request->mysql, m_conn_poll);
        request->process();

    }
}