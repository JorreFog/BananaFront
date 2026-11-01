# BananaFront
#   make                  the handheld build (BOARD=rgds-plus, or BOARD=rgds): build/<board>/bananafront
#   make host             a PC build with no panel and no sound (correctness tests only): build/host/bananafront
#   make test             PC unit tests
# Builds for the handhelds through ~/overnight-2026-11-01/buildslot.sh during shared runs; -j4 at most.
BOARD ?= rgds-plus
VERSION := $(shell git describe --always --dirty 2>/dev/null || echo dev)

SRC_COMMON := src/main.c src/core.c src/util.c src/resample.c src/statefile.c src/unzip.c src/memmap.c src/input.c \
	src/frontend.c src/third_party/bcomp/bcomp.c src/third_party/rcheevos/src/rhash/md5.c
SRC_DEVICE := $(SRC_COMMON) src/kms.c src/audio.c
SRC_HOST := $(SRC_COMMON) src/video_null.c src/audio_null.c
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation
DEFS := -DBF_VERSION=\"$(VERSION)\"

all: device

include $(HOME)/banana-emu/common/a55.mk

OUT := build/$(BOARD)
device: $(OUT)/bananafront
$(OUT)/bananafront: $(SRC_DEVICE) src/*.h Makefile
	@mkdir -p $(OUT)
	$(CC) $(A55_CFLAGS) $(A55_DRM_CFLAGS) -std=gnu11 $(WARN) $(DEFS) -o $@ $(SRC_DEVICE) $(A55_LDFLAGS) \
		-ldrm -lasound -lz -lm -ldl -lpthread
	@ls -la $@

HOSTCC ?= cc
host: build/host/bananafront
build/host/bananafront: $(SRC_HOST) src/*.h Makefile
	@mkdir -p build/host
	$(HOSTCC) -O1 -g -fsanitize=address,undefined -std=gnu11 $(WARN) $(DEFS) -o $@ $(SRC_HOST) -lz -lm -ldl

build/host/test_unit: tests/test_unit.c src/util.c src/resample.c src/statefile.c src/*.h
	@mkdir -p build/host
	$(HOSTCC) -O1 -g -fsanitize=address,undefined -std=gnu11 $(WARN) $(DEFS) -o $@ tests/test_unit.c src/util.c \
		src/resample.c src/statefile.c -lz -lm
test: build/host/test_unit
	./build/host/test_unit

# the shared companion code: the copies in src/third_party/bcomp follow ~/banana-emu/common/companion
sync-common:
	cp $(HOME)/banana-emu/common/companion/bcomp.[ch] $(HOME)/banana-emu/common/companion/bcomp_libretro.h src/third_party/bcomp/

clean:
	rm -rf build
.PHONY: all device host test clean sync-common
