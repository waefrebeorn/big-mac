/* test_captions.c — headless verification of the captions pipeline + G10
 * ASS styled-caption parser. The parser is pure C and testable without
 * ffmpeg; the burn path is exercised by the full export test when a clip
 * with an .ass file is present. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wbus/wbus_captions.h"

static int failures = 0, checks = 0;
#define CK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

int main(void) {
    printf("=== Captions + G10 ASS parser ===\n");

    /* Write a minimal but real ASS file with inline overrides.
     * NOTE: \\b \\i \\c \\pos are literal backslash tags; \n is a real
     * newline separating the file's lines. */
    const char *ass =
        "[Script Info]\n"
        "Title: Test\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour\n"
        "Style: Default,Arial,48,&H00FFFFFF,0,0,0,0\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:01.00,0:00:04.00,Default,,0,0,0,,Hello \\b1world\\b0 \\i1!\n"
        "Dialogue: 0,0:00:05.50,0:00:08.00,Default,,0,0,0,,\\c&H00FF00&\\pos(100,200)Colored\n";
    FILE *f = fopen("/tmp/test_cap.ass", "w");
    if (f) { fputs(ass, f); fclose(f); }

    wb_ass_line lines[8];
    int n = wb_ass_extract_dialogue("/tmp/test_cap.ass", lines, 8);
    CK(n == 2, "parsed 2 Dialogue lines");

    if (n >= 1) {
        /* text: \b1..world\b0 (bold toggles off) then \i1! (italic on) */
        CK(lines[0].start_ms == 1000, "line1 start 1.00s -> 1000ms");
        CK(lines[0].end_ms   == 4000, "line1 end 4.00s -> 4000ms");
        CK(strstr(lines[0].text, "world") != NULL, "line1 text contains 'world'");
        CK(strstr(lines[0].text, "\\b1") == NULL, "line1 \\b1 override stripped");
        CK(strstr(lines[0].text, "\\i1") == NULL, "line1 \\i1 override stripped");
        CK(lines[0].bold == 0, "line1 bold ends off (\\b1 then \\b0)");
        CK(lines[0].italic == 1, "line1 italic ends on (\\i1)");
    }
    if (n >= 2) {
        CK(lines[1].color_rgb == 0x00FF00, "line2 color &H00FF00& -> 0x00FF00 (BGR->RGB)");
        CK(lines[1].pos_x == 100 && lines[1].pos_y == 200,
           "line2 \\pos(100,200) parsed");
        CK(strstr(lines[1].text, "Colored") != NULL, "line2 text correct");
        CK(lines[1].color_rgb != -1, "line2 color set (not default -1)");
    }

    /* SRT burn helper still present (real ffmpeg path used by export test). */
    CK(wb_captions_burn != NULL, "SRT burn entry exists");
    CK(wb_captions_burn_ass != NULL, "ASS burn entry exists (G10)");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
