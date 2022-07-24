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


#include "./lock/locker.h"

#define MAX_FD 65536           // 最大文件描述符
#define MAX_EVENT_NUMBER 10000 // 最大监听事件数
#define TIMESLOT 5             // 最小超时单位


int main( int argc, char **argv)
{
    /* 判断执行程序时参数正确与否 */
    if ( argc <= 1)
    {
        printf( "usage %s ip_address port_number\n", basename(argv[0]));
        return 1; //出现错误，提前返回
    }

    int port = atoi(argv[1]);

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

    
}