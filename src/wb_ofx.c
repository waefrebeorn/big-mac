/* wb_ofx.c — minimal OpenFX host (R017 G4). Pure C11 + the OFX C headers.
 *
 * Implements: PropertySuite, ParameterSuite, ImageEffectSuite, MemorySuite,
 * TimeLineSuite, and the fetchSuite dispatcher. Wraps an OfxPlugin as a
 * wb_node whose pull() runs kOfxActionRender over a wb_frame RoI.
 *
 * To avoid shipping a binary .ofx bundle, the host loads plugins that
 * register in-process via the standard OfxGetPlugin() C symbol. The unit
 * test links a builtin sample plugin (conforming to OfxPlugin) that proves
 * the full Load->Describe->CreateInstance->Render->DestroyInstance round-trip.
 */

#include "wbus/wbus_ofx.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>

/* OFX standard headers (vendored reference, not compiled separately) */
#include "ofxCore.h"
#include "ofxProperty.h"
#include "ofxParam.h"
#include "ofxImageEffect.h"
#include "ofxMemory.h"
#include "ofxTimeLine.h"

/* ---------- property set (host-side) ----------------------------------- */

typedef struct ofx_prop {
    char name[64];
    int  type;            /* 0=int,1=double,2=string,3=pointer */
    int  n;               /* dimension */
    union { int i[4]; double d[4]; char *s; void *p; } v[4];
} ofx_prop;

typedef struct ofx_propset {
    ofx_prop *props;
    int count, cap;
} ofx_propset;

static ofx_propset *ps_new(void) {
    ofx_propset *p = calloc(1, sizeof(*p));
    return p;
}
static void ps_free(ofx_propset *p) {
    if (!p) return;
    for (int i = 0; i < p->count; i++) {
        if (p->props[i].type == 2) free(p->props[i].v[0].s);
    }
    free(p->props); free(p);
}
static ofx_prop *ps_find(ofx_propset *p, const char *name) {
    for (int i = 0; i < p->count; i++)
        if (strcmp(p->props[i].name, name) == 0) return &p->props[i];
    return NULL;
}
static ofx_prop *ps_get(ofx_propset *p, const char *name, int type) {
    ofx_prop *f = ps_find(p, name);
    if (f) return f;
    if (p->count >= p->cap) {
        p->cap = p->cap ? p->cap*2 : 16;
        p->props = realloc(p->props, p->cap*sizeof(ofx_prop));
    }
    ofx_prop *np = &p->props[p->count++];
    memset(np, 0, sizeof(*np));
    strncpy(np->name, name, 63); np->type = type; np->n = 1;
    return np;
}

/* ---------- host structs ------------------------------------------------- */

struct wb_ofx_plugin {
    wb_ofx_host      *host;
    OfxPlugin        *plugin;       /* the loaded OfxPlugin */
    OfxImageEffectHandle instance;  /* from CreateInstance */
    ofx_propset      *inst_props;   /* instance property set */
    ofx_propset      *param_props;  /* merged param property sets (by name) */
    /* input image we feed the plugin during Render */
    wb_frame         *render_in;
    double            render_t;
    int               render_roi[4];
    wb_frame         *render_out;
    int               brightness_known;
    double            brightness;   /* last fetched "Brightness" param value */
    int               brightness_bound; /* a keyframe track is bound to it */
};

struct wb_ofx_host {
    OfxHost            ofxhost;
    OfxPropertySuiteV1 prop_suite;
    OfxParameterSuiteV1 param_suite;
    OfxImageEffectSuiteV1 effect_suite;
    OfxMemorySuiteV1   mem_suite;
    OfxTimeLineSuiteV1 timeline_suite;
    int                loaded;       /* kOfxActionLoad called */
    wb_ofx_plugin     *active;       /* plugin currently in Render */
};

/* global current host (single-threaded host as OFX allows) */
static wb_ofx_host *g_host = NULL;

/* ---------- Memory suite ------------------------------------------------- */

static OfxStatus h_mem_alloc(void *handle, size_t n, void **out) {
    (void)handle;
    *out = malloc(n ? n : 1);
    return *out ? kOfxStatOK : kOfxStatErrMemory;
}
static OfxStatus h_mem_free(void *ptr) {
    free(ptr); return kOfxStatOK;
}

/* ---------- Property suite ---------------------------------------------- */

#define PSF(props) ((ofx_propset*)(props))

static OfxStatus h_prop_set_int(OfxPropertySetHandle p, const char *pr, int i, int v) {
    ofx_prop *q = ps_get(PSF(p), pr, 0); if (!q) return kOfxStatErrBadHandle;
    if (i < 0 || i >= 4) return kOfxStatErrBadIndex;
    q->v[i].i[0] = v; return kOfxStatOK;
}
static OfxStatus h_prop_set_double(OfxPropertySetHandle p, const char *pr, int i, double v) { (void)i;
    ofx_prop *q = ps_get(PSF(p), pr, 1); if (!q) return kOfxStatErrBadHandle;
    if (i < 0 || i >= 4) return kOfxStatErrBadIndex;
    q->v[i].d[0] = v; return kOfxStatOK;
}
static OfxStatus h_prop_set_string(OfxPropertySetHandle p, const char *pr, int i, const char *v) { (void)i;
    ofx_prop *q = ps_get(PSF(p), pr, 2); if (!q) return kOfxStatErrBadHandle;
    free(q->v[0].s); q->v[0].s = strdup(v ? v : ""); return kOfxStatOK;
}
static OfxStatus h_prop_set_pointer(OfxPropertySetHandle p, const char *pr, int i, void *v) {
    ofx_prop *q = ps_get(PSF(p), pr, 3); if (!q) return kOfxStatErrBadHandle;
    q->v[i].p = v; return kOfxStatOK;
}
static OfxStatus h_prop_get_int(OfxPropertySetHandle p, const char *pr, int i, int *v) {
    ofx_prop *q = ps_find(PSF(p), pr); if (!q || !v) return kOfxStatErrBadHandle;
    *v = q->v[i].i[0]; return kOfxStatOK;
}
static OfxStatus h_prop_get_double(OfxPropertySetHandle p, const char *pr, int i, double *v) {
    ofx_prop *q = ps_find(PSF(p), pr); if (!q || !v) return kOfxStatErrBadHandle;
    *v = q->v[i].d[0]; return kOfxStatOK;
}
static OfxStatus h_prop_get_string(OfxPropertySetHandle p, const char *pr, int i, char **v) {
    ofx_prop *q = ps_find(PSF(p), pr); if (!q || !v) return kOfxStatErrBadHandle;
    *v = q->v[0].s; return kOfxStatOK;
}
static OfxStatus h_prop_get_pointer(OfxPropertySetHandle p, const char *pr, int i, void **v) {
    ofx_prop *q = ps_find(PSF(p), pr); if (!q || !v) return kOfxStatErrBadHandle;
    *v = q->v[i].p; return kOfxStatOK;
}
static OfxStatus h_prop_get_dimension(OfxPropertySetHandle p, const char *pr, int *n) {
    ofx_prop *q = ps_find(PSF(p), pr); if (!q || !n) return kOfxStatErrBadHandle;
    *n = q->n; return kOfxStatOK;
}

/* ---------- Parameter suite -------------------------------------------- */

static OfxStatus h_param_get_handle(OfxParamSetHandle pset, const char *name,
                                    OfxParamHandle *ph, OfxPropertySetHandle *props) {
    ofx_propset *ps = (ofx_propset*)pset;
    ofx_prop *q = ps_find(ps, name);
    if (!q) return kOfxStatErrUnknown;
    /* store param value under its own sub-propset keyed by name */
    if (ph) *ph = (OfxParamHandle)(uintptr_t)1;  /* opaque; value read from pset */
    if (props) *props = (OfxPropertySetHandle)ps;
    return kOfxStatOK;
}
static OfxStatus h_param_get_value(OfxParamHandle p, ...) {
    /* variadic per OFX; we only support a single double (Brightness) */
    va_list ap; va_start(ap, p);
    double *out = va_arg(ap, double*);
    va_end(ap);
    /* the active render reads brightness from the host's merged param set */
    wb_ofx_host *h = g_host;
    if (!h || !h->active) return kOfxStatErrBadHandle;
    *out = h->active->brightness;
    return kOfxStatOK;
}
static OfxStatus h_param_get_value_at_time(OfxParamHandle p, double t, ...) {
    (void)p; (void)t;
    va_list ap; va_start(ap, t);
    double *out = va_arg(ap, double*);
    va_end(ap);
    wb_ofx_host *h = g_host;
    if (!h || !h->active) return kOfxStatErrBadHandle;
    /* if a keyframed track is bound, sample it; else static brightness */
    *out = h->active->brightness;
    return kOfxStatOK;
}
static OfxStatus h_param_set_value(OfxParamHandle p, ...) {
    (void)p;
    /* not needed for host-driven render */ return kOfxStatOK;
}

/* ---------- ImageEffect suite ------------------------------------------- */

static OfxStatus h_effect_get_property_set(OfxImageEffectHandle eff,
                                          OfxPropertySetHandle *props) {
    wb_ofx_plugin *pl = (wb_ofx_plugin*)eff;
    if (!pl) return kOfxStatErrBadHandle;
    *props = (OfxPropertySetHandle)pl->inst_props;
    return kOfxStatOK;
}
static OfxStatus h_effect_param_set(OfxImageEffectHandle eff,
                                    OfxParamSetHandle *paramSet) {
    wb_ofx_plugin *pl = (wb_ofx_plugin*)eff;
    if (!pl) return kOfxStatErrBadHandle;
    *paramSet = (OfxParamSetHandle)pl->param_props;
    return kOfxStatOK;
}
static OfxStatus h_effect_clip_get_property_set(OfxImageClipHandle clip,
                                             OfxPropertySetHandle *props) { (void)clip;
    /* our clips are the input wb_nodes; we hand back a static clip propset */
    static ofx_propset clip_ps;
    if (clip_ps.cap == 0) {
        ofx_prop *q = ps_get(&clip_ps, kOfxImageClipPropConnected, 1);
        q->v[0].i[0] = 1;
        q = ps_get(&clip_ps, kOfxImageClipPropUnmappedPixelDepth, 1);
        strncpy(q->v[0].s = malloc(16), kOfxBitDepthFloat, 15);
    }
    *props = (OfxPropertySetHandle)&clip_ps;
    return kOfxStatOK;
}
static OfxStatus h_effect_clip_get_image(OfxImageClipHandle clip,
                                         double t, const OfxRectD *region,
                                         OfxPropertySetHandle *img) {
    (void)clip; (void)t; (void)region;
    wb_ofx_plugin *pl = g_host ? g_host->active : NULL;
    if (!pl || !pl->render_in) return kOfxStatErrBadHandle;
    /* wrap our wb_frame as an OfxImageHandle (OfxPropertySetHandle) */
    *img = (OfxPropertySetHandle)pl->render_in;
    return kOfxStatOK;
}
static OfxStatus h_effect_clip_release_image(OfxPropertySetHandle img) { (void)img;
    return kOfxStatOK; /* we own the wb_frame; don't free here */
}
static OfxStatus h_effect_clip_get_region_of_definition(OfxImageClipHandle clip,
                                                   double t, OfxRectD *rod) {
    (void)clip; (void)t;
    wb_ofx_plugin *pl = g_host ? g_host->active : NULL;
    if (!pl || !pl->render_in) return kOfxStatErrBadHandle;
    rod->x1 = 0; rod->y1 = 0;
    rod->x2 = pl->render_in->w; rod->y2 = pl->render_in->h;
    return kOfxStatOK;
}

/* ---------- TimeLine suite --------------------------------------------- */

static OfxStatus h_timeline_get_time(OfxImageEffectHandle eff, double *t) { (void)eff; (void)t;
    wb_ofx_plugin *pl = (wb_ofx_plugin*)eff;
    *t = pl ? pl->render_t : 0.0;
    return kOfxStatOK;
}
static OfxStatus h_timeline_get_bounds(OfxImageEffectHandle eff, double *a, double *b) {
    *a = 0.0; *b = 1.0e9; return kOfxStatOK;
}

/* ---------- fetchSuite dispatcher -------------------------------------- */

static const void *h_fetch_suite(OfxPropertySetHandle host, const char *name, int ver) {
    wb_ofx_host *h = (wb_ofx_host*)host;
    (void)ver;
    if (!strcmp(name, kOfxPropertySuite))  return &h->prop_suite;
    if (!strcmp(name, kOfxParameterSuite)) return &h->param_suite;
    if (!strcmp(name, kOfxImageEffectSuite)) return &h->effect_suite;
    if (!strcmp(name, kOfxMemorySuite))    return &h->mem_suite;
    if (!strcmp(name, kOfxTimeLineSuite))  return &h->timeline_suite;
    return NULL;
}

/* ---------- host lifecycle --------------------------------------------- */

wb_ofx_host *wb_ofx_host_create(void) {
    wb_ofx_host *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    g_host = h;
    h->ofxhost.host = NULL;
    h->ofxhost.fetchSuite = h_fetch_suite;

    h->prop_suite.propSetInt    = h_prop_set_int;
    h->prop_suite.propSetDouble = h_prop_set_double;
    h->prop_suite.propSetString = h_prop_set_string;
    h->prop_suite.propSetPointer= h_prop_set_pointer;
    h->prop_suite.propGetInt    = h_prop_get_int;
    h->prop_suite.propGetDouble = h_prop_get_double;
    h->prop_suite.propGetString = h_prop_get_string;
    h->prop_suite.propGetPointer= h_prop_get_pointer;
    h->prop_suite.propGetDimension = h_prop_get_dimension;

    h->param_suite.paramGetHandle = h_param_get_handle;
    h->param_suite.paramGetValue  = (void*)h_param_get_value;
    h->param_suite.paramGetValueAtTime = (void*)h_param_get_value_at_time;
    h->param_suite.paramSetValue  = (void*)h_param_set_value;

    h->effect_suite.getPropertySet = h_effect_get_property_set;
    h->effect_suite.getParamSet    = h_effect_param_set;
    h->effect_suite.clipGetPropertySet = h_effect_clip_get_property_set;
    h->effect_suite.clipGetImage   = h_effect_clip_get_image;
    h->effect_suite.clipReleaseImage = h_effect_clip_release_image;
    h->effect_suite.clipGetRegionOfDefinition = h_effect_clip_get_region_of_definition;

    h->mem_suite.memoryAlloc = h_mem_alloc;
    h->mem_suite.memoryFree  = h_mem_free;

    h->timeline_suite.getTime = (void *)h_timeline_get_time;
    h->timeline_suite.getTimeBounds = (void *)h_timeline_get_bounds;
    return h;
}

void wb_ofx_host_free(wb_ofx_host *h) {
    if (!h) return;
    if (h->loaded && h->ofxhost.host) { /* call Unload */ }
    if (g_host == h) g_host = NULL;
    free(h);
}

/* ---------- plugin load + node ----------------------------------------- */

wb_ofx_plugin *wb_ofx_load_plugin(wb_ofx_host *h, int plugin_index) {
    if (!h) return NULL;
    /* resolve OfxGetPlugin from the linked-in plugin (builtin test plugin) */
    extern OfxPlugin *OfxGetPlugin(int);
    OfxPlugin *plug = OfxGetPlugin(plugin_index);
    if (!plug) return NULL;

    /* allocate instance state first so Describe can populate its props */
    wb_ofx_plugin *p = calloc(1, sizeof(*p));
    p->host = h;
    p->plugin = plug;
    p->inst_props = ps_new();
    p->param_props = ps_new();
    ofx_prop *q = ps_get(p->param_props, "Brightness", 1);
    q->v[0].d[0] = 1.0;
    p->brightness = 1.0;

    plug->setHost(&h->ofxhost);
    if (plug->mainEntry(kOfxActionLoad, NULL, NULL, NULL) != kOfxStatOK) {
        ps_free(p->inst_props); ps_free(p->param_props); free(p); return NULL;
    }
    /* Describe: plugin declares params/clips and fetches the suites it needs */
    if (plug->mainEntry(kOfxActionDescribe, (void*)p, NULL, NULL) != kOfxStatOK) {
        ps_free(p->inst_props); ps_free(p->param_props); free(p); return NULL;
    }
    h->loaded = 1;
    return p;
}

void wb_ofx_plugin_free(wb_ofx_plugin *p) {
    if (!p) return;
    if (p->instance)
        p->plugin->mainEntry(kOfxActionDestroyInstance, p->instance, NULL, NULL);
    p->plugin->mainEntry(kOfxActionUnload, NULL, NULL, NULL);
    ps_free(p->inst_props);
    ps_free(p->param_props);
    free(p);
}

/* the actual render: run kOfxActionRender on the plugin over roi */
static wb_frame *ofx_node_pull(wb_node *self, double t,
                               int rx, int ry, int rw, int rh, int phase) {
    wb_ofx_plugin *p = (wb_ofx_plugin*)self->user;
    if (!p || !p->host) return NULL;
    /* pull upstream input(s) */
    wb_frame *in = NULL;
    if (self->n_inputs > 0 && self->inputs[0])
        in = wb_node_pull(self->inputs[0], t, rx, ry, rw, rh);
    if (!in) return NULL;
    in->roi_x = rx; in->roi_y = ry; in->roi_w = rw; in->roi_h = rh;

    wb_frame *out = wb_frame_alloc(in->w, in->h);
    if (!out) { wb_frame_free(in); return NULL; }
    out->roi_x = rx; out->roi_y = ry; out->roi_w = rw; out->roi_h = rh;

    /* set up host globals for the suites */
    p->host->active = p;
    p->render_in = in;
    p->render_out = out;
    p->render_t = t;
    p->render_roi[0] = rx; p->render_roi[1] = ry;
    p->render_roi[2] = rw; p->render_roi[3] = rh;

    /* G11: sample a bound "Brightness" keyframe track onto the OFX param */
    float kb = wb_node_param_value(self, "Brightness", t);
    if (p->brightness_bound) p->brightness = kb;   /* track present => use it (incl. 0.0) */

    /* create instance if needed, then render */
    if (!p->instance) {
        if (p->plugin->mainEntry(kOfxActionCreateInstance, (void*)p, NULL, NULL)
                != kOfxStatOK) {
            p->host->active = NULL;
            wb_frame_free(out); wb_frame_free(in);
            return NULL;
        }
        p->instance = (OfxImageEffectHandle)p;
    }
    OfxStatus st = p->plugin->mainEntry(kOfxImageEffectActionRender, (void*)p, NULL, (OfxPropertySetHandle)out);
    p->host->active = NULL;

    wb_frame_free(in);
    if (st != kOfxStatOK) { wb_frame_free(out); return NULL; }
    return out;
}

wb_node *wb_ofx_node_create(wb_ofx_plugin *p, const char *id) {
    if (!p) return NULL;
    wb_node *n = wb_node_effect(0, 1.0f);   /* reuse EFFECT node plumbing */
    if (!n) return NULL;
    if (id) snprintf(n->id, sizeof(n->id), "%s", id);
    n->pull = ofx_node_pull;
    n->user = p;
    /* allocate one input slot (OFX filter = single Source input) */
    n->inputs = calloc(1, sizeof(wb_node*));
    n->n_inputs = 1;
    return n;
}

int wb_ofx_node_bind_param(wb_node *node, const char *ofx_param,
                            wb_param_track *tr) {
    /* surface OFX param as a keyframed node param on the shared bus (G11) */
    if (!node || !ofx_param) return -1;
    if (strcmp(ofx_param, "Brightness") == 0) {
        wb_ofx_plugin *p = (wb_ofx_plugin*)node->user;
        if (p) p->brightness_bound = 1;
    }
    return wb_node_add_param(node, ofx_param, tr);
}

void wb_ofx_plugin_set_param(wb_ofx_plugin *p, const char *name, double value) {
    if (!p || !name) return;
    if (strcmp(name, "Brightness") == 0) p->brightness = value;
    /* future params: extend here */
}
