#include "sds.h"
#include "zmalloc.h"
#include "testhelp.h"

#include <stdarg.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

sds sdsnewlen(const void *init, size_t initlen)
{
  struct sdshdr *sh;

  // 根据是否有初始化内容 选择适当内存分配方式
  if (init)
  {
    // zmalloc 不初始化所分配的内存
    sh = zmalloc(sizeof(struct sdshdr) + initlen + 1);
  }
  else
  {
    // zcalloc set all mem to 0
    sh = zcalloc(sizeof(struct sdshdr) + initlen + 1);
  }
  // 内存分配失败， 返回
  if (sh == NULL)
  {
    return NULL;
  }

  // 设置初始化长度
  sh->len = initlen;
  // 新sds不预留任何空间
  sh->free = 0;
  // 如果有指定初始化内容，将它们复制到sdshdr的buf中
  if (initlen && init)
  {
    memcpy(sh->buf, init, initlen);
  }
  // 以 '\0' 结尾
  sh->buf[initlen] = '\0';

  // 返回buf部分 而不是整个sdshdr
  return (char *)sh->buf;
}

// 新建一个空的sds
sds sdsempty(void)
{
  return sdsnewlen("", 0);
}

sds sdsnew(const char *init)
{
  size_t initlen = (init == NULL) ? 0 : strlen(init);
  return sdsnewlen(init, initlen);
}

// 复制 sds
sds sdsdup(const sds s)
{
  return sdsnewlen(s, sdslen(s));
}

// 释放sds
void sdsfree(sds s)
{
  if (s == NULL)
    return;
  zfree(s - sizeof(struct sdshdr)); // 从内存头开始释放
}

void sdsclear(sds s)
{
  struct sdshdr *sh = (void *)(s - sizeof(struct sdshdr));
  sh->free += sh->len; // clear used
  sh->len = 0;
  sh->buf[0] = 0;
}

/* Low level functions exposed to the user API
 *扩展sds的内存 */
sds sdsMakeRoomFor(sds s, size_t addlen)
{
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

sds sdsRemoveFreeSpace(sds s)
{
  struct sdshdr *sh;
  sh = (void *)(s - (sizeof(struct sdshdr)));

  // 重新分配内存空间
  sh = zrealloc(sh, sizeof(struct sdshdr) + sh->len + 1); // hdr+data(initlen->truelen)+EOF
  sh->free = 0;                                           // 剩余空间为0

  return sh->buf;
}

size_t sdsAllocSize(sds s)
{
  struct sdshdr *sh = (void *)(s - sizeof(struct sdshdr));
  return sizeof(*sh) + sh->len + sh->free + 1; // hdr+used+free+EOF
}

// 根据 incr 参数 增加sds 的长度 缩减空余空间
void sdsIncrLen(sds s, int incr)
{
  struct sdshdr *sh = (void *)(s - sizeof(struct sdshdr));
  assert(sh->free >= incr); // make sure we have enough space
  sh->len += incr;
  sh->free -= incr;

  assert(sh->free >= 0);
  s[sh->len] = 0;
}

// 扩充sds长度 未使用的部分用0填充
sds sdsgrowzero(sds s, size_t len)
{
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
sds sdscatlen(sds s, const void *t, size_t len)
{
  struct sdshdr *sh;

  // 原有字符串长度
  size_t curlen = sdslen(s);

  // 扩展 sds 空间
  s = sdsMakeRoomFor(s, len);

  // 内存不足？直接返回
  if (s == NULL)
    return NULL;

  // 复制数据
  memcpy(s + curlen, t, len);

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
sds sdscat(sds s, const char *t)
{
  return sdscatlen(s, t, strlen(t));
}

sds sdscatsds(sds s, const sds t)
{
  return sdscatlen(s, t, sdslen(t));
}

// 复制指定长度 用t的指定长度覆盖s
sds sdscpylen(sds s, const char *t, size_t len)
{
  struct sdshdr *sh = (void *)(s - (sizeof(struct sdshdr)));

  // sds 现有 buf 的长度
  size_t totlen = sh->free + sh->len;

  if (totlen < len)
  {
    s = sdsMakeRoomFor(s, len - sh->len);
    if (s == NULL)
      return NULL;
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
sds sdscpy(sds s, const char *t)
{
  return sdscpylen(s, t, strlen(t));
}

#define SDS_LLSTR_SIZE 21
int sdsll2str(char *s, long long value)
{
  char *p, aux;
  unsigned long long v;
  size_t l;

  /* Generate the string representation, this method produces
   * an reversed string. */
  v = (value < 0) ? -value : value;
  p = s;
  do
  {
    *p++ = '0' + (v % 10);
    v /= 10;
  } while (v);
  if (value < 0)
    *p++ = '-';

  /* Compute length and add null term. */
  l = p - s;
  *p = '\0';

  /* Reverse the string. */
  p--;
  while (s < p)
  {
    aux = *s;
    *s = *p;
    *p = aux;
    s++;
    p--;
  }
  return l;
}

/* Identical sdsll2str(), but for unsigned long long type. */
int sdsull2str(char *s, unsigned long long v)
{
  char *p, aux;
  size_t l;

  /* Generate the string representation, this method produces
   * an reversed string. */
  p = s;
  do
  {
    *p++ = '0' + (v % 10);
    v /= 10;
  } while (v);

  /* Compute length and add null term. */
  l = p - s;
  *p = '\0';

  /* Reverse the string. */
  p--;
  while (s < p)
  {
    aux = *s;
    *s = *p;
    *p = aux;
    s++;
    p--;
  }
  return l;
}

sds sdscatvprintf(sds s, const char *fmt, va_list ap)
{
  va_list cpy;
  char staticbuf[1024], *buf = staticbuf, *t;
  size_t buflen = strlen(fmt) * 2;

  /* We try to start using a static buffer for speed.
   * If not possible we revert to heap allocation. */
  if (buflen > sizeof(staticbuf))
  {
    buf = zmalloc(buflen);
    if (buf == NULL)
      return NULL;
  }
  else
  {
    buflen = sizeof(staticbuf);
  }

  /* Try with buffers two times bigger every time we fail to
   * fit the string in the current buffer size. */
  while (1)
  {
    buf[buflen - 2] = '\0';
    va_copy(cpy, ap);
    // T = O(N)
    vsnprintf(buf, buflen, fmt, cpy);
    if (buf[buflen - 2] != '\0')
    {
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

// 打印任意数量个字符串，并将这些字符串追加到给定 sds 的末尾
sds sdscatprintf(sds s, const char *fmt, ...) 
{
  va_list ap;
  char *t;
  va_start(ap, fmt);
  t = sdscatvprintf(s, fmt, ap);
  va_end(ap);
}

sds sdscatfmt(sds s, char const *fmt, ...)
{
  struct sdshdr *sh = (void *)(s - sizeof(struct sdshdr));
  size_t initlen = sdslen(s);
  const char *f = fmt;
  int i;
  va_list ap;

  va_start(ap, fmt);
  f = fmt;
  i = initlen;
  while (*f)
  {
    char next, *str;
    size_t l;
    long long num;
    unsigned long long unum;

    /* Make sure there is always space for at least 1 char. */
    if (sh->free == 0)
    {
      s = sdsMakeRoomFor(s, 1);
      sh = (void *)(s - (sizeof(struct sdshdr)));
    }

    switch (*f)
    {
    case '%':
      next = *(f + 1);
      f++;
      switch (next)
      {
      case 's':
      case 'S':
        str = va_arg(ap, char *);
        l = (next == 's') ? strlen(str) : sdslen(str);
        if (sh->free < l)
        {
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
          if (sh->free < l)
          {
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
          if (sh->free < l)
          {
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

sds sdstrim(sds s, const char *cset)
{
  struct sdshdr *sh = (void *)(s - sizeof(struct sdshdr));
  char *start, *end, *sp, *ep;
  size_t len;

  // 设置和记录指针
  sp = start = s;
  ep = end = s + sdslen(s) - 1;

  // 修剪， T = O(N^2)
  while (sp <= end && strchr(cset, *sp))
    sp++;
  while (ep > start && strchr(cset, *ep))
    ep--;

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

// 按照索引对截取sds字符串的其中一段 注意这将修改sds底层数据
// s = sdsnew("Hello World");
// sdsrange(s,1,-1); => "ello World"
void sdsrange(sds s, int start, int end)
{
  struct sdshdr *sh = (void *)(s - (sizeof(struct sdshdr)));
  size_t newlen, len = sdslen(s);

  if (len == 0)
    return;
  if (start < 0)
  {
    start = len + start;
    if (start < 0)
      start = 0;
  }

  if (end < 0)
  {
    end = len + end;
    if (end < 0)
      end = 0;
  }
  // 计算新长度
  newlen = (start > end) ? 0 : (end - start) + 1;
  if (newlen != 0)
  {
    if (start > (signed)len)
    {
      newlen = 0;
    }
    else if (end >= (signed)len)
    {
      end = len - 1;
      newlen = (start > end) ? 0 : (end - start) + 1;
    }
  }
  else
  {
    start = 0;
  }
  // 如果有需要，对字符串进行移动
  // T = O(N)
  if (start && newlen)
    memmove(sh->buf, sh->buf + start, newlen);

  // 添加终结符
  sh->buf[newlen] = 0;

  // 更新属性
  sh->free = sh->free + (sh->len - newlen);
  sh->len = newlen;
}

void sdsupdatelen(sds s) {
  struct sdshdr *sh = (void *) (s-(sizeof(struct sdshdr)));
  int reallen = strlen(s);
  sh->free += (sh->len-reallen);
  sh->len = reallen;
}

// 相同返回 0 s1大返回正数  s2大返回负数
int sdscmp(const sds s1, const sds s2) 
{
  size_t l1, l2, minlen;
  int cmp;

  l1 = sdslen(s1);
  l2 = sdslen(s2);
  minlen = (l1 < l2) ? l1 : l2;
  cmp = memcmp(s1, s2, minlen);
  if (cmp == 0)
    return l1 - l2;

  return cmp;
}

// www.github.com .
// [www github com] 3
sds *sdssplitlen(const char *s, int len, const char *sep, int seplen, int *count)
{
  int elements = 0, slots = 5, start = 0, j;
  sds *tokens;

  if (seplen < 1 || len < 0)
    return NULL;

  tokens = zmalloc(sizeof(sds) * slots);
  if (tokens == NULL)
    return NULL;

  if (len == 0)
  {
    *count = 0;
    return tokens;
  }

  // T = O(N^2)
  for (j = 0; j < (len - (seplen - 1)); j++)
  {
    /* make sure there is room for the next element and the final one */
    if (slots < elements + 2)
    {
      sds *newtokens;

      slots *= 2;
      newtokens = zrealloc(tokens, sizeof(sds) * slots);
      if (newtokens == NULL)
        goto cleanup;
      tokens = newtokens;
    }
    /* search the separator */
    // T = O(N)
    if ((seplen == 1 && *(s + j) == sep[0]) || (memcmp(s + j, sep, seplen) == 0))
    {
      tokens[elements] = sdsnewlen(s + start, j - start);
      if (tokens[elements] == NULL)
        goto cleanup;
      elements++;
      start = j + seplen;
      j = j + seplen - 1; /* skip the separator */
    }
  }
  /* Add the final element. We are sure there is room in the tokens array. */
  tokens[elements] = sdsnewlen(s + start, len - start);
  if (tokens[elements] == NULL)
    goto cleanup;
  elements++;
  *count = elements;
  return tokens;

cleanup:
{
  int i;
  for (i = 0; i < elements; i++)
    sdsfree(tokens[i]);
  zfree(tokens);
  *count = 0;
  return NULL;
}
}

// 释放 tokens 数组中 count 个 sds
void sdsfreesplitres(sds *tokens, int count)
{
  if (!tokens)
    return;
  while (count--)
    sdsfree(tokens[count]);
  zfree(tokens);
}

// s 转小写
void sdstolower(sds s)
{
  int len = sdslen(s), j;

  for (j = 0; j < len; j++)
    s[j] = tolower(s[j]);
}

// s 转大写
void sdstoupper(sds s)
{
  int len = sdslen(s), j;

  for (j = 0; j < len; j++)
    s[j] = toupper(s[j]);
}

sds sdsfromlonglong(long long value)
{
}

sds sdscatrepr(sds s, const char *p, size_t len)
{
  s = sdscatlen(s, "\"", 1);

  while (len--)
  {
    switch (*p)
    {
    case '\\':
    case '"':
      s = sdscatprintf(s, "\\%c", *p);
      break;
    case '\n':
      s = sdscatlen(s, "\\n", 2);
      break;
    case '\r':
      s = sdscatlen(s, "\\r", 2);
      break;
    case '\t':
      s = sdscatlen(s, "\\t", 2);
      break;
    case '\a':
      s = sdscatlen(s, "\\a", 2);
      break;
    case '\b':
      s = sdscatlen(s, "\\b", 2);
      break;
    default:
      if (isprint(*p))
        s = sdscatprintf(s, "%c", *p);
      else
        s = sdscatprintf(s, "\\x%02x", (unsigned char)*p);
      break;
    }
    p++;
  }

  return sdscatlen(s, "\"", 1);
}

int is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

int hex_digit_to_int(char c) {
    switch(c) {
    case '0': return 0;
    case '1': return 1;
    case '2': return 2;
    case '3': return 3;
    case '4': return 4;
    case '5': return 5;
    case '6': return 6;
    case '7': return 7;
    case '8': return 8;
    case '9': return 9;
    case 'a': case 'A': return 10;
    case 'b': case 'B': return 11;
    case 'c': case 'C': return 12;
    case 'd': case 'D': return 13;
    case 'e': case 'E': return 14;
    case 'f': case 'F': return 15;
    default: return 0;
    }
}

/* Split a line into arguments, where every argument can be in the
 * following programming-language REPL-alike form:
 *
 * 将一行文本分割成多个参数，每个参数可以有以下的类编程语言 REPL 格式：
 *
 * foo bar "newline are supported\n" and "\xff\x00otherstuff"
 *
 * The number of arguments is stored into *argc, and an array
 * of sds is returned.
 *
 * 参数的个数会保存在 *argc 中，函数返回一个 sds 数组。
 *
 * The caller should free the resulting array of sds strings with
 * sdsfreesplitres().
 *
 * 调用者应该使用 sdsfreesplitres() 来释放函数返回的 sds 数组。
 *
 * Note that sdscatrepr() is able to convert back a string into
 * a quoted string in the same format sdssplitargs() is able to parse.
 *
 * sdscatrepr() 可以将一个字符串转换为一个带引号（quoted）的字符串，
 * 这个带引号的字符串可以被 sdssplitargs() 分析。
 *
 * The function returns the allocated tokens on success, even when the
 * input string is empty, or NULL if the input contains unbalanced
 * quotes or closed quotes followed by non space characters
 * as in: "foo"bar or "foo'
 *
 * 即使输入出现空字符串， NULL ，或者输入带有未对应的括号，
 * 函数都会将已成功处理的字符串先返回。
 *
 * 这个函数主要用于 config.c 中对配置文件进行分析。
 * 例子：
 *  sds *arr = sdssplitargs("timeout 10086\r\nport 123321\r\n");
 * 会得出
 *  arr[0] = "timeout"
 *  arr[1] = "10086"
 *  arr[2] = "port"
 *  arr[3] = "123321"
 *
 * T = O(N^2)
 */
sds *sdssplitargs(const char *line, int *argc)
{
  const char *p = line;
  char *current = NULL;
  char **vector = NULL;

  *argc = 0;
  while (1)
  {

    /* skip blanks */
    // 跳过空白
    // T = O(N)
    while (*p && isspace(*p))
      p++;

    if (*p)
    {
      /* get a token */
      int inq = 0;  /* set to 1 if we are in "quotes" */
      int insq = 0; /* set to 1 if we are in 'single quotes' */
      int done = 0;

      if (current == NULL)
        current = sdsempty();

      // T = O(N)
      while (!done)
      {
        if (inq)
        {
          if (*p == '\\' && *(p + 1) == 'x' &&
              is_hex_digit(*(p + 2)) &&
              is_hex_digit(*(p + 3)))
          {
            unsigned char byte;

            byte = (hex_digit_to_int(*(p + 2)) * 16) +
                   hex_digit_to_int(*(p + 3));
            current = sdscatlen(current, (char *)&byte, 1);
            p += 3;
          }
          else if (*p == '\\' && *(p + 1))
          {
            char c;

            p++;
            switch (*p)
            {
            case 'n':
              c = '\n';
              break;
            case 'r':
              c = '\r';
              break;
            case 't':
              c = '\t';
              break;
            case 'b':
              c = '\b';
              break;
            case 'a':
              c = '\a';
              break;
            default:
              c = *p;
              break;
            }
            current = sdscatlen(current, &c, 1);
          }
          else if (*p == '"')
          {
            /* closing quote must be followed by a space or
             * nothing at all. */
            if (*(p + 1) && !isspace(*(p + 1)))
              goto err;
            done = 1;
          }
          else if (!*p)
          {
            /* unterminated quotes */
            goto err;
          }
          else
          {
            current = sdscatlen(current, p, 1);
          }
        }
        else if (insq)
        {
          if (*p == '\\' && *(p + 1) == '\'')
          {
            p++;
            current = sdscatlen(current, "'", 1);
          }
          else if (*p == '\'')
          {
            /* closing quote must be followed by a space or
             * nothing at all. */
            if (*(p + 1) && !isspace(*(p + 1)))
              goto err;
            done = 1;
          }
          else if (!*p)
          {
            /* unterminated quotes */
            goto err;
          }
          else
          {
            current = sdscatlen(current, p, 1);
          }
        }
        else
        {
          switch (*p)
          {
          case ' ':
          case '\n':
          case '\r':
          case '\t':
          case '\0':
            done = 1;
            break;
          case '"':
            inq = 1;
            break;
          case '\'':
            insq = 1;
            break;
          default:
            current = sdscatlen(current, p, 1);
            break;
          }
        }
        if (*p)
          p++;
      }
      /* add the token to the vector */
      // T = O(N)
      vector = zrealloc(vector, ((*argc) + 1) * sizeof(char *));
      vector[*argc] = current;
      (*argc)++;
      current = NULL;
    }
    else
    {
      /* Even on empty input string return something not NULL. */
      if (vector == NULL)
        vector = zmalloc(sizeof(void *));
      return vector;
    }
  }

err:
  while ((*argc)--)
    sdsfree(vector[*argc]);
  zfree(vector);
  if (current)
    sdsfree(current);
  *argc = 0;
  return NULL;
}

/*
 * 将字符串 s 中，
 * 所有在 from 中出现的字符，替换成 to 中的字符
 *
 * For instance: sdsmapchars(mystring, "ho", "01", 2)
 * will have the effect of turning the string "hello" into "0ell1".
 *
 * 比如调用 sdsmapchars(mystring, "ho", "01", 2)
 * 就会将 "hello" 转换为 "0ell1"
 *
 * The function returns the sds string pointer, that is always the same
 * as the input pointer since no resize is needed.
 * 因为无须对 sds 进行大小调整，
 * 所以返回的 sds 输入的 sds 一样
 *
 * T = O(N^2)
 */
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen)
{
  size_t l = sdslen(s);

  // 遍历输入字符串
  for (int j = 0; j < l; j++)
  {
    // 遍历映射
    for (int i = 0; i < setlen; i++)
    {
      // 替换字符串
      if (s[j] == from[i])
      {
        s[j] = to[i];
        break;
      }
    }
  }
  return s;
}

// www github com 3 .
// www.github.com
sds sdsjoin(char **argv, int argc, char *sep)
{
  sds join = sdsempty();

  for (int j = 0; j < argc; j++)
  {
    join = sdscat(join, argv[j]);
    if (j != argc - 1)
      join = sdscat(join, sep);
  }
  return join;
}

#if 0
int main(void) {
  {
    struct sdshdr *sh;
    sds x = sdsnew("foo"), y;

    test_cond("Create a string and obtain the length",
              sdslen(x) == 3 && memcmp(x, "foo\0", 4) == 0) sdsfree(x);
    x = sdsnewlen("foo", 2);
    test_cond("Create a string with specified length",
              sdslen(x) == 2 && memcmp(x, "fo\0", 3) == 0)

        x = sdscat(x, "bar");
    test_cond("Strings concatenation",
              sdslen(x) == 5 && memcmp(x, "fobar\0", 6) == 0);

    x = sdscpy(x, "a");
    test_cond("sdscpy() against an originally longer string",
              sdslen(x) == 1 && memcmp(x, "a\0", 2) == 0) x =
        sdscpy(x, "xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk");
    test_cond("sdscpy() against an originally shorter string",
              sdslen(x) == 33 &&
                  memcmp(x, "xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk\0", 33) == 0)

        sdsfree(x);
    x = sdscatprintf(sdsempty(), "%d", 123);
    test_cond("sdscatprintf() seems working in the base case",
              sdslen(x) == 3 && memcmp(x, "123\0", 4) == 0)

        sdsfree(x);
  }
}
#endif
