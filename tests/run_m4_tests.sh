#!/bin/sh
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

set -eu

PIRC="${1:?usage: run_m4_tests.sh <pirc-bin>}"

check_lir_directives() {
    f="$1"
    lir="$2"
    # Check EXPECT_LIR_CONTAINS
    sed -n 's|^// EXPECT_LIR_CONTAINS: ||p' "$f" | tr -d '\r' | while IFS= read -r pat; do
        if ! grep -Fq "$pat" "$lir"; then
            echo "FAIL: $f missing '$pat' in lowered LIR"
            cat "$lir"
            exit 1
        fi
    done
    # Check EXPECT_LIR_FORBIDS (use word boundaries for bareword identifiers so @c_i128_add doesn't false-match 'i128')
    sed -n 's|^// EXPECT_LIR_FORBIDS: ||p' "$f" | tr -d '\r' | while IFS= read -r pat; do
        if printf '%s' "$pat" | grep -Eq '^[A-Za-z_][A-Za-z0-9_]*$'; then
            if grep -Eq "(^|[^A-Za-z0-9_@])$pat([^A-Za-z0-9_]|$)" "$lir"; then
                echo "FAIL: $f contains forbidden '$pat' in lowered LIR"
                grep -En "(^|[^A-Za-z0-9_@])$pat([^A-Za-z0-9_]|$)" "$lir"
                exit 1
            fi
        else
            if grep -Fq "$pat" "$lir"; then
                echo "FAIL: $f contains forbidden '$pat' in lowered LIR"
                grep -Fn "$pat" "$lir"
                exit 1
            fi
        fi
    done
    # Check EXPECT_SIG: @fn (t0, t1) -> ret
    sed -n 's|^// EXPECT_SIG: ||p' "$f" | tr -d '\r' | while IFS= read -r sig; do
        fname=$(printf '%s' "$sig" | awk '{print $1}')
        want_types=$(printf '%s' "$sig" | sed "s|^$fname ||")
        # Normalize printed func decl by stripping `%name: ` parameter names
        actual_line=$(grep -F "func $fname(" "$lir" | head -n 1 || true)
        if [ -z "$actual_line" ]; then
            echo "FAIL: $f missing function '$fname' in lowered LIR"
            cat "$lir"
            exit 1
        fi
        norm=$(printf '%s' "$actual_line" | sed -E 's/%[A-Za-z0-9_]+: //g')
        if ! printf '%s\n' "$norm" | grep -Fq "func $fname$want_types"; then
            echo "FAIL: $f signature mismatch for $fname"
            echo "  expected: func $fname$want_types"
            echo "  actual:   $norm"
            exit 1
        fi
    done
}

echo "[M4 gate] Checking C ABI lowering snapshots in tests/lowering_abi/*.pir..."
for f in tests/lowering_abi/*.pir; do
    echo "  ABI $f"
    "$PIRC" --emit-lir "$f" > build/m4_lir.pir
    "$PIRC" --verify-lir build/m4_lir.pir
    check_lir_directives "$f" build/m4_lir.pir
done

echo "[M4 gate] Checking exception lowering snapshots in tests/lowering_exceptions/*.pir..."
for f in tests/lowering_exceptions/*.pir; do
    echo "  EXC $f"
    "$PIRC" --emit-lir "$f" > build/m4_lir.pir
    "$PIRC" --verify-lir build/m4_lir.pir
    check_lir_directives "$f" build/m4_lir.pir
done

echo "[M4 gate] Checking varargs lowering snapshots in tests/lowering_varargs/*.pir..."
for f in tests/lowering_varargs/*.pir; do
    echo "  VA $f"
    "$PIRC" --emit-lir "$f" > build/m4_lir.pir
    "$PIRC" --verify-lir build/m4_lir.pir
    check_lir_directives "$f" build/m4_lir.pir
done

echo "[M4 gate] Running end-to-end M4 HIR -> LIR -> interp tests in tests/hir_m4_exec/*.pir..."
for f in tests/hir_m4_exec/*.pir; do
    exp=$(sed -n 's|^// EXPECT_RET: ||p' "$f" | tr -d '\r' | head -n 1)
    if [ -z "$exp" ]; then exp=0; fi
    for prof in default scalar minimal; do
        echo "  M4_EXEC ($prof) $f"
        "$PIRC" --emit-lir --caps="$prof" "$f" > build/m4_lir.pir
        "$PIRC" --verify-lir --caps="$prof" build/m4_lir.pir
        set +e
        "$PIRC" --interp --caps="$prof" "$f" >/dev/null 2>/tmp/pirc_m4_err.txt
        rc=$?
        set -e
        if [ "$rc" -ne "$exp" ]; then
            echo "FAIL: $f (--caps=$prof) exited with $rc (expected $exp)"
            cat /tmp/pirc_m4_err.txt
            exit 1
        fi
    done
done

rm -f build/m4_lir.pir /tmp/pirc_m4_err.txt
echo "ALL M4 C-ABI / VARARGS / EXCEPTIONS TESTS PASSED"
