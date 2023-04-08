#include "redis.h"

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX.
 *
 * setGenericCommand() 函数实现了 SET 、 SETEX 、 PSETEX 和 SETNX 命令。
 *
 * 'flags' changes the behavior of the command (NX or XX, see belove).
 *
 * flags 参数的值可以是 NX 或 XX ，它们的意义请见下文。
 *
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 *
 * expire 定义了 Redis 对象的过期时间。
 *
 * 而这个过期时间的格式由 unit 参数指定。
 *
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 *
 * ok_reply 和 abort_reply 决定了命令回复的内容，
 * NX 参数和 XX 参数也会改变回复。
 *
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used.
 *
 * 如果 ok_reply 为 NULL ，那么 "+OK" 被返回。
 * 如果 abort_reply 为 NULL ，那么 "$-1" 被返回。
 */

#define REDIS_SET_NO_FLAGS 0
#define REDIS_SET_NX (1 << 0) /* Set if key not exists. */
#define REDIS_SET_XX (1 << 1) /* Set if key exists. */

void setGenericCommand(redisClient *c, int flags, robj *key, robj *val,
                       robj *expire, int uint, robj *ok_reply,
                       robj *abort_reply) {
  long long milliseconds = 0;

  // TODO

  // 将键值对关联到数据库
  setKey(c->db, key, val);
}

/* SET key value [NX] [XX] [EX <seconds>] [PX <milliseconds>] */
void setCommand(redisClient *c) {
  robj *expire = NULL;
  int unit = UNIT_SECONDS;
  int flags = REDIS_SET_NO_FLAGS;
  // 尝试编码
  c->argv[2] = tryObjectEncoding(c->argv[2]);

  setGenericCommand(c, flags, c->argv[1], c->argv[2], expire, unit, NULL, NULL);
}

void setnxCommand(redisClient *c) {
  c->argv[2] = tryObjectEncoding(c->argv[2]);
  setGenericCommand(c, REDIS_SET_NX, c->argv[1], c->argv[2], NULL, 0,
                    shared.cone, shared.czero);
}

void setexCommand(redisClient *c) {
  c->argv[3] = tryObjectEncoding(c->argv[3]);
  setGenericCommand(c, REDIS_SET_NO_FLAGS, c->argv[1], c->argv[3], c->argv[2],
                    UNIT_SECONDS, NULL, NULL);
}

void psetexCommand(redisClient *c) {
  c->argv[3] = tryObjectEncoding(c->argv[3]);
  setGenericCommand(c, REDIS_SET_NO_FLAGS, c->argv[1], c->argv[3], c->argv[2],
                    UNIT_MILLISECONDS, NULL, NULL);
}

int getGenericCommand(redisClient *c) {
  robj *o;

  // 尝试从数据库中取出键 c->argv[1] 对应的值对象
  // 如果键不存在时，向客户端发送回复信息，并返回 NULL
  if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.nullbulk)) == NULL)
    return REDIS_OK;

  // 值对象存在，检查它的类型
  if (o->type != REDIS_STRING) {
    // 类型错误
    // TODO addReply(c, shared.wrongtypeerr);
    return REDIS_ERR;
  } else {
    // 类型正确，向客户端返回对象的值
    // TODO addReplyBulk(c, o);
    return REDIS_OK;
  }

  void getCommand(redisClient * c) { getGenericCommand(c); }
