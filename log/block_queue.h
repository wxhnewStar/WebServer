/*************************************************************
*循环数组实现的阻塞队列，m_back = (m_back + 1) % m_max_size;  
*线程安全，每个操作前都要先加互斥锁，操作完后，再解锁
**************************************************************/


#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <sys/time.h>
#include <pthread.h>
#include "../lock/locker.h"
using namespace std;


template<class T>
class block_queue
{
public:
    block_queue(int);
    ~block_queue();
    void clear();
    bool full();    // 判断队列是否满了
    bool empty();   // 判断队列是否空了
    bool front(T&);   // 返回队首元素
    bool back(T&);    // 返回队尾元素
    int  size();    
    int  max_size();

    // 往队列添加元素，需要将所有使用队列的线程先唤醒
    // 当有元素 push 进队列时，相当于生产者生产了一个元素
    // 若当前没有线程等待条件变量，则唤醒无意义
    bool push(const T&);    
    
    // 如果当前队列没有元素，将会等待条件变量
    bool pop(T&);   

    // pop 的超时处理版本
    bool pop(T& , int);     


private:
    locker m_mutex;
    cond m_cond;

    T* m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};

template<class T>
block_queue<T>::block_queue(int max_size = 1000):m_max_size(max_size)
{
    if ( max_size <= 0)
    {
        exit(-1);
    }

    m_array = new T[max_size];
    m_size = 0;
    m_front = -1;
    m_back = -1;
}

template<class T>
block_queue<T>::~block_queue()
{
    m_mutex.lock();
    if ( m_array != NULL )
    {
        delete[] m_array;
    }
    m_mutex.unlock();
}

template<class T>
bool block_queue<T>::full()
{
    m_mutex.lock();
    if ( m_size >= m_max_size )
    {
        m_mutex.unlock();
        return true;
    }
    m_mutex.unlock();
    return false;
}

template<class T>
bool block_queue<T>::empty()
{
    m_mutex.lock();
    if ( 0 == m_size )
    {
        m_mutex.unlock();
        return true;
    }
    m_mutex.unlock();
    return false;
}

template<class T>
bool block_queue<T>::front(T& value)
{
    m_mutex.lock();
    if ( 0 == m_size)
    {
        m_mutex.unlock();
        return false;
    }
    value = m_array[m_front];
    m_mutex.unlock();
    return true;
}

template<class T>
bool block_queue<T>::back(T& vlaue)
{
    m_mutex.lock();
    if ( 0 == m_size )
    {
        m_mutex.unlock();
        return false;
    }
    value = m_array[m_back];
    m_mutex.unlock();
    return true;
}

template<class T>
int block_queue<T>::size()
{
    int tmp = 0;
    
    m_mutex.lock();
    tmp  = m_size;

    m_mutex.unlock();
    return tmp;
}

template<class T>
int block_queue<T>::max_size()
{
    int tmp = 0;

    m_mutex.lock();
    tmp = m_max_size;

    m_mutex.unlock();
    return tmp;
}


template<class T>
bool block_queue<T>::push( const T& item)
{
    m_mutex.lock();
    if ( m_size >= m_max_size )
    {
        m_cond.broadcast();
        m_mutex.unlock();
        return false;
    }

    m_back = ( m_back + 1 ) % m_max_size;
    m_array[m_back] = item;

    ++m_size;

    m_cond.broadcast();
    m_mutex.unlock();
    return true;
}


template<class T>
bool block_queue<T>::pop( T& item)
{
    m_mutex.lock();
    while( m_size <= 0 )
    {
        if ( !m_cond.wait(m_mutex.get()))
        {
            m_mutex.unlock();
            return false;
        }
    }

    m_front = ( m_front + 1 ) % m_max_size;
    item = m_array[m_front];
    --m_size;
    m_mutex.unlock();
    return true;
}


template<class T>
bool block_queue<T>::pop( T& item, int ms_timeout)
{
    struct timespec t = {0,0};
    struct timeval now = {0,0};
    gettimeofday(&now, NULL);
    m_mutex.lock();
    if ( m_size <= 0 )
    {
        t.tv_sec = now.tv_sec + ms_timeout / 1000;
        t.tv_nsec = (ms_timeout % 1000 ) * 1000;
        if ( !m_cond.timewait(m_mutex.get(), t))
        {
            m_mutex.unlock();
            return false;
        }
    }

    // 只等待一次唤醒
    if ( m_size <= 0 )
    {
        m_mutex.unlock();
        return false;
    }

    m_front = ( m_front + 1 ) % m_max_size;
    item = m_array[m_front];
    --m_size;
    m_mutex.unlock();
    return true;
}

#endif