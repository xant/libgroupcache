#include <shardcache.h>

shardcache_storage_t *storage_mem_create(const char **options);
void storage_mem_destroy(shardcache_storage_t *st);
