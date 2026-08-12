/* wb_automation.c — automation envelopes (breakpoint curves over time).
 * A lane owns an ordered list of (time,value) breakpoints and interpolates
 * a value at any song position (linear / hold / smooth). The engine applies
 * the interpolated value to a named parameter once per render block.
 *
 * Pure C11, no dependencies.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "wbus.h"

/* ---- lane lifecycle ---------------------------------------------------- */
wb_automation_lane *wb_automation_lane_create(const char *param) {
    wb_automation_lane *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    if (param) snprintf(l->param, sizeof(l->param), "%s", param);
    return l;
}

void wb_automation_lane_destroy(wb_automation_lane *l) {
    if (!l) return;
    free(l->points);
    free(l);
}

/* Add (or replace) a breakpoint at time; keeps points sorted by time. */
int wb_automation_add_point(wb_automation_lane *l, double time, double value, int curve) {
    if (!l) return -1;
    /* find insertion index (keep sorted, replace if same time) */
    uint32_t i = 0;
    while (i < l->point_count && l->points[i].time < time) i++;
    if (i < l->point_count && fabs(l->points[i].time - time) < 1e-9) {
        l->points[i].value = value;
        l->points[i].curve = curve;
        return 0;
    }
    wb_automation_point *np = realloc(l->points, (l->point_count + 1) * sizeof(wb_automation_point));
    if (!np) return -1;
    l->points = np;
    memmove(&l->points[i+1], &l->points[i], (l->point_count - i) * sizeof(wb_automation_point));
    l->points[i].time = time;
    l->points[i].value = value;
    l->points[i].curve = curve;
    l->point_count++;
    return 0;
}

int wb_automation_clear(wb_automation_lane *l) {
    if (!l) return -1;
    l->point_count = 0;
    return 0;
}

/* Interpolate the lane's value at song position `pos` (samples).
 * Returns the interpolated value, or `fallback` if no points bound it. */
double wb_automation_value_at(const wb_automation_lane *l, double pos, double fallback) {
    if (!l || l->point_count == 0) return fallback;
    /* before first point */
    if (pos <= l->points[0].time) return l->points[0].value;
    /* after last point */
    if (pos >= l->points[l->point_count-1].time)
        return l->points[l->point_count-1].value;
    /* find surrounding segment */
    uint32_t i = 0;
    while (i < l->point_count-1 && l->points[i+1].time < pos) i++;
    const wb_automation_point *a = &l->points[i];
    const wb_automation_point *b = &l->points[i+1];
    double span = b->time - a->time;
    if (span < 1e-12) return b->value;
    double t = (pos - a->time) / span;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    switch (a->curve) {
        case 1:  return a->value;                     /* hold */
        case 2: { /* smooth: cosine interp */
            double ct = (1.0 - cos(t * M_PI)) * 0.5;
            return a->value * (1.0 - ct) + b->value * ct;
        }
        default: return a->value * (1.0 - t) + b->value * t; /* linear */
    }
}
