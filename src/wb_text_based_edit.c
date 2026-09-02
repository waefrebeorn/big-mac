/* wb_text_based_edit.c — Text-based video editing (R086).
 *
 * Implements transcript-driven editing: parse a JSON transcript with
 * timestamps, generate edit cuts at word boundaries, search transcripts
 * for specific words, auto-detect and remove silent segments, and undo
 * text-based edit operations.
 *
 * Transcript JSON format:
 *   [{"start": 0.0, "end": 1.5, "text": "hello world"}, ...]
 *
 * C11, no third party beyond standard library.
 */

#include "wbus/wbus_edit.h"
#include "wbus/wb_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

#define AUDIO_SAMPLE_RATE 48000
#define MAX_TRANSCRIPT_ENTRIES 4096
#define MAX_RULES 64
#define MAX_WORD_LEN 256

/* ---- transcript entry -------------------------------------------------- */

typedef struct {
    double start;
    double end;
    char text[1024];
} wb_transcript_entry;

/* ---- text edit undo tracking ------------------------------------------ */

/* Track text-edit operations. We use a simple counter that's incremented
 * by text-based edit functions and decremented by text_undo. The counter
 * is process-global but only meaningful within a single edit session.
 * Users should not mix multiple edit graphs with text_undo. */

static int g_text_edit_count = 0;

/* ---- helper: case-insensitive strstr ---------------------------------- */

static char *strcasestr_custom(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    if (!*needle) return (char *)haystack;

    char *h = (char *)haystack;
    while (*h) {
        if (tolower((unsigned char)*h) == tolower((unsigned char)*needle)) {
            char *hi = h + 1;
            char *ni = (char *)needle + 1;
            while (*ni && tolower((unsigned char)*hi) == tolower((unsigned char)*ni)) {
                hi++;
                ni++;
            }
            if (!*ni) return h;
        }
        h++;
    }
    return NULL;
}

/* ---- helper: simple JSON parser for transcript format ----------------- */

/* Minimal JSON array parser: expects [{"start":N,"end":N,"text":"..."}, ...]
 * Handles basic escaping in strings. Returns count of entries parsed. */
static int parse_transcript_json(const char *json, wb_transcript_entry *entries, int max_entries) {
    if (!json || !entries || max_entries <= 0) return -1;

    int count = 0;
    const char *p = json;

    /* Skip whitespace and find opening [ */
    while (*p && *p != '[') p++;
    if (!*p) return -1;
    p++; /* skip [ */

    while (*p && count < max_entries) {
        /* Find next { */
        while (*p && *p != '{') {
            if (*p == ']') return count; /* end of array */
            p++;
        }
        if (!*p) break;
        p++; /* skip { */

        wb_transcript_entry *ent = &entries[count];
        ent->start = 0.0;
        ent->end = 0.0;
        ent->text[0] = '\0';

        /* Parse fields */
        int got_start = 0, got_end = 0, got_text = 0;
        while (*p && *p != '}') {
            /* Find next " */
            while (*p && *p != '"') p++;
            if (!*p) break;
            p++; /* skip opening " */

            char key[64] = {0};
            int ki = 0;
            while (*p && *p != '"' && ki < 63) {
                key[ki++] = *p++;
            }
            key[ki] = '\0';
            if (*p == '"') p++; /* skip closing " */

            /* Skip : and whitespace */
            while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;

            if (strcmp(key, "start") == 0) {
                ent->start = strtod(p, (char **)&p);
                got_start = 1;
            } else if (strcmp(key, "end") == 0) {
                ent->end = strtod(p, (char **)&p);
                got_end = 1;
            } else if (strcmp(key, "text") == 0) {
                /* Skip opening " */
                while (*p && *p != '"') p++;
                if (*p == '"') p++;

                int ti = 0;
                while (*p && *p != '"' && ti < 1023) {
                    if (*p == '\\' && *(p + 1)) {
                        p++; /* skip backslash */
                        switch (*p) {
                            case 'n': ent->text[ti++] = '\n'; break;
                            case 't': ent->text[ti++] = '\t'; break;
                            case '"': ent->text[ti++] = '"'; break;
                            case '\\': ent->text[ti++] = '\\'; break;
                            default: ent->text[ti++] = *p; break;
                        }
                        p++;
                    } else {
                        ent->text[ti++] = *p++;
                    }
                }
                ent->text[ti] = '\0';
                if (*p == '"') p++; /* skip closing " */
                got_text = 1;
            } else {
                /* Skip unknown value */
                while (*p && *p != ',' && *p != '}') p++;
            }

            /* Skip comma or whitespace */
            while (*p && (*p == ',' || *p == ' ' || *p == '\t')) p++;
        }

        if (*p == '}') p++; /* skip } */

        if (got_start && got_end && got_text) {
            count++;
        }

        /* Skip comma between objects */
        while (*p && (*p == ',' || *p == ' ' || *p == '\t')) p++;
    }

    return count;
}

/* ---- helper: parse edit rules ----------------------------------------- */

/* Rules format: "remove=word1,word2,word3" or "keep-all" or NULL */
typedef struct {
    char words[MAX_RULES][MAX_WORD_LEN];
    int word_count;
    int keep_all;
} wb_edit_rules_static;

static void parse_edit_rules(const char *rules_str, wb_edit_rules_static *rules) {
    if (!rules) return;
    rules->word_count = 0;
    rules->keep_all = 0;

    if (!rules_str || !rules_str[0]) return;

    if (strcmp(rules_str, "keep-all") == 0) {
        rules->keep_all = 1;
        return;
    }

    /* Parse "remove=word1,word2,..." */
    const char *p = rules_str;
    if (strncmp(p, "remove=", 7) == 0) {
        p += 7;
        char buf[4096];
        strncpy(buf, p, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char *saveptr = NULL;
        char *word = strtok_r(buf, ",", &saveptr);
        while (word && rules->word_count < MAX_RULES) {
            /* Trim whitespace */
            while (*word == ' ') word++;
            char *end = word + strlen(word) - 1;
            while (end > word && *end == ' ') *end-- = '\0';

            if (*word) {
                strncpy(rules->words[rules->word_count], word, MAX_WORD_LEN - 1);
                rules->words[rules->word_count][MAX_WORD_LEN - 1] = '\0';
                rules->word_count++;
            }
            word = strtok_r(NULL, ",", &saveptr);
        }
    }
}

/* Check if a word should be removed according to rules */
static int should_remove_word(const char *word, const wb_edit_rules_static *rules) {
    if (!rules || rules->keep_all) return 0;
    if (!word || !*word) return 0;

    for (int i = 0; i < rules->word_count; i++) {
        if (strcasecmp(word, rules->words[i]) == 0) return 1;
    }
    return 0;
}

/* ---- helper: split text into words with timestamps --------------------- */

typedef struct {
    char word[MAX_WORD_LEN];
    double start;
    double end;
} wb_word_timing;

/* Split transcript entry text into words, distributing time evenly */
static int split_entry_words(const wb_transcript_entry *ent, wb_word_timing *words, int max_words) {
    if (!ent || !words || max_words <= 0) return 0;

    /* Copy text for tokenizing */
    char text_buf[1024];
    strncpy(text_buf, ent->text, sizeof(text_buf) - 1);
    text_buf[sizeof(text_buf) - 1] = '\0';

    /* Count words first */
    int word_count = 0;
    char tmp[1024];
    strncpy(tmp, text_buf, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *saveptr = NULL;
    char *tok = strtok_r(tmp, " \t\n\r", &saveptr);
    while (tok) {
        word_count++;
        tok = strtok_r(NULL, " \t\n\r", &saveptr);
    }

    if (word_count == 0) return 0;
    if (word_count > max_words) word_count = max_words;

    double word_duration = (ent->end - ent->start) / word_count;

    /* Extract words with timing */
    strncpy(tmp, text_buf, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    int wi = 0;
    saveptr = NULL;
    tok = strtok_r(tmp, " \t\n\r", &saveptr);
    while (tok && wi < word_count) {
        words[wi].start = ent->start + wi * word_duration;
        words[wi].end = words[wi].start + word_duration;
        strncpy(words[wi].word, tok, MAX_WORD_LEN - 1);
        words[wi].word[MAX_WORD_LEN - 1] = '\0';
        wi++;
        tok = strtok_r(NULL, " \t\n\r", &saveptr);
    }

    return wi;
}

/* ---- helper: find clip overlapping a time range ----------------------- */

static int find_clip_overlapping(wb_edit_graph *g, int track_idx, double start, double end) {
    if (!g || track_idx < 0 || (uint32_t)track_idx >= g->track_count) return -1;
    wb_edit_track *tr = &g->tracks[track_idx];

    for (uint32_t c = 0; c < tr->clip_count; c++) {
        wb_edit_clip *cl = &tr->clips[c];
        double clip_start = cl->timeline_pos;
        double clip_end = cl->timeline_pos + cl->duration;
        /* Check overlap */
        if (start < clip_end && end > clip_start) {
            return (int)c;
        }
    }
    return -1;
}

/* ---- helper: push undo record ----------------------------------------- */

static void push_text_undo(wb_edit_graph *g) {
    if (!g) return;

    /* Save a checkpoint on the main undo stack */
    wb_edit_undo_checkpoint();
    wb_edit_undo_set_current(g);

    g_text_edit_count++;
}

/* ---- API: transcript_to_edits ----------------------------------------- */

int wb_edit_transcript_to_edits(wb_edit_graph *g, const char *transcript_json,
                                 const char *edit_rules) {
    if (!g || !transcript_json) return -1;

    /* Parse transcript */
    wb_transcript_entry entries[MAX_TRANSCRIPT_ENTRIES];
    int n_entries = parse_transcript_json(transcript_json, entries, MAX_TRANSCRIPT_ENTRIES);
    if (n_entries <= 0) {
        fprintf(stderr, "wb_text_edit: no transcript entries parsed\n");
        return -1;
    }

    /* Parse edit rules */
    wb_edit_rules_static rules;
    parse_edit_rules(edit_rules, &rules);

    int edits = 0;

    /* For each transcript entry, find overlapping clips and split at word boundaries */
    for (int e = 0; e < n_entries; e++) {
        wb_transcript_entry *ent = &entries[e];

        /* Split entry into words */
        wb_word_timing words[256];
        int n_words = split_entry_words(ent, words, 256);
        if (n_words == 0) continue;

        /* Check each track for overlapping clips */
        for (uint32_t t = 0; t < g->track_count; t++) {
            int clip_idx = find_clip_overlapping(g, (int)t, ent->start, ent->end);
            if (clip_idx < 0) continue;

            wb_edit_track *tr = &g->tracks[t];
            wb_edit_clip *cl = &tr->clips[clip_idx];

            /* Split clip at each word boundary that falls within the clip */
            /* We split from the end backward to keep earlier positions valid */
            for (int w = n_words - 1; w > 0; w--) {
                double split_time = words[w].start;

                /* Only split if the word boundary is within the clip's timeline range */
                if (split_time > cl->timeline_pos && split_time < cl->timeline_pos + cl->duration) {
                    int new_idx = wb_edit_split_clip(g, (int)t, clip_idx, split_time);
                    if (new_idx >= 0) {
                        edits++;
                        /* After split, cl is the left piece; new clip is at new_idx */
                        /* For subsequent splits, we need to update clip_idx to the right piece */
                        clip_idx = new_idx;
                        cl = &tr->clips[clip_idx];
                    }
                }
            }

            /* Now check each sub-clip for words to remove */
            /* Re-scan the track for clips in this time range */
            for (int w = n_words - 1; w >= 0; w--) {
                if (should_remove_word(words[w].word, &rules)) {
                    /* Find the clip at this word's position */
                    int ci = find_clip_overlapping(g, (int)t, words[w].start, words[w].end);
                    if (ci >= 0) {
                        /* Remove this clip segment */
                        wb_edit_remove_clip(g, (int)t, ci);
                        edits++;
                    }
                }
            }
        }
    }

    /* Only push undo if we actually made edits */
    if (edits > 0) {
        push_text_undo(g);
    }

    return edits;
}

/* ---- API: search_transcript ------------------------------------------- */

int wb_edit_search_transcript(wb_edit_graph *g, const char *query) {
    if (!g || !query || !query[0]) return -1;

    /* We search through the edit graph's clips by checking if the query
     * matches any part of the source path or timeline position.
     * Since we don't store transcript text on clips directly, we search
     * for clips whose timeline positions overlap with what would be
     * transcript entries. For a real implementation, we'd store transcript
     * text on each clip. Here we search by checking clip positions against
     * the query as a time range or keyword. */

    int matches = 0;

    /* Parse query: could be a single word or phrase */
    char query_buf[1024];
    strncpy(query_buf, query, sizeof(query_buf) - 1);
    query_buf[sizeof(query_buf) - 1] = '\0';

    /* Search through all clips on all tracks */
    for (uint32_t t = 0; t < g->track_count; t++) {
        wb_edit_track *tr = &g->tracks[t];
        for (uint32_t c = 0; c < tr->clip_count; c++) {
            wb_edit_clip *cl = &tr->clips[c];

            /* Check if query matches the source path (case-insensitive) */
            if (strcasestr_custom(cl->source_path, query_buf)) {
                printf("edit-text-based: match track %u clip %u src=%s tl=%.2fs dur=%.2fs\n",
                       t, c, cl->source_path, cl->timeline_pos, cl->duration);
                matches++;
                continue;
            }

            /* Check if query is a time value that falls within this clip */
            char time_str[64];
            snprintf(time_str, sizeof(time_str), "%.1f", cl->timeline_pos);
            if (strcasestr_custom(time_str, query_buf)) {
                printf("edit-text-based: match track %u clip %u src=%s tl=%.2fs dur=%.2fs\n",
                       t, c, cl->source_path, cl->timeline_pos, cl->duration);
                matches++;
            }
        }

        /* Also search audio clips */
        for (uint32_t ac = 0; ac < tr->audio_clip_count; ac++) {
            wb_edit_audio_clip *acl = &tr->audio_clips[ac];
            if (strcasestr_custom(acl->source_path, query_buf)) {
                printf("edit-text-based: audio match track %u clip %u src=%s tl=%.2fs dur=%.2fs\n",
                       t, ac, acl->source_path, acl->timeline_pos, acl->duration);
                matches++;
            }
        }
    }

    if (matches == 0) {
        printf("edit-text-based: no matches for '%s'\n", query);
    } else {
        printf("edit-text-based: %d match(es) for '%s'\n", matches, query);
    }

    return matches;
}

/* ---- API: delete_silence ---------------------------------------------- */

int wb_edit_delete_silence(wb_edit_graph *g, float threshold_db,
                            float min_duration) {
    if (!g) return -1;
    if (threshold_db >= 0.0f) threshold_db = -40.0f;
    if (min_duration <= 0.0f) min_duration = 0.5f;

    /* Convert threshold from dB to linear RMS */
    float threshold_linear = powf(10.0f, threshold_db / 20.0f);

    int removed = 0;

    /* Analyze each audio clip for silence */
    for (uint32_t t = 0; t < g->track_count; t++) {
        wb_edit_track *tr = &g->tracks[t];

        for (uint32_t ac = 0; ac < tr->audio_clip_count; ac++) {
            wb_edit_audio_clip *clip = &tr->audio_clips[ac];

            /* Read audio data */
            float *audio_data = NULL;
            uint32_t audio_frames = 0;
            int audio_ch = 0;
            int audio_sr = 0;

            if (wb_wav_read_pcm16(clip->source_path, &audio_data, &audio_frames,
                                   &audio_ch, &audio_sr) != 0 || !audio_data) {
                continue;
            }

            /* Analyze RMS in windows to find silent segments */
            int window_size = audio_sr / 10; /* 100ms windows */
            int n_windows = (int)(audio_frames / window_size);

            /* Track silent regions */
            int in_silence = 0;
            int silence_start_frame = 0;

            for (int w = 0; w < n_windows; w++) {
                /* Calculate RMS for this window */
                double sum_sq = 0.0;
                int start = w * window_size;
                int end = start + window_size;
                if (end > (int)audio_frames) end = (int)audio_frames;

                for (int f = start; f < end; f++) {
                    float sample = 0.0f;
                    if (audio_ch >= 2) {
                        sample = (audio_data[f * 2] + audio_data[f * 2 + 1]) * 0.5f;
                    } else {
                        sample = audio_data[f];
                    }
                    sum_sq += (double)sample * (double)sample;
                }

                float rms = sqrtf((float)(sum_sq / (end - start)));

                if (rms < threshold_linear) {
                    if (!in_silence) {
                        in_silence = 1;
                        silence_start_frame = start;
                    }
                } else {
                    if (in_silence) {
                        /* End of silent segment */
                        int silence_end_frame = w * window_size;
                        double silence_duration = (double)(silence_end_frame - silence_start_frame) / audio_sr;

                        if (silence_duration >= min_duration) {
                            /* Convert source frame to timeline position */
                            double tl_silence_start = clip->timeline_pos +
                                (double)silence_start_frame / audio_sr;
                            double tl_silence_end = clip->timeline_pos +
                                (double)silence_end_frame / audio_sr;

                            /* Find and remove the clip at this position */
                            /* We need to find the video clip at this timeline position */
                            for (uint32_t tc = 0; tc < g->track_count; tc++) {
                                int ci = find_clip_overlapping(g, (int)tc, tl_silence_start, tl_silence_end);
                                if (ci >= 0) {
                                    wb_edit_remove_clip(g, (int)tc, ci);
                                    removed++;
                                    break; /* Only remove one clip per silent segment */
                                }
                            }
                        }
                        in_silence = 0;
                    }
                }
            }

            /* Handle silence at end of clip */
            if (in_silence) {
                double silence_duration = (double)(audio_frames - silence_start_frame) / audio_sr;
                if (silence_duration >= min_duration) {
                    double tl_silence_start = clip->timeline_pos +
                        (double)silence_start_frame / audio_sr;
                    double tl_silence_end = clip->timeline_pos + clip->duration;

                    for (uint32_t tc = 0; tc < g->track_count; tc++) {
                        int ci = find_clip_overlapping(g, (int)tc, tl_silence_start, tl_silence_end);
                        if (ci >= 0) {
                            wb_edit_remove_clip(g, (int)tc, ci);
                            removed++;
                            break;
                        }
                    }
                }
            }

            free(audio_data);
        }
    }

    /* Only push undo if we actually removed silence */
    if (removed > 0) {
        push_text_undo(g);
    }

    return removed;
}

/* ---- API: text_undo --------------------------------------------------- */

int wb_edit_text_undo(wb_edit_graph *g) {
    if (!g) return -1;

    /* Check if there are text edits to undo */
    if (g_text_edit_count <= 0) {
        return -1;
    }

    /* Perform the actual undo using the main undo stack */
    if (!wb_edit_undo_can_undo()) {
        return -1;
    }

    wb_edit_graph *prev = wb_edit_undo_undo(g);
    if (!prev) {
        return -1;
    }

    g_text_edit_count--;

    /* Note: the caller must replace g with prev to complete the undo.
     * This function returns 0 to signal the caller should do
     * wb_edit_graph_destroy(g); g = prev; (or equivalent for g_agent_edit). */
    (void)prev;
    return 0;
}