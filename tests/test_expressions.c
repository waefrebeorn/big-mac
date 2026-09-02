/* test_expressions.c — verify AE-style expression engine */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wbus/wbus_expressions.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  PASS: %s\n", msg); passes++; } \
} while(0)

int main(void) {
    int passes = 0, failures = 0;

    printf("--- Expressions Engine ---\n");

    /* Basic math */
    {
        wb_expression *e = wb_expression_compile("1 + 2");
        CHECK(e != NULL, "compile 1+2");
        if (e) {
            double v = wb_expression_eval(e, 0, 1920, 1080, 0, NULL);
            CHECK(fabs(v - 3.0) < 0.001, "1+2 = 3");
            wb_expression_free(e);
        }
    }

    {
        wb_expression *e = wb_expression_compile("10 * 5");
        if (e) {
            double v = wb_expression_eval(e, 0, 1920, 1080, 0, NULL);
            CHECK(fabs(v - 50.0) < 0.001, "10*5 = 50");
            wb_expression_free(e);
        }
    }

    /* Time variable */
    {
        wb_expression *e = wb_expression_compile("t * 10");
        if (e) {
            double v = wb_expression_eval(e, 0.5, 1920, 1080, 0, NULL);
            CHECK(fabs(v - 5.0) < 0.001, "t*10 at t=0.5 = 5");
            wb_expression_free(e);
        }
    }

    /* Sin function */
    {
        wb_expression *e = wb_expression_compile("sin(t)");
        if (e) {
            double v = wb_expression_eval(e, 0.0, 1920, 1080, 0, NULL);
            CHECK(fabs(v - 0.0) < 0.001, "sin(0) = 0");
            wb_expression_free(e);
        }
    }

    /* Wiggle */
    {
        wb_expression *e = wb_expression_compile("wiggle(2, 50)");
        if (e) {
            double v = wb_expression_eval(e, 0.0, 1920, 1080, 0, NULL);
            CHECK(fabs(v - 0.0) < 0.001, "wiggle(2,50) at t=0 = 0");
            v = wb_expression_eval(e, 0.125, 1920, 1080, 0, NULL);
            CHECK(fabs(v - 50.0) < 1.0, "wiggle(2,50) at t=0.125 ≈ 50 (sin(PI/2)=1)");
            wb_expression_free(e);
        }
    }

    /* Width/height variables */
    {
        wb_expression *e = wb_expression_compile("w / 2");
        if (e) {
            double v = wb_expression_eval(e, 0, 1920, 1080, 0, NULL);
            CHECK(fabs(v - 960.0) < 0.001, "w/2 = 960");
            wb_expression_free(e);
        }
    }

    /* Complex expression */
    {
        wb_expression *e = wb_expression_compile("sin(t * 2 * 3.14159) * 50 + 100");
        if (e) {
            double v = wb_expression_eval(e, 0.0, 1920, 1080, 0, NULL);
            CHECK(fabs(v - 100.0) < 5.0, "sin(t*2*3.14159)*50+100 at t=0 ≈ 100");
            wb_expression_free(e);
        }
    }

    /* Loop time */
    {
        double lt = wb_expression_loop_time(12.0, 10.0, WB_LOOP_CYCLE, 2);
        CHECK(fabs(lt - 2.0) < 0.001, "loopOut cycle: 12 mod 10 = 2");
    }

    /* NULL safety */
    {
        wb_expression *e = wb_expression_compile(NULL);
        CHECK(e == NULL, "NULL expression returns NULL");
    }

    /* Error handling */
    {
        wb_expression *e = wb_expression_compile("1 + + 2");
        /* May or may not parse — just verify no crash */
        if (e) {
            double v = wb_expression_eval(e, 0, 1920, 1080, 0, NULL);
            (void)v;
            wb_expression_free(e);
        }
        CHECK(1, "malformed expression doesn't crash");
    }

    printf("\n=== Results: %d/%d passed ===\n", passes, passes + failures);
    return failures > 0 ? 1 : 0;
}
