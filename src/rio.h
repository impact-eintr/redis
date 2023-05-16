#ifndef RIO_H_
#define RIO_H_

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "sds.h"

// RIO API 接口和状态
struct _rio {
  size_t (*read)(struct _rio*, void *buf, size_t len);
  size_t (*write)(struct _rio *, const void *buf, size_t len);
  off_t (*tell)(struct _rio*);

  // 校验和计算函数
  void (*update_cksum)(struct _rio*, const void *buf, size_t len);

  // 当前校验和
  uint64_t cksum;

  /* number of bytes read or written */
  size_t processed_bytes;

  /* maximum single read or write chunk size */
  size_t max_processing_chunk;

  /* Backend-specific vars. */
  union {

    struct {
      // 缓存指针
      sds ptr;
      // 偏移量
      off_t pos;
    } buffer;

    struct {
      // 被打开文件的指针
      FILE *fp;
      // 最近一次 fsync() 以来，写入的字节量
      off_t buffered; /* Bytes written since last fsync. */
      // 写入多少字节之后，才会自动执行一次 fsync()
      off_t autosync; /* fsync after 'autosync' bytes written. */
    } file;
  } io;
};

typedef struct _rio rio;

static inline size_t rioWrite(rio *r, const void *buf, size_t len) {
  while (len) {
    size_t bytes_to_write =
        (r->max_processing_chunk && r->max_processing_chunk < len)
            ? r->max_processing_chunk
            : len;
    if (r->update_cksum)
      r->update_cksum(r, buf, bytes_to_write);
    if (r->write(r, buf, bytes_to_write) == 0) {
      return 0;
    }
    buf = (char *)buf + bytes_to_write;
    len -= bytes_to_write;
    r->processed_bytes += bytes_to_write;
  }
  return 1;
}

static inline size_t rioRead(rio *r, void *buf, size_t len) {
  while (len) {
    size_t bytes_to_read =
        (r->max_processing_chunk && r->max_processing_chunk < len)
            ? r->max_processing_chunk
            : len;
    if (r->update_cksum)
      r->update_cksum(r, buf, bytes_to_read);
    if (r->read(r, buf, bytes_to_read) == 0) {
      return 0;
    }
    buf = (char *)buf + bytes_to_read;
    len -= bytes_to_read;
    r->processed_bytes += bytes_to_read;
  }
  return 1;
}

static inline off_t rioTell(rio *r) {
  return r->tell(r);
}

void rioInitWithFile(rio *r, FILE *fp);
void rioInitWithBuffer(rio *r, sds s);

size_t rioWriteBulkCount(rio *r, char prefix, int count);
size_t rioWriteBulkString(rio *r, const char *buf, size_t len);
size_t rioWriteBulkLongLong(rio *r, long long l);
size_t rioWriteBulkDouble(rio *r, double d);

void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len);
void rioSetAutoSync(rio *r, off_t bytes);


#endif // RIO_H_
