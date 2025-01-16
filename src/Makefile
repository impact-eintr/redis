

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
CFLAGS +=-g -D _GNU_SOURCE
CC = gcc

CORE = zmalloc.c redis.c db.c ae.c ae_epoll.c
CONFIG = config.c

DATASTRUCT = object.c dict.c sds.c ziplist.c adlist.c t_string.c t_list.c t_hash.c t_zset.c
PERSISTENCE = rio.c rdb.c

NETWORK = networking.c  anet.c syncio.c

REPLICATION = replication.c
UTILS = endianconv.c util.c debug.c memtest.c
OTHER = multi.c

ALL:redis

slave:clean redis
	./redis slave.ini

master:clean redis
	./redis master.ini

redis:
	$(CC) $(CFLAGS) $(CORE) $(CONFIG) $(DATASTRUCT) $(NETWORK) $(PERSISTENCE) $(REPLICATION) $(OTHER) $(UTILS) -o $@

sds:
	$(CC) $(CFLAGS) -D SDS zmalloc.c sds.c -o $@

adlist:
	$(CC) $(CFLAGS) -D ADLIST zmalloc.c sds.c adlist.c  -o $@

dict:
	$(CC) $(CFLAGS) -D DICT zmalloc.c sds.c adlist.c dict.c -o $@

clean:
	rm redis sds adlist dict -rf