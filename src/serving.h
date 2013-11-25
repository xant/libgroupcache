#include "groupcache.h"

typedef struct __groupcache_serving_s {
    groupcache_t *cache;
    int sock;
    const char *auth;
} groupcache_serving_t;

void *accept_requests(void *priv);
