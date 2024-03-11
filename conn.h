#ifndef CONN_H
#define CONN_H

#include <arpa/inet.h>
#include "fdwrapper.h"
// 为什么连接又使用类了呢？

class conn
{
public:
    // 构造、析构
    conn();
    ~conn();

    // 初始化、重置
    void init_clt( int sockfd, const sockaddr_in& client_addr );
    void init_srv( int sockfd, const sockaddr_in& server_addr );
    void reset();

    RET_CODE read_clt();
    RET_CODE write_clt();
    RET_CODE read_srv();
    RET_CODE write_srv();

public:
    // 变量
    static const int BUF_SIZE = 2048;

    // clt: client
    char* m_clt_buf;
    int m_clt_read_idx;
    int m_clt_write_idx;
    //---------------
    sockaddr_in m_clt_address;
    int m_cltfd;

    // srv: server
    char* m_srv_buf;
    int m_srv_read_idx;
    int m_srv_write_idx;
    //---------------
    sockaddr_in m_srv_address;
    int m_srvfd;

    bool m_srv_closed;
};

#endif
