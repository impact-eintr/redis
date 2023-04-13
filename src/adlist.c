#include "adlist.h"
#include <stdlib.h>
#include "zmalloc.h"

// 创建一个新链表
list *listCreate(void) {
  struct list *list;
  // 分配内存
  if ((list = zmalloc(sizeof(*list))) == NULL) {
    return NULL;
  }

  // 初始化属性
  list->head = list->tail = NULL;
  list->len = 0;
  list->dup = NULL;
  list->free = NULL;
  list->match = NULL;

  return list;
}

// 释放链表占用的内存
void listRelease(list *list) {
  unsigned long len;
  listNode *current, *next;

  // 指向头指针
  current = list->head;
  // 遍历整个链表
  len = list->len;
  while(len--) {
    next = current->next;

    if (list->free) // 如果自定义了释放函数 表明链表的值是一个对象
      list->free(current->value);

    zfree(current);

    current = next;
  }
  // 释放链表自身
  zfree(list);
}

// 头插法
list *listAddNodeHead(list *list, void *value) {
  listNode *node;
  if ((node = zmalloc(sizeof(*node))) == NULL) {
    return NULL;
  }
  // 保存值指针
  node->value = value;

  // 添加节点到空链表
  if (list->len == 0) {
    list->head = list->tail = node;
    node->prev = node->next = NULL;
  } else {
    node->prev = NULL;
    node->next = list->head;
    list->head->prev = node;
    list->head = node;
  }
  // 更新链表节点数
  list->len++;

  return list;
}

// 尾插法
list *listAddNodeTail(list *list, void *value) {
  listNode *node;
  if ((node = zmalloc(sizeof(*node))) == NULL) {
    return NULL;
  }
  // 保存值指针
  node->value = value;

  // 添加节点到空链表
  if (list->len == 0) {
    list->head = list->tail = node;
    node->prev = node->next = NULL;
  } else {
    node->prev = list->tail;
    node->next = NULL;
    list->tail->next = node;
    list->tail = node;
  }
  // 更新链表节点数
  list->len++;

  return list;
}

// 指定位置插入
list *listInsertNode(list *list, listNode *old_node, void *value, int after) {
  listNode *node;

  // create a new node
  if ((node = zmalloc(sizeof(*node))) == NULL) {
    return NULL;
  }
  // 保存值
  node->value = value;

  // 将新节点添加到给定节点之后
  if (after) {
    node->prev = old_node;
    node->next = old_node->next;
    // 给定节点是原表尾节点
    if (list->tail == old_node) {
      list->tail = node;
    }
    // 将新节点添加到给定节点之前
  } else {
    node->next = old_node;
    node->prev = old_node->prev;
    // 给定节点是原表头节点
    if (list->head == old_node) {
      list->head = node;
    }
  }

  // 更新新节点的前置指针
  if (node->prev != NULL) {
    node->prev->next = node;
  }
  // 更新新节点的后置指针
  if (node->next != NULL) {
    node->next->prev = node;
  }

  // 更新链表节点数
  list->len++;

  return list;
}

// 删除指定节点
void listDelNode(list *list, listNode *node) {
  if (node->prev)
    node->prev->next = node->next;
  else
    list->head = node->next;

  if (node->next)
    node->next->prev = node->prev;
  else
    list->tail = node->prev;

  // 释放值
  if (list->free)
    list->free(node->value);
  // 释放节点
  zfree(node);
  list->len--;
}

listIter *listGetIterator(list *list, int direction) {
  // 为迭代器分配内存
  listIter *iter;
  if ((iter = zmalloc(sizeof(*iter))) == NULL)
    return NULL;

  // 根据迭代方向 设置迭代器的起始节点
  if (direction == AL_START_HEAD){
    iter->next = list->head;
  } else {
    iter->next = list->tail;
  }
  // 记录迭代方向
  iter->direction = direction;
  return iter;
}

listNode *listNext(listIter *iter) {
  listNode *current = iter->next;

  if (current != NULL) {
    if (iter->direction == AL_START_HEAD) {
      iter->next = current->next;
    } else {
      iter->next = current->prev;
    }
  }
  return current;
}

// 释放链表的迭代器
void listReleaseIterator(listIter *iter) {
  zfree(iter);
}

// 复制整个链表
list *listDup(list *orig) {
  list *copy;
  listIter *iter;
  listNode *node;

  // create the new list
  if ((copy = listCreate()) == NULL) {
    return NULL;
  }

  copy->dup = orig->dup;
  copy->free = orig->free;
  copy->dup = orig->dup;

  // 复制链表元素
  iter = listGetIterator(orig, AL_START_HEAD);
  while((node = listNext(iter)) != NULL) {
    void *value;

    // 复制节点到新节点
    if (copy->dup) {
      value = copy->dup(node->value); // 复制失败
      if (value == NULL) {
        listRelease(copy);
        listReleaseIterator(iter);
        return NULL;
      }
    } else {
      value = node->value; // 浅复制
    }

    // 添加该节点
    if (listAddNodeTail(copy, value) == NULL) { // 添加失败
      listRelease(copy);
      listReleaseIterator(iter);
      return NULL;
    }
  }
  // 释放迭代器
  listReleaseIterator(iter);

  // 返回副本
  return copy;
}

// 从链表中搜索某个值
listNode *listSearchKey(list *list, void *key) {
  listIter *iter;
  listNode *node;

  // 迭代整个链表
  iter = listGetIterator(list, AL_START_HEAD);
  while ((node = listNext(iter)) != NULL) {

    // 对比
    if (list->match) {
      if (list->match(node->value, key)) {
        listReleaseIterator(iter);
        // 找到
        return node;
      }
    } else {
      if (key == node->value) {
        listReleaseIterator(iter);
        // 找到
        return node;
      }
    }
  }

  listReleaseIterator(iter);

  // 未找到
  return NULL;
}

// 从链表中定位某个节点 支持负数
listNode *listIndex(list *list, long index) {
  listNode *n;

  // 如果索引为负数，从表尾开始查找
  if (index < 0) {
    index = (-index) - 1;
    n = list->tail;
    while (index-- && n)
      n = n->prev;
    // 如果索引为正数，从表头开始查找
  } else {
    n = list->head;
    while (index-- && n)
      n = n->next;
  }

  return n;
}

// 设置迭代器方向为 AL_START_HEAD 并设置迭代器到表头
void listRewind(list *list, listIter *li) {
  li->next = list->head;
  li->direction = AL_START_HEAD;
}

// 设置迭代器方向为 AL_START_TAIL 并设置迭代器到表尾
void listRewindTail(list *list, listIter *li) {
  li->next = list->tail;
  li->direction = AL_START_TAIL;
}

// 取出链表的尾节点插到头部成为头节点
void listRotate(list *list) {
  listNode *tail = list->tail;

  if (listLength(list) <= 1)
    return;

  list->tail = tail->prev;
  list->tail->next = NULL;

  list->head->prev = tail;
  tail->prev = NULL;
  tail->next = list->head;
  list->head = tail;
}
