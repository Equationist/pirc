#!/bin/sh
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

set -eu

BIN="${1:-./build/pirc-san}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/pirc_m6.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

PASS=0
FAIL=0

# Compile runtime/pir_rt.c once for linking exception tests
RT_OBJ="$TMP_DIR/pir_rt.o"
cc -O2 -c runtime/pir_rt.c -o "$RT_OBJ"

run_native_suite() {
    dir="$1"
    link_rt="$2"
    echo "[M6 gate] Native aarch64-apple-darwin differential execution: $dir/*.pir..."
    for f in "$dir"/*.pir; do
        base="$(basename "$f" .pir)"
        # Reference oracle: pirc --interp --target=aarch64-apple-darwin
        set +e
        "$BIN" --interp --target=aarch64-apple-darwin -O0 "$f" >"$TMP_DIR/ref.out" 2>"$TMP_DIR/ref.err"
        ref_rc=$?
        set -e

        for opt in -O0 -O2; do
            asm="$TMP_DIR/${base}_${opt#-}.s"
            exe="$TMP_DIR/${base}_${opt#-}"

            if ! "$BIN" --target=aarch64-apple-darwin "$opt" -S "$f" -o "$asm" 2>"$TMP_DIR/cg.err"; then
                echo "FAIL: codegen ($opt) failed on $f:"
                cat "$TMP_DIR/cg.err"
                FAIL=$((FAIL + 1))
                continue
            fi

            if [ "$link_rt" = "1" ]; then
                link_cmd_extra="$RT_OBJ"
            else
                link_cmd_extra=""
            fi

            if ! cc "$asm" $link_cmd_extra -o "$exe" 2>"$TMP_DIR/cc.err"; then
                echo "FAIL: cc assemble/link ($opt) failed on $f:"
                cat "$TMP_DIR/cc.err"
                FAIL=$((FAIL + 1))
                continue
            fi

            set +e
            "$exe" >"$TMP_DIR/nat.out" 2>"$TMP_DIR/nat.err"
            nat_rc=$?
            set -e

            if [ "$nat_rc" -ne "$ref_rc" ] || ! cmp -s "$TMP_DIR/ref.out" "$TMP_DIR/nat.out"; then
                echo "FAIL: native vs interp mismatch ($opt) on $f (ref_rc=$ref_rc nat_rc=$nat_rc)"
                echo "  ref_out: $(cat "$TMP_DIR/ref.out")"
                echo "  nat_out: $(cat "$TMP_DIR/nat.out")"
                FAIL=$((FAIL + 1))
            else
                PASS=$((PASS + 1))
            fi
        done
    done
}

run_native_suite "tests/interp_exec" 0
run_native_suite "tests/hir_exec" 0
run_native_suite "tests/hir_m4_exec" 1
run_native_suite "tests/backend_stress" 0

echo "[M6 gate] Checking -c object compilation and linking..."
if "$BIN" --target=aarch64-apple-darwin -O2 -c tests/backend_stress/01_high_reg_pressure_gp.pir -o "$TMP_DIR/obj_test.o" && \
   cc "$TMP_DIR/obj_test.o" -o "$TMP_DIR/obj_test"; then
    set +e
    "$TMP_DIR/obj_test"
    rc=$?
    set -e
    if [ "$rc" -eq 75 ]; then
        echo "  OBJ PASS (-c -> .o -> link -> exit 75)"
        PASS=$((PASS + 1))
    else
        echo "FAIL: -c object binary exited $rc (expected 75)"
        FAIL=$((FAIL + 1))
    fi
else
    echo "FAIL: -c object compilation failed"
    FAIL=$((FAIL + 1))
fi

echo "[M6 gate] Checking Linux aarch64-unknown-linux-gnu ELF assembly snapshots..."
LINUX_ASM="$TMP_DIR/linux_tls.s"
if "$BIN" --target=aarch64-unknown-linux-gnu -O2 -S tests/backend_stress/07_tls_and_pic_globals.pir -o "$LINUX_ASM"; then
    ok=1
    for pat in ".text" ".globl main" ".weak weak_helper" "TPIDR_EL0" ":tprel_hi12:t_slot" ":tprel_lo12_nc:t_slot" ".section .rodata" ".section .note.GNU-stack"; do
        if ! grep -Fq -- "$pat" "$LINUX_ASM"; then
            echo "FAIL: Linux ELF asm missing expected pattern: [$pat]"
            ok=0
        fi
    done
    for bad in "_main:" "@PAGE" "@TLVPPAGE" ".subsections_via_symbols"; do
        if grep -Fq -- "$bad" "$LINUX_ASM"; then
            echo "FAIL: Linux ELF asm contains forbidden Mach-O pattern: [$bad]"
            ok=0
        fi
    done
    if [ "$ok" -eq 1 ]; then
        echo "  LINUX ELF SNAPSHOT PASS"
        PASS=$((PASS + 1))
    else
        cat "$LINUX_ASM"
        FAIL=$((FAIL + 1))
    fi
else
    echo "FAIL: Linux aarch64-unknown-linux-gnu codegen failed"
    FAIL=$((FAIL + 1))
fi

echo "[M6 gate] Checking DWARF .file/.loc debug info on macOS ARM64..."
DBG_ASM="$TMP_DIR/dbg.s"
DBG_OBJ="$TMP_DIR/dbg.o"
DBG_BIN="$TMP_DIR/dbg_bin"
if "$BIN" --target=aarch64-apple-darwin -O0 -S tests/interp_exec/22_volatile_and_debug_locs.pir -o "$DBG_ASM" && \
   grep -Fq '.file 1 "tests/interp_exec/22_volatile_and_debug_locs.pir"' "$DBG_ASM" && \
   grep -Fq '.loc 1 8 5' "$DBG_ASM" && \
   grep -Fq '.loc 1 16 5' "$DBG_ASM" && \
   grep -Fq '.loc 1 21 5' "$DBG_ASM" && \
   cc -g -c "$DBG_ASM" -o "$DBG_OBJ" && \
   cc -g "$DBG_OBJ" -o "$DBG_BIN"; then
    dbg_ok=1
    if command -v dwarfdump >/dev/null 2>&1; then
        DLINE="$TMP_DIR/dwarfdump.txt"
        dwarfdump --debug-line "$DBG_OBJ" >"$DLINE" 2>/dev/null || true
        if ! grep -Fq "22_volatile_and_debug_locs.pir" "$DLINE" || \
           ! grep -Eq "[[:space:]]+8[[:space:]]+5[[:space:]]" "$DLINE" || \
           ! grep -Eq "[[:space:]]+16[[:space:]]+5[[:space:]]" "$DLINE" || \
           ! grep -Eq "[[:space:]]+21[[:space:]]+5[[:space:]]" "$DLINE"; then
            echo "FAIL: dwarfdump missing expected 22_volatile_and_debug_locs.pir line entries (8, 16, 21)"
            cat "$DLINE"
            dbg_ok=0
        fi
    fi
    if [ "$dbg_ok" -eq 1 ] && command -v lldb >/dev/null 2>&1; then
        LLDB_OUT="$TMP_DIR/lldb.txt"
        lldb --batch -o "image dump line-table 22_volatile_and_debug_locs.pir" "$DBG_BIN" >"$LLDB_OUT" 2>&1 || true
        if ! grep -Fq "22_volatile_and_debug_locs.pir:8:5" "$LLDB_OUT" || \
           ! grep -Fq "22_volatile_and_debug_locs.pir:16:5" "$LLDB_OUT" || \
           ! grep -Fq "22_volatile_and_debug_locs.pir:21:5" "$LLDB_OUT"; then
            echo "FAIL: lldb line-table did not resolve 22_volatile_and_debug_locs.pir lines 8, 16, 21 in linked binary"
            cat "$LLDB_OUT"
            dbg_ok=0
        fi
    fi
    if [ "$dbg_ok" -eq 1 ]; then
        echo "  DWARF & LLDB LINE-TABLE PASS (22_volatile_and_debug_locs.pir:8,16,21 verified in linked binary)"
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
else
    echo "FAIL: DWARF debug loc test failed"
    FAIL=$((FAIL + 1))
fi

echo "[M6 gate] Checking byte-identical deterministic assembly emission..."
"$BIN" --target=aarch64-apple-darwin -O2 -S tests/backend_stress/01_high_reg_pressure_gp.pir -o "$TMP_DIR/det1.s"
"$BIN" --target=aarch64-apple-darwin -O2 -S tests/backend_stress/01_high_reg_pressure_gp.pir -o "$TMP_DIR/det2.s"
if cmp -s "$TMP_DIR/det1.s" "$TMP_DIR/det2.s"; then
    echo "  DETERMINISM PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL: non-deterministic assembly output"
    diff -u "$TMP_DIR/det1.s" "$TMP_DIR/det2.s"
    FAIL=$((FAIL + 1))
fi

echo "[M6 gate] Summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
