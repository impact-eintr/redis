#include "rdb.h"
#include "rio.h"
#include "redis.h"
#include "dict.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
