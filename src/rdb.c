#include "rdb.h"
#include "rio.h"
#include "redis.h"
#include "dict.h"
#include <errno.h>
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
    buf[0] = (len&0xFF)|(REDIS_RDB_6BITLEN<<6);
    if (rdbWriteRaw(rdb, buf, 1) == -1) {
      return -1;
    }
    nwritten = 1;
  } else if (len < (1 << 14)) { // save a 14 bit len

  } else { // save a 32 bit len
  }

  return nwritten;
}

uint32_t rdbLoadLen(rio *rdb, int *isencoded) {

}

int rdbSaveObjectType(rio *rdb, robj *o) {

}

int rdbLoadObjectType(rio *rdb) {

}

int rdbLoad(char *filename) {

}

void rdbRemoveTempFile(pid_t childpid) {

}

int rdbSaveObject(rio *rdb, robj *o) {

}

off_t rdbSavedObjectLen(robj *o) {

}

off_t rdbSavedObjectPages(robj *o) {

}

robj *rdbLoadObject(int type, rio *rdb) {

}

void backgroundSaveDoneHandler(int exitcode, int bysignal) {

}

// [len][data]
int rdbSaveRawString(rio *rdb, unsigned char *s, size_t len) {
  int enclen;
  int n, nwritten = 0;

  if (len <= 11) {
    unsigned char buf[5];
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

// 保存 String 到 RDB 中
int rdbSaveStringObject(rio *rdb, robj *obj) {
  if (obj->encoding == REDIS_ENCODING_INT) { //

  } else { // String

  }
}

robj *rdbLoadStringObject(rio *rdb) {

}




int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, long long expiretime, long long now) {
  if (expiretime != -1) {
    if (expiretime < now) {
      return 0;
    }
    // 保存过期信息
    if (rdbSaveType(rdb, REDIS_RDB_OPCODE_EXPIRETIME_MS) == -1)
      return -1;
    if (rdbSaveMillsecondTime(rdb, expiretime) == -1)
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

  printf("tmpfile: %s\n", tmpfile);
  // 初始化IO
  rioInitWithFile(&rdb, fp);

  // 设置校验和
  if (server.rdb_checksum) {
    rdb.update_cksum = rioGenericUpdateChecksum;
  }

  // 写入 RDB 版本号
  // TODO

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

    while((de = dictNext(di)) != NULL) {
      sds keystr = dictGetKey(de);
      robj key, *o = dictGetVal(de);
      long long expire;

      expire = getExpire(db, &key);

      printf("%s 保存中\n", keystr);
    }
    dictReleaseIterator(di);
  }
  di = NULL;

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
  } else if (1) { // TODO
    addReplyStatus(c, "Background saving started");
  } else {
    addReply(c, shared.err);
  }
}
