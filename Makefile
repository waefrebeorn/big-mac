# Big Mac DAW — build system
# C11, zero-third-party except vendored SDL2 (for portable windowing/audio).
#
# SDL2 is vendored at third_party/SDL2-2.32.10/ (built as static lib). We link
# directly against build/.libs/libSDL2.a and the in-tree include dir — no system
# install, no Homebrew, fully self-contained and portable.

CC       := clang
CXX      := clang++
# R074 hop 148 (G-SF097): deterministic float policy — no fast-math,
# no FMA contraction; renders are bit-reproducible on this machine.
CFLAGS   := -std=c11 -O2 -ffp-contract=off -Wall -Wextra -g -D_THREAD_SAFE -msse2 -MMD -MP
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -g -D_THREAD_SAFE -MMD -MP
INC      := -Iinclude -Iinclude/wbus -Itools -Ithird_party/SDL2-2.32.10/include \
           -Ithird_party/openfx/include \
           -Ithird_party/vst3sdk \
           -Ithird_party/vst3sdk/pluginterfaces \
           -Ithird_party/vst3sdk/pluginterfaces/base \
           -Ithird_party/vst3sdk/pluginterfaces/vst \
           -Ithird_party/vst3sdk/public.sdk/source/vst/hosting \
           -I/Users/waefrebeorn/homebrew/include
LIBS     := third_party/SDL2-2.32.10/build/.libs/libSDL2.a \
            -lm -lobjc -lz -llzma -lbz2 -liconv \
            -Wl,-framework,CoreAudio -Wl,-framework,AudioToolbox \
            -Wl,-weak_framework,CoreHaptics -Wl,-weak_framework,GameController \
            -Wl,-framework,ForceFeedback -Wl,-framework,CoreVideo \
            -Wl,-framework,Cocoa -Wl,-framework,Carbon -Wl,-framework,IOKit \
            -Wl,-weak_framework,QuartzCore -Wl,-weak_framework,Metal -Wl,-framework,CoreMIDI -Wl,-framework,CoreFoundation \
            -framework VideoToolbox -framework CoreFoundation -framework CoreMedia -framework CoreServices -framework Security \
            -L/Users/waefrebeorn/homebrew/lib -lavformat -lavcodec -lswscale -lavutil -lswresample

# Core engine objects
CORE_SRCS := src/wb_core.c src/wb_transport.c src/wb_cmd.c src/wb_session.c src/wb_time_sig.c \
             src/wb_project.c \
             src/wb_dsp.c src/wb_osc.c src/wb_env.c src/wb_filter.c \
             src/wb_comp.c src/wb_reverb.c src/wb_delay.c src/wb_synth.c \
             src/wb_sampler.c src/wb_wav.c src/wb_backend.c src/wb_bg_render.c \
             src/wb_tuner.c src/wb_ui_font.c src/wb_midi_coremidi.c src/wb_clap.c \
             src/wb_session_file.c src/wb_unit.c src/wb_fm.c src/wb_fm_g2.c src/wb_fm_g3.c src/wb_synth_simd.c src/wb_drum_simd.c src/wb_granular.c src/wb_formant.c src/wb_bleep.c src/wb_kaleidoscope.c src/wb_chromakey.c src/wb_datamosh.c src/wb_audio_color.c src/wb_wah.c src/wb_char2d.c src/wb_vfx.c src/wb_vfx_node.c src/wb_light2d.c src/wb_video_edit.c src/wb_video_node.c src/wb_ytp.c src/wb_keys.c src/wb_drums.c \
             src/wb_chorus.c src/wb_eq.c src/wb_automation.c src/wb_recorder.c src/wb_undo.c src/wb_unit_clap.c src/wb_midifx.c src/wb_midi_remote.c src/wb_sat_simd.c src/wb_gate.c src/wb_multiband.c src/wb_captions.c src/wb_video.c src/wb_voice_polish.c src/wb_voice_isolate.c src/wb_fft.c src/wb_param_track.c src/wb_compositor.c src/wb_compositor_pro.c src/wb_compositor_encode.c src/wb_transcript.c src/wb_subtitle_translate.c src/wb_ofx.c src/wb_ofx_plugin_builtin.c src/wb_agent.c src/wb_tts.c src/wb_hpss.c src/wb_workspace.c src/wb_clip_edit.c src/wb_cgi.c src/wb_agi.c src/wb_rast.c src/wb_mesh.c src/wb_anim.c src/wb_bvh.c src/wb_mod.c src/wb_gltf.c src/wb_assets.c src/wb_cgiexport.c src/wb_shadowbin.c src/wb_duck.c src/wb_delivery.c src/wb_perf.c src/wb_perfclip.c src/wb_wavcache.c src/wb_import.c src/wb_capture.c src/wb_export_job.c src/wb_precision.c src/wb_lufs.c src/wb_ai_mix.c src/wb_input.c src/wb_limiter.c src/wb_master_adv.c src/wb_mastering_pro.c src/wb_cgi_react.c src/wb_cgi_bands.c src/wb_timestretch.c src/wb_smf.c src/wb_sf2.c src/wb_bitcrush.c src/wb_sfx.c src/wb_waveview.c src/wb_scenedesc.c src/wb_pattern.c src/wb_tga.c src/wb_csg.c src/wb_graphio.c src/wb_conv.c src/wb_biquad_cascade_simd.c src/wb_comp_simd.c src/wb_mix_simd.c src/wb_stereo.c src/wb_yin.c src/wb_tempo_detect.c src/wb_phaser.c src/wb_pitch_correct.c src/wb_ladder.c src/wb_drum_machine.c src/wb_drum_rack.c src/wb_fuzz.c src/wb_karplus.c src/wb_macro_rack.c src/wb_bass_boost.c src/wb_meme_sounds.c src/wb_beat_sync.c src/wb_auto_captions.c src/wb_chroma_key.c src/wb_stutter.c src/wb_transitions.c src/wb_transitions_pro.c src/wb_ftz.c src/wb_sidechain.c src/wb_pitch_bend.c src/wb_audio_reactive.c src/wb_audio_reactive_node.c src/wb_video_fx_pro.c src/wb_lut_node.c src/wb_tape_stop.c src/wb_spectrum.c src/wb_beat_slicer.c src/wb_parallel_comp.c src/wb_lfo_sidechain.c src/wb_deep_fry.c src/wb_vhs_effect.c src/wb_lyric_video.c src/wb_speed_ramp.c src/wb_transient_shaper.c src/wb_deesser.c src/wb_exciter.c src/wb_text_animate.c src/wb_text_edit.c src/wb_particle.c src/wb_particle_gpu.c src/wb_true_peak.c src/wb_dynamic_eq.c src/wb_stereo_image.c src/wb_mod_matrix.c src/wb_stabilize.c src/wb_stabilize2.c src/wb_chord_detect.c src/wb_audio_to_midi.c src/wb_cloud.c src/wb_cloud_collab.c src/wb_chord_ai.c src/wb_arrange_ai.c src/wb_arpeggiator.c src/wb_quantize.c src/wb_restore.c src/wb_freeze.c src/wb_spectral_edit.c src/wb_vocal_remove.c src/wb_midi_humanize.c src/wb_color_grading.c src/wb_comping.c src/wb_vca.c src/wb_mastering_chain.c src/wb_stem_export.c src/wb_reaction.c src/wb_keyframes.c src/wb_scene_detect.c src/wb_lut.c src/wb_motion_track.c src/wb_text_templates.c src/wb_surround.c src/wb_export_queue.c src/wb_midi_chordgen.c src/wb_proxy.c src/wb_midi_scale.c src/wb_midi_generators.c src/wb_melody_ai.c src/wb_aaf_export.c src/wb_track_folder.c src/wb_warp.c src/wb_vocal_synth.c src/wb_project_templates.c src/wb_wavetable.c src/wb_spatial_audio.c src/wb_linked_tracks.c src/wb_stem_split.c src/wb_autoreframe.c src/wb_dynamics_adv.c src/wb_lottie.c src/wb_sonogram.c src/wb_score.c src/wb_session_view.c src/wb_spectral_fx.c src/wb_edit.c src/wb_edit_serialize.c src/wb_audio_mix.c
CXX_SRCS := src/wb_vst3_host.cpp \
             third_party/vst3sdk/public.sdk/source/vst/hosting/module.cpp \
             third_party/vst3sdk/public.sdk/source/vst/hosting/processdata.cpp \
             third_party/vst3sdk/public.sdk/source/vst/hosting/parameterchanges.cpp \
             third_party/vst3sdk/public.sdk/source/vst/utility/stringconvert.cpp \
             third_party/vst3sdk/public.sdk/source/common/commonstringconvert.cpp \
             third_party/vst3sdk/pluginterfaces/base/funknown.cpp \
             third_party/vst3sdk/pluginterfaces/base/coreiids.cpp \
             third_party/vst3sdk/pluginterfaces/base/ustring.cpp \
             third_party/vst3sdk/public.sdk/source/vst/vstinitiids.cpp
MM_OBJS := build/third_party/vst3sdk/public.sdk/source/vst/hosting/module_mac.mm.o
CORE_OBJS := $(CORE_SRCS:%.c=build/%.o)
CORE_OBJS += $(CXX_SRCS:%.cpp=build/%.o)
CORE_OBJS += $(MM_OBJS)

# ---- targets -------------------------------------------------------------

all: build/wb_daw build/wb_render build/wb_selftest build/wb_test_clap build/test-clap/bigmac-test.clap

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INC) -c $< -o $@

build/third_party/vst3sdk/public.sdk/source/vst/hosting/module_mac.mm.o: third_party/vst3sdk/public.sdk/source/vst/hosting/module_mac.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fobjc-arc $(INC) -c $< -o $@

build/%.o: %.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fobjc-arc $(INC) -c $< -o $@

build/wb_daw: build/tools/wb_daw.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ $(LIBS)

build/wb_render: build/tools/wb_render.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ $(LIBS)

build/wb_selftest: build/tools/wb_selftest.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ $(LIBS)

# ---- CLAP host + test plugin --------------------------------------------

# Minimal test CLAP plugin dylib (compiled without engine headers)
build/test-clap/bigmac-test.clap: tests/test_clap_plugin.c
	@mkdir -p $(dir $@)
	$(CC) -shared -fPIC -O1 -o $@ $<

# CLAP host test executable (links full engine)
build/wb_test_clap: build/tools/test_clap.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm -lobjc $(LIBS)

test_clap: build/test-clap/bigmac-test.clap build/wb_test_clap
	./build/wb_test_clap build/test-clap

# ---- background render test -------------------------------------------
build/wb_test_bg_render: tests/test_bg_render.c $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< $(filter %.o,$^) -lm $(LIBS) -pthread

test_bg_render: build/wb_test_bg_render
	./build/wb_test_bg_render

# ---- tests (the gate) ----------------------------------------------------

test: build/wb_selftest
	./build/wb_selftest

test_launchpad_mk2: build/wb_test_launchpad_mk2
	./build/wb_test_launchpad_mk2

# ---- mk2 driver test ----------------------------------------------------

build/wb_test_launchpad_mk2: tests/test_launchpad_mk2.c $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm -lobjc -Wl,-framework,CoreMIDI -Wl,-framework,CoreFoundation $(LIBS)

build/wb_test_time_sig: tests/test_time_sig.c $(CORE_OBJS)
	$(CC) $(CFLAGS) $(INC) -c tests/test_time_sig.c -o build/test_time_sig.o
	$(CXX) $(CXXFLAGS) $(INC) -o $@ build/test_time_sig.o $(CORE_OBJS) -lm $(LIBS)

test_time_sig: build/wb_test_time_sig
	./build/wb_test_time_sig

# ---- tempo detection test (standalone: only needs wb_tempo_detect.o) -------

build/wb_test_tempo_detect: tests/test_tempo_detect.c build/src/wb_tempo_detect.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_tempo_detect.c build/src/wb_tempo_detect.o -lm

test_tempo_detect: build/wb_test_tempo_detect
	./build/wb_test_tempo_detect

# ---- misc -----------------------------------------------------------------

clean:
	rm -rf build

.PHONY: all clean test test_transport test-clap test_subtitle_translate

build/wb_test_video: build/tools/test_video.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

build/wb_test_export_e2e: build/tools/test_export_e2e.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_video: build/wb_test_video
	./build/wb_test_video

build/wb_test_video_tools: build/tools/test_video_tools.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_video_tools: build/wb_test_video_tools
	./build/wb_test_video_tools

build/wb_test_transcript: build/tools/test_transcript.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_transcript: build/wb_test_transcript
	./build/wb_test_transcript

build/wb_test_text_edit: tests/test_text_edit.c build/src/wb_transcript.o
	$(CC) $(CFLAGS) $(INC) -c tests/test_text_edit.c -o build/test_text_edit.o
	$(CC) $(CFLAGS) $(INC) -o $@ build/test_text_edit.o build/src/wb_transcript.o -lm

test_text_edit: build/wb_test_text_edit
	./build/wb_test_text_edit

build/wb_test_agent: build/tools/test_agent.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_agent: build/wb_test_agent
	./build/wb_test_agent

build/wb_test_ofx: build/tools/test_ofx.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_ofx: build/wb_test_ofx
	./build/wb_test_ofx

build/wb_test_captions: build/tools/test_captions.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_captions: build/wb_test_captions
	./build/wb_test_captions

build/wb_test_voice_isolate: build/tools/test_voice_isolate.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_voice_isolate: build/wb_test_voice_isolate
	./build/wb_test_voice_isolate

build/wb_test_fcpxml: build/tools/test_fcpxml.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_fcpxml: build/wb_test_fcpxml
	./build/wb_test_fcpxml

build/wb_test_aaf_export: tests/test_aaf_export.c $(CORE_OBJS)
	$(CC) $(CFLAGS) $(INC) -c tests/test_aaf_export.c -o build/test_aaf_export.o
	$(CXX) $(CXXFLAGS) $(INC) -o $@ build/test_aaf_export.o $(CORE_OBJS) -lm $(LIBS)

test_aaf_export: build/wb_test_aaf_export
	./build/wb_test_aaf_export

build/wb_test_loudness_meter: build/tools/test_loudness_meter.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_loudness_meter: build/wb_test_loudness_meter
	./build/wb_test_loudness_meter

build/wb_test_g1: build/tools/test_g1.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_g1: build/wb_test_g1
	./build/wb_test_g1

build/wb_mk_podcast: build/tools/mk_podcast.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

mk_podcast: build/wb_mk_podcast
	./build/wb_mk_podcast

build/wb_mk_srt: build/tools/mk_srt.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

mk_srt: build/wb_mk_srt
	./build/wb_mk_srt

build/wb_mk_burn: build/tools/mk_burn.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

mk_burn: build/wb_mk_burn
	./build/wb_mk_burn

build/wb_mk_tts_podcast: build/tools/mk_tts_podcast.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

build/wb_test_synth_timing: build/tools/test_synth_timing.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_synth_timing: build/wb_test_synth_timing
	./build/wb_test_synth_timing

# Fetch the Piper voice model on demand (NOT vendored in git — it's ~120MB).
# The engine (binary + dylibs) is vendored; only the model is fetched here.
# After this runs once, TTS works fully offline.
tts-voice:
	curl -sL --max-time 300 -o third_party/piper/voices/en_US-ryan-high.onnx \
	  https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/ryan/high/en_US-ryan-high.onnx
	curl -sL --max-time 60 -o third_party/piper/voices/en_US-ryan-high.onnx.json \
	  https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/en/en_US/ryan/high/en_US-ryan-high.onnx.json
	@echo "TTS voice model fetched -> third_party/piper/voices/en_US-ryan-high.onnx"

build/wb_test_tts: build/tools/test_tts.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_tts: build/wb_test_tts
	./build/wb_test_tts

build/wb_test_hpss: build/tools/test_hpss.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_hpss: build/wb_test_hpss
	./build/wb_test_hpss

build/wb_test_compositor: build/tools/test_compositor.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_compositor: build/wb_test_compositor
	./build/wb_test_compositor

build/wb_test_voice_polish: build/tools/test_voice_polish.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_voice_polish: build/wb_test_voice_polish
	./build/wb_test_voice_polish

test_export_e2e: build/wb_test_export_e2e
	./build/wb_test_export_e2e

build/wb_test_workspace: build/tools/test_workspace.o build/src/wb_workspace.o
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_workspace: build/wb_test_workspace
	./build/wb_test_workspace

build/wb_test_cgi_agi: build/tools/test_cgi_agi.o build/src/wb_cgi.o build/src/wb_agi.o
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm

build/wb_test_rast: build/tools/test_rast.o build/src/wb_rast.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_rast: build/wb_test_rast
	./build/wb_test_rast

build/wb_test_mesh: build/tools/test_mesh.o build/src/wb_mesh.o build/src/wb_rast.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_mesh: build/wb_test_mesh
	./build/wb_test_mesh

build/wb_test_anim: build/tools/test_anim.o build/src/wb_anim.o build/src/wb_mesh.o build/src/wb_rast.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_anim: build/wb_test_anim
	./build/wb_test_anim

build/wb_test_mod: build/tools/test_mod.o build/src/wb_mod.o build/src/wb_mesh.o build/src/wb_rast.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_mod: build/wb_test_mod
	./build/wb_test_mod

build/wb_test_anim2: build/tools/test_anim2.o build/src/wb_anim.o build/src/wb_mesh.o build/src/wb_rast.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_anim2: build/wb_test_anim2
	./build/wb_test_anim2

build/wb_test_bvh: build/tools/test_bvh.o build/src/wb_bvh.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_bvh: build/wb_test_bvh
	./build/wb_test_bvh

# ---- YTP Director (text description → edit) ----
build/wb_ytp_director: tools/wb_ytp_director.c
	$(CC) $(CFLAGS) $(INC) -o $@ $< -lm

ytp_director_test: build/wb_ytp_director
	./build/wb_ytp_director

build/wb_test_gltf: build/tools/test_gltf.o build/src/wb_gltf.o build/src/wb_mesh.o build/src/wb_rast.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_gltf: build/wb_test_gltf
	./build/wb_test_gltf

build/wb_test_assets: build/tools/test_assets.o build/src/wb_assets.o build/src/wb_gltf.o build/src/wb_mesh.o build/src/wb_rast.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_assets: build/wb_test_assets
	./build/wb_test_assets

build/wb_test_shadowbin: build/tools/test_shadowbin.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_shadowbin: build/wb_test_shadowbin
	./build/wb_test_shadowbin

build/wb_test_duck: build/tools/test_duck.o build/src/wb_duck.o build/src/wb_automation.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_duck: build/wb_test_duck
	./build/wb_test_duck

build/wb_test_perfclip: build/tools/test_perfclip.o build/src/wb_perfclip.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_perfclip: build/wb_test_perfclip
	./build/wb_test_perfclip

build/wb_test_track_folder: build/tests/test_track_folder.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

build/tests/test_track_folder.o: tests/test_track_folder.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

test_track_folder: build/wb_test_track_folder
	./build/wb_test_track_folder

build/wb_test_warp: build/tools/test_warp.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ build/tools/test_warp.o $(CORE_OBJS) -lm $(LIBS)

test_warp: build/wb_test_warp
	./build/wb_test_warp

build/wb_test_thumbnail: build/tools/test_thumbnail.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_thumbnail: build/wb_test_thumbnail
	./build/wb_test_thumbnail

build/wb_test_perf_export: build/tools/test_perf_export.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_perf_export: build/wb_test_perf_export
	./build/wb_test_perf_export

build/wb_test_mv: build/tools/test_mv.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_mv: build/wb_test_mv
	./build/wb_test_mv

build/wb_test_export_mv: build/tools/test_export_mv.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_export_mv: build/wb_test_export_mv
	./build/wb_test_export_mv

# ---- export queue test (R078: wired to real node pipeline) ----

build/wb_test_export_queue: tests/test_export_queue.c $(CORE_OBJS)
	$(CC) $(CFLAGS) $(INC) -c tests/test_export_queue.c -o build/test_export_queue.o
	$(CXX) $(CXXFLAGS) $(INC) -o $@ build/test_export_queue.o $(CORE_OBJS) -lm $(LIBS)

test_export_queue: build/wb_test_export_queue
	./build/wb_test_export_queue

build/wb_test_perf_freeze: build/tools/test_perf_freeze.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_perf_freeze: build/wb_test_perf_freeze
	./build/wb_test_perf_freeze

build/wb_test_perf: build/tools/test_perf.o build/src/wb_perf.o build/src/wb_mesh.o build/src/wb_rast.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_perf: build/wb_test_perf
	./build/wb_test_perf

build/wb_test_delivery: build/tools/test_delivery.o build/src/wb_delivery.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_delivery: build/wb_test_delivery
	./build/wb_test_delivery

build/wb_test_wavcache: build/tools/test_wavcache.o build/src/wb_wavcache.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_wavcache: build/wb_test_wavcache
	./build/wb_test_wavcache


build/wb_test_agent_cgi: build/tools/test_agent_cgi.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_agent_cgi: build/wb_test_agent_cgi
	./build/wb_test_agent_cgi

build/wb_test_cgiexport: build/tools/test_cgiexport.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_cgiexport: build/wb_test_cgiexport
	./build/wb_test_cgiexport


test_cgi_agi: build/wb_test_cgi_agi
	./build/wb_test_cgi_agi

build/wb_test_clip_edit: build/tools/test_clip_edit.o build/src/wb_clip_edit.o
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_clip_edit: build/wb_test_clip_edit
	./build/wb_test_clip_edit

build/wb_test_drum_timing: build/tools/test_drum_timing.o build/src/wb_drums.o build/src/wb_midi_coremidi.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_drum_timing: build/wb_test_drum_timing
	./build/wb_test_drum_timing

build/wb_test_conv: build/tools/test_conv.o build/src/wb_conv.o build/src/wb_fft.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_conv: build/wb_test_conv
	./build/wb_test_conv

build/wb_test_biquad_cascade: build/tools/test_biquad_cascade.o build/src/wb_biquad_cascade_simd.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_biquad_cascade: build/wb_test_biquad_cascade
	./build/wb_test_biquad_cascade

build/wb_test_sat_simd: build/tools/test_sat_simd.o build/src/wb_sat_simd.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_sat_simd: build/wb_test_sat_simd
	./build/wb_test_sat_simd

build/wb_test_granular: build/tools/test_granular.o build/src/wb_granular.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_granular: build/wb_test_granular
	./build/wb_test_granular

build/wb_test_comp_simd: build/tools/test_comp_simd.o build/src/wb_comp_simd.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_comp_simd: build/wb_test_comp_simd
	./build/wb_test_comp_simd

build/wb_test_mix_simd: build/tools/test_mix_simd.o build/src/wb_mix_simd.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_mix_simd: build/wb_test_mix_simd
	./build/wb_test_mix_simd

build/wb_test_stereo: build/tools/test_stereo.o build/src/wb_stereo.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_stereo: build/wb_test_stereo
	./build/wb_test_stereo

build/wb_test_yin: build/tools/test_yin.o build/src/wb_yin.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_yin: build/wb_test_yin
	./build/wb_test_yin

build/wb_test_phaser: build/tools/test_phaser.o build/src/wb_phaser.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_phaser: build/wb_test_phaser
	./build/wb_test_phaser

build/wb_test_pitch_correct: build/tools/test_pitch_correct.o build/src/wb_pitch_correct.o build/src/wb_yin.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_pitch_correct: build/wb_test_pitch_correct
	./build/wb_test_pitch_correct

build/wb_test_ladder: build/tools/test_ladder.o build/src/wb_ladder.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_ladder: build/wb_test_ladder
	./build/wb_test_ladder

build/wb_test_drum_machine: build/tools/test_drum_machine.o build/src/wb_drum_machine.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_drum_machine: build/wb_test_drum_machine
	./build/wb_test_drum_machine

build/wb_test_drum_rack: tests/test_drum_rack.c build/src/wb_drum_rack.o
	$(CC) $(CFLAGS) $(INC) -o $@ $< build/src/wb_drum_rack.o build/src/wb_wav.o -lm

test_drum_rack: build/wb_test_drum_rack
	./build/wb_test_drum_rack

build/wb_test_macro_rack: tests/test_macro_rack.c build/src/wb_macro_rack.o build/src/wb_synth.o build/src/wb_filter.o build/src/wb_comp.o build/src/wb_delay.o build/src/wb_reverb.o build/src/wb_osc.o build/src/wb_env.o build/src/wb_wav.o build/src/wb_dsp.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_macro_rack.c build/src/wb_macro_rack.o build/src/wb_synth.o build/src/wb_filter.o build/src/wb_comp.o build/src/wb_delay.o build/src/wb_reverb.o build/src/wb_osc.o build/src/wb_env.o build/src/wb_wav.o build/src/wb_dsp.o -lm

test_macro_rack: build/wb_test_macro_rack
	./build/wb_test_macro_rack

build/wb_test_midi_scale: tests/test_midi_scale.c build/src/wb_midi_scale.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_midi_scale.c build/src/wb_midi_scale.o -lm

test_midi_scale: build/wb_test_midi_scale
	./build/wb_test_midi_scale

build/wb_test_midi_generators: tests/test_midi_generators.c build/src/wb_midi_generators.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_midi_generators.c build/src/wb_midi_generators.o -lm

test_midi_generators: build/wb_test_midi_generators
	./build/wb_test_midi_generators

build/wb_test_melody_ai: tests/test_melody_ai.c build/src/wb_melody_ai.o build/src/wb_midi_scale.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_melody_ai.c build/src/wb_melody_ai.o build/src/wb_midi_scale.o -lm

test_melody_ai: build/wb_test_melody_ai
	./build/wb_test_melody_ai

build/wb_test_stem_split: tests/test_stem_split.c build/src/wb_stem_split.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_stem_split.c build/src/wb_stem_split.o -lm

test_stem_split: build/wb_test_stem_split
	./build/wb_test_stem_split

build/wb_test_autoreframe: tests/test_autoreframe.c build/src/wb_autoreframe.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_autoreframe.c build/src/wb_autoreframe.o -lm

test_autoreframe: build/wb_test_autoreframe
	./build/wb_test_autoreframe

build/wb_test_dynamics_adv: tests/test_dynamics_adv.c build/src/wb_dynamics_adv.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_dynamics_adv.c build/src/wb_dynamics_adv.o -lm

test_dynamics_adv: build/wb_test_dynamics_adv
	./build/wb_test_dynamics_adv

build/wb_test_lottie: tests/test_lottie.c build/src/wb_lottie.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_lottie.c build/src/wb_lottie.o -lm

test_lottie: build/wb_test_lottie
	./build/wb_test_lottie

build/wb_test_project_templates: tests/test_project_templates.c $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ tests/test_project_templates.c $(CORE_OBJS) $(LIBS)

test_project_templates: build/wb_test_project_templates
	./build/wb_test_project_templates

build/wb_test_wavetable: tests/test_wavetable.c build/src/wb_wavetable.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_wavetable.c build/src/wb_wavetable.o -lm

test_wavetable: build/wb_test_wavetable
	./build/wb_test_wavetable

build/wb_test_spatial_audio: tests/test_spatial_audio.c build/src/wb_spatial_audio.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_spatial_audio.c build/src/wb_spatial_audio.o -lm

test_spatial_audio: build/wb_test_spatial_audio
	./build/wb_test_spatial_audio

build/wb_test_mastering_pro: tests/test_mastering_pro.c build/src/wb_mastering_pro.o build/src/wb_comp.o build/src/wb_lufs.o build/src/wb_true_peak.o build/src/wb_stereo_image.o build/src/wb_filter.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_mastering_pro.c build/src/wb_mastering_pro.o build/src/wb_comp.o build/src/wb_lufs.o build/src/wb_true_peak.o build/src/wb_stereo_image.o build/src/wb_filter.o -lm

test_mastering_pro: build/wb_test_mastering_pro
	./build/wb_test_mastering_pro

build/wb_test_master_adv: tests/test_master_adv.c build/src/wb_master_adv.o build/src/wb_comp.o build/src/wb_lufs.o build/src/wb_true_peak.o build/src/wb_stereo_image.o build/src/wb_filter.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_master_adv.c build/src/wb_master_adv.o build/src/wb_comp.o build/src/wb_lufs.o build/src/wb_true_peak.o build/src/wb_stereo_image.o build/src/wb_filter.o -lm

test_master_adv: build/wb_test_master_adv
	./build/wb_test_master_adv

build/wb_test_vocal_synth: tests/test_vocal_synth.c build/src/wb_vocal_synth.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_vocal_synth.c build/src/wb_vocal_synth.o -lm

test_vocal_synth: build/wb_test_vocal_synth
	./build/wb_test_vocal_synth

build/wb_test_mpe: tests/test_mpe.c build/src/wb_mpe.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_mpe.c build/src/wb_mpe.o -lm

test_mpe: build/wb_test_mpe
	./build/wb_test_mpe

build/wb_test_expression: tests/test_expression.c build/src/wb_expression.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ tests/test_expression.c build/src/wb_expression.o $(CORE_OBJS) -lm $(LIBS)

test_expression: build/wb_test_expression
	./build/wb_test_expression

build/wb_test_fuzz: build/tools/test_fuzz.o build/src/wb_fuzz.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_fuzz: build/wb_test_fuzz
	./build/wb_test_fuzz

build/wb_test_audio_to_midi: build/tests/test_audio_to_midi.o build/src/wb_audio_to_midi.o build/src/wb_yin.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_audio_to_midi: build/wb_test_audio_to_midi
	./build/wb_test_audio_to_midi

build/wb_test_karplus: build/tools/test_karplus.o build/src/wb_karplus.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_karplus: build/wb_test_karplus
	./build/wb_test_karplus

build/wb_test_dsp_simd_timing: build/tools/test_dsp_simd_timing.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_dsp_simd_timing: build/wb_test_dsp_simd_timing
	./build/wb_test_dsp_simd_timing

# ---- spectral edit test (standalone: only needs wb_spectral_edit.o + wb_fft.o) ----
build/wb_test_spectral_edit: tests/test_spectral_edit.c build/src/wb_spectral_edit.o build/src/wb_fft.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_spectral_edit.c build/src/wb_spectral_edit.o build/src/wb_fft.o -lm

test_spectral_edit: build/wb_test_spectral_edit
	./build/wb_test_spectral_edit

# ---- spectral fx test (standalone: only needs wb_spectral_fx.o + wb_fft.o) ----
build/wb_test_spectral_fx: tests/test_spectral_fx.c build/src/wb_spectral_fx.o build/src/wb_fft.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_spectral_fx.c build/src/wb_spectral_fx.o build/src/wb_fft.o -lm

test_spectral_fx: build/wb_test_spectral_fx
	./build/wb_test_spectral_fx

# ---- compositor_pro test ----
build/wb_test_compositor_pro: tests/test_compositor_pro.c build/src/wb_compositor_pro.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_compositor_pro.c build/src/wb_compositor_pro.o -lm

test_compositor_pro: build/wb_test_compositor_pro
	./build/wb_test_compositor_pro

# ---- formant shift test ----
build/wb_test_formant: tests/test_formant.c build/src/wb_formant.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_formant.c build/src/wb_formant.o -lm

test_formant: build/wb_test_formant
	./build/wb_test_formant

# ---- bleep censor test ----
build/wb_test_bleep: tests/test_bleep.c build/src/wb_bleep.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_bleep.c build/src/wb_bleep.o -lm

test_bleep: build/wb_test_bleep
	./build/wb_test_bleep

# ---- kaleidoscope test ----
build/wb_test_kaleidoscope: tests/test_kaleidoscope.c build/src/wb_kaleidoscope.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_kaleidoscope.c build/src/wb_kaleidoscope.o -lm

test_kaleidoscope: build/wb_test_kaleidoscope
	./build/wb_test_kaleidoscope

# ---- chromakey test ----
build/wb_test_chromakey: tests/test_chromakey.c build/src/wb_chromakey.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_chromakey.c build/src/wb_chromakey.o -lm

test_chromakey: build/wb_test_chromakey
	./build/wb_test_chromakey

# ---- datamosh test ----
build/wb_test_datamosh: tests/test_datamosh.c build/src/wb_datamosh.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_datamosh.c build/src/wb_datamosh.o -lm

test_datamosh: build/wb_test_datamosh
	./build/wb_test_datamosh

# ---- audio-reactive color test ----
build/wb_test_audio_color: tests/test_audio_color.c build/src/wb_audio_color.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_audio_color.c build/src/wb_audio_color.o -lm

test_audio_color: build/wb_test_audio_color
	./build/wb_test_audio_color

# ---- auto-wah test ----
build/wb_test_wah: tests/test_wah.c build/src/wb_wah.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_wah.c build/src/wb_wah.o -lm

test_wah: build/wb_test_wah
	./build/wb_test_wah

# ---- restoration test (standalone: only needs wb_restoration.o + wb_fft.o) ----
build/wb_test_restoration: tests/test_restoration.c build/src/wb_restoration.o build/src/wb_fft.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_restoration.c build/src/wb_restoration.o build/src/wb_fft.o -lm

test_restoration: build/wb_test_restoration
	./build/wb_test_restoration

# ---- linked tracks test (full engine) ------------------------------------
build/wb_test_linked_tracks: tests/test_linked_tracks.c $(CORE_OBJS)
	$(CC) $(CFLAGS) $(INC) -c tests/test_linked_tracks.c -o build/test_linked_tracks.o
	$(CXX) $(CXXFLAGS) $(INC) -o $@ build/test_linked_tracks.o $(CORE_OBJS) -lm $(LIBS)

test_linked_tracks: build/wb_test_linked_tracks
	./build/wb_test_linked_tracks

# ---- AI mixing test (standalone: wb_ai_mix + fft + lufs + filter) ----
build/wb_test_ai_mix: tests/test_ai_mix.c build/src/wb_ai_mix.o build/src/wb_fft.o build/src/wb_lufs.o build/src/wb_filter.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_ai_mix.c build/src/wb_ai_mix.o build/src/wb_fft.o build/src/wb_lufs.o build/src/wb_filter.o -lm

test_ai_mix: build/wb_test_ai_mix
	./build/wb_test_ai_mix

# ---- PDC test (standalone: only needs wb_pdc.o) ----
build/wb_test_pdc: tests/test_pdc.c build/src/wb_pdc.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_pdc.c build/src/wb_pdc.o -lm

test_pdc: build/wb_test_pdc
	./build/wb_test_pdc

# ---- podcast production test (standalone: wb_podcast + wb_dsp for biquad) ----
build/wb_test_podcast: tests/test_podcast.c build/src/wb_podcast.o build/src/wb_filter.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_podcast.c build/src/wb_podcast.o build/src/wb_filter.o -lm

test_podcast: build/wb_test_podcast
	./build/wb_test_podcast

build/wb_test_transitions_pro: tests/test_transitions_pro.c build/src/wb_transitions_pro.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_transitions_pro.c build/src/wb_transitions_pro.o -lm

test_transitions_pro: build/wb_test_transitions_pro
	./build/wb_test_transitions_pro

build/wb_test_cloud_collab: tests/test_cloud_collab.c build/src/wb_cloud_collab.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_cloud_collab.c build/src/wb_cloud_collab.o -lm

test_cloud_collab: build/wb_test_cloud_collab
	./build/wb_test_cloud_collab

# ---- score/notation test (standalone: only needs wb_score.o) ----
build/wb_test_score: tests/test_score.c build/src/wb_score.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_score.c build/src/wb_score.o -lm

test_score: build/wb_test_score
	./build/wb_test_score

# ---- sonogram test (standalone: only needs wb_sonogram.o + wb_fft.o) ----
build/wb_test_sonogram: tests/test_sonogram.c build/src/wb_sonogram.o build/src/wb_fft.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_sonogram.c build/src/wb_sonogram.o build/src/wb_fft.o -lm

test_sonogram: build/wb_test_sonogram
	./build/wb_test_sonogram

# Session view test — standalone module, only needs wb_session_view.o
build/wb_test_session_view: tests/test_session_view.c build/src/wb_session_view.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_session_view.c build/src/wb_session_view.o -lm

test_session_view: build/wb_test_session_view
	./build/wb_test_session_view

# ---- cloud sync test (full engine) ------------------------------------
build/wb_test_cloud: tests/test_cloud.c $(CORE_OBJS)
	$(CC) $(CFLAGS) $(INC) -c tests/test_cloud.c -o build/test_cloud.o
	$(CXX) $(CXXFLAGS) $(INC) -o $@ build/test_cloud.o $(CORE_OBJS) -lm $(LIBS)

test_cloud: build/wb_test_cloud
	./build/wb_test_cloud

# ---- subtitle translation test (standalone: wb_subtitle_translate + wb_transcript) ----
build/wb_test_subtitle_translate: tests/test_subtitle_translate.c build/src/wb_subtitle_translate.o build/src/wb_transcript.o
	$(CC) $(CFLAGS) $(INC) -c tests/test_subtitle_translate.c -o build/test_subtitle_translate.o
	$(CC) $(CFLAGS) $(INC) -o $@ build/test_subtitle_translate.o build/src/wb_subtitle_translate.o build/src/wb_transcript.o -lm

test_subtitle_translate: build/wb_test_subtitle_translate
	./build/wb_test_subtitle_translate

build/wb_test_chord_ai: tests/test_chord_ai.c build/src/wb_chord_ai.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_chord_ai.c build/src/wb_chord_ai.o -lm

test_chord_ai: build/wb_test_chord_ai
	./build/wb_test_chord_ai

build/wb_test_color_grading: tests/test_color_grading.c build/src/wb_color_grading.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_color_grading.c build/src/wb_color_grading.o -lm

test_color_grading: build/wb_test_color_grading
	./build/wb_test_color_grading

# ---- Video edit graph test (R084) ----
# Link against all engine objects (same approach as selftest)
build/wb_test_edit: tests/test_edit.c $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< $(filter %.o,$^) $(LIBS)

test_edit: build/wb_test_edit
	./build/wb_test_edit

# ---- Procedural video generation test (R084) ----
build/wb_test_procedural: tests/test_procedural.c $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< $(filter %.o,$^) $(LIBS)

test_procedural: build/wb_test_procedural
	./build/wb_test_procedural

# ---- Professional video FX test (R085) ----
build/wb_test_video_fx_pro: tests/test_video_fx_pro.c $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< $(filter %.o,$^) $(LIBS)

test_video_fx_pro: build/wb_test_video_fx_pro
	./build/wb_test_video_fx_pro

# ---- GPU particle system test (standalone: only needs wb_particle_gpu.o) ----
build/wb_test_particle_gpu: tests/test_particle_gpu.c build/src/wb_particle_gpu.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_particle_gpu.c build/src/wb_particle_gpu.o -lm

test_particle_gpu: build/wb_test_particle_gpu
	./build/wb_test_particle_gpu

# ---- MIDI remote test (standalone: only needs wb_midi_remote.o) ----
build/wb_test_midi_remote: tests/test_midi_remote.c build/src/wb_midi_remote.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_midi_remote.c build/src/wb_midi_remote.o -lm

test_midi_remote: build/wb_test_midi_remote
	./build/wb_test_midi_remote

# G25 fix: header dependency tracking (stale-object nondeterminism)
-include $(wildcard build/*/*.d) $(wildcard build/*.d)

# ---- stabilize2 test (standalone: only needs wb_stabilize2.o) ----
build/wb_test_stabilize2: tests/test_stabilize2.c build/src/wb_stabilize2.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_stabilize2.c build/src/wb_stabilize2.o -lm

test_stabilize2: build/wb_test_stabilize2
	./build/wb_test_stabilize2

# ---- modulation matrix test (standalone: only needs wb_mod_matrix.o) ----
build/wb_test_mod_matrix: tests/test_mod_matrix.c build/src/wb_mod_matrix.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_mod_matrix.c build/src/wb_mod_matrix.o -lm

test_mod_matrix: build/wb_test_mod_matrix
	./build/wb_test_mod_matrix

# ---- audio analysis test (standalone: wb_analysis + wb_fft) ----
build/wb_test_analysis: tests/test_analysis.c build/src/wb_analysis.o build/src/wb_fft.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_analysis.c build/src/wb_analysis.o build/src/wb_fft.o -lm

test_analysis: build/wb_test_analysis
	./build/wb_test_analysis

# ---- spatial audio panner test (standalone: only needs wb_atmos.o) ----
build/wb_test_atmos: tests/test_atmos.c build/src/wb_atmos.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_atmos.c build/src/wb_atmos.o -lm

test_atmos: build/wb_test_atmos
	./build/wb_test_atmos

# ---- AI arrangement test (standalone: only needs wb_arrange_ai.o) ----
build/wb_test_arrange_ai: tests/test_arrange_ai.c build/src/wb_arrange_ai.o
	$(CC) $(CFLAGS) $(INC) -o $@ tests/test_arrange_ai.c build/src/wb_arrange_ai.o -lm

test_arrange_ai: build/wb_test_arrange_ai
	./build/wb_test_arrange_ai
