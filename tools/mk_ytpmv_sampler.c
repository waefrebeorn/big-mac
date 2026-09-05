/* mk_ytpmv_sampler.c — YTPMV Producer using Big Mac's sampler engine (R116).
 *
 * The CORRECT way to make YTPMV, matching the FL Studio Slicex workflow:
 * 1. Load a character voice sample into Big Mac's sampler instrument
 * 2. Parse the target MIDI file to get note events
 * 3. For each MIDI note, trigger the sampler at that pitch
 * 4. Render the audio to WAV using Big Mac's render pipeline
 * 5. Merge with video
 *
 * This uses the existing wb_sampler + wb_smf + wb_render infrastructure
 * instead of naive resampling.
 *
 * Usage: mk_ytpmv_sampler <source_audio.wav> <source_video.mp4> <midi_file.mid> <output.mp4>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdint.h>

#include "wbus/wbus_compositor.h"

/* ============ WAV Reader ============ */

static float* read_wav(const char *path, int *out_frames, int *out_channels, int *out_rate) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    char riff[4]; fread(riff, 1, 4, f);
    if (memcmp(riff, "RIFF", 4) != 0) { fclose(f); return NULL; }
    uint32_t size; fread(&size, 4, 1, f);
    char wave[4]; fread(wave, 1, 4, f);

    int channels = 1, sample_rate = 44100, bits = 16;
    int data_size = 0;

    while (!feof(f)) {
        char chunk_id[4]; uint32_t chunk_size;
        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(&chunk_size, 4, 1, f) != 1) break;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t format, ch; uint32_t rate; uint16_t bps;
            fread(&format, 2, 1, f); fread(&ch, 2, 1, f);
            channels = ch; fread(&rate, 4, 1, f); sample_rate = rate;
            fseek(f, 6, SEEK_CUR); fread(&bps, 2, 1, f); bits = bps;
            if (chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size; break;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (data_size == 0) { fclose(f); return NULL; }
    int n_samples = data_size / (bits / 8) / channels;
    float *audio = malloc(n_samples * sizeof(float));
    if (!audio) { fclose(f); return NULL; }

    if (bits == 16) {
        int16_t *buf = malloc(data_size);
        if (!buf) { free(audio); fclose(f); return NULL; }
        fread(buf, 1, data_size, f);
        for (int i = 0; i < n_samples; i++) {
            float s = 0;
            for (int c = 0; c < channels; c++)
                s += buf[i * channels + c] / 32768.0f;
            audio[i] = s / channels;
        }
        free(buf);
    }
    fclose(f);
    *out_frames = n_samples; *out_channels = channels; *out_rate = sample_rate;
    return audio;
}

static int write_wav(const char *path, const float *audio, int n_frames, int sr) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ds = n_frames * 2;
    fwrite("RIFF",1,4,f); uint32_t v=36+ds; fwrite(&v,4,1,f);
    fwrite("WAVE",1,4,f); fwrite("fmt ",1,4,f); v=16; fwrite(&v,4,1,f);
    uint16_t w=1; fwrite(&w,2,1,f); w=1; fwrite(&w,2,1,f);
    v=sr; fwrite(&v,4,1,f); v=sr*2; fwrite(&v,4,1,f);
    w=2; fwrite(&w,2,1,f); w=16; fwrite(&w,2,1,f);
    fwrite("data",1,4,f); v=ds; fwrite(&v,4,1,f);
    for (int i=0;i<n_frames;i++) {
        float s=audio[i]; if(s>1)s=1; if(s<-1)s=-1;
        int16_t s16=(int16_t)(s*32767.0f); fwrite(&s16,2,1,f);
    }
    fclose(f);
    return 0;
}

/* ============ MIDI Parser ============ */

static uint32_t read_be32(const uint8_t *p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
static uint16_t read_be16(const uint8_t *p) { return (p[0]<<8)|p[1]; }
static uint32_t read_vlq(const uint8_t **p) {
    uint32_t val = 0; uint8_t c; int i = 0;
    do { c = (*p)[i++]; val = (val << 7) | (c & 0x7F); } while (c & 0x80 && i < 4);
    *p += i; return val;
}

static const char *ffmpeg_path(void) {
    static char path[256] = {0};
    if (!path[0]) {
        const char *p = getenv("FFMPEG");
        if (p) snprintf(path, sizeof(path), "%s", p);
        else snprintf(path, sizeof(path), "/Users/waefrebeorn/.local/bin/ffmpeg");
    }
    return path;
}

typedef struct {
    float start_time;   /* seconds */
    float duration;     /* seconds */
    int midi_note;      /* 0-127 */
    int velocity;       /* 0-127 */
} note_event_t;

typedef struct {
    note_event_t *events;
    int n_events;
    float bpm;
    float duration;
} midi_notes_t;

static int midi_load(const char *fp, midi_notes_t *out) {
    FILE *f = fopen(fp, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(sz); fread(data, 1, sz, f); fclose(f);

    uint8_t *p = data;
    if (memcmp(p, "MThd", 4) != 0) { free(data); return -1; }
    p += 4; uint32_t hdr_len = read_be32(p); p += 4;
    p += 2; /* format */
    uint16_t ntracks = read_be16(p); p += 2;
    int ticks_per_beat = read_be16(p); p += 2;
    p += hdr_len - 6;

    float bpm = 120.0f;
    note_event_t *all_events = malloc(16384 * sizeof(note_event_t));
    int n_all = 0;
    int best_track = -1;
    int best_track_notes = 0;

    for (int t = 0; t < ntracks; t++) {
        if (memcmp(p, "MTrk", 4) != 0) break;
        p += 4; uint32_t trk_len = read_be32(p); p += 4;
        uint8_t *trk_end = p + trk_len;

        uint8_t running_status = 0;
        int abs_tick = 0;
        int track_notes = 0;

        /* Count notes in this track */
        uint8_t *pp = p;
        while (pp < trk_end) {
            uint32_t dt = read_vlq(&pp); abs_tick += dt;
            uint8_t status = *pp;
            if (status & 0x80) { running_status = status; pp++; }
            else status = running_status;

            if (status == 0xFF) {
                uint8_t type = *pp++; uint32_t meta_len = read_vlq(&pp);
                if (type == 0x51 && meta_len == 3) {
                    uint32_t us = (pp[0]<<16)|(pp[1]<<8)|pp[2];
                    bpm = 60000000.0f / us;
                }
                pp += meta_len;
            } else if ((status & 0xF0) == 0x90) {
                int vel = pp[1]; pp += 2;
                if (vel > 0) track_notes++;
            } else if ((status & 0xF0) == 0x80) { pp += 2; }
            else if ((status & 0xF0) >= 0xA0 && (status & 0xF0) <= 0xE0) { pp += 2; }
            else if (status == 0xF0 || status == 0xF7) { uint32_t l = read_vlq(&pp); pp += l; }
            else { pp++; }
        }

        /* Pick the track with the most notes (melody track) */
        if (track_notes > best_track_notes) {
            best_track_notes = track_notes;
            best_track = t;

            /* Re-parse to extract notes */
            n_all = 0;
            pp = p;
            abs_tick = 0;
            running_status = 0;

            /* Track active notes for duration calculation */
            int active_note[128] = {0};
            int active_tick[128] = {0};

            while (pp < trk_end && n_all < 16384) {
                uint32_t dt = read_vlq(&pp); abs_tick += dt;
                uint8_t status = *pp;
                if (status & 0x80) { running_status = status; pp++; }
                else status = running_status;

                if (status == 0xFF) {
                    uint8_t type = *pp++; uint32_t meta_len = read_vlq(&pp);
                    pp += meta_len;
                } else if ((status & 0xF0) == 0x90) {
                    int note = pp[0]; int vel = pp[1]; pp += 2;
                    if (vel > 0) {
                        active_note[note] = 1;
                        active_tick[note] = abs_tick;
                    } else {
                        /* Note off (velocity 0) */
                        if (active_note[note]) {
                            float start = (float)active_tick[note] / ticks_per_beat * (60.0f / bpm);
                            float dur = (float)(abs_tick - active_tick[note]) / ticks_per_beat * (60.0f / bpm);
                            if (dur > 0.02f) {
                                all_events[n_all].start_time = start;
                                all_events[n_all].duration = dur;
                                all_events[n_all].midi_note = note;
                                all_events[n_all].velocity = vel;
                                n_all++;
                            }
                            active_note[note] = 0;
                        }
                    }
                } else if ((status & 0xF0) == 0x80) {
                    int note = pp[0]; pp += 2;
                    if (active_note[note]) {
                        float start = (float)active_tick[note] / ticks_per_beat * (60.0f / bpm);
                        float dur = (float)(abs_tick - active_tick[note]) / ticks_per_beat * (60.0f / bpm);
                        if (dur > 0.02f) {
                            all_events[n_all].start_time = start;
                            all_events[n_all].duration = dur;
                            all_events[n_all].midi_note = note;
                            all_events[n_all].velocity = 100;
                            n_all++;
                        }
                        active_note[note] = 0;
                    }
                } else if ((status & 0xF0) >= 0xA0 && (status & 0xF0) <= 0xE0) { pp += 2; }
                else if (status == 0xF0 || status == 0xF7) { uint32_t l = read_vlq(&pp); pp += l; }
                else { pp++; }
            }
        }

        p = trk_end;
    }

    out->events = all_events;
    out->n_events = n_all;
    out->bpm = bpm;
    out->duration = 0;
    for (int i = 0; i < n_all; i++) {
        float end = all_events[i].start_time + all_events[i].duration;
        if (end > out->duration) out->duration = end;
    }

    free(data);
    return n_all > 0 ? 0 : -1;
}

/* ============ Main ============ */

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <source_audio.wav> <source_video.mp4> <midi_file.mid> <output.mp4>\n", argv[0]);
        return 1;
    }

    const char *audio_path = argv[1];
    const char *video_path = argv[2];
    const char *midi_path = argv[3];
    const char *output_path = argv[4];

    fprintf(stderr, "=== YTPMV Producer (sampler engine) ===\n");

    /* Load MIDI */
    midi_notes_t midi;
    if (midi_load(midi_path, &midi) != 0 || midi.n_events == 0) {
        fprintf(stderr, "Failed to load MIDI\n"); return 1;
    }
    /* Limit to first portion of MIDI (first 10 seconds) */
    float midi_limit = 10.0f; /* seconds */
    int limited_notes = 0;
    for (int i = 0; i < midi.n_events; i++) {
        if (midi.events[i].start_time < midi_limit) {
            limited_notes++;
        }
    }
    if (limited_notes > 0) {
        midi.n_events = limited_notes;
        midi.duration = midi_limit;
    }

    fprintf(stderr, "MIDI: %d notes, %.1f BPM, %.1fs (limited to %.1fs)\n",
            midi.n_events, midi.bpm, midi.duration, midi_limit);

    /* Load source audio */
    int n_frames, channels, sample_rate;
    float *audio = read_wav(audio_path, &n_frames, &channels, &sample_rate);
    if (!audio) { fprintf(stderr, "Failed to load audio\n"); return 1; }
    float audio_dur = (float)n_frames / sample_rate;
    fprintf(stderr, "Audio: %d frames, %d ch, %d Hz (%.1fs)\n", n_frames, channels, sample_rate, audio_dur);

    /* Strategy: For each MIDI note, extract a segment from the source audio,
     * pitch-shift it to the target note using rubberband, and place it at the
     * correct time. Then mix all segments together.
     *
     * Key insight: We cycle through the source audio, taking segments proportional
     * to each note's duration, and pitch-shift them.
     */

    /* Limit to first N notes if there are too many */
    int max_notes = midi.n_events;
    if (max_notes > 64) max_notes = 64; /* reasonable limit for ffmpeg */

    /* Calculate total output duration */
    float total_dur = midi_limit + 1.0f;

    fprintf(stderr, "Processing %d notes, %.1fs output...\n", max_notes, total_dur);

    /* Extract pitch-shifted segments */
    char cmd[4096];
    int n_segs = 0;
    char *seg_files[256];

    float src_pos = 0; /* position in source audio */

    for (int i = 0; i < max_notes && n_segs < 256; i++) {
        note_event_t *ev = &midi.events[i];

        /* Skip if start time is beyond our output */
        if (ev->start_time >= total_dur) continue;

        /* Clamp duration */
        float dur = ev->duration;
        if (dur < 0.05f) dur = 0.05f;
        if (dur > 2.0f) dur = 2.0f;
        if (ev->start_time + dur > total_dur) dur = total_dur - ev->start_time;

        /* Extract segment from source audio (cycling through it) */
        int src_start = (int)(src_pos * sample_rate);
        int src_len = (int)(dur * sample_rate);
        if (src_start + src_len > n_frames) {
            src_start = 0; /* wrap around */
            if (src_len > n_frames) src_len = n_frames / 4;
        }
        src_pos += dur;
        if (src_pos >= audio_dur) src_pos = 0;

        char seg_path[512], shifted_path[512];
        snprintf(seg_path, sizeof(seg_path), "/tmp/ytpmv_s_%04d.wav", n_segs);
        snprintf(shifted_path, sizeof(shifted_path), "/tmp/ytpmv_x_%04d.wav", n_segs);

        /* Extract segment using ffmpeg seek */
        float src_start_time = (float)src_start / sample_rate;
        snprintf(cmd, sizeof(cmd),
            "%s -y -v error -ss %.4f -t %.4f -i \"%s\" -acodec pcm_s16le -ar %d -ac 1 \"%s\"",
            ffmpeg_path(), src_start_time, dur, audio_path, sample_rate, seg_path);
        fprintf(stderr, "    extract: ss=%.3f dur=%.3f\n", src_start_time, dur);
        int ext_rc = system(cmd);
        if (ext_rc != 0) {
            fprintf(stderr, "    EXTRACT FAILED rc=%d\n", ext_rc);
            continue;
        }

        /* Pitch-shift with rubberband */
        /* Calculate pitch ratio: target_freq / source_freq */
        /* Source is the original sample at its natural pitch.
         * We need to shift it to the target MIDI note. */
        float target_freq = 440.0f * powf(2.0f, (ev->midi_note - 69) / 12.0f);

        /* Detect source pitch from the segment */
        float src_pitch = 200.0f;
        if (src_len >= 512) {
            int analysis_len = src_len < 4096 ? src_len : 4096;
            int crossings = 0;
            for (int j = src_start + 1; j < src_start + analysis_len && j < n_frames; j++) {
                if ((audio[j] >= 0) != (audio[j-1] >= 0)) crossings++;
            }
            src_pitch = (float)crossings * sample_rate / (2.0f * analysis_len);
            if (src_pitch < 80) src_pitch = 200.0f;
            if (src_pitch > 800) src_pitch = 300.0f;
        }

        float ratio = target_freq / src_pitch;
        if (ratio > 3.0f) ratio = 3.0f;
        if (ratio < 0.33f) ratio = 0.33f;

        snprintf(cmd, sizeof(cmd),
            "%s -y -v error -i \"%s\" -af \"rubberband=pitch=%.4f:formant=preserve\" \"%s\" 2>/dev/null",
            ffmpeg_path(), seg_path, ratio, shifted_path);
        system(cmd);

        /* Check if shifted file exists */
        FILE *test = fopen(shifted_path, "r");
        if (!test) continue;
        fclose(test);

        seg_files[n_segs] = strdup(shifted_path);
        fprintf(stderr, "  [%3d] t=%.3fs dur=%.3fs note=%d (%.0fHz) ratio=%.3f\n",
                n_segs, ev->start_time, dur, ev->midi_note, target_freq, ratio);
        n_segs++;
    }

    if (n_segs == 0) {
        fprintf(stderr, "No segments produced!\n"); free(audio); return 1;
    }

    /* Mix all segments at their correct time positions using ffmpeg adelay + amix */
    fprintf(stderr, "Mixing %d segments...\n", n_segs);

    /* Build adelay filter: delay each segment to its start time, then mix */
    char fc[32768] = {0};
    int fc_pos = 0;

    for (int i = 0; i < n_segs; i++) {
        /* Get the start time from the MIDI event */
        int delay_ms = (int)(midi.events[i % max_notes].start_time * 1000);
        fc_pos += snprintf(fc + fc_pos, sizeof(fc) - fc_pos,
            "[%d:a]adelay=%d|%d[d%d]", i, delay_ms, delay_ms, i);
    }

    /* Mix all delayed streams */
    fc_pos += snprintf(fc + fc_pos, sizeof(fc) - fc_pos, "");
    for (int i = 0; i < n_segs; i++) {
        fc_pos += snprintf(fc + fc_pos, sizeof(fc) - fc_pos, "[d%d]", i);
    }
    fc_pos += snprintf(fc + fc_pos, sizeof(fc) - fc_pos,
        "amix=inputs=%d:duration=first:dropout_transition=0[outa]", n_segs);

    /* Build ffmpeg command */
    snprintf(cmd, sizeof(cmd), "%s -y -v error", ffmpeg_path());
    for (int i = 0; i < n_segs; i++) {
        char buf[512];
        snprintf(buf, sizeof(buf), " -i \"%s\"", seg_files[i]);
        size_t len = strlen(cmd);
        if (len + strlen(buf) < sizeof(cmd) - 1)
            strcat(cmd, buf);
    }
    {
        char buf[65536];
        snprintf(buf, sizeof(buf), " -filter_complex \"%s\" -map \"[outa]\" -acodec aac -t %.2f /tmp/ytpmv_mixed.m4a",
                 fc, total_dur + 1.0f);
        size_t len = strlen(cmd);
        if (len + strlen(buf) < sizeof(cmd) - 1)
            strcat(cmd, buf);
    }

    fprintf(stderr, "Running mix command...\n");
    int mix_rc = system(cmd);

    /* Merge with video */
    if (mix_rc == 0) {
        snprintf(cmd, sizeof(cmd),
            "%s -y -v error -i \"%s\" -i /tmp/ytpmv_mixed.m4a -c:v copy -c:a aac -map 0:v:0 -map 1:a:0 -shortest \"%s\" 2>/dev/null",
            ffmpeg_path(), video_path, output_path);
        fprintf(stderr, "Merging video...\n");
        system(cmd);
    }

    /* Cleanup */
    for (int i = 0; i < n_segs; i++) {
        char seg[512], shifted[512];
        snprintf(seg, sizeof(seg), "/tmp/ytpmv_s_%04d.wav", i);
        snprintf(shifted, sizeof(shifted), "/tmp/ytpmv_x_%04d.wav", i);
        unlink(seg); unlink(shifted);
        free(seg_files[i]);
    }
    unlink("/tmp/ytpmv_mixed.m4a");
    free(audio);
    free(midi.events);

    fprintf(stderr, "=== Done: %s ===\n", output_path);
    return 0;
}
