#include "redis.h"

robj *hashTypeLookupWriteOrCreate(redisClient *c, robj *key) {
  robj *o = lookupKeyWrite(c->db, key);

  // 创建
  if (o == NULL) {
    o = createHashObject();
  } else { // 检查

  }

  return o;
}


/*
** ================== Hash Type Commands =====================
*/

void hsetCommand(redisClient *c) {
  int update;
  robj *o;

  if ((o = hashTypeLookupWriteOrCreate(c, c->argv[1])) == NULL) {
    return;
  }
}


void hsetnxCommand(redisClient *c) {

}

void hgetCommand(redisClient *c) {

}

void hmsetCommand(redisClient *c) {

}

void hmgetCommand(redisClient *c) {

}

void hdelCommand(redisClient *c) {

}

void hlenCommand(redisClient *c) {

}

void hkeysCommand(redisClient *c) {

}

void hvalsCommand(redisClient *c) {

}
