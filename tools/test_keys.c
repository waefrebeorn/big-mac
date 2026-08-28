/* test_keys.c — verify keyboard shortcut system */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Minimal redef for standalone test */
typedef enum {
    WB_KC_NONE = 0, WB_KC_PLAY_PAUSE, WB_KC_CUT, WB_KC_SPLIT,
    WB_KC_UNDO, WB_KC_REDO, WB_KC_ADD_TRANSITION, WB_KC_TOOL_SELECT,
    WB_KC_COUNT
} wb_key_cmd;

typedef struct {
    uint32_t modifiers;
    uint32_t keycode;
    wb_key_cmd cmd;
    const char *name;
} wb_key_brief;

#define WB_MOD_NONE  0u
#define WB_MOD_CMD   1u
#define WB_MOD_SHIFT 2u

#define KB_SPACE 0x31
#define KB_K     0x28
#define KB_N     0x25
#define KB_Z     0x06
#define KB_D     0x02
#define KB_V     0x09

static const wb_key_brief keymap[] = {
    { WB_MOD_NONE, KB_SPACE, WB_KC_PLAY_PAUSE, "Space" },
    { WB_MOD_CMD,  KB_K,     WB_KC_CUT,        "Cmd+K" },
    { WB_MOD_CMD,  KB_N,     WB_KC_SPLIT,      "Cmd+N" },
    { WB_MOD_CMD,  KB_Z,     WB_KC_UNDO,       "Cmd+Z" },
    { WB_MOD_CMD|WB_MOD_SHIFT, KB_Z, WB_KC_REDO, "Cmd+Shift+Z" },
    { WB_MOD_CMD,  KB_D,     WB_KC_ADD_TRANSITION, "Cmd+D" },
    { WB_MOD_NONE, KB_V,     WB_KC_TOOL_SELECT, "V" },
};

const wb_key_brief *wb_key_lookup(uint32_t modifiers, uint32_t keycode) {
    for (int i = 0; i < (int)(sizeof(keymap)/sizeof(keymap[0])); i++) {
        if (keymap[i].modifiers == modifiers && keymap[i].keycode == keycode)
            return &keymap[i];
    }
    return NULL;
}

int main(void) {
    int pass = 1;

    /* Test Space -> Play/Pause */
    const wb_key_brief *kb = wb_key_lookup(WB_MOD_NONE, KB_SPACE);
    if (!kb || kb->cmd != WB_KC_PLAY_PAUSE) { printf("FAIL: Space\n"); pass = 0; }
    else printf("PASS: Space -> Play/Pause\n");

    /* Test Cmd+K -> Cut */
    kb = wb_key_lookup(WB_MOD_CMD, KB_K);
    if (!kb || kb->cmd != WB_KC_CUT) { printf("FAIL: Cmd+K\n"); pass = 0; }
    else printf("PASS: Cmd+K -> Cut\n");

    /* Test Cmd+Z -> Undo */
    kb = wb_key_lookup(WB_MOD_CMD, KB_Z);
    if (!kb || kb->cmd != WB_KC_UNDO) { printf("FAIL: Cmd+Z\n"); pass = 0; }
    else printf("PASS: Cmd+Z -> Undo\n");

    /* Test Cmd+Shift+Z -> Redo */
    kb = wb_key_lookup(WB_MOD_CMD|WB_MOD_SHIFT, KB_Z);
    if (!kb || kb->cmd != WB_KC_REDO) { printf("FAIL: Cmd+Shift+Z\n"); pass = 0; }
    else printf("PASS: Cmd+Shift+Z -> Redo\n");

    /* Test V -> Select tool */
    kb = wb_key_lookup(WB_MOD_NONE, KB_V);
    if (!kb || kb->cmd != WB_KC_TOOL_SELECT) { printf("FAIL: V\n"); pass = 0; }
    else printf("PASS: V -> Select\n");

    /* Test nonexistent combo */
    kb = wb_key_lookup(WB_MOD_CMD, 0xFF);
    if (kb) { printf("FAIL: nonexistent should return NULL\n"); pass = 0; }
    else printf("PASS: nonexistent -> NULL\n");

    printf("%s\n", pass ? "ALL PASS" : "SOME FAIL");
    return pass ? 0 : 1;
}
