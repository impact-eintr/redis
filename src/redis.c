#include "redis.h"

#include <stdio.h>

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
//    {"get", getCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
    {"set", setCommand, -3, "wm", 0, NULL, 1, 1, 1, 0, 0}
//    {"setnx", setnxCommand, 3, "wm", 0, NULL, 1, 1, 1, 0, 0},
//    {"setex", setexCommand, 4, "wm", 0, NULL, 1, 1, 1, 0, 0},
//    {"psetex", psetexCommand, 4, "wm", 0, NULL, 1, 1, 1, 0, 0},
//    {"append", appendCommand, 3, "wm", 0, NULL, 1, 1, 1, 0, 0},
//    {"strlen", strlenCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
//    {"del", delCommand, -2, "w", 0, NULL, 1, -1, 1, 0, 0},
//    {"exists", existsCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
//    {"setbit", setbitCommand, 4, "wm", 0, NULL, 1, 1, 1, 0, 0},
//    {"getbit", getbitCommand, 3, "r", 0, NULL, 1, 1, 1, 0, 0},
//    {"setrange", setrangeCommand, 4, "wm", 0, NULL, 1, 1, 1, 0, 0},
//    {"getrange", getrangeCommand, 4, "r", 0, NULL, 1, 1, 1, 0, 0},
//    {"substr", getrangeCommand, 4, "r", 0, NULL, 1, 1, 1, 0, 0},
//    {"incr", incrCommand, 2, "wm", 0, NULL, 1, 1, 1, 0, 0},
//    {"decr", decrCommand, 2, "wm", 0, NULL, 1, 1, 1, 0, 0},
//    {"mget", mgetCommand, -2, "r", 0, NULL, 1, -1, 1, 0, 0},
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
//    {"zunionstore", zunionstoreCommand, -4, "wm", 0, zunionInterGetKeys, 0, 0, 0, 0, 0},
//    {"zinterstore", zinterstoreCommand, -4, "wm", 0, zunionInterGetKeys, 0, 0, 0, 0, 0},
//    {"zrange", zrangeCommand, -4, "r", 0, NULL, 1, 1, 1, 0, 0},
//    {"zrangebyscore", zrangebyscoreCommand, -4, "r", 0, NULL, 1, 1, 1, 0, 0},
//    {"zrevrangebyscore", zrevrangebyscoreCommand, -4, "r", 0, NULL, 1, 1, 1, 0, 0},
//    {"zrangebylex", zrangebylexCommand, -4, "r", 0, NULL, 1, 1, 1, 0, 0},
//    {"zrevrangebylex", zrevrangebylexCommand, -4, "r", 0, NULL, 1, 1, 1, 0, 0},
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
//    {"incrby", incrbyCommand, 3, "wm", 0, NULL, 1, 1, 1, 0, 0},
//    {"decrby", decrbyCommand, 3, "wm", 0, NULL, 1, 1, 1, 0, 0},
//    {"incrbyfloat", incrbyfloatCommand, 3, "wm", 0, NULL, 1, 1, 1, 0, 0},
//    {"getset", getsetCommand, 3, "wm", 0, NULL, 1, 1, 1, 0, 0},
//    {"mset", msetCommand, -3, "wm", 0, NULL, 1, -1, 2, 0, 0},
//    {"msetnx", msetnxCommand, -3, "wm", 0, NULL, 1, -1, 2, 0, 0},
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
//    {"ping", pingCommand, 1, "rt", 0, NULL, 0, 0, 0, 0, 0},
//    {"echo", echoCommand, 2, "r", 0, NULL, 0, 0, 0, 0, 0},
//    {"save", saveCommand, 1, "ars", 0, NULL, 0, 0, 0, 0, 0},
//    {"bgsave", bgsaveCommand, 1, "ar", 0, NULL, 0, 0, 0, 0, 0},
//    {"bgrewriteaof", bgrewriteaofCommand, 1, "ar", 0, NULL, 0, 0, 0, 0, 0},
//    {"shutdown", shutdownCommand, -1, "arlt", 0, NULL, 0, 0, 0, 0, 0},
//    {"lastsave", lastsaveCommand, 1, "rR", 0, NULL, 0, 0, 0, 0, 0},
//    {"type", typeCommand, 2, "r", 0, NULL, 1, 1, 1, 0, 0},
//    {"multi", multiCommand, 1, "rs", 0, NULL, 0, 0, 0, 0, 0},
//    {"exec", execCommand, 1, "sM", 0, NULL, 0, 0, 0, 0, 0},
//    {"discard", discardCommand, 1, "rs", 0, NULL, 0, 0, 0, 0, 0},
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
//    {"unsubscribe", unsubscribeCommand, -1, "rpslt", 0, NULL, 0, 0, 0, 0, 0},
//    {"psubscribe", psubscribeCommand, -2, "rpslt", 0, NULL, 0, 0, 0, 0, 0},
//    {"punsubscribe", punsubscribeCommand, -1, "rpslt", 0, NULL, 0, 0, 0, 0, 0},
//    {"publish", publishCommand, 3, "pltr", 0, NULL, 0, 0, 0, 0, 0},
//    {"pubsub", pubsubCommand, -2, "pltrR", 0, NULL, 0, 0, 0, 0, 0},
//    {"watch", watchCommand, -2, "rs", 0, NULL, 1, -1, 1, 0, 0},
//    {"unwatch", unwatchCommand, 1, "rs", 0, NULL, 0, 0, 0, 0, 0},
//    {"cluster", clusterCommand, -2, "ar", 0, NULL, 0, 0, 0, 0, 0},
//    {"restore", restoreCommand, -4, "awm", 0, NULL, 1, 1, 1, 0, 0},
//    {"restore-asking", restoreCommand, -4, "awmk", 0, NULL, 1, 1, 1, 0, 0},
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
//    {"pfdebug", pfdebugCommand, -3, "w", 0, NULL, 0, 0, 0, 0, 0}
};

void intServer() {
    int j;

    // 设置信号处理函数
    //signal(SIGHUP, SIG_IGN);
    //signal(SIGPIPE, SIG_IGN);
    //setupSignalHandlers();

    // 初始化并创建数据结构




    printf("test\n");
}

#if 1

int main(int argc, char **argv) {
    intServer();
}

#endif