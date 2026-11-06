#include "zmalloc.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

static size_t used_memory = 0;
static int zmalloc_thread_safe = 0;

pthread_mutex_t used_memory_mutex = PTHREAD_MUTEX_INITIALIZER;

#define update_zmalloc_stat_add(__n)                                           \
  do {                                                                         \
    pthread_mutex_lock(&used_memory_mutex);                                    \
    used_memory += (__n);                                                      \
    pthread_mutex_unlock(&used_memory_mutex);                                  \
  } while (0)

#define update_zmalloc_stat_sub(__n)                                           \
  do {                                                                         \
    pthread_mutex_lock(&used_memory_mutex);                                    \
    used_memory -= (__n);                                                      \
    pthread_mutex_unlock(&used_memory_mutex);                                  \
  } while(0)

#define update_zmalloc_stat_alloc(__n)                                         \
  do {                                                                         \
    size_t _n = (__n);                                                         \
    if (_n & (sizeof(long) - 1))                                               \
      _n += sizeof(long) - (_n & (sizeof(long) - 1));                          \
    if (zmalloc_thread_safe) {                                                 \
      update_zmalloc_stat_add(_n);                                             \
    } else {                                                                   \
      used_memory += _n;                                                       \
    }                                                                          \
  } while (0)

#define update_zmalloc_stat_free(__n)                                          \
  do {                                                                         \
    size_t _n = (__n);                                                         \
    if (_n & (sizeof(long) - 1))                                               \
      _n += sizeof(long) - (_n & (sizeof(long) - 1));                          \
    if (zmalloc_thread_safe) {                                                 \
      update_zmalloc_stat_sub(_n);                                             \
    } else {                                                                   \
      used_memory -= _n;                                                       \
    }                                                                          \
  } while (0)

#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif


static void zmalloc_default_oom(size_t size) {
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %zu bytes", size);
    fflush(stderr);
    abort();
}

// 内置的动态内存分配器
void *zmalloc(size_t size) {
    void *ptr = malloc(size + PREFIX_SIZE);
    if (!ptr)
        zmalloc_default_oom(size);
#ifdef HAVE_MALLOC_SIZE

    return ptr;
#else
    *((size_t *)ptr) = size; // 第一个size_t作为数据长度使用
    update_zmalloc_stat_alloc(size + PREFIX_SIZE);
    return (char *)ptr + PREFIX_SIZE;
#endif
}


void *zcalloc(size_t size) {
    void *ptr = calloc(1, size + PREFIX_SIZE);
    if (!ptr)
        zmalloc_default_oom(size);
#ifdef HAVE_MALLOC_SIZE

    return ptr;
#else
    *((size_t *)ptr) = size; // 第一个size_t作为数据长度使用
    update_zmalloc_stat_alloc(size + PREFIX_SIZE);
    return (char *)ptr + PREFIX_SIZE;
#endif
}

void *zrealloc(void *ptr, size_t size) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;

#endif
    size_t oldsize;
    void *newptr;
    if (ptr == NULL) {
        return zmalloc(size);
    }
#ifdef HAVE_MALLOC_SIZE

#else
    realptr = (char *)ptr - PREFIX_SIZE;
    oldsize = *((size_t *)realptr);
    newptr = realloc(realptr, size + PREFIX_SIZE);
    if (!newptr) {
        zmalloc_default_oom(size);
    }
    *((size_t *)newptr) = size;
    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(size);
    return (char *)newptr + PREFIX_SIZE;
#endif

}

void zfree(void *ptr) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif
    if (ptr == NULL) {
        return;
    }
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(oldsize + PREFIX_SIZE);
    free(realptr);
#else
    realptr = (char *)ptr - PREFIX_SIZE;
    oldsize = *((size_t *)realptr);
    update_zmalloc_stat_free(oldsize + PREFIX_SIZE);
    free(realptr);
#endif
}

char *zstrdup(const char *s) {
    size_t l = strlen(s) + 1;
    char *p = zmalloc(l);

    memcpy(p, s, l);
    return p;
}

// 内存监控
size_t zmalloc_used_memory(void) {
    size_t um;
    pthread_mutex_lock(&used_memory_mutex);
    um = used_memory;
    pthread_mutex_unlock(&used_memory_mutex);

    return um;
}


int main() {
    char *s = "hello eintr!";
    char *ns = zstrdup(s);
    size_t size = zmalloc_used_memory();

    printf("%s used %lu\n", ns, size);

    zfree(ns);
    return 0;
}