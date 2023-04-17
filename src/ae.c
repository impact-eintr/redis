#include "ae.h"

#include "zmalloc.h"
#include "config.h"

#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <stdio.h>
#include <assert.h>

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */
#ifdef HAVE_EPOLL
#include "ae_epoll.c"
#endif

aeEventLoop *aeCreateEventLoop(int setsize) {
  aeEventLoop *eventLoop;

  if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL)
    goto err;

  // 初始化文件事件
  eventLoop->events = zmalloc(sizeof(aeFileEvent) * setsize);
  eventLoop->fired = zmalloc(sizeof(aeFiredEvent) * setsize);
  if (eventLoop->events == NULL || eventLoop->fired == NULL) {
    goto err;
  }

  eventLoop->setsize = setsize;
  eventLoop->lastTime = time(NULL); // 最近一次执行时间

  // 初始化时间事件
  eventLoop->timeEventHead = NULL;
  eventLoop->timeEventNextId = 0;

  eventLoop->stop = 0;
  eventLoop->maxfd = -1;
  eventLoop->beforesleep = NULL;
  if (aeApiCreate(eventLoop) == -1) {
    goto err;
  }

  // 初始化监听事件
  for (int i = 0;i < setsize;i++) {
    eventLoop->events[i].mask = AE_NONE;
  }
  return eventLoop;

err:
  if (eventLoop) {
    zfree(eventLoop->events);
    zfree(eventLoop->fired);
    zfree(eventLoop);
  }
  return NULL;
}

// 删除事件处理器
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
  aeApiFree(eventLoop);
  zfree(eventLoop);
  zfree(eventLoop->events);
  zfree(eventLoop->fired);
  zfree(eventLoop);
}

void aeStop(aeEventLoop *eventLoop) {
  eventLoop->stop = 1;
}

// 根据 mask 参数的值 监听 fd 文件的状态
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
                      aeFileProc *proc, void *clientData) {
  if (fd >= eventLoop->setsize) {
    errno = ERANGE;
    return AE_ERR;
  }

  // FIXME 为什么要调用两次
  if (fd >= eventLoop->setsize) {
    return AE_ERR;
  }

  aeFileEvent *fe = &eventLoop->events[fd];

  // 监听指定fd的指定事件
  if (aeApiAddEvent(eventLoop, fd, mask) == -1) {
    return AE_ERR;
  }

  // 设置文件事件类型 以及 事件的处理器
  fe->mask |= mask;
  if (mask & AE_READABLE)
    fe->rfileProc = proc;
  if (mask & AE_WRITABLE)
    fe->wfileProc = proc;
  // 私有数据
  fe->clientData = clientData;

  if (fd > eventLoop->maxfd) {
    eventLoop->maxfd = fd;
  }
  return AE_OK;
}

void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask) {
  if (fd >= eventLoop->setsize) {
    return;
  }

  aeFileEvent *fe = &eventLoop->events[fd];
  if (fe->mask == AE_NONE) {
    return;
  }
  fe->mask = fe->mask & (~mask);
  if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
    int j; // 更新maxfd
    for (j = eventLoop->maxfd-1; j >= 0;j--) {
      if (eventLoop->events[j].mask != AE_NONE) {
        break;
      }
      eventLoop->maxfd = j;
    }
  }
  aeApiDelEvent(eventLoop, fd, mask);
}

// 获取给定的fd正在监听的事件类型
int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
  if (fd >= eventLoop->setsize)
    return 0;

  aeFileEvent *fe = &eventLoop->events[fd];

  return fe->mask;
}

// 取出当前事件的s ms
static void aeGetTime(long *seconds, long *milliseconds) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  *seconds = tv.tv_sec;
  *milliseconds = tv.tv_usec/1000;
}

/*
 * 在当前时间上加上 milliseconds 毫秒，
 * 并且将加上之后的秒数和毫秒数分别保存在 sec 和 ms 指针中。
 */
static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms, when_sec, when_ms;

    // 获取当前时间
    aeGetTime(&cur_sec, &cur_ms);

    // 计算增加 milliseconds 之后的秒数和毫秒数
    when_sec = cur_sec + milliseconds/1000;
    when_ms = cur_ms + milliseconds%1000;

    // 进位：
    // 如果 when_ms 大于等于 1000
    // 那么将 when_sec 增大一秒
    if (when_ms >= 1000) {
        when_sec ++;
        when_ms -= 1000;
    }

    // 保存到指针中
    *sec = when_sec;
    *ms = when_ms;
}


long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
                            aeTimeProc *proc, void *clientData,
                            aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);

static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop) {
  aeTimeEvent *te = eventLoop->timeEventHead;
  aeTimeEvent *nearest = NULL;

  while(te) {

    if (!nearest || te->when_sec < nearest->when_sec ||
        (te->when_sec == nearest->when_sec && te->when_ms < nearest->when_ms)) {
      nearest = te;
    }
    te = te->next;
  }
  return nearest;
}

// 处理所有已经到达的时间事件
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;
    time_t now = time(NULL);

    /* If the system clock is moved to the future, and then set back to the
     * right value, time events may be delayed in a random way. Often this
     * means that scheduled operations will not be performed soon enough.
     *
     * Here we try to detect system clock skews, and force all the time
     * events to be processed ASAP when this happens: the idea is that
     * processing events earlier is less dangerous than delaying them
     * indefinitely, and practice suggests it is. */
    // 通过重置事件的运行时间，
    // 防止因时间穿插（skew）而造成的事件处理混乱
    if (now < eventLoop->lastTime) {
        te = eventLoop->timeEventHead;
        while(te) {
            te->when_sec = 0;
            te = te->next;
        }
    }
    // 更新最后一次处理时间事件的时间
    eventLoop->lastTime = now;

    // 遍历链表
    // 执行那些已经到达的事件
    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId-1;
    while(te) {
      // TODO
      assert(0);
    }
    return processed;
}

int aeProcessEvents(aeEventLoop *eventLoop, int flags) {
  int processed = 0, numevents;

  /* Nothing to do? return ASAP */
  if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS))
    return 0;

  if (eventLoop->maxfd != -1 ||
      ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {

    int j;
    aeTimeEvent *shortest = NULL;
    struct timeval tv, *tvp;

    // 获取最近的时间事件
    if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT)) {
      shortest = aeSearchNearestTimer(eventLoop);
    }

    if (shortest) {
      // 如果时间事件存在的话
      // 那么根据最近可执行时间事件和现在时间的时间差来决定文件事件的阻塞时间
      long now_sec, now_ms;

      /* Calculate the time missing for the nearest
       * timer to fire. */
      // 计算距今最近的时间事件还要多久才能达到
      // 并将该时间距保存在 tv 结构中
      aeGetTime(&now_sec, &now_ms);
      tvp = &tv;
      tvp->tv_sec = shortest->when_sec - now_sec;
      if (shortest->when_ms < now_ms) {
        tvp->tv_usec = ((shortest->when_ms + 1000) - now_ms) * 1000;
        tvp->tv_sec--;
      } else {
        tvp->tv_usec = (shortest->when_ms - now_ms) * 1000;
      }

      // 时间差小于 0 ，说明事件已经可以执行了，将秒和毫秒设为 0 （不阻塞）
      if (tvp->tv_sec < 0)
        tvp->tv_sec = 0;
      if (tvp->tv_usec < 0)
        tvp->tv_usec = 0;
    } else {// 没有时间事件
      if (flags & AE_DONT_WAIT) {
        tv.tv_sec = tv.tv_usec = 0;
        tvp = &tv;
      } else {
        tvp = NULL; // 文件事件可以阻塞直到有事件到达为止
      }
    }

    // 处理文件事件 阻塞时间由tvp决定
    numevents = aeApiPoll(eventLoop, tvp);
    for (j = 0;j < numevents;j++) {
      aeFileEvent *fe = &eventLoop->events[eventLoop->fired[j].fd];

      int mask = eventLoop->fired[j].mask;
      int fd = eventLoop->fired[j].fd;
      int rfired = 0;

      // 读事件
      if (fe->mask & mask & AE_READABLE) {
        // rfired 确保读/写事件只能执行其中一个
        rfired = 1;
        fe->rfileProc(eventLoop, fd, fe->clientData, mask);
      }
      // 写事件
      if (fe->mask & mask & AE_WRITABLE) {
        if (!rfired || fe->wfileProc != fe->rfileProc)
          fe->wfileProc(eventLoop, fd, fe->clientData, mask);
      }

      processed++;
    }

    printf("处理事件\n");
    sleep(1);
  }

  // 执行时间事件
  if (flags & AE_TIME_EVENTS) {
    processed += processTimeEvents(eventLoop);
  }

    return processed;
}

int aeWait(int fd, int mask, long long milliseconds) {
    /* Wait for milliseconds until the given file descriptor becomes
     * writable/readable/exception
     *
     * 在给定毫秒内等待，直到 fd 变成可写、可读或异常
     */
    int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE)
      pfd.events |= POLLIN;
    if (mask & AE_WRITABLE)
      pfd.events |= POLLOUT;

    if ((retval = poll(&pfd, 1, milliseconds)) == 1) {
      if (pfd.revents & POLLIN)
        retmask |= AE_READABLE;
      if (pfd.revents & POLLOUT)
        retmask |= AE_WRITABLE;
      if (pfd.revents & POLLERR)
        retmask |= AE_WRITABLE;
      if (pfd.revents & POLLHUP)
        retmask |= AE_WRITABLE;
      return retmask;
    } else {
      return retval;
    }
    }
}

void aeMain(aeEventLoop *eventLoop) {

  eventLoop->stop = 0;

  while (!eventLoop->stop) {
    if (eventLoop->beforesleep != NULL) {
      eventLoop->beforesleep(eventLoop);
    }

    // 开始处理事件
    aeProcessEvents(eventLoop, AE_ALL_EVENTS);
  }
}

char *aeGetApiName(void) {
  return aeApiName();
}

void aeSetBeforeSleepProc(aeEventLoop *eventLoop,
                          aeBeforeSleepProc *beforesleep) {
  eventLoop->beforesleep = beforesleep;
}

int aeGetSetSize(aeEventLoop *eventLoop) {
  return eventLoop->setsize;
}

int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
  int i;

  if (setsize == eventLoop->setsize)
    return AE_OK;
  if (eventLoop->maxfd >= setsize)
    return AE_ERR;
  if (aeApiResize(eventLoop, setsize) == -1)
    return AE_ERR;

  eventLoop->events =
      zrealloc(eventLoop->events, sizeof(aeFileEvent) * setsize);
  eventLoop->fired = zrealloc(eventLoop->fired, sizeof(aeFiredEvent) * setsize);
  eventLoop->setsize = setsize;

  /* Make sure that if we created new slots, they are initialized with
   * an AE_NONE mask. */
  for (i = eventLoop->maxfd + 1; i < setsize; i++)
    eventLoop->events[i].mask = AE_NONE;
  return AE_OK;
}
