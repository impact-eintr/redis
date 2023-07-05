#include "ae.h"
#include "redis.h"
#include <asm-generic/errno-base.h>
#include <errno.h>
#include <unistd.h>

#define REDIS_SYNCIO_RESOLUTION 10 // Resolution in milliseconds

// timeout is milliseconds
ssize_t syncWrite(int fd, char *ptr, ssize_t size, long long timeout) {
  ssize_t nwritten, ret = size;
  long long start = mstime();
  long long remaining = timeout;

  while(1) {
    long long wait = (remaining > REDIS_SYNCIO_RESOLUTION) ? remaining : REDIS_SYNCIO_RESOLUTION;

    long long elapsed;

    nwritten = write(fd, ptr, size);
    if (nwritten == -1) {
      if (errno != EAGAIN) {
        return -1;
      }
    } else {
      ptr += nwritten;
      size -= nwritten;
    }

    if (size == 0) {
      return ret;
    }

    // Wait
    aeWait(fd, AE_WRITABLE, wait);
    elapsed = mstime() - start;
    if (elapsed >= timeout) {
      errno = ETIMEDOUT;
      return -1;
    }
    remaining = timeout - elapsed;
  }
}

ssize_t syncRead(int fd, char *ptr, ssize_t size, long long timeout) {
  ssize_t nread, totread = 0;
  long long start = mstime();
  long long remaining = timeout;

  if (size == 0) return 0;
  while(1) {
    long long wait = (remaining > REDIS_SYNCIO_RESOLUTION) ?
      remaining : REDIS_SYNCIO_RESOLUTION;
    long long elapsed;

    /* Optimistically try to read before checking if the file descriptor
     * is actually readable. At worst we get EAGAIN. */
    nread = read(fd,ptr,size);
    if (nread == 0) return -1; /* short read. */
    if (nread == -1) {
      if (errno != EAGAIN) return -1;
    } else {
      ptr += nread;
      size -= nread;
      totread += nread;
    }
    if (size == 0) return totread;

    /* Wait */
    aeWait(fd,AE_READABLE,wait);
    elapsed = mstime() - start;
    if (elapsed >= timeout) {
      errno = ETIMEDOUT;
      return -1;
    }
    remaining = timeout - elapsed;
  }
}

ssize_t syncReadLine(int fd, char *ptr, ssize_t size, long long timeout) {
  ssize_t nread = 0;

  size--;
  while(size) {
    char c;

    if (syncRead(fd,&c,1,timeout) == -1) return -1;
    if (c == '\n') {
      *ptr = '\0';
      if (nread && *(ptr-1) == '\r') *(ptr-1) = '\0';
      return nread;
    } else {
      *ptr++ = c;
      *ptr = '\0';
      nread++;
    }
  }
  return nread;
}
