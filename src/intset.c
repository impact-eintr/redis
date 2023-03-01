#include "intset.h"
#include "endianconv.h"
#include "zmalloc.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// 编码模式
#define INTSET_ENC_INT16 (sizeof(int16_t))
#define INTSET_ENC_INT32 (sizeof(int32_t))
#define INTSET_ENC_INT64 (sizeof(int64_t))

// 计算传入值 v 的编码方式
static uint8_t _intsetValueEncoding(int64_t v) {
  if (v < INT32_MIN || v > INT32_MAX)
    return INTSET_ENC_INT64;
  else if (v < INT16_MIN || v > INT16_MAX)
    return INTSET_ENC_INT32;
  else
    return INTSET_ENC_INT16;
}

intset *intsetNew(void) {
  intset *is = zmalloc(sizeof(intset));
  is->encoding = intrev32ifbe(INTSET_ENC_INT16);
  is->length = 0;
  return is;
}

static intset *intsetResize(intset *is, uint32_t len) {
  uint32_t size = len * intrev32ifbe(is->encoding);
  is = zrealloc(is, sizeof(intset) + size);

  return is;
}

static int64_t _intsetGetEncoded(intset *is, int pos, uint8_t enc) {
  int64_t v64;
  int32_t v32;
  int16_t v16;

  if (enc == INTSET_ENC_INT64) {
    memcpy(&v64, ((int64_t*)is->contents+pos), sizeof(v64));
    memrev64ifbe(&v64);
    return v64;
  } else if (enc == INTSET_ENC_INT32) {
    memcpy(&v32, ((int32_t*)is->contents+pos), sizeof(v32));
    memrev32ifbe(&v32);
    return v32;
  } else {
    memcpy(&v16, ((int16_t*)is->contents+pos), sizeof(v16));
    memrev16ifbe(&v16);
    return v16;
  }
}

static int64_t _intsetGet(intset *is, int pos) {
  return _intsetGetEncoded(is, pos, intrev32ifbe(is->encoding));
}

static void _intsetSet(intset *is, int pos, int64_t value) {
  // 取出集合的编码方式
  uint32_t encoding = intrev32ifbe(is->encoding);

  // 根据编码 ((Enc_t*)is->contents) 将数组转换回正确的类型
  // 然后 ((Enc_t*)is->contents)[pos] 定位到数组索引上
  // 接着 ((Enc_t*)is->contents)[pos] = value 将值赋给数组
  // 最后， ((Enc_t*)is->contents)+pos 定位到刚刚设置的新值上
  // 如果有需要的话， memrevEncifbe 将对值进行大小端转换
  if (encoding == INTSET_ENC_INT64) {
    ((int64_t *)is->contents)[pos] = value;
    memrev64ifbe(((int64_t *)is->contents) + pos);
  } else if (encoding == INTSET_ENC_INT32) {
    ((int32_t *)is->contents)[pos] = value;
    memrev32ifbe(((int32_t *)is->contents) + pos);
  } else {
    ((int16_t *)is->contents)[pos] = value;
    memrev16ifbe(((int16_t *)is->contents) + pos);
  }
}

// 在set中的底层数组中查找值为 value 所在的索引
static uint8_t intsetSearch(intset *is, int64_t value, uint32_t *pos) {
  int min = 0, max = intrev32ifbe(is->length) - 1, mid = -1;
  int64_t cur = -1;

  // is 为空
  if (intrev32ifbe(is->length) == 0) {
    if (pos)
      *pos = 0;
    return 0;
  } else {
    if (value > _intsetGet(is, intrev32ifbe(is->length) - 1)) {
      // 底层数组是有序的 如果 value 比数组中的最后一个数都要大
      // 添加到底层数组最后一位
      if (pos)
        *pos = intrev32ifbe(is->length);
      return 0;
    } else if (value < _intsetGet(is, 0)) {
      // 底层数组是有序的 如果 value 比数组中的最后一个数都要小
      // 添加到底层数组最前端
      if (pos)
        *pos = 0;
      return 0;
    }
  }

  // 二分查询
  while (max >= min) {
    mid = (min + max) / 2;
    cur = _intsetGet(is, mid);
    if (value > cur) {
      min = mid + 1;
    } else if (value < cur) {
      max = mid - 1;
    } else {
      break;
    }
  }

  // 检查是否已经查到了 value
  if (value == cur) {
    if (pos)
      *pos = mid;
    printf("找到 %ld\n", value);
    return 1;
  } else {
    if (pos)
      *pos = min;
    return 0;
  }
}

static intset *intsetUpgradeAndAdd(intset *is, int64_t value) {
  // 当前的编码方式
  uint8_t curenc = intrev32ifbe(is->encoding);
  // 新值所需的编码方式
  uint8_t newenc = _intsetValueEncoding(value);
  // 当前集合的元素数量
  int length = intrev32ifbe(is->length);

  int prepend = value < 0 ? 1 : 0;
  is->encoding = intrev32ifbe(newenc);
  is = intsetResize(is, intrev32ifbe(is->length)+1);
  // 迁移数据
  while(length--) {
    _intsetSet(is, length+prepend, _intsetGetEncoded(is, length, curenc));
    printf("迁移中\n");
  }


  return is;
}

static void intsetMoveTail(intset *is, uint32_t from, uint32_t to) {
  void *src, *dst;
  // 要移动的元素个数
  uint32_t bytes = intrev32ifbe(is->length) - from;
  uint32_t encoding = intrev32ifbe(is->encoding);

  if (encoding == INTSET_ENC_INT64) {
    src = (int64_t *)is->contents + from; // 移动开始的位置
    dst = (int64_t *)is->contents + to;   // 移动结束的位置
    bytes *= sizeof(int64_t);             // 需要移动多少字节
  } else if (encoding == INTSET_ENC_INT32) {
    src = (int32_t *)is->contents + from;
    dst = (int32_t *)is->contents + to;
    bytes *= sizeof(int32_t);
  } else {
    src = (int16_t *)is->contents + from;
    dst = (int16_t *)is->contents + to;
    bytes *= sizeof(int16_t);
  }
  // 移动数据
  memmove(dst, src, bytes);
}

// 添加元素
intset *intsetAdd(intset *is, int64_t value, uint8_t *success) {
  uint8_t valenc = _intsetValueEncoding(value);
  uint32_t pos;

  if (success)
    *success = 1;

  if (valenc > intrev32ifbe(is->encoding)) {
    return intsetUpgradeAndAdd(is, value); // 可以添加但需要升级
  }

  if (intsetSearch(is, value, &pos)) {
    if (success)
      *success = 0; // 不需要再次添加
    return is;
  } else { // 没找到
    is = intsetResize(is, intrev32ifbe(is->length)+1);
    if (pos < intrev32ifbe(is->length))
      intsetMoveTail(is, pos, pos+1); // 把pos移动到pos+1的位置
    _intsetSet(is, pos, value); //  插入数据

    is->length = intrev32ifbe(intrev32ifbe(is->length)+1); // 计数器递增
    return is;
  }
}

// 从整数集合中删除元素
intset *intsetRemove(intset *is, int64_t value, int *success) {
  uint8_t valenc = _intsetValueEncoding(value);
  uint32_t pos;

  if (success)
    *success = 0; // 默认失败

  if (valenc <= intrev32ifbe(is->encoding) && intsetSearch(is, value, &pos)) {
    uint32_t len = intrev32ifbe(is->length);

    if (success)
      *success = 1;

    if (pos < (len-1))
      intsetMoveTail(is, pos+1, pos);

    is = intsetResize(is,len-1);
    is->length = intrev32ifbe(len-1);
  }

  return is;
}

uint8_t intsetFind(intset *is, int64_t value) {}

int64_t intsetRandom(intset *is) {}

uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value) {}

uint32_t intsetLen(intset *is) {}

size_t intsetBlobLen(intset *is) {}

#if 0

#include <sys/time.h>

void intsetRepr(intset *is) {
  int i;
  for (i = 0; i < intrev32ifbe(is->length); i++) {
    printf("%lld\n", (uint64_t)_intsetGet(is, i));
  }
  printf("\n");
}

void error(char *err) {
  printf("%s\n", err);
  exit(1);
}

void ok(void) { printf("OK\n"); }

#define assert(_e)                                                             \
  ((_e) ? (void)0 : (_assert(#_e, __FILE__, __LINE__), exit(1)))
void _assert(char *estr, char *file, int line) {
  printf("\n\n=== ASSERTION FAILED ===\n");
  printf("==> %s:%d '%s' is not true\n", file, line, estr);
}

intset *createSet(int bits, int size) {
  uint64_t mask = (1 << bits) - 1;
  uint64_t i, value;
  intset *is = intsetNew();

  for (i = 0; i < size; i++) {
    if (bits > 32) {
      value = (rand() * rand()) & mask;
    } else {
      value = rand() & mask;
    }
    is = intsetAdd(is, value, NULL);
  }
  return is;
}

void checkConsistency(intset *is) {
  int i;

  for (i = 0; i < (intrev32ifbe(is->length) - 1); i++) {
    uint32_t encoding = intrev32ifbe(is->encoding);

    if (encoding == INTSET_ENC_INT16) {
      int16_t *i16 = (int16_t *)is->contents;
      assert(i16[i] < i16[i + 1]);
    } else if (encoding == INTSET_ENC_INT32) {
      int32_t *i32 = (int32_t *)is->contents;
      assert(i32[i] < i32[i + 1]);
    } else {
      int64_t *i64 = (int64_t *)is->contents;
      assert(i64[i] < i64[i + 1]);
    }
  }
}

long long usec(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (((long long)tv.tv_sec) * 1000000) + tv.tv_usec;
}

#include <time.h>

int main(int argc, char **argv) {
  uint8_t success;
  int i;
  intset *is;
  srand((unsigned long)time(NULL));

  printf("Value encodings: ");
  {
    assert(_intsetValueEncoding(-32768) == INTSET_ENC_INT16);
    assert(_intsetValueEncoding(+32767) == INTSET_ENC_INT16);
    assert(_intsetValueEncoding(-32769) == INTSET_ENC_INT32);
    assert(_intsetValueEncoding(+32768) == INTSET_ENC_INT32);
    assert(_intsetValueEncoding(-2147483648) == INTSET_ENC_INT32);
    assert(_intsetValueEncoding(+2147483647) == INTSET_ENC_INT32);
    assert(_intsetValueEncoding(-2147483649) == INTSET_ENC_INT64);
    assert(_intsetValueEncoding(+2147483648) == INTSET_ENC_INT64);
    assert(_intsetValueEncoding(-9223372036854775808ull) == INTSET_ENC_INT64);
    assert(_intsetValueEncoding(+9223372036854775807ull) == INTSET_ENC_INT64);
    ok();
  }

  printf("Basic adding: ");
  {
    is = intsetNew();
    is = intsetAdd(is, 5, &success);
    assert(success);
    is = intsetAdd(is, 6, &success);
    assert(success);
    is = intsetAdd(is, 4, &success);
    assert(success);
    is = intsetAdd(is, 4, &success);
    assert(!success);
    ok();
  }

  printf("Large number of random adds: ");
  {
    int inserts = 0;
    is = intsetNew();
    for (i = 0; i < 1024; i++) {
      is = intsetAdd(is, rand() % 0x800, &success);
      if (success)
        inserts++;
    }
    assert(intrev32ifbe(is->length) == inserts);
    checkConsistency(is);
    ok();
  }

  printf("Upgrade from int16 to int32: ");
  {
    is = intsetNew();
    is = intsetAdd(is, 32, NULL);
    assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
    is = intsetAdd(is, 65535, NULL);
    assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
    assert(intsetFind(is, 32));
    assert(intsetFind(is, 65535));
    checkConsistency(is);

    is = intsetNew();
    is = intsetAdd(is, 32, NULL);
    assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
    is = intsetAdd(is, -65535, NULL);
    assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
    assert(intsetFind(is, 32));
    assert(intsetFind(is, -65535));
    checkConsistency(is);
    ok();
  }

  printf("Upgrade from int16 to int64: ");
  {
    is = intsetNew();
    is = intsetAdd(is, 32, NULL);
    assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
    is = intsetAdd(is, 4294967295, NULL);
    assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
    assert(intsetFind(is, 32));
    assert(intsetFind(is, 4294967295));
    checkConsistency(is);

    is = intsetNew();
    is = intsetAdd(is, 32, NULL);
    assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
    is = intsetAdd(is, -4294967295, NULL);
    assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
    assert(intsetFind(is, 32));
    assert(intsetFind(is, -4294967295));
    checkConsistency(is);
    ok();
  }

  printf("Upgrade from int32 to int64: ");
  {
    is = intsetNew();
    is = intsetAdd(is, 65535, NULL);
    assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
    is = intsetAdd(is, 4294967295, NULL);
    assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
    assert(intsetFind(is, 65535));
    assert(intsetFind(is, 4294967295));
    checkConsistency(is);

    is = intsetNew();
    is = intsetAdd(is, 65535, NULL);
    assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
    is = intsetAdd(is, -4294967295, NULL);
    assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
    assert(intsetFind(is, 65535));
    assert(intsetFind(is, -4294967295));
    checkConsistency(is);
    ok();
  }

  printf("Stress lookups: ");
  {
    long num = 100000, size = 10000;
    int i, bits = 20;
    long long start;
    is = createSet(bits, size);
    checkConsistency(is);

    start = usec();
    for (i = 0; i < num; i++)
      intsetSearch(is, rand() % ((1 << bits) - 1), NULL);
    printf("%ld lookups, %ld element set, %lldusec\n", num, size,
           usec() - start);
  }

  printf("Stress add+delete: ");
  {
    int i, v1, v2;
    is = intsetNew();
    for (i = 0; i < 0xffff; i++) {
      v1 = rand() % 0xfff;
      is = intsetAdd(is, v1, NULL);
      assert(intsetFind(is, v1));

      v2 = rand() % 0xfff;
      is = intsetRemove(is, v2, NULL);
      assert(!intsetFind(is, v2));
    }
    checkConsistency(is);
    ok();
  }
}
#else

int main() {
  intset *is = intsetNew();
  uint8_t succ;
  intsetAdd(is, 100, &succ);
  intsetAdd(is, 200, &succ);
  intsetAdd(is, 1<<20, &succ);
  int succ2;
  intsetRemove(is, 300, &succ2);
}

#endif
