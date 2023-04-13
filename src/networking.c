#include "color.h"
#include "redis.h"
#include "util.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

redisClient *createClient(int fd);
void closeTimedoutClients(void);
void freeClient(redisClient *c);
void freeClientAsync(redisClient *c);
void resetClient(redisClient *c);
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask);

// 为客户端安装写处理器到事件循环
void addReply(redisClient *c, robj *obj) {
  printf("测试 %s\n", (char *)obj->ptr);
}

void *addDeferredMultiBulkLength(redisClient *c);
void setDeferredMultiBulkLength(redisClient *c, void *node, long length);
void addReplySds(redisClient *c, sds s);
void processInputBuffer(redisClient *c);
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask);
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
