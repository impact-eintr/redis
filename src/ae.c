#include "ae.h"

#include "zmalloc.h"

#include <stdio.h>

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

void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
                      aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
                            aeTimeProc *proc, void *clientData,
                            aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);


// 处理所有已经到达的时间事件
static int processTimeEvents(aeEventLoop *eventLoop) {

}

int aeProcessEvents(aeEventLoop *eventLoop, int flags) {
  int processed = 0, numevents;

  /* Nothing to do? return ASAP */
  if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS))
    return 0;

    if (eventLoop->maxfd != -1 || ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
      printf("处理事件\n");
      sleep(1);
    }

    // 执行时间事件
    if (flags & AE_TIME_EVENTS) {
      processed += processTimeEvents(eventLoop);
    }

    return processed;
}

int aeWait(int fd, int mask, long long milliseconds);
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

char *aeGetApiName(void);

void aeSetBeforeSleepProc(aeEventLoop *eventLoop,
                          aeBeforeSleepProc *beforesleep)
{
  eventLoop->beforesleep =beforesleep;
}
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);