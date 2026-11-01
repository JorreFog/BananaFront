# BananaFront
#   make                  the handheld build (BOARD=rgds-plus, or BOARD=rgds): build/<board>/bananafront
#   make host             a PC build with no panel and no sound (correctness tests only): build/host/bananafront
#   make test             PC unit tests
# Builds for the handhelds through ~/overnight-2026-11-01/buildslot.sh during shared runs; -j4 at most.
BOARD ?= rgds-plus
VERSION := $(shell git describe --always --dirty 2>/dev/null || echo dev)

SRC_COMMON := src/main.c src/core.c src/util.c src/resample.c src/statefile.c src/unzip.c src/memmap.c src/input.c \
	src/frontend.c src/cpugov.c src/gpufloor.c src/ra.c src/menu.c src/menu_host.c src/lang.c src/third_party/bcomp/bcomp.c
SRC_DEVICE := $(SRC_COMMON) src/kms.c src/shader.c src/audio.c
SRC_HOST := $(SRC_COMMON) src/video_null.c src/audio_null.c
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation
MENU_WARN := -Wno-misleading-indentation -Wno-unused-function -Wno-unused-variable -Wno-unused-const-variable
DEFS := -DBF_VERSION=\"$(VERSION)\" -DRC_CLIENT_SUPPORTS_HASH -DRC_DISABLE_LUA
RC := src/third_party/rcheevos
RC_SRC := $(RC)/src/rc_client.c $(RC)/src/rc_compat.c $(RC)/src/rc_util.c $(RC)/src/rc_version.c \
	$(RC)/src/rc_libretro.c $(wildcard $(RC)/src/rcheevos/*.c) $(wildcard $(RC)/src/rapi/*.c) \
	$(wildcard $(RC)/src/rhash/*.c)
RC_FLAGS := -Isrc -I$(RC)/include -DRC_DISABLE_LUA -DRC_CLIENT_SUPPORTS_HASH -w

all: device

-include $(HOME)/banana-emu/common/a55.mk

OUT := build/$(BOARD)
device: $(OUT)/bananafront
# rcheevos once per board (its own flags, warnings off: upstream code)
$(OUT)/rc/%.o: $(RC)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(A55_CFLAGS) $(RC_FLAGS) -c -o $@ $<
$(OUT)/librc.a: $(patsubst $(RC)/src/%.c,$(OUT)/rc/%.o,$(RC_SRC))
	$(AR) rcs $@ $^
$(OUT)/bananafront: $(SRC_DEVICE) src/*.h Makefile $(OUT)/librc.a
	@mkdir -p $(OUT)
	$(CC) $(A55_CFLAGS) $(A55_DRM_CFLAGS) -Isrc -I$(RC)/include -std=gnu11 $(WARN) $(MENU_WARN) $(DEFS) -o $@.new $(SRC_DEVICE) $(OUT)/librc.a \
		$(A55_LDFLAGS) -ldrm -lasound -lz -lm -ldl -lpthread
	@mv -f $@.new $@  # (whole: a test copying it meanwhile never gets half a program)
	@ls -la $@

HOSTCC ?= cc
host: build/host/bananafront
build/host/rc/%.o: $(RC)/src/%.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O1 -g $(RC_FLAGS) -c -o $@ $<
build/host/librc.a: $(patsubst $(RC)/src/%.c,build/host/rc/%.o,$(RC_SRC))
	ar rcs $@ $^
build/host/bananafront: $(SRC_HOST) src/*.h Makefile build/host/librc.a
	@mkdir -p build/host
	$(HOSTCC) -O1 -g -fsanitize=address,undefined -Isrc -I$(RC)/include -std=gnu11 $(WARN) $(MENU_WARN) $(DEFS) -o $@ $(SRC_HOST) \
		build/host/librc.a -lz -lm -ldl -lpthread

build/host/test_unit: tests/test_unit.c tests/stub_core.c src/util.c src/resample.c src/statefile.c src/memmap.c src/*.h
	@mkdir -p build/host
	$(HOSTCC) -O1 -g -fsanitize=address,undefined -std=gnu11 $(WARN) $(MENU_WARN) $(DEFS) -Isrc -o $@ tests/test_unit.c \
		tests/stub_core.c src/util.c src/resample.c src/statefile.c src/memmap.c -lz -lm
build/host/testcore.so: tests/testcore.c src/libretro.h
	@mkdir -p build/host
	$(HOSTCC) -O1 -g -shared -fPIC -o $@ tests/testcore.c
build/host/test_gpufloor: tests/test_gpufloor.c src/gpufloor.c src/gpufloor.h
	@mkdir -p $(@D)
	$(HOSTCC) -O1 -g -fsanitize=address,undefined -std=gnu11 -Isrc -o $@ tests/test_gpufloor.c src/gpufloor.c

build/host/test_cpugov: tests/test_cpugov.c src/cpugov.c src/cpugov.h
	@mkdir -p build/host
	$(HOSTCC) -O1 -g -fsanitize=address,undefined -std=gnu11 -Isrc -o $@ tests/test_cpugov.c src/cpugov.c
test: build/host/test_unit build/host/test_cpugov build/host/test_gpufloor build/host/bananafront build/host/testcore.so
	./build/host/test_unit
	./build/host/test_cpugov
	./build/host/test_gpufloor
	tests/core-test.sh

# the shared companion code: the copies in src/third_party/bcomp follow ~/banana-emu/common/companion
sync-common:
	cp $(HOME)/banana-emu/common/companion/bcomp.[ch] $(HOME)/banana-emu/common/companion/bcomp_libretro.h src/third_party/bcomp/

clean:
	rm -rf build
.PHONY: all device host test clean sync-common
