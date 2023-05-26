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

void hashTypeTryObjectEncoding(robj *subject, robj **o1, robj **o2) {
  if (subject->encoding == REDIS_ENCODING_HT) {
    if (o1) {
      *o1 = tryObjectEncoding(*o1);
    }
    if (o2) {
      *o2 = tryObjectEncoding(*o2);
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
  // 确保输入的对象是一个ziplist
  redisAssert(o->encoding == REDIS_ENCODING_ZIPLIST);

  if (enc == REDIS_ENCODING_ZIPLIST) {
    // Nothing to do
  } else if (enc == REDIS_ENCODING_HT) { // 把ziplist转换成ht
    hashTypeIterator *hi;
    dict *dict;
    int ret;
    // TODO 转换类型
    //hi = hashTypeInitIterator(o);

    //dict = dictCreate(&hashDictType, NULL);

    o->encoding = REDIS_ENCODING_HT;
    printf("转换成功\n");
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

int hashTypeGetFromZiplist(robj *o, robj *field, unsigned char **vstr,
                           unsigned int *vlen, long long *vll) {
  unsigned char *zl, *fptr = NULL, *vptr = NULL;
  int ret;

  redisAssert(o->encoding == REDIS_ENCODING_ZIPLIST);
}

int hashTypeGetFromHashTable(robj *o, robj *field, robj **value) {

}

robj *hashTypeGetObject() {

}

int hashTypeExists(robj *o, robj *field) {

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

  // 尝试编码 field 和 value 以节约空间
  hashTypeTryObjectEncoding(o, &c->argv[2], &c->argv[3]);
  update = hashTypeSet(o, c->argv[2], c->argv[3]);

  addReply(c, update ? shared.czero: shared.cone);

  // TODO 发送事件通知

  server.dirty++;
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
