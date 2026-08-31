/* test_aaf_export.c — AAF/OMF interchange export verification.
 *
 * Builds a 2-track session, exports to AAF and OMF, and verifies:
 *   1. AAF file exists, non-empty
 *   2. OMF file exists, non-empty
 *   3. AAF file contains session name
 *   4. AAF file contains track names
 *   5. OMF file contains valid header bytes ("OMFI" magic)
 *   6. Export with NULL session returns error
 *   7. Export with 0 tracks returns error
 *   8. Round-trip: export then verify file is parseable (basic structure check)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "wbus/wbus.h"

static int failures = 0, checks = 0;
#define CK(c, m) do { checks++; if (c) printf("  [PASS] %s\n", m); \
    else { printf("  [FAIL] %s\n", m); failures++; } } while (0)

/* Build a 2-track session with audio clips. */
static wb_session *make_2track_session(void) {
    wb_session *s = wb_session_create();
    if (!s) return NULL;
    snprintf(s->name, sizeof(s->name), "TestSession_AAF");
    s->bpm = 128.0;
    s->time_sig_num = 4;
    s->time_sig_den = 4;
    s->length = 44100.0 * 4.0;  /* 4 seconds */

    /* Track 0: audio */
    wb_track *t0 = wb_session_add_track(s, "Drums", WB_TRACK_KIND_AUDIO);
    {
        uint32_t nf = 44100 * 2;  /* 2 seconds */
        float *buf = (float*)calloc(nf, sizeof(float));
        wb_session_add_audio_clip(t0, 0.0, (double)nf, buf, nf, 1);
        free(buf);
    }

    /* Track 1: audio */
    wb_track *t1 = wb_session_add_track(s, "Bass", WB_TRACK_KIND_AUDIO);
    {
        uint32_t nf = 44100 * 3;  /* 3 seconds */
        float *buf = (float*)calloc(nf, sizeof(float));
        wb_session_add_audio_clip(t1, 44100.0, (double)nf, buf, nf, 2);
        free(buf);
    }
    return s;
}

/* Read a file into memory. Returns malloc'd buffer, sets *out_len. */
static char *read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char*)malloc(len + 1);
    if (buf) {
        fread(buf, 1, len, f);
        buf[len] = '\0';
        if (out_len) *out_len = len;
    }
    fclose(f);
    return buf;
}

int main(void) {
    printf("=== AAF/OMF interchange export ===\n\n");

    const char *aaf_path = "/tmp/test_aaf_export.aaf";
    const char *omf_path = "/tmp/test_aaf_export.omf";
    remove(aaf_path);
    remove(omf_path);

    /* Build session */
    wb_session *s = make_2track_session();
    CK(s != NULL, "2-track session created");
    if (!s) goto done;
    CK(s->track_count == 2, "session has 2 tracks");

    /* 1. Export to AAF — file exists, non-empty */
    int rc_aaf = wb_aaf_export(s, aaf_path);
    CK(rc_aaf == 0, "wb_aaf_export returns 0");
    {
        long len = 0;
        char *content = read_file(aaf_path, &len);
        CK(content != NULL && len > 0, "AAF file exists, non-empty");
        free(content);
    }

    /* 2. Export to OMF — file exists, non-empty */
    int rc_omf = wb_omf_export(s, omf_path);
    CK(rc_omf == 0, "wb_omf_export returns 0");
    {
        long len = 0;
        char *content = read_file(omf_path, &len);
        CK(content != NULL && len > 0, "OMF file exists, non-empty");
        free(content);
    }

    /* 3. AAF file contains session name */
    {
        long len = 0;
        char *content = read_file(aaf_path, &len);
        CK(content != NULL && strstr(content, "TestSession_AAF") != NULL,
           "AAF file contains session name");
        free(content);
    }

    /* 4. AAF file contains track names */
    {
        long len = 0;
        char *content = read_file(aaf_path, &len);
        int has_drums = (content && strstr(content, "Drums") != NULL);
        int has_bass  = (content && strstr(content, "Bass") != NULL);
        CK(has_drums, "AAF file contains track name 'Drums'");
        CK(has_bass,  "AAF file contains track name 'Bass'");
        free(content);
    }

    /* 5. OMF file contains valid header bytes ("OMFI" magic) */
    {
        FILE *f = fopen(omf_path, "rb");
        CK(f != NULL, "OMF file openable for binary read");
        if (f) {
            char magic[5] = {0};
            size_t n = fread(magic, 1, 4, f);
            CK(n == 4 && memcmp(magic, "OMFI", 4) == 0,
               "OMF file starts with 'OMFI' magic");
            /* verify version = 2 (OMF2) */
            uint16_t ver = 0;
            if (fread(&ver, 2, 1, f) == 1) {
                CK(ver == 2, "OMF version == 2 (OMF2)");
            } else {
                CK(0, "OMF version readable");
            }
            fclose(f);
        }
    }

    /* 6. Export with NULL session returns error */
    {
        int rc = wb_aaf_export(NULL, aaf_path);
        CK(rc != 0, "wb_aaf_export(NULL session) returns error");
        const char *err = wb_aaf_last_error();
        CK(err != NULL && err[0] != '\0', "wb_aaf_last_error() set on failure");
    }
    {
        int rc = wb_omf_export(NULL, omf_path);
        CK(rc != 0, "wb_omf_export(NULL session) returns error");
    }

    /* 7. Export with 0 tracks returns error */
    {
        wb_session *empty = wb_session_create();
        CK(empty != NULL, "empty session created");
        if (empty) {
            int rc = wb_aaf_export(empty, aaf_path);
            CK(rc != 0, "wb_aaf_export(0 tracks) returns error");
            rc = wb_omf_export(empty, omf_path);
            CK(rc != 0, "wb_omf_export(0 tracks) returns error");
            wb_session_destroy(empty);
        } else {
            CK(0, "empty session for 0-track test");
        }
    }

    /* 8. Round-trip: export then verify file is parseable (basic structure check) */
    {
        long len = 0;
        char *content = read_file(aaf_path, &len);
        if (content) {
            int has_xml_decl   = (strstr(content, "<?xml") != NULL);
            int has_aaf_open   = (strstr(content, "<AAF") != NULL);
            int has_aaf_close  = (strstr(content, "</AAF>") != NULL);
            int has_header     = (strstr(content, "<Header>") != NULL);
            int has_tracks     = (strstr(content, "<Track ") != NULL);
            int has_clips      = (strstr(content, "<Clip ") != NULL);
            int has_edl        = (strstr(content, "CMX3600") != NULL);
            CK(has_xml_decl,  "AAF: has XML declaration");
            CK(has_aaf_open,  "AAF: has <AAF> root element");
            CK(has_aaf_close, "AAF: has closing </AAF>");
            CK(has_header,    "AAF: has <Header>");
            CK(has_tracks,    "AAF: has <Track> elements");
            CK(has_clips,     "AAF: has <Clip> elements");
            CK(has_edl,       "AAF: has CMX3600 EDL section");
        } else {
            CK(0, "AAF: file readable for structure check");
        }
        free(content);
    }
    {
        /* OMF structure: verify we can read header fields */
        FILE *f = fopen(omf_path, "rb");
        if (f) {
            char magic[5] = {0};
            uint16_t ver = 0, ver_min = 0;
            uint32_t obj_count = 0, sr = 0, tc = 0;
            double len = 0;
            int ok = (fread(magic, 1, 4, f) == 4) &&
                     (fread(&ver, 2, 1, f) == 1) &&
                     (fread(&ver_min, 2, 1, f) == 1) &&
                     (fread(&obj_count, 4, 1, f) == 1) &&
                     (fread(&sr, 4, 1, f) == 1) &&
                     (fread(&len, 8, 1, f) == 1) &&
                     (fread(&tc, 4, 1, f) == 1);
            fclose(f);
            CK(ok, "OMF: all header fields readable");
            CK(obj_count > 0, "OMF: object count > 0");
            CK(sr == WB_SAMPLE_RATE, "OMF: sample rate == 44100");
            CK(tc == 2, "OMF: track count == 2");
        } else {
            CK(0, "OMF: file openable for structure check");
        }
    }

done:
    if (s) wb_session_destroy(s);
    remove(aaf_path);
    remove(omf_path);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}