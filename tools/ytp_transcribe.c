/* ytp_transcribe.c — Batch transcribe videos to word-level JSON (R083).
 *
 * Uses whisper.cpp CLI to get segment-level transcripts, then distributes
 * word timestamps evenly within each segment. Not perfect, but good enough
 * for YTP sentence mixing.
 *
 * Usage: ./build/ytp_transcribe <video1.mp4> [video2.mp4 ...]
 * Output: <video>.transcript.json (word-level timestamps)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_CMD 4096
#define MAX_LINE 4096
#define MAX_WORDS 8192

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

/* Extract audio from video to WAV */
int extract_audio(const char *video, const char *wav) {
    char cmd[MAX_CMD];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -i \"%s\" -vn -acodec pcm_s16le -ar 16000 -ac 1 "
        "\"%s\" 2>/dev/null", video, wav);
    return system(cmd);
}

/* Run whisper.cpp and get segment-level JSON */
int transcribe(const char *wav, const char *json_out) {
    char cmd[MAX_CMD];
    snprintf(cmd, sizeof(cmd),
        "/Users/waefrebeorn/whisper.cpp/build/bin/whisper-cli "
        "--model /Users/waefrebeorn/whisper.cpp/models/ggml-tiny.en-q5_1.bin "
        "--output-json -oj --language en "
        "-f \"%s\" -of \"%s\" 2>/dev/null",
        wav, json_out);
    return system(cmd);
}

/* Parse segment JSON and output word-level JSON */
int segments_to_words(const char *seg_json, const char *word_json) {
    FILE *f = fopen(seg_json, "r");
    if (!f) { fprintf(stderr, "Cannot open %s\n", seg_json); return -1; }
    
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(sz + 1);
    fread(data, 1, sz, f);
    data[sz] = '\0';
    fclose(f);
    
    /* Open output */
    FILE *out = fopen(word_json, "w");
    if (!out) { free(data); return -1; }
    
    fprintf(out, "{\n  \"words\": [\n");
    
    /* Parse segments array from whisper.cpp JSON format:
     * [{"timestamps":{"from":"00:00:00,000","to":"00:00:01,400"},
     *   "offsets":{"from":0,"to":1400},"text":" (whoosh)"}, ...]
     */
    
    char *p = data;
    int first_word = 1;
    
    while (*p) {
        /* Find "offsets" */
        char *offsets = strstr(p, "\"offsets\"");
        if (!offsets) break;
        
        /* Parse from/to in ms */
        char *fromp = strstr(offsets, "\"from\":");
        char *top = strstr(offsets, "\"to\":");
        if (!fromp || !top) break;
        
        int from_ms = atoi(fromp + 7);
        int to_ms = atoi(top + 5);
        
        /* Find "text" */
        char *textp = strstr(top, "\"text\"");
        if (!textp) break;
        char *tstart = strchr(textp + 6, '"');
        if (!tstart) break;
        tstart++;
        char *tend = strchr(tstart, '"');
        if (!tend) break;
        
        /* Extract text */
        int tlen = tend - tstart;
        char text[1024];
        if (tlen >= sizeof(text)) tlen = sizeof(text) - 1;
        strncpy(text, tstart, tlen);
        text[tlen] = '\0';
        
        /* Split text into words and distribute timestamps */
        char *wp = text;
        char *words[MAX_WORDS];
        int word_count = 0;
        
        while (*wp && word_count < MAX_WORDS) {
            while (*wp == ' ' || *wp == '\t') wp++;
            if (!*wp) break;
            char *we = wp;
            while (*we && *we != ' ' && *we != '\t') we++;
            char saved = *we;
            *we = '\0';
            
            /* Skip very short non-word tokens */
            if (strlen(wp) > 0) {
                words[word_count++] = strdup(wp);
            }
            
            *we = saved;
            wp = we;
        }
        
        /* Distribute timestamps */
        if (word_count > 0) {
            double seg_dur = to_ms - from_ms;
            double word_dur = seg_dur / word_count;
            
            for (int i = 0; i < word_count; i++) {
                int w_start = from_ms + (int)(i * word_dur);
                int w_end = from_ms + (int)((i + 1) * word_dur);
                
                if (!first_word) fprintf(out, ",\n");
                fprintf(out, "    {\"word\":\"%s\",\"start\":%d,\"end\":%d}",
                        words[i], w_start, w_end);
                first_word = 0;
                
                free(words[i]);
            }
        }
        
        p = tend + 1;
    }
    
    fprintf(out, "\n  ]\n}\n");
    fclose(out);
    free(data);
    
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <video1.mp4> [video2.mp4 ...]\n", argv[0]);
        printf("Output: <video>.transcript.json (word-level timestamps)\n");
        return 1;
    }
    
    for (int i = 1; i < argc; i++) {
        const char *video = argv[i];
        printf("Processing: %s\n", video);
        
        if (!file_exists(video)) {
            printf("  ERROR: file not found\n");
            continue;
        }
        
        /* Extract audio */
        char wav[512];
        snprintf(wav, sizeof(wav), "/tmp/ytp_audio_%d.wav", i);
        printf("  Extracting audio...\n");
        if (extract_audio(video, wav) != 0) {
            printf("  ERROR: audio extraction failed\n");
            continue;
        }
        
        /* Transcribe */
        char seg_json[512];
        snprintf(seg_json, sizeof(seg_json), "/tmp/ytp_seg_%d", i);
        printf("  Transcribing (whisper.cpp)...\n");
        if (transcribe(wav, seg_json) != 0) {
            printf("  ERROR: transcription failed\n");
            continue;
        }
        
        /* The actual JSON file is seg_json + ".json" */
        char actual_json[512];
        snprintf(actual_json, sizeof(actual_json), "%s.json", seg_json);
        
        /* Convert to word-level */
        char word_json[512];
        snprintf(word_json, sizeof(word_json), "%s.transcript.json", video);
        printf("  Converting to word-level...\n");
        segments_to_words(actual_json, word_json);
        
        printf("  ✓ %s\n", word_json);
        
        /* Cleanup temp files */
        unlink(wav);
        unlink(actual_json);
    }
    
    printf("\nDone.\n");
    return 0;
}
