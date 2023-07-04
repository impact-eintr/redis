#include "config.h"
#include "util.h"
#include "zmalloc.h"
#include "redis.h"
#include "sds.h"

#include <stdio.h>

void loadServerConfigFromString(char *config) {
  char *err = NULL;
  int linenum = 0, totlines, i;
  int slaveof_linenum = 0;
  sds *lines;

  lines = sdssplitlen(config, strlen(config), "\n", 1, &totlines);

  for (i = 0; i < totlines; i++) {
    sds *argv;
    int argc;

    linenum = i + 1;
    // 移除字符串的前置空白和后缀空白
    lines[i] = sdstrim(lines[i], " \t\r\n");

    /* Skip comments and blank lines */
    // 跳过空白行
    if (lines[i][0] == '#' || lines[i][0] == '\0') {
      continue;
    }

    /* Split into arguments */
    // 将字符串分割成多个参数
    argv = sdssplitargs(lines[i], &argc);
    if (argv == NULL) {
      err = "Unbalanced quotes in configuration line";
      goto loaderr;
    }

    /* Skip this line if the resulting command vector is empty. */
    // 跳过空白参数
    if (argc == 0) {
      sdsfreesplitres(argv, argc);
      continue;
    }

    // 将选项名字转换成小写
    // 例如 TIMEOUT 转换成 timeout
    sdstolower(argv[0]);

    if (!strcasecmp(argv[0], "port") && argc == 2) {
      server.port = atoi(argv[1]);
      if (server.port < 0 || server.port > 65535) {
        err = "Invalid port";
        goto loaderr;
      }
    } else if (!strcasecmp(argv[0], "dbfilename") && argc == 2) {
      if (!pathIsBaseName(argv[1])) {
        err = "dbfilename can not be a path, just a filename";
        goto loaderr;
      }
      zfree(server.rdb_filename);
      server.rdb_filename = zstrdup(argv[1]);
    }
  }

  sdsfreesplitres(lines, totlines);
  return;

loaderr:
  fprintf(stderr, "\n*** FATAL CONFIG FILE ERROR ***\n");
  fprintf(stderr, "Reading the configuration file, at line %d\n", linenum);
  fprintf(stderr, ">>> '%s'\n", lines[i]);
  fprintf(stderr, "%s\n", err);
  exit(1);
}

void loadServerConfig(char *filename, char *options) {
  sds config = sdsempty();
  char buf[REDIS_CONFIGLINE_MAX+1];
  FILE *fp;
  if (filename) {
    if (filename[0] == '-' && filename[1] == '\0') {
      fp = stdin;
    } else {
      if ((fp = fopen(filename, "r")) == NULL) {
        redisLog(REDIS_WARNING, "Fatal error, can not open config file: %s",  filename);
        exit(1);
      }
    }
    while(fgets(buf, REDIS_CONFIGLINE_MAX+1, fp) != NULL) {
      config = sdscat(config, buf);
    }
    if (fp != stdin) {
      fclose(fp);
    }
  }

  loadServerConfigFromString(config);
  sdsfree(config);
}

void appendServerSaveParams(time_t seconds, int changes);

void resetServerSaveParams() {


}

struct rewriteConfigState; /* Forward declaration to export API. */
void rewriteConfigRewriteLine(struct rewriteConfigState *state, char *option, sds line, int force);
int rewriteConfig(char *path);
