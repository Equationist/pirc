#!/bin/sh
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

set -eu

BIN="${1:-./build/pirc-san}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/pirc_m8.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

PASS=0
FAIL=0

# Compile runtime/pir_rt.c once for x86_64-apple-darwin exception tests
RT_OBJ="$TMP_DIR/pir_rt_x64.o"
cc -arch x86_64 -O2 -c runtime/pir_rt.c -o "$RT_OBJ"

run_x64_suite() {
    dir="$1"
    link_rt="$2"
    echo "[M8 gate] Rosetta x86_64-apple-darwin differential execution: $dir/*.pir..."
    for f in "$dir"/*.pir; do
        base="$(basename "$f" .pir)"
        set +e
        "$BIN" --interp --target=x86_64-apple-darwin -O0 "$f" >"$TMP_DIR/ref.out" 2>"$TMP_DIR/ref.err"
        ref_rc=$?
        set -e

        for opt in -O0 -O2; do
            asm="$TMP_DIR/${base}_${opt#-}.s"
            exe="$TMP_DIR/${base}_${opt#-}"

            if ! "$BIN" --target=x86_64-apple-darwin "$opt" -S "$f" -o "$asm" 2>"$TMP_DIR/cg.err"; then
                echo "FAIL: x64 codegen ($opt) failed on $f:"
                cat "$TMP_DIR/cg.err"
                FAIL=$((FAIL + 1))
                continue
            fi

            if [ "$link_rt" = "1" ]; then
                link_cmd_extra="$RT_OBJ"
            else
                link_cmd_extra=""
            fi

            if ! cc -arch x86_64 "$asm" $link_cmd_extra -o "$exe" 2>"$TMP_DIR/cc.err"; then
                echo "FAIL: cc -arch x86_64 assemble/link ($opt) failed on $f:"
                cat "$TMP_DIR/cc.err"
                FAIL=$((FAIL + 1))
                continue
            fi

            set +e
            arch -x86_64 "$exe" >"$TMP_DIR/nat.out" 2>"$TMP_DIR/nat.err"
            nat_rc=$?
            set -e

            if [ "$nat_rc" -ne "$ref_rc" ] || ! cmp -s "$TMP_DIR/ref.out" "$TMP_DIR/nat.out"; then
                echo "FAIL: x64 vs interp mismatch ($opt) on $f (ref_rc=$ref_rc nat_rc=$nat_rc)"
                echo "  ref_out: $(cat "$TMP_DIR/ref.out")"
                echo "  nat_out: $(cat "$TMP_DIR/nat.out")"
                FAIL=$((FAIL + 1))
            else
                PASS=$((PASS + 1))
            fi
        done
    done
}

run_x64_suite "tests/interp_exec" 0
run_x64_suite "tests/hir_exec" 0
run_x64_suite "tests/hir_m4_exec" 1
run_x64_suite "tests/backend_stress" 0

echo "[M8 gate] Checking -c object compilation and linking for x86_64-apple-darwin..."
if "$BIN" --target=x86_64-apple-darwin -O2 -c tests/backend_stress/01_high_reg_pressure_gp.pir -o "$TMP_DIR/obj_test_x64.o" && \
   cc -arch x86_64 "$TMP_DIR/obj_test_x64.o" -o "$TMP_DIR/obj_test_x64"; then
    set +e
    arch -x86_64 "$TMP_DIR/obj_test_x64"
    rc=$?
    set -e
    if [ "$rc" -eq 75 ]; then
        echo "  OBJ PASS (-c x86_64 -> .o -> link -> exit 75)"
        PASS=$((PASS + 1))
    else
        echo "FAIL: -c x86_64 object binary exited $rc (expected 75)"
        FAIL=$((FAIL + 1))
    fi
else
    echo "FAIL: -c x86_64 object compilation failed"
    FAIL=$((FAIL + 1))
fi

echo "[M8 gate] Checking Linux x86_64-unknown-linux-gnu ELF assembly snapshots & assembly..."
LINUX_ASM="$TMP_DIR/linux_x64_tls.s"
LINUX_OBJ="$TMP_DIR/linux_x64_tls.o"
if "$BIN" --target=x86_64-unknown-linux-gnu -O2 -S tests/backend_stress/07_tls_and_pic_globals.pir -o "$LINUX_ASM"; then
    ok=1
    for pat in ".text" ".intel_syntax noprefix" ".globl main" ".weak weak_helper" "fs:[0]" "t_slot@TPOFF" ".section .rodata" ".section .note.GNU-stack"; do
        if ! grep -Fq -- "$pat" "$LINUX_ASM"; then
            echo "FAIL: Linux x86_64 ELF asm missing expected pattern: [$pat]"
            ok=0
        fi
    done
    for bad in "_main:" "@TLVP" ".subsections_via_symbols"; do
        if grep -Fq -- "$bad" "$LINUX_ASM"; then
            echo "FAIL: Linux x86_64 ELF asm contains forbidden Mach-O pattern: [$bad]"
            ok=0
        fi
    done
    if [ "$ok" -eq 1 ] && ! clang --target=x86_64-unknown-linux-gnu -c "$LINUX_ASM" -o "$LINUX_OBJ" 2>"$TMP_DIR/elf_as.err"; then
        echo "FAIL: clang --target=x86_64-unknown-linux-gnu -c failed to assemble ELF asm:"
        cat "$TMP_DIR/elf_as.err"
        ok=0
    fi
    if [ "$ok" -eq 1 ]; then
        echo "  LINUX x86_64 ELF SNAPSHOT & ASSEMBLE PASS"
        PASS=$((PASS + 1))
    else
        cat "$LINUX_ASM"
        FAIL=$((FAIL + 1))
    fi
else
    echo "FAIL: Linux x86_64-unknown-linux-gnu codegen failed"
    FAIL=$((FAIL + 1))
fi

echo "[M8 gate] Checking DWARF .file/.loc debug info on x86_64-apple-darwin..."
DBG_ASM="$TMP_DIR/dbg_x64.s"
DBG_OBJ="$TMP_DIR/dbg_x64.o"
DBG_BIN="$TMP_DIR/dbg_x64_bin"
if "$BIN" --target=x86_64-apple-darwin -O0 -S tests/interp_exec/22_volatile_and_debug_locs.pir -o "$DBG_ASM" && \
   grep -Fq '.file 1 "tests/interp_exec/22_volatile_and_debug_locs.pir"' "$DBG_ASM" && \
   grep -Fq '.loc 1 8 5' "$DBG_ASM" && \
   grep -Fq '.loc 1 16 5' "$DBG_ASM" && \
   grep -Fq '.loc 1 21 5' "$DBG_ASM" && \
   cc -arch x86_64 -g -c "$DBG_ASM" -o "$DBG_OBJ" && \
   cc -arch x86_64 -g "$DBG_OBJ" -o "$DBG_BIN"; then
    dbg_ok=1
    if command -v dwarfdump >/dev/null 2>&1; then
        DLINE="$TMP_DIR/dwarfdump_x64.txt"
        dwarfdump --debug-line "$DBG_OBJ" >"$DLINE" 2>/dev/null || true
        if ! grep -Fq "22_volatile_and_debug_locs.pir" "$DLINE" || \
           ! grep -Eq "[[:space:]]+8[[:space:]]+5[[:space:]]" "$DLINE" || \
           ! grep -Eq "[[:space:]]+16[[:space:]]+5[[:space:]]" "$DLINE" || \
           ! grep -Eq "[[:space:]]+21[[:space:]]+5[[:space:]]" "$DLINE"; then
            echo "FAIL: dwarfdump missing expected x86_64 line entries (8, 16, 21)"
            cat "$DLINE"
            dbg_ok=0
        fi
    fi
    if [ "$dbg_ok" -eq 1 ]; then
        echo "  DWARF x86_64 LINE-TABLE PASS"
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
else
    echo "FAIL: DWARF x86_64 debug loc test failed"
    FAIL=$((FAIL + 1))
fi

echo "[M8 gate] Checking byte-identical deterministic x86_64 assembly emission..."
"$BIN" --target=x86_64-apple-darwin -O2 -S tests/backend_stress/01_high_reg_pressure_gp.pir -o "$TMP_DIR/det1.s"
"$BIN" --target=x86_64-apple-darwin -O2 -S tests/backend_stress/01_high_reg_pressure_gp.pir -o "$TMP_DIR/det2.s"
if cmp -s "$TMP_DIR/det1.s" "$TMP_DIR/det2.s"; then
    echo "  DETERMINISM PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL: non-deterministic x86_64 assembly output"
    diff -u "$TMP_DIR/det1.s" "$TMP_DIR/det2.s"
    FAIL=$((FAIL + 1))
fi

echo "[M8 gate] Running bidirectional C ABI conformance suite on x86_64-apple-darwin (Rosetta 2)..."
if PIRC="$BIN" TARGET=x86_64-apple-darwin bash tests/c_abi_conformance/run_c_abi.sh; then
    PASS=$((PASS + 1))
else
    echo "FAIL: C ABI conformance suite failed on x86_64-apple-darwin"
    FAIL=$((FAIL + 1))
fi

echo "[M8 gate] Summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
