#include "redis.h"

/*===================== Hash Type API ======================*/

void hashTypeTryConversion(robj *o, robj **argv, int start, int end) {
  int i;

  if (o->encoding != REDIS_ENCODING_ZIPLIST) {
    return;
  }

  for (i = start; i <= end; i++) {
    if (sdsEncodedObject(argv[i]) && sdslen(argv[i]->ptr) > server.hash_max_ziplist_value) {
      hashTypeConvert(o, REDIS_ENCODING_HT); // 将对象的编码转换成 REDIS_ENCODING_HT
      break;
    }
  }
}

robj *hashTypeLookupWriteOrCreate(redisClient *c, robj *key) {
  robj *o = lookupKeyWrite(c->db, key);

  // 创建
  if (o == NULL) {
    o = createHashObject();
    dbAdd(c->db, key, o);
  } else { // 检查
    if (o->type != REDIS_HASH) {
      addReply(c, shared.wrongtypeerr);
      return NULL;
    }
  }

  // 返回对象
  return o;
}

// 将一个ziplist编码的hash对象 o 转换成其他编码
void hashTypeConvertZiplist(robj *o, int enc) {
  redisAssert(o->encoding == REDIS_ENCODING_ZIPLIST);

  if (enc == REDIS_ENCODING_ZIPLIST) {
    // Nothing to do
  } else if (enc == REDIS_ENCODING_HT) {
    hashTypeIterator *hi;
  } else {
    redisPanic("Unknown hahs encoding");
  }
}

// 尝试将 ZIPLIST 转换为 HT
void hashTypeConvert(robj *o, int enc) {
  if (o->encoding == REDIS_ENCODING_ZIPLIST) {
    hashTypeConvertZiplist(o, enc);
  } else if (o->encoding == REDIS_ENCODING_HT) {
    redisPanic("Not implemented");
  } else {
    redisPanic("Unknown hash encoding");
  }
}


/*
** ================== Hash Type Commands =====================
*/

void hsetCommand(redisClient *c) {
  int update;
  robj *o;

  if ((o = hashTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) {
    return;
  }

  hashTypeTryConversion(o, c->argv, 2, 3);

}


void hsetnxCommand(redisClient *c) {

}

void hgetCommand(redisClient *c) {

}

void hmsetCommand(redisClient *c) {

}

void hmgetCommand(redisClient *c) {

}

void hdelCommand(redisClient *c) {

}

void hlenCommand(redisClient *c) {

}

void hkeysCommand(redisClient *c) {

}

void hvalsCommand(redisClient *c) {

}
