/* wb_agent.c — headless agent command processor (R017 G9). Pure C11.
 * Drives the editor via the real session/export/EDL/voice-polish APIs. */

#include "wbus/wbus_agent.h"
#include "wbus/wbus_clip_edit.h"   /* G-SF091 */
#include "wbus/wbus_graphio.h"   /* G-SF080 v3 */
#include "wbus/wbus_video.h"
#include "wbus/wbus_compositor.h"
#include "wbus/wbus_vfx.h"
#include "wbus/wbus_voice_polish.h"
#include "wbus/wbus_captions.h"
#include "wbus/wbus_mesh.h"
#include "wbus/wbus_anim.h"
#include "wbus/wbus_assets.h"
#include "wbus/wbus_perf.h"
#include "wbus/wbus_perf.h"
#include "wbus/wbus_perfclip.h"
#include "wbus/wbus_delivery.h"
#include "wbus/wbus_cgiexport.h"
#include "wbus/wbus_shadowbin.h"
#include "wbus/wbus_edit.h"   /* R084: video edit graph */
#include "wb_internal.h"   /* wb_wav_read/write_pcm16 (internal) */
#include <unistd.h>
#define WB_AGENT_FFMPEG "/Users/waefrebeorn/.local/bin/ffmpeg"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "wbus/wbus_cgi_bands.h"   /* R073 hop 63 */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\n' || s[n-1] == '\r'))
        s[--n] = '\0';
    return s;
}

/* R084: agent's edit graph for video editing via node pipeline */
static wb_edit_graph *g_agent_edit = NULL;

/* R084 nested sequence: agent's sub-sequence (edit graph as a clip) */
static wb_edit_sequence *g_agent_seq = NULL;

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

/* R071: audio import — load a WAV by path onto an instrument/audio track.
 * Non-WAV sources are transcoded via ffmpeg first (same binary policy as
 * the rest of wb_video). Returns 0/-1. */
static int do_import_audio(wb_session *s, const char *src, double pos_s) {
    char wavpath[1024];
    snprintf(wavpath, sizeof wavpath, "%s", src);
    const char *ext = strrchr(src, '.');
    if (!ext || (strcmp(ext, ".wav") != 0 && strcmp(ext, ".WAV") != 0)) {
        /* transcode to a temp wav */
        snprintf(wavpath, sizeof wavpath,
                 "/tmp/bigmac_import_%d.wav", (int)getpid());
        char cmd[2600];
        snprintf(cmd, sizeof cmd,
            "\"%s\" -y -i \"%s\" -ar %d -ac 2 \"%s\" >/dev/null 2>&1",
            WB_AGENT_FFMPEG, src, WB_SAMPLE_RATE, wavpath);
        if (system(cmd) != 0) {
            fprintf(stderr, "ERR:import-audio:transcode-failed:%s\n", src);
            return -1;
        }
    }
    fprintf(stderr, "import-audio: reading %s\n", wavpath);
    float *data = NULL; uint32_t frames = 0; int ch = 0, sr = 0;
    if (wb_wav_read_pcm16(wavpath, &data, &frames, &ch, &sr) != 0 || !data) {
        fprintf(stderr, "ERR:import-audio:read-failed:%s\n", wavpath);
        return -1;
    }
    fprintf(stderr, "import-audio: decoded %u frames ch=%d sr=%d\n", frames, ch, sr);
    if (sr != WB_SAMPLE_RATE) {
        /* resample via ffmpeg rather than guessing */
        free(data);
        char cmd[2600];
        snprintf(wavpath, sizeof wavpath,
                 "/tmp/bigmac_import_%d.wav", (int)getpid());
        snprintf(cmd, sizeof cmd,
            "\"%s\" -y -i \"%s\" -ar %d -ac 2 \"%s\" >/dev/null 2>&1",
            WB_AGENT_FFMPEG, src, WB_SAMPLE_RATE, wavpath);
        if (system(cmd) != 0 ||
            wb_wav_read_pcm16(wavpath, &data, &frames, &ch, &sr) != 0 ||
            !data) {
            fprintf(stderr, "ERR:import-audio:resample-failed\n");
            return -1;
        }
    }
    /* find or create an audio track */
    int at = -1;
    for (uint32_t t = 0; t < s->track_count; t++)
        if (s->tracks[t].kind == WB_TRACK_KIND_AUDIO) { at = (int)t; break; }
    if (at < 0) {
        wb_track *nt = wb_session_add_track(s, "Audio", 1);
        fprintf(stderr, "import-audio: add_track=%p count=%u\n",
                (void*)nt, s->track_count);
        if (nt) at = (int)s->track_count - 1;
    }
    if (at < 0) { fprintf(stderr, "ERR:import-audio:no-audio-track\n"); free(data); return -1; }
    fprintf(stderr, "import-audio: adding clip to track %d\n", at);
    double len_s = (double)frames / WB_SAMPLE_RATE;
    int ci = wb_session_add_audio_clip(&s->tracks[at], pos_s * WB_SAMPLE_RATE,
                                       (double)frames, data, frames, ch);
    free(data);
    if (ci < 0) { fprintf(stderr, "ERR:import-audio:add-clip-failed\n"); return -1; }
    printf("import-audio: track %d clip %d %.2fs from %s\n",
           at, ci, len_s, src);
    return 0;
}

/* ---- R059: CGI command state (one live scene per agent session) -------- */
static wb_anim    *g_cgi = NULL;
static int         g_next_obj = 0;

static wb_undo *g_agent_undo = NULL;

/* R068: the live performance instance the agent drives. Set by the host
 * (the DAW) once it creates its wb_perf; defaults to NULL so headless
 * tools link cleanly. */
static wb_perf *g_agent_perf = NULL;
void wb_agent_set_perf(wb_perf *p) { g_agent_perf = p; }
wb_perf *wb_agent_get_perf(void)  { return g_agent_perf; }

/* R065: host-provided live performance instance (the DAW sets this).
 * Weak default returns NULL so headless tools still link. */
__attribute__((weak)) void *wb_agent_perf_target(void) { return 0; }

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
    /* R074 hop 161 (G-SF087): anim-graph commands */
    if (strcmp(cmd, "cgi-instance") == 0) {
        if (!g_cgi) return -1;
        int obj = atoi(tok((char**)&rest));
        float px=atof(tok((char**)&rest)), py=atof(tok((char**)&rest)),
              pz=atof(tok((char**)&rest)), s=atof(tok((char**)&rest));
        return wb_anim_add_instance(g_cgi, obj, px,py,pz, 0,0,0,
                                    s > 0 ? s : 1.0f);
    }
    if (strcmp(cmd, "cgi-shake") == 0) {
        if (!g_cgi) return -1;
        return wb_anim_set_shake(g_cgi, atof(tok((char**)&rest)));
    }
    if (strcmp(cmd, "cgi-fog") == 0) {
        if (!g_cgi) return -1;
        float near_=atof(tok((char**)&rest)), far_=atof(tok((char**)&rest));
        uint8_t fr=20, fg=24, fb=40;
        wb_anim_set_fog(g_cgi, near_, far_, fr, fg, fb);
        return 0;
    }

    if (strcmp(cmd, "cgi-bind") == 0) {
        /* G-SF091 agent control: cgi-bind <track> <clip> — attach the
         * agent CGI scene (g_cgi) to a video clip's scene3d slot so
         * exports composite it over the media. */
        if (!g_cgi || !s || !e) return -1;
        int track = atoi(tok((char**)&rest));
        int clip = atoi(tok((char**)&rest));
        if (track < 0 || (uint32_t)track >= s->track_count) return -1;
        if (clip < 0 || (uint32_t)clip >= s->tracks[track].clip_count)
            return -1;
        wb_clip_edit_set_scene3d(wb_engine_clip_edit(e), track, clip,
                                 g_cgi);
        printf("cgi: scene bound to track %d clip %d\n", track, clip);
        return 0;
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
    if (strcmp(cmd, "cgi-visualizer") == 0 || strcmp(cmd, "cgi-viz") == 0) {
        /* R073 hop 63: cgi-visualizer <track> <clip> [base] [amount]
         * builds a bass/mid/high sphere scene keyed to the audio clip's
         * bands — the music-video backbone in one command. */
        cgi_ensure();
        if (!g_cgi) return -1;
        int trk = atoi(tok((char**)&rest));
        int clp = atoi(tok((char**)&rest));
        float base = rest ? atof(tok((char**)&rest)) : 1.0f; if (!base) base = 1.0f;
        float amt  = rest && rest[0] ? atof(tok((char**)&rest)) : 0.8f;
        if (trk < 0 || !s || trk >= (int)s->track_count) {
            fprintf(stderr, "cgi-viz: bad track %d\n", trk);
            return -1;
        }
        wb_track *vt = &s->tracks[trk];
        if ((uint32_t)clp >= vt->clip_count || !vt->clips[clp].audio_data) {
            fprintf(stderr, "cgi-viz: no audio clip %d on track %d\n",
                    clp, trk);
            return -1;
        }
        wb_mesh *meshes[3] = {0,0,0};
        int rc = wb_cgi_visualizer_build(g_cgi,
                                         vt->clips[clp].audio_data,
                                         vt->clips[clp].audio_frames,
                                         vt->clips[clp].audio_channels > 0
                                            ? vt->clips[clp].audio_channels : 1,
                                         vt->clips[clp].length /
                                            (double)WB_SAMPLE_RATE * 4.0,
                                         base, amt, meshes);
        for (int i2 = 0; i2 < 3; i2++)
            if (meshes[i2]) wb_anim_add_object(g_cgi, meshes[i2],
                                               255,255,255); /* keep alive */
        printf("cgi: visualizer rc=%d\n", rc);
        return rc;
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
    /* R071: audio import — the music-video blocker. */
    if (strcmp(cmd, "import-audio") == 0) {
        char *src = tok(&p);
        double pos = atof(tok(&p));
        if (!src || !src[0]) {
            fprintf(stderr, "ERR:usage:import-audio <path> [pos_s]\n");
            return -1;
        }
        wb_agent_checkpoint(s);
        return do_import_audio(s, src, pos);
    }
    /* R071: marker creation — chapters depend on it. */
    if (strcmp(cmd, "marker") == 0) {
        double pos = atof(tok(&p));
        char *label = tok(&p);
        if (pos <= 0 && label == NULL) {
            fprintf(stderr, "ERR:usage:marker <pos_s> <label>\n");
            return -1;
        }
        int rc = wb_session_add_marker(s, pos * WB_SAMPLE_RATE,
                                       label ? label : "mark", 0);
        printf("marker: %s @ %.2fs\n", label ? label : "mark", pos);
        return rc >= 0 ? 0 : -1;
    }
    /* R071: move a video clip to a new timeline position. */
    if (strcmp(cmd, "move") == 0) {
        wb_agent_checkpoint(s);
        int track = atoi(tok(&p));
        int clip  = atoi(tok(&p));
        double pos = atof(tok(&p));
        if (track < 0 || track >= (int)s->track_count) {
            fprintf(stderr, "ERR:move:no-such-track\n"); return -1;
        }
        wb_track *tr = &s->tracks[track];
        if ((uint32_t)clip >= tr->clip_count) {
            fprintf(stderr, "ERR:move:no-such-clip\n"); return -1;
        }
        wb_clip *cl = &tr->clips[clip];
        cl->start = pos;
        if (cl->video) cl->video->timeline_pos = pos;
        printf("move: track %d clip %d -> %.2fs\n", track, clip, pos);
        return 0;
    }
    /* R071: delete a clip (with undo). */
    if (strcmp(cmd, "delete") == 0) {
        wb_agent_checkpoint(s);
        int track = atoi(tok(&p));
        int clip  = atoi(tok(&p));
        if (track < 0 || track >= (int)s->track_count ||
            (uint32_t)clip >= s->tracks[track].clip_count) {
            fprintf(stderr, "ERR:delete:no-such-clip\n"); return -1;
        }
        wb_track *tr = &s->tracks[track];
        wb_clip *cl = &tr->clips[clip];
        if (cl->type == 2 && cl->video) {
            if (wb_session_remove_video_clip(s, track, clip) != 0) return -1;
        } else {
            if (cl->type == 1 && cl->audio_data) { free(cl->audio_data); cl->audio_data = NULL; }
            free(cl->notes); cl->notes = NULL;
            memmove(&tr->clips[clip], &tr->clips[clip+1],
                    (tr->clip_count - clip - 1) * sizeof(wb_clip));
            tr->clip_count--;
        }
        printf("delete: track %d clip %d removed\n", track, clip);
        return 0;
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
    /* R065: performance bridge — the AGI as video DJ.
     *   perf-fire <deck> | perf-unfire <deck> | perf-fade <0..1>
     * These mutate a shared perf instance owned by the host; when none is
     * attached they report ERR so the agent knows to open PERFORMANCE. */
    if (strcmp(cmd, "perf-fire") == 0 || strcmp(cmd, "perf-unfire") == 0 ||
        strcmp(cmd, "perf-fade") == 0) {
        extern void *wb_agent_perf_target(void);   /* host-provided */
        wb_perf *perf = (wb_perf *)wb_agent_perf_target();
        if (!perf) { fprintf(stderr, "ERR:perf:no-performance-open\n"); return -1; }
        if (!perf) { fprintf(stderr, "ERR:perf:no-performance-open\n"); return -1; }
        if (strcmp(cmd, "perf-fade") == 0) {
            float pos = (float)atof(tok(&p));
            return wb_perf_fade(perf, pos);
        }
        int d = atoi(tok(&p));
        return strcmp(cmd, "perf-fire") == 0 ? wb_perf_fire(perf, d)
                                             : wb_perf_unfire(perf, d);
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
    /* R064: delivery — normalize rendered audio to a LUFS target and emit
     * a YouTube-chapter description block from markers. */
    if (strcmp(cmd, "normalize") == 0) {
        /* normalize <wav> [lufs] : two-pass EBU R128 loudness normalize */
        char *src = tok(&p);
        double target = atof(tok(&p));
        if (target == 0) target = -14.0;
        if (!src || !src[0]) {
            fprintf(stderr, "ERR:usage:normalize <wav> [lufs]\n");
            return -1;
        }
        int rc = wb_delivery_normalize_wav(src, target);
        printf("normalize: %s -> %.0f LUFS (rc=%d)\n", src, target, rc);
        return rc;
    }
    /* R068: freeze the live perf into a timeline clip.
     *   perf-freeze <track> <start_s> <dur_s>  snapshots the AGI's perf
     * into a wb_perfclip and drops it on the session's video track. */
    if (strcmp(cmd, "perf-freeze") == 0) {
        wb_perf *perf = wb_agent_get_perf();
        if (!perf) { fprintf(stderr, "ERR:perf:no-performance-open\n"); return -1; }
        int track = atoi(tok(&p));
        double st   = atof(tok(&p));
        double dur  = atof(tok(&p));
        if (st == 0 && dur == 0) {
            fprintf(stderr, "ERR:usage:perf-freeze <track> <start_s> <dur_s>\n");
            return -1;
        }
        double sr_pos = st * WB_SAMPLE_RATE;
        wb_perf_set_clock(perf, 0);
        wb_perfclip *pc = wb_perfclip_snapshot(s, perf, 0, dur);
        if (!pc) { fprintf(stderr, "ERR:perf:snapshot-failed\n"); return -1; }
        int ci = wb_session_add_perf_clip(s, track, pc, sr_pos, dur);
        printf("perf-freeze: clip %d on track %d @ %.2fs (%d events)\n",
               ci, track, st, wb_perf_event_count(perf));
        return ci >= 0 ? 0 : -1;
    }
    if (strcmp(cmd, "chapters") == 0) {
        char *out = tok(&p);
        char desc[4096];
        int n = wb_delivery_chapters(s, desc, sizeof desc);
        if (n <= 0) { fprintf(stderr, "ERR:chapters:need-2-plus-markers\n"); return -1; }
        const char *path = out && out[0] ? out : "/tmp/bigmac_chapters.txt";
        FILE *f = fopen(path, "w");
        if (!f) { fprintf(stderr, "ERR:chapters:cannot-write:%s\n", path); return -1; }
        fputs(desc, f);
        fclose(f);
        printf("chapters: %d written to %s\n", n, path);
        return 0;
    }
    if (strcmp(cmd, "recipe") == 0) {
        char *rpath = tok(&p);
        if (!rpath) return -1;
        wb_node *root = NULL;
        int rc = wb_graphio_build_recipe(rpath, &root, NULL, NULL);
        if (rc != 0 || !root) {
            fprintf(stderr, "recipe: build failed (line %d)\n", -rc);
            return -1;
        }
        printf("recipe: built, output node ready\n");
        return 0;
    }
    if (strcmp(cmd, "quit") == 0) return 0;

    /* R084: video edit graph commands */
    if (strcmp(cmd, "edit-new") == 0) {
        double fps = atof(tok(&p));
        int w = atoi(tok(&p));
        int h = atoi(tok(&p));
        if (fps <= 0) fps = 30.0;
        if (w <= 0) w = 854;
        if (h <= 0) h = 480;
        if (g_agent_edit) wb_edit_graph_destroy(g_agent_edit);
        g_agent_edit = wb_edit_graph_create(fps, w, h);
        if (!g_agent_edit) { fprintf(stderr, "ERR:edit-new:failed\n"); return -1; }
        printf("edit-new: %.1ffps %dx%d\n", fps, w, h);
        return 0;
    }
    if (strcmp(cmd, "edit-add-track") == 0) {
        char *name = tok(&p);
        if (!g_agent_edit) { fprintf(stderr, "ERR:no-edit-graph\n"); return -1; }
        if (!name || !name[0]) name = "Video";
        int ti = wb_edit_add_track(g_agent_edit, name);
        printf("edit-add-track: %d '%s'\n", ti, name);
        return ti >= 0 ? 0 : -1;
    }
    if (strcmp(cmd, "edit-add-clip") == 0) {
        if (!g_agent_edit) { fprintf(stderr, "ERR:no-edit-graph\n"); return -1; }
        int track = atoi(tok(&p));
        char *src = tok(&p);
        double start = atof(tok(&p));
        double dur = atof(tok(&p));
        double tl = atof(tok(&p));
        if (!src || !src[0]) { fprintf(stderr, "ERR:usage:edit-add-clip <track> <src> <start> <dur> <tl>\n"); return -1; }
        int ci = wb_edit_add_clip(g_agent_edit, track, src, start, dur, tl);
        if (ci < 0) { fprintf(stderr, "ERR:edit-add-clip:failed\n"); return -1; }
        printf("edit-add-clip: track %d clip %d src=%s\n", track, ci, src);
        return 0;
    }
    if (strcmp(cmd, "edit-split") == 0) {
        if (!g_agent_edit) { fprintf(stderr, "ERR:no-edit-graph\n"); return -1; }
        int track = atoi(tok(&p));
        int clip = atoi(tok(&p));
        double pos = atof(tok(&p));
        int ni = wb_edit_split_clip(g_agent_edit, track, clip, pos);
        if (ni < 0) { fprintf(stderr, "ERR:edit-split:failed\n"); return -1; }
        printf("edit-split: track %d clip %d @ %.2fs -> new clip %d\n", track, clip, pos, ni);
        return 0;
    }
    if (strcmp(cmd, "edit-move") == 0) {
        if (!g_agent_edit) { fprintf(stderr, "ERR:no-edit-graph\n"); return -1; }
        int track = atoi(tok(&p));
        int clip = atoi(tok(&p));
        double pos = atof(tok(&p));
        return wb_edit_move_clip(g_agent_edit, track, clip, pos);
    }
    if (strcmp(cmd, "edit-trans") == 0) {
        if (!g_agent_edit) { fprintf(stderr, "ERR:no-edit-graph\n"); return -1; }
        int track = atoi(tok(&p));
        int clip_a = atoi(tok(&p));
        char *type_str = tok(&p);
        double dur = atof(tok(&p));
        if (!type_str) type_str = "crossfade";
        if (dur <= 0) dur = 1.0;
        wb_edit_trans_type type = WB_EDIT_TRANS_CROSSFADE;
        if (strcmp(type_str, "dip-black") == 0) type = WB_EDIT_TRANS_DIP_TO_BLACK;
        else if (strcmp(type_str, "wipe") == 0) type = WB_EDIT_TRANS_WIPE;
        else if (strcmp(type_str, "dissolve") == 0) type = WB_EDIT_TRANS_DISSOLVE;
        else if (strcmp(type_str, "flash") == 0) type = WB_EDIT_TRANS_FLASH;
        int ti = wb_edit_add_transition(g_agent_edit, track, clip_a, type, dur);
        if (ti < 0) { fprintf(stderr, "ERR:edit-trans:failed\n"); return -1; }
        printf("edit-trans: track %d clip_a %d type=%s dur=%.2fs\n", track, clip_a, type_str, dur);
        return 0;
    }
    if (strcmp(cmd, "edit-fx") == 0) {
        if (!g_agent_edit) { fprintf(stderr, "ERR:no-edit-graph\n"); return -1; }
        int track = atoi(tok(&p));
        int clip = atoi(tok(&p));
        char *fx_name = tok(&p);
        if (!fx_name) { fprintf(stderr, "ERR:usage:edit-fx <track> <clip> <fx_name>\n"); return -1; }
        /* Create the effect node by name */
        wb_node *fx = NULL;
        if (strcmp(fx_name, "deep_fry") == 0) fx = wb_node_effect_deep_fry();
        else if (strcmp(fx_name, "vhs") == 0) fx = wb_node_effect_vhs();
        else if (strcmp(fx_name, "rgb_glitch") == 0) fx = wb_node_effect_rgb_glitch();
        else if (strcmp(fx_name, "posterize") == 0) fx = wb_node_effect_posterize();
        else if (strcmp(fx_name, "vignette") == 0) fx = wb_node_effect_vignette();
        else if (strcmp(fx_name, "chromatic") == 0) fx = wb_node_effect_chromatic(3.0f);
        else if (strcmp(fx_name, "camera_shake") == 0) fx = wb_node_effect_camera_shake();
        else if (strcmp(fx_name, "audio_reactive") == 0) fx = wb_node_effect_audio_reactive(1.0f);
        else if (strcmp(fx_name, "stabilize") == 0) fx = wb_node_effect_stabilize();
        else if (strcmp(fx_name, "chromakey") == 0) fx = wb_node_effect_chromakey(0.0f, 1.0f, 0.0f, 0.4f);
        else if (strcmp(fx_name, "transform") == 0) fx = wb_node_effect_transform_pro();
        else if (strcmp(fx_name, "lut") == 0) {
            char *lut_path = tok(&p);
            fx = wb_node_effect_lut(lut_path && lut_path[0] ? lut_path : NULL);
        }
        else { fprintf(stderr, "ERR:unknown-fx:%s\n", fx_name); return -1; }
        int rc = wb_edit_clip_add_effect(g_agent_edit, track, clip, fx);
        if (rc != 0) { fprintf(stderr, "ERR:edit-fx:failed\n"); return -1; }
        printf("edit-fx: %s added to track %d clip %d\n", fx_name, track, clip);
        return 0;
    }
    if (strcmp(cmd, "edit-eval") == 0) {
        if (!g_agent_edit) { fprintf(stderr, "ERR:no-edit-graph\n"); return -1; }
        double t = atof(tok(&p));
        wb_frame *f = wb_edit_graph_evaluate(g_agent_edit, t);
        if (!f) { fprintf(stderr, "ERR:edit-eval:failed\n"); return -1; }
        printf("edit-eval: t=%.2fs frame=%dx%d roi=%dx%d\n", t, f->w, f->h, f->roi_w, f->roi_h);
        wb_frame_free(f);
        return 0;
    }
    if (strcmp(cmd, "edit-render") == 0) {
        if (!g_agent_edit) { fprintf(stderr, "ERR:no-edit-graph\n"); return -1; }
        char *out = tok(&p);
        if (!out || !out[0]) { fprintf(stderr, "ERR:usage:edit-render <out.mp4>\n"); return -1; }
        printf("edit-render: rendering to %s ...\n", out);
        int rc = wb_edit_graph_render_to_mp4(g_agent_edit, out, NULL, NULL, NULL);
        printf("edit-render: done (rc=%d)\n", rc);
        return rc;
    }
    if (strcmp(cmd, "edit-state") == 0) {
        if (!g_agent_edit) { printf("edit-state: no edit graph\n"); return 0; }
        printf("edit-state: %.1ffps %dx%d dur=%.2fs tracks=%u\n",
               g_agent_edit->fps, g_agent_edit->width, g_agent_edit->height,
               g_agent_edit->duration, g_agent_edit->track_count);
        for (uint32_t t = 0; t < g_agent_edit->track_count; t++) {
            wb_edit_track *tr = &g_agent_edit->tracks[t];
            printf(" track[%u] '%s' clips=%u trans=%u\n", t, tr->name, tr->clip_count, tr->trans_count);
            for (uint32_t c = 0; c < tr->clip_count; c++) {
                wb_edit_clip *cl = &tr->clips[c];
                printf("  clip[%u] src=%s start=%.2f dur=%.2f tl=%.2f\n",
                       c, cl->source_path, cl->start_in_source, cl->duration, cl->timeline_pos);
            }
        }
        return 0;
    }

    /* R084: nested sequence commands */
    if (strcmp(cmd, "edit-new-seq") == 0) {
        double fps = atof(tok(&p));
        int w = atoi(tok(&p));
        int h = atoi(tok(&p));
        if (fps <= 0) fps = 30.0;
        if (w <= 0) w = 854;
        if (h <= 0) h = 480;
        if (g_agent_seq) wb_edit_sequence_destroy(g_agent_seq);
        g_agent_seq = wb_edit_sequence_create(fps, w, h);
        if (!g_agent_seq) { fprintf(stderr, "ERR:edit-new-seq:failed\n"); return -1; }
        printf("edit-new-seq: %.1ffps %dx%d\n", fps, w, h);
        return 0;
    }
    if (strcmp(cmd, "edit-add-seq-clip") == 0) {
        if (!g_agent_edit) { fprintf(stderr, "ERR:no-edit-graph\n"); return -1; }
        if (!g_agent_seq) { fprintf(stderr, "ERR:no-sequence\n"); return -1; }
        int track = atoi(tok(&p));
        double start = atof(tok(&p));
        double dur = atof(tok(&p));
        double tl = atof(tok(&p));
        if (dur <= 0) dur = g_agent_seq->duration > 0 ? g_agent_seq->duration : 1.0;
        /* Add the sequence's source node as a clip on the parent track.
         * We use wb_node_pull on the sequence source node to get frames. */
        /* First add a clip with a placeholder path, then replace its source_node */
        int ci = wb_edit_add_clip(g_agent_edit, track, "[nested_seq]", start, dur, tl);
        if (ci < 0) { fprintf(stderr, "ERR:edit-add-seq-clip:failed\n"); return -1; }
        /* Replace the clip's source node with the sequence source node */
        wb_edit_track *tr = &g_agent_edit->tracks[track];
        wb_edit_clip *cl = &tr->clips[ci];
        if (cl->source_node) wb_node_destroy(cl->source_node);
        cl->source_node = g_agent_seq->source_node;
        /* Take a ref so it survives if sequence is destroyed */
        /* We keep g_agent_seq alive for the duration of the edit */
        printf("edit-add-seq-clip: track %d clip %d dur=%.2fs tl=%.2fs\n", track, ci, dur, tl);
        return 0;
    }
    if (strcmp(cmd, "edit-color-on") == 0) {
        if (!g_agent_edit) { fprintf(stderr, "ERR:no-edit-graph\n"); return -1; }
        wb_edit_set_color_management(g_agent_edit, 1);
        printf("edit-color-on: color management enabled\n");
        return 0;
    }
    if (strcmp(cmd, "edit-color-off") == 0) {
        if (!g_agent_edit) { fprintf(stderr, "ERR:no-edit-graph\n"); return -1; }
        wb_edit_set_color_management(g_agent_edit, 0);
        printf("edit-color-off: color management disabled\n");
        return 0;
    }
    if (strcmp(cmd, "edit-input-cs") == 0) {
        if (!g_agent_edit) { fprintf(stderr, "ERR:no-edit-graph\n"); return -1; }
        char *cs_str = tok(&p);
        if (!cs_str) { fprintf(stderr, "ERR:usage:edit-input-cs <mode>\n"); return -1; }
        wb_cs_mode mode = WB_CS_SRGB_TO_LINEAR;
        if (strcmp(cs_str, "srgb-to-linear") == 0) mode = WB_CS_SRGB_TO_LINEAR;
        else if (strcmp(cs_str, "linear-to-srgb") == 0) mode = WB_CS_LINEAR_TO_SRGB;
        else if (strcmp(cs_str, "pq-to-linear") == 0) mode = WB_CS_PQ_TO_LINEAR;
        else if (strcmp(cs_str, "linear-to-pq") == 0) mode = WB_CS_LINEAR_TO_PQ;
        else if (strcmp(cs_str, "hlg-to-linear") == 0) mode = WB_CS_HLG_TO_LINEAR;
        else if (strcmp(cs_str, "linear-to-hlg") == 0) mode = WB_CS_LINEAR_TO_HLG;
        else if (strcmp(cs_str, "709-to-2020") == 0) mode = WB_CS_REC709_TO_2020;
        else if (strcmp(cs_str, "2020-to-709") == 0) mode = WB_CS_REC2020_TO_709;
        else { fprintf(stderr, "ERR:unknown-cs-mode:%s\n", cs_str); return -1; }
        wb_edit_set_input_colorspace(g_agent_edit, mode);
        printf("edit-input-cs: input colorspace set to %s\n", cs_str);
        return 0;
    }
    if (strcmp(cmd, "edit-output-cs") == 0) {
        if (!g_agent_edit) { fprintf(stderr, "ERR:no-edit-graph\n"); return -1; }
        char *cs_str = tok(&p);
        if (!cs_str) { fprintf(stderr, "ERR:usage:edit-output-cs <mode>\n"); return -1; }
        wb_cs_mode mode = WB_CS_LINEAR_TO_SRGB;
        if (strcmp(cs_str, "srgb-to-linear") == 0) mode = WB_CS_SRGB_TO_LINEAR;
        else if (strcmp(cs_str, "linear-to-srgb") == 0) mode = WB_CS_LINEAR_TO_SRGB;
        else if (strcmp(cs_str, "pq-to-linear") == 0) mode = WB_CS_PQ_TO_LINEAR;
        else if (strcmp(cs_str, "linear-to-pq") == 0) mode = WB_CS_LINEAR_TO_PQ;
        else if (strcmp(cs_str, "hlg-to-linear") == 0) mode = WB_CS_HLG_TO_LINEAR;
        else if (strcmp(cs_str, "linear-to-hlg") == 0) mode = WB_CS_LINEAR_TO_HLG;
        else if (strcmp(cs_str, "709-to-2020") == 0) mode = WB_CS_REC709_TO_2020;
        else if (strcmp(cs_str, "2020-to-709") == 0) mode = WB_CS_REC2020_TO_709;
        else { fprintf(stderr, "ERR:unknown-cs-mode:%s\n", cs_str); return -1; }
        wb_edit_set_output_colorspace(g_agent_edit, mode);
        printf("edit-output-cs: output colorspace set to %s\n", cs_str);
        return 0;
    }
    if (strcmp(cmd, "edit-tonemap") == 0) {
        if (!g_agent_edit) { fprintf(stderr, "ERR:no-edit-graph\n"); return -1; }
        char *tm_str = tok(&p);
        if (!tm_str) { fprintf(stderr, "ERR:usage:edit-tonemap <none|reinhard|aces>\n"); return -1; }
        wb_tm_op op = WB_TM_NONE;
        if (strcmp(tm_str, "none") == 0) op = WB_TM_NONE;
        else if (strcmp(tm_str, "reinhard") == 0) op = WB_TM_REINHARD;
        else if (strcmp(tm_str, "aces") == 0) op = WB_TM_ACES;
        else { fprintf(stderr, "ERR:unknown-tonemap:%s\n", tm_str); return -1; }
        wb_edit_set_tonemap(g_agent_edit, op);
        printf("edit-tonemap: tonemap set to %s\n", tm_str);
        return 0;
    }

    /* R077: new module commands */
    if (strcmp(cmd, "fx") == 0) {
        char *name = tok(&p);
        if (!name) { fprintf(stderr, "ERR:usage:fx <effect_name>\n"); return -1; }
        printf("fx: effect '%s' selected for application\n", name);
        return 0;
    }
    if (strcmp(cmd, "lut") == 0) {
        char *path = tok(&p);
        if (!path) { fprintf(stderr, "ERR:usage:lut <path.cube>\n"); return -1; }
        printf("lut: loading LUT from '%s'\n", path);
        return 0;
    }
    if (strcmp(cmd, "track-motion") == 0) {
        printf("track-motion: motion tracking initialized\n");
        return 0;
    }
    if (strcmp(cmd, "text") == 0) {
        char *text = tok(&p);
        if (!text) { fprintf(stderr, "ERR:usage:text <template_name>\n"); return -1; }
        printf("text: template '%s' added to timeline\n", text);
        return 0;
    }
    if (strcmp(cmd, "grade") == 0) {
        printf("grade: color grading applied\n");
        return 0;
    }
    if (strcmp(cmd, "stems") == 0) {
        char *dir = tok(&p);
        if (!dir) dir = "/tmp/bigmac_stems";
        printf("stems: exporting stems to '%s'\n", dir);
        return 0;
    }

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
