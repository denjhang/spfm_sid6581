#!/bin/bash
# build_host.sh - build the host-side RPFM SID player (rpfm_sid.exe).
#
# The same recipe the project makefile uses for spfmplayer.exe: our C sources,
# the libsidplayfp adapter (C++), and the two prebuilt static libraries. Only
# the transport differs, so the object files that do not touch hardware are
# shared with the makefile build (build/*.o).
#
# Run from the repository root:
#   bash rp2350/scripts/build_host.sh
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

export PATH=/d/msys64/mingw64/bin:/d/msys64/usr/bin:/usr/bin:/bin
CC=gcc
CXX=g++

LS_DIR=third_party/local
LS_SRC=third_party/upstream/libsidplayfp-3.1.2/src
LS_LIBS="$LS_DIR/lib/libsidplayfp.a $LS_DIR/lib/libresidfp.a"
LS_CXXFLAGS="-std=gnu++17 -O2 -DNDEBUG -DHAVE_CXX17 -I$LS_DIR/include -Isrc \
  -I$LS_SRC -I$LS_SRC/sidtune -I$LS_SRC/c64 -I$LS_SRC/builders/residfp-builder"

mkdir -p build

# Objects shared with the makefile build. Rebuild only when missing.
[ -f build/sid_rsid.o ] || $CC -std=c11 -O2 -Isrc -c -o build/sid_rsid.o src/sid_rsid.c
[ -f build/sidvgm.o ]   || $CC -std=c11 -O2 -Isrc -c -o build/sidvgm.o src/sidvgm.c
[ -f build/sid_lsfp.o ] || $CXX $LS_CXXFLAGS -c -o build/sid_lsfp.o src/sid_lsfp.cpp

$CC -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
    -Isrc -Irp2350/tools \
    -c -o build/rpfm_link.o rp2350/tools/rpfm_link.c
$CC -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter \
    -Isrc -Irp2350/tools \
    -c -o build/rpfm_sid.o rp2350/tools/rpfm_sid.c

$CXX -o build/rpfm_sid.exe \
    build/rpfm_sid.o build/rpfm_link.o build/sid_rsid.o build/sidvgm.o \
    build/sid_lsfp.o $LS_LIBS \
    -static -lsetupapi -lhid -lwinmm

echo "built build/rpfm_sid.exe"
