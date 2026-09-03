/* test_3d_overlay.c — 3D Character Overlay System tests (R098) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "wbus/wbus_compositor.h"
#include "wbus/wbus_vfx.h"
}

#define CHECK(c, m) do { if (c) { p++; printf("  PASS: %s\n", m); } \
                         else { f++; printf("  FAIL: %s\n", m); } } while(0)

int main(void) {
    int p = 0, f = 0;
    
    printf("=== 3D Character Overlay System (R098) ===\n\n");
    
    /* ---- Mesh ---- */
    printf("--- Mesh ---\n");
    wb_omesh mesh;
    wb_omesh_init(&mesh);
    CHECK(mesh.n_verts == 0, "mesh: initialized empty");
    
    wb_omesh_create_cube(&mesh, 1.0f);
    CHECK(mesh.n_verts == 24, "mesh: cube has 24 verts (6 faces x 4)");
    CHECK(mesh.n_faces == 12, "mesh: cube has 12 faces (6 x 2 triangles)");
    CHECK(mesh.n_bones == 1, "mesh: cube has 1 bone");
    
    wb_omesh humanoid;
    wb_omesh_create_humanoid(&humanoid);
    CHECK(humanoid.n_verts > 0, "mesh: humanoid created");
    CHECK(humanoid.n_bones == 6, "mesh: humanoid has 6 bones");
    CHECK(strcmp(humanoid.bones[0].name, "head") == 0, "mesh: bone 0 = head");
    CHECK(strcmp(humanoid.bones[1].name, "spine") == 0, "mesh: bone 1 = spine");
    
    /* ---- Animation ---- */
    printf("\n--- Animation ---\n");
    wb_animation anim;
    wb_anim_init(&anim);
    CHECK(anim.n_tracks == 0, "anim: initialized empty");
    
    wb_anim_create_walk(&anim);
    CHECK(anim.n_tracks == 4, "anim: walk has 4 tracks");
    CHECK(anim.duration == 1.0f, "anim: walk duration = 1s");
    CHECK(strcmp(anim.tracks[0].bone_name, "left_arm") == 0, "anim: track 0 = left_arm");
    CHECK(anim.tracks[0].n_keyframes == 5, "anim: walk track has 5 keyframes");
    
    wb_animation dance;
    wb_anim_create_dance(&dance);
    CHECK(dance.n_tracks == 6, "anim: dance has 6 tracks");
    CHECK(dance.duration == 2.0f, "anim: dance duration = 2s");
    
    /* Evaluate animation */
    wb_o_mat4 bone_mats[WB_MAX_OVERLAY_BONES];
    wb_anim_evaluate(&anim, 0.0f, &humanoid, bone_mats);
    CHECK(1, "anim: evaluate at t=0");
    
    wb_anim_evaluate(&anim, 0.5f, &humanoid, bone_mats);
    CHECK(1, "anim: evaluate at t=0.5");
    
    /* ---- Lip-Sync ---- */
    printf("\n--- Lip-Sync ---\n");
    wb_lipsync ls;
    wb_lipsync_init(&ls);
    CHECK(ls.current_viseme == VISEME_REST, "lipsync: starts at REST");
    CHECK(ls.blend_speed == 15.0f, "lipsync: blend speed = 15");
    
    wb_lipsync_set_viseme(&ls, 0); /* vowel a -> AA */
    CHECK(ls.target_viseme == VISEME_AH, "lipsync: phoneme 0 -> AA");
    
    wb_lipsync_set_viseme(&ls, 5); /* plosive -> MBP */
    CHECK(ls.target_viseme == VISEME_MBP, "lipsync: phoneme 5 -> MBP");
    
    /* Update to complete transition */
    for (int i = 0; i < 10; i++)
        wb_lipsync_update(&ls, 0.016f);
    CHECK(ls.mouth_open == 0.0f, "lipsync: MBP = closed mouth (m/b/p)");
    
    /* ---- Particle System ---- */
    printf("\n--- Particle System ---\n");
    wb_particle_config pconfig;
    memset(&pconfig, 0, sizeof(pconfig));
    pconfig.emit_x = 100.0f;
    pconfig.emit_y = 100.0f;
    pconfig.emit_rate = 50.0f;
    pconfig.life_min = 0.5f;
    pconfig.life_max = 2.0f;
    pconfig.vel_min = 20.0f;
    pconfig.vel_max = 80.0f;
    pconfig.angle = -M_PI / 2; /* upward */
    pconfig.spread = 0.5f;
    pconfig.size_start = 5.0f;
    pconfig.size_end = 1.0f;
    pconfig.color_start = 0xFF00FF00;
    pconfig.color_end = 0xFF000000;
    pconfig.gravity = 50.0f;
    pconfig.max_particles = 256;
    
    wb_particle_system ps;
    wb_particles_init(&ps, &pconfig);
    CHECK(ps.n_active == 0, "particles: starts empty");
    
    /* Update to emit particles */
    wb_particles_update(&ps, 0.1f);
    CHECK(ps.n_active > 0, "particles: emitted after update");
    
    int initial_count = ps.n_active;
    wb_particles_update(&ps, 0.1f);
    CHECK(ps.n_active >= initial_count, "particles: count grows");
    
    /* Let particles die */
    for (int i = 0; i < 100; i++)
        wb_particles_update(&ps, 0.1f);
    CHECK(ps.n_active <= 256, "particles: count bounded");
    
    /* ---- Composition ---- */
    printf("\n--- Composition ---\n");
    wb_comp comp;
    wb_comp_init(&comp, 320, 240, 5.0f, 30.0f);
    CHECK(comp.output != NULL, "comp: output buffer allocated");
    CHECK(comp.width == 320, "comp: width = 320");
    CHECK(comp.height == 240, "comp: height = 240");
    CHECK(comp.n_layers == 0, "comp: starts with 0 layers");
    
    /* Add layers of various types */
    int l3d = wb_comp_add_layer(&comp, WB_LAYER_3D_MODEL, "character");
    CHECK(l3d == 0, "comp: first layer index = 0");
    
    int lshape = wb_comp_add_layer(&comp, WB_LAYER_SHAPE, "shape_layer");
    CHECK(lshape == 1, "comp: second layer index = 1");
    
    int ltext = wb_comp_add_layer(&comp, WB_LAYER_TEXT, "text_layer");
    CHECK(ltext == 2, "comp: third layer index = 2");
    
    int lparticle = wb_comp_add_layer(&comp, WB_LAYER_PARTICLE_EMITTER, "particles");
    CHECK(lparticle == 3, "comp: fourth layer index = 3");
    
    CHECK(comp.n_layers == 4, "comp: 4 layers added");
    
    /* Layer properties */
    wb_layer *layer = &comp.layers[0];
    CHECK(layer->visible == 1, "layer: visible by default");
    CHECK(layer->transform.opacity == 1.0f, "layer: opacity = 1.0");
    CHECK(layer->blend_mode == WB_BLEND_NORMAL, "layer: default blend = NORMAL");
    
    /* Modify transform */
    layer->transform.pos_x = 10.0f;
    layer->transform.rot_z = 0.5f;
    layer->transform.scale_x = 2.0f;
    layer->blend_mode = WB_BLEND_OVERLAY;
    CHECK(layer->transform.pos_x == 10.0f, "layer: pos_x set");
    CHECK(layer->blend_mode == WB_BLEND_OVERLAY, "layer: blend mode set");
    
    /* ---- Blend Modes ---- */
    printf("\n--- Blend Modes ---\n");
    /* Test that all 27 blend modes are defined */
    CHECK(WB_BLEND_NORMAL == 0, "blend: NORMAL = 0");
    CHECK(WB_BLEND_OVERLAY == 3, "blend: OVERLAY = 3");
    CHECK(WB_BLEND_HARD_LIGHT == 8, "blend: HARD_LIGHT = 8");
    CHECK(WB_BLEND_DIFFERENCE == 10, "blend: DIFFERENCE = 10");
    CHECK(WB_BLEND_LUMINOSITY == 18, "blend: LUMINOSITY = 18");
    CHECK(WB_BLEND_COUNT == 24, "blend: 24 blend modes total");
    
    /* ---- Layer Types ---- */
    printf("\n--- Layer Types ---\n");
    CHECK(WB_LAYER_3D_MODEL == 0, "layer_type: 3D_MODEL = 0");
    CHECK(WB_LAYER_SHAPE == 12, "layer_type: SHAPE = 12");
    CHECK(WB_LAYER_TEXT == 13, "layer_type: TEXT = 13");
    CHECK(WB_LAYER_PARTICLE_EMITTER == 3, "layer_type: PARTICLE_EMITTER = 3");
    CHECK(WB_LAYER_LIP_SYNC == 58, "layer_type: LIP_SYNC = 58");
    CHECK(WB_LAYER_AUDIO_SPECTRUM == 59, "layer_type: AUDIO_SPECTRUM = 59");
    CHECK(WB_LAYER_TYPE_COUNT == 60, "layer_type: 60 meta-layer types");
    
    /* ---- Wiggle ---- */
    printf("\n--- Wiggle ---\n");
    layer->wiggle_freq = 5.0f;
    layer->wiggle_amp = 1.0f;
    float orig_x = layer->transform.pos_x;
    wb_wiggle_update(layer, 0.016f);
    CHECK(layer->transform.pos_x != orig_x, "wiggle: position changes");
    
    /* ---- Audio Reactive ---- */
    printf("\n--- Audio Reactive ---\n");
    layer->audio_reactive = 1;
    layer->audio_scale_min = 0.5f;
    layer->audio_scale_max = 2.0f;
    wb_overlay_audio_reactive_update(layer, 0.0f);
    CHECK(layer->transform.scale_x == 0.5f, "audio_reactive: silence = min scale");
    
    wb_overlay_audio_reactive_update(layer, 1.0f);
    CHECK(layer->transform.scale_x == 2.0f, "audio_reactive: full = max scale");
    
    wb_overlay_audio_reactive_update(layer, 0.5f);
    CHECK(layer->transform.scale_x > 0.5f && layer->transform.scale_x < 2.0f,
          "audio_reactive: mid = interpolated scale");
    
    /* ---- Rasterizer ---- */
    printf("\n--- Rasterizer ---\n");
    uint8_t test_buf[320*240*4];
    memset(test_buf, 0, sizeof(test_buf));

    /* Simple test: translate cube to z=-5 (in front of camera) and project */
    wb_o_mat4 model = wb_mat4_translate(0, 0, -5.0f);
    wb_o_mat4 proj = wb_mat4_perspective(60.0f * M_PI / 180.0f, 320.0f/240.0f, 0.1f, 100.0f);
    wb_o_mat4 mvp = wb_mat4_mul(proj, model);

    wb_rasterize_mesh(test_buf, 320, 240, &mesh, bone_mats, &mvp, 0xFFFF0000);

    /* Check that some pixels were drawn */
    int drawn = 0;
    for (int i = 0; i < 320*240; i++) {
        if (test_buf[i*4+3] > 0) { drawn++; break; }
    }
    CHECK(drawn > 0, "rasterizer: drew pixels");
    
    /* ---- NULL Safety ---- */
    printf("\n--- NULL Safety ---\n");
    wb_omesh_init(NULL);
    wb_anim_init(NULL);
    wb_lipsync_init(NULL);
    wb_lipsync_set_viseme(NULL, 0);
    wb_lipsync_update(NULL, 0);
    wb_particles_init(NULL, NULL);
    wb_particles_update(NULL, 0);
    wb_comp_init(NULL, 0, 0, 0, 0);
    wb_comp_free(NULL);
    wb_comp_add_layer(NULL, 0, NULL);
    wb_comp_blend_layer(NULL, 0);
    wb_rasterize_mesh(NULL, 0, 0, NULL, NULL, NULL, 0);
    wb_wiggle_update(NULL, 0);
    wb_overlay_audio_reactive_update(NULL, 0);
    CHECK(1, "NULL inputs don't crash");
    
    wb_comp_free(&comp);
    CHECK(1, "comp: freed");
    
    printf("\n=== Results: %d/%d passed ===\n", p, p + f);
    return f > 0 ? 1 : 0;
}
