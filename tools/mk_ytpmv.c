/* mk_ytpmv.c — YTPMV Production CLI (R106).
 *
 * Usage: mk_ytpmv <source_audio.wav> <source_video.mp4> <output.mp4> [--bpm 120] [--scale major|minor|chromatic]
 *
 * Pipeline:
 * 1. Load source audio (WAV)
 * 2. Detect phonemes (energy-based segmentation)
 * 3. Detect pitch per phoneme (autocorrelation)
 * 4. Correct pitch to musical scale
 * 5. Render pitch-corrected audio (resampled phonemes at correct pitch)
 * 6. Extract video frames for each phoneme
 * 7. Composite video synced to audio timeline
 * 8. Output final MP4
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "wbus/wbus_compositor.h"

/* WAV reading helper */
static float* read_wav(const char *path, int *out_frames, int *out_channels, int *out_rate) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[mk_ytpmv] cannot open %s\n", path); return NULL; }
    
    /* Simple WAV header parse (16-bit PCM, mono/stereo) */
    char riff[4];
    fread(riff, 1, 4, f);
    if (memcmp(riff, "RIFF", 4) != 0) { fprintf(stderr, "[mk_ytpmv] not a RIFF file\n"); fclose(f); return NULL; }
    
    uint32_t size;
    fread(&size, 4, 1, f);
    char wave[4];
    fread(wave, 1, 4, f);
    
    /* Find fmt chunk */
    int channels = 1, sample_rate = 44100, bits = 16;
    int data_size = 0;
    
    while (!feof(f)) {
        char chunk_id[4];
        uint32_t chunk_size;
        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(&chunk_size, 4, 1, f) != 1) break;
        
        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            uint16_t format, ch;
            uint32_t rate, bits_per_sample;
            fread(&format, 2, 1, f);
            fread(&ch, 2, 1, f);
            channels = ch;
            fread(&rate, 4, 1, f);
            sample_rate = rate;
            fseek(f, 6, SEEK_CUR); /* skip byte rate + block align */
            fread(&bits_per_sample, 2, 1, f);
            bits = bits_per_sample;
            if (chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            break;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }
    
    if (data_size == 0) { fprintf(stderr, "[mk_ytpmv] no data chunk found\n"); fclose(f); return NULL; }
    
    int n_samples = data_size / (bits / 8) / channels;
    float *audio = (float *)malloc(n_samples * sizeof(float));
    if (!audio) { fclose(f); return NULL; }
    
    /* Read and convert to float mono */
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
    } else {
        fseek(f, data_size, SEEK_CUR);
    }
    
    fclose(f);
    *out_frames = n_samples;
    *out_channels = channels;
    *out_rate = sample_rate;
    return audio;
}

/* WAV writing helper */
static int write_wav(const char *path, const float *audio, int n_frames, int sample_rate) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    
    int data_size = n_frames * 2; /* 16-bit mono */
    int file_size = 36 + data_size;
    
    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    uint32_t v = file_size;
    fwrite(&v, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    
    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    v = 16; fwrite(&v, 4, 1, f);
    uint16_t fmt = 1; fwrite(&fmt, 2, 1, f); /* PCM */
    fmt = 1; fwrite(&fmt, 2, 1, f); /* mono */
    v = sample_rate; fwrite(&v, 4, 1, f);
    v = sample_rate * 2; fwrite(&v, 4, 1, f); /* byte rate */
    uint16_t block = 2; fwrite(&block, 2, 1, f);
    uint16_t bits = 16; fwrite(&bits, 2, 1, f);
    
    /* data chunk */
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
        fprintf(stderr, "Usage: %s <source_audio.wav> <source_video.mp4> <output.mp4> [--bpm N] [--scale major|minor|chromatic]\n", argv[0]);
        fprintf(stderr, "\nProduces a YTPMV from source audio+video.\n");
        fprintf(stderr, "  --bpm N      Target BPM (default 120)\n");
        fprintf(stderr, "  --scale S    Musical scale: major, minor, chromatic (default chromatic)\n");
        return 1;
    }
    
    const char *audio_path = argv[1];
    const char *video_path = argv[2];
    const char *output_path = argv[3];
    
    float bpm = 120.0f;
    int scale_type = 2; /* chromatic */
    
    /* Parse args */
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--bpm") == 0 && i + 1 < argc) {
            bpm = atof(argv[++i]);
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "major") == 0) scale_type = 0;
            else if (strcmp(argv[i], "minor") == 0) scale_type = 1;
            else scale_type = 2;
        }
    }
    
    fprintf(stderr, "[mk_ytpmv] Loading source audio: %s\n", audio_path);
    
    /* Load source audio */
    int n_frames, channels, sample_rate;
    float *audio = read_wav(audio_path, &n_frames, &channels, &sample_rate);
    if (!audio) {
        fprintf(stderr, "[mk_ytpmv] Failed to load audio\n");
        return 1;
    }
    
    fprintf(stderr, "[mk_ytpmv] Audio: %d frames, %d channels, %d Hz\n", n_frames, channels, sample_rate);
    
    /* Analyze: detect phonemes + pitches */
    ytpmv_producer prod;
    ytpmv_prod_init(&prod, (float)sample_rate);
    prod.bpm = bpm;
    prod.scale_type = scale_type;
    
    int n_ph = ytpmv_prod_analyze(&prod, audio, n_frames, channels);
    fprintf(stderr, "[mk_ytpmv] Detected %d phonemes\n", n_ph);
    
    if (n_ph == 0) {
        fprintf(stderr, "[mk_ytpmv] No phonemes detected!\n");
        free(audio);
        return 1;
    }
    
    /* Print phoneme info */
    for (int i = 0; i < n_ph && i < 20; i++) {
        fprintf(stderr, "  [%d] t=%.3fs dur=%.3fs pitch=%.1fHz -> MIDI %d (%.0f cents)\n",
                i, prod.start_times[i], prod.durations[i],
                prod.pitches[i], prod.midi_notes[i], prod.corrections[i].cents_off);
    }
    if (n_ph > 20) fprintf(stderr, "  ... (%d more)\n", n_ph - 20);
    
    /* Render pitch-corrected audio */
    int out_frames = n_frames;
    float *output_audio = (float *)calloc(out_frames, sizeof(float));
    if (!output_audio) {
        fprintf(stderr, "[mk_ytpmv] Failed to allocate output buffer\n");
        free(audio);
        return 1;
    }
    
    int rendered = ytpmv_prod_render(&prod, output_audio, out_frames, 1);
    fprintf(stderr, "[mk_ytpmv] Rendered %d frames of pitch-corrected audio\n", rendered);
    
    /* Write intermediate audio */
    char audio_out[512];
    snprintf(audio_out, sizeof(audio_out), "/tmp/wb_ytpmv_audio.wav");
    write_wav(audio_out, output_audio, rendered, sample_rate);
    fprintf(stderr, "[mk_ytpmv] Wrote intermediate audio: %s\n", audio_out);
    
    /* Build video using ffmpeg */
    /* For each phoneme, extract the corresponding video segment and concat them */
    fprintf(stderr, "[mk_ytpmv] Building video from source: %s\n", video_path);
    
    /* Create a concat file for ffmpeg */
    char concat_path[512];
    snprintf(concat_path, sizeof(concat_path), "/tmp/wb_ytpmv_concat.txt");
    FILE *cf = fopen(concat_path, "w");
    if (!cf) {
        fprintf(stderr, "[mk_ytpmv] Failed to create concat file\n");
        free(audio);
        free(output_audio);
        return 1;
    }
    
    /* Extract video segments for each phoneme using ffmpeg */
    char cmd[2048];
    for (int i = 0; i < n_ph; i++) {
        float start = prod.start_times[i];
        float dur = prod.durations[i];
        char seg_path[512];
        snprintf(seg_path, sizeof(seg_path), "/tmp/wb_ytpmv_seg_%04d.mp4", i);
        
        /* Extract segment from source video, speed-adjusted for pitch correction */
        float speed = prod.corrections[i].ratio;
        snprintf(cmd, sizeof(cmd),
            "ffmpeg -y -v error -ss %.4f -t %.4f -i \"%s\" "
            "-vf \"setpts=%.4f*PTS\" -af \"atempo=%.4f\" "
            "-c:v libx264 -preset fast -crf 23 -c:a aac \"%s\"",
            start, dur, video_path, 1.0f/speed, speed, seg_path);
        
        fprintf(stderr, "  Extracting segment %d: %.3fs-%.3fs (speed %.3f)\n", i, start, start+dur, speed);
        int rc = system(cmd);
        if (rc != 0) {
            fprintf(stderr, "    WARNING: ffmpeg failed for segment %d\n", i);
            continue;
        }
        
        fprintf(cf, "file '%s'\n", seg_path);
    }
    fclose(cf);
    
    /* Concatenate all segments */
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -v error -f concat -safe 0 -i \"%s\" -c copy /tmp/wb_ytpmv_video.mp4",
        concat_path);
    fprintf(stderr, "[mk_ytpmv] Concatenating video segments...\n");
    system(cmd);
    
    /* Merge audio + video */
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -v error -i /tmp/wb_ytpmv_video.mp4 -i \"%s\" "
        "-c:v copy -c:a aac -shortest \"%s\"",
        audio_out, output_path);
    fprintf(stderr, "[mk_ytpmv] Merging audio + video -> %s\n", output_path);
    int final_rc = system(cmd);
    
    if (final_rc == 0) {
        fprintf(stderr, "[mk_ytpmv] SUCCESS: Output written to %s\n", output_path);
    } else {
        fprintf(stderr, "[mk_ytpmv] FAILED: ffmpeg merge returned %d\n", final_rc);
    }
    
    /* Cleanup temp files */
    for (int i = 0; i < n_ph; i++) {
        char seg_path[512];
        snprintf(seg_path, sizeof(seg_path), "/tmp/wb_ytpmv_seg_%04d.mp4", i);
        unlink(seg_path);
    }
    unlink(concat_path);
    unlink(audio_out);
    unlink("/tmp/wb_ytpmv_video.mp4");
    
    free(audio);
    free(output_audio);
    
    return final_rc == 0 ? 0 : 1;
}
