#ifndef SDS_H_
#define SDS_H_

#define SDS_MAX_PREALLOC (1024 * 1024)

#include <stddef.h>
#include <stdarg.h>

typedef char *sds;

struct sdshdr
{
    int len;
    int free;
    char buf[];
};

/* 由 sds（即 sh->buf）反推回所属的 sdshdr 头指针 */
#define SDS_HDR(s) ((struct sdshdr *)((s) - sizeof(struct sdshdr)))

static inline size_t sdslen(const sds s)
{
    return SDS_HDR(s)->len;
}

static inline size_t sdsavail(const sds s)
{
    return SDS_HDR(s)->free;
}

sds sdsnew(const char* init);
sds sdsnewlen(const void* init, size_t initlen);
sds sdsempty(void);
sds sdsdup(const sds s);
void sdsfree(sds s);
sds sdsgrowzero(sds s, size_t len);
sds sdscatlen(sds s, const void* t, size_t len);
sds sdscat(sds s, const char* t);
sds sdscatsds(sds s, const sds t);
sds sdscpylen(sds s, const char* t, size_t len);
sds sdscpy(sds s, const char* t);

#endif