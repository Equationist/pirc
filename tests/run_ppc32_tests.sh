#!/bin/sh
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause
#
# PowerPC 32-bit Big-Endian (SysV SVR4) backend verification gate:
#   1. Differential LIR interpreter oracle (powerpc-unknown-elf -O0 vs -O2)
#   2. Native PPC32 assembly emission, LLVM-MC assembling (.o), & ld.lld ELF linking
#   3. Byte-identical determinism & Linux powerpc-unknown-linux-gnu ELF snapshot
#   4. Full QEMU bamboo bare-metal differential execution (when qemu-system-ppc is available)

set -eu

BIN="${1:-./build/pirc-san}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/pirc_ppc32.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

PASS=0
FAIL=0

# Locate PPC32-capable clang assembler/compiler
PPCCC="${PIRC_PPC32_CC:-}"
if [ -z "$PPCCC" ]; then
    for cand in /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang /usr/local/opt/llvm@17/bin/clang clang; do
        if command -v "$cand" >/dev/null 2>&1 && \
           printf '.text\nblr\n' | "$cand" --target=powerpc-unknown-elf -x assembler -c - -o "$TMP_DIR/probe.o" 2>/dev/null; then
            PPCCC="$cand"
            break
        fi
    done
fi

# Locate ld.lld for bare-metal PPC32 ELF linking
PPCLD="${PIRC_PPC32_LD:-}"
if [ -z "$PPCLD" ]; then
    for cand in /opt/homebrew/opt/llvm/bin/ld.lld /opt/homebrew/opt/lld/bin/ld.lld /usr/local/opt/llvm/bin/ld.lld /usr/local/opt/llvm@17/bin/ld.lld ld.lld; do
        if command -v "$cand" >/dev/null 2>&1; then
            PPCLD="$cand"
            break
        fi
    done
fi

# Locate qemu-system-ppc (optional live execution)
QEMU_PPC="${QEMU_PPC:-}"
if [ -z "$QEMU_PPC" ]; then
    for cand in qemu-system-ppc /opt/homebrew/bin/qemu-system-ppc /usr/local/bin/qemu-system-ppc; do
        if command -v "$cand" >/dev/null 2>&1; then
            QEMU_PPC="$cand"
            break
        fi
    done
fi

echo "[PPC32 gate] Toolchain: CC=${PPCCC:-none} LD=${PPCLD:-none} QEMU=${QEMU_PPC:-none}"

# Build bare-metal PPC32 runtime objects if clang + ld.lld are available
RT_OBJS=""
if [ -n "$PPCCC" ] && [ -n "$PPCLD" ]; then
    if "$PPCCC" --target=powerpc-unknown-elf -c runtime/pir_rt_ppc32.S -o "$TMP_DIR/pir_rt_ppc32_s.o" && \
       "$PPCCC" --target=powerpc-unknown-elf -ffreestanding -O2 -c runtime/pir_rt_ppc32.c -o "$TMP_DIR/pir_rt_ppc32_c.o"; then
        RT_OBJS="$TMP_DIR/pir_rt_ppc32_s.o $TMP_DIR/pir_rt_ppc32_c.o"
    else
        echo "FAIL: failed to compile PPC32 bare-metal runtime"
        FAIL=$((FAIL + 1))
    fi
fi

run_ppc32_suite() {
    dir="$1"
    echo "[PPC32 gate] Compiling & verifying $dir/*.pir..."
    for f in "$dir"/*.pir; do
        base="$(basename "$f" .pir)"

        # Reference interpreter run on powerpc-unknown-elf target (-O0)
        set +e
        "$BIN" --interp --target=powerpc-unknown-elf -O0 "$f" >"$TMP_DIR/ref.out" 2>"$TMP_DIR/ref.err"
        ref_rc=$?
        set -e

        for opt in -O0 -O2; do
            # Check -O2 interpreter parity against -O0
            if [ "$opt" = "-O2" ]; then
                set +e
                "$BIN" --interp --target=powerpc-unknown-elf -O2 "$f" >"$TMP_DIR/o2.out" 2>"$TMP_DIR/o2.err"
                o2_rc=$?
                set -e
                if [ "$o2_rc" -ne "$ref_rc" ] || ! cmp -s "$TMP_DIR/ref.out" "$TMP_DIR/o2.out"; then
                    echo "FAIL: PPC32 interp -O0 vs -O2 mismatch on $f (o0=$ref_rc o2=$o2_rc)"
                    FAIL=$((FAIL + 1))
                    continue
                fi
            fi

            asm="$TMP_DIR/${base}_${opt#-}.s"
            obj="$TMP_DIR/${base}_${opt#-}.o"
            elf="$TMP_DIR/${base}_${opt#-}.elf"

            if ! "$BIN" --target=powerpc-unknown-elf "$opt" -S "$f" -o "$asm" 2>"$TMP_DIR/cg.err"; then
                echo "FAIL: PPC32 codegen ($opt) failed on $f:"
                cat "$TMP_DIR/cg.err"
                FAIL=$((FAIL + 1))
                continue
            fi

            if [ -n "$PPCCC" ]; then
                if ! "$PPCCC" --target=powerpc-unknown-elf -c "$asm" -o "$obj" 2>"$TMP_DIR/as.err"; then
                    echo "FAIL: PPC32 assemble ($opt) failed on $f:"
                    cat "$TMP_DIR/as.err"
                    FAIL=$((FAIL + 1))
                    continue
                fi
                if [ -n "$RT_OBJS" ]; then
                    if ! "$PPCLD" -m elf32ppc -T runtime/ppc32_bamboo.ld $RT_OBJS "$obj" -o "$elf" 2>"$TMP_DIR/ld.err"; then
                        echo "FAIL: PPC32 link ($opt) failed on $f:"
                        cat "$TMP_DIR/ld.err"
                        FAIL=$((FAIL + 1))
                        continue
                    fi
                    # Skip live QEMU only on x86_64-hardcoded 64-bit `size_t = i64` extern C libc
                    # declarations (which pass i64 (hi=0, lo=n) in r3:r4 on big-endian PPC32,
                    # mismatching 32-bit C `size_t` in r3).
                    if [ -n "$QEMU_PPC" ] && \
                       [ "$base" != "21_heap_libc_shims" ] && \
                       [ "$base" != "21_heap_malloc_realloc_free" ] && \
                       [ "$base" != "22_mem_and_str_shims" ]; then
                        rm -f "$TMP_DIR/ec.bin"
                        set +e
                        "$QEMU_PPC" -M bamboo -display none -no-reboot -bios none \
                            -serial stdio -serial "file:$TMP_DIR/ec.bin" \
                            -kernel "$elf" >"$TMP_DIR/qemu.out" 2>"$TMP_DIR/qemu.err"
                        set -e
                        if [ -s "$TMP_DIR/ec.bin" ]; then
                            q_rc="$(od -An -tu1 -N1 "$TMP_DIR/ec.bin" | tr -d ' \t\n')"
                        else
                            q_rc=255
                        fi
                        if [ "$q_rc" -ne "$ref_rc" ] || ! cmp -s "$TMP_DIR/ref.out" "$TMP_DIR/qemu.out"; then
                            echo "FAIL: QEMU PPC32 vs interp mismatch ($opt) on $f (ref=$ref_rc qemu=$q_rc)"
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

run_ppc32_suite "tests/ppc32_exec"
run_ppc32_suite "tests/interp_exec"
run_ppc32_suite "tests/hir_exec"
run_ppc32_suite "tests/hir_m4_exec"

# Backend stress tests (compile + assemble all)
echo "[PPC32 gate] Compiling & assembling tests/backend_stress/*.pir..."
for f in tests/backend_stress/*.pir; do
    base="$(basename "$f" .pir)"
    for opt in -O0 -O2; do
        asm="$TMP_DIR/bs_${base}_${opt#-}.s"
        obj="$TMP_DIR/bs_${base}_${opt#-}.o"
        if ! "$BIN" --target=powerpc-unknown-elf "$opt" -S "$f" -o "$asm" 2>"$TMP_DIR/cg.err"; then
            echo "FAIL: PPC32 codegen ($opt) failed on $f:"
            cat "$TMP_DIR/cg.err"
            FAIL=$((FAIL + 1))
            continue
        fi
        if [ -n "$PPCCC" ]; then
            if ! "$PPCCC" --target=powerpc-unknown-elf -c "$asm" -o "$obj" 2>"$TMP_DIR/as.err"; then
                echo "FAIL: PPC32 assemble ($opt) failed on $f:"
                cat "$TMP_DIR/as.err"
                FAIL=$((FAIL + 1))
                continue
            fi
        fi
        PASS=$((PASS + 1))
    done
done

# Check -c direct object compilation & Linux ELF snapshot
if [ -n "$PPCCC" ]; then
    echo "[PPC32 gate] Checking -c object compilation & Linux powerpc-unknown-linux-gnu ELF snapshot..."
    if PIRC_PPC32_CC="$PPCCC" "$BIN" --target=powerpc-unknown-elf -O2 -c tests/ppc32_exec/01_ppc32_big_endian_comprehensive.pir -o "$TMP_DIR/direct.o"; then
        PASS=$((PASS + 1))
    else
        echo "FAIL: -c direct object compilation for powerpc-unknown-elf failed"
        FAIL=$((FAIL + 1))
    fi

    LIN_ASM="$TMP_DIR/ppc32_linux.s"
    LIN_OBJ="$TMP_DIR/ppc32_linux.o"
    if "$BIN" --target=powerpc-unknown-linux-gnu -O2 -S tests/backend_stress/07_tls_and_pic_globals.pir -o "$LIN_ASM" && \
       grep -Fq ".globl main" "$LIN_ASM" && \
       grep -Fq "@tprel@ha" "$LIN_ASM" && \
       grep -Fq ".section .note.GNU-stack" "$LIN_ASM" && \
       "$PPCCC" --target=powerpc-unknown-linux-gnu -c "$LIN_ASM" -o "$LIN_OBJ"; then
        echo "  LINUX PPC32 ELF SNAPSHOT & ASSEMBLE PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL: Linux PPC32 ELF snapshot/assemble failed"
        FAIL=$((FAIL + 1))
    fi
fi

# Byte-identical determinism check
echo "[PPC32 gate] Checking byte-identical deterministic PPC32 assembly emission..."
"$BIN" --target=powerpc-unknown-elf -O2 -S tests/ppc32_exec/01_ppc32_big_endian_comprehensive.pir -o "$TMP_DIR/det1.s"
"$BIN" --target=powerpc-unknown-elf -O2 -S tests/ppc32_exec/01_ppc32_big_endian_comprehensive.pir -o "$TMP_DIR/det2.s"
if cmp -s "$TMP_DIR/det1.s" "$TMP_DIR/det2.s"; then
    echo "  DETERMINISM PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL: non-deterministic PPC32 assembly output"
    diff -u "$TMP_DIR/det1.s" "$TMP_DIR/det2.s"
    FAIL=$((FAIL + 1))
fi

echo "[PPC32 gate] Summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
