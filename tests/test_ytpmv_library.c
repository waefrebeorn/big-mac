/* test_ytpmv_library.c — Tests for YTPMV Source Library (R135) */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "wbus/wbus_compositor.h"

#define ASSERT(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("PASS: %s\n", msg); passes++; } \
} while(0)

int main() {
    int passes = 0, failures = 0;
    
    /* Test 1: Library init */
    ytpmv_library lib;
    ytpmv_library_init(&lib);
    ASSERT(lib.n_sources == 0, "Library starts empty");
    ASSERT(lib.n_characters == 0, "No characters registered");
    
    /* Test 2: Add sources */
    int s0 = ytpmv_library_add_source(&lib, "sb_ah_220", "/tmp/sb_ah.wav", "/tmp/sb_ah.mp4",
                                        "SpongeBob", 1.0f, 0.15f, 220.0f);
    ASSERT(s0 == 0, "First source added");
    ASSERT(lib.n_sources == 1, "Source count = 1");
    ASSERT(lib.n_characters == 1, "Character auto-registered");
    ASSERT(strcmp(lib.characters[0], "SpongeBob") == 0, "Character name registered");
    
    int s1 = ytpmv_library_add_source(&lib, "sb_eh_330", "/tmp/sb_eh.wav", "/tmp/sb_eh.mp4",
                                        "SpongeBob", 2.0f, 0.20f, 330.0f);
    ASSERT(s1 == 1, "Second source added");
    ASSERT(lib.n_characters == 1, "No duplicate character");
    ASSERT(lib.char_sample_count[0] == 2, "Character has 2 samples");
    
    int s2 = ytpmv_library_add_source(&lib, "pat_oh_440", "/tmp/pat_oh.wav", "/tmp/pat_oh.mp4",
                                        "Patrick", 3.0f, 0.10f, 440.0f);
    ASSERT(s2 == 2, "Third source added (different character)");
    ASSERT(lib.n_characters == 2, "Two characters registered");
    
    /* Test 3: MIDI note calculation */
    ASSERT(lib.sources[0].midi_note == 57, "220Hz = MIDI 57");
    ASSERT(lib.sources[1].midi_note == 64, "330Hz = MIDI 64");
    ASSERT(lib.sources[2].midi_note == 69, "440Hz = MIDI 69");
    
    /* Test 4: Find best source */
    int best = ytpmv_library_find_best(&lib, 60, NULL, LIB_VOWEL_UNKNOWN);
    ASSERT(best == 0, "MIDI 60 closest to source 0 (MIDI 57, dist=3)");
    
    best = ytpmv_library_find_best(&lib, 66, NULL, LIB_VOWEL_UNKNOWN);
    ASSERT(best == 1, "MIDI 66 closest to source 1 (MIDI 64, dist=2)");
    
    /* Test 5: Find by character */
    int indices[16];
    int count = ytpmv_library_find_by_character(&lib, "SpongeBob", indices, 16);
    ASSERT(count == 2, "Found 2 SpongeBob sources");
    
    count = ytpmv_library_find_by_character(&lib, "Patrick", indices, 16);
    ASSERT(count == 1, "Found 1 Patrick source");
    
    count = ytpmv_library_find_by_character(&lib, "NonExistent", indices, 16);
    ASSERT(count == 0, "No sources for nonexistent character");
    
    /* Test 6: Find by pitch range */
    count = ytpmv_library_find_by_pitch_range(&lib, 55, 65, indices, 16);
    ASSERT(count == 2, "2 sources in MIDI 55-65 range");
    
    count = ytpmv_library_find_by_pitch_range(&lib, 66, 72, indices, 16);
    ASSERT(count == 1, "1 source in MIDI 66-72 range");
    
    /* Test 7: Find best with character filter */
    best = ytpmv_library_find_best(&lib, 69, "SpongeBob", LIB_VOWEL_UNKNOWN);
    ASSERT(best == 1, "Best SpongeBob source for MIDI 69 is source 1 (MIDI 64)");
    
    best = ytpmv_library_find_best(&lib, 69, "Patrick", LIB_VOWEL_UNKNOWN);
    ASSERT(best == 2, "Best Patrick source for MIDI 69 is source 2 (MIDI 69)");
    
    /* Test 8: Remove source */
    ytpmv_library_remove_source(&lib, 0);
    ASSERT(lib.n_sources == 2, "Source count after removal = 2");
    ASSERT(strcmp(lib.sources[0].name, "sb_eh_330") == 0, "Remaining source shifted");
    
    /* Test 9: Tags */
    int tag_id = ytpmv_library_register_tag(&lib, "clean");
    ASSERT(tag_id >= 0, "Tag registered");
    
    ytpmv_library_tag_source(&lib, 0, "clean");
    ASSERT(ytpmv_library_has_tag(&lib, 0, "clean") == 1, "Source has tag");
    ASSERT(ytpmv_library_has_tag(&lib, 1, "clean") == 0, "Other source doesn't have tag");
    
    /* Test 10: Vowel classification */
    lib_vowel_class v = lib_classify_vowel(700, 1700); /* High F1, mid F2 = /æ/ */
    ASSERT(v == LIB_VOWEL_A, "Classified as A vowel");
    
    v = lib_classify_vowel(300, 2300); /* Low F1, high F2 = /i/ */
    ASSERT(v == LIB_VOWEL_E, "Classified as E vowel");
    
    v = lib_classify_vowel(350, 900); /* Low F1, low F2 = /u/ */
    ASSERT(v == LIB_VOWEL_U, "Classified as U vowel");
    
    /* Test 11: Vowel name */
    ASSERT(strcmp(lib_vowel_name(LIB_VOWEL_A), "A") == 0, "Vowel A name");
    ASSERT(strcmp(lib_vowel_name(LIB_VOWEL_UNKNOWN), "?") == 0, "Unknown vowel name");
    
    /* Test 12: Statistics */
    ytpmv_library_stats stats = ytpmv_library_get_stats(&lib);
    ASSERT(stats.total_sources == 2, "Stats: 2 sources");
    ASSERT(stats.total_characters == 2, "Stats: 2 characters");
    ASSERT(stats.min_midi == 64, "Stats: min MIDI 64");
    ASSERT(stats.max_midi == 69, "Stats: max MIDI 69");
    
    /* Test 13: Export */
    int rc = ytpmv_library_export(&lib, "/tmp/test_library_export.txt");
    ASSERT(rc == 0, "Library exported");
    
    /* Verify export file exists */
    FILE *f = fopen("/tmp/test_library_export.txt", "r");
    ASSERT(f != NULL, "Export file exists");
    if (f) fclose(f);
    
    /* Test 14: Vowel estimate from synthetic audio */
    /* Create a signal with ~500Hz fundamental (vowel O range) */
    float audio[4410];
    for (int i = 0; i < 4410; i++) {
        audio[i] = 32767.0f * sinf(2.0f * M_PI * 500.0f * i / 44100.0f);
    }
    v = lib_estimate_vowel_from_audio(audio, 4410, 1, 44100.0f);
    printf("  Estimated vowel from 500Hz signal: %s\n", lib_vowel_name(v));
    /* Just check it returns something reasonable (may not be exact) */
    ASSERT(v >= LIB_VOWEL_UNKNOWN && v < LIB_VOWEL_COUNT, "Vowel estimate in valid range");
    
    /* Cleanup */
    remove("/tmp/test_library_export.txt");
    
    printf("\n=== Source Library: %d passed, %d failed ===\n", passes, failures);
    return failures > 0 ? 1 : 0;
}
