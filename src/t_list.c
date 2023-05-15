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

  // 处理跳表
  if (subject->encoding == REDIS_ENCODING_SKIPLIST &&
      ziplistLen(subject->ptr) >= server.list_max_ziplist_entries) {
    listTypeConvert(subject, REDIS_ENCODING_LINKEDLIST);
  }

  // ZIPLIST
  if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
    int pos = (where == REDIS_HEAD) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
    value = getDecodedObject(value);
    subject->ptr = ziplistPush(subject->ptr, value->ptr, sdslen(value->ptr), pos);
    decrRefCount(value);
  } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) { // 双端链表
    if (where == REDIS_HEAD) {
      printf("LinkedList Head");
      listAddNodeHead(subject->ptr, value);
    } else {
      printf("LinkedList Tail");
      listAddNodeTail(subject->ptr, value);
    }
    incrRefCount(value);
  } else { // 未知编码
    redisAssert("Unknown list encoding");
  }
}

robj *listTypePop(robj *subject, int where) {
  robj *value = NULL;

  if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
    unsigned char *p;
    unsigned char* vstr;
    unsigned int vlen;
    long long vlong;

    int pos = (where == REDIS_HEAD) ? 0 : -1;
    p = ziplistIndex(subject->ptr, pos);
    if (ziplistGet(p, &vstr, &vlen, &vlong)) {
      if (vstr) { // 弹出的元素是 string
        value = createStringObject((char *)vstr, vlen);
      } else { // 弹出的元素是数字
        value = createStringObjectFromLongLong(vlong);
      }
      subject->ptr = ziplistDelete(subject->ptr, &p);
    }
  } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
    list *list = subject->ptr;
    listNode *ln; // 索引

    if (where == REDIS_HEAD) {
      ln = listFirst(list);
    } else {
      ln = listLast(list);
    }
    // 删除弹出节点
    if (ln != NULL){
      value = listNodeValue(ln);
      incrRefCount(value);
      listDelNode(list, ln);
    }

  } else { // 未知编码
    redisPanic("Unknown list encoding");
  }

  return value;
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

void listTypeReleaseIterator(listTypeIterator *li) {
  zfree(li);
}

int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
  redisAssert(li->subject->encoding == li->encoding);

  entry->li = li;
  // 迭代 ZIPLIST
  if (li->encoding == REDIS_ENCODING_ZIPLIST) {
    entry->zi = li->zi;

    if (entry->zi != NULL) {
      if (li->direction == REDIS_TAIL) {
        li->zi = ziplistNext(li->subject->ptr, li->zi);
      } else {
        li->zi = ziplistPrev(li->subject->ptr, li->zi);
      }
      return 1;
    }
    // 迭代双端链表
  } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
    entry->ln = li->ln;

    if (entry->ln != NULL) {
      if (li->direction == REDIS_TAIL) {
        li->ln = li->ln->next;
      } else {
        li->ln = li->ln->prev;
      }
      return 1;
    }
  } else {
    redisPanic("Unknown los encoding");
  }

  return 0;
}

// 返回 entry 结构当前所保存的列表节点
robj *listTypeGet(listTypeEntry *entry) {
  listTypeIterator *li = entry->li;

  robj *value = NULL;

  if (li->encoding == REDIS_ENCODING_ZIPLIST) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    redisAssert(entry->zi != NULL);
    if (ziplistGet(entry->zi, &vstr, &vlen, &vlong)) {
      if (vstr) {
        value = createStringObject((char *)vstr, vlen);
      } else {
        value = createStringObjectFromLongDouble(vlong);
      }
    }
  } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
    redisAssert(entry->ln != NULL);
    value = listNodeValue(entry->ln);
    incrRefCount(value);
  } else {
    redisPanic("Unknown list encoding");
  }
  return value;
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

  // 返回列表长度
  addReplyLongLong(c, waiting + (lobj ? listTypeLength(lobj) : 0));

  server.dirty += pushed;
}

void lpushCommand(redisClient *c) {
  pushGenericCommand(c, REDIS_HEAD);
}

void rpushCommand(redisClient *c) {
  pushGenericCommand(c, REDIS_TAIL);
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

void popGenericCommand(redisClient *c, int where) {
  robj *o = lookupKeyWriteOrReply(c, c->argv[1], shared.nullbulk);

  if (o == NULL || checkType(c, o, REDIS_LIST)) {
    return;
  }

  robj *value = listTypePop(o, where); // 弹出数据
  if (value == NULL) {
    addReply(c, shared.nullbulk);
  } else {
    // 有数据
    char *event = (where == REDIS_HEAD) ? "lpop" : "rpop";
    addReplyBulk(c, value);
    printf("event %s\n", event);
    decrRefCount(value);
    if (listTypeLength(o) == 0) {
      // TODO 通知事件
      dbDelete(c->db, c->argv[1]);
    }
    server.dirty++;
  }
}

void lpopCommand(redisClient *c) {
  popGenericCommand(c, REDIS_HEAD);
}

void rpopCommand(redisClient *c) {
  popGenericCommand(c, REDIS_TAIL);
}
