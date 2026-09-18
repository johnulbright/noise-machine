CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter
NOISELIBS =

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  ALSA_OK := $(shell pkg-config --exists alsa 2>/dev/null && echo 1)
  ifeq ($(ALSA_OK),1)
    CFLAGS    += -DHAVE_ALSA
    NOISELIBS += $(shell pkg-config --libs alsa)
  else
    $(warning libasound2-dev not found: building without the ALSA backend)
  endif
endif
ifeq ($(UNAME_S),Darwin)
  # Play on macOS too, so the sound can be auditioned and the tone chosen
  # before any of this goes near the Pi. Same generator, different sink.
  CFLAGS    += -DHAVE_COREAUDIO
  NOISELIBS += -framework AudioToolbox -framework CoreFoundation
endif

all: bin/noise bin/analyze

bin/noise: src/noise.c | bin
	$(CC) $(CFLAGS) -o $@ $< $(NOISELIBS) -lm

bin/analyze: src/analyze.c | bin
	$(CC) $(CFLAGS) -o $@ $< -lm

bin:
	mkdir -p bin

# Render and measure on a development machine. No Pi, no ALSA.
check: bin/noise bin/analyze
	./bin/noise --f32 --seconds 600 --seed 1 --out $(or $(TMPDIR),/tmp)/noise-check.f32
	./bin/analyze $(or $(TMPDIR),/tmp)/noise-check.f32

clean:
	rm -rf bin

.PHONY: all check clean
