
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>
#include <ngx_event_connect.h>


/* 主动发起连接 */
ngx_int_t
ngx_event_connect_peer(ngx_peer_connection_t *pc)
{
    int                rc;
    ngx_int_t          event;
    ngx_err_t          err;
    ngx_uint_t         level;
    ngx_socket_t       s;
    ngx_event_t       *rev, *wev;
    ngx_connection_t  *c;

    /* 获取连接 */
    rc = pc->get(pc, pc->data);
    if (rc != NGX_OK) {
        return rc;
    }

    /* 调用ngx_socket创建一个TCP套接字 */
    s = ngx_socket(pc->sockaddr->sa_family, SOCK_STREAM, 0);

    ngx_log_debug1(NGX_LOG_DEBUG_EVENT, pc->log, 0, "socket %d", s);

    if (s == (ngx_socket_t) -1) {
        ngx_log_error(NGX_LOG_ALERT, pc->log, ngx_socket_errno,
                      ngx_socket_n " failed");
        return NGX_ERROR;
    }


    /* 从ngx_cycle_t结构中的free_connections成员指向的空闲连接池获取一个ngx_connection_t连接结构体 */
    c = ngx_get_connection(s, pc->log);

    if (c == NULL) {
        if (ngx_close_socket(s) == -1) {
            ngx_log_error(NGX_LOG_ALERT, pc->log, ngx_socket_errno,
                          ngx_close_socket_n "failed");
        }

        return NGX_ERROR;
    }

    /*
     * 若定义了连接的接收缓冲区标志，则调用setsockopt设置套接字缓冲区选项 */
    if (pc->rcvbuf) {
        if (setsockopt(s, SOL_SOCKET, SO_RCVBUF,
                       (const void *) &pc->rcvbuf, sizeof(int)) == -1)
        {
            ngx_log_error(NGX_LOG_ALERT, pc->log, ngx_socket_errno,
                          "setsockopt(SO_RCVBUF) failed");
            goto failed;
        }
    }

    /* 设置该TCP套接字为非阻塞模式 */
    if (ngx_nonblocking(s) == -1) {
        ngx_log_error(NGX_LOG_ALERT, pc->log, ngx_socket_errno,
                      ngx_nonblocking_n " failed");

        goto failed;
    }

    /* 若连接定义了本地地址信息，则调用bind将该TCP套接字绑定到该地址中 */
    if (pc->local) {
        if (bind(s, pc->local->sockaddr, pc->local->socklen) == -1) {
            ngx_log_error(NGX_LOG_CRIT, pc->log, ngx_socket_errno,
                          "bind(%V) failed", &pc->local->name);

            goto failed;
        }
    }

    /* 设置连接 接收 和 发送 网络字符流的方法 */
    c->recv = ngx_recv;
    c->send = ngx_send;
    /* 以ngx_chain_t链表方法接收、发送网络字符流 */
    c->recv_chain = ngx_recv_chain;
    c->send_chain = ngx_send_chain;

    /* 表示正在将文件的数据发往对端 */
    c->sendfile = 1;

    c->log_error = pc->log_error;

    /* 这里是针对UNIX域套接字来设置 */
    if (pc->sockaddr->sa_family == AF_UNIX) {
        c->tcp_nopush = NGX_TCP_NOPUSH_DISABLED;
        c->tcp_nodelay = NGX_TCP_NODELAY_DISABLED;

#if (NGX_SOLARIS)
        /* Solaris's sendfilev() supports AF_NCA, AF_INET, and AF_INET6 */
        c->sendfile = 0;
#endif
    }

    /* 设置连接的读、写事件方法 */
    rev = c->read;
    wev = c->write;

    rev->log = pc->log;
    wev->log = pc->log;

    /* 设置主动连接 */
    pc->connection = c;

    c->number = ngx_atomic_fetch_add(ngx_connection_counter, 1);

#if (NGX_THREADS)

    /* TODO: lock event when call completion handler */

    rev->lock = pc->lock;
    wev->lock = pc->lock;
    rev->own_lock = &c->lock;
    wev->own_lock = &c->lock;

#endif

    /* 将TCP套接字以期待 EPOLLIN | EPOLLOUT 事件的方式添加到epoll机制中 */
    if (ngx_add_conn) {
        if (ngx_add_conn(c) == NGX_ERROR) {
            goto failed;
        }
    }

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, pc->log, 0,
                   "connect to %V, fd:%d #%uA", pc->name, s, c->number);

    /*
     * 调用connection方法向上游服务器发起TCP连接；
     * 由于是非阻塞模式，该方法立刻返回，至于是否成功建立连接以下会进行判断；
     */
    rc = connect(s, pc->sockaddr, pc->socklen);

    /* 若建立连接失败，则进行以下相关处理 */
    if (rc == -1) {
        err = ngx_socket_errno;


        if (err != NGX_EINPROGRESS
#if (NGX_WIN32)
            /* Winsock returns WSAEWOULDBLOCK (NGX_EAGAIN) */
            && err != NGX_EAGAIN
#endif
            )
        {
            if (err == NGX_ECONNREFUSED
#if (NGX_LINUX)
                /*
                 * Linux returns EAGAIN instead of ECONNREFUSED
                 * for unix sockets if listen queue is full
                 */
                || err == NGX_EAGAIN
#endif
                || err == NGX_ECONNRESET
                || err == NGX_ENETDOWN
                || err == NGX_ENETUNREACH
                || err == NGX_EHOSTDOWN
                || err == NGX_EHOSTUNREACH)
            {
                level = NGX_LOG_ERR;

            } else {
                level = NGX_LOG_CRIT;
            }

            ngx_log_error(level, c->log, err, "connect() to %V failed",
                          pc->name);

            ngx_close_connection(c);
            pc->connection = NULL;

            return NGX_DECLINED;
        }
    }

    if (ngx_add_conn) {
        if (rc == -1) {

            /* NGX_EINPROGRESS */

            return NGX_AGAIN;
        }

        ngx_log_debug0(NGX_LOG_DEBUG_EVENT, pc->log, 0, "connected");

        wev->ready = 1;

        return NGX_OK;
    }

    if (ngx_event_flags & NGX_USE_AIO_EVENT) {

        ngx_log_debug1(NGX_LOG_DEBUG_EVENT, pc->log, ngx_socket_errno,
                       "connect(): %d", rc);

        /* aio, iocp */

        if (ngx_blocking(s) == -1) {
            ngx_log_error(NGX_LOG_ALERT, pc->log, ngx_socket_errno,
                          ngx_blocking_n " failed");
            goto failed;
        }

        /*
         * FreeBSD's aio allows to post an operation on non-connected socket.
         * NT does not support it.
         *
         * TODO: check in Win32, etc. As workaround we can use NGX_ONESHOT_EVENT
         */

        rev->ready = 1;
        wev->ready = 1;

        return NGX_OK;
    }

    if (ngx_event_flags & NGX_USE_CLEAR_EVENT) {

        /* kqueue */

        event = NGX_CLEAR_EVENT;

    } else {

        /* select, poll, /dev/poll */

        event = NGX_LEVEL_EVENT;
    }

    if (ngx_add_event(rev, NGX_READ_EVENT, event) != NGX_OK) {
        goto failed;
    }

    if (rc == -1) {

        /* NGX_EINPROGRESS */

        if (ngx_add_event(wev, NGX_WRITE_EVENT, event) != NGX_OK) {
            goto failed;
        }

        return NGX_AGAIN;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_EVENT, pc->log, 0, "connected");

    wev->ready = 1;

    return NGX_OK;

failed:

    ngx_close_connection(c);
    pc->connection = NULL;

    return NGX_ERROR;
}


ngx_int_t
ngx_event_get_peer(ngx_peer_connection_t *pc, void *data)
{
    return NGX_OK;
}
