

#================================================================
#   Copyright (C) 2023 Sangfor Ltd. All rights reserved.
#   
#   文件名称：Makefile
#   创 建 者：Impact-Eintr
#   创建日期：2023年07月03日
#   邮    箱：yixingwei4@gmail.com
#   Github  ：https://github.com/impact-eintr
#   描    述：
#

#================================================================
CFLAGS +=-DUSE_TCMALLOC -g
CC = gcc

CORE = zmalloc.c redis.c db.c ae.c ae_epoll.c
CONFIG = config.c

DATASTRUCT = object.c dict.c sds.c ziplist.c adlist.c t_string.c t_list.c t_hash.c t_zset.c
PERSISTENCE = rio.c rdb.c

NETWORK = networking.c  anet.c

REPLICATION = replication.c
UTILS = endianconv.c util.c debug.c memtest.c
OTHER = multi.c

ALL:redis

run:redis
	./redis redis.ini

redis:
	$(CC) $(CFLAGS) $(CORE) $(CONFIG) $(DATASTRUCT) $(NETWORK) $(PERSISTENCE) $(REPLICATION) $(OTHER) $(UTILS) -o $@


clean:
	rm redis -rf
