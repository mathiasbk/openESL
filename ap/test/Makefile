# Host unit tests for the platform-independent AP core (components/esl_core).
#   make            build and run, then read the drawn barcodes back (test_barcode_decode.py)
#   make SAN=1      with AddressSanitizer + UndefinedBehaviorSanitizer
#   make openapi    check docs/openapi.yaml against the code (openapi/check_openapi.py)
#   make clean

CC      ?= cc
CORE    := ../components/esl_core
LINK    := ../components/esl_link
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Werror -g -O1 -I$(CORE)/include -I$(LINK)/include -I../../shared -I../../tag/src
ifeq ($(SAN),1)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

TAG_SRCS := ../../tag/src/font.c ../../tag/src/barcode.c $(wildcard ../../tag/fonts/font_*.c)
SRCS := $(wildcard $(CORE)/*.c) $(wildcard $(LINK)/*.c) $(wildcard *.c) $(TAG_SRCS)
BIN  := build/esl_tests

.PHONY: all test openapi clean
all: test

$(BIN): $(SRCS) $(wildcard $(CORE)/include/*.h) $(wildcard $(LINK)/include/*.h) ../../shared/proto.h tinytest.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@

test: $(BIN)
	@mkdir -p build/barcodes
	ESL_TEST_OUT=build/barcodes ./$(BIN)
	ESL_TEST_OUT=build/barcodes python3 test_barcode_decode.py

build/api_host: $(wildcard $(CORE)/*.c) openapi/api_host.c $(wildcard $(CORE)/include/*.h) ../../shared/proto.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(wildcard $(CORE)/*.c) $(TAG_SRCS) openapi/api_host.c $(LDFLAGS) -o $@

openapi:
	python3 openapi/check_openapi.py

clean:
	rm -rf build
