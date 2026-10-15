# Host unit tests for the platform-independent AP core (components/esl_core).
#   make            build and run
#   make SAN=1      with AddressSanitizer + UndefinedBehaviorSanitizer
#   make openapi    check docs/openapi.yaml against the code (openapi/check_openapi.py)
#   make clean

CC      ?= cc
CORE    := ../components/esl_core
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -Werror -g -O1 -I$(CORE)/include -I../../shared
ifeq ($(SAN),1)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

SRCS := $(wildcard $(CORE)/*.c) $(wildcard *.c)
BIN  := build/esl_tests

.PHONY: all test openapi clean
all: test

$(BIN): $(SRCS) $(wildcard $(CORE)/include/*.h) ../../shared/proto.h tinytest.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@

test: $(BIN)
	./$(BIN)

build/api_host: $(wildcard $(CORE)/*.c) openapi/api_host.c $(wildcard $(CORE)/include/*.h) ../../shared/proto.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(wildcard $(CORE)/*.c) openapi/api_host.c $(LDFLAGS) -o $@

openapi:
	python3 openapi/check_openapi.py

clean:
	rm -rf build
