#ifndef HTTPCONNECTION_H
#define HTTPCONNECTOIN_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include "../lock/locker.h"
#include "../sqlpoll/sql_connection_poll.h"


class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;

    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };

    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };

    enum HTTP_CODE
    {
        NO_REQUEST = 0,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };

    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };


public:
    http_conn() {}
    ~http_conn() {}

public:
    void init( int sockfd, const sockaddr_in& addr );
    void close_conn( bool real_close = true );
    void process();
    bool read_once();
    bool write();

    sockaddr_in* get_address()
    {
        return &m_address;
    }

    void initmysql_result( connection_poll * conn_pool );

private:
    void init();
    HTTP_CODE process_read();
    bool process_write( char* text );
    HTTP_CODE parse_request_line( char* text );
    HTTP_CODE parse_headers( char* text );
    HTTP_CODE parse_content( char* text );
    HTTP_CODE do_request();
    char* get_line() { return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();
    void unmap();
    bool add_responce(const char* format, ...);
    bool add_content(const char* content);
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epoll_fd;
    static int m_user_count;
    MYSQL *mysql;

private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_read_idx;
    int m_checked_idx;
    int m_start_idx;
    int m_start_line;
    int m_write_idx;
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    int m_content_length;
    bool m_linger;
    char *m_file_address;
    struct stat m_file_stat;  // 保存返回用户文件的状态，在接触映射的时候可以用到其 st_size（即文件的长度）
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;            // 是否启用的 POST
    char* m_string;     // 存储请求头数据
    int bytes_to_send;
    int bytes_have_send;
};

#endif