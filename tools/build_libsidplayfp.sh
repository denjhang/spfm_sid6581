#!/bin/bash
# Build libresidfp + libsidplayfp from the pristine release tarballs into
# third_party/local, then link the reference renderer.
#
# Must run under MSYS2's own bash: Git Bash's autoconf cannot see gcc
# ("no acceptable C compiler found"). Do not run it from PowerShell.
#
#   /d/msys64/usr/bin/bash.exe tools/build_libsidplayfp.sh
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="$ROOT/third_party/local"
UP="$ROOT/third_party/upstream"

export MSYSTEM=MINGW64
export PATH=/mingw64/bin:/usr/bin:/bin

build() {
    local dir="$1"
    shift
    echo "=== $dir"
    cd "$UP/$dir"
    ./configure --prefix="$PREFIX" --disable-shared --enable-static "$@" \
        > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    make -j4 > make.log 2>&1 || { tail -40 make.log; exit 1; }
    make install > install.log 2>&1 || { tail -20 install.log; exit 1; }
    # libtool prints "Could not determine host file name ... Continuing" here
    # even on success; the .a files are already correct.
}

build libresidfp-1.3.0
build libsidplayfp-3.1.2

echo "=== libraries"
ls -l "$PREFIX/lib"/lib{sidplayfp,residfp}.a
echo "DONE"
