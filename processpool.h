#ifndef PROCESSPOOL_H
#define PROCESSPOOL_H

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
#include <vector>
#include "log.h"
#include "fdwrapper.h"

using std::vector;

// 线程:  budy_ratio, pid, pipefd
class process
{
public:
    process() : m_pid( -1 ){}

public:
    int m_busy_ratio; // 使用率
    pid_t m_pid; // 进程 id
    int m_pipefd[2]; // 进程通信管道
};

// 线程池
template< typename C, typename H, typename M >
class processpool
{
private:
    // 构造函数设置为私有，防止被调用
    processpool( int listenfd, int process_number = 8 );
public:
    // 返回一个线程池实例，使用单例模式
    static processpool< C, H, M >* create( int listenfd, int process_number = 8 )
    {
        if( !m_instance )
        {
            m_instance = new processpool< C, H, M >( listenfd, process_number );
        }
        return m_instance;
    }
    ~processpool()
    {
        delete [] m_sub_process;
    }
    void run( const vector<H>& arg );

private:
    void notify_parent_busy_ratio( int pipefd, M* manager );
    int get_most_free_srv();
    void setup_sig_pipe();
    void run_parent();
    void run_child( const vector<H>& arg );

private:
    static const int MAX_PROCESS_NUMBER = 16; // 最大进程数
    static const int USER_PER_PROCESS = 65536; // 每个进程最大用户数
    static const int MAX_EVENT_NUMBER = 10000; // 最大事件数
    int m_process_number; // 进程数
    int m_idx; // 进程 id
    int m_epollfd;
    int m_listenfd; // 监听 fd
    int m_stop; // 是否停止？
    process* m_sub_process; // 子进程数组

    static processpool< C, H, M >* m_instance; // 进程池实例
};


// 这段的意义是什么呢，静态类内变量不是本来就是NULL？
template< typename C, typename H, typename M >
processpool< C, H, M >* processpool< C, H, M >::m_instance = NULL;

static int EPOLL_WAIT_TIME = 5000; // 等待时间
static int sig_pipefd[2]; // 信号管道

// signal handler
static void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    // 将信号写到管道1
    // 收到信号后将信号发送给 sig_pipefd[0]，引起 epoll 的唤醒
    send( sig_pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

// add signal
static void addsig( int sig, void( handler )(int), bool restart = true )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if( restart )
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

//=================================================================================
// 进程池初始化：建立和每个子进程的双向管道、子进程任务数初始化，并给出对应 id
template< typename C, typename H, typename M >
processpool< C, H, M >::processpool( int listenfd, int process_number )
    : m_listenfd( listenfd ), m_process_number( process_number ), m_idx( -1 ), m_stop( false )
{
    assert( ( process_number > 0 ) && ( process_number <= MAX_PROCESS_NUMBER ) );

    m_sub_process = new process[ process_number ];
    assert( m_sub_process );

    for( int i = 0; i < process_number; ++i )
    {
        // Unix domain
        int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd );
        assert( ret == 0 );

        // 此时创建子进程、此后多个进程同步运行
        m_sub_process[i].m_pid = fork();
        assert( m_sub_process[i].m_pid >= 0 );
        // father
        if( m_sub_process[i].m_pid > 0 )
        {
            close( m_sub_process[i].m_pipefd[1] );
            m_sub_process[i].m_busy_ratio = 0;
            continue;
        }
        // child
        else
        {
            close( m_sub_process[i].m_pipefd[0] );
            m_idx = i;
            break;
        }
    }
}

// get most free server
template< typename C, typename H, typename M >
int processpool< C, H, M >::get_most_free_srv()
{
    int ratio = m_sub_process[0].m_busy_ratio;
    int idx = 0;
    for( int i = 0; i < m_process_number; ++i )
    {
        if( m_sub_process[i].m_busy_ratio < ratio )
        {
            idx = i;
            ratio = m_sub_process[i].m_busy_ratio;
        }
    }
    return idx;
}

// set up signal pipe
template< typename C, typename H, typename M >
void processpool< C, H, M >::setup_sig_pipe()
{
    m_epollfd = epoll_create( 5 ); // 创建一个大小为 5 的 epoll
    assert( m_epollfd != -1 );

    int ret = socketpair( PF_UNIX, SOCK_STREAM, 0, sig_pipefd );
    assert( ret != -1 );

    setnonblocking( sig_pipefd[1] ); // 写非阻塞
    add_read_fd( m_epollfd, sig_pipefd[0] ); // 读添加到 epoll

    // 添加 sigchld, sigterm, sigint 的处理函数
    addsig( SIGCHLD, sig_handler );
    addsig( SIGTERM, sig_handler );
    addsig( SIGINT, sig_handler );
    // 鹘落 sigpipe
    addsig( SIGPIPE, SIG_IGN );
}

// run 
template< typename C, typename H, typename M >
void processpool< C, H, M >::run( const vector<H>& arg )
{
    if( m_idx != -1 )
    {
        run_child( arg );
        return;
    }
    run_parent();
}

// notify parent busy ratio
template< typename C, typename H, typename M >
void processpool< C, H, M >::notify_parent_busy_ratio( int pipefd, M* manager )
{
    int msg = manager->get_used_conn_cnt();
    send( pipefd, ( char* )&msg, 1, 0 );
}

//===============================================================================

// run child
template< typename C, typename H, typename M >
void processpool< C, H, M >::run_child( const vector<H>& arg )
{
    // 建立信号管道
    setup_sig_pipe();

    int pipefd_read = m_sub_process[m_idx].m_pipefd[ 1 ];
    add_read_fd( m_epollfd, pipefd_read ); // 子线程的读管道

    epoll_event events[ MAX_EVENT_NUMBER ];

    M* manager = new M( m_epollfd, arg[m_idx] ); // 新建管理者
    assert( manager );

    int number = 0;
    int ret = -1;

    while( ! m_stop )
    {
        number = epoll_wait( m_epollfd, events, MAX_EVENT_NUMBER, EPOLL_WAIT_TIME );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "epoll failure" );
            break;
        }

        if( number == 0 ) // 没有就绪, 进行一下进程回收
        {
            manager->recycle_conns();
            continue;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            if( ( sockfd == pipefd_read ) && ( events[i].events & EPOLLIN ) ) // 父进程发来的消息
            {
                // 由run_parent 可知，client 只是作为一个有连接来的通知
                int client = 0;
                ret = recv( sockfd, ( char* )&client, sizeof( client ), 0 );
                if( ( ( ret < 0 ) && ( errno != EAGAIN ) ) || ret == 0 )
                {
                    continue;
                }
                else
                {
                    struct sockaddr_in client_address;
                    socklen_t client_addrlength = sizeof( client_address );
                    // 建立连接，由此可知 accept 是一种读事件
                    int connfd = accept( m_listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                    if ( connfd < 0 )
                    {
                        log( LOG_ERR, __FILE__, __LINE__, "errno: %s", strerror( errno ) );
                        continue;
                    }
                    add_read_fd( m_epollfd, connfd ); // 将刚刚建立的连接加入读监听
                    C* conn = manager->pick_conn( connfd );
                    if( !conn )
                    {
                        closefd( m_epollfd, connfd );
                        continue;
                    }
                    conn->init_clt( connfd, client_address );
                    notify_parent_busy_ratio( pipefd_read, manager );
                }
            }
            else if( ( sockfd == sig_pipefd[0] ) && ( events[i].events & EPOLLIN ) ) // 信号
            {
                int sig;
                char signals[1024];
                ret = recv( sig_pipefd[0], signals, sizeof( signals ), 0 );
                if( ret <= 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i )
                    {
                        switch( signals[i] )
                        {
                            case SIGCHLD: // 子进程也会收到 sigchld 吗
                            {
                                pid_t pid;
                                int stat;
                                while ( ( pid = waitpid( -1, &stat, WNOHANG ) ) > 0 )
                                {
                                    continue;
                                }
                                break;
                            }
                            case SIGTERM: // 终止子进程
                            case SIGINT:
                            {
                                m_stop = true;
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }
            else if( events[i].events & EPOLLIN ) // 管理者发来的什么？
            {
                 RET_CODE result = manager->process( sockfd, READ );
                 switch( result )
                 {
                     case CLOSED:
                     {
                         notify_parent_busy_ratio( pipefd_read, manager );
                         break;
                     }
                     default:
                         break;
                 }
            }
            else if( events[i].events & EPOLLOUT )
            {
                 RET_CODE result = manager->process( sockfd, WRITE );
                 switch( result )
                 {
                     case CLOSED:
                     {
                         notify_parent_busy_ratio( pipefd_read, manager );
                         break;
                     }
                     default:
                         break;
                 }
            }
            else
            {
                continue;
            }
        }
    }

    close( pipefd_read );
    close( m_epollfd );
}

// run parent
template< typename C, typename H, typename M >
void processpool< C, H, M >::run_parent()
{
    setup_sig_pipe(); // 初始化信号管道，添加管道读就绪
    for( int i = 0; i < m_process_number; ++i )
    {
        add_read_fd( m_epollfd, m_sub_process[i].m_pipefd[ 0 ] ); // 添加子进程的读就绪
    }
    add_read_fd( m_epollfd, m_listenfd ); // 添加监听的读就绪

    epoll_event events[ MAX_EVENT_NUMBER ];
    int sub_process_counter = 0;
    int new_conn = 1;
    int number = 0;
    int ret = -1;

    while( ! m_stop )
    {
        number = epoll_wait( m_epollfd, events, MAX_EVENT_NUMBER, EPOLL_WAIT_TIME ); // 等待事件就绪
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            log( LOG_ERR, __FILE__, __LINE__, "%s", "epoll failure" );
            break;
        }

        // 处理每个就绪的事件
        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            if( sockfd == m_listenfd ) // 监听事件
            {
                /*
                int i =  sub_process_counter;
                do
                {
                    if( m_sub_process[i].m_pid != -1 )
                    {
                        break;
                    }
                    i = (i+1)%m_process_number;
                }
                while( i != sub_process_counter );

                if( m_sub_process[i].m_pid == -1 )
                {
                    m_stop = true;
                    break;
                }
                sub_process_counter = (i+1)%m_process_number;
                */
                int idx = get_most_free_srv(); // 获取任务最少的子进程
                send( m_sub_process[idx].m_pipefd[0], ( char* )&new_conn, sizeof( new_conn ), 0 ); // 将新的连接发送给子进程
                log( LOG_INFO, __FILE__, __LINE__, "send request to child %d", idx );
            }
            else if( ( sockfd == sig_pipefd[0] ) && ( events[i].events & EPOLLIN ) ) // 管道读事件
            {
                int sig;
                char signals[1024];
                ret = recv( sig_pipefd[0], signals, sizeof( signals ), 0 ); // 从信号读取
                if( ret <= 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i ) // 对每个信号进行处理
                    {
                        switch( signals[i] )
                        {
                            case SIGCHLD: // 子进程终止
                            {
                                pid_t pid;
                                int stat;
                                while ( ( pid = waitpid( -1, &stat, WNOHANG ) ) > 0 )
                                {
                                    for( int i = 0; i < m_process_number; ++i )
                                    {
                                        if( m_sub_process[i].m_pid == pid )
                                        {
                                            log( LOG_INFO, __FILE__, __LINE__, "child %d join", i );
                                            close( m_sub_process[i].m_pipefd[0] );
                                            m_sub_process[i].m_pid = -1;
                                        }
                                    }
                                }
                                // 还有子进程在工作就不会停止
                                m_stop = true;
                                for( int i = 0; i < m_process_number; ++i )
                                {
                                    if( m_sub_process[i].m_pid != -1 )
                                    {
                                        m_stop = false;
                                    }
                                }
                                break;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                // 终止所有子进程
                                log( LOG_INFO, __FILE__, __LINE__, "%s", "kill all the clild now" );
                                for( int i = 0; i < m_process_number; ++i )
                                {
                                    int pid = m_sub_process[i].m_pid;
                                    if( pid != -1 )
                                    {
                                        kill( pid, SIGTERM );
                                    }
                                }
                                break;
                            }
                            default:
                            {
                                break;
                            }
                        }
                    }
                }
            }
            else if( events[i].events & EPOLLIN ) // 子进程消息，子进程只会发来自己的 busy_ratio 供自己更新
            {
                int busy_ratio = 0;
                ret = recv( sockfd, ( char* )&busy_ratio, sizeof( busy_ratio ), 0 );
                if( ( ( ret < 0 ) && ( errno != EAGAIN ) ) || ret == 0 )
                {
                    continue;
                }
                for( int i = 0; i < m_process_number; ++i )
                {
                    // 更新该 sockfd 的 busy_ratio?
                    if( sockfd == m_sub_process[i].m_pipefd[0] )
                    {
                        m_sub_process[i].m_busy_ratio = busy_ratio;
                        break;
                    }
                }
                continue;
            }
        }
    }

    // 停止，关闭所有子进程的文件描述符和 epoll
    for( int i = 0; i < m_process_number; ++i )
    {
        closefd( m_epollfd, m_sub_process[i].m_pipefd[ 0 ] );
    }
    close( m_epollfd );
}

#endif
