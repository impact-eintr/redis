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

static inline size_t sdslen(const sds s)
{
    struct sdshdr *sh = (void *)(s - sizeof(struct sdshdr));
    return sh->len;
}

static inline size_t sdsavail(const sds s)
{
    struct sdshdr *sh = (void *)(s - sizeof(struct sdshdr));
    return sh->free;
}

#endif