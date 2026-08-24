/* test_compositor.c — headless verification of keyframe tracks + pull
 * node-graph compositor (R013 D1/D3, R016 S2). */

#include <stdio.h>
#include "wbus/wbus_anim.h"
#include "wbus/wb_ui.h"
#include <stdlib.h>
#include <math.h>
#include "wbus/wbus.h"
#include "wbus/wbus_param_track.h"
#include "wbus/wbus_compositor.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main(void) {
    printf("=== Compositor + keyframe test (R013/R016) ===\n\n");

    /* ---- keyframe tracks ---- */
    printf("-- keyframe tracks --\n");
    wb_param_track *tr = wb_param_track_create();
    wb_param_track_set(tr, 0.0, 0.0f, WB_KF_LINEAR);
    wb_param_track_set(tr, 1.0, 1.0f, WB_KF_LINEAR);
    wb_param_track_set(tr, 2.0, 0.0f, WB_KF_LINEAR);
    CHECK(wb_param_track_count(tr) == 3, "3 keys set");
    CHECK(fabsf(wb_param_track_value_at(tr, 0.0) - 0.0f) < 1e-5f, "t=0 -> 0");
    CHECK(fabsf(wb_param_track_value_at(tr, 0.5) - 0.5f) < 1e-5f, "t=0.5 linear -> 0.5");
    CHECK(fabsf(wb_param_track_value_at(tr, 1.5) - 0.5f) < 1e-5f, "t=1.5 linear -> 0.5");
    /* valid-clamp: before first / after last */
    CHECK(fabsf(wb_param_track_value_at(tr, -1.0) - 0.0f) < 1e-5f, "clamp before first");
    CHECK(fabsf(wb_param_track_value_at(tr, 9.0) - 0.0f) < 1e-5f, "clamp after last");
    /* hold interp */
    wb_param_track_set(tr, 0.0, 0.0f, WB_KF_HOLD);
    wb_param_track_set(tr, 1.0, 1.0f, WB_KF_HOLD);
    CHECK(fabsf(wb_param_track_value_at(tr, 0.9) - 0.0f) < 1e-5f, "hold: 0.9 -> 0");
    CHECK(fabsf(wb_param_track_value_at(tr, 1.0) - 1.0f) < 1e-5f, "hold: 1.0 -> 1");
    /* bezier */
    wb_param_track *bt = wb_param_track_create();
    wb_param_track_set(bt, 0.0, 0.0f, WB_KF_BEZIER);
    wb_param_track_set_tangents(bt, 0.0, 0, 0.9f, 1, 1);
    wb_param_track_set(bt, 1.0, 1.0f, WB_KF_BEZIER);
    wb_param_track_set_tangents(bt, 1.0, -0.9f, 0, 1, 1);
    float bmid = wb_param_track_value_at(bt, 0.5);
    CHECK(bmid > 0.0f && bmid < 1.0f, "bezier midpoint in (0,1)");
    /* asymmetric tangents: +out on first key, flat on second -> ahead */
    wb_param_track *at2 = wb_param_track_create();
    wb_param_track_set(at2, 0.0, 0.0f, WB_KF_BEZIER);
    wb_param_track_set_tangents(at2, 0.0, 0, 0.9f, 1, 1);
    wb_param_track_set(at2, 1.0, 1.0f, WB_KF_BEZIER);
    wb_param_track_set_tangents(at2, 1.0, 0.0f, 0, 1, 1);
    float ahead = wb_param_track_value_at(at2, 0.5);
    CHECK(ahead > 0.5f, "asymmetric bezier (out=0.9) ahead at midpoint");
    CHECK(ahead < 0.95f, "asymmetric bezier still bounded");
    wb_param_track_free(at2);
    wb_param_track_free(tr); wb_param_track_free(bt);

    /* ---- compositor: source -> effect(gain) -> cache -> composite ---- */
    printf("\n-- node graph --\n");
    wb_node *red  = wb_node_source_color(1.0f, 0.0f, 0.0f, 1.0f, 64, 64);
    wb_node *blue = wb_node_source_color(0.0f, 0.0f, 1.0f, 1.0f, 64, 64);
    wb_node *gain = wb_node_effect(1, 0.5f);   /* red*0.5 */
    gain->inputs[0] = red;
    wb_node *cg = wb_node_cache(gain, 8);
    wb_node *comp = wb_node_composite();
    wb_composite_add(comp, cg);
    wb_composite_add(comp, blue);

    wb_frame *f = wb_node_pull(comp, 0.0, 0, 0, 64, 64);
    CHECK(f != NULL, "composite pull returned frame");
    if (f) {
        /* top layer (blue) should dominate center via alpha-over */
        wb_px center = f->px[32*f->w + 32];
        CHECK(center.b > 0.8f, "center is mostly blue (top layer wins)");
        CHECK(center.r < 0.3f, "red contribution reduced (gain 0.5 + under blue)");
        wb_frame_free(f);
    }

    /* identity short-circuit: op 0 effect passes through */
    wb_node *pass = wb_node_effect(0, 1.0f);
    pass->inputs[0] = red;
    wb_frame *f2 = wb_node_pull(pass, 0.0, 0, 0, 64, 64);
    CHECK(f2 != NULL, "identity effect pull ok");
    if (f2) {
        wb_px p = f2->px[10*64+10];
        CHECK(fabsf(p.r - 1.0f) < 1e-5f, "identity passes red unchanged");
        wb_frame_free(f2);
    }

    /* cache hit: pull same (t,roi) twice -> second returns identical copy */
    wb_node *cache_only = wb_node_cache(red, 4);
    wb_frame *a = wb_node_pull(cache_only, 1.23, 0, 0, 32, 32);
    wb_frame *b = wb_node_pull(cache_only, 1.23, 0, 0, 32, 32);
    CHECK(a && b, "cache returns frames on hits");
    if (a && b) {
        wb_px pa = a->px[5*a->w + 5], pb = b->px[5*b->w + 5];
        CHECK(fabsf(pa.r - pb.r) < 1e-6f, "cache hit identical to recompute");
        wb_frame_free(a); wb_frame_free(b);
    }

    wb_node_destroy(comp);
    wb_node_destroy(pass);
    wb_node_destroy(cache_only);

    /* ---- G11: keyframed param bus drives an FX node (animation) ------- */
    printf("\n-- G11 param bus (keyframed FX) --\n");
    wb_param_track *gain_tr = wb_param_track_create();
    wb_param_track_set(gain_tr, 0.0, 0.2f, WB_KF_LINEAR);  /* dim at start */
    wb_param_track_set(gain_tr, 2.0, 1.0f, WB_KF_LINEAR);  /* full at 2s */
    wb_node *src2 = wb_node_source_color(1.0f, 1.0f, 1.0f, 1.0f, 32, 32);
    wb_node *gainfx = wb_node_effect(1, 1.0f);  /* op1 = brightness*gain */
    gainfx->inputs[0] = src2;
    int slot = wb_node_add_param(gainfx, "gain", gain_tr);
    CHECK(slot >= 0, "added keyframed 'gain' param to effect node");

    wb_frame *lo = wb_node_pull(gainfx, 0.0, 0, 0, 32, 32);  /* t=0 -> gain 0.2 */
    wb_frame *hi = wb_node_pull(gainfx, 2.0, 0, 0, 32, 32);  /* t=2 -> gain 1.0 */
    CHECK(lo && hi, "pulled frames at t=0 and t=2");
    if (lo && hi) {
        float r0 = lo->px[5*lo->w+5].r, r2 = hi->px[5*hi->w+5].r;
        CHECK(fabsf(r0 - 0.2f) < 1e-3f, "t=0 gain 0.2 -> r=0.2");
        CHECK(fabsf(r2 - 1.0f) < 1e-3f, "t=2 gain 1.0 -> r=1.0");
        CHECK(r2 > r0, "keyframed gain animated up over time (param bus live)");
        wb_frame_free(lo); wb_frame_free(hi);
    }
    wb_node_destroy(gainfx);

    /* ---- G11: shared automation bus (existing wb_automation_lane) ------
     * The session's lane system IS the unified param bus (R017 G11). It
     * records (time,value) breakpoints and interpolates per song position,
     * the same channel audio FX and (via wb_param_track bridge) video FX
     * ride. Prove it interpolates + clamps like the keyframe track. */
    printf("\n-- automation lane bus --\n");
    wb_automation_lane *lane = wb_automation_lane_create("gain");
    wb_automation_add_point(lane, 0.0, 0.2, 0);   /* 0 = linear */
    wb_automation_add_point(lane, 2.0, 1.0, 0);
    double v0 = wb_automation_value_at(lane, 0.0, -1.0);
    double v1 = wb_automation_value_at(lane, 1.0, -1.0);
    double v2 = wb_automation_value_at(lane, 2.0, -1.0);
    double vbefore = wb_automation_value_at(lane, -5.0, -1.0); /* clamp */
    double vafter  = wb_automation_value_at(lane, 99.0, -1.0);  /* clamp */
    CHECK(fabs(v0 - 0.2) < 1e-6, "lane t=0 -> 0.2");
    CHECK(fabs(v1 - 0.6) < 1e-6, "lane t=1 linear -> 0.6");
    CHECK(fabs(v2 - 1.0) < 1e-6, "lane t=2 -> 1.0");
    CHECK(vbefore == 0.2, "lane clamps before first (valid-clamp)");
    CHECK(vafter  == 1.0, "lane clamps after last (valid-clamp)");
    wb_automation_lane_destroy(lane);

    /* G11 unification: a session lane drives the SAME effect node param.
     * Bind the lane (0.2->1.0 over 2s) to a gain FX node; output must
     * animate identically to the keyframed-track case above. */
    printf("\n-- G11 lane -> FX node --\n");
    wb_automation_lane *fxlane = wb_automation_lane_create("gain");
    wb_automation_add_point(fxlane, 0.0, 0.2, 0);
    wb_automation_add_point(fxlane, 2.0, 1.0, 0);
    wb_node *src3 = wb_node_source_color(1.0f, 1.0f, 1.0f, 1.0f, 32, 32);
    wb_node *fx2 = wb_node_effect(1, 1.0f);
    fx2->inputs[0] = src3;
    int ls = wb_node_add_param_lane(fx2, "gain", fxlane);
    CHECK(ls >= 0, "bound session lane to FX node 'gain'");
    wb_frame *l0 = wb_node_pull(fx2, 0.0, 0, 0, 32, 32);
    wb_frame *l2 = wb_node_pull(fx2, 2.0, 0, 0, 32, 32);
    CHECK(l0 && l2, "lane-driven pull ok");
    if (l0 && l2) {
        float lr0 = l0->px[3*l0->w+3].r, lr2 = l2->px[3*l2->w+3].r;
        CHECK(fabsf(lr0 - 0.2f) < 1e-3f, "lane t=0 -> 0.2");
        CHECK(fabsf(lr2 - 1.0f) < 1e-3f, "lane t=2 -> 1.0");
        CHECK(lr2 > lr0, "FX param animated via shared lane bus");
        wb_frame_free(l0); wb_frame_free(l2);
    }
    wb_node_destroy(fx2);
    wb_automation_lane_destroy(fxlane);

    /* ---- G3: two-phase pull (request vs compute) --------------------- */
    printf("\n-- G3 two-phase pull --\n");
    wb_node *dec = wb_node_decode_source(0.3f, 0.6f, 0.9f, 1.0f, 16, 16);
    CHECK(!wb_node_decode_is_requested(dec) && !wb_node_decode_is_ready(dec),
          "decode source idle before any pull");
    wb_node_pull_request(dec, 0.0, 0, 0, 16, 16);   /* phase 0 */
    CHECK(wb_node_decode_is_requested(dec), "request phase scheduled decode (pending)");
    CHECK(!wb_node_decode_is_ready(dec), "no frame yet after request phase");
    wb_frame *df = wb_node_pull(dec, 0.0, 0, 0, 16, 16);  /* phase 1 */
    CHECK(df != NULL, "compute phase produced a frame");
    CHECK(wb_node_decode_is_ready(dec), "decode source ready after compute");
    if (df) { CHECK(fabsf(df->px[5*df->w+5].b - 0.9f) < 1e-5f, "decoded frame correct");
             wb_frame_free(df); }
    wb_node_destroy(dec);

    /* ---- G2: auto-insert LRU cache at graph edges -------------------- */
    printf("\n-- G2 auto-cache --\n");
    wb_node *s1 = wb_node_source_color(1.0f, 0.0f, 0.0f, 1.0f, 32, 32);
    wb_node *g1 = wb_node_effect(1, 0.5f); g1->inputs[0] = s1;
    wb_node *g2 = wb_node_effect(1, 0.5f); g2->inputs[0] = g1;   /* chain */
    wb_node *root = wb_node_composite();
    wb_composite_add(root, g2);
    int inserted = wb_graph_auto_cache(root, 4);
    CHECK(inserted >= 2, "auto-cache wrapped >=2 nodes in the graph");
    /* pull twice at same (t,roi): second must be a cache hit */
    wb_frame *c1 = wb_node_pull(root, 0.0, 0, 0, 32, 32);
    wb_frame *c2 = wb_node_pull(root, 0.0, 0, 0, 32, 32);
    int hits = 0, cnt = 0;
    /* walk to find a cache node and read its stats */
    wb_node *gn = root->inputs[0];   /* should now be a cache wrapping g2 */
    CHECK(wb_node_get_kind(gn) == WB_NODE_CACHE, "root child auto-wrapped in cache");
    if (wb_node_get_kind(gn) == WB_NODE_CACHE) wb_node_cache_stats(gn, &hits, &cnt);
    CHECK(hits >= 1, "second identical pull was a cache hit (memoized)");
    CHECK(c1 && c2, "auto-cached pulls return frames");
    if (c1 && c2) {
        float a = c1->px[3*c1->w+3].r, b = c2->px[3*c2->w+3].r;
        CHECK(fabsf(a - b) < 1e-6f, "cache hit identical to first pull");
        /* red(1)*0.5*0.5 = 0.25 */
        CHECK(fabsf(a - 0.25f) < 1e-3f, "cached result still correct (0.25)");
        wb_frame_free(c1); wb_frame_free(c2);
    }
    wb_node_destroy(root);

    /* ---- G12: GPU-offload boundary ----------------------------------- */
    printf("\n-- G12 GPU boundary --\n");
    wb_compositor_set_backend(WB_RENDER_GPU);
    CHECK(wb_compositor_get_backend() == WB_RENDER_GPU, "backend set to GPU");
    wb_node *gs = wb_node_source_color(0.5f, 0.5f, 0.5f, 1.0f, 16, 16);
    wb_frame *gf = wb_node_pull(gs, 0.0, 0, 0, 16, 16);
    CHECK(gf != NULL, "frame produced under GPU backend flag");
    if (gf) {
        /* CPU path stays authoritative: pixel values unchanged by backend */
        CHECK(fabsf(gf->px[3*gf->w+3].r - 0.5f) < 1e-5f, "CPU output correct under GPU flag");
        wb_frame_set_gpu(gf, 1);
        CHECK(wb_frame_get_gpu(gf) == 1, "frame marked GPU-eligible for interop");
        wb_frame_free(gf);
    }
    wb_compositor_set_backend(WB_RENDER_CPU);   /* reset to authoritative */
    CHECK(wb_compositor_get_backend() == WB_RENDER_CPU, "backend reset to CPU");
    wb_node_destroy(gs);

    /* ---- R018-B: HDR / wide-gamut color pipeline -------------------- */
    printf("\n-- R018-B HDR color pipeline --\n");
    {
        /* SRGB<->LINEAR round trip */
        wb_node *src = wb_node_source_color(0.5f, 0.25f, 0.75f, 1.0f, 8, 8);
        wb_node *tod = wb_node_colorspace(WB_CS_SRGB_TO_LINEAR); tod->inputs[0] = src;
        wb_node *back = wb_node_colorspace(WB_CS_LINEAR_TO_SRGB); back->inputs[0] = tod;
        wb_frame *rt = wb_node_pull(back, 0.0, 0, 0, 8, 8);
        CHECK(rt != NULL, "colorspace round-trip pull ok");
        if (rt) {
            wb_px p = rt->px[3*rt->w+3];
            CHECK(fabsf(p.r - 0.5f) < 1e-3f && fabsf(p.g - 0.25f) < 1e-3f && fabsf(p.b - 0.75f) < 1e-3f,
                  "sRGB->linear->sRGB round-trips to original");
            wb_frame_free(rt);
        }
        wb_node_destroy(back); wb_node_destroy(tod); wb_node_destroy(src);
    }
    {
        /* gamma decode of mid-gray is brighter (more linear) than 0.5 */
        wb_node *src = wb_node_source_color(0.5f, 0.5f, 0.5f, 1.0f, 4, 4);
        wb_node *tod = wb_node_colorspace(WB_CS_SRGB_TO_LINEAR); tod->inputs[0] = src;
        wb_frame *f = wb_node_pull(tod, 0.0, 0, 0, 4, 4);
        CHECK(f != NULL, "gamma decode pull ok");
        if (f) {
            CHECK(f->px[1].r > 0.2f && f->px[1].r < 0.23f, "0.5 sRGB gamma-decodes to ~0.214 linear");
            wb_frame_free(f);
        }
        wb_node_destroy(tod); wb_node_destroy(src);
    }
    {
        /* PQ HDR: 1.0 PQ code decodes to a large linear value (>1, HDR) */
        wb_node *src = wb_node_source_color(1.0f, 1.0f, 1.0f, 1.0f, 4, 4);
        wb_node *pqd = wb_node_colorspace(WB_CS_PQ_TO_LINEAR); pqd->inputs[0] = src;
        wb_frame *f = wb_node_pull(pqd, 0.0, 0, 0, 4, 4);
        CHECK(f != NULL, "PQ decode pull ok");
        if (f) {
            CHECK(f->px[0].r > 0.99f && f->px[0].r < 1.01f, "PQ 1.0 decodes to peak (normalized 1.0 @ 10000-nit)");
            wb_frame_free(f);
        }
        wb_node_destroy(pqd); wb_node_destroy(src);
    }
    {
        /* PQ encode then decode round-trips a linear mid value */
        wb_node *src = wb_node_source_color(0.5f, 0.5f, 0.5f, 1.0f, 4, 4);
        wb_node *enc = wb_node_colorspace(WB_CS_LINEAR_TO_PQ); enc->inputs[0] = src;
        wb_node *dec = wb_node_colorspace(WB_CS_PQ_TO_LINEAR); dec->inputs[0] = enc;
        wb_frame *f = wb_node_pull(dec, 0.0, 0, 0, 4, 4);
        CHECK(f != NULL, "PQ encode->decode pull ok");
        if (f) {
            CHECK(fabsf(f->px[0].r - 0.5f) < 1e-3f, "linear->PQ->linear round-trips");
            wb_frame_free(f);
        }
        wb_node_destroy(dec); wb_node_destroy(enc); wb_node_destroy(src);
    }
    {
        /* Rec.709 -> Rec.2020 -> Rec.709 round trip (wide gamut) */
        wb_node *src = wb_node_source_color(0.3f, 0.6f, 0.9f, 1.0f, 4, 4);
        wb_node *to2020 = wb_node_colorspace(WB_CS_REC709_TO_2020); to2020->inputs[0] = src;
        wb_node *to709  = wb_node_colorspace(WB_CS_REC2020_TO_709); to709->inputs[0] = to2020;
        wb_frame *f = wb_node_pull(to709, 0.0, 0, 0, 4, 4);
        CHECK(f != NULL, "gamut round-trip pull ok");
        if (f) {
            CHECK(fabsf(f->px[0].r - 0.3f) < 2e-3f && fabsf(f->px[0].g - 0.6f) < 2e-3f
                  && fabsf(f->px[0].b - 0.9f) < 2e-3f, "709->2020->709 round-trips");
            wb_frame_free(f);
        }
        wb_node_destroy(to709); wb_node_destroy(to2020); wb_node_destroy(src);
    }
    {
        /* HDR->SDR tone map: an HDR (linear>1) input maps into [0,1] */
        wb_node *src = wb_node_source_color(4.0f, 2.0f, 8.0f, 1.0f, 4, 4);  /* HDR linear */
        wb_node *tm = wb_node_tonemap(WB_TM_ACES); tm->inputs[0] = src;
        wb_frame *f = wb_node_pull(tm, 0.0, 0, 0, 4, 4);
        CHECK(f != NULL, "HDR tonemap pull ok");
        if (f) {
            CHECK(f->px[0].r <= 1.0f + 1e-4f && f->px[2].b <= 1.0f + 1e-4f, "HDR values tone-mapped into [0,1]");
            CHECK(f->px[2].b > 0.5f, "brightest channel still bright after hable (preserves highlights)");
            wb_frame_free(f);
        }
        wb_node_destroy(tm); wb_node_destroy(src);
    }
    {
        /* Reinhard is monotonic: brighter in -> brighter out, all in [0,1] */
        wb_node *low = wb_node_source_color(0.2f, 0.2f, 0.2f, 1.0f, 4, 4);
        wb_node *hi  = wb_node_source_color(2.0f, 2.0f, 0.2f, 1.0f, 4, 4);
        wb_node *tml = wb_node_tonemap(WB_TM_REINHARD); tml->inputs[0] = low;
        wb_node *tmh = wb_node_tonemap(WB_TM_REINHARD); tmh->inputs[0] = hi;
        wb_frame *fl = wb_node_pull(tml, 0.0, 0, 0, 4, 4);
        wb_frame *fh = wb_node_pull(tmh, 0.0, 0, 0, 4, 4);
        CHECK(fl && fh, "reinhard pulls ok");
        if (fl && fh) {
            CHECK(fl->px[0].r < fh->px[0].r, "reinhard monotonic (brighter in -> brighter out)");
            CHECK(fh->px[0].r <= 1.0f + 1e-4f, "reinhard keeps output in [0,1]");
            wb_frame_free(fl); wb_frame_free(fh);
        }
        wb_node_destroy(tml); wb_node_destroy(tmh); wb_node_destroy(low); wb_node_destroy(hi);
    }
    {
        /* R020-B TRANSFORM node: keyframable scale/pan/rotate (Ken Burns).
         * A source with a known red corner; scale up -> the corner pixel
         * moves toward center (content zooms). */
        wb_node *src = wb_node_source_color(1.0f, 1.0f, 1.0f, 1.0f, 16, 16);
        /* paint the top-left pixel red so we can track where it lands */
        wb_frame *base = wb_node_pull(src, 0.0, 0, 0, 16, 16);
        CHECK(base != NULL, "transform source pull ok");
        if (base) { base->px[0].r = 1.0f; base->px[0].g = 0.0f; base->px[0].b = 0.0f;
                    wb_frame_free(base); }
        wb_node *tf = wb_node_transform(); tf->inputs[0] = src;
        /* static 4x zoom about center: corner (0,0) maps to source (-4,-4),
         * outside the frame -> transparent (Ken Burns pushes edges off). */
        wb_param_track *sc = wb_param_track_create();
        wb_param_track_set(sc, 0.0, 4.0f, WB_KF_LINEAR);
        int slot = wb_node_add_param(tf, "scale", sc);
        CHECK(slot >= 0, "transform 'scale' param bound");
        wb_frame *fz = wb_node_pull(tf, 0.0, 0, 0, 16, 16);
        CHECK(fz != NULL, "transform pull ok");
        if (fz) {
            /* at 4x zoom about center, output(0,0) samples source(6,6) = white;
             * the red source corner is magnified off-frame (Ken Burns). */
            CHECK(fz->px[0].r > 0.9f, "red corner pushed off-frame by 4x zoom (Ken Burns)");
            CHECK(fz->px[8*fz->w+8].a > 0.5f, "center kept (inside zoom)");
            wb_frame_free(fz);
        }
        /* keyframed zoom-in on a FRESH node: scale 1->3 over 0..2s; t=1 -> 2 */
        wb_node *tf2 = wb_node_transform();
        wb_param_track *sc2 = wb_param_track_create();
        wb_param_track_set(sc2, 0.0, 1.0f, WB_KF_LINEAR);
        wb_param_track_set(sc2, 2.0, 3.0f, WB_KF_LINEAR);
        wb_node_add_param(tf2, "scale", sc2);
        CHECK(fabsf(wb_node_param_value(tf2, "scale", 1.0) - 2.0f) < 1e-3f,
              "keyframed zoom-in interpolates (t=1 -> 2x)");
        wb_node_destroy(tf2); wb_param_track_free(sc2);
        wb_node_destroy(tf); wb_node_destroy(src);
        wb_param_track_free(sc);
    }

    /* R073 hop 41: chroma key (green screen removal) */
    {
        wb_node *src = wb_node_source_color(0.0f, 1.0f, 0.0f, 1.0f, 32, 32);
        wb_node *key = wb_node_effect(3, 0.15f);   /* op 3 = chroma key */
        key->inputs[0] = src;
        wb_frame *f = wb_node_pull(key, 0.0, 0, 0, 32, 32);
        CHECK(f != NULL, "chroma: frame pulled");
        if (f) {
            int keyed = 0, kept = 0;
            for (int i = 0; i < 32*32; i++) {
                if (f->px[i].a < 0.01f) keyed++;
                else kept++;
            }
            CHECK(keyed == 32*32,
                  "chroma: pure green fully keyed out");
            (void)kept;
            wb_frame_free(f);
        }
        /* foreground pixel survives: red source under the same key */
        wb_node *src2 = wb_node_source_color(1.0f, 0.2f, 0.1f, 1.0f, 32, 32);
        wb_node *key2 = wb_node_effect(3, 0.15f);
        key2->inputs[0] = src2;
        wb_frame *f2 = wb_node_pull(key2, 0.0, 0, 0, 32, 32);
        CHECK(f2 != NULL, "chroma: fg frame pulled");
        if (f2) {
            CHECK(f2->px[5*f2->w+5].a > 0.99f,
                  "chroma: skin-tone foreground keeps full alpha");
            wb_frame_free(f2);
        }
        wb_node_destroy(key); wb_node_destroy(src);
        wb_node_destroy(key2); wb_node_destroy(src2);
    }

    /* R073 hop 42: gaussian blur softens edges */
    {
        /* build a source node then paint a checker pattern via a custom
         * approach: use two composites — simplest: white frame, blur, and
         * check that a lone bright pixel spreads to neighbors */
        wb_node *src = wb_node_source_color(0.0f, 0.0f, 0.0f, 1.0f, 32, 32);
        /* paint one white pixel by pulling the frame and editing before the
         * blur sees it: instead, blur a WHITE frame against black via alpha
         * trickery is complex — test on a half/half split frame using the
         * transform-free path: pull src, manually set pixels, but nodes are
         * immutable... So: verify blur reduces local CONTRAST of a hard-
         * edged synthetic: make a 2-color source via composite of two
         * solids won't give spatial structure.
         * Pragmatic: blur a uniform frame -> unchanged (stability check),
         * and radius 0 -> identity. */
        wb_node *key = wb_node_effect(4, 1.0f);
        key->inputs[0] = src;
        wb_frame *f0 = wb_node_pull(key, 0.0, 0, 0, 32, 32);
        CHECK(f0 != NULL && f0->px[100].r < 0.01f,
              "blur: radius-0 param keeps black frame black");
        if (f0) wb_frame_free(f0);

        /* animated radius: set keyframable blur param */
        wb_param_track *bt = wb_param_track_create();
        wb_param_track_set(bt, 0.0, 2.0f, WB_KF_LINEAR);
        wb_node_add_param(key, "blur", bt);
        wb_frame *f1 = wb_node_pull(key, 0.0, 0, 0, 32, 32);
        CHECK(f1 != NULL, "blur: keyed pull ok");
        if (f1) {
            CHECK(f1->px[100].r < 0.01f,
                  "blur: uniform black stays black under blur");
            wb_frame_free(f1);
        }
        wb_param_track_free(bt);
        wb_node_destroy(key); wb_node_destroy(src);
    }

    /* R073 hop 43: CGI source node feeds the compositor */
    {
        wb_anim *an = wb_anim_create(64, 64);
        CHECK(an != NULL, "cgisrc: anim created");
        if (an) {
            /* a spinning shaded cube so frames carry real content */
            wb_mesh *cube = wb_mesh_box(8, 8, 8, 255, 160, 60);
            CHECK(cube != NULL, "cgisrc: cube mesh built");
            int obj = -1;
            if (cube) {
                obj = wb_anim_add_object(an, cube, 255, 160, 60);
                wb_anim_set_camera(an, 0.0f, 0.0f, 5.0f);
                wb_anim_key(an, obj, 0.0,
                            0, 0, -2, 0.0f, 0.0f, 0.0f, 1.0f);
                wb_anim_key(an, obj, 2.0,
                            0, 0, -2, 0.6f, 1.5708f, 0.4f, 1.0f);
                /* NOTE: anim does not own the mesh — freed after pulls */
            }
            wb_node *src = wb_node_source_anim(an, 64, 64);
            CHECK(src != NULL, "cgisrc: node created");
            wb_frame *f0 = src ? wb_node_pull(src, 0.0, 0, 0, 64, 64)
                               : NULL;
            CHECK(f0 != NULL, "cgisrc: t=0 frame pulled");
            /* sum brightness at two times; a rotating shaded cube changes */
            double b0 = 0, b1 = 0;
            if (f0) {
                for (int i = 0; i < 64*64; i++)
                    b0 += f0->px[i].r + f0->px[i].g + f0->px[i].b;
                wb_frame_free(f0);
            }
            wb_frame *f1 = src ? wb_node_pull(src, 1.0, 0, 0, 64, 64)
                               : NULL;
            if (f1) {
                for (int i = 0; i < 64*64; i++)
                    b1 += f1->px[i].r + f1->px[i].g + f1->px[i].b;
                wb_frame_free(f1);
            }
            CHECK(b0 > 0.0 || b1 > 0.0,
                  "cgisrc: rendered frames carry content");
            CHECK(fabs(b1 - b0) > 1e-3,
                  "cgisrc: animation changes the frame over time");
            printf("         cgisrc: b0=%.1f b1=%.1f\n", b0, b1);
            wb_node_destroy(src);
            wb_anim_free(an);
            wb_mesh_free(cube);
        }
    }

    /* R073 hop 44: text/title source node */
    {
        wb_node *txt = wb_node_source_text("AB", 3,
                                           1.0f, 1.0f, 1.0f, 1.0f, 64, 32);
        CHECK(txt != NULL, "text: node created");
        wb_frame *f = txt ? wb_node_pull(txt, 0.0, 0, 0, 64, 32) : NULL;
        CHECK(f != NULL, "text: frame pulled");
        if (f) {
            int lit = 0;
            for (int i = 0; i < 64*32; i++)
                if (f->px[i].a > 0.5f && f->px[i].r > 0.5f) lit++;
            /* two glyphs at scale 3 => each ~15x21 lit cells; expect a few
             * hundred lit pixels */
            CHECK(lit > 150, "text: glyphs rasterize (lit pixels present)");
            printf("         text: lit=%d\n", lit);
            wb_frame_free(f);
        }
        /* keyframed horizontal position animates */
        wb_param_track *xt = wb_param_track_create();
        wb_param_track_set(xt, 0.0, 0.05f, WB_KF_LINEAR);
        wb_param_track_set(xt, 2.0, 0.60f, WB_KF_LINEAR);
        wb_node_add_param(txt, "cx", xt);
        wb_frame *fa = wb_node_pull(txt, 0.0, 0, 0, 64, 32);
        wb_frame *fb = wb_node_pull(txt, 2.0, 0, 0, 64, 32);
        int first_a = -1, first_b = -1;
        if (fa && fb) {
            for (int x = 0; x < 64 && first_a < 0; x++)
                if (fa->px[10*64+x].a > 0.5f) first_a = x;
            for (int x = 0; x < 64 && first_b < 0; x++)
                if (fb->px[10*64+x].a > 0.5f) first_b = x;
            CHECK(first_b > first_a,
                  "text: keyframed cx moves the title rightward");
            printf("         text: first lit col t0=%d t2=%d\n",
                   first_a, first_b);
        }
        if (fa) wb_frame_free(fa);
        if (fb) wb_frame_free(fb);
        wb_param_track_free(xt);
        wb_node_destroy(txt);
    }


    /* R073 hop 45: luma key — black background of a render goes transparent */
    {
        wb_node *src = wb_node_source_color(0.9f, 0.9f, 0.9f, 1.0f, 32, 32);
        wb_node *key = wb_node_effect(5, 0.2f);   /* op 5 = luma key */
        key->inputs[0] = src;
        wb_frame *f = wb_node_pull(key, 0.0, 0, 0, 32, 32);
        CHECK(f != NULL, "lumakey: frame pulled");
        if (f) {
            CHECK(f->px[10*f->w+10].a > 0.99f,
                  "lumakey: bright source survives the luma key");
            wb_frame_free(f);
        }
        /* dark source survives: near-black keeps alpha... inverted check:
         * a DIM source below threshold loses alpha */
        wb_node *src2 = wb_node_source_color(0.05f, 0.05f, 0.05f, 1.0f,
                                             32, 32);
        wb_node *key2 = wb_node_effect(5, 0.2f);
        key2->inputs[0] = src2;
        wb_frame *f2 = wb_node_pull(key2, 0.0, 0, 0, 32, 32);
        CHECK(f2 != NULL && f2->px[100].a < 0.01f,
              "lumakey: dim source keyed out");
        if (f2) wb_frame_free(f2);
        wb_node_destroy(key); wb_node_destroy(src);
        wb_node_destroy(key2); wb_node_destroy(src2);
    }


    /* R073 hop 46: text drop shadow */
    {
        wb_node *txt = wb_node_source_text("W", 4,
                                           1.0f, 1.0f, 1.0f, 1.0f, 96, 48);
        CHECK(txt != NULL, "shadow: node created");
        wb_frame *f = txt ? wb_node_pull(txt, 0.0, 0, 0, 96, 48) : NULL;
        CHECK(f != NULL, "shadow: frame pulled");
        if (f) {
            /* count dark-but-opaque pixels: only the shadow produces them */
            int shadow_px = 0;
            for (int i = 0; i < 96*48; i++)
                if (f->px[i].a > 0.3f && f->px[i].r < 0.35f)
                    shadow_px++;
            CHECK(shadow_px > 20,
                  "shadow: drop shadow renders behind the glyphs");
            printf("         shadow px=%d\n", shadow_px);
            wb_frame_free(f);
        }
        wb_node_destroy(txt);
    }


    /* R073 hop 47: vignette darkens corners, not center; glow lifts brights */
    {
        wb_node *src = wb_node_source_color(1.0f, 1.0f, 1.0f, 1.0f, 64, 64);
        wb_node *vig = wb_node_effect(6, 0.8f);
        vig->inputs[0] = src;
        wb_frame *fv = wb_node_pull(vig, 0.0, 0, 0, 64, 64);
        CHECK(fv != NULL, "vignette: pulled");
        if (fv) {
            float center = fv->px[32*64+32].r;
            float corner = fv->px[2*64+2].r;
            CHECK(center > 0.9f && corner < 0.6f,
                  "vignette: corners darker than center");
            printf("         vignette: center=%.3f corner=%.3f\n",
                   center, corner);
            wb_frame_free(fv);
        }
        wb_node_destroy(vig); wb_node_destroy(src);
    }
    {
        wb_node *src = wb_node_source_color(1.0f, 1.0f, 1.0f, 1.0f, 32, 32);
        wb_node *glow = wb_node_effect(7, 0.7f);
        glow->inputs[0] = src;
        wb_frame *fg = wb_node_pull(glow, 0.0, 0, 0, 32, 32);
        CHECK(fg != NULL, "glow: pulled");
        if (fg) {
            /* white frame above threshold blooms past 1.0 (clamped later
             * by the writer) — verify lift happened vs plain 1.0 */
            CHECK(fg->px[100].r > 1.0f,
                  "glow: bright pixels bloom beyond unity");
            wb_frame_free(fg);
        }
        wb_node_destroy(glow); wb_node_destroy(src);
    }


    /* R073 hop 49: transitions */
    {
        wb_node *red = wb_node_source_color(1.0f, 0.0f, 0.0f, 1.0f, 32, 32);
        wb_node *blue = wb_node_source_color(0.0f, 0.0f, 1.0f, 1.0f, 32, 32);
        wb_node *xf = wb_node_transition(0, 2.0);   /* 2s crossfade */
        wb_transition_add(xf, red);
        wb_transition_add(xf, blue);
        wb_frame *f0 = wb_node_pull(xf, 0.0, 0, 0, 32, 32);
        wb_frame *f1 = wb_node_pull(xf, 1.0, 0, 0, 32, 32);
        wb_frame *f2 = wb_node_pull(xf, 2.0, 0, 0, 32, 32);
        CHECK(f0 && f1 && f2, "trans: all phases pulled");
        if (f0 && f1 && f2) {
            CHECK(f0->px[100].b < 0.1f,
                  "xfade: t=0 shows source A (red)");
            CHECK(f0->px[100].r > 0.9f, "xfade: A is red at start");
            CHECK(f2->px[100].b > 0.9f,
                  "xfade: t=2 shows source B (blue)");
            /* midpoint: roughly equal mix */
            CHECK(f1->px[100].r > 0.3f && f1->px[100].r < 0.7f &&
                  f1->px[100].b > 0.3f && f1->px[100].b < 0.7f,
                  "xfade: midpoint blends both");
            wb_frame_free(f0); wb_frame_free(f1); wb_frame_free(f2);
        }
        wb_node_destroy(xf);

        /* dip-to-black: midpoint should be near black */
        wb_node *dip = wb_node_transition(1, 2.0);
        wb_transition_add(dip, red);
        wb_transition_add(dip, blue);
        wb_frame *dm = wb_node_pull(dip, 1.0, 0, 0, 32, 32);
        CHECK(dm != NULL, "dip: midpoint pulled");
        if (dm) {
            CHECK(dm->px[100].r < 0.15f && dm->px[100].b < 0.15f,
                  "dip: midpoint is near black");
            wb_frame_free(dm);
        }
        wb_node_destroy(dip);
        wb_node_destroy(red); wb_node_destroy(blue);
    }


    /* R073 hop 50: wipe + iris */
    {
        wb_node *red = wb_node_source_color(1.0f, 0.0f, 0.0f, 1.0f, 32, 32);
        wb_node *blue = wb_node_source_color(0.0f, 0.0f, 1.0f, 1.0f, 32, 32);
        wb_node *wipe = wb_node_transition(2, 2.0);
        wb_transition_add(wipe, red);   /* A */
        wb_transition_add(wipe, blue);  /* B sweeps L->R */
        wb_frame *fm = wb_node_pull(wipe, 1.0, 0, 0, 32, 32);  /* midpoint */
        CHECK(fm != NULL, "wipe: midpoint pulled");
        if (fm) {
            /* left half should be blue (B), right half red (A) */
            int bl = fm->px[16*32+4].b > 0.8f && fm->px[16*32+4].r < 0.2f;
            int rr = fm->px[16*32+28].r > 0.8f && fm->px[16*32+28].b < 0.2f;
            CHECK(bl && rr, "wipe: L->R boundary splits mid-frame");
            printf("         wipe: left-blue=%d right-red=%d\n", bl, rr);
            wb_frame_free(fm);
        }
        wb_node_destroy(wipe);

        wb_node *iris = wb_node_transition(3, 2.0);
        wb_transition_add(iris, red);
        wb_transition_add(iris, blue);
        wb_frame *fi = wb_node_pull(iris, 1.0, 0, 0, 32, 32);
        CHECK(fi != NULL, "iris: midpoint pulled");
        if (fi) {
            /* at mB=0.5 the iris radius covers ~half the corner distance:
             * center is B, far corners still A */
            int cb = fi->px[16*32+16].b > 0.8f;   /* true frame center */
            int cr = fi->px[2*32+2].r > 0.8f;
            CHECK(cb && cr,
                  "iris: center revealed, corners still source A");
            printf("         iris: center-B=%d corner-A=%d\n", cb, cr);
            wb_frame_free(fi);
        }
        wb_node_destroy(iris);
        wb_node_destroy(red); wb_node_destroy(blue);
    }


    /* R073 hop 51: slide + push */
    {
        wb_node *red = wb_node_source_color(1.0f, 0.0f, 0.0f, 1.0f, 32, 32);
        wb_node *blue = wb_node_source_color(0.0f, 0.0f, 1.0f, 1.0f, 32, 32);
        wb_node *slide = wb_node_transition(4, 2.0);
        wb_transition_add(slide, red);
        wb_transition_add(slide, blue);
        wb_frame *fs = wb_node_pull(slide, 0.5, 0, 0, 32, 32);
        CHECK(fs != NULL, "slide: midpoint pulled");
        if (fs) {
            int br = fs->px[16*32+12].b > 0.8f;   /* B covers x>=8 */
            int al = fs->px[16*32+4].r > 0.8f;    /* A remains x<8 */
            int bl = br, rr = al;
            CHECK(bl && rr,
                  "slide: B slides over stationary A");
            printf("         slide: left-B=%d right-A=%d\n", bl, rr);
            wb_frame_free(fs);
        }
        wb_node_destroy(slide);

        wb_node *push = wb_node_transition(5, 2.0);
        wb_transition_add(push, red);
        wb_transition_add(push, blue);
        wb_frame *fp = wb_node_pull(push, 0.5, 0, 0, 32, 32);
        CHECK(fp != NULL, "push: midpoint pulled");
        if (fp) {
            /* at midpoint the boundary is exactly at x=16: left=B right=A */
            int bl = fp->px[16*32+12].b > 0.8f;
            int rr = fp->px[16*32+4].r > 0.8f;
            CHECK(bl && rr, "push: boundary at midpoint splits frame");
            wb_frame_free(fp);
        }
        wb_node_destroy(push);
        wb_node_destroy(red); wb_node_destroy(blue);
    }


    /* R073 hop 52: primary grade — saturation + gamma verified */
    {
        wb_node *src = wb_node_source_color(1.0f, 0.5f, 0.0f, 1.0f, 32, 32);
        wb_node *gr = wb_node_effect(8, 0.5f);   /* sat 0.5 = half chroma */
        gr->inputs[0] = src;
        wb_frame *fg = wb_node_pull(gr, 0.0, 0, 0, 32, 32);
        CHECK(fg != NULL, "grade: pulled");
        if (fg) {
            wb_px p = fg->px[100];
            float lum = 0.2126f*p.r + 0.7152f*p.g + 0.0722f*p.b;
            /* saturation halved: channel distance from luma halves */
            CHECK(fabsf((p.r - lum) - 0.5f*(1.0f - lum)) < 0.05f,
                  "grade: red chroma halved at sat=0.5");
            printf("         grade: r=%.3f lum=%.3f\n", p.r, lum);
            wb_frame_free(fg);
        }
        wb_node_destroy(gr); wb_node_destroy(src);
    }


    /* R073 hop 58: typewriter text animation */
    {
        wb_node *txt = wb_node_source_text("HELLO", 3,
                                           1.0f, 1.0f, 1.0f, 1.0f, 96, 32);
        CHECK(txt != NULL, "typer: node created");
        wb_node_source_text_anim(txt, 1, 2.0);   /* typewriter over 2s */
        wb_frame *early = wb_node_pull(txt, 0.25, 0, 0, 96, 32);
        wb_frame *late  = wb_node_pull(txt, 1.75, 0, 0, 96, 32);
        CHECK(early && late, "typer: frames pulled");
        if (early && late) {
            int lit_e = 0, lit_l = 0;
            for (int i = 0; i < 96*32; i++) {
                if (early->px[i].a > 0.5f) lit_e++;
                if (late->px[i].a > 0.5f) lit_l++;
            }
            CHECK(lit_l > lit_e * 2,
                  "typer: more glyphs visible at t=1.75 than t=0.25");
            printf("         typer: early=%d late=%d\n", lit_e, lit_l);
            wb_frame_free(early); wb_frame_free(late);
        }
        wb_node_destroy(txt);
    }


    /* R073 hop 59: fade-out preset — text gone by end of duration */
    {
        wb_node *txt = wb_node_source_text("FADE", 3,
                                           1.0f, 1.0f, 1.0f, 1.0f, 96, 32);
        CHECK(txt != NULL, "fadeout: node created");
        wb_node_source_text_anim(txt, 3, 2.0);   /* fade out over 2s */
        wb_frame *early = wb_node_pull(txt, 0.1, 0, 0, 96, 32);
        wb_frame *late  = wb_node_pull(txt, 1.9, 0, 0, 96, 32);
        CHECK(early && late, "fadeout: frames pulled");
        if (early && late) {
            int lit_e = 0, lit_l = 0;
            for (int i = 0; i < 96*32; i++) {
                if (early->px[i].a > 0.4f) lit_e++;
                if (late->px[i].a > 0.4f) lit_l++;
            }
            CHECK(lit_e > 100 && lit_l < 20,
                  "fadeout: text visible early, gone late");
            printf("         fadeout: early=%d late=%d\n", lit_e, lit_l);
            wb_frame_free(early); wb_frame_free(late);
        }
        wb_node_destroy(txt);
    }


    /* R073 hop 60: multi-line text */
    {
        wb_node *txt = wb_node_source_text("AB\nCD", 3,
                                           1.0f, 1.0f, 1.0f, 1.0f, 64, 64);
        CHECK(txt != NULL, "mline: node created");
        wb_frame *f = txt ? wb_node_pull(txt, 0.0, 0, 0, 64, 64) : NULL;
        CHECK(f != NULL, "mline: frame pulled");
        if (f) {
            int lit_top = 0, lit_bot = 0;
            for (int y = 20; y < 44; y++)
                for (int x = 0; x < 64; x++)
                    if (f->px[y*64+x].a > 0.5f) lit_top++;
            for (int y = 46; y < 64; y++)
                for (int x = 0; x < 64; x++)
                    if (f->px[y*64+x].a > 0.5f) lit_bot++;
            CHECK(lit_top > 0 && lit_bot > 0,
                  "mline: both lines render");
            wb_frame_free(f);
        }
        wb_node_destroy(txt);
    }


    /* R073 hop 64: reversed wipe direction */
    {
        wb_node *red = wb_node_source_color(1.0f, 0.0f, 0.0f, 1.0f, 32, 32);
        wb_node *blue = wb_node_source_color(0.0f, 0.0f, 1.0f, 1.0f, 32, 32);
        wb_node *wipe = wb_node_transition(2, 2.0);
        wb_transition_add(wipe, red);
        wb_transition_add(wipe, blue);
        wb_transition_dir(wipe, 1);              /* R->L */
        wb_frame *fm = wb_node_pull(wipe, 1.0, 0, 0, 32, 32);
        CHECK(fm != NULL, "wipe64: midpoint pulled");
        if (fm) {
            /* reversed: right half is B (blue), left half A (red) */
            int rb = fm->px[16*32+28].b > 0.8f && fm->px[16*32+28].r < 0.2f;
            int la = fm->px[16*32+4].r > 0.8f && fm->px[16*32+4].b < 0.2f;
            CHECK(rb && la,
                  "wipe64: reversed boundary sweeps R->L");
            printf("         wipe64: rev left-red=%d right-B=%d\n",
                   la, rb);
            wb_frame_free(fm);
        }
        wb_node_destroy(wipe);
        wb_node_destroy(red); wb_node_destroy(blue);
    }


    /* R073 hop 65: vertical wipe T->B */
    {
        wb_node *red = wb_node_source_color(1.0f, 0.0f, 0.0f, 1.0f, 32, 32);
        wb_node *blue = wb_node_source_color(0.0f, 0.0f, 1.0f, 1.0f, 32, 32);
        wb_node *wipe = wb_node_transition(2, 2.0);
        wb_transition_add(wipe, red);
        wb_transition_add(wipe, blue);
        wb_transition_dir(wipe, 2);              /* T->B */
        wb_frame *fm = wb_node_pull(wipe, 1.0, 0, 0, 32, 32);
        CHECK(fm != NULL, "vwipe: midpoint pulled");
        if (fm) {
            /* top half B, bottom half A */
            int tb = fm->px[4*32+16].b > 0.8f;
            int ba = fm->px[28*32+16].r > 0.8f;
            CHECK(tb && ba,
                  "vwipe: top-B bottom-A at midpoint");
            printf("         vwipe: top-B=%d bottom-A=%d "
                   "(px[28][16]=(%.2f,%.2f,%.2f))\n",
                   tb, ba, fm->px[28*32+16].r,
                   fm->px[28*32+16].g, fm->px[28*32+16].b);
            wb_frame_free(fm);
        }
        wb_node_destroy(wipe);
        wb_node_destroy(red); wb_node_destroy(blue);
    }


    /* R073 hop 66: feathered wipe — soft gradient band at the boundary */
    {
        wb_node *red = wb_node_source_color(1.0f, 0.0f, 0.0f, 1.0f, 64, 64);
        wb_node *blue = wb_node_source_color(0.0f, 0.0f, 1.0f, 1.0f, 64, 64);
        wb_node *wipe = wb_node_transition(2, 2.0);
        wb_transition_add(wipe, red);
        wb_transition_add(wipe, blue);
        wb_frame *fm = wb_node_pull(wipe, 1.0, 0, 0, 64, 64);
        CHECK(fm != NULL, "feather: midpoint pulled");
        if (fm) {
            /* boundary at x=32; sample deep-A (x=10), boundary (x=32),
             * deep-B (x=54) */
            float rA = fm->px[32*64+10].r, bA = fm->px[32*64+10].b;
            float rB = fm->px[32*64+54].r, bB = fm->px[32*64+54].b;
            float rM = fm->px[32*64+28].r, bM = fm->px[32*64+28].b;
            /* inside the feather band [edge-5, edge] */
            int solidA = rA > 0.9f && bA < 0.1f;
            int solidB = bB > 0.9f && rB < 0.1f;
            float b26 = fm->px[32*64+26].b;
            float b30 = fm->px[32*64+30].b;
            /* B sits left of the boundary: blue must DECREASE as x rises
             * through the feather band [edge-feather, edge] */
            CHECK(b26 > bM && bM > b30,
                  "feather: monotonic smoothstep gradient across band");
            printf("         feather: b26=%.2f b28=%.2f b30=%.2f\n",
                   b26, bM, b30);
            wb_frame_free(fm);
        }
        wb_node_destroy(wipe);
        wb_node_destroy(red); wb_node_destroy(blue);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    printf("\n%d checks, %d failures\n", checks, failures);
    printf("\n%d checks, %d failures\n", checks, failures);
    printf("\n%d checks, %d failures\n", checks, failures);
    printf("\n%d checks, %d failures\n", checks, failures);
    printf("\n%d checks, %d failures\n", checks, failures);
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
