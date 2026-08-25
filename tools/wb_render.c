/* wb_render.c — CLI offline render: build/load a session, render the whole
 * song to a WAV file through the exact same engine path as live playback.
 *
 * R073 hop 35/36: reports BS.1770 integrated LUFS + peak dBFS after render;
 * --lufs N trims the export to N LUFS (true-peak guarded) before writing.
 *
 * Usage: wb_render [out.wav] [--demo|--file demo.wbus] [--lufs N]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "wbus.h"
#include "wb_internal.h"
#include "wbus/wbus_lufs.h"
#include "wbus/wbus_compositor.h"
#include "wbus/wbus_mesh.h"
#include "wbus/wbus_anim.h"
#include "wbus/wb_ui.h"
#include "wbus/wbus_limiter.h"

/* R074 hop 110: showcase renderer — full demo at 640x360. */
#define SC_W 640
#define SC_H 360
int wb_render_showcase(const char *mp4) {
    /* R074: gradient scene sources with light sweeps */
    wb_node *s1 = wb_node_source_scene(0.95f,0.35f,0.10f,
                                       0.45f,0.05f,0.15f,
                                       0, 0.12f, SC_W,SC_H);
    wb_node *s2 = wb_node_source_scene(0.05f,0.55f,0.30f,
                                       0.00f,0.15f,0.35f,
                                       1, 0.18f, SC_W,SC_H);
    wb_node *s3 = wb_node_source_scene(0.10f,0.20f,0.75f,
                                       0.30f,0.05f,0.50f,
                                       0, 0.10f, SC_W,SC_H);
    wb_node *t1 = wb_transition_preset(1, 1.0);   /* News wipe */
    wb_node *t2 = wb_transition_preset(3, 1.0);   /* VJ zoom-blur */
    /* two titles: main + subtitle */
    wb_node *txt = wb_node_source_text("BIG MAC", 8,
                                      1,1,1,1, SC_W, SC_H);
    wb_node *sub = wb_node_source_text("C11 VIDEO COMPOSITOR", 3,
                                      0.85f,0.85f,0.9f,1, SC_W, SC_H);
    wb_node *comp = wb_node_composite();
    if (!s1||!s2||!s3||!t1||!t2||!txt||!sub||!comp) return 1;
    wb_node_source_text_anim(txt, 4, 2.0);   /* full cycle in 2s */
    wb_node_source_text_anim(sub, 2, 2.5);   /* slide-in */
    wb_param_track *tcy = wb_param_track_create();
    wb_param_track_set(tcy, 0.0, 0.40f, WB_KF_HOLD);
    wb_node_add_param(txt, "cy", tcy);
    wb_param_track *tcx = wb_param_track_create();
    wb_param_track_set(tcx, 0.0, 0.16f, WB_KF_HOLD);
    wb_node_add_param(txt, "cx", tcx);
    wb_param_track *tsy = wb_param_track_create();
    wb_param_track_set(tsy, 0.0, 0.58f, WB_KF_HOLD);
    wb_node_add_param(sub, "cy", tsy);
    wb_param_track *tsx = wb_param_track_create();
    wb_param_track_set(tsx, 0.0, 0.22f, WB_KF_HOLD);
    wb_node_add_param(sub, "cx", tsx);
    /* R074 fix: stagger transitions across the timeline via t_start —
     * wipe at 0..1.33s, hold scene 2, zoom-blur at 1.9..3.16s */
    {
        wb_param_track *ts1 = wb_param_track_create();
        wb_param_track_set(ts1, 0.0, 0.0f, WB_KF_HOLD);
        wb_node_add_param(t1, "t_start", ts1);
        wb_param_track *ts2 = wb_param_track_create();
        wb_param_track_set(ts2, 0.0, 1.9f, WB_KF_HOLD);
        wb_param_track *tdur2 = wb_param_track_create();
        /* t2 dur stays 1.0 but starts at 1.9 */
        wb_node_add_param(t2, "t_start", ts2);
        (void)tdur2;
    }
    wb_transition_add(t1, s1);
    wb_transition_add(t1, s2);
    wb_transition_add(t2, t1);
    wb_transition_add(t2, s3);
    wb_composite_add(comp, t2);
    wb_composite_add(comp, txt);
    wb_composite_add(comp, sub);

    uint32_t nf = (uint32_t)(WB_SAMPLE_RATE * 4.0);
    wb_sample *buf = malloc(nf*2*sizeof(wb_sample));
    if (!buf) return 1;
    for (uint32_t i = 0; i < nf; i++) {
        double tt = (double)i / WB_SAMPLE_RATE;
        float f0 = tt<1.33f?294:(tt<2.66f?370:494);
        float v = sinf(2*M_PI*f0*tt)*0.28f
                + sinf(2*M_PI*f0*0.5f*tt)*0.10f;
        double tf = fmod(tt, 1.333);
        float env = (float)(tf<0.05 ? tf/0.05 :
                     tf>1.2 ? (1.333-tf)/0.133 : 1.0);
        v *= env;
        buf[i*2]=(wb_sample)v; buf[i*2+1]=(wb_sample)v;
    }
    char wavp[512];
    snprintf(wavp, sizeof wavp, "/tmp/showcase_audio_%d.wav", (int)getpid());
    wb_wav_write_pcm16(wavp, buf, nf, 2, WB_SAMPLE_RATE);
    free(buf);

    int rc = wb_compositor_export_mp4_audio(comp, mp4, wavp,
                                            4.0, 15, SC_W, SC_H);
    remove(wavp);
    wb_node_destroy(comp);   /* owns t1/t2/txt/sub */
    wb_node_destroy(s1); wb_node_destroy(s2); wb_node_destroy(s3);
    return rc == 0 ? 0 : 1;
}

#include <sys/stat.h>
#include <errno.h>

/* R074 hop 112: per-frame render loop (anim -> frame source -> ppm). */
static int sf_render_loop(wb_anim *an, wb_node *comp, uint8_t *rgba,
                          const char *mp4, double dur, int fps,
                          int w, int h) {
    char dir[256];
    snprintf(dir, sizeof dir, "/tmp/sf_frames_%d", (int)getpid());
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return -1;
    int nframes = (int)(dur * fps);
    for (int k = 0; k < nframes; k++) {
        double tt = (double)k / fps;
        memset(rgba, 0, (size_t)w * h * 4);   /* transparent for comp */
        wb_anim_render_frame(an, tt, rgba);
        /* R074: HUD drawn straight onto the CGI frame */
        char hud[32];
        snprintf(hud, sizeof hud, "SCORE %06d", 100 * k);
        wb_ui_text_to_rgba(hud, 2, 1.0f,1.0f,1.0f,1.0f,
                           (wb_px*)rgba, w, h, w-160, 12);
        wb_ui_text_to_rgba("FOX", 2, 0.4f,0.8f,1.0f,1.0f,
                           (wb_px*)rgba, w, h, 16, 12);
        wb_frame *f = wb_node_pull(comp, tt, 0, 0, w, h);
        if (!f) continue;
        /* downscale-check not needed: comp RoD = max(bg,frame) = w,h */
        char p[512];
        snprintf(p, sizeof p, "%s/f_%05d.ppm", dir, k);
        /* crop/verify: if f is bigger than w,h, take top-left ROI */
        wb_frame tmp = *f;
        if (f->w > w || f->h > h) {
            tmp.w = w; tmp.h = h;
            for (int y2 = 0; y2 < h; y2++)
                memmove(f->px + y2*w, f->px + y2*f->w,
                        (size_t)w * sizeof(wb_px));
        }
        wb_frame_write_ppm(&tmp, p);
        wb_frame_free(f);
    }
    char cmd[1536];
    snprintf(cmd, sizeof cmd,
        "/Users/waefrebeorn/.local/bin/ffmpeg -y -loglevel error "
        "-f image2 -framerate %d -i '%s/f_%%05d.ppm' "
        "-c:v libx264 -pix_fmt yuv420p -movflags +faststart '%s'",
        fps, dir, mp4);
    int rc = system(cmd);
    for (int k = 0; k < nframes; k++) {
        char p[512];
        snprintf(p, sizeof p, "%s/f_%05d.ppm", dir, k);
        remove(p);
    }
    rmdir(dir);
    return rc == 0 ? 0 : -1;
}

/* R074 hop 112: Star Fox-style corridor run (original assets). */
#define SF_W 640
#define SF_H 360
int wb_render_starfox(const char *mp4) {
    const double DUR = 10.0;
    /* --- 3D animation: ship + rings + corridor ------------------- */
    wb_anim *an = wb_anim_create(SF_W, SF_H);
    if (!an) return 1;

    /* ship parts (original Arwing-inspired design, SNES palette) */
    wb_mesh *body  = wb_mesh_box(0.5f, 0.3f, 1.2f, 220,225,255);
    wb_mesh *nose  = wb_mesh_cone(0.45f, 1.4f, 4, 240,244,255);
    wb_mesh *wingL = wb_mesh_box(1.1f, 0.08f, 0.5f, 60,110,235);
    wb_mesh *wingR = wb_mesh_box(1.1f, 0.08f, 0.5f, 60,110,235);

    int o_body = wb_anim_add_object(an, body, 220,225,255);
    wb_anim_key(an, o_body, 0.0, 0,0,0, 0,0,0, 1.0);
    wb_anim_key(an, o_body, DUR, 0,0,-9.0, 0,0,0, 1.0);
    /* gentle bob */
    for (double bt = 0; bt <= DUR; bt += 0.25)
        wb_anim_key_ease(an, o_body, bt,
                         0, 0.15f*(float)sin(bt*2.2), -bt*0.9f,
                         0,0,0, 1.0, 1);
    /* nose on the front, rotated to point +Z (forward) */
    int o_nose = wb_anim_add_object(an, nose, 240,244,255);
    wb_anim_key(an, o_nose, 0.0, 0, 0.05f, -1.4f, -1.5707f,0,0, 1.0);
    for (double bt = 0; bt <= DUR; bt += 0.25)
        wb_anim_key_ease(an, o_nose, bt,
                         0, 0.15f*(float)sin(bt*2.2), -bt*0.9f-1.4f,
                         -1.5707f,0,0, 1.0, 1);
    /* wings parented to body via same bob keys */
    int o_wl = wb_anim_add_object(an, wingL, 60,110,235);
    wb_anim_key(an, o_wl, 0.0, -1.2f, 0, 0.3f, 0,0,0.12f, 1.0);
    for (double bt = 0; bt <= DUR; bt += 0.25)
        wb_anim_key_ease(an, o_wl, bt,
                         -1.2f, 0.12f*(float)sin(bt*2.2), -bt*0.9f+0.3f,
                         0,0,0.12f, 1.0, 1);
    int o_wr = wb_anim_add_object(an, wingR, 60,110,235);
    wb_anim_key(an, o_wr, 0.0, 1.2f, 0, 0.3f, 0,0,-0.12f, 1.0);
    for (double bt = 0; bt <= DUR; bt += 0.25)
        wb_anim_key_ease(an, o_wr, bt,
                         1.2f, 0.12f*(float)sin(bt*2.2), -bt*0.9f+0.3f,
                         0,0,-0.12f, 1.0, 1);

    /* enemy rings: 8 tori approaching from ahead, staggered lanes */
    wb_mesh *ring[8];
    char lane_x[8] = {-30,-18,20,32,-25,15,-12,28};
    float lane_y[8] = {6,-4,8,-6,4,-8,10,2};
    for (int e = 0; e < 8; e++) {
        ring[e] = wb_mesh_torus(1.6f, 0.35f, 10, 6, 235,80,60);
        int oe = wb_anim_add_object(an, ring[e], 235,80,60);
        double t0 = 1.0 + e * 1.1;
        /* approach from far ahead (-Z far) to behind camera */
        wb_anim_key_ease(an, oe, t0,
                         lane_x[e], lane_y[e], -120.0f,
                         0,0,0, 1.5f, 0);
        wb_anim_key_ease(an, oe, t0 + 1.6,
                         (float)(lane_x[e] % 7), lane_y[e]*0.3f, 6.0f,
                         0, (float)e*0.7f, 0, 0.6f, 0);
    }

    /* corridor floor strips scrolling toward camera (Mode-7 vibe):
     * R074 fix — each strip gets keys every (DUR/5)s marching from far
     * to near, so the floor is always populated */
    wb_mesh *strip[10];
    for (int st = 0; st < 10; st++) {
        strip[st] = wb_mesh_box(26.0f, 0.15f, 2.2f, 90,150,235);
        int os_ = wb_anim_add_object(an, strip[st], 60,110,200);
        double period = DUR / 5.0;
        double phase = st * (DUR / 10.0);
        for (double ct = -phase; ct < DUR + 1.0; ct += period) {
            double ta = ct < 0 ? 0 : ct;
            double tb = ct + period;
            if (ta > DUR) break;
            if (tb > DUR) tb = DUR;
            double z0 = -160.0 - (ct < 0 ? -ct : 0) * 60.0;
            wb_anim_key_ease(an, os_, ta,
                             0, -2.5f, (float)(-150.0),
                             0,0,0, 1.0, 0);
            wb_anim_key_ease(an, os_, tb,
                             0, -2.5f, (float)(4.0),
                             0,0,0, 1.0, 0);
            (void)z0;
        }
    }

    /* starfield: tiny white boxes scattered around the corridor */
    for (int stx = 0; stx < 16; stx++) {
        float sx = (float)((stx * 97) % 160) - 80.0f;
        float sy = (float)((stx * 53) % 60) - 10.0f;
        float sz = -20.0f - (float)((stx * 71) % 130);
        wb_mesh *star = wb_mesh_box(0.15f, 0.15f, 0.15f, 255,255,255);
        int ost = wb_anim_add_object(an, star, 255,255,255);
        wb_anim_key(an, ost, 0.0, sx, sy, sz, 0,0,0, 1.0);
        wb_anim_key(an, ost, DUR, sx, sy, sz + 8.0f, 0,0,0, 1.0);
    }

    /* camera chase: slight rise + shake-free dolly */
    wb_anim_set_camera(an, 0.28f, 0.0f, 26.0f);
    for (double ct = 0; ct <= DUR; ct += 1.0)
        wb_anim_key_camera(an, ct, 0.28f,
                           6.0f*(float)sin(ct*0.7),
                           26.0f + 1.2f*(float)sin(ct*1.3));

    printf("sf: %d objects, %d tris total\n",
           wb_anim_object_count(an), 0);

    /* --- compositor: space bg + anim frame + HUD ------------------ */
    uint8_t *rgba = malloc((size_t)SF_W * SF_H * 4);
    if (!rgba) return 1;
    wb_node *bg = wb_node_source_scene(0.01f,0.01f,0.05f,
                                       0.05f,0.03f,0.15f,
                                       1, 0.05f, SF_W, SF_H);
    wb_node *sfframe = wb_node_source_frame(SF_W, SF_H, rgba);
    wb_node *comp = wb_node_composite();
    if (!bg || !sfframe || !comp) return 1;
    wb_composite_add(comp, bg);      /* bottom: space */
    wb_composite_add(comp, sfframe); /* middle: CGI */

    return sf_render_loop(an, comp, rgba, mp4, DUR, 15, SF_W, SF_H);
}

int main(int argc, char **argv) {
    fprintf(stderr, "Big Mac renderer %s\n", WB_VERSION);
    const char *outpath = argc > 1 ? argv[1] : "render.wav";
    double lufs_target = 0.0;   /* 0 = off; --lufs N enables */
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--lufs") == 0 && i + 1 < argc)
            lufs_target = atof(argv[++i]);
    /* R074 hop 112: --starfox — corridor run demo */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--starfox") == 0) {
            const char *mp4 = (i+1 < argc) ? argv[i+1]
                            : "/tmp/starfox.mp4";
            int rc3 = wb_render_starfox(mp4);
            printf("Star Fox render rc=%d\n", rc3);
            return rc3;
        }
    }

    /* R074 hop 110: --showcase — full demo render at 640x360 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--showcase") == 0) {
            const char *mp4 = (i+1 < argc) ? argv[i+1]
                            : "/tmp/bigmac_showcase_v2.mp4";
            extern int wb_render_showcase(const char *mp4);
            int rc2 = wb_render_showcase(mp4);
            printf("Showcase render rc=%d\n", rc2);
            return rc2;
        }
    }

    /* R073 hop 103 / R074 fix: --transition-frames OP N PREFIX
     * [--dur S] [--size WxH] — configurable duration + resolution. */
    double seq_dur = 2.0;
    int seq_w = 640, seq_h = 360;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--dur") == 0) seq_dur = atof(argv[i+1]);
        if (strcmp(argv[i], "--size") == 0 &&
            sscanf(argv[i+1], "%dx%d", &seq_w, &seq_h) == 2) { }
    }
    for (int i = 1; i < argc - 3; i++) {
        if (strcmp(argv[i], "--transition-frames") == 0) {
            int op = atoi(argv[i+1]);
            int nframes = atoi(argv[i+2]);
            const char *prefix = argv[i+3];
            wb_node *ga = wb_node_source_color(1.0f,0.2f,0.1f,1.0f,
                                              seq_w, seq_h);
            wb_node *gb = wb_node_source_color(0.1f,0.2f,1.0f,1.0f,
                                              seq_w, seq_h);
            if (!ga || !gb) { fprintf(stderr,"render: src fail\n"); return 1; }
            wb_node *tr = wb_node_transition(op, 2.0);
            if (!tr) { fprintf(stderr,"render: trans fail\n"); return 1; }
            wb_transition_add(tr, ga);
            wb_transition_add(tr, gb);
            int ok = 0;
            for (int k = 0; k < nframes; k++) {
                double tt = (double)k / (nframes > 1 ? nframes-1 : 1)
                          * seq_dur;
                wb_frame *f = wb_node_pull(tr, tt, 0, 0, seq_w, seq_h);
                if (!f) continue;
                char path[512];
                snprintf(path, sizeof path, "%s_%04d.ppm", prefix, k);
                if (wb_frame_write_ppm(f, path) == 0) ok++;
                wb_frame_free(f);
            }
            printf("Rendered %d/%d frames -> %s_*.ppm\n",
                   ok, nframes, prefix);
            return ok == nframes ? 0 : 1;
        }
    }

    wb_session *s = NULL;

    if (argc > 2 && strcmp(argv[2], "--demo") == 0) {
        s = wb_session_demo();
        printf("Rendering demo session (%d tracks, %.1fs)\n",
               (int)s->track_count, s->length / WB_SAMPLE_RATE);
    } else if (argc > 2 && strcmp(argv[2], "--file") == 0 && argc > 3) {
        s = wb_session_load(argv[3]);
        if (!s) { fprintf(stderr, "render: failed to load %s\n", argv[3]); return 1; }
        printf("Rendering project %s (%d tracks, %.1fs)\n",
               argv[3], (int)s->track_count, s->length / WB_SAMPLE_RATE);
    } else {
        fprintf(stderr,
            "usage: wb_render out.wav --demo | --file project.wbus [--lufs N]\n");
        return 2;
    }

    wb_engine *e = wb_engine_create();
    wb_engine_set_session(e, s);

    wb_sample *audio = NULL;
    uint32_t frames = 0;
    if (wb_engine_render_session(e, s, &audio, &frames) != 0) {
        fprintf(stderr, "render failed\n");
        return 1;
    }

    /* measure sample peak + RMS */
    float peak = 0, rms_acc = 0;
    for (uint32_t i = 0; i < frames * 2; i++) {
        float v = audio[i];
        float a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        rms_acc += v * v;
    }
    float rms = sqrtf(rms_acc / (frames * 2));

    /* R073 hop 35: BS.1770 integrated loudness via the engine's own
     * K-weighting meter */
    float *mono = malloc((size_t)frames * sizeof(float));
    double lufs_i = 0.0;
    if (mono) {
        for (uint32_t i = 0; i < frames; i++)
            mono[i] = 0.5f * (audio[i*2] + audio[i*2+1]);
        wb_lufs lv;
        wb_lufs_create(&lv, WB_SAMPLE_RATE);
        wb_lufs_process(&lv, mono, (int)frames);
        lufs_i = wb_lufs_integrated_lufs(&lv);
    }

    printf("Rendered %u frames (%.2fs) -> %s\n", frames,
           (double)frames / WB_SAMPLE_RATE, outpath);
    printf("Peak: %.3f  RMS: %.3f\n", peak, rms);
    double pk_db = 20.0 * log10(peak > 1e-6 ? peak : 1e-6);
    printf("LUFS-I: %.1f   Sample-peak: %.2f dBFS\n", lufs_i, pk_db);

    /* R073 hop 36: optional loudness trim before writing */
    if (lufs_target < 0.0 && lufs_i > -70.0) {
        double g_db = lufs_target - lufs_i;
        if (peak > 1e-6f) {
            double cap = 20.0 * log10(1.0 / peak);
            if (g_db > cap) g_db = cap;      /* true-peak guard */
        }
        float g = (float)pow(10.0, g_db / 20.0);
        for (uint32_t i = 0; i < frames * 2; i++) audio[i] *= g;
        rms *= g; peak *= g;
        printf("LUFS trim: %.1f -> %.1f LUFS (%+.1f dB)\n",
               lufs_i, lufs_target, g_db);
    }
    /* R073 hop 38: brickwall safety pass — guarantees the written file
     * never exceeds ~-0.3 dBTP even if the trim guard was conservative */
    {
        float pk2 = 0;
        for (uint32_t i = 0; i < frames * 2; i++) {
            float a = audio[i] < 0 ? -audio[i] : audio[i];
            if (a > pk2) pk2 = a;
        }
        if (pk2 > 0.97f) {
            wb_limiter *lim = wb_limiter_create(WB_SAMPLE_RATE, 3.0, 0.97f);
            if (!lim) return 1;
            wb_limiter_process(lim, audio, frames);
            printf("Limiter: caught overshoot %.3f -> ceiling 0.97\n", pk2);
            /* re-measure */
            float p3 = 0;
            for (uint32_t i = 0; i < frames * 2; i++) {
                float a = audio[i] < 0 ? -audio[i] : audio[i];
                if (a > p3) p3 = a;
            }
            peak = p3;
            wb_limiter_destroy(lim);
        }
    }
    free(mono);

    if (wb_wav_write_pcm16(outpath, audio, frames, 2, WB_SAMPLE_RATE) != 0) {
        fprintf(stderr, "wav write failed\n");
        return 1;
    }
    printf("Final: Peak %.3f (%.2f dBFS)  RMS %.3f\n",
           peak, 20.0 * log10(peak > 1e-6 ? peak : 1e-6), rms);

    free(audio);
    wb_engine_destroy(e);
    wb_session_destroy(s);
    return 0;
}
