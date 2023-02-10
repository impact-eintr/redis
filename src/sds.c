#include "sds.h"
#include "zmalloc.h"

#include <stdarg.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

sds sdsnewlen(const void *init, size_t initlen) {
  struct sdshdr *sh;

  // 根据是否有初始化内容 选择适当内存分配方式
  if (init) {
    // zmalloc 不初始化所分配的内存
    sh = zmalloc(sizeof(struct sdshdr) + initlen + 1);
  } else {
    // zcalloc set all mem to 0
    sh = zcalloc(sizeof(struct sdshdr) + initlen + 1);
  }
  // 内存分配失败， 返回
  if (sh == NULL) {
    return NULL;
  }

  // 设置初始化长度
  sh->len = initlen;
  // 新sds不预留任何空间
  sh->free = 0;
  // 如果有指定初始化内容，将它们复制到sdshdr的buf中
  if (initlen && init) {
    memcpy(sh->buf, init, initlen);
  }
  // 以 '\0' 结尾
  sh->buf[initlen] = '\0';

  // 返回buf部分 而不是整个sdshdr
  return (char*)sh->buf;
}

// 新建一个空的sds
sds sdsempty(void) {
  return sdsnewlen("", 0);
}

sds sdsnew(const char *init) {
  size_t initlen = (init == NULL) ? 0 : strlen(init);
  return sdsnewlen(init, initlen);
}

// 复制 sds
sds sdsdup(const sds s) {
  return sdsnewlen(s, sdslen(s));
}

// 释放sds
void sdsfree(sds s) {
  if (s == NULL)
    return;
  zfree(s - sizeof(struct sdshdr)); // 从内存头开始释放
}


void sdsclear(sds s) {
  struct sdshdr *sh = (void*) (s-sizeof(struct sdshdr));
  sh->free += sh->len; // clear used
  sh->len = 0;
  sh->buf[0] = 0;
}

/* Low level functions exposed to the user API
 *扩展sds的内存 */
sds sdsMakeRoomFor(sds s, size_t addlen) {
  struct sdshdr *sh, *newsh;

  // 获取 s 目前的空余空间长度
  size_t free = sdsavail(s);

  size_t len, newlen;

  if (free > addlen)
    return s; // skip if we have more space

  len = sdslen(s);
  sh = (void *)(s - (sizeof(struct sdshdr)));

  // s 最少需要的长度
  newlen = (len + addlen);

  if (newlen < SDS_MAX_PREALLOC)
    newlen *= 2;
  else
    newlen += SDS_MAX_PREALLOC;
  newsh = zrealloc(sh, sizeof(struct sdshdr) + newlen + 1); // hdr+data+EOF

  // 内存不足 分配失败 返回
  if (newsh == NULL)
    return NULL;

  newsh->free = newlen - len; // newlen - oldlen

  // 返回 sds
  return newsh->buf;
}

sds sdsRemoveFreeSpace(sds s) {
  struct sdshdr *sh;
  sh = (void *) (s - (sizeof(struct sdshdr)));

  // 重新分配内存空间
  sh = zrealloc(sh, sizeof(struct sdshdr)+sh->len+1); // hdr+data(initlen->truelen)+EOF
  sh->free = 0; // 剩余空间为0

  return sh->buf;
}


size_t sdsAllocSize(sds s) {
  struct sdshdr *sh = (void *) (s - sizeof(struct sdshdr));
  return sizeof(*sh)+sh->len+sh->free+1;// hdr+used+free+EOF
}

// 根据 incr 参数 增加sds 的长度 缩减空余空间
void sdsIncrLen(sds s, int incr) {
  struct sdshdr *sh = (void *)(s - sizeof(struct sdshdr));
  assert(sh->free >= incr); // make sure we have enough space
  sh->len += incr;
  sh->free -= incr;

  assert(sh->free >= 0);
  s[sh->len] = 0;
}

// 扩充sds长度 未使用的部分用0填充
sds sdsgrowzero(sds s, size_t len) {
  struct sdshdr *sh = (void *)(s - (sizeof(struct sdshdr)));
  size_t totlen, curlen = sh->len;

  // 如果 len 比较字符串的现有长度小
  // 直接返回
  if (len <= curlen)
    return s;

  s = sdsMakeRoomFor(s, len - curlen);
  // 如果内存不足 直接返回
  if (s == NULL)
    return NULL;

  // 将新分配的空间用0填充 防止出现垃圾内容
  sh = (void *)(s - sizeof(struct sdshdr));
  memset(s + curlen, 0, (len - curlen + 1)); // hdr+data+00000...+EOF

  // 更新属性
  totlen = sh->len + sh->free;
  sh->len = len;
  sh->free = totlen - sh->len;

  // 返回新的 sds
  return s;
}

// 追加长度为 len 的一个字符串到另一个字符串
sds sdscatlen(sds s, const void *t, size_t len) {
  struct sdshdr *sh;

  // 原有字符串长度
  size_t curlen = sdslen(s);

  // 扩展 sds 空间
  s = sdsMakeRoomFor(s, len);

  // 内存不足？直接返回
  if (s == NULL) return NULL;

  // 复制数据
  memcpy(s+curlen, t, len);

  // 更新属性
  sh = (void *)(s - (sizeof(struct sdshdr)));
  sh->len = curlen + len;
  sh->free -= len;

  // 添加新结尾符号
  s[curlen + len] = '\0';
  // 返回新 sds
  return s;
}

// 追加字符串 s + t
sds sdscat(sds s, const char *t) {
  return sdscatlen(s, t, strlen(t));
}


sds sdscatsds(sds s, const sds t) {
  return sdscatlen(s, t, sdslen(t));
}

// 复制指定长度 用t的指定长度覆盖s
sds sdscpylen(sds s, const char *t, size_t len) {
  struct sdshdr *sh = (void *)(s - (sizeof(struct sdshdr)));

  // sds 现有 buf 的长度
  size_t totlen = sh->free + sh->len;

  if (totlen < len) {
    s = sdsMakeRoomFor(s, len-sh->len);
    if (s == NULL) return NULL;
    sh = (void *)(s - sizeof(struct sdshdr));
    totlen = sh->free + sh->len;
  }

  // 复制内容
  memcpy(s, t, len);
  // 添加终结符号
  s[len] = '\0';
  // 更新属性
  sh->len = len;
  sh->free = totlen - len;
  // 返回新的 sds
  return s;
}

// 复制整个字符串 用t覆盖s
sds sdscpy(sds s, const char *t) {
  return sdscpylen(s, t, strlen(t));
}

#define SDS_LLSTR_SIZE 21
int sdsll2str(char *s, long long value) {
    char *p, aux;
    unsigned long long v;
    size_t l;

    /* Generate the string representation, this method produces
     * an reversed string. */
    v = (value < 0) ? -value : value;
    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);
    if (value < 0) *p++ = '-';

    /* Compute length and add null term. */
    l = p-s;
    *p = '\0';

    /* Reverse the string. */
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

/* Identical sdsll2str(), but for unsigned long long type. */
int sdsull2str(char *s, unsigned long long v) {
  char *p, aux;
  size_t l;

  /* Generate the string representation, this method produces
   * an reversed string. */
  p = s;
  do {
    *p++ = '0' + (v % 10);
    v /= 10;
  } while (v);

  /* Compute length and add null term. */
  l = p - s;
  *p = '\0';

  /* Reverse the string. */
  p--;
  while (s < p) {
    aux = *s;
    *s = *p;
    *p = aux;
    s++;
    p--;
  }
  return l;
}

sds sdscatvprintf(sds s, const char *fmt, va_list ap) {
  va_list cpy;
  char staticbuf[1024], *buf = staticbuf, *t;
  size_t buflen = strlen(fmt) * 2;

  /* We try to start using a static buffer for speed.
   * If not possible we revert to heap allocation. */
  if (buflen > sizeof(staticbuf)) {
    buf = zmalloc(buflen);
    if (buf == NULL)
      return NULL;
  } else {
    buflen = sizeof(staticbuf);
  }

  /* Try with buffers two times bigger every time we fail to
   * fit the string in the current buffer size. */
  while (1) {
    buf[buflen - 2] = '\0';
    va_copy(cpy, ap);
    // T = O(N)
    vsnprintf(buf, buflen, fmt, cpy);
    if (buf[buflen - 2] != '\0') {
      if (buf != staticbuf)
        zfree(buf);
      buflen *= 2;
      buf = zmalloc(buflen);
      if (buf == NULL)
        return NULL;
      continue;
    }
    break;
  }

  /* Finally concat the obtained string to the SDS string and return it. */
  t = sdscat(s, buf);
  if (buf != staticbuf)
    zfree(buf);
  return t;
}

#ifdef __GNUC__
sds sdscatprintf(sds s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
sds sdscatprintf(sds s, const char *fmt, ...);
#endif

sds sdscatfmt(sds s, char const *fmt, ...) {
  struct sdshdr *sh = (void *)(s - sizeof(struct sdshdr));
  size_t initlen = sdslen(s);
  const char *f = fmt;
  int i;
  va_list ap;

  va_start(ap, fmt);
  f = fmt;
  i = initlen;
  while (*f) {
    char next, *str;
    size_t l;
    long long num;
    unsigned long long unum;

    /* Make sure there is always space for at least 1 char. */
    if (sh->free == 0) {
      s = sdsMakeRoomFor(s, 1);
      sh = (void *)(s - (sizeof(struct sdshdr)));
    }

    switch (*f) {
    case '%':
      next = *(f + 1);
      f++;
      switch (next) {
      case 's':
      case 'S':
        str = va_arg(ap, char *);
        l = (next == 's') ? strlen(str) : sdslen(str);
        if (sh->free < l) {
          s = sdsMakeRoomFor(s, l);
          sh = (void *)(s - (sizeof(struct sdshdr)));
        }
        memcpy(s + i, str, l);
        sh->len += l;
        sh->free -= l;
        i += l;
        break;
      case 'i':
      case 'I':
        if (next == 'i')
          num = va_arg(ap, int);
        else
          num = va_arg(ap, long long);
        {
          char buf[SDS_LLSTR_SIZE];
          l = sdsll2str(buf, num);
          if (sh->free < l) {
            s = sdsMakeRoomFor(s, l);
            sh = (void *)(s - (sizeof(struct sdshdr)));
          }
          memcpy(s + i, buf, l);
          sh->len += l;
          sh->free -= l;
          i += l;
        }
        break;
      case 'u':
      case 'U':
        if (next == 'u')
          unum = va_arg(ap, unsigned int);
        else
          unum = va_arg(ap, unsigned long long);
        {
          char buf[SDS_LLSTR_SIZE];
          l = sdsull2str(buf, unum);
          if (sh->free < l) {
            s = sdsMakeRoomFor(s, l);
            sh = (void *)(s - (sizeof(struct sdshdr)));
          }
          memcpy(s + i, buf, l);
          sh->len += l;
          sh->free -= l;
          i += l;
        }
        break;
      default: /* Handle %% and generally %<unknown>. */
        s[i++] = next;
        sh->len += 1;
        sh->free -= 1;
        break;
      }
      break;
    default:
      s[i++] = *f;
      sh->len += 1;
      sh->free -= 1;
      break;
    }
    f++;
  }
  va_end(ap);

  /* Add null-term */
  s[i] = '\0';
  return s;
}

sds sdstrim(sds s, const char *cset) {
  struct sdshdr *sh = (void *)(s - sizeof(struct sdshdr));
  char *start, *end, *sp, *ep;
  size_t len;

  // 设置和记录指针
  sp = start = s;
  ep = end = s+sdslen(s)-1;

  // 修剪， T = O(N^2)
  while(sp <= end && strchr(cset, *sp)) sp++;
  while(ep > start && strchr(cset, *ep)) ep--;

  len = (sp > ep) ? 0 : ((ep - sp) + 1);

  // 如果有需要，前移字符串内容
  // T = O(N)
  if (sh->buf != sp)
    memmove(sh->buf, sp, len);

  // 添加终结符
  sh->buf[len] = '\0';

  // 更新属性
  sh->free = sh->free + (sh->len - len);
  sh->len = len;

  // 返回修剪后的 sds
  return s;
}

// 按照索引对截取sds字符串的其中一段
void sdsrange(sds s, int start, int end) {

}

void sdsupdatelen(sds s);
int sdscmp(const sds s1, const sds s2);
sds *sdssplitlen(const char *s, int len, const char *sep, int seplen,
                 int *count);
void sdsfreesplitres(sds *tokens, int count);
void sdstolower(sds s);
void sdstoupper(sds s);
sds sdsfromlonglong(long long value);
sds sdscatrepr(sds s, const char *p, size_t len);
sds *sdssplitargs(const char *line, int *argc);
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen);
sds sdsjoin(char **argv, int argc, char *sep);
