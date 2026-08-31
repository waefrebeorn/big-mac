/* wb_mocap_overlay.c — Render BVH mocap skeleton overlay on video (R083).
 *
 * Pipeline:
 *   1. Load BVH file (motion capture data)
 *   2. For each video frame, sample BVH at corresponding time
 *   3. Render skeleton as colored lines on transparent RGBA buffer
 *   4. Use ffmpeg to overlay skeleton on video
 *
 * Usage: ./build/wb_mocap_overlay <video.mp4> <motion.bvh> <output.mp4> [options]
 * Options:
 *   --color R,G,B     skeleton color (default: 0,255,0)
 *   --scale N         skeleton scale (default: 1.0)
 *   --xoff N          X offset in pixels (default: center)
 *   --yoff N          Y offset in pixels (default: center)
 *   --flip            horizontally flip the motion
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "wbus/wbus_bvh.h"

#define MAX_CMD 4096
#define MAX_JOINTS 128

/* Draw a line on RGBA buffer (Bresenham) */
static void draw_line(uint8_t *rgba, int w, int h,
                       int x0, int y0, int x1, int y1,
                       uint8_t r, uint8_t g, uint8_t b, int thickness) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    
    for (int step = 0; step < 10000; step++) {
        for (int tx = -thickness/2; tx <= thickness/2; tx++) {
            for (int ty = -thickness/2; ty <= thickness/2; ty++) {
                int px = x0 + tx, py = y0 + ty;
                if (px >= 0 && px < w && py >= 0 && py < h) {
                    int idx = (py * w + px) * 4;
                    rgba[idx+0] = r;
                    rgba[idx+1] = g;
                    rgba[idx+2] = b;
                    rgba[idx+3] = 255;
                }
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Render skeleton frame to RGBA buffer */
static int render_skeleton(uint8_t *rgba, int w, int h,
                            const wb_bvh *bvh, const float *frame_data,
                            float scale, int xoff, int yoff,
                            uint8_t cr, uint8_t cg, uint8_t cb, int flip) {
    int n_joints = wb_bvh_joint_count(bvh);
    if (n_joints > MAX_JOINTS) n_joints = MAX_JOINTS;
    
    /* Clear buffer */
    memset(rgba, 0, w * h * 4);
    
    /* Compute 2D positions */
    float positions[MAX_JOINTS * 2];
    wb_bvh_compute_positions_2d(bvh, frame_data, positions, n_joints,
                                 scale, (float)xoff, (float)yoff);
    
    /* Draw bones */
    const wb_bvh_joint *joints = wb_bvh_get_joints(bvh);
    for (int j = 0; j < n_joints; j++) {
        if (joints[j].parent < 0) continue;
        if (joints[j].is_site) continue;
        
        int parent = joints[j].parent;
        
        int x0 = (int)(positions[j*2+0] * (flip ? -1 : 1)) + xoff;
        int y0 = (int)(positions[j*2+1]);
        int x1 = (int)(positions[parent*2+0] * (flip ? -1 : 1)) + xoff;
        int y1 = (int)(positions[parent*2+1]);
        
        draw_line(rgba, w, h, x0, y0, x1, y1, cr, cg, cb, 3);
    }
    
    /* Draw joint dots */
    for (int j = 0; j < n_joints; j++) {
        if (joints[j].is_site) continue;
        int x = (int)(positions[j*2+0] * (flip ? -1 : 1)) + xoff;
        int y = (int)(positions[j*2+1]);
        draw_line(rgba, w, h, x-3, y-3, x+3, y+3, 255, 255, 255, 2);
    }
    
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <video.mp4> <motion.bvh> <output.mp4> [options]\n", argv[0]);
        printf("Options:\n");
        printf("  --color R,G,B   skeleton color (default: 0,255,0 green)\n");
        printf("  --scale N       skeleton scale (default: 1.5)\n");
        printf("  --xoff N        X offset (default: video width/2)\n");
        printf("  --yoff N        Y offset (default: video height/2)\n");
        printf("  --flip          flip motion horizontally\n");
        return 1;
    }
    
    const char *video = argv[1];
    const char *bvh_file = argv[2];
    const char *output = argv[3];
    
    uint8_t cr = 0, cg = 255, cb = 0;
    float scale = 1.5f;
    int xoff = -1, yoff = -1;
    int flip = 0;
    
    /* Parse options */
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--color") == 0 && i+1 < argc) {
            sscanf(argv[++i], "%hhu,%hhu,%hhu", &cr, &cg, &cb);
        } else if (strcmp(argv[i], "--scale") == 0 && i+1 < argc) {
            scale = atof(argv[++i]);
        } else if (strcmp(argv[i], "--xoff") == 0 && i+1 < argc) {
            xoff = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--yoff") == 0 && i+1 < argc) {
            yoff = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--flip") == 0) {
            flip = 1;
        }
    }
    
    /* Load BVH */
    printf("Loading BVH: %s\n", bvh_file);
    wb_bvh *bvh = wb_bvh_load(bvh_file);
    if (!bvh) {
        printf("ERROR: %s\n", wb_bvh_error_string());
        return 1;
    }
    
    int n_joints = wb_bvh_joint_count(bvh);
    int n_frames = wb_bvh_frame_count(bvh);
    double duration = wb_bvh_duration(bvh);
    printf("  Joints: %d | Frames: %d | Duration: %.1fs\n",
           n_joints, n_frames, duration);
    
    /* Get video info */
    char cmd[MAX_CMD];
    snprintf(cmd, sizeof(cmd),
        "ffprobe -v error -select_streams v:0 "
        "-show_entries stream=width,height,duration,r_frame_rate "
        "-of csv=p=0 \"%s\"", video);
    
    FILE *probe = popen(cmd, "r");
    int vw = 854, vh = 480;
    double vdur = duration;
    if (probe) {
        char line[256];
        if (fgets(line, sizeof(line), probe)) {
            char *p = line;
            vw = atoi(p);
            p = strchr(p, ','); if (p) vh = atoi(++p);
            p = strchr(p, ','); if (p) vdur = atof(++p);
        }
        pclose(probe);
    }
    
    if (xoff < 0) xoff = vw / 2;
    if (yoff < 0) yoff = vh / 2;
    
    printf("  Video: %dx%d @ %.1fs\n", vw, vh, vdur);
    printf("  Skeleton: color=(%d,%d,%d) scale=%.1f offset=(%d,%d)%s\n",
           cr, cg, cb, scale, xoff, yoff, flip ? " [flipped]" : "");
    
    /* Render skeleton frames as PNG sequence */
    printf("Rendering skeleton overlay...\n");
    
    int total_frames = (int)(vdur * 24); /* 24 fps */
    uint8_t *rgba = calloc(vw * vh * 4, 1);
    float frame_data[768];
    
    /* Create temp dir for frames */
    system("mkdir -p /tmp/mocap_frames");
    
    for (int f = 0; f < total_frames; f++) {
        double t = (double)f / 24.0;
        if (t > duration) t = duration;
        
        /* Sample BVH at this time */
        wb_bvh_sample(bvh, t, frame_data);
        
        /* Render skeleton */
        render_skeleton(rgba, vw, vh, bvh, frame_data,
                        scale, xoff, yoff, cr, cg, cb, flip);
        
        /* Save as PNG */
        char png_path[256];
        snprintf(png_path, sizeof(png_path),
                 "/tmp/mocap_frames/skel_%05d.png", f);
        
        /* Write PPM then convert to PNG (simpler than libpng) */
        char ppm_path[256];
        snprintf(ppm_path, sizeof(ppm_path),
                 "/tmp/mocap_frames/skel_%05d.ppm", f);
        
        FILE *ppm = fopen(ppm_path, "wb");
        if (ppm) {
            fprintf(ppm, "P6\n%d %d\n255\n", vw, vh);
            for (int p = 0; p < vw * vh; p++) {
                /* Only write non-transparent pixels */
                if (rgba[p*4+3] > 0) {
                    fputc(rgba[p*4+0], ppm);
                    fputc(rgba[p*4+1], ppm);
                    fputc(rgba[p*4+2], ppm);
                } else {
                    fputc(0, ppm);
                    fputc(0, ppm);
                    fputc(0, ppm);
                }
            }
            fclose(ppm);
            
            /* Convert to PNG with alpha */
            snprintf(cmd, sizeof(cmd),
                "ffmpeg -y -i \"%s\" -vf "
                "\"colorkey=0x000000:0.01:0,format=rgba\" "
                "\"%s\" 2>/dev/null",
                ppm_path, png_path);
            system(cmd);
            unlink(ppm_path);
        }
        
        if (f % 24 == 0) {
            printf("  Frame %d/%d (%.0fs)\n", f, total_frames, t);
        }
    }
    
    free(rgba);
    wb_bvh_free(bvh);
    
    /* Overlay skeleton on video using ffmpeg */
    printf("Compositing with video...\n");
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -i \"%s\" -framerate 24 -i \"/tmp/mocap_frames/skel_%%05d.png\" "
        "-filter_complex "
        "\"[1:v]colorchannelmixer=aa=0.8[skel];"
        "[0:v][skel]overlay=0:0:shortest=1[out]\" "
        "-map \"[out]\" -map 0:a? "
        "-c:v libx264 -preset ultrafast -crf 28 -r 24 "
        "-c:a aac -b:a 64k -movflags +faststart "
        "\"%s\" 2>&1 | tail -3",
        video, output);
    
    printf("Rendering: %s\n", output);
    int ret = system(cmd);
    
    /* Cleanup */
    printf("Cleaning up temp frames...\n");
    system("rm -rf /tmp/mocap_frames");
    
    if (ret == 0) {
        printf("✓ Done! %s\n", output);
    } else {
        printf("✗ Failed (exit %d)\n", ret);
    }
    
    return ret;
}
