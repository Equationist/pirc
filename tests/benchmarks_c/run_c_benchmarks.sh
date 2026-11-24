#!/usr/bin/env bash
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BDIR="$ROOT/tests/benchmarks_c"
PIRC="${PIRC:-$ROOT/build/pirc}"
SLIMCC="${SLIMCC:-$ROOT/frontends/slimcc-pir/slimcc-pir}"
CLANG="${CLANG:-clang}"
GCC="${GCC:-/opt/homebrew/Cellar/gcc/16.2.0/bin/gcc-16}"
ITERS="${ITERS:-15}"

TMPB="$(mktemp -d "${TMPDIR:-/tmp}/pirc_c_bench.XXXXXX")"
trap 'rm -rf "$TMPB"' EXIT

echo "=== General C99 Benchmark Suite (aarch64-apple-darwin, ${ITERS} iters median) ==="
echo "  Clang:      $("$CLANG" --version | head -n 1)"
echo "  GCC:        $("$GCC" --version | head -n 1) ($("$GCC" -dumpmachine))"
echo "  slimcc+pir: $("$PIRC" --version)"
echo ""
printf "%-18s | %10s | %10s | %15s | %10s | %10s | %s\n" \
    "Benchmark" "clang -O2" "gcc-16 -O2" "slimcc+pirc -O2" "vs clang" "vs gcc-16" "Checksum"
echo "------------------------------------------------------------------------------------------------------------"

sum_log_cl=0
sum_log_gc=0
cnt=0

for f in "$BDIR"/0*.c; do
    base="$(basename "$f" .c)"
    "$CLANG" -std=c99 -O2 -I"$BDIR" "$f" -lm -o "$TMPB/${base}_clang"
    "$GCC"   -std=c99 -O2 -I"$BDIR" "$f" -lm -o "$TMPB/${base}_gcc"
    "$SLIMCC" --pirc="$PIRC" -O2 -I"$BDIR" "$f" -lm -o "$TMPB/${base}_pir"

    c_out="$("$TMPB/${base}_clang" "$ITERS")"
    g_out="$("$TMPB/${base}_gcc"   "$ITERS")"
    p_out="$("$TMPB/${base}_pir"   "$ITERS")"

    c_chk="$(echo "$c_out" | awk '{print $1}')"
    g_chk="$(echo "$g_out" | awk '{print $1}')"
    p_chk="$(echo "$p_out" | awk '{print $1}')"
    c_ms="$(echo "$c_out" | awk -F= '{print $3}')"
    g_ms="$(echo "$g_out" | awk -F= '{print $3}')"
    p_ms="$(echo "$p_out" | awk -F= '{print $3}')"

    r_c="$(awk -v c="$c_ms" -v p="$p_ms" 'BEGIN { printf "%.1f%%", (c / p) * 100.0 }')"
    r_g="$(awk -v g="$g_ms" -v p="$p_ms" 'BEGIN { printf "%.1f%%", (g / p) * 100.0 }')"
    status="OK ($p_chk)"
    if [ "$c_chk" != "$p_chk" ] || [ "$g_chk" != "$p_chk" ]; then
        status="MISMATCH (cl=$c_chk gc=$g_chk pir=$p_chk)"
    fi
    printf "%-18s | %7s ms | %7s ms | %12s ms | %10s | %10s | %s\n" \
        "$base" "$c_ms" "$g_ms" "$p_ms" "$r_c" "$r_g" "$status"

    sum_log_cl="$(awk -v s="$sum_log_cl" -v c="$c_ms" -v p="$p_ms" 'BEGIN { printf "%.12f", s + log(c / p) }')"
    sum_log_gc="$(awk -v s="$sum_log_gc" -v g="$g_ms" -v p="$p_ms" 'BEGIN { printf "%.12f", s + log(g / p) }')"
    cnt=$((cnt + 1))
done

echo "------------------------------------------------------------------------------------------------------------"
gm_c="$(awk -v s="$sum_log_cl" -v n="$cnt" 'BEGIN { printf "%.1f%%", exp(s / n) * 100.0 }')"
gm_g="$(awk -v s="$sum_log_gc" -v n="$cnt" 'BEGIN { printf "%.1f%%", exp(s / n) * 100.0 }')"
printf "%-18s | %10s | %10s | %15s | %10s | %10s |\n" \
    "GEOMETRIC MEAN" "" "" "" "$gm_c" "$gm_g"
