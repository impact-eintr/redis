#include "adlist.h"
#include "color.h"
#include "redis.h"
#include "sds.h"
#include "util.h"
#include "ae.h"
#include "version.h"
#include "zmalloc.h"
#include "anet.h"

#include <asm-generic/errno-base.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void setProtocolError(redisClient *c, int pos);


size_t zmalloc_size_sds(sds s) {
  return zmalloc_size(s-sizeof(struct sdshdr));
}

size_t getStringObjectSdsUsedMemory(robj *o) {
  redisAssertWithInfo(NULL, o, o->type == REDIS_STRING);
  switch (o->encoding) {
  case REDIS_ENCODING_RAW:
    return zmalloc_size_sds(o->ptr);
  case REDIS_ENCODING_EMBSTR:
    return zmalloc_size_sds(o->ptr);
  default:
    return 0;
  }
}

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
  c->bufpos = 0;
  c->querybuf = sdsempty();
  c->querybuf_peak = 0;
  c->reqtype = 0;
  c->argc = 0;
  c->argv = NULL;
  c->cmd = c->lastcmd = NULL;
  c->multibulklen = 0; // querybuf中未读入的命令内容数量
  c->bulklen = -1; // 读入的参数的长度
  c->sentlen = 0; // 已发送的字节数
  c->flags = 0;

  // 回复
  c->reply = listCreate();
  c->reply_bytes = 0;


  if (fd != -1) {
    listAddNodeTail(server.clients, c);
  }

  return c;
}

int prepareClientToWrite(redisClient *c) {
  if (c->flags & REDIS_LUA_CLIENT) {
    return REDIS_OK;
  }

  if ((c->flags & REDIS_MASTER) &&
      !(c->flags & REDIS_MASTER_FORCE_REPLY)) {
    return REDIS_ERR;
  }

  if (c->fd <= 0) {
    return REDIS_ERR;
  }

  // 注册写事件到el中 一旦可写就会调用
  if (aeCreateFileEvent(server.el, c->fd, AE_WRITABLE, sendReplyToClient, c) == AE_ERR) {
    return REDIS_ERR;
  }

  return REDIS_OK;
}

robj *dupLastObjectIfNeeded(list *reply) {
  robj *new, *cur;
  listNode *ln;
  redisAssert(listLength(reply) > 0);
  ln = listLast(reply);
  cur = listNodeValue(ln);
  if (cur->refcount > 1) {
    new = dupStringObject(cur);
    decrRefCount(cur);
    listNodeValue(ln) = new;
  }
  return listNodeValue(ln);
}

// 尝试将回复添加到 c->buf 中
int _addReplyToBuffer(redisClient *c, char *s, size_t len) {
  size_t available = sizeof(c->buf) - c->bufpos;

  if (c->flags & REDIS_CLOSE_AFTER_REPLY) {
    return REDIS_OK;
  }

  if (listLength(c->reply) > 0) {
    return REDIS_ERR; // 链表中有内容 不添加到buf中
  }

  if (len > available) {
    return REDIS_ERR;
  }

  memcpy(c->buf+c->bufpos, s, len);
  c->bufpos += len;

  return REDIS_OK;
}

// 尝试将回复添加到 c->reply 中
void _addReplyObjectToList(redisClient *c, robj *o) {

  assert(0);
}

// 和 _addReplyObjectToList 类似，但会负责 SDS 的释放功能（如果需要的话）
void _addReplySdsToList(redisClient *c, sds s) {
    robj *tail;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) {
        sdsfree(s);
        return;
    }

    if (listLength(c->reply) == 0) {
        listAddNodeTail(c->reply,createObject(REDIS_STRING,s));
        c->reply_bytes += zmalloc_size_sds(s);
    } else {
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL && tail->encoding == REDIS_ENCODING_RAW &&
            sdslen(tail->ptr)+sdslen(s) <= REDIS_REPLY_CHUNK_BYTES)
        {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr,s,sdslen(s));
            c->reply_bytes += zmalloc_size_sds(tail->ptr);
            sdsfree(s);
        } else {
            listAddNodeTail(c->reply,createObject(REDIS_STRING,s));
            c->reply_bytes += zmalloc_size_sds(s);
        }
    }
    assert(0);
    // TODO asyncCloseClientOnOutputBufferLimitReached(c);
}

void _addReplyStringToList(redisClient *c, char *s, size_t len) {
    robj *tail;

    if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

    if (listLength(c->reply) == 0) {
        // 为字符串创建字符串对象并追加到回复链表末尾
        robj *o = createStringObject(s,len);

        listAddNodeTail(c->reply,o);
        c->reply_bytes += getStringObjectSdsUsedMemory(o);
    } else {
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        if (tail->ptr != NULL && tail->encoding == REDIS_ENCODING_RAW &&
            sdslen(tail->ptr)+len <= REDIS_REPLY_CHUNK_BYTES)
        {
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            // 将字符串拼接到一个 SDS 之后
            tail->ptr = sdscatlen(tail->ptr,s,len);
            c->reply_bytes += zmalloc_size_sds(tail->ptr);
        } else {
            // 为字符串创建字符串对象并追加到回复链表末尾
            robj *o = createStringObject(s,len);

            listAddNodeTail(c->reply,o);
            c->reply_bytes += getStringObjectSdsUsedMemory(o);
        }
    }
    assert(0);
    // TODO asyncCloseClientOnOutputBufferLimitReached(c);
}


void closeTimedoutClients(void);

static void freeClientArgv(redisClient *c) {
  int j;
  for (j = 0;j < c->argc; j++) {
    decrRefCount(c->argv[j]);
  }
  c->argc = 0;
  c->cmd = NULL;
}

// 同步释放客户端
void freeClient(redisClient *c) {
  listNode *ln;
  if (server.current_client == c) {
    server.current_client = NULL;
  }

  // TODO 退订所有频道和模式


  if (c->fd != -1) {
    aeDeleteFileEvent(server.el, c->fd, AE_READABLE);
    aeDeleteFileEvent(server.el, c->fd, AE_WRITABLE);
    close(c->fd);
  }

  listRelease(c->reply);
  freeClientArgv(c);

  if (c->name) decrRefCount(c->name);
  zfree(c->argv);
  zfree(c);
}

// 异步释放客户端
void freeClientAsync(redisClient *c) {

}

// 在客户端执行完命令后重置客户端 准备执行下个指令
void resetClient(redisClient *c) {
  redisCommandProc *prevcmd = c->cmd ? c->cmd->proc : NULL;

  freeClientArgv(c);
  c->reqtype = 0;
  c->multibulklen = 0;
  c->bulklen = -1;
  // TODO AskCommand
}

// 事件处理器 命令回复处理器
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
  redisClient *c = privdata;
  int nwritten = 0, totwritten = 0, objlen; // 单次写入 共计写入 单个对象长度
  size_t objmem; // 单个对象占用的内存
  robj *o;
  REDIS_NOTUSED(el);
  REDIS_NOTUSED(mask);

  while (c->bufpos > 0 || listLength(c->reply)) {
    if (c->bufpos > 0) {
      nwritten = write(fd, c->buf+c->sentlen, c->bufpos-c->sentlen);
      if (nwritten <= 0) break; // 出错跳出
      c->sentlen += nwritten;
      totwritten += nwritten;

      if (c->sentlen == c->bufpos) {
        c->bufpos = 0;
        c->sentlen = 0;
      }
    } else {
      o = listNodeValue(listFirst(c->reply));
      objlen = sdslen(o->ptr);
      objmem = getStringObjectSdsUsedMemory(o);

      // 略过空对象
      if (objlen == 0) {
        listDelNode(c->reply, listFirst(c->reply));
        c->reply_bytes -= objmem;
        continue;
      }

      // c->sentlen用于处理 short write case
      // 对于short write 导致一次未能写完 +sentlen可以正确偏移
      nwritten = write(fd, ((char *)o->ptr)+c->sentlen, objlen-c->sentlen);
      if (nwritten <= 0) break;
      c->sentlen += nwritten;
      totwritten += nwritten;

      if (c->sentlen == objlen) { // 写完了 删掉写完的节点
        listDelNode(c->reply, listFirst(c->reply));
        c->sentlen = 0;
        c->reply_bytes -= objmem;
      }
    }
    /*
     * 为了避免一个非常大的回复独占服务器，
     * 当写入的总数量大于 REDIS_MAX_WRITE_PER_EVENT ，
     * 临时中断写入，将处理时间让给其他客户端，
     * 剩余的内容等下次写入就绪再继续写入
     *
     * 不过，如果服务器的内存占用已经超过了限制，
     * 那么为了将回复缓冲区中的内容尽快写入给客户端，
     * 然后释放回复缓冲区的空间来回收内存，
     * 这时即使写入量超过了 REDIS_MAX_WRITE_PER_EVENT ，
     * 程序也继续进行写入
     */
    if (totwritten > REDIS_MAX_WRITE_PER_EVENT &&
        (server.maxmemory == 0 || zmalloc_used_memory() < server.maxmemory)) {
      break;
    }
  }

  // 处理写入出错
  if (nwritten == -1) {
    if (errno == EAGAIN) {
      nwritten = 0;
    } else {
      redisLog(REDIS_VERBOSE, "Error writing to client: %s", strerror(errno));
      freeClient(c);
      return;
    }
  }

  if (totwritten > 0) {
    // TODO 复制
  }

  if (c->bufpos == 0 && listLength(c->reply) == 0) {
    c->sentlen = 0;
    aeDeleteFileEvent(server.el, c->fd, AE_WRITABLE);
    // 关闭客户端
    if (c->flags & REDIS_CLOSE_AFTER_REPLY) {
      freeClient(c);
    }
  }
}

// 为客户端安装写处理器到事件循环
void addReply(redisClient *c, robj *obj) {
  if (prepareClientToWrite(c) != REDIS_OK) {
    return;
  }
  printf("%s\n", (char *)obj->ptr);
  if (sdsEncodedObject(obj)) {
    if (_addReplyToBuffer(c, obj->ptr, sdslen(obj->ptr)) != REDIS_OK) {
      _addReplyObjectToList(c, obj);
    }
  } else if (obj->encoding == REDIS_ENCODING_INT) {
    // 优化，如果 c->buf 中有等于或多于 32 个字节的空间
    // 那么将整数直接以字符串的形式复制到 c->buf 中
    if (listLength(c->reply) == 0 && (sizeof(c->buf) - c->bufpos) >= 32) {
      char buf[32];
      int len;

      len = ll2string(buf, sizeof(buf), (long)obj->ptr);
      if (_addReplyToBuffer(c, buf, len) == REDIS_OK)
        return;
      /* else... continue with the normal code path, but should never
       * happen actually since we verified there is room. */
    }
    // 执行到这里，代表对象是整数，并且长度大于 32 位
    // 将它转换为字符串
    obj = getDecodedObject(obj);
    // 保存到缓存中
    if (_addReplyToBuffer(c, obj->ptr, sdslen(obj->ptr)) != REDIS_OK)
      _addReplyObjectToList(c, obj);
    decrRefCount(obj);
  } else {
    redisPanic("Wrong obj->encoding in addReply()");
  }
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
      setProtocolError(c, 0);
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
    setProtocolError(c, 0);
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

// 如果在读入协议内容的时候 发现不符合协议
static void setProtocolError(redisClient *c, int pos) {
  if (server.verbosity >= REDIS_VERBOSE) {
    // TODO
    redisLog(REDIS_VERBOSE, "Protocol error from client: ");
  }
  c->flags |= REDIS_CLOSE_AFTER_REPLY;
  sdsrange(c->querybuf, pos, -1);
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

void addReplySds(redisClient *c, sds s) {
  if (prepareClientToWrite(c) != REDIS_OK) {
    sdsfree(s);
    return;
  }

  if (_addReplyToBuffer(c, s, sdslen(s)) == REDIS_OK) {
    sdsfree(s);
  } else {
    _addReplySdsToList(c, s);
  }
}

// 将 c String 中的内容复制到回复缓冲区
void addReplyString(redisClient *c, char *s, size_t len) {
  if (prepareClientToWrite(c) != REDIS_OK)
    return;
  if (_addReplyToBuffer(c, s, len) != REDIS_OK)
    _addReplyStringToList(c, s, len);
}

void addReplyErrorLength(redisClient *c, char *s, size_t len) {
  addReplyString(c, "-ERR", 5);
  addReplyString(c, s, len);
  addReplyString(c, "\r\n", 2);
}

void addReplyError(redisClient *c, char *err) {
  addReplyErrorLength(c, err, strlen(err));
}

void addReplyErrorFormat(redisClient *c, const char *fmt, ...) {
  size_t l, j;
  va_list ap;
  va_start(ap, fmt);
  sds s = sdscatvprintf(sdsempty(),fmt, ap);
  va_end(ap);

  l = sdslen(s);
  for (j = 0;j < l;j++) {
    if (s[j] == '\r' || s[j] == '\n')
      s[j] = ' ';
  }
  addReplyErrorLength(c, s, sdslen(s));
  sdsfree(s);
}

void addReplyStatusLength(redisClient *c, char *s, size_t len) {
  addReplyString(c, "+", 1);
  addReplyString(c, s, len);
  addReplyString(c, "\r\n", 2);
}

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

  if (prefix == '*' && ll < REDIS_SHARED_BULKHDR_LEN) {
    // 多条批量回复
    addReply(c, shared.bulkhdr[ll]);
    return;
  } else if (prefix == '$' && ll < REDIS_SHARED_BULKHDR_LEN) {
    // 批量回复
    addReply(c, shared.bulkhdr[ll]);
    return;
  }

  buf[0] = prefix;
  len = ll2string(buf+1, sizeof(buf)-1, ll);
  buf[len+1] = '\r';
  buf[len+2] = '\n';
  addReplyString(c, buf, len+3);
}

void addReplyBulkLen(redisClient *c, robj *obj) {
  size_t len;

  if (sdsEncodedObject(obj)) {
    len = sdslen(obj->ptr);
  } else {
    long n = (long)obj->ptr;

    len = 1;
    if (n < 0) {
      len++;
      n = -n; // 转正数
    }
    while((n = n/10) != 0) {
      len++; // 统计long的字符串长度
    }
  }

  if (len < REDIS_SHARED_BULKHDR_LEN) {
    addReply(c, shared.bulkhdr[len]);
  } else {
    addReplyLongLongWithPrefix(c, len, '$');
  }
}

// 返回一个 Redis 对象作为回复
void addReplyBulk(redisClient *c, robj *obj) {
  addReplyBulkLen(c, obj);
  addReply(c, obj);
  addReply(c, shared.crlf);
}


void addReplyBulkCString(redisClient *c, char *s) {
  if (s == NULL) {
    addReply(c, shared.nullbulk);
  } else {
    addReplyBulkCBuffer(c, s, strlen(s));
  }
}

void addReplyBulkCBuffer(redisClient *c, void *p, size_t len) {
  addReplyLongLongWithPrefix(c, len, '$');
  addReplyString(c, p, len);
  addReply(c, shared.crlf);
}

void addReplyBulkLongLong(redisClient *c, long long ll) {
  char buf[64];
  int len;

  len = ll2string(buf, 64, ll);
  addReplyBulkCBuffer(c, buf, len);
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
