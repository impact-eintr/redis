#include "dict.h"
#include "zmalloc.h"

#include <limits.h>
#include <sys/time.h>
#include <ctype.h>
#include <stdio.h>

/*
 * 通过 dictEnableResize() 和 dictDisableResize() 两个函数，
 * 程序可以手动地允许或阻止哈希表进行 rehash ，
 * 这在 Redis 使用子进程进行保存操作时，可以有效地利用 copy-on-write 机制。
 *
 * 需要注意的是，并非所有 rehash 都会被 dictDisableResize 阻止：
 * 如果已使用节点的数量和字典大小之间的比率，
 * 大于字典强制 rehash 比率 dict_force_resize_ratio ，
 * 那么 rehash 仍然会（强制）进行。
 */

// 只是dict是否启动 rehash
static int dict_can_resize = 1;

// 强制rehash的比率
static unsigned int dict_force_resize_ratio = 5;

/*
** ----------------------- 私有方法 ---------------------------
 */
static int _dictExpandIfNeeded(dict *ht);
static unsigned long _dictNextPower(unsigned long size);
static int _dictKeyIndex(dict *ht, const void *key);
static int _dictInit(dict *ht, dictType *type, void *privDataPtr);


// Thomas Wang's 32 bits MIX Function
unsigned int dictIntHashFunction(unsigned int key) {
  key += ~(key << 15);
  key ^= (key >> 10);
  key += (key << 3);
  key ^= (key >> 6);
  key += ~(key << 11);
  key ^= (key >> 16);
  return key;
}

/* Identity hash function for integer keys */
unsigned int dictIdentityHashFunction(unsigned int key) { return key; }

static uint32_t dict_hash_function_seed = 5381;

// 初始化hash的属性
static void _dictReset(dictht *ht) {
  ht->table = NULL;
  ht->size = 0;
  ht->sizemask = 0;
  ht->used = 0;
}

// Create a new dict
dict *dictCreate(dictType *type, void *privDataPtr) {
  dict *d;

  d = zmalloc(sizeof(*d));

  _dictInit(d, type, privDataPtr);

  return d;
}

// 初始化hash table
int _dictInit(dict *d, dictType *type, void *privatePtr) {
  _dictReset(&d->ht[0]);
  _dictReset(&d->ht[1]);

  d->type = type; // 配置实现接口
  d->privdata = privatePtr;
  d->rehashidx = -1;
  d->iterators = 0;

  return DICT_OK;
}

/*
 * 创建一个新的哈希表，并根据字典的情况，选择以下其中一个动作来进行：
 *
 * 1) 如果字典的 0 号哈希表为空，那么将新哈希表设置为 0 号哈希表
 * 2) 如果字典的 0 号哈希表非空，那么将新哈希表设置为 1 号哈希表，
 *    并打开字典的 rehash 标识，使得程序可以开始对字典进行 rehash
 *
 * size 参数不够大，或者 rehash 已经在进行时，返回 DICT_ERR 。
 *
 * 成功创建 0 号哈希表，或者 1 号哈希表时，返回 DICT_OK 。
 *
 * T = O(N)
 */
int dictExpand(dict *d, unsigned long size){
  // 新的哈希表
  dictht n;
  unsigned long realsize = _dictNextPower(size);
  if (dictIsRehashing(d) || d->ht[0].used > size) {
    return DICT_ERR;
  }

  // 为哈希表分配空间，并将所有指针指向 NULL
  n.size = realsize;
  n.sizemask = realsize - 1;
  // T = O(N)
  n.table = zcalloc(realsize * sizeof(dictEntry *));
  n.used = 0;

  if (d->ht[0].table == NULL) {
    // Init
    d->ht[0] = n;
  } else {
    // rehash
    d->ht[1] = n;
  }

  return DICT_OK;
}

/*
 * 在字典不存在安全迭代器的情况下，对字典进行单步 rehash 。
 *
 * 字典有安全迭代器的情况下不能进行 rehash ，
 * 因为两种不同的迭代和修改操作可能会弄乱字典。
 *
 * 这个函数被多个通用的查找、更新操作调用，
 * 它可以让字典在被使用的同时进行 rehash 。
 *
 * T = O(1)
 */

static void _dictRehashStep(dict *d) {
  if (d->iterators == 0)
    dictRehash(d, 1);
}

// 添加键值对到字典中
int dictAdd(dict *d, void *key, void *val) {
  dictEntry *entry = dictAddRaw(d, key);

  if (!entry)
    return DICT_ERR;

  // 值不存在 设置节点的值
  dictSetVal(d, entry, val);

  return DICT_OK;
}
// 尝试添加键到字典中 添加键成功了 才能设置值
dictEntry *dictAddRaw(dict *d, void *key) {
  int index;
  dictEntry *entry;
  dictht *ht;

  if (dictIsRehashing(d))
    _dictRehashStep(d);

  // 计算键在hash table 中的索引值
  if ((index = _dictKeyIndex(d, key)) == -1) {
    return NULL;
  }

  // 如果ht在rehash 将新数据存在ht[1]中
  ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
  entry = zmalloc(sizeof(*entry));
  entry->next = ht->table[index]; // 不冲突就是NULL 有冲突就是后面的节点
  ht->table[index] = entry;
  ht->used++;

  // 设置新节点的键
  dictSetKey(d, entry, key);
  printf("TEST%d\n", *(int *)key);

  return entry;
}

int dictReplace(dict *d, void *key, void *val) {
  return 0;
}

dictEntry *dictReplaceRaw(dict *d, void *key) {
  return 0;
}

int dictDelete(dict *d, const void *key) {
  return 0;
}

int dictDeleteNoFree(dict *d, const void *key) {
  return 0;
}

void dictRelease(dict *d) {}

dictEntry *dictFind(dict *d, const void *key) {
  dictEntry *he;
  unsigned int h, idx, table;

  if (d->ht[0].size == 0)
    return NULL;

  // 条件允许的话进行 rehash
  if (dictIsRehashing(d))
    _dictRehashStep(d);

  // 计算键的哈希值
  h = dictHashKey(d, key);

  // 遍历两个哈希表
  for (table = 0;table <= 1; table++) {
    // 计算哈希值
    idx = h & d->ht[table].sizemask;

    // 遍历给定索引上的链表的所有节点
    he = d->ht[table].table[idx];
    while(he) {
      if (dictCompareKeys(d, key, he->key))
        return he;
      he = he->next;
    }
    // 如果程序遍历完 0 号哈希表，仍然没找到指定的键的节点
    // 那么程序会检查字典是否在进行 rehash ，
    // 然后才决定是直接返回 NULL ，还是继续查找 1 号哈希表
    if (!dictIsRehashing(d))
      return NULL;
  }

  return NULL; // 两个表都没找到
}

void *dictFetchValue(dict *d, const void *key) {}

int dictResize(dict *d) {

}

dictIterator *dictGetIterator(dict *d) {}
dictIterator *dictGetSafeIterator(dict *d) {}
dictEntry *dictNext(dictIterator *iter) {}
void dictReleaseIterator(dictIterator *iter) {}
dictEntry *dictGetRandomKey(dict *d) {}
int dictGetRandomKeys(dict *d, dictEntry **des, int count) {}
void dictPrintStats(dict *d) {}

unsigned int dictGenHashFunction(const void *key, int len) {
  uint32_t seed = dict_hash_function_seed;
  const uint32_t m = 0x5bd1e995;
  const int r = 24;

  /* Initialize the hash to a 'random' value */
  uint32_t h = seed ^ len;

  /* Mix 4 bytes at a time into the hash */
  const unsigned char *data = (const unsigned char *)key;

  while (len >= 4) {
    uint32_t k = *(uint32_t *)data;

    k *= m;
    k ^= k >> r;
    k *= m;

    h *= m;
    h ^= k;

    data += 4;
    len -= 4;
  }

  /* Handle the last few bytes of the input array  */
  switch (len) {
  case 3:
    h ^= data[2] << 16;
  case 2:
    h ^= data[1] << 8;
  case 1:
    h ^= data[0];
    h *= m;
  };

  /* Do a few final mixes of the hash to ensure the last few
   * bytes are well-incorporated. */
  h ^= h >> 13;
  h *= m;
  h ^= h >> 15;

  return (unsigned int)h;
}

unsigned int dictGenCaseHashFunction(const unsigned char *buf, int len) {
  unsigned int hash = (unsigned int)dict_hash_function_seed;

  while (len--)
    hash = ((hash << 5) + hash) + (tolower(*buf++)); /* hash * 33 + c */
  return hash;
}

// 计算第一个大于等于 size 的2的N次方
static unsigned long _dictNextPower(unsigned long size) {
  unsigned long i = DICT_HT_INITIAL_SIZE;

  if (size >= LONG_MAX)
    return LONG_MAX;

  while(1) {
    if (i >= size) {
      return i;
    }
    i *= 2;
  }
}

// 返回可以将 key 插入到 哈希表的索引位置
static int _dictKeyIndex(dict *d, const void *key) {
  unsigned int h, idx, table;
  dictEntry *he;

  // 尝试去单步 rehash
  if (_dictExpandIfNeeded(d) == DICT_ERR) {
    return -1;
  }

  // 计算 key 的哈希值
  h = dictHashKey(d, key);
  // T = O(1)
  for (table = 0; table <= 1; table++) {

    // 计算索引值
    idx = h & d->ht[table].sizemask;

    /* Search if this slot does not already contain the given key */
    // 查找 key 是否存在
    // T = O(1)
    he = d->ht[table].table[idx];
    while (he) {
      if (dictCompareKeys(d, key, he->key))
        return -1;
      he = he->next;
    }

    // 如果运行到这里时，说明 0 号哈希表中所有节点都不包含 key
    // 如果这时 rehahs 正在进行，那么继续对 1 号哈希表进行 rehash
    if (!dictIsRehashing(d))
      break;
  }

  return idx;
}

void dictEmpty(dict *d, void(callback)(void *)) {}

void dictEnableResize(void) {}

void dictDisableResize(void) {}

int dictRehash(dict *d, int n) {
  return 0;
}

int dictRehashMilliseconds(dict *d, int ms) {
  struct timeval tv;

  gettimeofday(&tv, NULL);
  return (((long long)tv.tv_sec) * 1000) + (tv.tv_usec / 1000);
}

void dictSetHashFunctionSeed(uint32_t seed) { dict_hash_function_seed = seed; }

uint32_t dictGetHashFunctionSeed(void) { return dict_hash_function_seed; }

unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn,
                       void *privdata) {
  return 0;
}


#if 1
#include <string.h>

// 计算哈希值的函数
static unsigned int hashFunction(const void *key) {
  unsigned int iKey = *(unsigned int*)key;
  return dictIntHashFunction(iKey);
}

// 复制键的函数
static void *keyDup(void *privdata, const void *key) {
  void *copy = zmalloc(sizeof(*key));
  memcpy(copy, key, sizeof(*key));
  return copy;
}

// 复制值的函数
static void *valDup(void *privdata, const void *obj) {
  void *copy = zmalloc(sizeof(*obj));
  memcpy(copy, obj, sizeof(*obj));
  return copy;
}

// 对比键的函数
static int keyCompare(void *privdata, const void *key1, const void *key2);

// 销毁键的函数
static void keyDestructor(void *privdata, void *key) {}

// 销毁值的函数
static void valDestructor(void *privdata, void *obj) {}

int main() {
  dictType *type;
  dict *d;

  if ((type = zmalloc(sizeof(*type))) == NULL) {
    return -1;
  }
  type->hashFunction = hashFunction;
  type->keyDup = keyDup;
  type->valDup = valDup;

  d = dictCreate(type, NULL);
  if (dictExpand(d, 10) == DICT_ERR) {
    return -1;
  }
  int k = 1;
  int v = 1;
  dictAdd(d, &k, &v);

}

#endif
