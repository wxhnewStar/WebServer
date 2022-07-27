#include "sql_connection_poll.h"
#include <stdio.h>
#include <pthread.h>

connection_poll::connection_poll()
{
    this->m_used_conn = 0;
    this->m_free_conn = 0;
}

connection_poll::~connection_poll()
{
    DestroyPoll();
}

connection_poll* connection_poll::GetInstance()
{
    // c++ 11 以后静态局部变量初始化，会保证其多线程安全
    static connection_poll conn_Poll;
    return &conn_Poll;
}

void connection_poll::init(string url, string user, string password, string database_name, int port, unsigned int max_conn)
{
    m_url = url;
    m_user = user;
    m_password = password;
    m_database_name = database_name;
    m_port = port;
    
    m_locker.lock();
    for ( int i  = 0; i < max_conn; ++i )
    {
        MYSQL* con = NULL;
        con = mysql_init(con);  // 初始化

        if ( con == NULL )
        {
            cout << "Mysql Error: " << mysql_error(con);
            exit(1);
        }

        con = mysql_real_connect( con, m_url.c_str(), m_user.c_str(), m_password.c_str(),
            m_database_name.c_str(), m_port, NULL, 0 );
        if ( con = NULL )
        {
            cout<< "Mysql Error: " << mysql_errno(con);
            exit(1);
        }
        m_conn_list.push_back(con);
        ++m_free_conn;
    }

    m_liststat = sem(m_free_conn);  // 信号量的值即为当前可用的连接数量

    this->m_max_Conn = m_free_conn; // 根据实际建立起来的连接来确定 max_conn

    m_locker.unlock();
}


// 当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL* connection_poll::GetConnection()
{
    MYQSL* con = NULL;

    if ( 0 == m_conn_list.size() )
        return NULL;
    
    m_liststat.wait();

    m_locker.lock();
    
    con = m_conn_list.front();
    m_conn_list.pop_front();

    --m_free_conn;
    ++m_used_conn;

    m_locker.unlock();
    return con;
}

// 释放当前使用的连接
bool connection_poll::ReleaseConnection( MYSQL* con )
{
    if ( con == NULL )
        return false;
    
    m_locker.lock();
    
    m_conn_list.push_back(con);

    ++m_free_conn;
    --m_used_conn;

    m_locker.unlock();
    m_liststat.post();
    return true;
}

// 销毁数据库连接池
// ??: destroy 的时候会不会有连接还没 Release 呢？
void connection_poll::DestroyPoll()
{
    m_locker.lock();
    
    if ( m_conn_list.size() == 0 )
    {
        m_locker.unlock();
        return;
    }

    for( int i = 0 ; i < m_conn_list.size(); ++i )
    {
        mysql_close( m_conn_list[i] );
    }

    m_used_conn = 0;
    m_free_conn = 0;
    m_conn_list.clear();

    m_locker.unlock();
}

// 获取当前空闲的连接数
int connection_poll::GetFreeConn()
{
    int tmp = 0;
    m_locker.lock();
    tmp = m_free_conn;
    m_locker.unlock();
    return tmp;
}

// 这个类每次创建时，从连接池中获取一个连接，在析构时释放
connection_RAII::connection_RAII(MYSQL** con , connection_poll* connPoll)
{
    *con = connPoll->GetConnection();

    conRAII = *con;
    pollRAII = connPoll;
}

connection_RAII::~connection_RAII()
{
    pollRAII->ReleaseConnection(conRAII);
}