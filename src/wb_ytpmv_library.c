/* wb_ytpmv_library.c — YTPMV Source Library Manager (R135).
 *
 * Manages a library of source clips organized by:
 * - Character name
 * - Pitch range (MIDI note range)
 * - Vowel class (A/E/I/O/U)
 * - Quality score
 * - Tags (custom labels)
 *
 * Supports:
 * - Add/remove sources
 * - Query by pitch, vowel, character, tags
 * - Auto-classify vowel from audio
 * - Export/import library to JSON-like format
 * - Source recommendation (best match for a target note)
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

#define LIB_MAX_SOURCES 256
#define LIB_MAX_CHARS 32
#define LIB_MAX_TAGS 8
#define LIB_MAX_TAG_LEN 32
#define LIB_MAX_PATH 256
#define LIB_MAX_NAME 64

/* ================================================================
 * VOWEL CLASSIFICATION
 * ================================================================ */

typedef enum {
    LIB_VOWEL_UNKNOWN = -1,
    LIB_VOWEL_A = 0,  /* /æ/, /ɑ/ — wide open */
    LIB_VOWEL_E,      /* /i/, /ɪ/ — smile */
    LIB_VOWEL_I,      /* /ɪ/ — relaxed */
    LIB_VOWEL_O,      /* /o/, /ɔ/ — rounded open */
    LIB_VOWEL_U,      /* /u/, /ʊ/ — tightly rounded */
    LIB_VOWEL_COUNT
} lib_vowel_class;

const char *lib_vowel_name(lib_vowel_class v) {
    switch (v) {
        case LIB_VOWEL_A: return "A";
        case LIB_VOWEL_E: return "E";
        case LIB_VOWEL_I: return "I";
        case LIB_VOWEL_O: return "O";
        case LIB_VOWEL_U: return "U";
        default: return "?";
    }
}

/* Classify vowel from formant frequencies (F1, F2) */
/* Based on Peterson-Barney formant data */
lib_vowel_class lib_classify_vowel(float f1, float f2) {
    if (f1 < 0 || f2 < 0) return LIB_VOWEL_UNKNOWN;
    
    /* /i/ (EE): low F1 (~300Hz), high F2 (~2300Hz) */
    if (f1 < 400 && f2 > 1800) return LIB_VOWEL_E;
    
    /* /æ/ (AA): high F1 (~700Hz), mid F2 (~1700Hz) */
    if (f1 > 550 && f2 > 1400 && f2 < 2000) return LIB_VOWEL_A;
    
    /* /u/ (OO): low F1 (~350Hz), low F2 (~900Hz) */
    if (f1 < 450 && f2 < 1200) return LIB_VOWEL_U;
    
    /* /o/ (OH): mid F1 (~500Hz), low-mid F2 (~1000Hz) */
    if (f1 > 400 && f1 < 600 && f2 > 800 && f2 < 1300) return LIB_VOWEL_O;
    
    /* /ɪ/ (IH): mid F1 (~400Hz), mid F2 (~1500Hz) */
    if (f1 > 350 && f1 < 500 && f2 > 1200 && f2 < 1800) return LIB_VOWEL_I;
    
    return LIB_VOWEL_UNKNOWN;
}

/* Estimate formant frequencies from zero-crossing rate and energy */
lib_vowel_class lib_estimate_vowel_from_audio(const float *audio, int n_frames,
                                                int n_channels, float sample_rate) {
    if (!audio || n_frames <= 0) return LIB_VOWEL_UNKNOWN;
    
    /* Calculate ZCR as proxy for F1 */
    int crossings = 0;
    float energy = 0;
    for (int i = 1; i < n_frames; i++) {
        float s = 0, sp = 0;
        for (int c = 0; c < n_channels; c++) {
            s += audio[i * n_channels + c];
            sp += audio[(i-1) * n_channels + c];
        }
        s /= n_channels; sp /= n_channels;
        if ((s >= 0) != (sp >= 0)) crossings++;
        energy += s * s;
    }
    energy /= n_frames;
    float zcr = (float)crossings / n_frames;
    float f1_est = sample_rate * zcr / 2.0f;
    
    /* Use energy distribution to estimate F2 */
    /* High energy at high frequencies = high F2 */
    float high_energy = 0, low_energy = 0;
    int split = n_frames / 2;
    for (int i = 0; i < n_frames; i++) {
        float s = 0;
        for (int c = 0; c < n_channels; c++)
            s += audio[i * n_channels + c];
        s /= n_channels;
        if (i < split) low_energy += s * s;
        else high_energy += s * s;
    }
    float f2_est = 1000.0f;
    if (low_energy > 0) {
        float ratio = high_energy / low_energy;
        f2_est = 1000.0f + ratio * 1000.0f;
        if (f2_est > 3000.0f) f2_est = 3000.0f;
    }
    
    return lib_classify_vowel(f1_est, f2_est);
}

/* ================================================================
 * SOURCE ENTRY
 * ================================================================ */

typedef struct {
    char name[LIB_MAX_NAME];
    char audio_path[LIB_MAX_PATH];
    char video_path[LIB_MAX_PATH];
    char character[LIB_MAX_NAME];
    
    float start_time;      /* Position in source audio */
    float duration;
    float pitch_hz;
    int midi_note;
    lib_vowel_class vowel;
    
    float quality;         /* 0-1 quality score */
    int tags[LIB_MAX_TAGS];
    int n_tags;
    
    /* Usage statistics */
    int use_count;         /* How many times this source was used */
    float last_used;       /* Timestamp of last use */
} lib_source;

void lib_source_init(lib_source *src) {
    if (!src) return;
    memset(src, 0, sizeof(*src));
    src->pitch_hz = -1;
    src->midi_note = -1;
    src->vowel = LIB_VOWEL_UNKNOWN;
    src->quality = 0.5f;
}

/* ================================================================
 * SOURCE LIBRARY
 * ================================================================ */

typedef struct {
    lib_source sources[LIB_MAX_SOURCES];
    int n_sources;
    
    /* Character registry */
    char characters[LIB_MAX_CHARS][LIB_MAX_NAME];
    int char_sample_count[LIB_MAX_CHARS];
    int n_characters;
    
    /* Tag registry */
    char tag_names[LIB_MAX_TAGS * 4][LIB_MAX_TAG_LEN];
    int n_tags;
} ytpmv_library;

void ytpmv_library_init(ytpmv_library *lib) {
    if (!lib) return;
    memset(lib, 0, sizeof(*lib));
}

/* Register a character, return index */
int ytpmv_library_register_character(ytpmv_library *lib, const char *name) {
    if (!lib || lib->n_characters >= LIB_MAX_CHARS) return -1;
    
    /* Check if already registered */
    for (int i = 0; i < lib->n_characters; i++) {
        if (strcmp(lib->characters[i], name) == 0) return i;
    }
    
    int idx = lib->n_characters++;
    strncpy(lib->characters[idx], name, LIB_MAX_NAME - 1);
    return idx;
}

/* Add a source to the library */
int ytpmv_library_add_source(ytpmv_library *lib, const char *name,
                               const char *audio_path, const char *video_path,
                               const char *character, float start, float dur,
                               float pitch_hz) {
    if (!lib || lib->n_sources >= LIB_MAX_SOURCES) return -1;
    
    int idx = lib->n_sources++;
    lib_source *src = &lib->sources[idx];
    lib_source_init(src);
    
    strncpy(src->name, name, LIB_MAX_NAME - 1);
    strncpy(src->audio_path, audio_path, LIB_MAX_PATH - 1);
    strncpy(src->video_path, video_path, LIB_MAX_PATH - 1);
    strncpy(src->character, character, LIB_MAX_NAME - 1);
    src->start_time = start;
    src->duration = dur;
    src->pitch_hz = pitch_hz;
    src->midi_note = (pitch_hz > 0) ? (int)(69.0f + 12.0f * log2f(pitch_hz / 440.0f) + 0.5f) : -1;
    
    /* Register character */
    int char_idx = ytpmv_library_register_character(lib, character);
    if (char_idx >= 0) lib->char_sample_count[char_idx]++;
    
    return idx;
}

/* Remove a source by index */
void ytpmv_library_remove_source(ytpmv_library *lib, int index) {
    if (!lib || index < 0 || index >= lib->n_sources) return;
    
    /* Shift remaining sources */
    for (int i = index; i < lib->n_sources - 1; i++) {
        lib->sources[i] = lib->sources[i + 1];
    }
    lib->n_sources--;
}

/* ================================================================
 * QUERY / SEARCH
 * ================================================================ */

/* Find best source for a target MIDI note and optional character */
int ytpmv_library_find_best(ytpmv_library *lib, int target_midi,
                              const char *character, lib_vowel_class vowel) {
    if (!lib || lib->n_sources == 0) return -1;
    
    int best = -1;
    float best_score = -1.0f;
    
    for (int i = 0; i < lib->n_sources; i++) {
        lib_source *src = &lib->sources[i];
        
        /* Filter by character if specified */
        if (character && strlen(character) > 0) {
            if (strcmp(src->character, character) != 0) continue;
        }
        
        /* Filter by vowel if specified */
        if (vowel != LIB_VOWEL_UNKNOWN && src->vowel != LIB_VOWEL_UNKNOWN) {
            if (src->vowel != vowel) continue;
        }
        
        /* Score: pitch proximity + quality */
        float pitch_dist = (src->midi_note >= 0) ? fabsf((float)(target_midi - src->midi_note)) : 999.0f;
        float pitch_score = fmaxf(0.0f, 1.0f - pitch_dist / 12.0f);
        float score = pitch_score * 0.6f + src->quality * 0.4f;
        
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    
    return best;
}

/* Find all sources for a character */
int ytpmv_library_find_by_character(ytpmv_library *lib, const char *character,
                                      int *indices, int max_results) {
    if (!lib || !indices) return 0;
    
    int count = 0;
    for (int i = 0; i < lib->n_sources && count < max_results; i++) {
        if (strcmp(lib->sources[i].character, character) == 0) {
            indices[count++] = i;
        }
    }
    return count;
}

/* Find all sources in a MIDI range */
int ytpmv_library_find_by_pitch_range(ytpmv_library *lib, int min_midi, int max_midi,
                                        int *indices, int max_results) {
    if (!lib || !indices) return 0;
    
    int count = 0;
    for (int i = 0; i < lib->n_sources && count < max_results; i++) {
        lib_source *src = &lib->sources[i];
        if (src->midi_note >= min_midi && src->midi_note <= max_midi) {
            indices[count++] = i;
        }
    }
    return count;
}

/* Find all sources by vowel class */
int ytpmv_library_find_by_vowel(ytpmv_library *lib, lib_vowel_class vowel,
                                  int *indices, int max_results) {
    if (!lib || !indices) return 0;
    
    int count = 0;
    for (int i = 0; i < lib->n_sources && count < max_results; i++) {
        if (lib->sources[i].vowel == vowel) {
            indices[count++] = i;
        }
    }
    return count;
}

/* ================================================================
 * TAG MANAGEMENT
 * ================================================================ */

/* Register a tag, return tag ID */
int ytpmv_library_register_tag(ytpmv_library *lib, const char *tag_name) {
    if (!lib || lib->n_tags >= LIB_MAX_TAGS * 4) return -1;
    
    /* Check if already exists */
    for (int i = 0; i < lib->n_tags; i++) {
        if (strcmp(lib->tag_names[i], tag_name) == 0) return i;
    }
    
    int idx = lib->n_tags++;
    strncpy(lib->tag_names[idx], tag_name, LIB_MAX_TAG_LEN - 1);
    return idx;
}

/* Add tag to a source */
void ytpmv_library_tag_source(ytpmv_library *lib, int source_idx, const char *tag_name) {
    if (!lib || source_idx < 0 || source_idx >= lib->n_sources) return;
    
    int tag_id = ytpmv_library_register_tag(lib, tag_name);
    if (tag_id < 0) return;
    
    lib_source *src = &lib->sources[source_idx];
    if (src->n_tags >= LIB_MAX_TAGS) return;
    src->tags[src->n_tags++] = tag_id;
}

/* Check if source has a tag */
int ytpmv_library_has_tag(ytpmv_library *lib, int source_idx, const char *tag_name) {
    if (!lib || source_idx < 0 || source_idx >= lib->n_sources) return 0;
    
    int tag_id = -1;
    for (int i = 0; i < lib->n_tags; i++) {
        if (strcmp(lib->tag_names[i], tag_name) == 0) { tag_id = i; break; }
    }
    if (tag_id < 0) return 0;
    
    lib_source *src = &lib->sources[source_idx];
    for (int i = 0; i < src->n_tags; i++) {
        if (src->tags[i] == tag_id) return 1;
    }
    return 0;
}

/* ================================================================
 * STATISTICS
 * ================================================================ */

typedef struct {
    int total_sources;
    int total_characters;
    int total_tags;
    int min_midi;
    int max_midi;
    float avg_quality;
    char most_used_char[LIB_MAX_NAME];
    int most_used_count;
} ytpmv_library_stats;

ytpmv_library_stats ytpmv_library_get_stats(ytpmv_library *lib) {
    ytpmv_library_stats stats = {0};
    if (!lib) return stats;
    
    stats.total_sources = lib->n_sources;
    stats.total_characters = lib->n_characters;
    stats.total_tags = lib->n_tags;
    stats.min_midi = 127;
    stats.max_midi = 0;
    stats.most_used_count = 0;
    
    float total_quality = 0;
    for (int i = 0; i < lib->n_sources; i++) {
        lib_source *src = &lib->sources[i];
        if (src->midi_note >= 0) {
            if (src->midi_note < stats.min_midi) stats.min_midi = src->midi_note;
            if (src->midi_note > stats.max_midi) stats.max_midi = src->midi_note;
        }
        total_quality += src->quality;
        
        if (src->use_count > stats.most_used_count) {
            stats.most_used_count = src->use_count;
            strncpy(stats.most_used_char, src->character, LIB_MAX_NAME - 1);
        }
    }
    
    if (lib->n_sources > 0) {
        stats.avg_quality = total_quality / lib->n_sources;
    }
    
    return stats;
}

/* ================================================================
 * AUTO-POPULATE FROM DIRECTORY
 * ================================================================ */

/* Scan a directory for audio/video files and auto-add to library */
int ytpmv_library_scan_directory(ytpmv_library *lib, const char *dir_path) {
    if (!lib || !dir_path) return 0;
    
    /* Use system() to list files — simple approach */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "find \"%s\" -name \"*.wav\" -o -name \"*.mp4\" | head -256", dir_path);
    
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    
    char line[512];
    int added = 0;
    while (fgets(line, sizeof(line), fp) && lib->n_sources < LIB_MAX_SOURCES) {
        /* Remove newline */
        line[strcspn(line, "\n")] = '\0';
        if (strlen(line) == 0) continue;
        
        /* Extract filename without extension */
        const char *basename = strrchr(line, '/');
        basename = basename ? basename + 1 : line;
        
        char name[LIB_MAX_NAME];
        strncpy(name, basename, LIB_MAX_NAME - 1);
        char *dot = strrchr(name, '.');
        if (dot) *dot = '\0';
        
        /* Determine character from filename prefix (e.g., "spongebob_ah_220") */
        char character[LIB_MAX_NAME] = "unknown";
        char *underscore = strchr(name, '_');
        if (underscore) {
            int len = underscore - name;
            if (len < LIB_MAX_NAME) {
                strncpy(character, name, len);
                character[len] = '\0';
            }
        }
        
        /* Add source (pitch will be estimated later) */
        int idx = ytpmv_library_add_source(lib, name, line, "", character, 0.0f, 0.0f, -1.0f);
        if (idx >= 0) added++;
    }
    
    pclose(fp);
    return added;
}

/* ================================================================
 * EXPORT (simple text format)
 * ================================================================ */

/* Export library to a text file */
int ytpmv_library_export(ytpmv_library *lib, const char *path) {
    if (!lib || !path) return -1;
    
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    
    fprintf(f, "# YTPMV Library Export\n");
    fprintf(f, "# characters: %d sources: %d\n\n", lib->n_characters, lib->n_sources);
    
    for (int i = 0; i < lib->n_characters; i++) {
        fprintf(f, "[character] %s (samples: %d)\n", lib->characters[i], lib->char_sample_count[i]);
    }
    fprintf(f, "\n");
    
    for (int i = 0; i < lib->n_sources; i++) {
        lib_source *src = &lib->sources[i];
        fprintf(f, "[source] %s\n", src->name);
        fprintf(f, "  character: %s\n", src->character);
        fprintf(f, "  audio: %s\n", src->audio_path);
        fprintf(f, "  video: %s\n", src->video_path);
        fprintf(f, "  start: %.4f duration: %.4f\n", src->start_time, src->duration);
        fprintf(f, "  pitch: %.1fHz midi: %d\n", src->pitch_hz, src->midi_note);
        fprintf(f, "  vowel: %s quality: %.2f\n", lib_vowel_name(src->vowel), src->quality);
        fprintf(f, "  tags: %d uses: %d\n", src->n_tags, src->use_count);
        fprintf(f, "\n");
    }
    
    fclose(f);
    return 0;
}
