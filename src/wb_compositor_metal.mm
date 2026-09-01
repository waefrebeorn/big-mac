/* wb_compositor_metal.mm — Metal GPU compute pipeline for Big Mac compositor.
 *
 * Objective-C++ layer that wraps Metal for GPU-accelerated effect processing.
 * Falls back to CPU path when Metal is unavailable or fails.
 *
 * Usage:
 *   wb_compositor_metal_init();            // call once at startup
 *   wb_compositor_metal_process_grade(f, lift, gamma, gain, sat);
 *   wb_compositor_metal_shutdown();        // call at exit
 */

#include <Foundation/Foundation.h>
#include <Metal/Metal.h>

extern "C" {
#include "wbus/wbus_compositor.h"
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Maximum texture dimensions supported */
#define METAL_MAX_DIM 8192

/* Metal device lifecycle state */
static id<MTLDevice>              g_device = nil;
static id<MTLCommandQueue>        g_cmdq   = nil;
static id<MTLLibrary>             g_lib    = nil;
static id<MTLComputePipelineState> g_pipe_grade         = nil;
static id<MTLComputePipelineState> g_pipe_gain          = nil;
static id<MTLComputePipelineState> g_pipe_invert_alpha  = nil;
static id<MTLComputePipelineState> g_pipe_white_balance = nil;
static id<MTLComputePipelineState> g_pipe_deep_fry      = nil;
static int g_metal_available = 0;

/* ---- helpers ---- */

static const char *kernel_name_grade         = "grade_primary";
static const char *kernel_name_gain          = "effect_gain";
static const char *kernel_name_invert_alpha  = "effect_invert_alpha";
static const char *kernel_name_white_balance = "effect_white_balance";
static const char *kernel_name_deep_fry      = "effect_deep_fry";

/* Try to load a pre-compiled Metal library from the build directory.
 * Falls back to runtime source compilation if the .metallib isn't found. */
static int load_metal_library(void) {
    NSError *error = nil;

    /* Attempt 1: load pre-compiled metallib from build dir */
    NSArray *metallib_paths = @[
        @"build/metal_shaders.metallib",
        @"metal_shaders.metlib",
    ];
    for (NSString *path in metallib_paths) {
        NSURL *url = [NSURL fileURLWithPath:path];
        if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
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
        fprintf(stderr, "[Metal] failed to read shader source: %s\n",
                [[error localizedDescription] UTF8String]);
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

/* Initialize Metal. Returns 0 on success, -1 if Metal is unavailable. */
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

        /* Create all pipeline states */
        g_pipe_grade         = make_pipeline(kernel_name_grade);
        g_pipe_gain          = make_pipeline(kernel_name_gain);
        g_pipe_invert_alpha  = make_pipeline(kernel_name_invert_alpha);
        g_pipe_white_balance = make_pipeline(kernel_name_white_balance);
        g_pipe_deep_fry      = make_pipeline(kernel_name_deep_fry);

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

/* Check if Metal is available for GPU processing */
int wb_compositor_metal_is_available(void) {
    return g_metal_available;
}

/* Shutdown Metal and release resources */
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

/* Core GPU processing: apply primary color grade to a wb_frame.
 * Runs the Metal compute shader and writes results back to f->px.
 * Returns 0 on success, -1 on failure (caller should fall back to CPU). */
int wb_compositor_metal_process_grade(wb_frame *f,
                                      float lift, float gamma,
                                      float gain, float saturation) {
    if (!g_metal_available || !f || !f->px) return -1;
    if (f->w <= 0 || f->h <= 0) return -1;
    if (f->w > METAL_MAX_DIM || f->h > METAL_MAX_DIM) return -1;
    if (saturation <= 0.0f) saturation = 1.0f;

    @autoreleasepool {
        /* Create input/output textures from the frame's pixel data */
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                    width:(NSUInteger)f->w
                                                                                   height:(NSUInteger)f->h
                                                                                mipmapped:NO];
        [td setStorageMode:MTLStorageModeShared];  /* CPU-visible */
        [td setUsage:MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite];

        /* Input texture: copy frame data in */
        id<MTLTexture> tex_in = [g_device newTextureWithDescriptor:td];
        MTLRegion region = MTLRegionMake2D(0, 0, (NSUInteger)f->w, (NSUInteger)f->h);
        [tex_in replaceRegion:region
                  mipmapLevel:0
                    withBytes:f->px
                  bytesPerRow:(NSUInteger)(f->w * sizeof(float) * 4)];

        /* Output texture */
        id<MTLTexture> tex_out = [g_device newTextureWithDescriptor:td];

        /* Encode compute command */
        id<MTLCommandBuffer> cmd = [g_cmdq commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];

        [enc setComputePipelineState:g_pipe_grade];
        [enc setTexture:tex_in  atIndex:0];
        [enc setTexture:tex_out atIndex:1];

        /* Set grade parameters */
        float params[4] = {lift, gamma, gain, saturation};
        [enc setBytes:params length:sizeof(params) atIndex:0];

        /* Dispatch thread groups */
        MTLSize threadgroup = {16, 16, 1};
        if (threadgroup.width > (NSUInteger)f->w)  threadgroup.width  = (NSUInteger)f->w;
        if (threadgroup.height > (NSUInteger)f->h) threadgroup.height = (NSUInteger)f->h;
        MTLSize grid = {
            ((NSUInteger)f->w + threadgroup.width - 1)  / threadgroup.width,
            ((NSUInteger)f->h + threadgroup.height - 1) / threadgroup.height,
            1
        };

        [enc dispatchThreadgroups:grid threadsPerThreadgroup:threadgroup];
        [enc endEncoding];

        [cmd commit];
        [cmd waitUntilCompleted];

        if ([cmd error]) {
            fprintf(stderr, "[Metal] grade command failed: %s\n",
                    [[[cmd error] localizedDescription] UTF8String]);
            return -1;
        }

        /* Read back results */
        [tex_out getBytes:f->px
              bytesPerRow:(NSUInteger)(f->w * sizeof(float) * 4)
               fromRegion:region
              mipmapLevel:0];

        return 0;
    }
}

/* Apply brightness gain effect on GPU */
int wb_compositor_metal_process_gain(wb_frame *f, float gain) {
    if (!g_metal_available || !f || !f->px) return -1;
    if (f->w <= 0 || f->h <= 0 || f->w > METAL_MAX_DIM || f->h > METAL_MAX_DIM) return -1;

    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                    width:(NSUInteger)f->w
                                                                                   height:(NSUInteger)f->h
                                                                                mipmapped:NO];
        [td setStorageMode:MTLStorageModeShared];
        [td setUsage:MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite];

        id<MTLTexture> tex_in = [g_device newTextureWithDescriptor:td];
        MTLRegion region = MTLRegionMake2D(0, 0, (NSUInteger)f->w, (NSUInteger)f->h);
        [tex_in replaceRegion:region mipmapLevel:0
                    withBytes:f->px
                  bytesPerRow:(NSUInteger)(f->w * sizeof(float) * 4)];

        id<MTLTexture> tex_out = [g_device newTextureWithDescriptor:td];

        id<MTLCommandBuffer> cmd = [g_cmdq commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];

        [enc setComputePipelineState:g_pipe_gain];
        [enc setTexture:tex_in  atIndex:0];
        [enc setTexture:tex_out atIndex:1];
        [enc setBytes:&gain length:sizeof(gain) atIndex:0];

        MTLSize threadgroup = {16, 16, 1};
        MTLSize grid = {((NSUInteger)f->w + 15) / 16, ((NSUInteger)f->h + 15) / 16, 1};
        [enc dispatchThreadgroups:grid threadsPerThreadgroup:threadgroup];
        [enc endEncoding];

        [cmd commit];
        [cmd waitUntilCompleted];

        if ([cmd error]) return -1;

        [tex_out getBytes:f->px
              bytesPerRow:(NSUInteger)(f->w * sizeof(float) * 4)
               fromRegion:region
              mipmapLevel:0];
        return 0;
    }
}

/* Apply deep fry effect on GPU */
int wb_compositor_metal_process_deep_fry(wb_frame *f,
                                         float saturation,
                                         float contrast,
                                         float brightness,
                                         float noise) {
    if (!g_metal_available || !f || !f->px) return -1;
    if (f->w <= 0 || f->h <= 0 || f->w > METAL_MAX_DIM || f->h > METAL_MAX_DIM) return -1;

    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                    width:(NSUInteger)f->w
                                                                                   height:(NSUInteger)f->h
                                                                                mipmapped:NO];
        [td setStorageMode:MTLStorageModeShared];
        [td setUsage:MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite];

        id<MTLTexture> tex_in = [g_device newTextureWithDescriptor:td];
        MTLRegion region = MTLRegionMake2D(0, 0, (NSUInteger)f->w, (NSUInteger)f->h);
        [tex_in replaceRegion:region mipmapLevel:0
                    withBytes:f->px
                  bytesPerRow:(NSUInteger)(f->w * sizeof(float) * 4)];

        id<MTLTexture> tex_out = [g_device newTextureWithDescriptor:td];

        id<MTLCommandBuffer> cmd = [g_cmdq commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];

        [enc setComputePipelineState:g_pipe_deep_fry];
        [enc setTexture:tex_in  atIndex:0];
        [enc setTexture:tex_out atIndex:1];

        float params[4] = {saturation, contrast, brightness, noise};
        [enc setBytes:params length:sizeof(params) atIndex:0];

        MTLSize threadgroup = {16, 16, 1};
        MTLSize grid = {((NSUInteger)f->w + 15) / 16, ((NSUInteger)f->h + 15) / 16, 1};
        [enc dispatchThreadgroups:grid threadsPerThreadgroup:threadgroup];
        [enc endEncoding];

        [cmd commit];
        [cmd waitUntilCompleted];

        if ([cmd error]) return -1;

        [tex_out getBytes:f->px
              bytesPerRow:(NSUInteger)(f->w * sizeof(float) * 4)
               fromRegion:region
              mipmapLevel:0];
        return 0;
    }
}

} /* extern "C" */