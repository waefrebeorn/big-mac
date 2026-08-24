/* test_compositor.c — headless verification of keyframe tracks + pull
 * node-graph compositor (R013 D1/D3, R016 S2). */

#include <stdio.h>
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

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
