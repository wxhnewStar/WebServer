#include <sys/socket.h>
#include <netinet/in.h>  // 使用了这个头文件中的主机字节序和网络字节序转函数
#include <arpa/inet.h>   // 使用了这个头文件中的 点分十进制字符串到网络字节序整数表示的 IP 地址转换
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include <string.h>
#include <signal.h>
#include <bits/signum.h> // 保存着 Linux 的可用信号，包括：标准信号 及 POSIX实时信号


#include "./lock/locker.h"
#include "./threadpoll/threadpoll.h"
#include "./sqlpoll/sql_connection_poll.h"
#include "./log/log.h"
#include "./http/http_conn.h"
#include "./timer/lst_timer.h"

#define MAX_FD 65536           // 最大文件描述符
#define MAX_EVENT_NUMBER 10000 // 最大监听事件数
#define TIMESLOT 5             // 最小超时单位


/*--- 两个全局设置，通过宏定义  ---*/

#define SYNLOG  //同步写日志
//#define ASYNLOG //异步写日志

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞


/* --- 设置统一事件源及定时器相关参数 ---*/
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

// 在 http_conn 中定义的三个函数，有关于连接在 epoll 中的添加删除及非阻塞设置
extern int addfd(int epollfd, int fd, bool one_shot);
extern int removefd( int epollfd, int fd);
extern int setnonblocking( int fd );



/* --- 信号处理部分 --- */

// 信号处理函数:将信号传给通道
void sig_handler( int sig )
{
    // 为保证函数的可重入性，保留原来的 errno
    int save_errno  = errno;
    int msg = sig;
    send( pipefd[1], char(*) &msg, 1, 0); // 发送给事件统一处理
    errno  = save_errno;
}

// 设置信号函数
void addsig( int sig ,void(handler)(int), bool restart = true )
{
    // signal 系统调用，用于给信号类型指定一个处理函数, 返回值是之前的信号处理函数
    // sigaction 是更健壮的接口,同时设置 1）信号处理函数 2）信号掩码

    struct sigaction sa;
    memset( &sa, '\0', sizeof(sa) );
    sa.sa_handler = handler;
    if ( restart )
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert( sigaction( sig, &sa, NULL) != -1 );
    
}


/* --- 定时器相关函数 --- */

// 定时处理任务，重新定时以不断出发 SIGALRM 信号
void timer_handler()
{
    timer_lst.tick();
    alarm(TIMESLOT);  // 在 TIMESLOT 后触发定时器
}

// 定时器回调函数，删除非活动连接在 socket 上的注册事件，并关闭
void cb_func( client_data* user_data )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0 );
    assert( user_data );
    close( user_data->sockfd );
    http_conn::m_user_count--;
    LOG_INFO("close fd %d", user_data->sockfd );
    Log::get_instance()->flush();
}


void show_error( int connfd, const char* info )
{
    printf("%s" , info);
    send( connfd, info , strlen(info), 0 );
    close( connfd );
}

int main( int argc, char **argv)
{

#ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif


    /* 判断执行程序时参数正确与否 */
    if ( argc <= 1)
    {
        printf( "usage %s ip_address port_number\n", basename(argv[0]));
        return 1; //出现错误，提前返回
    }

    int port = atoi(argv[1]);

    // 读端关闭的socket中再写入数据，会引起默认的 Term 操作，因此这里改成忽略该信号
    addsig( SIGPIPE, SIG_IGN);

    // 创建数据库连接池
    connection_poll *connPoll = connection_poll::GetInstance();
    connPoll->init("localhost", "navicat", "20161016", "wxhdb", 3306, 8);

    // 创建线程池
    threadpoll<http_conn> *poll = NULL;
    try
    {
        poll = new threadpoll<http_conn>( connPoll );
    }
    catch (...)
    {
        return 1;
    }

    // 创建用户数组
    http_conn *users = new http_conn[MAX_FD]; // 记录着所有的连接，对应下标就是 fd
    assert( users );

    // 初始化数据库读取 user 表
    users->initmysql_result( connPoll );
    

    // 注册监听事件描述符
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );  // 参数1：协议族、 参数2：服务类型（流服务、数据报服务）、参数3：预定的设置，通用写0代表默认配置
    assert( listenfd >= 0);

    // 初始化监听地址
    int ret = 0;
    struct sockaddr_in address;  // sockaddr 是通用地址结构， sockaddr_in 是专用地址结构
    bzero( &address, sizeof(address));
    address.sin_family = AF_INET; // 指定地址族 address protocol
    address.sin_addr.s_addr = htonl(INADDR_ANY); // 即监听所有地址？ 一般这部分是采用 inet_pton 这个函数来转化ip为网络字节序
    address.sin_port = htons( port );

    // 开始绑定地址与监听事件, 并开启监听
    int flag = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag) );  // 设置 SO_REUSEADDR 来强制使用处于 TIME_WAIT 状态的连接占用的socket地址
    ret = bind( listenfd, (struct sockaddr*)&address, sizeof(address) ); // 将 socket 和 socket 地址 绑定， 称为“socket命名”
    assert( ret >= 0 );  // 失败时返回两种错误码 EACCES  EADDRINUSE
    ret = listen( listenfd, 5 ); // 参数2 backlog： 设置内核监听队列的最大长度
    assert( ret >= 0 );

    // 创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert( epollfd != -1 );

    // 添加对 服务器fd 的监听，并设在连接类中设置 epollfd
    addfd( epollfd, listenfd ,false );
    http_conn::m_epoll_fd = epollfd;

    // 创建事件通知的管道
    ret = socketpair( PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1]);
    addfd( epollfd, pipefd[0], false ); // 监听管道的读事件

    // 设置对 SIGALRM SIGTERM 这两种事件的监听
    addsig( SIGALRM, sig_handler, false );
    addsig( SIGTERM, sig_handler, false );
    bool stop_server = false;

    client_data* users_timer = new client_data[MAX_FD]; // 所有连接的的定时器数据

    bool timeout = false;
    alarm(TIMESLOT);

    while ( ! stop_server )
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( number < 0 && errno != EINTR )
        {
            LOG_ERROR("%s" , "epoll failure");
            break;
        }

        // 开始循环处理 触发的事件
        for ( int i = 0; i < number; ++i )
        {
            int sockfd = events[i].data.fd;

            // 如果是新连接的客户
            if ( sockfd == listenfd )
            {
                struct sockaddr_in client_address;
                socklen_t client_address_length = sizeof( client_address );
#ifdef listenfdLT
                int connfd = accept( listenfd, (struct sockaddr*)&client_address, &client_address_length );
                if ( connfd < 0 )
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno );
                    continue;
                }
                if ( http_conn::m_user_count >= MAX_FD )
                {
                    // 如果已经超过最大连接数，则直接返回一个信息给客户端
                    show_error( connfd , "Internal server busy");
                    LOG_ERROR( "%s", "Internal server busy" );
                    continue;
                }

                // 添加该连接并进行设置：创建定时器、设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users[connfd].init( connfd, client_address );  // 添加到 users 数组中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer();
                timer->user_data = &users[connfd]; 
                timer->cb_func = cb_func;
                time_t cur = time(NULL); // 获取当前时间
                timer->expire = cur + 3 * TIMESLOT; // 设置超时时间
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
#endif

#ifdef listenfdET
                // 一直接收直到接收完， connfd 返回 -1
                while ( 1 )
                {
                    int connfd = accept( listenfd, (struct sockaddr*)&client_address, &client_address_length );
                    if ( connfd < 0 )
                    {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno );
                        break;
                    }
                    if ( http_conn::m_user_count >= MAX_FD )
                    {
                        // 如果已经超过最大连接数，则直接返回一个信息给客户端
                        show_error( connfd , "Internal server busy");
                        LOG_ERROR( "%s", "Internal server busy" );
                        break;
                    }

                    // 添加该连接并进行设置：创建定时器、设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users[connfd].init( connfd, client_address );  // 添加到 users 数组中
                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer();
                    timer->user_data = &users[connfd]; 
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL); // 获取当前时间
                    timer->expire = cur + 3 * TIMESLOT; // 设置超时时间
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif
            }
            else if ( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) )
            {
                // 如果某个连接客户发来关闭，则服务器端关闭该连接，移除对应的定时器
                util_timer* timer = users_timer[sockfd].timer;
                timer->cb_func( &users_timer[sockfd] );

                if ( timer )
                {
                    timer_lst.del_timer(timer);
                }
            }
            else if ( ( sockfd == pipefd[0]) && ( events[i].events & EPOLLIN ) )
            {
                // 处理信号
                 int sig;
                 char signals[1024];
                 ret = recv( pipefd[0], signals, sizeof(signals), 0 );
                 if ( ret == -1 )
                 {
                    continue;
                 }
                 else if ( ret == 0 )
                 {
                    continue;
                 }
                 else
                 {
                    for ( int i = 0; i < ret; ++i )
                    {
                        switch ( signals[i] )
                        {
                        case SIGALRM:
                        {
                            timeout = true; // 记录是否要执行定时任务的变量置位
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                        }
                        default:
                            break;
                        }
                    }
                 }
            }
            else if( events[i].events & EPOLLIN )
            {
                // 处理客户连接上接收到的数据,接收完再将任务打包给线程池！
                util_timer *timer = users_timer[sockfd].timer;
                if( users[sockfd].read_once() )
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    // 若检测到读事件，将该事件放入请求队列
                    if ( !pool->append( users + sockfd ) )
                    {
                        // 当线程池的 工作队列满时，无法添加，则打日志
                        LOG_ERROR("%s","thread poll workqueue is full");
                        Log::get_instance()->flush();
                        continue;
                    } 
                    
                    // 若有数据传输，则将定时器往后延迟 3 个单位,即重新激活咯
                    // 并对新的定时器在链表上的位置进行调整
                    if ( timer )
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    // 读取数据出错的时候，则关闭，不处理
                    timer->cb_func( &users_timer[sockfd] );
                    LOG_ERROR("client(%s) read fail", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    if ( timer )
                    {
                        timer_lst.del_timer( timer );
                    }
                }
            }
            else if ( events[i].events & EPOLLOUT )
            {
                // 已经处理好客户端需要返回的内容，准备写数据回去
                util_timer* timer = users_timer[sockfd].timer;
                if ( users[sockfd].write() )
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();

                    // 若有数据传输，则重新将该连接往后延 3 个单位
                    // 并对新的定时器在链表上的位置进行调整
                    if ( timer )
                    {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO( "%s", "adjust timer once" );
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer( timer );
                    }
                }
                else
                {
                    // 写回数据失败
                    timer->cb_func( &users_timer[sockfd] );
                    LOG_ERROR("client(%s) write fail", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    if ( timer )
                    {
                        timer_lst.del_timer( timer );
                    }
                }
            }
        }

        if ( timeout )
        {
            // 如果前面有接收到信号 SIGALRM
            timer_handler();
            timeout = false;
        }
    }

    // 收尾工作，关闭服务器后关闭所有相关的文件描述符
    close(epollfd);
    close(listenfd);
    close(pipefd[0]);
    close(pipefd[1]);
    delete []users;
    delete []users_timer;
    delete poll;

    // 最后打个日志
    LOG_INFO("%s", "server close , yeah~");
    return 0;
}