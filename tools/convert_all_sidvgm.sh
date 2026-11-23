#!/bin/bash
# Convert every .sid under sid/ into sidvgm/, one file per subsong.
#
#   sid/x.sid (1 subsong)   -> sidvgm/x.sidvgm
#   sid/x.sid (N subsongs)  -> sidvgm/x_s01.sidvgm ... x_sNN.sidvgm
#   sid/rsid/y.sid          -> sidvgm/rsid/...
#
# Default duration is 180 s per subsong, so a track never stops before you have
# heard enough of it.
#
# Existing outputs are skipped unless --force, so an interrupted run resumes.
# The recorder is sidextract's default (libsidplayfp), the only one that keeps
# digi drums and the real RSID interrupt cadence.
#
#   tools/convert_all_sidvgm.sh [--seconds N] [--force] [src_dir] [dst_dir]
set -u
SECS=180
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
INFO=build/sidtest.exe

[ -x "$EXT" ] || { echo "missing $EXT (run: make tools)"; exit 1; }

total=0; made=0; skip=0; fail=0
mapfile -t files < <(find "$SRC" -name '*.sid' -type f | sort)
total=${#files[@]}
echo "sources $total, ${SECS}s per subsong -> $DST"

for f in "${files[@]}"; do
    rel="${f#$SRC/}"
    stem="${rel%.sid}"
    sub=$(dirname "$stem")
    base=$(basename "$stem")
    if [ "$sub" = "." ]; then outdir="$DST"; else outdir="$DST/$sub"; fi
    mkdir -p "$outdir"

    ns=$("$INFO" sidinfo "$f" 2>/dev/null | sed -n 's/^subsongs  *: *\([0-9]*\).*/\1/p')
    [ -z "$ns" ] && ns=1

    for ((s=0; s<ns; s++)); do
        # One subsong stays a plain file; several get their own folder so the
        # player can treat "," / "." as previous/next subsong.
        if [ "$ns" -eq 1 ]; then
            out="$outdir/$base.sidvgm"
        else
            outdir2="$outdir/$base"
            mkdir -p "$outdir2"
            out=$(printf "%s/%s_s%02d.sidvgm" "$outdir2" "$base" "$((s+1))")
        fi

        if [ "$FORCE" -eq 0 ] && [ -s "$out" ]; then
            skip=$((skip+1)); continue
        fi

        if "$EXT" "$f" "$out" --seconds "$SECS" --subsong "$s" \
                >/dev/null 2>"$out.err"; then
            rm -f "$out.err"
            made=$((made+1))
            printf '  %s\n' "${out#$DST/}"
        else
            msg=$(cat "$out.err" 2>/dev/null)
            rm -f "$out" "$out.err"
            case "$msg" in
                *"SID chips"*) echo "  skip 2SID: $rel" ;;
                *) echo "  FAILED: $rel s$s -- $msg"; fail=$((fail+1)) ;;
            esac
        fi
    done
done

echo "made $made, skipped $skip, failed $fail"
[ "$fail" -eq 0 ]
