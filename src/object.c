#include "redis.h"
#include "util.h"
#include "sds.h"
#include "zmalloc.h"

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

robj *dupStringObject(robj *o);
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

robj *getDecodedObject(robj *o);
size_t stringObjectLen(robj *o);
robj *createStringObjectFromLongLong(long long value);
robj *createStringObjectFromLongDouble(long double value);
robj *createListObject(void);
robj *createZiplistObject(void);
robj *createSetObject(void);
robj *createIntsetObject(void);
robj *createHashObject(void);
robj *createZsetObject(void);
robj *createZsetZiplistObject(void);
