/* gen_ytp_sfx.c — Generate classic YTP sound effects programmatically (R083).
 *
 * Creates the essential YTP sounds using ffmpeg's audio sources:
 *   - vine_boom: deep bass thud (the classic "BOOM")
 *   - airhorn: loud horn blast
 *   - bruh: descending tone
 *   - censor_beep: 1000Hz sine wave (TV censor)
 *   - sega: ascending "SEGA" jingle
 *   - to_be_continued: dramatic sting
 *   - record_scratch: vinyl scratch effect
 *   - vine_bass: even deeper boom
 *   - error: Windows XP error sound
 *   - startup: Windows XP startup chime
 *
 * Usage: ./build/gen_ytp_sfx
 * Output: assets/sound_effects/*.wav
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CMD 1024
#define OUT_DIR "assets/sound_effects"

int main() {
    char cmd[MAX_CMD];
    int ret = 0;
    
    printf("=== YTP Sound Effects Generator ===\n\n");
    
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", OUT_DIR);
    system(cmd);
    
    // Vine boom - deep bass thud
    printf("Generating vine_boom.wav...\n");
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -f lavfi -i \"sine=frequency=60:duration=0.3\" "
        "-af \"volume=3,alimiter=limit=0.9\" "
        "\"%s/vine_boom.wav\" 2>/dev/null", OUT_DIR);
    ret |= system(cmd);
    
    // Vine bass - even deeper
    printf("Generating vine_bass.wav...\n");
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -f lavfi -i \"sine=frequency=40:duration=0.5\" "
        "-af \"volume=4,alimiter=limit=0.9\" "
        "\"%s/vine_bass.wav\" 2>/dev/null", OUT_DIR);
    ret |= system(cmd);
    
    // Airhorn - loud horn
    printf("Generating airhorn.wav...\n");
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -f lavfi -i \"sine=frequency=440:duration=0.8\" "
        "-i - -filter_complex "
        "\"[0:a]asetrate=44100*0.7,volume=2,alimiter=limit=0.9[a]\" "
        "-map \"[a]\" \"%s/airhorn.wav\" 2>/dev/null", OUT_DIR);
    // Simpler approach:
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -f lavfi -i \"sine=frequency=440:duration=0.6\" "
        "-af \"volume=2.5,alimiter=limit=0.9\" "
        "\"%s/airhorn.wav\" 2>/dev/null", OUT_DIR);
    ret |= system(cmd);
    
    // Bruh - descending tone
    printf("Generating bruh.wav...\n");
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -f lavfi -i \"sine=frequency=200:duration=0.4\" "
        "-af \"asetrate=44100*0.5,volume=2,alimiter=limit=0.9\" "
        "\"%s/bruh.wav\" 2>/dev/null", OUT_DIR);
    ret |= system(cmd);
    
    // Censor beep - 1000Hz sine
    printf("Generating censor_beep.wav...\n");
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -f lavfi -i \"sine=frequency=1000:duration=0.3\" "
        "-af \"volume=1.5\" \"%s/censor_beep.wav\" 2>/dev/null", OUT_DIR);
    ret |= system(cmd);
    
    // SEGA jingle - ascending notes
    printf("Generating sega.wav...\n");
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y "
        "-f lavfi -i \"sine=frequency=523:duration=0.15\" "
        "-f lavfi -i \"sine=frequency=659:duration=0.15\" "
        "-f lavfi -i \"sine=frequency=784:duration=0.15\" "
        "-f lavfi -i \"sine=frequency=1047:duration=0.3\" "
        "-filter_complex "
        "\"[0:a]adelay=0|0[a0];"
        "[1:a]adelay=150|150[a1];"
        "[2:a]adelay=300|300[a2];"
        "[3:a]adelay=450|450[a3];"
        "[a0][a1][a2][a3]amix=inputs=4:duration=longest,volume=1.5,alimiter=limit=0.9\" "
        "\"%s/sega.wav\" 2>/dev/null", OUT_DIR);
    ret |= system(cmd);
    
    // To Be Continued - dramatic sting (descending low)
    printf("Generating to_be_continued.wav...\n");
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y "
        "-f lavfi -i \"sine=frequency=440:duration=0.2\" "
        "-f lavfi -i \"sine=frequency=370:duration=0.2\" "
        "-f lavfi -i \"sine=frequency=330:duration=0.2\" "
        "-f lavfi -i \"sine=frequency=220:duration=0.5\" "
        "-filter_complex "
        "\"[0:a]adelay=0|0[a0];"
        "[1:a]adelay=200|200[a1];"
        "[2:a]adelay=400|400[a2];"
        "[3:a]adelay=600|600[a3];"
        "[a0][a1][a2][a3]amix=inputs=4:duration=longest,volume=2,alimiter=limit=0.9\" "
        "\"%s/to_be_continued.wav\" 2>/dev/null", OUT_DIR);
    ret |= system(cmd);
    
    // Record scratch - noise burst
    printf("Generating record_scratch.wav...\n");
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -f lavfi -i \"anoisesrc=color=brown:duration=0.3\" "
        "-af \"volume=2,highpass=f=1000,lowpass=f=5000,alimiter=limit=0.9\" "
        "\"%s/record_scratch.wav\" 2>/dev/null", OUT_DIR);
    ret |= system(cmd);
    
    // Error sound - two-tone buzz
    printf("Generating error.wav...\n");
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y "
        "-f lavfi -i \"sine=frequency=800:duration=0.1\" "
        "-f lavfi -i \"sine=frequency=600:duration=0.1\" "
        "-filter_complex "
        "\"[0:a]adelay=0|0[a0];"
        "[1:a]adelay=100|100[a1];"
        "[a0][a1]amix=inputs=2:duration=longest,volume=1.5\" "
        "\"%s/error.wav\" 2>/dev/null", OUT_DIR);
    ret |= system(cmd);
    
    // Startup chime - ascending pleasant
    printf("Generating startup.wav...\n");
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y "
        "-f lavfi -i \"sine=frequency=523:duration=0.2\" "
        "-f lavfi -i \"sine=frequency=659:duration=0.2\" "
        "-f lavfi -i \"sine=frequency=784:duration=0.4\" "
        "-filter_complex "
        "\"[0:a]adelay=0|0[a0];"
        "[1:a]adelay=200|200[a1];"
        "[2:a]adelay=400|400[a2];"
        "[a0][a1][a2]amix=inputs=3:duration=longest,volume=1.2\" "
        "\"%s/startup.wav\" 2>/dev/null", OUT_DIR);
    ret |= system(cmd);
    
    // Scream - high pitched noise
    printf("Generating scream.wav...\n");
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -f lavfi -i \"sine=frequency=2000:duration=0.3\" "
        "-af \"asetrate=44100*1.5,volume=1.5,alimiter=limit=0.9\" "
        "\"%s/scream.wav\" 2>/dev/null", OUT_DIR);
    ret |= system(cmd);
    
    // Pause menu - short blip
    printf("Generating pause_blip.wav...\n");
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -f lavfi -i \"sine=frequency=1200:duration=0.08\" "
        "-af \"volume=1.5\" \"%s/pause_blip.wav\" 2>/dev/null", OUT_DIR);
    ret |= system(cmd);
    
    printf("\nDone! Sound effects in %s/\n", OUT_DIR);
    
    // List generated files
    snprintf(cmd, sizeof(cmd), "ls -lh %s/", OUT_DIR);
    system(cmd);
    
    return ret;
}
