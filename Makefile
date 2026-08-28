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
CORE_SRCS := src/wb_core.c src/wb_transport.c src/wb_cmd.c src/wb_session.c \
             src/wb_project.c \
             src/wb_dsp.c src/wb_osc.c src/wb_env.c src/wb_filter.c \
             src/wb_comp.c src/wb_reverb.c src/wb_delay.c src/wb_synth.c \
             src/wb_sampler.c src/wb_wav.c src/wb_backend.c \
             src/wb_tuner.c src/wb_ui_font.c src/wb_midi_coremidi.c src/wb_clap.c \
             src/wb_session_file.c src/wb_unit.c src/wb_fm.c src/wb_fm_g2.c src/wb_fm_g3.c src/wb_synth_simd.c src/wb_drum_simd.c src/wb_granular.c src/wb_char2d.c src/wb_vfx.c src/wb_light2d.c src/wb_video_edit.c src/wb_ytp.c src/wb_keys.c src/wb_drums.c \
             src/wb_chorus.c src/wb_eq.c src/wb_automation.c src/wb_recorder.c src/wb_undo.c src/wb_unit_clap.c src/wb_modulation.c src/wb_midifx.c src/wb_sat_simd.c src/wb_gate.c src/wb_multiband.c src/wb_captions.c src/wb_video.c src/wb_voice_polish.c src/wb_voice_isolate.c src/wb_fft.c src/wb_param_track.c src/wb_compositor.c src/wb_transcript.c src/wb_ofx.c src/wb_ofx_plugin_builtin.c src/wb_agent.c src/wb_tts.c src/wb_hpss.c src/wb_workspace.c src/wb_clip_edit.c src/wb_cgi.c src/wb_agi.c src/wb_rast.c src/wb_mesh.c src/wb_anim.c src/wb_mod.c src/wb_gltf.c src/wb_assets.c src/wb_cgiexport.c src/wb_shadowbin.c src/wb_duck.c src/wb_delivery.c src/wb_perf.c src/wb_perfclip.c src/wb_wavcache.c src/wb_import.c src/wb_capture.c src/wb_export_job.c src/wb_precision.c src/wb_lufs.c src/wb_input.c src/wb_limiter.c src/wb_cgi_react.c src/wb_cgi_bands.c src/wb_timestretch.c src/wb_smf.c src/wb_sf2.c src/wb_bitcrush.c src/wb_sfx.c src/wb_waveview.c src/wb_scenedesc.c src/wb_pattern.c src/wb_tga.c src/wb_csg.c src/wb_graphio.c src/wb_conv.c src/wb_biquad_cascade_simd.c src/wb_comp_simd.c src/wb_mix_simd.c src/wb_stereo.c src/wb_yin.c src/wb_phaser.c src/wb_pitch_correct.c src/wb_ladder.c src/wb_drum_machine.c src/wb_fuzz.c src/wb_karplus.c src/wb_bass_boost.c src/wb_meme_sounds.c
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

test-clap: build/test-clap/bigmac-test.clap build/wb_test_clap
	./build/wb_test_clap build/test-clap

# ---- tests (the gate) ----------------------------------------------------

test: build/wb_selftest
	./build/wb_selftest

test_launchpad_mk2: build/wb_test_launchpad_mk2
	./build/wb_test_launchpad_mk2

# ---- mk2 driver test ----------------------------------------------------

build/wb_test_launchpad_mk2: tests/test_launchpad_mk2.c $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm -lobjc -Wl,-framework,CoreMIDI -Wl,-framework,CoreFoundation $(LIBS)

# ---- misc -----------------------------------------------------------------

clean:
	rm -rf build

.PHONY: all clean test test_transport test-clap

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

build/wb_test_fuzz: build/tools/test_fuzz.o build/src/wb_fuzz.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_fuzz: build/wb_test_fuzz
	./build/wb_test_fuzz

build/wb_test_karplus: build/tools/test_karplus.o build/src/wb_karplus.o
	$(CC) $(CFLAGS) $(INC) -o $@ $^ -lm

test_karplus: build/wb_test_karplus
	./build/wb_test_karplus

# G25 fix: header dependency tracking (stale-object nondeterminism)
-include $(wildcard build/*/*.d) $(wildcard build/*.d)
