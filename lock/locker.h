#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

/* 封装好的 信号量类  */
class sem
{
public:
    sem()
    {
        if ( sem_init( &m_sem, 0 , 0) != 0 ) // sem 系列函数，成功返回 0 ，失败返回 -1，第二个参数为0 代表这个信号量是进程内共享，而非跨进程共享
        {
            throw std::exception();
        }
    }

    sem( unsigned int num ) 
    {
        if( sem_init( &m_sem, 0, num) != 0 )
        {
            throw std::exception();
        }
    }

    ~sem()
    {
        sem_destroy( &m_sem);
    }

    /*--- caller ---*/
    bool wait()
    {
        return sem_wait( &m_sem ) == 0;
    }

    bool post()
    {
        return sem_post( &m_sem ) == 0;
    }

private:
    sem_t m_sem;
}

/* 封装好的 互斥锁类  */
class locker
{
public:
    locker() {
        if (pthread_mutex_init( &m_mutex, NULL) != 0 )
        {
            throw std::exception();
        }
    }

    ~locker()
    {
        pthread_mutexattr_destroy( &m_mutex );
    }

    /*--- caller ---*/
    bool lock() 
    {
        return pthread_mutex_lock( &m_mutex ) == 0;
    }

    bool unlock()
    {
        return pthread_mutex_unlock( &m_mutex ) == 0;
    }

    pthread_mutex_t* get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
}



/* 封装好的 条件变量类  */
/* --- cond 的函数成功返回 0，失败返回错误码 --- */
class cond
{
public:
    cond()
    {
        if( pthread_cond_init( &m_cond, NULl ) != 0 ) // 第二个参数是设置 条件变量的属性 pthread_condattrt
        {
            throw std::exception();
        }
    }

    ~cond()
    {
        pthread_cond_destroy( &m_cond );
    }

    /*--- caller ---*/
    bool wait( pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        // pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait( &m_cond, m_mutex );
        // pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }

    bool timewait( pthread_mutex_t* m_mutex, struct  timespec t)
    {
        int ret = 0;
        // pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait( &m_cond, m_mutex, &t );
        // pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }

    bool signal()
    {
        return pthread_cond_signal( &m_cond ) == 0;
    }

    bool broadcast()
    {
        return pthread_cond_broadcast( &m_cond ) == 0;
    }
    

private:
    pthread_cond_t m_cond;
}

#endif