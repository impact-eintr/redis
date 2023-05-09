#include "adlist.h"
#include "redis.h"
#include "ziplist.h"

unsigned long listTypeLength(robj *subject) {

  // ZIPLIST
  if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
    return ziplistLen(subject->ptr);
    // 双端链表
  } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
    return listLength((list*)subject->ptr);
    // 未知编码
  } else {
    redisPanic("Unknow list encoding");
  }
}

/* ======================== List Commands ========================= */

void pushGenericCommand(redisClient *c, int where) {
  int j, waiting = 0, pushed = 0;
  // 取出列表对象
  robj *lobj = lookupKeyWrite(c->db, c->argv[1]);
  // 如果列表对象不存在 处理一下
  int may_have_waiting_clients = (lobj == NULL);
  if (lobj && lobj->type != REDIS_LIST) {
    addReply(c, shared.wrongtypeerr);
    return;
  }
  if (may_have_waiting_clients) {
    // TODO
  }

  for (j = 2;j < c->argc;j++) {

  }

  addReplyLongLong(c, waiting + (lobj ? listTypeLength(lobj) : 0));

  server.dirty += pushed;
}

void lpushCommand(redisClient *c) {
  pushGenericCommand(c, REDIS_HEAD);
}

void rpushCommand(redisClient *c) {
  pushGenericCommand(c, REDIS_HEAD);

}

void pushxGenericCommand(redisClient *c, int where) {

}

void lpushxCommand(redisClient *c) {
  c->argv[2] = tryObjectEncoding(c->argv[2]);
  pushxGenericCommand(c, REDIS_HEAD);
}

void rpushxCommand(redisClient *c) {
  c->argv[2] = tryObjectEncoding(c->argv[2]);
  pushxGenericCommand(c, REDIS_TAIL);
}
