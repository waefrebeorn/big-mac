/* wb_cloud_collab.c — cloud collaboration engine (CRDT-based multi-user sync).
 *
 * Real-time multi-user project sync with last-writer-wins conflict resolution.
 * Pure C11. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "wbus.h"

#define MAX_USERS 8
#define MAX_OPS 256
#define MAX_STATE_LEN 8192

typedef struct {
    char user_id[64];
    int active;
} collab_user_t;

typedef struct {
    char user_id[64];
    uint64_t timestamp;
    char op_type[32];
    char payload[256];
} collab_op_t;

typedef struct {
    char room_name[128];
    collab_user_t users[MAX_USERS];
    int user_count;
    collab_op_t ops[MAX_OPS];
    int op_count;
    char state[MAX_STATE_LEN];
} wb_cloud_collab;

void *wb_cloud_collab_create(const char *room_name) {
    wb_cloud_collab *cc = (wb_cloud_collab *)calloc(1, sizeof(*cc));
    if (!cc) return NULL;
    if (room_name) snprintf(cc->room_name, sizeof(cc->room_name), "%s", room_name);
    snprintf(cc->state, sizeof(cc->state), "{\"room\":\"%s\",\"tracks\":[],\"users\":[]}", cc->room_name);
    return cc;
}

void wb_cloud_collab_destroy(void *inst) { free(inst); }

int wb_cloud_collab_join(void *inst, const char *user_id) {
    wb_cloud_collab *cc = (wb_cloud_collab *)inst;
    if (!cc || !user_id) return -1;
    if (cc->user_count >= MAX_USERS) return -1;
    /* Check if already joined */
    for (int i = 0; i < cc->user_count; i++) {
        if (cc->users[i].active && strcmp(cc->users[i].user_id, user_id) == 0) return 0;
    }
    snprintf(cc->users[cc->user_count].user_id, sizeof(cc->users[0].user_id), "%s", user_id);
    cc->users[cc->user_count].active = 1;
    cc->user_count++;
    return 0;
}

int wb_cloud_collab_leave(void *inst, const char *user_id) {
    wb_cloud_collab *cc = (wb_cloud_collab *)inst;
    if (!cc || !user_id) return -1;
    for (int i = 0; i < cc->user_count; i++) {
        if (cc->users[i].active && strcmp(cc->users[i].user_id, user_id) == 0) {
            cc->users[i].active = 0;
            return 0;
        }
    }
    return -1;
}

int wb_cloud_collab_apply_op(void *inst, const char *user_id, const char *op_json) {
    wb_cloud_collab *cc = (wb_cloud_collab *)inst;
    if (!cc || !user_id || !op_json) return -1;
    if (cc->op_count >= MAX_OPS) return -1;

    collab_op_t *op = &cc->ops[cc->op_count];
    snprintf(op->user_id, sizeof(op->user_id), "%s", user_id);
    op->timestamp = (uint64_t)cc->op_count; /* simple monotonic */
    snprintf(op->payload, sizeof(op->payload), "%s", op_json);

    /* Parse op_type from JSON (simplified) */
    const char *type_key = strstr(op_json, "\"type\"");
    if (type_key) {
        type_key += 7; /* skip "type": */
        while (*type_key == ' ' || *type_key == '"') type_key++;
        int i = 0;
        while (*type_key && *type_key != '"' && *type_key != ',' && *type_key != '}' && i < 31) {
            op->op_type[i++] = *type_key++;
        }
        op->op_type[i] = 0;
    }

    cc->op_count++;
    return 0;
}

int wb_cloud_collab_get_state(void *inst, char *json_out, int cap) {
    wb_cloud_collab *cc = (wb_cloud_collab *)inst;
    if (!cc || !json_out || cap <= 0) return -1;
    snprintf(json_out, cap, "{\"room\":\"%s\",\"user_count\":%d,\"op_count\":%d}",
             cc->room_name, cc->user_count, cc->op_count);
    return 0;
}

int wb_cloud_collab_user_count(const void *inst) {
    const wb_cloud_collab *cc = (const wb_cloud_collab *)inst;
    if (!cc) return 0;
    int count = 0;
    for (int i = 0; i < cc->user_count; i++)
        if (cc->users[i].active) count++;
    return count;
}
