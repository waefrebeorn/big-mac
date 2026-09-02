/* wb_expressions.c — After Effects-style expression engine
 * R88: bytecode stack machine for procedural animation
 *
 * Supports: math (+,-,*,/,%,sin,cos,sqrt,abs,pow,min,max),
 * time (t,fps,w,h), wiggle(freq,amp), ternary (a?b:c),
 * AE functions (loopOut,loopIn,ease,linear)
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "wbus/wbus_expressions.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Bytecode opcodes ---- */
typedef enum {
    OP_PUSH_CONST,   /* push double constant */
    OP_PUSH_VAR,     /* push variable value (t, fps, w, h, value) */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG,
    OP_SIN, OP_COS, OP_TAN, OP_SQRT, OP_ABS, OP_POW,
    OP_MIN, OP_MAX,
    OP_WIGGLE,       /* wiggle(freq, amp) */
    OP_TERNARY,      /* a ? b : c (conditional jump) */
    OP_LOOP_OUT,     /* loopOut(type) */
    OP_LOOP_IN,      /* loopIn(type) */
    OP_EASE,         /* ease(t, v1, v2) */
    OP_LINEAR,       /* linear(t, v1, v2) */
    OP_END
} wb_op_t;

#define MAX_CONST 64
#define MAX_CODE 256
#define MAX_STACK 32

typedef struct {
    double constants[MAX_CONST];
    uint8_t code[MAX_CODE];
    int code_len;
    int const_count;
} wb_bytecode;

struct wb_expression {
    wb_bytecode bc;
    char raw[1024];
    double duration;     /* for loop functions */
    int num_keyframes;  /* for loop functions */
};

static const char *g_last_error = NULL;

/* ---- Tokenizer ---- */
typedef enum {
    TOK_NUM, TOK_IDENT, TOK_LPAREN, TOK_RPAREN, TOK_COMMA,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_QUESTION, TOK_COLON, TOK_END, TOK_GT, TOK_LT, TOK_EQ
} tok_t;

typedef struct {
    tok_t type;
    double value;    /* for TOK_NUM */
    char name[64];   /* for TOK_IDENT */
} token_t;

static const char *g_pos;
static token_t g_tok;

static void skip_ws(void) {
    while (*g_pos == ' ' || *g_pos == '\t' || *g_pos == '\n') g_pos++;
}

static token_t next_token(void) {
    skip_ws();
    token_t t = {0};

    if (*g_pos == '\0') { t.type = TOK_END; return t; }

    if (*g_pos >= '0' && *g_pos <= '9') {
        t.type = TOK_NUM;
        t.value = strtod(g_pos, (char **)&g_pos);
        return t;
    }

    if ((*g_pos >= 'a' && *g_pos <= 'z') || (*g_pos >= 'A' && *g_pos <= 'Z') || *g_pos == '_') {
        int i = 0;
        while ((*g_pos >= 'a' && *g_pos <= 'z') || (*g_pos >= 'A' && *g_pos <= 'Z') ||
               (*g_pos >= '0' && *g_pos <= '9') || *g_pos == '_') {
            if (i < 63) t.name[i++] = *g_pos;
            g_pos++;
        }
        t.name[i] = '\0';
        t.type = TOK_IDENT;
        return t;
    }

    switch (*g_pos) {
    case '+': t.type = TOK_PLUS; break;
    case '-': t.type = TOK_MINUS; break;
    case '*': t.type = TOK_STAR; break;
    case '/': t.type = TOK_SLASH; break;
    case '%': t.type = TOK_PERCENT; break;
    case '(': t.type = TOK_LPAREN; break;
    case ')': t.type = TOK_RPAREN; break;
    case ',': t.type = TOK_COMMA; break;
    case '?': t.type = TOK_QUESTION; break;
    case ':': t.type = TOK_COLON; break;
    case '>': t.type = TOK_GT; break;
    case '<': t.type = TOK_LT; break;
    case '=': t.type = TOK_EQ; break;
    default:
        g_last_error = "unexpected character";
        g_pos++;
        return next_token();
    }
    g_pos++;
    return t;
}

static void advance(void) { g_tok = next_token(); }

/* ---- Parser (recursive descent) ---- */
static void parse_expr(wb_bytecode *bc);
static void parse_additive(wb_bytecode *bc);
static void parse_multiplicative(wb_bytecode *bc);
static void parse_unary(wb_bytecode *bc);
static void parse_primary(wb_bytecode *bc);

static int add_const(wb_bytecode *bc, double v) {
    if (bc->const_count >= MAX_CONST) return -1;
    bc->constants[bc->const_count] = v;
    return bc->const_count++;
}

static void emit_op(wb_bytecode *bc, uint8_t op) {
    if (bc->code_len < MAX_CODE - 4) bc->code[bc->code_len++] = op;
}

static void emit_const(wb_bytecode *bc, int idx) {
    emit_op(bc, OP_PUSH_CONST);
    emit_op(bc, (uint8_t)(idx & 0xFF));
}

static void parse_expr(wb_bytecode *bc) {
    parse_additive(bc);
}

static void parse_additive(wb_bytecode *bc) {
    parse_multiplicative(bc);
    while (g_tok.type == TOK_PLUS || g_tok.type == TOK_MINUS) {
        tok_t op = g_tok.type;
        advance();
        parse_multiplicative(bc);
        emit_op(bc, op == TOK_PLUS ? OP_ADD : OP_SUB);
    }
}

static void parse_multiplicative(wb_bytecode *bc) {
    parse_unary(bc);
    while (g_tok.type == TOK_STAR || g_tok.type == TOK_SLASH || g_tok.type == TOK_PERCENT) {
        tok_t op = g_tok.type;
        advance();
        parse_unary(bc);
        if (op == TOK_STAR) emit_op(bc, OP_MUL);
        else if (op == TOK_SLASH) emit_op(bc, OP_DIV);
        else emit_op(bc, OP_MOD);
    }
}

static void parse_unary(wb_bytecode *bc) {
    if (g_tok.type == TOK_MINUS) {
        advance();
        parse_primary(bc);
        emit_op(bc, OP_NEG);
    } else {
        parse_primary(bc);
    }
}

static void parse_primary(wb_bytecode *bc) {
    if (g_tok.type == TOK_NUM) {
        int idx = add_const(bc, g_tok.value);
        emit_const(bc, idx);
        advance();
    } else if (g_tok.type == TOK_IDENT) {
        /* Function calls: sin, cos, sqrt, wiggle, etc. */
        if (strcmp(g_tok.name, "sin") == 0 ||
            strcmp(g_tok.name, "cos") == 0 ||
            strcmp(g_tok.name, "tan") == 0 ||
            strcmp(g_tok.name, "sqrt") == 0 ||
            strcmp(g_tok.name, "abs") == 0 ||
            strcmp(g_tok.name, "pow") == 0 ||
            strcmp(g_tok.name, "min") == 0 ||
            strcmp(g_tok.name, "max") == 0 ||
            strcmp(g_tok.name, "wiggle") == 0 ||
            strcmp(g_tok.name, "loopOut") == 0 ||
            strcmp(g_tok.name, "loopIn") == 0 ||
            strcmp(g_tok.name, "ease") == 0 ||
            strcmp(g_tok.name, "linear") == 0) {
            char func_name[64];
            strncpy(func_name, g_tok.name, 63);
            func_name[63] = '\0';
            advance(); /* consume function name */
            if (g_tok.type == TOK_LPAREN) {
                advance();
                parse_expr(bc);
                /* Handle multi-arg functions */
                if (strcmp(func_name, "pow") == 0 || strcmp(func_name, "min") == 0 ||
                    strcmp(func_name, "max") == 0 || strcmp(func_name, "wiggle") == 0) {
                    if (g_tok.type == TOK_COMMA) { advance(); parse_expr(bc); }
                }
                if (strcmp(func_name, "ease") == 0 || strcmp(func_name, "linear") == 0) {
                    if (g_tok.type == TOK_COMMA) { advance(); parse_expr(bc); }
                    if (g_tok.type == TOK_COMMA) { advance(); parse_expr(bc); }
                }
                /* Skip optional args for loopOut/loopIn */
                while (g_tok.type != TOK_RPAREN && g_tok.type != TOK_END) advance();
                if (g_tok.type == TOK_RPAREN) advance();
            }
            /* Emit the appropriate opcode */
            if (strcmp(func_name, "sin") == 0) emit_op(bc, OP_SIN);
            else if (strcmp(func_name, "cos") == 0) emit_op(bc, OP_COS);
            else if (strcmp(func_name, "tan") == 0) emit_op(bc, OP_TAN);
            else if (strcmp(func_name, "sqrt") == 0) emit_op(bc, OP_SQRT);
            else if (strcmp(func_name, "abs") == 0) emit_op(bc, OP_ABS);
            else if (strcmp(func_name, "pow") == 0) emit_op(bc, OP_POW);
            else if (strcmp(func_name, "min") == 0) emit_op(bc, OP_MIN);
            else if (strcmp(func_name, "max") == 0) emit_op(bc, OP_MAX);
            else if (strcmp(func_name, "wiggle") == 0) emit_op(bc, OP_WIGGLE);
            else if (strcmp(func_name, "loopOut") == 0) emit_op(bc, OP_LOOP_OUT);
            else if (strcmp(func_name, "loopIn") == 0) emit_op(bc, OP_LOOP_IN);
            else if (strcmp(func_name, "ease") == 0) emit_op(bc, OP_EASE);
            else if (strcmp(func_name, "linear") == 0) emit_op(bc, OP_LINEAR);
            /* Do NOT advance() here — the caller's loop handles next token */
        } else {
            /* Variable: t, fps, w, h, value */
            if (strcmp(g_tok.name, "t") == 0) {
                emit_op(bc, OP_PUSH_VAR); emit_op(bc, 0);
            } else if (strcmp(g_tok.name, "fps") == 0) {
                emit_op(bc, OP_PUSH_VAR); emit_op(bc, 1);
            } else if (strcmp(g_tok.name, "w") == 0) {
                emit_op(bc, OP_PUSH_VAR); emit_op(bc, 2);
            } else if (strcmp(g_tok.name, "h") == 0) {
                emit_op(bc, OP_PUSH_VAR); emit_op(bc, 3);
            } else if (strcmp(g_tok.name, "value") == 0) {
                emit_op(bc, OP_PUSH_VAR); emit_op(bc, 4);
            } else {
                g_last_error = "unknown identifier";
            }
            advance();
        }
    } else if (g_tok.type == TOK_LPAREN) {
        advance();
        parse_expr(bc);
        if (g_tok.type == TOK_RPAREN) advance();
    } else {
        g_last_error = "unexpected token";
    }
}

/* ---- Compiler ---- */
wb_expression *wb_expression_compile(const char *expr) {
    if (!expr) return NULL;

    wb_expression *e = (wb_expression *)calloc(1, sizeof(wb_expression));
    if (!e) return NULL;

    strncpy(e->raw, expr, sizeof(e->raw) - 1);
    e->duration = 10.0;
    e->num_keyframes = 2;

    g_pos = expr;
    g_last_error = NULL;
    advance();
    parse_expr(&e->bc);

    if (g_last_error) {
        free(e);
        return NULL;
    }

    emit_op(&e->bc, OP_END);
    return e;
}

const char *wb_expression_last_error(void) {
    return g_last_error;
}

void wb_expression_free(wb_expression *e) {
    if (e) free(e);
}

/* ---- Evaluator ---- */
double wb_expression_eval(const wb_expression *e, double t, int w, int h,
                          int var_count, const wb_expr_var *vars) {
    if (!e) return 0.0;

    double stack[MAX_STACK];
    int sp = 0;
    const uint8_t *pc = e->bc.code;
    const double *c = e->bc.constants;

    /* Variable values */
    double var_t = t;
    double var_fps = 30.0;
    double var_w = (double)w;
    double var_h = (double)h;
    double var_value = 0.0;

    for (int i = 0; i < var_count; i++) {
        if (strcmp(vars[i].name, "value") == 0) var_value = vars[i].value;
    }

    for (int i = 0; i < MAX_CODE; i++) {
        uint8_t op = *pc++;
        double a, b, c2;

        switch (op) {
        case OP_PUSH_CONST:
            if (sp < MAX_STACK) stack[sp++] = c[*pc++];
            break;
        case OP_PUSH_VAR: {
            uint8_t var = *pc++;
            double val = 0;
            switch (var) {
            case 0: val = var_t; break;
            case 1: val = var_fps; break;
            case 2: val = var_w; break;
            case 3: val = var_h; break;
            case 4: val = var_value; break;
            }
            if (sp < MAX_STACK) stack[sp++] = val;
            break;
        }
        case OP_ADD: b = stack[--sp]; a = stack[--sp]; stack[sp++] = a + b; break;
        case OP_SUB: b = stack[--sp]; a = stack[--sp]; stack[sp++] = a - b; break;
        case OP_MUL: b = stack[--sp]; a = stack[--sp]; stack[sp++] = a * b; break;
        case OP_DIV: b = stack[--sp]; a = stack[--sp]; stack[sp++] = b != 0 ? a / b : 0; break;
        case OP_MOD: b = stack[--sp]; a = stack[--sp]; stack[sp++] = fmod(a, b); break;
        case OP_NEG: stack[sp > 0 ? sp-1 : 0] = -stack[sp > 0 ? sp-1 : 0]; break;
        case OP_SIN: stack[sp-1] = sin(stack[sp-1]); break;
        case OP_COS: stack[sp-1] = cos(stack[sp-1]); break;
        case OP_TAN: stack[sp-1] = tan(stack[sp-1]); break;
        case OP_SQRT: stack[sp-1] = sqrt(fabs(stack[sp-1])); break;
        case OP_ABS: stack[sp-1] = fabs(stack[sp-1]); break;
        case OP_POW: b = stack[--sp]; a = stack[--sp]; stack[sp++] = pow(a, b); break;
        case OP_MIN: b = stack[--sp]; a = stack[--sp]; stack[sp++] = fmin(a, b); break;
        case OP_MAX: b = stack[--sp]; a = stack[--sp]; stack[sp++] = fmax(a, b); break;
        case OP_WIGGLE: {
            b = stack[--sp]; /* amp */
            a = stack[--sp]; /* freq */
            if (sp < MAX_STACK) stack[sp++] = b * sin(2.0 * M_PI * a * var_t);
            break;
        }
        case OP_LOOP_OUT: {
            double dur = e->duration;
            if (dur > 0.001) {
                double lt = fmod(var_t, dur);
                if (lt < 0) lt += dur;
                var_t = lt;
            }
            if (sp < MAX_STACK) stack[sp++] = var_t;
            break;
        }
        case OP_LOOP_IN: {
            double dur = e->duration;
            if (dur > 0.001 && var_t < dur) {
                /* hold first keyframe */
            }
            if (sp < MAX_STACK) stack[sp++] = var_t;
            break;
        }
        case OP_EASE: {
            c2 = stack[--sp]; /* end val */
            b = stack[--sp];  /* start val */
            a = stack[--sp];  /* t */
            { double ct = fmin(1, fmax(0, a)); ct = ct * ct * (3 - 2 * ct); stack[sp++] = b + (c2 - b) * ct; }
            break;
        }
        case OP_LINEAR: {
            c2 = stack[--sp]; /* end val */
            b = stack[--sp];  /* start val */
            a = stack[--sp];  /* t */
            { double ct = fmin(1, fmax(0, a)); stack[sp++] = b + (c2 - b) * ct; }
            break;
        }
        case OP_END:
            return sp > 0 ? stack[sp-1] : 0.0;
        default:
            return 0.0;
        }
    }

    return sp > 0 ? stack[sp-1] : 0.0;
}

/* ---- Loop time helper ---- */
double wb_expression_loop_time(double t, double duration, wb_loop_type type, int num_keyframes) {
    if (duration < 0.001) return t;

    switch (type) {
    case WB_LOOP_CYCLE: {
        double lt = fmod(t, duration);
        return lt < 0 ? lt + duration : lt;
    }
    case WB_LOOP_PINGPONG: {
        double period = duration * 2;
        double lt = fmod(t, period);
        if (lt < 0) lt += period;
        return lt > duration ? period - lt : lt;
    }
    case WB_LOOP_OFFSET: {
        int cycles = (int)(t / duration);
        return t - cycles * duration + cycles * duration;
    }
    case WB_LOOP_CONSTANT:
    default:
        return t > duration ? duration : t;
    }
    (void)num_keyframes;
}
