#include "ae.h"
#include "zmalloc.h"

#include <stdio.h>
#include <sys/epoll.h>

typedef struct aeApiState
{
  int epfd;                   // epoll_event file desc
  struct epoll_event *events; // 事件槽
} aeApiState;

// 新建一个新的epoll实例
int aeApiCreate(aeEventLoop *eventLoop)
{

  aeApiState *state = zmalloc(sizeof(aeApiState));

  if (!state)
    return -1;

  // 初始化事件槽空间
  struct epoll_event *events = zmalloc(sizeof(struct epoll_event) * eventLoop->setsize);
  state->events = events;
  if (!state->events)
  {
    zfree(state);
    return -1;
  }

  // 创建 epoll 实例
  state->epfd = epoll_create(1024); /* 1024 is just a hint for the kernel */
  if (state->epfd == -1)
  {
    zfree(state->events);
    zfree(state);
    return -1;
  }

  // 赋值给 eventLoop
  eventLoop->apidata = state;
  return 0;
}

/*
 * 调整事件槽大小
 */
static int aeApiResize(aeEventLoop *eventLoop, int setsize)
{
  aeApiState *state = eventLoop->apidata;

  state->events = zrealloc(state->events, sizeof(struct epoll_event) * setsize);
  return 0;
}

/*
 * 释放 epoll 实例和事件槽
 */
static void aeApiFree(aeEventLoop *eventLoop)
{
  aeApiState *state = eventLoop->apidata;

  close(state->epfd);
  zfree(state->events);
  zfree(state);
}

/*
 * 关联给定事件到 fd
 */
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask)
{
  aeApiState *state = eventLoop->apidata;
  struct epoll_event ee;

  /* If the fd was already monitored for some event, we need a MOD
   * operation. Otherwise we need an ADD operation.
   *
   * 如果 fd 没有关联任何事件，那么这是一个 ADD 操作。
   *
   * 如果已经关联了某个/某些事件，那么这是一个 MOD 操作。
   */
  int op = eventLoop->events[fd].mask == AE_NONE ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

  // 注册事件到 epoll
  ee.events = 0;
  mask |= eventLoop->events[fd].mask; /* Merge old events */
  if (mask & AE_READABLE)
    ee.events |= EPOLLIN;
  if (mask & AE_WRITABLE)
    ee.events |= EPOLLOUT;
  ee.data.u64 = 0; /* avoid valgrind warning */
  ee.data.fd = fd;

  if (epoll_ctl(state->epfd, op, fd, &ee) == -1)
    return -1;

  return 0;
}

/*
 * 返回当前正在使用的 poll 库的名字
 */
static char *aeApiName(void)
{
  return "epoll";
}