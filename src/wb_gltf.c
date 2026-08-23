/* wb_gltf.c — minimal GLB (glTF 2.0) importer (R056).
 *
 * Container layout (per Khronos spec):
 *   u32 magic  = 0x46546C67 ('glTF')
 *   u32 version= 2
 *   u32 length = total file bytes
 *   chunk: {u32 len, u32 type=0x4E4F534A 'JSON', data}   (first, required)
 *   chunk: {u32 len, u32 type=0x004E4942 'BIN',  data}   (optional)
 *
 * The JSON side is scanned with a tiny hand-rolled reader — we only need
 * top-level arrays "meshes"/"accessors"/"bufferViews"/"nodes" and the
 * primitive attributes. No DOM, no allocation-heavy parser.
 */

#include "wbus/wbus_gltf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ---------------- tiny JSON value scanner --------------------------------
 * Supports exactly what glTF JSON uses: objects, arrays, strings, numbers,
 * true/false/null. Whitespace-insensitive. NOT a general parser. */

typedef struct {
    const char *p, *end;
} jscan;

static void js_ws(jscan *j) {
    while (j->p < j->end && (*j->p==' '||*j->p=='\t'||*j->p=='\n'||*j->p=='\r')) j->p++;
}
static int js_peek(jscan *j) { js_ws(j); return j->p < j->end ? *j->p : 0; }
static int js_eat(jscan *j, char c) {
    js_ws(j);
    if (j->p < j->end && *j->p == c) { j->p++; return 1; }
    return 0;
}
/* parse a string literal into out (truncates at cap-1); advances past it */
static int js_string(jscan *j, char *out, int cap) {
    js_ws(j);
    if (j->p >= j->end || *j->p != '"') return 0;
    j->p++;
    int n = 0;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && j->p < j->end) c = *j->p++;
        if (n < cap-1) out[n++] = c;
    }
    if (j->p < j->end) j->p++;      /* closing quote */
    out[n] = 0;
    return 1;
}
static double js_number(jscan *j) {
    js_ws(j);
    char *end;
    double v = strtod(j->p, &end);
    if (end == j->p) return 0;
    j->p = end;
    return v;
}
/* skip any value (object/array/string/number/bool/null) */
static void js_skip(jscan *j) {
    js_ws(j);
    if (j->p >= j->end) return;
    char c = *j->p;
    if (c == '{' || c == '[') {
        char open = c, close = (c=='{') ? '}' : ']';
        int depth = 0;
        while (j->p < j->end) {
            char d = *j->p;
            if (d == '"') {                      /* skip strings atomically */
                j->p++;
                while (j->p < j->end && *j->p != '"') {
                    if (*j->p == '\\') j->p++;
                    j->p++;
                }
                if (j->p < j->end) j->p++;
                continue;
            }
            if (d == open) depth++;
            else if (d == close) { depth--; if (!depth) { j->p++; return; } }
            j->p++;
        }
    } else if (c == '"') {
        char tmp[8]; js_string(j, tmp, sizeof tmp);
    } else {
        while (j->p < j->end && *j->p!=',' && *j->p!='}' && *j->p!=']'
               && *j->p!=' ' && *j->p!='\n') j->p++;
    }
}
/* find "key": inside the CURRENT object level and return scanner positioned
 * at the value. Does not descend. Returns 1 if found. */
static int js_find_key(jscan *j, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *hit = NULL;
    /* scan only this object's extent */
    jscan tmp = *j;
    int depth = 0;
    while (tmp.p < tmp.end) {
        char c = *tmp.p;
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth <= 0) break; }
        else if (c == '"') {
            const char *sstart = tmp.p;
            tmp.p++;
            while (tmp.p < tmp.end && *tmp.p != '"') {
                if (*tmp.p == '\\') tmp.p++;
                tmp.p++;
            }
            if (tmp.p >= tmp.end) break;
            tmp.p++;
            size_t slen = (size_t)(tmp.p - sstart);
            if (depth == 1 &&
                slen == strlen(pat) &&
                strncmp(sstart, pat, slen) == 0) {
                js_ws(&tmp);
                if (tmp.p < tmp.end && tmp.p[0] == ':') {
                    tmp.p++;
                    hit = tmp.p;
                    break;
                }
            }
            continue;
        }
        tmp.p++;
    }
    if (!hit) return 0;
    j->p = hit;
    return 1;
}

/* ---------------- GLB container ------------------------------------------ */

typedef struct {
    unsigned char *json; size_t json_len;
    unsigned char *bin;  size_t bin_len;
} glb_file;

static int glb_load(const char *path, glb_file *g) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12) { fclose(f); return -1; }
    uint32_t magic, version, length;
    memcpy(&magic,   hdr,    4);
    memcpy(&version, hdr+4,  4);
    memcpy(&length,  hdr+8,  4);
    if (magic != 0x46546C67u || version != 2) { fclose(f); return -1; }

    memset(g, 0, sizeof(*g));
    while (g->json == NULL || g->bin == NULL) {
        uint32_t clen, ctype;
        if (fread(&clen, 4, 1, f) != 1) break;
        if (fread(&ctype, 4, 1, f) != 1) break;
        if (ctype == 0x4E4F534Au) {                     /* JSON */
            g->json = malloc(clen + 1);
            if (!g->json || fread(g->json, 1, clen, f) != clen) break;
            g->json[clen] = 0; g->json_len = clen;
        } else if (ctype == 0x004E4942u) {              /* BIN */
            g->bin = malloc(clen);
            if (!g->bin || fread(g->bin, 1, clen, f) != clen) break;
            g->bin_len = clen;
        } else {
            fseek(f, clen, SEEK_CUR);                   /* unknown: skip */
        }
    }
    fclose(f);
    if (!g->json) { free(g->json); free(g->bin); memset(g,0,sizeof(*g)); return -1; }
    return 0;
}
static void glb_free(glb_file *g) { free(g->json); free(g->bin); memset(g,0,sizeof(*g)); }

/* ---------------- accessor reading ---------------------------------------- */

/* accessor -> bufferView -> byte offset into BIN.
 * We support componentType 5126 (float), 5125 (u32), 5123 (u16). */

typedef struct {
    int   valid;
    long  bufferview, count;
    long  comp_type;     /* 5126/5125/5123 */
    long  type;          /* VEC3=3 floats, SCALAR=1 */
    long  byteoffset;    /* accessor-level byteOffset */
} acc_info;

/* find accessors[i]'s bufferView byteOffset by scanning the JSON array.
 * This is O(n^2)-ish but asset JSON is small (tens of KB). */
static long bufferview_offset(const glb_file *g, long bvidx) {
    jscan j = { (char*)g->json, (char*)g->json + g->json_len };
    if (!js_find_key(&j, "bufferViews")) return -1;
    if (!js_eat(&j, '[')) return -1;
    long idx = 0;
    while (js_peek(&j) != 0 && js_peek(&j) != ']') {
        if (idx == bvidx) {
            long off = 0;
            jscan obj = j;
            js_skip(&obj);                       /* extent of this object */
            jscan probe = j;
            probe.end = obj.p;
            if (js_find_key(&probe, "byteOffset")) off = (long)js_number(&probe);
            return off;
        }
        js_skip(&j);                             /* skip this object */
        js_eat(&j, ',');
        idx++;
    }
    return -1;
}

static acc_info accessor_info(const glb_file *g, long accidx) {
    acc_info ai; memset(&ai, 0, sizeof ai);
    jscan j = { (char*)g->json, (char*)g->json + g->json_len };
    if (!js_find_key(&j, "accessors")) return ai;
    if (!js_eat(&j, '[')) return ai;
    long idx = 0;
    while (js_peek(&j) != 0 && js_peek(&j) != ']') {
        if (idx == accidx) {
            jscan obj = j;
            js_skip(&obj);
            /* R056-fix: each key search must start from the OBJECT START.
             * js_find_key consumes the scanner, so re-copy per key. */
            if (1) {
                jscan q = j; q.end = obj.p;
                if (js_find_key(&q, "bufferView"))
                    ai.bufferview = (long)js_number(&q);
            }
            if (1) {
                jscan q = j; q.end = obj.p;
                if (js_find_key(&q, "count"))
                    ai.count = (long)js_number(&q);
            }
            if (1) {
                jscan q = j; q.end = obj.p;
                if (js_find_key(&q, "componentType"))
                    ai.comp_type = (long)js_number(&q);
            }
            if (1) {
                jscan q = j; q.end = obj.p;
                if (js_find_key(&q, "type")) {
                    char ts[16];
                    js_string(&q, ts, sizeof ts);
                    ai.type = strcmp(ts,"VEC3")==0 ? 3 : 1;
                }
            }
            if (1) {
                jscan q = j; q.end = obj.p;
                if (js_find_key(&q, "byteOffset"))
                    ai.byteoffset = (long)js_number(&q);
            }
            ai.valid = (ai.comp_type==5126||ai.comp_type==5125||ai.comp_type==5123);
            return ai;
        }
        js_skip(&j);
        js_eat(&j, ',');
        idx++;
    }
    return ai;
}

/* resolve accessor -> pointer into BIN + element size */
static const unsigned char *acc_data(const glb_file *g, const acc_info *ai,
                                     size_t *elem_size) {
    if (!ai->valid || !g->bin) return NULL;
    long bvo = bufferview_offset(g, ai->bufferview);
    if (bvo < 0) return NULL;
    size_t comps = (size_t)(ai->type <= 0 ? 1 : ai->type);
    size_t csz = ai->comp_type == 5126 ? 4 : (ai->comp_type == 5125 ? 4 : 2);
    *elem_size = comps * csz;
    return g->bin + bvo + ai->byteoffset;
}

wb_mesh *wb_gltf_load_glb_ex(const char *path, float scale,
                             uint8_t br, uint8_t bg, uint8_t bb) {
    glb_file g;
    if (glb_load(path, &g) != 0) return NULL;

    /* locate meshes[0].primitives[0].attributes.POSITION / indices */
    jscan root = { (char*)g.json, (char*)g.json + g.json_len };
    wb_mesh *out = NULL;

    if (!js_find_key(&root, "meshes")) goto fail;
    if (!js_eat(&root, '[')) goto fail;
    if (js_peek(&root) != '{') goto fail;

    {
        jscan mesh_obj = root;
        js_skip(&mesh_obj);

        jscan m = root; m.end = mesh_obj.p;
        if (!js_find_key(&m, "primitives")) goto fail;
        if (!js_eat(&m, '[')) goto fail;

        /* iterate ALL primitives (Kenney models can be multi-material) */
        /* growable accumulation */
        wb_rast_vertex *verts = NULL; int nv = 0, capv = 0;
        wb_rast_tri *tris = NULL; int nt = 0, capt = 0;

        while (js_peek(&m) != 0 && js_peek(&m) != ']') {
            jscan prim = m;
            js_skip(&prim);              /* extent of this primitive */
            jscan p = m; p.end = prim.p;
            m.p = prim.p;                /* R056-fix: advance past it */

            long pos_acc = -1, idx_acc = -1, col_acc = -1;
            /* R056-fix: per-key rescoping (scanner-consumption bug) */
            {
                jscan q = p; q.end = prim.p;
                if (js_find_key(&q, "attributes")) {
                    jscan attrs = q;                 /* at '{' */
                    js_skip(&attrs);                 /* extent */
                    jscan a = attrs; a.p = q.p; a.end = attrs.p;
                    /* a now spans the attributes object */
                    { jscan k = a;
                      if (js_find_key(&k, "POSITION")) pos_acc=(long)js_number(&k); }
                    { jscan k = a;
                      if (js_find_key(&k, "COLOR_0"))  col_acc=(long)js_number(&k); }
                }
            }
            { jscan q = p; q.end = prim.p;
              if (js_find_key(&q, "indices")) idx_acc = (long)js_number(&q); }

            if (pos_acc >= 0) {
                acc_info pai = accessor_info(&g, pos_acc);
                size_t esz;
                const unsigned char *pd = acc_data(&g, &pai, &esz);
                if (pd) {
                    int base = nv;
                    for (long v = 0; v < pai.count; v++) {
                        float x, y, z;
                        memcpy(&x, pd + v*esz,     4);
                        memcpy(&y, pd + v*esz + 4, 4);
                        memcpy(&z, pd + v*esz + 8, 4);
                        if (nv >= capv) {
                            capv = capv ? capv*2 : 256;
                            verts = realloc(verts, (size_t)capv*sizeof(*verts));
                        }
                        verts[nv].x = x*scale; verts[nv].y = y*scale; verts[nv].z = z*scale;
                        nv++;
                    }
                    /* indices */
                    if (idx_acc >= 0) {
                        acc_info iai = accessor_info(&g, idx_acc);
                        size_t isz;
                        const unsigned char *idat = acc_data(&g, &iai, &isz);
                        if (idat) {
                            for (long q = 0; q + 2 < iai.count; q += 3) {
                                unsigned i0,i1,i2;
                                if (iai.comp_type == 5125) {
                                    memcpy(&i0, idat+(q)*isz,4);
                                    memcpy(&i1, idat+(q+1)*isz,4);
                                    memcpy(&i2, idat+(q+2)*isz,4);
                                } else {
                                    unsigned short s0,s1,s2;
                                    memcpy(&s0, idat+(q)*isz,2);
                                    memcpy(&s1, idat+(q+1)*isz,2);
                                    memcpy(&s2, idat+(q+2)*isz,2);
                                    i0=s0;i1=s1;i2=s2;
                                }
                                uint8_t cr=br,cg=bg,cb=bb;
                                if (col_acc >= 0) {
                                    acc_info cai = accessor_info(&g, col_acc);
                                    size_t csz;
                                    const unsigned char *cd = acc_data(&g,&cai,&csz);
                                    if (cd && cai.type==3) {
                                        long vi0 = base + (long)i0;
                                        long local = vi0 - base;
                                        (void)local;
                                        /* COLOR_0 is per-vertex over THIS primitive's
                                         * accessor space; map via i0 directly */
                                        long cidx = (long)i0;
                                        float r,g_,b_;
                                        memcpy(&r, cd+cidx*csz,   4);
                                        memcpy(&g_,cd+cidx*csz+4, 4);
                                        memcpy(&b_,cd+cidx*csz+8, 4);
                                        cr=(uint8_t)(r*255); cg=(uint8_t)(g_*255); cb=(uint8_t)(b_*255);
                                    }
                                }
                                if (nt >= capt) {
                                    capt = capt ? capt*2 : 512;
                                    tris = realloc(tris, (size_t)capt*sizeof(*tris));
                                }
                                tris[nt].v0=base+i0; tris[nt].v1=base+i1; tris[nt].v2=base+i2;
                                tris[nt].r=cr; tris[nt].g=cg; tris[nt].b=cb;
                                nt++;
                            }
                        }
                    }
                }
            }
            js_eat(&m, ',');
        }

        if (nv > 0 && nt > 0)
            out = wb_mesh_build(verts, nv, tris, nt);
        free(verts); free(tris);
    }

fail:
    glb_free(&g);
    return out;
}

wb_mesh *wb_gltf_load_glb(const char *path) {
    return wb_gltf_load_glb_ex(path, 1.0f, 200, 200, 200);
}
