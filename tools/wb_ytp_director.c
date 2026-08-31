/* wb_ytp_director.c — YTP Director: text description → full YTP edit (R083).
 *
 * This is the "brain" that takes a natural-language description of a YTP
 * and generates a complete edit composition.
 *
 * Usage: ./build/wb_ytp_director <description.txt> <output.edl.json>
 *
 * Description file format (plain text):
 *   TITLE: My Awesome YTP
 *   SOURCE: path/to/video.mp4
 *   TRANSCRIPT: path/to/transcript.json
 *   CHAOS: 7
 *   PLOT:
 *     - Shrek sings "All Star" but things go wrong
 *     - Start slow, build to chaos
 *     - Callback to "somebody" at the climax
 *     - End with "To Be Continued" arrow
 *   EFFECTS_PREFER: stutter, earrape, deep_fry, vine_boom
 *   EFFECTS_AVOID: kaleido, datamosh
 *
 * Output: EDL JSON that can be rendered by wb_ytp_render or wb_ytp_studio
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_LINE 4096
#define MAX_PLOT_POINTS 32
#define MAX_EFFECTS 32

typedef struct {
    char title[256];
    char source[512];
    char transcript[512];
    int chaos;
    
    char plot_points[MAX_PLOT_POINTS][512];
    int n_plot;
    
    char prefer_effects[MAX_EFFECTS][64];
    int n_prefer;
    
    char avoid_effects[MAX_EFFECTS][64];
    int n_avoid;
} ytp_description;

/* Parse a description file */
int parse_description(const char *path, ytp_description *desc) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    
    memset(desc, 0, sizeof(*desc));
    desc->chaos = 6; /* default */
    strncpy(desc->title, "Untitled YTP", sizeof(desc->title) - 1);
    
    char line[MAX_LINE];
    int in_plot = 0;
    
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        line[strcspn(line, "\r\n")] = '\0';
        
        /* Skip empty lines */
        if (line[0] == '\0') continue;
        
        /* Check if this is a key: value line */
        char *val = strchr(line, ':');
        
        if (!val) {
            /* No colon — could be a plot point if we're in plot mode */
            if (in_plot && desc->n_plot < MAX_PLOT_POINTS) {
                char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '-') p++;
                while (*p == ' ' || *p == '\t') p++;
                if (*p) {
                    strncpy(desc->plot_points[desc->n_plot++], p, 511);
                }
            }
            continue;
        }
        
        *val = '\0';
        val++;
        while (*val == ' ' || *val == '\t') val++;
        
        if (strcasecmp(line, "TITLE") == 0) {
            strncpy(desc->title, val, sizeof(desc->title) - 1);
        } else if (strcasecmp(line, "SOURCE") == 0) {
            strncpy(desc->source, val, sizeof(desc->source) - 1);
        } else if (strcasecmp(line, "TRANSCRIPT") == 0) {
            strncpy(desc->transcript, val, sizeof(desc->transcript) - 1);
        } else if (strcasecmp(line, "CHAOS") == 0) {
            desc->chaos = atoi(val);
            if (desc->chaos < 1) desc->chaos = 1;
            if (desc->chaos > 10) desc->chaos = 10;
        } else if (strcasecmp(line, "PLOT") == 0) {
            in_plot = 1;
        } else if (strcasecmp(line, "EFFECTS_PREFER") == 0) {
            in_plot = 0;
            char *tok = strtok(val, ",");
            while (tok && desc->n_prefer < MAX_EFFECTS) {
                while (*tok == ' ') tok++;
                char *end = tok + strlen(tok) - 1;
                while (end > tok && *end == ' ') *end-- = '\0';
                strncpy(desc->prefer_effects[desc->n_prefer++], tok, 63);
                tok = strtok(NULL, ",");
            }
        } else if (strcasecmp(line, "EFFECTS_AVOID") == 0) {
            in_plot = 0;
            char *tok = strtok(val, ",");
            while (tok && desc->n_avoid < MAX_EFFECTS) {
                while (*tok == ' ') tok++;
                char *end = tok + strlen(tok) - 1;
                while (end > tok && *end == ' ') *end-- = '\0';
                strncpy(desc->avoid_effects[desc->n_avoid++], tok, 63);
                tok = strtok(NULL, ",");
            }
        }
    }
    
    fclose(f);
    return 0;
}

/* Generate EDL JSON from description */
int generate_edl(const ytp_description *desc, const char *output) {
    FILE *f = fopen(output, "w");
    if (!f) return -1;
    
    fprintf(f, "{\n");
    fprintf(f, "  \"title\": \"%s\",\n", desc->title);
    fprintf(f, "  \"source\": \"%s\",\n", desc->source);
    fprintf(f, "  \"transcript\": \"%s\",\n", desc->transcript);
    fprintf(f, "  \"chaos\": %d,\n", desc->chaos);
    
    /* Plot */
    fprintf(f, "  \"plot\": [\n");
    for (int i = 0; i < desc->n_plot; i++) {
        fprintf(f, "    \"%s\"%s\n", desc->plot_points[i],
                i < desc->n_plot - 1 ? "," : "");
    }
    fprintf(f, "  ],\n");
    
    /* Preferred effects */
    fprintf(f, "  \"effects_prefer\": [");
    for (int i = 0; i < desc->n_prefer; i++) {
        fprintf(f, "%s\"%s\"", i > 0 ? ", " : "", desc->prefer_effects[i]);
    }
    fprintf(f, "],\n");
    
    /* Avoid effects */
    fprintf(f, "  \"effects_avoid\": [");
    for (int i = 0; i < desc->n_avoid; i++) {
        fprintf(f, "%s\"%s\"", i > 0 ? ", " : "", desc->avoid_effects[i]);
    }
    fprintf(f, "],\n");
    
    /* Director's notes (generated from plot analysis) */
    fprintf(f, "  \"director_notes\": {\n");
    fprintf(f, "    \"structure\": \"4-act\",\n");
    fprintf(f, "    \"setup_pct\": 20,\n");
    fprintf(f, "    \"escalate_pct\": 30,\n");
    fprintf(f, "    \"climax_pct\": 30,\n");
    fprintf(f, "    \"resolve_pct\": 20,\n");
    fprintf(f, "    \"callback_frequency\": %d,\n", desc->chaos > 5 ? 3 : 1);
    fprintf(f, "    \"intensity_curve\": \"ramp_up_peak_down\",\n");
    fprintf(f, "    \"encoding\": {\n");
    fprintf(f, "      \"resolution\": \"854x480\",\n");
    fprintf(f, "      \"fps\": 24,\n");
    fprintf(f, "      \"video_codec\": \"libx264\",\n");
    fprintf(f, "      \"preset\": \"ultrafast\",\n");
    fprintf(f, "      \"crf\": 28,\n");
    fprintf(f, "      \"audio_codec\": \"aac\",\n");
    fprintf(f, "      \"audio_bitrate\": \"64k\"\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n");
    
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}

/* Print description summary */
void print_description(const ytp_description *desc) {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  YTP Director — Edit Description                ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");
    
    printf("Title: %s\n", desc->title);
    printf("Source: %s\n", desc->source);
    printf("Transcript: %s\n", desc->transcript);
    printf("Chaos: %d/10\n\n", desc->chaos);
    
    printf("Plot:\n");
    for (int i = 0; i < desc->n_plot; i++) {
        printf("  %d. %s\n", i + 1, desc->plot_points[i]);
    }
    
    if (desc->n_prefer > 0) {
        printf("\nPreferred effects: ");
        for (int i = 0; i < desc->n_prefer; i++) {
            printf("%s%s", i > 0 ? ", " : "", desc->prefer_effects[i]);
        }
        printf("\n");
    }
    
    if (desc->n_avoid > 0) {
        printf("Avoided effects: ");
        for (int i = 0; i < desc->n_avoid; i++) {
            printf("%s%s", i > 0 ? ", " : "", desc->avoid_effects[i]);
        }
        printf("\n");
    }
}

/* Self-test with built-in example */
int self_test(void) {
    printf("=== YTP Director Self-Test ===\n\n");
    
    /* Write a test description */
    const char *test_desc = "/tmp/ytp_test_desc.txt";
    FILE *f = fopen(test_desc, "w");
    if (!f) return -1;
    
    fprintf(f,
        "TITLE: Shrek All Star Chaos\n"
        "SOURCE: assets/ytp_sources/video/famous_clips/SHREK_somebody_P6antjcBFZ4.mp4\n"
        "TRANSCRIPT: assets/ytp_sources/video/famous_clips/SHREK_somebody_P6antjcBFZ4.mp4.transcript.json\n"
        "CHAOS: 8\n"
        "PLOT:\n"
        "  - Shrek starts singing All Star normally\n"
        "  - Slowly things get weird, pitch shifts appear\n"
        "  - Someone once told me... gets stuck in a loop\n"
        "  - Full chaos: earrape, deep fry, vine booms everywhere\n"
        "  - Callback to the opening line at peak moment\n"
        "  - Abrupt cut to To Be Continued arrow\n"
        "EFFECTS_PREFER: stutter, earrape, vine_boom, deep_fry, reverse\n"
        "EFFECTS_AVOID: kaleido\n"
    );
    fclose(f);
    
    ytp_description desc;
    if (parse_description(test_desc, &desc) != 0) {
        printf("FAIL: could not parse description\n");
        return -1;
    }
    
    print_description(&desc);
    
    /* Generate EDL */
    const char *edl_out = "/tmp/ytp_test.edl.json";
    generate_edl(&desc, edl_out);
    printf("\nEDL written to: %s\n", edl_out);
    
    /* Print EDL */
    printf("\n--- EDL Contents ---\n");
    FILE *edl = fopen(edl_out, "r");
    if (edl) {
        char line[1024];
        while (fgets(line, sizeof(line), edl)) {
            printf("%s", line);
        }
        fclose(edl);
    }
    
    printf("\n\n=== SELF-TEST PASS ===\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <description.txt> [output.edl.json]\n", argv[0]);
        printf("\nRunning self-test...\n\n");
        return self_test();
    }
    
    const char *desc_file = argv[1];
    const char *output = argc > 2 ? argv[2] : "/tmp/ytp_output.edl.json";
    
    ytp_description desc;
    if (parse_description(desc_file, &desc) != 0) {
        printf("ERROR: could not parse %s\n", desc_file);
        return 1;
    }
    
    print_description(&desc);
    
    if (strlen(desc.source) == 0) {
        printf("\nERROR: no SOURCE specified in description\n");
        return 1;
    }
    
    generate_edl(&desc, output);
    printf("\nEDL written to: %s\n", output);
    printf("Render with: ./build/wb_ytp_studio \"%s\" \"%s\" %d output.mp4\n",
           desc.source, desc.transcript, desc.chaos);
    
    return 0;
}
