/* wb_compositor_metal.mm — Metal GPU compute pipeline for Big Mac compositor.
 *
 * Objective-C++ layer that wraps Metal for GPU-accelerated effect processing.
 * Falls back to CPU path when Metal is unavailable or fails.
 *
 * Uses buffer-based compute (not textures) for maximum compatibility with
 * older Intel GPUs that may not support shader-write on float textures.
 */

#include <Foundation/Foundation.h>
#include <Metal/Metal.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Maximum buffer size */
#define METAL_MAX_PIXELS (8192 * 8192)

/* Metal device lifecycle state */
static id<MTLDevice>              g_device = nil;
static id<MTLCommandQueue>        g_cmdq   = nil;
static id<MTLLibrary>             g_lib    = nil;
static id<MTLComputePipelineState> g_pipe_grade         = nil;
static id<MTLComputePipelineState> g_pipe_gain          = nil;
static id<MTLComputePipelineState> g_pipe_invert_alpha  = nil;
static id<MTLComputePipelineState> g_pipe_white_balance = nil;
static id<MTLComputePipelineState> g_pipe_deep_fry      = nil;
static id<MTLComputePipelineState> g_pipe_pq_encode     = nil;
static id<MTLComputePipelineState> g_pipe_hlg_encode    = nil;
static id<MTLComputePipelineState> g_pipe_aces_tonemap  = nil;
static int g_metal_available = 0;

/* ---- helpers ---- */

static int load_metal_library(void) {
    NSError *error = nil;

    /* Attempt 1: load pre-compiled metallib from build dir */
    NSArray *metallib_paths = @[
        @"build/metal_shaders.metallib",
        @"metal_shaders.metlib",
    ];
    for (NSString *path in metallib_paths) {
        if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
            NSURL *url = [NSURL fileURLWithPath:path];
            id<MTLLibrary> lib = [g_device newLibraryWithURL:url error:&error];
            if (lib) {
                g_lib = lib;
                fprintf(stderr, "[Metal] loaded metallib: %s\n", [path UTF8String]);
                return 0;
            }
        }
    }

    /* Attempt 2: compile from source at runtime */
    NSString *src_path = @"shaders/metal_shaders.metal";
    if (![[NSFileManager defaultManager] fileExistsAtPath:src_path]) {
        fprintf(stderr, "[Metal] shader source not found: %s\n", [src_path UTF8String]);
        return -1;
    }

    NSString *src = [NSString stringWithContentsOfFile:src_path
                                              encoding:NSUTF8StringEncoding
                                                 error:&error];
    if (!src) {
        fprintf(stderr, "[Metal] failed to read shader source\n");
        return -1;
    }

    id<MTLLibrary> lib = [g_device newLibraryWithSource:src
                                                options:nil
                                                  error:&error];
    if (!lib) {
        fprintf(stderr, "[Metal] shader compile failed: %s\n",
                [[error localizedDescription] UTF8String]);
        return -1;
    }
    g_lib = lib;
    fprintf(stderr, "[Metal] compiled shaders from source\n");
    return 0;
}

static id<MTLComputePipelineState> make_pipeline(const char *name) {
    NSString *ns_name = [NSString stringWithUTF8String:name];
    id<MTLFunction> fn = [g_lib newFunctionWithName:ns_name];
    if (!fn) {
        fprintf(stderr, "[Metal] kernel '%s' not found\n", name);
        return nil;
    }
    NSError *error = nil;
    id<MTLComputePipelineState> pipe = [g_device newComputePipelineStateWithFunction:fn
                                                                              error:&error];
    if (!pipe) {
        fprintf(stderr, "[Metal] pipeline '%s' failed: %s\n", name,
                [[error localizedDescription] UTF8String]);
        return nil;
    }
    return pipe;
}

/* ---- public API (C-callable) ---- */

extern "C" {

int wb_compositor_metal_init(void) {
    if (g_metal_available) return 0;

    @autoreleasepool {
        g_device = MTLCreateSystemDefaultDevice();
        if (!g_device) {
            fprintf(stderr, "[Metal] no Metal-capable GPU found\n");
            return -1;
        }

        g_cmdq = [g_device newCommandQueue];
        if (!g_cmdq) {
            fprintf(stderr, "[Metal] failed to create command queue\n");
            return -1;
        }

        if (load_metal_library() != 0) {
            fprintf(stderr, "[Metal] failed to load shader library\n");
            return -1;
        }

        g_pipe_grade         = make_pipeline("grade_primary");
        g_pipe_gain          = make_pipeline("effect_gain");
        g_pipe_invert_alpha  = make_pipeline("effect_invert_alpha");
        g_pipe_white_balance = make_pipeline("effect_white_balance");
        g_pipe_deep_fry      = make_pipeline("effect_deep_fry");

        if (!g_pipe_grade) {
            fprintf(stderr, "[Metal] critical kernel 'grade_primary' missing\n");
            return -1;
        }

        fprintf(stderr, "[Metal] initialized on '%s'\n",
                [[g_device name] UTF8String]);
        g_metal_available = 1;
        return 0;
    }
}

int wb_compositor_metal_is_available(void) {
    return g_metal_available;
}

void wb_compositor_metal_shutdown(void) {
    @autoreleasepool {
        g_pipe_deep_fry      = nil;
        g_pipe_white_balance = nil;
        g_pipe_invert_alpha  = nil;
        g_pipe_gain          = nil;
        g_pipe_grade         = nil;
        g_lib                = nil;
        g_cmdq               = nil;
        g_device             = nil;
        g_metal_available    = 0;
    }
}

/* Helper: run a compute shader on a shared buffer of float4 pixels. */
static int run_compute_buffer(id<MTLComputePipelineState> pipe,
                               id<MTLBuffer> buf_in,
                               id<MTLBuffer> buf_out,
                               int n_pixels,
                               const void *params, size_t params_size) {
    @autoreleasepool {
        id<MTLCommandBuffer> cmd = [g_cmdq commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];

        [enc setComputePipelineState:pipe];
        [enc setBuffer:buf_in  offset:0 atIndex:0];
        [enc setBuffer:buf_out offset:0 atIndex:1];
        if (params && params_size > 0) {
            [enc setBytes:params length:params_size atIndex:2];
        }

        /* Thread group size 256 for 1D compute */
        NSUInteger tg = 256;
        if (tg > (NSUInteger)n_pixels) tg = (NSUInteger)n_pixels;
        MTLSize threadgroup = {tg, 1, 1};
        MTLSize grid = {((NSUInteger)n_pixels + tg - 1) / tg, 1, 1};

        [enc dispatchThreadgroups:grid threadsPerThreadgroup:threadgroup];
        [enc endEncoding];

        [cmd commit];
        [cmd waitUntilCompleted];

        if ([cmd error]) {
            fprintf(stderr, "[Metal] compute failed: %s\n",
                    [[[cmd error] localizedDescription] UTF8String]);
            return -1;
        }
        return 0;
    }
}

int wb_compositor_metal_process_grade(wb_frame *f,
                                      float lift, float gamma,
                                      float gain, float saturation) {
    if (!g_metal_available || !f || !f->px) return -1;
    if (f->w <= 0 || f->h <= 0) return -1;
    int n = f->w * f->h;
    if (n > METAL_MAX_PIXELS) return -1;
    if (saturation <= 0.0f) saturation = 1.0f;

    @autoreleasepool {
        size_t buf_size = (size_t)n * sizeof(float) * 4;

        /* Create shared buffers (CPU-visible) */
        id<MTLBuffer> buf_in = [g_device newBufferWithBytes:f->px
                                                    length:buf_size
                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> buf_out = [g_device newBufferWithLength:buf_size
                                                     options:MTLResourceStorageModeShared];
        if (!buf_in || !buf_out) return -1;

        float params[4] = {lift, gamma, gain, saturation};
        int result = run_compute_buffer(g_pipe_grade, buf_in, buf_out, n,
                                        params, sizeof(params));

        if (result == 0) {
            /* Copy results back */
            memcpy(f->px, [buf_out contents], buf_size);
        }
        return result;
    }
}

int wb_compositor_metal_process_gain(wb_frame *f, float gain) {
    if (!g_metal_available || !f || !f->px) return -1;
    if (f->w <= 0 || f->h <= 0) return -1;
    int n = f->w * f->h;
    if (n > METAL_MAX_PIXELS) return -1;

    @autoreleasepool {
        size_t buf_size = (size_t)n * sizeof(float) * 4;

        id<MTLBuffer> buf_in = [g_device newBufferWithBytes:f->px
                                                    length:buf_size
                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> buf_out = [g_device newBufferWithLength:buf_size
                                                     options:MTLResourceStorageModeShared];
        if (!buf_in || !buf_out) return -1;

        int result = run_compute_buffer(g_pipe_gain, buf_in, buf_out, n,
                                        &gain, sizeof(gain));

        if (result == 0) {
            memcpy(f->px, [buf_out contents], buf_size);
        }
        return result;
    }
}

int wb_compositor_metal_process_deep_fry(wb_frame *f,
                                         float saturation,
                                         float contrast,
                                         float brightness,
                                         float noise) {
    if (!g_metal_available || !f || !f->px) return -1;
    if (f->w <= 0 || f->h <= 0) return -1;
    int n = f->w * f->h;
    if (n > METAL_MAX_PIXELS) return -1;

    @autoreleasepool {
        size_t buf_size = (size_t)n * sizeof(float) * 4;

        id<MTLBuffer> buf_in = [g_device newBufferWithBytes:f->px
                                                    length:buf_size
                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> buf_out = [g_device newBufferWithLength:buf_size
                                                     options:MTLResourceStorageModeShared];
        if (!buf_in || !buf_out) return -1;

        float params[4] = {saturation, contrast, brightness, noise};
        int result = run_compute_buffer(g_pipe_deep_fry, buf_in, buf_out, n,
                                        params, sizeof(params));

        if (result == 0) {
            memcpy(f->px, [buf_out contents], buf_size);
        }
        return result;
    }
}

int wb_compositor_metal_process_white_balance(wb_frame *f,
                                               float temp,
                                               float tint) {
    if (!g_metal_available || !f || !f->px) return -1;
    if (f->w <= 0 || f->h <= 0) return -1;
    int n = f->w * f->h;
    if (n > METAL_MAX_PIXELS) return -1;

    @autoreleasepool {
        size_t buf_size = (size_t)n * sizeof(float) * 4;

        id<MTLBuffer> buf_in = [g_device newBufferWithBytes:f->px
                                                    length:buf_size
                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> buf_out = [g_device newBufferWithLength:buf_size
                                                     options:MTLResourceStorageModeShared];
        if (!buf_in || !buf_out) return -1;

        /* Pack temp/tint into float4 (match shader constant float2 at index 2) */
        float params[2] = {temp, tint};
        int result = run_compute_buffer(g_pipe_white_balance, buf_in, buf_out, n,
                                        params, sizeof(params));

        if (result == 0) {
            memcpy(f->px, [buf_out contents], buf_size);
        }
        return result;
    }
}

/* ---- HDR compute (R091) ---- */
extern "C" int wb_compositor_metal_process_hdr(float *float_buf, int n, int mode) {
    if (!g_device || n <= 0 || !float_buf) return -1;
    if (!g_pipe_pq_encode) return -1;

    @autoreleasepool {
        int buf_size = n * 4 * sizeof(float);
        id<MTLBuffer> buf_in = [g_device newBufferWithBytes:float_buf length:buf_size options:MTLResourceStorageModeShared];
        id<MTLBuffer> buf_out = [g_device newBufferWithLength:buf_size options:MTLResourceStorageModeShared];
        if (!buf_in || !buf_out) return -1;

        id<MTLComputePipelineState> pipe = nil;
        switch (mode) {
            case 0: pipe = g_pipe_aces_tonemap; break;
            case 1: pipe = g_pipe_pq_encode; break;
            case 2: pipe = g_pipe_hlg_encode; break;
            default: pipe = g_pipe_aces_tonemap; break;
        }
        if (!pipe) return -1;

        id<MTLCommandBuffer> cmd = [g_cmdq commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:pipe];
        [enc setBuffer:buf_in offset:0 atIndex:0];
        [enc setBuffer:buf_out offset:0 atIndex:1];

        MTLSize grid = MTLSizeMake(n, 1, 1);
        NSUInteger wg = [pipe maxTotalThreadsPerThreadgroup];
        if (wg > (NSUInteger)n) wg = (NSUInteger)n;
        MTLSize threadgroup = MTLSizeMake(wg, 1, 1);
        [enc dispatchThreads:grid threadsPerThreadgroup:threadgroup];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        if (cmd.error) return -1;
        memcpy(float_buf, [buf_out contents], buf_size);
        return 0;
    }
}

} /* extern "C" */