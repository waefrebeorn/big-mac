/* wbus_expressions.h — After Effects-style expression engine for procedural
 * animation. Tokenizer → AST → bytecode stack machine → eval.
 *
 * Supports:
 *   Math: +, -, *, /, %, sin, cos, tan, sqrt, abs, pow, min, max, mod
 *   Time: t (seconds), fps, w (width), h (height)
 *   Value: value (current property value), wiggle(freq, amp)
 *   Conditionals: ternary (a ? b : c)
 *   AE functions: loopOut(), loopIn(), ease(), linear()
 *
 * Usage:
 *   wb_expression *e = wb_expression_compile("sin(t * 2 * PI) * 50");
 *   double v = wb_expression_eval(e, 0.5, 1920, 1080, 0, NULL, NULL);
 *   wb_expression_free(e);
 */

#ifndef WUBUS_WBUS_EXPRESSIONS_H
#define WUBUS_WBUS_EXPRESSIONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Opaque compiled expression. */
typedef struct wb_expression wb_expression;

/* Variable binding: name + value pairs for user-defined variables. */
typedef struct {
    const char *name;
    double      value;
} wb_expr_var;

/* Evaluate a pre-compiled expression at time t (seconds), with the given
 * frame dimensions (w, h), and optional user variables.
 * Returns the computed double result. On error, returns 0.0. */
double wb_expression_eval(const wb_expression *e, double t, int w, int h,
                          int var_count, const wb_expr_var *vars);

/* Compile an expression string to bytecode. Returns NULL on parse error.
 * On failure, if err_msg is non-NULL, *err_msg is set to a static string
 * describing the error (valid until next compile call). */
wb_expression *wb_expression_compile(const char *expr);
const char    *wb_expression_last_error(void);

/* Free a compiled expression. */
void wb_expression_free(wb_expression *e);

/* ---- AE-style keyframe loop helpers (used by loopOut/loopIn) ------- */

/* Loop types matching After Effects. */
typedef enum {
    WB_LOOP_CYCLE = 0,   /* repeat keyframes */
    WB_LOOP_PINGPONG,    /* alternate forward/reverse */
    WB_LOOP_OFFSET,      /* offset by loop count */
    WB_LOOP_CONSTANT      /* hold last keyframe */
} wb_loop_type;

/* Compute looped time given segment duration, loop type, and loop count
 * (number of keyframes in the segment). Used internally by loopOut/loopIn
 * but exposed for node use. */
double wb_expression_loop_time(double t, double duration,
                               wb_loop_type type, int num_keyframes);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_EXPRESSIONS_H */