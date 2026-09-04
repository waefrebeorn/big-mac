/* mk_smb_ytpmv.c — Super Mario Bros 1-1 YTPMV Producer (R113).
 *
 * Uses the actual SMB1OW.mid MIDI file for the target melody.
 * Uses real YTP character voice audio.
 *
 * Usage: mk_smb_ytpmv <source_audio.wav> <source_video.mp4> <midi_file.mid> <output.mp4>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdint.h>

#include "wbus/wbus_compositor.h"

/* ============ MIDI Parser ============ */

static uint32_t read_be32(const uint8_t *p) {
    return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];
}
static uint16_t read_be16(const uint8_t *p) {
    return (p[0]<<8)|p[1];
}
static uint32_t read_vlq(const uint8_t **p) {
    uint32_t val = 0;
    uint8_t c;
    int i = 0;
    do {
        c = (*p)[i++];
        val = (val << 7) | (c & 0x7F);
    } while (c & 0x80 && i < 4);
    *p += i;
    return val;
}

typedef struct {
    int tick;
    int note;
    int vel;
} midi_note_t;

typedef struct {
    midi_note_t *notes;
    int n_notes;
    int capacity;
    float bpm;
    int ticks_per_beat;
    float duration_sec;
} midi_melody_t;

static int midi_load(const char *path, midi_melody_t *mel) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(sz);
    fread(data, 1, sz, f);
    fclose(f);

    uint8_t *p = data;
    if (memcmp(p, "MThd", 4) != 0) { free(data); return -1; }
    p += 4;
    uint32_t hdr_len = read_be32(p); p += 4;
    uint16_t format = read_be16(p); p += 2;
    uint16_t ntracks = read_be16(p); p += 2;
    mel->ticks_per_beat = read_be16(p); p += 2;
    p += hdr_len - 6;

    mel->bpm = 120.0f; /* default */
    mel->notes = NULL;
    mel->n_notes = 0;
    mel->capacity = 0;

    for (int t = 0; t < ntracks; t++) {
        if (memcmp(p, "MTrk", 4) != 0) break;
        p += 4;
        uint32_t trk_len = read_be32(p); p += 4;
        uint8_t *trk_end = p + trk_len;

        uint8_t running_status = 0;
        int abs_tick = 0;
        int note_count = 0;

        /* First pass: count notes */
        uint8_t *pp = p;
        while (pp < trk_end) {
            uint32_t dt = read_vlq(&pp);
            abs_tick += dt;
            uint8_t status = *pp;
            if (status & 0x80) { running_status = status; pp++; }
            else status = running_status;

            if (status == 0xFF) {
                uint8_t type = *pp++;
                uint32_t meta_len = read_vlq(&pp);
                if (type == 0x51 && meta_len == 3) {
                    uint32_t us = (pp[0]<<16)|(pp[1]<<8)|pp[2];
                    mel->bpm = 60000000.0f / us;
                }
                pp += meta_len;
            } else if ((status & 0xF0) == 0x90) {
                int vel = pp[1];
                pp += 2;
                if (vel > 0) note_count++;
            } else if ((status & 0xF0) == 0x80) {
                pp += 2;
            } else if ((status & 0xF0) == 0xA0 || (status & 0xF0) == 0xB0 || (status & 0xF0) == 0xE0) {
                pp += 2;
            } else if (status == 0xF0 || status == 0xF7) {
                uint32_t syx_len = read_vlq(&pp);
                pp += syx_len;
            } else {
                pp++;
            }
        }

        /* Allocate if this track has the most notes */
        if (note_count > mel->capacity) {
            free(mel->notes);
            mel->capacity = note_count + 100;
            mel->notes = malloc(mel->capacity * sizeof(midi_note_t));
            mel->n_notes = 0;
        }

        /* Second pass: store notes from the track with most notes */
        if (note_count > mel->n_notes / 2) {
            /* Reset and store this track's notes */
            int stored = 0;
            pp = p;
            abs_tick = 0;
            running_status = 0;
            mel->n_notes = 0;

            while (pp < trk_end && mel->n_notes < mel->capacity) {
                uint32_t dt = read_vlq(&pp);
                abs_tick += dt;
                uint8_t status = *pp;
                if (status & 0x80) { running_status = status; pp++; }
                else status = running_status;

                if (status == 0xFF) {
                    uint8_t type = *pp++;
                    uint32_t meta_len = read_vlq(&pp);
                    pp += meta_len;
                } else if ((status & 0xF0) == 0x90) {
                    int note = pp[0];
                    int vel = pp[1];
                    pp += 2;
                    if (vel > 0) {
                        mel->notes[mel->n_notes].tick = abs_tick;
                        mel->notes[mel->n_notes].note = note;
                        mel->notes[mel->n_notes].vel = vel;
                        mel->n_notes++;
                    }
                } else if ((status & 0xF0) == 0x80) {
                    pp += 2;
                } else if ((status & 0xF0) == 0xA0 || (status & 0xF0) == 0xB0 || (status & 0xF0) == 0xE0) {
                    pp += 2;
                } else if (status == 0xF0 || status == 0xF7) {
                    uint32_t syx_len = read_vlq(&pp);
                    pp += syx_len;
                } else {
                    pp++;
                }
            }
        }

        p = trk_end;
    }

    /* Compute duration */
    if (mel->n_notes > 0) {
        int last_tick = mel->notes[mel->n_notes-1].tick;
        float beats = (float)last_tick / mel->ticks_per_beat;
        mel->duration_sec = beats * (60.0f / mel->bpm);
    }

    free(data);
    return 0;
}

/* ============ WAV Helpers ============ */

static float* read_wav(const char *path, int *out_frames, int *out_channels, int *out_rate) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    char riff[4];
    fread(riff, 1, 4, f);
    if (memcmp(riff, "RIFF", 4) != 0) { fclose(f); return NULL; }

    uint32_t size;
    fread(&size, 4, 1, f);
    char wave[4];
    fread(wave, 1, 4, f);

    int channels = 1, sample_rate = 44100, bits = 16;
    int data_size = 0;

    while (!feof(f)) {
        char chunk_id[4];
        uint32_t chunk_size;
        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(&chunk_size, 4, 1, f) != 1) break;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t format, ch;
            uint32_t rate;
            uint16_t bps;
            fread(&format, 2, 1, f);
            fread(&ch, 2, 1, f);
            channels = ch;
            fread(&rate, 4, 1, f);
            sample_rate = rate;
            fseek(f, 6, SEEK_CUR);
            fread(&bps, 2, 1, f);
            bits = bps;
            if (chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            break;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (data_size == 0) { fclose(f); return NULL; }

    int n_samples = data_size / (bits / 8) / channels;
    float *audio = malloc(n_samples * sizeof(float));

    if (bits == 16) {
        int16_t *buf = malloc(data_size);
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
    *out_frames = n_samples;
    *out_channels = channels;
    *out_rate = sample_rate;
    return audio;
}

static int write_wav(const char *path, const float *audio, int n_frames, int sample_rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    int data_size = n_frames * 2;
    fwrite("RIFF", 1, 4, f);
    uint32_t v = 36 + data_size; fwrite(&v, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); v = 16; fwrite(&v, 4, 1, f);
    uint16_t w = 1; fwrite(&w, 2, 1, f);
    w = 1; fwrite(&w, 2, 1, f);
    v = sample_rate; fwrite(&v, 4, 1, f);
    v = sample_rate * 2; fwrite(&v, 4, 1, f);
    w = 2; fwrite(&w, 2, 1, f);
    w = 16; fwrite(&w, 2, 1, f);
    fwrite("data", 1, 4, f); v = data_size; fwrite(&v, 4, 1, f);

    for (int i = 0; i < n_frames; i++) {
        float s = audio[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        int16_t s16 = (int16_t)(s * 32767.0f);
        fwrite(&s16, 2, 1, f);
    }
    fclose(f);
    return 0;
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

    fprintf(stderr, "=== SMB 1-1 YTPMV Producer ===\n");

    /* Load MIDI */
    midi_melody_t mel;
    if (midi_load(midi_path, &mel) != 0 || mel.n_notes == 0) {
        fprintf(stderr, "Failed to load MIDI: %s\n", midi_path);
        return 1;
    }
    fprintf(stderr, "MIDI: %d notes, %.1f BPM, %.1fs duration\n",
            mel.n_notes, mel.bpm, mel.duration_sec);

    /* Load source audio */
    int n_frames, channels, sample_rate;
    float *audio = read_wav(audio_path, &n_frames, &channels, &sample_rate);
    if (!audio) {
        fprintf(stderr, "Failed to load audio: %s\n", audio_path);
        return 1;
    }
    fprintf(stderr, "Audio: %d frames, %d ch, %d Hz (%.1fs)\n",
            n_frames, channels, sample_rate, (float)n_frames / sample_rate);

    /* Build target melody from MIDI — limit to audio duration */
    float audio_dur = (float)n_frames / sample_rate;
    wb_melody target;
    wb_melody_init(&target, mel.bpm, audio_dur);

    float beat_dur = 60.0f / mel.bpm;
    for (int i = 0; i < mel.n_notes; i++) {
        float start = (float)mel.notes[i].tick / mel.ticks_per_beat * beat_dur;
        if (start >= audio_dur) break; /* Don't add notes past audio end */

        float gap;
        if (i + 1 < mel.n_notes) {
            gap = (float)(mel.notes[i+1].tick - mel.notes[i].tick) / mel.ticks_per_beat * beat_dur;
        } else {
            gap = beat_dur;
        }
        /* Clamp gap so note doesn't extend past audio */
        if (start + gap > audio_dur - 0.05f)
            gap = audio_dur - 0.05f - start;
        if (gap < 0.05f) gap = 0.05f;

        wb_melody_add_note(&target, start, gap, mel.notes[i].note, mel.notes[i].vel / 127.0f);
    }
    target.total_duration = audio_dur;
    fprintf(stderr, "Target melody: %d events, %.1fs\n", target.n_events, target.total_duration);

    /* Detect phonemes using real speech segmentation */
    ytpmv_producer prod;
    ytpmv_prod_init(&prod, (float)sample_rate);
    prod.bpm = mel.bpm;
    prod.scale_type = 2;

    /* Use onset-based segmentation for real speech */
    int segs[4096];
    int n_segs = wb_extract_phonemes_real(audio, n_frames, channels, (float)sample_rate, segs, 4096);
    fprintf(stderr, "Segmentation: %d onsets detected\n", n_segs);

    /* Build phoneme data from onsets */
    prod.n_phonemes = 0;
    int prev = 0;
    for (int i = 0; i <= n_segs && prod.n_phonemes < YTPMV_MAX_PHONEMES; i++) {
        int end = (i < n_segs) ? segs[i] : n_frames;
        if (end <= prev) continue;
        int seg_len = end - prev;
        if (seg_len < 2048) { prev = end; continue; }

        prod.segments[prod.n_phonemes] = prev;
        prod.start_times[prod.n_phonemes] = (float)prev / sample_rate;
        prod.durations[prod.n_phonemes] = (float)seg_len / sample_rate;
        prod.midi_notes[prod.n_phonemes] = 60;
        prod.pitches[prod.n_phonemes] = 200.0f;
        prod.velocities[prod.n_phonemes] = 0.8f;
        prod.n_phonemes++;
        prev = end;
    }
    int n_ph = prod.n_phonemes;
    fprintf(stderr, "Detected %d phonemes\n", n_ph);

    if (n_ph == 0) {
        fprintf(stderr, "No phonemes detected!\n");
        free(audio);
        return 1;
    }

    for (int i = 0; i < n_ph && i < 20; i++) {
        fprintf(stderr, "  [%2d] t=%.3fs dur=%.3fs pitch=%.1fHz MIDI %d\n",
                i, prod.start_times[i], prod.durations[i], prod.pitches[i], prod.midi_notes[i]);
    }
    if (n_ph > 20) fprintf(stderr, "  ... (%d more)\n", n_ph - 20);

    /* Quantize to beat grid */
    float duration_sec = (float)n_frames / sample_rate;
    wb_beat_grid bg;
    wb_beat_grid_init(&bg, mel.bpm, (float)sample_rate, duration_sec);
    wb_beat_grid_quantize_phonemes(&bg, prod.start_times, prod.durations, n_ph);

    /* Map phonemes to MIDI melody */
    wb_melody_mapper mm;
    wb_mapper_init(&mm, (float)sample_rate);
    mm.target = target;
    mm.source_audio = audio;
    mm.source_frames = n_frames;
    mm.source_channels = channels;
    wb_mapper_assign(&mm, prod.start_times, prod.durations, n_ph);

    fprintf(stderr, "Melody assignments:\n");
    for (int i = 0; i < mm.n_phonemes && i < 20; i++) {
        if (mm.phoneme_target_midi[i] > 0) {
            fprintf(stderr, "  [%2d] t=%.3fs -> MIDI %d (%.1f Hz)\n",
                    i, mm.phoneme_starts[i], mm.phoneme_target_midi[i], mm.phoneme_pitch_ratio[i]);
        }
    }

    /* Render */
    int out_frames = n_frames; /* Match audio length */

    float *output_audio = calloc(out_frames, sizeof(float));
    int rendered = wb_mapper_render(&mm, output_audio, out_frames);
    fprintf(stderr, "Rendered %d frames\n", rendered);

    /* Write audio */
    write_wav("/tmp/smb_ytpmv_audio.wav", output_audio, rendered > 0 ? rendered : out_frames, sample_rate);

    /* Build video */
    fprintf(stderr, "Building video...\n");
    FILE *cf = fopen("/tmp/smb_ytpmv_concat.txt", "w");
    if (!cf) { free(audio); free(output_audio); return 1; }

    char cmd[2048];
    for (int i = 0; i < n_ph; i++) {
        float start = prod.start_times[i];
        float dur = prod.durations[i];
        char seg[512];
        snprintf(seg, sizeof(seg), "/tmp/smb_ytpmv_seg_%04d.mp4", i);

        float speed = 1.0f;
        if (i < mm.n_phonemes && mm.phoneme_target_midi[i] > 0 && prod.pitches[i] > 0) {
            speed = mm.phoneme_pitch_ratio[i] / prod.pitches[i];
            /* ffmpeg atempo range is [0.5, 2.0] — clamp to valid range */
            if (speed > 2.0f) speed = 2.0f;
            if (speed < 0.5f) speed = 0.5f;
        }

        snprintf(cmd, sizeof(cmd),
            "ffmpeg -y -v error -ss %.4f -t %.4f -i \"%s\" "
            "-vf \"setpts=%.4f*PTS\" -af \"atempo=%.4f\" "
            "-c:v libx264 -preset fast -crf 23 -c:a aac \"%s\"",
            start, dur, video_path, 1.0f/speed, speed, seg);
        system(cmd);
        fprintf(cf, "file '%s'\n", seg);
    }
    fclose(cf);

    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -v error -f concat -safe 0 -i \"/tmp/smb_ytpmv_concat.txt\" -c copy /tmp/smb_ytpmv_video.mp4");
    system(cmd);

    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -v error -i /tmp/smb_ytpmv_video.mp4 -i /tmp/smb_ytpmv_audio.wav "
        "-c:v copy -c:a aac -shortest \"%s\"", output_path);
    int rc = system(cmd);

    /* Cleanup */
    for (int i = 0; i < n_ph; i++) {
        char seg[512];
        snprintf(seg, sizeof(seg), "/tmp/smb_ytpmv_seg_%04d.mp4", i);
        unlink(seg);
    }
    unlink("/tmp/smb_ytpmv_concat.txt");
    unlink("/tmp/smb_ytpmv_video.mp4");
    unlink("/tmp/smb_ytpmv_audio.wav");
    free(audio);
    free(output_audio);
    free(mel.notes);
    wb_mapper_free(&mm);

    if (rc == 0) {
        fprintf(stderr, "\n=== SUCCESS: %s ===\n", output_path);
    } else {
        fprintf(stderr, "\n=== FAILED ===\n");
    }
    return rc == 0 ? 0 : 1;
}
