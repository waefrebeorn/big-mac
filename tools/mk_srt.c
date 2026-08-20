/* mk_srt.c — build a Big Mac wb_transcript from Whisper word CSV,
 * then export a real SRT via wb_transcript_write_srt. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wbus/wbus_transcript.h"

/* Whisper -ocsv format: start,end,text  (ms,ms,"words") */
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <words.csv> <out.srt>\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "open %s failed\n", argv[1]); return 1; }
    wb_transcript *t = wb_transcript_create();

    char line[4096];
    fgets(line, sizeof(line), f); /* header */
    long n = 0;
    while (fgets(line, sizeof(line), f)) {
        /* parse start,end,"text" */
        char *comma1 = strchr(line, ',');
        if (!comma1) continue;
        char *comma2 = strchr(comma1 + 1, ',');
        if (!comma2) continue;
        *comma1 = 0; *comma2 = 0;
        double start = atof(line);
        double end = atof(comma1 + 1);
        char *txt = comma2 + 1;
        /* strip surrounding quotes + trailing newline */
        if (txt[0] == '"') txt++;
        size_t L = strlen(txt);
        while (L > 0 && (txt[L-1] == '"' || txt[L-1] == '\n' || txt[L-1] == '\r' || txt[L-1]==' '))
            txt[--L] = 0;
        if (L == 0) continue;
        /* split into words, distribute timing evenly across the segment */
        char *save = NULL;
        char *w = strtok_r(txt, " ", &save);
        int cnt = 0; char *ws[512];
        while (w && cnt < 512) { ws[cnt++] = w; w = strtok_r(NULL, " ", &save); }
        if (cnt == 0) continue;
        double span = (end - start) / cnt;
        for (int i = 0; i < cnt; i++)
            wb_transcript_add(t, start + i*span, start + (i+1)*span, ws[i]);
        n++;
    }
    fclose(f);
    fprintf(stderr, "[mk_srt] segments=%ld words=%d duration=%.0fms\n",
            n, wb_transcript_count(t), wb_transcript_duration_ms(t));
    int rc = wb_transcript_write_srt(t, argv[2]);
    wb_transcript_free(t);
    fprintf(stderr, "[mk_srt] wrote %s (rc=%d)\n", argv[2], rc);
    return rc == 0 ? 0 : 1;
}
