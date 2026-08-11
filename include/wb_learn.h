/*
 * wb_learn.h — Q-learning TABLE for the whimsical voice changer
 *
 * The "quick learning" half of the backwards-RVC system. This is a tiny
 * tabular Q-learner — NO neural nets, just a table (fits the one-core
 * constraint):
 *
 *   state  = (f0 bucket, formant bucket)   — the speaker's voice region
 *   action = which tract tune to apply     — e.g. 0=neutral, 1=brighter,
 *                                            2=darker, 3=more open lips,
 *                                            4=more closed lips
 *   reward = how well the re-rendered voice matched the input (formant
 *            similarity + voicing agreement) — measured after the render
 *
 * The agent learns, per voice region, which tract tuning best recreates
 * the input — a "quick use" mapping that improves with every capture.
 */
#ifndef WB_LEARN_H
#define WB_LEARN_H

#define WB_LEARN_NF0   8    /* f0 buckets */
#define WB_LEARN_NFORM 8    /* formant buckets */
#define WB_LEARN_NACT  5    /* actions: tract tunes */
#define WB_LEARN_GAMMA 0.9
#define WB_LEARN_ALPHA 0.3

typedef struct {
    double q[WB_LEARN_NF0][WB_LEARN_NFORM][WB_LEARN_NACT];
    int count[WB_LEARN_NF0][WB_LEARN_NFORM][WB_LEARN_NACT];
} wb_learn_t;

/* Bucket helpers */
int wb_learn_f0_bucket(double f0);            /* 40..500 Hz -> 0..7 */
int wb_learn_form_bucket(double f1);          /* 200..1000 Hz -> 0..7 */

/* Pick action for a state: epsilon-greedy (explore when count low). */
int wb_learn_pick(const wb_learn_t *l, int s_f0, int s_f, double eps);

/* Update Q after applying an action and measuring reward. */
void wb_learn_update(wb_learn_t *l, int s_f0, int s_f, int act, double reward,
                     int s_f0_next, int s_f_next);

/* Save/load the Q table (text). */
int wb_learn_save(const char *path, const wb_learn_t *l);
int wb_learn_load(const char *path, wb_learn_t *l);

/* The 5 tract tunes: (tongue_index_offset, tongue_dia_offset, lips). */
void wb_learn_apply_tune(int act, double *ti, double *td, double *lips);

#endif /* WB_LEARN_H */
