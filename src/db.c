#include "dict.h"
#include "redis.h"
#include "sds.h"

#include <ctype.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int removeExpire(redisDb *db, robj *key) {
  redisAssertWithInfo(NULL, key, dictFind(db->dict, key->ptr) != NULL);

  return dictDelete(db->expires, key->ptr) == DICT_OK;
}

void propagateExpire(redisDb *db, robj *key) {

}

int expireIfNeeded(redisDb *db, robj *key) {
  mstime_t when = getExpire(db, key);
  mstime_t now;

  if (when < 0) {
    return 0;
  }

  // TODO 载入中不进行检测
  now = mstime();

  // TODO 处理附属节点

  if (now <= when) {
    printf("未过期\n");
    return 0; // 未过期
  }

  // 处理过期信息

  return dbDelete(db, key);
}

long long getExpire(redisDb *db, robj *key) {
  dictEntry *de;

  if (dictSize(db->expires) == 0 || (de = dictFind(db->expires, key->ptr)) == NULL) {
    return -1;
  }

  redisAssertWithInfo(NULL, key, dictFind(db->dict, key->ptr) != NULL);

  // 返回过期时间
  return dictGetSignedIntegerVal(de);
}

void setExpire(redisDb *db, robj *key, long long when) {
  dictEntry  *kde, *de;

  kde = dictFind(db->dict, key->ptr);

  redisAssertWithInfo(NULL, key, key != NULL);

  de = dictReplaceRaw(db->expires, dictGetKey(kde));

  printf("expire: %lld", when);
  dictSetSignedIntegerVal(de, when); // 设置键的过期时间
}

robj *lookupKey(redisDb *db, robj *key) {
  // 查找键空间
  dictEntry *de = dictFind(db->dict, key->ptr);
  if (de) {
    robj *val = dictGetVal(de);

    // 更新时间
    if (server.rdb_child_pid == -1 && server.aof_child_pid == -1) {
      val->lru = LRU_CLOCK();
    }
    return val;
  } else {
    return NULL;
  }
}

robj *lookupKeyRead(redisDb *db, robj *key) {
  robj *val;

  // 检查key是否过期
  expireIfNeeded(db, key);
  val = lookupKey(db, key);

  if (val == NULL) {
    server.stat_keyspace_misses++;
    // TODO 更新状态
  } else {
    server.stat_keyspace_hits++;
    printf("找到了\n");
  }

  return val;
}

robj *lookupKeyWrite(redisDb *db, robj *key) {
  // 过期机制
  expireIfNeeded(db, key);

  return lookupKey(db, key);
}

robj *lookupKeyReadOrReply(redisClient *c, robj *key, robj *reply) {
  // 查找
  robj *o = lookupKeyRead(c->db, key);

  // 决定是否发送信息
  if (!o) {
    addReply(c,reply);
  }

  return o;
}

robj *lookupKeyWriteOrReply(redisClient *c, robj *key, robj *reply) {
  // 查找
  robj *o = lookupKeyWrite(c->db, key);

  // 决定是否发送信息
  if (!o) {
    addReply(c,reply);
  }

  return o;
}

void dbAdd(redisDb *db, robj *key, robj *val) {
  sds copy = sdsdup(key->ptr);

  int retval = dictAdd(db->dict, copy, val);
  redisAssertWithInfo(NULL, key, retval == REDIS_OK);
  // TODO 处理集群
}

void dbOverwrite(redisDb *db, robj *key, robj *val) {
  dictEntry *de = dictFind(db->dict, key->ptr);

  // 节点必须存在，否则中止
  redisAssertWithInfo(NULL, key, de != NULL);

  // 覆写旧值
  dictReplace(db->dict, key->ptr, val);
}

void setKey(redisDb *db, robj *key, robj *val) {
  if (lookupKeyWrite(db, key) == NULL) {
    dbAdd(db, key, val);
  } else {
    dbOverwrite(db, key, val);
  }

  incrRefCount(val);
  // 重置过期
  removeExpire(db, key);

  // TODO  推送事件

}

int dbExists(redisDb *db, robj *key);
robj *dbRandomKey(redisDb *db);

int dbDelete(redisDb *db, robj *key) {
  if (dictSize(db->expires) > 0) {
    dictDelete(db->expires, key->ptr);
  }

  // 删除键值对
  if (dictDelete(db->dict, key->ptr) == DICT_OK) {
    // TODO 处理集群
    return 1;
  } else {
    return 0;
  }
}

robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o);
long long emptyDb(void(callback)(void*));
int selectDb(redisClient *c, int id);

void signalModifiedKey(redisDb *db, robj *key) {
  // TODO
}
void signalFlushedDb(int dbid);
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count);
unsigned int countKeysInSlot(unsigned int hashslot);
unsigned int delKeysInSlot(unsigned int hashslot);
int verifyClusterConfigWithData(void);
void scanGenericCommand(redisClient *c, robj *o, unsigned long cursor);

int parseScanCursorOrReply(redisClient *c, robj *o, unsigned long *cursor) {
  char *eptr;

  /* Use strtoul() because we need an *unsigned* long, so
   * getLongLongFromObject() does not cover the whole cursor space. */
  errno = 0;
  *cursor = strtoul(o->ptr, &eptr, 10);
  if (isspace(((char *)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE) {
    addReplyError(c, "invalid cursor");
    return REDIS_ERR;
  }
  return REDIS_OK;
}

/*=========================== 类型无关的数据库操作 =========================*/

void flushdbCommand(redisClient *c) {

}

void flushallCommand(redisClient *c) {

}

void delCommand(redisClient *c) {
  int deleted = 0, j;

  // 遍历所有输入键
  for (j = 1;j < c->argc;j++) {
    // 先删除过期的键
    expireIfNeeded(c->db, c->argv[j]);
    // 尝试删除
    if (dbDelete(c->db, c->argv[j])) {
      signalModifiedKey(c->db, c->argv[j]);
      server.dirty++;
      deleted++;
    }
  }
}

void existsCommand(redisClient *c) {

}

void selectCommand(redisClient *c) {

}

void randomkeyCommand(redisClient *c) {

}


void keysCommand(redisClient *c) {

}

/* This command implements SCAN, HSCAN and SSCAN commands.
 *
 * 这是 SCAN 、 HSCAN 、 SSCAN 命令的实现函数。
 *
 * If object 'o' is passed, then it must be a Hash or Set object, otherwise
 * if 'o' is NULL the command will operate on the dictionary associated with
 * the current database.
 *
 * 如果给定了对象 o ，那么它必须是一个哈希对象或者集合对象，
 * 如果 o 为 NULL 的话，函数将使用当前数据库作为迭代对象。
 *
 * When 'o' is not NULL the function assumes that the first argument in
 * the client arguments vector is a key so it skips it before iterating
 * in order to parse options.
 *
 * 如果参数 o 不为 NULL ，那么说明它是一个键对象，函数将跳过这些键对象，
 * 对给定的命令选项进行分析（parse）。
 *
 * In the case of a Hash object the function returns both the field and value
 * of every element on the Hash.
 *
 * 如果被迭代的是哈希对象，那么函数返回的是键值对。
 */
void scanGenericCommand(redisClient *c, robj *o, unsigned long cursor) {}

/* The SCAN command completely relies on scanGenericCommand. */
void scanCommand(redisClient *c) {
  unsigned long cursor;
  if (parseScanCursorOrReply(c, c->argv[1], &cursor) == REDIS_ERR)
    return;
  scanGenericCommand(c, NULL, cursor);
}

void dbsizeCommand(redisClient *c) {
  addReplyLongLong(c, dictSize(c->db->dict));
}

void lastsaveCommand(redisClient *c) { addReplyLongLong(c, server.lastsave); }

void typeCommand(redisClient *c) {
  robj *o;
  char *type;

  o = lookupKeyRead(c->db, c->argv[1]);

  if (o == NULL) {
    type = "none";
  } else {
    switch (o->type) {
    case REDIS_STRING:
      type = "string";
      break;
    case REDIS_LIST:
      type = "list";
      break;
    case REDIS_SET:
      type = "set";
      break;
    case REDIS_ZSET:
      type = "zset";
      break;
    case REDIS_HASH:
      type = "hash";
      break;
    default:
      type = "unknown";
      break;
    }
  }

  addReplyStatus(c, type);
}

void shutdownCommand(redisClient *c) {
  exit(1);
}

void renameGenericCommand(redisClient *c, int nx) {}

void renameCommand(redisClient *c) { renameGenericCommand(c, 0); }

void renamenxCommand(redisClient *c) { renameGenericCommand(c, 1); }

void moveCommand(redisClient *c) {}
