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

hashTypeIterator *hashTypeInitIterator(robj *subject) {
  hashTypeIterator *hi = zmalloc(sizeof(hashTypeIterator));

  hi->subject = subject; // 指向对象
  hi->encoding = subject->encoding;

  if (hi->encoding == REDIS_ENCODING_ZIPLIST) {
    hi->fptr = NULL;
    hi->vptr = NULL;
  } else if (hi->encoding == REDIS_ENCODING_HT) {
    hi->di = dictGetIterator(subject->ptr);
  } else {
    redisPanic("Unknown hash encoding");
  }

  // 返回迭代器
  return hi;
}

// 释放迭代器
void hashTypeReleaseIterator(hashTypeIterator *hi) {
  if (hi->encoding == REDIS_ENCODING_HT) {
    dictReleaseIterator(hi->di);
  }
  // 释放 ziplist 迭代器
  zfree(hi);
}

int hashTypeNext(hashTypeIterator *hi) {
  if (hi->encoding == REDIS_ENCODING_ZIPLIST) {
    unsigned char *zl;
    unsigned char *fptr, *vptr;
    zl = hi->subject->ptr;
    fptr = hi->fptr;
    vptr = hi->vptr;

    // 第一次执行时 初始化指针
    if (fptr == NULL) {
      redisAssert(vptr == NULL);
      fptr = ziplistIndex(zl, 0);
    } else {
      redisAssert(vptr != NULL);
      fptr = ziplistNext(zl, vptr);
    }

    if (fptr == NULL)
      return REDIS_ERR;

    vptr = ziplistNext(zl, fptr);
    redisAssert(vptr != NULL);

    // 更新迭代器指针
    hi->fptr = fptr;
    hi->vptr = vptr;
  } else if (hi->encoding == REDIS_ENCODING_HT) {
    if ((hi->de = dictNext(hi->di)) == NULL) {
      return REDIS_ERR;
    }
  } else { // 未知编码
    redisPanic("Unknown hash encoding");
  }

  // 迭代成功
  return REDIS_OK;
}

void hashTypeCurrentFromZiplist(hashTypeIterator *hi, int what,
                                unsigned char **vstr, unsigned int *vlen,
                                long long *vll) {
  int ret;

  // 确保编码正确
  redisAssert(hi->encoding == REDIS_ENCODING_ZIPLIST);

  // 取出键
  if (what & REDIS_HASH_KEY) {
    ret = ziplistGet(hi->fptr, vstr, vlen, vll);
    redisAssert(ret);

    // 取出值
  } else {
    ret = ziplistGet(hi->vptr, vstr, vlen, vll);
    redisAssert(ret);
  }
}

void hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what, robj **dst) {
  redisAssert(hi->encoding == REDIS_ENCODING_HT);

  // 取出键
  if (what & REDIS_HASH_KEY) {
    *dst = dictGetKey(hi->de);

    // 取出值
  } else {
    *dst = dictGetVal(hi->de);
  }
}

robj *hashTypeCurrentObject(hashTypeIterator *hi, int what) {
  robj *dst;

  if (hi->encoding == REDIS_ENCODING_ZIPLIST) { // ZIPLIST
    unsigned char *vstr = NULL;
    unsigned int vlen = UINT_MAX;
    long long vll = LLONG_MAX;

    // 取出键或值
    hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);

    // 创建键或值的对象
    if (vstr) {
      dst = createStringObject((char *)vstr, vlen);
    } else {
      dst = createStringObjectFromLongLong(vll);
    }
  } else if (hi->encoding == REDIS_ENCODING_HT) { // HT
    hashTypeCurrentFromHashTable(hi, what, &dst);
    incrRefCount(dst);
  } else { // 未知编码
    redisPanic("Unknown hash encoding");
  }

  // 返回对象
  return dst;
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

    dict = dictCreate(&hashDictType, NULL);
    while (hashTypeNext(hi) != REDIS_ERR) {
      robj *field, *value;

      // 取出 ziplist 里的键
      field = hashTypeCurrentObject(hi, REDIS_HASH_KEY);
      field = tryObjectEncoding(field);
      printf("ziplist element: %s\n", (char *)field->ptr);

      // 取出 ziplist 里的值
      value = hashTypeCurrentObject(hi, REDIS_HASH_VALUE);
      value = tryObjectEncoding(value);

      // 将键值对添加到字典
      ret = dictAdd(dict, field, value);
      if (ret != DICT_OK) {
        //redisLogHexDump(REDIS_WARNING, "ziplist with dup elements dump", o->ptr,
        //                ziplistBlobLen(o->ptr));
        redisAssert(ret == DICT_OK);
      }
    }

    // 释放 ziplist 的迭代器
    hashTypeReleaseIterator(hi);
    zfree(o->ptr); // 释放原来的 ziplist
    o->encoding = REDIS_ENCODING_HT;
    o->ptr = dict;
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
  dictEntry *de;

  redisAssert(o->encoding == REDIS_ENCODING_HT);

  de = dictFind(o->ptr, field);

  if (de == NULL) {
    printf("????? %s\n", (char *)o->ptr);
    return -1;
  }

  *value = dictGetVal(de);

  return 0;
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
  printf("获取 %s %s \n", (char *)o->ptr, (char *)c->argv[2]->ptr);
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
