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

/* wb_synth.c */
void *wb_synth_create(uint32_t sr);
void  wb_synth_destroy(void *inst);
void  wb_synth_note(void *inst, int note, int vel);
void  wb_synth_render_block(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_synth_ensure_registered(void);

/* wb_fm.c */
void *wb_fm_create(uint32_t sr);
void  wb_fm_destroy(void *inst);
void  wb_fm_note(void *inst, int note, int vel);
void  wb_fm_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

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

/* wb_comp.c */
void *wb_comp_create(uint32_t sr);
void  wb_comp_destroy(void *inst);
void  wb_comp_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_comp_inplace_wet(void *inst, wb_sample *L, wb_sample *R, uint32_t n, float w);

/* wb_delay.c */
void *wb_delay_create(uint32_t sr);
void  wb_delay_destroy(void *inst);
void  wb_delay_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_delay_inplace_wet(void *inst, wb_sample *L, wb_sample *R, uint32_t n, float w);

/* wb_reverb.c */
void *wb_reverb_create(uint32_t sr);
void  wb_reverb_destroy(void *inst);
void  wb_reverb_process(void *inst, wb_sample *L, wb_sample *R, uint32_t n);
void  wb_reverb_inplace_wet(void *inst, wb_sample *L, wb_sample *R, uint32_t n, float w);

/* wb_sampler.c */
void *wb_sampler_create(uint32_t sr);
void  wb_sampler_destroy(void *inst);
void  wb_sampler_load(void *inst, const wb_sample *data, uint32_t count, int loop);
void  wb_sampler_note(void *inst, int note, int vel);
void  wb_sampler_render(void *inst, wb_sample *L, wb_sample *R, uint32_t n);

/* wb_midi.c */
double wb_midi_note_to_freq(int note);
float  wb_midi_map01(float v01, float min, float max);

/* wb_tuner.c — recursive learn/sense/act/measure/compare/fix loop */
typedef struct wb_tuner wb_tuner;
wb_tuner *wb_tuner_create(wb_engine *e);
void      wb_tuner_start(wb_tuner *t);
void      wb_tuner_stop(wb_tuner *t);
void      wb_tuner_destroy(wb_tuner *t);
double    wb_tuner_last_loss(const wb_tuner *t);

/* wb_unit.c */
void wb_unit_ensure_all(void);

/* wb_unit_clap.c — CLAP plugin bridge */
struct wb_clap_host;
void wb_unit_clap_ensure(void);
void *wb_unit_clap_create(struct wb_clap_host *h, const char *id, uint32_t sr);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WB_INTERNAL_H */
