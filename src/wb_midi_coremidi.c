/* wb_midi_coremidi.c — CoreMIDI input (macOS).
 * Implements the portable wb_midi interface using CoreMIDI's classic MIDI 1.0
 * API (device -> entity -> source endpoint; MIDIInputPortCreate + connect +
 * read callback). CoreMIDI is the platform's own C library (like CoreAudio).
 *
 * Enumerates devices, opens one by name (or substring), and forwards every
 * incoming packet to the caller's event callback on CoreMIDI's receive
 * thread. Safe to call wb_engine_note() from the callback — it only pushes
 * to the lock-free queue.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

#include "wbus.h"
#include "wbus_midi.h"
#include "wb_internal.h"
#include <math.h>

/* ---- shared MIDI tuning helper (used by all instruments) --------------- */
double wb_midi_note_to_freq(int note) {
    return 440.0 * pow(2.0, (note - 69) / 12.0);
}

struct wb_midi {
    MIDIClientRef client;
    MIDIPortRef   in_port;
    MIDIEndpointRef source;      /* the source endpoint we opened */
    void (*on_event)(wb_midi_event, void *);
    void *userdata;

    /* output (Launchpad / controller feedback) */
    MIDIPortRef   out_port;
    MIDIEndpointRef dest;        /* the destination endpoint we opened */

    /* optional capture buffer (test/inspection): when cap != NULL, every
     * sent byte (short messages + sysex) is appended here instead of (or in
     * addition to) being sent to the device. Lets headless tests assert the
     * exact wire bytes without hardware. */
    uint8_t *cap;
    int      cap_cap;
    int      cap_len;

    /* when null_handle is true, send() returns -1 (no device) but still
     * captures so tests can assert wire bytes. */
    int      null_handle;
};

/* Enable a capture buffer (test hook). `buf` must hold `cap` bytes. Pass
 * NULL to disable. Captured bytes are appended across calls. Returns 0. */
int wb_midi_capture(wb_midi *m, uint8_t *buf, int cap) {
    if (!m) return -1;
    m->cap = buf; m->cap_cap = cap; m->cap_len = 0;
    return 0;
}

/* Return how many bytes were captured so far. */
int wb_midi_capture_len(wb_midi *m) {
    if (!m) return -1;
    return m->cap_len;
}

/* Create a headless MIDI handle with NO device attached (for testing /
 * inspection only). All wb_midi_send / wb_midi_send_sysex calls will fail to
 * reach hardware but can be captured via wb_midi_capture(). Returns NULL on
 * alloc failure. Free with wb_midi_close(). */
wb_midi *wb_midi_create_null(void) {
    wb_midi *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->null_handle = 1;
    m->cap = NULL;
    m->cap_cap = 0;
    m->cap_len = 0;
    return m;
}

/* ---- device name helpers ---------------------------------------------- */
static void get_device_name(MIDIObjectRef obj, char *out, size_t cap) {
    out[0] = '\0';
    CFStringRef name = NULL;
    if (MIDIObjectGetStringProperty(obj, kMIDIPropertyName, &name) == noErr && name) {
        CFStringGetCString(name, out, (CFIndex)cap, kCFStringEncodingUTF8);
        CFRelease(name);
    }
}

int wb_midi_enumerate(char (*names)[64], int max, int *out_count) {
    int n = 0;
    ItemCount ndev = MIDIGetNumberOfDevices();
    for (ItemCount i = 0; i < ndev && n < max; i++) {
        MIDIDeviceRef dev = MIDIGetDevice(i);
        ItemCount ne = MIDIDeviceGetNumberOfEntities(dev);
        for (ItemCount j = 0; j < ne; j++) {
            MIDIEntityRef ent = MIDIDeviceGetEntity(dev, j);
            ItemCount ns = MIDIEntityGetNumberOfSources(ent);
            for (ItemCount k = 0; k < ns && n < max; k++) {
                MIDIEndpointRef src = MIDIEntityGetSource(ent, k);
                get_device_name(src, names[n], 64);
                n++;
            }
        }
    }
    *out_count = n;
    return n;
}

/* ---- read callback (CoreMIDI receive thread) -------------------------- */
static void midi_read_cb(const MIDIPacketList *pktlist,
                         void *readProcRefCon, void *srcConnRefCon) {
    (void)srcConnRefCon;
    wb_midi *m = readProcRefCon;
    if (!m || !m->on_event) return;

    const MIDIPacket *pkt = &pktlist->packet[0];
    for (unsigned i = 0; i < pktlist->numPackets; i++) {
        if (pkt->length >= 3) {
            wb_midi_event ev;
            ev.status = pkt->data[0];
            ev.data1  = pkt->data[1];
            ev.data2  = pkt->data[2];
            /* normalize note-off (0x80 or 0x90 with vel 0) */
            if ((ev.status & 0xF0) == 0x90 && ev.data2 == 0)
                ev.status = (uint8_t)(0x80 | (ev.status & 0x0F));
            m->on_event(ev, m->userdata);
        }
        pkt = MIDIPacketNext(pkt);
    }
}

/* ---- open -------------------------------------------------------------- */
static wb_midi *open_source(MIDIEndpointRef src, const char *name,
                            void (*cb)(wb_midi_event, void *), void *ud) {
    wb_midi *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->on_event = cb;
    m->userdata = ud;
    m->source = src;

    CFStringRef cname = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    OSStatus err = MIDIClientCreate(cname, NULL, NULL, &m->client);
    CFRelease(cname);
    if (err != noErr) { free(m); return NULL; }

    err = MIDIInputPortCreate(m->client, CFSTR("wbus in"), midi_read_cb, m, &m->in_port);
    if (err != noErr) { MIDIClientDispose(m->client); free(m); return NULL; }

    err = MIDIPortConnectSource(m->in_port, m->source, NULL);
    if (err != noErr) { MIDIPortDispose(m->in_port); MIDIClientDispose(m->client); free(m); return NULL; }

    return m;
}

wb_midi *wb_midi_open(const char *name,
                      void (*cb)(wb_midi_event, void *), void *ud) {
    ItemCount ndev = MIDIGetNumberOfDevices();
    for (ItemCount i = 0; i < ndev; i++) {
        MIDIDeviceRef dev = MIDIGetDevice(i);
        ItemCount ne = MIDIDeviceGetNumberOfEntities(dev);
        for (ItemCount j = 0; j < ne; j++) {
            MIDIEntityRef ent = MIDIDeviceGetEntity(dev, j);
            ItemCount ns = MIDIEntityGetNumberOfSources(ent);
            for (ItemCount k = 0; k < ns; k++) {
                MIDIEndpointRef src = MIDIEntityGetSource(ent, k);
                char devname[64];
                get_device_name(src, devname, sizeof(devname));
                if (strcmp(devname, name) == 0)
                    return open_source(src, devname, cb, ud);
            }
        }
    }
    return NULL;
}

wb_midi *wb_midi_open_contains(const char *substr,
                               void (*cb)(wb_midi_event, void *), void *ud) {
    ItemCount ndev = MIDIGetNumberOfDevices();
    for (ItemCount i = 0; i < ndev; i++) {
        MIDIDeviceRef dev = MIDIGetDevice(i);
        ItemCount ne = MIDIDeviceGetNumberOfEntities(dev);
        for (ItemCount j = 0; j < ne; j++) {
            MIDIEntityRef ent = MIDIDeviceGetEntity(dev, j);
            ItemCount ns = MIDIEntityGetNumberOfSources(ent);
            for (ItemCount k = 0; k < ns; k++) {
                MIDIEndpointRef src = MIDIEntityGetSource(ent, k);
                char devname[64];
                get_device_name(src, devname, sizeof(devname));
                if (strcasestr(devname, substr))
                    return open_source(src, devname, cb, ud);
            }
        }
    }
    return NULL;
}

void wb_midi_close(wb_midi *m) {
    if (!m) return;
    if (m->out_port) { MIDIPortDispose(m->out_port); m->out_port = 0; }
    if (m->in_port) { MIDIPortDispose(m->in_port); m->in_port = 0; }
    if (m->client)  { MIDIClientDispose(m->client); m->client = 0; }
    free(m);
}

/* ---- MIDI output -------------------------------------------------------- */
/* Open the first MIDI destination whose name contains `substr`. Creates an
 * output port on the shared client. Returns 0 ok, -1 if none found. */
int wb_midi_open_output(wb_midi *m, const char *substr) {
    if (!m || !m->client) return -1;
    ItemCount ndest = MIDIGetNumberOfDestinations();
    MIDIEndpointRef found = 0;
    char devname[64];
    for (ItemCount i = 0; i < ndest; i++) {
        MIDIEndpointRef dst = MIDIGetDestination(i);
        get_device_name(dst, devname, sizeof(devname));
        if (substr && substr[0] && !strcasestr(devname, substr)) continue;
        found = dst;
        break;
    }
    if (!found) return -1;
    OSStatus err = MIDIOutputPortCreate(m->client, CFSTR("wbus out"), &m->out_port);
    if (err != noErr) return -1;
    m->dest = found;
    return 0;
}

int wb_midi_send(wb_midi *m, uint8_t status, uint8_t data1, uint8_t data2) {
    if (!m) return -1;
    /* capture hook (test/inspection) */
    if (m->cap && m->cap_len + 3 <= m->cap_cap) {
        m->cap[m->cap_len++] = status;
        m->cap[m->cap_len++] = data1;
        m->cap[m->cap_len++] = data2;
    }
    if (m->null_handle || !m->out_port || !m->dest) return -1;
    Byte packet_data[3] = { status, data1, data2 };
    MIDIPacketList pktlist;
    MIDIPacket *pkt = MIDIPacketListInit(&pktlist);
    pkt = MIDIPacketListAdd(&pktlist, sizeof(pktlist), pkt, 0, 3, packet_data);
    if (!pkt) return -1;
    OSStatus err = MIDISend(m->out_port, m->dest, &pktlist);
    return (err == noErr) ? 0 : -1;
}

/* Send a raw byte stream (e.g. a SysEx message) to the output destination.
 * CoreMIDI accepts any well-formed packet; we just wrap the bytes. */
int wb_midi_send_sysex(wb_midi *m, const uint8_t *data, int len) {
    if (!m || !data || len <= 0) return -1;
    /* capture hook (test/inspection) */
    if (m->cap && m->cap_len + len <= m->cap_cap) {
        for (int i = 0; i < len; i++) m->cap[m->cap_len++] = data[i];
    }
    if (m->null_handle || !m->out_port || !m->dest) return -1;
    MIDIPacketList pktlist;
    MIDIPacket *pkt = MIDIPacketListInit(&pktlist);
    pkt = MIDIPacketListAdd(&pktlist, sizeof(pktlist), pkt, 0,
                            (UInt32)len, data);
    if (!pkt) return -1;
    OSStatus err = MIDISend(m->out_port, m->dest, &pktlist);
    return (err == noErr) ? 0 : -1;
}

/* ---- Classic Launchpad (1st/2nd gen: velocity-as-color) --------------- */
/* NOTE: Launchpad Mk2 uses a different layout + RGB SysEx — see wb_lp_mk2_*. */
int wb_launchpad_classic_note(int row, int col) {
    if (row < 0 || row > 7 || col < 0 || col > 7) return -1;
    return row * 16 + col;
}

int wb_launchpad_classic_led(wb_midi *m, int row, int col, uint8_t color) {
    int note = wb_launchpad_classic_note(row, col);
    if (note < 0) return -1;
    return wb_midi_send(m, 0x90, (uint8_t)note, color);
}

int wb_launchpad_classic_clear(wb_midi *m) {
    if (!m) return -1;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            wb_launchpad_classic_led(m, r, c, 0);
    return 0;
}

/* ---- Launchpad Mk2 (our own driver, C11, class-compliant) ------------- */
/* Grid note = 11 + col + row*10 (Mk2 firmware layout). */
int wb_lp_mk2_note(int row, int col) {
    if (row < 0 || row > 7 || col < 0 || col > 7) return -1;
    return 11 + col + row * 10;
}

int wb_lp_mk2_top_note(int idx) {
    if (idx < 0 || idx > 7) return -1;
    return 91 + idx;   /* top row = notes 91..98 */
}

/* The Mk2 RGB SysEx: F0 00 20 29 02 18 0B <note> <r> <g> <b> F7
 * Each color channel is 0..63. Total = 12 bytes. Top-row + grid both use
 * the same command. */
static int lp_mk2_set(wb_midi *m, int note, uint8_t r, uint8_t g, uint8_t b) {
    if (!m) return -1;
    if (r > 63) r = 63; if (g > 63) g = 63; if (b > 63) b = 63;
    uint8_t syx[12] = { 0xF0, 0x00, 0x20, 0x29, 0x02, 0x18,
                        0x0B, (uint8_t)note, r, g, b, 0xF7 };
    return wb_midi_send_sysex(m, syx, sizeof(syx));
}

/* Named state colors -> RGB triples (R006 §4: color = meaning). */
static void lp_color_rgb(wb_lp_color c, uint8_t *r, uint8_t *g, uint8_t *b) {
    switch (c) {
        case WB_LP_WHITE: *r=63; *g=63; *b=63; break;
        case WB_LP_GREEN: *r=0;  *g=63; *b=0;  break;
        case WB_LP_AMBER: *r=63; *g=45; *b=0;  break;
        case WB_LP_BLUE:  *r=0;  *g=20; *b=63; break;
        case WB_LP_RED:   *r=63; *g=0;  *b=0;  break;
        case WB_LP_CYAN:  *r=0;  *g=63; *b=63; break;
        case WB_LP_DIM:   *r=8;  *g=8;  *b=8;  break;
        case WB_LP_OFF:
        default:          *r=0;  *g=0;  *b=0;  break;
    }
}

int wb_lp_mk2_led(wb_midi *m, int row, int col, wb_lp_color c) {
    int note = wb_lp_mk2_note(row, col);
    if (note < 0) return -1;
    uint8_t r, g, b; lp_color_rgb(c, &r, &g, &b);
    return lp_mk2_set(m, note, r, g, b);
}

int wb_lp_mk2_led_rgb(wb_midi *m, int row, int col, uint8_t r, uint8_t g, uint8_t b) {
    int note = wb_lp_mk2_note(row, col);
    if (note < 0) return -1;
    return lp_mk2_set(m, note, r, g, b);
}

int wb_lp_mk2_top_rgb(wb_midi *m, int idx, uint8_t r, uint8_t g, uint8_t b) {
    int note = wb_lp_mk2_top_note(idx);
    if (note < 0) return -1;
    return lp_mk2_set(m, note, r, g, b);
}

int wb_lp_mk2_clear(wb_midi *m) {
    if (!m) return -1;
    /* Mk2 "clear all" SysEx: F0 00 20 29 02 18 0E 00 F7 (light all to 0). */
    uint8_t clr[9] = { 0xF0, 0x00, 0x20, 0x29, 0x02, 0x18, 0x0E, 0x00, 0xF7 };
    int rc = wb_midi_send_sysex(m, clr, sizeof(clr));
    if (rc != 0) {
        /* fallback: turn each pad off individually */
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                wb_lp_mk2_led(m, r, c, WB_LP_OFF);
    }
    return rc;
}

/* Inverse: given a Mk2 MIDI note, find the (row,col) on the 8x8 grid.
 * Grid notes are 11..88 (11 + col + row*10), top-row are 91..98.
 * Returns 0 on success (fills *row,*col), -1 if note is not a grid/top note. */
int wb_lp_mk2_row_col_from_note(int note, int *row, int *col) {
    if (!row || !col) return -1;
    if (note >= 11 && note <= 88) {
        int off = note - 11;
        if (off < 0) return -1;
        *col = off % 10;
        *row = off / 10;
        if (*row <= 7 && *col <= 7) return 0;
        return -1;
    }
    if (note >= 91 && note <= 98) {
        *row = 0;   /* top row can be treated as row 0 for feedback */
        *col = note - 91;
        return 0;
    }
    return -1;
}

/* ---- Scale helpers (R006 §3: scale lock, owned) ----------------------- */
/* Interval tables (semitones from root) for the supported scale types. */
static const int wb_scale_steps[][7] = {
    {0,2,4,5,7,9,11},  /* 0 major */
    {0,2,3,5,7,8,10},  /* 1 natural minor */
    {0,2,3,5,7,9,10},  /* 2 dorian */
    {0,2,4,5,7,9,10},  /* 3 mixolydian */
    {0,1,2,3,4,5,6,7,8,9,10,11}, /* 4 chromatic (handled specially) */
};
static const int wb_scale_sizes[] = {7,7,7,7,12};

int wb_scale_contains(int scale_root, int scale_type, int note) {
    if (note < 0 || note > 127) return 0;
    int pc = note % 12;
    int root = scale_root % 12; if (root < 0) root += 12;
    int rel = pc - root; if (rel < 0) rel += 12;
    if (scale_type == 4) return 1;  /* chromatic: everything in */
    if (scale_type < 0 || scale_type > 3) return 0;
    const int *steps = wb_scale_steps[scale_type];
    int n = wb_scale_sizes[scale_type];
    for (int i = 0; i < n; i++) if (steps[i] == rel) return 1;
    return 0;
}

int wb_scale_snap(int scale_root, int scale_type, int note) {
    if (note < 0) note = 0; if (note > 127) note = 127;
    if (wb_scale_contains(scale_root, scale_type, note)) return note;
    /* walk outward to the nearest in-scale pitch class */
    for (int d = 1; d <= 12; d++) {
        if (wb_scale_contains(scale_root, scale_type, note - d)) return note - d;
        if (wb_scale_contains(scale_root, scale_type, note + d)) return note + d;
    }
    return note;  /* fallback (shouldn't happen) */
}

/* G81: chord tones from root + scale. mode: 0=off, 1=triad, 2=7th, 3=9th.
 * All tones derived diatonically from the scale table. Fills `out` (cap 8). */
int wb_chord_tones(int scale_root, int scale_type, int mode, int out[8]) {
    if (mode <= 0 || !out) return 0;
    int root = scale_root % 12; if (root < 0) root += 12;
    if (scale_type < 0 || scale_type > 3) return 0;
    const int *iv = wb_scale_steps[scale_type];
    int n = wb_scale_sizes[scale_type];
    int cnt = 0;
    /* triad core: root, 3rd, 5th (scale degrees 0, 2, 4) */
    int thirds[] = { 0, 2, 4 };
    for (int i = 0; i < 3 && cnt < 8; i++) {
        int deg = thirds[i] % n;
        out[cnt++] = root + iv[deg];
    }
    if (mode >= 2 && cnt < 8) {                              /* 7th */
        out[cnt++] = root + iv[6 % n];
    }
    if (mode >= 3 && cnt < 8) {                              /* 9th = octave + 2nd */
        out[cnt++] = root + 12 + iv[1 % n];
    }
    return cnt;
}

/* Map a named state color to its RGB triple (each channel 0..63).
 * This is the public mirror of the internal lp_color_rgb() used by wb_lp_mk2_led. */
void wb_lp_color_rgb(wb_lp_color c, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (!r || !g || !b) return;
    lp_color_rgb(c, r, g, b);
}
