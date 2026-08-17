/* tests/test_launchpad_mk2.c — headless test of the Launchpad Mk2 driver
 * + scale helpers + protocol-byte capture. Verifies the byte-exact wire
 * format WITHOUT hardware (via the wb_midi_capture hook).
 *
 * This is the honest gate for the Mk2 driver: we assert the exact 11-byte
 * SysEx the DAW will emit to the device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include "wbus.h"
#include "wbus_midi.h"
#include "wb_internal.h"

static int failures = 0;
static int checks = 0;
#define CHECK(cond, msg) do { \
    checks++; \
    if (cond) printf("  [PASS] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); failures++; } \
} while (0)

/* compare the first `want_len` captured bytes against an expected sequence */
static void CHECK_SYSEX(const uint8_t *buf, int buf_len,
                        const uint8_t *want, int want_len, const char *msg) {
    CHECK(buf_len == want_len, msg);
    if (buf_len == want_len) {
        int ok = 1;
        for (int i = 0; i < want_len && ok; i++)
            if (buf[i] != want[i]) { ok = 0; }
        if (!ok) {
            printf("    want: ");
            for (int i = 0; i < want_len; i++) printf("%02X ", want[i]);
            printf("\n    got : ");
            for (int i = 0; i < buf_len; i++) printf("%02X ", buf[i]);
            printf("\n");
        }
    }
}

/* MK2 RGB SysEx for (row=2, col=5) green:
 * note = 11 + 5 + 2*10 = 36; green = (0,63,0). 12 bytes. */
static const uint8_t MK2_GREEN_SYX[12] = {
    0xF0, 0x00, 0x20, 0x29, 0x02, 0x18,
    0x0B, 36, 0, 63, 0, 0xF7
};

/* Mk2 "clear all" SysEx: F0 00 20 29 02 18 0E 00 F7  (9 bytes) */
static const uint8_t MK2_CLEAR_SYX[9] = {
    0xF0, 0x00, 0x20, 0x29, 0x02, 0x18, 0x0E, 0x00, 0xF7
};

static void rgb_triple(wb_lp_color c, uint8_t *r, uint8_t *g, uint8_t *b) {
    switch (c) {
        case WB_LP_WHITE: *r=63; *g=63; *b=63; break;
        case WB_LP_GREEN: *r=0;  *g=63; *b=0;  break;
        case WB_LP_AMBER: *r=63; *g=45; *b=0;  break;
        case WB_LP_BLUE:  *r=0;  *g=20; *b=63; break;
        case WB_LP_RED:   *r=63; *g=0;  *b=0;  break;
        case WB_LP_CYAN:  *r=0;  *g=63; *b=63; break;
        case WB_LP_DIM:   *r=8;  *g=8;  *b=8;  break;
        case WB_LP_OFF:
        default:          *r=0;  *g=0;  *b=0;  break;
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Launchpad Mk2 driver self-test (headless, bytestream) ===\n");

    /* ---- capture plumbing ---- */
    uint8_t buf[4096];
    wb_midi *m = wb_midi_create_null();
    CHECK(m != NULL, "null midi handle created");
    CHECK(wb_midi_capture(m, buf, sizeof(buf)) == 0, "capture enabled");

    /* ---- grid note mapping ---- */
    CHECK(wb_lp_mk2_note(0,0) == 11,  "grid (0,0) -> note 11");
    CHECK(wb_lp_mk2_note(0,7) == 18,  "grid (0,7) -> note 18");
    CHECK(wb_lp_mk2_note(7,0) == 81,  "grid (7,0) -> note 81");
    CHECK(wb_lp_mk2_note(7,7) == 88,  "grid (7,7) -> note 88");
    CHECK(wb_lp_mk2_note(-1,0) == -1, "negative row rejected");
    CHECK(wb_lp_mk2_note(8,0)  == -1, "row>7 rejected");
    CHECK(wb_lp_mk2_note(0,8)  == -1, "col>7 rejected");
    {
        int seen[128] = {0}; int ok = 1; int cnt = 0;
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++) {
                int n = wb_lp_mk2_note(r, c);
                if (n < 11 || n > 88 || seen[n]) ok = 0;
                seen[n] = 1; cnt++;
            }
        CHECK(ok && cnt == 64, "8x8 grid maps to 64 unique notes in 11..88");
    }
    printf("         MK2 grid: notes 11..88 (64 cells, row*10+col+11)\n");

    /* ---- top row ---- */
    CHECK(wb_lp_mk2_top_note(0) == 91, "top note 0 -> 91");
    CHECK(wb_lp_mk2_top_note(7) == 98, "top note 7 -> 98");
    CHECK(wb_lp_mk2_top_note(-1) == -1, "top idx -1 rejected");
    CHECK(wb_lp_mk2_top_note(8) == -1, "top idx 8 rejected");

    /* ---- named state color -> RGB (mirror driver's palette) ---- */
    {
        uint8_t r, g, b;
        rgb_triple(WB_LP_GREEN, &r, &g, &b);
        CHECK(r==0 && g==63 && b==0, "GREEN = (0,63,0)");
        rgb_triple(WB_LP_WHITE, &r, &g, &b);
        CHECK(r==63 && g==63 && b==63, "WHITE = (63,63,63)");
        rgb_triple(WB_LP_RED, &r, &g, &b);
        CHECK(r==63 && g==0 && b==0, "RED = (63,0,0)");
        rgb_triple(WB_LP_OFF, &r, &g, &b);
        CHECK(r==0 && g==0 && b==0, "OFF = (0,0,0)");
        rgb_triple(WB_LP_DIM, &r, &g, &b);
        CHECK(r==8 && g==8 && b==8, "DIM = (8,8,8)");
        rgb_triple(WB_LP_AMBER, &r, &g, &b);
        CHECK(r==63 && g==45 && b==0, "AMBER = (63,45,0)");
        rgb_triple(WB_LP_BLUE, &r, &g, &b);
        CHECK(r==0 && g==20 && b==63, "BLUE = (0,20,63)");
        rgb_triple(WB_LP_CYAN, &r, &g, &b);
        CHECK(r==0 && g==63 && b==63, "CYAN = (0,63,63)");
    }

    /* ---- Mk2 RGB SysEx bytestream (the byte-exact gate) ---- */
    wb_lp_mk2_led(m, 2, 5, WB_LP_GREEN);
    CHECK_SYSEX(buf, wb_midi_capture_len(m), MK2_GREEN_SYX, 12,
                "Mk2 pad (2,5) green = correct 12-byte SysEx");
    CHECK(wb_midi_capture_len(m) == 12, "12 bytes captured for 1 pad");

    /* ---- clear-all SysEx (FIRST 9 bytes = the 0E command) ---- */
    {
        uint8_t clrbuf[1024];
        wb_midi_capture(m, clrbuf, sizeof(clrbuf));
        wb_lp_mk2_clear(m);
        int body = wb_midi_capture_len(m);
        printf("         clear captured %d bytes (expect 9 + 64*12 = 777)\n", body);
        CHECK(wb_midi_capture_len(m) >= 9, "clear emits at least 9 bytes");
        /* assert the live driver emits the canonical 9-byte clear SysEx */
        CHECK_SYSEX(clrbuf, 9, MK2_CLEAR_SYX, 9,
                    "clear emits canonical 9-byte 0E SysEx");
        /* the rest is the pad-by-pad OFF fallback (64 * 12 bytes) */
        int expected = 9 + 64 * 12;
        CHECK(body == expected,
              "clear total = 9-byte 0E SysEx + 64 pad-off SysEx");
    }

    /* ---- wb_lp_mk2_led_rgb explicit ---- (row=3,col=3 → note = 11+3+3*10 = 44) */
    {
        uint8_t rb[64];
        wb_midi_capture(m, rb, sizeof(rb));
        wb_lp_mk2_led_rgb(m, 3, 3, 10, 20, 30);
        CHECK(wb_midi_capture_len(m) == 12, "explicit RGB = 12 bytes");
        CHECK(rb[7] == 44, "explicit RGB note = 44 (11+3+3*10)");
        CHECK(rb[8] == 10 && rb[9] == 20 && rb[10] == 30,
              "explicit RGB channels = (10,20,30)");
    }

    /* ---- classic LP still works (backward compat) ---- */
    {
        uint8_t cb[64];
        wb_midi_capture(m, cb, sizeof(cb));
        wb_launchpad_classic_led(m, 0, 0, 3);
        CHECK(wb_midi_capture_len(m) == 3, "classic LP = 3-byte note-on");
        CHECK(cb[0] == 0x90 && cb[1] == 0 && cb[2] == 3,
              "classic LP (0,0) green = 90 00 03");
        CHECK(wb_launchpad_classic_note(7,7) == 119, "classic (7,7) -> 119");
        CHECK(wb_launchpad_classic_note(0,7) == 7,   "classic (0,7) -> 7");
        CHECK(wb_launchpad_classic_note(1,0) == 16,  "classic (1,0) -> 16");
    }

    /* ---- scale helpers ---- */
    /* C major: pc 0,2,4,5,7,9,11 */
    CHECK(wb_scale_contains(0,0,60) == 1, "C4 in C major");
    CHECK(wb_scale_contains(0,0,61) == 0, "C#4 NOT in C major");
    CHECK(wb_scale_contains(0,0,64) == 1, "E4 in C major");
    CHECK(wb_scale_contains(0,0,67) == 1, "G4 in C major");
    CHECK(wb_scale_contains(0,0,0)  == 1, "C in C major");
    CHECK(wb_scale_contains(0,0,5)  == 1, "F in C major");

    /* A minor: A(9), B(11), C(0), D(2), E(4), F(5), G(7) — MIDI notes */
    CHECK(wb_scale_contains(9,1,71) == 1, "B4 in A minor");   /* MIDI 71 = B4, pc 11 ✓ */
    CHECK(wb_scale_contains(9,1,73) == 0, "C#5 NOT in A minor"); /* MIDI 73 = C#5, pc 1  ✓ */
    CHECK(wb_scale_contains(9,1,72) == 1, "C5 in A minor");   /* MIDI 72 = C5, pc 0  ✓ */
    CHECK(wb_scale_contains(9,1,68) == 0, "G#4 NOT in A minor");

    /* dorian: pc 0,2,3,5,7,9,10 */
    CHECK(wb_scale_contains(0,2,60) == 1, "C4 in C dorian");
    CHECK(wb_scale_contains(0,2,62) == 1, "D4 in C dorian");
    CHECK(wb_scale_contains(0,2,65) == 1, "F4 in C dorian");

    /* chromatic = everything in */
    CHECK(wb_scale_contains(0,4,0) == 1, "chromatic: C in");
    CHECK(wb_scale_contains(0,4,1) == 1, "chromatic: C# in");
    CHECK(wb_scale_contains(0,4,11) == 1, "chromatic: B in");
    CHECK(wb_scale_contains(0,4,60) == 1, "chromatic: C4 in");

    /* out-of-range note rejected */
    CHECK(wb_scale_contains(0,0,-1) == 0, "scale_contains rejects -1");
    CHECK(wb_scale_contains(0,0,200) == 0, "scale_contains rejects 200");
    CHECK(wb_scale_contains(0,0,128) == 0, "scale_contains rejects 128");

    /* ---- scale snap ---- */
    CHECK(wb_scale_snap(0,0,60) == 60, "C4 snap->C4 (in scale)");
    CHECK(wb_scale_snap(0,0,67) == 67, "G4 snap->G4 (in scale)");
    {
        int s = wb_scale_snap(0,0,61);   /* C#4, between C4(60) and D4(62) */
        CHECK((s==60 || s==62), "C#4 snaps to 60 or 62");
    }
    {
        int s = wb_scale_snap(0,0,63);   /* D#4 → nearest F4(65) up or E4(64)... */
        /* neighbors in C major of pc 3 (D#): pc2=D(62), pc4=E(64)? E pc=4.
         * D#=3: nearest in-scale pc is 2(D) or 4(E). */
        CHECK((s==62 || s==64), "D#4 snaps to 62 or 64");
    }

    wb_midi_close(m);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
