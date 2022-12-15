/* Linux epoll(2) based ae.c module
 *
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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


#include <sys/epoll.h>
//epoll使用详解：epoll_create、epoll_ctl、epoll_wait、close
//https://www.cnblogs.com/xuewangkai/p/11158576.html
typedef struct aeApiState {
    int epfd;// epoll监听的内核注册表(持有所有受监听的事件)的文件描述符
    struct epoll_event *events; // 指向epoll_event(单个事件集合)缓冲区，已经发生的事件，放到该区域。
} aeApiState;
/**
 * 进行初始化，封装了epoll_Create方法
 * @param eventLoop
 * @return
 */
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    state->events = zmalloc(sizeof(struct epoll_event)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    //https://www.cnblogs.com/xuewangkai/p/11158576.html
    //返回一个监听事件的文件描述符
    state->epfd = epoll_create(1024); /* 1024只是内核的提示.1024 is just a hint for the kernel */
    if (state->epfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    anetCloexec(state->epfd);
    eventLoop->apidata = state;
    return 0;
}
/**
 * 调整内存
 * @param eventLoop
 * @param setsize
 * @return
 */
static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;
    //主要是开辟了一块儿空间，来保存一定的数量的epoll_event事件。
    state->events = zrealloc(state->events, sizeof(struct epoll_event)*setsize);
    return 0;
}
/**
 * 释放内存
 * @param eventLoop
 */
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->epfd);
    zfree(state->events);
    zfree(state);
}
/**
 * 新增事件，封装了epoll_ctl方法
 * @param eventLoop -eventLoop是一个大管家
 * @param fd -要操作的事件的文件描述符
 * @param mask  -该事件的操作类型
 * @return
 */
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    //创建一个epoll_event事件
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    /* 如果fd已经被监控了一些事件，我们需要MOD操作。否则我们需要ADD操作。
     * If the fd was already monitored for some event, we need a MOD
     * operation. Otherwise we need an ADD operation.  */
    //events:已经持有的aeFileEvent数组。
    //原来就有该事件，就是修改，否则就是新增。
    int op = eventLoop->events[fd].mask == AE_NONE ?
            EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    ee.events = 0;
    mask |= eventLoop->events[fd].mask; /* Merge old events */
    //如果是读事件
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    //如果是写事件
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.fd = fd;//该事件对应的文件描述符
    /*
     * 第一个参数是epoll_create()的返回值，
     * 第二个参数表示动作，用三个宏来表示：EPOLL_CTL_ADD：注册新的fd到epfd中；EPOLL_CTL_MOD：修改已经注册的fd的监听事件；EPOLL_CTL_DEL：从epfd中删除一个fd；
     * 第三个参数是需要监听的事件的文件描述符fd，
     * 第四个参数是告诉内核需要监听什么事件，
     */
    if (epoll_ctl(state->epfd,op,fd,&ee) == -1) return -1;
    //epoll_ctl:该方法中应该就是新增连接的文件描述符或者更改现有连接的监听类型或删除对应的连接
    return 0;
}
/**
 * 删除事件
 * @param eventLoop
 * @param fd
 * @param delmask
 */
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = 0;
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.fd = fd;
    if (mask != AE_NONE) {
        epoll_ctl(state->epfd,EPOLL_CTL_MOD,fd,&ee);
    } else {
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for
         * EPOLL_CTL_DEL. */
        epoll_ctl(state->epfd,EPOLL_CTL_DEL,fd,&ee);
    }
}
/**
 * 拉取事件，封装了epoll_wait方法
 * @param eventLoop
 * @param tvp
 * @return
 */
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    /*
     * 参数：
     * 1：epoll监听的内核注册表(持有所有受监听的事件)的文件描述符
     * 2：指向epoll_event(单个事件集合)缓冲区，已经发生的事件，放到该区域。
     * 3：总的处理的大小，可能并不起作用。
     * 4：超时时间。
     */
    //对所有持有的连接和(所有的事件类型)类型进行监听，有对应事件后或者超时后，返回数量。
    retval = epoll_wait(state->epfd,state->events,eventLoop->setsize,
            tvp ? (tvp->tv_sec*1000 + (tvp->tv_usec + 999)/1000) : -1);
    if (retval > 0) {
        int j;

        numevents = retval;
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event *e = state->events+j;//依次获取对应的事件

            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            if (e->events & EPOLLERR) mask |= AE_WRITABLE|AE_READABLE;
            if (e->events & EPOLLHUP) mask |= AE_WRITABLE|AE_READABLE;
            //保存已经激活的事件的文件描述符和事件类型
            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
        }
    } else if (retval == -1 && errno != EINTR) {
        panic("aeApiPoll: epoll_wait, %s", strerror(errno));
    }

    return numevents;
}

static char *aeApiName(void) {
    return "epoll";
}
