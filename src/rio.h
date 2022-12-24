/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2019, Salvatore Sanfilippo <antirez at gmail dot com>
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


#ifndef __REDIS_RIO_H
#define __REDIS_RIO_H

#include <stdio.h>
#include <stdint.h>
#include "sds.h"
#include "connection.h"

#define RIO_FLAG_READ_ERROR (1<<0)
#define RIO_FLAG_WRITE_ERROR (1<<1)

#define RIO_TYPE_FILE (1<<0)
#define RIO_TYPE_BUFFER (1<<1)
#define RIO_TYPE_CONN (1<<2)
#define RIO_TYPE_FD (1<<3)

struct _rio {
    /* 后端功能。由于此函数不允许短写或短读，因此返回值被简化为：出错时为零，完全成功时为非零。
     * Backend functions.
     * Since this functions do not tolerate short writes or reads the return
     * value is simplified to: zero on error, non zero on complete success. */
    size_t (*read)(struct _rio *, void *buf, size_t len);// 指向数据写入函数的指针
    size_t (*write)(struct _rio *, const void *buf, size_t len);// 指向数据写入函数的指针
    off_t (*tell)(struct _rio *);// 指向获取当前的读写偏移量函数的指针
    int (*flush)(struct _rio *);// 指向flush函数的指针
    /* The update_cksum method if not NULL is used to compute the checksum of
     * all the data that was read or written so far. The method should be
     * designed so that can be called with the current checksum, and the buf
     * and len fields pointing to the new block of data to add to the checksum
     * computation. */
    void (*update_cksum)(struct _rio *, const void *buf, size_t len);// 指向用于计算校验和函数的指针

    /* 校验和、flags标志位
     * The current checksum and flags (see RIO_FLAG_*) */
    uint64_t cksum, flags;

    /* 已经读写的字节数
     * number of bytes read or written */
    size_t processed_bytes;

    /* 单次读写的上限值
     * maximum single read or write chunk size */
    size_t max_processing_chunk;

    /* 底层读写的真正结构，可以是buffer、文件、网络连接
     * Backend-specific vars. */
    union {
        /* 内存缓冲区目标。
         * In-memory buffer target. */
        struct {
            sds ptr;// 指向buffer的指针
            off_t pos;// 当前读写的位置
        } buffer;
        /* Stdio文件指针目标.Stdio file pointer target. */
        struct {
            FILE *fp;// 读写的文件
            off_t buffered; /* 自上次fsync以来写入的字节数.Bytes written since last fsync. */
            off_t autosync; /* 是否异步刷新.fsync after 'autosync' bytes written. */
        } file;
        /*连接对象（用于从套接字读取）. Connection object (used to read from socket) */
        struct {
            connection *conn;   /* 指向网络连接.Connection */
            off_t pos;    /* 缓冲区中的数据.pos in buf that was returned */
            sds buf;      /* 读写缓冲区.buffered data */
            size_t read_limit;  /* 从该连接中读取数据的上限值（字节）. don't allow to buffer/read more than that */
            size_t read_so_far; /* 从rio读取的数据量（未缓冲）.amount of data read from the rio (not buffered) */
        } conn;
        /*FD目标（用于写入管道） FD target (used to write to pipe). */
        struct {
            int fd;       /* 读写的文件描述符. File descriptor. */
            off_t pos;/*缓冲区中的读写位置*/
            sds buf;/*缓冲区*/
        } fd;
    } io;
};

typedef struct _rio rio;

/* 以下函数是我们与流的接口。他们将调用读写告诉的实际实现，并在需要时更新校验和。
 * The following functions are our interface with the stream. They'll call the
 * actual implementation of read / write / tell, and will update the checksum
 * if needed. */

static inline size_t rioWrite(rio *r, const void *buf, size_t len) {
    if (r->flags & RIO_FLAG_WRITE_ERROR) return 0;// 检查flags的异常标志位
    while (len) {
        // 确认需要写入的字节数
        size_t bytes_to_write = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        if (r->update_cksum) r->update_cksum(r,buf,bytes_to_write);// 计算校验和
        // 调用rio->write()函数，向底层数据源写入数据
        if (r->write(r,buf,bytes_to_write) == 0) {
            // 如果写入过程发生异常，会在flags标志位设置相应标记
            r->flags |= RIO_FLAG_WRITE_ERROR;
            return 0;
        }
        buf = (char*)buf + bytes_to_write;// 后移buf指针
        len -= bytes_to_write;// 更新此次写入的字节数
        r->processed_bytes += bytes_to_write;// 更新写入的总字节数
    }
    return 1;
}

static inline size_t rioRead(rio *r, void *buf, size_t len) {
    if (r->flags & RIO_FLAG_READ_ERROR) return 0;// 检查flags的异常标志位
    while (len) {
        // 确认读取的目标字节数
        size_t bytes_to_read = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        // 调用rio->read()函数，从底层数据源读取数据
        if (r->read(r,buf,bytes_to_read) == 0) {
            // 如果读取过程发生异常，会在flags标志位设置相应标记
            r->flags |= RIO_FLAG_READ_ERROR;
            return 0;
        }
        // 对计算校验和
        if (r->update_cksum) r->update_cksum(r,buf,bytes_to_read);
        buf = (char*)buf + bytes_to_read;// 后移buf指针
        len -= bytes_to_read;// 更新此次读取的目标字节数
        r->processed_bytes += bytes_to_read;// 更新读取的总字节数
    }
    return 1;
}

static inline off_t rioTell(rio *r) {
    return r->tell(r);
}

static inline int rioFlush(rio *r) {
    return r->flush(r);
}

/* This function allows to know if there was a read error in any past
 * operation, since the rio stream was created or since the last call
 * to rioClearError(). */
static inline int rioGetReadError(rio *r) {
    return (r->flags & RIO_FLAG_READ_ERROR) != 0;
}

/* Like rioGetReadError() but for write errors. */
static inline int rioGetWriteError(rio *r) {
    return (r->flags & RIO_FLAG_WRITE_ERROR) != 0;
}

static inline void rioClearErrors(rio *r) {
    r->flags &= ~(RIO_FLAG_READ_ERROR|RIO_FLAG_WRITE_ERROR);
}

void rioInitWithFile(rio *r, FILE *fp);
void rioInitWithBuffer(rio *r, sds s);
void rioInitWithConn(rio *r, connection *conn, size_t read_limit);
void rioInitWithFd(rio *r, int fd);

void rioFreeFd(rio *r);
void rioFreeConn(rio *r, sds* out_remainingBufferedData);

size_t rioWriteBulkCount(rio *r, char prefix, long count);
size_t rioWriteBulkString(rio *r, const char *buf, size_t len);
size_t rioWriteBulkLongLong(rio *r, long long l);
size_t rioWriteBulkDouble(rio *r, double d);

struct redisObject;
int rioWriteBulkObject(rio *r, struct redisObject *obj);

void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len);
void rioSetAutoSync(rio *r, off_t bytes);
uint8_t rioCheckType(rio *r);
#endif
