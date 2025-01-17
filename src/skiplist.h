#ifndef SKIPLIST_H_
#define SKIPLIST_H_

#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <string.h>

#include "adlist.h"
#include "anet.h"
#include "dict.h"
#include "sds.h"

//#define ZSKIPLIST_MAXLEVEL 32
#define ZSKIPLIST_MAXLEVEL 16
#define ZSKIPLIST_P 0.25

/*跳表节点


首先 跳表是一个有序链表 其次为了提高这个链表的查找速度 给他加上了索引 这些索引称为 "level"

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
typedef struct zskiplistNode {
  unsigned int h; // 本节点高度
  // 成员对象
  void *obj;
  // 分值
  double score;
  // 后退指针
  struct zskiplistNode *backward;
  // 层
  struct zskiplistLevel {
    // 前进指针
    struct zskiplistNode *forward;
    // 跨度 用于记录两个节点之间的距离
    unsigned int span;
  } level[];  // 索引
} zskiplistNode;


// 跳表
typedef struct zskiplist {
  // 表头节点和表尾节点
  struct zskiplistNode *header, *tail;
  // 表中节点的数量
  unsigned long length;
  // 表中层数最大的节点的层数
  int level;

} zskiplist;

// 开区间 闭区间
typedef struct
{
  double min,max; // 最大值 最小值

  int minex, maxex; // 是否是闭区间 1是闭区间 0是开区间
} zrangespec;

typedef struct {
  void *min,*max; // 最大值 最小值

  int minex, maxex; // 是否是闭区间 1是闭区间 0是开区间
} zlexrangespec;


// 跳表API
zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
zskiplistNode *zslInsert(zskiplist *zsl, double score, void *obj);
unsigned char *zzlInsert(unsigned char *zl, void *ele, double score);
int zslDelete(zskiplist *zsl, double score, void *obj);
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range);
zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range);
double zzlGetScore(unsigned char *sptr);
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
unsigned int zsetLength(void *zobj);
void zsetConvert(void *zobj, int encoding);
unsigned long zslGetRank(zskiplist *zsl, double score, void *o);

#endif // SKIPLIST_H_
