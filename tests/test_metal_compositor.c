/* test_metal_compositor.c — verify Metal GPU compositor output matches CPU.
 *
 * Tests the GPU-accelerated color grade path by:
 *   1. Running the CPU effect (op 8) on a test frame
 *   2. Running the Metal GPU grade on an identical copy
 *   3. Comparing pixel outputs within float tolerance
 *
 * If Metal is unavailable, the test verifies the graceful fallback. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_compositor.h"

static int failures = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

/* CPU reference: apply primary grade matching the shader's math */
static void cpu_grade(wb_frame *f, float lift, float gamma, float gain, float sat) {
    for (int i = 0; i < f->w * f->h; i++) {
        wb_px *p = &f->px[i];
        float r = p->r + lift, g = p->g + lift, b = p->b + lift;
        if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
        if (r > 1) r = 1; if (g > 1) g = 1; if (b > 1) b = 1;

        /* sRGB decode */
        float lr = r <= 0.04045f ? r / 12.92f : powf((r + 0.055f) / 1.055f, 2.4f);
        float lg = g <= 0.04045f ? g / 12.92f : powf((g + 0.055f) / 1.055f, 2.4f);
        float lb = b <= 0.04045f ? b / 12.92f : powf((b + 0.055f) / 1.055f, 2.4f);

        /* gamma + gain */
        if (gamma <= 0.0f) gamma = 1.0f;
        if (gain <= 0.0f) gain = 1.0f;
        lr = powf(lr, 1.0f / gamma) * gain;
        lg = powf(lg, 1.0f / gamma) * gain;
        lb = powf(lb, 1.0f / gamma) * gain;

        /* sRGB encode */
        r = lr <= 0.0031308f ? lr * 12.92f : 1.055f * powf(lr, 1.0f/2.4f) - 0.055f;
        g = lg <= 0.0031308f ? lg * 12.92f : 1.055f * powf(lg, 1.0f/2.4f) - 0.055f;
        b = lb <= 0.0031308f ? lb * 12.92f : 1.055f * powf(lb, 1.0f/2.4f) - 0.055f;

        /* saturation */
        float lum = 0.2126f*r + 0.7152f*g + 0.0722f*b;
        p->r = lum + (r - lum) * sat;
        p->g = lum + (g - lum) * sat;
        p->b = lum + (b - lum) * sat;
        /* clamp to [0,1] to match GPU path */
        if (p->r < 0.0f) p->r = 0.0f; if (p->r > 1.0f) p->r = 1.0f;
        if (p->g < 0.0f) p->g = 0.0f; if (p->g > 1.0f) p->g = 1.0f;
        if (p->b < 0.0f) p->b = 0.0f; if (p->b > 1.0f) p->b = 1.0f;
    }
}

int main(void) {
    printf("=== Metal GPU Compositor Test ===\n\n");

    /* ---- Test 1: backend API ---- */
    printf("-- backend API --\n");
    wb_compositor_set_backend(WB_RENDER_CPU);
    CHECK(wb_compositor_get_backend() == WB_RENDER_CPU, "CPU backend set/get");

    wb_compositor_set_backend(WB_RENDER_GPU);
    CHECK(wb_compositor_get_backend() == WB_RENDER_GPU, "GPU backend set/get (flag stored)");

    wb_compositor_set_backend(WB_RENDER_CPU);  /* reset */
    CHECK(wb_compositor_get_backend() == WB_RENDER_CPU, "CPU backend restored");

    /* ---- Test 2: Metal availability ---- */
    printf("\n-- Metal availability --\n");
    int metal_ok = wb_compositor_metal_init();
    if (metal_ok == 0) {
        CHECK(wb_compositor_metal_is_available(), "Metal is available");
        printf("  [INFO] Metal GPU path active\n");
    } else {
        CHECK(!wb_compositor_metal_is_available(), "Metal unavailable (expected on non-GPU systems)");
        printf("  [INFO] Metal unavailable — CPU fallback will be used\n");
    }

    /* ---- Test 3: GPU grade matches CPU ---- */
    if (wb_compositor_metal_is_available()) {
        printf("\n-- GPU vs CPU grade comparison --\n");

        /* Create test frame with gradient */
        int W = 64, H = 64;
        wb_frame *f_cpu = wb_frame_alloc(W, H);
        wb_frame *f_gpu = wb_frame_alloc(W, H);
        CHECK(f_cpu && f_gpu, "test frames allocated");

        /* Fill with a gradient pattern */
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                float u = (float)x / (float)(W - 1);
                float v = (float)y / (float)(H - 1);
                wb_px px = { u, v, 1.0f - u, 1.0f };
                f_cpu->px[y * W + x] = px;
                f_gpu->px[y * W + x] = px;
            }
        }

        /* Grade parameters */
        float lift = 0.05f, gamma = 1.2f, gain = 1.1f, sat = 1.5f;

        /* CPU reference */
        cpu_grade(f_cpu, lift, gamma, gain, sat);

        /* GPU path */
        int gpu_result = wb_compositor_metal_process_grade(f_gpu, lift, gamma, gain, sat);
        CHECK(gpu_result == 0, "GPU grade call succeeded");

        /* Compare */
        int n = W * H;
        if (gpu_result == 0) {
            float max_err = 0.0f;
            int max_px = 0;
            for (int i = 0; i < n; i++) {
                float er = fabsf(f_cpu->px[i].r - f_gpu->px[i].r);
                float eg = fabsf(f_cpu->px[i].g - f_gpu->px[i].g);
                float eb = fabsf(f_cpu->px[i].b - f_gpu->px[i].b);
                float ea = fabsf(f_cpu->px[i].a - f_gpu->px[i].a);
                float pixel_max = fmaxf(er, fmaxf(eg, fmaxf(eb, ea)));
                if (pixel_max > max_err) { max_err = pixel_max; max_px = i; }
            }
            printf("  [INFO] max pixel error: %.6f at px[%d]\n", max_err, max_px);
            if (max_px < n) {
                wb_px cpu = f_cpu->px[max_px];
                wb_px gpu = f_gpu->px[max_px];
                printf("  [DBG] CPU=(%.6f,%.6f,%.6f,%.6f) GPU=(%.6f,%.6f,%.6f,%.6f)\n",
                       cpu.r, cpu.g, cpu.b, cpu.a, gpu.r, gpu.g, gpu.b, gpu.a);
            }
            float avg_err = 0.0f;
            for (int i = 0; i < n; i++) {
                avg_err += fabsf(f_cpu->px[i].r - f_gpu->px[i].r);
                avg_err += fabsf(f_cpu->px[i].g - f_gpu->px[i].g);
                avg_err += fabsf(f_cpu->px[i].b - f_gpu->px[i].b);
                avg_err += fabsf(f_cpu->px[i].a - f_gpu->px[i].a);
            }
            avg_err /= (n * 4.0f);
            printf("  [INFO] avg error: %.6f\n", avg_err);
            CHECK(max_err < 0.01f, "GPU grade matches CPU within 1% tolerance");
            CHECK(max_err < 0.001f, "GPU grade matches CPU within 0.1% (strict)");
        }

        /* ---- Test 4: GPU gain matches CPU ---- */
        printf("\n-- GPU vs CPU gain comparison --\n");
        wb_frame *g_cpu = wb_frame_alloc(W, H);
        wb_frame *g_gpu = wb_frame_alloc(W, H);
        for (int i = 0; i < W * H; i++) {
            wb_px px = { 0.3f, 0.5f, 0.7f, 1.0f };
            g_cpu->px[i] = px;
            g_gpu->px[i] = px;
        }
        float gain_val = 1.5f;
        /* CPU gain */
        for (int i = 0; i < W * H; i++) {
            g_cpu->px[i].r *= gain_val;
            g_cpu->px[i].g *= gain_val;
            g_cpu->px[i].b *= gain_val;
            /* clamp */
            if (g_cpu->px[i].r > 1.0f) g_cpu->px[i].r = 1.0f;
            if (g_cpu->px[i].g > 1.0f) g_cpu->px[i].g = 1.0f;
            if (g_cpu->px[i].b > 1.0f) g_cpu->px[i].b = 1.0f;
        }
        int gpu_gain = wb_compositor_metal_process_gain(g_gpu, gain_val);
        CHECK(gpu_gain == 0, "GPU gain call succeeded");
        if (gpu_gain == 0) {
            float max_err = 0.0f;
            for (int i = 0; i < W * H; i++) {
                float er = fabsf(g_cpu->px[i].r - g_gpu->px[i].r);
                float eg = fabsf(g_cpu->px[i].g - g_gpu->px[i].g);
                float eb = fabsf(g_cpu->px[i].b - g_gpu->px[i].b);
                float m = fmaxf(er, fmaxf(eg, eb));
                if (m > max_err) max_err = m;
            }
            printf("  [INFO] gain max error: %.6f\n", max_err);
            CHECK(max_err < 1e-5f, "GPU gain matches CPU exactly (float ops)");
        }

        wb_frame_free(f_cpu);
        wb_frame_free(f_gpu);
        wb_frame_free(g_cpu);
        wb_frame_free(g_gpu);

        /* ---- Test 5: GPU deep fry runs ---- */
        printf("\n-- GPU deep fry --\n");
        wb_frame *df_gpu = wb_frame_alloc(32, 32);
        for (int i = 0; i < 32 * 32; i++) {
            df_gpu->px[i].r = 0.5f;
            df_gpu->px[i].g = 0.3f;
            df_gpu->px[i].b = 0.7f;
            df_gpu->px[i].a = 1.0f;
        }
        int df_result = wb_compositor_metal_process_deep_fry(df_gpu, 3.0f, 2.0f, 1.3f, 0.1f);
        CHECK(df_result == 0, "GPU deep fry call succeeded");
        /* Verify output is in valid range */
        if (df_result == 0) {
            int valid = 1;
            for (int i = 0; i < 32 * 32; i++) {
                if (df_gpu->px[i].r < 0.0f || df_gpu->px[i].r > 1.0f ||
                    df_gpu->px[i].g < 0.0f || df_gpu->px[i].g > 1.0f ||
                    df_gpu->px[i].b < 0.0f || df_gpu->px[i].b > 1.0f) {
                    valid = 0; break;
                }
            }
            CHECK(valid, "deep fry output in valid [0,1] range");
        }
        wb_frame_free(df_gpu);

        wb_compositor_metal_shutdown();
    } else {
        printf("\n-- GPU tests skipped (Metal unavailable) --\n");
        printf("  [INFO] CPU fallback is authoritative; GPU tests deferred\n");
    }

    /* ---- Summary ---- */
    printf("\n=== %d/%d checks passed", checks - failures, checks);
    if (failures) printf(", %d FAILED", failures);
    printf(" ===\n");

    return failures ? 1 : 0;
}