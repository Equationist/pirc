#!/usr/bin/env bash
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BENCH_DIR="$ROOT_DIR/tests/benchmarks"
BUILD_DIR="$ROOT_DIR/build/m7_benchmarks"
PIRC="${PIRC:-$ROOT_DIR/build/pirc}"
CC="${CC:-clang}"
TARGET="${TARGET:-aarch64-apple-darwin}"
ITERS="${ITERS:-5}"
THRESHOLD_PCT="${THRESHOLD_PCT:-80}"

mkdir -p "$BUILD_DIR"

if [ ! -x "$PIRC" ]; then
    echo "Error: pirc binary not found at $PIRC" >&2
    exit 1
fi

BENCHES=(
    01_matmul
    02_quicksort
    03_wyhash_i128
    04_simd_clamp
    05_switch_vm
)

echo "=== M7 Benchmark Gate ($TARGET: pirc -O2 vs $CC -O2, min ${THRESHOLD_PCT}%) ==="
printf "%-18s %12s %12s %10s %s\n" "Benchmark" "clang -O2" "pirc -O2" "Ratio" "Status"
echo "---------------------------------------------------------------------"

failed=0
for b in "${BENCHES[@]}"; do
    bdir="$BENCH_DIR/$b"
    c_bin="$BUILD_DIR/${b}_c"
    p_asm="$BUILD_DIR/${b}_pir.s"
    p_bin="$BUILD_DIR/${b}_pir"

    "$CC" -std=c99 -O2 -Wall -Wextra -Werror "$BENCH_DIR/bench_harness.c" "$bdir/ref.c" -o "$c_bin"
    "$PIRC" --target "$TARGET" -O2 --emit-asm "$bdir/impl.pir" -o "$p_asm"
    "$CC" -std=c99 -O2 -Wall -Wextra -Werror "$BENCH_DIR/bench_harness.c" "$p_asm" -o "$p_bin"

    c_out="$("$c_bin" "$ITERS")"
    p_out="$("$p_bin" "$ITERS")"

    c_chk="$(echo "$c_out" | awk '{print $1}')"
    p_chk="$(echo "$p_out" | awk '{print $1}')"
    c_ms="$(echo "$c_out" | awk -F= '{print $3}')"
    p_ms="$(echo "$p_out" | awk -F= '{print $3}')"

    if [ "$c_chk" != "$p_chk" ]; then
        printf "%-18s %10s ms %10s ms %9s  FAIL (checksum %s != %s)\n" \
            "$b" "$c_ms" "$p_ms" "N/A" "$p_chk" "$c_chk"
        failed=1
        continue
    fi

    pct="$(awk -v c="$c_ms" -v p="$p_ms" 'BEGIN { if (p <= 0) print 100.0; else printf "%.1f", (c / p) * 100.0 }')"
    ok="$(awk -v pct="$pct" -v thr="$THRESHOLD_PCT" 'BEGIN { print (pct + 0.0 >= thr + 0.0) ? 1 : 0 }')"
    if [ "$ok" -eq 1 ]; then
        printf "%-18s %10s ms %10s ms %8s%%  PASS\n" "$b" "$c_ms" "$p_ms" "$pct"
    else
        printf "%-18s %10s ms %10s ms %8s%%  FAIL (<%s%%)\n" "$b" "$c_ms" "$p_ms" "$pct" "$THRESHOLD_PCT"
        failed=1
    fi
done

if [ "$failed" -ne 0 ]; then
    echo "M7 BENCHMARK GATE FAILED." >&2
    exit 1
fi
echo "ALL M7 BENCHMARKS PASSED CHECKSUM AND >=${THRESHOLD_PCT}% THROUGHPUT GATE."
