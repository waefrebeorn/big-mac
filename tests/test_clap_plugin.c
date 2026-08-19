/* test_clap_plugin.c — Minimal CLAP test plugin (.clap dylib).

Exports TWO plugins via one factory:
  org.bigmac.test.gain  — gain 0.5x
  org.bigmac.test.lowpass — simple 1-pole lowpass, fc=1kHz

Host (wb_clap.c) loads via dlopen, finds `clap_entry` symbol
(const pointer-to-pointer), calls entry->init, then
get_factory("clap.plugin-factory"), enumerates + creates per-plugin.

All struct definitions must exactly match wb_clap.c (lines 33–132).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define CLAP_VERSION_MAJOR 1
#define CLAP_VERSION_MINOR 2
#define CLAP_VERSION_REVISION 0

typedef struct { uint32_t major, minor, revision; } clap_version_t;
#define CLAP_VERSION { CLAP_VERSION_MAJOR, CLAP_VERSION_MINOR, CLAP_VERSION_REVISION }

typedef struct clap_plugin_descriptor {
    clap_version_t clap_version;
    const char *id, *name, *vendor, *url, *manual_url, *support_url, *version;
    const char *description;
    const char *const *features;
} clap_plugin_descriptor_t;

typedef struct clap_host clap_host_t;
typedef struct clap_plugin clap_plugin_t;

typedef struct clap_host {
    clap_version_t clap_version;
    void *host_data;
    const void *(*get_extension)(const clap_host_t *, const char *);
    void (*request_restart)(const clap_host_t *);
    void (*request_process)(const clap_host_t *);
    void (*request_callback)(const clap_host_t *);
} clap_host_t;

typedef struct clap_plugin {
    const clap_plugin_descriptor_t *desc;
    void *plugin_data;
    bool (*init)(const clap_plugin_t *);
    void (*destroy)(const clap_plugin_t *);
    bool (*activate)(const clap_plugin_t *, double, uint32_t, uint32_t);
    void (*deactivate)(const clap_plugin_t *);
    bool (*start_processing)(const clap_plugin_t *);
    void (*stop_processing)(const clap_plugin_t *);
    void (*reset)(const clap_plugin_t *);
    const void *(*get_extension)(const clap_plugin_t *, const char *);
    void (*on_main_thread)(const clap_plugin_t *);
    void (*process)(const clap_plugin_t *, const void *);
} clap_plugin_t;

typedef struct clap_plugin_factory {
    uint32_t (*get_plugin_count)(const struct clap_plugin_factory *);
    const clap_plugin_descriptor_t *(*get_plugin_descriptor)(const struct clap_plugin_factory *, uint32_t);
    const clap_plugin_t *(*create_plugin)(const struct clap_plugin_factory *, const clap_host_t *, const char *plugin_id);
} clap_plugin_factory_t;

typedef struct clap_plugin_entry {
    clap_version_t clap_version;
    bool (*init)(const char *plugin_path);
    void (*deinit)(void);
    const void *(*get_factory)(const char *factory_id);
} clap_plugin_entry_t;

typedef struct clap_audio_buffer {
    float **data32;
    uint32_t channel_count;
    uint32_t latency;
    uint64_t constant_mask;
} clap_audio_buffer_t;

typedef struct clap_input_events {
    uint32_t (*size)(const struct clap_input_events *);
    const void *(*get)(const struct clap_input_events *, uint32_t);
} clap_input_events_t;

typedef struct clap_output_events {
    bool (*try_push)(const struct clap_output_events *, const void *);
} clap_output_events_t;

typedef struct clap_process {
    int64_t steady_time;
    uint32_t frames_count;
    const void *transport;
    const clap_audio_buffer_t *audio_inputs;
    clap_audio_buffer_t *audio_outputs;
    uint32_t audio_inputs_count;
    uint32_t audio_outputs_count;
    const clap_input_events_t *in_events;
    const clap_output_events_t *out_events;
} clap_process_t;

#define CLAP_PLUGIN_FACTORY_ID "clap.plugin-factory"

typedef struct {
    float gain;
    float lp_state_l;
    float lp_state_r;
    float sr;
} plugin_state_t;

/* ---- gain plugin ---- */
static const char gain_id[]    = "org.bigmac.test.gain";
static const char gain_name[]  = "Big Mac Test Gain";
static const char gain_vendor[] = "Big Mac";
static const char gain_ver[]   = "1.0.0";
static const char *gain_features[] = { NULL };

static const clap_plugin_descriptor_t gain_desc = {
    CLAP_VERSION,
    gain_id, gain_name, gain_vendor, NULL, NULL, NULL, gain_ver,
    "Gain plugin (0.5x)",
    gain_features
};

static bool gain_init(const clap_plugin_t *p) {
    plugin_state_t *st = (plugin_state_t*)calloc(1, sizeof(*st));
    if (!st) return false;
    st->gain = 0.5f;
    st->sr = 44100.0f;
    st->lp_state_l = 0.0f;
    st->lp_state_r = 0.0f;
    /* CLAP pattern: init() sets plugin_data via const cast */
    ((clap_plugin_t*)p)->plugin_data = st;
    return true;
}

static void gain_destroy(const clap_plugin_t *p) {
    plugin_state_t *st = (plugin_state_t*)p->plugin_data;
    if (st) free(st);
}

static bool gain_activate(const clap_plugin_t *p, double sr, uint32_t max_blocks, uint32_t block_size) {
    (void)max_blocks; (void)block_size;
    plugin_state_t *st = (plugin_state_t*)p->plugin_data;
    if (!st) return false;
    st->sr = (float)sr;
    st->gain = 0.5f;
    st->lp_state_l = 0.0f;
    st->lp_state_r = 0.0f;
    return true;
}

static void gain_deactivate(const clap_plugin_t *p) { (void)p; }
static bool gain_start_processing(const clap_plugin_t *p) { (void)p; return true; }
static void gain_stop_processing(const clap_plugin_t *p) { (void)p; }
static void gain_reset(const clap_plugin_t *p) {
    plugin_state_t *st = (plugin_state_t*)p->plugin_data;
    if (st) { st->lp_state_l = 0; st->lp_state_r = 0; }
}
static const void *gain_get_extension(const clap_plugin_t *p, const char *id) { (void)p; (void)id; return NULL; }
static void gain_on_main_thread(const clap_plugin_t *p) { (void)p; }

static void gain_process(const clap_plugin_t *p, const void *v) {
    const clap_process_t *proc = (const clap_process_t *)v;
    plugin_state_t *st = (plugin_state_t*)p->plugin_data;
    if (!st || !proc) return;
    const clap_audio_buffer_t *in = proc->audio_inputs;
    clap_audio_buffer_t *out = proc->audio_outputs;
    if (!in || !out || in->channel_count < 2 || out->channel_count < 2) return;
    float *inL = in->data32[0];
    float *inR = in->data32[1];
    float *outL = out->data32[0];
    float *outR = out->data32[1];
    float g = st->gain;
    uint32_t n = proc->frames_count;
    for (uint32_t i = 0; i < n; i++) {
        outL[i] = inL[i] * g;
        outR[i] = inR[i] * g;
    }
}

/* ---- factory ---- */
static const char lp_id[]    = "org.bigmac.test.lowpass";
static const char lp_name[]  = "Big Mac Test Lowpass";
static const char lp_vendor[] = "Big Mac";
static const char lp_ver[]   = "1.0.0";
static const char *lp_features[] = { NULL };

static const clap_plugin_descriptor_t lp_desc = {
    CLAP_VERSION,
    lp_id, lp_name, lp_vendor, NULL, NULL, NULL, lp_ver,
    "Simple 1-pole lowpass, fc=1kHz",
    lp_features
};

static float lp_step(float x, float *s, float fc, float sr) {
    float rc = 1.0f / (2.0f * (float)M_PI * fc);
    float dt = 1.0f / sr;
    float alpha = dt / (rc + dt);
    float y = *s + alpha * (x - *s);
    *s = y;
    return y;
}

static bool lp_init(const clap_plugin_t *p) {
    plugin_state_t *st = (plugin_state_t*)calloc(1, sizeof(*st));
    if (!st) return false;
    st->gain = 1.0f;
    st->sr = 44100.0f;
    st->lp_state_l = 0.0f;
    st->lp_state_r = 0.0f;
    ((clap_plugin_t*)p)->plugin_data = st;
    return true;
}

static void lp_destroy(const clap_plugin_t *p) {
    plugin_state_t *st = (plugin_state_t*)p->plugin_data;
    if (st) free(st);
}

static bool lp_activate(const clap_plugin_t *p, double sr, uint32_t max_blocks, uint32_t block_size) {
    (void)max_blocks; (void)block_size;
    plugin_state_t *st = (plugin_state_t*)p->plugin_data;
    if (!st) return false;
    st->sr = (float)sr;
    st->lp_state_l = 0.0f;
    st->lp_state_r = 0.0f;
    return true;
}

static void lp_deactivate(const clap_plugin_t *p) { (void)p; }
static bool lp_start_processing(const clap_plugin_t *p) { (void)p; return true; }
static void lp_stop_processing(const clap_plugin_t *p) { (void)p; }
static void lp_reset(const clap_plugin_t *p) {
    plugin_state_t *st = (plugin_state_t*)p->plugin_data;
    if (st) { st->lp_state_l = 0; st->lp_state_r = 0; }
}
static const void *lp_get_extension(const clap_plugin_t *p, const char *id) { (void)p; (void)id; return NULL; }
static void lp_on_main_thread(const clap_plugin_t *p) { (void)p; }

static void lp_process(const clap_plugin_t *p, const void *v) {
    const clap_process_t *proc = (const clap_process_t *)v;
    plugin_state_t *st = (plugin_state_t*)p->plugin_data;
    if (!st || !proc) return;
    const clap_audio_buffer_t *in = proc->audio_inputs;
    clap_audio_buffer_t *out = proc->audio_outputs;
    if (!in || !out || in->channel_count < 2 || out->channel_count < 2) return;
    float *inL = in->data32[0];
    float *inR = in->data32[1];
    float *outL = out->data32[0];
    float *outR = out->data32[1];
    float fc = 1000.0f;
    uint32_t n = proc->frames_count;
    for (uint32_t i = 0; i < n; i++) {
        outL[i] = lp_step(inL[i], &st->lp_state_l, fc, st->sr);
        outR[i] = lp_step(inR[i], &st->lp_state_r, fc, st->sr);
    }
}

/* ---- factory ---- */
static const clap_plugin_descriptor_t *all_descs[] = { &gain_desc, &lp_desc };

static uint32_t factory_get_count(const clap_plugin_factory_t *f) {
    (void)f; return 2;
}

static const clap_plugin_descriptor_t *factory_get_descriptor(const clap_plugin_factory_t *f, uint32_t i) {
    (void)f;
    if (i >= 2) return NULL;
    return all_descs[i];
}

static const clap_plugin_t *factory_create_plugin(const clap_plugin_factory_t *f, const clap_host_t *host, const char *plugin_id) {
    (void)host; (void)f;
    if (!plugin_id) return NULL;
    /* CLAP spec: create_plugin must return a FRESH plugin instance,
     * not a pointer to a static struct. The host calls init() which
     * writes plugin_data — static .rodata on macOS would SIGBUS. */
    clap_plugin_t *p = (clap_plugin_t*)calloc(1, sizeof(*p));
    if (!p) return NULL;
    if (strcmp(plugin_id, gain_id) == 0) {
        p->desc = &gain_desc;
        p->init = gain_init; p->destroy = gain_destroy;
        p->activate = gain_activate; p->deactivate = gain_deactivate;
        p->start_processing = gain_start_processing; p->stop_processing = gain_stop_processing;
        p->reset = gain_reset; p->get_extension = gain_get_extension;
        p->on_main_thread = gain_on_main_thread; p->process = gain_process;
    } else if (strcmp(plugin_id, lp_id) == 0) {
        p->desc = &lp_desc;
        p->init = lp_init; p->destroy = lp_destroy;
        p->activate = lp_activate; p->deactivate = lp_deactivate;
        p->start_processing = lp_start_processing; p->stop_processing = lp_stop_processing;
        p->reset = lp_reset; p->get_extension = lp_get_extension;
        p->on_main_thread = lp_on_main_thread; p->process = lp_process;
    } else {
        free(p);
        return NULL;
    }
    return p;
}

static const clap_plugin_factory_t factory = {
    factory_get_count,
    factory_get_descriptor,
    factory_create_plugin
};

/* ---- entry point ---- */
static bool entry_init(const char *plugin_path) {
    (void)plugin_path; return true;
}
static void entry_deinit(void) { }
static const void *entry_get_factory(const char *factory_id) {
    if (factory_id && strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0)
        return (const void*)&factory;
    return NULL;
}

static const clap_plugin_entry_t entry = {
    CLAP_VERSION,
    entry_init, entry_deinit, entry_get_factory
};

const clap_plugin_entry_t *clap_entry = &entry;
