/* wb_undo.c — undo/redo via session snapshots.
 *
 * Before a mutation, the caller pushes the current session onto the stack.
 * undo() restores the previous session (the engine re-binds to the new one);
 * redo() walks forward again. Bounded depth to cap memory.
 *
 * The snapshots are deep copies (wb_session_copy) so the engine's bound
 * session and the history are fully independent.
 */

#include <stdlib.h>
#include <string.h>

#include "wbus.h"

#define WB_UNDO_MAX 64

struct wb_undo {
    wb_session **undo_stack;   /* index 0 = oldest */
    int undo_depth;
    int undo_head;             /* next free slot (circular) */
    wb_session **redo_stack;
    int redo_depth;
    int redo_head;
};

/* Replace `*owner` with `*replacement`: destroy owner, take ownership of
 * replacement. Returns the new session. */
static wb_session *replace_session(wb_session **owner, wb_session *replacement) {
    if (*owner) wb_session_destroy(*owner);
    *owner = replacement;
    return replacement;
}

wb_undo *wb_undo_create(void) {
    wb_undo *u = calloc(1, sizeof(*u));
    if (!u) return NULL;
    u->undo_stack = calloc(WB_UNDO_MAX, sizeof(void*));
    u->redo_stack = calloc(WB_UNDO_MAX, sizeof(void*));
    if (!u->undo_stack || !u->redo_stack) {
        free(u->undo_stack); free(u->redo_stack); free(u);
        return NULL;
    }
    return u;
}

void wb_undo_destroy(wb_undo *u) {
    if (!u) return;
    for (int i = 0; i < u->undo_depth; i++)
        wb_session_destroy(u->undo_stack[i]);
    for (int i = 0; i < u->redo_depth; i++)
        wb_session_destroy(u->redo_stack[i]);
    free(u->undo_stack); free(u->redo_stack); free(u);
}

/* Record the current session as an undo checkpoint. Clears the redo branch. */
void wb_undo_checkpoint(wb_undo *u, const wb_session *current) {
    if (!u || !current) return;
    wb_session *snap = wb_session_copy(current);
    if (!snap) return;
    /* clear redo branch (new edit invalidates redo) */
    for (int i = 0; i < u->redo_depth; i++)
        wb_session_destroy(u->redo_stack[i]);
    u->redo_depth = 0;

    if (u->undo_depth < WB_UNDO_MAX) {
        u->undo_stack[u->undo_depth++] = snap;
    } else {
        /* drop oldest to keep bounded */
        wb_session_destroy(u->undo_stack[0]);
        memmove(&u->undo_stack[0], &u->undo_stack[1], (WB_UNDO_MAX-1) * sizeof(void*));
        u->undo_stack[WB_UNDO_MAX-1] = snap;
    }
}

/* Undo: restore the last checkpoint into `*owner`. Returns 1 if undone,
 * 0 if there is nothing to undo. */
int wb_undo_undo(wb_undo *u, wb_session **owner) {
    if (!u || !owner || u->undo_depth == 0) return 0;
    wb_session *prev = u->undo_stack[u->undo_depth - 1];
    u->undo_depth--;
    /* push the current session onto the redo stack */
    if (*owner) {
        wb_session *cur_snap = wb_session_copy(*owner);
        if (cur_snap) {
            if (u->redo_depth < WB_UNDO_MAX) u->redo_stack[u->redo_depth++] = cur_snap;
            else { wb_session_destroy(u->redo_stack[0]);
                   memmove(&u->redo_stack[0], &u->redo_stack[1], (WB_UNDO_MAX-1)*sizeof(void*));
                   u->redo_stack[WB_UNDO_MAX-1] = cur_snap; }
        }
    }
    replace_session(owner, prev);
    return 1;
}

/* Redo: restore the next redo session into `*owner`. Returns 1 if redone,
 * 0 if there is nothing to redo. */
int wb_undo_redo(wb_undo *u, wb_session **owner) {
    if (!u || !owner || u->redo_depth == 0) return 0;
    wb_session *next = u->redo_stack[u->redo_depth - 1];
    u->redo_depth--;
    if (*owner) {
        wb_session *cur_snap = wb_session_copy(*owner);
        if (cur_snap) {
            if (u->undo_depth < WB_UNDO_MAX) u->undo_stack[u->undo_depth++] = cur_snap;
            else { wb_session_destroy(u->undo_stack[0]);
                   memmove(&u->undo_stack[0], &u->undo_stack[1], (WB_UNDO_MAX-1)*sizeof(void*));
                   u->undo_stack[WB_UNDO_MAX-1] = cur_snap; }
        }
    }
    replace_session(owner, next);
    return 1;
}

int wb_undo_depth(const wb_undo *u) { return u ? u->undo_depth : 0; }
int wb_undo_redo_depth(const wb_undo *u) { return u ? u->redo_depth : 0; }
