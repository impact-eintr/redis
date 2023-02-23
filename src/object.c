#include "redis.h"

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
