# Big Mac — C11 voice engine
CC      = cc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Iinclude
LDLIBS  = -lm

SRC     = src/wb_tract.c src/wb_glottis.c src/wb_wav.c src/wb_dsp.c src/wb_reader.c src/wb_measure.c src/wb_print.c src/wb_aiff.c src/wb_midi.c src/wb_retrieve.c src/wb_learn.c src/wb_resample.c \
          src/wuburvc/wubu_master.c src/wuburvc/wubu_consonant.c src/wuburvc/wubu_breath.c src/wuburvc/wubu_harmony.c src/wuburvc/wubu_fft.c src/wuburvc/wubu_stft.c
OBJ     = $(SRC:.c=.o)

TOOLS   = tools/wb_speak tools/wb_analyze tools/wb_fit tools/wb_absorb tools/wb_toon tools/wb_sing tools/wb_master tools/wb_vc

all: $(TOOLS)

tools/wb_speak: tools/wb_speak.c $(OBJ)
	$(CC) $(CFLAGS) -o $@ tools/wb_speak.c $(OBJ) $(LDLIBS)

tools/wb_analyze: tools/wb_analyze.c $(OBJ)
	$(CC) $(CFLAGS) -o $@ tools/wb_analyze.c $(OBJ) $(LDLIBS)

tools/wb_fit: tools/wb_fit.c $(OBJ)
	$(CC) $(CFLAGS) -o $@ tools/wb_fit.c $(OBJ) $(LDLIBS)

tools/wb_absorb: tools/wb_absorb.c $(OBJ)
	$(CC) $(CFLAGS) -o $@ tools/wb_absorb.c $(OBJ) $(LDLIBS)

tools/wb_toon: tools/wb_toon.c $(OBJ)
	$(CC) $(CFLAGS) -o $@ tools/wb_toon.c $(OBJ) $(LDLIBS)

tools/wb_sing: tools/wb_sing.c $(OBJ)
	$(CC) $(CFLAGS) -o $@ tools/wb_sing.c $(OBJ) $(LDLIBS)

tools/wb_master: tools/wb_master.c $(OBJ)
	$(CC) $(CFLAGS) -o $@ tools/wb_master.c $(OBJ) $(LDLIBS)

tools/wb_vc: tools/wb_vc.c $(OBJ)
	$(CC) $(CFLAGS) -o $@ tools/wb_vc.c $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: tools/wb_speak tools/wb_analyze
	./tools/wb_speak /tmp/bigmac_test.wav 0.5 140
	@echo "--- verifying WAV ---"
	@python3 -c "import wave; w=wave.open('/tmp/bigmac_test.wav'); print('wav ok:', w.getnchannels(), 'ch', w.getframerate(), 'Hz', w.getnframes(), 'frames')"
	@echo "--- absorb loop: analyze the render ---"
	./tools/wb_analyze /tmp/bigmac_test.wav

test-yin: tools/wb_toon tools/wb_analyze
	@echo "--- YIN regression: falsetto + baritone characters ---"
	@./tools/wb_toon mickey /tmp/t_mickey.wav >/dev/null 2>&1
	@./tools/wb_toon homer /tmp/t_homer.wav >/dev/null 2>&1
	@./tools/wb_toon betty /tmp/t_betty.wav >/dev/null 2>&1
	@f0=$$(./tools/wb_analyze /tmp/t_mickey.wav 2>/dev/null | grep "F0 (YIN)" | awk '{print $$3}'); \
	 echo "mickey (preset 320): measured $$f0"; \
	 awk -v f="$$f0" 'BEGIN{ok=(f>310 && f<330); print (ok?"PASS":"FAIL"), "mickey F0", f; exit !ok}'
	@f0=$$(./tools/wb_analyze /tmp/t_homer.wav 2>/dev/null | grep "F0 (YIN)" | awk '{print $$3}'); \
	 echo "homer (preset 105): measured $$f0"; \
	 awk -v f="$$f0" 'BEGIN{ok=(f>95 && f<115); print (ok?"PASS":"FAIL"), "homer F0", f; exit !ok}'
	@f0=$$(./tools/wb_analyze /tmp/t_betty.wav 2>/dev/null | grep "F0 (YIN)" | awk '{print $$3}'); \
	 echo "betty (preset 380): measured $$f0"; \
	 awk -v f="$$f0" 'BEGIN{ok=(f>370 && f<390); print (ok?"PASS":"FAIL"), "betty F0", f; exit !ok}'

clean:
	rm -f $(OBJ) $(TOOLS) /tmp/bigmac_test.wav /tmp/t_*.wav

.PHONY: all test test-yin clean
