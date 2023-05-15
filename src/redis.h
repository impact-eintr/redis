#ifndef REDIS_H_
#define REDIS_H_

#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <pthread.h>
#include <netinet/in.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <string.h>

#include "ae.h"
#include "adlist.h"
#include "anet.h"
#include "dict.h"
#include "sds.h"

#define REDIS_OK 0
#define REDIS_ERR -1

/* 默认的服务器配置值 */
#define REDIS_DEFAULT_HZ 10 // Time interrupt calls/sec.
#define REDIS_MIN_HZ 1
#define REDIS_MAX_HZ 500
#define REDIS_SERVERPORT 6379 // TCP port
#define REDIS_TCP_BACKLOG 511 // TCP listen backlog
#define REDIS_MAXIDLETIME 0   // default client timeout: infinite
#define REDIS_DEFAULT_DBNUM 16
#define REDIS_CONFIGLINE_MAX 1024
#define REDIS_DBCRON_DBS_PER_CALL 16
#define REDIS_MAX_WRITE_PER_EVENT (1024 * 64)
#define REDIS_SHARED_SELECT_CMDS 10
#define REDIS_SHARED_INTEGERS 10000
#define REDIS_SHARED_BULKHDR_LEN 32
#define REDIS_MAX_LOGMSG_LEN     1024 // Default maximum length of syslog messages
#define REDIS_AOF_REWRITE_PERC 100
#define REDIS_AOF_REWRITE_MIN_SIZE (64 * 1024 * 1024)
#define REDIS_AOF_REWRITE_ITEMS_PER_CMD 64
#define REDIS_SLOWLOG_LOG_SLOWER_THAN 10000
#define REDIS_SLOWLOG_MAX_LEN 128
#define REDIS_MAX_CLIENTS 10000
#define REDIS_AUTHPASS_MAX_LEN 512
#define REDIS_DEFAULT_SLAVE_PRIORITY 100
#define REDIS_REPL_TIMEOUT 60
#define REDIS_REPL_PING_SLAVE_PERIOD 10
#define REDIS_RUN_ID_SIZE 40
#define REDIS_OPS_SEC_SAMPLES 16
#define REDIS_DEFAULT_REPL_BACKLOG_SIZE (1024 * 1024)   // 1mb
#define REDIS_DEFAULT_REPL_BACKLOG_TIME_LIMIT (60 * 60) // 1 hour
#define REDIS_REPL_BACKLOG_MIN_SIZE (1024 * 16)         // 16k
#define REDIS_BGSAVE_RETRY_DELAY 5 // Wait a few secs before trying again.
#define REDIS_DEFAULT_PID_FILE "/var/run/redis.pid"
#define REDIS_DEFAULT_SYSLOG_IDENT "redis"
#define REDIS_DEFAULT_CLUSTER_CONFIG_FILE "nodes.conf"
#define REDIS_DEFAULT_DAEMONIZE 0
#define REDIS_DEFAULT_UNIX_SOCKET_PERM 0
#define REDIS_DEFAULT_TCP_KEEPALIVE 0
#define REDIS_DEFAULT_LOGFILE ""
#define REDIS_DEFAULT_SYSLOG_ENABLED 0
#define REDIS_DEFAULT_STOP_WRITES_ON_BGSAVE_ERROR 1
#define REDIS_DEFAULT_RDB_COMPRESSION 1
#define REDIS_DEFAULT_RDB_CHECKSUM 1
#define REDIS_DEFAULT_RDB_FILENAME "dump.rdb"
#define REDIS_DEFAULT_SLAVE_SERVE_STALE_DATA 1
#define REDIS_DEFAULT_SLAVE_READ_ONLY 1
#define REDIS_DEFAULT_REPL_DISABLE_TCP_NODELAY 0
#define REDIS_DEFAULT_MAXMEMORY 0
#define REDIS_DEFAULT_MAXMEMORY_SAMPLES 5
#define REDIS_DEFAULT_AOF_FILENAME "appendonly.aof"
#define REDIS_DEFAULT_AOF_NO_FSYNC_ON_REWRITE 0
#define REDIS_DEFAULT_ACTIVE_REHASHING 1
#define REDIS_DEFAULT_AOF_REWRITE_INCREMENTAL_FSYNC 1
#define REDIS_DEFAULT_MIN_SLAVES_TO_WRITE 0
#define REDIS_DEFAULT_MIN_SLAVES_MAX_LAG 10
#define REDIS_IP_STR_LEN INET6_ADDRSTRLEN
#define REDIS_PEER_ID_LEN                                                      \
  (REDIS_IP_STR_LEN + 32) // Must be enough for ip:port
#define REDIS_BINDADDR_MAX 16
#define REDIS_MIN_RESERVED_FDS 32

#define ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP 20 /* Loopkups per loop. */
#define ACTIVE_EXPIRE_CYCLE_FAST_DURATION 1000  /* Microseconds */
#define ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC                                     \
  25 /* CPU max % for keys collection */
#define ACTIVE_EXPIRE_CYCLE_SLOW 0
#define ACTIVE_EXPIRE_CYCLE_FAST 1

/* Protocol and I/O related defines */
#define REDIS_MAX_QUERYBUF_LEN (1024 * 1024 * 1024) /* 1GB max query buffer.   \
                                                     */
#define REDIS_IOBUF_LEN (1024 * 16)         /* Generic I/O buffer size */
#define REDIS_REPLY_CHUNK_BYTES (16 * 1024) /* 16k output buffer */
#define REDIS_INLINE_MAX_SIZE (1024 * 64)   /* Max size of inline reads */
#define REDIS_MBULK_BIG_ARG (1024 * 32)
#define REDIS_LONGSTR_SIZE 21 /* Bytes needed for long -> str */

// 指示 AOF 程序每累积这个量的写入数据
// 就执行一次显式的 fsync
#define REDIS_AOF_AUTOSYNC_BYTES (1024 * 1024 * 32) /* fdatasync every 32MB */
/* When configuring the Redis eventloop, we setup it so that the total number
 * of file descriptors we can handle are server.maxclients + RESERVED_FDS +
 * FDSET_INCR that is our safety margin. */
#define REDIS_EVENTLOOP_FDSET_INCR (REDIS_MIN_RESERVED_FDS + 96)

/* Hash table parameters */
#define REDIS_HT_MINFILL 10 /* Minimal hash table fill 10% */

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



/* Client flags */
#define REDIS_SLAVE (1<<0)   /* This client is a slave server */
#define REDIS_MASTER (1<<1)  /* This client is a master server */
#define REDIS_MONITOR (1<<2) /* This client is a slave monitor, see MONITOR */
#define REDIS_MULTI (1<<3)   /* This client is in a MULTI context */
#define REDIS_BLOCKED (1<<4) /* The client is waiting in a blocking operation */
#define REDIS_DIRTY_CAS (1<<5) /* Watched keys modified. EXEC will fail. */
#define REDIS_CLOSE_AFTER_REPLY (1<<6) /* Close after writing entire reply. */
#define REDIS_UNBLOCKED (1<<7) /* This client was unblocked and is stored in
                                  server.unblocked_clients */
#define REDIS_LUA_CLIENT (1<<8) /* This is a non connected client used by Lua */
#define REDIS_ASKING (1<<9)     /* Client issued the ASKING command */
#define REDIS_CLOSE_ASAP (1<<10)/* Close this client ASAP */
#define REDIS_UNIX_SOCKET (1<<11) /* Client connected via Unix domain socket */
#define REDIS_DIRTY_EXEC (1<<12)  /* EXEC will fail for errors while queueing */
#define REDIS_MASTER_FORCE_REPLY (1<<13)  /* Queue replies even if is master */
#define REDIS_FORCE_AOF (1<<14)   /* Force AOF propagation of current cmd. */
#define REDIS_FORCE_REPL (1<<15)  /* Force replication of current cmd. */
#define REDIS_PRE_PSYNC (1<<16)   /* Instance don't understand PSYNC. */
#define REDIS_READONLY (1<<17)    /* Cluster client is in read-only state. */


/* Client request types */
#define REDIS_REQ_INLINE 1
#define REDIS_REQ_MULTIBULK 2


/* List related stuff */
#define REDIS_HEAD 0
#define REDIS_TAIL 1

/* Log levels */
#define REDIS_DEBUG 0
#define REDIS_VERBOSE 1
#define REDIS_NOTICE 2
#define REDIS_WARNING 3
#define REDIS_LOG_RAW (1<<10) /* Modifier to log without timestamp */
#define REDIS_DEFAULT_VERBOSITY REDIS_NOTICE


#define REDIS_NOTUSED(v) ((void) v);

#define ZSKIPLIST_MAXLEVEL 32
#define ZSKIPLIST_P 0.25

// Zip Structrue
#define REDIS_HASH_MAX_ZIPLIST_ENTRIES 512
#define REDIS_HASH_MAX_ZIPLIST_VALUE 64
#define REDIS_LIST_MAX_ZIPLIST_ENTRIES 512
#define REDIS_LIST_MAX_ZIPLIST_VALUE 3
#define REDIS_SET_MAX_INTSET_ENTRIES 512
#define REDIS_ZSET_MAX_ZIPLIST_ENTRIES 128
#define REDIS_ZSET_MAX_ZIPLIST_VALUE 64

/* HyperLogLog defines */
#define REDIS_DEFAULT_HLL_SPARSE_MAX_BYTES 3000

#define UNIT_SECONDS 0
#define UNIT_MILLISECONDS 1


// Command call flags see call() function
#define REDIS_CALL_NONE 0
#define REDIS_CALL_SLOWLOG 1
#define REDIS_CALL_STATS 2
#define REDIS_CALL_PROPAGATE 4
#define REDIS_CALL_FULL                                                        \
  (REDIS_CALL_SLOWLOG | REDIS_CALL_STATS | REDIS_CALL_PROPAGATE)

#define REDIS_LRU_BITS 24
#define REDIS_LRU_CLOCK_MAX ((1<<REDIS_LRU_BITS)-1) // Max value of obj->lru
#define REDIS_LRU_CLOCK_RESOLUTION 1000

#define redisAssertWithInfo(_c, _o, _e)                                        \
  ((_e) ? (void)0                                                              \
        : (_redisAssertWithInfo(_c, _o, #_e, __FILE__, __LINE__), _exit(1)))
#define redisAssert(_e)                                                        \
  ((_e) ? (void)0 : (_redisAssert(#_e, __FILE__, __LINE__), _exit(1)))
#define redisPanic(_e) _redisPanic(#_e, __FILE__, __LINE__), _exit(1)

typedef long long mstime_t;


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

  robj *name; // 客户端名称

  sds querybuf; // 查询缓冲区
  size_t querybuf_peak; // 查询缓冲区长度峰值

  // 参数
  int argc;
  robj **argv;

  // 记录被客户端执行的命令
  struct redisCommand *cmd, *lastcmd;

  // 请求的类型：内联命令还是多条命令
  int reqtype;

  // 剩余未读取的命令内容数量
  int multibulklen; /* number of multi bulk arguments left to read */

  // 命令内容的长度
  long bulklen; /* length of bulk argument in multi bulk request */

  // 回复链表
  list *reply;

  // 回复链表中对象的总大小
  unsigned long reply_bytes; /* Tot bytes of objects in reply list */

  // 已发送字节，处理 short write 用
  int sentlen; /* Amount of bytes already sent in the current
                  buffer or object being sent. */

  // 创建客户端的时间
  time_t ctime; /* Client creation time */

  // 客户端最后一次和服务器互动的时间
  time_t lastinteraction; /* time of the last interaction, used for timeout */

  // 客户端的输出缓冲区超过软性限制的时间
  time_t obuf_soft_limit_reached_time;

  // 客户端状态标志
  int flags; /* REDIS_SLAVE | REDIS_MONITOR | REDIS_MULTI ... */

  // TODO

  // 回复缓冲区
  int bufpos;
  char buf[REDIS_REPLY_CHUNK_BYTES];
} redisClient;


struct sharedObjectsStruct {
  robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *cnegone, *pong, *space,
      *colon, *nullbulk, *nullmultibulk, *queued, *emptymultibulk,
      *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr, *outofrangeerr,
      *noscripterr, *loadingerr, *slowscripterr, *bgsaveerr, *masterdownerr,
      *roslaveerr, *execaborterr, *noautherr, *noreplicaserr, *busykeyerr,
      *oomerr, *plus, *messagebulk, *pmessagebulk, *subscribebulk,
      *unsubscribebulk, *psubscribebulk, *punsubscribebulk, *del, *rpop, *lpop,
      *lpush, *emptyscan, *minstring, *maxstring,
      *select[REDIS_SHARED_SELECT_CMDS], *integers[REDIS_SHARED_INTEGERS],
      *mbulkhdr[REDIS_SHARED_BULKHDR_LEN], /* "*<value>\r\n" */
      *bulkhdr[REDIS_SHARED_BULKHDR_LEN];  /* "$<value>\r\n" */
};

/* The redisOp structure defines a Redis Operation, that is an instance of
 * a command with an argument vector, database ID, propagation target
 * (REDIS_PROPAGATE_*), and command pointer.
 *
 * redisOp 结构定义了一个 Redis 操作，
 * 它包含指向被执行命令的指针、命令的参数、数据库 ID
 * 、传播目标（REDIS_PROPAGATE_*）。
 *
 * Currently only used to additionally propagate more commands to
 * AOF/Replication after the propagation of the executed command.
 *
 * 目前只用于在传播被执行命令之后，传播附加的其他命令到 AOF 或 Replication 中。
 */
typedef struct redisOp {
  // 参数
  robj **argv;
  // 参数数量、数据库 ID 、传播目标
  int argc, dbid, target;
  // 被执行命令的指针
  struct redisCommand *cmd;
} redisOp;

/* Defines an array of Redis operations. There is an API to add to this
 * structure in a easy way.
 *
 * redisOpArrayInit();
 * redisOpArrayAppend();
 * redisOpArrayFree();
 */
typedef struct redisOpArray {
  redisOp *ops;
  int numops;
} redisOpArray;

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

  // 事件状态
  aeEventLoop *el;

  // 最近一次使用时钟
  unsigned lruclock:REDIS_LRU_BITS;

  // 关闭服务器
  int shutdown_asap;

  int activerehashing; // databaseCron进行渐进式Rehash

  char *pidfile; // pid文件

  int sentinel_mode; // 服务是否运行在哨兵模式

  /* Networking */

  // TCP 监听端口
  int port; /* TCP listening port */

  int tcp_backlog; /* TCP listen() backlog */

  // 地址
  char *bindaddr[REDIS_BINDADDR_MAX]; /* Addresses we should bind to */
  // 地址数量
  int bindaddr_count; /* Number of addresses in server.bindaddr[] */

  // UNIX 套接字
  char *unixsocket;      /* UNIX socket path */
  mode_t unixsocketperm; /* UNIX socket permission */

  // 描述符
  int ipfd[REDIS_BINDADDR_MAX]; /* TCP socket file descriptors */
  // 描述符数量
  int ipfd_count; /* Used slots in ipfd[] */

  // UNIX 套接字文件描述符
  int sofd; /* Unix socket file descriptor */

  int cfd[REDIS_BINDADDR_MAX]; /* Cluster bus listening socket */
  int cfd_count;               /* Used slots in cfd[] */

  // 一个链表，保存了所有客户端状态结构
  list *clients; /* List of active clients */
  // 链表，保存了所有待关闭的客户端
  list *clients_to_close; /* Clients to close asynchronously */

  // 链表，保存了所有从服务器，以及所有监视器
  list *slaves, *monitors; /* List of slaves and MONITORs */

  // 服务器的当前客户端，仅用于崩溃报告
  redisClient *current_client; /* Current client, only used on crash report */

  int clients_paused;              /* True if clients are currently paused */
  mstime_t clients_pause_end_time; /* Time when we undo clients_paused */

  // 网络错误
  char neterr[ANET_ERR_LEN]; /* Error buffer for anet.c */

  // MIGRATE 缓存
  dict *migrate_cached_sockets; /* MIGRATE cached sockets */

  /* Fields used only for stats */

  // 服务器启动时间
  time_t stat_starttime;          /* Server start time */

  // 已处理命令的数量
  long long stat_numcommands;     /* Number of processed commands */

  // 服务器接到的连接请求数量
  long long stat_numconnections;  /* Number of connections received */

  // 已过期的键数量
  long long stat_expiredkeys;     /* Number of expired keys */

  // 因为回收内存而被释放的过期键的数量
  long long stat_evictedkeys;     /* Number of evicted keys (maxmemory) */

  // 成功查找键的次数
  long long stat_keyspace_hits;   /* Number of successful lookups of keys */

  // 查找键失败的次数
  long long stat_keyspace_misses; /* Number of failed lookups of keys */

  // 已使用内存峰值
  size_t stat_peak_memory;        /* Max used memory record */

  // 最后一次执行 fork() 时消耗的时间
  long long stat_fork_time;       /* Time needed to perform latest fork() */

  // 服务器因为客户端数量过多而拒绝客户端连接的次数
  long long stat_rejected_conn;   /* Clients rejected because of maxclients */

  // 执行 full sync 的次数
  long long stat_sync_full;       /* Number of full resyncs with slaves. */

  // PSYNC 成功执行的次数
  long long stat_sync_partial_ok; /* Number of accepted PSYNC requests. */

  // PSYNC 执行失败的次数
  long long stat_sync_partial_err;/* Number of unaccepted PSYNC requests. */

  /* 配置字段 */

  // 日志可见性
  int verbosity; /* Loglevel in redis.conf */

  // 客户端最大空转时间
  int maxidletime; /* Client timeout in seconds */

  // 是否开启 SO_KEEPALIVE 选项
  int tcpkeepalive;               /* Set SO_KEEPALIVE if non-zero. */
  int active_expire_enabled;      /* Can be disabled for testing purposes. */
  size_t client_max_querybuf_len; /* Limit for client query buffer length */
  int dbnum;                      /* Total number of configured DBs */
  int daemonize;                  /* True if running as a daemon */
  // 客户端输出缓冲区大小限制
  // 数组的元素有 REDIS_CLIENT_LIMIT_NUM_CLASSES 个
  // 每个代表一类客户端：普通、从服务器、pubsub，诸如此类
  // TODO clientBufferLimitsConfig client_obuf_limits[REDIS_CLIENT_LIMIT_NUM_CLASSES];

  // AOF persistence
  int aof_state;                  /* REDIS_AOF_(ON|OFF|WAIT_REWRITE) */

  // 所使用的 fsync 策略（每个写入/每秒/从不）
  int aof_fsync;                  /* Kind of fsync() policy */
  char *aof_filename;             /* Name of the AOF file */
  int aof_no_fsync_on_rewrite;    /* Don't fsync if a rewrite is in prog. */
  int aof_rewrite_perc;           /* Rewrite AOF if % growth is > M and... */
  off_t aof_rewrite_min_size;     /* the AOF file is at least N bytes. */

  // 最后一次执行 BGREWRITEAOF 时， AOF 文件的大小
  off_t aof_rewrite_base_size;    /* AOF size on latest startup or rewrite. */

  // AOF 文件的当前字节大小
  off_t aof_current_size;         /* AOF current size. */
  int aof_rewrite_scheduled;      /* Rewrite once BGSAVE terminates. */

  // 负责进行 AOF 重写的子进程 ID
  pid_t aof_child_pid;            /* PID if rewriting process */

  // AOF 重写缓存链表，链接着多个缓存块
  list *aof_rewrite_buf_blocks;   /* Hold changes during an AOF rewrite. */

  // AOF 缓冲区
  sds aof_buf;      /* AOF buffer, written before entering the event loop */

  // AOF 文件的描述符
  int aof_fd;       /* File descriptor of currently selected AOF file */

  // AOF 的当前目标数据库
  int aof_selected_db; /* Currently selected DB in AOF */

  // 推迟 write 操作的时间
  time_t aof_flush_postponed_start; /* UNIX time of postponed AOF flush */

  // 最后一直执行 fsync 的时间
  time_t aof_last_fsync;            /* UNIX time of last fsync() */
  time_t aof_rewrite_time_last;   /* Time used by last AOF rewrite run. */

  // AOF 重写的开始时间
  time_t aof_rewrite_time_start;  /* Current AOF rewrite start time. */

  // 最后一次执行 BGREWRITEAOF 的结果
  int aof_lastbgrewrite_status;   /* REDIS_OK or REDIS_ERR */

  // 记录 AOF 的 write 操作被推迟了多少次
  unsigned long aof_delayed_fsync;  /* delayed AOF fsync() counter */

  // 指示是否需要每写入一定量的数据，就主动执行一次 fsync()
  int aof_rewrite_incremental_fsync;/* fsync incrementally while rewriting? */
  int aof_last_write_status;      /* REDIS_OK or REDIS_ERR */
  int aof_last_write_errno;       /* Valid if aof_last_write_status is ERR */

/* RDB persistence */

  // 自从上次 SAVE 执行以来，数据库被修改的次数
  long long dirty;                /* Changes to DB from the last save */

  // BGSAVE 执行前的数据库被修改次数
  long long dirty_before_bgsave;  /* Used to restore dirty on failed BGSAVE */

  // 负责执行 BGSAVE 的子进程的 ID
  // 没在执行 BGSAVE 时，设为 -1
  pid_t rdb_child_pid;            /* PID of RDB saving child */
  struct saveparam *saveparams;   /* Save points array for RDB */
  int saveparamslen;              /* Number of saving points */
  char *rdb_filename;             /* Name of RDB file */
  int rdb_compression;            /* Use compression in RDB? */
  int rdb_checksum;               /* Use RDB checksum? */

  // 最后一次完成 SAVE 的时间
  time_t lastsave;                /* Unix time of last successful save */

  // 最后一次尝试执行 BGSAVE 的时间
  time_t lastbgsave_try;          /* Unix time of last attempted bgsave */

  // 最近一次 BGSAVE 执行耗费的时间
  time_t rdb_save_time_last;      /* Time used by last RDB save run. */

  // 数据库最近一次开始执行 BGSAVE 的时间
  time_t rdb_save_time_start;     /* Current RDB save start time. */

  // 最后一次执行 SAVE 的状态
  int lastbgsave_status;          /* REDIS_OK or REDIS_ERR */
  int stop_writes_on_bgsave_err;  /* Don't allow writes if can't BGSAVE */

  /* Propagation of commands in AOF / replication */
  redisOpArray also_propagate; /* Additional command to propagate. */

  // Limits
  int maxclients;
  unsigned long long maxmemory;
  int maxmemory_policy;  /* Policy for key eviction */
  int maxmemory_samples; /* Pricision of random sampling */

  // Zip structure config serr redis.conf for more information
  size_t hash_max_ziplist_entries;
  size_t hash_max_ziplist_value;
  size_t list_max_ziplist_entries;
  size_t list_max_ziplist_value;
  size_t set_max_intset_entries;
  size_t zset_max_ziplist_entries;
  size_t zset_max_ziplist_value;
  size_t hll_sparse_max_bytes;
  time_t unixtime;
  long long mstime;

  /*  CLUSTER */
  int cluster_enabled;


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

  // 参数个数 > 0 固定 < 0 至少
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

/* Utils */
long long ustime(void);
long long mstime(void);
void getRandomHexChars(char *p, unsigned int len);
uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);
void exitFromChild(int retcode);
size_t redisPopcount(void *s, long count);
void redisSetProcTitle(char *title);


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
int freeMemoryIfNeeded(void);
int processCommand(redisClient *c);
void setupSignalHandlers(void);
struct redisCommand *lookupCommand(sds name);
struct redisCommand *lookupCommandByCString(char *s);
struct redisCommand *lookupCommandOrOriginal(sds name);
void call(redisClient *c, int flags);
void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int flags);
void alsoPropagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int target);
void forceCommandPropagation(redisClient *c, int flags);
int prepareForShutdown();
#ifdef __GNUC__
void redisLog(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void redisLog(int level, const char *fmt, ...);
#endif
void redisLogRaw(int level, const char *msg);
void redisLogFromHandler(int level, const char *msg);
void usage();
void updateDictResizePolicy(void);
int htNeedsResize(dict *dict);
void oom(const char *msg);
void populateCommandTable(void);
void resetCommandTableStats(void);
void adjustOpenFilesLimit(void);
void closeListeningSockets(int unlink_unix_socket);
void updateCachedTime(void);
void resetServerStats(void);
unsigned int getLRUClock(void);

// 列表迭代器对象
typedef struct {
  robj *subject; // 列表对象
  unsigned char encoding; // 对象使用的编码
  // 迭代的方向
  unsigned char direction;
  unsigned char *zi; // ziplist 索引 迭代ziplist的list使用
  // 链表节点的指针 迭代Linkedlist编码的list使用
  listNode *ln;
} listTypeIterator;


// 迭代列表时使用的记录结构
typedef struct {
  // 列表迭代器
  listTypeIterator *li;

  unsigned char *zi; // ziplist 索引 迭代ziplist的list使用
  listNode *ln;      // 链表节点的指针 迭代Linkedlist编码的list使用
} listTypeEntry;

// 抽象set迭代器
typedef struct {
  robj *subject;

  int encoding; // 对象编码
  int ii; // intset 的索引
  // 字典迭代器 编码为 HT 时使用
  dictIterator *di;
} setTypeIterator;

// hash 迭代器
typedef struct {
  robj *subject;

  int encoding;

  unsigned char *fptr, *vptr; // 在迭代 ZIPLIST 编码的hash对象时使用

  dictIterator *di;
  dictEntry *de;
} hashTypeIterator;

#define REDIS_HASH_KEY 1
#define REDIS_HASH_VALUE 2

/* List data type  */
void listTypeTryConversion(robj *subject, robj *value);
void listTypePush(robj *subject, robj *value, int where);
robj *listTypePop(robj *subject, int where);
unsigned long listTypeLength(robj *subject);
listTypeIterator *listTypeInitIterator(robj *subject, long index,
                                       unsigned char direction);
void listTypeReleaseIterator(listTypeIterator *li);
int listTypeNext(listTypeIterator *li, listTypeEntry *entry);
robj *listTypeGet(listTypeEntry *entry);
void listTypeInsert(listTypeEntry *entry, robj *value, int where);
int listTypeEqual(listTypeEntry *entry, robj *o);
void listTypeDelete(listTypeEntry *entry);
void listTypeConvert(robj *subject, int enc);
void unblockClientWaitingData(redisClient *c);
void handleClientsBlockedOnLists(void);
void popGenericCommand(redisClient *c, int where);

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
// 1 没有通过检查 0 通过检查
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

/* networking.c -- Networking and Client related operations */
redisClient *createClient(int fd);
void closeTimedoutClients(void);
void freeClient(redisClient *c);
void freeClientAsync(redisClient *c);
void resetClient(redisClient *c);
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask);
void addReply(redisClient *c, robj *obj);
void *addDeferredMultiBulkLength(redisClient *c);
void setDeferredMultiBulkLength(redisClient *c, void *node, long length);
void addReplySds(redisClient *c, sds s);
void processInputBuffer(redisClient *c);
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask);
void addReplyBulk(redisClient *c, robj *obj);
void addReplyBulkCString(redisClient *c, char *s);
void addReplyBulkCBuffer(redisClient *c, void *p, size_t len);
void addReplyBulkLongLong(redisClient *c, long long ll);
void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void addReply(redisClient *c, robj *obj);
void addReplySds(redisClient *c, sds s);
void addReplyError(redisClient *c, char *err);
void addReplyStatus(redisClient *c, char *status);
void addReplyDouble(redisClient *c, double d);
void addReplyLongLong(redisClient *c, long long ll);
void addReplyMultiBulkLen(redisClient *c, long length);
void copyClientOutputBuffer(redisClient *dst, redisClient *src);
void *dupClientReplyValue(void *o);
void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer);
void formatPeerId(char *peerid, size_t peerid_len, char *ip, int port);
char *getClientPeerId(redisClient *client);
sds catClientInfoString(sds s, redisClient *client);
sds getAllClientsInfoString(void);
void rewriteClientCommandVector(redisClient *c, int argc, ...);
void rewriteClientCommandArgument(redisClient *c, int i, robj *newval);
unsigned long getClientOutputBufferMemoryUsage(redisClient *c);
void freeClientsInAsyncFreeQueue(void);
void asyncCloseClientOnOutputBufferLimitReached(redisClient *c);
int getClientLimitClassByName(char *name);
char *getClientLimitClassName(int class);
void flushSlavesOutputBuffers(void);
void disconnectSlaves(void);
int listenToPort(int port, int *fds, int *count);
void pauseClients(mstime_t duration);
int clientsArePaused(void);
int processEventsWhileBlocked(void);

#ifdef __GNUC__
void addReplyErrorFormat(redisClient *c, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));
void addReplyStatusFormat(redisClient *c, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));
#else
void addReplyErrorFormat(redisClient *c, const char *fmt, ...);
void addReplyStatusFormat(redisClient *c, const char *fmt, ...);
#endif

/* Debugging stuff */
void _redisAssertWithInfo(redisClient *c, robj *o, char *estr, char *file,
                          int line);
void _redisAssert(char *estr, char *file, int line);
void _redisPanic(char *msg, char *file, int line);



#endif // REDIS_H_
