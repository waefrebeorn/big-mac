/* wbus_bvh.h — BVH (Biovision Hierarchy) motion capture parser (R081).
 *
 * Parses BVH files (the standard mocap format from CMU, cgspeed, etc.) into
 * a bone hierarchy + per-frame joint rotations. This lets us:
 *   1. Load CMU mocap data (2605 free animations)
 *   2. Render 2D skeleton overlays on video
 *   3. Drive wb_char2d bone rigs with real human motion
 *   4. Apply mocap to characters (retargeting)
 *
 * Pure C11, no third party. Opaque structs.
 */
#ifndef WUBUS_WBUS_BVH_H
#define WUBUS_WBUS_BVH_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wb_bvh wb_bvh;

/* ---- Joint channel ordering ------------------------------------------- */
typedef enum {
    BVH_CH_XPOS = 0,
    BVH_CH_YPOS,
    BVH_CH_ZPOS,
    BVH_CH_ZROT,
    BVH_CH_YROT,
    BVH_CH_XROT,
    BVH_CH_COUNT
} bvh_channel;

/* ---- Parsed joint ----------------------------------------------------- */
typedef struct {
    char name[64];
    float offset[3];        /* bone offset from parent */
    int   channel_indices[BVH_CH_COUNT];  /* index into frame data, -1 if unused */
    int   n_channels;       /* 3 or 6 */
    int   parent;           /* parent joint index, -1 = root */
    int   is_site;          /* 1 = end effector (no children) */
} wb_bvh_joint;

/* ---- Load / Free ------------------------------------------------------ */

/* Parse a BVH file from disk. Returns NULL on error. */
wb_bvh *wb_bvh_load(const char *path);

/* Free a loaded BVH. */
void wb_bvh_free(wb_bvh *b);

/* ---- Query ------------------------------------------------------------ */

int    wb_bvh_joint_count(const wb_bvh *b);
int    wb_bvh_frame_count(const wb_bvh *b);
double wb_bvh_frame_time(const wb_bvh *b);    /* seconds per frame */
double wb_bvh_duration(const wb_bvh *b);      /* total seconds */

const wb_bvh_joint *wb_bvh_get_joints(const wb_bvh *b);

/* ---- Frame Sampling --------------------------------------------------- */

/* Get raw channel data for a frame (length = total_channels).
 * Returns pointer into internal buffer (do not free). */
const float *wb_bvh_get_frame(const wb_bvh *b, int frame_idx);

/* Sample interpolated frame at time t (seconds). Writes frame_data
 * (length = total_channels). Returns 0 on success. */
int wb_bvh_sample(const wb_bvh *b, double t, float *frame_data);

/* ---- 2D Skeleton Rendering ------------------------------------------- */

/* Compute 2D joint positions from a sampled frame.
 * Output: joint_positions[i*2+0] = x, joint_positions[i*2+1] = y
 * Camera: simple orthographic projection (x,y from root + joint offsets).
 * scale: pixels per world unit. cx, cy: screen center.
 * Returns number of joints written. */
int wb_bvh_compute_positions_2d(const wb_bvh *b, const float *frame_data,
                                  float *joint_positions, int n_joints,
                                  float scale, float cx, float cy);

/* Render skeleton as colored lines into an RGBA buffer.
 * w, h: buffer dimensions.
 * color: RGB line color (alpha = 255).
 * line_width: thickness in pixels.
 * Returns 0 on success. */
int wb_bvh_render_skeleton(const wb_bvh *b, const float *frame_data,
                            uint8_t *rgba, int w, int h,
                            float scale, float cx, float cy,
                            uint8_t cr, uint8_t cg, uint8_t cb,
                            float line_width);

/* ---- CMU Mocap Integration ------------------------------------------- */

/* Download + convert a CMU subject/motion to BVH.
 * subject_num: CMU subject number (e.g. 6)
 * motion_num:  CMU motion number (e.g. 1 for "walking")
 * out_path:    where to write the .bvh file
 * Returns 0 on success.
 *
 * Requires: amc2bvh binary at the specified path, internet access. */
int wb_bvh_download_cmu(int subject_num, int motion_num,
                         const char *out_path,
                         const char *amc2bvh_path);

/* ---- Error Reporting -------------------------------------------------- */
const char *wb_bvh_error_string(void);

#ifdef __cplusplus
}
#endif

#endif /* WUBUS_WBUS_BVH_H */
