#ifndef LST_TIMER_H
#define LST_TIMER_H

#include <time.h>

class util_timer;  // 前向说明

struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};


// 定时器类
class util_timer
{
public:
    util_timer() : prev(NULL),next(NULL){}

public:
    time_t expire;
    void (*cb_func)(client_data*);
    client_data* user_data;
    util_timer* prev;
    util_timer* next;
};

//  定时器容器，其本质为一个升序链表
class sort_timer_lst
{
public:
    sort_timer_lst(): head(NULL), tail(NULL) {}
    ~sort_timer_lst()
    {
        util_timer* tmp = head;
        while( tmp )
        {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }

    void add_timer( util_timer* timer )
    {
        if ( !timer )
        {
            return;
        }
        if ( !head )
        {
            head = tail = timer;
            return;
        }

        if ( timer->expire < head->expire )
        {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }

        // 调用辅助函数插在后面
        add_timer(timer, head);
    }

    // 当某个定时任务发生变化的时候，调整对应的定时器在链表中的位置。这个函数只考虑被调整的
    // 定时器的超时时间延长的情况，即该定时器需要往链表的尾部移动
    void adjust_timer( util_timer* timer )
    {
        if ( !timer )
            return;
            
        util_timer* tmp = timer->next;
        if (!tmp || (timer->expire < tmp->expire))
        {
            return;
        }
        if ( timer == head )
        {
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer, head);
        }
        else
        {
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }

    // 删除特定的定时器
    void del_timer( util_timer* timer)
    {
        if ( !timer )
            return;
        
        if ( (timer == head) && ( timer == tail) )
        {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        if ( timer == head )
        {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }
        if ( timer == tail )
        {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    // SIGALRM 信号出发后，就调用这个函数，用于处理链表上到期的任务
    void tick()
    {   
        if( !head )
        {
            return;
        }

        time_t cur = time(NULL);
        util_timer* tmp = head;
        while( tmp )
        {
            if ( cur < tmp->expire )
            {
                break;
            }
            tmp->cb_func( tmp->user_data );
            head = tmp->next;
            if ( head )
            {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }

private:
    // 重载的辅助函数，被公有的 add_timer 函数和 adjust_timer 函数调用。表示将目标定时器 timer 添加到节点 lst_head 之后的部分链表中
    void add_timer(util_timer* timer, util_timer*lst_head)
    {
        util_timer* prev = lst_head;
        util_timer* tmp = prev->next;

        while( tmp )
        {
            // 找到第一个大于目标定时器的节点，插入在其前
            if ( timer->expire < tmp->expire )
            {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }

        // 即 timer 应该插在尾部时
        if ( !tmp )
        {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }

private:
    util_timer* head;
    util_timer* tail;
};


#endif