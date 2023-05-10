#include "adlist.h"
#include "redis.h"
#include "ziplist.h"
#include "zmalloc.h"

void listTypeTryConversion(robj *subject, robj *value) {
  if (subject->encoding != REDIS_ENCODING_ZIPLIST) {
    return;
  }
  if (sdsEncodedObject(value) &&
      sdslen(value->ptr) > server.list_max_ziplist_value) {
    printf("转换编码 为双端列表\n");
    listTypeConvert(subject, REDIS_ENCODING_LINKEDLIST);
  }
}

void listTypePush(robj *subject, robj *value, int where) {

  // 尝试转换编码
  listTypeTryConversion(subject, value);


}

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

listTypeIterator *listTypeInitIterator(robj *subject, long index, unsigned char direction) {
  listTypeIterator *li = zmalloc(sizeof(listTypeIterator));
  li->subject = subject;
  li->encoding = subject->encoding;
  li->direction = direction;

  if (li->encoding == REDIS_ENCODING_ZIPLIST) {
    li->zi = ziplistIndex(subject->ptr, index);
  } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
    li->ln = listIndex(subject->ptr, index);
  } else {
    redisPanic("Unknow list encoding");
  }

  return li;
}

void litTyeReleaseIterator(listTypeIterator *li) {
  zfree(li);
}

// 将列表的底层编码从 ziplist 转换成双端链表
void listTypeConvert(robj *subject, int enc) {
  listTypeIterator *li;
  listTypeEntry entry;

  redisAssertWithInfo(NULL, subject, subject->type == REDIS_LIST);

  if (enc == REDIS_ENCODING_LINKEDLIST) {
    list *l = listCreate();
    listSetFreeMethod(l, decrRefCountVoid);

    li = listTypeInitIterator(subject, 0, REDIS_TAIL);
    while (listTypeNext(li, &entry)) {
      listAddNodeTail(l, listTypeGet(&entry));
    }
    listTypeReleaseIterator(li);

    // 更新编码
    subject->encoding = REDIS_ENCODING_LINKEDLIST;

    zfree(subject->ptr);
    subject->ptr = l; // 指向新的列表
  } else {
    redisPanic("Unsupported list convertion");
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
    c->argv[j] = tryObjectEncoding(c->argv[j]);

    if (!lobj) { // 如果不存在该列表
      lobj = createZiplistObject();
      dbAdd(c->db, c->argv[1], lobj);
    }
    listTypePush(lobj, c->argv[j], where);
    pushed++;
    printf("PUSHING......\n");
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
