/* wb_subtitle_translate.c — R079: multi-language subtitle translation engine.
 *
 * Dictionary-based caption translator (AI-subtitle-generator style). Scans a
 * transcript of common subtitle phrases, matches words/phrases against an
 * in-memory lookup table, and replaces them with translations in the selected
 * target language. Timing is preserved (words keep their [start, end] spans);
 * only the displayed word text is replaced. No ML model — pure lookup table.
 *
 * Supports 28 languages via ISO 639-1 codes. 18 common subtitle phrases are
 * fully localized across all languages. Phrase matching handles multi-word
 * phrases by joining consecutive normalized words.
 */

#include "wbus/wbus.h"
#include "wbus/wbus_transcript.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* ---- language table ------------------------------------------------------ */

#define WB_LANG_COUNT 28

typedef struct {
    const char *code;   /* ISO 639-1, e.g. "es" */
    const char *name;   /* English name, e.g. "Spanish" */
} wb_lang_entry;

static const wb_lang_entry wb_langs[WB_LANG_COUNT] = {
    { "en", "English"     },
    { "es", "Spanish"     },
    { "fr", "French"      },
    { "de", "German"      },
    { "it", "Italian"     },
    { "pt", "Portuguese"  },
    { "ru", "Russian"     },
    { "ja", "Japanese"    },
    { "ko", "Korean"      },
    { "zh", "Chinese"     },
    { "ar", "Arabic"      },
    { "hi", "Hindi"       },
    { "nl", "Dutch"       },
    { "pl", "Polish"      },
    { "tr", "Turkish"     },
    { "sv", "Swedish"     },
    { "da", "Danish"      },
    { "fi", "Finnish"     },
    { "no", "Norwegian"   },
    { "el", "Greek"       },
    { "th", "Thai"        },
    { "vi", "Vietnamese"  },
    { "id", "Indonesian"  },
    { "ms", "Malay"       },
    { "uk", "Ukrainian"   },
    { "cs", "Czech"       },
    { "ro", "Romanian"    },
    { "hu", "Hungarian"   },
};

/* ---- phrase dictionary --------------------------------------------------- */
/*
 * 18 common subtitle phrases. The dictionary key is the normalized English
 * phrase (lowercase, no punctuation). Each language provides a translation
 * for all 18 phrases, indexed by position in this array.
 *
 * The English source (index 0) mirrors the key but with original casing.
 */

#define WB_PHRASE_COUNT 18

static const char *wb_phrases[WB_PHRASE_COUNT] = {
    "hello",         /* 0 */
    "thank you",     /* 1 */
    "yes",           /* 2 */
    "no",            /* 3 */
    "goodbye",       /* 4 */
    "please",        /* 5 */
    "sorry",         /* 6 */
    "i love you",    /* 7 */
    "what",          /* 8 */
    "where",         /* 9 */
    "how",           /* 10 */
    "why",           /* 11 */
    "when",          /* 12 */
    "who",           /* 13 */
    "good morning",  /* 14 */
    "good night",    /* 15 */
    "welcome",       /* 16 */
    "congratulations", /* 17 */
};

/* Maximum word count in any phrase key. */
#define WB_MAX_PHRASE_WORDS 3

/*
 * Translation table: dict[lang_index][phrase_index].
 * All strings are static constants — no allocation needed.
 */
static const char *const wb_dict[WB_LANG_COUNT][WB_PHRASE_COUNT] = {
    /* 0: English */
    { "Hello", "Thank you", "Yes", "No", "Goodbye", "Please", "Sorry",
      "I love you", "What?", "Where?", "How?", "Why?", "When?", "Who?",
      "Good morning", "Good night", "Welcome", "Congratulations" },

    /* 1: Spanish */
    { "Hola", "Gracias", "Sí", "No", "Adiós", "Por favor", "Lo siento",
      "Te amo", "¿Qué?", "¿Dónde?", "¿Cómo?", "¿Por qué?", "¿Cuándo?",
      "¿Quién?", "Buenos días", "Buenas noches", "Bienvenido", "¡Felicidades" },

    /* 2: French */
    { "Bonjour", "Merci", "Oui", "Non", "Au revoir", "S'il vous plaît",
      "Désolé", "Je t'aime", "Quoi ?", "Où ?", "Comment ?", "Pourquoi ?",
      "Quand ?", "Qui ?", "Bonjour", "Bonne nuit", "Bienvenue", "Félicitations" },

    /* 3: German */
    { "Hallo", "Danke", "Ja", "Nein", "Auf Wiedersehen", "Bitte", "Es tut mir leid",
      "Ich liebe dich", "Was?", "Wo?", "Wie?", "Warum?", "Wann?", "Wer?",
      "Guten Morgen", "Gute Nacht", "Willkommen", "Herzlichen Glückwunsch" },

    /* 4: Italian */
    { "Ciao", "Grazie", "Sì", "No", "Arrivederci", "Per favore", "Mi dispiace",
      "Ti amo", "Che cosa?", "Dove?", "Come?", "Perché?", "Quando?", "Chi?",
      "Buongiorno", "Buona notte", "Benvenuto", "Congratulazioni" },

    /* 5: Portuguese */
    { "Olá", "Obrigado", "Sim", "Não", "Adeus", "Por favor", "Desculpe",
      "Eu te amo", "O quê?", "Onde?", "Como?", "Por quê?", "Quando?",
      "Quem?", "Bom dia", "Boa noite", "Bem-vindo", "Parabéns" },

    /* 6: Russian */
    { "Привет", "Спасибо", "Да", "Нет", "До свидания", "Пожалуйста", "Извините",
      "Я люблю тебя", "Что?", "Где?", "Как?", "Почему?", "Когда?", "Кто?",
      "Доброе утро", "Спокойной ночи", "Добро пожаловать", "Поздравляю" },

    /* 7: Japanese */
    { "こんにちは", "ありがとう", "はい", "いいえ", "さようなら", "おねがいします",
      "すみません", "あなたを愛している", "なに？", "どこ？", "どのように？",
      "なぜ？", "いつ？", "だれ？", "おはようございます", "おやすみなさい",
      "ようこそ", "おめでとうございます" },

    /* 8: Korean */
    { "안녕하세요", "감사합니다", "예", "아니요", "안녕히 가세요", "부탁합니다",
      "죄송합니다", "사랑해요", "무엇?", "어디?", "어떻게?", "왜?", "언제?",
      "누구?", "좋은 아침", "안녕히 자세요", "환영합니다", "축하합니다" },

    /* 9: Chinese */
    { "你好", "谢谢", "是", "不", "再见", "请", "对不起",
      "我爱你", "什么？", "哪里？", "怎么？", "为什么？", "什么时候？",
      "谁？", "早上好", "晚安", "欢迎", "恭喜" },

    /* 10: Arabic */
    { "مرحبا", "شكرا", "نعم", "لا", "مع السلامة", "من فضلك", "عذرا",
      "أحبك", "ماذا؟", "أين؟", "كيف؟", "لماذا؟", "متى؟", "من؟",
      "صباح الخير", "تصبح على خير", "مرحبا", "مبروك" },

    /* 11: Hindi */
    { "नमस्ते", "धन्यवाद", "हाँ", "नहीं", "अलविदा", "कृपया", "मुझे खेद है",
      "मैं तुमसे प्यार करता हूँ", "क्या?", "कहाँ?", "कैसे?", "क्यों?", "कब?",
      "कौन?", "शुभ प्रभात", "शुभ रात्रि", "स्वागत है", "बधाई हो" },

    /* 12: Dutch */
    { "Hallo", "Dank je", "Ja", "Nee", "Tot ziens", "Alstublieft", "Pardon",
      "Ik hou van je", "Wat?", "Waar?", "Hoe?", "Waarom?", "Wanneer?",
      "Wie?", "Goedemorgen", "Goedenacht", "Welkom", "Gefeliciteerd" },

    /* 13: Polish */
    { "Cześć", "Dziękuję", "Tak", "Nie", "Do widzenia", "Proszę", "Przepraszam",
      "Kocham cię", "Co?", "Gdzie?", "Jak?", "Dlaczego?", "Kiedy?", "Kto?",
      "Dzień dobry", "Dobra noc", "Witaj", "Gratulacje" },

    /* 14: Turkish */
    { "Merhaba", "Teşekkürler", "Evet", "Hayır", "Güle güle", "Lütfen",
      "Özür dilerim", "Seni seviyorum", "Ne?", "Nerede?", "Nasıl?",
      "Neden?", "Ne zaman?", "Kim?", "Günaydın", "İyi uykular",
      "Hoş geldiniz", "Tebrikler" },

    /* 15: Swedish */
    { "Hej", "Tack", "Ja", "Nej", "Farvel", "Var god", "Förlåt",
      "Jag älskar dig", "Vad?", "Var?", "Hur?", "Varför?", "När?", "Vem?",
      "God morgon", "God natt", "Välkommen", "Grattis" },

    /* 16: Danish */
    { "Hej", "Tak", "Ja", "Nej", "Farvel", "Vennligst", "Beklager",
      "Jeg elsker dig", "Hvad?", "Hvor?", "Hvordan?", "Hvorfor?", "Når?",
      " hvem?", "God morgen", "Godnat", "Velkommen", "Tillykke" },

    /* 17: Finnish */
    { "Hei", "Kiitos", "Kyllä", "Ei", "Näkemiin", "Ole hyvä", "Anteeksi",
      "Rakastan sinua", "Mikä?", "Missä?", "Miten?", "Miksi?", "Milloin?",
      "Kuka?", "Hyvää huomenta", "Hyvää yötä", "Tervetuloa", "Onnittelut" },

    /* 18: Norwegian */
    { "Hei", "Takk", "Ja", "Nei", "Farvel", "Vennligst", "Beklager",
      "Jeg elsker deg", "Hva?", "Hvor?", "Hvordan?", "Hvorfor?", "Når?",
      "Hvem?", "God morgen", "God natt", "Velkommen", "Gratulerer" },

    /* 19: Greek */
    { "Γεια σου", "Ευχαριστώ", "Ναι", "Όχι", "Αντίο", "Παρακαλώ", "Συγγνώμη",
      "Σ' αγαπώ", "Τι;", "Πού;", "Πώς;", "Γιατί;", "Πότε;", "Ποιος;",
      "Καλημέρα", "Καληνύχτα", "Καλώς ήρθατε", "Συγχαρητήρια" },

    /* 20: Thai */
    { "สวัสดี", "ขอบคุณ", "ใช่", "ไม่", "บ๊าย", "กรุณา", "ขอโทษ",
      "ฉันรักคุณ", "อะไร?", "ที่ไหน?", "อย่างไร?", "ทำไม?", "เมื่อไหร่?",
      "ใคร?", "สวัสดีตอนเช้า", "ราบลมคืนดี", "ยินดีต้อนรับ", "ขอใหม่" },

    /* 21: Vietnamese */
    { "Xin chào", "Cảm ơn", "Vâng", "Không", "Tạm biệt", "Làm ơn", "Xin lỗi",
      "Anh yêu em", "Cái gì?", "Ở đâu?", "Thế nào?", "Tại sao?", "Khi nào?",
      "Ai?", "Chào buổi sáng", "Chúc ngon mơ", "Chào mừng", "Chúc mừng" },

    /* 22: Indonesian */
    { "Halo", "Terima kasih", "Ya", "Tidak", "Selamat tinggal", "Silakan",
      "Maaf", "Aku mencintaimu", "Apa?", "Di mana?", "Bagaimana?", "Mengapa?",
      "Kapan?", "Siapa?", "Selamat pagi", "Selamat malam", "Selamat datang",
      "Selamat" },

    /* 23: Malay */
    { "Halo", "Terima kasih", "Ya", "Tidak", "Selamat tinggal", "Sila",
      "Maaf", "Saya mencintaimu", "Apakah?", "Di mana?", "Bagaimana?",
      "Mengapa?", "Bila?", "Siapa?", "Selamat pagi", "Selamat malam",
      "Selamat datang", "Tahniah" },

    /* 24: Ukrainian */
    { "Привіт", "Дякую", "Так", "Ні", "До побачення", "Будь ласка",
      "Вибачте", "Я тебя кохаю", "Що?", "Де?", "Як?", "Чому?", "Коли?",
      "Хто?", "Доброго ранку", "Спокійної ночі", "Ласкависто запрошую",
      "Вітаю" },

    /* 25: Czech */
    { "Ahoj", "Děkuji", "Ano", "Ne", "Na shledanou", "Prosím", "Promiňte",
      "Miluji tě", "Cože?", "Kde?", "Jak?", "Proč?", "Kdy?", "Kdo?",
      "Dobrý den", "Dobrou noc", "Vítejte", "Gratuluji" },

    /* 26: Romanian */
    { "Salut", "Mulțumesc", "Da", "Nu", "La revedere", "Vă rog", "Îmi pare rău",
      "Te iubesc", "Ce?", "Unde?", "Cum?", "De ce?", "Când?", "Cine?",
      "Buna dimineața", "Noapte bună", "Bine ai venit", "Felicitări" },

    /* 27: Hungarian */
    { "Szia", "Köszönöm", "Igen", "Nem", "Viszontlátón", "Kérem",
      "Elnézést", "Szeretlek", "Mi?", "Hol?", "Hogyan?", "Miért?",
      "Mikor?", "Ki?", "Jó reggelt", "Jó éjszakát", "Üdvözöljük",
      "Gratulálok" },
};

/* ---- helpers ------------------------------------------------------------- */

/* Normalize a single word: lowercase + strip leading/trailing punctuation.
 * Writes into `dst` (capacity `cap`), NUL-terminated. */
static void wb_normalize_word(char *dst, int cap, const char *src) {
    if (!dst || cap <= 0 || !src) { if (dst && cap > 0) dst[0] = '\0'; return; }
    /* skip leading punctuation/space */
    while (*src && (ispunct((unsigned char)*src) || isspace((unsigned char)*src)))
        src++;
    char *p = dst;
    char *end = dst + cap - 1;
    while (*src && p < end) {
        if (ispunct((unsigned char)*src) || isspace((unsigned char)*src))
            break;
        *p++ = (char)tolower((unsigned char)*src);
        src++;
    }
    /* strip trailing punctuation */
    while (p > dst && ispunct((unsigned char)*(p - 1)))
        p--;
    *p = '\0';
}

/* Find the language index for a given ISO 639-1 code. Returns -1 if not found. */
static int wb_lang_index(const char *code) {
    if (!code) return -1;
    for (int i = 0; i < WB_LANG_COUNT; i++) {
        if (strcmp(wb_langs[i].code, code) == 0)
            return i;
    }
    return -1;
}

/* Look up a phrase key in the dictionary; returns the phrase index or -1. */
static int wb_phrase_lookup(const char *key) {
    if (!key) return -1;
    for (int i = 0; i < WB_PHRASE_COUNT; i++) {
        if (strcmp(wb_phrases[i], key) == 0)
            return i;
    }
    return -1;
}

/* Maximum number of normalized words we need to buffer for phrase matching. */
#define WB_MAX_WORDS_PER_PHRASE WB_MAX_PHRASE_WORDS

/* Populate the internal transcript with the 18 common subtitle phrases,
 * each split into individual words with 1-second timing slots. */
static void wb_populate_default_transcript(wb_transcript *t) {
    double t_pos = 0.0;
    for (int p = 0; p < WB_PHRASE_COUNT; p++) {
        const char *src = wb_phrases[p];  /* e.g. "thank you" */
        const char *cursor = src;
        while (*cursor) {
            /* skip spaces */
            while (*cursor == ' ') cursor++;
            if (*cursor == '\0') break;
            /* find word boundary */
            const char *wstart = cursor;
            while (*cursor && *cursor != ' ') cursor++;
            int wlen = (int)(cursor - wstart);
            if (wlen > 63) wlen = 63;
            char wordbuf[64];
            memcpy(wordbuf, wstart, (size_t)wlen);
            wordbuf[wlen] = '\0';
            double end_t = t_pos + 1000.0;  /* 1 second per word */
            wb_transcript_add(t, t_pos, end_t, wordbuf);
            t_pos = end_t;
        }
        /* 500ms gap between phrases */
        t_pos += 500.0;
    }
}

/* ---- public API ---------------------------------------------------------- */

int wb_subtitle_translate_init(wb_subtitle_translate *st, wb_session *session) {
    if (!st) return -1;
    memset(st, 0, sizeof(*st));
    st->session = session;
    st->lang_index = 0;  /* default: English */
    st->transcript = wb_transcript_create();
    if (!st->transcript) return -1;
    wb_populate_default_transcript(st->transcript);
    st->output[0] = '\0';
    st->processed = 0;
    return 0;
}

int wb_subtitle_translate_language(wb_subtitle_translate *st, const char *lang_code) {
    if (!st) return -1;
    int idx = wb_lang_index(lang_code);
    if (idx < 0) return -1;
    st->lang_index = idx;
    st->processed = 0;  /* invalidate previous output */
    return 0;
}

int wb_subtitle_translate_process(wb_subtitle_translate *st) {
    if (!st || !st->transcript) return -1;
    int n = wb_transcript_count(st->transcript);
    if (n == 0) {
        st->output[0] = '\0';
        st->processed = 1;
        return 0;
    }

    char *out = st->output;
    int out_cap = (int)sizeof(st->output);
    int out_pos = 0;

    int i = 0;
    while (i < n) {
        /* Try to match the longest phrase starting at position i. */
        int matched = 0;

        for (int try_len = WB_MAX_PHRASE_WORDS; try_len >= 1; try_len--) {
            if (i + try_len > n) continue;

            /* Build normalized key from words[i..i+try_len-1]. */
            char key[128];
            key[0] = '\0';
            for (int k = 0; k < try_len; k++) {
                char norm[64];
                wb_normalize_word(norm, sizeof(norm),
                                  wb_transcript_word(st->transcript, i + k)->word);
                if (k > 0) {
                    if ((int)strlen(key) + 1 >= (int)sizeof(key)) break;
                    strncat(key, " ", 1);
                }
                if ((int)strlen(key) + (int)strlen(norm) >= (int)sizeof(key)) break;
                strcat(key, norm);
            }

            int pidx = wb_phrase_lookup(key);
            if (pidx >= 0 && try_len > 1) {
                /* Multi-word phrase match: translate and advance. */
                const char *trans = wb_dict[st->lang_index][pidx];
                if (out_pos > 0 && out_pos + 1 < out_cap) {
                    out[out_pos++] = ' ';
                }
                int wlen = (int)strlen(trans);
                if (out_pos + wlen >= out_cap) wlen = out_cap - 1 - out_pos;
                if (wlen > 0) {
                    memcpy(out + out_pos, trans, (size_t)wlen);
                    out_pos += wlen;
                }
                /* Update transcript: first word gets the full translation,
                 * remaining words are cleared (timing preserved via spans). */
                wb_transcript_set_word(st->transcript, i, trans);
                for (int k = 1; k < try_len; k++) {
                    wb_transcript_set_word(st->transcript, i + k, "");
                }
                i += try_len;
                matched = 1;
                break;
            }
            if (pidx >= 0 && try_len == 1) {
                /* Single-word match. */
                const char *trans = wb_dict[st->lang_index][pidx];
                if (out_pos > 0 && out_pos + 1 < out_cap) {
                    out[out_pos++] = ' ';
                }
                int wlen = (int)strlen(trans);
                if (out_pos + wlen >= out_cap) wlen = out_cap - 1 - out_pos;
                if (wlen > 0) {
                    memcpy(out + out_pos, trans, (size_t)wlen);
                    out_pos += wlen;
                }
                wb_transcript_set_word(st->transcript, i, trans);
                i++;
                matched = 1;
                break;
            }
        }

        if (!matched) {
            /* No dictionary match: keep the original word. */
            const wb_word *w = wb_transcript_word(st->transcript, i);
            if (w && w->word) {
                if (out_pos > 0 && out_pos + 1 < out_cap) {
                    out[out_pos++] = ' ';
                }
                int wlen = (int)strlen(w->word);
                if (out_pos + wlen >= out_cap) wlen = out_cap - 1 - out_pos;
                if (wlen > 0) {
                    memcpy(out + out_pos, w->word, (size_t)wlen);
                    out_pos += wlen;
                }
            }
            i++;
        }
    }

    out[out_pos] = '\0';
    st->processed = 1;
    return 0;
}

int wb_subtitle_translate_get_output(const wb_subtitle_translate *st,
                                     char *out, int cap) {
    if (!st || !out || cap <= 0) return -1;
    size_t n = strlen(st->output);
    size_t limit = (size_t)cap - 1;
    if (n > limit) n = limit;
    memcpy(out, st->output, n);
    out[n] = '\0';
    return 0;
}

int wb_subtitle_translate_get_language_count(void) {
    return WB_LANG_COUNT;
}

const char *wb_subtitle_translate_get_language_name(int index) {
    if (index < 0 || index >= WB_LANG_COUNT) return NULL;
    return wb_langs[index].name;
}
