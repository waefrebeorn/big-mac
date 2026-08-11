/*
 * wb_print.c — voice-print JSON save/load (strict C11, our own tiny JSON)
 */
#include "wb_print.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int wb_print_save(const char *path, const wb_voiceprint_t *vp) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "{\n");
    fprintf(f, "  \"name\": \"%s\",\n", vp->name);
    fprintf(f, "  \"f0\": { \"mean\": %.2f, \"min\": %.2f, \"max\": %.2f, \"sd\": %.2f, "
               "\"vibrato_rate\": %.3f, \"vibrato_depth\": %.2f, \"voiced\": %.3f },\n",
            vp->f0.f0_mean, vp->f0.f0_min, vp->f0.f0_max, vp->f0.f0_sd,
            vp->f0.vibrato_rate, vp->f0.vibrato_depth, vp->f0.voiced_fraction);
    fprintf(f, "  \"formants\": { \"n\": %d, \"F\": [", vp->formants.n);
    for (int i = 0; i < vp->formants.n; i++) {
        fprintf(f, "%s%.1f", i ? ", " : "", vp->formants.F[i]);
    }
    fprintf(f, "], \"BW\": [");
    for (int i = 0; i < vp->formants.n; i++) {
        fprintf(f, "%s%.1f", i ? ", " : "", vp->formants.BW[i]);
    }
    fprintf(f, "] },\n");
    fprintf(f, "  \"quality\": { \"jitter\": %.4f, \"shimmer\": %.4f, \"hnr\": %.2f, "
               "\"cpp\": %.3f, \"h1h2\": %.2f, \"tilt\": %.3f },\n",
            vp->quality.jitter_pct, vp->quality.shimmer_pct, vp->quality.hnr_db,
            vp->quality.cpp, vp->quality.h1h2_db, vp->quality.tilt_db_per_oct);
    fprintf(f, "  \"f0_render\": %.2f,\n", vp->f0_render);
    fprintf(f, "  \"diameters\": [");
    for (int i = 0; i < vp->n_diameters; i++) {
        fprintf(f, "%s%.4f", i ? ", " : "", vp->diameters[i]);
    }
    fprintf(f, "]\n}\n");
    fclose(f);
    return 0;
}

/* ---- minimal JSON parser (no third party) ---- */
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *find_key(const char *json, const char *key, const char **val_out) {
    /* find "key": ... */
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strstr(p, key)) != NULL) {
        /* check it's a quoted key followed by colon */
        const char *q = p + klen;
        q = skip_ws(q);
        if (*q == ':') {
            q = skip_ws(q + 1);
            *val_out = q;
            return p;
        }
        p = p + klen;
    }
    return NULL;
}

static double parse_num(const char **pp) {
    const char *p = skip_ws(*pp);
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) { *pp = p; return 0.0; }
    *pp = end;
    return v;
}

static int parse_array_doubles(const char **pp, double *out, int maxn) {
    const char *p = skip_ws(*pp);
    if (*p != '[') return 0;
    p = skip_ws(p + 1);
    int n = 0;
    while (*p && *p != ']' && n < maxn) {
        out[n++] = parse_num(&p);
        p = skip_ws(p);
        if (*p == ',') p = skip_ws(p + 1);
    }
    *pp = p;
    return n;
}

static void parse_str_value(const char **pp, char *out, size_t outsz) {
    const char *p = skip_ws(*pp);
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < outsz) out[i++] = *p++;
        out[i] = 0;
        if (*p == '"') p++;
        *pp = p;
    } else {
        out[0] = 0;
    }
}

int wb_print_load(const char *path, wb_voiceprint_t *vp) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return -1; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = 0;
    fclose(f);

    memset(vp, 0, sizeof(*vp));
    const char *v;

    if (find_key(buf, "name", &v)) parse_str_value(&v, vp->name, sizeof(vp->name));
    if (find_key(buf, "f0_mean", &v)) vp->f0.f0_mean = parse_num(&v);
    if (find_key(buf, "f0_min", &v)) vp->f0.f0_min = parse_num(&v);
    if (find_key(buf, "f0_max", &v)) vp->f0.f0_max = parse_num(&v);
    if (find_key(buf, "f0_sd", &v)) vp->f0.f0_sd = parse_num(&v);
    if (find_key(buf, "vibrato_rate", &v)) vp->f0.vibrato_rate = parse_num(&v);
    if (find_key(buf, "vibrato_depth", &v)) vp->f0.vibrato_depth = parse_num(&v);
    if (find_key(buf, "voiced", &v)) vp->f0.voiced_fraction = parse_num(&v);
    if (find_key(buf, "jitter", &v)) vp->quality.jitter_pct = parse_num(&v);
    if (find_key(buf, "shimmer", &v)) vp->quality.shimmer_pct = parse_num(&v);
    if (find_key(buf, "hnr", &v)) vp->quality.hnr_db = parse_num(&v);
    if (find_key(buf, "cpp", &v)) vp->quality.cpp = parse_num(&v);
    if (find_key(buf, "h1h2", &v)) vp->quality.h1h2_db = parse_num(&v);
    if (find_key(buf, "tilt", &v)) vp->quality.tilt_db_per_oct = parse_num(&v);
    if (find_key(buf, "f0_render", &v)) vp->f0_render = parse_num(&v);
    if (find_key(buf, "F", &v)) vp->formants.n = parse_array_doubles(&v, vp->formants.F, 4);
    if (find_key(buf, "BW", &v)) { double bw[4]; int nb = parse_array_doubles(&v, bw, 4); for (int i = 0; i < nb && i < 4; i++) vp->formants.BW[i] = bw[i]; }
    if (find_key(buf, "diameters", &v)) vp->n_diameters = parse_array_doubles(&v, vp->diameters, 44);

    free(buf);
    return 0;
}
