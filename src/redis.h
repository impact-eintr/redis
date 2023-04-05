#ifndef REDIS_H_
#define REDIS_H_

#include <unistd.h>
#include <assert.h>

#include "dict.h"
#include "sds.h"

// 对象类型
#define REDIS_STRING 0
#define REDIS_LIST 1
#define REDIS_SET 2
#define REDIS_ZSET 3
#define REDIS_HASH 4

// 对象编码
#define REDIS_ENCODING_RAW 0        /* Raw representation */
#define REDIS_ENCODING_INT 1        /* Encoded as integer */
#define REDIS_ENCODING_HT 2         /* Encoded as hash table */
#define REDIS_ENCODING_ZIPMAP 3     /* Encoded as zipmap */
#define REDIS_ENCODING_LINKEDLIST 4 /* Encoded as regular linked list */
#define REDIS_ENCODING_ZIPLIST 5    /* Encoded as ziplist */
#define REDIS_ENCODING_INTSET 6     /* Encoded as intset */
#define REDIS_ENCODING_SKIPLIST 7   /* Encoded as skiplist */
#define REDIS_ENCODING_EMBSTR 8     /* Embedded sds string encoding */

#define ZSKIPLIST_MAXLEVEL 32
#define ZSKIPLIST_P 0.25


#define REDIS_LRU_BITS 24

typedef struct redisObject {
  // 类型
  unsigned type:4;
  // 编码
  unsigned encoding:4;
  // 对象对后一次被访问的时间
  unsigned lru:REDIS_LRU_BITS;
  // 引用计数
  int refcount;
  // 指向实际值的指针
  void *ptr;
} robj;

// 跳表节点
typedef struct zskiplistNode {
  unsigned int h; // 本节点高度
  // 成员对象
  robj *obj;
  // 分值
  double score;
  // 后退指针
  struct zskiplistNode *backward;
  // 层
  struct zskiplistLevel {
    // 前进指针
    struct zskiplistNode *forward;
    // 跨度 用于记录两个节点之间的距离
    unsigned int span;
  } level[];
} zskiplistNode;


// 跳表
typedef struct zskiplist {
  // 表头节点和表尾节点
  struct zskiplistNode *header, *tail;
  // 表中节点的数量
  unsigned long length;
  // 表中层数最大的节点的层数
  int level;

} zskiplist;

// 有序集合
typedef struct zset {
  // 字典
  // 用于 O(1)复杂度的按成员取分值操作
  dict *dict;
  // 跳表
  // 用于 O(longN)复杂度的按分值定位成员操作 以及 范围操作
  zskiplist *zsl;
} zset;



#define REDIS_EVICTION_POOL_SIZE 16
struct evictionPoolEntry {
    unsigned long long idle;    /* Object idle time. */
    sds key;                    /* Key name. */
};

typedef struct redisDb
{
  // 数据库键空间，保存着数据库中的所有键值对
  dict *dict; /* The keyspace for this DB */

  // 键的过期时间，字典的键为键，字典的值为过期事件 UNIX 时间戳
  dict *expires; /* Timeout of keys with a timeout set */

  // 正处于阻塞状态的键
  dict *blocking_keys; /* Keys with clients waiting for data (BLPOP) */

  // 可以解除阻塞的键
  dict *ready_keys; /* Blocked keys that received a PUSH */

  // 正在被 WATCH 命令监视的键
  dict *watched_keys; /* WATCHED keys for MULTI/EXEC CAS */

  struct evictionPoolEntry *eviction_pool; /* Eviction pool of keys */

  // 数据库号码
  int id; /* Database ID */

  // 数据库的键的平均 TTL ，统计信息
  long long avg_ttl; /* Average TTL, just for stats */
} redisDb;

typedef struct redisClient
{
  // 套接字描述符
  int fd;

  // 当前正在使用的数据库
  redisDb *db;

  // 当前正在使用的数据库的 id （号码）
  int dictid;
} redisClient;


// redis服务器
struct redisServer
{
  // 数据库
  redisDb *db;

  // 命令表（受到 rename 配置选项的作用）
  dict *commands; /* Command table */
  // 命令表（无 rename 配置选项的作用）
  dict *orig_commands; /* Command table before command renaming. */

  int dbnum; // default is 16

};

typedef void redisCommandProc(redisClient *c);
typedef int *redisGetKeysProc(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);

/*
 * Redis 命令
 */
struct redisCommand
{
  // 命令名字
  char *name;

  // 实现函数
  redisCommandProc *proc;

  // 参数个数
  int arity;

  // 字符串表示的 FLAG
  char *sflags; /* Flags as string representation, one char per flag. */

  // 实际 FLAG
  int flags; /* The actual flags, obtained from the 'sflags' field. */

  /* Use a function to determine keys arguments in a command line.
   * Used for Redis Cluster redirect. */
  // 从命令中判断命令的键参数。在 Redis 集群转向时使用。
  redisGetKeysProc *getkeys_proc;

  /* What keys should be loaded in background when calling this command? */
  // 指定哪些参数是 key
  int firstkey; /* The first argument that's a key (0 = no keys) */
  int lastkey;  /* The last argument that's a key */
  int keystep;  /* The step between first and last key */

  // 统计信息
  // microseconds 记录了命令执行耗费的总毫微秒数
  // calls 是命令被执行的总次数
  long long microseconds, calls;
};

// 开区间 闭区间
typedef struct
{
  double min,max; // 最大值 最小值

  int minex, maxex; // 是否是闭区间 1是闭区间 0是开区间
} zrangespec;

typedef struct {
  robj *min,*max; // 最大值 最小值

  int minex, maxex; // 是否是闭区间 1是闭区间 0是开区间
} zlexrangespec;


zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
zskiplistNode *zslInsert(zskiplist *zsl, double score, robj *obj);
unsigned char *zzlInsert(unsigned char *zl, robj *ele, double score);
int zslDelete(zskiplist *zsl, double score, robj *obj);
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range);
zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range);
double zzlGetScore(unsigned char *sptr);
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
unsigned int zsetLength(robj *zobj);
void zsetConvert(robj *zobj, int encoding);
unsigned long zslGetRank(zskiplist *zsl, double score, robj *o);

// Redis Object implementation
void decrRefCount(robj *obj);
void decrRefCountVoid(void *o);
int compareStringObjects(robj *a, robj *b);
int equalStringObjects(robj *a, robj *b);

/* db.c -- Keyspace access API */
int removeExpire(redisDb *db, robj *key);
void propagateExpire(redisDb *db, robj *key);
int expireIfNeeded(redisDb *db, robj *key);
long long getExpire(redisDb *db, robj *key);
void setExpire(redisDb *db, robj *key, long long when);
robj *lookupKey(redisDb *db, robj *key);
robj *lookupKeyRead(redisDb *db, robj *key);
robj *lookupKeyWrite(redisDb *db, robj *key);
robj *lookupKeyReadOrReply(redisClient *c, robj *key, robj *reply);
robj *lookupKeyWriteOrReply(redisClient *c, robj *key, robj *reply);
void dbAdd(redisDb *db, robj *key, robj *val);
void dbOverwrite(redisDb *db, robj *key, robj *val);
void setKey(redisDb *db, robj *key, robj *val);
int dbExists(redisDb *db, robj *key);
robj *dbRandomKey(redisDb *db);
int dbDelete(redisDb *db, robj *key);
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o);
long long emptyDb(void(callback)(void *));
int selectDb(redisClient *c, int id);
void signalModifiedKey(redisDb *db, robj *key);
void signalFlushedDb(int dbid);
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count);
unsigned int countKeysInSlot(unsigned int hashslot);
unsigned int delKeysInSlot(unsigned int hashslot);
int verifyClusterConfigWithData(void);
void scanGenericCommand(redisClient *c, robj *o, unsigned long cursor);
int parseScanCursorOrReply(redisClient *c, robj *o, unsigned long *cursor);

#define redisAssert(_e) ((_e)?(void)0 : (assert(_e),_exit(1)))

#endif // REDIS_H_
