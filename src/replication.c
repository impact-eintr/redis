#include "adlist.h"
#include "redis.h"
#include "sds.h"
#include "zmalloc.h"

void disconnectSlaves(void) {
  while (listLength(server.slaves)) {
    listNode *ln = listFirst(server.slaves);
    freeClient((redisClient*)ln->value);
  }
}

void replicationDiscardCachedMaster(void) {
  if (server.cached_master == NULL) {
    return;
  }

  redisLog(REDIS_NOTICE, "Discarding previously cached master state.");
  server.cached_master->flags &= ~REDIS_MASTER;
  freeClient(server.cached_master);
  server.cached_master = NULL;
}

int cancelReplicationHandshake(void) {
  if (server.repl_state == REDIS_REPL_TRANSFER) {
    // TODO
  } else if (server.repl_state == REDIS_REPL_CONNECTING ||
             server.repl_state == REDIS_REPL_RECEIVE_PONG) {
    // TODO
  } else {
    return 0;
  }

  return 1;
}

// 释放backlog
void freeReplicationBacklog(void) {
  redisAssert(listLength(server.slaves) == 0);
  zfree(server.repl_backlog);
  server.repl_backlog = NULL;
}

void replicationSetMaster(char *ip, int port) {
  sdsfree(server.masterhost);
  server.masterhost = sdsnew(ip);
  server.masterport = port;
  if (server.master) {
    freeClient(server.master);
  }
  disconnectSlaves();
  replicationDiscardCachedMaster();
  freeReplicationBacklog();
  cancelReplicationHandshake();

  // 进入连接状态
  server.repl_state = REDIS_REPL_CONNECT;
  server.master_repl_offset = 0;
  server.repl_down_since = 0;
}

void replicationUnsetMaster(void) {
  if (server.masterhost == NULL)
    return; /* Nothing to do. */

  sdsfree(server.masterhost);
  server.masterhost = NULL;

  if (server.master) {
    if (listLength(server.slaves) == 0) {
      /* If this instance is turned into a master and there are no
       * slaves, it inherits the replication offset from the master.
       * Under certain conditions this makes replicas comparable by
       * replication offset to understand what is the most updated. */
      server.master_repl_offset = server.master->reploff;
      freeReplicationBacklog();
    }
    freeClient(server.master);
  }

  replicationDiscardCachedMaster();

  cancelReplicationHandshake();

  server.repl_state = REDIS_REPL_NONE;
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

// 尝试进行部分 resync 成功返回 REDIS_OK 失败返回 REDIS_ERR
int masterTryPartialResyncChronization(redisClient *c) {
  long long psync_offset, psync_len;
  char *master_runid = c->argv[1]->ptr;
  char buf[128];
  int buflen;

  // 检查 master id 是否和 runid 一致，只有一致的情况下才有 PSYNC 的可能
  if (strcasecmp(master_runid, server.runid)) {
    /* Run id "?" is used by slaves that want to force a full resync. */
    // 从服务器提供的 run id 和服务器的 run id 不一致
    if (master_runid[0] != '?') {
      redisLog(REDIS_NOTICE,
               "Partial resynchronization not accepted: "
               "Runid mismatch (Client asked for runid '%s', my runid is '%s')",
               master_runid, server.runid);
      // 从服务器提供的 run id 为 '?' ，表示强制 FULL RESYNC
    } else {
      redisLog(REDIS_NOTICE, "Full resync requested by slave.");
    }
    // 需要 full resync
    goto need_full_resync;
  }

need_full_resync:
  psync_offset = server.master_repl_offset;
  if (server.repl_backlog == NULL) psync_offset++;

  // 发送 +FULLRESYNC ，表示需要完整重同步
  buflen = snprintf(buf, sizeof(buf), "+FULLRESYNC %s %lld\r\n", server.runid,
                    psync_offset);
  if (write(c->fd, buf, buflen) != buflen) {
    freeClientAsync(c);
    return REDIS_OK;
  }

  return REDIS_ERR;
}

void syncCommand(redisClient *c) {
  if (c->flags & REDIS_SLAVE) { // slave不处理 sync 命令
    return;
  }

  if (server.masterhost && server.repl_state != REDIS_REPL_CONNECTED) {
    addReplyError(c, "Can not sync while not connected with my master");
    return;
  }

  // 客户端还有数据没有接收结束
  if (listLength(c->reply) != 0 || c->bufpos != 0) {
    addReplyError(c, "SYNC and PSYNC are invalid woth pending output");
    return;
  }

  if (!strcasecmp(c->argv[0]->ptr, "psync")) {
    // 尝试进行 PSYNC
    if (masterTryPartialResynchronization(c) == REDIS_OK) {
      // 可执行 PSYNC
      server.stat_sync_partial_ok++;
      return; /* No full resync needed, return. */
    } else {
      // 不可执行 PSYNC
      char *master_runid = c->argv[1]->ptr;

      /* Increment stats for failed PSYNCs, but only if the
       * runid is not "?", as this is used by slaves to force a full
       * resync on purpose when they are not albe to partially
       * resync. */
      if (master_runid[0] != '?')
        server.stat_sync_partial_err++;
    }

  } else {
    //c->flags |= REDIS_PER_PSYNC;
  }

  if (server.rdb_child_pid != -1) {

  }

  c->repldbfd = -1;

  c->flags |= REDIS_SLAVE;

  server.slaveseldb = -1;

  listAddNodeTail(server.slaves, c);
  if (listLength(server.slaves) == 1 && server.repl_backlog == NULL) {
    // TODO 初始化backlog
  }

  return;
}
