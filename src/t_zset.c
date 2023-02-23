#include "redis.h"
#include <stdlib.h>
#include <stdio.h>
#include "zmalloc.h"
#include <math.h>

/*
** Sorted set API
*/

/*
** ZSET 同时使用两种数据结构来持有一个同一个元素
** 从而提供 O(logN) 复杂度的有序数据结构的插入和移除操作
** Hash 将Redis 对象映射到分值上
** SkipList将分值映射到 Redis 对象上
** 以 SkipList 的角度而言 可以说 Redis 对象是根据分值来排序的
*/

/*
** 我们的SkipList有以下特点
** 1 这个实现允许有重复的分值
** 2 对元素的比对不仅仅要比对其分值 还要比较其对象
** 3 每个节点有后退指针 允许从尾到头的遍历
*/

#define DEBUG

static int zslLexValueGteMin(robj *value, zlexrangespec *spec); // >= MIN
static int zslLexValueLteMax(robj *value, zlexrangespec *spec); // <= MAX

// 创建一个空跳表节点
zskiplistNode *zslCreateNode(int level, double score, robj *obj) {
  zskiplistNode *zn = zmalloc(sizeof(*zn)+level*sizeof(struct zskiplistLevel));
  // 设置属性
  zn->score = score;
  zn->obj = obj;
#ifdef DEBUG
  zn->h = level;
#endif

  return zn;
}

// 创建并返回一个新的跳表
zskiplist *zslCreate(void) {
  int j;
  zskiplist *zsl;

  // 分配空间
  zsl = zmalloc(sizeof(*zsl));

  // 设置高度和起始层数
  zsl->level = 1;
  zsl->length = 0;

  // 初始化表头节点
  zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL, 0, NULL);
  for (int j = 0;j < ZSKIPLIST_MAXLEVEL;j++) {
    zsl->header->level[j].forward = NULL;
    zsl->header->level[j].span = 0;
  }

  // 设置表尾
  zsl->tail = NULL;

  return zsl;
}

void zslFreeNode(zskiplistNode *node) {
  decrRefCount(node->obj);

  zfree(node);
}

// 释放
void zslFree(zskiplist *zsl) {
  // 头节点 迭代节点
  zskiplistNode *node = zsl->header->level[0].forward, *next;

  zfree(zsl->header);

  // 释放表中的所有节点
  while(node) {
    next = node->level[0].forward;
    zslFreeNode(node);
    node = next;
  }

  zfree(zsl);
}

// 返回一个随机层高
int zslRandomLevel(void) {
  int level = 1;

  while ((random() & 0xFFFF) < (ZSKIPLIST_P * 0xFFFF)) {
    level += 1;
  }

  return (level < ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}

// 创建一个成员为 obj 分值为 score 的新节点
// 并将这个新节点插入到 zsl 中
zskiplistNode *zslInsert(zskiplist *zsl, double score, robj *obj) {
  zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
  unsigned int rank[ZSKIPLIST_MAXLEVEL];
  int i, level;

  redisAssert(!isnan(score));

  // 在各个层查找节点的插入位置
  x = zsl->header;
  for (i = zsl->level-1; i >= 0; i--) {
    rank[i] = i == (zsl->level-1) ? 0 : rank[i+1];

    // 有序插入
    while (x->level[i].forward &&
           (x->level[i].forward->score < score || // 对比分值
            (x->level[i].forward->score == score &&
             compareStringObjects(x->level[i].forward->obj, obj) < 0))) { // 对比成员
      // 记录沿途跨越了多少个节点
      rank[i] += x->level[i].span;
      // 移动到下一个指针
      x =x->level[i].forward;
      //printf("Crossing...\n");
    }
      //printf("Crossing... i:%d\n", i);
    update[i] = x; // 记录将要和新节点相连接的节点
  }

  // 调用者应当保证同分值且同成员的元素不会出现
  level = zslRandomLevel();

  if (level > zsl->level) { // 更新最大层数
    // 初始化未使用的层
    for (i = zsl->level; i < level; i++) {
      rank[i] = 0;
      update[i] = zsl->header;
      update[i]->level[i].span = zsl->length;
    }
    zsl->level = level;
  }

  x = zslCreateNode(level, score, obj);

  // 将前面记录的指针指向新节点 并做相应的设置
  for (int i = 0;i < level; i++) {
    x->level[i].forward = update[i]->level[i].forward; // 新节点继承沿途记录的forward
    update[i]->level[i].forward = x; // 将沿途记录的各个节点的forward 指针指向新节点
    x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]); // 计算新节点跨越的节点数量

    update[i]->level[i].span = (rank[0] - rank[i]) + 1; // 更新沿途节点的 span 值 +1表示新节点
  }
  // 从未接触的节点 span 也需要增加 这些节点直接从表头指向新节点
  for (i = level; i < zsl->level; i++) {
    update[i]->level[i].span++;
  }

  // 设新节点的后退指针
  x->backward = (update[0] == zsl->header) ? NULL : update[0];
  if (x->level[0].forward) { // 有下一层
    x->level[0].forward->backward = x;
  } else { // 标记末尾节点
    zsl->tail = x;
  }

  // 跳表节点计数增加
  zsl->length++;
  return x;
}

unsigned char *zzlInsert(unsigned char *zl, robj *ele, double score) {

}

// 在 zsl 中删除 x
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update) {
  // 更新所有和被删除节点 x 有关的节点的指针
  for (int i = 0; i < zsl->level; i++) {
    if (update[i]->level[i].forward == x) {
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
    } else {
      update[i]->level[i].span -= 1;
    }
  }

  // 更新所有和被删除节点x有关的节点的指针
  if (x->level[0].forward) {
    x->level[0].forward->backward = x->backward;
  } else {
    zsl->tail = x->backward;
  }

  // 更新跳表的层高
  while(zsl->level > 1 && zsl->header->level[zsl->level-1].forward == NULL) {
    zsl->level--;
  }

  zsl->length--;
}

// 从 zsl 中找到 score obj 的节点 删掉
int zslDelete(zskiplist *zsl, double score, robj *obj) {
  zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
  int i;

  // 遍历跳表查找目标节点 并记录沿途所有节点
  x = zsl->header;
  for (i = zsl->level - 1; i >= 0; i--) {
    while (x->level[i].forward &&
           (x->level[i].forward->score < score || // 对比分值
            (x->level[i].forward->score == score &&
             compareStringObjects(x->level[i].forward->obj, obj) < 0))) { // 对比成员
      x = x->level[i].forward;
      //printf("%d ", (int)x->score);
    }
    //printf( "\n");
    update[i] = x; // 记录沿途
  }

  x = x->level[0].forward;
  if (x && score == x->score && equalStringObjects(x->obj,obj)) {
    zslDeleteNode(zsl, x, update);
    zslFreeNode(x);
    return 1;
  } else {
    return 0;
  }
}

zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range) {

}

zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range) {

}

double zzlGetScore(unsigned char *sptr) {

}

void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {

}

void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {

}

unsigned int zsetLength(robj *zobj) {

}

void zsetConvert(robj *zobj, int encoding) {

}

// 查查找包含给定分值和成员对象的节点在跳表中的排位
unsigned long zslGetRank(zskiplist *zsl, double score, robj *o) {
  zskiplistNode *x;
  unsigned lonmg rank = 0;
  int i;

  // 遍历整个跳表
  x = zsl->header;
  for (i = zsl->level-1; i >= 0; i--) {
while (x->level[i].forward &&
           (x->level[i].forward->score < score || // 对比分值
            (x->level[i].forward->score == score &&
             compareStringObjects(x->level[i].forward->obj, obj) < 0)))  // 对比成员

  {

  }
  }

  // 没找到
  return 0;
}

#if 1

void printSkl(zskiplist *zsl) {
  printf("[lvl:%d|header|len:%3ld|", zsl->level, zsl->length);
  for (int i = 0; i < zsl->level;i++) {
    printf("    |");
  }
  printf("tail]\n\n");

  zskiplistNode *x = zsl->header;
  for (int i = 0; i < zsl->length; i++) {
    int lv = x->h - 1;
    while (x->level[lv].forward == NULL && lv > -1) {
      lv--;
    }
    if (lv < 0) {
      break;
    }

    x = x->level[0].forward;

    printf("[lvl:%d|score:%3d|obj:|", x->h, (int)x->score);
    for (int i = 0;i < x->h;i++) {
      printf("    |");
    }
    printf("]\n\n");
  }

  printf("[====================|NULL|NULL|NULL|NULL]\n");
}

int main(int argc, char **argv) {
  zskiplist *zsl = zslCreate();
  int value = 666;
  robj *obj = zmalloc(sizeof(*obj));
  obj->encoding = REDIS_ENCODING_INT;
  obj->ptr = &value;

  for (int i = 10;i > 0;i--) {
    zslInsert(zsl, i, obj);
  }
  for (int i = 11;i <21;i++) {
    zslInsert(zsl, i, obj);
  }

  printSkl(zsl);
  zslDelete(zsl, 9, obj);
  printSkl(zsl);
}

#endif
