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

#define MAX_FD 65536           // 最大文件描述符
#define MAX_EVENT_NUMBER 10000 // 最大监听事件数
#define TIMESLOT 5             // 最小超时单位



/* --- 设置统一事件源 ---*/
static int pipefd[2];

// 将文件描述符设置成非阻塞
int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option);
    return old_option;
}



/* --- 信号处理部分 --- */

// 信号处理函数
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


int main( int argc, char **argv)
{
    /* 判断执行程序时参数正确与否 */
    if ( argc <= 1)
    {
        printf( "usage %s ip_address port_number\n", basename(argv[0]));
        return 1; //出现错误，提前返回
    }

    int port = atoi(argv[1]);

    // 读端关闭的socket中再写入数据，会引起默认的 Term 操作，因此这里改成忽略该信号
    addsig( SIGPIPE, SIG_IGN);

    // 创建线程池
    

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


    // 创建事件通知的管道
    ret = socketpair( PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1]);


}