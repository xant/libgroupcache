#include "hashtable.h"
#include "sqlite3.h"
#include "kepaxos.h"
#include "atomic.h"
#ifndef HAVE_UINT64_T
#define HAVE_UINT64_T
#endif
#include <siphash.h>

#include <stdio.h>
#include <arpa/inet.h>

#define MAX(a, b) ( (a) > (b) ? (a) : (b) )
#define MIN(a, b) ( (a) < (b) ? (a) : (b) )

typedef enum {
    KEPAXOS_CMD_STATUS_NONE=0,
    KEPAXOS_CMD_STATUS_PRE_ACCEPTED,
    KEPAXOS_CMD_STATUS_ACCEPTED,
    KEPAXOS_CMD_STATUS_COMMITTED,
} kepaxos_cmd_status_t;

typedef enum {
    KEPAXOS_MSG_TYPE_PRE_ACCEPT,
    KEPAXOS_MSG_TYPE_PRE_ACCEPT_RESPONSE,
    KEPAXOS_MSG_TYPE_ACCEPT,
    KEPAXOS_MSG_TYPE_ACCEPT_RESPONSE,
    KEPAXOS_MSG_TYPE_COMMIT,
} kepaxos_msg_type_t;

typedef struct {
    char *peer;
    uint32_t ballot;
    void *key;
    size_t klen;
    uint32_t seq;
} kepaxos_vote_t;

struct __kepaxos_cmd_s {
    kepaxos_cmd_type_t type;
    kepaxos_msg_type_t msg;
    kepaxos_cmd_status_t status;
    uint32_t seq;
    void *key;
    size_t klen;
    void *data;
    size_t dlen;
    kepaxos_vote_t *votes;
    uint16_t num_votes;
    uint32_t max_seq;
    char *max_voter;
    uint32_t ballot;
};

struct __kepaxos_s {
    sqlite3 *log;
    hashtable_t *commands; // key => cmd 
    char **peers;
    int num_peers;
    unsigned char my_index;
    kepaxos_callbacks_t callbacks;
    pthread_mutex_t lock;
    uint32_t ballot;
    sqlite3_stmt *select_stmt;
    sqlite3_stmt *insert_stmt;
};

static void kepaxos_command_destroy(kepaxos_cmd_t *c)
{
    free(c->key);
    if (c->data)
        free(c->data);
    free(c);
}

kepaxos_t *
kepaxos_context_create(char *dbfile, char **peers, int num_peers, kepaxos_callbacks_t *callbacks)
{
    kepaxos_t *ke = calloc(1, sizeof(kepaxos_t));

    int rc = sqlite3_open(dbfile, &ke->log);
    if (rc != SQLITE_OK) {
        // TODO - Error messages
        free(ke);
        return NULL;
    }

    const char *create_table_sql = "CREATE TABLE IF NOT EXISTS ReplicaLog (ballot int, keyhash1 int, keyhash2 int, seq int, PRIMARY KEY(keyhash1, keyhash2))";
    rc = sqlite3_exec(ke->log, create_table_sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        // TODO - Errors
        sqlite3_close(ke->log);
        free(ke);
    }

    char sql[2048];
    snprintf(sql, sizeof(sql), "SELECT MAX(seq) FROM ReplicaLog WHERE keyhash1=? AND keyhash2=?");
    const char *tail = NULL;
    rc = sqlite3_prepare_v2(ke->log, sql, sizeof(sql), &ke->select_stmt, &tail);
    if (rc != SQLITE_OK) {
        // TODO - Errors
    }

    snprintf(sql, sizeof(sql), "INSERT OR REPLACE INTO ReplicaLog VALUES(?, ?, ?, ?)");
    rc = sqlite3_prepare_v2(ke->log, sql, sizeof(sql), &ke->insert_stmt, &tail);
    if (rc != SQLITE_OK) {
        // TODO - Errors
    }


    ke->peers = malloc(sizeof(char *) * num_peers);
    int i;
    for (i = 0; i < num_peers; i++)
        ke->peers[i] = strdup(peers[i]);

    if (callbacks)
        memcpy(&ke->callbacks, callbacks, sizeof(kepaxos_callbacks_t));

    ke->commands = ht_create(128, 1024, (ht_free_item_callback_t)kepaxos_command_destroy);

    MUTEX_INIT(&ke->lock);

    return ke;
}

void
kepaxos_context_destroy(kepaxos_t *ke)
{
    sqlite3_finalize(ke->select_stmt);
    sqlite3_finalize(ke->insert_stmt);
    sqlite3_close(ke->log);
    int i;
    for (i = 0; i < ke->num_peers; i++)
        free(ke->peers[i]);
    free(ke->peers);
    ht_destroy(ke->commands);
    MUTEX_DESTROY(&ke->lock);
    free(ke);
}

static void
kepaxos_compute_key_hashes(void *key, size_t klen, uint64_t *hash1, uint64_t *hash2)
{
    unsigned char auth1[16] = "0123456789ABCDEF";
    unsigned char auth2[16] = "ABCDEF0987654321";

    *hash1 = sip_hash24(auth1, key, klen);
    *hash2 = sip_hash24(auth2, key, klen);
}

static uint32_t
last_seq_for_key(kepaxos_t *ke, void *key, size_t klen)
{
    uint64_t keyhash1, keyhash2;
    uint64_t seq = 0;

    int rc = sqlite3_reset(ke->select_stmt);

    kepaxos_compute_key_hashes(key, klen, &keyhash1, &keyhash2);

    rc = sqlite3_bind_int64(ke->select_stmt, 1, keyhash1);
    if (rc != SQLITE_OK) {
        // TODO - Errors
    }

    rc = sqlite3_bind_int64(ke->select_stmt, 2, keyhash2);
    if (rc != SQLITE_OK) {
        // TODO - Errors
    }

    //int cnt = 0;
    rc = sqlite3_step(ke->select_stmt);
    if (rc == SQLITE_ROW)
        seq = sqlite3_column_int64(ke->select_stmt, 0);

    return seq;
}

static void
set_last_seq_for_key(kepaxos_t *ke, void *key, size_t klen, uint32_t ballot, uint32_t seq)
{
    uint64_t keyhash1, keyhash2;
    //uint64_t last_seq = 0;

    int rc = sqlite3_reset(ke->insert_stmt);

    kepaxos_compute_key_hashes(key, klen, &keyhash1, &keyhash2);

    rc = sqlite3_bind_int(ke->insert_stmt, 1, ballot);
    if (rc != SQLITE_OK) {
        // TODO - Errors
    }

    rc = sqlite3_bind_int64(ke->insert_stmt, 2, keyhash1);
    if (rc != SQLITE_OK) {
        // TODO - Errors
    }

    rc = sqlite3_bind_int64(ke->insert_stmt, 3, keyhash2);
    if (rc != SQLITE_OK) {
        // TODO - Errors
    }

    rc = sqlite3_bind_int(ke->insert_stmt, 4, seq);
    if (rc != SQLITE_OK) {
        // TODO - Errors
    }

    rc = sqlite3_step(ke->insert_stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Insert Failed! %d\n", rc);
        return;
    }


 
}


static size_t
kepaxos_build_message(char **out,
                      kepaxos_msg_type_t mtype,
                      kepaxos_cmd_type_t ctype, 
                      uint32_t ballot,
                      void *key,
                      uint32_t klen,
                      void *data,
                      uint32_t dlen,
                      uint32_t seq,
                      int committed)
{
    size_t msglen = klen + dlen + 3 + sizeof(uint32_t) * 4;
    char *msg = malloc(msglen);
    unsigned char committed_byte = committed ? 1 : 0;
    unsigned char mtype_byte = (unsigned char)mtype;
    unsigned char ctype_byte = (unsigned char)ctype;
    uint32_t nbo = htonl(ballot);
    memcpy(msg, &nbo, sizeof(uint32_t));

    nbo = htonl(seq);
    memcpy(msg + sizeof(uint32_t), &nbo, sizeof(uint32_t));

    memcpy(msg + (2*sizeof(uint32_t)), &mtype_byte, 1);
    memcpy(msg + (2*sizeof(uint32_t)) + 1, &ctype_byte, 1);
    memcpy(msg + (2*sizeof(uint32_t)) + 2, &committed_byte, 1);

    nbo = htonl(klen);
    memcpy(msg + (2*sizeof(uint32_t)) + 3, &nbo, sizeof(uint32_t));
    memcpy(msg + (3*sizeof(uint32_t)) + 3, key, klen);

    nbo = htonl(dlen);
    memcpy(msg + (3*sizeof(uint32_t)) + 3 + klen, &nbo, sizeof(uint32_t));
    memcpy(msg + (4*sizeof(uint32_t)) + 3 + klen, data, dlen);

    *out = msg;
    return msglen;
}

static int
kepaxos_send_preaccept(kepaxos_t *ke, uint32_t ballot, void *key, size_t klen, uint32_t seq)
{
    char *receivers[ke->num_peers-1];
    int i, n = 0;
    for (i = 0; i < ke->num_peers; i++) {
        if (i == ke->my_index)
            continue;
        receivers[n++] = ke->peers[i];
    }

    char *msg = NULL;
    size_t msglen = kepaxos_build_message(&msg, KEPAXOS_MSG_TYPE_PRE_ACCEPT, 0, ballot, key, klen, NULL, 0, seq, 0);
    int rc = ke->callbacks.send(receivers, ke->num_peers-1, (void *)msg, msglen);
    free(msg);
    return rc;
}

int
kepaxos_run_command(kepaxos_t *ke,
                    char *peer,
                    kepaxos_cmd_type_t type,
                    void *key,
                    size_t klen,
                    void *data)
{
    // Replica R1 receives a new set/del/evict request for key K
    MUTEX_LOCK(&ke->lock);
    uint32_t seq = last_seq_for_key(ke, key, klen);
    kepaxos_cmd_t *cmd = (kepaxos_cmd_t *)ht_get(ke->commands, key, klen, NULL);
    if (!cmd) {
        cmd = calloc(1, sizeof(kepaxos_cmd_t));
        ht_set(ke->commands, key, klen, cmd, sizeof(kepaxos_cmd_t));
    }
    uint32_t interfering_seq = cmd ? cmd->seq : 0; 
    seq = MAX(seq, interfering_seq);
    // an eventually uncommitted command for K would be overwritten here
    // hence it will be ignored and will fail silently
    // (NOTE: in libshardcache we only care about the most recent command for a key 
    //        and not about the entire sequence of commands)
    cmd->seq = seq;
    cmd->type = type;
    cmd->key = malloc(klen);
    memcpy(cmd->key, key, klen);
    cmd->klen = klen;
    if (cmd->data)
        free(cmd->data);
    cmd->data = data;
    cmd->status = KEPAXOS_CMD_STATUS_PRE_ACCEPTED;
    uint32_t ballot = (ATOMIC_READ(ke->ballot)|0xFFFFFF00) >> 1;
    ballot++;
    ballot = (ballot << 1) | ke->my_index;
    MUTEX_UNLOCK(&ke->lock);
    return kepaxos_send_preaccept(ke, ballot, key, klen, seq);
}

static int
kepaxos_send_pre_accept_response(kepaxos_t *ke,
                                 char *peer,
                                 uint32_t ballot,
                                 void *key,
                                 size_t klen,
                                 uint32_t seq,
                                 unsigned char committed)
{
    char *msg = NULL;
    size_t msglen = kepaxos_build_message(&msg, KEPAXOS_MSG_TYPE_PRE_ACCEPT_RESPONSE, 0, ballot, key, klen, NULL, 0, seq, committed);
    int rc = ke->callbacks.send(&peer, 1, (void *)msg, msglen);
    free(msg);
    return rc;
}

static int
kepaxos_send_commit(kepaxos_t *ke, kepaxos_cmd_t *cmd)
{
    char *receivers[ke->num_peers-1];
    int i, n = 0;
    for (i = 0; i < ke->num_peers; i++) {
        if (i == ke->my_index)
            continue;
        receivers[n++] = ke->peers[i];
    }

    char *msg = NULL;
    size_t msglen = kepaxos_build_message(&msg,
                                          KEPAXOS_MSG_TYPE_COMMIT,
                                          cmd->type,
                                          cmd->ballot,
                                          cmd->key,
                                          cmd->klen,
                                          cmd->data,
                                          cmd->dlen,
                                          cmd->seq, 1);

    int rc =  ke->callbacks.send(receivers, ke->num_peers-1, (void *)msg, msglen);
    free(msg);
    return rc;
}

static int
kepaxos_commit(kepaxos_t *ke, kepaxos_cmd_t *cmd)
{
    ke->callbacks.commit(cmd->type, cmd->key, cmd->klen, cmd->data, cmd->dlen);
    set_last_seq_for_key(ke, cmd->key, cmd->klen, cmd->ballot, cmd->seq);
    return kepaxos_send_commit(ke, cmd);
}

static int
kepaxos_send_accept(kepaxos_t *ke, uint32_t ballot, void *key, size_t klen, uint32_t seq)
{
    char *receivers[ke->num_peers-1];
    int i, n = 0;
    for (i = 0; i < ke->num_peers; i++) {
        if (i == ke->my_index)
            continue;
        receivers[n++] = ke->peers[i];
    }

    char *msg = NULL;
    size_t msglen = kepaxos_build_message(&msg, KEPAXOS_MSG_TYPE_ACCEPT, 0, ballot, key, klen, NULL, 0, seq, 0);
    int rc = ke->callbacks.send(receivers, ke->num_peers-1, (void *)msg, msglen);
    free(msg);
    return rc;
}

static int
kepaxos_send_accept_response(kepaxos_t *ke,
                                 char *peer,
                                 uint32_t ballot,
                                 void *key,
                                 size_t klen,
                                 uint32_t seq,
                                 unsigned char committed)
{
    char *msg = NULL;
    size_t msglen = kepaxos_build_message(&msg, KEPAXOS_MSG_TYPE_ACCEPT_RESPONSE, 0, ballot, key, klen, NULL, 0, seq, committed);
    int rc = ke->callbacks.send(&peer, 1, (void *)msg, msglen);
    free(msg);
    return rc;
}

int
kepaxos_received_command(kepaxos_t *ke, char *peer, void *cmd, size_t cmdlen)
{
    if (cmdlen < sizeof(uint32_t) * 4)
        return -1;

    // parse the message
    char *p = cmd;

    uint32_t ballot = ntohl(*((uint32_t *)p));
    p += sizeof(uint32_t);

    uint32_t seq = ntohl(*((uint32_t *)p));
    p += sizeof(uint32_t);

    unsigned char mtype = *p++;
    unsigned char ctype = *p++;
    unsigned char committed = *p++;

    uint32_t klen = ntohl(*((uint32_t *)p));
    p += sizeof(uint32_t);
    void *key = p;
    p += klen;

    uint32_t dlen = ntohl(*((uint32_t *)p));
    p += sizeof(uint32_t);
    void *data = dlen ? p : NULL;
    // done with parsing

    // update the ballot if the current ballot number is bigger
    uint32_t updated_ballot = (ballot&0xFFFFFF00) >> 1;
    updated_ballot++;
    ATOMIC_SET_IF(ke->ballot, <, (updated_ballot << 1) | ke->my_index, uint32_t);

    switch(mtype) {
        case KEPAXOS_MSG_TYPE_PRE_ACCEPT:
        {
            // Any replica R receiving a PRE_ACCEPT(BALLOT, K, SEQ) from R1
            MUTEX_LOCK(&ke->lock);
            uint32_t local_seq = last_seq_for_key(ke, key, klen);
            kepaxos_cmd_t *cmd = (kepaxos_cmd_t *)ht_get(ke->commands, key, klen, NULL);
            uint32_t interfering_seq = 0;
            if (cmd) {
                if (ballot < cmd->ballot) {
                    // ignore this message ... the ballot is too old
                    MUTEX_UNLOCK(&ke->lock);
                    return -1;
                }
                cmd->ballot = MAX(ballot, cmd->ballot);
                interfering_seq = cmd->seq;
            }
            interfering_seq = MAX(local_seq, interfering_seq);
            uint32_t max_seq = MAX(seq, interfering_seq);
            if (max_seq == seq)
                cmd->status = KEPAXOS_CMD_STATUS_PRE_ACCEPTED;
            committed = (max_seq == local_seq);
            MUTEX_UNLOCK(&ke->lock);
            return kepaxos_send_pre_accept_response(ke, peer, ATOMIC_READ(ke->ballot), key, klen, max_seq, (int)committed);
            break;
        }
        case KEPAXOS_MSG_TYPE_PRE_ACCEPT_RESPONSE:
        {
            MUTEX_LOCK(&ke->lock);
            kepaxos_cmd_t *cmd = (kepaxos_cmd_t *)ht_get(ke->commands, key, klen, NULL);
            if (cmd) {
                if (ballot < cmd->ballot) {
                    MUTEX_UNLOCK(&ke->lock);
                    return -1;
                }
                if (cmd->status != KEPAXOS_CMD_STATUS_PRE_ACCEPTED) {
                    MUTEX_UNLOCK(&ke->lock);
                    return -1;
                }
                cmd->votes = realloc(cmd->votes, sizeof(kepaxos_vote_t) * ++cmd->num_votes);
                cmd->votes[cmd->num_votes].seq = seq;
                cmd->votes[cmd->num_votes].ballot = ballot;
                cmd->votes[cmd->num_votes].peer = peer;
                cmd->max_seq = MAX(cmd->max_seq, seq);
                if (cmd->max_seq == seq)
                    cmd->max_voter = peer;
                if (cmd->num_votes < ke->num_peers/2)
                    return 0; // we don't have a quorum yet
                if (cmd->seq >= cmd->max_seq) {
                    // commit (short path)
                    ht_delete(ke->commands, key, klen, (void **)&cmd, NULL);
                    MUTEX_UNLOCK(&ke->lock);
                    return kepaxos_commit(ke, cmd);
                } else {
                    if (committed) {
                        ke->callbacks.recover(cmd->max_voter, key, klen);
                    } else {
                        // run the paxos-like protocol (long path)
                        free(cmd->votes);
                        cmd->votes = NULL;
                        cmd->num_votes = 0;
                        cmd->seq = cmd->max_seq + 1;
                        cmd->max_seq = 0;
                        cmd->max_voter = NULL;
                        uint32_t new_seq = cmd->seq;
                        MUTEX_UNLOCK(&ke->lock);
                        return kepaxos_send_accept(ke, ballot, key, klen, new_seq);
                    }
                }
            }
            MUTEX_UNLOCK(&ke->lock);
            break;
        }
        case KEPAXOS_MSG_TYPE_ACCEPT:
        {
            // Any replica R receiving an ACCEPT(BALLOT, K, SEQ) from R1
            int accepted_ballot = ballot;
            int accepted_seq = seq;
            MUTEX_LOCK(&ke->lock);
            kepaxos_cmd_t *cmd = (kepaxos_cmd_t *)ht_get(ke->commands, key, klen, NULL);
            if (cmd) {
                if (ballot < cmd->ballot) {
                    // ignore this message
                    MUTEX_UNLOCK(&ke->lock);
                    return 0;
                }
                if (seq < cmd->seq) {
                    accepted_ballot = cmd->ballot;
                    accepted_seq = cmd->seq;
                }
            } else {
                cmd = calloc(1, sizeof(kepaxos_cmd_t));
                cmd->key = malloc(klen);
                memcpy(cmd->key, key, klen);
                cmd->klen = klen;
                ht_set(ke->commands, key, klen, cmd, sizeof(kepaxos_cmd_t));
            }
            if (seq >= cmd->seq) {
                cmd->seq = seq;
                cmd->ballot = ballot;
                cmd->status = KEPAXOS_CMD_STATUS_ACCEPTED;
                accepted_ballot = ballot;
                accepted_seq = seq;
            }
            MUTEX_UNLOCK(&ke->lock);
            return kepaxos_send_accept_response(ke, peer, accepted_ballot, key, klen, accepted_seq, 0);
            break;
        }
        case KEPAXOS_MSG_TYPE_ACCEPT_RESPONSE:
        {
            MUTEX_LOCK(&ke->lock);
            kepaxos_cmd_t *cmd = (kepaxos_cmd_t *)ht_get(ke->commands, key, klen, NULL);
            if (cmd) {
                if (ballot < cmd->ballot) {
                    MUTEX_UNLOCK(&ke->lock);
                    return -1;
                }
                if (cmd->status != KEPAXOS_CMD_STATUS_ACCEPTED) {
                    MUTEX_UNLOCK(&ke->lock);
                    return -1;
                }
                cmd->votes = realloc(cmd->votes, sizeof(kepaxos_vote_t) * ++cmd->num_votes);
                cmd->votes[cmd->num_votes].seq = seq;
                cmd->votes[cmd->num_votes].ballot = ballot;
                cmd->votes[cmd->num_votes].peer = peer;
                cmd->max_seq = MAX(cmd->max_seq, seq);
                if (cmd->max_seq == seq)
                    cmd->max_voter = peer;
                int i;
                int count_ok = 0;
                for (i = 0; i < cmd->num_votes; i++)
                    if (cmd->votes[i].seq == seq && cmd->votes[i].ballot == ballot)
                        count_ok++;

                if (count_ok < ke->num_peers/2) {
                    if (cmd->num_votes >= ke->num_peers/2) {
                        // we need to retry paxos increasing the ballot number

                        if (cmd->seq <= cmd->max_seq)
                            cmd->seq++;

                        uint32_t new_ballot = ATOMIC_READ(ke->ballot);
                        cmd->ballot = new_ballot;
                        free(cmd->votes);
                        cmd->votes = NULL;
                        cmd->num_votes = 0;
                        cmd->max_seq = 0;
                        cmd->max_voter = NULL;
                        uint32_t new_seq = cmd->seq;
                        MUTEX_UNLOCK(&ke->lock);
                        return kepaxos_send_accept(ke, new_ballot, key, klen, new_seq);
                    }
                    MUTEX_UNLOCK(&ke->lock);
                    return 0; // we don't have a quorum yet
                }
                // the command has been accepted by a quorum
                ht_delete(ke->commands, key, klen, (void **)&cmd, NULL);
                MUTEX_UNLOCK(&ke->lock);
                return kepaxos_commit(ke, cmd);
            }
            break;
        }
        case KEPAXOS_MSG_TYPE_COMMIT:
        {
            MUTEX_LOCK(&ke->lock);
            // Any replica R on receiving a COMMIT(BALLOT, K, SEQ, CMD, DATA) message
            kepaxos_cmd_t *cmd = (kepaxos_cmd_t *)ht_get(ke->commands, key, klen, NULL);
            if (cmd && cmd->seq == seq && cmd->ballot > ballot) {
                // ignore this message ... the ballot is too old
                MUTEX_UNLOCK(&ke->lock);
                return -1;
            }
            uint32_t last_recorded_seq = last_seq_for_key(ke, key, klen);
            if (seq < last_recorded_seq) {
                // ignore this commit message (it's too old)
                MUTEX_UNLOCK(&ke->lock);
                return 0;
            }
            ke->callbacks.commit(ctype, key, klen, data, dlen);
            set_last_seq_for_key(ke, key, klen, ballot, seq);
            if (cmd && cmd->seq == seq)
                ht_delete(ke->commands, key, klen, NULL, NULL);
            MUTEX_UNLOCK(&ke->lock);
            break;
        }
        default:
            break;
    }
    return 0;
}
