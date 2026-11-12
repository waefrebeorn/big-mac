# big-mac -- the WuBuOS host audio output backend.
#
# One small library (bigmac_out) and its behavior gate. The backend order is
# pulse -> alsa -> wav; link only what you need:
#
#   make            # library + test
#   make test       # build and run the behavior gate
#   make static     # a static archive for embedding

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
LDLIBS  ?= -lpulse-simple -lpulse -lasound -lm

LIB     = libbigmac_out.a
STATIC  = libbigmac_out_static.a
TEST    = bigmac_out_test

all: $(LIB)

$(LIB): bigmac_out.o
	$(AR) rcs $@ $<

bigmac_out.o: bigmac_out.c bigmac_out.h
	$(CC) $(CFLAGS) -c $< -o $@

static: $(STATIC)
$(STATIC): bigmac_out.o
	$(AR) rcs $@ $<

$(TEST): bigmac_out.o bigmac_out_test.c
	$(CC) $(CFLAGS) bigmac_out.o bigmac_out_test.c -o $@ $(LDLIBS)

test: $(TEST)
	./$(TEST)

clean:
	rm -f *.o *.a $(TEST)

.PHONY: all static test clean
