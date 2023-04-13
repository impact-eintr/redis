#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "limits.h"
#include "endianconv.h"
#include "sds.h"
#include "ziplist.h"
#include "zmalloc.h"
#include "util.h"

// =========================== 各种宏定义 ==================================
/*
 * ziplist 末端标识符，以及 5 字节长长度标识符
 */
#define ZIP_END 255
#define ZIP_BIGLEN 254

#define ZIP_STR_MASK 0xc0
#define ZIP_INT_MASK 0x30

/*
 * 字符串编码类型
 */
#define ZIP_STR_06B (0 << 6)
#define ZIP_STR_14B (1 << 6)
#define ZIP_STR_32B (2 << 6)

/*
 * 整数编码类型
 */
#define ZIP_INT_16B (0xc0 | 0 << 4)
#define ZIP_INT_32B (0xc0 | 1 << 4)
#define ZIP_INT_64B (0xc0 | 2 << 4)
#define ZIP_INT_24B (0xc0 | 3 << 4)
#define ZIP_INT_8B 0xfe

/* 4 bit integer immediate encoding
 *
 * 4 位整数编码的掩码和类型
 */
#define ZIP_INT_IMM_MASK 0x0f
#define ZIP_INT_IMM_MIN 0xf1 /* 11110001 */
#define ZIP_INT_IMM_MAX 0xfd /* 11111101 */
#define ZIP_INT_IMM_VAL(v) (v & ZIP_INT_IMM_MASK)

/*
 * 24 位整数的最大值和最小值
 */
#define INT24_MAX 0x7fffff
#define INT24_MIN (-INT24_MAX - 1)

/* Macro to determine type
 *
 * 查看给定编码 enc 是否字符串编码
 */
#define ZIP_IS_STR(enc) (((enc)&ZIP_STR_MASK) < ZIP_STR_MASK)

/* Utility macros */
/*
 * ziplist 属性宏
 */
// 定位到 ziplist 的 bytes 属性，该属性记录了整个 ziplist 所占用的内存字节数
// 用于取出 bytes 属性的现有值，或者为 bytes 属性赋予新值
#define ZIPLIST_BYTES(zl) (*((uint32_t *)(zl)))
// 定位到 ziplist 的 offset 属性，该属性记录了到达表尾节点的偏移量
// 用于取出 offset 属性的现有值，或者为 offset 属性赋予新值
#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t *)((zl) + sizeof(uint32_t))))
// 定位到 ziplist 的 length 属性，该属性记录了 ziplist 包含的节点数量
// 用于取出 length 属性的现有值，或者为 length 属性赋予新值
#define ZIPLIST_LENGTH(zl) (*((uint16_t *)((zl) + sizeof(uint32_t) * 2)))
// 返回 ziplist 表头的大小
#define ZIPLIST_HEADER_SIZE (sizeof(uint32_t) * 2 + sizeof(uint16_t))
// 返回指向 ziplist 第一个节点（的起始位置）的指针
#define ZIPLIST_ENTRY_HEAD(zl) ((zl) + ZIPLIST_HEADER_SIZE)
// 返回指向 ziplist 最后一个节点（的起始位置）的指针
#define ZIPLIST_ENTRY_TAIL(zl) ((zl) + intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)))
// 返回指向 ziplist 末端 ZIP_END （的起始位置）的指针
#define ZIPLIST_ENTRY_END(zl) ((zl) + intrev32ifbe(ZIPLIST_BYTES(zl)) - 1)

/*----------------------------------------------------------------*
 * |zlbytes|zltail|zllen|entry1|entry2|entry3|...|entryN|zlend|
 * |4     B|4    B|2   B|  ... | ...  | ...  |...|entryN|1   B|
 *
 */

/* We know a positive increment can only be 1 because entries can only be
 * pushed one at a time. */
/*
 * 增加 ziplist 的节点数
 *
 * T = O(1)
 */
#define ZIPLIST_INCR_LENGTH(zl, incr)                            \
  {                                                              \
    if (ZIPLIST_LENGTH(zl) < UINT16_MAX)                         \
      ZIPLIST_LENGTH(zl) =                                       \
          intrev16ifbe(intrev16ifbe(ZIPLIST_LENGTH(zl)) + incr); \
  }

/* ziplist的节点
 * |prevrawlensize|prevrawlen|lensize|  len  |headersize|encoding| p   |
 * |4            B|4        B|4     B|4     B|4        B|1      B|N   B|
 */
typedef struct zlentry
{
  // 编码前置节点长度的字节数  前置节点长度
  unsigned int prevrawlensize, prevrawlen;

  // 编码节点长度的字节数 当前节点长度
  unsigned int lensize, len;

  // 当前节点header的大小 = prevrawlensize + lensize
  unsigned int headersize;

  // 当前节点使用的编码类型
  unsigned char encoding;

  // 指向当前节点的指针
  unsigned char *p;
} zlentry;

// 获取编码
#define ZIP_ENTRY_ENCODING(ptr, encoding) \
  do                                      \
  {                                       \
    (encoding) = (ptr[0]);                \
    if ((encoding) < ZIP_STR_MASK)        \
      (encoding) &= ZIP_STR_MASK;         \
  } while (0)

static unsigned int zipIntSize(unsigned char encoding)
{
  switch (encoding)
  {
  case ZIP_INT_8B:
    return 1;
  case ZIP_INT_16B:
    return 2;
  case ZIP_INT_24B:
    return 3;
  case ZIP_INT_32B:
    return 4;
  case ZIP_INT_64B:
    return 8;
  default:
    return 0;
  }

  assert(NULL);
  return 0;
}

// 编码节点长度 写入 p 中 返回消耗了多少字节
static unsigned int zipEncodeLength(unsigned char *p, unsigned char encoding, unsigned int rawlen)
{
  unsigned char len = 1, buf[5];

  // 编码字符串
  if (ZIP_IS_STR(encoding))
  {
    if (rawlen <= 0x3f)
    { // 不需要额外的字节
      if (!p)
        return len;
      buf[0] = ZIP_STR_06B | rawlen;
    }
    else if (rawlen <= 0x3fff)
    { // 需要额外的一个字节
      len += 1;
      if (!p)
        return len;
      buf[0] = ZIP_STR_14B | ((rawlen >> 8) & 0x3f);
      buf[1] = rawlen & 0xff;
    }
    else
    {
      len += 4; // 需要额外4字节编码
      if (!p)
        return len;
      buf[0] = ZIP_STR_32B;
      buf[1] = (rawlen >> 24) & 0xff;
      buf[2] = (rawlen >> 16) & 0xff;
      buf[3] = (rawlen >> 8) & 0xff;
      buf[4] = rawlen & 0xff;
    }
    // 编码整数
  }
  else
  {
    if (!p)
      return len;
    buf[0] = encoding; // len为1
  }

  memcpy(p, buf, len);

  return len;
}

// 获取编码 编码长度 数据长度
#define ZIP_DECODE_LENGTH(ptr, encoding, lensize, len)                  \
  do                                                                    \
  {                                                                     \
    ZIP_ENTRY_ENCODING((ptr), (encoding));                              \
    if ((encoding) < ZIP_STR_MASK)                                      \
    {                                                                   \
      if ((encoding) == ZIP_STR_06B)                                    \
      {                                                                 \
        (lensize) = 1;                                                  \
        (len) = (ptr)[0] & 0x3f;                                        \
      }                                                                 \
      else if ((encoding) == ZIP_STR_14B)                               \
      {                                                                 \
        (lensize) = 2;                                                  \
        (len) = (((ptr)[0] & 0x3f) << 8) | (ptr)[1];                    \
      }                                                                 \
      else if (encoding == ZIP_STR_32B)                                 \
      {                                                                 \
        (lensize) = 5;                                                  \
        (len) = ((ptr)[1] << 24) | ((ptr)[2] << 16) | ((ptr)[3] << 8) | \
                ((ptr)[4]);                                             \
      }                                                                 \
      else                                                              \
      {                                                                 \
        assert(NULL);                                                   \
      }                                                                 \
    }                                                                   \
    else                                                                \
    {                                                                   \
      (lensize) = 1;                                                    \
      (len) = zipIntSize(encoding);                                     \
    }                                                                   \
  } while (0);

/*=================== 静态函数组=======================*/

// 对前置节点的长度len进行编码 并写入到 P 中 如果 p NULL 仅仅返回长度
static unsigned int zipPrevEncodeLength(unsigned char *p, unsigned int len)
{
  // 仅返回编码 len 所需要的字节
  if (p == NULL)
  {
    return (len < ZIP_BIGLEN) ? 1 : sizeof(len) + 1;
  }
  else
  {
    // 1 B
    if (len < ZIP_BIGLEN)
    {
      p[0] = len;
      return 1;
    }
    else
    {
      // 添加5字节长度标识
      p[0] = ZIP_BIGLEN;
      // 写入编码
      memcpy(p + 1, &len, sizeof(len));
      memrev32ifbe(p + 1); // 转换大小端
      return 1 + sizeof(len);
    }
  }
}

// 将原本需要 1 字节的长度使用5字节保存
static void zipPrevEncodeLengthForceLarge(unsigned char *p, unsigned int len)
{
  if (p == NULL)
    return;
  p[0] = ZIP_BIGLEN;

  memcpy(p + 1, &len, sizeof(len));
  memrev32ifbe(p + 1);
}

// 解码ptr指针 取出 prevlensize
#define ZIP_DECODE_PREVLENSIZE(ptr, prevlensize) \
  do                                             \
  {                                              \
    if ((ptr)[0] < ZIP_BIGLEN)                   \
    {                                            \
      (prevlensize) = 1;                         \
    }                                            \
    else                                         \
    {                                            \
      (prevlensize) = 5;                         \
    }                                            \
  } while (0);

// 解码ptr指针 取出 prevlensize 进而取出 prevlen
#define ZIP_DECODE_PREVLEN(ptr, prevlensize, prevlen) \
  do                                                  \
  {                                                   \
    ZIP_DECODE_PREVLENSIZE(ptr, prevlensize);         \
    if ((prevlensize) == 1)                           \
    {                                                 \
      (prevlen) = ptr[0];                             \
    }                                                 \
    else if ((prevlensize) == 5)                      \
    {                                                 \
      assert(sizeof(prevlensize) == 4);               \
      memcpy(&(prevlen), ((char *)(ptr)) + 1, 4);     \
      memrev32ifbe(&prevlen);                         \
    }                                                 \
  } while (0);

// p 准备插入的位置 计算新编码与之前位置节点的编码的差距
static int zipPrevLenByteDiff(unsigned char *p, unsigned int len)
{
  // 先计算被编码长度需要的字节数
  unsigned int prevlensize;
  ZIP_DECODE_PREVLENSIZE(p, prevlensize);
  // printf("%d %d\n", zipPrevEncodeLength(NULL, len) , prevlensize);
  return zipPrevEncodeLength(NULL, len) - prevlensize;
}

// zip entry 的内存占用
static unsigned int zipRawEntryLength(unsigned char *p)
{
  unsigned int prevlensize, encoding, lensize, len;

  ZIP_DECODE_PREVLENSIZE(p, prevlensize);
  ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);

  return prevlensize + lensize + len;
}

// 尝试转换编码 将成功转换的新编码保存到encoding中
static int zipTryEncoding(unsigned char *entry, unsigned int entrylen, long long *v, unsigned char *encoding)
{
  long long value;

  if (entrylen >= 32 || entrylen == 0)
    return 0;

  // 尝试转换
  if (string2ll((char *)entry, entrylen, &value))
  {
    if (value >= 0 && value <= 12)
    {
      *encoding = ZIP_INT_IMM_MIN + value;
    }
    else if (value >= INT8_MIN && value <= INT8_MAX)
    {
      *encoding = ZIP_INT_8B;
    }
    else if (value >= INT16_MIN && value <= INT16_MAX)
    {
      *encoding = ZIP_INT_16B;
    }
    else if (value >= INT24_MIN && value <= INT24_MAX)
    {
      *encoding = ZIP_INT_24B;
    }
    else if (value >= INT32_MIN && value <= INT32_MAX)
    {
      *encoding = ZIP_INT_32B;
    }
    else
    {
      *encoding = ZIP_INT_64B;
    }
    *v = value;

    return 1; // 成功转换
  }
  return 0; // 转换失败
}

static void zipSaveInteger(unsigned char *p, int64_t value, unsigned char encoding)
{
  int16_t i16;
  int32_t i32;
  int64_t i64;

  if (encoding == ZIP_INT_8B)
  {
    ((int8_t *)p)[0] = (int8_t)value;
  }
  else if (encoding == ZIP_INT_16B)
  {
    i16 = value;
    memcpy(p, &i16, sizeof(i16));
    memrev16ifbe(p);
  }
  else if (encoding == ZIP_INT_24B)
  {
    i32 = value << 8;
    memrev32ifbe(&i32);
    memcpy(p, ((uint8_t *)&i32) + 1, sizeof(i32) - sizeof(uint8_t));
  }
  else if (encoding == ZIP_INT_32B)
  {
    i32 = value;
    memcpy(p, &i32, sizeof(i32));
    memrev32ifbe(p);
  }
  else if (encoding == ZIP_INT_64B)
  {
    i64 = value;
    memcpy(p, &i64, sizeof(i64));
    memrev64ifbe(p);
  }
  else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX)
  {
    // Nothing to do
  }
  else
  {
    assert(NULL);
  }
}

static int64_t zipLoadInteger(unsigned char *p, unsigned char encoding)
{
  int16_t i16;
  int32_t i32;
  int64_t i64, ret = 0;

  if (encoding == ZIP_INT_8B)
  {
    ret = ((int8_t *)p)[0];
  }
  else if (encoding == ZIP_INT_16B)
  {
    memcpy(&i16, p, sizeof(i16));
    memrev16ifbe(&i16);
    ret = i16;
  }
  else if (encoding == ZIP_INT_32B)
  {
    memcpy(&i32, p, sizeof(i32));
    memrev32ifbe(&i32);
    ret = i32;
  }
  else if (encoding == ZIP_INT_24B)
  {
    i32 = 0;
    memcpy(((uint8_t *)&i32) + 1, p, sizeof(i32) - sizeof(uint8_t));
    memrev32ifbe(&i32);
    ret = i32 >> 8;
  }
  else if (encoding == ZIP_INT_64B)
  {
    memcpy(&i64, p, sizeof(i64));
    memrev64ifbe(&i64);
    ret = i64;
  }
  else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX)
  {
    ret = (encoding & ZIP_INT_IMM_MASK) - 1;
  }
  else
  {
    assert(NULL);
  }

  return ret;
}

// 将p所指向的列表节点的信息全部保存在 zlentry 中 并返回该 zlentry
static zlentry zipEntry(unsigned char *p)
{
  zlentry e;

  ZIP_DECODE_PREVLEN(p, e.prevrawlensize, e.prevrawlen);
  ZIP_DECODE_LENGTH(p + e.prevrawlensize, e.encoding, e.lensize, e.len);

  // 计算头节点的字节数
  e.headersize = e.prevrawlensize + e.lensize;
  // 记录指针
  e.p = p;

  //printf("entry: prevrawlensize: %d, prevrawlen: %d, lensize: %d, len: %d\n", e.prevrawlensize, e.prevrawlen, e.lensize, e.len);
  return e;
}

unsigned char *ziplistNew(void)
{
  unsigned int bytes = ZIPLIST_HEADER_SIZE + 1;
  unsigned char *zl = zmalloc(bytes);
  ZIPLIST_BYTES(zl) = intrev32ifbe(bytes);
  ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(ZIPLIST_HEADER_SIZE);
  ZIPLIST_LENGTH(zl) = 0;

  // 设置表末端
  zl[bytes - 1] = ZIP_END;

  return zl;
}

// 调整尺寸
static unsigned char *ziplistResize(unsigned char *zl, unsigned int len)
{
  zl = zrealloc(zl, len);
  ZIPLIST_BYTES(zl) = intrev32ifbe(len);
  zl[len - 1] = ZIP_END;

  return zl;
}

// 当前节点需要更多内存来保存的新节点的长度 下一个节点需要更多内存来保存当前节点的长度  级联更新
static unsigned char *__ziplistCascadeUpdate(unsigned char *zl, unsigned char *p)
{
  size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), rawlen, rawlensize;
  size_t offset, noffset, extra;
  unsigned char *np; // next p
  zlentry cur, next;

  while (p[0] != ZIP_END)
  {
    cur = zipEntry(p);
    rawlen = cur.headersize + cur.len;              // 当前节点的总长度
    rawlensize = zipPrevEncodeLength(NULL, rawlen); // 计算下一个节点编码当前节点需要的长度
    if (p[rawlen] == ZIP_END)                       // 没有后续空间需要更新
      break;

    next = zipEntry(p + rawlen);   // 下一个节点
    if (next.prevrawlen == rawlen) // 不需要额外的编码空间
      break;

    //printf("下一个节点保存编码当前节点 长度 %ld 需要 %ld字节来编码\n", rawlen, rawlensize);
    if (next.prevrawlensize < rawlensize)
    {
      // 真正地级联更新
      //printf("开始级联更新\n");
      offset = p - zl;
      extra = rawlensize - next.prevrawlensize; // 计算需要增加的字节数量
      zl = ziplistResize(zl, curlen + extra);
      p = zl + offset; // 新位置

      np = p + rawlen;
      noffset = np - zl; // 下个节点在表中的偏移量
      if ((zl + intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))) != np)
      {
        ZIPLIST_TAIL_OFFSET(zl) =
            intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + extra);
      }

      /* Move the tail to the back. */
      // 向后移动 cur 节点之后的数据，为 cur 的新 header 腾出空间
      //
      // 示例：
      //
      // | header | value |  ==>  | header |    | value |  ==>  | header      | value |
      //                                   |<-->|
      //                            为新 header 腾出的空间
      // T = O(N)
      memmove(np + rawlensize, np + next.prevrawlensize,
              curlen - noffset - next.prevrawlensize - 1);
      // 将新的前一节点长度值编码进新的 next 节点的 header
      // T = O(1)
      zipPrevEncodeLength(np, rawlen);

      /* Advance the cursor */
      // 移动指针，继续处理下个节点
      p += rawlen;
      curlen += extra;
    }
    else
    {
      if (next.prevrawlensize > rawlensize)
      {
        zipPrevEncodeLengthForceLarge(p + rawlen, rawlen); // 更新下一个节点保存的当前节点的长度
      }
      else
      {
        zipPrevEncodeLength(p + rawlen, rawlen); // 更新下一个节点保存的当前节点的长度
      }
      break;
    }
  }
  return zl;
}

static unsigned char *__ziplistDelete(unsigned char *zl, unsigned char *p, unsigned int num)
{
  unsigned int i, totlen, deleted = 0;
  size_t offset;
  int nextdiff = 0;
  zlentry first, tail;

  // 计算被删除节点总共占用的内存字节数
  // 以及被删除节点的总个数
  // T = O(N)
  first = zipEntry(p);
  for (i = 0; p[0] != ZIP_END && i < num; i++)
  {
    p += zipRawEntryLength(p);
    deleted++;
  }

  // totlen 是所有被删除节点总共占用的内存字节数
  totlen = p - first.p;
  if (totlen > 0)
  {
    if (p[0] != ZIP_END)
    {

      // 执行这里，表示被删除节点之后仍然有节点存在

      /* Storing `prevrawlen` in this entry may increase or decrease the
       * number of bytes required compare to the current `prevrawlen`.
       * There always is room to store this, because it was previously
       * stored by an entry that is now being deleted. */
      // 因为位于被删除范围之后的第一个节点的 header 部分的大小
      // 可能容纳不了新的前置节点，所以需要计算新旧前置节点之间的字节数差
      // T = O(1)
      nextdiff = zipPrevLenByteDiff(p, first.prevrawlen);
      // 如果有需要的话，将指针 p 后退 nextdiff 字节，为新 header 空出空间
      p -= nextdiff;
      // 将 first 的前置节点的长度编码至 p 中
      // T = O(1)
      zipPrevEncodeLength(p, first.prevrawlen);

      /* Update offset for tail */
      // 更新到达表尾的偏移量
      // T = O(1)
      ZIPLIST_TAIL_OFFSET(zl) =
          intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) - totlen);

      /* When the tail contains more than one entry, we need to take
       * "nextdiff" in account as well. Otherwise, a change in the
       * size of prevlen doesn't have an effect on the *tail* offset. */
      // 如果被删除节点之后，有多于一个节点
      // 那么程序需要将 nextdiff 记录的字节数也计算到表尾偏移量中
      // 这样才能让表尾偏移量正确对齐表尾节点
      // T = O(1)
      tail = zipEntry(p);
      if (p[tail.headersize + tail.len] != ZIP_END)
      {
        ZIPLIST_TAIL_OFFSET(zl) =
            intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + nextdiff);
      }

      /* Move tail to the front of the ziplist */
      // 从表尾向表头移动数据，覆盖被删除节点的数据
      // T = O(N)
      memmove(first.p, p,
              intrev32ifbe(ZIPLIST_BYTES(zl)) - (p - zl) - 1);
    }
    else
    {

      // 执行这里，表示被删除节点之后已经没有其他节点了

      /* The entire tail was deleted. No need to move memory. */
      // T = O(1)
      ZIPLIST_TAIL_OFFSET(zl) =
          intrev32ifbe((first.p - zl) - first.prevrawlen);
    }

    /* Resize and update length */
    // 缩小并更新 ziplist 的长度
    offset = first.p - zl;
    zl = ziplistResize(zl, intrev32ifbe(ZIPLIST_BYTES(zl)) - totlen + nextdiff);
    ZIPLIST_INCR_LENGTH(zl, -deleted);
    p = zl + offset;

    /* When nextdiff != 0, the raw length of the next entry has changed, so
     * we need to cascade the update throughout the ziplist */
    // 如果 p 所指向的节点的大小已经变更，那么进行级联更新
    // 检查 p 之后的所有节点是否符合 ziplist 的编码要求
    // T = O(N^2)
    if (nextdiff != 0)
      zl = __ziplistCascadeUpdate(zl, p);
  }

  return zl;
}

// 根据指针 p 指定的位置 将 长度为 slen 的字符串 s 插入到 zl 中
static unsigned char *__ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen)
{
  // 记录当前的长度
  size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), reqlen, prevlen = 0;
  size_t offset;
  int nextdiff = 0;
  unsigned char encoding = 0;
  long long value = 123456789;

  zlentry entry, tail;

  // 获取上一个节点的占用
  if (p[0] != ZIP_END)
  { // 列表非空 且指向某个节点
    //printf("有牛，别看!\n");
    entry = zipEntry(p);
    prevlen = entry.prevrawlen; // 上一节点的长度
  }
  else
  { // 列表可能为空
    //printf("撅\n");
    unsigned char *ptail = ZIPLIST_ENTRY_TAIL(zl); // 尾节点
    if (ptail[0] != ZIP_END)
    { // 列表不为空
      prevlen = zipRawEntryLength(ptail);
    }
  }

  // 尝试将String转换为Int 这样可以节省空间
  if (zipTryEncoding(s, slen, &value, &encoding))
  { // Int
    reqlen = zipIntSize(encoding);
  }
  else
  { // String
    reqlen = slen;
  }

  reqlen += zipPrevEncodeLength(NULL, prevlen);
  reqlen += zipEncodeLength(NULL, encoding, slen);
  // printf("编码前一个 %d 编码当前 %d\n",zipPrevEncodeLength(NULL, prevlen), zipEncodeLength(NULL, encoding, slen));

  // 只要新节点不是被添加到列表末尾 就检查 p 指向的节点的header能否编码新节点的长度
  // nextdiff 保存了新旧编码之间的字节大小差 如果这个值大于0 需要对p指向的节点的header进行扩展
  // 这里是可能造成连锁更新的
  nextdiff = (p[0] != ZIP_END) ? zipPrevLenByteDiff(p, reqlen) : 0;

  offset = p - zl; // /插入位置与表头的偏移
  zl = ziplistResize(zl, curlen + reqlen + nextdiff);
  p = zl + offset; // 找到新的插入位置

  // printf("reqlen %ld nextdiff %d cal %ld curlen %ld\n", reqlen, nextdiff, curlen-offset-1+nextdiff, curlen);
  if (p[0] != ZIP_END)
  { // 新元素后还有节点
    memmove(p + reqlen, p - nextdiff, curlen - offset - 1 + nextdiff);
    zipPrevEncodeLength(p + reqlen, reqlen);                                                // 重新计算上一个节点的编码长度
    ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + reqlen); // 更新表尾偏移
    tail = zipEntry(p + reqlen);                                                            // 下一个节点是否是最后一个节点
    if (p[reqlen + tail.headersize + tail.len] != ZIP_END)
    {
      ZIPLIST_TAIL_OFFSET(zl) =
          intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + nextdiff); // 更新由于编码导致的偏移
    }
  }
  else
  {
    // 最后一个节点到表头的距离
    ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(p - zl);
  }

  // 当前节点需要更多内存来保存的新节点的长度 下一个节点需要更多内存来保存当前节点的长度  级联更新
  if (nextdiff != 0)
  {
    zl = __ziplistCascadeUpdate(zl, p + reqlen); // p+reqlen 表示插入位置的原有节点
    p = zl + offset;
  }

  p += zipPrevEncodeLength(p, prevlen);
  p += zipEncodeLength(p, encoding, slen);

  if (ZIP_IS_STR(encoding))
  {
    memcpy(p, s, slen);
  }
  else
  {
    zipSaveInteger(p, value, encoding);
  }

  ZIPLIST_INCR_LENGTH(zl, 1);

  return zl;
}

unsigned char *ziplistPush(unsigned char *zl, unsigned char *s,
                           unsigned int slen, int where)
{
  unsigned char *p;
  p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_END(zl);

  return __ziplistInsert(zl, p, s, slen);
}

// 根据给定的索引 遍历找到指定位置上的节点
unsigned char *ziplistIndex(unsigned char *zl, int index)
{
  unsigned char *p;

  zlentry entry;

  // 处理负数索引
  if (index < 0)
  {
    unsigned int len = ziplistLen(zl);
    index = len + index;
  }

  // 定位到表头节点
  p = ZIPLIST_ENTRY_HEAD(zl);

  // T = O(N)
  while (p[0] != ZIP_END && index--)
  {
    // 后移指针
    // T = O(1)
    p += zipRawEntryLength(p);
  }

  // 返回结果
  return (p[0] == ZIP_END || index > 0) ? NULL : p;
}

// 下一个节点
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p)
{
  ((void)zl);

  if (p[0] == ZIP_END)
  {
    return NULL; // 已经是最后一个节点
  }

  p += zipRawEntryLength(p);
  if (p[0] == ZIP_END)
  {
    return NULL; // 来到最后一个节点
  }

  return p;
}

// 前一个节点
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p)
{
  zlentry entry;
  if (p[0] == ZIP_END)
  {
    p = ZIPLIST_ENTRY_TAIL(zl);
    return (p[0] == ZIP_END) ? NULL : p;
  }
  else if (p == ZIPLIST_ENTRY_HEAD(zl))
  {
    return NULL;
  }
  else
  {
    entry = zipEntry(p);
    assert(entry.prevrawlen > 0);
    return p - entry.prevrawlen; // 指向前一个节点
  }
}

unsigned int ziplistGet(unsigned char *p, unsigned char **sstr,
                        unsigned int *slen, long long *sval)
{
  zlentry entry;
  if (p == NULL || p[0] == ZIP_END)
    return 0;

  if (sstr)
    *sstr = NULL;

  entry = zipEntry(p);
  if (ZIP_IS_STR(entry.encoding))
  {
    if (sstr)
    {
      *slen = entry.len;
      *sstr = p + entry.headersize;
    }
  }
  else
  {
    if (sval)
    {
      *sval = zipLoadInteger(p + entry.headersize, entry.encoding);
    }
  }
  return 1;
}

unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p,
                             unsigned char *s, unsigned int slen)
{
  return __ziplistInsert(zl, p, s, slen);
}

unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p)
{

  size_t offset = *p - zl;
  zl = __ziplistDelete(zl, *p, 1);

  *p = zl + offset;
  return zl;
}

unsigned char *ziplistDeleteRange(unsigned char *zl, unsigned int index,
                                  unsigned int num)
{
  // 根据索引定位到节点
  // T = O(N)
  unsigned char *p = ziplistIndex(zl, index);

  // 连续删除 num 个节点
  // T = O(N^2)
  return (p == NULL) ? zl : __ziplistDelete(zl, p, num);
}

unsigned int ziplistCompare(unsigned char *p, unsigned char *sstr,
                            unsigned int slen)
{

  zlentry entry;
  unsigned char sencoding;
  long long zval, sval;
  if (p[0] == ZIP_END)
    return 0;

  // 取出节点
  entry = zipEntry(p);
  if (ZIP_IS_STR(entry.encoding))
  {

    // 节点值为字符串，进行字符串对比
    if (entry.len == slen)
    {
      // T = O(N)
      return memcmp(p + entry.headersize, sstr, slen) == 0;
    }
    else
    {
      return 0;
    }
  }
  else
  {

    // 节点值为整数，进行整数对比

    /* Try to compare encoded values. Don't compare encoding because
     * different implementations may encoded integers differently. */
    if (zipTryEncoding(sstr, slen, &sval, &sencoding))
    {
      // T = O(1)
      zval = zipLoadInteger(p + entry.headersize, entry.encoding);
      return zval == sval;
    }
  }

  return 0;
}

unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip)
{
  int skipcnt = 0;
  unsigned char vencoding = 0;
  long long vll = 0;

  // 只要未到达列表末端，就一直迭代
  // T = O(N^2)
  while (p[0] != ZIP_END)
  {
    unsigned int prevlensize, encoding, lensize, len;
    unsigned char *q;

    ZIP_DECODE_PREVLENSIZE(p, prevlensize);
    ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
    q = p + prevlensize + lensize;

    if (skipcnt == 0)
    {

      /* Compare current entry with specified entry */
      // 对比字符串值
      // T = O(N)
      if (ZIP_IS_STR(encoding))
      {
        if (len == vlen && memcmp(q, vstr, vlen) == 0)
        {
          return p;
        }
      }
      else
      {
        /* Find out if the searched field can be encoded. Note that
         * we do it only the first time, once done vencoding is set
         * to non-zero and vll is set to the integer value. */
        // 因为传入值有可能被编码了，
        // 所以当第一次进行值对比时，程序会对传入值进行解码
        // 这个解码操作只会进行一次
        if (vencoding == 0)
        {
          if (!zipTryEncoding(vstr, vlen, &vll, &vencoding))
          {
            /* If the entry can't be encoded we set it to
             * UCHAR_MAX so that we don't retry again the next
             * time. */
            vencoding = UCHAR_MAX;
          }
          /* Must be non-zero by now */
          assert(vencoding);
        }

        /* Compare current entry with specified entry, do it only
         * if vencoding != UCHAR_MAX because if there is no encoding
         * possible for the field it can't be a valid integer. */
        // 对比整数值
        if (vencoding != UCHAR_MAX)
        {
          // T = O(1)
          long long ll = zipLoadInteger(q, encoding);
          if (ll == vll)
          {
            return p;
          }
        }
      }

      /* Reset skip count */
      skipcnt = skip;
    }
    else
    {
      /* Skip entry */
      skipcnt--;
    }

    /* Move to next entry */
    // 后移指针，指向后置节点
    p = q + len;
  }

  // 没有找到指定的节点
  return NULL;
}

unsigned int ziplistLen(unsigned char *zl)
{
  unsigned int len = 0;

  // 节点数小于 UINT16_MAX
  // T = O(1)
  if (intrev16ifbe(ZIPLIST_LENGTH(zl)) < UINT16_MAX)
  {
    len = intrev16ifbe(ZIPLIST_LENGTH(zl));

    // 节点数大于 UINT16_MAX 时，需要遍历整个列表才能计算出节点数
    // T = O(N)
  }
  else
  {
    unsigned char *p = zl + ZIPLIST_HEADER_SIZE;
    while (*p != ZIP_END)
    {
      p += zipRawEntryLength(p);
      len++;
    }

    /* Re-store length if small enough */
    if (len < UINT16_MAX)
      ZIPLIST_LENGTH(zl) = intrev16ifbe(len);
  }

  return len;
}

size_t ziplistBlobLen(unsigned char *zl)
{
  return intrev32ifbe(ZIPLIST_BYTES(zl));
}

void ziplistRepr(unsigned char *zl)
{
  unsigned char *p;
  int index = 0;
  zlentry entry;

  printf("{total bytes %d} "
         "{length %u}\n"
         "{tail offset %u}\n",
         intrev32ifbe(ZIPLIST_BYTES(zl)), intrev16ifbe(ZIPLIST_LENGTH(zl)),
         intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)));
  p = ZIPLIST_ENTRY_HEAD(zl);
  while (*p != ZIP_END)
  {
    entry = zipEntry(p);
    printf("{"
           "addr 0x%08lx, "
           "index %2d, "
           "offset %5ld, "
           "rl: %5u, "
           "hs %2u, "
           "pl: %5u, "
           "pls: %2u, "
           "payload %5u"
           "} ",
           (long unsigned)p, index, (unsigned long)(p - zl),
           entry.headersize + entry.len, entry.headersize, entry.prevrawlen,
           entry.prevrawlensize, entry.len);
    p += entry.headersize;
    if (ZIP_IS_STR(entry.encoding))
    {
      if (entry.len > 40)
      {
        if (fwrite(p, 40, 1, stdout) == 0)
          perror("fwrite");
        printf("...");
      }
      else
      {
        if (entry.len && fwrite(p, entry.len, 1, stdout) == 0)
          perror("fwrite");
      }
    }
    else
    {
      printf("%lld", (long long)zipLoadInteger(p, entry.encoding));
    }
    printf("\n");
    p += entry.len;
    index++;
  }
  printf("{end}\n\n");
}

#if 0
#include <sys/time.h>
#include "adlist.h"
#include "sds.h"

#define debug(f, ...) { if (DEBUG) printf(f, __VA_ARGS__); }

unsigned char *createList() {
    unsigned char *zl = ziplistNew();
    zl = ziplistPush(zl, (unsigned char*)"foo", 3, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"quux", 4, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"hello", 5, ZIPLIST_HEAD);
    zl = ziplistPush(zl, (unsigned char*)"1024", 4, ZIPLIST_TAIL);
    return zl;
}

unsigned char *createIntList() {
    unsigned char *zl = ziplistNew();
    char buf[32];

    sprintf(buf, "100");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "128000");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "-100");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "4294967296");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "non integer");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "much much longer non integer");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    return zl;
}

long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

void stress(int pos, int num, int maxsize, int dnum) {
    int i,j,k;
    unsigned char *zl;
    char posstr[2][5] = { "HEAD", "TAIL" };
    long long start;
    for (i = 0; i < maxsize; i+=dnum) {
        zl = ziplistNew();
        for (j = 0; j < i; j++) {
            zl = ziplistPush(zl,(unsigned char*)"quux",4,ZIPLIST_TAIL);
        }

        /* Do num times a push+pop from pos */
        start = usec();
        for (k = 0; k < num; k++) {
            zl = ziplistPush(zl,(unsigned char*)"quux",4,pos);
            zl = ziplistDeleteRange(zl,0,1);
        }
        printf("List size: %8d, bytes: %8d, %dx push+pop (%s): %6lld usec\n",
            i,intrev32ifbe(ZIPLIST_BYTES(zl)),num,posstr[pos],usec()-start);
        zfree(zl);
    }
}

void pop(unsigned char *zl, int where) {
    unsigned char *p, *vstr;
    unsigned int vlen;
    long long vlong;

    p = ziplistIndex(zl,where == ZIPLIST_HEAD ? 0 : -1);
    if (ziplistGet(p,&vstr,&vlen,&vlong)) {
        if (where == ZIPLIST_HEAD)
            printf("Pop head: ");
        else
            printf("Pop tail: ");

        if (vstr)
            if (vlen && fwrite(vstr,vlen,1,stdout) == 0) perror("fwrite");
        else
            printf("%lld", vlong);

        printf("\n");
        ziplistDeleteRange(zl,-1,1);
    } else {
        printf("ERROR: Could not pop\n");
        exit(1);
    }
}

int randstring(char *target, unsigned int min, unsigned int max) {
    int p = 0;
    int len = min+rand()%(max-min+1);
    int minval, maxval;
    switch(rand() % 3) {
    case 0:
        minval = 0;
        maxval = 255;
    break;
    case 1:
        minval = 48;
        maxval = 122;
    break;
    case 2:
        minval = 48;
        maxval = 52;
    break;
    default:
        assert(NULL);
    }

    while(p < len)
        target[p++] = minval+rand()%(maxval-minval+1);
    return len;
}

void verify(unsigned char *zl, zlentry *e) {
    int i;
    int len = ziplistLen(zl);
    zlentry _e;

    for (i = 0; i < len; i++) {
        memset(&e[i], 0, sizeof(zlentry));
        e[i] = zipEntry(ziplistIndex(zl, i));

        memset(&_e, 0, sizeof(zlentry));
        _e = zipEntry(ziplistIndex(zl, -len + i));

        assert(memcmp(&e[i], &_e, sizeof(zlentry)) == 0);
    }
}

int main(int argc, char **argv) {
    unsigned char *zl, *p;
    unsigned char *entry;
    unsigned int elen;
    long long value;

    /* If an argument is given, use it as the random seed. */
    if (argc == 2)
        srand(atoi(argv[1]));

    zl = createIntList();
    ziplistRepr(zl);

    zl = createList();
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_HEAD);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    printf("Regression test deleting next to last entries:\n");
    {
        char v[3][257];
        zlentry e[3];
        int i;

        for (i = 0; i < (sizeof(v) / sizeof(v[0])); i++)
        {
            memset(v[i], 'a' + i, sizeof(v[0]));
        }

        v[0][256] = '\0';
        v[1][1] = '\0';
        v[2][256] = '\0';

        zl = ziplistNew();
        for (i = 0; i < (sizeof(v) / sizeof(v[0])); i++)
        {
            zl = ziplistPush(zl, (unsigned char *)v[i], strlen(v[i]), ZIPLIST_TAIL);
        }

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);
        assert(e[2].prevrawlensize == 1);

        /* Deleting entry 1 will increase `prevrawlensize` for entry 2 */
        unsigned char *p = e[1].p;
        zl = ziplistDelete(zl, &p);

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);

        printf("SUCCESS\n\n");
    }

    printf("Stress with random payloads of different encoding:\n");
    {
        int i,j,len,where;
        unsigned char *p;
        char buf[1024];
        int buflen;
        list *ref;
        listNode *refnode;

        /* Hold temp vars from ziplist */
        unsigned char *sstr;
        unsigned int slen;
        long long sval;

        for (i = 0; i < 20000; i++) {
            zl = ziplistNew();
            ref = listCreate();
            listSetFreeMethod(ref,(void (*)(void*))sdsfree);
            len = rand() % 256;

            /* Create lists */
            for (j = 0; j < len; j++) {
                where = (rand() & 1) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
                if (rand() % 2) {
                    buflen = randstring(buf,1,sizeof(buf)-1);
                } else {
                    switch(rand() % 3) {
                    case 0:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) >> 20);
                        break;
                    case 1:
                        buflen = sprintf(buf,"%lld",(0LL + rand()));
                        break;
                    case 2:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) << 20);
                        break;
                    default:
                        assert(NULL);
                    }
                }

                /* Add to ziplist */
                zl = ziplistPush(zl, (unsigned char*)buf, buflen, where);

                /* Add to reference list */
                if (where == ZIPLIST_HEAD) {
                    listAddNodeHead(ref,sdsnewlen(buf, buflen));
                } else if (where == ZIPLIST_TAIL) {
                    listAddNodeTail(ref,sdsnewlen(buf, buflen));
                } else {
                    assert(NULL);
                }
            }

            assert(listLength(ref) == ziplistLen(zl));
            for (j = 0; j < len; j++) {
                /* Naive way to get elements, but similar to the stresser
                 * executed from the Tcl test suite. */
                p = ziplistIndex(zl,j);
                refnode = listIndex(ref,j);

                assert(ziplistGet(p,&sstr,&slen,&sval));
                if (sstr == NULL) {
                    buflen = sprintf(buf,"%lld",sval);
                } else {
                    buflen = slen;
                    memcpy(buf,sstr,buflen);
                    buf[buflen] = '\0';
                }
                assert(memcmp(buf,listNodeValue(refnode),buflen) == 0);
            }
            zfree(zl);
            listRelease(ref);
        }
        printf("SUCCESS\n\n");
    }

    printf("Stress with variable ziplist size:\n");
    {
        stress(ZIPLIST_HEAD,100000,16384,256);
        stress(ZIPLIST_TAIL,100000,16384,256);
    }

    return 0;
}

#endif