#ifndef SRVMGR_H
#define SRVMGR_H

#include <map>
#include <arpa/inet.h>
#include "fdwrapper.h"
#include "conn.h"

using std::map;

// 主机类存放一些信息
class host
{
public:
    char m_hostname[1024];
    int m_port;
    int m_conncnt;
};

// manager: 网络连接和负载均衡
class mgr
{
public:
    mgr( int epollfd, const host& srv );
    ~mgr();

    /* 客户端连接服务器 */
    int conn2srv( const sockaddr_in& address ); // 连接到服务器
    conn* pick_conn( int sockfd ); // 选择一个连接给该客户端使用
    void free_conn( conn* connection ); // 清除连接
    int get_used_conn_cnt(); // 获取已使用的连接数
    void recycle_conns(); // 将 freed 转化为 conns
    RET_CODE process( int fd, OP_TYPE type ); // 处理对应 fd(客户端或服务器端) 的读或写

private:
    static int m_epollfd; // epollfd
    map< int, conn* > m_conns; // connections
    map< int, conn* > m_used; // used
    map< int, conn* > m_freed; // freed
    host m_logic_srv; // logical server
};

#endif
