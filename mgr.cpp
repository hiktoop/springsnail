#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <exception>
#include "log.h"
#include "mgr.h"

using std::pair;

int mgr::m_epollfd = -1; // peollfd 开始没有
 
// connect to server
int mgr::conn2srv( const sockaddr_in& address )
{
    int sockfd = socket( PF_INET, SOCK_STREAM, 0 );
    if( sockfd < 0 )
    {
        return -1;
    }

    if ( connect( sockfd, ( struct sockaddr* )&address, sizeof( address ) ) != 0  )
    {
        log(LOG_INFO, __FILE__, __LINE__, "connect error");
        close( sockfd );
        return -1;
    }
    return sockfd;
}

// set epollfd and host server
mgr::mgr( int epollfd, const host& srv ) : m_logic_srv( srv )
{
    m_epollfd = epollfd;

    // 初始配置
    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET; // ipv4
    inet_pton( AF_INET, srv.m_hostname, &address.sin_addr );
    address.sin_port = htons( srv.m_port ); // host to net short
    log( LOG_INFO, __FILE__, __LINE__, "logcial srv host info: (%s, %d)", srv.m_hostname, srv.m_port );

    // 创建多个连接并加入到连接表中
    for( int i = 0; i < srv.m_conncnt; ++i )
    {
        sleep( 1 ); // ?/?
        int sockfd = conn2srv( address ); // 建立连接返回 fd
        if( sockfd < 0 )
        {
            log( LOG_ERR, __FILE__, __LINE__, "build connection %d failed", i );
        }
        else
        {
            log( LOG_INFO, __FILE__, __LINE__, "build connection %d to server success", i );
            conn* tmp = NULL;
            try
            {
                tmp = new conn;
            }
            catch( ... ) // 创建连接失败时关闭 sockfd
            {
                close( sockfd );
                continue;
            }
            tmp->init_srv( sockfd, address ); // 只初始化服务端
            m_conns.insert( pair< int, conn* >( sockfd, tmp ) ); // 将连接加入连接表(map)
        }
    }
}

mgr::~mgr()
{
}

// get used connections cnt
int mgr::get_used_conn_cnt()
{
    return m_used.size();
}

// pick connections
conn* mgr::pick_conn( int cltfd  )
{
    if( m_conns.empty() ) // 没有连接
    {
        log( LOG_ERR, __FILE__, __LINE__, "%s", "not enough srv connections to server" );
        return NULL;
    }

    // 在连接表头部获得一个连接
    map< int, conn* >::iterator iter =  m_conns.begin();
    int srvfd = iter->first;
    conn* tmp = iter->second;
    if( !tmp )
    {
        log( LOG_ERR, __FILE__, __LINE__, "%s", "empty server connection object" );
        return NULL;
    }
    m_conns.erase( iter );
    // 在 used 里添加 ctlfd 和 sevfd
    // 和客户端的连接是在进程池里创建的，服务器的连接是 mgr 初始化的时候创建的
    // 但是为什么两者都要放到 m_used 里面？
    m_used.insert( pair< int, conn* >( cltfd, tmp ) ); // 传入的客户端 fd
    m_used.insert( pair< int, conn* >( srvfd, tmp ) ); // 之前在 conn 中的信息
    // 添加到 epoll 中
    // run_child 中只将信号和父进程管道加入了 epoll，此处再加入服务端和客户端的 socket
    add_read_fd( m_epollfd, cltfd );
    add_read_fd( m_epollfd, srvfd );
    log( LOG_INFO, __FILE__, __LINE__, "bind client sock %d with server sock %d", cltfd, srvfd );
    return tmp; // 返回连接
}

// free connection
void mgr::free_conn( conn* connection )
{
    int cltfd = connection->m_cltfd;
    int srvfd = connection->m_srvfd;
    // 关闭带有 epoll 任务的 fd
    closefd( m_epollfd, cltfd );
    closefd( m_epollfd, srvfd );
    m_used.erase( cltfd );
    // 从使用表中移除
    m_used.erase( srvfd );
    connection->reset();
    // 记录移除的连接
    m_freed.insert( pair< int, conn* >( srvfd, connection ) );
}

// recycle connection
void mgr::recycle_conns()
{
    if( m_freed.empty() )
    {
        return;
    }
    // assert(!m_freed.empty())
    // 将 freed 表中的所有 conn 重新插入到 connection 表中
    for( map< int, conn* >::iterator iter = m_freed.begin(); iter != m_freed.end(); iter++ )
    {
        sleep( 1 );
        int srvfd = iter->first;
        conn* tmp = iter->second;
        srvfd = conn2srv( tmp->m_srv_address ); // 循环利用的时候要重新建立连接？那岂不是和没有池的无区别？
        if( srvfd < 0 )
        {
            // 怎么变成 srvfd < 0 的？
            log( LOG_ERR, __FILE__, __LINE__, "%s", "fix connection failed");
        }
        else
        {
            log( LOG_INFO, __FILE__, __LINE__, "%s", "fix connection success" );
            tmp->init_srv( srvfd, tmp->m_srv_address );
            m_conns.insert( pair< int, conn* >( srvfd, tmp ) );
        }
    }
    m_freed.clear();
}

// process
RET_CODE mgr::process( int fd, OP_TYPE type ) // OP_TYPE can be read, write and error
{
    conn* connection = m_used[ fd ]; // 获取传入 fd 的连接
    if( !connection )
    {
        return NOTHING;
    }
    if( connection->m_cltfd == fd ) // 处理客户端
    {
        int srvfd = connection->m_srvfd;
        switch( type )
        {
            case READ: // 读
            {
                RET_CODE res = connection->read_clt();
                switch( res )
                {
                    case OK:
                    {
                        log( LOG_DEBUG, __FILE__, __LINE__, "content read from client: %s", connection->m_clt_buf );
                    }
                    case BUFFER_FULL: // 缓冲区满了，开始写
                    {
                        modfd( m_epollfd, srvfd, EPOLLOUT );
                        break;
                    }
                    case IOERR: // io 错误
                    case CLOSED: // 已关闭
                    {
                        free_conn( connection );
                        return CLOSED;
                    }
                    default:
                        break;
                }
                if( connection->m_srv_closed ) // 服务器端关闭
                {
                    free_conn( connection );
                    return CLOSED;
                }
                break;
            }
            case WRITE: // 写
            {
                RET_CODE res = connection->write_clt();
                switch( res )
                {
                    case TRY_AGAIN: // try again
                    {
                        modfd( m_epollfd, fd, EPOLLOUT );
                        break;
                    }
                    case BUFFER_EMPTY: // 缓冲区空
                    {
                        modfd( m_epollfd, srvfd, EPOLLIN );
                        modfd( m_epollfd, fd, EPOLLIN );
                        break;
                    }
                    case IOERR:
                    case CLOSED:
                    {
                        free_conn( connection );
                        return CLOSED;
                    }
                    default:
                        break;
                }
                if( connection->m_srv_closed )
                {
                    free_conn( connection );
                    return CLOSED;
                }
                break;
            }
            default: // 其他情况尚未支持
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "other operation not support yet" );
                break;
            }
        }
    }
    else if( connection->m_srvfd == fd ) // 处理服务端
    {
        int cltfd = connection->m_cltfd;
        switch( type )
        {
            case READ:
            {
                RET_CODE res = connection->read_srv();
                switch( res )
                {
                    case OK:
                    {
                        log( LOG_DEBUG, __FILE__, __LINE__, "content read from server: %s", connection->m_srv_buf );
                    }
                    case BUFFER_FULL:
                    {
                        modfd( m_epollfd, cltfd, EPOLLOUT );
                        break;
                    }
                    case IOERR:
                    case CLOSED:
                    {
                        modfd( m_epollfd, cltfd, EPOLLOUT );
                        connection->m_srv_closed = true;
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
            case WRITE:
            {
                RET_CODE res = connection->write_srv();
                switch( res )
                {
                    case TRY_AGAIN:
                    {
                        modfd( m_epollfd, fd, EPOLLOUT );
                        break;
                    }
                    case BUFFER_EMPTY:
                    {
                        modfd( m_epollfd, cltfd, EPOLLIN );
                        modfd( m_epollfd, fd, EPOLLIN );
                        break;
                    }
                    case IOERR:
                    case CLOSED:
                    {
                        /*
                        if( connection->m_srv_write_idx == connection->m_srvread_idx )
                        {
                            free_conn( connection );
                        }
                        else
                        {
                            modfd( m_epollfd, cltfd, EPOLLOUT );
                        }
                        */
                        modfd( m_epollfd, cltfd, EPOLLOUT );
                        connection->m_srv_closed = true;
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
            default:
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "other operation not support yet" );
                break;
            }
        }
    }
    else // 既不是客户端，也不是服务端，那是啥
    {
        return NOTHING;
    }
    return OK; // 处理完毕
}
