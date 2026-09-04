/* mk_smb_ytpmv.c — Super Mario Bros 1-1 YTPMV Producer (R113).
 *
 * Produces a YTPMV using the SMB 1-1 overworld melody as the target.
 * Uses real speech audio as source, pitch-shifts phonemes to follow
 * the iconic Mario melody.
 *
 * Usage: mk_smb_ytpmv <source_audio.wav> <source_video.mp4> <output.mp4>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "wbus/wbus_compositor.h"

/* SMB 1-1 Overworld melody — the iconic first section.
 * Each entry: {midi_note, duration_in_beats}
 * Tempo: ~174 BPM, so 1 beat = 0.345s
 *
 * The melody (simplified):
 * E5 E5 E5 C5 E5 G5  | C5 G4 E4 A4 B4 Bb4 A4 |
 * G4 E5 G5 A5 F5 G5  | E5 C5 D5 B4            |
 * (repeat with variations)
 */

typedef struct {
    int midi_note;     /* MIDI note number, 0 = rest */
    float beats;       /* duration in beats */
} smb_note_t;

/* First phrase of SMB overworld — the iconic "duh duh duh duh duh duh DA-DA" */
static const smb_note_t smb_phrase1[] = {
    /* Bar 1: E E E C E G */
    {76, 0.5f}, {76, 0.5f}, {76, 0.5f}, {72, 0.5f}, {76, 0.5f}, {79, 1.0f},
    /* Bar 2: C G E A B Bb A */
    {72, 1.0f}, {67, 1.0f}, {64, 1.0f}, {69, 1.0f}, {71, 0.5f}, {70, 0.5f}, {69, 1.0f},
    /* Bar 3: G E G A F G */
    {67, 0.5f}, {76, 0.5f}, {79, 0.5f}, {81, 0.5f}, {77, 0.5f}, {79, 0.5f},
    /* Bar 4: E C D B */
    {76, 1.0f}, {72, 1.0f}, {74, 1.0f}, {71, 1.0f},
};

/* Second phrase — variation */
static const smb_note_t smb_phrase2[] = {
    /* Bar 5: E C D B */
    {76, 0.5f}, {72, 0.5f}, {74, 0.5f}, {71, 0.5f},
    /* Bar 6: C5 G4 E4 G4 A4 F4 G4 */
    {72, 0.5f}, {67, 0.5f}, {64, 0.5f}, {67, 0.5f}, {69, 0.5f}, {65, 0.5f}, {67, 0.5f},
    /* Bar 7: E4 C4 D4 B4 C4 */
    {64, 0.5f}, {60, 0.5f}, {62, 0.5f}, {71, 0.5f}, {72, 0.5f},
    /* Bar 8: G4 F#4 F4 D4 */
    {67, 0.5f}, {66, 0.5f}, {65, 0.5f}, {62, 0.5f},
};

/* WAV reading helper */
static float* read_wav(const char *path, int *out_frames, int *out_channels, int *out_rate) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[smb_ytpmv] cannot open %s\n", path); return NULL; }

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
            uint32_t rate, bps;
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
    float *audio = (float *)malloc(n_samples * sizeof(float));
    if (!audio) { fclose(f); return NULL; }

    if (bits == 16) {
        int16_t *buf = (int16_t *)malloc(data_size);
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
    *out_frames = n_samples;
    *out_channels = channels;
    *out_rate = sample_rate;
    return audio;
}

static int write_wav(const char *path, const float *audio, int n_frames, int sample_rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    int data_size = n_frames * 2;
    int file_size = 36 + data_size;

    fwrite("RIFF", 1, 4, f);
    uint32_t v = file_size; fwrite(&v, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    fwrite("fmt ", 1, 4, f);
    v = 16; fwrite(&v, 4, 1, f);
    uint16_t fmt = 1; fwrite(&fmt, 2, 1, f);
    fmt = 1; fwrite(&fmt, 2, 1, f);
    v = sample_rate; fwrite(&v, 4, 1, f);
    v = sample_rate * 2; fwrite(&v, 4, 1, f);
    uint16_t block = 2; fwrite(&block, 2, 1, f);
    uint16_t bits = 16; fwrite(&bits, 2, 1, f);

    fwrite("data", 1, 4, f);
    v = data_size; fwrite(&v, 4, 1, f);

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

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <source_audio.wav> <source_video.mp4> <output.mp4> [--bpm 174]\n", argv[0]);
        return 1;
    }

    const char *audio_path = argv[1];
    const char *video_path = argv[2];
    const char *output_path = argv[3];
    float bpm = 174.0f;

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--bpm") == 0 && i + 1 < argc) {
            bpm = atof(argv[++i]);
        }
    }

    fprintf(stderr, "=== SMB 1-1 YTPMV Producer ===\n");
    fprintf(stderr, "BPM: %.0f\n", bpm);

    /* Load source audio */
    int n_frames, channels, sample_rate;
    float *audio = read_wav(audio_path, &n_frames, &channels, &sample_rate);
    if (!audio) {
        fprintf(stderr, "[smb_ytpmv] Failed to load audio\n");
        return 1;
    }
    fprintf(stderr, "Audio: %d frames, %d ch, %d Hz (%.1fs)\n",
            n_frames, channels, sample_rate, (float)n_frames / sample_rate);

    /* Build SMB melody */
    wb_melody melody;
    wb_melody_init(&melody, bpm, 0);

    float beat_dur = 60.0f / bpm;
    float time = 0;

    /* Add phrase 1 twice */
    for (int rep = 0; rep < 2; rep++) {
        int n1 = sizeof(smb_phrase1) / sizeof(smb_phrase1[0]);
        for (int i = 0; i < n1; i++) {
            float dur = smb_phrase1[i].beats * beat_dur;
            wb_melody_add_note(&melody, time, dur, smb_phrase1[i].midi_note, 0.9f);
            time += dur;
        }
    }
    /* Add phrase 2 */
    int n2 = sizeof(smb_phrase2) / sizeof(smb_phrase2[0]);
    for (int i = 0; i < n2; i++) {
        float dur = smb_phrase2[i].beats * beat_dur;
        wb_melody_add_note(&melody, time, dur, smb_phrase2[i].midi_note, 0.9f);
        time += dur;
    }
    melody.total_duration = time;

    fprintf(stderr, "Melody: %d notes, %.1fs duration\n", melody.n_events, melody.total_duration);

    /* Detect phonemes using existing engine */
    ytpmv_producer prod;
    ytpmv_prod_init(&prod, (float)sample_rate);
    prod.bpm = bpm;
    prod.scale_type = 2; /* chromatic */

    int n_ph = ytpmv_prod_analyze(&prod, audio, n_frames, channels);
    fprintf(stderr, "Detected %d phonemes\n", n_ph);

    if (n_ph == 0) {
        fprintf(stderr, "No phonemes detected!\n");
        free(audio);
        return 1;
    }

    /* Print phoneme info */
    for (int i = 0; i < n_ph && i < 30; i++) {
        fprintf(stderr, "  [%2d] t=%.3fs dur=%.3fs pitch=%.1fHz -> MIDI %d\n",
                i, prod.start_times[i], prod.durations[i],
                prod.pitches[i], prod.midi_notes[i]);
    }
    if (n_ph > 30) fprintf(stderr, "  ... (%d more)\n", n_ph - 30);

    /* Quantize to beat grid */
    float duration_sec = (float)n_frames / sample_rate;
    wb_beat_grid bg;
    wb_beat_grid_init(&bg, bpm, (float)sample_rate, duration_sec);
    wb_beat_grid_quantize_phonemes(&bg, prod.start_times, prod.durations, n_ph);
    fprintf(stderr, "Quantized to %.0f BPM beat grid\n", bpm);

    /* Use melody mapper to follow SMB melody */
    wb_melody_mapper mm;
    wb_mapper_init(&mm, (float)sample_rate);
    mm.target = melody;
    mm.source_audio = audio;
    mm.source_frames = n_frames;
    mm.source_channels = channels;

    wb_mapper_assign(&mm, prod.start_times, prod.durations, n_ph);

    /* Print melody assignments */
    fprintf(stderr, "Melody assignments:\n");
    for (int i = 0; i < mm.n_phonemes && i < 30; i++) {
        if (mm.phoneme_target_midi[i] > 0) {
            fprintf(stderr, "  [%2d] t=%.3fs -> MIDI %d (%.1f Hz)\n",
                    i, mm.phoneme_starts[i], mm.phoneme_target_midi[i],
                    mm.phoneme_pitch_ratio[i]);
        }
    }

    /* Render melody-follow audio */
    int out_frames = n_frames;
    if ((int)(melody.total_duration * sample_rate) > out_frames)
        out_frames = (int)(melody.total_duration * sample_rate);

    float *output_audio = (float *)calloc(out_frames, sizeof(float));
    if (!output_audio) {
        fprintf(stderr, "Failed to allocate output\n");
        free(audio);
        return 1;
    }

    int rendered = wb_mapper_render(&mm, output_audio, out_frames);
    fprintf(stderr, "Rendered %d frames of melody-follow audio\n", rendered);

    /* Write audio */
    char audio_out[512];
    snprintf(audio_out, sizeof(audio_out), "/tmp/smb_ytpmv_audio.wav");
    write_wav(audio_out, output_audio, rendered > 0 ? rendered : out_frames, sample_rate);
    fprintf(stderr, "Wrote: %s\n", audio_out);

    /* Build video — extract segments per phoneme */
    fprintf(stderr, "Building video...\n");

    char concat_path[512];
    snprintf(concat_path, sizeof(concat_path), "/tmp/smb_ytpmv_concat.txt");
    FILE *cf = fopen(concat_path, "w");
    if (!cf) {
        fprintf(stderr, "Failed to create concat file\n");
        free(audio);
        free(output_audio);
        return 1;
    }

    char cmd[2048];
    for (int i = 0; i < n_ph; i++) {
        float start = prod.start_times[i];
        float dur = prod.durations[i];
        char seg_path[512];
        snprintf(seg_path, sizeof(seg_path), "/tmp/smb_ytpmv_seg_%04d.mp4", i);

        /* Speed adjustment for pitch correction */
        float speed = 1.0f;
        if (i < mm.n_phonemes && mm.phoneme_target_midi[i] > 0) {
            float target_freq = mm.phoneme_pitch_ratio[i];
            if (prod.pitches[i] > 0)
                speed = target_freq / prod.pitches[i];
            if (speed > 3.0f) speed = 3.0f;
            if (speed < 0.33f) speed = 0.33f;
        }

        snprintf(cmd, sizeof(cmd),
            "ffmpeg -y -v error -ss %.4f -t %.4f -i \"%s\" "
            "-vf \"setpts=%.4f*PTS\" -af \"atempo=%.4f\" "
            "-c:v libx264 -preset fast -crf 23 -c:a aac \"%s\"",
            start, dur, video_path, 1.0f/speed, speed, seg_path);

        fprintf(stderr, "  Seg %2d: %.3fs-%.3fs speed=%.3f midi=%d\n",
                i, start, start+dur, speed,
                (i < mm.n_phonemes) ? mm.phoneme_target_midi[i] : 0);
        int rc = system(cmd);
        if (rc != 0) {
            fprintf(stderr, "    WARNING: failed\n");
            continue;
        }
        fprintf(cf, "file '%s'\n", seg_path);
    }
    fclose(cf);

    /* Concatenate video */
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -v error -f concat -safe 0 -i \"%s\" -c copy /tmp/smb_ytpmv_video.mp4",
        concat_path);
    fprintf(stderr, "Concatenating video...\n");
    system(cmd);

    /* Merge audio + video */
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -v error -i /tmp/smb_ytpmv_video.mp4 -i \"%s\" "
        "-c:v copy -c:a aac -shortest \"%s\"",
        audio_out, output_path);
    fprintf(stderr, "Merging...\n");
    int final_rc = system(cmd);

    /* Cleanup */
    for (int i = 0; i < n_ph; i++) {
        char seg_path[512];
        snprintf(seg_path, sizeof(seg_path), "/tmp/smb_ytpmv_seg_%04d.mp4", i);
        unlink(seg_path);
    }
    unlink(concat_path);
    unlink("/tmp/smb_ytpmv_video.mp4");
    unlink(audio_out);
    free(audio);
    free(output_audio);
    wb_mapper_free(&mm);

    if (final_rc == 0) {
        fprintf(stderr, "\n=== SUCCESS: %s ===\n", output_path);
    } else {
        fprintf(stderr, "\n=== FAILED (rc=%d) ===\n", final_rc);
    }

    return final_rc == 0 ? 0 : 1;
}
