#include "http_conn.h"
#include "../log/log.h"
#include <map>
#include <mysql/mysql.h>
#include <fstream>

//#define connfdET //边缘触发非阻塞
#define connfdLT //水平触发阻塞

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

// 当浏览器出现连接重置时，可能是网站根目录出错或 http 相应格式出错或访问文件中内容完全为空
const char *doc_root = "/home/ubuntu/github/webserver/root";

// 将表中的用户名和密码放入 map
map<string, string> users;
locker m_lock;


// 连接数据库，并读取用户名和密码
// ?? 后面新注册的用户怎么搞啊？这个一开始就初始化，后面就不更新了吗
void http_conn::initmysql_result( connection_poll* conn_poll)
{
    // 先从连接池中取一个连接
    MYSQL* mysql = NULL;
    connection_RAII mysqlcon(&mysql, conn_poll);

    // 在 user 表中检索 username， passwd 数据，浏览器段输入
    if ( mysql_query( mysql, "SELECT username,passwd FROM user") )
    {
        // 该函数查询成功返回 0， 失败则返回 非 0 值，即进入这里
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 在表中检索完整的结果集
    MYSQL_RES* result = mysql_store_result(mysql);

    // 获取结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD* fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入 map 中
    while ( MYSQL_ROW row = mysql_fetch_row(result) )
    {
        string tmp_u_name(row[0]);
        string tmp_passwd(row[1]);
        users[tmp_u_name] = tmp_passwd;
    }
}

// 将文件描述符设置为非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl( fd,F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册事件，ET 模式，选择开启 EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot )
{
    epoll_event event;
    event.data.fd = fd;
    
    #ifdef connfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    #endif

    #ifdef connfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
    #endif

    #ifdef listenfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    #endif

    #ifdef listenfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
    #endif

    if ( one_shot )
        event.events |= EPOLLONESHOT;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking(fd);
}

// 从内核事件表删除描述符
void removefd( int epollfd, int fd )
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 每次处理完后需要将事件重置为 EPOLLONESHOT
void modfd( int epollfd, int fd, int ev )
{
    epoll_event event;
    event.data.fd = fd;

    #ifdef connfdET
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    #endif

    #ifdef connfdLT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    #endif

    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event);
}


// 初始化静态变量
int http_conn::m_user_count = 0;
int http_conn::m_epoll_fd = -1;


// 关闭连接，客户总量减一
void http_conn::close_conn( bool real_close )
{
    if ( real_close && (m_sockfd != -1) )
    {
        removefd( m_epoll_fd, m_sockfd);
        m_sockfd = -1;
        --m_user_count;
    }
}


// 初始化连接，外部调用初始化套接字地址
void http_conn::init( int sockfd, const sockaddr_in& addr )
{
    m_sockfd = sockfd;
    m_address = addr;

    // 在 init 里进行一个 epoll_fd 的添加，所有的连接类都是用这个
    addfd(m_epoll_fd, sockfd, true);
    ++m_user_count;
    init();
}

// 初始化新接收的连接
// check_state 默认为“分析请求行状态”,private 函数
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = NULL;
    m_version = NULL;
    m_content_length = 0;
    m_host = NULL;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset( m_read_buf , '\0', READ_BUFFER_SIZE );
    memset( m_write_buf, '\0', WRITE_BUFFER_SIZE );
    memset( m_real_file, '\0', FILENAME_LEN );
}


// 从状态机，用于分析出请求的一行的内容
// 返回值：行的读取状态，有 LINE_OK, LINE_BAD, LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx )
    {
        temp = m_read_buf[m_checked_idx];
        if ( temp == '\r' )
        {
            if ( (m_checked_idx + 1) == m_read_idx ) // 此时仍然没有读完
                return LINE_OPEN;
            else if ( m_read_buf[m_checked_idx+1] == '\n')  // 此时已经读到了一整行
            {
                // 将"\r\n"替换成"\0\0"
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if ( temp == '\n' )
        {
            if ( m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r' )
            {
                // 将"\r\n"替换成"\0\0"
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    retrun LINE_OPEN;
}

// 循环读取客户数据，直到无数据可读或对方关闭连接
// 非阻塞 ET 工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if ( m_read_idx >= READ_BUFFER_SIZE )
    {
        return false;
    }
    int bytes_read = 0;

    #ifdef connfdLT
    bytes_read = recv( m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0 );
    if ( bytes_read <= 0 )
    {
        return false;
    }

    m_read_idx += bytes_read;
    return true;
    #endif

    #ifdef connfdET
    while ( true )
    {
        bytes_read = recv( m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if ( bytes_read == -1 )
        {
            if ( errno == EAGAIN || errno == EWOULDBLOCK )
                break;
            return false;
        }
        else if ( bytes_read == 0 )
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
    #endif
}


// 解析 http 请求，获得请求方法，目标 url 以及 http 版本号
http_conn::HTTP_CODE http_conn::parse_request_line( char *text )
{
    // 获取 目标url的起始位置
    // 找到第一个 空格 或 制表符
    m_url = strpbrk(text, " \t");
    if ( !m_url )
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';

    char* method = text;
    if ( strcasecmp( method, "GET" ) == 0 )
        m_method = GET;
    else if ( strcasecmp( method, "POST" ) == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST; // 其他的请求方法不支持

    // 获取 m_version
    m_url += strspn( m_url, " \t" );
    m_version = strpbrk(m_url, " \t");
    if ( !m_version )
        return BAD_REQUEST;
    *m_version++ = '\0';

    // 处理 version
    m_version += strspn(m_version, " \t");
    if ( strcasecmp( m_version, "HTTP/1.1") != 0 )  // 仅支持 http/1.1
        return BAD_REQUEST;
    
    // 跳过 url 中的协议部分
    if ( strncasecmp( m_url, "http://", 7 ) == 0 )
    {
        m_url += 7;
        m_url = strchr( m_url, '/');
    }
    if ( strncasecmp(m_url, "https://", 8 ) == 0 )
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    if ( !m_url || m_url[0] != '/')
        return BAD_REQUEST;
    
    // 当 url 为 / 时，返回判断界面
    if ( strlen(m_url) == 1 )
    {
        strcat(m_url, "judge.html");
    }

    // 读取完请求行了， 进入下一状态
    m_check_state = CHECK_STATE_HEADER;
    
    return NO_REQUEST;
}

// 解析 http 请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers( char* text )
{
    if ( text[0] == '\0' )
    {
        // header 读完以后，如果内容长度等于0说明是 GET，否则要去检查 CONTENT
        if ( m_content_length != 0 )
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if ( strncasecmp( text, "Connection:", 11) == 0 )
    {
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text , "keep-alive") == 0 )
        {
            m_linger = true;
        }
    }
    else if ( strncasecmp( text,  "Content-length:", 15) == 0 )
    {
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    }
    else if ( strncasecmp( text, "Host:", 5) == 0 )
    {
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    }
    else
    {
        LOG_INFO("unknow header: %s", text);
        Log::get_instance()->flush();
    }
    
    return NO_REQUEST;
}



// 判断 http 请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content( char * text )
{
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[m_content_length] = '\0';
        // POST 请求中最后为输入的用户名和密码
        m_string = text;  // 此时 m_string 保存着content中的内容，post 中则为输入的用户名和密码
        return GET_REQUEST;
    }
    return NO_REQUEST;
}


http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = NULL;

    while ( (m_check_state == CHECK_STATE_CONTENT  && line_status == LINE_OK ) || ( ( line_status = parse_line() ) == LINE_OK ) )
    {
        // 获取新的一行
        text = get_line();
        m_start_line = m_checked_idx;

        LOG_INFO("%s", text);
        Log::get_instance()->flush();

        switch ( m_check_state )
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line( text );
            if ( ret == BAD_REQUEST )
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers( text );
            if ( ret == BAD_REQUEST )
                return BAD_REQUEST;
            else if ( ret == GET_REQUEST )
            {
                return do_request();
            }
            // 否则继续跳转到解析 content部分
            break;
        }
        case  CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if ( ret == GET_REQUEST )
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}


// ？？ 用于处理请求，根据请求打开要返回的网页，映射到 m_file_address，
// 除了成功找到文件，还有另外三种错误情况： 无资源、是文件夹、没权限
http_conn::HTTP_CODE http_conn::do_request()
{
    // 先将网站文件根目录加上去
    strcpy( m_real_file, doc_root );

    int len = strlen( doc_root );
    // 获取到 目标url 中最后的文件名？
    const char *p = strrchr(m_url, '/');
    
    // 处理 cgi
    if ( cgi == 1 && ( *(p+1) == '2' || *(p+1) == '3') )
    {
        // 根据标志判断是登录检测还是注册检测 
        // ?? 根本没用到啊？
        char flag = m_url[1];

        char *m_url_real = (char*) malloc(sizeof(char) * 200 );
        strcpy( m_url_real, "/" );
        strcat( m_url_real, m_url + 2 );
        strncpy( m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 将用户名和密码提取出来
        // 格式如 user=123&passwd=123
        char name[100], password[100];
        int i;
        for ( int i = 5; m_string[i] != '&'; ++i )
            name[i-5] = m_string[i]; 
        name[i-5] = '\0';
        int j = 0;
        for ( i = i + 10;  m_string[i] != '\0'; ++i, ++j )
            password[j] = m_string[i];
        password[j]= '\0';

        // 同步线程登录校验
        if ( *(p+1) == '3' )
        {
            // 如果是注册，先检测数据库中是否有重名的
            // 没有重名的，进行增加数据
            char *sql_insert = (char*) malloc(sizeof(char) * 200 );
            strcpy(sql_insert, "INSERT INTO user( name,passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            // 如果在用户列表中没找到，就插入到 user表中
            if ( users.find( name ) == users.end() )
            {
                m_lock.lock();
                int res =  mysql_query( mysql, sql_insert);
                users.insert( pair<string,string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html"); // 成功返回登录界面
                else
                    strcpy(m_url, "/registerError.html"); // 失败返回 error 界面
            }
            else
                strcpy( m_url, "/registerError.html"); // 如果已经存在了，也返回注册失败界面
        }
        // 如果是登录，直接判断
        // 若浏览器端输入的用户名和 密码可以在表中找到，返回1，否则返回 0
        else if ( *(p+1) == '2' )
        {
            if ( users.find( name) != users.end() || users[name] != password )
            {
                strcpy( m_url, "/welcome.html"); // 成功登录
            }
            else
            {
                strcpy( m_url, "/logError.html");
            }
        }
    }

    if ( *(p+1) == '0' )
    {
        char *m_url_real = (char*) malloc( sizeof(char) * 200 );
        strcpy( m_url_real, "/register.html" );
        strncpy( m_real_file + len , m_url_real, strlen(m_url_real) );

        free( m_url_real );
    }
    else if ( *(p+1) == '1' )
    {
        char *m_url_real = ( char* )malloc( sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy( m_real_file + len , m_url_real, strlen(m_url_real) );

        free(m_url_real);
    }
    else if ( *(p+1) == '5' )
    {
        char *m_url_real = (char*) malloc(sizeof(char) * 200);
        strcpy( m_url_real, "/picture.html" );
        strncpy( m_real_file + len, m_url_real, strlen(m_url_real) );
        
        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1 );
    
    // 检查读取文件的状态
    if ( stat( m_real_file , &m_file_stat) < 0 )
        return NO_RESOURCE; // 所有错误直接当成找不到指定文件
    if ( !(m_file_stat.st_mode & S_IROTH) )  // 表示“其他用户是否具备可读权限”
        return FORBIDDEN_REQUEST;
    if ( S_ISDIR(m_file_stat.st_mode) ) // 判断是否为目录的宏
        return BAD_REQUEST;

    // 打开文件并映射到一段共享空间，其长度记录在 m_file_stat 结构体的 st_size 属性中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*) mmap( 0 , m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}


// 将返回文件的 mmap 映射给解除
void http_conn::unmap()
{
    if ( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = NULL;
    }
}


bool http_conn::write()
{
    int temp = 0;

    if ( bytes_to_send == 0 )
    {
        modfd( m_epoll_fd , m_sockfd, EPOLLIN );
        init();
        return true;
    }

    while ( 1 )
    {
        temp = writev( m_sockfd, m_iv, m_iv_count );

        if ( temp < 0 )
        {
            // 写阻塞， 可能发生了写入缓冲区已满之类的问题，此时继续等待下一次写
            if ( errno == EAGAIN )
            {
                modfd( m_epoll_fd, m_sockfd , EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if ( bytes_have_send >= m_iv[0].iov_len )
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + ( bytes_have_send - m_write_idx );
            m_iv[1].iov_len = bytes_to_send; 
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if ( bytes_to_send <= 0 )
        {
            unmap();
            modfd( m_epoll_fd, m_sockfd, EPOLLIN );

            if( m_linger )
            {
                init();
                return true;    // 如果是长连接，就返回 true，重置其他参数
            }
            else
            {
                return false;
            }
        }
    }
}

bool http_conn::add_responce( const char* format, ...)
{
    if ( m_write_idx >= WRITE_BUFFER_SIZE )
        return false;
    va_list arg_list;
    va_start( arg_list, format );
    
    // 将添加的东西复制到 m_write_buf 中去
    int len = vsnprintf( m_write_buf + m_write_idx , WRITE_BUFFER_SIZE - 1- m_write_idx, format , arg_list );
    if ( len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false; // 如果 buf 不够用，直接返回失败
    }

    m_write_idx += len;
    va_end(arg_list);
    LOG_INFO( "request:%s", m_write_buf );
    Log::get_instance() -> flush();
    return true;
}

bool http_conn::add_status_line( int status, const char* title )
{
    return add_responce("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_content_length( int content_len )
{
    return add_responce("Content-Length:%s\r\n", content_len );
}

bool http_conn::add_content_type()
{
    return add_responce("Content-Type:%s\r\n", "text/html"); // 本服务器返回内容只有 html 文件
}

bool http_conn::add_linger()
{
    return add_responce("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_responce("%s", "\r\n");
}

bool http_conn::add_headers( int content_len )
{
    bool result = true;
    result = result &  add_content_length( content_len );
    result = result & add_content_type();
    result = result & add_linger();
    result = result & add_blank_line();
    return result;
}

bool http_conn::add_content( const char* content )
{
    return add_responce("%s" , content );
}

bool http_conn::process_write( HTTP_CODE ret )
{
    // 对于不同的处理结果，对写回内容进行不同的处理
    // ??? 貌似对于 NO_RESOUCE 没有进行处理
    switch ( ret )
    {
    case INTERNAL_ERROR:
    {
        add_status_line( 500 , error_500_title );
        add_headers( strlen( error_500_form ) );
        if ( !add_content(error_500_form ) )
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line( 200 , ok_200_title );
        if ( m_file_stat.st_size != 0 )
        {
            // 如果有要发送的网页，即 content 有内容
            add_headers( m_file_stat.st_size );
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    
    default:
        return false;
    }

    // 只返回头部即可
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}


void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    
    if ( read_ret == NO_REQUEST )
    {
        modfd( m_epoll_fd, m_sockfd , EPOLLIN );
        return;
    }

    bool write_ret = process_write( read_ret );
    if ( !write_ret )
    {
        // 如果写入失败，直接不处理了，关闭连接...
        close_conn();
    }
    modfd( m_epoll_fd, m_sockfd, EPOLLOUT );
}

#endif