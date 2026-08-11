/*
 * wb_mlp.h — SHALLOW MLP prosody planner (the WordVoice bound-token,
 * non-neural version). One hidden layer, trained in C11 with backprop —
 * the planner that predicts word-level acoustic attributes from word
 * features, replacing WordVoice's 0.5B LLM planner.
 *
 * Output heads (WB_MLP_OUT = 12):
 *   [0]      dur_mult   (regression, 0.5..2)
 *   [1]      energy     (regression, 0..1)
 *   [2]      pitch      (regression, -1..1)
 *   [3..9]   tone logits (7 classes: flat, rise, strong_rise, fall,
 *             strong_fall, peak, valley) — softmax, argmax = tone class
 *   [10..14] boundary logits (5 classes b0..b4) — softmax, argmax
 */
#ifndef WB_MLP_H
#define WB_MLP_H

#define WB_MLP_IN   12    /* word features */
#define WB_MLP_HID  16    /* hidden units */
#define WB_MLP_OUT  15    /* 3 regression + 7 tone logits + 5 boundary logits */

#define WB_MLP_NTONE 7
#define WB_MLP_NBND  5

typedef struct {
    double w1[WB_MLP_IN][WB_MLP_HID];
    double b1[WB_MLP_HID];
    double w2[WB_MLP_HID][WB_MLP_OUT];
    double b2[WB_MLP_OUT];
} wb_mlp_t;

/* Forward pass. out[0..2] = regression values, out[3..9] = tone logits,
 * out[10..14] = boundary logits. */
void wb_mlp_forward(const wb_mlp_t *m, const double *features, double *out);

/* Train one epoch on (features, targets) with learning rate lr.
 * Loss = MSE on regression + cross-entropy on tone + boundary logits.
 * Targets: targs[0..2] = regression targets, targs[3..9] = one-hot tone,
 * targs[10..14] = one-hot boundary. Returns total loss. */
double wb_mlp_train(wb_mlp_t *m, const double *feats, const double *targs,
                    int n, double lr);

/* Init weights small random. */
void wb_mlp_init(wb_mlp_t *m, unsigned int seed);

/* Save/load weights (text). */
int wb_mlp_save(const char *path, const wb_mlp_t *m);
int wb_mlp_load(const char *path, wb_mlp_t *m);

#endif /* WB_MLP_H */
