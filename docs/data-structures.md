# 数据结构层交接文档

> 目标读者：几个月没碰这个工程、想重新接手的自己。
> 基线：Redis 2.8 时代的代码布局（`redis.c` + `t_*.c`），全中文注释。
> 最后更新：2026-11-05

---

## 0. 三句话总览

1. 这个工程的"数据结构层"是**照 Redis 2.8 抄的**，结构定义和 API 基本一一对应，可以直接对照 annotated 源码看。
2. 六个结构里 **sds / adlist / dict / ziplist 是完整可用的**，**intset 完整但没进构建**，**skiplist 有两份互相打架的实现**（一份在 `t_zset.c`，一份在 `skiplist.c`）。
3. 当前 `make` **编不过**，原因是一个多余的调试 `main`（见 §5）。

---

## 1. 文件清单

| 文件 | 行数 | 进构建? | 状态 | 用途 |
|---|---:|---|---|---|
| [sds.c](../src/sds.c) / [sds.h](../src/sds.h) | 1086 / 77 | ✅ | 完整 | 动态字符串，所有 key/value 的底座 |
| [adlist.c](../src/adlist.c) / [adlist.h](../src/adlist.h) | 413 / 119 | ✅ | 完整 | 双端链表 |
| [dict.c](../src/dict.c) / [dict.h](../src/dict.h) | 1136 / 225 | ✅ | 完整（含调试打印） | 哈希表 + 渐进式 rehash |
| [ziplist.c](../src/ziplist.c) / [ziplist.h](../src/ziplist.h) | 1407 / 22 | ✅ | 完整 | 紧凑连续内存列表 |
| [intset.c](../src/intset.c) / [intset.h](../src/intset.h) | 486 / 30 | ❌ | 完整 | 整数集合（set 的编码之一） |
| [skiplist.c](../src/skiplist.c) / [skiplist.h](../src/skiplist.h) | 736 / 102 | 仅 `make skiplist` | 练习版，与 t_zset 重复 | 跳表 |
| [t_zset.c](../src/t_zset.c) | ~520 | ✅ | 只有跳表层，无命令 | zset 的 skiplist 实现 |
| [object.c](../src/object.c) | 365 | ✅ | 完整 | robj 对象 + 编码转换 |
| [t_string.c](../src/t_string.c) | — | ✅ | 部分 | string 命令 |
| [t_list.c](../src/t_list.c) | — | ✅ | 部分 | list 命令 |
| [t_hash.c](../src/t_hash.c) | — | ✅ | 部分 | hash 命令 |
| **`src/t_set.c`** | — | — | **不存在** | **set 命令完全没写** |

---

## 2. 结构之间的关系

```
                    robj (object.c)
                      │ ptr
      ┌───────┬───────┼────────┬─────────┐
      ▼       ▼       ▼        ▼         ▼
    sds    adlist   dict    ziplist   intset    (skiplist)
     │        │       │        │         │
     │        │       │        │         └── set 的小整数编码
     │        │       │        └── list/hash/zset 的小对象编码
     │        │       ├── 整个 keyspace (db->dict)
     │        │       ├── 过期字典 (db->expires)
     │        │       ├── hash 的 HT 编码
     │        │       └── zset 的 member→score
     │        └── list 的 LINKEDLIST 编码、slaves 列表、clients 列表
     └── 所有字符串值、AOF 缓冲、命令回复
```

---

## 3. 逐个结构

### 3.1 sds —— 动态字符串

**定义**（`src/sds.h:12`）

```c
typedef char *sds;              // sds 指向 buf，不是指向头部
struct sdshdr {
  int  len;    // 已用长度
  int  free;   // 剩余空间
  char buf[];  // 柔性数组，实际数据
};
```

关键点：**`sds` 变量指向 `buf`，头部在它前面 `sizeof(struct sdshdr)` 字节处**。所以取长度的代价是 O(1)。

**扩容策略**

- 追加后 `len < 1MB` → `free = len`（翻倍）
- 追加后 `len >= 1MB` → `free = 1MB`（`SDS_MAX_PREALLOC`）
- 因此 `sdscat` 的均摊复杂度是 O(1)

**API 分类**

| 类别 | 函数 |
|---|---|
| 创建/释放 | `sdsnewlen` `sdsnew` `sdsempty` `sdsdup` `sdsfree` |
| 追加 | `sdscatlen` `sdscat` `sdscatsds` `sdscatprintf` `sdscatfmt` |
| 拷贝/裁剪 | `sdscpylen` `sdscpy` `sdstrim` `sdsrange` `sdsclear` |
| 工具 | `sdssplitlen` `sdssplitargs` `sdscatrepr` `sdsmapchars` `sdsjoin` `sdstolower` `sdstoupper` `sdsfromlonglong` |
| 底层 | `sdsMakeRoomFor` `sdsIncrLen` `sdsRemoveFreeSpace` `sdsAllocSize` |

**坑**

- `sds.h:21` 有一个 `static inline size_t sdslen()`，但 `sds.h:35` 又声明了一次 `size_t sdslen(const sds s);`；`sdsavail` 同样。功能上不冲突（`sds.c` 里没有重复定义），但每个包含这个头的编译单元都会报 `defined but not used`。**接手时建议删掉 `sds.h:35` 和 `sds.h:38` 两行外部声明。**
- `sdsIncrLen` 的参数是 `int incr`（Redis 原版也如此），在 `size_t` 语义下做减法要留意符号。

---

### 3.2 adlist —— 双端链表

**定义**（`src/adlist.h`）

```
list     { head, tail, dup, free, match, len }
listNode { prev, next, value }        // value 是 void*，类型无关
listIter { next, direction }          // AL_START_HEAD=0 / AL_START_TAIL=1
```

**特性**：`len` 维护在表头，`listLength` 是 O(1)；`dup/free/match` 三个函数指针让链表可以托管任意类型的值（Redis 用它存 `redisClient*`、`redisSlave*`，以及 linkedlist 编码的 list）。

**API**：`listCreate` `listRelease` `listAddNodeHead` `listAddNodeTail` `listInsertNode` `listDelNode` `listGetIterator` `listNext` `listReleaseIterator` `listDup` `listSearchKey` `listIndex` `listRewind` `listRewindTail` `listRotate` —— 与 Redis 2.8 完全一致，15 个全实现了。

**坑**：`listDelNode` 不负责释放 `value`，只调用 `list->free`（如果设了）。用的时候要么设 `free` 回调，要么调用方自己 `decrRefCount`。

**使用方**：[object.c](../src/object.c)（`createListObject`）、[networking.c](../src/networking.c)（clients）、[redis.c](../src/redis.c)（slaves）、[t_list.c](../src/t_list.c)（LINKEDLIST 编码）、[ziplist.c](../src/ziplist.c)（demo）。

---

### 3.3 dict —— 哈希表 + 渐进式 rehash

**定义**（`src/dict.h`）

```
dictht    { dictEntry **table; size; sizemask; used }   // 一个哈希表
dict      { dictType *type; privdata; dictht ht[2]; rehashidx; iterators }
dictEntry { key; union { val, u64, s64 } v; next }      // 拉链法
dictType  { hashFunction, keyDup, valDup, keyCompare, keyDestructor, valDestructor }
```

**核心机制**

| 机制 | 实现位置 | 说明 |
|---|---|---|
| 初始大小 | `DICT_HT_INITIAL_SIZE` = 4 | 第一次写入才分配 |
| 扩容条件 | `_dictExpandIfNeeded` | `used >= size` 且允许 resize，或负载因子 > `dict_force_resize_ratio`(5) |
| 收缩 | `dictResize` | 负载 < 0.1 时缩到 `used` |
| 渐进式 rehash | `dictRehash(d, n)` | 每次搬 n 个桶；`rehashidx` = -1 表示未进行 |
| 自动推进 | `_dictRehashStep` | `add/find/delete` 时顺带搬 1 个桶，rehash 不会阻塞 |
| 时间片 rehash | `dictRehashMilliseconds(d, ms)` | 后台任务用 |
| 遍历 | `dictScan` | reverse binary iteration，rehash 期间也能完整扫一遍 |

**迭代器的安全语义**

- `dictGetIterator` = 不安全迭代器：迭代期间只能调 `dictNext`，增删会触发 `dictFingerprint` 校验失败 → panic
- `dictGetSafeIterator` = 安全迭代器：把 `d->iterators` 加一，从而**暂停 rehash**，此时可以调用 `dictAdd` / `dictFind`

**内置类型**：`dictTypeHeapStringCopyKey`、`dictTypeHeapStrings`、`dictTypeHeapStringCopyKeyValue`（在 `dict.c` 末尾）

**坑**

- `dict.c:817` 有一句 `printf("开始扩张\n");` 调试打印。**每次扩容都会输出**，高频写入时会刷屏。接手第一件事就该删掉。
- `dict.c:1098` 的 `main` 由 `#ifdef DICT` 正确保护（供 `make dict` 用）。
- 不安全迭代器被误用于修改时，报错发生在**下一次 `dictNext`**，不是当场，排查时容易误判。

---

### 3.4 ziplist —— 紧凑连续内存列表

**内存布局**

```
<zlbytes 4B><zltail 4B><zllen 2B><entry><entry>...<0xFF>
 └────────── ZIPLIST_HEADER_SIZE = 10 字节 ──────────┘
```

每个 entry：

```
<prevlen><encoding+len><data>
   ↑ 前一个 entry 的长度（<254 时 1 字节，否则 1+4 字节），
     用来支持从后往前遍历
```

**编码**

| 宏 | 含义 |
|---|---|
| `ZIP_STR_06B` / `ZIP_STR_14B` / `ZIP_STR_32B` | 字符串长度 6/14/32 位 |
| `ZIP_INT_8B` / `16B` / `24B` / `32B` / `64B` | 整数，按实际大小选最省的一种 |
| `ZIP_INT_IMM_*` | 4 位立即数（0~12，`0xF1`~`0xFD`） |
| `ZIP_END` = 0xFF | 列表结束标记 |

**API**（13 个，[ziplist.h](../src/ziplist.h) 里的全部有实现）

`ziplistNew` `ziplistPush` `ziplistIndex` `ziplistNext` `ziplistPrev` `ziplistGet` `ziplistInsert` `ziplistDelete` `ziplistDeleteRange` `ziplistCompare` `ziplistFind` `ziplistLen` `ziplistBlobLen`

**特性与代价**

- 省内存：没有指针，每个元素只多 1~5 字节
- 查找 O(N)：`ziplistIndex` 从头逐个走
- 插入/删除可能触发 **连锁更新（cascade update）**：某个 entry 的 `prevlen` 从 1 字节变成 5 字节时，后续所有 entry 都要跟着改。最坏 O(N²)，但实际很少发生——这正是 Redis 敢用它做小对象编码的前提
- `zlbytes` / `zltail` / `zllen` 的读写必须走 `intrev32ifbe` / `intrev16ifbe`，否则大端机器上会错（`ziplist.c` 已包含 `endianconv.h`）

**依赖**：[endianconv.c](../src/endianconv.c)（字节序转换）、[util.c](../src/util.c)（`string2ll` 之类）

---

### 3.5 intset —— 整数集合

**定义**（`src/intset.h:8`）

```c
typedef struct intset {
  uint32_t encoding;    // INTSET_ENC_INT16 / INT32 / INT64
  uint32_t length;
  int8_t   contents[];  // 实际按 encoding 解释，不是真的 int8_t 数组
} intset;
```

**规则**

- 元素**有序、无重复**，查找用二分 → O(log N)
- 只能升不能降：插入超出当前范围的值 → `intsetUpgradeAndAdd` 整体升级（O(N)），**删除后不会降级**
- 升级由 `_intsetValueEncoding(v)` 决定需要哪种宽度

**API**：`intsetNew` `intsetAdd` `intsetRemove` `intsetFind` `intsetRandom` `intsetGet` `intsetLen` `intsetBlobLen`

**坑**

- `intset.c:338` 的 `main` **没有 `#ifdef` 保护**，所以 `intset.c` 不能直接加进 [src/Makefile](../src/Makefile) 的构建列表（会和 `redis.c` 的 `main` 冲突）。要单独跑 demo 得先补 `#ifdef INTSET ... #endif`。
- `intset.c` 目前根本没进 `src/Makefile`，也就是说 **set 类型的 INTSET 编码虽然有实现，但没人调用**（因为 `t_set.c` 不存在）。

---

### 3.6 skiplist —— ⚠️ 有两份实现

这是接手时**最容易踩**的地方。

| | 实现 A | 实现 B |
|---|---|---|
| 头文件 | [redis.h:287](../src/redis.h) | [skiplist.h](../src/skiplist.h) |
| 实现 | [t_zset.c](../src/t_zset.c) | [skiplist.c](../src/skiplist.c) |
| `obj` 类型 | `robj *` | `void *` |
| 额外字段 | 无 | `unsigned int h`（本节点高度） |
| `ZSKIPLIST_MAXLEVEL` | 32（`redis.h:212`） | 16（`skiplist.h`，32 被注释掉了） |
| 进构建 | ✅ | 只 `make skiplist` |
| 有命令 | ❌ 只有结构层 | ❌ |

**两份的节点布局恰好相同**（`void*` 和 `robj*` 都是 8 字节），所以指针强转能跑，但**类型上是重复定义**：任何同时 `#include "redis.h"` 和 `"skiplist.h"` 的编译单元都会报 `redefinition of struct zskiplistNode`。目前 `skiplist.h` 只被 `skiplist.c` 包含，这个问题还潜伏着。

**建议**：把 `skiplist.c/h` 降级成纯练习代码（移到 `review/` 或加注释标明"不参与主工程"），主工程只保留 `t_zset.c` 那份，`ZSKIPLIST_MAXLEVEL` 统一成 32。

**跳表结构要点**

```
zskiplistNode { h, obj, score, backward, level[] { forward, span } }
zskiplist     { header, tail, length, level }
```

- 层级用 `ZSKIPLIST_P = 0.25` 的几何分布随机
- `span` 记录跨度，让按排名定位（`ZRANK` 类操作）是 O(log N)
- `backward` 只有一层，支持从尾到头遍历（`ZREVRANGE`）
- 分值可以重复，重复时分值相同再比 `obj`

**未完成的部分**

- `t_zset.c:394` 的 `double zzlGetScore(unsigned char *sptr) {}` **函数体是空的**，返回 `double` 却没有 `return` → 未定义行为。ziplist 编码的 zset 走这条路必错。
- `t_zset.c` 里没有任何 `zaddCommand` / `zrangeCommand` 等命令函数，zset 类型**只有数据结构，没有对外命令**。

---

### 3.7 object —— 对象模型（把上面全部串起来）

**定义**（`src/redis.h`）

```c
typedef struct redisObject {
  unsigned type:4;        // STRING/LIST/SET/ZSET/HASH
  unsigned encoding:4;    // 见下表
  unsigned lru:REDIS_LRU_BITS;
  int refcount;
  void *ptr;              // 指向下面某个结构
} robj;
```

**类型 × 编码**

| type | encoding | ptr 实际指向 | 切换阈值（`src/redis.h:216`） |
|---|---|---|---|
| STRING | `INT` | 整数直接存在 `ptr` 里 | 能解析成 long 且长度短 |
| STRING | `EMBSTR` | robj 和 sdshdr 一次性分配 | `len <= 39` |
| STRING | `RAW` | 独立 `sds` | `len > 39` |
| LIST | `ZIPLIST` | `ziplist` | ≤512 个元素 **且** 每个 ≤**3** 字节 |
| LIST | `LINKEDLIST` | `adlist` | 超过上面任一条 |
| HASH | `ZIPLIST` | `ziplist`（field/value 交替） | ≤512 对 **且** 每个 ≤64 字节 |
| HASH | `HT` | `dict` | 超过任一条 |
| SET | `INTSET` | `intset` | 全是整数 **且** ≤512 个 |
| SET | `HT` | `dict` | 否则（**未实现**） |
| ZSET | `ZIPLIST` | `ziplist` | ≤128 个 **且** 每个 ≤64 字节 |
| ZSET | `SKIPLIST` | `zset{dict + zskiplist}` | 超过任一条 |

> ⚠️ `REDIS_LIST_MAX_ZIPLIST_VALUE` 是 **3**，Redis 上游是 64。这个值小得反常，会让 list 很容易转成 linkedlist。如果不是刻意做实验，应改回 64。

**主要函数**（[object.c](../src/object.c)）

| 函数 | 作用 |
|---|---|
| `createObject(type, ptr)` | 造一个 encoding=RAW 的对象 |
| `createStringObject(ptr, len)` | 按 39 字节切 EMBSTR / RAW |
| `createEmbeddedStringObject` | `zmalloc(sizeof(robj)+sizeof(sdshdr)+len+1)` 一次分配 |
| `tryObjectEncoding(o)` | 尝试转 INT / 把 RAW 转 EMBSTR |
| `dupStringObject` | 按编码复制 |
| `getDecodedObject` | INT 转回字符串 |
| `incrRefCount` / `decrRefCount` | 引用计数，归零时按 encoding 调 `freeXxxObject` |
| `stringObjectLen` | 按编码取长度 |

**坑**：`createStringObject` 用 39 判断 EMBSTR/RAW，而 `sdsnewlen` 对 `len > 39` 还有一次自己的判断；改阈值时别只改一处。

---

## 4. 构建与自测

### 主构建（[src/Makefile](../src/Makefile)）

```bash
cd src
make            # 构建 redis 服务端
make master     # clean + 用 master.ini 启动
make slave      # clean + 用 slave.ini 启动
make clean
```

### 各数据结构的单文件 demo

每个 `.c` 文件末尾都有一个 `main`，用 `-D 宏名` 开关：

```bash
make sds        # -D SDS      zmalloc.c sds.c
make adlist     # -D ADLIST   zmalloc.c sds.c adlist.c
make dict       # -D DICT     zmalloc.c sds.c adlist.c dict.c
make skiplist   # -D SKIPLIST zmalloc.c sds.c adlist.c dict.c skiplist.c
```

`ziplist.c`（`-D ZIPLIST`）和 `intset.c` 没有对应的 make 目标，需要手动编：

```bash
gcc -g -D _GNU_SOURCE -D ZIPLIST ziplist.c zmalloc.c endianconv.c util.c sds.c -o ziplist_demo
```

---

## 5. 已知问题清单（按修复优先级）

| P | 问题 | 位置 | 后果 |
|---|---|---|---|
| **P0** | 调试 `main` 没有 `#ifdef` 保护，与 `redis.c` 的 `main` 冲突 | [t_zset.c:498](../src/t_zset.c) | **`make` 直接失败**（`multiple definition of main`） |
| **P0** | `#ifdef SKIPLIST` 写在 `main` **之后**，等于没保护 | [t_zset.c](../src/t_zset.c) 末尾、[skiplist.c:735](../src/skiplist.c) | 同上（skiplist.c 因没和 redis.c 一起编才侥幸没炸） |
| **P1** | `zzlGetScore` 空函数体，无 `return` | [t_zset.c:394](../src/t_zset.c) | ziplist 编码的 zset 取值未定义 |
| **P1** | 调试 `printf("开始扩张\n")` | [dict.c:817](../src/dict.c) | 高频写入时日志刷屏，掩盖真实输出 |
| **P1** | `intset.c` 的 `main` 没有保护 | [intset.c:338](../src/intset.c) | 无法加入主构建 |
| **P2** | `zskiplistNode` 定义了两份，`ZSKIPLIST_MAXLEVEL` 16 vs 32 | [redis.h:287](../src/redis.h) / [skiplist.h](../src/skiplist.h) | 同时 include 会重定义；两份行为不一致 |
| **P2** | `sdslen`/`sdsavail` 同时有 inline 定义和 extern 声明 | [sds.h:21](../src/sds.h)、`sds.h:35` | 每个 TU 一个 `unused-function` 警告 |
| **P2** | `REDIS_LIST_MAX_ZIPLIST_VALUE` = 3 | [redis.h:219](../src/redis.h) | 与上游(64)不符，list 过早转 linkedlist |
| **P3** | 没有 `t_set.c` | — | SET 类型命令完全不可用，`intset` 无人调用 |
| **P3** | zset 没有任何 `z*Command` | [t_zset.c](../src/t_zset.c) | ZSET 类型不可用 |

---

## 6. 重新上手的建议顺序

1. **先修 P0**（挪掉两个调试 `main`、给 `intset.c` 补 `#ifdef`），让 `make` 能过。这一步不做，后面都是空谈。
2. **删掉 `dict.c:817` 的调试打印**，否则你永远看不清服务端真实输出。
3. **从 zset 打通一条链**：`ziplist` 编码 → `zslInsert` → `zaddCommand`。这是唯一"两种编码都要走"的类型，打通它等于复习了 ziplist + skiplist + dict + object 四层。
4. 再补 `t_set.c`（`intset` → `HT` 转换），它是六个数据结构里唯一完全没人用的那个。
5. 最后再回到 replication / aof 这些上层。

---

## 7. 参考资料

- 代码基线：Redis 2.8（`redis.c` + `t_*.c` 布局，`sds` 用 `int len/free` 而非 5.0 的 `sdshdr8/16/32/64`）
- `dict.c` 的 `dictScan` 是 Redis 2.8 引入的 reverse binary iteration，值得单独读
- `ziplist` 的连锁更新在后续版本被 `listpack` 取代，读完 ziplist 可以对照看 listpack 为什么能省掉 `prevlen`
