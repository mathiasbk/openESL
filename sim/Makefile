# esl-sim: the tag firmware (tag/src) and the AP core (ap/components/esl_core) on the PC,
# joined by a simulated radio link (see README.md).
#   make            build both tag models and run the end-to-end scenarios
#                   (screens as PNG in build/<model>/screens/)
#   make SAN=1      the same with AddressSanitizer + UndefinedBehaviorSanitizer
#   make serial     esl_client.py against "esl-sim serve" over a pseudo-terminal
#   make clean

CC      ?= cc
ROOT    := $(abspath ..)
TAG     := $(ROOT)/tag
CORE    := $(ROOT)/ap/components/esl_core
MODELS  := $(shell python3 ../shared/models.py builds)   # shared/models.json
# The firmware version of each tag model (tools/fw_version.py tag-<build>), as text and as the
# number the tag sends in POLL, worked out once per model: TAG_VERSION_29, TAG_FW_VER_29, ...
$(foreach m,$(MODELS),$(eval TAG_VERSION_$(m) := $(shell python3 $(ROOT)/tools/fw_version.py tag-$(m))))
$(foreach m,$(MODELS),$(eval TAG_FW_VER_$(m) := $(shell python3 -c "v = '$(TAG_VERSION_$(m))'.split('.'); print(int(v[0]) * 10000 + int(v[1]) * 100 + int(v[2]))")))

WARN    := -Wall -Wextra -g -O1
ifeq ($(SAN),1)
WARN    += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif
# The tag code is built as tools/build.py builds the host tests
TAG_CFLAGS = -std=c99 $(WARN) -DMODEL=$(1) -DFW_VERSION=$(TAG_FW_VER_$(1)) -DFW_VERSION_STR='"$(TAG_VERSION_$(1))"' \
             -I$(TAG)/src -I$(TAG)/boot -I$(ROOT)/shared -I$(TAG)/test
AP_CFLAGS  := -std=c11 $(WARN) -Werror -Wpedantic -I$(CORE)/include -I$(ROOT)/shared -I$(TAG)/src \
              -DSIM_ROOT='"$(abspath $(ROOT))"'

TAG_SRCS := $(TAG)/src/app.c $(TAG)/src/proto.c $(TAG)/src/font.c $(TAG)/src/draw.c $(TAG)/src/layout.c $(TAG)/src/barcode.c \
            $(TAG)/src/ota.c $(TAG)/boot/boot_core.c $(wildcard $(TAG)/fonts/font_*.c) $(TAG)/test/fake_epd.c \
            $(TAG)/test/fake_xflash.c sim_tag.c
# esl_screen.c (the optional LCD on the C6) is left out: it shares the tag's font code
AP_SRCS  := $(filter-out $(CORE)/esl_screen.c,$(wildcard $(CORE)/*.c)) sim_ap.c e2e.c main.c png.c
DEPS     := $(wildcard *.h $(TAG)/src/*.h $(TAG)/boot/*.h $(TAG)/src/hal/*.h $(CORE)/include/*.h $(ROOT)/shared/*.h) \
            $(TAG_SRCS) $(AP_SRCS) $(ROOT)/VERSION

BINS   := $(MODELS:%=build/%/esl-sim)

.PHONY: all test serial clean
all: test

# Each side is compiled with its own flags, then linked into one program
build/%/esl-sim: $(DEPS)
	@mkdir -p build/$*/obj
	cd build/$*/obj && $(CC) $(call TAG_CFLAGS,$*) $(abspath $(TAG_SRCS)) -c
	cd build/$*/obj && $(CC) $(AP_CFLAGS) $(abspath $(AP_SRCS)) -c
	$(CC) build/$*/obj/*.o $(LDFLAGS) -o $@

test: $(BINS)
	@for m in $(MODELS); do mkdir -p build/$$m/screens && ./build/$$m/esl-sim test build/$$m/screens || exit 1; done

serial: build/154/esl-sim
	python3 test_serial.py

clean:
	rm -rf build
