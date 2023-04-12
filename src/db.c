#include "dict.h"
#include "redis.h"
#include "sds.h"

#include <stdio.h>

int removeExpire(redisDb *db, robj *key) {

}

void propagateExpire(redisDb *db, robj *key) {

}

int expireIfNeeded(redisDb *db, robj *key) {
  return 0;
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

  dictSetSignedIntegerVal(de, when); // 设置键的过期时间
}

robj *lookupKey(redisDb *db, robj *key) {
  // 查找键空间
  dictEntry *de = dictFind(db->dict, key->ptr);
  if (de) {
    robj *val = dictGetVal(de);

    // TODO 更新时间
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
    // TODO 更新状态
  } else {

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
      //if (!o) addReply(c,reply);

      return o;

}

robj *lookupKeyWriteOrReply(redisClient *c, robj *key, robj *reply) {
  // TODO 涉及事件机制
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
  // TODO 重置过期 推送事件

}

int dbExists(redisDb *db, robj *key);
robj *dbRandomKey(redisDb *db);
int dbDelete(redisDb *db, robj *key);
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o);
long long emptyDb(void(callback)(void*));
int selectDb(redisClient *c, int id);
void signalModifiedKey(redisDb *db, robj *key);
void signalFlushedDb(int dbid);
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count);
unsigned int countKeysInSlot(unsigned int hashslot);
unsigned int delKeysInSlot(unsigned int hashslot);
int verifyClusterConfigWithData(void);
void scanGenericCommand(redisClient *c, robj *o, unsigned long cursor);
int parseScanCursorOrReply(redisClient *c, robj *o, unsigned long *cursor);

