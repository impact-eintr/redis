#include "redis.h"
#include <stdio.h>


/* =========================== Crash handling  ============================== */

void _redisAssertWithInfo(redisClient *c, robj *o, char *estr, char *file, int line) {
  if (c) {
    printf("Client Error\n");
  }

  if (o) {
    printf("Object Error\n");
  }
  _redisAssert(estr, file, line);
}

void _redisAssert(char *estr, char *file, int line) {
  printf("=== ASSERTION FAILED ===\n");
  printf("%s [%d]: %s\n", file, line, estr);
}

void _redisPanic(char *msg, char *file, int line) {
  printf("=== PANIC !!! ===\n");
  printf("%s [%d]: %s\n", file, line, msg);
}
