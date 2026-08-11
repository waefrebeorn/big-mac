# Big Mac — C11 voice engine
CC      = cc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Iinclude
LDLIBS  = -lm

SRC     = src/wb_tract.c src/wb_glottis.c src/wb_wav.c
OBJ     = $(SRC:.c=.o)

TOOLS   = tools/wb_speak

all: $(TOOLS)

tools/wb_speak: tools/wb_speak.c $(OBJ)
	$(CC) $(CFLAGS) -o $@ tools/wb_speak.c $(OBJ) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: tools/wb_speak
	./tools/wb_speak /tmp/bigmac_test.wav 0.5 140
	@echo "--- verifying WAV ---"
	@python3 -c "import wave; w=wave.open('/tmp/bigmac_test.wav'); print('wav ok:', w.getnchannels(), 'ch', w.getframerate(), 'Hz', w.getnframes(), 'frames')"

clean:
	rm -f $(OBJ) $(TOOLS) /tmp/bigmac_test.wav

.PHONY: all test clean
