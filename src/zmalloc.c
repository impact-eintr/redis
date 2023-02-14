#include "zmalloc.h"

#include <pthread.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

void zlibc_free(void *ptr) { free(ptr); }

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

// 私有内存 其他文件无法访问
static size_t used_memory = 0;
static int zmalloc_thread_safe = 0;

pthread_mutex_t used_memory_mutex = PTHREAD_MUTEX_INITIALIZER;

#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif

// OOM 的处理函数
static void zmalloc_default_oom(size_t size) {
  fprintf(stderr, "zmalloc: Out of memory trying to allocate %zu bytes\n",
          size);
  fflush(stderr);
  abort();
}

static void (*zmalloc_oom_handler)(size_t) = zmalloc_default_oom;

void *zmalloc(size_t size) {
  void *ptr = malloc(size + PREFIX_SIZE);
  if (!ptr)
    zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
  // 更新状态
  update_zmalloc_stat_alloc(zmalloc_size(ptr));
  return ptr;
#else
  // 头部数据作为内存长度
  *((size_t *)ptr) = size;
  update_zmalloc_stat_alloc(size + PREFIX_SIZE);
  return (char *)ptr + PREFIX_SIZE; // 返回有效数据起始地址
#endif
}

void *zcalloc(size_t size) {
  void *ptr = calloc(1, size + PREFIX_SIZE);
  if (!ptr)
    zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
  // 更新状态
  update_zmalloc_stat_alloc(size + PREFIX_SIZE);
  return ptr;
#else
  // 头部数据作为内存长度
  *((size_t *)ptr) = size;
  update_zmalloc_stat_alloc(size + PREFIX_SIZE);
  return (char *)ptr + PREFIX_SIZE; // 返回有效数据起始地址
#endif

}

void *zrealloc(void *ptr, size_t size) {
#ifndef HAVE_MALLOC_SIZE
  void *realptr;
#endif
  size_t oldsize;
  void *newptr;

  if (ptr == NULL)
    return zmalloc(size);
#ifdef HAVE_MALLOC_SIZE
  oldsize = zmalloc_size(ptr);
  newptr = realloc(ptr, size);
  if (!newptr)
    zmalloc_oom_handler(size);

  update_zmalloc_stat_free(oldsize);
  update_zmalloc_stat_alloc(zmalloc_size(newptr));
  return newptr;
#else
  realptr = (char *)ptr - PREFIX_SIZE;
  oldsize = *((size_t *)realptr);
  newptr = realloc(realptr, size + PREFIX_SIZE);
  if (!newptr)
    zmalloc_oom_handler(size);

  *((size_t *)newptr) = size;
  update_zmalloc_stat_free(oldsize); // 更新全局信息
  update_zmalloc_stat_alloc(size);
  return (char *)newptr + PREFIX_SIZE;
#endif
}

#ifdef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr) {
  void *realptr = (char *)ptr - PREFIX_SIZE;
  size_t size = *((size_t *)realptr);
  /* Assume at least that all the allocations are padded at sizeof(long) by
   * the underlying allocator. */
  if (size & (sizeof(long) - 1))
    size += sizeof(long) - (size & (sizeof(long) - 1));
  return size + PREFIX_SIZE;
}
#endif

void zfree(void *ptr) {
#ifndef HAVE_MALLOC_SIZE
  void *realptr;
  size_t oldsize;
#endif

  if (ptr == NULL)
    return;
#ifdef HAVE_MALLOC_SIZE
  update_zmalloc_stat_free(zmalloc_size(ptr));
  free(ptr);
#else
  realptr = (char *)ptr - PREFIX_SIZE;
  oldsize = *((size_t *)realptr);
  update_zmalloc_stat_free(oldsize + PREFIX_SIZE); // 更新全局信息
  free(realptr); // 释放内存
#endif
}

// 复制一条字符串 但是自带头部长度
char* zstrdup(const char* s) {
  size_t l = strlen(s)+1;
  char *p = zmalloc(l);

  memcpy(p, s, l);
  return p;
}

size_t zmalloc_used_memory(void) {
  size_t um;

  if (zmalloc_thread_safe) { // 开启多线程保护
#ifdef HAVE_ATOMIC
    um = __sync_add_and_fetch(&used_memory, 0);
#else
    pthread_mutex_lock(&used_memory_mutex);
    um = used_memory;
    pthread_mutex_unlock(&used_memory_mutex);
#endif
  } else {
    um = used_memory;
  }

  return um;
}

// 开启线程安全标识
void zmalloc_enable_thread_safeness(void) { zmalloc_thread_safe = 1; }

// 配置OOM处理函数
void zmalloc_set_oom_handler(void (*oom_handler)(size_t)) {
  zmalloc_oom_handler = oom_handler;
}

#if defined(HAVE_PROC_STAT)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

size_t zmalloc_get_rss(void) {
  int page = sysconf(_SC_PAGESIZE);
  size_t rss;
  char buf[4096];
  char filename[256];
  int fd, count;
  char *p, *x;

  snprintf(filename, 256, "/proc/%d/stat", getpid());
  if ((fd = open(filename, O_RDONLY)) == -1)
    return 0;
  if (read(fd, buf, 4096) <= 0) {
    close(fd);
    return 0;
  }
  close(fd);

  p = buf;
  count = 23; /* RSS is the 24th field in /proc/<pid>/stat */
  while (p && count--) {
    p = strchr(p, ' ');
    if (p)
      p++;
  }
  if (!p)
    return 0;
  x = strchr(p, ' ');
  if (!x)
    return 0;
  *x = '\0';

  rss = strtoll(p, NULL, 10);
  rss *= page;
  return rss;
}

#elif defined(HAVE_TASKINFO)
#include <mach/mach_init.h>
#include <mach/task.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

size_t zmalloc_get_rss(void) {
  task_t task = MACH_PORT_NULL;
  struct task_basic_info t_info;
  mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

  if (task_for_pid(current_task(), getpid(), &task) != KERN_SUCCESS)
    return 0;
  task_info(task, TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count);

  return t_info.resident_size;
}
#else
size_t zmalloc_get_rss(void) {
  /* If we can't get the RSS in an OS-specific way for this system just
   * return the memory usage we estimated in zmalloc()..
   *
   * Fragmentation will appear to be always 1 (no fragmentation)
   * of course... */
  return zmalloc_used_memory();
}
#endif

// 获取内存碎片比例
float zmalloc_get_fragmentation_ratio(size_t rss) {
  return (float)rss/zmalloc_used_memory();
}

#if defined(HAVE_PROC_SMAPS)
size_t zmalloc_get_private_dirty(void) {
  char line[1024];
  size_t pd = 0;
  FILE *fp = fopen("/proc/self/smaps", "r");

  if (!fp)
    return 0;
  while (fgets(line, sizeof(line), fp) != NULL) {
    if (strncmp(line, "Private_Dirty:", 14) == 0) {
      char *p = strchr(line, 'k');
      if (p) {
        *p = '\0';
        pd += strtol(line + 14, NULL, 10) * 1024;
      }
    }
  }
  fclose(fp);
  return pd;
}

#else
size_t zmalloc_get_private_dirty(void) { return 0; }
#endif
