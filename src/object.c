#include "redis.h"
#include "util.h"
#include "sds.h"
#include "zmalloc.h"

#include <limits.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define REDIS_COMPARE_BINARY (1<<0)
#define REDIS_COMPARE_COLL (1<<1)

int compareStringObjectsWithFlags(robj *a, robj *b, int flags) {
  return 0;
}

int compareStringObjects(robj *a, robj *b) {
  return compareStringObjectsWithFlags(a, b, REDIS_COMPARE_BINARY);
}

int equalStringObjects(robj *a, robj *b) {
  if (a->encoding == REDIS_ENCODING_INT && b->encoding == REDIS_ENCODING_INT) {
    return a->ptr == b->ptr;
  } else {
    return compareStringObjects(a, b);
  }
}

void incrRefCount(robj *o) {
  o->refcount++;
}

void decrRefCount(robj *o) {
  o->refcount--;
}


void decrRefCountVoid(void *o) {
  decrRefCount(o);
}

robj *resetRefCount(robj *obj);
void freeStringObject(robj *o);
void freeListObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeHashObject(robj *o);

robj *createObject(int type, void *ptr) {
  robj *o = zmalloc(sizeof(*o));

  o->type = type;
  o->encoding = REDIS_ENCODING_RAW;
  o->ptr = ptr;
  o->refcount = 1;
  o->lru =LRU_CLOCK();

  return o;
}

#define REDIS_ENCODING_EMBSTR_SIZE_LIMIT 39
robj *createStringObject(char *ptr, size_t len) {
  if (len <= REDIS_ENCODING_EMBSTR_SIZE_LIMIT)
    return createEmbeddedStringObject(ptr, len);
  else
    return createRawStringObject(ptr, len);
}

// 创建一个 REDIS_ENCODING_RAW 编码的字符对象
robj *createRawStringObject(char *ptr, size_t len) {
  return createObject(REDIS_STRING, sdsnewlen(ptr, len));
}

// 创建一个 REDIS_ENCODING_EMBSTR 编码的字符对象
robj *createEmbeddedStringObject(char *ptr, size_t len) {
  robj *o = zmalloc(sizeof(robj)+sizeof(struct sdshdr)+len+1);
  struct sdshdr *sh = (void *)(o+1);

  o->type = REDIS_STRING;
  o->encoding = REDIS_ENCODING_EMBSTR;
  o->ptr = sh+1;
  o->refcount = 1;
  o->lru = LRU_CLOCK();

  sh->len =len;
  sh->free = 0;
  if (ptr) {
    memcpy(sh->buf, ptr, len);
    sh->buf[len] = '\0';
  } else {
    memset(sh->buf, 0, len+1);
  }
  return o;
}

// 返回一个非共享的字符串
robj *dupStringObject(robj *o) {
  robj *d;
  redisAssert(o->type == REDIS_STRING);

  switch (o->encoding) {
    case REDIS_ENCODING_RAW:
      return createRawStringObject(o->ptr, sdslen(o->ptr));
    case REDIS_ENCODING_EMBSTR:
      return createEmbeddedStringObject(o->ptr, sdslen(o->ptr));
    case REDIS_ENCODING_INT:
      d = createObject(REDIS_STRING, NULL);
      d->encoding = REDIS_ENCODING_INT;
      d->ptr = o->ptr;
      return d;
    default:
      redisPanic("Wrong encoding,");
      break;
  }
}

int isObjectRepresentableAsLongLong(robj *o, long long *llongval);

robj *tryObjectEncoding(robj *o) {
  long value;
  sds s = o->ptr;
  size_t len;

  redisAssertWithInfo(NULL, o, o->type == REDIS_STRING);

  // 不对共享对象进行编码
  if (!sdsEncodedObject(o))
    return o;

  if (o->refcount > 1)
    return 0;

  len = sdslen(s);
  if (len <= 21 && string2l(s,len,&value)) {
    // TODO 处理 long 类型
  }

  if (len <= REDIS_ENCODING_EMBSTR_SIZE_LIMIT) {
    robj *emb;

    if (o->encoding == REDIS_ENCODING_EMBSTR)
      return o;
    emb = createEmbeddedStringObject(s, sdslen(s));
    decrRefCount(o); // 脱离原对象
    return emb;
  }

  if (o->encoding == REDIS_ENCODING_RAW && sdsavail(s) > len/10) {
    o->ptr = sdsRemoveFreeSpace(o->ptr);
  }
  return o;
}

robj *getDecodedObject(robj *o) {
  robj *dec;
  if (sdsEncodedObject(o)) {
    incrRefCount(o);
    return o;
  }
  // 解码对象
  if (o->type == REDIS_STRING && o->encoding == REDIS_ENCODING_INT) {
    char buf[32];

    ll2string(buf, 32, (long)o->ptr);
    dec = createStringObject(buf, strlen(buf));
    return dec;
  } else {
    redisPanic("Unknown encoding type.");
  }
}


// 返回字符串对象中值的长度
size_t stringObjectLen(robj *o) {
  redisAssertWithInfo(NULL, o, o->type == REDIS_STRING);

  if (sdsEncodedObject(o)) {
    return sdslen(o->ptr);
  } else {
    char buf[32];
    return ll2string(buf, 32, (long)o->ptr);
  }
}

robj *createStringObjectFromLongLong(long long value) {
  robj *o;

  if (value >= 0 && value < REDIS_SHARED_INTEGERS) {
    // 使用共享对象
    incrRefCount(shared.integers[value]);
    o = shared.integers[value];
  } else {
    if (value >= LONG_MIN && value <= LONG_MAX) {
      o = createObject(REDIS_STRING, NULL);
      o->encoding = REDIS_ENCODING_INT;
      o->ptr = (void*)((long)value);
    } else { // long long 使用 REDIS_ENCODING_RAW 来保存
      o = createObject(REDIS_STRING, sdsfromlonglong(value));
    }
  }

  return o;
}

robj *createStringObjectFromLongDouble(long double value) {

}

robj *createListObject(void) {

}

robj *createZiplistObject(void);
robj *createSetObject(void);
robj *createIntsetObject(void);
robj *createHashObject(void);
robj *createZsetObject(void);
robj *createZsetZiplistObject(void);


int getLongFromObjectOrReply(redisClient *c, robj *o, long *target,
                             const char *msg);
int checkType(redisClient *c, robj *o, int type) {
  if (o->type != type) {
    addReply(c, shared.wrongtypeerr);
    return 1;
  }
  return 0;
}

int getLongLongFromObjectOrReply(redisClient *c, robj *o, long long *target,
                                 const char *msg) {
  long long value;
  char *eptr;

  if (o == NULL) {
    value = 0;
  } else {
    redisAssertWithInfo(NULL, o, o->type == REDIS_STRING);
    if (sdsEncodedObject(o)) {
      errno = 0;
      // T = O(N)
      value = strtoll(o->ptr, &eptr, 10);
      if (isspace(((char *)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE)
        return REDIS_ERR;

    } else if (o->encoding == REDIS_ENCODING_INT) {
      value = (long)o->ptr;
    } else {
      redisPanic("Unknown string encoding");
    }
  }
  if (target)
    *target = value;

  return REDIS_OK;
}

int getDoubleFromObjectOrReply(redisClient *c, robj *o, double *target,
                               const char *msg);
int getLongLongFromObject(robj *o, long long *target);
int getLongDoubleFromObject(robj *o, long double *target);
int getLongDoubleFromObjectOrReply(redisClient *c, robj *o, long double *target,
                                   const char *msg);
char *strEncoding(int encoding);
int compareStringObjects(robj *a, robj *b);
int collateStringObjects(robj *a, robj *b);
int equalStringObjects(robj *a, robj *b);
unsigned long long estimateObjectIdleTime(robj *o);
