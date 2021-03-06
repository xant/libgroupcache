#include <shardcache_client.h>
#include <unistd.h>
#include <sys/types.h>
#include <ut.h>
#include <libgen.h>
#include <arpa/inet.h>

int main(int argc, char **argv)
{
    int i;
    int num_nodes = 2;
    shardcache_node_t **nodes = NULL;
    shardcache_t *servers[num_nodes];

    shardcache_log_init("shardcached", LOG_WARNING);


    ut_init(basename(argv[0]));

    nodes = malloc(sizeof(shardcache_node_t *) * num_nodes);
    for (i = 0; i < num_nodes; i++) {
        char label[32];
        sprintf(label, "peer%d", i);
        char address[32];
        sprintf(address, "127.0.0.1:975%d", i);
        char *address_array[1] = { address };
        nodes[i] = shardcache_node_create(label, address_array, 1);
    }

    // create a set of servers
    for (i = 0; i < num_nodes; i++) {
        ut_testing("shardcache_create(nodes[%d].label, nodes, num_nodes, NULL, NULL, 5, 1<<29", i);
        servers[i] = shardcache_create(shardcache_node_get_label(nodes[i]),
                                       nodes,
                                       num_nodes,
                                       NULL,
                                       5,
                                       0,
                                       1<<29);
        if (servers[i]) {
            ut_success();
            shardcache_iomux_run_timeout_low(servers[i], 5000);
        } else {
            ut_failure("Errors creating the shardcache instance");
        }
    }

    sleep(1); // let the servers complete their startup

    // now create a client to communicate with the servers
    ut_testing("shardcache_client_create(nodes, num_nodes");
    shardcache_client_t *client = shardcache_client_create(nodes, num_nodes);
    ut_validate_int((client != NULL), 1);

    char key[32];
    char val[32];
    void *value = NULL;
    size_t size;

    sprintf(key, "test_key1");
    sprintf(val, "test_value1");

    ut_testing("shardcache_client_set(client, test_key1, 9, test_value1, 11, 0) == 0");
    int ret = shardcache_client_set(client, key, strlen(key), val, strlen(val), 0);
    ut_validate_int(ret, 0);

    ut_testing("shardcache_client_get(client, test_key1, 9, &value) == test_value1");
    size = shardcache_client_get(client, key, strlen(key), (void **)&value);
    ut_validate_buffer(value, size, val, strlen(val));
    free(value);

    ut_testing("shardcache_client_del(client, test_key1, 9) == 0");
    ret = shardcache_client_del(client, "test_key1", 9);
    ut_validate_int(ret, 0);

    ut_testing("shardcache_client_get(client, test_key1, 9, &value) == NULL");
    size = shardcache_client_get(client, "test_key1", 9, &value);
    ut_validate_int((size == 0 && value == NULL), 1);

    ut_testing("shardcache_client_exists(client, test_key1, 9) == 0");
    ret = shardcache_client_exists(client, "test_key1", 9);
    ut_validate_int(ret, 0);

    shardcache_client_del(client, "test_key2", 9);
    ut_testing("shardcache_client_add(client, test_key2, 9, test_value2, 11, 0) == 0");
    ret = shardcache_client_add(client, "test_key2", 9, "test_value2", 11, 0);
    ut_validate_int(ret, 0);

    ut_testing("shardcache_client_exists(client, test_key2, 9) == 1");
    ret = shardcache_client_exists(client, "test_key2", 9);
    ut_validate_int(ret, 1);

    ut_testing("shardcache_client_get(client, test_key2, 9, &value) == test_value2");
    size = shardcache_client_get(client, "test_key2", 9, &value);
    ut_validate_buffer(value, size, "test_value2", 11);
    free(value);

    ut_testing("shardcache_client_add(client, test_key2, 9, test_value_modified, 19, 0) == 1");
    ret = shardcache_client_add(client, "test_key2", 9, "test_value_modified", 19, 0);
    ut_validate_int(ret, 1);

    // the value is unchanged because already existing
    ut_testing("shardcache_client_get(client, test_key2, 9, &value) == test_value2 (unchanged)");
    size = shardcache_client_get(client, "test_key2", 9, &value);
    ut_validate_buffer(value, size, "test_value2", 11);
    free(value);

    // the value is unchanged because already existing
    ut_testing("shardcache_client_offset(client, test_key2, 9, 5, &partial, 5) == value");
    char partial[6];
    size = shardcache_client_offset(client, "test_key2", 9, 5, &partial, 5);
    ut_validate_buffer(partial, 5, "value", 5);
    
    ut_testing("shardcache_client_offset(client, test_key1, 9, 5, &partial, 6) == 0");
    size = shardcache_client_offset(client, "test_key1", 9, 5, &partial, 6);
    ut_validate_int(size, 0);

    shardcache_client_set(client, "test_key3", 9, "test_value3", 11, 0);
    ut_testing("shardcache_client_offset(client, test_key3, 9, 5, &partial, 6) == value3");
    size = shardcache_client_offset(client, "test_key3", 9, 5, &partial, 6);
    ut_validate_buffer(partial, 6, "value3", 6);
    



    // create now two clients each knowing exclusively about 1 server
    // (different among the two clients)
    shardcache_client_t *client1 = shardcache_client_create(&nodes[0], 1);
    shardcache_client_t *client2 = shardcache_client_create(&nodes[1], 1);

    // the following tests communication among peers
    // (sets 100 keys using one node and reads them using the other node,
    //  so some keys will be owned from the node used for writing while other
    //  ones will be owned by the node used for reading)
    ut_testing("shardcache_client_set(c1, k, kl, &v) == shardcache_client_get(c2, k, kl)");
    int failed = 0;
    for (i = 100; i < 200; i++) {
        char k[64];
        char v[64];
        char *vv = NULL;;

        sprintf(k, "test_key%d", i);
        sprintf(v, "test_value%d", i);
        shardcache_client_set(client1, k, strlen(k), v, strlen(v), 0);
        void *vptr = NULL;
        size_t s = shardcache_client_get(client2, k, strlen(k), &vptr);
        vv = (char *)vptr;
        if (s == 0) {
            ut_failure("no data for key %s", k);
            failed = 1;
        } else if(strcmp(vv, v) != 0) {
            ut_failure("%s != %s", vv, v);
            failed = 1;
        }
        free(vv);
        int count = i - 100;
        if (count%10 == 0)
            ut_progress(count);
    }
    if (!failed)
        ut_success();

    shc_multi_item_t *items[11];
    for (i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "test_key%d", 100+i);
        items[i] = shc_multi_item_create(key, strlen(key), NULL, 0);
    }
    items[10] = NULL; // null-terminate it

    failed = 0;
    ut_testing("shardcache_client_get_multi(c, items)");
    shardcache_client_get_multi(client, items);

    for (i = 0; i < 10; i++) {
        char v[64];
        if (!failed) {
            sprintf(v, "test_value%d", 100+i);
            if (!items[i]->data || strncmp(items[i]->data, v, items[i]->dlen) != 0)
            { 
                ut_failure("%s != %s", items[i]->data, v);
                failed = 1;
                break;
            }
        }
        shc_multi_item_destroy(items[i]);
    }
    if (!failed)
        ut_success();

    for (i = 0; i < 10; i++) {
        char key[32];
        char value[32];
        snprintf(key, sizeof(key), "test_key%d", 200+i);
        snprintf(value, sizeof(value), "test_value%d", 200+i);
        items[i] = shc_multi_item_create(key, strlen(key), value, strlen(value));
    }

    ut_testing("shardcache_client_set_multi(c, items)");
    shardcache_client_set_multi(client, items);

    failed = 0;
    for (i = 0; i < 10; i++) {
        void *value;
        char key[32];
        snprintf(key, sizeof(key), "test_key%d", 200+i);
        if (items[i]->status != 0) {
            ut_failure("status for key %s != 0", key);
            failed = 1;
            break;
        }
        size_t size = shardcache_client_get(client, items[i]->key, items[i]->klen, &value);
        if (size != items[i]->dlen) {
            ut_failure("size != items[%d]->dlen (%zu != %zu)", i, size, items[i]->dlen);
            failed = 1;
            break;
        }
        if (strncmp(value, items[i]->data, items[i]->dlen) != 0) {
            ut_failure("%s != %s", items[i]->data, value);
            failed = 1;
            break;
        }
        shc_multi_item_destroy(items[i]);
        free(value);
    }
    if (!failed)
        ut_success();

    ut_testing("shardcache_client_getf(client, test_key200) == test_value200");
    int fd = shardcache_client_getf(client, "test_key200", 11);
    if (fd >= 0) {
        char buf[64];
        int rb = 0;
        int rs = 0;
        char p[64];
        while ((rb = read(fd, &buf, sizeof(buf))) > 0) {
            memcpy(p + (rs ? (rs - 1) : 0), buf, rb);
            rs += rb;
        }
        ut_validate_buffer(p, rs, "test_value200", 13);
        close(fd);
    } else {
        ut_failure("Can't obtain a valid fd from shardcache_client_getf()");
    }

    for (i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "test_key%d", 200+i);
        items[i] = shc_multi_item_create(key, strlen(key), NULL, 0);
    }
    items[10] = NULL; // null-terminate it

    failed = 0;
    ut_testing("shardcache_client_get_multif(c, items)");
    fd = shardcache_client_get_multif(client, items);

    if (fd >= 0) {
        int cnt = 0;
        int rb = 0;
        int rs = 0;
        int ofx = 0;
        uint32_t ls = 0;
        uint32_t idx = 0;
        char buf[64];
        char v[64];
        char p[1024];
        while ((rb = read(fd, &buf, sizeof(buf))) > 0) {
            memcpy(p + rs, buf, rb);
            rs += rb;
        }

        while (rs > ofx) {
            if (!ls && rs >= ofx + 12) {
                idx = ntohl(*((uint32_t *)(p + ofx)));
                ofx += sizeof(uint32_t);
                ls = ntohl(*((uint32_t *)(p + ofx)));
                ofx += sizeof(uint32_t);
            } else if (ls > 0 && rs >= ofx + ls) {
                char rv[64];
                snprintf(v, ls+1, "%s", p + ofx);
                sprintf(rv, "test_value%d", 200+idx);
                if (!v[0] || strncmp(v, rv, ls) != 0)
                { 
                    ut_failure("%s != %s", v, rv);
                    failed = 1;
                    break;
                }
                cnt++;
                ofx += ls;
                ls = 0;
            } else {
                break;
            }
        }
        if (!failed && cnt == 10)
            ut_success();

        close(fd);
    } else {
        ut_failure("Can't obtain a valid fd from shardcache_client_get_multif()");
    }

    for (i = 0; i < 10; i++)
        shc_multi_item_destroy(items[i]);

    char *volatile_key = "volatile_key";
    char *volatile_value = "volatile_value";

    ut_testing("setting volatile key");
    int rc = shardcache_client_set(client, volatile_key, strlen(volatile_key), volatile_value, strlen(volatile_value), 1);
    ut_validate_int(rc, 0);

    ut_testing("volatile key exists");
    size = shardcache_client_get(client, volatile_key, strlen(volatile_key), (void **)&value);
    ut_validate_buffer(value, size, volatile_value, strlen(volatile_value));
    free(value);

    sleep(3);

    ut_testing("volatile expired");
    size = shardcache_client_get(client, volatile_key, strlen(volatile_key), (void **)&value);
    ut_validate_int(size, 0);

    int64_t amount = 0; 
    // counter will start from 1 (if not present) and will be incremented of 2
    ut_testing("setting a counter: shardcache_increment(servers[0], counter, 7, 2, 1, 0, 0, NULL, NULL)");
    shardcache_increment(servers[0], "counter", 7, 2, 1, 0, 0, &amount, NULL, NULL);
    ut_validate_int(amount, 3);

    ut_testing("fetching the counter: shardcache_get_sync(servers[1], counter, 7, &v. &vlen, NULL) == 3");
    void *v = NULL;
    size_t vlen = 0;
    shardcache_get_sync(servers[1], "counter", 7, &v, &vlen, NULL);
    ut_validate_buffer(v, vlen, "3", 1);

    // counter will be incremented by 1 (counter++)
    ut_testing("incrementing the counter: shardcache_increment(servers[0], counter, 7, 1, 0, 0, 0, NULL, NULL)");
    shardcache_increment(servers[0], "counter", 7, 1, 0, 0, 0, &amount, NULL, NULL);
    ut_validate_int(amount, 4);

    ut_testing("fetching the counter again: shardcache_get_sync(servers[1], counter, 7, &v. &vlen, NULL) == 4");
    v = NULL;
    vlen = 0;
    shardcache_get_sync(servers[1], "counter", 7, &v, &vlen, NULL);
    ut_validate_buffer(v, vlen, "4", 1);


    // counter will be decremented by 1 (counter--)
    ut_testing("decrementing the counter: shardcache_decrement(servers[0], counter, 7, 1, 0, 0, 0, NULL, NULL)");
    shardcache_decrement(servers[0], "counter", 7, 1, 0, 0, 0, &amount, NULL, NULL);
    ut_validate_int(amount, 3);

    ut_testing("fetching the counter again: shardcache_get_sync(servers[1], counter, 7, &v. &vlen, NULL) == 3");
    v = NULL;
    vlen = 0;
    shardcache_get_sync(servers[1], "counter", 7, &v, &vlen, NULL);
    ut_validate_buffer(v, vlen, "3", 1);

    ut_testing("shardcache_client_increment(client, counter, 7, 1, 0, 0) == 4");
    int64_t k = 0;
    shardcache_client_increment(client, "counter", 7, 1, 0, &k, 0);
    ut_validate_int(k, 4);
    ut_testing("shardcache_client_decrement(client, counter, 7, 1, 0, 0) == 3");
    shardcache_client_decrement(client, "counter", 7, 1, 0, &k, 0);
    ut_validate_int(k, 3);


    shardcache_set_size(servers[0], 1 << 10);
    ut_testing("shardcache_set_workers_num(servers[0], 2) == -3");
    ut_validate_int(shardcache_set_workers_num(servers[0], 2), -3);
    shardcache_clear(servers[0]);

    ut_testing("destroying all clients");
    shardcache_client_destroy(client);
    shardcache_client_destroy(client1);
    shardcache_client_destroy(client2);
    ut_success();

    for (i = 0; i < num_nodes; i++) {
        ut_testing("destroying server %d", i);
        shardcache_destroy(servers[i]);
        ut_success();
    }

    free(nodes);

    ut_summary();
    exit(ut_failed);
}
