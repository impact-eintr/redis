#ifndef DICT_H_
#define DICT_H_

/*----------------------------------------------------------------*
 * A MEMORY HASH TABLE
 * support for insert delete replace search and get a readom element
 * collosions are handled by chaining
 */

#include <stdint.h>

// 操作状态
#define DICT_OK 0
#define DICT_ERR 1

// 如果字典的私有数据不使用时
// 用这个宏来避免编译器错误
#define DICT_NOTUSED(V) ((void)V)

// hash entry
typedef struct dictEntry {
  // key
  void *key;
  // value
  union {
    void *val;
    uint64_t u64;
    int64_t s64;
  } v;
  // 拉链法的下一个节点
  struct dictEntry *next;
} dictEntry;

// 字典接口
typedef struct dictType {
  // 计算哈希值的函数
  unsigned int (*hashFunction)(const void *key);

  // 复制键的函数
  void *(*keyDup)(void *privdata, const void *key);

  // 复制值的函数
  void *(*valDup)(void *privdata, const void *obj);

  // 对比键的函数
  int (*keyCompare)(void *privdata, const void *key1, const void *key2);

  // 销毁键的函数
  void (*keyDestructor)(void *privdata, void *key);

  // 销毁值的函数
  void (*valDestructor)(void *privdata, void *obj);
} dictType;

// Hash Table 每个dict 都会使用了个HashTable 作为底层数据存储
// 从而实现渐进式rehash
typedef struct dictht {
  // 哈希表数组 指针数组
  dictEntry **table;

  // 哈希表大小 cap(map)
  unsigned long size;

  // 哈希表大小掩码 用于计算索引值
  unsigned long sizemask;

  // 该哈希表已有的节点数量 len(map)
  unsigned long used;
} dictht;

// 字典
typedef struct dict {
  dictType *type; // 实现接口

  // 私有数据
  void *privdata;

  // 哈希表
  dictht ht[2];

  // rehash 索引 不进行rehash时 = -1
  int rehashidx;

  // 目前正在运行的安全迭代器的数量
  int iterators;
} dict;

// 字典迭代器
// if safe == 1 we could invoke dictAdd dictFind and other function modify the
// dict if not so the only function we could call is dictNext
typedef struct dictIterator {
  // 目标字典
  dict *d;

  // table ：正在被迭代的哈希表号码，值可以是 0 或 1 。
  // index ：迭代器当前所指向的哈希表索引位置。
  // safe ：标识这个迭代器是否安全
  int table, index, safe;

  // entry ：当前迭代到的节点的指针
  // nextEntry ：当前迭代节点的下一个节点
  //             因为在安全迭代器运作时， entry 所指向的节点可能会被修改，
  //             所以需要一个额外的指针来保存下一节点的位置，
  //             从而防止指针丢失
  dictEntry *entry, *nextEntry;

  long long fingerprint;
} dictIterator;

// 字典遍历函数
typedef void(dictScanFunction)(void *privatedata, const dictEntry *de);

// 初始大小
#define DICT_HT_INITIAL_SIZE 4

/*
** ------------------------- Macros ---------------------------
*/
// 释放给定字典节点的值
#define dictFreeVal(d, entry)                                                  \
  if ((d)->type->valDestructor)                                                \
  (d)->type->valDestructor((d)->privdata, (entry)->v.val)

// 设置给定字典节点的值 采用复制
#define dictSetVal(d, entry, _val_)                                            \
  do {                                                                         \
    if ((d)->type->valDup)                                                     \
      entry->v.val = (d)->type->valDup((d)->privdata, _val_);                  \
    else                                                                       \
      entry->v.val = (_val_);                                                  \
  } while (0)

// 将一个有符号整数设为节点的值
#define dictSetSignedIntegerVal(entry, _val_)                                  \
  do {                                                                         \
    entry->v.s64 = _val_;                                                      \
  } while (0)

// 将一个无符号整数设为节点的值
#define dictSetUnsignedIntegerVal(entry, _val_)                                \
  do {                                                                         \
    entry->v.u64 = _val_;                                                      \
  } while (0)

// 设置给定字典节点的键 采用复制
#define dictSetKey(d, entry, _key_)                                            \
  do {                                                                         \
    if ((d)->type->keyDup)                                                     \
      entry->key = (d)->type->keyDup((d)->privdata, _key_);                    \
    else                                                                       \
      entry->key = (_key_);                                                    \
  } while (0)

// 比对两个键 有自定义比较就采用 否则直接比较
#define dictCompareKeys(d, key1, key2)                                         \
  (((d)->type->keyCompare) ? (d)->type->keyCompare((d)->privdata, key1, key2)  \
                           : (key1) == (key2))

// 计算给定键的哈希值
#define dictHashKey(d, key) (d)->type->hashFunction(key)

// 返回获取给定节点的键
#define dictGetKey(he) ((he)->key)

// 返回获取给定节点的值
#define dictGetVal(he) ((he)->v.val)

// 返回获取给定节点的有符号整数值
#define dictGetSignedIntegerVal(he) ((he)->v.s64)

// 返回给定节点的无符号整数值
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)

// 返回给定字典的大小 两个hashtable的总和
#define dictSlots(d) ((d)->ht[0].size + (d)->ht[1].size)

// 返回字典的已有节点数量 两个hashtable的总和
#define dictSize(d) ((d)->ht[0].used + (d)->ht[1].used)

// 查看字典是否正在 rehash
#define dictIsRehashing(ht) ((ht)->rehashidx != -1)

/* API */
dict *dictCreate(dictType *type, void *privDataPtr);
int dictExpand(dict *d, unsigned long size);
int dictAdd(dict *d, void *key, void *val);
dictEntry *dictAddRaw(dict *d, void *key);
int dictReplace(dict *d, void *key, void *val);
dictEntry *dictReplaceRaw(dict *d, void *key);
int dictDelete(dict *d, const void *key);
int dictDeleteNoFree(dict *d, const void *key);
void dictRelease(dict *d);
dictEntry *dictFind(dict *d, const void *key);
void *dictFetchValue(dict *d, const void *key);
int dictResize(dict *d);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
dictEntry *dictNext(dictIterator *iter);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
int dictGetRandomKeys(dict *d, dictEntry **des, int count);
void dictPrintStats(dict *d);
unsigned int dictGenHashFunction(const void *key, int len);
unsigned int dictGenCaseHashFunction(const unsigned char *buf, int len);
void dictEmpty(dict *d, void(callback)(void *));
void dictEnableResize(void);
void dictDisableResize(void);
int dictRehash(dict *d, int n);
int dictRehashMilliseconds(dict *d, int ms);
void dictSetHashFunctionSeed(unsigned int initval);
unsigned int dictGetHashFunctionSeed(void);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn,
                       void *privdata);

/* Hash table types */
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#endif // DICT_H_
