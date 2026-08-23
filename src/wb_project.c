/* wb_project.c — G69: multiple timelines (sequences) per project.
 *
 * A project owns up to WB_MAX_SEQUENCES sessions; sequence 0 always exists.
 * Save format: a normal wbus_session 1.0 file per sequence inside one
 * container directory-free single file using explicit BEGIN/END markers:
 *
 *   wbus_project 1.0
 *   active <i>
 *   BEGIN_SEQUENCE "<name>"
 *   <full wbus_session body — everything wb_session_save writes>
 *   END_SEQUENCE
 *   ...
 *
 * Loading a legacy file (starts with "wbus_session") yields a one-sequence
 * project via wb_session_load. C11, opaque struct, stdlib only.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "wbus.h"

struct wb_project {
    wb_session *seq[WB_MAX_SEQUENCES];
    char        name[WB_MAX_SEQUENCES][64];
    int         count;
    int         active;
};

wb_project *wb_project_create(void) {
    wb_project *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->seq[0] = wb_session_create();
    if (!p->seq[0]) { free(p); return NULL; }
    snprintf(p->name[0], sizeof(p->name[0]), "Sequence 1");
    p->count = 1; p->active = 0;
    return p;
}

wb_project *wb_project_from_session(wb_session *owned) {
    if (!owned) return NULL;
    wb_project *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->seq[0] = owned;
    snprintf(p->name[0], sizeof(p->name[0]), "Sequence 1");
    p->count = 1; p->active = 0;
    return p;
}

void wb_project_destroy(wb_project *p) {
    if (!p) return;
    for (int i = 0; i < p->count; i++)
        if (p->seq[i]) wb_session_destroy(p->seq[i]);
    free(p);
}

int wb_project_sequence_count(const wb_project *p) { return p ? p->count : 0; }

wb_session *wb_project_sequence(const wb_project *p, int i) {
    if (!p || i < 0 || i >= p->count) return NULL;
    return p->seq[i];
}

wb_session *wb_project_active(const wb_project *p) {
    return p ? p->seq[p->active] : NULL;
}

int wb_project_active_index(const wb_project *p) { return p ? p->active : -1; }

int wb_project_add_sequence(wb_project *p, const char *name) {
    if (!p || p->count >= WB_MAX_SEQUENCES) return -1;
    wb_session *s = wb_session_create();
    if (!s) return -1;
    if (name && name[0])
        snprintf(s->name, sizeof(s->name), "%s", name);
    else
        snprintf(s->name, sizeof(s->name), "Sequence %d", p->count + 1);
    int idx = p->count++;
    p->seq[idx] = s;
    snprintf(p->name[idx], sizeof(p->name[idx]), "%s", s->name);
    return idx;
}

int wb_project_remove_sequence(wb_project *p, int i) {
    if (!p || i <= 0 || i >= p->count) return -1;   /* never remove 0 */
    wb_session_destroy(p->seq[i]);
    for (int k = i; k + 1 < p->count; k++) {
        p->seq[k]  = p->seq[k+1];
        memcpy(p->name[k], p->name[k+1], sizeof(p->name[0]));
    }
    p->count--;
    if (p->active >= p->count) p->active = p->count - 1;
    return 0;
}

int wb_project_set_active(wb_project *p, int i) {
    if (!p || i < 0 || i >= p->count) return -1;
    p->active = i;
    return 0;
}

/* ---- save / load -------------------------------------------------------- */

/* Serialize one session body. We reuse wb_session_save by writing to a temp
 * file and copying its bytes verbatim — the embedded "wbus_session 1.0"
 * magic line MUST be kept, wb_session_load requires it when parsing the
 * sequence back out of the project container. */
static int write_sequence_body(FILE *out, const wb_session *s) {
    const char *tmp = "/tmp/bigmac_proj_seq.wbus";
    if (wb_session_save(s, tmp) != 0) return -1;
    FILE *in = fopen(tmp, "r");
    if (!in) return -1;
    char line[1024];
    while (fgets(line, sizeof(line), in))
        fputs(line, out);
    fclose(in);
    remove(tmp);
    return 0;
}

int wb_project_save(const wb_project *p, const char *path) {
    if (!p || !path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "wbus_project 1.0\n");
    fprintf(f, "active %d\n", p->active);
    for (int i = 0; i < p->count; i++) {
        fprintf(f, "BEGIN_SEQUENCE \"%s\"\n", p->name[i]);
        if (write_sequence_body(f, p->seq[i]) != 0) { fclose(f); return -1; }
        fprintf(f, "END_SEQUENCE\n");
    }
    fclose(f);
    return 0;
}

wb_project *wb_project_load(const char *path) {
    if (!path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char magic[64] = {0};
    if (!fgets(magic, sizeof(magic), f)) { fclose(f); return NULL; }

    if (strncmp(magic, "wbus_session", 12) == 0) {
        /* legacy single-sequence file */
        fclose(f);
        wb_session *s = wb_session_load(path);
        if (!s) return NULL;
        return wb_project_from_session(s);
    }
    if (strncmp(magic, "wbus_project", 12) != 0) { fclose(f); return NULL; }

    wb_project *p = calloc(1, sizeof(*p));
    if (!p) { fclose(f); return NULL; }

    /* read whole rest of file into memory for simple scanning */
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, ftell(f) > 0 ? sz : 0, SEEK_SET);
    rewind(f);
    char *buf = malloc((size_t)(sz > 0 ? sz + 1 : 2));
    if (!buf) { free(p); fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = 0;
    fclose(f);

    /* parse active */
    char *cur = strstr(buf, "\nactive ");
    if (cur) p->active = atoi(cur + 8);

    /* split on BEGIN_SEQUENCE/END_SEQUENCE pairs; each inner body is a full
     * session minus its magic line — write to temp and use wb_session_load. */
    const char *const B = "/tmp/bigmac_proj_load_seq.wbus";
    char *pos = buf;
    while ((pos = strstr(pos, "BEGIN_SEQUENCE")) != NULL) {
        pos += strlen("BEGIN_SEQUENCE");
        /* name is the quoted token right after */
        char nm[64] = {0};
        const char *q1 = strchr(pos, '"');
        if (q1) {
            const char *q2 = strchr(q1 + 1, '"');
            if (q2 && (size_t)(q2 - q1 - 1) < sizeof(nm)) {
                memcpy(nm, q1 + 1, (size_t)(q2 - q1 - 1));
                pos = (char *)(q2 + 1);
            }
        }
        char *end = strstr(pos, "END_SEQUENCE");
        if (!end) break;
        long body_len = end - pos;
        /* find the session magic line inside the body */
        char *body = strstr(pos, "wbus_session");
        if (body && body < end) {
            FILE *tf = fopen(B, "w");
            if (!tf) break;
            fwrite(body, 1, (size_t)body_len, tf);
            fclose(tf);
            wb_session *s = wb_session_load(B);
            if (s && p->count < WB_MAX_SEQUENCES) {
                int idx = p->count++;
                p->seq[idx] = s;
                snprintf(p->name[idx], sizeof(p->name[idx]), "%s",
                         nm[0] ? nm : s->name);
            } else if (s) {
                wb_session_destroy(s);
            }
        }
        pos = end + strlen("END_SEQUENCE");
    }
    free(buf);
    remove(B);
    if (p->count == 0) { free(p); return NULL; }
    if (p->active < 0 || p->active >= p->count) p->active = 0;
    return p;
}
