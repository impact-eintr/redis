#include "redis.h"
#include "version.h"
#include "adlist.h"
#include "ae.h"
#include "anet.h"
#include "color.h"
#include "dict.h"
#include "util.h"
#include "zmalloc.h"

#include <string.h>
#include <stdio.h>
#include <sys/syslog.h>
#include <sys/time.h>

struct sharedObjectsStruct shared;

// Global vars
struct redisServer server;
struct redisCommand *commandTable;

/* Our command table.
 *
 * 命令表
 *
 * Every entry is composed of the following fields:
 *
 * 表中的每个项都由以下域组成：
 *
 * name: a string representing the command name.
 *       命令的名字
 *
 * function: pointer to the C function implementing the command.
 *           一个指向命令的实现函数的指针
 *
 * arity: number of arguments, it is possible to use -N to say >= N
 *        参数的数量。可以用 -N 表示 >= N
 *
 * sflags: command flags as string. See below for a table of flags.
 *         字符串形式的 FLAG ，用来计算以下的真实 FLAG
 *
 * flags: flags as bitmask. Computed by Redis using the 'sflags' field.
 *        位掩码形式的 FLAG ，根据 sflags 的字符串计算得出
 *
 * get_keys_proc: an optional function to get key arguments from a command.
 *                This is only used when the following three fields are not
 *                enough to specify what arguments are keys.
 *                一个可选的函数，用于从命令中取出 key 参数，仅在以下三个参数都不足以表示 key 参数时使用
 *
 * first_key_index: first argument that is a key
 *                  第一个 key 参数的位置
 *
 * last_key_index: last argument that is a key
 *                 最后一个 key 参数的位置
 *
 * key_step: step to get all the keys from first to last argument. For instance
 *           in MSET the step is two since arguments are key,val,key,val,...
 *           从 first 参数和 last 参数之间，所有 key 的步数（step）
 *           比如说， MSET 命令的格式为 MSET key value [key value ...]
 *           它的 step 就为 2
 *
 * microseconds: microseconds of total execution time for this command.
 *               执行这个命令耗费的总微秒数
 *
 * calls: total number of calls of this command.
 *        命令被执行的总次数
 *
 * The flags, microseconds and calls fields are computed by Redis and should
 * always be set to zero.
 *
 * microseconds 和 call 由 Redis 计算，总是初始化为 0 。
 *
 * Command flags are expressed using strings where every character represents
 * a flag. Later the populateCommandTable() function will take care of
 * populating the real 'flags' field using this characters.
 *
 * 命令的 FLAG 首先由 SFLAG 域设置，之后 populateCommandTable() 函数从 sflags 属性中计算出真正的 FLAG 到 flags 属性中。
 *
 * This is the meaning of the flags:
 *
 * 以下是各个 FLAG 的意义：
 *
 * w: write command (may modify the key space).
 *    写入命令，可能会修改 key space
 *
 * r: read command  (will never modify the key space).
 *    读命令，不修改 key space
 * m: may increase memory usage once called. Don't allow if out of memory.
 *    可能会占用大量内存的命令，调用时对内存占用进行检查
 *
 * a: admin command, like SAVE or SHUTDOWN.
 *    管理用途的命令，比如 SAVE 和 SHUTDOWN
 *
 * p: Pub/Sub related command.
 *    发布/订阅相关的命令
 *
 * f: force replication of this command, regardless of server.dirty.
 *    无视 server.dirty ，强制复制这个命令。
 *
 * s: command not allowed in scripts.
 *    不允许在脚本中使用的命令
 *
 * R: random command. Command is not deterministic, that is, the same command
 *    with the same arguments, with the same key space, may have different
 *    results. For instance SPOP and RANDOMKEY are two random commands.
 *    随机命令。
 *    命令是非确定性的：对于同样的命令，同样的参数，同样的键，结果可能不同。
 *    比如 SPOP 和 RANDOMKEY 就是这样的例子。
 *
 * S: Sort command output array if called from script, so that the output
 *    is deterministic.
 *    如果命令在 Lua 脚本中执行，那么对输出进行排序，从而得出确定性的输出。
 *
 * l: Allow command while loading the database.
 *    允许在载入数据库时使用的命令。
 *
 * t: Allow command while a slave has stale data but is not allowed to
 *    server this data. Normally no command is accepted in this condition
 *    but just a few.
 *    允许在附属节点带有过期数据时执行的命令。
 *    这类命令很少有，只有几个。
 *
 * M: Do not automatically propagate the command on MONITOR.
 *    不要在 MONITOR 模式下自动广播的命令。
 *
 * k: Perform an implicit ASKING for this command, so the command will be
 *    accepted in cluster mode if the slot is marked as 'importing'.
 *    为这个命令执行一个显式的 ASKING ，
 *    使得在集群模式下，一个被标示为 importing 的槽可以接收这命令。
 */
struct redisCommand redisCommandTable[] = {
    {"get", getCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
    {"set", setCommand, -3, "wm", 0, NULL, 1, 1, 1, 0, 0},
    {"setnx", setnxCommand, 3, "wm", 0, NULL, 1, 1, 1, 0, 0},
    {"setex", setexCommand, 4, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //{"psetex", psetexCommand, 4, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //{"append", appendCommand, 3, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //{"strlen", strlenCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
    //{"del", delCommand, -2, "w", 0, NULL, 1, -1, 1, 0, 0},
    //{"exists", existsCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
    //{"setbit", setbitCommand, 4, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //{"getbit", getbitCommand, 3, "r", 0, NULL, 1, 1, 1, 0, 0},
    //{"setrange", setrangeCommand, 4, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //{"getrange", getrangeCommand, 4, "r", 0, NULL, 1, 1, 1, 0, 0},
    //{"substr", getrangeCommand, 4, "r", 0, NULL, 1, 1, 1, 0, 0},
    //{"incr", incrCommand, 2, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //{"decr", decrCommand, 2, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //{"mget", mgetCommand, -2, "r", 0, NULL, 1, -1, 1, 0, 0},
    //    {"rpush", rpushCommand, -3, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"lpush", lpushCommand, -3, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"rpushx", rpushxCommand, 3, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"lpushx", lpushxCommand, 3, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"linsert", linsertCommand, 5, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"rpop", rpopCommand, 2, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"lpop", lpopCommand, 2, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"brpop", brpopCommand, -3, "ws", 0, NULL, 1, 1, 1, 0, 0},
    //    {"brpoplpush", brpoplpushCommand, 4, "wms", 0, NULL, 1, 2, 1, 0, 0},
    //    {"blpop", blpopCommand, -3, "ws", 0, NULL, 1, -2, 1, 0, 0},
    //    {"llen", llenCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"lindex", lindexCommand, 3, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"lset", lsetCommand, 4, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"lrange", lrangeCommand, 4, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"ltrim", ltrimCommand, 4, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"lrem", lremCommand, 4, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"rpoplpush", rpoplpushCommand, 3, "wm", 0, NULL, 1, 2, 1, 0, 0},
    //    {"sadd", saddCommand, -3, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"srem", sremCommand, -3, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"smove", smoveCommand, 4, "w", 0, NULL, 1, 2, 1, 0, 0},
    //    {"sismember", sismemberCommand, 3, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"scard", scardCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"spop", spopCommand, 2, "wRs", 0, NULL, 1, 1, 1, 0, 0},
    //    {"srandmember", srandmemberCommand, -2, "rR", 0, NULL, 1, 1, 1, 0, 0},
    //    {"sinter", sinterCommand, -2, "rS", 0, NULL, 1, -1, 1, 0, 0},
    //    {"sinterstore", sinterstoreCommand, -3, "wm", 0, NULL, 1, -1, 1, 0, 0},
    //    {"sunion", sunionCommand, -2, "rS", 0, NULL, 1, -1, 1, 0, 0},
    //    {"sunionstore", sunionstoreCommand, -3, "wm", 0, NULL, 1, -1, 1, 0, 0},
    //    {"sdiff", sdiffCommand, -2, "rS", 0, NULL, 1, -1, 1, 0, 0},
    //    {"sdiffstore", sdiffstoreCommand, -3, "wm", 0, NULL, 1, -1, 1, 0, 0},
    //    {"smembers", sinterCommand, 2, "rS", 0, NULL, 1, 1, 1, 0, 0},
    //    {"sscan", sscanCommand, -3, "rR", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zadd", zaddCommand, -4, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zincrby", zincrbyCommand, 4, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zrem", zremCommand, -3, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zremrangebyscore", zremrangebyscoreCommand, 4, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zremrangebyrank", zremrangebyrankCommand, 4, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zremrangebylex", zremrangebylexCommand, 4, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zunionstore", zunionstoreCommand, -4, "wm", 0, zunionInterGetKeys,
    //    0, 0, 0, 0, 0},
    //    {"zinterstore", zinterstoreCommand, -4, "wm", 0, zunionInterGetKeys,
    //    0, 0, 0, 0, 0},
    //    {"zrange", zrangeCommand, -4, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zrangebyscore", zrangebyscoreCommand, -4, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zrevrangebyscore", zrevrangebyscoreCommand, -4, "r", 0, NULL, 1, 1,
    //    1, 0, 0},
    //    {"zrangebylex", zrangebylexCommand, -4, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zrevrangebylex", zrevrangebylexCommand, -4, "r", 0, NULL, 1, 1, 1,
    //    0, 0},
    //    {"zcount", zcountCommand, 4, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zlexcount", zlexcountCommand, 4, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zrevrange", zrevrangeCommand, -4, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zcard", zcardCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zscore", zscoreCommand, 3, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zrank", zrankCommand, 3, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zrevrank", zrevrankCommand, 3, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"zscan", zscanCommand, -3, "rR", 0, NULL, 1, 1, 1, 0, 0},
    //    {"hset", hsetCommand, 4, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"hsetnx", hsetnxCommand, 4, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"hget", hgetCommand, 3, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"hmset", hmsetCommand, -4, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"hmget", hmgetCommand, -3, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"hincrby", hincrbyCommand, 4, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"hincrbyfloat", hincrbyfloatCommand, 4, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"hdel", hdelCommand, -3, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"hlen", hlenCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"hkeys", hkeysCommand, 2, "rS", 0, NULL, 1, 1, 1, 0, 0},
    //    {"hvals", hvalsCommand, 2, "rS", 0, NULL, 1, 1, 1, 0, 0},
    //    {"hgetall", hgetallCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"hexists", hexistsCommand, 3, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"hscan", hscanCommand, -3, "rR", 0, NULL, 1, 1, 1, 0, 0},
    //{"incrby", incrbyCommand, 3, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //{"decrby", decrbyCommand, 3, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //{"incrbyfloat", incrbyfloatCommand, 3, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //{"getset", getsetCommand, 3, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //{"mset", msetCommand, -3, "wm", 0, NULL, 1, -1, 2, 0, 0},
    //{"msetnx", msetnxCommand, -3, "wm", 0, NULL, 1, -1, 2, 0, 0},
    //    {"randomkey", randomkeyCommand, 1, "rR", 0, NULL, 0, 0, 0, 0, 0},
    //    {"select", selectCommand, 2, "rl", 0, NULL, 0, 0, 0, 0, 0},
    //    {"move", moveCommand, 3, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"rename", renameCommand, 3, "w", 0, NULL, 1, 2, 1, 0, 0},
    //    {"renamenx", renamenxCommand, 3, "w", 0, NULL, 1, 2, 1, 0, 0},
    //    {"expire", expireCommand, 3, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"expireat", expireatCommand, 3, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"pexpire", pexpireCommand, 3, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"pexpireat", pexpireatCommand, 3, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"keys", keysCommand, 2, "rS", 0, NULL, 0, 0, 0, 0, 0},
    //    {"scan", scanCommand, -2, "rR", 0, NULL, 0, 0, 0, 0, 0},
    //    {"dbsize", dbsizeCommand, 1, "r", 0, NULL, 0, 0, 0, 0, 0},
    //    {"auth", authCommand, 2, "rslt", 0, NULL, 0, 0, 0, 0, 0},
    //    {"save", saveCommand, 1, "ars", 0, NULL, 0, 0, 0, 0, 0},
    //    {"bgsave", bgsaveCommand, 1, "ar", 0, NULL, 0, 0, 0, 0, 0},
    //    {"bgrewriteaof", bgrewriteaofCommand, 1, "ar", 0, NULL, 0, 0, 0, 0,
    //    0},
    //    {"shutdown", shutdownCommand, -1, "arlt", 0, NULL, 0, 0, 0, 0, 0},
    //    {"lastsave", lastsaveCommand, 1, "rR", 0, NULL, 0, 0, 0, 0, 0},
    //    {"type", typeCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
    {"multi", multiCommand, 1, "rs", 0, NULL, 0, 0, 0, 0, 0},
    {"exec", execCommand, 1, "sM", 0, NULL, 0, 0, 0, 0, 0},
    {"discard", discardCommand, 1, "rs", 0, NULL, 0, 0, 0, 0, 0},
    //    {"sync", syncCommand, 1, "ars", 0, NULL, 0, 0, 0, 0, 0},
    //    {"psync", syncCommand, 3, "ars", 0, NULL, 0, 0, 0, 0, 0},
    //    {"replconf", replconfCommand, -1, "arslt", 0, NULL, 0, 0, 0, 0, 0},
    //    {"flushdb", flushdbCommand, 1, "w", 0, NULL, 0, 0, 0, 0, 0},
    //    {"flushall", flushallCommand, 1, "w", 0, NULL, 0, 0, 0, 0, 0},
    //    {"sort", sortCommand, -2, "wm", 0, sortGetKeys, 1, 1, 1, 0, 0},
    //    {"info", infoCommand, -1, "rlt", 0, NULL, 0, 0, 0, 0, 0},
    //    {"monitor", monitorCommand, 1, "ars", 0, NULL, 0, 0, 0, 0, 0},
    //    {"ttl", ttlCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"pttl", pttlCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"persist", persistCommand, 2, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"slaveof", slaveofCommand, 3, "ast", 0, NULL, 0, 0, 0, 0, 0},
    //    {"debug", debugCommand, -2, "as", 0, NULL, 0, 0, 0, 0, 0},
    //    {"config", configCommand, -2, "art", 0, NULL, 0, 0, 0, 0, 0},
    //    {"subscribe", subscribeCommand, -2, "rpslt", 0, NULL, 0, 0, 0, 0, 0},
    //    {"unsubscribe", unsubscribeCommand, -1, "rpslt", 0, NULL, 0, 0, 0, 0,
    //    0},
    //    {"psubscribe", psubscribeCommand, -2, "rpslt", 0, NULL, 0, 0, 0, 0,
    //    0},
    //    {"punsubscribe", punsubscribeCommand, -1, "rpslt", 0, NULL, 0, 0, 0,
    //    0, 0},
    //    {"publish", publishCommand, 3, "pltr", 0, NULL, 0, 0, 0, 0, 0},
    //    {"pubsub", pubsubCommand, -2, "pltrR", 0, NULL, 0, 0, 0, 0, 0},
    {"watch", watchCommand, -2, "rs", 0, NULL, 1, -1, 1, 0, 0},
    //    {"unwatch", unwatchCommand, 1, "rs", 0, NULL, 0, 0, 0, 0, 0},
    //    {"cluster", clusterCommand, -2, "ar", 0, NULL, 0, 0, 0, 0, 0},
    //    {"restore", restoreCommand, -4, "awm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"restore-asking", restoreCommand, -4, "awmk", 0, NULL, 1, 1, 1, 0,
    //    0},
    //    {"migrate", migrateCommand, -6, "aw", 0, NULL, 0, 0, 0, 0, 0},
    //    {"asking", askingCommand, 1, "r", 0, NULL, 0, 0, 0, 0, 0},
    //    {"readonly", readonlyCommand, 1, "r", 0, NULL, 0, 0, 0, 0, 0},
    //    {"readwrite", readwriteCommand, 1, "r", 0, NULL, 0, 0, 0, 0, 0},
    //    {"dump", dumpCommand, 2, "ar", 0, NULL, 1, 1, 1, 0, 0},
    //    {"object", objectCommand, -2, "r", 0, NULL, 2, 2, 2, 0, 0},
    //    {"client", clientCommand, -2, "ar", 0, NULL, 0, 0, 0, 0, 0},
    //    {"eval", evalCommand, -3, "s", 0, evalGetKeys, 0, 0, 0, 0, 0},
    //    {"evalsha", evalShaCommand, -3, "s", 0, evalGetKeys, 0, 0, 0, 0, 0},
    //    {"slowlog", slowlogCommand, -2, "r", 0, NULL, 0, 0, 0, 0, 0},
    //    {"script", scriptCommand, -2, "ras", 0, NULL, 0, 0, 0, 0, 0},
    //    {"time", timeCommand, 1, "rR", 0, NULL, 0, 0, 0, 0, 0},
    //    {"bitop", bitopCommand, -4, "wm", 0, NULL, 2, -1, 1, 0, 0},
    //    {"bitcount", bitcountCommand, -2, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"bitpos", bitposCommand, -3, "r", 0, NULL, 1, 1, 1, 0, 0},
    //    {"wait", waitCommand, 3, "rs", 0, NULL, 0, 0, 0, 0, 0},
    //    {"pfselftest", pfselftestCommand, 1, "r", 0, NULL, 0, 0, 0, 0, 0},
    //    {"pfadd", pfaddCommand, -2, "wm", 0, NULL, 1, 1, 1, 0, 0},
    //    {"pfcount", pfcountCommand, -2, "w", 0, NULL, 1, 1, 1, 0, 0},
    //    {"pfmerge", pfmergeCommand, -2, "wm", 0, NULL, 1, -1, 1, 0, 0},
    //    {"pfdebug", pfdebugCommand, -3, "w", 0, NULL, 0, 0, 0, 0, 0},
    {"ping", pingCommand, 1, "rt", 0, NULL, 0, 0, 0, 0, 0},
    {"echo", echoCommand, 2, "r", 0, NULL, 0, 0, 0, 0, 0}
};

/* Return the UNIX time in microseconds */
// 返回微秒格式的 UNIX 时间
// 1 秒 = 1 000 000 微秒
long long ustime(void) {
  struct timeval tv;
  long long ust;

  gettimeofday(&tv, NULL);
  ust = ((long long)tv.tv_sec) * 1000000;
  ust += tv.tv_usec;
  return ust;
}

/* Return the UNIX time in milliseconds */
// 返回毫秒格式的 UNIX 时间
// 1 秒 = 1 000 毫秒
long long mstime(void) { return ustime() / 1000; }

unsigned int getLRUClock(void) {
  return (mstime()/REDIS_LRU_CLOCK_RESOLUTION) & REDIS_LRU_CLOCK_MAX;
}


/*          SDS DICT            */
unsigned int dictSdsHash(const void *key) {
  return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

int dictSdsKeyCompare(void *privdata, const void *key1, const void*key2) {
  int l1, l2;
  DICT_NOTUSED(privdata);

  l1 = sdslen((sds)key1);
  l2 = sdslen((sds)key2);
  if (l1 != l2) {
    return 0;;
  }
  return memcmp(key1, key2, l1) == 0;
}

void dictSdsDestructor(void *privdata, void *val) {
  DICT_NOTUSED(privdata);

  sdsfree(val);
}

void dictRedisObjectDestructor(void *privdata, void *val) {
  DICT_NOTUSED(privdata);
  if (val == NULL) {
    return;
  }
  decrRefCount(val); // 共享对象
}

/* Db->dict, keys are sds strings, vals are Redis objects. */
dictType dbDictType = {
    dictSdsHash,              /* hash function */
    NULL,                     /* key dup */
    NULL,                     /* val dup */
    dictSdsKeyCompare,        /* key compare */
    dictSdsDestructor,        /* key destructor */
    dictRedisObjectDestructor /* val destructor */
};
/*          SDS DICT            */

/*          COMMAND TABLE DICT          */
unsigned int dictSdsCaseHash(const void *key) {
  return dictGenCaseHashFunction((unsigned char*)key, sdslen((char*)key));
}

int dictSdsKeyCaseCompare(void *privdata, const void *key1, const void *key2) {
  DICT_NOTUSED(privdata);

  return strcasecmp(key1, key2) == 0;
}

dictType commandTableDictType = {
    dictSdsCaseHash,       /* hash function */
    NULL,                  /* key dup */
    NULL,                  /* val dup */
    dictSdsKeyCaseCompare, /* key compare */
    dictSdsDestructor,     /* key destructor */
    NULL                   /* val destructor */
};
/*          COMMAND TABLE DICT          */


/*             KEY PTR DICT           */

dictType keyptrDictType = {
    dictSdsHash,
    NULL,                  /* key dup */
    NULL,                  /* val dup */
    dictSdsKeyCompare, /* key compare */
    NULL,                  /* key destructor */
    NULL                   /* val destructor */
};

/*             KEY PTR DICT           */

// 根据 redis.c 文件顶部的命令列表，创建命令表
void populateCommandTable(void) {
  int j;

  // 命令的数量
  int numcommands = sizeof(redisCommandTable) / sizeof(struct redisCommand);

  for (j = 0; j < numcommands; j++) {

    // 指定命令
    struct redisCommand *c = redisCommandTable + j;

    // 取出字符串 FLAG
    char *f = c->sflags;

    int retval1, retval2;

    // 根据字符串 FLAG 生成实际 FLAG
    while (*f != '\0') {
      switch(*f) {
      case 'w':
        c->flags |= REDIS_CMD_WRITE;
        break;
      case 'r':
        c->flags |= REDIS_CMD_READONLY;
        break;
      case 'm':
        c->flags |= REDIS_CMD_DENYOOM;
        break;
      case 'a':
        c->flags |= REDIS_CMD_ADMIN;
        break;
      case 'p':
        c->flags |= REDIS_CMD_PUBSUB;
        break;
      case 's':
        c->flags |= REDIS_CMD_NOSCRIPT;
        break;
      case 'R':
        c->flags |= REDIS_CMD_RANDOM;
        break;
      case 'S':
        c->flags |= REDIS_CMD_SORT_FOR_SCRIPT;
        break;
      case 'l':
        c->flags |= REDIS_CMD_LOADING;
        break;
      case 't':
        c->flags |= REDIS_CMD_STALE;
        break;
      case 'M':
        c->flags |= REDIS_CMD_SKIP_MONITOR;
        break;
      case 'k':
        c->flags |= REDIS_CMD_ASKING;
        break;
      default:
        redisPanic("Unsupported command flag");
        break;
      }
      f++;
    }

    // 将命令关联到命令表
    retval1 = dictAdd(server.commands, sdsnew(c->name), c);

    /* Populate an additional dictionary that will be unaffected
     * by rename-command statements in redis.conf.
     *
     * 将命令也关联到原始命令表
     *
     * 原始命令表不会受 redis.conf 中命令改名的影响
     */
    retval2 = dictAdd(server.orig_commands, sdsnew(c->name), c);

    redisAssert(retval1 == DICT_OK && retval2 == DICT_OK);
  }
}

void pingCommand(redisClient *c) {
  // TODO
  printf("PONG\n");
}

void echoCommand(redisClient *c) {
  // TODO
}

// 每次处理事件前执行
void beforeSleep(struct aeEventLoop *eventLoop) {
  // TODO
}

int serveCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
  int j;
  REDIS_NOTUSED(eventLoop);
  REDIS_NOTUSED(id);
  REDIS_NOTUSED(clientData);


}

// ======================= 服务初始化 ========================
void createSharedObjects() {
  int j;
  // 常用回复
  shared.crlf = createObject(REDIS_STRING, sdsnew("\r\n"));
  shared.ok = createObject(REDIS_STRING, sdsnew("+OK\r\n"));
  shared.err = createObject(REDIS_STRING, sdsnew("-ERR\r\n"));
  shared.emptybulk = createObject(REDIS_STRING, sdsnew("$0\r\n\r\n"));
  shared.czero = createObject(REDIS_STRING, sdsnew(":0\r\n"));
  shared.cone = createObject(REDIS_STRING, sdsnew(":1\r\n"));
  shared.cnegone = createObject(REDIS_STRING, sdsnew(":-1\r\n"));
  shared.nullbulk = createObject(REDIS_STRING, sdsnew("$-1\r\n"));
  shared.nullmultibulk = createObject(REDIS_STRING, sdsnew("*-1\r\n"));
  shared.emptymultibulk = createObject(REDIS_STRING, sdsnew("*0\r\n"));
  shared.pong = createObject(REDIS_STRING, sdsnew("+PONG\r\n"));
  shared.queued = createObject(REDIS_STRING, sdsnew("+QUEUED\r\n"));
  shared.emptyscan =
      createObject(REDIS_STRING, sdsnew("*2\r\n$1\r\n0\r\n*0\r\n"));
  // 常用错误回复
  shared.wrongtypeerr =
      createObject(REDIS_STRING, sdsnew("-WRONGTYPE Operation against a key "
                                        "holding the wrong kind of value\r\n"));
  shared.nokeyerr = createObject(REDIS_STRING, sdsnew("-ERR no such key\r\n"));
  shared.syntaxerr =
      createObject(REDIS_STRING, sdsnew("-ERR syntax error\r\n"));
  shared.sameobjecterr = createObject(
      REDIS_STRING,
      sdsnew("-ERR source and destination objects are the same\r\n"));
  shared.outofrangeerr =
      createObject(REDIS_STRING, sdsnew("-ERR index out of range\r\n"));
  shared.noscripterr = createObject(
      REDIS_STRING,
      sdsnew("-NOSCRIPT No matching script. Please use EVAL.\r\n"));
  shared.loadingerr = createObject(
      REDIS_STRING,
      sdsnew("-LOADING Redis is loading the dataset in memory\r\n"));
  shared.slowscripterr = createObject(
      REDIS_STRING, sdsnew("-BUSY Redis is busy running a script. You can only "
                           "call SCRIPT KILL or SHUTDOWN NOSAVE.\r\n"));
  shared.masterdownerr = createObject(
      REDIS_STRING, sdsnew("-MASTERDOWN Link with MASTER is down and "
                           "slave-serve-stale-data is set to 'no'.\r\n"));
  shared.bgsaveerr = createObject(
      REDIS_STRING,
      sdsnew("-MISCONF Redis is configured to save RDB snapshots, but is "
             "currently not able to persist on disk. Commands that may modify "
             "the data    set are disabled. Please check Redis logs for "
             "details about the error.\r\n"));
  shared.roslaveerr = createObject(
      REDIS_STRING,
      sdsnew("-READONLY You can't write against a read only slave.\r\n"));
  shared.noautherr = createObject(
      REDIS_STRING, sdsnew("-NOAUTH Authentication required.\r\n"));
  shared.oomerr = createObject(
      REDIS_STRING,
      sdsnew("-OOM command not allowed when used memory > 'maxmemory'.\r\n"));
  shared.execaborterr = createObject(
      REDIS_STRING,
      sdsnew(
          "-EXECABORT Transaction discarded because of previous errors.\r\n"));
  shared.noreplicaserr = createObject(
      REDIS_STRING, sdsnew("-NOREPLICAS Not enough good slaves to write.\r\n"));
  shared.busykeyerr = createObject(
      REDIS_STRING, sdsnew("-BUSYKEY Target key name already exists.\r\n"));

  // 常用字符
  shared.space = createObject(REDIS_STRING, sdsnew(" "));
  shared.colon = createObject(REDIS_STRING, sdsnew(":"));
  shared.plus = createObject(REDIS_STRING, sdsnew("+"));

  // 常用 SELECT 命令
  for (j = 0; j < REDIS_SHARED_SELECT_CMDS; j++) {
    char dictid_str[64];
    int dictid_len;

    dictid_len = ll2string(dictid_str, sizeof(dictid_str), j);
    shared.select[j] = createObject(
        REDIS_STRING,
        sdscatprintf(sdsempty(), "*2\r\n$6\r\nSELECT\r\n$%d\r\n%s\r\n",
                     dictid_len, dictid_str));
  }

  // 常用命令
  shared.del = createStringObject("DEL", 3);
  shared.rpop = createStringObject("RPOP", 4);
  shared.lpop = createStringObject("LPOP", 4);
  shared.lpush = createStringObject("LPUSH", 5);

  // 常用整数
  for (j = 0; j < REDIS_SHARED_INTEGERS; j++) {
    shared.integers[j] = createObject(REDIS_STRING, (void *)(long)j);
    shared.integers[j]->encoding = REDIS_ENCODING_INT;
  }

  // 常用长度 bulk 或者 multi bulk 回复
  for (j = 0; j < REDIS_SHARED_BULKHDR_LEN; j++) {
    shared.mbulkhdr[j] = createObject(REDIS_STRING,
                                      sdscatprintf(sdsempty(),"*%d\r\n",j));
    shared.bulkhdr[j] = createObject(REDIS_STRING,
                                     sdscatprintf(sdsempty(),"$%d\r\n",j));
  }
  /* The following two shared objects, minstring and maxstrings, are not
   * actually used for their value but as a special object meaning
   * respectively the minimum possible string and the maximum possible
   * string in string comparisons for the ZRANGEBYLEX command. */
  shared.minstring = createStringObject("minstring",9);
  shared.maxstring = createStringObject("maxstring",9);
}

void initServerConfig() {
  int j;
  /*服务器状态*/

  server.configfile = NULL;
  // 设置默认服务器频率
  server.hz = REDIS_DEFAULT_HZ;
  server.port = REDIS_SERVERPORT;
  server.tcp_backlog = REDIS_TCP_BACKLOG;
  server.bindaddr_count = 0;

  server.ipfd_count = 0;
  server.dbnum = REDIS_DEFAULT_DBNUM;
  server.tcpkeepalive = REDIS_DEFAULT_TCP_KEEPALIVE;


  server.maxclients = REDIS_MAX_CLIENTS;

  server.commands = dictCreate(&commandTableDictType, NULL);
  server.orig_commands = dictCreate(&commandTableDictType, NULL);
  populateCommandTable();
}

// 监听端口
int listenToPort(int port, int *fds, int *count) {
  int j;

  if (server.bindaddr_count == 0) {
    server.bindaddr[0] = NULL;
  }

  // 监听所有绑定的IP
  for (j = 0; j < server.bindaddr_count||j == 0; j++) {
    if (server.bindaddr[j] == NULL) {
      /* Bind * for both IPv6 and IPv4, we enter here only if
       * server.bindaddr_count == 0. */
      fds[*count] =
          anetTcp6Server(server.neterr, port, NULL, server.tcp_backlog);
      if (fds[*count] != ANET_ERR) {
        anetNonBlock(NULL, fds[*count]);
        (*count)++;
      }
      fds[*count] =
          anetTcpServer(server.neterr, port, NULL, server.tcp_backlog);
      if (fds[*count] != ANET_ERR) {
        anetNonBlock(NULL, fds[*count]);
        (*count)++;
      }
      /* Exit the loop if we were able to bind * on IPv4 or IPv6,
       * otherwise fds[*count] will be ANET_ERR and we'll print an
       * error and return to the caller with an error. */
      if (*count)
        break;
    } else if (strchr(server.bindaddr[j], ':')) {
      // bind IPv6
      fds[*count] = anetTcp6Server(server.neterr, port, server.bindaddr[j], server.tcp_backlog);
    } else {
      fds[*count] = anetTcpServer(server.neterr, port, server.bindaddr[j],
                                  server.tcp_backlog);
    }
    if (fds[*count] == ANET_ERR) {
      redisLog(REDIS_WARNING, "Creating Server TCP listening socket %s:%d: %s",
               server.bindaddr[j] ? server.bindaddr[j] : "*", port,
               server.neterr);
      return REDIS_ERR;
    }
    anetNonBlock(NULL, fds[*count]);
    (*count)++;
  }
  return REDIS_OK;
}

// 重置服务器状态
void resetServerState() {
  server.stat_numcommands = 0;
  server.stat_numconnections = 0;
  server.stat_expiredkeys = 0;
  server.stat_evictedkeys = 0;
  server.stat_keyspace_misses = 0;
  server.stat_keyspace_hits = 0;
  server.stat_fork_time = 0;
  server.stat_rejected_conn = 0;
  server.stat_sync_full = 0;
  server.stat_sync_partial_ok = 0;
  server.stat_sync_partial_err = 0;
  //memset(server.ops_sec_samples, 0, sizeof(server.ops_sec_samples));
  //server.ops_sec_idx = 0;
  //server.ops_sec_last_sample_time = mstime();
  //server.ops_sec_last_sample_ops = 0;
}

void initServer() {
    int j;

    // 设置信号处理函数
    //signal(SIGHUP, SIG_IGN);
    //signal(SIGPIPE, SIG_IGN);
    //setupSignalHandlers();

    // 初始化并创建数据结构
    server.current_client = NULL;
    server.clients = listCreate();
    server.clients_to_close = listCreate();
    // TODO

    // 创建共享对象
    createSharedObjects();
    server.db = zmalloc(sizeof(redisDb)*server.dbnum);
    server.el = aeCreateEventLoop(server.maxclients + REDIS_EVENTLOOP_FDSET_INCR);

    // 打开TCP监听端口 用于等待客户端的命令请求
    if (server.port != 0 && listenToPort(server.port, server.ipfd, &server.ipfd_count) == REDIS_ERR) {
      exit(1);
    }
    // TODO 监听本地UNIX连接

    /* Abort if there are no listening sockets at all. */
    if (server.ipfd_count == 0 && server.sofd < 0) {
      redisLog(REDIS_WARNING, "Configured to not listen anywhere, exiting.");
      exit(1);
    }

    // 创建并初始化数据库结构
    for (j = 0; j < server.dbnum; j++) {
      server.db[j].dict = dictCreate(&dbDictType, NULL);
      server.db[j].expires = dictCreate(&keyptrDictType, NULL);
      //server.db[j].blocking_keys = dictCreate(&keylistDictType, NULL);
      //server.db[j].ready_keys = dictCreate(&setDictType, NULL);
      //server.db[j].watched_keys = dictCreate(&keylistDictType, NULL);
      //server.db[j].eviction_pool = evictionPoolAlloc();
      server.db[j].id = j;
      server.db[j].avg_ttl = 0;
    }

    server.rdb_child_pid = -1;
    server.aof_child_pid = -1;

    // 为 serverCron() 创建时间事件
    if (aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL) == AE_ERR) {
      redisPanic("Can't create the serverCron time event.");
      exit(1);
    }

    for (j = 0;j < server.ipfd_count;j++) {
      if (aeCreateFileEvent(server.el, server.ipfd[j], AE_READABLE,
                            acceptTcpHandler, NULL) == AE_ERR) {
        redisPanic("Unrecoverable error creating server.ipfd file event.");
      }
    }
    // TODO 本地连接 AOF功能 集群

}

/*============================ Utility functions ============================ */

/* Low level logging. To use only for very big messages, otherwise
 * redisLog() is to prefer. */
void redisLogRaw(int level, const char *msg) {
  const int syslogLevelMap[] = { LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING };

  // TODO 开启syslog
  switch (syslogLevelMap[level]) {
    case LOG_DEBUG:
      printf(BLUESTR("%s\n"), msg);
      break;
    case LOG_INFO:
      printf(GREENSTR("%s\n"), msg);
      break;
    case LOG_NOTICE:
      printf(YELLOWSTR("%s\n"), msg);
      break;
    case LOG_WARNING:
      printf(REDSTR("%s\n"), msg);
      break;
    default:
      printf(REDSTR("%s\n"), msg);
  }
}

/* Like redisLogRaw() but with printf-alike support. This is the function that
 * is used across the code. The raw version is only used in order to dump
 * the INFO output on crash. */
void redisLog(int level, const char *fmt, ...) {
  va_list ap;
  char msg[REDIS_MAX_LOGMSG_LEN];

  if ((level&0xff) < server.verbosity) return;

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  redisLogRaw(level,msg);
}

/* ========================== Redis OP Array API ============================ */

void redisOpArrayInit(redisOpArray *oa) {
  oa->ops = NULL;
  oa->numops = 0;
}

int redisOpArrayAppend(redisOpArray *oa, struct redisCommand *cmd, int dbid,
                       robj **argv, int argc, int target) {
  redisOp *op;

  oa->ops = zrealloc(oa->ops, sizeof(redisOp) * (oa->numops + 1));
  op = oa->ops + oa->numops;
  op->cmd = cmd;
  op->dbid = dbid;
  op->argv = argv;
  op->argc = argc;
  op->target = target;
  oa->numops++;
  return oa->numops;
}

void redisOpArrayFree(redisOpArray *oa) {
  while (oa->numops) {
      int j;
      redisOp *op;

      oa->numops--;
      op = oa->ops + oa->numops;
      for (j = 0; j < op->argc; j++)
        decrRefCount(op->argv[j]);
      zfree(op->argv);
  }
  zfree(oa->ops);
}

struct redisCommand *lookupCommand(sds name) {
  return dictFetchValue(server.commands, name);
}

/*
 * 根据给定命令名字（C 字符串），查找命令
 */
struct redisCommand *lookupCommandByCString(char *s) {
  struct redisCommand *cmd;
  sds name = sdsnew(s);

  cmd = dictFetchValue(server.commands, name);
  sdsfree(name);
  return cmd;
}

struct redisCommand *lookupCommandOrOriginal(sds name) {

  // 查找当前表
  struct redisCommand *cmd = dictFetchValue(server.commands, name);

  // 如果有需要的话，查找原始表
  if (!cmd)
      cmd = dictFetchValue(server.orig_commands, name);

  return cmd;
}

int freeMemoryIfNeeded() {
  // TODO 计算Redis目前占用的内存信息 有需要话删除过期的键
  return 1;
}

// 调用命令的实现函数
void call(redisClient *c, int flags) {
  // start 记录命令开始执行的时间
  long long dirty, start, duration;
  // 记录命令开始执行前的 FLAG
  int client_old_flags = c->flags;
  /* Call the command. */
  c->flags &= ~(REDIS_FORCE_AOF | REDIS_FORCE_REPL);
  redisOpArrayInit(&server.also_propagate);
  // 保留旧 dirty 计数器值
  dirty = server.dirty;
  // 计算命令开始执行的时间
  start = ustime();
  // 执行实现函数
  c->cmd->proc(c);
  // 计算命令执行耗费的时间
  duration = ustime() - start;
  // 计算命令执行之后的 dirty 值
  dirty = server.dirty - dirty;

  // 更新命令的统计信息
  if (flags & REDIS_CALL_STATS) {
      c->cmd->microseconds += duration;
      c->cmd->calls++;
  }

  server.stat_numcommands++;
}

int processCommand(redisClient *c) {
  if (!strcasecmp(c->argv[0]->ptr, "quit")) {
      addReply(c, shared.ok);
      c->flags |= REDIS_CLOSE_AFTER_REPLY;
      return REDIS_ERR;
  }
  c->cmd = c->lastcmd = lookupCommand(c->argv[0]->ptr);
  if (!c->cmd) { // 没找到
      // TODO flagTransaction(c);
      addReplyErrorFormat(c, "unknown command '%s'", (char *)c->argv[0]->ptr);
      return REDIS_OK;
  } else if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) ||
             (c->argc < -c->cmd->arity)) { // 参数个数错误
      // TODO flagTransaction(c);
      addReplyErrorFormat(c, "wrong number of arguments for '%s' command",
                          c->cmd->name);
      return REDIS_OK;
  }
  //  TODO 检查认证信息 && 处理集群

  // 如果设置了最大内存 尝试删除过期键来释放内存
  if (server.maxmemory) {
    int retval = freeMemoryIfNeeded();
    // TODO 处理内存错误
  }

  // TODO 处理 BGSAVE 命令

  // TODO 处理状态异常的服务器 只读服务器 发布订阅模式 载入数据 Lua脚本

  // 执行命令
  if (c->flags & REDIS_MULTI && c->cmd->proc != execCommand &&
      c->cmd->proc != discardCommand && c->cmd->proc != multiCommand &&
      c->cmd->proc != watchCommand) {
    // 在事务上下文中
    // 除 EXEC 、 DISCARD 、 MULTI 和 WATCH 命令之外
    // 其他所有命令都会被入队到事务队列中
    // TODO queueMultiCommand(c);
    addReply(c, shared.queued);
  } else {
    // 执行命令
    call(c, REDIS_CALL_FULL);

    // TODO c->woff = server.master_repl_offset;
    // TODO 处理那些解除了阻塞的键
    //if (listLength(server.ready_keys))
    //    handleClientsBlockedOnLists();
  }

  return REDIS_OK;
}


#if 1

void test() {
  redisClient *cli = zmalloc(sizeof(redisClient));
  robj *argv[5];
  argv[0] = createStringObject("SET", 3);
  argv[1] = createStringObject("key", 3);
  argv[2] = createStringObject("value", 5);
  argv[3] = createStringObject("ex", 2);
  argv[4] = createStringObjectFromLongLong(2);
  cli->argc = 5;
  cli->argv = argv;
  cli->db = server.db;

  listAddNodeHead(server.clients, cli);

  listNode *head;
  redisClient *c;
  head = listFirst(server.clients);
  c = listNodeValue(head);

  setCommand(c);

  sleep(1);

  cli->argc = 3;
  cli->argv[0] = createStringObject("GET", 3);
  cli->argv[1] = createStringObject("key", 3);
  cli->argv[2] = createStringObject("value", 5);
  getCommand(c);

}

void redisAsciiArt(void) {
#include "asciilogo.h"
  char *buf = zmalloc(1024 * 16);
  char *mode = "stand alone";

  if (server.cluster_enabled)
      mode = "cluster";
  else if (server.sentinel_mode)
      mode = "sentinel";

  snprintf(buf, 1024 * 16, ascii_logo, REDIS_VERSION,
           (sizeof(long) == 8) ? "64" : "32", mode, server.port,
           (long)getpid());
  //redisLogRaw(REDIS_NOTICE | REDIS_LOG_RAW, buf);
  redisLogRaw(REDIS_NOTICE, buf);
  zfree(buf);
}

int main(int argc, char **argv) {
    // 初始化服务器
    initServerConfig();
    initServer();

    redisAsciiArt();

    // 运行事件处理器，一直到服务器关闭为止
    aeSetBeforeSleepProc(server.el, beforeSleep);
    aeMain(server.el);

    // 服务器关闭，停止事件循环
    aeDeleteEventLoop(server.el);


    return 0;
}

#endif
