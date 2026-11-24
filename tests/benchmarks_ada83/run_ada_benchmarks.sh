#!/usr/bin/env bash
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BDIR="$ROOT/tests/benchmarks_ada83"
ADA_CC="$ROOT/frontends/ada83-pir/ada83-pir"
PIRC="${PIRC:-$ROOT/build/pirc}"
SDK15="/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/ada83_bench.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

echo "=== Ada 83 Benchmark Suite (aarch64-apple-darwin, best-of-N ms via Calendar.Clock) ==="
echo "  GNAT:       $(gnatmake --version | head -n 1)"
echo "  ada83+pirc: $("$PIRC" --version)"
echo ""

run_mode() {
    local title="$1"
    local gnat_flags="$2"
    local pir_flags="$3"

    echo "--- $title ---"
    printf "%-18s | %12s | %15s | %10s | %s\n" \
        "Benchmark" "GNAT -O2" "ada83+pirc -O2" "vs GNAT" "Checksum"
    echo "---------------------------------------------------------------------------------"

    local sum_log=0
    local cnt=0

    for f in "$BDIR"/bench*.adb; do
        local base="$(basename "$f" .adb)"
        local gdir="$TMP/g_${base}_${cnt}"
        mkdir -p "$gdir"
        cp "$f" "$gdir/$base.adb"
        (cd "$gdir" && gnatmake -q -gnat83 -O2 $gnat_flags "$base.adb" -o "$gdir/bin" -largs -B/usr/bin -Wl,-syslibroot,"$SDK15")
        "$ADA_CC" --pirc="$PIRC" -O2 $pir_flags "$f" -o "$TMP/${base}_pir"

        local g_out="$("$gdir/bin")"
        local p_out="$("$TMP/${base}_pir")"
        local g_chk="$(echo "$g_out" | awk '{print $1}')"
        local p_chk="$(echo "$p_out" | awk '{print $1}')"
        local g_ms="$(echo "$g_out" | awk -F= '{print $2}')"
        local p_ms="$(echo "$p_out" | awk -F= '{print $2}')"

        local ratio="$(awk -v g="$g_ms" -v p="$p_ms" 'BEGIN { printf "%.1f%%", (g / p) * 100.0 }')"
        local st="OK ($p_chk)"
        if [ "$g_chk" != "$p_chk" ]; then
            st="MISMATCH (gnat=$g_chk pir=$p_chk)"
        fi
        printf "%-18s | %9s ms | %12s ms | %10s | %s\n" \
            "$base" "$g_ms" "$p_ms" "$ratio" "$st"
        sum_log="$(awk -v s="$sum_log" -v g="$g_ms" -v p="$p_ms" 'BEGIN { printf "%.12f", s + log(g / p) }')"
        cnt=$((cnt + 1))
    done

    echo "---------------------------------------------------------------------------------"
    local gm="$(awk -v s="$sum_log" -v n="$cnt" 'BEGIN { printf "%.1f%%", exp(s / n) * 100.0 }')"
    printf "%-18s | %12s | %15s | %10s |\n\n" "GEOMETRIC MEAN" "" "" "$gm"
}

run_mode "Mode A: Full Ada Runtime Checks (GNAT -O2 -gnato vs ada83-pir -O2 --checks)" "-gnato" "--checks"
run_mode "Mode B: Unchecked Performance Mode (GNAT -O2 -gnatp vs ada83-pir -O2 --no-checks)" "-gnatp" "--no-checks"
