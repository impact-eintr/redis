#ifndef REDIS_H_
#define REDIS_H_

#include <unistd.h>
#include <assert.h>

#include "dict.h"
#include "sds.h"
#include "adlist.h"

#define  REDIS_DEFAULT_HZ     10
#define  REDIS_DEFAULT_DBNUM  16
// 命令标志
#define  REDIS_CMD_WRITE            1     /* "w" flag  */
#define  REDIS_CMD_READONLY         2     /* "r" flag  */
#define  REDIS_CMD_DENYOOM          4     /* "m" flag  */
#define  REDIS_CMD_NOT_USED_1       8     /* no  longer used  flag  */
#define  REDIS_CMD_ADMIN            16    /* "a" flag  */
#define  REDIS_CMD_PUBSUB           32    /* "p" flag  */
#define  REDIS_CMD_NOSCRIPT         64    /* "s" flag  */
#define  REDIS_CMD_RANDOM           128   /* "R" flag  */
#define  REDIS_CMD_SORT_FOR_SCRIPT  256   /* "S" flag  */
#define  REDIS_CMD_LOADING          512   /* "l" flag  */
#define  REDIS_CMD_STALE            1024  /* "t" flag  */
#define  REDIS_CMD_SKIP_MONITOR     2048  /* "M" flag  */
#define  REDIS_CMD_ASKING           4096  /* "k" flag  */

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

#define UNIT_SECONDS 0
#define UNIT_MILLISECONDS 1

#define REDIS_LRU_BITS 24
#define REDIS_LRU_CLOCK_MAX ((1<<REDIS_LRU_BITS)-1) // Max value of obj->lru
#define REDIS_LRU_CLOCK_RESOLUTION 1000
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

#define LRU_CLOCK() ((1000/server.hz <= REDIS_LRU_CLOCK_RESOLUTION) ? server.lruclock : getLRUClock())

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

// 因为IO复用 需要为每个客户端维持一个状态
// 多个客户端状态被服务器用链表链接起来
typedef struct redisClient
{
  // 套接字描述符
  int fd;

  // 当前正在使用的数据库
  redisDb *db;

  // 当前正在使用的数据库的 id （号码）
  int dictid;

  // 参数
  int argc;
  robj **argv;

} redisClient;


// redis服务器
struct redisServer
{
  char *configfile; // 配置文件的绝对路径

  // serverCron() 每秒调用的次数
  int hz;

  // 数据库
  redisDb *db;

  // 命令表（受到 rename 配置选项的作用）
  dict *commands; /* Command table */
  // 命令表（无 rename 配置选项的作用）
  dict *orig_commands; /* Command table before command renaming. */

  // TODO 事件状态

  unsigned lruclock:REDIS_LRU_BITS;

  int dbnum; // default is 16

  list *clients; // 一个链表 保存了所有客户端状态
  list *clients_to_close; // 一个链表 保存了所有即将关闭的客户端

  redisClient *current_client; // 服务器的当前客户端 仅仅用于崩溃报告

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

/*-----------------------------------------------------------------------------
 * Extern declarations
 *----------------------------------------------------------------------------*/
extern struct redisServer server;
extern struct sharedObjectsStruct shared;
extern dictType setDictType;
extern dictType zsetDictType;
extern dictType clusterNodesDictType;
extern dictType clusterNodesBlackListDictType;
extern dictType dbDictType;
extern dictType shaScriptObjectDictType;
extern double R_Zero, R_PosInf, R_NegInf, R_Nan;
extern dictType hashDictType;
extern dictType replScriptCacheDictType;

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

// Core funtions
unsigned int getLRUClock(void);

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

/* Debugging stuff */
void _redisAssertWithInfo(redisClient *c, robj *o, char *estr, char *file,
                          int line);
void _redisAssert(char *estr, char *file, int line);
void _redisPanic(char *msg, char *file, int line);

#define redisAssertWithInfo(_c, _o, _e)                                        \
  ((_e) ? (void)0                                                              \
        : (_redisAssertWithInfo(_c, _o, #_e, __FILE__, __LINE__), _exit(1)))
#define redisAssert(_e)                                                        \
  ((_e) ? (void)0 : (_redisAssert(#_e, __FILE__, __LINE__), _exit(1)))
#define redisPanic(_e) _redisPanic(#_e, __FILE__, __LINE__), _exit(1)

//#define redisAssert(_e) ((_e)?(void)0 : (assert(_e),_exit(1)))
//#define redisPanic(_e) ((_e)?(void)0 : (assert(_e),_exit(1)))

/* Commands prototypes */
void authCommand(redisClient *c);
void pingCommand(redisClient *c);
void echoCommand(redisClient *c);
void setCommand(redisClient *c);
void setnxCommand(redisClient *c);
void setexCommand(redisClient *c);
void psetexCommand(redisClient *c);
void getCommand(redisClient *c);
void delCommand(redisClient *c);
void existsCommand(redisClient *c);
void setbitCommand(redisClient *c);
void getbitCommand(redisClient *c);
void setrangeCommand(redisClient *c);
void getrangeCommand(redisClient *c);
void incrCommand(redisClient *c);
void decrCommand(redisClient *c);
void incrbyCommand(redisClient *c);
void decrbyCommand(redisClient *c);
void incrbyfloatCommand(redisClient *c);
void selectCommand(redisClient *c);
void randomkeyCommand(redisClient *c);
void keysCommand(redisClient *c);
void scanCommand(redisClient *c);
void dbsizeCommand(redisClient *c);
void lastsaveCommand(redisClient *c);
void saveCommand(redisClient *c);
void bgsaveCommand(redisClient *c);
void bgrewriteaofCommand(redisClient *c);
void shutdownCommand(redisClient *c);
void moveCommand(redisClient *c);
void renameCommand(redisClient *c);
void renamenxCommand(redisClient *c);
void lpushCommand(redisClient *c);
void rpushCommand(redisClient *c);
void lpushxCommand(redisClient *c);
void rpushxCommand(redisClient *c);
void linsertCommand(redisClient *c);
void lpopCommand(redisClient *c);
void rpopCommand(redisClient *c);
void llenCommand(redisClient *c);
void lindexCommand(redisClient *c);
void lrangeCommand(redisClient *c);
void ltrimCommand(redisClient *c);
void typeCommand(redisClient *c);
void lsetCommand(redisClient *c);
void saddCommand(redisClient *c);
void sremCommand(redisClient *c);
void smoveCommand(redisClient *c);
void sismemberCommand(redisClient *c);
void scardCommand(redisClient *c);
void spopCommand(redisClient *c);
void srandmemberCommand(redisClient *c);
void sinterCommand(redisClient *c);
void sinterstoreCommand(redisClient *c);
void sunionCommand(redisClient *c);
void sunionstoreCommand(redisClient *c);
void sdiffCommand(redisClient *c);
void sdiffstoreCommand(redisClient *c);
void sscanCommand(redisClient *c);
void syncCommand(redisClient *c);
void flushdbCommand(redisClient *c);
void flushallCommand(redisClient *c);
void sortCommand(redisClient *c);
void lremCommand(redisClient *c);
void rpoplpushCommand(redisClient *c);
void infoCommand(redisClient *c);
void mgetCommand(redisClient *c);
void monitorCommand(redisClient *c);
void expireCommand(redisClient *c);
void expireatCommand(redisClient *c);
void pexpireCommand(redisClient *c);
void pexpireatCommand(redisClient *c);
void getsetCommand(redisClient *c);
void ttlCommand(redisClient *c);
void pttlCommand(redisClient *c);
void persistCommand(redisClient *c);
void slaveofCommand(redisClient *c);
void debugCommand(redisClient *c);
void msetCommand(redisClient *c);
void msetnxCommand(redisClient *c);
void zaddCommand(redisClient *c);
void zincrbyCommand(redisClient *c);
void zrangeCommand(redisClient *c);
void zrangebyscoreCommand(redisClient *c);
void zrevrangebyscoreCommand(redisClient *c);
void zrangebylexCommand(redisClient *c);
void zrevrangebylexCommand(redisClient *c);
void zcountCommand(redisClient *c);
void zlexcountCommand(redisClient *c);
void zrevrangeCommand(redisClient *c);
void zcardCommand(redisClient *c);
void zremCommand(redisClient *c);
void zscoreCommand(redisClient *c);
void zremrangebyscoreCommand(redisClient *c);
void zremrangebylexCommand(redisClient *c);
void multiCommand(redisClient *c);
void execCommand(redisClient *c);
void discardCommand(redisClient *c);
void blpopCommand(redisClient *c);
void brpopCommand(redisClient *c);
void brpoplpushCommand(redisClient *c);
void appendCommand(redisClient *c);
void strlenCommand(redisClient *c);
void zrankCommand(redisClient *c);
void zrevrankCommand(redisClient *c);
void hsetCommand(redisClient *c);
void hsetnxCommand(redisClient *c);
void hgetCommand(redisClient *c);
void hmsetCommand(redisClient *c);
void hmgetCommand(redisClient *c);
void hdelCommand(redisClient *c);
void hlenCommand(redisClient *c);
void zremrangebyrankCommand(redisClient *c);
void zunionstoreCommand(redisClient *c);
void zinterstoreCommand(redisClient *c);
void zscanCommand(redisClient *c);
void hkeysCommand(redisClient *c);
void hvalsCommand(redisClient *c);
void hgetallCommand(redisClient *c);
void hexistsCommand(redisClient *c);
void hscanCommand(redisClient *c);
void configCommand(redisClient *c);
void hincrbyCommand(redisClient *c);
void hincrbyfloatCommand(redisClient *c);
void subscribeCommand(redisClient *c);
void unsubscribeCommand(redisClient *c);
void psubscribeCommand(redisClient *c);
void punsubscribeCommand(redisClient *c);
void publishCommand(redisClient *c);
void pubsubCommand(redisClient *c);
void watchCommand(redisClient *c);
void unwatchCommand(redisClient *c);
void clusterCommand(redisClient *c);
void restoreCommand(redisClient *c);
void migrateCommand(redisClient *c);
void askingCommand(redisClient *c);
void readonlyCommand(redisClient *c);
void readwriteCommand(redisClient *c);
void dumpCommand(redisClient *c);
void objectCommand(redisClient *c);
void clientCommand(redisClient *c);
void evalCommand(redisClient *c);
void evalShaCommand(redisClient *c);
void scriptCommand(redisClient *c);
void timeCommand(redisClient *c);
void bitopCommand(redisClient *c);
void bitcountCommand(redisClient *c);
void bitposCommand(redisClient *c);
void replconfCommand(redisClient *c);
void waitCommand(redisClient *c);
void pfselftestCommand(redisClient *c);
void pfaddCommand(redisClient *c);
void pfcountCommand(redisClient *c);
void pfmergeCommand(redisClient *c);
void pfdebugCommand(redisClient *c);

/* Redis object implementation */
void decrRefCount(robj *o);
void decrRefCountVoid(void *o);
void incrRefCount(robj *o);
robj *resetRefCount(robj *obj);
void freeStringObject(robj *o);
void freeListObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeHashObject(robj *o);
robj *createObject(int type, void *ptr);
robj *createStringObject(char *ptr, size_t len);
robj *createRawStringObject(char *ptr, size_t len);
robj *createEmbeddedStringObject(char *ptr, size_t len);
robj *dupStringObject(robj *o);
int isObjectRepresentableAsLongLong(robj *o, long long *llongval);
robj *tryObjectEncoding(robj *o);
robj *getDecodedObject(robj *o);
size_t stringObjectLen(robj *o);
robj *createStringObjectFromLongLong(long long value);
robj *createStringObjectFromLongDouble(long double value);
robj *createListObject(void);
robj *createZiplistObject(void);
robj *createSetObject(void);
robj *createIntsetObject(void);
robj *createHashObject(void);
robj *createZsetObject(void);
robj *createZsetZiplistObject(void);
int getLongFromObjectOrReply(redisClient *c, robj *o, long *target,
                             const char *msg);
int checkType(redisClient *c, robj *o, int type);
int getLongLongFromObjectOrReply(redisClient *c, robj *o, long long *target,
                                 const char *msg);
int getDoubleFromObjectOrReply(redisClient *c, robj *o, double *target,
                               const char *msg);
int getLongLongFromObject(robj *o, long long *target);
int getLongDoubleFromObject(robj *o, long double *target);
int getLongDoubleFromObjectOrReply(redisClient *c, robj *o, long double *target,
                                   const char *msg);
char *strEncoding(int encoding);
int compareStringObjects(robj *a, robj *b);
int collateStringObjects(robj *a, robj *b);
int equalStringObjects(robj *a, robj *b);
unsigned long long estimateObjectIdleTime(robj *o);
#define sdsEncodedObject(objptr)                                               \
  (objptr->encoding == REDIS_ENCODING_RAW ||                                   \
   objptr->encoding == REDIS_ENCODING_EMBSTR)

#endif // REDIS_H_
