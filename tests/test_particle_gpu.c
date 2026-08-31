/* tests/test_particle_gpu.c — GPU particle system test suite.
 *
 * Tests: create/destroy, emit, update motion, render output, gravity, lifetime decay.
 * Links only with build/src/wb_particle_gpu.o (no full engine needed).
 *
 * Pure C11. */

#include "wbus.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } \
    else { printf("  ok: %s\n", msg); } \
} while (0)

int main(void) {
    printf("=== test_particle_gpu ===\n");

    /* 1. Create / destroy */
    printf("\n[1] Create/destroy\n");
    {
        void *ps = wb_gpu_particle_create(1000);
        CHECK(ps != NULL, "create returns non-NULL");
        wb_gpu_particle_destroy(ps);
        wb_gpu_particle_destroy(NULL); /* should not crash */
        CHECK(1, "destroy does not crash");
    }

    /* 2. Emit particles */
    printf("\n[2] Emit particles\n");
    {
        void *ps = wb_gpu_particle_create(1000);
        CHECK(ps != NULL, "create for emit test");

        wb_gpu_particle_emit(ps, 0.0f, 0.0f, 0.0f, 100);
        int count = wb_gpu_particle_get_active_count(ps);
        CHECK(count == 100, "emit 100 -> active count = 100");

        wb_gpu_particle_emit(ps, 1.0f, 2.0f, 3.0f, 50);
        count = wb_gpu_particle_get_active_count(ps);
        CHECK(count == 150, "emit 50 more -> active count = 150");

        wb_gpu_particle_destroy(ps);
    }

    /* 3. Update moves particles */
    printf("\n[3] Update moves particles\n");
    {
        void *ps = wb_gpu_particle_create(1000);
        wb_gpu_particle_set_gravity(ps, 0.0f, 0.0f, 0.0f); /* no gravity for this test */
        wb_gpu_particle_set_lifetime(ps, 10.0f, 10.0f);    /* long life */
        wb_gpu_particle_emit(ps, 0.0f, 0.0f, 0.0f, 10);

        /* We can't read particle positions directly (opaque), but we can
         * verify that after update, particles are still alive and count unchanged */
        wb_gpu_particle_update(ps, 0.016f); /* ~1 frame at 60fps */
        int count = wb_gpu_particle_get_active_count(ps);
        CHECK(count == 10, "particles still alive after 1 update (no gravity)");

        /* Run several more updates — particles should still be alive */
        for (int i = 0; i < 60; i++)
            wb_gpu_particle_update(ps, 0.016f);
        count = wb_gpu_particle_get_active_count(ps);
        CHECK(count == 10, "particles alive after 1 second (10s lifetime)");

        wb_gpu_particle_destroy(ps);
    }

    /* 4. Render produces non-zero pixels */
    printf("\n[4] Render produces non-zero pixels\n");
    {
        void *ps = wb_gpu_particle_create(1000);
        wb_gpu_particle_set_gravity(ps, 0.0f, 0.0f, 0.0f);
        wb_gpu_particle_set_lifetime(ps, 10.0f, 10.0f);
        wb_gpu_particle_set_colors(ps, 0xFFFF0000, 0xFF00FF00); /* red to green */
        wb_gpu_particle_set_size(ps, 4.0f, 4.0f);
        wb_gpu_particle_emit(ps, 0.0f, 0.0f, 5.0f, 50); /* emit at z=5 (in front of camera) */

        int w = 320, h = 240;
        uint8_t *rgba = (uint8_t *)calloc(w * h * 4, 1);
        CHECK(rgba != NULL, "allocate RGBA buffer");

        wb_gpu_particle_render(ps, rgba, w, h);

        /* Count non-zero pixels */
        int non_zero = 0;
        for (int i = 0; i < w * h * 4; i++) {
            if (rgba[i] != 0) { non_zero++; break; }
        }
        CHECK(non_zero > 0, "render produced non-zero pixels");

        /* Count total lit pixels for a stronger assertion */
        int lit_pixels = 0;
        for (int p = 0; p < w * h; p++) {
            if (rgba[p*4] != 0 || rgba[p*4+1] != 0 || rgba[p*4+2] != 0)
                lit_pixels++;
        }
        printf("  lit_pixels = %d\n", lit_pixels);
        CHECK(lit_pixels >= 10, "at least 10 lit pixels from 50 particles");

        free(rgba);
        wb_gpu_particle_destroy(ps);
    }

    /* 5. Gravity affects trajectory */
    printf("\n[5] Gravity affects trajectory\n");
    {
        /* With gravity, particles should fall. We compare centroids. */
        void *ps1 = wb_gpu_particle_create(1000);
        void *ps2 = wb_gpu_particle_create(1000);

        /* ps1: no gravity */
        wb_gpu_particle_set_gravity(ps1, 0.0f, 0.0f, 0.0f);
        wb_gpu_particle_set_lifetime(ps1, 10.0f, 10.0f);
        wb_gpu_particle_set_size(ps1, 4.0f, 4.0f);
        wb_gpu_particle_emit(ps1, 0.0f, 0.0f, 5.0f, 50);

        /* ps2: moderate negative Y gravity */
        wb_gpu_particle_set_gravity(ps2, 0.0f, -10.0f, 0.0f);
        wb_gpu_particle_set_lifetime(ps2, 10.0f, 10.0f);
        wb_gpu_particle_set_size(ps2, 4.0f, 4.0f);
        wb_gpu_particle_emit(ps2, 0.0f, 0.0f, 5.0f, 50);

        /* Update for 0.2s — enough for gravity to shift but not fly off */
        for (int i = 0; i < 12; i++) {
            wb_gpu_particle_update(ps1, 0.016f);
            wb_gpu_particle_update(ps2, 0.016f);
        }

        int w = 320, h = 240;
        uint8_t *rgba1 = (uint8_t *)calloc(w * h * 4, 1);
        uint8_t *rgba2 = (uint8_t *)calloc(w * h * 4, 1);

        wb_gpu_particle_render(ps1, rgba1, w, h);
        wb_gpu_particle_render(ps2, rgba2, w, h);

        /* Compute centroid Y for each */
        int sum_y1 = 0, count1 = 0, sum_y2 = 0, count2 = 0;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int idx = (y * w + x) * 4;
                if (rgba1[idx] || rgba1[idx+1] || rgba1[idx+2]) {
                    sum_y1 += y; count1++;
                }
                if (rgba2[idx] || rgba2[idx+1] || rgba2[idx+2]) {
                    sum_y2 += y; count2++;
                }
            }
        }

        printf("  no-gravity: %d lit pixels, gravity: %d lit pixels\n", count1, count2);

        /* With downward gravity, centroid should be lower (higher y) */
        if (count1 > 0 && count2 > 0) {
            float cy1 = (float)sum_y1 / count1;
            float cy2 = (float)sum_y2 / count2;
            printf("  no-gravity centroid y = %.1f, gravity centroid y = %.1f\n", cy1, cy2);
            CHECK(cy2 > cy1, "gravity pulls particles downward (higher screen y)");
        } else {
            /* Fallback: just verify both have particles on screen */
            CHECK(count1 > 0 && count2 > 0, "both renders produced visible particles");
        }

        free(rgba1);
        free(rgba2);
        wb_gpu_particle_destroy(ps1);
        wb_gpu_particle_destroy(ps2);
    }

    /* 6. Active count decreases over time */
    printf("\n[6] Active count decreases over time\n");
    {
        void *ps = wb_gpu_particle_create(1000);
        wb_gpu_particle_set_gravity(ps, 0.0f, 0.0f, 0.0f);
        wb_gpu_particle_set_lifetime(ps, 0.5f, 0.5f); /* short life */
        wb_gpu_particle_emit(ps, 0.0f, 0.0f, 0.0f, 200);

        int initial = wb_gpu_particle_get_active_count(ps);
        CHECK(initial == 200, "200 particles emitted");

        /* Update for 0.3s — should still be alive */
        for (int i = 0; i < 18; i++)
            wb_gpu_particle_update(ps, 0.016f);
        int mid = wb_gpu_particle_get_active_count(ps);
        CHECK(mid == 200, "all alive at 0.3s (0.5s lifetime)");

        /* Update past lifetime */
        for (int i = 0; i < 30; i++)
            wb_gpu_particle_update(ps, 0.016f);
        int dead = wb_gpu_particle_get_active_count(ps);
        printf("  after ~0.8s: active = %d\n", dead);
        CHECK(dead == 0, "all particles dead after lifetime expires");

        wb_gpu_particle_destroy(ps);
    }

    /* 7. Max particles cap */
    printf("\n[7] Max particles cap\n");
    {
        void *ps = wb_gpu_particle_create(50);
        wb_gpu_particle_set_lifetime(ps, 10.0f, 10.0f);
        wb_gpu_particle_emit(ps, 0.0f, 0.0f, 0.0f, 100); /* try to emit 100, cap at 50 */
        int count = wb_gpu_particle_get_active_count(ps);
        CHECK(count == 50, "emitting 100 into pool of 50 caps at 50");
        wb_gpu_particle_destroy(ps);
    }

    printf("\n=== %d failures ===\n", fails);
    return fails > 0 ? 1 : 0;
}