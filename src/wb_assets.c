/* wb_assets.c — asset library index + cache (R057). */

#include "wbus/wbus_assets.h"
#include "wbus/wbus_gltf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#define WB_ASSETS_MAX_KITS 64
#define WB_ASSETS_MAX_MODELS 512
#define WB_ASSETS_NAME 128

typedef struct {
    char name[WB_ASSETS_NAME];
    /* models */
    char (*models)[WB_ASSETS_NAME];
    int nmodels, capmodels;
} wb_kit;

struct wb_assets {
    char root[1024];
    wb_kit kits[WB_ASSETS_MAX_KITS];
    int nkits;

    /* cache */
    struct {
        char key[2*WB_ASSETS_NAME+2];
        wb_mesh *mesh;
    } cache[64];
    int ncache;
};

wb_assets *wb_assets_open(const char *root) {
    DIR *d = opendir(root);
    if (!d) return NULL;
    closedir(d);
    wb_assets *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    snprintf(a->root, sizeof a->root, "%s", root);

    d = opendir(root);
    struct dirent *e;
    while ((e = readdir(d)) && a->nkits < WB_ASSETS_MAX_KITS) {
        if (e->d_name[0] == '.') continue;
        char kitpath[1200];
        snprintf(kitpath, sizeof kitpath, "%s/%s", root, e->d_name);
        struct stat st;
        if (stat(kitpath, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        wb_kit *k = &a->kits[a->nkits];
        snprintf(k->name, WB_ASSETS_NAME, "%s", e->d_name);
        DIR *kd = opendir(kitpath);
        if (!kd) continue;
        struct dirent *ke;
        while ((ke = readdir(kd)) && k->nmodels < WB_ASSETS_MAX_MODELS) {
            const char *ext = strrchr(ke->d_name, '.');
            if (!ext || strcmp(ext, ".glb") != 0) continue;
            if (k->nmodels >= k->capmodels) {
                int nc = k->capmodels ? k->capmodels*2 : 16;
                char (*nm)[WB_ASSETS_NAME] =
                    realloc(k->models, (size_t)nc * sizeof(*nm));
                if (!nm) break;
                k->models = nm; k->capmodels = nc;
            }
            snprintf(k->models[k->nmodels], WB_ASSETS_NAME, "%s", ke->d_name);
            k->nmodels++;
        }
        closedir(kd);
        if (k->nmodels > 0) a->nkits++;
    }
    closedir(d);
    return a;
}

wb_assets *wb_assets_open_default(void) {
    const char *env = getenv("WB_ASSETS_ROOT");
    return wb_assets_open(env && env[0] ? env : "assets/kits");
}

void wb_assets_close(wb_assets *a) {
    if (!a) return;
    for (int i = 0; i < a->nkits; i++) free(a->kits[i].models);
    for (int i = 0; i < a->ncache; i++) wb_mesh_free(a->cache[i].mesh);
    free(a);
}

int  wb_assets_kit_count(const wb_assets *a) { return a ? a->nkits : 0; }
const char *wb_assets_kit_name(const wb_assets *a, int kit) {
    if (!a || kit < 0 || kit >= a->nkits) return NULL;
    return a->kits[kit].name;
}
int  wb_assets_model_count(const wb_assets *a, int kit) {
    if (!a || kit < 0 || kit >= a->nkits) return 0;
    return a->kits[kit].nmodels;
}
const char *wb_assets_model_name(const wb_assets *a, int kit, int model) {
    if (!a || kit < 0 || kit >= a->nkits) return NULL;
    if (model < 0 || model >= a->kits[kit].nmodels) return NULL;
    return a->kits[kit].models[model];
}

int wb_assets_total(const wb_assets *a) {
    if (!a) return 0;
    int n = 0;
    for (int i = 0; i < a->nkits; i++) n += a->kits[i].nmodels;
    return n;
}

wb_mesh *wb_assets_load(wb_assets *a, const char *kit, const char *model) {
    if (!a || !kit || !model) return NULL;
    char key[2*WB_ASSETS_NAME+2];
    snprintf(key, sizeof key, "%s/%s", kit, model);
    for (int i = 0; i < a->ncache; i++)
        if (strcmp(a->cache[i].key, key) == 0)
            return a->cache[i].mesh;

    /* resolve actual file (append .glb if missing) */
    char path[1400];
    if (strrchr(model, '.'))
        snprintf(path, sizeof path, "%s/%s/%s", a->root, kit, model);
    else
        snprintf(path, sizeof path, "%s/%s/%s.glb", a->root, kit, model);

    wb_mesh *m = wb_gltf_load_glb_ex(path, 1.0f, 200, 200, 200);
    if (!m) return NULL;

    if (a->ncache < 64) {
        snprintf(a->cache[a->ncache].key, sizeof a->cache[0].key, "%s", key);
        a->cache[a->ncache].mesh = m;
        a->ncache++;
    }
    return m;
}
