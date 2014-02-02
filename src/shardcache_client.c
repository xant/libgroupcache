#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <regex.h>
#include <chash.h>
#include <fbuf.h>
#include <rbuf.h>
#include <linklist.h>
#include <iomux.h>

#include <pthread.h>

#include "connections.h"
#include "messaging.h"
#include "connections_pool.h"
#include "shardcache_client.h"

typedef struct chash_t chash_t;

struct shardcache_client_s {
    chash_t *chash;
    shardcache_node_t *shards;
    connections_pool_t *connections;
    int num_shards;
    const char *auth;
    int errno;
    char errstr[1024];
};

#define ADDR_REGEXP "^[a-z0-9_\\.\\-]+(:[0-9]+)?$"
static int check_address_string(char *str)
{
    regex_t addr_regexp;
    int rc = regcomp(&addr_regexp, ADDR_REGEXP, REG_EXTENDED|REG_ICASE);
    if (rc != 0) {
        char errbuf[1024];
        regerror(rc, &addr_regexp, errbuf, sizeof(errbuf));
        fprintf(stderr, "Can't compile regexp %s: %s\n", ADDR_REGEXP, errbuf);
        return -1;
    }

    int matched = regexec(&addr_regexp, str, 0, NULL, 0);
    regfree(&addr_regexp);

    if (matched != 0) {
        return -1;
    }

    return 0;
}

shardcache_client_t *shardcache_client_create(shardcache_node_t *nodes, int num_nodes, char *auth)
{
    int i;
    if (!num_nodes) {
        fprintf(stderr, "Can't create a shardcache client with no nodes\n");
        return NULL;
    }
    shardcache_client_t *c = calloc(1, sizeof(shardcache_client_t));

    size_t shard_lens[num_nodes];
    char *shard_names[num_nodes];

    c->shards = malloc(sizeof(shardcache_node_t) * num_nodes);
    c->connections = connections_pool_create(30);
    memcpy(c->shards, nodes, sizeof(shardcache_node_t) * num_nodes);
    for (i = 0; i < num_nodes; i++) {
        if (check_address_string(c->shards[i].address) != 0) {
            fprintf(stderr, "Bad address format %s\n", c->shards[i].address);
            free(c->shards);
            free(c);
            return NULL;
        }
        shard_names[i] = c->shards[i].label;
        shard_lens[i] = strlen(shard_names[i]);
    }

    c->num_shards = num_nodes;

    c->chash = chash_create((const char **)shard_names, shard_lens, c->num_shards, 200);

    if (auth && *auth) {
        c->auth = calloc(1, 16);
        strncpy((char *)c->auth, auth, 16);
    }

    return c;
}

char *select_node(shardcache_client_t *c, void *key, size_t klen)
{
    const char *node_name;
    size_t name_len = 0;
    int i;
    char *addr = NULL;
    chash_lookup(c->chash, key, klen, &node_name, &name_len);

    for (i = 0; i < c->num_shards; i++) {
        if (strncmp(node_name, c->shards[i].label, name_len) == 0) {
            addr = c->shards[i].address;
            break;
        }
    }

    return addr;
}

size_t shardcache_client_get(shardcache_client_t *c, void *key, size_t klen, void **data)
{
    char *node = select_node(c, key, klen);
    int fd = connections_pool_get(c->connections, node);
    if (fd < 0)
        return 0;

    fbuf_t value = FBUF_STATIC_INITIALIZER;
    int rc = fetch_from_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, &value, fd);
    if (rc == 0) {
        if (data)
            *data = fbuf_data(&value);

        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;

        connections_pool_add(c->connections, node, fd);
        return fbuf_used(&value);
    } else {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr), "Can't fetch data from node '%s'", node);
        return 0;
    }
    return 0;
}

size_t shardcache_client_offset(shardcache_client_t *c, void *key, size_t klen, uint32_t offset, void *data, uint32_t dlen)
{
    char *node = select_node(c, key, klen);
    int fd = connections_pool_get(c->connections, node);
    if (fd < 0)
        return 0;

    fbuf_t value = FBUF_STATIC_INITIALIZER;
    int rc = offset_from_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, offset, dlen, &value, fd);
    if (rc == 0) {
        uint32_t to_copy = dlen > fbuf_used(&value) ? fbuf_used(&value) : dlen;
        if (data)
            memcpy(data, fbuf_data(&value), to_copy);

        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;

        connections_pool_add(c->connections, node, fd);
        fbuf_destroy(&value);
        return to_copy;
    } else {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr), "Can't fetch data from node '%s'", node);
    }
    fbuf_destroy(&value);
    return 0;
}

int shardcache_client_exists(shardcache_client_t *c, void *key, size_t klen)
{
    char *node = select_node(c, key, klen);
    int fd = connections_pool_get(c->connections, node);
    if (fd < 0)
        return -1;
    int rc = exists_on_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, fd);
    if (rc == -1) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr),
                "Can't check existance of data on node '%s'", node);
    } else {
        connections_pool_add(c->connections, node, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }
    return rc;
}

int shardcache_client_touch(shardcache_client_t *c, void *key, size_t klen)
{
    char *node = select_node(c, key, klen);
    int fd = connections_pool_get(c->connections, node);
    if (fd < 0)
        return -1;
    int rc = touch_on_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, fd);
    if (rc == -1) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr),
                 "Can't touch key '%s' on node '%s'", (char *)key, node);
    } else {
        connections_pool_add(c->connections, node, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }
    return rc;
}

static int
_shardcache_client_set_internal(shardcache_client_t *c, void *key, size_t klen, void *data, size_t dlen, uint32_t expire, int inx)
{
    char *node = select_node(c, key, klen);
    int fd = connections_pool_get(c->connections, node);
    if (fd < 0)
        return -1;

    int rc = -1;
    if (inx)
        rc = add_to_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, data, dlen, expire, fd);
    else
        rc = send_to_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, data, dlen, expire, fd);

    if (rc == -1) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr), "Can't set new data on node '%s'", node);
    } else {
        connections_pool_add(c->connections, node, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }
    return rc;
}

int
shardcache_client_set(shardcache_client_t *c, void *key, size_t klen, void *data, size_t dlen, uint32_t expire)
{
     return _shardcache_client_set_internal(c, key, klen, data, dlen, expire, 0);
}

int
shardcache_client_add(shardcache_client_t *c, void *key, size_t klen, void *data, size_t dlen, uint32_t expire)
{
     return _shardcache_client_set_internal(c, key, klen, data, dlen, expire, 1);
}

int shardcache_client_del(shardcache_client_t *c, void *key, size_t klen)
{
    char *node = select_node(c, key, klen);
    int fd = connections_pool_get(c->connections, node);
    if (fd < 0)
        return -1;
    int rc = delete_from_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, fd);
    if (rc != 0) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr), "Can't delete data from node '%s'", node);
    } else {
        connections_pool_add(c->connections, node, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }
    return rc;
}

int shardcache_client_evict(shardcache_client_t *c, void *key, size_t klen)
{
    char *node = select_node(c, key, klen);
    int fd = connections_pool_get(c->connections, node);
    if (fd < 0)
        return -1;

    int rc = evict_from_peer(node, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, key, klen, fd);
    if (rc != 0) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr), "Can't evict data from node '%s'", node);
    } else {
        connections_pool_add(c->connections, node, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }

    return rc;
}

shardcache_node_t *shardcache_get_node(shardcache_client_t *c, char *node_name)
{
    shardcache_node_t *node = NULL;

    int i;
    for (i = 0; i < c->num_shards; i++) {
        if (strcmp(node_name, c->shards[i].label) == 0) {
            node = &c->shards[i];
            break;
        }
    }

    if (!node) {
        c->errno = SHARDCACHE_CLIENT_ERROR_ARGS;
        snprintf(c->errstr, sizeof(c->errstr), "Unknown node '%s'", node_name);
        return NULL;
    }

    return node;
}
int shardcache_client_stats(shardcache_client_t *c, char *node_name, char **buf, size_t *len)
{

    shardcache_node_t *node = shardcache_get_node(c, node_name);
    if (!node)
        return -1;

    int fd = connections_pool_get(c->connections, node->address);
    if (fd < 0)
        return -1;

    int rc = stats_from_peer(node->address, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, buf, len, fd);
    if (rc != 0) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr), "Can't get stats from node '%s'", node->label);
    } else {
        connections_pool_add(c->connections, node->address, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }

    return rc;
}

int shardcache_client_check(shardcache_client_t *c, char *node_name) {
    shardcache_node_t *node = shardcache_get_node(c, node_name);
    if (!node)
        return -1;

    int fd = connections_pool_get(c->connections, node->address);
    if (fd < 0)
        return -1;

    int rc = check_peer(node->address, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, fd);
    if (rc != 0) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr), "Can't check node '%s'", node->label);
    } else {
        connections_pool_add(c->connections, node->address, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }
    return rc;
}

void shardcache_client_destroy(shardcache_client_t *c)
{
    chash_free(c->chash);
    free(c->shards);
    if (c->auth)
        free((void *)c->auth);
    connections_pool_destroy(c->connections);
    free(c);
}

int shardcache_client_errno(shardcache_client_t *c)
{
    return c->errno;
}

char *shardcache_client_errstr(shardcache_client_t *c)
{
    return c->errstr;
}

shardcache_storage_index_t *shardcache_client_index(shardcache_client_t *c, char *node_name)
{
    shardcache_node_t *node = shardcache_get_node(c, node_name);
    if (!node)
        return NULL;

    int fd = connections_pool_get(c->connections, node->address);
    if (fd < 0)
        return NULL;

    shardcache_storage_index_t *index = index_from_peer(node->address, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, fd);
    if (!index) {
        close(fd);
        c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
        snprintf(c->errstr, sizeof(c->errstr), "Can't get index from node '%s'", node->label);
    } else {
        connections_pool_add(c->connections, node->address, fd);
        c->errno = SHARDCACHE_CLIENT_OK;
        c->errstr[0] = 0;
    }

    return index;
}

int shardcache_client_migration_begin(shardcache_client_t *c, shardcache_node_t *nodes, int num_nodes)
{
    fbuf_t mgb_message = FBUF_STATIC_INITIALIZER;

    int i;
    for (i = 0; i < num_nodes; i++) {
        if (check_address_string(nodes[i].address) != 0) {
            c->errno = SHARDCACHE_CLIENT_ERROR_ARGS;
            snprintf(c->errstr, sizeof(c->errstr), "Bad address format %s\n", nodes[i].address);
            fbuf_destroy(&mgb_message);
            return -1;
        }
        if (i > 0) 
            fbuf_add(&mgb_message, ",");
        fbuf_printf(&mgb_message, "%s:%s", nodes[i].label, nodes[i].address);
    }

    for (i = 0; i < c->num_shards; i++) {
        int fd = connections_pool_get(c->connections, c->shards[i].address);
        if (fd < 0) {
            fbuf_destroy(&mgb_message);
            return -1;
        }

        int rc = migrate_peer(c->shards[i].address,
                              (char *)c->auth,
                              SHC_HDR_SIGNATURE_SIP,
                              fbuf_data(&mgb_message),
                              fbuf_used(&mgb_message), fd);
        if (rc != 0) {
            close(fd);
            c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
            snprintf(c->errstr, sizeof(c->errstr), "Node '%s' (%s) didn't aknowledge the migration\n",
                    c->shards[i].label, c->shards[i].address);
            fbuf_destroy(&mgb_message);
            // XXX - should we abort migration on peers that have been notified (if any)? 
            return -1;
        }
        connections_pool_add(c->connections, c->shards[i].address, fd);
    }
    fbuf_destroy(&mgb_message);

    c->errno = SHARDCACHE_CLIENT_OK;
    c->errstr[0] = 0;

    return 0;
}

int shardcache_client_migration_abort(shardcache_client_t *c)
{
    int i;
    for (i = 0; i < c->num_shards; i++) {
        int fd = connections_pool_get(c->connections, c->shards[i].address);
        if (fd < 0) {
            return -1;
        }

        int rc =  abort_migrate_peer(c->shards[i].label, (char *)c->auth, SHC_HDR_SIGNATURE_SIP, fd);

        if (rc != 0) {
            close(fd);
            c->errno = SHARDCACHE_CLIENT_ERROR_NODE;
            snprintf(c->errstr, sizeof(c->errstr),
                     "Can't abort migration from node '%s'", c->shards[i].label);
            return -1;
        }
        connections_pool_add(c->connections, c->shards[i].address, fd);
    }
 
    c->errno = SHARDCACHE_CLIENT_OK;
    c->errstr[0] = 0;

    return 0;
}

int shardcache_client_get_async(shardcache_client_t *c,
                                   void *key,
                                   size_t klen,
                                   shardcache_client_get_aync_data_cb data_cb,
                                   void *priv)
{
    char *node = select_node(c, key, klen);
    int fd = connections_pool_get(c->connections, node);
    if (fd < 0)
        return -1;

    return fetch_from_peer_async(node, (char *)c->auth, SHC_HDR_CSIGNATURE_SIP, key, klen, data_cb, priv, fd, NULL);
}

shc_multi_item_t *shc_multi_item_create(shardcache_client_t *c,
                                        void  *key,
                                        size_t klen,
                                        void  *data,
                                        size_t dlen)
{
    shc_multi_item_t *item = calloc(1, sizeof(shc_multi_item_t));
    item->key = malloc(klen);
    memcpy(item->key, key, klen);
    item->klen = klen;

    if (dlen) {
        item->data = malloc(dlen);
        memcpy(item->data, data, dlen);
        item->dlen = dlen;
    }

    item->c = c;
    return item;
}

void shc_multi_item_destroy(shc_multi_item_t *item)
{
    free(item->key);
    if (item->data)
        free(item->data);
}

static void *shc_get_multi(void *priv)
{
    linked_list_t *items = (linked_list_t *)priv;

    int i;
    for (i = 0; i < list_count(items); i++) {
        shc_multi_item_t *item = pick_value(items, i);
        item->dlen = shardcache_client_get(item->c, item->key, item->klen, &item->data);
    }
    return NULL;
}

static void *shc_set_multi(void *priv)
{
    linked_list_t *items = (linked_list_t *)priv;

    int i;
    for (i = 0; i < list_count(items); i++) {
        shc_multi_item_t *item = pick_value(items, i);
        if (item->data && item->dlen)
            item->status = shardcache_client_set(item->c, item->key, item->klen, item->data, item->dlen, item->expire);
        else
            item->status = shardcache_client_del(item->c, item->key, item->klen);

    }
    return NULL;
}

static linked_list_t *shc_split_buckets(shardcache_client_t *c,
                                        shc_multi_item_t **items)
{
    linked_list_t *pools = create_list();
    int i;
    for(i = 0; items[i]; i++) {
        const char *node_name;
        size_t name_len = 0;

        shc_multi_item_t *item = items[i];

        chash_lookup(c->chash, item->key, item->klen, &node_name, &name_len);

        char name_string[name_len+1];
        snprintf(name_string, name_len+1, "%s", node_name);

        tagged_value_t *tval = get_tagged_value(pools, name_string);
        if (!tval) {
            linked_list_t *sublist = create_list();
            tval = create_tagged_sublist(name_string, sublist);
            push_tagged_value(pools, tval);
        }

        push_value((linked_list_t *)tval->value, item);

    }

    return pools;
}

int shardcache_client_get_multi(shardcache_client_t *c,
                                shc_multi_item_t **items)
{
    linked_list_t *pools = shc_split_buckets(c, items);

    pthread_t getters[list_count(pools)];

    int i;
    for (i = 0; i < list_count(pools); i++) {
        pthread_create(&getters[i], NULL, shc_get_multi, pick_tagged_value(pools, i)->value);
    }

    for (i = 0; i < list_count(pools); i++) {
        pthread_join(getters[i], NULL);
    }

    destroy_list(pools);
    return 0;
}

int shardcache_client_set_multi(shardcache_client_t *c,
                                shc_multi_item_t **items)

{
    linked_list_t *pools = shc_split_buckets(c, items);

    pthread_t setters[list_count(pools)];

    int i;
    for (i = 0; i < list_count(pools); i++) {
        pthread_create(&setters[i], NULL, shc_set_multi, pick_tagged_value(pools, i)->value);
    }

    for (i = 0; i < list_count(pools); i++) {
        pthread_join(setters[i], NULL);
    }

    destroy_list(pools);
    return 0;
}
