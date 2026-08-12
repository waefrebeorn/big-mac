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

struct wb_midi {
    MIDIClientRef client;
    MIDIPortRef   in_port;
    MIDIEndpointRef source;      /* the source endpoint we opened */
    void (*on_event)(wb_midi_event, void *);
    void *userdata;
};

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
    if (m->in_port) { MIDIPortDispose(m->in_port); m->in_port = 0; }
    if (m->client)  { MIDIClientDispose(m->client); m->client = 0; }
    free(m);
}
