CC      ?= cc
# gnu11 rather than c11: alsa-lib's snd_pcm_hw_params_alloca() macros expand to
# alloca(), whose declaration is only reliably visible with GNU extensions
# enabled. The source already defines _GNU_SOURCE, so this just makes the
# language mode agree with that instead of gambling on a transitive include.
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter
NOISELIBS =

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  ALSA_OK := $(shell pkg-config --exists alsa 2>/dev/null && echo 1)
  ifeq ($(ALSA_OK),1)
    CFLAGS    += -DHAVE_ALSA
    NOISELIBS += $(shell pkg-config --libs alsa)
  else
    # A hard error, not a warning. Without the backend this still builds, still
    # renders files, still passes every test in the suite (they only exercise
    # the render path) -- and never plays a sound. That is precisely the kind of
    # silent success this project exists to stop shipping.
    ifeq ($(filter clean,$(MAKECMDGOALS)),)
      $(error libasound2-dev not found: pkg-config cannot see module 'alsa'. Run: sudo apt-get install -y libasound2-dev pkg-config)
    endif
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

# Regression suite: asserts the properties the appliance depends on. Needs only
# a C compiler -- no Pi, no ALSA, no sox, no Python.
test: bin/noise bin/analyze
	@bash test/run-tests.sh

# Render and measure by eye.
check: bin/noise bin/analyze
	./bin/noise --f32 --seconds 600 --seed 1 --out - | ./bin/analyze - --band 200 8000

# Prove no drift over a full night. ~90 s of CPU, streamed, no disk.
soak: bin/noise bin/analyze
	./bin/noise --f32 --seconds 28800 --seed 11 --out - | ./bin/analyze - --band 200 8000

clean:
	rm -rf bin

.PHONY: all test check soak clean
