#include "skiplist.h"
#include <stdlib.h>
#include <stdio.h>
#include "zmalloc.h"
#include <math.h>

/*

表头（head）：负责维护跳跃表的节点指针。
跳跃表节点：保存着元素值，以及多个层。
层：保存着指向其他元素的指针。高层的指针越过的元素数量大于等于低层的指针，为了提高查找的效率，程序总是从高层先开始访问，然后随着元素值范围的缩小，慢慢降低层次。
表尾：全部由 NULL 组成，表示跳跃表的末尾。


** 我们的SkipList有以下特点
** 1 这个实现允许有重复的分值
** 2 对元素的比对不仅仅要比对其分值 还要比较其对象
** 3 每个节点有后退指针 允许从尾到头的遍历
*/

#define DEBUG

typedef struct redisObject
{
  // 类型
  unsigned type : 4;
  // 编码
  unsigned encoding : 4;
  // 引用计数
  int refcount;
  // 指向实际值的指针
  void *ptr;
} robj;

void incrRefCount(robj *o)
{
  o->refcount++;
}

void decrRefCount(robj *o)
{
  o->refcount--;
}

int compareStringObjects(robj *a, robj *b)
{
  return strcmp((const char *)a, (const char *)b);
}

int equalStringObjects(robj *a, robj *b)
{
  return compareStringObjects(a, b);
}

static int zslLexValueGteMin(void *value, zlexrangespec *spec); // >= MIN
static int zslLexValueLteMax(void *value, zlexrangespec *spec); // <= MAX

// 创建一个空跳表节点
zskiplistNode *zslCreateNode(int level, double score, void *obj)
{
  zskiplistNode *zn = zmalloc(sizeof(*zn) + level * sizeof(struct zskiplistLevel));
  // 设置属性
  zn->score = score;
  zn->obj = obj;
#ifdef DEBUG
  zn->h = level;
#endif

  return zn;
}

// 创建并返回一个新的跳表 只有一个节点
zskiplist *zslCreate(void)
{
  int j;
  zskiplist *zsl;

  // 分配空间
  zsl = zmalloc(sizeof(*zsl));

  // 设置高度和起始层数
  zsl->level = 1;
  zsl->length = 0;

  // 初始化表头节点 有32个节点
  zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL, 0, NULL);
  for (int j = 0; j < ZSKIPLIST_MAXLEVEL; j++)
  {
    zsl->header->level[j].forward = NULL; // 初始化
    zsl->header->level[j].span = 0;
  }

  // 设置表尾
  zsl->tail = NULL;

  return zsl;
}

void zslFreeNode(zskiplistNode *node)
{
  decrRefCount(node->obj);

  zfree(node);
}

// 释放
void zslFree(zskiplist *zsl)
{
  // 头节点 迭代节点
  zskiplistNode *node = zsl->header->level[0].forward, *next;

  zfree(zsl->header);

  // 释放表中的所有节点
  while (node)
  {
    next = node->level[0].forward;
    zslFreeNode(node);
    node = next;
  }

  zfree(zsl);
}

// 返回一个随机层高
int zslRandomLevel(void)
{
  int level = 1;

  while ((random() & 0xFFFF) < (ZSKIPLIST_P * 0xFFFF))
  {
    level += 1;
  }

  return (level < ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}

// 创建一个成员为 obj 分值为 score 的新节点
// 并将这个新节点插入到 zsl 中
/*

{h|obj*|score|backward*||level[(forward*|span, (forward*|span), (forward*|span), (forward*|span)]}  这是头指针
                                   |               |                |                |
{h|obj*|  3  |backward*||level[(forward*|span, (forward*|span), (forward*|span), (forward*|span)]}
                                   |               |                |                |
{h|obj*|  4  |backward*||level[(forward*|span, (forward*|span), (forward*|span)]}
                                   |               |                |                |
{h|obj*|  6  |backward*||level[(forward*|span]}
                                   |               |                |                |
{h|obj*|  9  |backward*||level[(forward*|span, (forward*|span)]}
                                   |               |                |                |
{h|obj*|  10  |backward*||level[(forward*|span, (forward*|span), (forward*|span)]}
                                   |               |                |                |
                                 NULL             NULL             NULL             NULL            这是表尾

*/
zskiplistNode *zslInsert(zskiplist *zsl, double score, void *obj)
{
  zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x; // update 沿途路过的节点
  int i, level;
  unsigned int rank[ZSKIPLIST_MAXLEVEL]; // 沿途的跨度数组

  assert(!isnan(score));

  // 在各个层查找节点的插入位置
  x = zsl->header; // 从头节点的最后一个索引开始
  for (i = zsl->level - 1; i >= 0; i--)
  {
    rank[i] = i == (zsl->level - 1) ? 0 : rank[i + 1];
    // 有序插入
    while (x->level[i].forward &&                 // 倒序找
           (x->level[i].forward->score < score || // 对比分值
            (x->level[i].forward->score == score &&
             compareStringObjects(x->level[i].forward->obj, obj) < 0)))
    { // 对比成员
      // 记录沿途跨越了多少个节点
      rank[i] += x->level[i].span;
      // 移动到下一个指针
      x = x->level[i].forward;
    }
     update[i] = x; // 记录将要和新节点相连接的节点
  }

  // 调用者应当保证同分值且同成员的元素不会出现
  level = zslRandomLevel();

  if (level > zsl->level)
  { // 更新最大层数
    // 初始化未使用的层
    for (i = zsl->level; i < level; i++)
    {
      rank[i] = 0;
      update[i] = zsl->header;
      update[i]->level[i].span = zsl->length;
    }
    zsl->level = level;
  }

  x = zslCreateNode(level, score, obj);

  // 将前面记录的指针指向新节点 并做相应的设置
  for (int i = 0; i < level; i++)
  {                                                                    // 更新新节点的索引
    x->level[i].forward = update[i]->level[i].forward;                 // 新节点继承沿途记录的forward 比如初始化的时候就将x第一个节点的forward指向NULL
    update[i]->level[i].forward = x;                                   // 将沿途记录的各个节点的forward 指针指向新节点 比如初始化的时候就将头节点的forward指向x
    x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]); // 计算新节点跨越的节点数量

    update[i]->level[i].span = (rank[0] - rank[i]) + 1; // 更新沿途节点的 span 值 +1表示新节点
  }
  // 从未接触的节点 span 也需要增加 这些节点直接从表头指向新节点
  for (i = level; i < zsl->level; i++)
  {
    update[i]->level[i].span++;
  }

  // 设新节点的后退指针 第一个元素的的后退指针如果是头节点 就不指了NULL
  x->backward = (update[0] == zsl->header) ? NULL : update[0];
  if (x->level[0].forward)
  { // 有下一层
    x->level[0].forward->backward = x;
  }
  else
  { // 标记末尾节点
    zsl->tail = x;
  }

  // 跳表节点计数增加
  zsl->length++;
  return x;
}

unsigned char *zzlInsert(unsigned char *zl, void *ele, double score)
{
}

// 在 zsl 中删除 x
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update)
{
  // 更新所有和被删除节点 x 有关的节点的指针
  for (int i = 0; i < zsl->level; i++)
  {
    if (update[i]->level[i].forward == x)
    {
      /*
      ** | | | |         | | | |
      **
      ** | | |           | | |
      **
      ** | | | |   ----> | | | | |
      **
      ** | | | | |
      **
      */
      update[i]->level[i].span += x->level[i].span - 1;
      update[i]->level[i].forward = x->level[i].forward;
    }
    else
    {
      update[i]->level[i].span -= 1;
    }
  }

  // 更新所有和被删除节点x有关的节点的指针
  if (x->level[0].forward)
  {
    x->level[0].forward->backward = x->backward;
  }
  else
  {
    zsl->tail = x->backward;
  }

  // 更新跳表的层高
  while (zsl->level > 1 && zsl->header->level[zsl->level - 1].forward == NULL)
  {
    zsl->level--;
  }

  zsl->length--;
}

// 从 zsl 中找到 score obj 的节点 删掉
int zslDelete(zskiplist *zsl, double score, void *obj)
{
  zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
  int i;

  // 遍历跳表查找目标节点 并记录沿途所有节点
  x = zsl->header;
  for (i = zsl->level - 1; i >= 0; i--)
  {
    while (x->level[i].forward &&
           (x->level[i].forward->score < score || // 对比分值
            (x->level[i].forward->score == score &&
             compareStringObjects(x->level[i].forward->obj, obj) < 0)))
    { // 对比成员
      x = x->level[i].forward;
      // printf("%d ", (int)x->score);
    }
    // printf( "\n");
    update[i] = x; // 记录沿途
  }

  x = x->level[0].forward;
  if (x && score == x->score && equalStringObjects(x->obj, obj))
  {
    zslDeleteNode(zsl, x, update);
    zslFreeNode(x);
    return 1;
  }
  else
  {
    return 0;
  }
}

// 大于等于 min
static int zslValueGteMin(double value, zrangespec *spec)
{
  return spec->minex ? (value > spec->min) : (value >= spec->min);
}

// 小于等于 max
static int zslValueLteMax(double value, zrangespec *spec)
{
  return spec->maxex ? (value < spec->max) : (value <= spec->max);
}

int zslIsInRange(zskiplist *zsl, zrangespec *range)
{
  zskiplistNode *x;
  if (range->min > range->max ||
      (range->min == range->max && (range->minex || range->maxex)))
    return 0;

  // 检查最大分值
  x = zsl->tail;
  if (x == NULL || !zslValueGteMin(x->score, range))
    return 0;

  // 检查最小分值
  x = zsl->header->level[0].forward;
  if (x == NULL || !zslValueLteMax(x->score, range))
    return 0;

  return 1;
}

zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range)
{
  zskiplistNode *x;
  if (!zslIsInRange(zsl, range))
    return NULL;

  // 遍历跳表
  x = zsl->header;
  for (int i = zsl->level - 1; i >= 0; i--)
  {
    while (x->level[i].forward &&
           !zslValueGteMin(x->level[i].forward->score, range))
    {
      x = x->level[i].forward;
    }
  }

  // 需要往后推一位
  x = x->level[0].forward;
  assert(x != NULL);

  if (!zslValueLteMax(x->score, range))
    return NULL;
  return x;
}

zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range)
{
  zskiplistNode *x;
  int i;
  if (!zslIsInRange(zsl, range))
    return NULL;

  // 遍历跳表
  x = zsl->header;
  for (int i = zsl->level - 1; i >= 0; i--)
  {
    while (x->level[i].forward &&
           zslValueLteMax(x->level[i].forward->score, range))
    {
      x = x->level[i].forward;
    }
  }

  assert(x != NULL);

  if (!zslValueGteMin(x->score, range))
    return NULL;
  return x;
}

unsigned long zslDeleteRangeByScore(zskiplist *zsl, zrangespec *range,
                                    dict *dict)
{
  zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
  unsigned long removed = 0;
  int i;

  // 记录所有和被删除节点（们）有关的节点
  // T_wrost = O(N) , T_avg = O(log N)
  x = zsl->header;
  for (i = zsl->level - 1; i >= 0; i--)
  {
    while (x->level[i].forward &&
           (range->minex ? x->level[i].forward->score <= range->min
                         : x->level[i].forward->score < range->min))
      x = x->level[i].forward;
    update[i] = x;
  }

  x = x->level[0].forward;
  while (x && (range->maxex ? x->score < range->max : x->score <= range->max))
  {
    zskiplistNode *next = x->level[0].forward;
    zslDeleteNode(zsl, x, update);
    dictDelete(dict, x->obj);
    zslFreeNode(x);
    removed++;
    x = next;
  }
  return removed;
}

unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start,
                                   unsigned int end, dict *dict)
{
  zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
  unsigned long traversed = 0, removed = 0;
  int i;

  // 沿着前进指针移动到指定排位的起始位置，并记录所有沿途指针
  // T_wrost = O(N) , T_avg = O(log N)
  x = zsl->header;
  for (i = zsl->level - 1; i >= 0; i--)
  {
    while (x->level[i].forward && (traversed + x->level[i].span) < start)
    {
      traversed += x->level[i].span;
      x = x->level[i].forward;
    }
    update[i] = x;
  }

  // 移动到排位的起始的第一个节点
  traversed++;
  x = x->level[0].forward;
  // 删除所有在给定排位范围内的节点
  // T = O(N)
  while (x && traversed <= end)
  {

    // 记录下一节点的指针
    zskiplistNode *next = x->level[0].forward;

    // 从跳跃表中删除节点
    zslDeleteNode(zsl, x, update);
    // 从字典中删除节点
    dictDelete(dict, x->obj);
    // 释放节点结构
    zslFreeNode(x);

    // 为删除计数器增一
    removed++;

    // 为排位计数器增一
    traversed++;

    // 处理下个节点
    x = next;
  }

  // 返回被删除节点的数量
  return removed;
}

double zzlGetScore(unsigned char *sptr) {}

void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr)
{
}

void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr)
{
}

unsigned int zsetLength(void *zobj)
{
}

void zsetConvert(void *zobj, int encoding)
{
}

// 查查找包含给定分值和成员对象的节点在跳表中的排位
unsigned long zslGetRank(zskiplist *zsl, double score, void *o)
{
  zskiplistNode *x;
  unsigned long rank = 0;

  // 遍历整个跳表
  x = zsl->header;
  for (int i = zsl->level - 1; i >= 0; i--)
  {
    while (
        x->level[i].forward &&
        (x->level[i].forward->score < score || // 对比分值
         (x->level[i].forward->score == score &&
          compareStringObjects(x->level[i].forward->obj, o) <= 0))) // 对比成员
    {
      rank += x->level[i].span;
      x = x->level[i].forward;
    }

    if (x->obj && equalStringObjects(x->obj, o))
    {
      return rank;
    }
  }

  // 没找到
  return 0;
}

// 根据排位在跳表中查找元素 排位的起始值为1
zskiplistNode *zslGetElementByRank(zskiplist *zsl, unsigned long rank)
{
  zskiplistNode *x;
  unsigned long traversed = 0;
  int i;

  // T_wrost = O(N), T_avg = O(log N)
  x = zsl->header;
  for (i = zsl->level - 1; i >= 0; i--)
  {

    // 遍历跳跃表并累积越过的节点数量
    while (x->level[i].forward && (traversed + x->level[i].span) <= rank)
    {
      traversed += x->level[i].span;
      x = x->level[i].forward;
    }

    // 如果越过的节点数量已经等于 rank
    // 那么说明已经到达要找的节点
    if (traversed == rank)
    {
      return x;
    }
  }

  // 没找到目标节点
  return NULL;
}

void printSkl(zskiplist *zsl)
{
  printf("[lvl:%d|header|len:%3ld|tail]\n\n", zsl->level, zsl->length);

  zskiplistNode *x = zsl->header;

  for (int i = 0; i <= zsl->length; i++)
  {
    printf("[本节点索引层数:%2d|score:%3d|obj:|", x->h, (int)x->score);

    for (int j = 0; j < x->h; j++)
    {
      printf(" %2d |", x->level[j].span);
    }
    printf("]\n");
    x = x->level[0].forward;
  }

  printf("[================================");

  for (int i = 0; i < ZSKIPLIST_MAXLEVEL; i++)
  {
    printf("|NULL");
  }
  printf("|]\n\n");
}

/*

[lvl:1|header|len:  1|tail]

[本节点索引层数:16|score:  0|obj:|  1 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |]
[本节点索引层数: 1|score:  3|obj:|  0 |]
[================================|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|]

[lvl:1|header|len:  2|tail]

[本节点索引层数:16|score:  0|obj:|  1 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |]
[本节点索引层数: 1|score:  2|obj:|  1 |]
[本节点索引层数: 1|score:  3|obj:|  0 |]
[================================|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|]

[lvl:1|header|len:  3|tail]

[本节点索引层数:16|score:  0|obj:|  1 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |]
[本节点索引层数: 1|score:  2|obj:|  1 |]
[本节点索引层数: 1|score:  3|obj:|  1 |]
[本节点索引层数: 1|score:  7|obj:|  0 |]
[================================|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|]

[lvl:1|header|len:  4|tail]

[本节点索引层数:16|score:  0|obj:|  1 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |]
[本节点索引层数: 1|score:  2|obj:|  1 |]
[本节点索引层数: 1|score:  3|obj:|  1 |]
[本节点索引层数: 1|score:  7|obj:|  1 |]
[本节点索引层数: 1|score: 10|obj:|  0 |]
[================================|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|]

[lvl:4|header|len:  5|tail]

[本节点索引层数:16|score:  0|obj:|  1 |  3 |  3 |  3 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |]
[本节点索引层数: 1|score:  2|obj:|  1 |]
[本节点索引层数: 1|score:  3|obj:|  1 |]
[本节点索引层数: 4|score:  5|obj:|  1 |  2 |  2 |  2 |]
[本节点索引层数: 1|score:  7|obj:|  1 |]
[本节点索引层数: 1|score: 10|obj:|  0 |]
[================================|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|]

[lvl:4|header|len:  6|tail]

[本节点索引层数:16|score:  0|obj:|  1 |  4 |  4 |  4 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |]
[本节点索引层数: 1|score:  2|obj:|  1 |]
[本节点索引层数: 1|score:  3|obj:|  1 |]
[本节点索引层数: 1|score:  4|obj:|  1 |]
[本节点索引层数: 4|score:  5|obj:|  1 |  2 |  2 |  2 |]
[本节点索引层数: 1|score:  7|obj:|  1 |]
[本节点索引层数: 1|score: 10|obj:|  0 |]
[================================|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|]

[lvl:4|header|len:  7|tail]

[本节点索引层数:16|score:  0|obj:|  1 |  4 |  4 |  4 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |]
[本节点索引层数: 1|score:  2|obj:|  1 |]
[本节点索引层数: 1|score:  3|obj:|  1 |]
[本节点索引层数: 1|score:  4|obj:|  1 |]
[本节点索引层数: 4|score:  5|obj:|  1 |  1 |  1 |  3 |]
[本节点索引层数: 3|score:  6|obj:|  1 |  2 |  2 |]
[本节点索引层数: 1|score:  7|obj:|  1 |]
[本节点索引层数: 1|score: 10|obj:|  0 |]
[================================|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|]

[lvl:4|header|len:  8|tail]

[本节点索引层数:16|score:  0|obj:|  1 |  4 |  4 |  4 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |]
[本节点索引层数: 1|score:  2|obj:|  1 |]
[本节点索引层数: 1|score:  3|obj:|  1 |]
[本节点索引层数: 1|score:  4|obj:|  1 |]
[本节点索引层数: 4|score:  5|obj:|  1 |  1 |  1 |  4 |]
[本节点索引层数: 3|score:  6|obj:|  1 |  3 |  3 |]
[本节点索引层数: 1|score:  7|obj:|  1 |]
[本节点索引层数: 1|score:  8|obj:|  1 |]
[本节点索引层数: 1|score: 10|obj:|  0 |]
[================================|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|]

[lvl:4|header|len:  9|tail]

[本节点索引层数:16|score:  0|obj:|  1 |  4 |  4 |  4 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |]
[本节点索引层数: 1|score:  2|obj:|  1 |]
[本节点索引层数: 1|score:  3|obj:|  1 |]
[本节点索引层数: 1|score:  4|obj:|  1 |]
[本节点索引层数: 4|score:  5|obj:|  1 |  1 |  1 |  5 |]
[本节点索引层数: 3|score:  6|obj:|  1 |  3 |  4 |]
[本节点索引层数: 1|score:  7|obj:|  1 |]
[本节点索引层数: 1|score:  8|obj:|  1 |]
[本节点索引层数: 2|score:  9|obj:|  1 |  1 |]
[本节点索引层数: 1|score: 10|obj:|  0 |]
[================================|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|]

[lvl:4|header|len: 10|tail]

[本节点索引层数:16|score:  0|obj:|  1 |  1 |  1 |  5 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |  0 |]
[本节点索引层数: 3|score:  1|obj:|  1 |  4 |  4 |]
[本节点索引层数: 1|score:  2|obj:|  1 |]
[本节点索引层数: 1|score:  3|obj:|  1 |]
[本节点索引层数: 1|score:  4|obj:|  1 |]
[本节点索引层数: 4|score:  5|obj:|  1 |  1 |  1 |  5 |]
[本节点索引层数: 3|score:  6|obj:|  1 |  3 |  4 |]
[本节点索引层数: 1|score:  7|obj:|  1 |]
[本节点索引层数: 1|score:  8|obj:|  1 |]
[本节点索引层数: 2|score:  9|obj:|  1 |  1 |]
[本节点索引层数: 1|score: 10|obj:|  0 |]
[================================|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|]


*/

#include <time.h>
int main(int argc, char **argv)
{
  srandom((unsigned long)time(NULL));
  zskiplist *zsl = zslCreate();
  int value = 666;
  robj *obj = zmalloc(sizeof(*obj));
  obj->ptr = &value;

  zslInsert(zsl, 3, obj);
  printSkl(zsl);
  zslInsert(zsl, 2, obj);
  printSkl(zsl);
  zslInsert(zsl, 7, obj);
  printSkl(zsl);
  zslInsert(zsl, 10, obj);
  printSkl(zsl);
  zslInsert(zsl, 5, obj);
  printSkl(zsl);
  zslInsert(zsl, 4, obj);
  printSkl(zsl);
  zslInsert(zsl, 6, obj);
  printSkl(zsl);
  zslInsert(zsl, 8, obj);
  printSkl(zsl);
  zslInsert(zsl, 9, obj);
  printSkl(zsl);
  zslInsert(zsl, 1, obj);
  printSkl(zsl);

  // printf("%ld\n", zslGetRank(zsl, 12, obj));
  // zskiplistNode *x;
  // x = zslGetElementByRank(zsl, 18);
  // printf("%d\n", (int)x->score);
}
#ifdef SKIPLIST
#endif
