#ifndef FDWRAPPER_H
#define FDWRAPPER_H

#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>

// set no block
int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL ); // 获取文件状态
    int new_option = old_option | O_NONBLOCK; // 状态改为非阻塞
    fcntl( fd, F_SETFL, new_option ); // 设置文件状态
    return old_option;
}

// add read fd to epoll
void add_read_fd( int epollfd, int fd )
{
    epoll_event event;
    event.data.fd = fd; // data 存储信息
    event.events = EPOLLIN | EPOLLET; // events 表示事件
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event ); // 可选 ADD, MOD, DEL
    setnonblocking( fd ); // 添加到 epoll 之后就要设置非阻塞，以使得 fd 不再阻塞，而是就绪之后再执行对应的回调函数?
}

// add write fd to epoll
void add_write_fd( int epollfd, int fd )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLOUT | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}

// remove fd from epoll and close fd
void closefd( int epollfd, int fd )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 ); // 删除对该 fd 的监控
    close( fd ); // 直接关闭文件描述符
}

// remove fd from epoll
void removefd( int epollfd, int fd )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
}

// change the fd's mod in epoll
void modfd( int epollfd, int fd, int ev )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

#endif
