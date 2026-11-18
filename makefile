# SPFM SID 6581 driver - console smoke test
#
# Toolchain: MinGW-w64 (MSYS2). On this machine use /d/msys64/mingw64/bin/gcc.
#   make CC=/d/msys64/mingw64/bin/gcc
# or add that directory to PATH.

CC      ?= gcc
BUILD   := build
FTDI    := third_party/ftd2xx

CFLAGS  := -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
           -Isrc -I$(FTDI)
LDFLAGS := -L$(FTDI) -lftd2xx -lws2_32 -static

SRCS := src/spfm_transport.c src/sid6581.c src/sid_freq_table.c src/sidtest.c
OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
TARGET := $(BUILD)/sidtest.exe

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	cp $(FTDI)/ftd2xx64.dll $(BUILD)/ 2>/dev/null || true

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

freq:
	python tools/gen_freq.py

selftest: $(TARGET)
	./$(TARGET) selftest

clean:
	rm -f $(BUILD)/*.o $(TARGET)

.PHONY: all clean freq selftest
