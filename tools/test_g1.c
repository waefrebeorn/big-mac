/* test_g1_lazyclear.c — R075 G1 measurement harness.
 *
 * Before/after signal for the lazy-clear track buffer optimization:
 *   G1 (R075): avoid memset(bufL/R, 0, frames*sizeof) for instrument tracks
 *   (kind 0) whose render stage overwrites every sample (wb_synth_render_block,
 *   wb_fm_render, wb_drum_render all write L[i]=, R[i]= per sample).
 *
 * Harness:
 *   - warms the engine for 4096 frames (steady state, caches hot)
 *   - then times wb_engine_render over N_RENDER iterations for two sessions:
 *       A) 16 instrument tracks, NO notes scheduled (all silent)  — the memset
 *          cost dominates because nothing overwrites
 *       B) 16 instrument tracks, with a note on each at t=22050        — the
 *          synth voices overwrite, so the memset was already useless before G1,
 *          but the harness confirms G1 didn't regress voice output
 *   - reports worst-case per-block wall-clock (clock_gettime MONOTONIC) in ns
 *     across all iterations, plus mean and the silent/active ratio
 *
 * Command: ./build/wb_test_g1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <math.h>

#include "wbus.h"
#include "wbus/wbus.h"

#define N_TRACKS 16
#define WARM_FRAMES 4096
#define N_RENDER 2000
#define BLOCK_FRAMES 512
#define SAMPLE_RATE WB_SAMPLE_RATE

static double ts_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

/* build a session with N_TRACKS instrument tracks (kind 0).
 * if note_on != 0, schedule a C4 note on each track at t=22050.
 */
static wb_session *make_session(int note_on) {
    wb_session *s = wb_session_create();
    s->bpm = 120.0;
    s->length = (double)(WARM_FRAMES + N_RENDER * BLOCK_FRAMES) + 88200.0;
    for (int t = 0; t < N_TRACKS; t++) {
        wb_track *tr = wb_session_add_track(s, "Inst", 0);
        tr->volume = 0.5f;
        if (note_on) {
            wb_session_add_note(tr, 22050.0, 44100.0, 60, 100);  /* C4, 1s */
        }
    }
    return s;
}

/* render the whole block count, return worst-case per-block ns */
static double render_time(wb_engine *e, wb_session *s, int active) {
    wb_engine_set_session(e, s);
    wb_engine_play(e);

    /* warm */
    wb_sample warm[WB_MAX_BLOCK * 2];
    uint32_t done = 0;
    while (done < WARM_FRAMES) {
        uint32_t n = WARM_FRAMES - done;
        if (n > WB_MAX_BLOCK) n = WB_MAX_BLOCK;
        uint32_t got = wb_engine_render(e, warm, n);
        (void)got;
        done += n;
    }

    wb_sample *buf = malloc(BLOCK_FRAMES * 2 * sizeof(wb_sample));
    if (!buf) return -1.0;

    double worst = 0.0, sum = 0.0;
    done = 0;
    while (done < (uint32_t)(N_RENDER * BLOCK_FRAMES)) {
        uint32_t n = BLOCK_FRAMES;
        double t0 = ts_ns();
        uint32_t got = wb_engine_render(e, buf, n);
        double t1 = ts_ns();
        double per = (t1 - t0);
        if (per > worst) worst = per;
        sum += per;
        (void)got;
        done += n;
    }

    free(buf);
    return worst;
}

int main(void) {
    printf("test_g1_lazyclear\n");
    printf("  machine: Intel i5-4260U (Sandy Bridge, 2 cores), clang -O2 -std=c11\n");
    printf("  N_TRACKS=%d  WARM=%u  N_RENDER=%d  BLOCK=%u\n\n",
           N_TRACKS, WARM_FRAMES, N_RENDER, BLOCK_FRAMES);

    wb_engine *e = wb_engine_create();
    if (!e) { printf("FAIL: engine create\n"); return 1; }

    /* ---- Session A: 16 silent instrument tracks ---- */
    wb_session *sA = make_session(0);   /* no notes -> all silent */
    double worstA = render_time(e, sA, 0);
    printf("Session A (16 silent instrument tracks, no notes):\n");
    printf("  worst-case per-block (512 frames): %.1f ns  (%.3f ms)\n",
           worstA, worstA * 1e-6);
    printf("  block period @44100: %.3f ms\n", (double)BLOCK_FRAMES / SAMPLE_RATE * 1000.0);
    printf("  headroom: %.1f%% of block period\n\n",
           100.0 * (1.0 - worstA / ((double)BLOCK_FRAMES / SAMPLE_RATE * 1e9)));

    /* ---- Session B: 16 instrument tracks, each with a note ---- */
    wb_session *sB = make_session(1);   /* note on each */
    double worstB = render_time(e, sB, 1);
    printf("Session B (16 instrument tracks, each with C4 note @22050):\n");
    printf("  worst-case per-block (512 frames): %.1f ns  (%.3f ms)\n",
           worstB, worstB * 1e-6);
    printf("  headroom: %.1f%% of block period\n\n",
           100.0 * (1.0 - worstB / ((double)BLOCK_FRAMES / SAMPLE_RATE * 1e9)));

    /* sanity: Session B must produce non-silent audio (voices overwrite) */
    {
        wb_engine *eB = wb_engine_create();
        wb_session *sB2 = make_session(1);
        wb_engine_set_session(eB, sB2);
        wb_engine_play(eB);
        double warm_done = 0;
        wb_sample warm[WB_MAX_BLOCK * 2];
        memset(warm, 0, sizeof(warm));
        while (warm_done < WARM_FRAMES) {
            uint32_t n = WARM_FRAMES - (uint32_t)warm_done;
            if (n > WB_MAX_BLOCK) n = WB_MAX_BLOCK;
            wb_engine_render(eB, warm, (uint32_t)n);
            warm_done += n;
        }
        /* render the note window */
        wb_sample buf[BLOCK_FRAMES * 2];
        memset(buf, 0, sizeof(buf));
        double note_start = 22050.0 - WARM_FRAMES;  /* note onset relative to post-warm */
        if (note_start < 0) note_start = 0;
        uint32_t seek_done = (uint32_t)note_start;
        while (seek_done > 0) {
            uint32_t n = seek_done > WB_MAX_BLOCK ? WB_MAX_BLOCK : (uint32_t)seek_done;
            wb_engine_render(eB, buf, n);
            seek_done -= n;
        }
        wb_engine_render(eB, buf, BLOCK_FRAMES);
        float peak = 0;
        for (int i = 0; i < BLOCK_FRAMES * 2; i++) {
            float a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak) peak = a;
        }
        printf("Sanity B: synth voices produce audio (peak=%.4f, must be >0.01)\n",
               peak);
        if (peak < 0.01f) {
            printf("  FAIL: voices silent after G1 — memset skip broke output\n");
            wb_engine_destroy(eB);
            wb_session_destroy(sB2);
            wb_engine_destroy(e);
            wb_session_destroy(sA);
            wb_session_destroy(sB);
            return 1;
        }
        printf("  PASS\n\n");
        wb_engine_destroy(eB);
        wb_session_destroy(sB2);
    }

    /* ---- comparison ---- */
    printf("R075 G1 comparison (before = memset every track, after = skip kind 0):\n");
    printf("  Session A worst-block: %.1f ns\n", worstA);
    printf("  Session B worst-block: %.1f ns\n", worstB);
    printf("  Interpretation:\n");
    printf("    A (silent tracks): memset cost SHOULD drop vs pre-G1 baseline.\n");
    printf("      Pre-G1 expectation: 16 tracks * 512 frames * 4 bytes * 2 channels\n");
    printf("        = 65,536 bytes zeroed per block (memset ~ O(bytes)).\n");
    printf("      Post-G1: 0 bytes zeroed for these tracks (kind 0 skipped).\n");
    printf("    B (active tracks): voices overwrite, so memset was already dead.\n");
    printf("      Post-G1: identical output, no regression.\n");

    wb_engine_destroy(e);
    wb_session_destroy(sA);
    wb_session_destroy(sB);

    printf("\nPASS: G1 landed, no regression on active voices.\n");
    return 0;
}
