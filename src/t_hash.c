#include "dict.h"
#include "zmalloc.h"
#include "redis.h"
#include "ziplist.h"
#include <limits.h>

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


unsigned long hashTypeLength(robj *o) {
  unsigned long length = ULONG_MAX;

  if (o->encoding == REDIS_ENCODING_ZIPLIST) {
    length = ziplistLen(o->ptr) / 2; // key + value
  } else if (o->encoding == REDIS_ENCODING_HT) {
    length = dictSize((dict *)o->ptr);
  } else {
    redisPanic("Unknown hash encoding");
  }
  return length;
}

hashTypeIterator *hashTypeInitIterator(robj *dubject) {
  hashTypeIterator *hi = zmalloc(sizeof(hashTypeIterator));
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
    hi = hashTypeInitIterator(o);

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

  field = getDecodedObject(field);

  zl = o->ptr;
  fptr = ziplistIndex(zl, ZIPLIST_HEAD);
  if (fptr != NULL) {
    fptr = ziplistFind(fptr, field->ptr, sdslen(field->ptr), 1);
    if (fptr != NULL) {
      vptr = ziplistNext(zl, fptr);
      redisAssert(vptr != NULL);
    }
  }

  decrRefCount(field);

  if (vptr != NULL) {
    ret = ziplistGet(vptr, vstr, vlen, vll);
    redisAssert(ret);
    return 0;
  }

  return -1;
}

int hashTypeGetFromHashTable(robj *o, robj *field, robj **value) {

}

robj *hashTypeGetObject(robj *o, robj *key) {

}

// 检查 field 是否存在于 hash o 中 存在 1 不存在 0
int hashTypeExists(robj *o, robj *field) {
  if (o->encoding == REDIS_ENCODING_ZIPLIST) {
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    if (hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll)) {
      return 1;
    }

  } else if (o->encoding == REDIS_ENCODING_HT) {
    robj *aux;
    if (hashTypeGetFromHashTable(o, field, &aux)) {
      return 1;
    }
  } else {
    redisPanic("Unknown hash encoding");
  }
  // 不存在
  return 0;
}

int hashTypeSet(robj *o, robj *field, robj *value) {
  int update = 0;

  if (o->encoding == REDIS_ENCODING_ZIPLIST) {
    unsigned char *zl, *fptr, *vptr;

    field = getDecodedObject(field);
    value = getDecodedObject(value);

    zl = o->ptr;
    fptr = ziplistIndex(zl, ZIPLIST_HEAD);
    if (fptr != NULL) {
      fptr = ziplistFind(fptr, field->ptr, sdslen(field->ptr), 1);
      if (fptr != NULL) {
        vptr = ziplistNext(zl, fptr);
        redisAssert(vptr != NULL);

        update = 1; // 这是一次更新
        // 删除后添加
        zl = ziplistDelete(zl, &vptr);
        zl = ziplistInsert(zl, vptr, value->ptr, sdslen(value->ptr));
      }
    }

    if (!update) { // 不是更新
      zl = ziplistPush(zl, field->ptr, sdslen(field->ptr), ZIPLIST_TAIL);
      zl = ziplistPush(zl, value->ptr, sdslen(value->ptr), ZIPLIST_TAIL);
    }

    o->ptr = zl;

    decrRefCount(field);
    decrRefCount(value);

    // 检查一下是否需要更换编码
    if (hashTypeLength(o) > server.hash_max_ziplist_value) {
      hashTypeConvert(o, REDIS_ENCODING_HT);
    }

  } else if (o->encoding == REDIS_ENCODING_HT) {
    if (dictReplace(o->ptr, field, value)) {
      incrRefCount(field);
    } else {
      update = 1;
    }
    incrRefCount(value);
  } else {
    redisPanic("Unknown hash encoding");
  }

  // 更新指示变量
  return update;
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
  robj *o;
  if ((o = hashTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) {
    return;
  }

  hashTypeTryConversion(o, c->argv, 2, 3);

  if (hashTypeExists(o, c->argv[2])) {
    addReply(c, shared.czero); // 已经存在 发送0
  } else {

    // 尝试编码 field 和 value 以节约空间
    hashTypeTryObjectEncoding(o, &c->argv[2], &c->argv[3]);
    hashTypeSet(o, c->argv[2], c->argv[3]);

    addReply(c, shared.cone);

    // TODO 发送事件通知

    server.dirty++;
  }
}

// 将hash中的  field 添加到回复中
static void addHashFieldToReply(redisClient *c, robj *o, robj *field) {
  int ret;

  // 对象不存在
  if (o == NULL) {
    addReply(c, shared.nullbulk);
    return;
  }

  if (o->encoding == REDIS_ENCODING_ZIPLIST) {
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    ret = hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll);
    if (ret < 0) {
      addReply(c, shared.nullbulk);
    } else {
      if (vstr) {
        addReplyBulkCBuffer(c, vstr, vlen);
      } else {
        addReplyBulkLongLong(c, vll);
      }
    }
  } else if (o->encoding == REDIS_ENCODING_HT) {
    robj *value;

    ret = hashTypeGetFromHashTable(o, field, &value);

    if (ret < 0) {
      addReply(c, shared.nullbulk);
    } else {
      addReplyBulk(c, value);
    }
  } else {
    redisPanic("Unknown hash encoding");
  }
}

void hgetCommand(redisClient *c) {
  robj *o;

  if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.nullbulk)) == NULL ||
      checkType(c, o, REDIS_HASH)) {
    return;
  }

  addHashFieldToReply(c, o, c->argv[2]);
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
