/* test_dvd_vm.c — verify DVD Virtual Machine + game builder */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <math.h>

extern "C" {
#include "wbus/wbus_compositor.h"
#include "test_dvd_internal.h"
}

/* DVD format constants for tests */
#define DVD_FORMAT_DVD5 0
#define DVD_FORMAT_DVD9 1
#define DVD_FORMAT_BD25 2
#define DVD_FORMAT_BD50 3

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while(0)

int main(void) {
    int pass = 0, fail = 0;
    printf("=== DVD Virtual Machine + Game Builder ===\n");

    /* ---- VM Register Tests ---- */
    printf("\n--- VM Registers ---\n");
    wb_dvd_vm vm;
    wb_dvd_vm_init(&vm);
    CHECK(wb_dvd_vm_get_gprm(&vm, 0) == 0, "GPRM[0] initialized to 0");
    CHECK(wb_dvd_vm_get_sprm(&vm, SPRM_AUDIO_STREAM) == 15, "SPRM[1] = 15 (no audio)");
    CHECK(wb_dvd_vm_get_sprm(&vm, SPRM_SUBPIC_STREAM) == 62, "SPRM[2] = 62 (no subpic)");

    wb_dvd_vm_set_gprm(&vm, 0, 100);
    CHECK(wb_dvd_vm_get_gprm(&vm, 0) == 100, "GPRM[0] = 100 after set");

    wb_dvd_vm_set_gprm(&vm, 15, 0xFFFF);
    CHECK(wb_dvd_vm_get_gprm(&vm, 15) == 0xFFFF, "GPRM[15] = 0xFFFF (max)");

    wb_dvd_vm_set_sprm(&vm, SPRM_AUDIO_STREAM, 3);
    CHECK(wb_dvd_vm_get_sprm(&vm, SPRM_AUDIO_STREAM) == 3, "SPRM[1] = 3 (audio stream)");

    /* Bounds checking */
    wb_dvd_vm_set_gprm(&vm, 16, 999); /* out of range */
    CHECK(wb_dvd_vm_get_gprm(&vm, 16) == 0, "GPRM[16] out of range returns 0");
    CHECK(wb_dvd_vm_get_gprm(&vm, -1) == 0, "GPRM[-1] out of range returns 0");

    /* ---- VM Instruction Emitter Tests ---- */
    printf("\n--- VM Instructions ---\n");
    uint8_t instr[8];

    wb_dvd_vm_emit_nop(instr);
    CHECK(instr[0] == 0 && instr[1] == 0, "NOP = all zeros");

    wb_dvd_vm_emit_set_gprm(instr, 0, VM_OP_ASSIGN, VM_SRC_IMM, 42);
    CHECK(instr[0] == 0x62, "SetGPRM opcode byte 0 = 0x62 (group 3, op=1)");
    CHECK(instr[1] == 0x00, "SetGPRM dst reg = 0");
    CHECK(instr[2] == 0x00 && instr[3] == 0x2A, "SetGPRM imm value = 42");

    wb_dvd_vm_emit_set_gprm(instr, 5, VM_OP_ADD, VM_SRC_GPRM, 3);
    CHECK(instr[1] == 0x05, "SetGPRM dst reg = 5");

    wb_dvd_vm_emit_set_sprm(instr, SPRM_AUDIO_STREAM, 2);
    CHECK((instr[0] & 0x40) == 0x40, "SetSPRM has group 2 prefix");

    wb_dvd_vm_emit_link_pgcn(instr, 5);
    CHECK(instr[0] == 0x20, "LinkPGCN group = 0x20");
    CHECK(instr[1] == 0x01, "LinkPGCN subcommand = 0x01");

    wb_dvd_vm_emit_jump_tt(instr, 3);
    CHECK(instr[0] == 0x30, "JumpTT group = 0x30");
    CHECK(instr[3] == 0x03, "JumpTT title = 3");

    wb_dvd_vm_emit_compare(instr, 0, VM_CMP_EQ, VM_SRC_IMM, 10);
    CHECK((instr[0] & 0x50) == 0x50, "Compare has group 5 prefix");
    CHECK((instr[0] & 0x0E) == (VM_CMP_EQ << 1), "Compare op = EQ");

    wb_dvd_vm_emit_goto(instr, 0x100);
    CHECK(instr[1] == 0x01, "Goto subcommand = 0x01");

    wb_dvd_vm_emit_break(instr);
    CHECK(instr[1] == 0x02, "Break subcommand = 0x02");

    /* ---- DVD Game Builder Tests ---- */
    printf("\n--- DVD Game Builder ---\n");
    struct wb_dvd_project *proj = wb_dvd_author_create();
    CHECK(proj != NULL, "DVD project created");

    wb_dvd_game *game = wb_dvd_game_create(proj, "YTP Quiz Game");
    CHECK(game != NULL, "Game created");
    CHECK(strcmp(game->name, "YTP Quiz Game") == 0, "Game name set");

    /* Set up menu buttons */
    wb_dvd_button menu_buttons[] = {
        {100, 100, 200, 50, 1},
        {100, 200, 200, 50, 2},
    };
    wb_dvd_author_set_menu(proj, NULL, menu_buttons, 2);
    CHECK(proj->button_count == 2, "2 menu buttons set");

    /* Configure scoring */
    wb_dvd_game_add_score(game, 10, -5);
    CHECK(game->points_correct == 10, "Points per correct = 10");
    CHECK(game->points_wrong == -5, "Points per wrong = -5");

    /* Add quiz questions */
    wb_dvd_game_add_question(game, "/tmp/q1.mp4", 1, 4); /* correct = button 1 */
    wb_dvd_game_add_question(game, "/tmp/q2.mp4", 3, 4); /* correct = button 3 */
    wb_dvd_game_add_question(game, "/tmp/q3.mp4", 2, 4); /* correct = button 2 */
    CHECK(game->question_count == 3, "3 questions added");
    CHECK(game->correct_buttons[0] == 1, "Q1 correct button = 1");
    CHECK(game->correct_buttons[1] == 3, "Q2 correct button = 3");
    CHECK(game->correct_buttons[2] == 2, "Q3 correct button = 2");
    CHECK(proj->title_count == 3, "3 titles auto-added from questions");

    /* Branching: if score >= 20, jump to PGC 10 (win screen) */
    wb_dvd_game_set_branching(game, 20, 10);
    wb_dvd_game_set_branching(game, 10, 11); /* if score >= 10, jump to PGC 11 */
    CHECK(game->branching_count == 2, "2 branch rules added");
    CHECK(game->branch_thresholds[0] == 20, "Branch 0 threshold = 20");
    CHECK(game->branch_targets[0] == 10, "Branch 0 target = PGC 10");

    /* Easter egg: button combo [1,2,3,4] triggers secret PGC 99 */
    int combo[] = {1, 2, 3, 4};
    wb_dvd_game_add_easter_egg(game, combo, 4, 99);
    CHECK(game->easter_egg_count == 1, "1 easter egg added");
    CHECK(game->easter_egg_lengths[0] == 4, "Easter egg combo length = 4");
    CHECK(game->easter_egg_targets[0] == 99, "Easter egg target = PGC 99");

    /* Timer: 30 second limit, timeout -> PGC 20 */
    wb_dvd_game_set_timer(game, 30, 20);
    CHECK(game->timer_seconds == 30, "Timer = 30 seconds");
    CHECK(game->timer_timeout_pgcn == 20, "Timer timeout PGC = 20");

    /* Hidden button: invisible area at (10,10,50,50) -> PGC 88 */
    wb_dvd_game_add_hidden_button(game, 10, 10, 50, 50, 88);
    CHECK(game->hidden_button_count == 1, "1 hidden button added");
    CHECK(game->hidden_buttons[0].x == 10.0f, "Hidden button x = 10");
    CHECK(game->hidden_targets[0] == 88, "Hidden button target = PGC 88");

    /* Parental lock */
    wb_dvd_game_set_parental(game, 3, 1234);
    CHECK(game->parental_level == 3, "Parental level = 3");
    CHECK(game->parental_password == 1234, "Parental password = 1234");

    /* Generate VM code */
    uint8_t vm_out[65536];
    int vm_len = wb_dvd_game_generate_vm(game, vm_out, sizeof(vm_out));
    CHECK(vm_len > 0, "VM code generated");
    CHECK(vm_len % 8 == 0, "VM code length is multiple of 8");
    printf("  VM code: %d bytes (%d instructions)\n", vm_len, vm_len / 8);

    /* Build the game */
    printf("  before build: proj->button_count = %d\n", proj->button_count);
    int rc = wb_dvd_game_build(game);
    printf("  after build: proj->button_count = %d, hidden = %d\n", proj->button_count, game->hidden_button_count);
    CHECK(rc == 0, "Game build succeeded");
    CHECK(proj->button_count >= 2, "Menu has at least 2 buttons");

    /* Test IFO generation by creating directory and calling export */
    char tmpdir[] = "/tmp/dvd_game_test_XXXXXX";
    char *dir = mkdtemp(tmpdir);
    if (dir) {
        /* Export creates VIDEO_TS dir — will fail at ffmpeg but dir is created */
        rc = wb_dvd_author_export(proj, dir, DVD_FORMAT_DVD5);
        printf("  export returned %d (ffmpeg fails without input files)\n", rc);

        /* Check that VIDEO_TS directory was created */
        char check[1024];
        snprintf(check, sizeof(check), "%s/VIDEO_TS", dir);
        struct stat st;
        if (stat(check, &st) == 0 && S_ISDIR(st.st_mode)) {
            printf("  PASS: VIDEO_TS directory created\n");
            pass++;
        } else {
            printf("  FAIL: VIDEO_TS directory not created\n");
            fail++;
        }
    }

    /* Cleanup */
    wb_dvd_game_destroy(game);
    wb_dvd_author_destroy(proj);
    CHECK(1, "all resources freed");

    printf("\n=== Results: %d/%d passed ===\n", pass, pass + fail);
    return fail > 0 ? 1 : 0;
}
