/* wb_midi_remote.c — MIDI remote control mapping.
 * Ableton MIDI remote script style: map CC numbers to track parameters.
 * Max 128 mappings (one per CC). JSON save/load. Normalized 0..1 value apply.
 *
 * Parameter mapping: 0=volume 1=pan 2=mute 3=solo 4=sendA 5=sendB
 */

#include "wbus.h"
#include "wbus_midi_remote.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define WB_MIDI_REMOTE_MAX_CC 128
#define WB_MIDI_REMOTE_MAX_PARAM 6

/* A single CC -> (track, param) mapping. */
typedef struct {
    int active;   /* 0 = slot free, 1 = mapped */
    int track;    /* target track index */
    int param;    /* target parameter (0..5) */
    float value;  /* last received normalized value (0..1) */
} wb_midi_remote_entry;

struct wb_midi_remote {
    wb_midi_remote_entry entries[WB_MIDI_REMOTE_MAX_CC];
};

wb_midi_remote *wb_midi_remote_create(void) {
    wb_midi_remote *mr = (wb_midi_remote *)calloc(1, sizeof(wb_midi_remote));
    return mr;  /* calloc zeroes all entries (active=0) */
}

void wb_midi_remote_destroy(wb_midi_remote *mr) {
    free(mr);
}

int wb_midi_remote_add_map(wb_midi_remote *mr, int cc, int track, int param) {
    if (!mr || cc < 0 || cc >= WB_MIDI_REMOTE_MAX_CC || param < 0 || param >= WB_MIDI_REMOTE_MAX_PARAM)
        return -1;
    mr->entries[cc].active = 1;
    mr->entries[cc].track = track;
    mr->entries[cc].param = param;
    mr->entries[cc].value = 0.0f;
    return 0;
}

int wb_midi_remote_remove_map(wb_midi_remote *mr, int cc) {
    if (!mr || cc < 0 || cc >= WB_MIDI_REMOTE_MAX_CC)
        return -1;
    if (!mr->entries[cc].active)
        return -1;
    mr->entries[cc].active = 0;
    mr->entries[cc].track = 0;
    mr->entries[cc].param = 0;
    mr->entries[cc].value = 0.0f;
    return 0;
}

int wb_midi_remote_process(wb_midi_remote *mr, int cc, float value) {
    if (!mr || cc < 0 || cc >= WB_MIDI_REMOTE_MAX_CC)
        return -1;
    if (!mr->entries[cc].active)
        return -1;
    /* Clamp value to 0..1 */
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;
    mr->entries[cc].value = value;
    /* In a full implementation, this would push to the engine's param bus.
     * For now we store the value; the consumer reads it via get_mapped_param
     * + the value field. Return 0 = mapping found and value applied. */
    return 0;
}

int wb_midi_remote_get_mapped_param(wb_midi_remote *mr, int cc, int *track, int *param) {
    if (!mr || cc < 0 || cc >= WB_MIDI_REMOTE_MAX_CC || !track || !param)
        return -1;
    if (!mr->entries[cc].active)
        return -1;
    *track = mr->entries[cc].track;
    *param = mr->entries[cc].param;
    return 0;
}

int wb_midi_remote_clear(wb_midi_remote *mr) {
    if (!mr) return -1;
    for (int i = 0; i < WB_MIDI_REMOTE_MAX_CC; i++) {
        mr->entries[i].active = 0;
        mr->entries[i].track = 0;
        mr->entries[i].param = 0;
        mr->entries[i].value = 0.0f;
    }
    return 0;
}

int wb_midi_remote_count(wb_midi_remote *mr) {
    if (!mr) return 0;
    int count = 0;
    for (int i = 0; i < WB_MIDI_REMOTE_MAX_CC; i++) {
        if (mr->entries[i].active) count++;
    }
    return count;
}

/* ---- JSON helpers (minimal, no third-party) ---------------------------- */

/* Skip whitespace. */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Parse a JSON integer value at *p. Advances *p past the number. */
static int parse_json_int(const char **p) {
    const char *s = *p;
    s = skip_ws(s);
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    int val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    *p = s;
    return val * sign;
}

/* Read a JSON string key: expects a quoted string. Returns 0 if matches expected. */
static int match_json_key(const char **p, const char *expected) {
    const char *s = *p;
    s = skip_ws(s);
    if (*s != '"') return 0;
    s++;
    const char *e = expected;
    while (*e && *s == *e) { e++; s++; }
    if (*e != '\0') return 0;
    if (*s != '"') return 0;
    s++;
    *p = s;
    return 1;
}

int wb_midi_remote_load(wb_midi_remote *mr, const char *path) {
    if (!mr || !path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 1024 * 1024) { fclose(f); return -1; }

    char *buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return -1; }
    size_t nread = fread(buf, 1, (size_t)fsize, f);
    buf[nread] = '\0';
    fclose(f);

    wb_midi_remote_clear(mr);

    const char *p = buf;
    p = skip_ws(p);
    if (*p != '{') { free(buf); return -1; }
    p++; /* skip { */
    p = skip_ws(p);

    /* Optional "mappings" key */
    if (match_json_key(&p, "mappings")) {
        p = skip_ws(p);
        if (*p != ':') { free(buf); return -1; }
        p++;
        p = skip_ws(p);
        if (*p != '[') { free(buf); return -1; }
        p++; /* skip [ */
        p = skip_ws(p);

        while (*p != ']' && *p != '\0') {
            p = skip_ws(p);
            if (*p != '{') break;
            p++; /* skip { */

            int cc = -1, track = -1, param = -1;
            for (;;) {
                p = skip_ws(p);
                if (*p == '}') { p++; break; }
                if (*p == ',') { p++; continue; }
                if (match_json_key(&p, "cc")) {
                    p = skip_ws(p); if (*p == ':') p++;
                    cc = parse_json_int(&p);
                } else if (match_json_key(&p, "track")) {
                    p = skip_ws(p); if (*p == ':') p++;
                    track = parse_json_int(&p);
                } else if (match_json_key(&p, "param")) {
                    p = skip_ws(p); if (*p == ':') p++;
                    param = parse_json_int(&p);
                } else {
                    /* skip unknown key */
                    p = skip_ws(p);
                    if (*p == '"') {
                        p++;
                        while (*p && *p != '"') p++;
                        if (*p == '"') p++;
                    }
                    p = skip_ws(p);
                    if (*p == ':') p++;
                    p = skip_ws(p);
                    /* skip value */
                    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
                    else { while (*p && *p != ',' && *p != '}' && *p != ']') p++; }
                }
            }
            if (cc >= 0 && cc < 128 && track >= 0 && param >= 0 && param < WB_MIDI_REMOTE_MAX_PARAM) {
                wb_midi_remote_add_map(mr, cc, track, param);
            }
            p = skip_ws(p);
            if (*p == ',') p++;
        }
    }

    free(buf);
    return 0;
}

int wb_midi_remote_save(wb_midi_remote *mr, const char *path) {
    if (!mr || !path) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "{\n  \"mappings\": [\n");
    int first = 1;
    for (int cc = 0; cc < WB_MIDI_REMOTE_MAX_CC; cc++) {
        if (!mr->entries[cc].active) continue;
        if (!first) fprintf(f, ",\n");
        fprintf(f, "    {\"cc\": %d, \"track\": %d, \"param\": %d}",
                cc, mr->entries[cc].track, mr->entries[cc].param);
        first = 0;
    }
    if (!first) fprintf(f, "\n");
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return 0;
}