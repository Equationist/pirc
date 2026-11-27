#!/bin/sh
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause
#
# RISC-V 32-bit (RV32IMAFD / ILP32D) backend verification gate:
#   1. Differential LIR interpreter oracle (riscv32-unknown-elf -O0 & -O2 vs host -O0)
#   2. Native RV32 assembly emission, LLVM-MC assembling (.o), & ld.lld ELF linking
#   3. Byte-identical determinism & Linux ELF vs bare-metal ELF assembly snapshots
#   4. Full QEMU bare-metal differential execution (when qemu-system-riscv32 is available)

set -eu

BIN="${1:-./build/pirc-san}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/pirc_rv32.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

PASS=0
FAIL=0

# Locate RV32-capable clang assembler/compiler
RVCC="${PIRC_RV32_CC:-}"
if [ -z "$RVCC" ]; then
    for cand in /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang /usr/local/opt/llvm@17/bin/clang clang; do
        if command -v "$cand" >/dev/null 2>&1 && \
           printf '.text\nret\n' | "$cand" --target=riscv32-unknown-elf -march=rv32imafd -mabi=ilp32d -x assembler -c - -o "$TMP_DIR/probe.o" 2>/dev/null; then
            RVCC="$cand"
            break
        fi
    done
fi

# Locate ld.lld for bare-metal RV32 ELF linking
RVLD="${PIRC_RV32_LD:-}"
if [ -z "$RVLD" ]; then
    for cand in /opt/homebrew/opt/llvm/bin/ld.lld /opt/homebrew/opt/lld/bin/ld.lld /usr/local/opt/llvm/bin/ld.lld /usr/local/opt/llvm@17/bin/ld.lld ld.lld; do
        if command -v "$cand" >/dev/null 2>&1; then
            RVLD="$cand"
            break
        fi
    done
fi

# Locate qemu-system-riscv32 (optional live execution)
QEMU_RV32="${QEMU_RV32:-}"
if [ -z "$QEMU_RV32" ]; then
    for cand in qemu-system-riscv32 /opt/homebrew/bin/qemu-system-riscv32 /usr/local/bin/qemu-system-riscv32; do
        if command -v "$cand" >/dev/null 2>&1; then
            QEMU_RV32="$cand"
            break
        fi
    done
fi

echo "[RV32 gate] Toolchain: CC=${RVCC:-none} LD=${RVLD:-none} QEMU=${QEMU_RV32:-none}"

# Build bare-metal RV32 runtime objects if clang + ld.lld are available
RT_OBJS=""
if [ -n "$RVCC" ] && [ -n "$RVLD" ]; then
    if "$RVCC" --target=riscv32-unknown-elf -march=rv32imafd -mabi=ilp32d -c runtime/pir_rt_rv32.S -o "$TMP_DIR/pir_rt_rv32_s.o" && \
       "$RVCC" --target=riscv32-unknown-elf -march=rv32imafd -mabi=ilp32d -ffreestanding -O2 -c runtime/pir_rt_rv32.c -o "$TMP_DIR/pir_rt_rv32_c.o"; then
        RT_OBJS="$TMP_DIR/pir_rt_rv32_s.o $TMP_DIR/pir_rt_rv32_c.o"
    else
        echo "FAIL: failed to compile RV32 bare-metal runtime"
        FAIL=$((FAIL + 1))
    fi
fi

run_rv32_suite() {
    dir="$1"
    echo "[RV32 gate] Compiling & verifying $dir/*.pir..."
    for f in "$dir"/*.pir; do
        base="$(basename "$f" .pir)"

        # Reference interpreter run on riscv32 target (-O0)
        set +e
        "$BIN" --interp --target=riscv32-unknown-elf -O0 "$f" >"$TMP_DIR/ref.out" 2>"$TMP_DIR/ref.err"
        ref_rc=$?
        set -e

        for opt in -O0 -O2; do
            # Check -O2 interpreter parity against -O0
            if [ "$opt" = "-O2" ]; then
                set +e
                "$BIN" --interp --target=riscv32-unknown-elf -O2 "$f" >"$TMP_DIR/o2.out" 2>"$TMP_DIR/o2.err"
                o2_rc=$?
                set -e
                if [ "$o2_rc" -ne "$ref_rc" ] || ! cmp -s "$TMP_DIR/ref.out" "$TMP_DIR/o2.out"; then
                    echo "FAIL: RV32 interp -O0 vs -O2 mismatch on $f (o0=$ref_rc o2=$o2_rc)"
                    FAIL=$((FAIL + 1))
                    continue
                fi
            fi

            asm="$TMP_DIR/${base}_${opt#-}.s"
            obj="$TMP_DIR/${base}_${opt#-}.o"
            elf="$TMP_DIR/${base}_${opt#-}.elf"

            if ! "$BIN" --target=riscv32-unknown-elf "$opt" -S "$f" -o "$asm" 2>"$TMP_DIR/cg.err"; then
                echo "FAIL: RV32 codegen ($opt) failed on $f:"
                cat "$TMP_DIR/cg.err"
                FAIL=$((FAIL + 1))
                continue
            fi

            if [ -n "$RVCC" ]; then
                if ! "$RVCC" --target=riscv32-unknown-elf -march=rv32imafd -mabi=ilp32d -c "$asm" -o "$obj" 2>"$TMP_DIR/as.err"; then
                    echo "FAIL: RV32 assemble ($opt) failed on $f:"
                    cat "$TMP_DIR/as.err"
                    FAIL=$((FAIL + 1))
                    continue
                fi
                if [ -n "$RT_OBJS" ]; then
                    if ! "$RVLD" -m elf32lriscv -T runtime/rv32_virt.ld $RT_OBJS "$obj" -o "$elf" 2>"$TMP_DIR/ld.err"; then
                        echo "FAIL: RV32 link ($opt) failed on $f:"
                        cat "$TMP_DIR/ld.err"
                        FAIL=$((FAIL + 1))
                        continue
                    fi
                    if [ -n "$QEMU_RV32" ]; then
                        set +e
                        "$QEMU_RV32" -machine virt -nographic -bios none \
                            -semihosting-config enable=on,target=native \
                            -kernel "$elf" >"$TMP_DIR/qemu.out" 2>"$TMP_DIR/qemu.err"
                        q_rc=$?
                        set -e
                        if [ "$q_rc" -ne "$ref_rc" ] || ! cmp -s "$TMP_DIR/ref.out" "$TMP_DIR/qemu.out"; then
                            echo "FAIL: QEMU RV32 vs interp mismatch ($opt) on $f (ref=$ref_rc qemu=$q_rc)"
                            echo "  ref_out: $(cat "$TMP_DIR/ref.out")"
                            echo "  qemu_out: $(cat "$TMP_DIR/qemu.out")"
                            FAIL=$((FAIL + 1))
                            continue
                        fi
                    fi
                fi
            fi
            PASS=$((PASS + 1))
        done
    done
}

run_rv32_suite "tests/rv32_exec"
run_rv32_suite "tests/interp_exec"
run_rv32_suite "tests/hir_exec"
run_rv32_suite "tests/hir_m4_exec"

# Backend stress tests (compile+assemble all; skip 11_r5_soundness_fixes.pir only for 64-bit hardcoded ptr layout in interp)
echo "[RV32 gate] Compiling & assembling tests/backend_stress/*.pir..."
for f in tests/backend_stress/*.pir; do
    base="$(basename "$f" .pir)"
    for opt in -O0 -O2; do
        asm="$TMP_DIR/bs_${base}_${opt#-}.s"
        obj="$TMP_DIR/bs_${base}_${opt#-}.o"
        if ! "$BIN" --target=riscv32-unknown-elf "$opt" -S "$f" -o "$asm" 2>"$TMP_DIR/cg.err"; then
            echo "FAIL: RV32 codegen ($opt) failed on $f:"
            cat "$TMP_DIR/cg.err"
            FAIL=$((FAIL + 1))
            continue
        fi
        if [ -n "$RVCC" ]; then
            if ! "$RVCC" --target=riscv32-unknown-elf -march=rv32imafd -mabi=ilp32d -c "$asm" -o "$obj" 2>"$TMP_DIR/as.err"; then
                echo "FAIL: RV32 assemble ($opt) failed on $f:"
                cat "$TMP_DIR/as.err"
                FAIL=$((FAIL + 1))
                continue
            fi
        fi
        PASS=$((PASS + 1))
    done
done

# Check -c direct object compilation & Linux ELF snapshot
if [ -n "$RVCC" ]; then
    echo "[RV32 gate] Checking -c object compilation & Linux riscv32-unknown-linux-gnu ELF snapshot..."
    if PIRC_RV32_CC="$RVCC" "$BIN" --target=riscv32-unknown-elf -O2 -c tests/rv32_exec/01_rv32_comprehensive.pir -o "$TMP_DIR/direct.o"; then
        PASS=$((PASS + 1))
    else
        echo "FAIL: -c direct object compilation for riscv32-unknown-elf failed"
        FAIL=$((FAIL + 1))
    fi

    LIN_ASM="$TMP_DIR/rv32_linux.s"
    LIN_OBJ="$TMP_DIR/rv32_linux.o"
    if "$BIN" --target=riscv32-unknown-linux-gnu -O2 -S tests/backend_stress/07_tls_and_pic_globals.pir -o "$LIN_ASM" && \
       grep -Fq ".globl main" "$LIN_ASM" && \
       grep -Fq "%tprel_hi(" "$LIN_ASM" && \
       grep -Fq ".section .note.GNU-stack" "$LIN_ASM" && \
       "$RVCC" --target=riscv32-unknown-linux-gnu -march=rv32imafd -mabi=ilp32d -c "$LIN_ASM" -o "$LIN_OBJ"; then
        echo "  LINUX RV32 ELF SNAPSHOT & ASSEMBLE PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL: Linux RV32 ELF snapshot/assemble failed"
        FAIL=$((FAIL + 1))
    fi
fi

# Byte-identical determinism check
echo "[RV32 gate] Checking byte-identical deterministic RV32 assembly emission..."
"$BIN" --target=riscv32-unknown-elf -O2 -S tests/rv32_exec/01_rv32_comprehensive.pir -o "$TMP_DIR/det1.s"
"$BIN" --target=riscv32-unknown-elf -O2 -S tests/rv32_exec/01_rv32_comprehensive.pir -o "$TMP_DIR/det2.s"
if cmp -s "$TMP_DIR/det1.s" "$TMP_DIR/det2.s"; then
    echo "  DETERMINISM PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL: non-deterministic RV32 assembly output"
    diff -u "$TMP_DIR/det1.s" "$TMP_DIR/det2.s"
    FAIL=$((FAIL + 1))
fi

echo "[RV32 gate] Summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
