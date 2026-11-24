#!/bin/sh
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

set -eu

PIRC="${1:?usage: run_m3_tests.sh <pirc-bin>}"

echo "[M3 gate] Checking aggregate layout & LIR lowering in tests/lowering_layout/*.pir..."
for f in tests/lowering_layout/*.pir; do
    echo "  LAYOUT $f"
    layout_out=$("$PIRC" --dump-layout "$f")
    # Verify every EXPECT_LAYOUT line matches computed layout
    sed -n 's|^// EXPECT_LAYOUT: ||p' "$f" | tr -d '\r' | while IFS= read -r want; do
        if ! printf '%s\n' "$layout_out" | grep -Fqx "$want"; then
            echo "FAIL: $f layout mismatch"
            echo "  expected line: $want"
            echo "  actual output:"
            printf '%s\n' "$layout_out"
            exit 1
        fi
    done

    # Emit LIR, verify emitted LIR round-trips cleanly through --verify-lir
    "$PIRC" --emit-lir "$f" > build/m3_lir.pir
    "$PIRC" --verify-lir --print build/m3_lir.pir > build/m3_lir2.pir
    diff -u build/m3_lir.pir build/m3_lir2.pir

    # Check EXPECT_GLOBAL_LIR directives if present
    sed -n 's|^// EXPECT_GLOBAL_LIR: ||p' "$f" | tr -d '\r' | while IFS= read -r gspec; do
        gname=$(printf '%s' "$gspec" | awk '{print $1}')
        gbytes=$(printf '%s' "$gspec" | sed -n 's/.*bytes=\([0-9]*\).*/\1/p')
        galign=$(printf '%s' "$gspec" | sed -n 's/.*align=\([0-9]*\).*/\1/p')
        want_line="$gname = global [$gbytes] = zeroinit, align $galign"
        if ! grep -Fq "$want_line" build/m3_lir.pir; then
            echo "FAIL: $f missing flattened global '$want_line'"
            cat build/m3_lir.pir
            exit 1
        fi
    done
done

echo "[M3 gate] Running HIR -> LIR -> interp differential tests across default/scalar/minimal/avx2..."
for f in tests/hir_exec/*.pir; do
    exp=$(sed -n 's|^// EXPECT_RET: ||p' "$f" | tr -d '\r' | head -n 1)
    if [ -z "$exp" ]; then exp=0; fi
    profiles="default scalar minimal"
    if grep -q "TEST_SIMD_CAPS:.*256" "$f"; then
        profiles="avx2 default scalar minimal"
    fi
    for prof in $profiles; do
        echo "  HIR ($prof) $f"
        # Verify emitted LIR passes standalone --verify-lir and round-trips
        "$PIRC" --emit-lir --caps="$prof" "$f" > build/m3_lir.pir
        "$PIRC" --verify-lir --caps="$prof" build/m3_lir.pir

        set +e
        "$PIRC" --interp --caps="$prof" "$f" > /dev/null 2>/tmp/pirc_m3_err.txt
        rc=$?
        set -e
        if [ "$rc" -ne "$exp" ]; then
            echo "FAIL: $f (--caps=$prof) exited with $rc (expected $exp)"
            cat /tmp/pirc_m3_err.txt
            exit 1
        fi
    done
done
rm -f build/m3_lir.pir build/m3_lir2.pir /tmp/pirc_m3_err.txt
echo "ALL M3 LOWERING TESTS PASSED"
