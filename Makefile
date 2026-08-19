# Big Mac DAW — build system
# C11, zero-third-party except vendored SDL2 (for portable windowing/audio).
#
# SDL2 is vendored at third_party/SDL2-2.32.10/ (built as static lib). We link
# directly against build/.libs/libSDL2.a and the in-tree include dir — no system
# install, no Homebrew, fully self-contained and portable.

CC       := clang
CXX      := clang++
CFLAGS   := -std=c11 -O2 -Wall -Wextra -g -D_THREAD_SAFE
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -g -D_THREAD_SAFE
INC      := -Iinclude -Iinclude/wbus -Ithird_party/SDL2-2.32.10/include \
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
             src/wb_dsp.c src/wb_osc.c src/wb_env.c src/wb_filter.c \
             src/wb_comp.c src/wb_reverb.c src/wb_delay.c src/wb_synth.c \
             src/wb_sampler.c src/wb_wav.c src/wb_backend.c \
             src/wb_tuner.c src/wb_ui_font.c src/wb_midi_coremidi.c src/wb_clap.c \
             src/wb_session_file.c src/wb_unit.c src/wb_fm.c src/wb_drums.c \
             src/wb_chorus.c src/wb_eq.c src/wb_automation.c src/wb_recorder.c src/wb_undo.c src/wb_unit_clap.c src/wb_modulation.c src/wb_midifx.c src/wb_saturation.c src/wb_gate.c src/wb_multiband.c src/wb_captions.c src/wb_video.c src/wb_voice_polish.c src/wb_param_track.c src/wb_compositor.c src/wb_transcript.c src/wb_ofx.c src/wb_ofx_plugin_builtin.c src/wb_agent.c
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
build/wb_test_clap: tools/test_clap.c $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm -lobjc $(LIBS)

test-clap: build/test-clap/bigmac-test.clap build/wb_test_clap
	./build/wb_test_clap build/test-clap

# ---- tests (the gate) ----------------------------------------------------

test: build/wb_selftest
	./build/wb_selftest

build/wb_test_captions: build/tools/test_captions.o build/src/wb_captions.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_captions: build/wb_test_captions
	./build/wb_test_captions

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

build/wb_e2e_export: tools/test_export_e2e.c $(CORE_OBJS)
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

build/wb_test_compositor: build/tools/test_compositor.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_compositor: build/wb_test_compositor
	./build/wb_test_compositor

build/wb_test_voice_polish: build/tools/test_voice_polish.o $(CORE_OBJS)
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $^ -lm $(LIBS)

test_voice_polish: build/wb_test_voice_polish
	./build/wb_test_voice_polish

test_export_e2e: build/wb_e2e_export
	./build/wb_e2e_export
