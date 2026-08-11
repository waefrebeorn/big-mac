/*
 * wb_glottis.h — Big Mac glottis (LF glottal flow model)
 *
 * The sound source: vocal folds. LF (Liljencrants-Fant) model with
 * tenseness → Rd parameter. Jitter/shimmer/vibrato included.
 */
#ifndef WB_GLOTTIS_H
#define WB_GLOTTIS_H

typedef struct wb_glottis wb_glottis_t;

wb_glottis_t *wb_glottis_new(void);
void wb_glottis_free(wb_glottis_t *g);

/* Controls */
void wb_glottis_set_frequency(wb_glottis_t *g, double hz);
void wb_glottis_set_tenseness(wb_glottis_t *g, double t);  /* 0 breathy .. 1 pressed */
void wb_glottis_set_vibrato(wb_glottis_t *g, double depth, double rate);
void wb_glottis_set_jitter(wb_glottis_t *g, double amount);
void wb_glottis_set_shimmer(wb_glottis_t *g, double amount);
void wb_glottis_set_intensity(wb_glottis_t *g, double intensity);

/* One sample of glottal flow. aspiration_noise in [-1,1]. */
double wb_glottis_run_step(wb_glottis_t *g, double lambda, double aspiration_noise);

/* End of block: glide frequency toward target, apply vibrato, ramp intensity. */
void wb_glottis_finish_block(wb_glottis_t *g, int voiced, double block_time);

#endif /* WB_GLOTTIS_H */
