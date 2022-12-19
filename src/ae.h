/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __AE_H__
#define __AE_H__

#include "monotonic.h"

#define AE_OK 0
#define AE_ERR -1

#define AE_NONE 0       /* No events registered. */
#define AE_READABLE 1   /* Fire when descriptor is readable. */
#define AE_WRITABLE 2   /* Fire when descriptor is writable. */
#define AE_BARRIER 4    /* With WRITABLE, never fire the event if the
                           READABLE event already fired in the same event
                           loop iteration. Useful when you want to persist
                           things to disk before sending replies, and want
                           to do that in a group fashion. */

#define AE_FILE_EVENTS (1<<0)
#define AE_TIME_EVENTS (1<<1)
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
#define AE_DONT_WAIT (1<<2)
#define AE_CALL_BEFORE_SLEEP (1<<3)
#define AE_CALL_AFTER_SLEEP (1<<4)

#define AE_NOMORE -1
#define AE_DELETED_EVENT_ID -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

struct aeEventLoop;

/* Types and data structures */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* File event structure */
typedef struct aeFileEvent {
    // 事件掩码，用来记录发生的事件，
    // 可选标志位为AE_READABLE(1)、AE_WRITABLE(2)、AE_BARRIER(4)
    int mask; /* one of AE_(READABLE|WRITABLE|BARRIER) */
    // 如果发生可读事件，会调用rfileProc指向的函数进行处理
    aeFileProc *rfileProc;
    // 如果发生可写事件，会调用wfileProc指向的函数进行处理
    aeFileProc *wfileProc;
    // 指向对应的客户端对象
    void *clientData;
} aeFileEvent;

/* Time event structure */
typedef struct aeTimeEvent {
    // 唯一标识，通过eventLoop->timeEventNextId字段计算得来
    long long id; /*设置为-1，即为删除，不在触发 time event identifier. */
    // 时间事件触发的时间戳（微秒级别）
    monotime when;
    aeTimeProc *timeProc; // 处理该时间事件的函数
    aeEventFinalizerProc *finalizerProc;// 删除时间事件之前会调用该函数
    void *clientData;// 该时间事件关联的客户端实例
    struct aeTimeEvent *prev;// 前指针
    struct aeTimeEvent *next;// 后指针
    // 当前时间事件被引用的次数，要释放该aeTimeEvent实例，需要refcount为0
    int refcount; /* refcount to prevent timer events from being
  		   * freed in recursive time event calls. */
} aeTimeEvent;

/* 一个激发的事件
 * A fired event */
typedef struct aeFiredEvent {
    /**
     * 文件描述符
     */
    int fd;
    /**
     *
     */
    int mask;
} aeFiredEvent;

/*一个aeEventLoop代表一个事件循环？一个Reactor模型？
 * 保存有该循环需要处理，监听的所有的事件信息：事件数量，所有的事件，激活的事件，文件事件，时间事件等
 * 基于事件的程序的状态，是 Redis 事件驱动的核心结构体
 * State of an event based program */
typedef struct aeEventLoop {
    int maxfd;   /* 当前注册的文件描述符的最大值。highest file descriptor currently registered */
    int setsize; /* 能够注册的文件描述符个数上限。max number of file descriptors tracked */
    long long timeEventNextId;//用于计算时间事件的唯一标识
    //events指向了一个网络事件数组，记录了已经注册的网络事件，数组长度为setsize
    aeFileEvent *events; /* 已注册事件集合。 Registered events */
    //fired数组记录了被触发的网络事件
    aeFiredEvent *fired; /* 激发的事件集合.Fired events */
    // timeEventHead指向了时间事件链表的头节点
    aeTimeEvent *timeEventHead;/*时间事件集合 */
    // 停止的标识符，设置为1表示aeEventLoop事件循环已停止
    int stop;
    // Redis在不同平台会使用4种不同的I/O多路复用模型（evport、epoll、kueue、select），
    // apidata字段是对这四种模型的进一步封装，指向aeApiState一个实例
    void *apidata; /* 用于轮询API特定数据.This is used for polling API specific data */
    // Redis主线程阻塞等待网络事件时，会在阻塞之前(拉取新的事件前)调用beforesleep函数，
    aeBeforeSleepProc *beforesleep;
    // 在被唤醒之后调用aftersleep函数
    aeBeforeSleepProc *aftersleep;
    int flags;
} aeEventLoop;

/* Prototypes */
aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
/* 通过ip的文件描述符，给监听的ip注册事件处理器*/
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
void *aeGetFileClientData(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);
void aeSetDontWait(aeEventLoop *eventLoop, int noWait);

#endif
