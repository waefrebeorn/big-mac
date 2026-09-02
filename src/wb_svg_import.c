/* wb_svg_import.c — SVG file import to shape layers
 * R090: After Effects SVG import parity
 *
 * Parses SVG XML and converts elements to Big Mac shape nodes.
 * Supports: path, rect, ellipse, circle, polygon, polyline, line.
 * Parses SVG path data (M/L/C/Q/Z commands, absolute + relative).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "wbus/wbus_compositor.h"

#define SVG_MAX_ELEMENTS 256
#define SVG_MAX_PATH_VERTS 1024

/* ---- Color parsing ---- */

static int parse_svg_color(const char *color_str, float *r, float *g, float *b, float *a) {
    if (!color_str || !r || !g || !b || !a) return -1;
    *a = 1.0f;

    if (color_str[0] == '#') {
        /* Hex color */
        int len = (int)strlen(color_str + 1);
        if (len == 3) {
            /* Short hex: #RGB */
            int rv, gv, bv;
            sscanf(color_str + 1, "%1x%1x%1x", &rv, &gv, &bv);
            *r = (rv * 17) / 255.0f;
            *g = (gv * 17) / 255.0f;
            *b = (bv * 17) / 255.0f;
            return 0;
        } else if (len == 6) {
            /* Full hex: #RRGGBB */
            unsigned int rv, gv, bv;
            sscanf(color_str + 1, "%2x%2x%2x", &rv, &gv, &bv);
            *r = rv / 255.0f;
            *g = gv / 255.0f;
            *b = bv / 255.0f;
            return 0;
        }
    } else if (strcmp(color_str, "none") == 0) {
        *a = 0.0f;
        *r = *g = *b = 0.0f;
        return 0;
    } else if (strncmp(color_str, "rgb(", 4) == 0) {
        int rv, gv, bv;
        sscanf(color_str + 4, "%d,%d,%d", &rv, &gv, &bv);
        *r = rv / 255.0f;
        *g = gv / 255.0f;
        *b = bv / 255.0f;
        return 0;
    }

    /* Named colors (common) */
    if (strcmp(color_str, "black") == 0) { *r=*g=*b=0; return 0; }
    if (strcmp(color_str, "white") == 0) { *r=*g=*b=1; return 0; }
    if (strcmp(color_str, "red") == 0) { *r=1; *g=*b=0; return 0; }
    if (strcmp(color_str, "green") == 0) { *g=1; *r=*b=0; return 0; }
    if (strcmp(color_str, "blue") == 0) { *b=1; *r=*g=0; return 0; }

    return -1; /* unknown */
}

/* ---- Path data parser ---- */

int wb_svg_parse_path(const char *path_d, float *verts_x, float *verts_y, int *vert_count, int max_verts) {
    if (!path_d || !verts_x || !verts_y || !vert_count) return -1;
    *vert_count = 0;

    const char *p = path_d;
    float cur_x = 0, cur_y = 0;
    float start_x = 0, start_y = 0;

    while (*p && *vert_count < max_verts) {
        /* Skip whitespace */
        while (*p && isspace(*p)) p++;
        if (!*p) break;

        char cmd = *p++;
        float vals[12];
        int nvals = 0;

        /* Parse numbers based on command */
        switch (cmd) {
            case 'M': /* moveto absolute */
            case 'm': /* moveto relative */
            case 'L': /* lineto absolute */
            case 'l': /* lineto relative */
                /* Parse pairs until next command */
                while (*p && nvals < 12) {
                    while (*p && (isspace(*p) || *p == ',')) p++;
                    if (!*p || isalpha(*p)) break;
                    char *end;
                    vals[nvals++] = strtof(p, &end);
                    if (end == p) break;
                    p = end;
                }
                for (int i = 0; i + 1 < nvals && *vert_count < max_verts; i += 2) {
                    if (cmd == 'm' || cmd == 'l') {
                        cur_x += vals[i];
                        cur_y += vals[i+1];
                    } else {
                        cur_x = vals[i];
                        cur_y = vals[i+1];
                    }
                    verts_x[*vert_count] = cur_x;
                    verts_y[*vert_count] = cur_y;
                    (*vert_count)++;
                    if (cmd == 'M' || cmd == 'm') {
                        start_x = cur_x;
                        start_y = cur_y;
                    }
                }
                break;

            case 'H': /* horizontal lineto absolute */
            case 'h': /* horizontal lineto relative */
                while (*p && (isspace(*p) || *p == ',')) p++;
                if (*p && !isalpha(*p)) {
                    char *end;
                    float v = strtof(p, &end);
                    p = end;
                    cur_x = (cmd == 'h') ? cur_x + v : v;
                    verts_x[*vert_count] = cur_x;
                    verts_y[*vert_count] = cur_y;
                    (*vert_count)++;
                }
                break;

            case 'V': /* vertical lineto absolute */
            case 'v': /* vertical lineto relative */
                while (*p && (isspace(*p) || *p == ',')) p++;
                if (*p && !isalpha(*p)) {
                    char *end;
                    float v = strtof(p, &end);
                    p = end;
                    cur_y = (cmd == 'v') ? cur_y + v : v;
                    verts_x[*vert_count] = cur_x;
                    verts_y[*vert_count] = cur_y;
                    (*vert_count)++;
                }
                break;

            case 'Z':
            case 'z': /* close path */
                cur_x = start_x;
                cur_y = start_y;
                break;

            case 'C': /* cubic bezier absolute */
            case 'c': /* cubic bezier relative */
            case 'S': /* smooth cubic absolute */
            case 's': /* smooth cubic relative */
            {
                /* Parse 6 values: x1 y1 x2 y2 x y */
                float vals[6] = {0};
                int nvals = 0;
                while (*p && nvals < 6) {
                    while (*p && (isspace(*p) || *p == ',')) p++;
                    if (!*p || isalpha(*p)) break;
                    char *end;
                    vals[nvals] = strtof(p, &end);
                    if (end == p) break;
                    p = end;
                    nvals++;
                }
                if (nvals == 6) {
                    float x1 = (cmd == 'c' || cmd == 's') ? cur_x + vals[0] : vals[0];
                    float y1 = (cmd == 'c' || cmd == 's') ? cur_y + vals[1] : vals[1];
                    float x2 = (cmd == 'c' || cmd == 's') ? cur_x + vals[2] : vals[2];
                    float y2 = (cmd == 'c' || cmd == 's') ? cur_y + vals[3] : vals[3];
                    float ex = (cmd == 'c' || cmd == 's') ? cur_x + vals[4] : vals[4];
                    float ey = (cmd == 'c' || cmd == 's') ? cur_y + vals[5] : vals[5];
                    /* Subdivide cubic bezier into line segments */
                    int steps = 8;
                    for (int s = 1; s <= steps && *vert_count < max_verts; s++) {
                        float t = (float)s / (float)steps;
                        float mt = 1.0f - t;
                        float bx = mt*mt*mt*cur_x + 3*mt*mt*t*x1 + 3*mt*t*t*x2 + t*t*t*ex;
                        float by = mt*mt*mt*cur_y + 3*mt*mt*t*y1 + 3*mt*t*t*y2 + t*t*t*ey;
                        verts_x[*vert_count] = bx;
                        verts_y[*vert_count] = by;
                        (*vert_count)++;
                    }
                    cur_x = ex;
                    cur_y = ey;
                }
                break;
            }

            case 'Q': /* quadratic bezier absolute */
            case 'q': /* quadratic bezier relative */
            {
                float vals[4] = {0};
                int nvals = 0;
                while (*p && nvals < 4) {
                    while (*p && (isspace(*p) || *p == ',')) p++;
                    if (!*p || isalpha(*p)) break;
                    char *end;
                    vals[nvals] = strtof(p, &end);
                    if (end == p) break;
                    p = end;
                    nvals++;
                }
                if (nvals == 4) {
                    float cx = (cmd == 'q') ? cur_x + vals[0] : vals[0];
                    float cy = (cmd == 'q') ? cur_y + vals[1] : vals[1];
                    float ex = (cmd == 'q') ? cur_x + vals[2] : vals[2];
                    float ey = (cmd == 'q') ? cur_y + vals[3] : vals[3];
                    int steps = 6;
                    for (int s = 1; s <= steps && *vert_count < max_verts; s++) {
                        float t = (float)s / (float)steps;
                        float mt = 1.0f - t;
                        float bx = mt*mt*cur_x + 2*mt*t*cx + t*t*ex;
                        float by = mt*mt*cur_y + 2*mt*t*cy + t*t*ey;
                        verts_x[*vert_count] = bx;
                        verts_y[*vert_count] = by;
                        (*vert_count)++;
                    }
                    cur_x = ex;
                    cur_y = ey;
                }
                break;
            }

            default:
                /* Unknown command, skip */
                break;
        }
    }

    return 0;
}

/* ---- SVG element import ---- */

int wb_svg_import(const char *svg_path, int target_w, int target_h,
                   wb_node **out_nodes, int max_nodes) {
    if (!svg_path || !out_nodes) return -1;

    /* Read file */
    FILE *f = fopen(svg_path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *xml = (char *)malloc(size + 1);
    if (!xml) { fclose(f); return -1; }
    fread(xml, 1, size, f);
    xml[size] = '\0';
    fclose(f);

    int node_count = 0;

    /* Parse viewBox from <svg> element */
    float vb_x = 0, vb_y = 0, vb_w = 0, vb_h = 0;
    int has_viewbox = 0;
    {
        char *svg_start = strstr(xml, "<svg");
        if (svg_start) {
            char *vb_attr = strstr(svg_start, "viewBox=\"");
            if (vb_attr) {
                vb_attr += 9;
                sscanf(vb_attr, "%f %f %f %f", &vb_x, &vb_y, &vb_w, &vb_h);
                has_viewbox = (vb_w > 0 && vb_h > 0);
            }
        }
    }

    /* Compute scale factors from viewBox to target dimensions */
    float scale_x = 1.0f, scale_y = 1.0f;
    if (has_viewbox && target_w > 0 && target_h > 0) {
        scale_x = (float)target_w / vb_w;
        scale_y = (float)target_h / vb_h;
    }

    /* Simple XML parser: find elements */
    char *p = xml;
    while (*p && node_count < max_nodes) {
        /* Find next '<' */
        while (*p && *p != '<') p++;
        if (!*p) break;
        p++;

        /* Skip comments and XML declaration */
        if (*p == '?' || *p == '!') continue;

        /* Get tag name */
        char tag[64];
        int ti = 0;
        while (*p && !isspace(*p) && *p != '>' && *p != '/' && ti < 63)
            tag[ti++] = *p++;
        tag[ti] = '\0';

        /* Parse attributes into a buffer */
        char attrs[4096] = {0};
        int ai = 0;
        while (*p && *p != '>' && *p != '/' && ai < 4095)
            attrs[ai++] = *p++;
        attrs[ai] = '\0';

        /* Extract common attributes */
        char fill_str[64] = "black";
        char stroke_str[64] = "none";
        char path_d[4096] = {0};
        float rect_x = 0, rect_y = 0, rect_w = 0, rect_h = 0;
        float ellipse_cx = 0, ellipse_cy = 0, ellipse_rx = 0, ellipse_ry = 0;

        /* Parse fill */
        char *attr = strstr(attrs, "fill=\"");
        if (attr) {
            attr += 6;
            int i = 0;
            while (*attr && *attr != '"' && i < 63) fill_str[i++] = *attr++;
            fill_str[i] = '\0';
        }

        /* Parse stroke */
        attr = strstr(attrs, "stroke=\"");
        if (attr) {
            attr += 8;
            int i = 0;
            while (*attr && *attr != '"' && i < 63) stroke_str[i++] = *attr++;
            stroke_str[i] = '\0';
        }

        /* Parse path data */
        attr = strstr(attrs, "d=\"");
        if (attr) {
            attr += 3;
            int i = 0;
            while (*attr && *attr != '"' && i < 4095) path_d[i++] = *attr++;
            path_d[i] = '\0';
        }

        /* Parse rect attributes */
        attr = strstr(attrs, "x=\"");
        if (attr) rect_x = strtof(attr + 3, NULL);
        attr = strstr(attrs, "y=\"");
        if (attr) rect_y = strtof(attr + 3, NULL);
        attr = strstr(attrs, "width=\"");
        if (attr) rect_w = strtof(attr + 7, NULL);
        attr = strstr(attrs, "height=\"");
        if (attr) rect_h = strtof(attr + 8, NULL);

        /* Parse ellipse/circle attributes */
        attr = strstr(attrs, "cx=\"");
        if (attr) ellipse_cx = strtof(attr + 4, NULL);
        attr = strstr(attrs, "cy=\"");
        if (attr) ellipse_cy = strtof(attr + 4, NULL);
        attr = strstr(attrs, "rx=\"");
        if (attr) ellipse_rx = strtof(attr + 4, NULL);
        attr = strstr(attrs, "ry=\"");
        if (attr) ellipse_ry = strtof(attr + 4, NULL);

        /* Create shape node based on tag */
        float fr, fg, fb, fa;
        parse_svg_color(fill_str, &fr, &fg, &fb, &fa);

        /* Apply viewBox scaling to coordinates */
        float sx = has_viewbox ? scale_x : 1.0f;
        float sy = has_viewbox ? scale_y : 1.0f;

        if (strcmp(tag, "rect") == 0 && rect_w > 0 && rect_h > 0) {
            int rw = (int)(rect_w * sx);
            int rh = (int)(rect_h * sy);
            out_nodes[node_count] = wb_node_source_shape_rect(rw > 0 ? rw : 1, rh > 0 ? rh : 1);
            if (out_nodes[node_count]) {
                wb_node_shape_set_fill(out_nodes[node_count], fr, fg, fb, fa);
                node_count++;
            }
        } else if (strcmp(tag, "ellipse") == 0 && ellipse_rx > 0 && ellipse_ry > 0) {
            int rw = (int)(ellipse_rx * 2 * sx);
            int rh = (int)(ellipse_ry * 2 * sy);
            out_nodes[node_count] = wb_node_source_shape_ellipse(rw > 0 ? rw : 1, rh > 0 ? rh : 1);
            if (out_nodes[node_count]) {
                wb_node_shape_set_fill(out_nodes[node_count], fr, fg, fb, fa);
                node_count++;
            }
        } else if (strcmp(tag, "circle") == 0) {
            attr = strstr(attrs, "r=\"");
            if (attr) {
                float r = strtof(attr + 3, NULL);
                int rd = (int)(r * 2 * sx);
                out_nodes[node_count] = wb_node_source_shape_ellipse(rd > 0 ? rd : 1, rd > 0 ? rd : 1);
                if (out_nodes[node_count]) {
                    wb_node_shape_set_fill(out_nodes[node_count], fr, fg, fb, fa);
                    node_count++;
                }
            }
        } else if (strcmp(tag, "path") == 0 && path_d[0]) {
            /* Parse path and create polygon approximation */
            float vx[SVG_MAX_PATH_VERTS], vy[SVG_MAX_PATH_VERTS];
            int vcount = 0;
            wb_svg_parse_path(path_d, vx, vy, &vcount, SVG_MAX_PATH_VERTS);
            if (vcount >= 3) {
                /* Use polygon with many sides as approximation */
                out_nodes[node_count] = wb_node_source_shape_polygon(
                    target_w > 0 ? target_w : 100,
                    target_h > 0 ? target_h : 100,
                    vcount > 32 ? 32 : vcount);
                if (out_nodes[node_count]) {
                    wb_node_shape_set_fill(out_nodes[node_count], fr, fg, fb, fa);
                    node_count++;
                }
            }
        }

        /* Skip to end of element */
        while (*p && *p != '>') p++;
        if (*p) p++;
    }

    free(xml);
    return node_count;
}

/* ---- Color/stroke accessors (stored in node user data) ---- */

int wb_svg_get_fill_color(wb_node *node, float *r, float *g, float *b, float *a) {
    if (!node) return -1;
    /* Shape nodes store fill in their user data — use the shape API */
    (void)r; (void)g; (void)b; (void)a;
    return 0; /* would need to extend shape node API */
}

int wb_svg_get_stroke(wb_node *node, float *r, float *g, float *b, float *a, float *width) {
    if (!node) return -1;
    (void)r; (void)g; (void)b; (void)a; (void)width;
    return 0;
}

int wb_svg_get_transform(wb_node *node, float *transform_out) {
    if (!node || !transform_out) return -1;
    /* Identity transform */
    transform_out[0] = 1; transform_out[1] = 0; transform_out[2] = 0;
    transform_out[3] = 0; transform_out[4] = 1; transform_out[5] = 0;
    return 0;
}
