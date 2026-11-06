#include "sds.h"
#include "zmalloc.h"

#include <string.h>
#include <stdio.h>


sds sdsnew(const char* init) {
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

sds sdsnewlen(const void* init, size_t initlen) {
    struct sdshdr *sh;

    if (init) {
        sh = zmalloc(sizeof(struct sdshdr) + initlen + 1);
    } else {
        sh = zcalloc(sizeof(struct sdshdr) + initlen + 1);
    }
    if (sh == NULL) {
        return NULL;
    }

    sh->len = initlen;
    sh->free = 0;
    if (initlen && init) {
        memcpy(sh->buf, init, initlen);
    }
    sh->buf[initlen] = '\0';

    return (char *)sh->buf;
}

sds sdsempty(void) {
    return sdsnewlen("", 0);
}

sds sdsdup(const sds s) {
    return sdsnewlen(s, sdslen(s));
}

void sdsfree(sds s) {
    if (s == NULL) return ;
    zfree(SDS_HDR(s));
}

void sdsclean(sds s) {
    struct sdshdr *sh = SDS_HDR(s);
    sh->free += sh->len;
    sh->len = 0;
    sh->buf[0] = 0;
}

sds sdsMakeRoomFor(sds s, size_t addlen) {
    struct sdshdr *sh, *newsh;
    size_t free = sdsavail(s);

    size_t len, newlen;

    if (free > addlen) return s;

    len = sdslen(s);
    sh = SDS_HDR(s);

    newlen = (len + addlen);
    if (newlen < SDS_MAX_PREALLOC) {
        newlen *=2 ;
    } else {
        newlen += SDS_MAX_PREALLOC;
    }
    newsh = zrealloc(sh, sizeof(struct sdshdr) + newlen + 1);

    if (newsh == NULL) return NULL;

    newsh->free = newlen - len;

    return newsh->buf;
}

sds sdsgrowzero(sds s, size_t len) {
    struct sdshdr *sh = SDS_HDR(s);
    size_t totlen, curlen = sh->len;

    if (len <= curlen) return s;
    s = sdsMakeRoomFor(s, len - curlen);

    if (s == NULL) return NULL;

    sh = SDS_HDR(s);
    memset(s + curlen, 0, (len - curlen + 1));

    totlen = sh->len + sh->free;
    sh->len = len;
    sh->free = totlen - sh->len;
    return s;    
}

sds sdscatlen(sds s, const void* t, size_t len) {
    struct sdshdr *sh;
    size_t curlen = sdslen(s);

    s = sdsMakeRoomFor(s, len);

    if (s == NULL) return NULL;
    memcpy(s + curlen, t, len);

    sh = SDS_HDR(s);
    sh->len = curlen + len;
    sh->free -= len;
    
    s[curlen + len] = '\0';

    return s;
}
sds sdscat(sds s, const char* t) {
    return sdscatlen(s, t, strlen(t));
}

sds sdscatsds(sds s, const sds t) {
    return sdscatlen(s, t, sdslen(t));
}

sds sdscpylen(sds s, const char* t, size_t len) {
    struct sdshdr *sh = SDS_HDR(s);

    size_t totlen = sh->len + sh->free;

    if (totlen < len) {
        s = sdsMakeRoomFor(s, len - sh->len);
        if (s == NULL) return NULL;
        sh = SDS_HDR(s);
        totlen = sh->len + sh->free;
    }
    memcpy(s, t, len);
    s[len] = '\0';
    sh->len = len;
    sh->free = totlen - len;
    return s;
}

sds sdscpy(sds s, const char* t) {
    return sdscpylen(s, t, strlen(t));
}

int main() {
    sds s = sdsnew("Hello, World!");
    printf("sds: %s\n", s);
    sdsfree(s);
    return 0;
}