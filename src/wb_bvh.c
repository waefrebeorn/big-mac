/* wb_bvh.c — BVH (Biovision Hierarchy) motion capture parser (R081).
 *
 * Parses the BVH format used by CMU mocap database, cgspeed, and most
 * motion capture tools. Two-part format:
 *   1. HIERARCHY: joint tree with offsets and channel declarations
 *   2. MOTION: frame count, frame time, then per-frame channel data
 *
 * After parsing, we can:
 *   - Sample any time point (with interpolation)
 *   - Compute 2D joint positions via forward kinematics
 *   - Render skeleton overlays onto video frames
 *   - Drive our wb_char2d bone system with real human motion
 *
 * Pure C11, no third party.
 */

#include "wbus/wbus_bvh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#define BVH_MAX_JOINTS 128
#define BVH_MAX_FRAMES 65536
#define BVH_MAX_CHANNELS 768

/* ---- Error reporting -------------------------------------------------- */
static char bvh_err[256] = "";
static const char *bvh_error_string(void) { return bvh_err; }

/* ---- Internal Structures ---------------------------------------------- */

typedef struct {
    wb_bvh_joint joints[BVH_MAX_JOINTS];
    int n_joints;

    /* Channel layout: each joint contributes 3 or 6 channels.
     * channel_to_joint[i] = joint index for channel i */
    int channel_to_joint[BVH_MAX_CHANNELS];
    int channel_type[BVH_MAX_CHANNELS];  /* bvh_channel enum */
    int total_channels;

    /* Frame data: tightly packed [frame][channel] */
    float *frames;
    int n_frames;
    double frame_time;
} wb_bvh_internal;

struct wb_bvh {
    wb_bvh_internal d;
};

/* ---- Forward Kinematic State ------------------------------------------ */

/* Per-joint transform accumulated during FK */
typedef struct {
    float pos[3];       /* world position of joint */
    float rot[3][3];    /* accumulated rotation matrix */
} fk_state;

/* ---- Helper: read file into memory ------------------------------------ */

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(bvh_err, sizeof(bvh_err), "cannot open %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); snprintf(bvh_err, sizeof(bvh_err), "out of memory"); return NULL; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    if (out_len) *out_len = (size_t)len;
    return buf;
}

/* ---- Helper: tokenizer ------------------------------------------------ */

typedef struct {
    const char *src;
    size_t pos;
    size_t len;
} bvh_tok;

static void tok_init(bvh_tok *t, const char *src, size_t len) {
    t->src = src; t->pos = 0; t->len = len;
}

static int tok_skip_ws(bvh_tok *t) {
    while (t->pos < t->len && isspace((unsigned char)t->src[t->pos])) t->pos++;
    return t->pos < t->len;
}

/* Read next whitespace-delimited token into buf. Returns 1 on success. */
static int tok_next(bvh_tok *t, char *buf, int bufsize) {
    if (!tok_skip_ws(t)) return 0;
    int i = 0;
    while (t->pos < t->len && !isspace((unsigned char)t->src[t->pos])) {
        if (i < bufsize - 1) buf[i++] = t->src[t->pos];
        t->pos++;
    }
    buf[i] = '\0';
    return 1;
}

/* ---- Helper: rotation matrices ---------------------------------------- */

static void mat_identity(float m[3][3]) {
    memset(m, 0, 9 * sizeof(float));
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

static void mat_mul(float out[3][3], const float a[3][3], const float b[3][3]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            out[i][j] = 0;
            for (int k = 0; k < 3; k++)
                out[i][j] += a[i][k] * b[k][j];
        }
}

static void mat_rot_x(float m[3][3], float angle) {
    float c = cosf(angle), s = sinf(angle);
    mat_identity(m);
    m[1][1] = c; m[1][2] = -s;
    m[2][1] = s; m[2][2] = c;
}

static void mat_rot_y(float m[3][3], float angle) {
    float c = cosf(angle), s = sinf(angle);
    mat_identity(m);
    m[0][0] = c; m[0][2] = s;
    m[2][0] = -s; m[2][2] = c;
}

static void mat_rot_z(float m[3][3], float angle) {
    float c = cosf(angle), s = sinf(angle);
    mat_identity(m);
    m[0][0] = c; m[0][1] = -s;
    m[1][0] = s; m[1][1] = c;
}

/* Parse a channel name to enum */
static int parse_channel(const char *name) {
    if (strcmp(name, "Xposition") == 0) return BVH_CH_XPOS;
    if (strcmp(name, "Yposition") == 0) return BVH_CH_YPOS;
    if (strcmp(name, "Zposition") == 0) return BVH_CH_ZPOS;
    if (strcmp(name, "Zrotation") == 0) return BVH_CH_ZROT;
    if (strcmp(name, "Yrotation") == 0) return BVH_CH_YROT;
    if (strcmp(name, "Xrotation") == 0) return BVH_CH_XROT;
    return -1;
}

/* ---- Parse HIERARCHY section ------------------------------------------ */
/*
 * Recursive descent parser for BVH hierarchy.
 * Grammar:
 *   hierarchy := "HIERARCHY" root
 *   root      := "ROOT" STRING "{" joint_body "}"
 *   joint     := "JOINT" STRING "{" joint_body "}"
 *   joint_body:= (OFFSET | CHANNELS | child_joint | end_site)*
 *   OFFSET    := "OFFSET" FLOAT FLOAT FLOAT
 *   CHANNELS  := "CHANNELS" INT (STRING)+
 *   end_site  := "End" "Site" "{" OFFSET "}"
 *
 * Returns 0 on success, -1 on error.
 */

static int parse_joint_body(wb_bvh_internal *d, bvh_tok *t, int joint_idx);

static int parse_joint(wb_bvh_internal *d, bvh_tok *t, int is_root) {
    char tok[256];
    const char *kw = is_root ? "ROOT" : "JOINT";

    if (!tok_next(t, tok, sizeof(tok)) || strcmp(tok, kw) != 0) {
        snprintf(bvh_err, sizeof(bvh_err), "expected %s", kw);
        return -1;
    }

    if (d->n_joints >= BVH_MAX_JOINTS) {
        snprintf(bvh_err, sizeof(bvh_err), "too many joints (%d)", BVH_MAX_JOINTS);
        return -1;
    }

    int ji = d->n_joints++;
    wb_bvh_joint *j = &d->joints[ji];
    memset(j, 0, sizeof(*j));
    j->parent = -1;  /* set by caller */
    for (int c = 0; c < BVH_CH_COUNT; c++) j->channel_indices[c] = -1;

    /* Read joint name */
    if (!tok_next(t, tok, sizeof(tok))) {
        snprintf(bvh_err, sizeof(bvh_err), "expected joint name after %s", kw);
        return -1;
    }
    if (tok[0] == '{') {
        snprintf(j->name, sizeof(j->name), "joint_%d", ji);
    } else {
        strncpy(j->name, tok, sizeof(j->name) - 1);
        if (!tok_next(t, tok, sizeof(tok)) || tok[0] != '{') {
            snprintf(bvh_err, sizeof(bvh_err), "expected '{' after joint name '%s'", j->name);
            return -1;
        }
    }

    /* Parse body */
    return parse_joint_body(d, t, ji);
}

static int parse_end_site(wb_bvh_internal *d, bvh_tok *t, int parent_idx) {
    char tok[256];

    /* Already consumed "End" */
    if (!tok_next(t, tok, sizeof(tok)) || strcmp(tok, "Site") != 0) {
        snprintf(bvh_err, sizeof(bvh_err), "expected 'Site' after 'End'");
        return -1;
    }
    if (!tok_next(t, tok, sizeof(tok)) || tok[0] != '{') {
        snprintf(bvh_err, sizeof(bvh_err), "expected '{' after 'End Site'");
        return -1;
    }

    if (d->n_joints >= BVH_MAX_JOINTS) {
        snprintf(bvh_err, sizeof(bvh_err), "too many joints");
        return -1;
    }

    int si = d->n_joints++;
    wb_bvh_joint *s = &d->joints[si];
    memset(s, 0, sizeof(*s));
    s->parent = parent_idx;
    s->is_site = 1;
    for (int c = 0; c < BVH_CH_COUNT; c++) s->channel_indices[c] = -1;
    snprintf(s->name, sizeof(s->name), "site_%d", si);

    /* OFFSET */
    if (!tok_next(t, tok, sizeof(tok)) || strcmp(tok, "OFFSET") != 0) {
        snprintf(bvh_err, sizeof(bvh_err), "expected OFFSET in End Site");
        return -1;
    }
    if (!tok_next(t, tok, sizeof(tok))) return -1;
    s->offset[0] = (float)atof(tok);
    if (!tok_next(t, tok, sizeof(tok))) return -1;
    s->offset[1] = (float)atof(tok);
    if (!tok_next(t, tok, sizeof(tok))) return -1;
    s->offset[2] = (float)atof(tok);

    /* Closing '}' */
    if (!tok_next(t, tok, sizeof(tok)) || tok[0] != '}') {
        snprintf(bvh_err, sizeof(bvh_err), "expected '}' after End Site offset");
        return -1;
    }

    return 0;
}

static int parse_joint_body(wb_bvh_internal *d, bvh_tok *t, int joint_idx) {
    char tok[256];
    wb_bvh_joint *j = &d->joints[joint_idx];

    while (tok_next(t, tok, sizeof(tok))) {
        if (tok[0] == '}') {
            return 0;  /* end of this joint's body */
        }

        if (strcmp(tok, "OFFSET") == 0) {
            if (!tok_next(t, tok, sizeof(tok))) return -1;
            j->offset[0] = (float)atof(tok);
            if (!tok_next(t, tok, sizeof(tok))) return -1;
            j->offset[1] = (float)atof(tok);
            if (!tok_next(t, tok, sizeof(tok))) return -1;
            j->offset[2] = (float)atof(tok);
        } else if (strcmp(tok, "CHANNELS") == 0) {
            if (!tok_next(t, tok, sizeof(tok))) return -1;
            j->n_channels = atoi(tok);
            for (int c = 0; c < j->n_channels; c++) {
                if (!tok_next(t, tok, sizeof(tok))) return -1;
                int ch = parse_channel(tok);
                if (ch >= 0) {
                    j->channel_indices[ch] = d->total_channels;
                    d->channel_to_joint[d->total_channels] = joint_idx;
                    d->channel_type[d->total_channels] = ch;
                    d->total_channels++;
                }
            }
        } else if (strcmp(tok, "JOINT") == 0) {
            /* Child joint — recurse.
             * "JOINT" token already consumed by this loop, but parse_joint
             * expects to read it. Rewind so parse_joint sees it. */
            t->pos -= strlen(tok);
            while (t->pos > 0 && isspace((unsigned char)t->src[t->pos - 1])) t->pos--;
            int ci = d->n_joints;
            if (parse_joint(d, t, 0) != 0) return -1;
            d->joints[ci].parent = joint_idx;
        } else if (strcmp(tok, "End") == 0) {
            if (parse_end_site(d, t, joint_idx) != 0) return -1;
        } else {
            snprintf(bvh_err, sizeof(bvh_err), "unexpected token '%s' in joint body", tok);
            return -1;
        }
    }

    snprintf(bvh_err, sizeof(bvh_err), "unexpected end of file in joint body");
    return -1;
}

static int parse_hierarchy(wb_bvh_internal *d, bvh_tok *t) {
    char tok[256];

    if (!tok_next(t, tok, sizeof(tok)) || strcmp(tok, "HIERARCHY") != 0) {
        snprintf(bvh_err, sizeof(bvh_err), "expected HIERARCHY");
        return -1;
    }

    /* Parse the root joint */
    if (parse_joint(d, t, 1) != 0) return -1;

    /* Next token should be "MOTION" — consume it so parse_motion can proceed */
    if (!tok_next(t, tok, sizeof(tok))) {
        snprintf(bvh_err, sizeof(bvh_err), "unexpected EOF after hierarchy");
        return -1;
    }
    if (strcmp(tok, "MOTION") != 0) {
        /* Maybe there are more roots? Not standard but handle gracefully */
        /* Rewind and let caller deal with it */
        t->pos -= strlen(tok);
        while (t->pos > 0 && isspace((unsigned char)t->src[t->pos - 1])) t->pos--;
    }

    return 0;
}

/* ---- Parse MOTION section --------------------------------------------- */

static int parse_motion(wb_bvh_internal *d, bvh_tok *t) {
    char tok[256];

    /* Already consumed "MOTION" token in hierarchy parse */
    if (!tok_next(t, tok, sizeof(tok)) || strcmp(tok, "Frames:") != 0) {
        snprintf(bvh_err, sizeof(bvh_err), "expected Frames:");
        return -1;
    }
    if (!tok_next(t, tok, sizeof(tok))) {
        snprintf(bvh_err, sizeof(bvh_err), "expected frame count");
        return -1;
    }
    d->n_frames = atoi(tok);
    if (d->n_frames <= 0 || d->n_frames > BVH_MAX_FRAMES) {
        snprintf(bvh_err, sizeof(bvh_err), "invalid frame count: %d", d->n_frames);
        return -1;
    }

    if (!tok_next(t, tok, sizeof(tok)) || strcmp(tok, "Frame") != 0) {
        snprintf(bvh_err, sizeof(bvh_err), "expected Frame Time:");
        return -1;
    }
    if (!tok_next(t, tok, sizeof(tok)) || strcmp(tok, "Time:") != 0) {
        snprintf(bvh_err, sizeof(bvh_err), "expected Frame Time:");
        return -1;
    }
    if (!tok_next(t, tok, sizeof(tok))) {
        snprintf(bvh_err, sizeof(bvh_err), "expected frame time value");
        return -1;
    }
    d->frame_time = atof(tok);
    if (d->frame_time <= 0) d->frame_time = 1.0 / 120.0;  /* CMU default */

    /* Allocate frame data */
    size_t total_floats = (size_t)d->n_frames * d->total_channels;
    d->frames = calloc(total_floats, sizeof(float));
    if (!d->frames) {
        snprintf(bvh_err, sizeof(bvh_err), "out of memory for frames");
        return -1;
    }

    /* Read frame data — just read all remaining floats */
    int fi = 0;
    int ci = 0;
    while (tok_next(t, tok, sizeof(tok)) && fi < d->n_frames) {
        d->frames[(size_t)fi * d->total_channels + ci] = (float)atof(tok);
        ci++;
        if (ci >= d->total_channels) {
            ci = 0;
            fi++;
        }
    }

    return 0;
}

/* ---- Public API: Load ------------------------------------------------- */

wb_bvh *wb_bvh_load(const char *path) {
    size_t len;
    char *buf = read_file(path, &len);
    if (!buf) return NULL;

    wb_bvh *b = calloc(1, sizeof(wb_bvh));
    if (!b) { free(buf); return NULL; }

    bvh_tok t;
    tok_init(&t, buf, len);

    if (parse_hierarchy(&b->d, &t) != 0 ||
        parse_motion(&b->d, &t) != 0) {
        wb_bvh_free(b);
        free(buf);
        return NULL;
    }

    free(buf);
    return b;
}

void wb_bvh_free(wb_bvh *b) {
    if (!b) return;
    free(b->d.frames);
    free(b);
}

/* ---- Public API: Query ------------------------------------------------ */

int    wb_bvh_joint_count(const wb_bvh *b) { return b ? b->d.n_joints : 0; }
int    wb_bvh_frame_count(const wb_bvh *b) { return b ? b->d.n_frames : 0; }
double wb_bvh_frame_time(const wb_bvh *b)  { return b ? b->d.frame_time : 0; }
double wb_bvh_duration(const wb_bvh *b)    { return b ? b->d.n_frames * b->d.frame_time : 0; }
const wb_bvh_joint *wb_bvh_get_joints(const wb_bvh *b) { return b ? b->d.joints : NULL; }

const float *wb_bvh_get_frame(const wb_bvh *b, int frame_idx) {
    if (!b || frame_idx < 0 || frame_idx >= b->d.n_frames) return NULL;
    return &b->d.frames[(size_t)frame_idx * b->d.total_channels];
}

int wb_bvh_sample(const wb_bvh *b, double time_sec, float *frame_data) {
    if (!b || !frame_data) return -1;
    double ft = b->d.frame_time;
    double f = time_sec / ft;
    int i0 = (int)floor(f);
    int i1 = i0 + 1;
    float alpha = (float)(f - floor(f));

    if (i0 < 0) i0 = 0;
    if (i1 >= b->d.n_frames) i1 = b->d.n_frames - 1;
    if (i0 >= b->d.n_frames) i0 = b->d.n_frames - 1;

    const float *f0 = &b->d.frames[(size_t)i0 * b->d.total_channels];
    const float *f1 = &b->d.frames[(size_t)i1 * b->d.total_channels];

    for (int c = 0; c < b->d.total_channels; c++) {
        frame_data[c] = f0[c] * (1.0f - alpha) + f1[c] * alpha;
    }
    return 0;
}

/* ---- Forward Kinematics → 2D Positions ------------------------------- */

int wb_bvh_compute_positions_2d(const wb_bvh *b, const float *frame_data,
                                  float *joint_positions, int n_joints,
                                  float scale, float cx, float cy) {
    if (!b || !frame_data || !joint_positions) return 0;

    int nj = b->d.n_joints;
    if (n_joints < nj) nj = n_joints;

    /* FK state per joint */
    fk_state states[BVH_MAX_JOINTS];
    memset(states, 0, sizeof(states));

    for (int j = 0; j < nj; j++) {
        const wb_bvh_joint *joint = &b->d.joints[j];

        /* Build local rotation matrix from channel data */
        float local_rot[3][3];
        mat_identity(local_rot);

        /* Apply rotations in the order specified by channels */
        /* BVH typically uses Z, Y, X order */
        float rx = 0, ry = 0, rz = 0;
        int has_pos = 0;
        float px = 0, py = 0, pz = 0;

        for (int ch = 0; ch < BVH_CH_COUNT; ch++) {
            int ci = joint->channel_indices[ch];
            if (ci < 0) continue;
            float val = frame_data[ci];
            switch (ch) {
                case BVH_CH_XPOS: px = val; has_pos = 1; break;
                case BVH_CH_YPOS: py = val; has_pos = 1; break;
                case BVH_CH_ZPOS: pz = val; has_pos = 1; break;
                case BVH_CH_XROT: rx = val * (float)M_PI / 180.0f; break;
                case BVH_CH_YROT: ry = val * (float)M_PI / 180.0f; break;
                case BVH_CH_ZROT: rz = val * (float)M_PI / 180.0f; break;
            }
        }

        /* Build rotation: R = Rz * Ry * Rx (BVH convention) */
        float rz_m[3][3], ry_m[3][3], rx_m[3][3], temp[3][3];
        mat_rot_z(rz_m, rz);
        mat_rot_y(ry_m, ry);
        mat_rot_x(rx_m, rx);
        mat_mul(temp, rz_m, ry_m);
        mat_mul(local_rot, temp, rx_m);

        if (joint->parent < 0) {
            /* Root: position from channels or offset */
            states[j].pos[0] = has_pos ? px : joint->offset[0];
            states[j].pos[1] = has_pos ? py : joint->offset[1];
            states[j].pos[2] = has_pos ? pz : joint->offset[2];
            memcpy(states[j].rot, local_rot, 9 * sizeof(float));
        } else {
            /* Child: position = parent_pos + parent_rot * offset */
            fk_state *parent = &states[joint->parent];
            float ox = joint->offset[0], oy = joint->offset[1], oz = joint->offset[2];
            states[j].pos[0] = parent->pos[0] + parent->rot[0][0]*ox + parent->rot[0][1]*oy + parent->rot[0][2]*oz;
            states[j].pos[1] = parent->pos[1] + parent->rot[1][0]*ox + parent->rot[1][1]*oy + parent->rot[1][2]*oz;
            states[j].pos[2] = parent->pos[2] + parent->rot[2][0]*ox + parent->rot[2][1]*oy + parent->rot[2][2]*oz;
            mat_mul(states[j].rot, parent->rot, local_rot);
        }

        /* Project to 2D: orthographic (x, y) → screen */
        joint_positions[j * 2 + 0] = cx + states[j].pos[0] * scale;
        joint_positions[j * 2 + 1] = cy - states[j].pos[1] * scale;  /* flip Y */
    }

    return nj;
}

/* ---- Render Skeleton onto RGBA ---------------------------------------- */

int wb_bvh_render_skeleton(const wb_bvh *bh, const float *frame_data,
                            uint8_t *rgba, int w, int h,
                            float scale, float cx, float cy,
                            uint8_t lr, uint8_t lg, uint8_t lb,
                            float line_width) {
    const wb_bvh *b = bh;
    if (!b || !frame_data || !rgba) return -1;

    int nj = b->d.n_joints;
    float positions[BVH_MAX_JOINTS * 2];
    wb_bvh_compute_positions_2d(b, frame_data, positions, nj, scale, cx, cy);

    /* Draw lines between parent-child pairs */
    for (int j = 0; j < nj; j++) {
        if (b->d.joints[j].is_site) continue;
        int p = b->d.joints[j].parent;
        if (p < 0) continue;
        if (b->d.joints[p].is_site) continue;

        float x0 = positions[p * 2 + 0], y0 = positions[p * 2 + 1];
        float x1 = positions[j * 2 + 0], y1 = positions[j * 2 + 1];

        /* Bresenham line (thick) */
        int ix0 = (int)x0, iy0 = (int)y0, ix1 = (int)x1, iy1 = (int)y1;
        int dx = abs(ix1 - ix0), dy = abs(iy1 - iy0);
        int sx = (ix0 < ix1) ? 1 : -1, sy = (iy0 < iy1) ? 1 : -1;
        int err = dx - dy;
        int half_w = (int)(line_width / 2);

        for (;;) {
            /* Draw thick point */
            for (int oy = -half_w; oy <= half_w; oy++)
                for (int ox = -half_w; ox <= half_w; ox++) {
                    int px = ix0 + ox, py = iy0 + oy;
                    if (px >= 0 && px < w && py >= 0 && py < h) {
                        int idx = (py * w + px) * 4;
                        rgba[idx] = lr; rgba[idx+1] = lg; rgba[idx+2] = lb; rgba[idx+3] = 255;
                    }
                }
            if (ix0 == ix1 && iy0 == iy1) break;
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; ix0 += sx; }
            if (e2 < dx) { err += dx; iy0 += sy; }
        }
    }

    /* Draw joint dots */
    for (int j = 0; j < nj; j++) {
        if (b->d.joints[j].is_site) continue;
        int jx = (int)positions[j * 2 + 0], jy = (int)positions[j * 2 + 1];
        int r = (int)line_width + 2;
        for (int oy = -r; oy <= r; oy++)
            for (int ox = -r; ox <= r; ox++) {
                if (ox*ox + oy*oy > r*r) continue;
                int px = jx + ox, py = jy + oy;
                if (px >= 0 && px < w && py >= 0 && py < h) {
                    int idx = (py * w + px) * 4;
                    /* White center for joints */
                    rgba[idx] = 255; rgba[idx+1] = 255; rgba[idx+2] = 255; rgba[idx+3] = 255;
                }
            }
    }

    return 0;
}

/* ---- CMU Download ----------------------------------------------------- */

int wb_bvh_download_cmu(int subject_num, int motion_num,
                         const char *out_path,
                         const char *amc2bvh_path) {
    char cmd[2048];
    char asf_path[] = "/tmp/cmu_tmp.asf";
    char amc_path[] = "/tmp/cmu_tmp.amc";

    /* Download ASF (skeleton) */
    snprintf(cmd, sizeof(cmd),
             "curl -s \"https://mocap.cs.cmu.edu/subjects/%d/%d.asf\" -o %s",
             subject_num, subject_num, asf_path);
    if (system(cmd) != 0) {
        snprintf(bvh_err, sizeof(bvh_err), "failed to download ASF for subject %d", subject_num);
        return -1;
    }

    /* Download AMC (motion) */
    snprintf(cmd, sizeof(cmd),
             "curl -s \"https://mocap.cs.cmu.edu/subjects/%d/%d_%02d.amc\" -o %s",
             subject_num, subject_num, motion_num, amc_path);
    if (system(cmd) != 0) {
        /* Try alternate naming */
        snprintf(cmd, sizeof(cmd),
                 "curl -s \"https://mocap.cs.cmu.edu/subjects/%d/%d_%d.amc\" -o %s",
                 subject_num, subject_num, motion_num, amc_path);
        if (system(cmd) != 0) {
            snprintf(bvh_err, sizeof(bvh_err), "failed to download AMC for subject %d motion %d",
                     subject_num, motion_num);
            return -1;
        }
    }

    /* Convert to BVH */
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" \"%s\" -o \"%s\"",
             amc2bvh_path, asf_path, amc_path, out_path);
    int rc = system(cmd);
    remove(asf_path);
    remove(amc_path);

    if (rc != 0) {
        snprintf(bvh_err, sizeof(bvh_err), "amc2bvh conversion failed");
        return -1;
    }
    return 0;
}

const char *wb_bvh_error_string(void) { return bvh_err; }
