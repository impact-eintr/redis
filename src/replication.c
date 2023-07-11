#include "adlist.h"
#include "ae.h"
#include "anet.h"
#include "color.h"
#include "config.h"
#include "rdb.h"
#include "redis.h"
#include "sds.h"
#include "zmalloc.h"
#include <asm-generic/errno-base.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

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
      printf("不可执行 PSYNC: %s\n", master_runid);

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
    if (arg == NULL) {
      va_end(ap);
      break;
    }

    if (sdslen(cmd) != 0) {
      cmd = sdscatlen(cmd, " ", 1);
    }
    cmd = sdscat(cmd, arg);
  }
  cmd = sdscatlen(cmd, "\r\n", 2);
  printf("CMD %s\n", cmd);

  /* Transfer command to the server. */
  // 发送命令到主服务器
  if (syncWrite(fd, cmd, sdslen(cmd), server.repl_syncio_timeout * 1000) == -1) {
    sdsfree(cmd);
    return sdscatprintf(sdsempty(), "-Writing to master: %s", strerror(errno));
  }
  sdsfree(cmd);

  /* Read the reply from the server. */
  // 从主服务器中读取回复
  if (syncReadLine(fd, buf, sizeof(buf), server.repl_syncio_timeout * 1000) == -1) {
    return sdscatprintf(sdsempty(), "-Reading from master: %s",
                        strerror(errno));
  }
  printf("MASTER RESPONSE: %s\n", buf);
  return sdsnew(buf);
}

// 将缓存中的 master 设置为服务器当前的 master
void replicationResurrectCachedMaster(int newfd) {
  server.master = server.cached_master;
  server.cached_master = NULL;
  server.master->fd = newfd;
  server.master->flags &= ~(REDIS_CLOSE_AFTER_REPLY | REDIS_CLOSE_ASAP);

  server.repl_state = REDIS_REPL_CONNECTED;
  listAddNodeTail(server.clients, server.master);
  // 监听 Master 的读事件
  if (aeCreateFileEvent(server.el, newfd, AE_READABLE, readQueryFromClient,
                        server.master)) {
    redisLog(REDIS_WARNING,
             "Error resurrecting the cached master, impossible to add the "
             "readable handler: %s",
             strerror(errno));
    freeClientAsync(server.master);
  }
  // 监听 Master 的写事件
  if (server.master->bufpos || listLength(server.master->reply)) {
    if (aeCreateFileEvent(server.el, newfd, AE_READABLE, readQueryFromClient,
                          server.master)) {
      redisLog(REDIS_WARNING,
               "Error resurrecting the cached master, impossible to add the "
               "writable handler: %s",
               strerror(errno));
      freeClientAsync(server.master);
    }
  }
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
    redisLog(REDIS_NOTICE, "Partial resynchtonization (no cached mastger).");
    psync_runid = "?";
    memcpy(psync_offset, "-1", 3);
  }

  reply = sendSynchronousCommand(fd, "PSYNC", psync_runid, psync_offset, NULL);

  // 接收到 FULLRESYNC ，进行 full-resync
  if (!strncmp(reply, "+FULLRESYNC", 11)) {
    char *runid = NULL, *offset = NULL;
    runid = strchr(reply, ' ');
    if (runid) {
      runid++;
      offset = strchr(runid, ' ');
      if (offset) {
        offset++;
      }
    }
    if (!runid || !offset || (offset - runid - 1) != REDIS_RUN_ID_SIZE) {
      // 主服务器支持 PSYNC 但是发来了异常的 run id
      memset(server.repl_master_runid, 0, REDIS_RUN_ID_SIZE + 1);
      redisLog(REDIS_WARNING, "Master replied with wrong +FULLRESYNC syntax");
    } else {
      // 保存 run id
      memcpy(server.repl_master_runid, runid, offset - runid - 1);
      server.repl_master_runid[REDIS_RUN_ID_SIZE] = '\0';
      server.repl_master_initial_offset = strtoll(offset, NULL, 10);
      // 打印日志，这是一个 FULL resync
      redisLog(REDIS_NOTICE, "Full resync from master: %s:%lld",
               server.repl_master_runid, server.repl_master_initial_offset);
    }

    replicationDiscardCachedMaster(); // 要开始完整重同步 缓存中的master没用了
    sdsfree(reply);
    return PSYNC_FULLRESYNC;
  }

  // 接收到 CONTINUE ，进行 partial resync
  if (!strncmp(reply, "+CONTINUE", 9)) {
    redisLog(REDIS_NOTICE, "Successful partial resynchronization with master.");
    sdsfree(reply);
    // 将缓存中的master设置为当前的master
    replicationResurrectCachedMaster(fd);
    return PSYNC_CONTINUE;
  }

  /* If we reach this point we receied either an error since the master does
   * not understand PSYNC, or an unexpected reply from the master.
   * Return PSYNC_NOT_SUPPORTED to the caller in both cases. */

  // 接收到错误？
  if (strncmp(reply, "-ERR", 4)) {
    redisLog(REDIS_WARNING, "Unexpected reply to PSYNC from master: %s", reply);
  } else {
    redisLog(REDIS_NOTICE, "Master does not support PSYNC or is in error state (reply: %s)", reply);
  }
  sdsfree(reply);
  replicationDiscardCachedMaster();

  // 主服务器不支持 PSYNC
  return PSYNC_NOT_SUPPORTED;
}

void readSyncBulkPayload(aeEventLoop *el, int fd, void *privdata, int mask) {
  printf("读取数据 RDB\n");
  sleep(1);
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
    err = sendSynchronousCommand(fd, "REPLCONF", "listening-port", port, NULL);
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

  // 接着写这里 执行resync 或者 full-resync
  psync_result = slaveTryPartialResynchronization(fd);

  if (psync_result == PSYNC_CONTINUE) {
    redisLog(
        REDIS_NOTICE,
        "MASTER <-> SLAVE sync: Master accepted a Partial Resynchronization.");
    // 返回
    return;
  }

  if (psync_result == PSYNC_NOT_SUPPORTED) { // 不支持 PSYNC
    redisLog(REDIS_NOTICE, "Retrying with SYNC...");
    // 向主服务器发送 SYNC 命令
    if (syncWrite(fd, "SYNC\r\n", 6, server.repl_syncio_timeout * 1000) == -1) {
      redisLog(REDIS_WARNING, "I/O error writing to MASTER: %s",
               strerror(errno));
      goto error;
    }
  }

  // 如果执行到这里 REDIS_FULLRESYNC or  PSYNC_NOT_SUPPORTED
  while (maxtries--) { // 打开临时文件保存从主服务器下载的 RDB
    snprintf(tmpfile, 256, "temp-%d.%ld.rdb", (int)server.unixtime,
             (long int)getpid());
    dfd = open(tmpfile, O_CREAT | O_WRONLY | O_EXCL, 0644);
    if (dfd != -1)
      break;
    sleep(1);
  }
  if (dfd == -1) {
    redisLog(
        REDIS_WARNING,
        "Opening the temp file needed for MASTER <-> SLAVE synchronization: %s",
        strerror(errno));
    goto error;
  }

  // 设置一个读事件处理器
  if (aeCreateFileEvent(server.el, fd, AE_READABLE, readSyncBulkPayload, NULL)) {
    redisLog(REDIS_WARNING, "Can't create readable event for SYNC: %s (fd=%d)",
             strerror(errno), fd);
    goto error;
  }
  // 设置状态
  server.repl_state = REDIS_REPL_TRANSFER;

  // 更新统计信息
  server.repl_transfer_size = -1;
  server.repl_transfer_read = 0;
  //server.repl_transfer_last_fsync_off = 0;
  server.repl_transfer_fd = dfd;
  server.repl_transfer_lastio = server.unixtime;
  server.repl_transfer_tmpfile = zstrdup(tmpfile);

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

void sendBulkToSlave(aeEventLoop *el, int fd, void *privdata, int mask) {
  printf("向slave写数据\n");
  redisClient *slave = privdata;
  REDIS_NOTUSED(el);
  REDIS_NOTUSED(mask);
  char buf[REDIS_IOBUF_LEN];
  ssize_t nwritten, buflen;

  if (slave->replpreamble) {
    exit(1);
  }

  lseek(slave->repldbfd, slave->repldboff, SEEK_SET);
  buflen = read(slave->repldbfd, buf, REDIS_IOBUF_LEN);
  if (buflen <= 0) {
    redisLog(REDIS_WARNING, "Read error sending DB to slave: %s",
             (buflen == 0) ? "premature EOF" : strerror(errno));
    freeClient(slave);
    return;
  }

  // 写入数据到slave
  if ((nwritten = write(fd, buf, buflen)) == -1) {
    if (errno != EAGAIN) {
      redisLog(REDIS_WARNING, "Write error sending DB to slave: %s", strerror(errno));
      freeClient(slave);
    }
    return;
  }

  // 如果写入成功 南岸更新写入字节
  slave->repldboff += nwritten;

  // 如果写入已经完成
  if (slave->repldboff == slave->repldbsize) {
    close(slave->repldbfd);
    slave->repldbfd = -1;
    aeDeleteFileEvent(server.el, slave->fd, AE_WRITABLE); // 写完了 取消写事件
    printf("结束\n");
  }
  sleep(1);
}

void updateSlavesWaitingBgsave(int bgsaveerr) {
  listNode *ln;
  int startbgsave = 0;
  listIter li;
  listRewind(server.slaves, &li);
  while ((ln = listNext(&li))) {
    redisClient *slave = ln->value;
    if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_START) {
      startbgsave = 1; // 之前的不能用
      slave->replstate = REDIS_REPL_WAIT_BGSAVE_END;
    } else if (slave->replstate == REDIS_REPL_WAIT_BGSAVE_END) {
      struct redis_stat buf;

      if (bgsaveerr != REDIS_OK) {
        freeClient(slave);
        redisLog(REDIS_WARNING, "SYNC failed. Can not open/stat DB after BGSAVE: %s", strerror(errno));
        continue;
      }

      // 打开 RDB 文件 准备 dup 到 master 与 slave 的tcpfd中
      if ((slave->repldbfd = open(server.rdb_filename, O_RDONLY)) == -1 ||
          redis_fstat(slave->repldbfd, &buf) == -1) {
        freeClient(slave);
        redisLog(REDIS_WARNING,
                 "SYNC failed. Can not open/stat DB after BGSAVE: %s",
                 strerror(errno));
        continue;
      }
      slave->repldboff = 0;
      slave->repldbsize = buf.st_size;
      slave->replstate = REDIS_REPL_SEND_BULK; // 接收RDB文件中
      // 更新写事件处理器
      aeDeleteFileEvent(server.el, slave->fd, AE_WRITABLE);
      if (aeCreateFileEvent(server.el, slave->fd, AE_WRITABLE, sendBulkToSlave, slave) == AE_ERR) {
        freeClient(slave);
      }
    }
  }

  // 重新执行bgsave
  if (startbgsave) {
    // TODO 清空脚本缓存
    if (rdbSaveBackground(server.rdb_filename) != REDIS_OK) {
      listIter li;
      listRewind(server.slaves, &li);
    }
  }
  redisLog(REDIS_NOTICE, "通知slave 领取RDB");
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
