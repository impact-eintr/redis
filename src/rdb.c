#include "rdb.h"
#include "adlist.h"
#include "endianconv.h"
#include "rio.h"
#include "redis.h"
#include "dict.h"
#include "sds.h"
#include "ziplist.h"
#include "util.h"
#include "zmalloc.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int rdbWriteRaw(rio *rdb, void *p, size_t len) {
  if (rdb && rioWrite(rdb, p, len) == 0) {
    return -1;
  }
  return len;
}

// 写入1字节的类型
int rdbSaveType(rio *rdb, unsigned char type) {
  return rdbWriteRaw(rdb, &type, 1);
}

// 载入1字节的类型
int rdbLoadType(rio *rdb) {
  unsigned char type;

  if (rioRead(rdb, &type, 1) == 0)
    return -1;
  return type;
}


// 载入以秒为单位的过期时间 长度为 4 bytes
time_t rdbLoadTime(rio *rdb) {
  int32_t t32;
  if (rioRead(rdb, &t32, 4) == 0)
    return -1;
  return (time_t)t32;
}

long long rdbLoadMillisecondTime(rio *rdb) {
  int64_t t64;
  if (rioRead(rdb, &t64, 8) == 0)
    return -1;

  return (long long)t64;
}

int rdbSaveMillisecondTime(rio *rdb, long long t) {
  int64_t t64;
  if (rioRead(rdb, &t64, 8) == 0)
    return -1;
  return (long long)t64;
}

int rdbSaveLen(rio *rdb, uint32_t len) {
  unsigned char buf[2];
  size_t nwritten;

  if (len < (1 << 6)) { // save a 6 bit len
    buf[0] = (len&0x3F)|(REDIS_RDB_6BITLEN<<6);
    if (rdbWriteRaw(rdb, buf, 1) == -1) {
      return -1;
    }
    nwritten = 1;
  } else if (len < (1 << 14)) { // save a 14 bit len
    buf[0] = ((len>>8)&0x3F)|(REDIS_RDB_14BITLEN<<6);
    buf[1] = len&0xFF;
    if (rdbWriteRaw(rdb, buf, 2) == -1) {
      return -1;
    }
    nwritten = 2;
  } else { // save a 32 bit len
    buf[0] = (REDIS_RDB_32BITLEN<<6);
    if (rdbWriteRaw(rdb, buf, 1) == -1) {
      return -1;
    }
    len = htonl(len);
    if (rdbWriteRaw(rdb, &len, 4) == -1) {
      return -1;
    }
    nwritten = 1+4;
  }

  return nwritten;
}

int rdbEncodeInteger(long long value, unsigned char *enc) {
  if (value >= -(1<<7) && value <= (1<<7)-1) {
    enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT8;
    enc[1] = value&0xFF;
    return 2;
  } else if (value >= -(1<<15) && value <= (1<<15)-1) {
    enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT16;
    enc[1] = value&0xFF;
    enc[2] = (value>>8)&0xFF;
    return 3;
  } else if (value >= -(long long)(1<<31) && value <= (long long)(1<<31)-1) {
    enc[0] = (REDIS_RDB_ENCVAL<<6)|REDIS_RDB_ENC_INT32;
    enc[1] = value&0xFF;
    enc[2] = (value>>8)&0xFF;
    enc[3] = (value>>16)&0xFF;
    enc[4] = (value>>24)&0xFF;

    return 5;
  } else {
    return 0;
  }
}

// 载入整数编码对象
robj *rdbLoadIntegerObject(rio *rdb, int enctype, int encode) {
  unsigned char enc[4];
  long long val;

  printf("加载String长度\n");
  // 整数编码
  if (enctype == REDIS_RDB_ENC_INT8) {
    if (rioRead(rdb, enc, 1) == 0) {
      return NULL;
    }
    val = (signed char)enc[0];
    printf("val len: %lld", val);
  } else if (enctype == REDIS_RDB_ENC_INT16) {
    uint16_t v;
    if (rioRead(rdb, enc, 2) == 0) {
      return NULL;
    }
    v = enc[0]|(enc[1]<<8);
    val = (int16_t)v;
  } else if (enctype == REDIS_RDB_ENC_INT32) {
    uint32_t v;
    if (rioRead(rdb, enc, 4) == 0)
      return NULL;
    v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
    val = (int32_t)v;
  } else {
    val = 0;
    redisPanic("Unknown RDB integer encoding type");
  }

  if (encode) {
    return createStringObjectFromLongLong(val);
  } else {
    return createObject(REDIS_STRING, sdsfromlonglong(val));
  }
}

int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
  long long value;
  char *endptr, buf[32];

  value = strtoll(s, &endptr, 10);
  if (endptr[0] != '\0') {
    return 0;
  }

  ll2string(buf, 32, value);

  if (strlen(buf) != len || memcpy(buf, s, len)) {
    return 0;
  }

  return rdbEncodeInteger(value, enc);
}

uint32_t rdbLoadLen(rio *rdb, int *isencoded) {
  unsigned char buf[2];
  uint32_t len;
  int type;

  if (isencoded) *isencoded = 0;

  if (rioRead(rdb, buf, 1) == 0)
    return REDIS_RDB_LENERR;

  type = (buf[0]&0xC0)>>6;
  printf("load type: %d\n", type);
  if (type == REDIS_RDB_ENCVAL) { // value
    if (isencoded)
      *isencoded = 1;
    return buf[0]&0x3F; // 获取编码类型
  } else if (type == REDIS_RDB_6BITLEN) {
    return buf[0]&0x3F; // 获取6bit的长度
  } else if (type == REDIS_RDB_14BITLEN) {
    if (rioRead(rdb, buf+1, 1) == 0)
      return REDIS_RDB_LENERR;
    printf("load type: %d len %d\n", type, ((buf[0]&0x3F)<<8)|buf[1]);
    return ((buf[0]&0x3F)<<8)|buf[1];
  } else {
    if (rioRead(rdb, &len, 4) == 0)
      return REDIS_RDB_LENERR;
    return ntohl(len);
  }
}

int rdbSaveObjectType(rio *rdb, robj *o) {
  switch (o->type) {
  case REDIS_STRING:
    return rdbSaveType(rdb, REDIS_RDB_TYPE_STRING);
  case REDIS_LIST:
    if (o->encoding == REDIS_ENCODING_ZIPLIST)
      return rdbSaveType(rdb, REDIS_RDB_TYPE_LIST_ZIPLIST);
    else if (o->encoding == REDIS_ENCODING_LINKEDLIST)
      return rdbSaveType(rdb, REDIS_RDB_TYPE_LIST);
    else
      redisPanic("Unknown list encoding");

  case REDIS_SET:
    if (o->encoding == REDIS_ENCODING_INTSET)
      return rdbSaveType(rdb, REDIS_RDB_TYPE_SET_INTSET);
    else if (o->encoding == REDIS_ENCODING_HT)
      return rdbSaveType(rdb, REDIS_RDB_TYPE_SET);
    else
      redisPanic("Unknown set encoding");

  case REDIS_ZSET:
    if (o->encoding == REDIS_ENCODING_ZIPLIST)
      return rdbSaveType(rdb, REDIS_RDB_TYPE_ZSET_ZIPLIST);
    else if (o->encoding == REDIS_ENCODING_SKIPLIST)
      return rdbSaveType(rdb, REDIS_RDB_TYPE_ZSET);
    else
      redisPanic("Unknown sorted set encoding");

  case REDIS_HASH:
    if (o->encoding == REDIS_ENCODING_ZIPLIST)
      return rdbSaveType(rdb, REDIS_RDB_TYPE_HASH_ZIPLIST);
    else if (o->encoding == REDIS_ENCODING_HT)
      return rdbSaveType(rdb, REDIS_RDB_TYPE_HASH);
    else
      redisPanic("Unknown hash encoding");

  default:
    redisPanic("Unknown object type");
  }
  return -1;
}

int rdbLoadObjectType(rio *rdb) {

}

void startLoading(FILE *fp) {
  struct stat sb;

  server.loading = 1;
  server.loading_start_time = time(NULL);
  if (fstat(fileno(fp), &sb) == -1) {
    server.loading_total_bytes = 1;
  } else {
    server.loading_total_bytes = sb.st_size;
  }
}

void loadingProgress(off_t pos) {
  server.loading_loaded_bytes = pos;
  if (server.stat_peak_memory > zmalloc_used_memory()) {
    server.stat_peak_memory = zmalloc_used_memory();
  }
}

void stopLoading(void) {
  server.loading = 0;
}

// 记录载入进度信息，以便让客户端进行查询
// 这也会在计算 RDB 校验和时用到。
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len) {
  if (server.rdb_checksum)
    rioGenericUpdateChecksum(r, buf, len);
  if (server.loading_process_events_interval_bytes &&
      (r->processed_bytes + len) /
              server.loading_process_events_interval_bytes >
          r->processed_bytes / server.loading_process_events_interval_bytes) {
    /* The DB can take some non trivial amount of time to load. Update
     * our cached time since it is used to create and update the last
     * interaction time with clients and for other important things. */
    updateCachedTime();
    //if (server.masterhost && server.repl_state == REDIS_REPL_TRANSFER)
    //  replicationSendNewlineToMaster();
    loadingProgress(r->processed_bytes);
    //processEventsWhileBlocked();
  }
}

int rdbLoad(char *filename) {
  uint32_t dbid;
  int type, rdbver;
  redisDb *db = server.db+0;
  char buf[1024];
  long long expiretime, now = mstime();
  FILE *fp;
  rio rdb;

  if ((fp = fopen(filename, "r")) == NULL) {
    return REDIS_ERR;
  }

  rioInitWithFile(&rdb, fp);
  rdb.update_cksum = rdbLoadProgressCallback;

  if (rioRead(&rdb, buf, 9) == 0)
    goto eoferr;

  // 检查版本号
  if (memcmp(buf, "REDIS", 5) != 0) {
    fclose(fp);
    redisLog(REDIS_WARNING, "Wrong signature trying to load DB from file");
    errno = EINVAL;
    return REDIS_ERR;
  }
  rdbver = atoi(buf + 5);
  if (rdbver < 1 || rdbver > REDIS_RDB_VERSION) {
    fclose(fp);
    redisLog(REDIS_WARNING, "Can't handle RDB format version %d", rdbver);
    errno = EINVAL;
    return REDIS_ERR;
  }

  startLoading(fp);
  while (1) {
    robj *key, *val;
    expiretime = -1;

    if ((type = rdbLoadType(&rdb)) == -1)
      goto eoferr;

    if (type == REDIS_RDB_OPCODE_EXPIRETIME) {

    } else if (type == REDIS_RDB_OPCODE_EXPIRETIME_MS) {

    }

    if (type == REDIS_RDB_OPCODE_EOF)
      break;

    // 读入切换数据库指令
    if (type == REDIS_RDB_OPCODE_SELECTDB) {
      if ((dbid = rdbLoadLen(&rdb, NULL)) == REDIS_RDB_LENERR) {
        goto eoferr;
      }

      printf("dbid %d\n", dbid);
       // 检查数据库号码的正确性
      if (dbid >= (unsigned)server.dbnum) {
        redisLog(REDIS_WARNING,
                 "FATAL: Data file was created with a Redis server configured "
                 "to handle more than %d databases.           Exiting\n",
                 server.dbnum);
        exit(1);
      }

      // 在程序内容切换数据库
      db = server.db + dbid;

      // 跳过
      continue;
    }

    // 读入键
    if ((key = rdbLoadStringObject(&rdb)) == NULL) {
      printf("加载键失败\n");
      goto eoferr;
    }

    // 读入值
    if ((val = rdbLoadObject(type, &rdb)) == NULL) {
      printf("加载%s: 值失败\n", (char *)key->ptr);
      goto eoferr;
    }
    // TODO 处理 MASTER

    dbAdd(db, key, val);

    if (expiretime != -1)
      setExpire(db, key, expiretime);

    decrRefCount(key);
  }

  /* Verify the checksum if RDB version is >= 5
   *
   * 如果 RDB 版本 >= 5 ，那么比对校验和
   */
  if (rdbver >= 5 && server.rdb_checksum) {
    uint64_t cksum, expected = rdb.cksum;

    // 读入文件的校验和
    if (rioRead(&rdb, &cksum, 8) == 0)
      goto eoferr;
    memrev64ifbe(&cksum);

    // 比对校验和
    if (cksum == 0) {
      redisLog(
          REDIS_WARNING,
          "RDB file was saved with checksum disabled: no check performed.");
    } else if (cksum != expected) {
      redisLog(REDIS_WARNING, "Wrong RDB checksum. Aborting now.");
      exit(1);
    }
  }

  // 关闭 RDB
  fclose(fp);

  // 服务器从载入状态中退出
  stopLoading();

  return REDIS_OK;

eoferr: /* unexpected end of file is handled here with a fatal exit */
  redisLog(REDIS_WARNING,
           "Short read or OOM loading DB. Unrecoverable error, aborting now.");
  exit(1);
  return REDIS_ERR; /* Just to avoid warning */
}

void rdbRemoveTempFile(pid_t childpid) {
  char tmpfile[256];

  snprintf(tmpfile, 256, "temp-%d.rdb", (int)childpid);
  unlink(tmpfile);
}



void backgroundSaveDoneHandler(int exitcode, int bysignal) {

}

// 对于字符串 使用 [len][data] 保存
int rdbSaveRawString(rio *rdb, unsigned char *s, size_t len) {
  int enclen;
  int n, nwritten = 0;

    printf("??? %d\n", len);
  if (len <= 11) {
    unsigned char buf[5];
    if ((enclen = rdbTryIntegerEncoding((char *)s, len, buf)) > 0){
      if (rdbWriteRaw(rdb, buf, enclen) == -1) {
        return -1;
      }
      return enclen;
    }
  }

  if (server.rdb_compression && len > 20) {
    printf("尝试压缩\n");
  }

  // 写入长度
  if ((n = rdbSaveLen(rdb, len)) == -1)
    return -1;
  nwritten += n;

  // 写入内容
  if (len > 0) {
    if (rdbWriteRaw(rdb, s, len) == -1)
      return -1;
    nwritten += n;
  }
  return nwritten;
}

// 将long long 保存成 String
int rdbSaveLongLongAsStringObject(rio *rdb, long long value) {
  unsigned char buf[32];
  int n, nwritten = 0;

  int enclen = rdbEncodeInteger(value, buf);

  if (enclen > 0) {

  } else { // 编码失败

  }

  return nwritten;
}

// 保存 String 到 RDB 中
int rdbSaveStringObject(rio *rdb, robj *obj) {
  if (obj->encoding == REDIS_ENCODING_INT) { //
    return rdbSaveLongLongAsStringObject(rdb, (long)obj->ptr);
  } else { // String
    redisAssertWithInfo(NULL, obj->ptr, sdsEncodedObject(obj));
    return rdbSaveRawString(rdb, obj->ptr, sdslen(obj->ptr));
  }
}

robj *rdbLoadLzfStringObject(rio *rdb) {
  exit(1);
}

robj *rdbGenericLoadStringObject(rio *rdb, int encode) {
  int isencoded;
  uint32_t len;
  sds val;

  // 先加载编码长度
  len = rdbLoadLen(rdb, &isencoded);
  printf("len : %d\n", len);

  if (isencoded) {
    switch (len) {
      // 整数编码
      case REDIS_RDB_ENC_INT8:
      case REDIS_RDB_ENC_INT16:
      case REDIS_RDB_ENC_INT32:
        return rdbLoadIntegerObject(rdb, len, encode); // 加载字符串长度
      // LZF 压缩编码
      case REDIS_RDB_ENC_LZF:
        return rdbLoadLzfStringObject(rdb);
      default:
        redisPanic("Unknown RDB encoding type");
    }
  }

  if (len == REDIS_RDB_LENERR)
    return NULL;

  val = sdsnewlen(NULL, len);
  if (len && rioRead(rdb, val, len) == 0) {
    sdsfree(val);
    return NULL;
  }

  return createObject(REDIS_STRING, val);
}

robj *rdbLoadStringObject(rio *rdb) {
  return rdbGenericLoadStringObject(rdb, 0);
}

robj *rdbLoadEncodedStringObject(rio *rdb) {
  return rdbGenericLoadStringObject(rdb, 1);
}

// 保存指定对象
int rdbSaveObject(rio *rdb, robj *o) {
  int n, nwritten = 0;

  // 保存String
  if (o->type == REDIS_STRING) {
    if ((n = rdbSaveStringObject(rdb, o)) == -1) {
      return -1;
    }
    nwritten+=n;
    // 保存List
  } else if (o->type == REDIS_LIST) {
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
      size_t l = ziplistBlobLen((unsigned char *)o->ptr);
      // 以字符串形式保存整个 ZIPLIST 列表
      if ((n = rdbSaveRawString(rdb, o->ptr, l)) == -1) {
        return -1;
      }
      nwritten += n;

    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
      list *list = o->ptr;
      listIter li;
      listNode *ln;

      if ((n = rdbSaveLen(rdb, listLength(list))) == -1) {
        return -1;
      }
      nwritten += n;

      listRewind(list, &li);

      while ((ln = listNext(&li))) {
        robj *eleobj = listNodeValue(ln);
        if ((n = rdbSaveStringObject(rdb, eleobj)) == -1) {
          return -1;
        }
        nwritten += n;
      }
    } else {
      redisPanic("Unknown set encoding");
    }
    // 保存Set
  } else if (o->type == REDIS_SET) {

    if (o->encoding == REDIS_ENCODING_HT) {
    } else if (o->encoding == REDIS_ENCODING_INTSET) {

    } else {
      redisPanic("Unknown set encoding");
    }
    // 保存Zset
  } else if (o->type == REDIS_ZSET) {

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {

    } else if (o->encoding == REDIS_ENCODING_SKIPLIST) {

    } else {
      redisPanic("Unknown zset encoding");
    }
    // 保存Hash
  } else if (o->type == REDIS_HASH) {

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
      size_t l = ziplistBlobLen((unsigned char *)o->ptr);
      if ((n = rdbSaveRawString(rdb, o->ptr, l)) == -1) {
        return -1;
      }
      nwritten += n;
    } else if (o->encoding == REDIS_ENCODING_HT) {
      dictIterator *di = dictGetIterator(o->ptr);
      dictEntry *de;

      if ((n = rdbSaveLen(rdb, dictSize((dict *)o->ptr))) == -1) {
        return -1;
      }
      nwritten += n;

      // 迭代字典
      while ((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);
        robj *val = dictGetVal(de);

        // 键和值都以字符串对象的形式保存
        if ((n = rdbSaveStringObject(rdb, key)) == -1) {
          return -1;
        }
        nwritten += n;
        if ((n = rdbSaveStringObject(rdb, val)) == -1) {
          return -1;
        }
        nwritten += n;
      }
      dictReleaseIterator(di);
    } else {
      redisPanic("Unknown hash encoding");
    }
  } else {
    redisPanic("Unknown object type");
  }

  return nwritten;
}

off_t rdbSavedObjectLen(robj *o) {

}

off_t rdbSavedObjectPages(robj *o) {

}

robj *rdbLoadObject(int rdbtype, rio *rdb) {
  robj *o, *ele, *dec;
  size_t len;
  unsigned int i;

  // 载入各种对象
  if (rdbtype == REDIS_RDB_TYPE_STRING) {
    printf("RDB STRING type\n");
    if ((o = rdbLoadEncodedStringObject(rdb)) == NULL) {
      return NULL;
    }
    o = tryObjectEncoding(o);
  } else if (rdbtype == REDIS_RDB_TYPE_LIST) {
    printf("RDB LIST type\n");
    if ((len = rdbLoadLen(rdb, NULL)) == REDIS_RDB_LENERR) {
      return NULL;
    }
    if (len > server.list_max_ziplist_entries) {
      o = createListObject();
    } else {
      o = createZiplistObject();
    }

    while (len--) {
      if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) {
        return NULL;
      }

      // 根据载入的元素长度判断是否继续要使用ziplist保存
      if (o->encoding == REDIS_ENCODING_ZIPLIST && sdsEncodedObject(ele) &&
          sdslen(ele->ptr) > server.list_max_ziplist_value) {
      }

      if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        dec = getDecodedObject(ele);
        o->ptr = ziplistPush(o->ptr, dec->ptr, sdslen(dec->ptr), REDIS_TAIL);

        decrRefCount(dec);
        decrRefCount(ele);
      } else {
        ele = tryObjectEncoding(ele);
        listAddNodeTail(o->ptr, ele);
      }
      printf("元素 %s\n", (char *)ele->ptr);
    }
  } else if (rdbtype == REDIS_RDB_TYPE_SET) {
    printf("RDB SET type\n");
  } else if (rdbtype == REDIS_RDB_TYPE_ZSET) {
    printf("RDB ZSET type\n");

  } else if (rdbtype == REDIS_RDB_TYPE_HASH) {
    printf("RDB HASH type\n");
    // 加载 Hash 类型的数据
    size_t len;
    int ret;

    len = rdbLoadLen(rdb, NULL);
    if (len == REDIS_RDB_LENERR) {
      return NULL;
    }

    // 创建哈希表
    o = createHashObject();

    if (len > server.hash_max_ziplist_entries) {
      hashTypeConvert(o, REDIS_ENCODING_HT);
    }

    // 载入数据
    while (o->encoding == REDIS_ENCODING_ZIPLIST && len > 0) {
      robj *field, *value;
      len--;

      field = rdbLoadStringObject(rdb); // 载入域
      if (field == NULL) return NULL;
      redisAssert(sdsEncodedObject(field));
      value = rdbLoadStringObject(rdb); // 载入域
      if (value == NULL) return NULL;
      redisAssert(sdsEncodedObject(value));

      o->ptr = ziplistPush(o->ptr, field->ptr, sdslen(field->ptr), ZIPLIST_TAIL);
      o->ptr = ziplistPush(o->ptr, value->ptr, sdslen(value->ptr), ZIPLIST_TAIL);

      if (sdslen(field->ptr) > server.hash_max_ziplist_value ||
        sdslen(value->ptr) > server.hash_max_ziplist_value) {
        decrRefCount(field);
        decrRefCount(value);
        hashTypeConvert(o, REDIS_ENCODING_HT);
        break;
      }
      decrRefCount(field);
      decrRefCount(value);
    }

    // 有可能进行编码转换
    while (o->encoding == REDIS_ENCODING_HT && len > 0) {
      robj *field, *value;

      len--;

      /* Load encoded strings */
      // 域和值都载入为字符串对象
      field = rdbLoadEncodedStringObject(rdb);
      if (field == NULL)
        return NULL;
      value = rdbLoadEncodedStringObject(rdb);
      if (value == NULL)
        return NULL;

      // 尝试编码
      field = tryObjectEncoding(field);
      value = tryObjectEncoding(value);

      ret = dictAdd((dict *)o->ptr, field, value);
      redisAssert(ret == REDIS_OK);
    }

    redisAssert(len == 0);
  } else if (rdbtype == REDIS_RDB_TYPE_HASH_ZIPMAP  ||
               rdbtype == REDIS_RDB_TYPE_LIST_ZIPLIST ||
               rdbtype == REDIS_RDB_TYPE_SET_INTSET   ||
               rdbtype == REDIS_RDB_TYPE_ZSET_ZIPLIST ||
             rdbtype == REDIS_RDB_TYPE_HASH_ZIPLIST) {

    printf("RDB Other type\n");
    robj *aux = rdbLoadStringObject(rdb);

    if (aux == NULL) {
      return NULL;
    }

    o = createObject(REDIS_STRING, NULL); // string 仅仅是一个占位符
    o->ptr = zmalloc(sdslen(aux->ptr));
    memcpy(o->ptr, aux->ptr, sdslen(aux->ptr));
    decrRefCount(aux);

    // 在创建对象的过程中，将值恢复成原来的编码对象
    switch (rdbtype) {
    case REDIS_RDB_TYPE_HASH_ZIPMAP:
      // TODO
      break;
    case REDIS_RDB_TYPE_LIST_ZIPLIST:
      o->type = REDIS_LIST;
      o->encoding = REDIS_ENCODING_ZIPLIST;

      if (ziplistLen(o->ptr) > server.list_max_ziplist_entries) {
        listTypeConvert(o, REDIS_ENCODING_LINKEDLIST);
      }
      break;
    case REDIS_RDB_TYPE_SET_INTSET:
      break;
    case REDIS_RDB_TYPE_ZSET_ZIPLIST:
      break;
    case REDIS_RDB_TYPE_HASH_ZIPLIST:
      o->type = REDIS_HASH;
      o->encoding = REDIS_ENCODING_ZIPLIST;
      if (hashTypeLength(o) > server.hash_max_ziplist_entries) {
        hashTypeConvert(o, REDIS_ENCODING_ZIPLIST);
      }
      break;
    default:
      break;
    }
  } else {
    redisPanic("Unknown object type");
  }

  return o;
}

int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, long long expiretime, long long now) {
  printf("Saving Object: %s\n", (char *)key->ptr);
  if (expiretime != -1) {
    if (expiretime < now) {
      return 0;
    }
    // 保存过期信息
    if (rdbSaveType(rdb, REDIS_RDB_OPCODE_EXPIRETIME_MS) == -1)
      return -1;
    if (rdbSaveMillisecondTime(rdb, expiretime) == -1)
      return -1;
  }

  // 保存类型 键 值
  if (rdbSaveObjectType(rdb, val) == -1)
    return -1;
  if (rdbSaveStringObject(rdb, key) == -1)
    return -1;
  if (rdbSaveObject(rdb, val) == -1)
    return -1;
  return 1;
}

// 将数据库保存到磁盘中
int rdbSave(char *filename) {
  dictIterator *di = NULL;
  dictEntry *de;
  char tmpfile[256];
  char magic[10];
  int j;
  long long now = mstime();
  FILE *fp;
  rio rdb;
  uint64_t cksum;

  // 创建临时文件
  snprintf(tmpfile, 256, "temp-%d.rdb", (int)getpid());
  fp = fopen(tmpfile, "w");
  if (!fp) {
    redisLog(REDIS_WARNING, "Failed opening .rdb for saving %s", strerror(errno));
    return REDIS_ERR;
  }

  // 初始化IO
  rioInitWithFile(&rdb, fp);

  // 设置校验和
  if (server.rdb_checksum) {
    rdb.update_cksum = rioGenericUpdateChecksum;
  }

  // 写入 RDB 版本号
  snprintf(magic, sizeof(magic), "REDIS%04d", REDIS_RDB_VERSION);
  if (rdbWriteRaw(&rdb, magic, 9) == -1)
    goto werr;

  // 遍历所有数据库
  for (j = 0;j < server.dbnum;j++) {
    redisDb *db = server.db+j;
    dict *d = db->dict;
    if (dictSize(d) == 0) continue;

    di = dictGetSafeIterator(d);
    if (!di) {
      fclose(fp);
      return REDIS_ERR;
    }

    // 写入 DB 选择器
    if (rdbSaveType(&rdb, REDIS_RDB_OPCODE_SELECTDB) == -1)
      goto werr;
    if (rdbSaveLen(&rdb, j) == -1) // 写入数据库号
      goto werr;

    while((de = dictNext(di)) != NULL) {
      sds keystr = dictGetKey(de);
      robj key, *o = dictGetVal(de);
      long long expire;

      expire = getExpire(db, &key);
      initStaticStringObject(key, keystr);

      if (rdbSaveKeyValuePair(&rdb, &key, o, expire, now) == -1)
        goto werr;
    }
    dictReleaseIterator(di);
  }
  di = NULL;

  if (rdbSaveType(&rdb, REDIS_RDB_OPCODE_EOF) == -1)
    goto werr;

  cksum = rdb.cksum;
  memrev64ifbe(&cksum);
  rioWrite(&rdb, &cksum, 8);

  if (fflush(fp) == EOF)
    goto werr;
  if (fsync(fileno(fp)) == -1)
    goto werr;
  if (fclose(fp) == EOF)
    goto werr;

  if (rename(tmpfile, filename) == -1) {
    redisLog(REDIS_WARNING,
             "Error moving temp DB file on the final destination: %s",
             strerror(errno));
    unlink(tmpfile);
    return REDIS_ERR;
  }

  redisLog(REDIS_NOTICE, "DB saved on disk");
  server.dirty = 0; // 取消数据库脏状态
  server.lastsave = time(NULL); // 记录最后一次完成 SAVE  的时间
  server.lastbgsave_status = REDIS_OK; // 记录最后一次执行 SAVE 的状态

  return REDIS_OK;

werr:
  // 关闭文件
  fclose(fp);
  // 删除文件
  unlink(tmpfile);

  if (di) {
    dictReleaseIterator(di);
  }
  return REDIS_ERR;
}

// 后台保存
int rdbSaveBackground(char *filename) {
  pid_t childpid;
  long long start;

  if (server.rdb_child_pid != -1) {
    return REDIS_ERR;
  }

  server.dirty_before_bgsave = server.dirty; // 记录BGSAVE之前的修改次数
  server.lastbgsave_try = time(NULL);
  start = ustime();

  if ((childpid = fork()) == 0) { // 子进程
    int retval;
    closeListeningSockets(0); // 关闭子进程的socket连接 因为 fork 后复制了整个进程空间

    // TODO 设置进程的标题
    retval = rdbSave(filename);

    if (retval == REDIS_OK) {
      size_t private_dirty = zmalloc_get_private_dirty();

      if (private_dirty) {
        redisLog(REDIS_NOTICE, "RDB: %zu MB of memory used by copy-on-write",
                 private_dirty / (1024 * 1024));
      }
    }

    // 向父进程发送信号
    exitFromChild((retval == REDIS_OK) ? 0 : 1);

  } else {
    server.stat_fork_time = ustime() - start;

    if (childpid == -1) {
      server.lastbgsave_status = REDIS_ERR;
      redisLog(REDIS_WARNING, "Can not save in background: fork: %s", strerror(errno));
      return REDIS_ERR;
    }

    // 打印 BGSAVE 开始的日志
    redisLog(REDIS_NOTICE, "Background saving started by pid %d", childpid);

    // 记录数据库开始 BGSAVE 的时间
    server.rdb_save_time_start = time(NULL);

    // 记录负责执行 BGSAVE 的子进程 ID
    server.rdb_child_pid = childpid;

    // 关闭自动 rehash
    updateDictResizePolicy();

    return REDIS_OK;
  }

  return REDIS_OK;
}


void saveCommand(redisClient *c) {
  if (server.rdb_child_pid != -1) {
    addReplyError(c, "Background saving already in progress");
  }

  // 执行
  if (rdbSave(server.rdb_filename) == REDIS_OK) {
    addReply(c, shared.ok);
  } else {
    addReply(c, shared.err);
  }
}

void bgsaveCommand(redisClient *c) {
  // 不可以重复执行
  if (server.rdb_child_pid != -1) {
    addReplyError(c, "Background saving already in progress");
  } else if (server.aof_child_pid != -1) { // 后台执行aof时不执行
    addReplyError(c, "Can't BGSAVE while AOF log rewriting is in progress");
  } else if (rdbSaveBackground(server.rdb_filename) == REDIS_OK) {
    addReplyStatus(c, "Background saving started");
  } else {
    addReply(c, shared.err);
  }
}
