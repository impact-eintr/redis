#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

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
    } else if (rawlen <= 0x3fff) { // 需要2字节编码
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

// 对前置节点的长度len进行编码 并写入到 P 中 如果 p NULL 仅仅返回长度
static unsigned int zipPrevEncodeLength(unsigned char *p, unsigned int len) {
  // 仅返回编码 len 所需要的字节
  if (p == NULL) {
    return (len < ZIP_BIGLEN) ? 1 : sizeof(len) + 1;
  } else {
    // 1 B
    if (len < ZIP_BIGLEN) {
      p[0] = len;
      return 1;
    } else {
      // 添加5字节长度标识
      p[0] = ZIP_BIGLEN;
      // 写入编码
      memcpy(p+1, &len, sizeof(len));
      memrev32ifbe(p+1); // 转换大小端
      return 1+sizeof(len);
    }
  }
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

static int zipPrevLenByteDiff(unsigned char *p, unsigned int len) {
  // 先计算被编码长度需要的字节数
  unsigned int prevlensize;
  ZIP_DECODE_PREVLENSIZE(p, prevlensize);
  return zipPrevEncodeLength(NULL, len) - prevlensize;
}


// zip entry 的内存占用
static unsigned int zipRawEntryLength(unsigned char *p) {
  unsigned int prevlensize, encoding, lensize, len;

  ZIP_DECODE_PREVLENSIZE(p, prevlensize);
  ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);

  return prevlensize + lensize + len;
}

// 尝试转换编码 将成功转换的新编码保存到encoding中
static int zipTryEncoding(unsigned char *entry, unsigned int entrylen, long long *v, unsigned char *encoding) {
  long long value;

  if (entrylen >= 32 || entrylen == 0)
    return 0;

  // 尝试转换
  if (string2ll((char*)entry, entrylen, &value)) {
    if (value >= 0 && value <= 12) {
      *encoding = ZIP_INT_IMM_MIN+value;
    } else if (value >= INT8_MIN && value <= INT8_MAX) {
      *encoding = ZIP_INT_8B;
    } else if (value >= INT16_MIN && value <= INT16_MAX) {
      *encoding = ZIP_INT_16B;
    } else if (value >= INT24_MIN && value <= INT24_MAX) {
      *encoding = ZIP_INT_24B;
    } else if (value >= INT32_MIN && value <= INT32_MAX) {
      *encoding = ZIP_INT_32B;
    } else {
      *encoding = ZIP_INT_64B;
    }
    *v = value;

    return 1; // 成功转换
  }
  return 0; // 转换失败
}

static void zipSaveInteger(unsigned char*p, int64_t value, unsigned char encoding) {
  int16_t i16;
  int32_t i32;
  int64_t i64;

  if (encoding == ZIP_INT_8B) {
    ((int8_t*)p)[0] = (int8_t)value;
  } else if (encoding == ZIP_INT_16B) {
    i16 = value;
    memcpy(p, &i16, sizeof(i16));
    memrev16ifbe(p);
  } else if (encoding == ZIP_INT_24B) {
    i32 = value<<8;
    memrev32ifbe(&i32);
    memcpy(p, ((uint8_t*)&i32)+1, sizeof(i32)-sizeof(uint8_t));
  } else if (encoding == ZIP_INT_32B) {
    i32 = value;
    memcpy(p, &i32, sizeof(i32));
    memrev32ifbe(p);
  } else if (encoding == ZIP_INT_64B) {
    i64 = value;
    memcpy(p, &i64, sizeof(i64));
    memrev64ifbe(p);
  } else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
    // Nothing to do
  } else {
    assert(NULL);
  }
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
  printf("扩展长度: %d\n", len);
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

  // 获取上一个节点的占用
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

  reqlen += zipPrevEncodeLength(NULL, prevlen);
  reqlen += zipEncodeLength(NULL, encoding, slen);

  // 只要新节点不是被添加到列表末尾 就检查 p 指向的节点的header能否编码新节点的长度
  // nextdiff 保存了新旧编码之间的字节大小差 如果这个值大于0 需要对p指向的节点的header进行扩展
  // 这里是可能造成连锁更新的
  nextdiff = (p[0] != ZIP_END) ? zipPrevLenByteDiff(p, reqlen) : 0;

  printf("reqlen %ld %d\n", reqlen, nextdiff);
  offset = p-zl; // /插入位置与表头的偏移
  zl = ziplistResize(zl, curlen+reqlen+nextdiff);
  p = zl+offset; // 找到新的插入位置

  if (p[0] != ZIP_END) { // 新元素后还有节点
    memmove(p+reqlen, p-nextdiff, curlen-offset+nextdiff);
    zipPrevEncodeLength(p+reqlen, prevlen);
    ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+reqlen);
    tail = zipEntry(p+reqlen);
    if (p[reqlen + tail.headersize + tail.len] != ZIP_END) {
      ZIPLIST_TAIL_OFFSET(zl) =
          intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)) + nextdiff);
    }
  } else {
    // 最后一个节点到表头的距离
    ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(p - zl); //
  }

  if (ZIP_IS_STR(encoding)) {
    memcpy(p, s, slen);
  } else {
    zipSaveInteger(p, value, encoding);
  }

  ZIPLIST_INCR_LENGTH(zl, 1);

  return zl;
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
  char *buf = zmalloc(1000);
  memset(buf, 'A', sizeof(char));
  ziplistPush(zl, (unsigned char*)buf, 1000, ZIPLIST_TAIL);
}

#endif
