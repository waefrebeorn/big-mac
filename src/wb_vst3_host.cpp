// src/wb_vst3_host.cpp — VST3 plugin host (C++ shim, exposes C11 API)
//
// Big Mac is pure C11. VST3 SDK is C++. This shim bridges them: it owns
// VST3 SDK objects and exposes a C11-compatible API that the C engine calls.
//
// DESIGN:
//   - All VST3 objects owned by C++, ref-counted via S::IPtr (smartpointer.h).
//   - C side holds opaque void* (wb_vst3_handle) calling the C11 API declared
//     in include/wbus/wbus_vst3.h. Every VST3 object held in S::IPtr.
//   - Loads .vst3 bundles via VST3::Hosting::Module::create(); enumerates
//     classes via PluginFactory::classInfos(); creates instances via
//     PluginFactory::createInstance<T>(uid).
//   - Process: build ProcessData with AudioBusBuffers pointing at Big Mac's
//     float* buffers, call IAudioProcessor::process(data).
//   - Params: IEditController::getParameterCount/getParameter/setParameter.
//   - Bypass: IComponent::setBypass (optional, best-effort).

#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <dirent.h>
#include <stdexcept>

extern "C" {
#include "wbus.h"
#include "wbus_vst3.h"
}

// ---- VST3 SDK headers (vendored) ---------------------------------------
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/fplatform.h"
#include "pluginterfaces/base/fstrdefs.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

namespace Vst  = Steinberg::Vst;
namespace S    = Steinberg;
namespace Host = VST3::Hosting;
using Steinberg::int32;

// ---- Big Mac VST3 instance (owned by C++) -------------------------------
struct wb_vst3_inst {
    Steinberg::IPtr<Vst::IComponent>         component;
    Steinberg::IPtr<Vst::IAudioProcessor>    processor;
    Steinberg::IPtr<Vst::IEditController>    controller;
    std::shared_ptr<Host::Module>            module;
    std::string                              name;
    std::string                              vendor;
    std::string                              category;
    Steinberg::TUID                          classId;
    double                                  sampleRate;
    // Pending parameter changes to be delivered on the next process() call.
    // VST3 requires param changes to arrive via ProcessData.inputParameterChanges
    // (the processor only sees automation through the processing call).
    std::vector<std::pair<Vst::ParamID, Vst::ParamValue>> pendingParams;
};

// ---- global registry -----------------------------------------------------
static std::vector<std::shared_ptr<Host::Module>> g_modules;
static std::vector<std::string>                    g_plugin_dirs;
static std::vector<std::string>                    g_plugin_names;
static bool                                        g_scanned = false;
static std::string                                 g_error_msg;

// ---- minimal IUnknown for initialize() hostContext ----------------------
class CompilerUnknown : public S::FUnknown {
    Steinberg::uint32 _refcount = 0;
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        (void)obj;
        if (S::FUnknownPrivate::iidEqual(iid, S::FUnknown::iid)) { addRef(); return S::kResultOk; }
        return S::kResultFalse;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { ++_refcount; return _refcount; }
    Steinberg::uint32 PLUGIN_API release() override {
        if (--_refcount == 0) { delete this; return 0; }
        return _refcount;
    }
    virtual ~CompilerUnknown() {}
};

// ---- scan a directory for .vst3 bundles --------------------------------
static void scanDir(const char *dirPath) {
    if (!dirPath) return;
    for (auto &d : g_plugin_dirs) if (d == dirPath) return;
    g_plugin_dirs.push_back(dirPath);

    DIR *dir = opendir(dirPath);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (name.size() < 5 || name.substr(name.size() - 5) != ".vst3") continue;
        std::string bundlePath = std::string(dirPath) + "/" + name;

        std::string modulePath;
#if defined(__APPLE__)
        modulePath = bundlePath + "/Contents/MacOS/" + name.substr(0, name.size() - 5);
#else
        modulePath = bundlePath + "/Contents/Plugins/" + name;
#endif
        try {
            std::string err;
            auto m = Host::Module::create(modulePath, err);
            if (m) g_modules.push_back(m);
        } catch (...) {
            // skip unloadable modules silently
        }
    }
    closedir(dir);
}

// ---- collect class names from all modules ------------------------------
static void collectClasses(std::vector<std::string> &names) {
    names.clear();
    for (auto &m : g_modules) {
        const Host::PluginFactory &pf = m->getFactory();
        for (auto &ci : pf.classInfos()) {
            std::string cat = ci.category();
            if (cat != "" && cat != "AudioEffect" && cat != "Instrument" &&
                cat != "Audio Module" && cat != "Fx" && cat != "MidiEffect")
                continue;
            names.push_back(ci.name());
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
}

// ---- internal: create by name -----------------------------------------
static void* createByName(const char *name) {
    if (!name || !*name) return nullptr;
    if (g_modules.empty()) {
        scanDir("/Library/Audio/Plug-Ins/VST3");
        scanDir("/Users/waefrebeorn/Library/Audio/Plug-Ins/VST3");
        scanDir("./vst3_plugins");
    }

    for (auto &m : g_modules) {
        const Host::PluginFactory &pf = m->getFactory();
        for (auto &ci : pf.classInfos()) {
            if (ci.name() != std::string(name)) continue;

            Steinberg::IPtr<Vst::IComponent> comp = pf.createInstance<Vst::IComponent>(ci.ID());
            if (!comp) return nullptr;

            Steinberg::IPtr<Vst::IAudioProcessor> proc = pf.createInstance<Vst::IAudioProcessor>(ci.ID());
            Steinberg::IPtr<Vst::IEditController> ctl  = pf.createInstance<Vst::IEditController>(ci.ID());

            wb_vst3_inst *v = new wb_vst3_inst;
            v->module     = m;
            v->name       = ci.name();
            v->vendor     = ci.vendor();
            v->category   = ci.category();
            std::memcpy(v->classId, ci.ID().data(), sizeof(Steinberg::TUID));
            v->component  = comp;
            v->processor  = proc;
            v->controller = ctl;
            v->sampleRate = 44100.0;

            // initialize + activate
            CompilerUnknown *ctx = new CompilerUnknown();
            comp->initialize(ctx);
            comp->setActive(S::TBool(true));
            delete ctx;

            if (proc) {
                Vst::SpeakerArrangement inArr  = Vst::SpeakerArr::kStereo;
                Vst::SpeakerArrangement outArr = Vst::SpeakerArr::kStereo;
                proc->setBusArrangements(&inArr, 1, &outArr, 1);
                Vst::ProcessSetup setup{};
                setup.processMode         = Vst::kRealtime;
                setup.symbolicSampleSize  = Vst::kSample32;
                setup.maxSamplesPerBlock  = 256;
                setup.sampleRate          = v->sampleRate;
                proc->setupProcessing(setup);
                proc->setProcessing(S::TBool(true));
            }
            return (void*)v;
        }
    }
    return nullptr;
}

// ---- C11 API -----------------------------------------------------------
extern "C" int   wb_vst3_scan(const char *dirPath) {
    if (!dirPath || !*dirPath) {
        scanDir("/Library/Audio/Plug-Ins/VST3");
        scanDir("/Users/waefrebeorn/Library/Audio/Plug-Ins/VST3");
        scanDir("./vst3_plugins");
    } else {
        scanDir(dirPath);
    }
    collectClasses(g_plugin_names);
    g_scanned = true;
    return (int)g_plugin_names.size();
}

extern "C" int   wb_vst3_plugin_count(void) {
    return (int)g_plugin_names.size();
}

extern "C" const char* wb_vst3_plugin_name(int i) {
    if (i < 0 || i >= (int)g_plugin_names.size()) return nullptr;
    return g_plugin_names[i].c_str();
}

extern "C" void*  wb_vst3_create(const char *name, uint32_t sample_rate) {
    (void)sample_rate;
    void *h = createByName(name);
    if (!h) {
        g_error_msg = "wb_vst3_create: plugin '" + std::string(name ? name : "") +
                      "' not found (run wb_vst3_scan first)";
    }
    return h;
}

extern "C" void   wb_vst3_destroy(void *h) {
    if (!h) return;
    wb_vst3_inst *v = (wb_vst3_inst *)h;
    v->controller.reset();
    v->processor.reset();
    v->component.reset();
    v->module.reset();
    delete v;
}

extern "C" int    wb_vst3_set_sample_rate(void *h, uint32_t sr) {
    if (!h) return -1;
    wb_vst3_inst *v = (wb_vst3_inst *)h;
    v->sampleRate = sr;
    if (v->processor) {
        Vst::ProcessSetup setup{};
        setup.processMode         = Vst::kRealtime;
        setup.symbolicSampleSize  = Vst::kSample32;
        setup.maxSamplesPerBlock  = 256;
        setup.sampleRate          = sr;
        v->processor->setupProcessing(setup);
        return 0;
    }
    return -1;
}

extern "C" int    wb_vst3_process(void *h, const float *inL, const float *inR,
                                    float *outL, float *outR, uint32_t n) {
    if (!h) return -1;
    wb_vst3_inst *v = (wb_vst3_inst *)h;
    if (!v->component || !v->processor) return -1;
    if (n == 0) return 0;

    int32 numIn  = v->component->getBusCount(Vst::kAudio, Vst::kInput);
    int32 numOut = v->component->getBusCount(Vst::kAudio, Vst::kOutput);
    if (numIn < 1 || numOut < 1) return -1;

    Vst::AudioBusBuffers *inBufs  = new Vst::AudioBusBuffers[numIn];
    Vst::AudioBusBuffers *outBufs = new Vst::AudioBusBuffers[numOut];
    if (!inBufs || !outBufs) { delete[] inBufs; delete[] outBufs; return -1; }
    std::memset(inBufs, 0, sizeof(Vst::AudioBusBuffers) * numIn);
    std::memset(outBufs, 0, sizeof(Vst::AudioBusBuffers) * numOut);

    Vst::Sample32 *inChans[2] = { (Vst::Sample32*)inL, (Vst::Sample32*)inR };
    Vst::Sample32 *outChans[2] = { (Vst::Sample32*)outL, (Vst::Sample32*)outR };

    for (int32 i = 0; i < numIn; i++) {
        inBufs[i].numChannels = 2;
        inBufs[i].channelBuffers32 = inChans;
    }
    for (int32 i = 0; i < numOut; i++) {
        outBufs[i].numChannels = 2;
        outBufs[i].channelBuffers32 = outChans;
    }

    Vst::ProcessData data;
    std::memset(&data, 0, sizeof(data));
    data.numSamples         = (int32)n;
    data.numInputs          = numIn;
    data.numOutputs         = numOut;
    data.inputs             = inBufs;
    data.outputs            = outBufs;
    data.symbolicSampleSize = Vst::kSample32;
    data.processMode        = Vst::kRealtime;

    // Deliver pending parameter changes to the processor via the processing call
    // (VST3: the processor only observes automation through ProcessData).
    Vst::ParameterChanges paramChanges;
    if (!v->pendingParams.empty()) {
        for (auto &pp : v->pendingParams) {
            int32 qidx = 0;
            Vst::IParamValueQueue *q = paramChanges.addParameterData(pp.first, qidx);
            if (q) {
                int32 pidx = 0;
                q->addPoint(0, pp.second, pidx);
            }
        }
        v->pendingParams.clear();
        data.inputParameterChanges = &paramChanges;
    }

    Steinberg::tresult rc = v->processor->process(data);

    delete[] inBufs;
    delete[] outBufs;
    return (rc == S::kResultOk) ? 0 : -1;
}

extern "C" float  wb_vst3_get_param(void *h, int param_index) {
    if (!h) return 0.0f;
    wb_vst3_inst *v = (wb_vst3_inst *)h;
    if (!v->controller) return 0.0f;
    return (float)v->controller->getParamNormalized((Vst::ParamID)param_index);
}

extern "C" int    wb_vst3_set_param(void *h, int param_index, float value) {
    if (!h) return -1;
    wb_vst3_inst *v = (wb_vst3_inst *)h;
    if (!v->controller) return -1;
    Vst::ParamID pid = (Vst::ParamID)param_index;
    Vst::ParamValue pv = (Vst::ParamValue)value;
    // Host side: push the normalized value to the controller (GUI sync per VST3 spec).
    v->controller->setParamNormalized(pid, pv);
    // Queue for delivery to the processor on the next process() call.
    v->pendingParams.push_back({pid, pv});
    return 0;
}

extern "C" int    wb_vst3_get_info(void *h,
                                    char *name,  int name_sz,
                                    char *vendor, int vendor_sz,
                                    char *category, int cat_sz) {
    if (!h) {
        if (name && name_sz > 0) name[0] = '\0';
        if (vendor && vendor_sz > 0) vendor[0] = '\0';
        if (category && cat_sz > 0) category[0] = '\0';
        return -1;
    }
    wb_vst3_inst *v = (wb_vst3_inst *)h;
    auto cp = [&](const std::string &s, char *dst, int sz) {
        if (!dst || sz <= 0) return;
        int c = (int)s.size(); if (c >= sz) c = sz - 1;
        std::memcpy(dst, s.data(), c); dst[c] = '\0';
    };
    cp(v->name, name, name_sz);
    cp(v->vendor, vendor, vendor_sz);
    cp(v->category, category, cat_sz);
    return 0;
}

extern "C" const char* wb_vst3_error(void) {
    return g_error_msg.empty() ? nullptr : g_error_msg.c_str();
}
