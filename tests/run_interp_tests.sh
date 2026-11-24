#!/bin/sh
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

set -eu

PIRC="${1:?usage: run_interp_tests.sh <pirc-bin>}"

echo "[M2 gate] Running positive LIR interpreter tests in tests/interp_exec/*.pir..."
for f in tests/interp_exec/*.pir; do
    caps=""
    cap_val=$(sed -n 's|^// CAPS: ||p' "$f" | head -n 1)
    if [ -n "$cap_val" ]; then caps="--caps=$cap_val"; fi
    exp_exit=$(sed -n 's|^// EXPECT_EXIT: ||p' "$f" | head -n 1)
    if [ -z "$exp_exit" ]; then
        exp_exit=$(sed -n 's|^// EXPECT_RET: ||p' "$f" | head -n 1)
    fi
    if [ -z "$exp_exit" ]; then exp_exit=0; fi
    exp_out=$(sed -n 's|^// EXPECT_OUT: ||p' "$f" | head -n 1)

    echo "  EXEC $f"
    set +e
    out=$("$PIRC" --interp $caps "$f" 2>/tmp/pirc_interp_err.txt)
    rc=$?
    set -e
    if [ "$rc" -ne "$exp_exit" ]; then
        echo "FAIL: $f exited with $rc (expected $exp_exit)"
        cat /tmp/pirc_interp_err.txt
        exit 1
    fi
    if [ -n "$exp_out" ] && [ "$out" != "$exp_out" ]; then
        echo "FAIL: $f stdout mismatch"
        echo "  expected: '$exp_out'"
        echo "  got:      '$out'"
        exit 1
    fi
done

echo "[M2 gate] Running sanitizer tests in tests/interp_sanitizer/*.pir..."
for f in tests/interp_sanitizer/*.pir; do
    if grep -q "EXPECT_TRAP" "$f"; then
        exp_rc=132
        exp_pat="pir-interp: trap:"
    else
        exp_rc=134
        exp_pat=$(sed -n 's|^// EXPECT_SANITIZER_ERR: ||p' "$f" | head -n 1)
        if [ -z "$exp_pat" ]; then exp_pat="pir-interp: ub:"; fi
    fi
    echo "  SAN $f"
    set +e
    err=$("$PIRC" --interp "$f" 2>&1 >/dev/null)
    rc=$?
    set -e
    if [ "$rc" -ne "$exp_rc" ]; then
        echo "FAIL: $f exited with $rc (expected $exp_rc)"
        echo "  stderr: $err"
        exit 1
    fi
    if ! printf '%s\n' "$err" | grep -q "$exp_pat"; then
        echo "FAIL: $f missing '$exp_pat' in stderr: $err"
        exit 1
    fi
done

echo "[M2 gate] Running negative UB/trap sanitizer tests in tests/interp_neg/*.pir..."
for f in tests/interp_neg/*.pir; do
    kind=$(sed -n 's|^// EXPECT_KIND: ||p' "$f" | head -n 1)
    if [ "$kind" = "trap" ]; then
        exp_rc=132
        exp_pat="pir-interp: trap:"
    else
        exp_rc=134
        exp_pat="pir-interp: ub:"
    fi
    echo "  NEG ($kind) $f"
    set +e
    err=$("$PIRC" --interp "$f" 2>&1 >/dev/null)
    rc=$?
    set -e
    if [ "$rc" -ne "$exp_rc" ]; then
        echo "FAIL: $f exited with $rc (expected $exp_rc for $kind)"
        echo "  stderr: $err"
        exit 1
    fi
    if ! printf '%s\n' "$err" | grep -q "$exp_pat"; then
        echo "FAIL: $f missing '$exp_pat' in stderr: $err"
        exit 1
    fi
done
rm -f /tmp/pirc_interp_err.txt
echo "ALL M2 INTERPRETER TESTS PASSED"
