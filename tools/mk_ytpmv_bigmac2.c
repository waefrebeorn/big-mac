/* mk_ytpmv_bigmac2.c — YTPMV Producer using Big Mac's sampler engine.
 * Usage: mk_ytpmv_bigmac2 <sample.wav> <events.csv> <output.wav> [--transpose N]
 * events.csv: note,start_sec,dur_sec,velocity (one per line, # for comments)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "wbus/wbus_compositor.h"

#define MAX_EVENTS 8192
#define MAX_SAMPLE (1024*1024)

typedef struct { int note; float start, dur, vel; } event_t;

static float sample_audio[MAX_SAMPLE];
static int sample_frames = 0;

int load_wav(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char riff[4], wave[4]; uint32_t sz;
    fread(riff,1,4,f); fread(&sz,4,1,f); fread(wave,1,4,f);
    char cid[4]; uint32_t csz;
    uint16_t fmt=0, ch=0, bps=16; uint32_t sr=44100;
    int found_fmt=0, found_data=0;
    while (!feof(f)) {
        if (fread(cid,1,4,f)!=4) break;
        if (fread(&csz,4,1,f)!=1) break;
        if (memcmp(cid,"fmt ",4)==0) {
            fread(&fmt,2,1,f); fread(&ch,2,1,f); fread(&sr,4,1,f);
            fseek(f,6,SEEK_CUR); fread(&bps,2,1,f);
            if (csz>16) fseek(f,csz-16,SEEK_CUR);
            found_fmt=1;
        } else if (memcmp(cid,"data",4)==0) {
            if (!found_fmt) break;
            int n = csz / (bps/8);
            if (n > MAX_SAMPLE) n = MAX_SAMPLE;
            if (bps==16) {
                int16_t *buf = malloc(csz);
                fread(buf,1,csz,f);
                for (int i=0;i<n;i++) sample_audio[i] = buf[i]/32768.0f;
                free(buf);
            }
            sample_frames = n; found_data=1; break;
        } else fseek(f,csz,SEEK_CUR);
    }
    fclose(f);
    return found_data ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <sample.wav> <events.csv> <output.wav> [--transpose N]\n", argv[0]);
        return 1;
    }
    const char *sample_path = argv[1], *events_path = argv[2], *output_path = argv[3];
    int transpose = 0;
    for (int i=4;i<argc;i++) if (strcmp(argv[i],"--transpose")==0 && i+1<argc) transpose=atoi(argv[++i]);

    if (load_wav(sample_path) != 0) { fprintf(stderr,"Cannot load %s\n", sample_path); return 1; }
    printf("Loaded: %d frames\n", sample_frames);

    /* Load events from CSV */
    FILE *f = fopen(events_path, "r");
    if (!f) { fprintf(stderr,"Cannot open %s\n", events_path); return 1; }
    event_t events[MAX_EVENTS];
    int n_events = 0;
    char line[256];
    while (fgets(line,sizeof(line),f) && n_events<MAX_EVENTS) {
        if (line[0]=='#'||line[0]=='\n') continue;
        int note; float start,dur=0.15f,vel=1.0f;
        if (sscanf(line,"%d , %f , %f , %f",&note,&start,&dur,&vel)>=2) {
            if (dur<=0) dur=0.15f;
            if (vel<=0) vel=1.0f;
            events[n_events].note=note; events[n_events].start=start;
            events[n_events].dur=dur; events[n_events].vel=vel;
            n_events++;
        }
    }
    fclose(f);
    printf("Loaded %d events\n", n_events);

    /* Calculate range and duration */
    int min_n=127, max_n=0; float total_dur=0;
    for (int i=0;i<n_events;i++) {
        int n=events[i].note+transpose;
        if (n<min_n) min_n=n; if (n>max_n) max_n=n;
        float end=events[i].start+events[i].dur;
        if (end>total_dur) total_dur=end;
    }
    total_dur += 0.5f;
    printf("Range: %d-%d, Duration: %.1fs\n", min_n, max_n, total_dur);

    /* Create sampler with regions every 12 semitones */
    wb_sampler_inst sampler;
    wb_sampler_init(&sampler);
    sampler.loop = 1;
    for (int root=0; root<128; root+=12) {
        char name[32]; snprintf(name,32,"r%d",root);
        wb_sampler_add_region(&sampler, sample_audio, sample_frames, 1, root, name);
    }
    printf("Sampler: %d regions\n", sampler.n_regions);

    /* Render */
    float sr = 44100.0f;
    int total_frames = (int)(total_dur * sr);
    float *master = calloc(total_frames, sizeof(float));
    if (!master) { fprintf(stderr,"OOM\n"); return 1; }

    int rendered = 0;
    for (int i=0; i<n_events; i++) {
        int note = events[i].note + transpose;
        if (note < 0) note = 0; if (note > 127) note = 127;
        int offset = (int)(events[i].start * sr);
        int len = (int)(events[i].dur * sr);
        if (offset+len > total_frames) len = total_frames - offset;
        if (offset < 0 || len <= 0) continue;
        wb_sampler_play_note(&sampler, note, events[i].vel, master+offset, len, 1, sr);
        rendered++;
    }
    printf("Rendered %d notes\n", rendered);

    /* Normalize */
    float maxv = 0.0001f;
    for (int i=0;i<total_frames;i++) { float a=master[i]; if(a<0)a=-a; if(a>maxv)maxv=a; }
    float scale = 0.8f / maxv;
    for (int i=0;i<total_frames;i++) master[i] *= scale;

    /* Write WAV */
    FILE *out = fopen(output_path, "wb");
    if (!out) { fprintf(stderr,"Cannot write %s\n", output_path); return 1; }
    uint32_t dsz = total_frames*2, fsz = 36+dsz;
    fwrite("RIFF",1,4,out); fwrite(&fsz,4,1,out); fwrite("WAVE",1,4,out);
    fwrite("fmt ",1,4,out); uint32_t fl=16; fwrite(&fl,4,1,out);
    uint16_t fp[4]; fp[0]=1; fp[1]=1; fp[2]=(uint16_t)sr; fp[3]=(uint16_t)(sr*2);
    fwrite(&fp[0],2,1,out); fwrite(&fp[1],2,1,out); fwrite(&fp[2],4,1,out); fwrite(&fp[3],4,1,out);
    uint16_t ba=2,bps16=16; fwrite(&ba,2,1,out); fwrite(&bps16,2,1,out);
    fwrite("data",1,4,out); fwrite(&dsz,4,1,out);
    for (int i=0;i<total_frames;i++) { int16_t s=(int16_t)(master[i]*32767.0f); fwrite(&s,2,1,out); }
    fclose(out);

    printf("Written: %s (%.1fs)\n", output_path, total_dur);
    wb_sampler_free(&sampler); free(master);
    return 0;
}
