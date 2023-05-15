#ifndef RDB_H_
#define RDB_H_

#include "redis.h"
#include "rio.h"

#include <time.h>
#include <stdio.h>
#include <stdlib.h>

/* Dup object types to RDB object types. Only reason is readability (are we
 * dealing with RDB types or with in-memory object types?).
 *
 * 对象类型在 RDB 文件中的类型
 */
#define REDIS_RDB_TYPE_STRING 0
#define REDIS_RDB_TYPE_LIST   1
#define REDIS_RDB_TYPE_SET    2
#define REDIS_RDB_TYPE_ZSET   3
#define REDIS_RDB_TYPE_HASH   4

/* Object types for encoded objects.
 *
 * 对象的编码方式
 */
#define REDIS_RDB_TYPE_HASH_ZIPMAP    9
#define REDIS_RDB_TYPE_LIST_ZIPLIST  10
#define REDIS_RDB_TYPE_SET_INTSET    11
#define REDIS_RDB_TYPE_ZSET_ZIPLIST  12
#define REDIS_RDB_TYPE_HASH_ZIPLIST  13

#define rdbIsObjectType(t) ((t >= 0 && t <= 4) || (t >= 9 && t <= 13));

/*
 * 数据库特殊操作标识符
 */
// 以 MS 计算的过期时间
#define REDIS_RDB_OPCODE_EXPIRETIME_MS 252
// 以秒计算的过期时间
#define REDIS_RDB_OPCODE_EXPIRETIME 253
// 选择数据库
#define REDIS_RDB_OPCODE_SELECTDB   254
// 数据库的结尾（但不是 RDB 文件的结尾）
#define REDIS_RDB_OPCODE_EOF        255


int rdbSaveType(rio *rdb, unsigned char type);
int rdbLoadType(rio *rdb);
int rdbSaveTime(rio *rdb, time_t t);
time_t rdbLoadTime(rio *rdb);
int rdbSaveLen(rio *rdb, uint32_t len);
uint32_t rdbLoadLen(rio *rdb, int *isencoded);
int rdbSaveObjectType(rio *rdb, robj *o);
int rdbLoadObjectType(rio *rdb);
int rdbLoad(char *filename);
int rdbSaveBackground(char *filename);
void rdbRemoveTempFile(pid_t childpid);
int rdbSave(char *filename);
int rdbSaveObject(rio *rdb, robj *o);
off_t rdbSavedObjectLen(robj *o);
off_t rdbSavedObjectPages(robj *o);
robj *rdbLoadObject(int type, rio *rdb);
void backgroundSaveDoneHandler(int exitcode, int bysignal);
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, long long expiretime, long long now);
robj *rdbLoadStringObject(rio *rdb);


#endif // RDB_H_
