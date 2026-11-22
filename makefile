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

SRCS := src/spfm_transport.c src/sid6581.c src/sid_freq_table.c \
        src/sid_rsid.c src/sidmeta.c src/sidvgm.c src/sidplay.c src/sidtest.c
OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
TARGET := $(BUILD)/sidtest.exe

# The emulator alone, without the driver or FTDI: turns a .sid into a .sidreg
# or .sidvgm on a machine with no hardware attached. Replaces the STC32G
# tools/sid_extract.
EXTRACT := $(BUILD)/sidextract.exe

all: $(TARGET) $(EXTRACT)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	cp $(FTDI)/ftd2xx64.dll $(BUILD)/ 2>/dev/null || true

$(EXTRACT): src/sidextract_cli.c $(BUILD)/sid_rsid.o $(BUILD)/sidmeta.o \
            $(BUILD)/sidvgm.o
	$(CC) -o $@ $^ -static

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

freq:
	python tools/gen_freq.py

selftest: $(TARGET)
	./$(TARGET) selftest

clean:
	rm -f $(BUILD)/*.o $(TARGET) $(EXTRACT)

.PHONY: all clean freq selftest
