#include "redis.h"

#define REDIS_SET_NO_FLAGS 0

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

  setGenericCommand(c,flags, c->argv[1], c->argv[2], expire, unit, NULL, NULL);
}

void getCommand(redisClient *c) {

}
