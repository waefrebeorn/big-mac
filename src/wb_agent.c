/* wb_agent.c — headless agent command processor (R017 G9). Pure C11.
 * Drives the editor via the real session/export/EDL/voice-polish APIs. */

#include "wbus/wbus_agent.h"
#include "wbus/wbus_video.h"
#include "wbus/wbus_compositor.h"
#include "wbus/wbus_voice_polish.h"
#include "wbus/wbus_captions.h"
#include "wbus/wbus_mesh.h"
#include "wbus/wbus_anim.h"
#include "wbus/wbus_assets.h"
#include "wbus/wbus_cgiexport.h"
#include "wbus/wbus_shadowbin.h"
#include "wb_internal.h"   /* wb_wav_read/write_pcm16 (internal) */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\n' || s[n-1] == '\r'))
        s[--n] = '\0';
    return s;
}

/* Read a quoted-or-unquoted token; advances *pp past it. Returns a pointer
 * into a small rotating buffer (so up to 4 tokens stay live simultaneously —
 * do NOT free; the next 4 calls overwrite them). */
static char *tok(char **pp) {
    static char buf[4][1024];
    static int slot = 0;
    char *dst = buf[slot];
    slot = (slot + 1) & 3;
    char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < 1023) dst[i++] = *p++;
        if (*p == '"') p++;
        dst[i] = '\0';
    } else {
        int i = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && i < 1023)
            dst[i++] = *p++;
        dst[i] = '\0';
    }
    *pp = p;
    return dst;
}

static int do_import(wb_session *s, const char *src) {
    int vt = -1;
    for (uint32_t t = 0; t < s->track_count; t++)
        if (s->tracks[t].kind == WB_TRACK_KIND_VIDEO) { vt = (int)t; break; }
    if (vt < 0) vt = wb_session_add_video_track(s, "V1");
    int clip = wb_session_add_video_clip(s, vt, src, 0.0);
    return clip >= 0 ? 0 : -1;
}

/* ---- R059: CGI command state (one live scene per agent session) -------- */
static wb_anim    *g_cgi = NULL;
static int         g_next_obj = 0;

static wb_undo *g_agent_undo = NULL;

/* R062: snapshot the session so `agent-undo` can restore it. Called
 * automatically before destructive agent ops and explicitly via
 * `checkpoint`. */
static void wb_agent_checkpoint(wb_session *s) {
    if (!g_agent_undo) g_agent_undo = wb_undo_create();
    if (g_agent_undo) wb_undo_checkpoint(g_agent_undo, s);
}

static void cgi_ensure(void) {
    if (!g_cgi) {
        g_cgi = wb_anim_create(640, 360);
        g_next_obj = 0;
    }
}

static int cgi_command(wb_session *s, wb_engine *e,
                       const char *cmd, const char *rest) {
    (void)s;
    if (strcmp(cmd, "cgi-box") == 0 || strcmp(cmd, "cgi-sphere") == 0 ||
        strcmp(cmd, "cgi-cylinder") == 0 || strcmp(cmd, "cgi-cone") == 0) {
        cgi_ensure();
        if (!g_cgi) return -1;
        float x=0,y=0,z=0,size=1;
        int r=200,g=200,b=200;
        sscanf(rest, "%f %f %f %f %d %d %d", &x,&y,&z,&size,&r,&g,&b);
        wb_mesh *m = NULL;
        if      (strcmp(cmd,"cgi-box") == 0)
            m = wb_mesh_box(size,size,size,(uint8_t)r,(uint8_t)g,(uint8_t)b);
        else if (strcmp(cmd,"cgi-sphere") == 0)
            m = wb_mesh_sphere(size,10,14,(uint8_t)r,(uint8_t)g,(uint8_t)b);
        else if (strcmp(cmd,"cgi-cylinder") == 0)
            m = wb_mesh_cylinder(size,size*2,12,(uint8_t)r,(uint8_t)g,(uint8_t)b);
        else if (strcmp(cmd,"cgi-cone") == 0)
            m = wb_mesh_cone(size,size*2,12,(uint8_t)r,(uint8_t)g,(uint8_t)b);
        if (!m) return -1;
        wb_anim_add_object(g_cgi, m, (uint8_t)r,(uint8_t)g,(uint8_t)b);
        /* CGI scene is anim-owned, separate from session; no checkpoint */
        /* static key at t=0 so it's on stage; caller keys motion after */
        wb_anim_key(g_cgi, g_next_obj, 0.0, x,y,z, 0,0,0, 1);
        printf("cgi: added obj %d (%s)\n", g_next_obj, cmd);
        return g_next_obj++;
    }
    if (strcmp(cmd, "cgi-key") == 0) {
        if (!g_cgi) return -1;
        int obj = atoi(tok((char**)&rest));
        double t = atof(tok((char**)&rest));
        float px=atof(tok((char**)&rest)), py=atof(tok((char**)&rest)),
              pz=atof(tok((char**)&rest)), sc=atof(tok((char**)&rest));
        int ease = 0;
        const char *e_tok = tok((char**)&rest);
        if (e_tok && e_tok[0]) ease = atoi(e_tok);
        if (sc <= 0) sc = 1;
        return wb_anim_key_ease(g_cgi, obj, t, px,py,pz, 0,0,0, sc, ease);
    }
    if (strcmp(cmd, "cgi-asset") == 0) {
        /* cgi-asset <kit> <model> <x> <y> <z> : stamp a library GLB */
        cgi_ensure();
        if (!g_cgi) return -1;
        wb_assets *lib = wb_assets_open_default();
        if (!lib) { fprintf(stderr, "cgi: no asset library\n"); return -1; }
        char kit[128], model[128];
        sscanf(rest, "%127s %127s", kit, model);
        wb_mesh *m = wb_assets_load(lib, kit, model);
        if (!m) { wb_assets_close(lib); return -1; }
        m = wb_assets_release(lib, m);   /* own it: lib closes below */
        wb_assets_close(lib);
        int idx = wb_anim_add_object(g_cgi, m, 200,200,200);
        float x=0,y=0,z=0;
        sscanf(rest, "%*s %*s %f %f %f", &x,&y,&z);
        wb_anim_key(g_cgi, idx, 0.0, x,y,z, 0,0,0, 1);
        return idx;
    }
    if (strcmp(cmd, "cgi-list") == 0) {
        wb_assets *lib = wb_assets_open_default();
        if (!lib) { printf("cgi: no asset library\n"); return 0; }
        for (int k = 0; k < wb_assets_kit_count(lib); k++) {
            printf("kit %s:\n", wb_assets_kit_name(lib,k));
            for (int mm = 0; mm < wb_assets_model_count(lib,k); mm++)
                printf("  %s\n", wb_assets_model_name(lib,k,mm));
        }
        wb_assets_close(lib);
        return 0;
    }
    if (strcmp(cmd, "cgi-render") == 0) {
        /* cgi-render <out.mp4> <start> <dur> */
        if (!g_cgi) { fprintf(stderr, "cgi: empty scene\n"); return -1; }
        char out[1024];
        double start = 0, dur = 2.0;
        sscanf(rest, "%1023s %lf %lf", out, &start, &dur);
        wb_cgi_overlay ov = { .anim = g_cgi, .t_start = start, .duration = dur };
        int rc = wb_video_export_cgi(s, e, out, NULL, WB_VIDEO_CODEC_H264, &ov);
        printf("cgi: render rc=%d -> %s\n", rc, out);
        return rc;
    }
    return -2;   /* not a cgi command */
}

int wb_agent_command(wb_session *s, wb_engine *e, const char *line) {
    char buf[2048];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *p = trim(buf);
    if (*p == '\0' || *p == '#') return 0;
    char *cmd = tok(&p);

    if (strcmp(cmd, "import") == 0) {
        char *src = tok(&p);
        return do_import(s, src);
    }
    if (strcmp(cmd, "split") == 0) {
        wb_agent_checkpoint(s);   /* R062: undoable */
        int track = atoi(tok(&p));
        int clip  = atoi(tok(&p));
        double t  = atof(tok(&p));
        return wb_session_split_video_clip(s, track, clip, t) >= 0 ? 0 : -1;
    }
    if (strcmp(cmd, "quality") == 0) {
        double q = atof(tok(&p));
        wb_compositor_set_quality(q);
        return 0;
    }
    if (strcmp(cmd, "edl") == 0) {
        return wb_session_export_edl(s, tok(&p));
    }
    if (strcmp(cmd, "fcpxml") == 0) {
        return wb_session_export_fcpxml(s, tok(&p));
    }
    if (strcmp(cmd, "export") == 0) {
        char *out = tok(&p);
        char *srt = tok(&p);
        if (!srt || *srt == '\0') srt = NULL;
        return wb_video_export(s, e, out, srt);
    }
    /* R059: CGI pipeline commands — the AGI face of the MiniBlender.
     *
     *   cgi-box <x> <y> <z> <size> <r> <g> <b>
     *   cgi-sphere <x> <y> <z> <r> ...
     *   (primitives append to the current cgi scene)
     *   cgi-key <obj> <t> <px> <py> <pz> <scale> [ease]
     *   cgi-render <out.mp4> <start> <dur>  -> wb_video_export_cgi
     *   cgi-list                             -> list kits/models to stdout
     */
    /* R062: agent session state — introspection, undo, shadow bin */
    if (strcmp(cmd, "state") == 0) {
        printf("session: bpm=%.1f length=%.2fs tracks=%u markers=%d\n",
               s->bpm, s->length / WB_SAMPLE_RATE,
               s->track_count, s->marker_count);
        for (uint32_t t = 0; t < s->track_count; t++) {
            wb_track *tr = &s->tracks[t];
            const char *kind = tr->kind == 3 ? "video" :
                               tr->kind == 1 ? "audio" : "instr";
            printf(" track[%u] %s \"%s\" vol=%.2f clips=%u\n",
                   t, kind, tr->name, tr->volume, tr->clip_count);
            for (uint32_t c = 0; c < tr->clip_count && c < 16; c++) {
                wb_clip *cl = &tr->clips[c];
                if (cl->type == 2 && cl->video)
                    printf("   clip[%u] video start=%.2fs len=%.2fs src=%s\n",
                           c, cl->start, cl->length,
                           cl->video->source_path[0] ?
                               cl->video->source_path : "?");
                else
                    printf("   clip[%u] type=%d start=%.2f\n",
                           c, cl->type, cl->start);
            }
        }
        return 0;
    }
    if (strcmp(cmd, "agent-undo") == 0 || strcmp(cmd, "undo") == 0) {
        if (!g_agent_undo) {
            printf("undo: nothing to undo\n");
            return 0;   /* R062: idempotent — no-op is success for agents */
        }
        wb_session *restored = NULL;
        int did = wb_undo_undo(g_agent_undo, &restored);
        if (did != 1 || !restored) {
            printf("undo: nothing to undo\n");
            return 0;   /* idempotent */
        }
        /* copy restored content into the caller's session in place */
        wb_session *tmp = wb_session_copy(restored);
        if (tmp) { *s = *tmp; free(tmp); }
        printf("undo: restored earlier state\n");
        return 0;
    }
    if (strcmp(cmd, "checkpoint") == 0) {
        wb_agent_checkpoint(s);
        return 0;
    }
    if (strcmp(cmd, "shadow-save") == 0) {
        char path[1200];
        char *out = tok(&p);   /* optional explicit path */
        if (out && out[0]) snprintf(path, sizeof path, "%s", out);
        else wb_shadowbin_path_for(s->name[0] ? s->name : "project",
                                   path, sizeof path);
        int rc = wb_shadowbin_write(s, path);
        printf("shadowbin: wrote %s (rc=%d)\n", path, rc);
        return rc;
    }
    if (strcmp(cmd, "shadow-load") == 0) {
        char path[1200];
        char *in = tok(&p);
        if (!in || !in[0]) { fprintf(stderr,"ERR:usage: shadow-load <path>\n"); return -1; }
        snprintf(path, sizeof path, "%s", in);
        int n = wb_shadowbin_read(s, path);
        if (n < 0) { fprintf(stderr,"ERR:parse:%s\n", path); return -1; }
        printf("shadowbin: restored %d clips\n", n);
        return 0;
    }
    if (strncmp(cmd, "cgi-", 4) == 0) {
        return cgi_command(s, e, cmd, p);
    }
    if (strcmp(cmd, "polish") == 0) {
        /* polish <src.wav> <out.wav> <lufs> : two-pass voice polish (G8) */
        char *src = tok(&p);
        char *out = tok(&p);
        double lufs = atof(tok(&p));
        /* load wav, polish, write */
        int sr = 0, ch = 0; uint32_t frames = 0;
        float *data = NULL;
        if (wb_wav_read_pcm16(src, &data, &frames, &ch, &sr) != 0 || !data)
            return -1;
        wb_voice_polish_apply_twopass(data, frames, ch, (float)sr, (float)lufs);
        int rc = wb_wav_write_pcm16(out, data, frames, ch, sr);
        free(data);
        return rc;
    }
    if (strcmp(cmd, "quit") == 0) return 0;
    fprintf(stderr, "wb_agent: unknown command '%s'\n", cmd);
    return -1;
}

int wb_agent_run(FILE *in, wb_session *s, wb_engine *e) {
    if (!in || !s || !e) return -1;
    char line[2048];
    int rc = 0;
    while (fgets(line, sizeof(line), in)) {
        if (wb_agent_command(s, e, line) != 0) rc = -1;
    }
    return rc;
}
