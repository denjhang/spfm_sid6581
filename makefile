# SPFM SID 6581 driver - console smoke test + .sid player
#
# Toolchain: MinGW-w64 (MSYS2). On this machine use /d/msys64/mingw64/bin/gcc.
#   make CC=/d/msys64/mingw64/bin/gcc
# or add that directory to PATH.

CC      ?= gcc
BUILD   := build
FTDI    := third_party/ftd2xx

CFLAGS  := -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
           -Isrc -I$(FTDI)
LDFLAGS := -L$(FTDI) -lftd2xx -lws2_32 -lwinmm -static

# The hardware player. Deliberately unchanged: no sidvgm object is linked into
# it, so rebuilding the offline tools never relinks or replaces this binary.
SRCS := src/spfm_transport.c src/sid6581.c src/sid_freq_table.c \
        src/sid_rsid.c src/sidplay.c src/sidtest.c
OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
TARGET := $(BUILD)/sidtest.exe

# Offline tools: no FTDI, no driver.
#   sidextract  .sid -> .sidreg / .sidvgm
#   sidvgmplay  .sidvgm / .sidreg -> WAV or waveOut, through a software model
EXTRACT := $(BUILD)/sidextract.exe
PLAYER  := $(BUILD)/sidvgmplay.exe

EXTRACT_OBJS := $(BUILD)/sidextract_cli.o $(BUILD)/sid_rsid.o $(BUILD)/sidvgm.o
PLAYER_OBJS  := $(BUILD)/sidvgmplay.o $(BUILD)/sidmodel.o \
                $(BUILD)/sidvgm.o $(BUILD)/sid_rsid.o

all: $(TARGET) tools

# `make tools` is the safe target while the player binary is running.
tools: $(EXTRACT) $(PLAYER)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	cp $(FTDI)/ftd2xx64.dll $(BUILD)/ 2>/dev/null || true

$(EXTRACT): $(EXTRACT_OBJS)
	$(CC) -o $@ $^ -static

$(PLAYER): $(PLAYER_OBJS)
	$(CC) -o $@ $^ -static -lwinmm

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

freq:
	python tools/gen_freq.py

selftest: $(TARGET)
	./$(TARGET) selftest

clean:
	rm -f $(BUILD)/*.o $(TARGET) $(EXTRACT) $(PLAYER)

.PHONY: all tools clean freq selftest
