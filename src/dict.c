#include "dict.h"
#include "zmalloc.h"

#include <assert.h>
#include <limits.h>
#include <sys/time.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

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
  n.sizemask = realsize - 1; // readsize - 1=1111111...
  // T = O(N)
  n.table = zcalloc(realsize * sizeof(dictEntry *));
  n.used = 0;

  if (d->ht[0].table == NULL) { // 这是一次初始化
    // Init
    d->ht[0] = n;
  } else { // 这是一次rehash
    // rehash
    d->ht[1] = n;
    d->rehashidx = 0; // 开启 rehash
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

  return entry;
}

// 将给定的键值对添加到字典中 如果键已经存在 那么替换 返回 0 否则返回 1
int dictReplace(dict *d, void *key, void *val) {
  dictEntry *entry, auxentry;

  // 尝试直接添加
  if (dictAdd(d, key, val) == DICT_OK) {
    return 1;
  }

  entry = dictFind(d, key);

  // 先保存原有的值的指针
  auxentry = *entry;
  // 然后设置新的值
  // T = O(1)
  dictSetVal(d, entry, val);
  // 然后释放旧值
  // T = O(1)
  dictFreeVal(d, &auxentry);
  return 0;
}

/*
 * dictAddRaw() 根据给定 key 是否存在，执行以下动作：
 *
 * 1) key 已经存在，返回包含该 key 的字典节点
 * 2) key 不存在，那么将 key 添加到字典
 *
 * 不论发生以上的哪一种情况，
 * dictAddRaw() 都总是返回包含给定 key 的字典节点。
 *
 * T = O(N)
 */
dictEntry *dictReplaceRaw(dict *d, void *key) {
  dictEntry *entry = dictFind(d, key);
  return entry ? entry : dictAddRaw(d, key);
}

static int dictGenericDelete(dict *d, const void *key, int nofree) {
  unsigned int h, idx;
  dictEntry *he, *prevHe;
  int table;

  if (d->ht[0].size == 0)
    return DICT_ERR;

  // 进行 单步rehash
  if (dictIsRehashing(d))
    _dictRehashStep(d);

  // 计算哈希值
  h = dictHashKey(d, key);

  for (table = 0;table <= 1;table++) {
    idx = h & d->ht[table].sizemask;
    he = d->ht[table].table[idx];
    prevHe = NULL;

    while(he) {
      // 找到了
      if (dictCompareKeys(d, key, he->key)) {
        if (prevHe)
          prevHe->next = he->next;
        else
          d->ht[table].table[idx] = he->next;

        if (!nofree) {
          dictFreeKey(d, he);
          dictFreeVal(d, he);
        }

        zfree(he);

        d->ht[table].used--;

        // 报告已经找到
        return DICT_OK;
      }

      prevHe = he;
      he = he->next;
    }
  }
  // 没找到
  return DICT_ERR;
}

// 删除key 释放value
int dictDelete(dict *d, const void *key) {
  return dictGenericDelete(d, key, 0);
}

// 不释放 value
int dictDeleteNoFree(dict *d, const void *key) {
  return dictGenericDelete(d, key, 1);
}

// 删除哈希表上的所有节点
// 每65535次调用一次回调函数 用于窥视状态
int _dictClear(dict *d, dictht *ht, void(callback)(void *)) {
  for (unsigned long i = 0;i < ht->size && ht->used > 0;i++) {
    dictEntry *he, *nextHe;

    if (callback && (i & 65535) == 0)
      callback(d->privdata);

    // 跳过空索引
    if ((he = ht->table[i]) == NULL)
      continue;

    // 遍历整个链表
    // T = O(1)
    while (he) {
      nextHe = he->next;
      // 删除键
      dictFreeKey(d, he);
      // 删除值
      dictFreeVal(d, he);
      // 释放节点
      zfree(he);

      // 更新已使用节点计数
      ht->used--;

      // 处理下个节点
      he = nextHe;
    }
  }

  // 释放哈希表结构
  zfree(ht->table);

  // 重置哈希表属性
  _dictReset(ht);

  return DICT_OK;
}

// 清空字典
void dictRelease(dict *d) {
  // 删除并清空两个哈希表
  _dictClear(d, &d->ht[0], NULL);
  _dictClear(d, &d->ht[1], NULL);

  zfree(d);
}

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
      if (dictCompareKeys(d, key, he->key)) {
        return he;
      }
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

// 获取包含给定键的节点的值
void *dictFetchValue(dict *d, const void *key) {
  dictEntry *he;
  he = dictFind(d, key);
  printf("%s\n", (char *)he->key);
  return he ? dictGetVal(he) : NULL;
}

// 手动rehash
int dictResize(dict *d) {
  int minimal;

  // 不能在关闭 rehash 或者正在 rehash 的时候调用
  if (!dict_can_resize || dictIsRehashing(d))
    return DICT_ERR;

  // 计算让比率接近 1：1 所需要的最少节点数量
  minimal = d->ht[0].used;
  if (minimal < DICT_HT_INITIAL_SIZE)
    minimal = DICT_HT_INITIAL_SIZE;

  // 调整字典的大小 注意仅仅是标记了开启rehash 整个过程仍然是渐进式的
  // T = O(N)
  return dictExpand(d, minimal);
}

dictIterator *dictGetIterator(dict *d) {
  dictIterator *iter = zmalloc(sizeof(*iter));

  iter->d = d;
  iter->table = 0;
  iter->index = -1;
  iter->safe = 0;
  iter->entry = NULL;
  iter->nextEntry = NULL;

  return iter;
}

// 获取安全迭代器
dictIterator *dictGetSafeIterator(dict *d) {
  dictIterator *i = dictGetIterator(d);

  // 设置安全迭代器标识
  i->safe = 1;

  return i;
}

long long dictFingerprint(dict *d) {
  long long integers[6], hash = 0;
  int j;

  integers[0] = (long)d->ht[0].table;
  integers[1] = d->ht[0].size;
  integers[2] = d->ht[0].used;
  integers[3] = (long)d->ht[1].table;
  integers[4] = d->ht[1].size;
  integers[5] = d->ht[1].used;

  /* We hash N integers by summing every successive integer with the integer
   * hashing of the previous sum. Basically:
   *
   * Result = hash(hash(hash(int1)+int2)+int3) ...
   *
   * This way the same set of integers in a different order will (likely) hash
   * to a different number. */
  for (j = 0; j < 6; j++) {
    hash += integers[j];
    /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
    hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
    hash = hash ^ (hash >> 24);
    hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
    hash = hash ^ (hash >> 14);
    hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
    hash = hash ^ (hash >> 28);
    hash = hash + (hash << 31);
  }
  return hash;
}

// 返回迭代器指向的当前节点 如果迭代结束返回NULL
dictEntry *dictNext(dictIterator *iter) {
  while (1) {
    if (iter->entry == NULL) { // 1 迭代器第一次执行 2 迭代器到了链表尾部
      // 指向被迭代的哈希表
      dictht *ht =  &iter->d->ht[iter->table];
      // 初次迭代时执行
      if (iter->index == -1 && iter->table == 0) {
        if (iter->safe) { // 安全迭代
          iter->d->iterators++;
        } else { // 非安全迭代 需要计算指纹
          iter->fingerprint = dictFingerprint(iter->d);
        }
      }
      iter->index++; // 更新索引

      if (iter->index >= (signed) ht->size) {
        if (dictIsRehashing(iter->d) && iter->table == 0) {
          // 如果正在 rehash #1 ht 也在使用
          iter->table++;
          iter->index = 0;
          ht = &iter->d->ht[1];
        } else { // 如果没有 rehash 那么迭代已经完成
          break;
        }
      }

      iter->entry = ht->table[iter->index];
    } else { // 正在迭代
      iter->entry = iter->nextEntry;
    }

    // 如果当前节点不为空 那么也记录下该节点的下个节点
    // 因为安全迭代器有可能将迭代器返回的当前节点删除
    if (iter->entry) {
      iter->nextEntry = iter->entry->next;
      return iter->entry;
    }
  }

  return NULL;
}

// 释迭代器占用的资源
void dictReleaseIterator(dictIterator *iter) {
  if (!(iter->index == -1 && iter->table == 0)) { // 一个有效的迭代器
    if (iter->safe) // 释放安全迭代器时 计数器--
      iter->d->iterators--;
    else // 释放不安全迭代器时 验证指纹变化
      assert(iter->fingerprint == dictFingerprint(iter->d));
  }
  zfree(iter);
}

// 获取一个随机节点
dictEntry *dictGetRandomKey(dict *d) {
  dictEntry *he, *orighe;
  unsigned int h;
  int listlen, listele;

  // 字典为空
  if (dictSize(d) == 0)
    return NULL;

  // 进行单步 rehash
  if (dictIsRehashing(d))
    _dictRehashStep(d);

  // 如果正在 rehash ，那么将 1 号哈希表也作为随机查找的目标
  if (dictIsRehashing(d)) {
    // T = O(N)
    do {
      h = random() % (d->ht[0].size + d->ht[1].size);
      he = (h >= d->ht[0].size) ? d->ht[1].table[h - d->ht[0].size]
                                : d->ht[0].table[h];
    } while (he == NULL);
    // 否则，只从 0 号哈希表中查找节点
  } else {
    // T = O(N)
    do {
      h = random() & d->ht[0].sizemask;
      he = d->ht[0].table[h];
    } while (he == NULL);
  }

  // 目前 he 已经指向一个非空的节点链表
    // 程序将从这个链表随机返回一个节点
    listlen = 0;
    orighe = he;
    // 计算节点数量, T = O(1)
    while(he) {
        he = he->next;
        listlen++;
    }
    // 取模，得出随机节点的索引
    listele = random() % listlen;
    he = orighe;
    // 按索引查找节点
    // T = O(1)
    while(listele--) he = he->next;

    // 返回随机节点
    return he;
}

int dictGetRandomKeys(dict *d, dictEntry **des, int count) {
    int j; /* internal hash table id, 0 or 1. */
    int stored = 0;

    if (dictSize(d) < count)
        count = dictSize(d);
    while (stored < count) {
        for (j = 0; j < 2; j++) {
      /* Pick a random point inside the hash table 0 or 1. */
      unsigned int i = random() & d->ht[j].sizemask;
      int size = d->ht[j].size;

      /* Make sure to visit every bucket by iterating 'size' times. */
      while (size--) {
        dictEntry *he = d->ht[j].table[i];
        while (he) {
          /* Collect all the elements of the buckets found non
           * empty while iterating. */
          *des = he;
          des++;
          he = he->next;
          stored++;
          if (stored == count)
            return stored;
        }
        i = (i + 1) & d->ht[j].sizemask;
      }
      /* If there is only one table and we iterated it all, we should
       * already have 'count' elements. Assert this condition. */
      assert(dictIsRehashing(d) != 0);
        }
    }
    return stored; /* Never reached. */
}

void dictPrintStats(dict *d) {
  // 调试使用
}

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

/*
**  ======================== 私有函数 =========================
 */

// 初始化hash table
static int _dictInit(dict *d, dictType *type, void *privatePtr) {
  _dictReset(&d->ht[0]);
  _dictReset(&d->ht[1]);

  d->type = type; // 配置实现接口
  d->privdata = privatePtr;
  d->rehashidx = -1;
  d->iterators = 0;

  return DICT_OK;
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
      if (dictCompareKeys(d, key, he->key)) {
        return -1;
      }
      he = he->next;
    }

    // 如果运行到这里时，说明 0 号哈希表中所有节点都不包含 key
    // 如果这时 rehash 正在进行，那么继续对 1 号哈希表进行查找
    if (!dictIsRehashing(d))
      break;
  }

  return idx;
}


// 根据需要 初始化字典 或者对哈希表进行扩展
static int _dictExpandIfNeeded(dict *d) {
  if (dictIsRehashing(d))
    return DICT_OK;

  if (d->ht[0].size == 0) { // 空表进行初始化
    return dictExpand(d, DICT_HT_INITIAL_SIZE);
  }

  // 一下两个条件之一为真时，对字典进行扩展
  // 1）字典已使用节点数和字典大小之间的比率接近 1：1
  //    并且 dict_can_resize 为真
  // 2）已使用节点数和字典大小之间的比率超过 dict_force_resize_ratio
  if (d->ht[0].used >= d->ht[0].size &&
      (dict_can_resize ||
       d->ht[0].used / d->ht[0].size > dict_force_resize_ratio)) {
    // 新哈希表的大小至少是目前已使用节点数的两倍
    // T = O(N)
    return dictExpand(d, d->ht[0].used * 2);
  }

  return DICT_OK;
}

void dictEmpty(dict *d, void(callback)(void *)) {
  _dictClear(d, &d->ht[0], callback);
  _dictClear(d, &d->ht[1], callback);
  // 重置属性
  d->rehashidx = -1;
  d->iterators = 0;
}

void dictEnableResize(void) {
  dict_can_resize = 1;
}

void dictDisableResize(void) {
  dict_can_resize = 0;
}

int dictRehash(dict *d, int n) {
  if (!dictIsRehashing(d))
    return 0;

  // 进行N步迁移 每步迁移一个slot
  while (n--) {
    dictEntry *de, *nextde;

    // 如果 #0 ht 为空 表示 rehash执行结束
    if (d->ht[0].used == 0) {
      // 释放 0 号哈希表
      zfree(d->ht[0].table);
      // 将原来的 1 号哈希表设置为新的 0 号哈希表
      d->ht[0] = d->ht[1];
      // 重置旧的 1 号哈希表
      _dictReset(&d->ht[1]);
      // 关闭 rehash 标识
      d->rehashidx = -1;
      // 返回 0 ，向调用者表示 rehash 已经完成
      return 0;
    }

    // 确保 rehashidx 没有越界
    assert(d->ht[0].size > (unsigned)d->rehashidx);

    // 跳过空slot
    while(d->ht[0].table[d->rehashidx] == NULL)
      d->rehashidx++;

    de = d->ht[0].table[d->rehashidx];
    while (de) { // 将 ht[0] 的数据迁移到 ht[1] 中去
      unsigned int h;

      // 保存下个节点的指针
      nextde = de->next;

      /* Get the index in the new hash table */
      // 计算新哈希表的哈希值，以及节点插入的索引位置
      h = dictHashKey(d, de->key) & d->ht[1].sizemask;
      //printf("转移 %d  %d\n",*(int *)de->key, h);

      // 插入节点到新哈希表
      de->next = d->ht[1].table[h];
      d->ht[1].table[h] = de;

      // 更新计数器
      d->ht[0].used--;
      d->ht[1].used++;

      // 继续处理下个节点
      de = nextde;
    }

    d->ht[0].table[d->rehashidx] = NULL; // 迁移后的链表标记为空
    d->rehashidx++; // 更新 rehash 索引
  }

  return 1;
}

long long timeInMillseconds(void) {
  struct timeval tv;

  gettimeofday(&tv, NULL);
  return (((long long)tv.tv_sec) * 1000) + (tv.tv_usec / 1000);
}

// 在 ms 时间内 以100 step 为单位进行Rehash
int dictRehashMilliseconds(dict *d, int ms) {
  long long start = timeInMillseconds();
  int rehashes = 0;

  while(dictRehash(d, 100)) {
    rehashes+=100;
    if (timeInMillseconds()-start > ms) {
      break;
    }
  }
  return rehashes;
}

void dictSetHashFunctionSeed(uint32_t seed) { dict_hash_function_seed = seed; }

uint32_t dictGetHashFunctionSeed(void) { return dict_hash_function_seed; }

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
static unsigned long rev(unsigned long v) {
  unsigned long s = 8 * sizeof(v); // bit size; must be power of 2
  unsigned long mask = ~0;
  while ((s >>= 1) > 0) {
    mask ^= (mask << s);
    v = ((v >> s) & mask) | ((v << s) & ~mask);
  }
  return v;
}

/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * dictScan() 函数用于迭代给定字典中的元素。
 *
 * Iterating works in the following way:
 *
 * 迭代按以下方式执行：
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 *    一开始，你使用 0 作为游标来调用函数。
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value that you must use in the next call.
 *    函数执行一步迭代操作，
 *    并返回一个下次迭代时使用的新游标。
 * 3) When the returned cursor is 0, the iteration is complete.
 *    当函数返回的游标为 0 时，迭代完成。
 * 函数保证，在迭代从开始到结束期间，一直存在于字典的元素肯定会被迭代到，
 * 但一个元素可能会被返回多次。
 *
 * 每当一个元素被返回时，回调函数 fn 就会被执行，
 * fn 函数的第一个参数是 privdata ，而第二个参数则是字典节点 de 。
 */
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn,
                       void *privdata) {
  dictht *t0, *t1;
  const dictEntry *de;
  unsigned long m0, m1;

  // 跳过空字典
  if (dictSize(d) == 0)
    return 0;

  if (!dictIsRehashing(d)) { // 迭代只有一个ht的字典
    // 指向哈希表
    t0 = &(d->ht[0]);

    // 记录 mask
    m0 = t0->sizemask;
    de = t0->table[v & m0];
    while (de) {
      fn(privdata, de);
      de = de->next;
    }
  } else { // 迭代有两个ht的字典
    t0 = &(d->ht[0]);
    t1 = &(d->ht[1]);

    // 指向两个哈希表
    t0 = &d->ht[0];
    t1 = &d->ht[1];

    /* Make sure t0 is the smaller and t1 is the bigger table */
    // 确保 t0 比 t1 要小
    if (t0->size > t1->size) {
      t0 = &d->ht[1];
      t1 = &d->ht[0];
    }

    // 记录掩码
    m0 = t0->sizemask;
    m1 = t1->sizemask;

    /* Emit entries at cursor */
    // 指向桶，并迭代桶中的所有节点
    de = t0->table[v & m0];
    while (de) {
      fn(privdata, de);
      de = de->next;
    }

    // Iterate over indices in larger table             // 迭代大表中的桶
    // that are the expansion of the index pointed to   // 这些桶被索引的 expansion 所指向
    // by the cursor in the smaller table
    do {
      /* Emit entries at cursor */
      // 指向桶，并迭代桶中的所有节点
      de = t1->table[v & m1];
      while (de) {
        fn(privdata, de);
        de = de->next;
      }

      /* Increment bits not covered by the smaller mask */
      v = (((v | m0) + 1) & ~m0) | (v & m0);

      /* Continue while bits covered by mask difference is non-zero */
    } while (v & (m0 ^ m1));
  }

  v |= ~m0;

  v = rev(v);
  v++;
  v = rev(v);

  return v;
}


#if 0
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
static int keyCompare(void *privdata, const void *key1, const void *key2) {
  return *(int *)key1 == *(int *)key2;
}

// 销毁键的函数
static void keyDestructor(void *privdata, void *key) {}

// 销毁值的函数
static void valDestructor(void *privdata, void *obj) {}

int main() {
  dictType *type ;
  dict *d;

  if ((type = zmalloc(sizeof(*type))) == NULL) {
    return -1;
  }
  type->hashFunction = hashFunction;
  type->keyCompare = keyCompare;
  type->keyDup = keyDup;
  type->valDup = valDup;

  d = dictCreate(type, NULL);
  if (dictExpand(d, 10) == DICT_ERR) {
    return -1;
  }
  int k = 0;
  int v = 100;
  for (int i = 0;i < 100;i++) {
    k++;
    v++;
    dictAdd(d, &k, &v);
  }

  dictIterator *di;
  di = dictGetSafeIterator(d);
  dictEntry *de;
  while((de = dictNext(di))) {
    printf("%d\t", *(int *)de->v.val);
  }
  printf("\n");
  dictReleaseIterator(di);
}

#endif
