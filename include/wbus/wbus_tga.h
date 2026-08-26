/* wbus_tga.h — R074 hop 181 (G-SF014): minimal TGA image loader for
 * texturing. Uncompressed 24/32-bit true-color only (the common case). */
#ifndef WUBUS_TGA_H
#define WUBUS_TGA_H

#include <stdint.h>

typedef struct {
    int w, h;
    uint8_t *px;      /* rgba, w*h*4 (caller frees) */
} wb_tga;

/* Load from file. Returns 0 on success; -1 on error (px untouched). */
int  wb_tga_load(wb_tga *t, const char *path);
void wb_tga_free(wb_tga *t);

#endif /* WUBUS_TGA_H */
