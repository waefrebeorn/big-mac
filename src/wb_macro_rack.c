/* wb_macro_rack.c — macro/parameter rack (Ableton Instrument Rack style).
 * A chain of units (synth -> filter -> comp -> delay -> reverb) with 8 macro
 * knobs, each bindable to any parameter on any unit. Macro value 0..1 maps to
 * the bound parameter's (min..max) range.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "wbus.h"
#include "wbus_dsp.h"
#include "wb_internal.h"

#define WB_RACK_MAX_UNITS 8
#define WB_RACK_MAX_MACROS 8
#define WB_RACK_MAX_BINDINGS 64 /* total macro->param bindings across all macros */

/* Unit type identifiers */
enum {
    UNIT_SYNTH = 0,
    UNIT_FILTER,
    UNIT_COMP,
    UNIT_DELAY,
    UNIT_REVERB,
    UNIT_COUNT
};

static const char *unit_type_names[] = {
    "synth", "filter", "comp", "delay", "reverb"
};

/* A binding: one macro -> one parameter on one unit */
typedef struct {
    int      active;
    int      unit_index;
    int      param_index;
    float    min_val;
    float    max_val;
} wb_rack_binding;

/* A macro knob */
typedef struct {
    float    value;       /* 0..1 */
    char     name[32];
    wb_rack_binding bindings[WB_RACK_MAX_BINDINGS];
    int      binding_count;
} wb_rack_macro;

/* A unit in the rack's chain */
typedef struct {
    int      type;        /* UNIT_* */
    void     *inst;       /* instance pointer from unit create */
    char     name[32];
    int      active;
} wb_rack_unit;

/* The rack implementation struct (opaque wb_rack handle in public API).
 * The typedef "wb_rack" is declared in wbus.h; here we complete the struct. */
struct wb_rack {
    uint32_t     sr;
    char         name[64];
    wb_rack_unit units[WB_RACK_MAX_UNITS];
    int          unit_count;
    wb_rack_macro macros[WB_RACK_MAX_MACROS];
    int          midi_in_enabled;
};

/* ---- filter unit wrapper (uses wb_biquad directly) ---- */
typedef struct {
    wb_biquad bq;
    int       type;       /* 0=lowpass,1=highpass,2=bandpass,3=notch */
    float     cutoff;
    float     resonance;
} wb_filter_unit;

static void *filter_create(uint32_t sr) {
    wb_filter_unit *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->type = 0;
    f->cutoff = 1000.0f;
    f->resonance = 0.7f;
    wb_biquad_init(&f->bq, (float)sr);
    wb_biquad_set(&f->bq, f->type, f->cutoff, f->resonance, 0.0f);
    return f;
}

static void filter_destroy(void *inst) { free(inst); }

static void filter_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n) {
    wb_filter_unit *f = inst;
    for (uint32_t i = 0; i < n; i++) {
        L[i] = wb_biquad_process(&f->bq, L[i]);
        R[i] = wb_biquad_process(&f->bq, R[i]);
    }
}

static void filter_set_param(void *inst, int param, float v) {
    wb_filter_unit *f = inst;
    switch (param) {
    case 0: /* cutoff */
        f->cutoff = v;
        wb_biquad_set(&f->bq, f->type, f->cutoff, f->resonance, 0.0f);
        break;
    case 1: /* resonance */
        f->resonance = v;
        wb_biquad_set(&f->bq, f->type, f->cutoff, f->resonance, 0.0f);
        break;
    case 2: /* type */
        f->type = (int)v;
        if (f->type < 0) f->type = 0;
        if (f->type > 3) f->type = 3;
        wb_biquad_set(&f->bq, f->type, f->cutoff, f->resonance, 0.0f);
        break;
    default: break;
    }
}

/* ---- parameter set dispatch per unit type ---- */
static void rack_set_unit_param(wb_rack *r, int unit_index, int param_index, float v) {
    if (unit_index < 0 || unit_index >= r->unit_count) return;
    wb_rack_unit *u = &r->units[unit_index];
    if (!u->active || !u->inst) return;

    switch (u->type) {
    case UNIT_SYNTH:
        wb_synth_set(u->inst, param_index, v);
        break;
    case UNIT_FILTER:
        filter_set_param(u->inst, param_index, v);
        break;
    case UNIT_COMP:
        wb_comp_set(u->inst, param_index, v);
        break;
    case UNIT_DELAY: {
        wb_delay_inst *d = (wb_delay_inst *)u->inst;
        switch (param_index) {
        case 0: d->time_ms = v; break;
        case 1: d->feedback = v; break;
        case 2: d->mix = v; break;
        default: break;
        }
        break;
    }
    case UNIT_REVERB: {
        wb_reverb_inst *rv = (wb_reverb_inst *)u->inst;
        switch (param_index) {
        case 0: rv->feedback = v; break;
        case 1: rv->mix = v; break;
        default: break;
        }
        break;
    }
    default: break;
    }
}

/* ---- public API ---- */

void *wb_rack_create(uint32_t sr, const char *name) {
    wb_rack *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->sr = sr;
    strncpy(r->name, name ? name : "Rack", sizeof(r->name) - 1);
    r->name[sizeof(r->name) - 1] = '\0';
    r->unit_count = 0;
    r->midi_in_enabled = 0;

    /* Initialize macros with default names and center value */
    for (int i = 0; i < WB_RACK_MAX_MACROS; i++) {
        r->macros[i].value = 0.5f;
        snprintf(r->macros[i].name, sizeof(r->macros[i].name), "Macro %d", i + 1);
        r->macros[i].binding_count = 0;
    }

    return (void *)r;
}

void wb_rack_destroy(void *rack) {
    wb_rack *r = (wb_rack *)rack;
    if (!r) return;
    for (int i = 0; i < r->unit_count; i++) {
        wb_rack_unit *u = &r->units[i];
        if (!u->active || !u->inst) continue;
        switch (u->type) {
        case UNIT_SYNTH:  wb_synth_destroy(u->inst); break;
        case UNIT_FILTER: filter_destroy(u->inst); break;
        case UNIT_COMP:   wb_comp_destroy(u->inst); break;
        case UNIT_DELAY:  wb_delay_destroy(u->inst); break;
        case UNIT_REVERB: wb_reverb_destroy(u->inst); break;
        default: break;
        }
    }
    free(r);
}

int wb_rack_add_unit(void *rack, const char *unit_type) {
    wb_rack *r = (wb_rack *)rack;
    if (!r || !unit_type) return -1;
    if (r->unit_count >= WB_RACK_MAX_UNITS) return -1;

    int type = -1;
    for (int i = 0; i < UNIT_COUNT; i++) {
        if (strcmp(unit_type, unit_type_names[i]) == 0) { type = i; break; }
    }
    if (type < 0) return -1;

    void *inst = NULL;
    switch (type) {
    case UNIT_SYNTH:  inst = wb_synth_create(r->sr); break;
    case UNIT_FILTER: inst = filter_create(r->sr); break;
    case UNIT_COMP:   inst = wb_comp_create(r->sr); break;
    case UNIT_DELAY:  inst = wb_delay_create(r->sr); break;
    case UNIT_REVERB: inst = wb_reverb_create(r->sr); break;
    default: return -1;
    }
    if (!inst) return -1;

    int idx = r->unit_count;
    wb_rack_unit *u = &r->units[idx];
    u->type = type;
    u->inst = inst;
    u->active = 1;
    strncpy(u->name, unit_type, sizeof(u->name) - 1);
    u->name[sizeof(u->name) - 1] = '\0';
    r->unit_count++;
    return idx;
}

int wb_rack_remove_unit(void *rack, int index) {
    wb_rack *r = (wb_rack *)rack;
    if (!r || index < 0 || index >= r->unit_count) return -1;
    wb_rack_unit *u = &r->units[index];
    if (!u->active) return -1;

    /* Destroy the unit instance */
    switch (u->type) {
    case UNIT_SYNTH:  wb_synth_destroy(u->inst); break;
    case UNIT_FILTER: filter_destroy(u->inst); break;
    case UNIT_COMP:   wb_comp_destroy(u->inst); break;
    case UNIT_DELAY:  wb_delay_destroy(u->inst); break;
    case UNIT_REVERB: wb_reverb_destroy(u->inst); break;
    default: break;
    }

    /* Shift remaining units down */
    for (int i = index; i < r->unit_count - 1; i++) {
        r->units[i] = r->units[i + 1];
    }
    r->unit_count--;
    memset(&r->units[r->unit_count], 0, sizeof(wb_rack_unit));

    /* Remove bindings that reference the removed unit, and decrement
     * bindings pointing to units after the removed one */
    for (int m = 0; m < WB_RACK_MAX_MACROS; m++) {
        wb_rack_macro *mac = &r->macros[m];
        int write = 0;
        for (int b = 0; b < mac->binding_count; b++) {
            wb_rack_binding *bd = &mac->bindings[b];
            if (bd->unit_index == index) continue; /* drop removed */
            if (bd->unit_index > index) bd->unit_index--; /* shift down */
            mac->bindings[write++] = *bd;
        }
        mac->binding_count = write;
    }

    return 0;
}

int wb_rack_unit_count(const void *rack) {
    const wb_rack *r = (const wb_rack *)rack;
    return r ? r->unit_count : 0;
}

void wb_rack_set_macro(void *rack, int macro_index, float value) {
    wb_rack *r = (wb_rack *)rack;
    if (!r || macro_index < 0 || macro_index >= WB_RACK_MAX_MACROS) return;
    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;

    wb_rack_macro *mac = &r->macros[macro_index];
    mac->value = value;

    /* Apply all bindings for this macro */
    for (int b = 0; b < mac->binding_count; b++) {
        wb_rack_binding *bd = &mac->bindings[b];
        if (!bd->active) continue;
        float mapped = bd->min_val + value * (bd->max_val - bd->min_val);
        rack_set_unit_param(r, bd->unit_index, bd->param_index, mapped);
    }
}

void wb_rack_set_macro_name(void *rack, int macro_index, const char *name) {
    wb_rack *r = (wb_rack *)rack;
    if (!r || macro_index < 0 || macro_index >= WB_RACK_MAX_MACROS || !name) return;
    strncpy(r->macros[macro_index].name, name, sizeof(r->macros[macro_index].name) - 1);
    r->macros[macro_index].name[sizeof(r->macros[macro_index].name) - 1] = '\0';
}

int wb_rack_bind_param(void *rack, int macro_index, int unit_index,
                       int param_index, float min_val, float max_val) {
    wb_rack *r = (wb_rack *)rack;
    if (!r) return -1;
    if (macro_index < 0 || macro_index >= WB_RACK_MAX_MACROS) return -1;
    if (unit_index < 0 || unit_index >= r->unit_count) return -1;
    if (param_index < 0) return -1;

    wb_rack_macro *mac = &r->macros[macro_index];
    if (mac->binding_count >= WB_RACK_MAX_BINDINGS) return -1;

    wb_rack_binding *bd = &mac->bindings[mac->binding_count];
    bd->active = 1;
    bd->unit_index = unit_index;
    bd->param_index = param_index;
    bd->min_val = min_val;
    bd->max_val = max_val;
    mac->binding_count++;

    /* Immediately apply current macro value to the bound parameter */
    float mapped = min_val + mac->value * (max_val - min_val);
    rack_set_unit_param(r, unit_index, param_index, mapped);

    return 0;
}

void wb_rack_process(void *rack, wb_sample *out, uint32_t frames) {
    wb_rack *r = (wb_rack *)rack;
    if (!r || !out || frames == 0) return;
    if (frames > WB_MAX_BLOCK) frames = WB_MAX_BLOCK;

    /* Temporary buffers for the chain */
    wb_sample L[WB_MAX_BLOCK], R[WB_MAX_BLOCK];
    memset(L, 0, frames * sizeof(wb_sample));
    memset(R, 0, frames * sizeof(wb_sample));

    /* Find the synth unit (if any) — it generates audio */
    int synth_idx = -1;
    for (int i = 0; i < r->unit_count; i++) {
        if (r->units[i].active && r->units[i].type == UNIT_SYNTH) {
            synth_idx = i;
            break;
        }
    }

    /* If there's a synth, render it first */
    if (synth_idx >= 0) {
        wb_synth_render_block(r->units[synth_idx].inst, L, R, frames);
    }

    /* Process through remaining units in series (skip synth, already rendered) */
    for (int i = 0; i < r->unit_count; i++) {
        if (i == synth_idx) continue;
        wb_rack_unit *u = &r->units[i];
        if (!u->active || !u->inst) continue;

        switch (u->type) {
        case UNIT_FILTER:
            filter_process(u->inst, L, R, frames);
            break;
        case UNIT_COMP:
            wb_comp_process(u->inst, L, R, frames);
            break;
        case UNIT_DELAY:
            wb_delay_process(u->inst, L, R, frames);
            break;
        case UNIT_REVERB:
            wb_reverb_process(u->inst, L, R, frames);
            break;
        default:
            break;
        }
    }

    /* Interleave L/R into output buffer */
    for (uint32_t i = 0; i < frames; i++) {
        out[i * 2]     = L[i];
        out[i * 2 + 1] = R[i];
    }
}

void wb_rack_note(void *rack, int note, int vel) {
    wb_rack *r = (wb_rack *)rack;
    if (!r) return;

    /* Send note to all synth units */
    for (int i = 0; i < r->unit_count; i++) {
        wb_rack_unit *u = &r->units[i];
        if (!u->active || !u->inst) continue;
        if (u->type == UNIT_SYNTH) {
            wb_synth_note(u->inst, note, vel);
        }
    }
}

void wb_rack_set_midi_in(void *rack, int enable) {
    wb_rack *r = (wb_rack *)rack;
    if (!r) return;
    r->midi_in_enabled = enable ? 1 : 0;
}