/* mk_ytpmv_bigmac.c — YTPMV Producer using Big Mac's sampler engine.
 *
 * Usage: mk_ytpmv_bigmac <events.txt> <output.wav> [--root N] [--rate N]
 *
 * events.txt format: one event per line:
 *   NOTE START_SEC DUR_SEC VELOCITY
 *   60 0.0 0.2 1.0
 *   62 0.25 0.15 0.8
 *   ...
 *
 * Loads a WAV sample, creates multiple sampler regions across the full
 * MIDI range, then plays each event through the sampler engine.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "wbus/wbus_compositor.h"

#define MAX_EVENTS 5000
#define MAX_SAMPLE (1024*1024)

typedef struct {
    int note;
    float start;
    float dur;
    float vel;
} event_t;

static float sample_audio[MAX_SAMPLE];
static int sample_frames = 0;
static float sample_rate = 44100;

int load_wav(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }
    
    char riff[4]; uint32_t size; char wave[4];
    fread(riff, 1, 4, f); fread(&size, 4, 1, f); fread(wave, 1, 4, f);
    
    char chunk_id[4]; uint32_t chunk_size;
    int fmt_found = 0, data_found = 0;
    uint16_t format, channels; uint32_t sr;
    uint16_t bits_per_sample;
    
    while (!feof(f)) {
        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(&chunk_size, 4, 1, f) != 1) break;
        
        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            fread(&format, 2, 1, f); fread(&channels, 2, 1, f);
            fread(&sr, 4, 1, f); fseek(f, 6, SEEK_CUR);
            fread(&bits_per_sample, 2, 1, f);
            if (chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
            fmt_found = 1; sample_rate = sr;
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            if (!fmt_found) break;
            int n = chunk_size / 2; /* 16-bit */
            if (n > MAX_SAMPLE) n = MAX_SAMPLE;
            int16_t *buf = (int16_t *)malloc(chunk_size);
            size_t nread = fread(buf, 1, chunk_size, f);
            (void)nread;
            for (int i = 0; i < n; i++)
                sample_audio[i] = buf[i] / 32768.0f;
            sample_frames = n;
            data_found = 1;
            free(buf);
            break;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }
    fclose(f);
    return data_found ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <events.txt> <output.wav> <sample.wav> [--root N]\n", argv[0]);
        return 1;
    }
    
    const char *events_path = argv[1];
    const char *output_path = argv[2];
    const char *sample_path = argv[3];
    int base_root = 72;
    
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0 && i+1 < argc) base_root = atoi(argv[++i]);
    }
    
    /* Load sample */
    if (load_wav(sample_path) != 0) return 1;
    printf("Loaded %d frames at %.0f Hz\n", sample_frames, sample_rate);
    
    /* Load events */
    FILE *f = fopen(events_path, "r");
    if (!f) { fprintf(stderr, "Cannot open %s\n", events_path); return 1; }
    
    event_t events[MAX_EVENTS];
    int n_events = 0;
    char line[256];
    
    while (fgets(line, sizeof(line), f) && n_events < MAX_EVENTS) {
        if (line[0] == '#' || line[0] == '\n') continue;
        event_t *e = &events[n_events];
        if (sscanf(line, "%d %f %f %f", &e->note, &e->start, &e->dur, &e->vel) >= 3) {
            if (e->vel <= 0) e->vel = 1.0f;
            n_events++;
        }
    }
    fclose(f);
    printf("Loaded %d events\n", n_events);
    
    /* Calculate total duration */
    float total_dur = 0;
    for (int i = 0; i < n_events; i++) {
        float end = events[i].start + events[i].dur;
        if (end > total_dur) total_dur = end;
    }
    total_dur += 0.5f;
    
    /* Create sampler */
    wb_sampler_inst sampler;
    wb_sampler_init(&sampler);
    sampler.loop = 1; /* Loop samples to fill note duration */
    
    /* Add regions every 12 semitones covering full range */
    for (int root = 12; root < 128; root += 12) {
        char name[32];
        snprintf(name, sizeof(name), "region_%d", root);
        wb_sampler_add_region(&sampler, sample_audio, sample_frames, 1, root, name);
    }
    printf("Added %d sampler regions\n", sampler.n_regions);
    
    /* Calculate transpose to center melody in sampler range */
    int min_note = 127, max_note = 0;
    for (int i = 0; i < n_events; i++) {
        if (events[i].note < min_note) min_note = events[i].note;
        if (events[i].note > max_note) max_note = events[i].note;
    }
    int melody_center = (min_note + max_note) / 2;
    int transpose = base_root - melody_center;
    /* Limit to ±6 semitones */
    if (transpose > 6) transpose = 6;
    if (transpose < -6) transpose = -6;
    printf("Transpose: %d semitones (melody center %d, base %d)\n", transpose, melody_center, base_root);
    
    /* Render */
    int total_frames = (int)(total_dur * sample_rate);
    float *output = (float *)calloc(total_frames, sizeof(float));
    if (!output) { fprintf(stderr, "Out of memory\n"); return 1; }
    
    printf("Rendering %d frames (%.1fs)...\n", total_frames, total_dur);
    
    for (int i = 0; i < n_events; i++) {
        event_t *e = &events[i];
        int note = e->note + transpose;
        if (note < 0) note = 0;
        if (note > 127) note = 127;
        
        int offset = (int)(e->start * sample_rate);
        int len = (int)(e->dur * sample_rate);
        if (offset + len > total_frames) len = total_frames - offset;
        if (offset < 0 || len <= 0) continue;
        
        wb_sampler_play_note(&sampler, note, e->vel, output + offset, len, 1, sample_rate);
    }
    
    /* Normalize */
    float max_val = 0.0001f;
    for (int i = 0; i < total_frames; i++) {
        float a = output[i];
        if (a < 0) a = -a;
        if (a > max_val) max_val = a;
    }
    float scale = 0.8f / max_val;
    for (int i = 0; i < total_frames; i++)
        output[i] *= scale;
    
    /* Write WAV */
    FILE *out = fopen(output_path, "wb");
    if (!out) { fprintf(stderr, "Cannot write %s\n", output_path); return 1; }
    
    uint32_t data_size = total_frames * 2;
    uint32_t file_size = 36 + data_size;
    uint16_t fmt_params[4] = {1, 1, (uint16_t)sample_rate, (uint16_t)(sample_rate * 2)};
    
    fwrite("RIFF", 1, 4, out); fwrite(&file_size, 4, 1, out); fwrite("WAVE", 1, 4, out);
    fwrite("fmt ", 1, 4, out); uint32_t fmt_size = 16; fwrite(&fmt_size, 4, 1, out);
    fwrite(&fmt_params[0], 2, 1, out); fwrite(&fmt_params[1], 2, 1, out);
    fwrite(&fmt_params[2], 4, 1, out); fwrite(&fmt_params[3], 4, 1, out);
    uint16_t block_align = 2, bps = 16;
    fwrite(&block_align, 2, 1, out); fwrite(&bps, 2, 1, out);
    fwrite("data", 1, 4, out); fwrite(&data_size, 4, 1, out);
    
    for (int i = 0; i < total_frames; i++) {
        int16_t s = (int16_t)(output[i] * 32767.0f);
        fwrite(&s, 2, 1, out);
    }
    fclose(out);
    
    printf("Written: %s (%.1fs, %d Hz)\n", output_path, (float)total_frames / sample_rate, (int)sample_rate);
    
    wb_sampler_free(&sampler);
    free(output);
    return 0;
}
