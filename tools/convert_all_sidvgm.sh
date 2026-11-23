#!/bin/bash
# Convert every .sid under sid/ into sidvgm/, mirroring the tree:
#
#   sid/x.sid        -> sidvgm/x.sidvgm
#   sid/rsid/y.sid   -> sidvgm/rsid/y.sidvgm
#
# Existing .sidvgm files are skipped unless --force is given, so an interrupted
# run can be resumed. The recorder is sidextract's default (libsidplayfp), which
# is the only one that keeps digi-drums and real RSID cadence.
#
#   tools/convert_all_sidvgm.sh [--seconds N] [--force] [src_dir] [dst_dir]
set -u
SECS=60
FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --seconds) SECS="$2"; shift 2 ;;
        --force)   FORCE=1; shift ;;
        *) break ;;
    esac
done
SRC="${1:-sid}"
DST="${2:-sidvgm}"
EXT=build/sidextract.exe

[ -x "$EXT" ] || { echo "missing $EXT (run: make tools)"; exit 1; }

total=0; done_n=0; skip=0; fail=0
mapfile -t files < <(find "$SRC" -name '*.sid' -type f | sort)
total=${#files[@]}

for f in "${files[@]}"; do
    rel="${f#$SRC/}"
    out="$DST/${rel%.sid}.sidvgm"
    mkdir -p "$(dirname "$out")"
    if [ "$FORCE" -eq 0 ] && [ -s "$out" ]; then
        skip=$((skip+1)); continue
    fi
    printf '[%d/%d] %s\n' "$((done_n+skip+1))" "$total" "$rel"
    if "$EXT" "$f" "$out" --seconds "$SECS" >/dev/null 2>"$out.err"; then
        rm -f "$out.err"
        done_n=$((done_n+1))
    else
        echo "  FAILED: $(cat "$out.err")"
        rm -f "$out" "$out.err"
        fail=$((fail+1))
    fi
done

echo "converted $done_n, skipped $skip, failed $fail, total $total"
# 2SID/3SID tunes are expected failures: the container holds one socket.
[ "$fail" -eq 0 ]
