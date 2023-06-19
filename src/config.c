#include "config.h"
#include "zmalloc.h"
#include "redis.h"

void loadServerConfig(char *filename, char *options);
void appendServerSaveParams(time_t seconds, int changes);

void resetServerSaveParams() {
  zfree(server.saveparams);
}

struct rewriteConfigState; /* Forward declaration to export API. */
void rewriteConfigRewriteLine(struct rewriteConfigState *state, char *option, sds line, int force);
int rewriteConfig(char *path);
