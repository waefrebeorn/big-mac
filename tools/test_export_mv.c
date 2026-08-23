#include <stdio.h>
#include "wbus/wbus.h"
#include "wbus/wbus_agent.h"
#include "wbus/wbus_cgiexport.h"
int main(void){
    wb_session *s = wb_session_create();
    s->length = 44100.0 * 20;
    wb_engine *e = wb_engine_create();
    FILE *scr = fopen("/tmp/mv_script.txt","r");
    if (!scr){ printf("no script\n"); return 1; }
    int rc = wb_agent_run(scr, s, e);
    fclose(scr);
    printf("build rc=%d\n", rc);
    int erc = wb_video_export_perf_overlays(s, e,
        "/Users/waefrebeorn/Documents/big-mac/test_media/musicvideo/mv_test_export.mp4",
        NULL, WB_VIDEO_CODEC_H264);
    printf("export rc=%d\n", erc);
    wb_engine_destroy(e);
    return erc;
}
