# Host unit tests for the platform-independent AP core (components/esl_core).
#   make            build and run, then read the drawn barcodes back (test_barcode_decode.py),
#                   and the same_as tests (sameas/) with a registry of their own
#   make SAN=1      with AddressSanitizer + UndefinedBehaviorSanitizer
#   make openapi    check docs/openapi.yaml against the code (openapi/check_openapi.py)
#   make clean

CC      ?= cc
CORE    := ../components/esl_core
COPROC  := ../components/esl_coproc
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Werror -g -O1 -I$(CORE)/include -I$(COPROC)/include -I../../shared -I../../tag/src
ifeq ($(SAN),1)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

TAG_SRCS := ../../tag/src/font.c ../../tag/src/barcode.c $(wildcard ../../tag/fonts/font_*.c)
SRCS := $(wildcard $(CORE)/*.c) $(wildcard $(COPROC)/*.c) $(wildcard *.c) $(TAG_SRCS)
BIN  := build/esl_tests

.PHONY: all test sameas openapi clean
all: test

$(BIN): $(SRCS) $(wildcard $(CORE)/include/*.h) $(wildcard $(COPROC)/include/*.h) ../../shared/proto.h tinytest.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@

test: $(BIN) sameas
	@mkdir -p build/barcodes
	ESL_TEST_OUT=build/barcodes ./$(BIN)
	ESL_TEST_OUT=build/barcodes python3 test_barcode_decode.py

# One layout file for several models (#181): the core built with sameas/models_gen.h, which
# has models that share a file, in place of shared/models_gen.h (whose guard it takes)
SAMEAS := build/esl_tests_sameas
$(SAMEAS): $(wildcard $(CORE)/*.c) $(wildcard $(COPROC)/*.c) $(wildcard sameas/*) $(wildcard $(CORE)/include/*.h) ../../shared/proto.h tinytest.h
	@mkdir -p build
	$(CC) $(CFLAGS) -include sameas/models_gen.h $(wildcard $(CORE)/*.c) $(wildcard $(COPROC)/*.c) sameas/test_sameas.c $(TAG_SRCS) $(LDFLAGS) -o $@

sameas: $(SAMEAS)
	./$(SAMEAS)

build/api_host: $(wildcard $(CORE)/*.c) openapi/api_host.c $(wildcard $(CORE)/include/*.h) ../../shared/proto.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(wildcard $(CORE)/*.c) $(TAG_SRCS) openapi/api_host.c $(LDFLAGS) -o $@

openapi:
	python3 openapi/check_openapi.py

clean:
	rm -rf build
