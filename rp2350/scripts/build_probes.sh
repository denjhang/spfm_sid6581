#!/bin/bash
# build_probes.sh - build the diagnostic probes in rp2350/tools. None of them
# make sound; each one measures a single number (see the table in README.md).
#
# Run from the repository root:
#   bash rp2350/scripts/build_probes.sh
#
# These reuse the objects the host build already produces, so run
# rp2350/scripts/build_host.sh first.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

export PATH=/d/msys64/mingw64/bin:/d/msys64/usr/bin:/usr/bin:/bin
CC=gcc
CXX=g++
CFLAGS="-std=c11 -O2 -Isrc -Irp2350/tools"
LS_DIR=third_party/local
LS_LIBS="$LS_DIR/lib/libsidplayfp.a $LS_DIR/lib/libresidfp.a"

mkdir -p build

# Hardware-only probes: just the HID link.
for t in hidbench wbench qprobe gapsprobe; do
    $CC $CFLAGS -o "build/$t.exe" "rp2350/tools/$t.c" rp2350/tools/rpfm_link.c \
        -lsetupapi -lhid
    echo "built build/$t.exe"
done

# Probes that need to read a .sidvgm.
for t in drainprobe pairprobe; do
    $CC $CFLAGS -o "build/$t.exe" "rp2350/tools/$t.c" rp2350/tools/rpfm_link.c \
        build/sidvgm.o build/sid_rsid.o -lsetupapi -lhid
    echo "built build/$t.exe"
done

# Probes that need libsidplayfp.
$CC $CFLAGS -c -o build/liveprobe.o rp2350/tools/liveprobe.c
$CXX -o build/liveprobe.exe build/liveprobe.o build/rpfm_link.o build/sid_rsid.o \
    build/sid_lsfp.o $LS_LIBS -static -lwinmm -lsetupapi -lhid
echo "built build/liveprobe.exe"

$CC $CFLAGS -c -o build/lsfpdump.o rp2350/tools/lsfpdump.c
$CXX -o build/lsfpdump.exe build/lsfpdump.o build/sid_rsid.o build/sid_lsfp.o \
    $LS_LIBS -static -lwinmm
echo "built build/lsfpdump.exe"

# Ground truth: the spacing libsidplayfp produces before any container.
$CC $CFLAGS -c -o build/realprobe.o rp2350/tools/realprobe.c
$CXX -o build/realprobe.exe build/realprobe.o build/sid_rsid.o build/sid_lsfp.o \
    $LS_LIBS -static -lwinmm
echo "built build/realprobe.exe"

# Timeline probes: what t=0 is, and whether the board's deadline arithmetic
# matches the recorder when the queue never runs dry.
for t in originprobe firstev; do
    $CC $CFLAGS -c -o "build/$t.o" "rp2350/tools/$t.c"
    $CXX -o "build/$t.exe" "build/$t.o" build/sid_rsid.o build/sid_lsfp.o \
        $LS_LIBS -static -lwinmm
    echo "built build/$t.exe"
done

$CC $CFLAGS -c -o build/deadsim.o rp2350/tools/deadsim.c
$CXX -o build/deadsim.exe build/deadsim.o build/sid_rsid.o build/sid_lsfp.o \
    $LS_LIBS -static -lwinmm
echo "built build/deadsim.exe"

# Render OUR event list through reSIDfp, so a waveform diff against
# build/lsfpref.exe measures timeline error and nothing else.
$CC $CFLAGS -c -o build/replaywav.o rp2350/tools/replaywav.c
$CXX -o build/replaywav.exe build/replaywav.o build/sid_rsid.o build/sid_lsfp.o \
    $LS_LIBS -static -lwinmm
echo "built build/replaywav.exe"

# live stream vs a recorded .sidvgm, event by event.
$CC $CFLAGS -c -o build/cmpfirst.o rp2350/tools/cmpfirst.c
$CXX -o build/cmpfirst.exe build/cmpfirst.o build/sid_rsid.o build/sid_lsfp.o \
    build/sidvgm.o $LS_LIBS -static -lwinmm
echo "built build/cmpfirst.exe"
