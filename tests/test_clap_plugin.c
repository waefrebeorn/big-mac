/* test_clap_plugin.c — a minimal real CLAP plugin, built as a .dylib.
 * Used ONLY to verify the Big Mac CLAP host end-to-end. It implements a
 * gain plugin (1st factory plugin) and a lowpass (2nd). This is a genuine
 * CLAP-ABI dso: exports clap_entry, has init/deinit/get_factory.
 *
 * Build: cc -shared -fPIC -o test_clap_plugin.clap test_clap_plugin.c
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define CLAP_VERSION_MAJOR 1
#define CLAP_VERSION_MINOR 2
#define CLAP_VERSION_REVISION 0
#define CLAP_ABI

typedef struct clap_version { uint32_t major, minor, revision; } clap_version_t;
#define CLAP_VERSION { CLAP_VERSION_MAJOR, CLAP_VERSION_MINOR, CLAP_VERSION_REVISION }

/* ---- minimal descriptors used by the host + this plugin --------------- */
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
    void *(*on_main_thread)(const clap_plugin_t *);
    void *(*process)(const clap_plugin_t *, const void *);
    const void *(*get_plugin_info)(const clap_plugin_t *);
    void *(*get_parameter)(const clap_plugin_t *, uint32_t, void *);
} clap_plugin_t;

typedef struct clap_plugin_factory {
    uint32_t (*get_plugin_count)(const struct clap_plugin_factory *);
    const clap_plugin_descriptor_t *(*get_plugin_descriptor)(const struct clap_plugin_factory *, uint32_t);
    const clap_plugin_t *(*create_plugin)(const struct clap_plugin_factory *, const clap_host_t *, const char *);
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

typedef struct clap_event_header {
    uint32_t size, time;
    uint16_t space_id, type;
    uint32_t flags;
} clap_event_header_t;

typedef struct clap_input_events {
    uint32_t (*size)(const struct clap_input_events *);
    const clap_event_header_t *(*get)(const struct clap_input_events *, uint32_t);
} clap_input_events_t;

typedef struct clap_output_events {
    bool (*try_push)(const struct clap_output_events *, const clap_event_header_t *);
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

/* ---- plugin implementations ------------------------------------------- */
typedef struct gain_inst { double gain; } gain_inst;
typedef struct lp_inst { double cutoff; double sr; double stateL, stateR; } lp_inst;

static const char *g_features[] = { "audio-effect", NULL };
static const char *l_features[] = { "audio-effect", "filter", NULL };

static const clap_plugin_descriptor_t gain_desc = {
    CLAP_VERSION, "org.bigmac.test.gain", "BigMac Test Gain", "Big Mac",
    NULL, NULL, NULL, "1.0.0", "test gain", g_features
};
static const clap_plugin_descriptor_t lp_desc = {
    CLAP_VERSION, "org.bigmac.test.lowpass", "BigMac Test Lowpass", "Big Mac",
    NULL, NULL, NULL, "1.0.0", "test lowpass", l_features
};

static bool p_init(const clap_plugin_t *p) { (void)p; return true; }
static void p_destroy(const clap_plugin_t *p) { free(p->plugin_data); }
static bool p_activate(const clap_plugin_t *p, double sr, uint32_t a, uint32_t b) {
    (void)a; (void)b;
    if (strcmp(p->desc->id, "org.bigmac.test.lowpass") == 0) {
        lp_inst *lp = p->plugin_data;
        lp->sr = sr; lp->cutoff = 1000.0; lp->stateL = lp->stateR = 0.0;
    }
    return true;
}
static void p_deactivate(const clap_plugin_t *p) { (void)p; }
static bool p_start(const clap_plugin_t *p) { (void)p; return true; }
static void p_stop(const clap_plugin_t *p) { (void)p; }
static void p_reset(const clap_plugin_t *p) { (void)p; }

static void *p_process(const clap_plugin_t *p, const void *proc_in) {
    const clap_process_t *proc = proc_in;
    if (proc->frames_count == 0) return NULL;
    float **in = proc->audio_inputs[0].data32;
    float **out = proc->audio_outputs[0].data32;
    for (uint32_t i = 0; i < proc->frames_count; i++) {
        if (strcmp(p->desc->id, "org.bigmac.test.gain") == 0) {
            gain_inst *g = p->plugin_data;
            out[0][i] = in[0][i] * (float)g->gain;
            out[1][i] = in[1][i] * (float)g->gain;
        } else {
            lp_inst *lp = p->plugin_data;
            double rc = 1.0 / (2.0 * 3.14159265 * lp->cutoff);
            double alpha = rc / (rc + 1.0 / lp->sr);
            /* stereo one-pole LPF, per-channel state */
            lp->stateL += alpha * ((double)in[0][i] - lp->stateL);
            out[0][i] = (float)lp->stateL;
            lp->stateR += alpha * ((double)in[1][i] - lp->stateR);
            out[1][i] = (float)lp->stateR;
        }
    }
    return NULL;
}

static const void *p_getext(const clap_plugin_t *p, const char *id) { (void)p; (void)id; return NULL; }
static void *p_onthread(const clap_plugin_t *p) { (void)p; return NULL; }
static const void *p_info(const clap_plugin_t *p) { (void)p; return NULL; }
static void *p_param(const clap_plugin_t *p, uint32_t i, void *o) { (void)p; (void)i; (void)o; return NULL; }

static clap_plugin_t make_plugin(const clap_plugin_descriptor_t *desc) {
    clap_plugin_t p;
    memset(&p, 0, sizeof(p));
    p.desc = desc;
    p.init = p_init;
    p.destroy = p_destroy;
    p.activate = p_activate;
    p.deactivate = p_deactivate;
    p.start_processing = p_start;
    p.stop_processing = p_stop;
    p.reset = p_reset;
    p.get_extension = p_getext;
    p.on_main_thread = p_onthread;
    p.process = p_process;
    p.get_plugin_info = p_info;
    p.get_parameter = p_param;
    return p;
}

static clap_plugin_t gain_plugin, lp_plugin;

static uint32_t fac_count(const clap_plugin_factory_t *f) { (void)f; return 2; }
static const clap_plugin_descriptor_t *fac_desc(const clap_plugin_factory_t *f, uint32_t i) {
    (void)f; return i == 0 ? &gain_desc : (i == 1 ? &lp_desc : NULL);
}
static const clap_plugin_t *fac_create(const clap_plugin_factory_t *f, const clap_host_t *h, const char *id) {
    (void)f; (void)h;
    if (strcmp(id, "org.bigmac.test.gain") == 0) {
        gain_plugin = make_plugin(&gain_desc);
        gain_inst *g = calloc(1, sizeof(gain_inst));
        g->gain = 0.5;
        gain_plugin.plugin_data = g;
        return &gain_plugin;
    }
    if (strcmp(id, "org.bigmac.test.lowpass") == 0) {
        lp_plugin = make_plugin(&lp_desc);
        lp_inst *l = calloc(1, sizeof(lp_inst));
        lp_plugin.plugin_data = l;
        return &lp_plugin;
    }
    return NULL;
}

static clap_plugin_factory_t g_factory = { fac_count, fac_desc, fac_create };

static bool entry_init(const char *path) { (void)path; return true; }
static void entry_deinit(void) {}
static const void *entry_factory(const char *id) {
    if (strcmp(id, "clap.plugin-factory") == 0) return &g_factory;
    return NULL;
}

static clap_plugin_entry_t g_entry = { CLAP_VERSION, entry_init, entry_deinit, entry_factory };

const clap_plugin_entry_t *clap_entry = &g_entry;
