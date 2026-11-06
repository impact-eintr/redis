#ifndef ADLIST_H_
#define ADLIST_H_

typedef struct listNode {
    struct listNode *prev;
    struct listNode *next;
    void *value;
} listNode;


typedef struct listIter {
    listNode *next;
    int direction;
} listIter;


typedef struct list {
    listNode *head;
    listNode *tail;
    void *(*dup)(void *ptr); // 函数指针
    void (*free)(void *ptr);
    int (*match)(void *ptr, void *another);
    unsigned long len;
} list;

#define listLength(l) ((l)->len)
#define listFirst(l) ((l)->head)
#define listLast(l) ((l)->tail)
#define listPrevNode(n) ((n)->prev)
#define listNextNode(n) ((n)->next)
#define listNodeValue(n) ((n)->value)



#endif