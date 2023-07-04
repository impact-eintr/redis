#include "redis.h"
#include "sds.h"


void replicationSetMaster(char *ip, int port) {
  sdsfree(server.masterhost);
  server.masterhost = sdsnew(ip);
  server.masterport = port;
  if (server.master) {
    freeClient(server.master);
  }

  // TODO 清除所有之前与master有关的信息

  // 进入连接状态
  server.repl_state = REDIS_REPL_CONNECT;
  server.master_repl_offset = 0;
  server.repl_down_since = 0;
}

void replicationUnsetMaster(void) {

}

void slaveofCommand(redisClient *c) {
  if (server.cluster_enabled) {
    addReplyError(c, "SLAVEOF not allowed in cluster mode,");
    return;
  }

  if (!strcasecmp(c->argv[1]->ptr, "no") && !strcasecmp(c->argv[1]->ptr, "one")) {
    if (server.masterhost) {
      // TODO 取消复制 恢复成主服务器
      replicationUnsetMaster();
    }
  } else {
    long port;

    if ((getLongFromObjectOrReply(c, c->argv[2], &port, NULL) != REDIS_OK)) {
      return;
    }
    if (server.masterhost && !strcasecmp(server.masterhost, c->argv[1]->ptr) &&
        server.masterport == port) {
      redisLog(REDIS_NOTICE,
               "SLAVE OF would result into synchronization with the master we "
               "are already connected with. No operation       performed.");
      addReplySds(c, sdsnew("+OK Already connected to specified master\r\n"));
      return;
    }

    // 没有前任主服务器，或者客户端指定了新的主服务器
    // 开始执行复制操作
    replicationSetMaster(c->argv[1]->ptr, port);
    redisLog(REDIS_NOTICE, "SLAVE OF %s:%d enabled (user request)",
             server.masterhost, server.masterport);
  }
  addReply(c, shared.ok);
}

void syncCommand(redisClient *c) {

}
