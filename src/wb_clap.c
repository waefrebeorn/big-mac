/* wb_clap.c — CLAP plugin host (self-contained).
 *
 * Loads .clap shared libraries via dlopen, finds the "clap_entry" symbol,
 * calls clap_entry.init(), gets the plugin factory, enumerates descriptors,
 * instantiates a plugin, and bridges it into the wbus engine as a plugin.
 *
 * The minimal CLAP ABI structs are defined here (they are a stable public
 * contract; we "make our own" rather than pull the whole header tree). Only
 * the parts a host needs to load/scan/instantiate/process are included.
 *
 * Plugin lifetime (per spec):
 *   entry.init(path) -> factory.get_plugin_count / get_plugin_descriptor
 *   -> factory.create_plugin(desc) -> plugin.init() -> plugin.activate(sr,min,max)
 *   -> plugin.start_processing() -> process() x N -> stop_processing()
 *   -> deactivate() -> plugin.destroy() -> entry.deinit()
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <dirent.h>

#include "wbus.h"
#include "wbus_clap.h"

/* ---- minimal CLAP ABI (stable public contract) ------------------------ */
#define CLAP_VERSION_MAJOR 1
#define CLAP_VERSION_MINOR 2
#define CLAP_VERSION_REVISION 0
#define CLAP_ABI

typedef struct clap_version {
    uint32_t major, minor, revision;
} clap_version_t;

#define CLAP_VERSION { CLAP_VERSION_MAJOR, CLAP_VERSION_MINOR, CLAP_VERSION_REVISION }

typedef struct clap_plugin_descriptor {
    clap_version_t clap_version;
    const char *id, *name, *vendor, *url, *manual_url, *support_url, *version;
    const char *description;
    const char *const *features;
} clap_plugin_descriptor_t;

typedef struct clap_host clap_host_t;
typedef struct clap_plugin clap_plugin_t;
typedef struct clap_process clap_process_t;

typedef struct clap_host {
    clap_version_t clap_version;
    void *host_data;
    const void *(CLAP_ABI *get_extension)(const clap_host_t *, const char *);
    void (CLAP_ABI *request_restart)(const clap_host_t *);
    void (CLAP_ABI *request_process)(const clap_host_t *);
    void (CLAP_ABI *request_callback)(const clap_host_t *);
} clap_host_t;

typedef struct clap_plugin {
    const clap_plugin_descriptor_t *desc;
    void *plugin_data;
    bool (CLAP_ABI *init)(const clap_plugin_t *);
    void (CLAP_ABI *destroy)(const clap_plugin_t *);
    bool (CLAP_ABI *activate)(const clap_plugin_t *, double, uint32_t, uint32_t);
    void (CLAP_ABI *deactivate)(const clap_plugin_t *);
    bool (CLAP_ABI *start_processing)(const clap_plugin_t *);
    void (CLAP_ABI *stop_processing)(const clap_plugin_t *);
    void (CLAP_ABI *reset)(const clap_plugin_t *);
    const void *(CLAP_ABI *get_extension)(const clap_plugin_t *, const char *);
    void (CLAP_ABI *on_main_thread)(const clap_plugin_t *);
    void (CLAP_ABI *process)(const clap_plugin_t *, const clap_process_t *);
} clap_plugin_t;

typedef struct clap_plugin_factory {
    uint32_t (CLAP_ABI *get_plugin_count)(const struct clap_plugin_factory *);
    const clap_plugin_descriptor_t *(CLAP_ABI *get_plugin_descriptor)(const struct clap_plugin_factory *, uint32_t);
    const clap_plugin_t *(CLAP_ABI *create_plugin)(const struct clap_plugin_factory *, const clap_host_t *, const char *plugin_id);
} clap_plugin_factory_t;

typedef struct clap_plugin_entry {
    clap_version_t clap_version;
    bool (CLAP_ABI *init)(const char *plugin_path);
    void (CLAP_ABI *deinit)(void);
    const void *(CLAP_ABI *get_factory)(const char *factory_id);
} clap_plugin_entry_t;

typedef struct clap_audio_buffer {
    float **data32;
    uint32_t channel_count;
    uint32_t latency;
    uint64_t constant_mask;
} clap_audio_buffer_t;

typedef struct clap_event_header {
    uint32_t size;
    uint32_t time;
    uint16_t space_id;
    uint16_t type;
    uint32_t flags;
} clap_event_header_t;

typedef struct clap_event_note {
    clap_event_header_t header;
    int32_t note_id;
    int16_t port_index, channel, key;
    double velocity;
    double pitch;
} clap_event_note_t;

#define CLAP_EVENT_NOTE_ON  0
#define CLAP_EVENT_NOTE_OFF 1

typedef struct clap_input_events {
    uint32_t (CLAP_ABI *size)(const struct clap_input_events *);
    const clap_event_header_t *(CLAP_ABI *get)(const struct clap_input_events *, uint32_t);
} clap_input_events_t;

typedef struct clap_output_events {
    bool (CLAP_ABI *try_push)(const struct clap_output_events *, const clap_event_header_t *);
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

/* ---- implementation ---------------------------------------------------- */

struct wb_clap_lib {
    void *handle;
    clap_plugin_entry_t *entry;
    clap_plugin_factory_t *factory;
    char path[512];
    struct wb_clap_lib *next;
};

struct wb_clap_host {
    struct wb_clap_lib *libs;
    /* flat plugin index */
    struct { struct wb_clap_lib *lib; uint32_t idx; } *plugins;
    uint32_t nplugins;
    clap_host_t host;
};

struct wb_clap_plugin {
    const clap_plugin_t *plugin;
    clap_host_t *host;
    /* our deinterleaved stereo buffers */
    float *inL, *inR, *outL, *outR;
    float *dummy_in[2], *dummy_out[2];
    clap_audio_buffer_t inbuf[2], outbuf[2];
    clap_process_t proc;
    int active;
    uint32_t sr;
    int64_t steady;
};

/* ---- empty host callbacks --------------------------------------------- */
static const void *host_get_extension(const clap_host_t *h, const char *id) {
    (void)h; (void)id; return NULL;
}
static void host_request_restart(const clap_host_t *h) { (void)h; }
static void host_request_process(const clap_host_t *h) { (void)h; }
static void host_request_callback(const clap_host_t *h) { (void)h; }

/* ---- empty event lists ------------------------------------------------ */
static uint32_t ev_size(const clap_input_events_t *l) { (void)l; return 0; }
static const clap_event_header_t *ev_get(const clap_input_events_t *l, uint32_t i) {
    (void)l; (void)i; return NULL;
}
static bool ev_push(const clap_output_events_t *l, const clap_event_header_t *e) {
    (void)l; (void)e; return true;
}
static const clap_input_events_t g_input_events = { ev_size, ev_get };
static const clap_output_events_t g_output_events = { ev_push };

/* ---- loading ----------------------------------------------------------- */
static void scan_dir(wb_clap_host *h, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        const char *n = ent->d_name;
        size_t len = strlen(n);
        int is_ext = (len > 5 && strcmp(n+len-5, ".clap") == 0) ||
                     (len > 6 && strcmp(n+len-6, ".dylib") == 0) ||
                     (len > 3 && strcmp(n+len-3, ".so") == 0);
        if (!is_ext) continue;

        char full[600];
        snprintf(full, sizeof(full), "%s/%s", dir, n);

        void *handle = dlopen(full, RTLD_NOW | RTLD_LOCAL);
        if (!handle) { fprintf(stderr, "clap: dlopen %s: %s\n", full, dlerror()); continue; }

        /* entry symbol — on macOS the symbol is the storage of the pointer
         * (clap_entry is `const clap_plugin_entry_t *clap_entry`), so we
         * dereference it to get the struct, like LMMS/Ardour do. */
        void *sym = dlsym(handle, "clap_entry");
        if (!sym) { fprintf(stderr, "clap: no clap_entry in %s\n", full); dlclose(handle); continue; }
        clap_plugin_entry_t *entry = *(clap_plugin_entry_t **)sym;
        if (!entry->init || !entry->get_factory) { dlclose(handle); continue; }

        if (!entry->init(full)) { dlclose(handle); continue; }

        const clap_plugin_factory_t *factory =
            (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
        if (!factory || !factory->get_plugin_count || !factory->get_plugin_descriptor) {
            entry->deinit(); dlclose(handle); continue;
        }

        struct wb_clap_lib *lib = calloc(1, sizeof(*lib));
        lib->handle = handle;
        lib->entry = entry;
        lib->factory = (clap_plugin_factory_t *)factory;
        snprintf(lib->path, sizeof(lib->path), "%s", full);
        lib->next = h->libs;
        h->libs = lib;

        uint32_t np = factory->get_plugin_count(factory);
        for (uint32_t i = 0; i < np; i++) {
            const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, i);
            if (desc && desc->id && desc->name) {
                h->nplugins++;
                h->plugins = realloc(h->plugins, h->nplugins * sizeof(*h->plugins));
                h->plugins[h->nplugins-1].lib = lib;
                h->plugins[h->nplugins-1].idx = i;
            }
        }
    }
    closedir(d);
}

wb_clap_host *wb_clap_host_create(const char *search_path) {
    wb_clap_host *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->host.clap_version = (clap_version_t)CLAP_VERSION;
    h->host.host_data = h;
    h->host.get_extension = host_get_extension;
    h->host.request_restart = host_request_restart;
    h->host.request_process = host_request_process;
    h->host.request_callback = host_request_callback;

    char dirs[4][512]; int nd = 0;
    if (search_path) {
        snprintf(dirs[nd++], 512, "%s", search_path);
    } else {
        const char *home = getenv("HOME");
        if (home) snprintf(dirs[nd++], 512, "%s/.clap", home);
        snprintf(dirs[nd++], 512, "/usr/local/lib/clap");
        snprintf(dirs[nd++], 512, "/Library/Audio/Plug-Ins/CLAP");
    }
    for (int i = 0; i < nd; i++) scan_dir(h, dirs[i]);
    return h;
}

uint32_t wb_clap_host_plugin_count(wb_clap_host *h) { return h ? h->nplugins : 0; }

int wb_clap_host_plugin_info(wb_clap_host *h, uint32_t index,
                             const char **name, const char **id) {
    if (!h || index >= h->nplugins) return -1;
    struct wb_clap_lib *lib = h->plugins[index].lib;
    const clap_plugin_descriptor_t *desc =
        lib->factory->get_plugin_descriptor(lib->factory, h->plugins[index].idx);
    if (name) *name = desc->name;
    if (id)   *id   = desc->id;
    return 0;
}

/* ---- instantiate ------------------------------------------------------- */
wb_clap_plugin *wb_clap_plugin_create(wb_clap_host *h, uint32_t index, uint32_t sample_rate) {
    if (!h || index >= h->nplugins) return NULL;
    struct wb_clap_lib *lib = h->plugins[index].lib;
    const clap_plugin_descriptor_t *desc =
        lib->factory->get_plugin_descriptor(lib->factory, h->plugins[index].idx);
    const clap_plugin_t *plugin = lib->factory->create_plugin(lib->factory, &h->host, desc->id);
    if (!plugin) { fprintf(stderr, "clap: create_plugin(%s) failed\n", desc->id); return NULL; }
    if (!plugin->init) { return NULL; }
    if (!plugin->init(plugin)) { fprintf(stderr, "clap: plugin init failed\n"); return NULL; }

    wb_clap_plugin *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->plugin = plugin;
    p->host = &h->host;
    p->sr = sample_rate;
    p->inL  = calloc(1, WB_MAX_BLOCK * sizeof(float));
    p->inR  = calloc(1, WB_MAX_BLOCK * sizeof(float));
    p->outL = calloc(1, WB_MAX_BLOCK * sizeof(float));
    p->outR = calloc(1, WB_MAX_BLOCK * sizeof(float));
    p->dummy_in[0] = p->inL;  p->dummy_in[1] = p->inR;
    p->dummy_out[0] = p->outL; p->dummy_out[1] = p->outR;
    p->inbuf[0].data32 = p->dummy_in;  p->inbuf[0].channel_count = 2;
    p->inbuf[1].data32 = p->dummy_in;  p->inbuf[1].channel_count = 2;
    p->outbuf[0].data32 = p->dummy_out; p->outbuf[0].channel_count = 2;
    p->outbuf[1].data32 = p->dummy_out; p->outbuf[1].channel_count = 2;
    p->proc.audio_inputs = p->inbuf;
    p->proc.audio_outputs = p->outbuf;
    p->proc.audio_inputs_count = 1;
    p->proc.audio_outputs_count = 1;
    p->proc.in_events = &g_input_events;
    p->proc.out_events = &g_output_events;
    p->proc.transport = NULL;
    p->proc.steady_time = 0;

    if (plugin->activate) {
        if (!plugin->activate(plugin, (double)sample_rate, 1, WB_MAX_BLOCK)) {
            fprintf(stderr, "clap: activate failed\n");
            wb_clap_plugin_destroy(p); return NULL;
        }
    }
    p->active = 1;
    if (plugin->start_processing)
        plugin->start_processing(plugin);
    return p;
}

int wb_clap_plugin_process(wb_clap_plugin *p,
                           const float *inL, const float *inR,
                           float *outL, float *outR, uint32_t frames) {
    if (!p || !p->plugin->process) return -1;
    memcpy(p->inL, inL, frames*sizeof(float));
    memcpy(p->inR, inR, frames*sizeof(float));
    p->proc.frames_count = frames;
    p->proc.steady_time = p->steady;
    p->proc.audio_inputs = p->inbuf;
    p->proc.audio_outputs = p->outbuf;
    p->steady += frames;
    p->plugin->process(p->plugin, &p->proc);
    memcpy(outL, p->outL, frames*sizeof(float));
    memcpy(outR, p->outR, frames*sizeof(float));
    return 0;
}

void wb_clap_plugin_destroy(wb_clap_plugin *p) {
    if (!p) return;
    if (p->plugin) {
        if (p->plugin->stop_processing) p->plugin->stop_processing(p->plugin);
        if (p->plugin->deactivate) p->plugin->deactivate(p->plugin);
        p->plugin->destroy(p->plugin);
    }
    free(p->inL); free(p->inR); free(p->outL); free(p->outR);
    free(p);
}

void wb_clap_host_destroy(wb_clap_host *h) {
    if (!h) return;
    struct wb_clap_lib *lib = h->libs;
    while (lib) {
        struct wb_clap_lib *nxt = lib->next;
        if (lib->entry) lib->entry->deinit();
        if (lib->handle) dlclose(lib->handle);
        free(lib);
        lib = nxt;
    }
    free(h->plugins);
    free(h);
}
