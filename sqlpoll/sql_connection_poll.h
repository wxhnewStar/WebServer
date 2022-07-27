#ifndef SQL_CONNECTION_POLL_H
#define SQL_CONNECTION_POLL_H

#include <stdio.h>
#include <list>
#include <error.h>
#include <mysql/mysql.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
using namespace std;


class connection_poll
{
public:

    MYSQL* GetConnection();     // 获取数据库连接
    bool ReleaseConnection(MYSQL*);   // 释放连接
    int  GetFreeConn();         // 获取连接
    void DestroyPoll();         // 销毁所有连接

    // 单例模式
    static connection_poll* GetInstance();

    void init(string url, string user, string password, string database_name, int port, unsigned int max_conn);

private:
    unsigned int m_max_Conn;    // 最大连接数
    unsigned int m_used_conn;   // 当前已使用的连接数
    unsigned int m_free_conn;   // 当前空闲的连接数

private:
    locker m_locker;
    list<MySQL*> m_conn_list;  // 连接池
    sem    m_liststat;

private:
    string m_url;             // 主机地址
    string m_port;            // 数据库端口号
    string m_user;            // 登录数据库用户名
    string m_password;        // 登录数据库密码
    string m_database_name;   // 使用数据库名 

private:
    connection_poll();
    ~connection_poll();
    connection_poll(const connection_poll&) = delete;
    connection_poll& operator=(const connection_poll&) = delete;
};



class connection_RAII
{
private:
    MYSQL *conRAII;
    connection_poll* pollRAII;
public:
    connection_RAII( MYSQL** con , connection_poll* connPoll);
    ~connection_RAII();
};



#endif