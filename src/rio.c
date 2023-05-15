#include "rio.h"
#include "config.h"
#include <sys/types.h>
#include <string.h>


static size_t rioBufferRead(rio *r, void *buf, size_t len) {
  if (sdslen(r->io.buffer.ptr)-r->io.buffer.pos < len) {
    return 0;
  }
  memcpy(buf, r->io.buffer.ptr+r->io.buffer.pos, len);
  r->io.buffer.pos += len;

  return 1;
}

static size_t rioBufferWrite(rio *r, const void *buf, size_t len) {
  r->io.buffer.ptr = sdscatlen(r->io.buffer.ptr, (char *)buf, len);
  r->io.buffer.pos += len;
  return 1;
}

static off_t rioBufferTell(rio *r) {
  return r->io.buffer.pos;
}

// 流为文件时使用的结构
static const rio rioBufferIO = {
  rioBufferRead,
  rioBufferWrite,
  rioBufferTell,
  NULL,
  0,
  0,
  0,
  {{NULL, 0}}
};


static size_t rioFileRead(rio *r, void *buf, size_t len) {
  return fread(buf, len, 1, r->io.file.fp);
}

static size_t rioFileWrite(rio *r, const void *buf, size_t len) {
  size_t retval;

  retval = fwrite(buf, len, 1, r->io.file.fp);
  r->io.file.buffered += len;

  if (r->io.file.autosync &&
      r->io.file.buffered >= r->io.file.autosync) {
    fflush(r->io.file.fp);
    aof_fsync(fileno(r->io.file.fp));
    r->io.file.buffered = 0;
  }

  return retval;
}

static off_t rioFileTell(rio *r) {
  return ftello(r->io.file.fp);
}

// 流为文件时使用的结构
static const rio rioFileIO = {
  rioFileRead,
  rioFileWrite,
  rioFileTell,
  NULL,
  0,
  0,
  0,
  {{NULL, 0}}
};


void rioInitWithFile(rio *r, FILE *fp) {
  *r = rioFileIO;
  r->io.file.fp = fp;
  r->io.file.buffered = 0;
  r->io.file.autosync = 0;
}

void rioInitWithBuffer(rio *r, sds s)  {
  *r = rioFileIO;
  r->io.buffer.ptr = s;
  r->io.buffer.pos = 0;
}

size_t rioWriteBulkCount(rio *r, char prefix, int count);
size_t rioWriteBulkString(rio *r, const char *buf, size_t len);
size_t rioWriteBulkLongLong(rio *r, long long l);
size_t rioWriteBulkDouble(rio *r, double d);

void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len) {
  //r->cksum = crc64(r->cksum, buf, len);
}

void rioSetAutoSync(rio *r, off_t bytes);
