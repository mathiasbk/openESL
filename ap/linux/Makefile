# openesl-ap: the AP core (ap/components/esl_core) as a Linux program (docs/linux-ap.md).
#   make                  build/openesl-ap for this machine (cc)
#   make test             build it and run test.py against it (radio sim, uart on a pty), and
#                         test_spimodem (the SPI modem's framing)
#   make TARGET=armv7     static build with zig: armv7, aarch64 or x86_64 (pip install ziglang)
#   make SAN=1            with AddressSanitizer + UndefinedBehaviorSanitizer
#   make clean

ROOT    := $(abspath ../..)
CORE    := $(ROOT)/ap/components/esl_core
COPROC  := $(ROOT)/ap/components/esl_coproc
VERSION := $(shell cat $(ROOT)/VERSION)

ZIG_armv7   := arm-linux-musleabihf -mcpu=cortex_a9   # Cortex-A9 and newer
ZIG_aarch64 := aarch64-linux-musl
ZIG_x86_64  := x86_64-linux-musl
ifdef TARGET
ifeq ($(ZIG_$(TARGET)),)
$(error TARGET is armv7, aarch64 or x86_64)
endif
CC      := python3 -m ziglang cc -target $(ZIG_$(TARGET))
LDFLAGS += -static
OUT     := build/$(TARGET)
else
CC      ?= cc
OUT     := build/host$(if $(filter 1,$(SAN)),-san)
endif

WARN    := -Wall -Wextra -Werror -O2 -g
ifeq ($(SAN),1)
WARN    += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif
CFLAGS  := -std=c11 $(WARN) -I$(CORE)/include -I$(COPROC)/include -I$(ROOT)/shared -I$(ROOT)/tag/src -I$(ROOT)/ap/main \
           -DOPENESL_VERSION='"$(VERSION)"' -DESL_BOARD='"linux"' -DESL_BOARD_NAME='"Linux"'
LDLIBS  := -lpthread -lm

# esl_screen.c (the optional LCD on the C6) is left out: it shares the tag's font code
CORE_SRCS := $(filter-out $(CORE)/esl_screen.c,$(wildcard $(CORE)/*.c))
# the tag's fonts and barcodes, which esl_render draws with (as esl_core/CMakeLists.txt)
TAG_SRCS  := $(ROOT)/tag/src/font.c $(ROOT)/tag/src/barcode.c $(wildcard $(ROOT)/tag/fonts/font_*.c)
SRCS      := main.c store_files.c http.c radio_sim.c radio_coproc.c radio_spimodem.c
OBJS      := $(SRCS:%.c=$(OUT)/%.o) $(CORE_SRCS:$(CORE)/%.c=$(OUT)/core/%.o) $(OUT)/esl_coproc.o \
             $(TAG_SRCS:$(ROOT)/tag/%.c=$(OUT)/tag/%.o) $(OUT)/webui_files.o
DEPS      := ap.h Makefile $(wildcard $(CORE)/include/*.h $(ROOT)/shared/*.h) $(ROOT)/VERSION

.PHONY: all test clean
all: $(OUT)/openesl-ap

$(OUT)/openesl-ap: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@

$(OUT)/%.o: %.c $(DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/core/%.o: $(CORE)/%.c $(DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wpedantic -c $< -o $@

$(OUT)/esl_coproc.o: $(COPROC)/esl_coproc.c $(COPROC)/include/esl_coproc.h $(DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wpedantic -c $< -o $@

$(OUT)/tag/%.o: $(ROOT)/tag/%.c $(DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# The web page's tabs, packed from web/ as the ESP32 boards have them (ap/main/webui.py): the
# Dashboard, and the Design tab when its renderer is built (python3 web/build.py --wasm, with
# Emscripten, into WASM); without the renderer the Dashboard alone, as on a C6 Super Mini.
# No link to /setup: WiFi and the token are the host's here.
WASM    ?= $(ROOT)/web/dist/wasm
UI      := dashboard $(if $(wildcard $(WASM)/render-*.wasm),design)
build/webui_files.c: $(ROOT)/ap/main/webui.py $(wildcard $(ROOT)/web/index.html $(ROOT)/web/css/*.css $(ROOT)/web/js/*.js $(ROOT)/templates/*/*.json $(WASM)/render-*)
	@mkdir -p build
	$(if $(filter design,$(UI)),,@echo "openesl-ap: no renderer in $(WASM) (python3 web/build.py --wasm): the Dashboard tab only")
	python3 $(ROOT)/ap/main/webui.py $(ROOT)/web $@ --no-setup --wasm $(WASM) $(UI)

$(OUT)/webui_files.o: build/webui_files.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/test_spimodem: test_spimodem.c radio_spimodem.c $(DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

test: $(OUT)/openesl-ap $(OUT)/test_spimodem
	$(OUT)/test_spimodem
	python3 test.py $(OUT)/openesl-ap

clean:
	rm -rf build
