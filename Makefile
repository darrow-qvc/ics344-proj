# MiniSh build.  Default target is size-optimised for the Raspberry Pi.
#   make            -> build/minish (-Os, gc-sections, stripped copy in build/minish.stripped)
#   make debug      -> build/minish with -O0 -g -fsanitize=address,undefined
#   make test       -> run the regression suite
#   make bench      -> run the benchmark harness (needs bash; dash optional)
#   make export-<member> -> tarball with one member's portion (see scripts/)

CC      ?= gcc
BUILD   ?= build
TARGET  := $(BUILD)/minish

CSTD    := -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
WARN    := -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes \
           -Wmissing-prototypes -Wvla -Wformat=2
OPT     ?= -Os -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables \
           -fno-stack-protector -fomit-frame-pointer
LDOPT   ?= -Wl,--gc-sections -Wl,-O1 -Wl,--as-needed
CFLAGS  += $(CSTD) $(WARN) $(OPT) -Iinclude
LDFLAGS += $(LDOPT)

# Set STATIC=1 for a fully static binary (useful on a Pi with minimal rootfs).
ifeq ($(STATIC),1)
LDFLAGS += -static
endif

SRC := src/parser/lexer.c src/parser/parser.c src/parser/main.c \
       src/exec/exec.c    src/exec/builtins.c \
       src/io/redirect.c  src/io/pipeline.c \
       src/jobs/jobs.c    src/jobs/signals.c \
       src/common/util.c
OBJ := $(SRC:%.c=$(BUILD)/%.o)

.PHONY: all debug analyze clean test bench size export-karrar export-abdulwadood export-talal export-husain

all: $(TARGET) $(TARGET).stripped size

$(TARGET): $(OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LDFLAGS)

$(TARGET).stripped: $(TARGET)
	strip -s -o $@ $<

$(BUILD)/%.o: %.c include/minish.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# SAN=0 disables the sanitizers (needed where libasan/libubsan are not installed).
SAN ?= 1
ifeq ($(SAN),1)
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
endif
debug:
	$(MAKE) BUILD=build-debug OPT="-O0 -g3 $(SANFLAGS)" LDOPT="$(SANFLAGS)" build-debug/minish

# GCC static analyzer pass over every source file (no binary produced).
analyze:
	$(CC) $(CSTD) $(WARN) -O2 -fanalyzer -Iinclude -fsyntax-only $(SRC)

size: $(TARGET).stripped
	@size $(TARGET) | sed 's/^/[size] /'
	@printf '[size] stripped binary: %s bytes\n' "$$(stat -c %s $(TARGET).stripped)"

test: $(TARGET)
	tests/run_tests.sh $(TARGET)
	python3 tests/pty_tests.py $(TARGET)

bench: $(TARGET).stripped
	bench/run_bench.sh $(TARGET).stripped

clean:
	rm -rf $(BUILD) build-debug bench/results

export-karrar:      ; scripts/export-member.sh karrar
export-abdulwadood: ; scripts/export-member.sh abdulwadood
export-talal:       ; scripts/export-member.sh talal
export-husain:      ; scripts/export-member.sh husain
