/* test_subtitle_translate.c — R079 gate test for the subtitle translation engine.
 *
 * Tests:
 *   1. Init with session
 *   2. Translate to Spanish
 *   3. Translate to Japanese
 *   4. Get language count > 10
 *   5. Get language name
 *   6. Output contains translated text
 *   7. Bad language code rejects
 *   8. English round-trip (default language)
 *   9. Full output buffer (capacity respected)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "wbus/wbus.h"

/* Forward-declare the session lifecycle (opaque type, only public API used). */
/* wb_session_create / wb_session_destroy are declared in wbus.h. */

static int checks = 0, failures = 0;

#define CHECK(cond, msg) do {                                          \
    checks++;                                                         \
    if (cond) {                                                       \
        printf("  [PASS] %s\n", msg);                                 \
    } else {                                                          \
        printf("  [FAIL] %s\n", msg);                                 \
        failures++;                                                   \
    }                                                                 \
} while (0)

int main(void) {
    printf("=== Subtitle translation test ===\n\n");

    /* ---- Test 1: Init with session ---- */
    {
        wb_session *s = wb_session_create();
        wb_subtitle_translate st;
        int rc = wb_subtitle_translate_init(&st, s);
        CHECK(rc == 0, "1: init returns 0 with valid session");
        CHECK(st.processed == 0, "1: processed flag is 0 after init");
        CHECK(st.lang_index == 0, "1: default language is English (index 0)");
        wb_subtitle_translate_process(&st);  /* populate output */
        wb_session_destroy(s);
    }

    /* ---- Test 2: Translate to Spanish ---- */
    {
        wb_session *s = wb_session_create();
        wb_subtitle_translate st;
        wb_subtitle_translate_init(&st, s);
        int rc = wb_subtitle_translate_language(&st, "es");
        CHECK(rc == 0, "2: language set to 'es' returns 0");
        wb_subtitle_translate_process(&st);

        char buf[8192];
        wb_subtitle_translate_get_output(&st, buf, sizeof(buf));

        /* Verify Spanish translations appear. */
        int has_hola      = (strstr(buf, "Hola")      != NULL);
        int has_gracias   = (strstr(buf, "Gracias")   != NULL);
        int has_adios     = (strstr(buf, "Adiós")     != NULL);
        int has_buenos    = (strstr(buf, "Buenos")    != NULL);

        CHECK(has_hola,    "2: Spanish 'Hola' in output");
        CHECK(has_gracias, "2: Spanish 'Gracias' in output");
        CHECK(has_adios,   "2: Spanish 'Adiós' in output");
        CHECK(has_buenos,  "2: Spanish 'Buenos días' in output");

        wb_session_destroy(s);
    }

    /* ---- Test 3: Translate to Japanese ---- */
    {
        wb_session *s = wb_session_create();
        wb_subtitle_translate st;
        wb_subtitle_translate_init(&st, s);
        int rc = wb_subtitle_translate_language(&st, "ja");
        CHECK(rc == 0, "3: language set to 'ja' returns 0");
        wb_subtitle_translate_process(&st);

        char buf[8192];
        wb_subtitle_translate_get_output(&st, buf, sizeof(buf));

        /* 'こんにちは' = konnichiwa (hello), 'ありがとう' = arigatou (thanks) */
        CHECK(strlen(buf) > 0, "3: Japanese output is non-empty");
        /* Check for Japanese characters (multi-byte UTF-8). */
        int has_japanese = 0;
        for (const char *p = buf; *p; p++) {
            if ((unsigned char)*p >= 0xE0) {  /* 3-byte UTF-8 lead */
                has_japanese = 1;
                break;
            }
        }
        CHECK(has_japanese, "3: output contains Japanese UTF-8 characters");

        wb_session_destroy(s);
    }

    /* ---- Test 4: Get language count > 10 ---- */
    {
        int count = wb_subtitle_translate_get_language_count();
        printf("  language count = %d\n", count);
        CHECK(count > 10, "4: language count > 10");
    }

    /* ---- Test 5: Get language name ---- */
    {
        const char *name0 = wb_subtitle_translate_get_language_name(0);
        const char *name1 = wb_subtitle_translate_get_language_name(1);
        const char *name_bad = wb_subtitle_translate_get_language_name(-1);

        CHECK(name0 != NULL && strcmp(name0, "English") == 0,
              "5: name[0] = 'English'");
        CHECK(name1 != NULL && strcmp(name1, "Spanish") == 0,
              "5: name[1] = 'Spanish'");
        CHECK(name_bad == NULL, "5: out-of-range returns NULL");
        /* Also verify last language. */
        int count = wb_subtitle_translate_get_language_count();
        const char *name_last = wb_subtitle_translate_get_language_name(count - 1);
        CHECK(name_last != NULL, "5: last language name is non-NULL");
    }

    /* ---- Test 6: Output contains translated text ---- */
    {
        wb_session *s = wb_session_create();
        wb_subtitle_translate st;
        wb_subtitle_translate_init(&st, s);
        wb_subtitle_translate_language(&st, "fr");
        wb_subtitle_translate_process(&st);

        char buf[8192];
        wb_subtitle_translate_get_output(&st, buf, sizeof(buf));
        CHECK(strlen(buf) > 0, "6: output is non-empty after process");
        CHECK(strstr(buf, "Bonjour") != NULL,
              "6: French 'Bonjour' (Hello) in output");
        CHECK(strstr(buf, "Merci") != NULL,
              "6: French 'Merci' (Thank you) in output");
        wb_session_destroy(s);
    }

    /* ---- Test 7: Bad language code is rejected ---- */
    {
        wb_session *s = wb_session_create();
        wb_subtitle_translate st;
        wb_subtitle_translate_init(&st, s);
        int rc = wb_subtitle_translate_language(&st, "xx");
        CHECK(rc == -1, "7: unknown language code returns -1");
        CHECK(st.lang_index == 0, "7: language index unchanged on bad code");
        wb_session_destroy(s);
    }

    /* ---- Test 8: English round-trip (default language) ---- */
    {
        wb_session *s = wb_session_create();
        wb_subtitle_translate st;
        wb_subtitle_translate_init(&st, s);
        wb_subtitle_translate_process(&st);
        char buf[8192];
        wb_subtitle_translate_get_output(&st, buf, sizeof(buf));
        CHECK(strstr(buf, "Hello") != NULL,
              "8: English 'Hello' preserved in default output");
        CHECK(strstr(buf, "Thank you") != NULL,
              "8: English 'Thank you' preserved in default output");
        CHECK(strstr(buf, "Congratulations") != NULL,
              "8: English 'Congratulations' in default output");
        wb_session_destroy(s);
    }

    /* ---- Test 9: Output buffer capacity is respected ---- */
    {
        wb_session *s = wb_session_create();
        wb_subtitle_translate st;
        wb_subtitle_translate_init(&st, s);
        wb_subtitle_translate_language(&st, "de");
        wb_subtitle_translate_process(&st);

        char small[8];
        int rc = wb_subtitle_translate_get_output(&st, small, sizeof(small));
        CHECK(rc == 0, "9: get_output returns 0 even with small buffer");
        /* Output should be truncated but NUL-terminated. */
        CHECK(strlen(small) < sizeof(small),
              "9: output fits in small buffer (truncated + NUL)");
        CHECK(small[sizeof(small) - 1] == '\0',
              "9: output is NUL-terminated within capacity");
        wb_session_destroy(s);
    }

    printf("\n=== %d/%d checks passed ===\n", checks - failures, checks);
    return failures > 0 ? 1 : 0;
}
