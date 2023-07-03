#include "config.h"
#include "zmalloc.h"
#include "redis.h"
#include "sds.h"

#include <stdio.h>

void loadServerConfigFromString(char *config) {
  char *err = NULL;
  int line = 0, totlines, i;
  int slaveof_linenum = 0;
  sds *lines;

  lines = sdssplitlen(config, strlen(config), "\n", 1, &totlines);

  for (i = 0;i < totlines; i++) {
    printf("%s\n", lines[i]);
  }
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
