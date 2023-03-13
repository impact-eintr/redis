#include <stddef.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "endianconv.h"
#include "ziplist.h"
#include "zmalloc.h"

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
#define ZIPLIST_INCR_LENGTH(zl, incr)                                          \
  {                                                                            \
    if (ZIPLIST_LENGTH(zl) < UINT16_MAX)                                       \
      ZIPLIST_LENGTH(zl) =                                                     \
          intrev16ifbe(intrev16ifbe(ZIPLIST_LENGTH(zl)) + incr);               \
  }

/* ziplist的节点
 * |prevrawlensize|prevrawlen|lensize|  len  |headersize|encoding| p   |
 * |4            B|4        B|4     B|4     B|4        B|1      B|N   B|
 */
typedef struct zlentry {
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
#define ZIP_ENTRY_ENCODING(ptr, encoding)                                      \
  do {                                                                         \
    (encoding) = (ptr[0]);                                                     \
    if ((encoding) < ZIP_STR_MASK)                                             \
      (encoding) &= ZIP_STR_MASK;                                              \
  } while (0)

static unsigned int zipIntSize(unsigned char encoding) {
  switch (encoding) {
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
static unsigned int zipEncodeLength(unsigned char *p, unsigned char encoding, unsigned int rawlen) {
  unsigned char len = 1, buf[5];

  // 编码字符串
  if (ZIP_IS_STR(encoding)) {
    if (rawlen <= 0x3f) {
      if (!p)
        return len;
      buf[0] = ZIP_STR_06B | rawlen;
    } else if (rawlen <= 0x3fff) {
      len += 1;
      if (!p)
        return len;
      buf[0] = ZIP_STR_14B | ((rawlen >> 8) & 0x3f);
      buf[1] = rawlen & 0xff;
    } else {
      len += 4; // 需要4字节编码
      if (!p)
        return len;
      buf[0] = ZIP_STR_32B;
      buf[1] = (rawlen >> 24) & 0xff;
      buf[2] = (rawlen >> 16) & 0xff;
      buf[3] = (rawlen >> 8) & 0xff;
      buf[4] = rawlen & 0xff;
    }
  // 编码整数
  } else {
    if (!p)
      return len;
    buf[0] = encoding; // len为1
  }

  memcpy(p, buf, len);

  return len;
}

// 解码ptr指针 取出 prevlensize
#define ZIP_DECODE_PREVLENSIZE(ptr, prevlensize)                               \
  do {                                                                         \
    if ((ptr)[0] < ZIP_BIGLEN) {                                               \
      (prevlensize) = 1;                                                       \
    } else {                                                                   \
      (prevlensize) = 5;                                                       \
    }                                                                          \
  } while (0);

// 解码ptr指针 取出 prevlensize 进而取出 prevlen
#define ZIP_DECODE_PREVLEN(ptr, prevlensize, prevlen)                          \
  do {                                                                         \
    ZIP_DECODE_PREVLENSIZE(ptr, prevlensize);                                  \
    if ((prevlensize) == 1) {                                                  \
      (prevlen) = ptr[0];                                                      \
    } else if ((prevlensize) == 5) {                                           \
      assert(sizeof(prevlensize) == 4);                                        \
      memcpy(&(prevlen), ((char *)(ptr)) + 1, 4);                              \
      memrev32ifbe(&prevlen);                                                  \
    }                                                                          \
  } while (0);

// 获取编码 编码长度 数据长度
#define ZIP_DECODE_LENGTH(ptr, encoding, lensize, len)                         \
  do {                                                                         \
    ZIP_ENTRY_ENCODING((ptr), (encoding));                                     \
    if ((encoding) < ZIP_STR_MASK) {                                           \
      if ((encoding) == ZIP_STR_06B) {                                         \
        (lensize) = 1;                                                         \
        (len) = (ptr)[0] & 0x3f;                                               \
      } else if ((encoding) == ZIP_STR_14B) {                                  \
        (lensize) = 2;                                                         \
        (len) = (((ptr)[0] & 0x3f) << 8) | (ptr)[1];                           \
      } else if (encoding == ZIP_STR_32B) {                                    \
        (lensize) = 5;                                                         \
        (len) = ((ptr)[1] << 24) | ((ptr)[2] << 16) | ((ptr)[3] << 8) |        \
                ((ptr)[4]);                                                    \
      } else {                                                                 \
        assert(NULL);                                                          \
      }                                                                        \
    } else {                                                                   \
      (lensize) = 1;                                                           \
      (len) = zipIntSize(encoding);                                            \
    }                                                                          \
  } while (0);





/*=================== 静态函数组=======================*/

// zip entry 的内存占用
static unsigned int zipRawEntryLength(unsigned char *p) {
  unsigned int prevlensize, encoding, lensize, len;

  ZIP_DECODE_PREVLENSIZE(p, prevlensize);
  ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);

  return prevlensize + lensize + len;
}

static int zipTryEncoding(unsigned char *entry, unsigned int entrylen, long long *v, unsigned char *encoding) {

}




// 将p所指向的列表节点的信息全部保存在 zlentry 中 并返回该 zlentry
static zlentry zipEntry(unsigned char *p) {
  zlentry e;

  ZIP_DECODE_PREVLEN(p, e.prevrawlensize, e.prevrawlen);

  // 计算头节点的字节数
  e.headersize = e.prevrawlensize + e.lensize;
  // 记录指针
  e.p = p;

  return e;
}

unsigned char *ziplistNew(void) {
  unsigned int bytes = ZIPLIST_HEADER_SIZE + 1;
  unsigned char *zl = zmalloc(bytes);
  ZIPLIST_BYTES(zl) = intrev32ifbe(bytes);
  ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(ZIPLIST_HEADER_SIZE);
  ZIPLIST_LENGTH(zl) = 0;

  // 设置表末端
  zl[bytes-1] = ZIP_END;

  return zl;
}

// 调整尺寸
static unsigned char *ziplistResize(unsigned char *zl, unsigned int len) {
  zl = zrealloc(zl, len);
  ZIPLIST_BYTES(zl) = intrev32ifbe(len);
  zl[len-1] = ZIP_END;

  return zl;
}

// 根据指针 p 指定的位置 将 长度为 slen 的字符串 s 插入到 zl 中
static unsigned char *__ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
  // 记录当前的长度
  size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), reqlen, prevlen = 0;
  size_t offset;
  int nextdiff = 0;
  unsigned char encoding = 0;
  long long value = 123456789;

  zlentry entry, tail;

  if (p[0] != ZIP_END) { // 列表非空 且指向某个节点
    entry = zipEntry(p);
    prevlen = entry.prevrawlen; // 上一节点的长度
  } else { // 列表可能为空
    unsigned char *ptail = ZIPLIST_ENTRY_TAIL(zl); // 尾节点
    if (ptail[0] != ZIP_END) { // 列表不为空
      prevlen = zipRawEntryLength(ptail);
    }
  }

  // 尝试将String转换为Int 这样可以节省空间
  if (zipTryEncoding(s, slen, &value, &encoding)) { // Int
    reqlen = zipIntSize(encoding);
  } else { // String
    reqlen = slen;
  }


}

unsigned char *ziplistPush(unsigned char *zl, unsigned char *s,
                           unsigned int slen, int where) {
  unsigned char *p;
  p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_END(zl);

  return __ziplistInsert(zl, p, s, slen);
}

unsigned char *ziplistIndex(unsigned char *zl, int index);
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p);
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p);
unsigned int ziplistGet(unsigned char *p, unsigned char **sval,
                        unsigned int *slen, long long *lval);
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p,
                             unsigned char *s, unsigned int slen);
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p);
unsigned char *ziplistDeleteRange(unsigned char *zl, unsigned int index,
                                  unsigned int num);
unsigned int ziplistCompare(unsigned char *p, unsigned char *s,
                            unsigned int slen);
unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr,
                           unsigned int vlen, unsigned int skip);
unsigned int ziplistLen(unsigned char *zl);
size_t ziplistBlobLen(unsigned char *zl);

#if 1
int main() {
  unsigned char* zl = ziplistNew();
  ziplistPush(zl, )
}

#endif
