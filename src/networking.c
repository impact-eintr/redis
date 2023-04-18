#include "adlist.h"
#include "color.h"
#include "redis.h"
#include "sds.h"
#include "util.h"
#include "ae.h"
#include "zmalloc.h"
#include "anet.h"

#include <asm-generic/errno-base.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

redisClient *createClient(int fd) {
  redisClient *c = zmalloc(sizeof(redisClient));

  // -1创建伪客户端 !-1 创建普通客户端
  if (fd != -1) {
    // 非阻塞
    anetNonBlock(NULL, fd);
    // 禁用Nagle算法
    anetEnableTcpNoDelay(NULL, fd);
    if (server.tcpkeepalive) {
      anetKeepAlive(NULL, fd, server.tcpkeepalive);
    }
    // NOTE 添加事件
    if (aeCreateFileEvent(server.el, fd, AE_READABLE, readQueryFromClient, c) == AE_ERR) {
      close(fd);
      zfree(c);
      return NULL;
    }

  }

  // 初始化属性
  selectDb(c, 0);
  c->fd = fd;
  c->name = NULL;
  c->querybuf = sdsempty();
  c->reqtype = 0;
  c->argc = 0;
  c->argv = NULL;
  c->flags = 0;

  if (fd != -1) {
    listAddNodeTail(server.clients, c);
  }

  return c;
}

void closeTimedoutClients(void);


// 同步释放客户端
void freeClient(redisClient *c) {
  // TODO

  zfree(c);
}

// 异步释放客户端
void freeClientAsync(redisClient *c) {

}

static void freeClientArgv(redisClient *c) {
  for (int j = 0;j < c->argc;j++) {
    decrRefCountVoid(c->argv[j]);
  }
  c->argc = 0;
  c->cmd = NULL;
}

// 在客户端执行完命令后重置客户端 准备执行下个指令
void resetClient(redisClient *c) {
  redisCommandProc *prevcmd = c->cmd ? c->cmd->proc : NULL;

  freeClientArgv(c);
  c->reqtype = 0;
  c->multibulklen = 0;
  c->bulklen = -1;
  // TODO AskCommand
  printf("释放 Client\n");
}

// 事件处理器 命令回复处理器
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {

}

// 为客户端安装写处理器到事件循环
void addReply(redisClient *c, robj *obj) {
  printf("测试 %s\n", (char *)obj->ptr);
}

void *addDeferredMultiBulkLength(redisClient *c);
void setDeferredMultiBulkLength(redisClient *c, void *node, long length);
void addReplySds(redisClient *c, sds s);
void processInputBuffer(redisClient *c);

#define MAX_ACCEPTS_PER_CALL 1000
static void acceptCommonHandler(int fd, int flags) {
  // 创建客户端
  redisClient *c;
  if ((c = createClient(fd)) == NULL) {
    redisLog(REDIS_WARNING,
             "Error registering fd event for the new client: %s (fd=%d)",
             strerror(errno), fd);
    close(fd);
    return;
  }

  if (listLength(server.clients) > server.maxclients) {
    char *err = "-ERR max number of clients reached\r\n";
    if (write(c->fd, err, strlen(err)) == -1) {}
    server.stat_rejected_conn++;
    freeClient(c);
    return;
  }

  server.stat_numconnections++;

  // 设置Flag
  c->flags |= flags;
  printf(YELLOWSTR("一个TCP连接到来\n"));
}

// 事件处理器 连接应答处理器
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
  int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
  char cip[REDIS_IP_STR_LEN];
  REDIS_NOTUSED(el);
  REDIS_NOTUSED(mask);
  REDIS_NOTUSED(privdata);

  while(max--) {
    // accept 客户端连接
    cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
    if (cfd == ANET_ERR) {
      if (errno != EWOULDBLOCK) {
        redisLog(REDIS_WARNING, "Accepting client connection: %s", server.neterr);
      }
      return;
    }
    redisLog(REDIS_VERBOSE, "Accepted %s:%d", cip, cport);
    acceptCommonHandler(cfd, REDIS_UNIX_SOCKET); // 为本地客户端创建客户端状态
  }
}

// 事件处理器 本地连接处理器
void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
  int cfd, max = MAX_ACCEPTS_PER_CALL;
  REDIS_NOTUSED(el);
  REDIS_NOTUSED(mask);
  REDIS_NOTUSED(privdata);

  while(max--) {
    // accept 客户端连接
    cfd = anetUnixAccept(server.neterr, fd);
    if (cfd == ANET_ERR) {
      if (errno != EWOULDBLOCK) {
        redisLog(REDIS_WARNING, "Accepting client connection: %s", server.neterr);
      }
      return;
    }
    redisLog(REDIS_VERBOSE, "Accepted connection to %s", server.unixsocket);
    acceptCommonHandler(cfd, REDIS_UNIX_SOCKET); // 为本地客户端创建客户端状态
  }
}

// 处理内联命令 并创建参数对象
// <arg0> <arg1> <arg2> <arg3>\r\n
int processInlineBuffer(redisClient *c) {
  // 使用c->querybuf 填充c的部分字段
  char *newline;
  int argc, j;
  sds *argv, aux;
  size_t querylen;

  newline = strchr(c->querybuf, '\n');
  // 收到的查询内容不符合协议格式，出错
  if (newline == NULL) {
    if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
      addReplyError(c, "Protocol error: too big inline request");
      //TODO setProtocolError(c, 0);
    }
    return REDIS_ERR;
  }

  /* Handle the \r\n case. */
  if (newline && newline != c->querybuf && *(newline - 1) == '\r')
    newline--;

  /* Split the input buffer up to the \r\n */
  // 根据空格，分割命令的参数
  // 比如说 SET msg hello \r\n 将分割为
  // argv[0] = SET
  // argv[1] = msg
  // argv[2] = hello
  // argc = 3
  querylen = newline - (c->querybuf);
  aux = sdsnewlen(c->querybuf, querylen);
  argv = sdssplitargs(aux, &argc);
  sdsfree(aux);
  if (argv == NULL) {
    addReplyError(c, "Protocol error: unbalanced quotes in request");
    //setProtocolError(c, 0);
    return REDIS_ERR;
  }

  /* Newline from slaves can be used to refresh the last ACK time.
   * This is useful for a slave to ping back while loading a big
   * RDB file. */
  if (querylen == 0 && c->flags & REDIS_SLAVE) {
    // TODO c->repl_ack_time = server.unixtime;
  }

  /* Leave data after the first line of the query in the buffer */

  // 从缓冲区中删除已 argv 已读取的内容
  // 剩余的内容是未读取的
  sdsrange(c->querybuf, querylen + 2, -1);

  /* Setup argv array on client structure */
  // 为客户端的参数分配空间
  if (c->argv) {
    zfree(c->argv);
  }
  c->argv = zmalloc(sizeof(robj *) * argc);

  // 为每个参数创建一个字符串对象
  for (c->argc = 0,j = 0;j < argc;j++) {
    if (sdslen(argv[j])) {
      c->argv[c->argc] = createObject(REDIS_STRING, argv[j]);
      c->argc++;
    } else {
      sdsfree(argv[j]);
    }
  }

  zfree(argv);
  return REDIS_OK;
}

int processMultibulkBuffer(redisClient *c) {
  return REDIS_OK;
}

void processInputBuffer(redisClient *c) {
  while(sdslen(c->querybuf)) {
    // TODO 处理客户端在各种状态下的行为

    // 简单来说，多条查询是一般客户端发送来的，
    // 而内联查询则是 TELNET 发送来的
    if (!c->reqtype) {
      if (c->querybuf[0] == '*') {
        // 多条查询
        c->reqtype = REDIS_REQ_MULTIBULK;
      } else {
        // 内联查询
        c->reqtype = REDIS_REQ_INLINE;
      }
    }

    if (c->reqtype == REDIS_REQ_INLINE) {
      if (processInlineBuffer(c) != REDIS_OK) {
        break;
      }
    } else if (c->reqtype == REDIS_REQ_MULTIBULK) {
      if (processMultibulkBuffer(c) != REDIS_OK) {
        break;
      }
    } else {
      redisPanic("Unknown request type");
    }

    if (c->argc == 0) {
      resetClient(c);
    } else {
      if (processCommand(c) == REDIS_OK) {
        resetClient(c);
      }
    }
  }
}

// 事件处理器 命令请求处理器
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
  redisClient *c = (redisClient *)privdata;
  int nread, readlen;
  size_t qblen;
  REDIS_NOTUSED(el);
  REDIS_NOTUSED(mask);

  // 设置服务器当前处理的客户端
  server.current_client = c;
  readlen = REDIS_IOBUF_LEN;

  // TODO

  qblen = sdslen(c->querybuf);
  if (c->querybuf_peak < qblen) {
    c->querybuf_peak = qblen;
  }
  c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);
  nread = read(fd, c->querybuf+qblen, readlen); // 这里是一个阻塞的系统调用

  if (nread == -1) { // 遇到EOF
    if (errno == EAGAIN) {
      nread = 0;
    } else {
      redisLog(REDIS_VERBOSE, "Reading from client: %s", strerror(errno));
      freeClient(c);
      return;
    }
  } else if (nread == 0) {
    redisLog(REDIS_VERBOSE, "Client closed connection");
    freeClient(c);
    return;
  }

  if (nread) {
    char s[nread];
    snprintf(s, nread, "%s", c->querybuf+qblen);
    printf("处理客户端请求 %s\n", s);

    sdsIncrLen(c->querybuf, nread);
    // TODO c->lastinteraction = server.unixtime;
    // TODO 处理master 更新复制偏移量
  } else {
    server.current_client = NULL;
    return;
  }

  processInputBuffer(c);
  server.current_client = NULL;
}

void addReplyBulk(redisClient *c, robj *obj);
void addReplyBulkCString(redisClient *c, char *s);
void addReplyBulkCBuffer(redisClient *c, void *p, size_t len);
void addReplyBulkLongLong(redisClient *c, long long ll);
void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void addReply(redisClient *c, robj *obj);
void addReplySds(redisClient *c, sds s);

// 将 c String 中的内容复制到回复缓冲区
void addReplyString(redisClient *c, char *s, size_t len) {
  // TODO
  printf(REDSTR("%s"), s);
}

void addReplyErrorLength(redisClient *c, char *s, size_t len) {
  addReplyString(c, "-ERR", 5);
  addReplyString(c, s, len);
  addReplyString(c, "\r\n", 2);
}

void addReplyError(redisClient *c, char *err) {
  addReplyErrorLength(c, err, strlen(err));
}

void addReplyStatusLength(redisClient *c, char *s, size_t len) {
  addReplyString(c, "+", 1);
  addReplyString(c, s, len);
  addReplyString(c, "\r\n", 2);
}

/*
 * 返回一个状态回复
 *
 * 例子 +OK\r\n
 */
void addReplyStatus(redisClient *c, char *status) {
  addReplyStatusLength(c, status, strlen(status));
}

void addReplyDouble(redisClient *c, double d);

/* Add a long long as integer reply or bulk len / multi bulk count.
 *
 * 添加一个 long long 为整数回复，或者 bulk 或 multi bulk 的数目
 *
 * Basically this is used to output <prefix><long long><crlf>.
 *
 * 输出格式为 <prefix><long long><crlf>
 *
 * 例子:
 *
 * *5\r\n10086\r\n
 *
 * $5\r\n10086\r\n
 */
void addReplyLongLongWithPrefix(redisClient *c, long long ll, char prefix) {
  char buf[128];
  int len;

  /* Things like $3\r\n or *2\r\n are emitted very often by the protocol
   * so we have a few shared objects to use if the integer is small
   * like it is most of the times. */
  if (prefix == '*' && ll < REDIS_SHARED_BULKHDR_LEN) {
    // 多条批量回复
    addReply(c, shared.mbulkhdr[ll]);
    return;
  } else if (prefix == '$' && ll < REDIS_SHARED_BULKHDR_LEN) {
    // 批量回复
    addReply(c, shared.bulkhdr[ll]);
    return;
  }

  buf[0] = prefix;
  len = ll2string(buf + 1, sizeof(buf) - 1, ll);
  buf[len + 1] = '\r';
  buf[len + 2] = '\n';
  addReplyString(c, buf, len + 3);
}

/*
 * 返回一个整数回复
 *
 * 格式为 :10086\r\n
 */
void addReplyLongLong(redisClient *c, long long ll) {
  if (ll == 0)
    addReply(c, shared.czero);
  else if (ll == 1)
    addReply(c, shared.cone);
  else
    addReplyLongLongWithPrefix(c, ll, ':');
}

void addReplyMultiBulkLen(redisClient *c, long length);
void copyClientOutputBuffer(redisClient *dst, redisClient *src);
void *dupClientReplyValue(void *o);
void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer);
void formatPeerId(char *peerid, size_t peerid_len, char *ip, int port);
char *getClientPeerId(redisClient *client);
sds catClientInfoString(sds s, redisClient *client);
sds getAllClientsInfoString(void);
void rewriteClientCommandVector(redisClient *c, int argc, ...);
void rewriteClientCommandArgument(redisClient *c, int i, robj *newval);
unsigned long getClientOutputBufferMemoryUsage(redisClient *c);
void freeClientsInAsyncFreeQueue(void);
void asyncCloseClientOnOutputBufferLimitReached(redisClient *c);
int getClientLimitClassByName(char *name);
char *getClientLimitClassName(int class);
void flushSlavesOutputBuffers(void);
void disconnectSlaves(void);
int listenToPort(int port, int *fds, int *count);
void pauseClients(mstime_t duration);
int clientsArePaused(void);
int processEventsWhileBlocked(void);
