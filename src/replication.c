#include "adlist.h"
#include "ae.h"
#include "anet.h"
#include "redis.h"
#include "sds.h"
#include "zmalloc.h"
#include "rdb.h"
#include <stdarg.h>
#include <sys/socket.h>
#include <errno.h>
#include <time.h>

/*
** host1 (master)
** host2 (slave)
**
** STAGE 1
** cli -slaveof host1-> host2
** host2.slaveCommand 处理 设置repl_state 为 REDIS_CONNECT
** host2.replicationCron 每秒执行一次 检测到 repl_state 变更为 REDIS_CONNECT 调用connectWithMaster 尝试连接 host1(master)
** connectWithMaster 设置repl_state 为 REDIS_CONNECTING 并与 master 建立非阻塞连接 异步地调用 syncWithMaster (读写事件处理器监听)
** syncWithMaster 向master发送一条 PING 设置repl_state 为 REDIS_REPL_RECEIVE_PONG
**
** STAGE 2
** master 收到一条 PING 回复一个 PONG
** slave.syncWithMaster 收到读事件读取 PONG 执行REDIS_REPL_RECEIVE_PONG的分支
** slave 向 master 发送 REPLCONF listening-port
**
** STAGE 3
** master 执行 replconfCommand 记录slave的 port 并回复ok
** slave 收到ok 执行 PSYNC 指令
**
** STAGE 3.1
** slave 没有缓存master 执行全同步
**
** STAGE 3.1
** slave 缓存了master 执行部分同步
**
**
**
**
**
**
**
*/

// 创建backlog
void createReplicationBacklog(void) {
  redisAssert(server.repl_backlog == NULL);
  server.repl_backlog = zmalloc(server.repl_backlog_size);
  server.repl_backlog_histlen = 0;
  server.repl_backlog_idx = 0;
  server.master_repl_offset++;
  server.repl_backlog_off = server.master_repl_offset+1;
}

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
int masterTryPartialResynchronization(redisClient *c) {
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
    c->flags |= REDIS_PRE_PSYNC;
  }

  server.stat_sync_full++;

  // 检查是否有 BGSAVE 在执行
  if (server.rdb_child_pid != -1) {
    redisClient *slave;
    listNode *ln;
    listIter li;

    // 如果有至少一个 slave 在等待这个 BGSAVE 完成
    // 那么说明正在进行的 BGSAVE 所产生的 RDB 也可以为其他 slave 所用
    listRewind(server.slaves, &li);
    while ((ln = listNext(&li))) {
      slave = ln->value;
      if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END)
        break;
    }

    if (ln) {
      // 幸运的情况，可以使用目前 BGSAVE 所生成的 RDB
      copyClientOutputBuffer(c, slave);
      c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
      redisLog(REDIS_NOTICE, "Waiting for end of BGSAVE for SYNC");
    } else {
      /* No way, we need to wait for the next BGSAVE in order to
       * register differences */
      // 不好运的情况，必须等待下个 BGSAVE
      c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
      redisLog(REDIS_NOTICE, "Waiting for next BGSAVE for SYNC");
    }

  } else {
    // 没有 BGSAVE 在进行，开始一个新的 BGSAVE
    redisLog(REDIS_NOTICE, "Starting BGSAVE for SYNC");
    if (rdbSaveBackground(server.rdb_filename) != REDIS_OK) {
      redisLog(REDIS_NOTICE, "Replication failed, can't BGSAVE");
      addReplyError(c, "Unable to perform background save");
      return;
    }
    // 设置状态
    c->replstate = REDIS_REPL_WAIT_BGSAVE_END;
    /* Flush the script cache for the new slave. */
    // 因为新 slave 进入，刷新复制脚本缓存
    // TODO replicationScriptCacheFlush();
  }

  if (server.repl_disable_tcp_nodelay) {
    anetDisableTcpNoDelay(NULL, c->fd);
  }

  c->repldbfd = -1;

  c->flags |= REDIS_SLAVE;

  server.slaveseldb = -1;

  // 添加到 slave 列表中
  listAddNodeTail(server.slaves, c);
  if (listLength(server.slaves) == 1 && server.repl_backlog == NULL) {
    // 初始化backlog
    createReplicationBacklog();
  }

  return;
}

// Redis 通常情况下是将命令的发送和回复用不同的事件处理器来异步处理的
// 但这里是同步地发送然后读取
char *sendSynchronousCommand(int fd, ...) {
  va_list ap;
  sds cmd = sdsempty();
  char *arg, buf[256];

  /* Create the command to send to the master, we use simple inline
   * protocol for simplicity as currently we only send simple strings. */
  va_start(ap, fd);
  while (1) {
    arg = va_arg(ap, char *);
    if (arg == NULL)
      break;

    if (sdslen(cmd) != 0) {
      cmd = sdscatlen(cmd, " ", 1);
    }
    cmd = sdscat(cmd, arg);
  }
  va_end(ap);
  cmd = sdscatlen(cmd, "\r\n", 2);

  /* Transfer command to the server. */
  // 发送命令到主服务器
  if (syncWrite(fd, cmd, sdslen(cmd), server.repl_syncio_timeout * 1000) ==
      -1) {
    sdsfree(cmd);
    return sdscatprintf(sdsempty(), "-Writing to master: %s", strerror(errno));
  }
  sdsfree(cmd);

  /* Read the reply from the server. */
  // 从主服务器中读取回复
  if (syncReadLine(fd, buf, sizeof(buf), server.repl_syncio_timeout * 1000) ==
      -1) {
    return sdscatprintf(sdsempty(), "-Reading from master: %s",
                        strerror(errno));
  }
  return sdsnew(buf);
}

#define PSYNC_CONTINUE 0
#define PSYNC_FULLRESYNC 1
#define PSYNC_NOT_SUPPORTED 2
// slave 尝试同步 master
int slaveTryPartialResynchronization(int fd) {
  char *psync_runid;
  char psync_offset[32];
  sds reply;

  server.repl_master_initial_offset = -1;

  if (server.cached_master) { // cache hit
    // 发送 "PSYNC <master_run_id> <repl_offset>"
    psync_runid = server.cached_master->replrunid;
    snprintf(psync_offset, sizeof(psync_offset), "%lld",
             server.cached_master->reploff + 1);
    redisLog(REDIS_NOTICE,
             "Trying a partial resynchronization (request %s:%s).", psync_runid,
             psync_offset);
  } else { // cache miss
    // 发送 "PSYNC ? -1" 要求完整同步
    redisLog(REDIS_NOTICE, "Partial resynchtonization (request %s:%s).", psync_runid, psync_offset);
    psync_runid = "?";
    memcpy(psync_offset, "-1", 3);
  }

  reply = sendSynchronousCommand(fd, "PSYNC", psync_runid, psync_offset);

  // 接收到 FULLRESYNC ，进行 full-resync
  if (!strncmp(reply, "+FULLRESYNC", 11)) {
  }

  // 接收到 CONTINUE ，进行 partial resync
  if (!strncmp(reply, "+CONTINUE", 9)) {
  }

  /* If we reach this point we receied either an error since the master does
   * not understand PSYNC, or an unexpected reply from the master.
   * Return PSYNC_NOT_SUPPORTED to the caller in both cases. */

  // 接收到错误？
  if (strncmp(reply, "-ERR", 4)) {
  }
  sdsfree(reply);
  replicationDiscardCachedMaster();

  // 主服务器不支持 PSYNC
  return PSYNC_NOT_SUPPORTED;

}

void syncWithMaster(aeEventLoop *el, int fd, void *privdata, int mask) {
  char tmpfile[256], *err;
  int dfd, maxtries = 5;
  int sockerr = 0, psync_result;
  socklen_t errlen = sizeof(sockerr);
  REDIS_NOTUSED(el);
  REDIS_NOTUSED(privdata);
  REDIS_NOTUSED(mask);

  // 如果处于 SLAVEOF NO ONE 模式 那么关闭 fd
  if (server.repl_state == REDIS_REPL_NONE) {
    close(fd);
    return;
  }

  // 检查套接字错误
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &errlen) == -1) {
    sockerr = errno;
  }
  if (sockerr) {
    aeDeleteFileEvent(server.el, fd, AE_READABLE|AE_WRITABLE);
    redisLog(REDIS_WARNING, "Error condition on socket for SYNC: %s", strerror(sockerr));
    goto error;
  }

  switch (server.repl_state) {
  case REDIS_REPL_CONNECTING:
    redisLog(REDIS_NOTICE, "Non blocking connect for SYNC fired the event.");
    aeDeleteFileEvent(server.el, fd,
                      AE_WRITABLE); // 手动发送一个同步 PING 暂时取消写事件
    server.repl_state = REDIS_REPL_RECEIVE_PONG; // 更新状态
    syncWrite(fd, "PING\r\n", 6, 100);
    return;

  case REDIS_REPL_RECEIVE_PONG:
    aeDeleteFileEvent(server.el, fd, AE_READABLE);
    char buf[1024];
    buf[0] = 0;
    if (syncReadLine(fd, buf, sizeof(buf), server.repl_syncio_timeout * 1000) == -1) {
      redisLog(REDIS_WARNING, "I/O error reading PING reply from master: %s",
               strerror(errno));
      goto error;
    }

    if (buf[0] != '+' && strncmp(buf, "-NOAUTH", 7) != 0 &&
        strncmp(buf, "-ERR operation not permitted", 28) != 0) {
      // 接收到未验证错误
      redisLog(REDIS_WARNING, "Error reply to PING from master: '%s'", buf);
      goto error;
    } else {
      // 接收到 PONG
      redisLog(REDIS_NOTICE,
               "Master replied to PING, replication can continue...");
    }
    break;

  default:
    break;
  }

  // 接收PONG 命令
  if (server.masterauth) {
    // TODO 验证身份
  }

  {
    // 给master添加 slave 的 port
    sds port = sdsfromlonglong(server.port);
    err = sendSynchronousCommand(fd, "REPLCONF", "listening-port", port);
    sdsfree(port);
    /* Ignore the error if any, not all the Redis versions support
     * REPLCONF listening-port. */
    if (err[0] == '-') {
      redisLog(REDIS_NOTICE,
               "(Non critical) Master does not understand REPLCONF "
               "listening-port: %s",
               err);
    }
    sdsfree(err);
  }

  // TODO 接着写这里 执行resync 或者 full-resync
  psync_result = slaveTryPartialResynchronization(fd);


  return;

error:
  close(fd);
  server.repl_transfer_s = -1;
  server.repl_state = REDIS_REPL_CONNECT;
  return;
}

// 以非阻塞的方式连接主服务器
int connectWithMaster() {
  int fd;
  fd = anetTcpNonBlockConnect(NULL, server.masterhost, server.masterport);
  if (fd == -1) {
    redisLog(REDIS_WARNING, "Unable to connect to MASTER: %s", strerror(errno));
    return REDIS_ERR;
  }

  // 监听主服务器 fd 的 读写事件
  if (aeCreateFileEvent(server.el, fd, AE_READABLE | AE_WRITABLE,
                        syncWithMaster, NULL) == AE_ERR) {
    close(fd);
    redisLog(REDIS_WARNING, "Unable to connect to MASTER: %s", strerror(errno));
    return REDIS_ERR;
  }

  // 初始化统计变量
  server.repl_transfer_lastio = server.unixtime;
  server.repl_transfer_s = fd;
  server.repl_state = REDIS_REPL_CONNECTING;
  return REDIS_OK;
}

void replconfCommand(redisClient *c) {
  int j;

  if ((c->argc % 2) == 0) {
    addReply(c, shared.syntaxerr);
    return;
  }

  for (j = 1; j < c->argc; j+=2) {
    if (!strcasecmp(c->argv[j]->ptr, "listening-port")) {
      // 从服务器发来 REPLCONF listening-port <port> 命令
      // master 将 slave 的 port 记录下来
      long port;
      if ((getLongFromObjectOrReply(c, c->argv[j+1], &port, NULL) != REDIS_OK)) {
        return;
      }
      c->slave_listening_port = port;
      redisLog(REDIS_NOTICE, "SLAVE_LISTENING_PORT: %d", c->slave_listening_port);
    } else if (!strcasecmp(c->argv[j]->ptr, "ack")) {
      // TODO

    } else if (!strcasecmp(c->argv[j]->ptr, "getack")) {
      // TODO

    } else {
      // TODO

      return;
    }
  }

  addReply(c, shared.ok);
}

void replicationCron(void) {
  if (server.masterhost &&
      (server.repl_state == REDIS_REPL_CONNECTING ||
       server.repl_state == REDIS_REPL_RECEIVE_PONG) &&
      (time(NULL) - server.repl_transfer_lastio) > server.repl_timeout) {
    redisLog(REDIS_WARNING, "Timeout connecting to the MASTER");
    // TODO 取消连接
    printf("取消连接\n");
  }

  if (server.repl_state == REDIS_REPL_CONNECT) {
    redisLog(REDIS_NOTICE, "Connecting to the MASTER %s:%d", server.masterhost,
             server.masterport);
    if (connectWithMaster() == REDIS_OK) {
      redisLog(REDIS_NOTICE, "MASTER <-> SLAVE sync started");
    }
  }
}
