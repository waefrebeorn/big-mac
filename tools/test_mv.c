#include <stdio.h>
#include "wbus/wbus.h"
#include "wbus/wbus_agent.h"
#include "wbus/wbus_video.h"
#include "wbus/wbus_agent.h"

int main(void){
    wb_session *s = wb_session_create();
    s->length = 44100.0 * 20;
    wb_engine *e = wb_engine_create();
    /* R071 debug: replicate the agent path exactly — command handler calls
     * checkpoint BEFORE do_import_audio. */
    wb_agent_command(s, e, "checkpoint");

    FILE *scr = fopen("/tmp/mv_script.txt","r");
    if (!scr){ printf("no script\n"); return 1; }
    int rc = wb_agent_run(scr, s, e);
    printf("run rc=%d\n", rc);
    /* verify session contents */
    for (uint32_t t = 0; t < s->track_count; t++){
        wb_track *tr = &s->tracks[t];
        printf("track %u (%s): %u clips\n", t,
               tr->kind == 3 ? "video" : tr->kind == 1 ? "audio" : "?",
               tr->clip_count);
        for (uint32_t c = 0; c < tr->clip_count && c < 4; c++){
            wb_clip *cl = &tr->clips[c];
            double st = cl->type==1 ? cl->start/44100.0 : cl->start;
            printf("   clip %u type=%d start=%.2fs len=%.2fs\n",
                   c, cl->type, st, cl->type==1 ? cl->length/44100.0 : cl->length);
        }
    }
    printf("markers=%d\n", s->marker_count);
    for (int i = 0; i < s->marker_count && i < 5; i++)
        printf("   marker '%s' @ %.2fs\n", s->markers[i].label,
               s->markers[i].pos/44100.0);
    wb_engine_destroy(e);
    return rc;
}
