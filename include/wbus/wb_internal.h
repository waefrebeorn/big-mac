#ifndef WUBUS_WB_INTERNAL_H
#define WUBUS_WB_INTERNAL_H

/* Internal cross-module declarations shared between engine .c files.
 * Not part of the public ABI (wbus.h / wbus_plugin.h are).
 */

#include "wbus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* wb_session.c */
void wb_session_destroy(wb_session *s);
wb_session *wb_session_demo(void);
wb_session *wb_session_create(void);

/* wb_transport.c */
void wb_transport_schedule_notes(wb_track *track, double block_start, uint32_t n,
                                 void (*note_on)(void*, int, int), void *voice);
void wb_transport_schedule_notes_sw(wb_track *track, double block_start, uint32_t n,
                                    void (*note_on)(void*, int, int), void *voice,
                                    double bpm, double swing);

/* wb_synth.c */
typedef struct wb_synth_inst wb_synth_inst;
struct wb_synth_inst {
    uint32_t sr;
    void    *_voices[16];
    float    master_vol;
    float    filter_cutoff;
    float    filter_res;
    int      waveform;
    float    a, d, s, r;
};
void *wb_synth_create(uint32_t sr);
void  wb_synth_destroy(void *inst);
void  wb_synth_note(void *inst, int note, int vel);
void  wb_synth_set(void *inst, int param, float v);
void  wb_synth_render_block(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_synth_render_block_simd(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_synth_render_block_simd_2x(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_synth_render_block_wt(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_synth_ensure_registered(void);

/* wb_fm.c */
void *wb_fm_create(uint32_t sr);
void  wb_fm_destroy(void *inst);
void  wb_fm_note(void *inst, int note, int vel);
void  wb_fm_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_fm_render_fast(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_fm_set_speed_mode(int mode);

/* wb_drums.c */
void *wb_drum_create(uint32_t sr);
void  wb_drum_destroy(void *inst);
void  wb_drum_note(void *inst, int note, int vel);
void  wb_drum_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

/* wb_chorus.c */
void *wb_chorus_create(uint32_t sr);
void  wb_chorus_destroy(void *inst);
void  wb_chorus_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

/* wb_eq.c */
void *wb_eq_create(uint32_t sr);
void  wb_eq_destroy(void *inst);
void  wb_eq_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

/* wb_wav.c */
int wb_wav_write_pcm16(const char *path, const wb_sample *data, uint32_t frames,
                       uint8_t channels, uint32_t sample_rate);
int wb_wav_write_f32(const char *path, const wb_sample *data, uint32_t frames,
                     uint8_t channels, uint32_t sample_rate);
int wb_wav_write_bwf(const char *path, const wb_sample *data, uint32_t frames,
                      uint8_t channels, uint32_t sample_rate,
                      const char *description, const char *originator,
                      const char *originator_ref, time_t orig_time);
int wb_wav_read_pcm16(const char *path, float **out_data, uint32_t *out_frames,
                      int *out_channels, int *out_sr);

/* wb_comp.c */
void *wb_comp_create(uint32_t sr);
void  wb_comp_destroy(void *inst);
void  wb_comp_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_comp_set(void *inst, int param, float v);
void  wb_comp_inplace_wet(void *inst, wb_sample *L, wb_sample *R, uint32_t n, float w);
void  wb_comp_set_key(void *inst, const wb_sample *keyL, const wb_sample *keyR,
                     uint32_t n);

/* wb_delay.c */
typedef struct wb_delay_inst wb_delay_inst;
struct wb_delay_inst {
    uint32_t sr;
    wb_sample *bufL, *bufR;
    uint32_t cap;
    uint32_t pos;
    float time_ms;
    float feedback;
    float mix;
    float lp_state;
};
void *wb_delay_create(uint32_t sr);
void  wb_delay_destroy(void *inst);
void  wb_delay_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_delay_inplace_wet(void *inst, wb_sample *L, wb_sample *R, uint32_t n, float w);

/* wb_reverb.c */
typedef struct wb_reverb_inst wb_reverb_inst;
struct wb_reverb_inst {
    uint32_t sr;
    float feedback;
    float mix;
    void *_impl[16];
};
void *wb_reverb_create(uint32_t sr);
void  wb_reverb_destroy(void *inst);
void  wb_reverb_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_reverb_inplace_wet(void *inst, wb_sample *L, wb_sample *R, uint32_t n, float w);

/* wb_saturation.c */
void *wb_sat_create(uint32_t sr);
void  wb_sat_destroy(void *inst);
void  wb_sat_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_sat_set(void *inst, int param, float v);

/* wb_gate.c */
void *wb_gate_create(uint32_t sr);
void  wb_gate_destroy(void *inst);
void  wb_gate_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_gate_set(void *inst, int param, float v);

/* wb_multiband.c */
void *wb_mb_create(uint32_t sr);
void  wb_mb_destroy(void *inst);
void  wb_mb_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
int   wb_mb_has_param(const void *inst, const char *name);
void  wb_mb_set_param(void *inst, const char *name, float v);
float wb_mb_get_param(const void *inst, const char *name);

/* wb_sampler.c */
void *wb_sampler_create(uint32_t sr);
void  wb_sampler_destroy(void *inst);
void  wb_sampler_load(void *inst, const wb_sample *data, uint32_t count, int loop);
void  wb_sampler_note(void *inst, int note, int vel);
void  wb_sampler_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

/* wb_granular.c */
void *wb_granular_create(uint32_t sr);
void  wb_granular_destroy(void *inst);
void  wb_granular_load(void *inst, const wb_sample *data, uint32_t count, int loop);
void  wb_granular_note(void *inst, int note, int vel);
void  wb_granular_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

/* wb_midi.c */
double wb_midi_note_to_freq(int note);
float  wb_midi_map01(float v01, float min, float max);

/* wb_transcript.c — needed by wb_text_edit.c for word-level editing */
struct wb_transcript {
    wb_word *words;
    int   count;
    int   cap;
};

/* wb_autoreframe.c */
/* struct wb_autoreframe is defined in wbus.h */

/* wb_tuner.c */
typedef struct wb_tuner wb_tuner;
wb_tuner *wb_tuner_create(wb_engine *e);
void      wb_tuner_start(wb_tuner *t);
void      wb_tuner_stop(wb_tuner *t);
void      wb_tuner_destroy(wb_tuner *t);
double    wb_tuner_last_loss(const wb_tuner *t);

/* wb_unit.c */
void wb_unit_ensure_all(void);

/* wb_unit_clap.c */
struct wb_clap_host;
void wb_unit_clap_ensure(void);
void *wb_unit_clap_create(struct wb_clap_host *h, const char *id, uint32_t sr);

/* ---- Noise Gate (spectral subtraction) ---- */
typedef struct wb_noise_gate wb_noise_gate;
wb_noise_gate *wb_noise_gate_create(float sample_rate);
void           wb_noise_gate_destroy(wb_noise_gate *ng);
void           wb_noise_gate_set_params(wb_noise_gate *ng, float sensitivity, float reduction_db, float attack_ms, float release_ms);
int            wb_noise_gate_learn(wb_noise_gate *ng, const float *buf, int n_frames, int channels, int fft_size);
int            wb_noise_gate_process(wb_noise_gate *ng, float *buf, int n_frames, int channels);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WB_INTERNAL_H */