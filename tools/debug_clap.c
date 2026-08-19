/* debug_clap.c — minimal dlopen test for bigmac-test.clap */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

typedef struct {
    uint32_t major, minor, revision;
} clap_version_t;

#define CLAP_PLUGIN_FACTORY_ID "clap.plugin-factory"

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

int main(int argc, char **argv) {
    const char *path = argv[1] ? argv[1] : "build/test-clap/bigmac-test.clap";
    printf("Loading: %s\n", path);

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    printf("dlopen OK\n");

    void *sym = dlsym(handle, "clap_entry");
    if (!sym) {
        fprintf(stderr, "dlsym clap_entry failed: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }
    printf("dlsym OK, sym=%p\n", sym);

    const clap_plugin_entry_t *entry = *(const clap_plugin_entry_t **)sym;
    printf("entry=%p\n", (void*)entry);
    if (!entry) {
        fprintf(stderr, "entry is NULL\n");
        dlclose(handle);
        return 1;
    }

    printf("entry->init=%p\n", (void*)entry->init);
    printf("entry->deinit=%p\n", (void*)entry->deinit);
    printf("entry->get_factory=%p\n", (void*)entry->get_factory);

    bool init_rc = entry->init(path);
    printf("entry->init(%s) = %d\n", path, init_rc);

    const void *factory_ptr = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    printf("get_factory(%s) = %p\n", CLAP_PLUGIN_FACTORY_ID, factory_ptr);

    if (!factory_ptr) {
        fprintf(stderr, "factory not found\n");
        entry->deinit();
        dlclose(handle);
        return 1;
    }

    const clap_plugin_factory_t *factory = (const clap_plugin_factory_t *)factory_ptr;
    printf("factory->get_plugin_count=%p\n", (void*)factory->get_plugin_count);
    printf("factory->get_plugin_descriptor=%p\n", (void*)factory->get_plugin_descriptor);
    printf("factory->create_plugin=%p\n", (void*)factory->create_plugin);

    uint32_t count = factory->get_plugin_count(factory);
    printf("plugin count: %u\n", count);

    for (uint32_t i = 0; i < count; i++) {
        const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, i);
        printf("  [%u] id=%s name=%s vendor=%s ver=%s\n",
               i, desc->id, desc->name, desc->vendor, desc->version);
    }

    entry->deinit();
    dlclose(handle);
    printf("Done.\n");
    return 0;
}
