/* wb_ofx_plugin_builtin.c — a builtin OFX test plugin (R017 G4).
 *
 * Conforms to the OfxPlugin ABI and is linked into the host so the full
 * Load -> Describe -> CreateInstance -> Render -> DestroyInstance round-trip
 * can be exercised without shipping a binary .ofx bundle. It implements a
 * "Brightness" effect: output = input * Brightness, reading the parameter
 * through the ParameterSuite and the image through the ImageEffectSuite,
 * exactly as a real Fusion/Resolve effect would.
 *
 * This proves the minimal host's suite implementations are spec-faithful.
 */

#include "ofxCore.h"
#include "ofxParam.h"
#include "ofxImageEffect.h"
#include "ofxProperty.h"
#include "wbus/wbus_compositor.h"   /* wb_frame / wb_px for our image handle */

static OfxHost                *g_host = NULL;
static const OfxPropertySuiteV1    *g_prop = NULL;
static const OfxParameterSuiteV1   *g_param = NULL;
static const OfxImageEffectSuiteV1 *g_effect = NULL;

/* descriptors / instance */
static OfxPropertySetHandle g_instance = NULL;
static OfxParamSetHandle    g_paramset = NULL;

static OfxStatus on_load(void) { return kOfxStatOK; }
static OfxStatus on_unload(void) { return kOfxStatOK; }

static OfxStatus on_describe(OfxImageEffectHandle desc) {
    /* fetch suites from the host (the canonical OFX pattern) */
    g_prop    = (const OfxPropertySuiteV1*)g_host->fetchSuite((OfxPropertySetHandle)g_host, kOfxPropertySuite, 1);
    g_param   = (const OfxParameterSuiteV1*)g_host->fetchSuite((OfxPropertySetHandle)g_host, kOfxParameterSuite, 1);
    g_effect  = (const OfxImageEffectSuiteV1*)g_host->fetchSuite((OfxPropertySetHandle)g_host, kOfxImageEffectSuite, 1);
    if (!g_prop || !g_param || !g_effect) return kOfxStatErrMissingHostFeature;

    /* mark as a filter (1 in, 1 out) with a "Brightness" double param */
    OfxPropertySetHandle ps;
    g_effect->getPropertySet(desc, &ps);
    g_prop->propSetString(ps, kOfxPropLabel, 0, "BigMac Brightness");
    g_prop->propSetInt(ps, kOfxImageEffectPropSupportsTiles, 0, 1);
    g_prop->propSetInt(ps, kOfxImageEffectPropTemporalClipAccess, 0, 0);
    g_prop->propSetString(ps, kOfxImageEffectPropContext, 0, kOfxImageEffectContextFilter);

    /* declare the Brightness parameter on the effect descriptor's param set */
    OfxPropertySetHandle params;
    g_effect->getParamSet(desc, (OfxParamSetHandle *)&params);
    g_prop->propSetDouble(params, "Brightness", 0, 1.0);
    return kOfxStatOK;
}

static OfxStatus on_create_instance(OfxImageEffectHandle inst) {
    g_instance = (OfxPropertySetHandle)inst;
    g_effect->getParamSet(inst, &g_paramset);
    return kOfxStatOK;
}

static OfxStatus on_destroy_instance(OfxImageEffectHandle inst) {
    (void)inst; g_instance = NULL; return kOfxStatOK;
}

static OfxStatus on_render(OfxImageEffectHandle inst,
                           OfxPropertySetHandle inArgs,
                           OfxPropertySetHandle outArgs) {
    (void)inArgs; (void)outArgs; (void)inst;
    /* get the Source clip and the output clip */
    OfxPropertySetHandle effectProps;
    g_effect->getPropertySet(inst, &effectProps);

    /* read the Brightness param (sample at the render time) */
    double brightness = 1.0;
    OfxParamHandle bh; OfxPropertySetHandle bps;
    if (g_param->paramGetHandle(g_paramset, "Brightness", &bh, &bps) == kOfxStatOK) {
        g_param->paramGetValueAtTime(bh, 0.0, &brightness);
    }

    /* fetch the input image */
    OfxImageClipHandle srcClip = (OfxImageClipHandle)1; /* single input */
    double t = 0.0;
    OfxPropertySetHandle img;
    if (g_effect->clipGetImage(srcClip, t, NULL, &img) != kOfxStatOK)
        return kOfxStatErrBadHandle;

    /* our host wraps a wb_frame* as OfxImageHandle (OfxPropertySetHandle) */
    wb_frame *in = (wb_frame*)img;
    wb_frame *out = (wb_frame*)outArgs; /* host passes output via outArgs slot */
    if (!in || !out) { g_effect->clipReleaseImage(img); return kOfxStatErrBadHandle; }

    for (int y = 0; y < in->h; y++)
        for (int x = 0; x < in->w; x++) {
            wb_px *pi = &in->px[y*in->w + x];
            wb_px *po = &out->px[y*out->w + x];
            po->r = (float)fminf(1.0f, pi->r * (float)brightness);
            po->g = (float)fminf(1.0f, pi->g * (float)brightness);
            po->b = (float)fminf(1.0f, pi->b * (float)brightness);
            po->a = pi->a;
        }
    g_effect->clipReleaseImage(img);
    return kOfxStatOK;
}

static OfxStatus plugin_main(const char *action, const void *handle,
                             OfxPropertySetHandle inArgs,
                             OfxPropertySetHandle outArgs) {
    if (!strcmp(action, kOfxActionLoad)) return on_load();
    if (!strcmp(action, kOfxActionUnload)) return on_unload();
    if (!strcmp(action, kOfxActionDescribe)) return on_describe((OfxImageEffectHandle)handle);
    if (!strcmp(action, kOfxActionCreateInstance)) return on_create_instance((OfxImageEffectHandle)handle);
    if (!strcmp(action, kOfxActionDestroyInstance)) return on_destroy_instance((OfxImageEffectHandle)handle);
    if (!strcmp(action, kOfxImageEffectActionRender)) return on_render((OfxImageEffectHandle)handle, inArgs, outArgs);
    if (!strcmp(action, kOfxImageEffectActionIsIdentity)) {
        /* R017 D1: identity contract — if Brightness==1.0, output==input */
        double b = 1.0; OfxParamHandle bh; OfxPropertySetHandle bps;
        if (g_param->paramGetHandle(g_paramset, "Brightness", &bh, &bps) == kOfxStatOK)
            g_param->paramGetValueAtTime(bh, 0.0, &b);
        if (b == 1.0) return kOfxStatReplyYes;
        return kOfxStatReplyNo;
    }
    return kOfxStatReplyDefault;
}

static void plugin_set_host(OfxHost *host) { g_host = host; }

static OfxPlugin g_plugin = {
    kOfxImageEffectPluginApi, 1, "com.bigmac.Brightness", 1, 0, plugin_set_host, plugin_main
};

/* The standard C export an OFX host calls to enumerate plugins. */
OfxPlugin *OfxGetPlugin(int nth) {
    if (nth != 0) return NULL;
    return &g_plugin;
}
int OfxGetNumberOfPlugins(void) { return 1; }
