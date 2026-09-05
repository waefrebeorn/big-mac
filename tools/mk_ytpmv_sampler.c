/* mk_ytpmv_sampler.c — YTPMV Producer using Big Mac's sampler engine.
 *
 * Usage: mk_ytpmv_sampler <sample.wav> <midi.mid> <output.wav> [--root-note N] [--transpose N]
 *
 * Loads a character voice sample into Big Mac's sampler, plays a MIDI file
 * through it, and renders the result to WAV. Then use ffmpeg to merge with video.
 *
 * This is the CORRECT way to make YTPMV audio:
 * - The sampler handles pitch-shifting with proper interpolation
 * - Each MIDI note triggers the sample at the correct pitch
 * - ADSR envelope prevents clicks
 * - Formant preservation keeps the character's voice recognizable
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_compositor.h"
#include "wbus/wbus.h"

/* WAV reading (simple PCM 16-bit) */
static int read_wav(const char *path, float **out_audio, int *out_frames, int *out_channels, float *out_sr) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }
    
    char riff[4]; uint32_t size; char wave[4];
    fread(riff, 1, 4, f); fread(&size, 4, 1, f); fread(wave, 1, 4, f);
    
    /* Find fmt chunk */
    char chunk_id[4]; uint32_t chunk_size;
    int fmt_found = 0, data_found = 0;
    uint16_t format, channels; uint32_t sample_rate, byte_rate;
    uint16_t block_align, bits_per_sample;
    float *audio = NULL;
    int n_frames = 0, n_channels = 0;
    
    while (!feof(f)) {
        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(&chunk_size, 4, 1, f) != 1) break;
        
        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            fread(&format, 2, 1, f);
            fread(&channels, 2, 1, f);
            fread(&sample_rate, 4, 1, f);
            fread(&byte_rate, 4, 1, f);
            fread(&block_align, 2, 1, f);
            fread(&bits_per_sample, 2, 1, f);
            if (chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
            fmt_found = 1;
            n_channels = channels;
            *out_sr = (float)sample_rate;
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            if (!fmt_found) { fprintf(stderr, "data before fmt\n"); fclose(f); return -1; }
            n_frames = chunk_size / (n_channels * 2); /* 16-bit */
            audio = (float *)calloc(n_frames * n_channels, sizeof(float));
            int16_t *buf = (int16_t *)malloc(chunk_size);
            size_t nread = fread(buf, 1, chunk_size, f);
            (void)nread;
            for (int i = 0; i < n_frames * n_channels; i++) {
                audio[i] = buf[i] / 32768.0f;
            }
            free(buf);
            data_found = 1;
            break;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }
    fclose(f);
    
    if (!data_found) { fprintf(stderr, "No data chunk in %s\n", path); return -1; }
    
    *out_audio = audio;
    *out_frames = n_frames;
    *out_channels = n_channels;
    return 0;
}

/* Simple MIDI parser — extracts note events with timing */
#define MAX_MIDI_EVENTS 4096

typedef struct {
    float start_time;   /* seconds */
    float duration;     /* seconds */
    int note;           /* MIDI note number */
    int velocity;       /* 0-127 */
} midi_event_t;

typedef struct {
    midi_event_t events[MAX_MIDI_EVENTS];
    int n_events;
    float total_duration;
} midi_sequence_t;

static int parse_midi(const char *path, midi_sequence_t *seq, int max_notes) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return -1; }
    
    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc(fsize);
    size_t nread = fread(data, 1, fsize, f);
    (void)nread;
    fclose(f);
    
    /* Parse header */
    if (memcmp(data, "MThd", 4) != 0) { fprintf(stderr, "Not a MIDI file\n"); free(data); return -1; }
    
    uint32_t header_len = (data[4]<<24)|(data[5]<<16)|(data[6]<<8)|data[7];
    uint16_t format = (data[8]<<8)|data[9];
    uint16_t n_tracks = (data[10]<<8)|data[11];
    uint16_t ticks_per_beat = (data[12]<<8)|data[13];
    
    if (ticks_per_beat == 0) ticks_per_beat = 96;
    
    seq->n_events = 0;
    seq->total_duration = 0;
    
    uint8_t *ptr = data + 8 + header_len;
    uint8_t *end = data + fsize;
    
    float tempo = 500000.0f; /* default 120 BPM */
    
    /* Find the track with the most notes */
    int best_track = -1; int best_count = 0;
    
    for (int t = 0; t < n_tracks && ptr < end; t++) {
        if (memcmp(ptr, "MTrk", 4) != 0) break;
        uint32_t track_len = (ptr[4]<<24)|(ptr[5]<<16)|(ptr[6]<<8)|ptr[7];
        uint8_t *tend = ptr + 8 + track_len;
        
        int count = 0;
        uint8_t *p = ptr + 8;
        while (p < tend) {
            /* Read variable-length delta */
            uint32_t delta = 0;
            uint8_t b;
            do { b = *p++; delta = (delta << 7) | (b & 0x7F); } while (b & 0x80);
            
            uint8_t status = *p;
            if (status == 0xFF) {
                uint8_t type = p[1];
                uint32_t len = 0;
                uint8_t *lp = p + 2;
                do { b = *lp++; len = (len << 7) | (b & 0x7F); } while (b & 0x80);
                if (type == 0x51 && len >= 3) {
                    tempo = (lp[0]<<16)|(lp[1]<<8)|lp[2];
                }
                p = lp + len;
            } else if ((status & 0x80) == 0) {
                /* Running status */
                p++;
            } else {
                uint8_t type = status >> 4;
                uint8_t chan = status & 0x0F;
                (void)chan;
                if (type == 0x8 || type == 0x9) {
                    count++;
                    p += 2;
                } else if (type == 0xA || type == 0xB || type == 0xE) {
                    p += 2;
                } else if (type == 0xC || type == 0xD) {
                    p += 1;
                } else if (status == 0xF0 || status == 0xF7) {
                    /* SysEx */
                    while (*p != 0xF7) p++;
                    p++;
                } else {
                    p++;
                }
            }
        }
        
        if (count > best_count) {
            best_count = count;
            best_track = t;
        }
        
        ptr = tend;
    }
    
    if (best_track < 0) { fprintf(stderr, "No notes found\n"); free(data); return -1; }
    
    /* Re-parse the best track to extract events */
    ptr = data + 8 + header_len;
    for (int t = 0; t <= best_track && ptr < end; t++) {
        uint32_t tlen2 = (ptr[4]<<24)|(ptr[5]<<16)|(ptr[6]<<8)|ptr[7];
        if (t == best_track) {
            uint8_t *tend = ptr + 8 + tlen2;
            uint8_t *p = ptr + 8;
            float time = 0;
            tempo = 500000.0f;
            
            /* Track active notes for duration calculation */
            struct { float start; int active; } note_state[128];
            memset(note_state, 0, sizeof(note_state));
            
            while (p < tend && seq->n_events < MAX_MIDI_EVENTS) {
                uint32_t delta = 0;
                uint8_t b;
                do { b = *p++; delta = (delta << 7) | (b & 0x7F); } while (b & 0x80);
                
                time += (float)delta / ticks_per_beat * (tempo / 1000000.0f);
                
                uint8_t status = *p;
                if (status == 0xFF) {
                    uint8_t type = p[1];
                    uint32_t len = 0;
                    uint8_t *lp = p + 2;
                    do { b = *lp++; len = (len << 7) | (b & 0x7F); } while (b & 0x80);
                    if (type == 0x51 && len >= 3) {
                        tempo = (lp[0]<<16)|(lp[1]<<8)|lp[2];
                    }
                    p = lp + len;
                } else if ((status & 0x80) == 0) {
                    /* Running status — note_on with note=status, vel=p[0] */
                    uint8_t note = status;
                    uint8_t vel = p[0];
                    p++;
                    if (vel > 0) {
                        note_state[note].start = time;
                        note_state[note].active = 1;
                    } else if (note_state[note].active) {
                        if (seq->n_events < max_notes) {
                            midi_event_t *e = &seq->events[seq->n_events++];
                            e->start_time = note_state[note].start;
                            e->duration = time - note_state[note].start;
                            e->note = note;
                            e->velocity = 100;
                            if (e->duration > seq->total_duration) seq->total_duration = e->duration + note_state[note].start;
                        }
                        note_state[note].active = 0;
                    }
                } else {
                    uint8_t type = status >> 4;
                    if (type == 0x9) { /* Note on */
                        uint8_t note = p[1];
                        uint8_t vel = p[2];
                        p += 3;
                        if (vel > 0) {
                            note_state[note].start = time;
                            note_state[note].active = 1;
                        }
                    } else if (type == 0x8) { /* Note off */
                        uint8_t note = p[1];
                        p += 2;
                        if (note_state[note].active) {
                            if (seq->n_events < max_notes) {
                                midi_event_t *e = &seq->events[seq->n_events++];
                                e->start_time = note_state[note].start;
                                e->duration = time - note_state[note].start;
                                e->note = note;
                                e->velocity = 100;
                                if (time > seq->total_duration) seq->total_duration = time;
                            }
                            note_state[note].active = 0;
                        }
                    } else if (type == 0xC || type == 0xD) {
                        p += 2;
                    } else {
                        p += 3;
                    }
                }
            }
        }
        ptr += 8 + tlen2;
    }
    
    free(data);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <sample.wav> <midi.mid> <output.wav> [--root-note N] [--transpose N]\n", argv[0]);
        return 1;
    }
    
    const char *sample_path = argv[1];
    const char *midi_path = argv[2];
    const char *output_path = argv[3];
    int root_note = 89;  /* Default: Kirby MIDI 89 */
    int transpose = 0;
    
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--transpose") == 0 && i+1 < argc) transpose = atoi(argv[++i]);
    }
    
    /* Load sample */
    float *audio = NULL; int n_frames = 0, n_channels = 0; float sample_rate = 44100;
    if (read_wav(sample_path, &audio, &n_frames, &n_channels, &sample_rate) != 0) return 1;
    
    printf("Loaded sample: %d frames, %d ch, %.0f Hz\n", n_frames, n_channels, sample_rate);
    
    /* Parse MIDI */
    midi_sequence_t seq;
    if (parse_midi(midi_path, &seq, 2000) != 0) { free(audio); return 1; }
    
    printf("Parsed MIDI: %d events, %.1fs total\n", seq.n_events, seq.total_duration);
    
    /* Calculate transpose to fit sample range */
    int min_note = 127, max_note = 0;
    for (int i = 0; i < seq.n_events; i++) {
        if (seq.events[i].note < min_note) min_note = seq.events[i].note;
        if (seq.events[i].note > max_note) max_note = seq.events[i].note;
    }
    printf("MIDI range: %d-%d\n", min_note, max_note);
    
    /* Create sampler instrument */
    wb_sampler_inst sampler;
    wb_sampler_init(&sampler);
    
    /* Add multiple regions covering the full MIDI range.
     * Each region covers ±12 semitones from its root note.
     * We create regions every 24 semitones to cover 0-127. */
    for (int r = 0; r < 128; r += 24) {
        char name[32];
        snprintf(name, sizeof(name), "voice_%d", r);
        int region = wb_sampler_add_region(&sampler, audio, n_frames, n_channels, r + 12, name);
        if (region < 0) break;
    }
    
    if (sampler.n_regions == 0) { fprintf(stderr, "Failed to add any regions\n"); free(audio); return 1; }
    printf("Added %d sampler regions covering MIDI %d-%d\n", sampler.n_regions, 
           sampler.regions[0].low_note, sampler.regions[sampler.n_regions-1].high_note);
    
    /* Transpose melody to center of instrument range */
    int center = (min_note + max_note) / 2;
    int target_center = (sampler.regions[0].low_note + sampler.regions[sampler.n_regions-1].high_note) / 2;
    transpose = target_center - center;
    printf("Transpose: %d semitones\n", transpose);
    
    /* Render audio */
    float render_sr = 44100.0f;
    int render_frames = (int)((seq.total_duration + 1.0f) * render_sr);
    float *output = (float *)calloc(render_frames * 2, sizeof(float)); /* stereo */
    
    printf("Rendering %d frames...\n", render_frames);
    
    for (int i = 0; i < seq.n_events; i++) {
        midi_event_t *e = &seq.events[i];
        int note = e->note + transpose;
        
        /* Clamp to valid MIDI range */
        if (note < 0) note = 0;
        if (note > 127) note = 127;
        
        /* Calculate output position */
        int out_offset = (int)(e->start_time * render_sr);
        int out_len = (int)((e->duration + 0.1f) * render_sr);
        if (out_len < 100) out_len = 100; /* Minimum 100ms */
        if (out_offset + out_len > render_frames) out_len = render_frames - out_offset;
        if (out_offset < 0 || out_len <= 0) continue;
        
        /* Render this note */
        float note_buf[8192 * 2]; /* Max 8192 frames per note */
        int to_render = out_len;
        if (to_render > 8192) to_render = 8192;
        
        wb_sampler_play_note(&sampler, note, e->velocity / 127.0f, note_buf, to_render, 2, render_sr);
        
        /* Mix into output */
        for (int j = 0; j < to_render; j++) {
            int idx = (out_offset + j) * 2;
            if (idx + 1 < render_frames * 2) {
                output[idx] += note_buf[j * 2];
                output[idx + 1] += note_buf[j * 2 + 1];
            }
        }
    }
    
    /* Normalize */
    float max_val = 0.0001f;
    for (int i = 0; i < render_frames * 2; i++) {
        if (fabsf(output[i]) > max_val) max_val = fabsf(output[i]);
    }
    float scale = 0.8f / max_val;
    for (int i = 0; i < render_frames * 2; i++) {
        output[i] *= scale;
    }
    
    /* Write WAV */
    FILE *f = fopen(output_path, "wb");
    if (!f) { fprintf(stderr, "Cannot write %s\n", output_path); free(audio); free(output); return 1; }
    
    /* WAV header */
    uint32_t data_size = render_frames * 2 * 2; /* 16-bit stereo */
    uint32_t file_size = 36 + data_size;
    uint16_t fmt_params[4] = {1, 2, (uint16_t)render_sr, (uint16_t)(render_sr * 2 * 2)};
    uint16_t bps = 16;
    
    fwrite("RIFF", 1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&fmt_params[0], 2, 1, f); /* PCM */
    fwrite(&fmt_params[1], 2, 1, f); /* stereo */
    fwrite(&fmt_params[2], 4, 1, f); /* sample rate */
    fwrite(&fmt_params[3], 4, 1, f); /* byte rate */
    uint16_t block_align = 4;
    fwrite(&block_align, 2, 1, f);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
    
    /* Convert float to 16-bit */
    for (int i = 0; i < render_frames * 2; i++) {
        int16_t s = (int16_t)(output[i] * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    
    printf("Written: %s (%.1fs, %d Hz)\n", output_path, (float)render_frames / render_sr, (int)render_sr);
    
    wb_sampler_free(&sampler);
    free(audio);
    free(output);
    return 0;
}
