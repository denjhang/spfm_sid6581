# SPFM SID 6581 driver - console smoke test + .sid player
#
# Toolchain: MinGW-w64 (MSYS2). On this machine use /d/msys64/mingw64/bin/gcc.
#   make CC=/d/msys64/mingw64/bin/gcc
# or add that directory to PATH.

CC      ?= gcc
CXX     ?= g++
BUILD   := build
FTDI    := third_party/ftd2xx

CFLAGS  := -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
           -Isrc -I$(FTDI)
LDFLAGS := -L$(FTDI) -lftd2xx -lws2_32 -lwinmm -static

# Vendored WebSid (third_party/websid, unmodified upstream wothke/websid).
# -DRPI4 turns on the recordPokeSID() capture hook in src/sid.cpp.
# -include compat.h only supplies MinGW's missing __uint32_t / stdint types.
WS_DIR      := third_party/websid
WS_CFLAGS   := -std=gnu11 -O2 -DRPI4 -include $(WS_DIR)/win/compat.h
WS_CXXFLAGS := -std=gnu++11 -O2 -DRPI4 -fno-rtti -include $(WS_DIR)/win/compat.h
WS_C_OBJS   := $(patsubst $(WS_DIR)/src/%.c,$(BUILD)/websid/%.o,$(wildcard $(WS_DIR)/src/*.c))
WS_CXX_OBJS := $(patsubst $(WS_DIR)/src/%.cpp,$(BUILD)/websid/%.o,$(wildcard $(WS_DIR)/src/*.cpp))
WS_OBJS     := $(WS_C_OBJS) $(WS_CXX_OBJS) $(BUILD)/websid/sid_websid.o

# libsidplayfp 3.1.2 + reSIDfp 1.3.0 (third_party/local, built from the release
# tarballs in third_party/upstream). This is the reference engine: its own
# 6502/VIC/CIA for recording, reSIDfp for audio.
#   - the public headers are in third_party/local/include
#   - the internal ones (sidemu.h and friends) only exist in the built source
#     tree; they are needed to subclass sidemu for capture
#   - HAVE_CXX11/14/17 are passed directly so sidcxx11.h does not pull in the
#     lib's config.h, which claims C++23 ([[likely]]) and would break -std=gnu++17.
LS_DIR      := third_party/local
LS_SRC      := third_party/upstream/libsidplayfp-3.1.2/src
LS_CXXFLAGS := -std=gnu++17 -O2 -DNDEBUG -DHAVE_CXX17 \
               -I$(LS_DIR)/include -Isrc \
               -I$(LS_SRC) -I$(LS_SRC)/sidtune -I$(LS_SRC)/c64 \
               -I$(LS_SRC)/builders/residfp-builder
LS_LIBS     := $(LS_DIR)/lib/libsidplayfp.a $(LS_DIR)/lib/libresidfp.a
LS_OBJS     := $(BUILD)/sid_lsfp.o

# Reference renderer: the untouched upstream API, for judging our own output.
# It links the same two static libraries as the tools.
LSFPREF     := $(BUILD)/lsfpref.exe

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
#   sidscan     classify tunes by driver (IRQ / CIA / VBI) and size them
SCAN    := $(BUILD)/sidscan.exe

EXTRACT_OBJS := $(BUILD)/sidextract_cli.o $(BUILD)/sid_rsid.o $(BUILD)/sidvgm.o
PLAYER_OBJS  := $(BUILD)/sidvgmplay.o $(BUILD)/sidmodel.o \
                $(BUILD)/sidvgm.o $(BUILD)/sid_rsid.o
ENGINE_OBJS  := $(WS_OBJS) $(LS_OBJS)

all: $(TARGET) tools

# `make tools` is the safe target while the player binary is running.
tools: $(EXTRACT) $(PLAYER)

# The reference renderer is built on demand (make ref), not by tools/all: it is
# a measuring instrument, not something a user needs. Same for sidscan.
ref: $(LSFPREF) $(SCAN)

$(LSFPREF): tools/lsfpref.cpp $(LS_LIBS) | $(BUILD)
	$(CXX) -std=gnu++17 -O2 -I$(LS_DIR)/include -o $@ $< $(LS_LIBS) -static

$(SCAN): tools/sidscan.cpp $(LS_OBJS) $(BUILD)/sid_rsid.o $(BUILD)/sidvgm.o $(LS_LIBS) | $(BUILD)
	$(CXX) $(LS_CXXFLAGS) -o $@ tools/sidscan.cpp $(LS_OBJS) \
	    $(BUILD)/sid_rsid.o $(BUILD)/sidvgm.o $(LS_LIBS) -static

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	cp $(FTDI)/ftd2xx64.dll $(BUILD)/ 2>/dev/null || true

# Both offline tools link the engines; the hardware player does not.
# The engines are C++ so the final link uses $(CXX).
$(EXTRACT): $(EXTRACT_OBJS) $(ENGINE_OBJS) $(LS_LIBS)
	$(CXX) -o $@ $(EXTRACT_OBJS) $(ENGINE_OBJS) $(LS_LIBS) -static

$(PLAYER): $(PLAYER_OBJS) $(ENGINE_OBJS) $(LS_LIBS)
	$(CXX) -o $@ $(PLAYER_OBJS) $(ENGINE_OBJS) $(LS_LIBS) -static -lwinmm

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# sid_websid.cpp is the adapter; the engine itself comes from $(WS_DIR)/src.
$(BUILD)/websid/%.o: $(WS_DIR)/src/%.c | $(BUILD)/websid
	$(CC) $(WS_CFLAGS) -c -o $@ $<

$(BUILD)/websid/%.o: $(WS_DIR)/src/%.cpp | $(BUILD)/websid
	$(CXX) $(WS_CXXFLAGS) -c -o $@ $<

$(BUILD)/sid_lsfp.o: src/sid_lsfp.cpp | $(BUILD)
	$(CXX) $(LS_CXXFLAGS) -c -o $@ $<

$(BUILD)/websid/sid_websid.o: src/sid_websid.cpp | $(BUILD)/websid
	$(CXX) $(WS_CXXFLAGS) -Isrc -I$(WS_DIR)/src -c -o $@ $<

$(BUILD)/websid:
	mkdir -p $(BUILD)/websid

$(BUILD):
	mkdir -p $(BUILD)

freq:
	python tools/gen_freq.py

selftest: $(TARGET)
	./$(TARGET) selftest

clean:
	rm -f $(BUILD)/*.o $(TARGET) $(EXTRACT) $(PLAYER) $(LSFPREF) $(SCAN)
	rm -rf $(BUILD)/websid

.PHONY: all tools ref clean freq selftest
