
/*
 * Copyright (c) 2019, Redis Labs
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __REDIS_CONNECTION_H
#define __REDIS_CONNECTION_H

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>

#include "ae.h"

#define CONN_INFO_LEN   32
#define CONN_ADDR_STR_LEN 128 /* Similar to INET6_ADDRSTRLEN, hoping to handle other protocols. */
#define MAX_ACCEPTS_PER_CALL 1000

struct aeEventLoop;
typedef struct connection connection;
typedef struct connListener connListener;

typedef enum {
    /*没有连接 */
    CONN_STATE_NONE = 0,
    /*正常连接中 */
    CONN_STATE_CONNECTING,
    /*正在建立连接中 */
    CONN_STATE_ACCEPTING,
    /* 连接结束*/
    CONN_STATE_CONNECTED,
    /* 连接关闭*/
    CONN_STATE_CLOSED,
    /* 连接出错*/
    CONN_STATE_ERROR
} ConnectionState;

#define CONN_FLAG_CLOSE_SCHEDULED   (1<<0)      /* 由处理程序计划关闭。Closed scheduled by a handler */
#define CONN_FLAG_WRITE_BARRIER     (1<<1)      /* 请求写入屏障(先写)。Write barrier requested */
/*tcp连接类型 */
#define CONN_TYPE_SOCKET            "tcp"
/* unix连接类型 */
#define CONN_TYPE_UNIX              "unix"
/*tls连接类型 */
#define CONN_TYPE_TLS               "tls"
#define CONN_TYPE_MAX               8           /* 8 is enough to be extendable */

typedef void (*ConnectionCallbackFunc)(struct connection *conn);

/* ConnectionType类型的实例  CT_Socket */
typedef struct ConnectionType {
    /* connection type */
    const char *(*get_type)(struct connection *conn);

    /* connection type initialize & finalize & configure */
    void (*init)(void); /* auto-call during register */
    void (*cleanup)(void);
    int (*configure)(void *priv, int reconfigure);

    /*ae&接受&侦听&错误&地址处理程序.
     * 实际指向了  connSocketEventHandler函数
     * ae & accept & listen & error & address handler */
    void (*ae_handler)(struct aeEventLoop *el, int fd, void *clientData, int mask);//注册的aeFileEvent的rfileProc、wfileProc指针都指向这个ae_handler函数

    aeFileProc *accept_handler;//accept_handler处理器，建立链接.实际指向了 connSocketAcceptHandler处理器
    int (*addr)(connection *conn, char *ip, size_t ip_len, int *port, int remote);
    int (*listen)(connListener *listener);

    /* create/shutdown/close connection */
    connection* (*conn_create)(void);
    connection* (*conn_create_accepted)(int fd, void *priv);
    void (*shutdown)(struct connection *conn);
    void (*close)(struct connection *conn);

    /*封装了处理发起连接的逻辑，主要是作为客户端的时候用，比如说从库主动连接主库. connect & accept */
    int (*connect)(struct connection *conn, const char *addr, int port, const char *source_addr, ConnectionCallbackFunc connect_handler);
    int (*blocking_connect)(struct connection *conn, const char *addr, int port, long long timeout);
    int (*accept)(struct connection *conn, ConnectionCallbackFunc accept_handler);

    /* IO
     * 封装了从连接读取数据，以及向连接中写入数据的函数。
     * 这里的writev函数底层调用了Linux的writev()，
     * 可以一次向Socket写入多块连续不同的buffer空间，
     * 可以减少系统调用的次数
     * */
    int (*write)(struct connection *conn, const void *data, size_t data_len);
    int (*writev)(struct connection *conn, const struct iovec *iov, int iovcnt);
    int (*read)(struct connection *conn, void *buf, size_t buf_len);
    // 用于设置connection中的读写回调函数，也就是write_handler、read_handler函数
    int (*set_write_handler)(struct connection *conn, ConnectionCallbackFunc handler, int barrier);
    int (*set_read_handler)(struct connection *conn, ConnectionCallbackFunc handler);
    const char *(*get_last_error)(struct connection *conn);
    // 下面是阻塞版本的读写函数以及connect函数
    ssize_t (*sync_write)(struct connection *conn, char *ptr, ssize_t size, long long timeout);
    ssize_t (*sync_read)(struct connection *conn, char *ptr, ssize_t size, long long timeout);
    ssize_t (*sync_readline)(struct connection *conn, char *ptr, ssize_t size, long long timeout);

    /* pending data */
    int (*has_pending_data)(void);
    int (*process_pending_data)(void);

    /* TLS specified methods */
    sds (*get_peer_cert)(struct connection *conn);
} ConnectionType;

struct connection {
    // 当前网络连接的类型，其中记录了非常多的回调函数，下面会展开介绍
    ConnectionType *type;
    // 当前连接所处的状态，例如，在acceptTcpHandler()函数中刚刚创建的连接
    // 初始化为CONN_STATE_ACCEPTING状态，后续建连完成之后就切换为
    // CONN_STATE_CONNECTED状态。
    ConnectionState state;
    short int flags;// 标识符
    short int refs;// 当前连接被引用的次数
    int last_errno;// 最后一次发生的错误码
    void *private_data;    // 当前连接关联的信息，该字段指向客户端对应的client实例
    // 下面是该连接在connect、read、write截断的回调函数，其实，aeFileEvent中的
    // rfileProc、wfileProc两个字段指向的就是read_handler、write_handler这两个函数
    ConnectionCallbackFunc conn_handler;
    ConnectionCallbackFunc write_handler;
    ConnectionCallbackFunc read_handler;
    int fd;// 该连接对应的文件描述符
};

#define CONFIG_BINDADDR_MAX 16

/* 监听ip
 * 按连接类型设置侦听器
 * Setup a listener by a connection type */
struct connListener {
    int fd[CONFIG_BINDADDR_MAX];// 记录了每个监听地址对应的文件描述符
    int count; // 监听ip地址的fd的个数
    char **bindaddr;//监听地址
    int bindaddr_count;//绑定地址总数量
    int port;//监听端口
    ConnectionType *ct;
    void *priv; /*由连接类型指定的数据使用。 used by connection type specified data */
};

/* 连接模块不处理侦听和接受套接字，因此我们假设在创建传入连接时有一个套接字。
 * 因此，提供的fd应该与一个已经accept（）ed的套接字相关联。
 * connAccept（）可以直接调用accept_handler（），或者稍后返回并调用它。
 * 这种行为有点尴尬，但目的是减少等待下一个事件循环的需要，如果不需要额外的握手。
 * 重要提示：accept_handler可能会决定关闭连接，调用connClose（）。
 * 为了确保安全，在这种情况下，连接仅标记为CONN_FLAG_CLOSE_SCHEDULED，connAccept（）返回错误。
 * connAccept（）调用方必须始终检查返回值，出现错误（C_ERR）时必须调用connClose（）。
 *
 * The connection module does not deal with listening and accepting sockets,
 * so we assume we have a socket when an incoming connection is created.
 *
 * The fd supplied should therefore be associated with an already accept()ed
 * socket.
 *
 * connAccept() may directly call accept_handler(), or return and call it
 * at a later time. This behavior is a bit awkward but aims to reduce the need
 * to wait for the next event loop, if no additional handshake is required.
 *
 * IMPORTANT: accept_handler may decide to close the connection, calling connClose().
 * To make this safe, the connection is only marked with CONN_FLAG_CLOSE_SCHEDULED
 * in this case, and connAccept() returns with an error.
 *
 * connAccept() callers must always check the return value and on error (C_ERR)
 * a connClose() must be called.
 */

static inline int connAccept(connection *conn, ConnectionCallbackFunc accept_handler) {
    return conn->type->accept(conn, accept_handler);
}

/*
 * 建立连接。连接建立时或发生错误时，将调用connect_handler。
 * 连接处理程序将负责根据需要设置任何读写处理程序。
 * 如果返回C_ERR，则操作失败，不应使用连接处理程序。
 * Establish a connection.  The connect_handler will be called when the connection
 * is established, or if an error has occurred.
 *
 * The connection handler will be responsible to set up any read/write handlers
 * as needed.
 *
 * If C_ERR is returned, the operation failed and the connection handler shall
 * not be expected.
 */
static inline int connConnect(connection *conn, const char *addr, int port, const char *src_addr,
        ConnectionCallbackFunc connect_handler) {
    return conn->type->connect(conn, addr, port, src_addr, connect_handler);
}

/*
 * 阻塞连接。
 * 注意：这是为了简化到抽象连接的转换而实现的，但可能应该在集群之外进行重构。
 * c和复制。c、 支持纯异步实现。
 * Blocking connect.
 *
 * NOTE: This is implemented in order to simplify the transition to the abstract
 * connections, but should probably be refactored out of cluster.c and replication.c,
 * in favor of a pure async implementation.
 */
static inline int connBlockingConnect(connection *conn, const char *addr, int port, long long timeout) {
    return conn->type->blocking_connect(conn, addr, port, timeout);
}

/* 写入到连接
 * Like write(2), a short write is possible. A -1 return indicates an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
static inline int connWrite(connection *conn, const void *data, size_t data_len) {
    return conn->type->write(conn, data, data_len);
}

/*
 * 写入到连接:可以一次向Socket写入多块连续不同的buffer空间，可以减少系统调用的次数.
 * Gather output data from the iovcnt buffers specified by the members of the iov
 * array: iov[0], iov[1], ..., iov[iovcnt-1] and write to connection, behaves the same as writev(3).
 *
 * Like writev(3), a short write is possible. A -1 return indicates an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
static inline int connWritev(connection *conn, const struct iovec *iov, int iovcnt) {
    return conn->type->writev(conn, iov, iovcnt);
}

/*
 * 从连接读取，
 * Read from the connection, behaves the same as read(2).
 * Like read(2), a short read is possible.  A return value of 0 will indicate the
 * connection was closed, and -1 will indicate an error.
 *
 * The caller should NOT rely on errno. Testing for an EAGAIN-like condition, use
 * connGetState() to see if the connection state is still CONN_STATE_CONNECTED.
 */
static inline int connRead(connection *conn, void *buf, size_t buf_len) {
    int ret = conn->type->read(conn, buf, buf_len);
    return ret;
}

/*
 * 注册一个写处理程序，在连接可写时调用。如果为NULL，则删除现有处理程序。
 * Register a write handler, to be called when the connection is writable.
 * If NULL, the existing handler is removed.
 */
static inline int connSetWriteHandler(connection *conn, ConnectionCallbackFunc func) {
    return conn->type->set_write_handler(conn, func, 0);
}

/* 注册一个读处理程序，当连接可读时调用。如果为NULL，则删除现有处理程序。
 * Register a read handler, to be called when the connection is readable.
 * If NULL, the existing handler is removed.
 */
static inline int connSetReadHandler(connection *conn, ConnectionCallbackFunc func) {
    return conn->type->set_read_handler(conn, func);
}

/* 设置写处理程序，并可能启用写屏障，当更改或删除写处理程序时，此标志将被清除。
 * 在启用屏障的情况下，如果读取处理程序已经在同一事件循环迭代中启动，我们就不会启动事件。
 * 当您希望在发送回复之前将内容保存到磁盘上，并且希望以组的方式这样做时，这很有用。
 * Set a write handler, and possibly enable a write barrier, this flag is
 * cleared when write handler is changed or removed.
 * With barrier enabled, we never fire the event if the read handler already
 * fired in the same event loop iteration. Useful when you want to persist
 * things to disk before sending replies, and want to do that in a group fashion. */
static inline int connSetWriteHandlerWithBarrier(connection *conn, ConnectionCallbackFunc func, int barrier) {
    return conn->type->set_write_handler(conn, func, barrier);
}
/*
 * 连接关闭
 */
static inline void connShutdown(connection *conn) {
    conn->type->shutdown(conn);
}

static inline void connClose(connection *conn) {
    conn->type->close(conn);
}

/* 以字符串形式返回连接遇到的最后一个错误。如果没有错误，则返回NULL。
 * Returns the last error encountered by the connection, as a string.  If no error,
 * a NULL is returned.
 */
static inline const char *connGetLastError(connection *conn) {
    return conn->type->get_last_error(conn);
}
/*
 * 同步写
 */
static inline ssize_t connSyncWrite(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_write(conn, ptr, size, timeout);
}
/*
 * 同步读
 */
static inline ssize_t connSyncRead(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_read(conn, ptr, size, timeout);
}
/*
 * 同步按行读
 */
static inline ssize_t connSyncReadLine(connection *conn, char *ptr, ssize_t size, long long timeout) {
    return conn->type->sync_readline(conn, ptr, size, timeout);
}

/* 返回指定连接的CONN_TYPE_*
 * Return CONN_TYPE_* for the specified connection */
static inline const char *connGetType(connection *conn) {
    return conn->type->get_type(conn);
}

static inline int connLastErrorRetryable(connection *conn) {
    return conn->last_errno == EINTR;
}

/* Get address information of a connection.
 * remote works as boolean type to get local/remote address */
static inline int connAddr(connection *conn, char *ip, size_t ip_len, int *port, int remote) {
    if (conn && conn->type->addr) {
        return conn->type->addr(conn, ip, ip_len, port, remote);
    }

    return -1;
}

/* Format an IP,port pair into something easy to parse. If IP is IPv6
 * (matches for ":"), the ip is surrounded by []. IP and port are just
 * separated by colons. This the standard to display addresses within Redis. */
static inline int formatAddr(char *buf, size_t buf_len, char *ip, int port) {
    return snprintf(buf, buf_len, strchr(ip,':') ?
           "[%s]:%d" : "%s:%d", ip, port);
}

static inline int connFormatAddr(connection *conn, char *buf, size_t buf_len, int remote)
{
    char ip[CONN_ADDR_STR_LEN];
    int port;

    if (connAddr(conn, ip, sizeof(ip), &port, remote) < 0) {
        return -1;
    }

    return formatAddr(buf, buf_len, ip, port);
}

static inline int connAddrPeerName(connection *conn, char *ip, size_t ip_len, int *port) {
    return connAddr(conn, ip, ip_len, port, 1);
}

static inline int connAddrSockName(connection *conn, char *ip, size_t ip_len, int *port) {
    return connAddr(conn, ip, ip_len, port, 0);
}

static inline int connGetState(connection *conn) {
    return conn->state;
}

/* Returns true if a write handler is registered */
static inline int connHasWriteHandler(connection *conn) {
    return conn->write_handler != NULL;
}

/* Returns true if a read handler is registered */
static inline int connHasReadHandler(connection *conn) {
    return conn->read_handler != NULL;
}

/* Associate a private data pointer with the connection */
static inline void connSetPrivateData(connection *conn, void *data) {
    conn->private_data = data;
}

/* 获取关联的私有数据指针，即返回Clients
 * Get the associated private data pointer */
static inline void *connGetPrivateData(connection *conn) {
    return conn->private_data;
}

/* Return a text that describes the connection, suitable for inclusion
 * in CLIENT LIST and similar outputs.
 *
 * For sockets, we always return "fd=<fdnum>" to maintain compatibility.
 */
static inline const char *connGetInfo(connection *conn, char *buf, size_t buf_len) {
    snprintf(buf, buf_len-1, "fd=%i", conn == NULL ? -1 : conn->fd);
    return buf;
}

/* 连接到连接器的anet样式包装
 * anet-style wrappers to conns */
int connBlock(connection *conn);
int connNonBlock(connection *conn);
int connEnableTcpNoDelay(connection *conn);
int connDisableTcpNoDelay(connection *conn);
int connKeepAlive(connection *conn, int interval);
int connSendTimeout(connection *conn, long long ms);
int connRecvTimeout(connection *conn, long long ms);

/* 获取安全连接的证书
 * Get cert for the secure connection */
static inline sds connGetPeerCert(connection *conn) {
    if (conn->type->get_peer_cert) {
        return conn->type->get_peer_cert(conn);
    }

    return NULL;
}

/* Initialize the redis connection framework */
int connTypeInitialize();

/* Register a connection type into redis connection framework */
int connTypeRegister(ConnectionType *ct);

/* Lookup a connection type by type name */
ConnectionType *connectionByType(const char *typename);

/* Fast path to get TCP connection type */
ConnectionType *connectionTypeTcp();

/* Fast path to get TLS connection type */
ConnectionType *connectionTypeTls();

/* Fast path to get Unix connection type */
ConnectionType *connectionTypeUnix();

/* Lookup the index of a connection type by type name, return -1 if not found */
int connectionIndexByType(const char *typename);

/* Create a connection of specified type */
static inline connection *connCreate(ConnectionType *ct) {
    return ct->conn_create();
}

/* Create an accepted connection of specified type.
 * priv is connection type specified argument */
static inline connection *connCreateAccepted(ConnectionType *ct, int fd, void *priv) {
    return ct->conn_create_accepted(fd, priv);
}

/* Configure a connection type. A typical case is to configure TLS.
 * priv is connection type specified,
 * reconfigure is boolean type to specify if overwrite the original config */
static inline int connTypeConfigure(ConnectionType *ct, void *priv, int reconfigure) {
    return ct->configure(priv, reconfigure);
}

/* Walk all the connection types and cleanup them all if possible */
void connTypeCleanupAll();

/* Test all the connection type has pending data or not. */
int connTypeHasPendingData(void);

/* walk all the connection types and process pending data for each connection type */
int connTypeProcessPendingData(void);

/* Listen on an initialized listener */
static inline int connListen(connListener *listener) {
    return listener->ct->listen(listener);
}

/* Get accept_handler of a connection type */
static inline aeFileProc *connAcceptHandler(ConnectionType *ct) {
    if (ct)
        return ct->accept_handler;
    return NULL;
}

/* Get Listeners information, note that caller should free the non-empty string */
sds getListensInfoString(sds info);

int RedisRegisterConnectionTypeSocket();
int RedisRegisterConnectionTypeUnix();
int RedisRegisterConnectionTypeTLS();

#endif  /* __REDIS_CONNECTION_H */
