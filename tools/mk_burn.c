/* mk_burn.c — burn an SRT into a video via Big Mac's wb_captions_burn
 * (ffmpeg subtitles filter). input video + srt -> output mp4. */
#include <stdio.h>
#include "wbus/wbus_captions.h"

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <in.mp4> <in.srt> <out.mp4>\n", argv[0]); return 1; }
    int rc = wb_captions_burn(argv[1], argv[2], argv[3]);
    fprintf(stderr, "[mk_burn] wb_captions_burn rc=%d (%s)\n", rc, rc==0?"ok":"fail");
    return rc == 0 ? 0 : 1;
}
