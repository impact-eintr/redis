#ifndef ZMALLOC_H_
#define ZMALLOC_H_H


#include <stddef.h>

void *zmalloc(size_t size);
void *zcalloc(size_t size);
void *zrealloc(void *ptr, size_t size);

void zfree(void *ptr);

char *zstrdup(const char *ptr);

// 内存监控
size_t zmalloc_used_memory(void);

#endif