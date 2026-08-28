/* wb_keys.c — keyboard shortcuts for video editing (R077 Phase 6).
 *
 * Professional video editing keyboard shortcuts (Premiere/FCP/DaVinci).
 * Maps key combinations to editing commands.
 * Pure C11, no third party.
 */

#include <stdint.h>
#include <string.h>

/* Command IDs */
typedef enum {
    WB_KC_NONE = 0,
    WB_KC_PLAY_PAUSE,
    WB_KC_STOP,
    WB_KC_REWIND,
    WB_KC_FAST_FORWARD,
    WB_KC_FRAME_LEFT,
    WB_KC_FRAME_RIGHT,
    WB_KC_IN_POINT,
    WB_KC_OUT_POINT,
    WB_KC_MARKER,
    WB_KC_CUT,
    WB_KC_TRIM,
    WB_KC_RIPPLE_DELETE,
    WB_KC_SLIP,
    WB_KC_SLIDE,
    WB_KC_REDO,
    WB_KC_UNDO,
    WB_KC_COPY,
    WB_KC_PASTE,
    WB_KC_SPLIT,
    WB_KC_LIFT,
    WB_KC_EXTRACT,
    WB_KC_ADD_TRANSITION,
    WB_KC_ADD_DEFAULT_TRANSITION,
    WB_KC_ZOOM_IN,
    WB_KC_ZOOM_OUT,
    WB_KC_ZOOM_FIT,
    WB_KC_TOOL_SELECT,
    WB_KC_TOOL_RIPPLE,
    WB_KC_TOOL_TRIM,
    WB_KC_TOOL_SLIP,
    WB_KC_TOOL_SLIDE,
    WB_KC_SOURCE_MONITOR,
    WB_KC_PROGRAM_MONITOR,
    WB_KC_MEDIA_POOL,
    WB_KC_EFFECTS,
    WB_KC_AUDIO_MIXER,
    WB_KC_COLOR,
    WB_KC_FUSION,
    WB_KC_DELIVER,
    WB_KC_RENDER_QUEUE,
    WB_KC_EXPORT_FRAME,
    WB_KC_FULLSCREEN,
    WB_KC_SETTINGS,
    WB_KC_PREFERENCES,
    WB_KC_COUNT
} wb_key_cmd;

typedef struct {
    uint32_t modifiers;  /* CMD=1, SHIFT=2, OPT=4, CTRL=8 */
    uint32_t keycode;    /* platform-specific key code */
    wb_key_cmd cmd;
    const char *name;
} wb_key_brief;

#define WB_MOD_NONE  0u
#define WB_MOD_CMD   1u
#define WB_MOD_SHIFT 2u
#define WB_MOD_OPT   4u
#define WB_MOD_CTRL  8u

/* macOS keycodes (subset) */
#define KB_SPACE      0x31
#define KB_RETURN     0x24
#define KB_BACKSPACE  0x33
#define KB_DELETE     0x73
#define KB_TAB        0x30
#define KB_ESCAPE     0x35
#define KB_PERIOD     0x2F
#define KB_COMMA      0x2B
#define KB_SEMICOLON  0x29
#define KB_SLASH      0x2C
#define KB_APOSTROPHE 0x2A
#define KB_K          0x28
#define KB_N          0x25
#define KB_O          0x1F
#define KB_P          0x23
#define KB_U          0x20
#define KB_Z          0x06
#define KB_X          0x07
#define KB_C          0x08
#define KB_V          0x09
#define KB_A          0x00
#define KB_B          0x0B
#define KB_D          0x02
#define KB_E          0x0E
#define KB_F          0x03
#define KB_I          0x22
#define KB_J          0x26
#define KB_L          0x2D
#define KB_M          0x2E
#define KB_Q          0x0C
#define KB_R          0x0F
#define KB_S          0x01
#define KB_T          0x11
#define KB_MINUS      0x1B
#define KB_EQUAL      0x18
#define KB_1          0x12
#define KB_2          0x13
#define KB_3          0x14
#define KB_4          0x15
#define KB_5          0x16
#define KB_6          0x17
#define KB_7          0x18
#define KB_8          0x19
#define KB_9          0x1A
#define KB_0          0x1D
#define KB_LEFT       0x7B
#define KB_RIGHT      0x7C
#define KB_DOWN       0x7D
#define KB_UP         0x7E

/* Standard editing shortcuts (Premiere/FCP/DaVinci compatible) */
static const wb_key_brief wb_keymap[] = {
    /* Transport */
    { WB_MOD_NONE, KB_SPACE,       WB_KC_PLAY_PAUSE,    "Space" },
    { WB_MOD_NONE, KB_RETURN,      WB_KC_FULLSCREEN,    "Return" },
    { WB_MOD_NONE, KB_ESCAPE,      WB_KC_STOP,          "Esc" },
    { WB_MOD_NONE, KB_DOWN,        WB_KC_FRAME_RIGHT,   "Down" },
    { WB_MOD_NONE, KB_UP,          WB_KC_FRAME_LEFT,    "Up" },
    { WB_MOD_NONE, KB_LEFT,        WB_KC_REWIND,        "Left" },
    { WB_MOD_NONE, KB_RIGHT,       WB_KC_FAST_FORWARD,  "Right" },

    /* JKL shuttle */
    { WB_MOD_NONE, KB_J, WB_KC_REWIND,        "J" },
    { WB_MOD_NONE, KB_K, WB_KC_PLAY_PAUSE,    "K" },
    { WB_MOD_NONE, KB_L, WB_KC_FAST_FORWARD,  "L" },

    /* Mark In/Out */
    { WB_MOD_NONE, KB_I, WB_KC_IN_POINT,  "I" },
    { WB_MOD_NONE, KB_O, WB_KC_OUT_POINT, "O" },
    { WB_MOD_NONE, KB_M, WB_KC_MARKER,    "M" },

    /* Edit tools */
    { WB_MOD_NONE, KB_B, WB_KC_TOOL_RIPPLE,  "B" },
    { WB_MOD_NONE, KB_N, WB_KC_TOOL_TRIM,    "N" },
    { WB_MOD_NONE, KB_V, WB_KC_TOOL_SELECT,  "V" },
    { WB_MOD_NONE, KB_T, WB_KC_TOOL_SLIP,    "T" },
    { WB_MOD_NONE, KB_E, WB_KC_TOOL_SLIDE,   "E" },
    { WB_MOD_NONE, KB_A, WB_KC_TOOL_TRIM,    "A" },

    /* Timeline */
    { WB_MOD_CMD,  KB_K, WB_KC_CUT,            "Cmd+K" },
    { WB_MOD_CMD,  KB_N, WB_KC_SPLIT,          "Cmd+N" },
    { WB_MOD_NONE, KB_SEMICOLON, WB_KC_LIFT,   ";" },
    { WB_MOD_NONE, KB_APOSTROPHE, WB_KC_EXTRACT, "'" },

    /* Transitions */
    { WB_MOD_CMD,  KB_D, WB_KC_ADD_TRANSITION,          "Cmd+D" },
    { WB_MOD_CMD|WB_MOD_SHIFT, KB_D, WB_KC_ADD_DEFAULT_TRANSITION, "Cmd+Shift+D" },

    /* Undo/Redo */
    { WB_MOD_CMD,  KB_Z, WB_KC_UNDO,   "Cmd+Z" },
    { WB_MOD_CMD|WB_MOD_SHIFT, KB_Z, WB_KC_REDO, "Cmd+Shift+Z" },

    /* Copy/Paste/Cut */
    { WB_MOD_CMD, KB_C,  WB_KC_COPY,  "Cmd+C" },
    { WB_MOD_CMD, KB_V,  WB_KC_PASTE, "Cmd+V" },
    { WB_MOD_CMD, KB_X,  WB_KC_CUT,   "Cmd+X" },

    /* Zoom */
    { WB_MOD_CMD, KB_EQUAL,  WB_KC_ZOOM_IN,   "Cmd+=" },
    { WB_MOD_CMD, KB_MINUS,  WB_KC_ZOOM_OUT,  "Cmd+-" },
    { WB_MOD_CMD|WB_MOD_OPT, KB_A, WB_KC_ZOOM_FIT, "Cmd+Opt+A" },

    /* Window panels */
    { WB_MOD_CMD, KB_1, WB_KC_MEDIA_POOL,       "Cmd+1" },
    { WB_MOD_CMD, KB_2, WB_KC_SOURCE_MONITOR,   "Cmd+2" },
    { WB_MOD_CMD, KB_3, WB_KC_PROGRAM_MONITOR,  "Cmd+3" },
    { WB_MOD_CMD, KB_4, WB_KC_EFFECTS,          "Cmd+4" },
    { WB_MOD_CMD, KB_5, WB_KC_COLOR,            "Cmd+5" },
    { WB_MOD_CMD, KB_6, WB_KC_FUSION,           "Cmd+6" },
    { WB_MOD_CMD, KB_7, WB_KC_AUDIO_MIXER,      "Cmd+7" },
    { WB_MOD_CMD, KB_8, WB_KC_DELIVER,          "Cmd+8" },
    { WB_MOD_CMD, KB_9, WB_KC_SETTINGS,         "Cmd+9" },

    /* Export/Render */
    { WB_MOD_CMD, KB_E, WB_KC_EXPORT_FRAME,    "Cmd+E" },
    { WB_MOD_CMD, KB_RETURN, WB_KC_RENDER_QUEUE, "Cmd+Return" },
};

/* Lookup command by modifiers + keycode */
const wb_key_brief *wb_key_lookup(uint32_t modifiers, uint32_t keycode) {
    for (int i = 0; i < (int)(sizeof(wb_keymap)/sizeof(wb_keymap[0])); i++) {
        if (wb_keymap[i].modifiers == modifiers && wb_keymap[i].keycode == keycode) {
            return &wb_keymap[i];
        }
    }
    return NULL;
}

const char *wb_key_name(wb_key_cmd cmd) {
    for (int i = 0; i < (int)(sizeof(wb_keymap)/sizeof(wb_keymap[0])); i++) {
        if (wb_keymap[i].cmd == cmd) {
            return wb_keymap[i].name;
        }
    }
    return "Unassigned";
}

int wb_keymap_count(void) {
    return (int)(sizeof(wb_keymap) / sizeof(wb_keymap[0]));
}
