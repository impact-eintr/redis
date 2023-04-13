#include "ae.h"

#include <sys/epoll.h>

typedef struct aeApiState {
  int epfd; // epoll_event file desc
  struct epoll_event *event; //事件槽
} aeApiState;

// 新建一个新的epoll实例
int aeApiCreate(aeEventLoop *eventLoop) {
  aeApiState *state = zmalloc(sizeof(aeApiState));
}
