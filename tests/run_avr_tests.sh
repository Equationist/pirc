#!/bin/sh
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause
#
# AVR 8-bit / 16-bit-pointer (ATmega1280, avr-unknown-elf) backend verification gate:
#   1. Differential LIR interpreter oracle (avr-unknown-elf -O0 vs -O2)
#   2. Native AVR assembly emission, LLVM-MC assembling (.o), & ld.lld ELF linking
#   3. Byte-identical determinism & -c direct object compilation
#   4. Full QEMU ATmega1280 (-M mega) bare-metal differential execution

set -eu

BIN="${1:-./build/pirc-san}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/pirc_avr.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

PASS=0
FAIL=0

# Locate AVR-capable clang assembler/compiler
AVRCC="${PIRC_AVR_CC:-}"
if [ -z "$AVRCC" ]; then
    for cand in /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang /usr/local/opt/llvm@17/bin/clang clang; do
        if command -v "$cand" >/dev/null 2>&1 && \
           printf '.text\nret\n' | "$cand" --target=avr -mmcu=atmega1280 -x assembler -c - -o "$TMP_DIR/probe.o" 2>/dev/null; then
            AVRCC="$cand"
            break
        fi
    done
fi

# Locate ld.lld for bare-metal AVR ELF linking
AVRLD="${PIRC_AVR_LD:-}"
if [ -z "$AVRLD" ]; then
    for cand in /opt/homebrew/opt/llvm/bin/ld.lld /opt/homebrew/opt/lld/bin/ld.lld /usr/local/opt/llvm/bin/ld.lld /usr/local/opt/llvm@17/bin/ld.lld ld.lld; do
        if command -v "$cand" >/dev/null 2>&1; then
            AVRLD="$cand"
            break
        fi
    done
fi

# Locate qemu-system-avr (optional live execution)
QEMU_AVR="${QEMU_AVR:-}"
if [ -z "$QEMU_AVR" ]; then
    for cand in qemu-system-avr /opt/homebrew/bin/qemu-system-avr /usr/local/bin/qemu-system-avr; do
        if command -v "$cand" >/dev/null 2>&1; then
            QEMU_AVR="$cand"
            break
        fi
    done
fi

echo "[AVR gate] Toolchain: CC=${AVRCC:-none} LD=${AVRLD:-none} QEMU=${QEMU_AVR:-none}"

# Build bare-metal ATmega1280 runtime objects (3 tiny startup variants + shared C runtime)
RT_OK=0
if [ -n "$AVRCC" ] && [ -n "$AVRLD" ]; then
    if "$AVRCC" --target=avr -mmcu=atmega1280 -c runtime/pir_rt_avr.S -o "$TMP_DIR/rt_s_i16.o" && \
       "$AVRCC" --target=avr -mmcu=atmega1280 -DPIR_AVR_MAIN_I32 -c runtime/pir_rt_avr.S -o "$TMP_DIR/rt_s_i32.o" && \
       "$AVRCC" --target=avr -mmcu=atmega1280 -DPIR_AVR_MAIN_I32_SRET -c runtime/pir_rt_avr.S -o "$TMP_DIR/rt_s_sret.o" && \
       "$AVRCC" --target=avr -mmcu=atmega1280 -mdouble=64 -ffreestanding -fno-builtin -O2 -c runtime/pir_rt_avr.c -o "$TMP_DIR/rt_c.o"; then
        RT_OK=1
    else
        echo "FAIL: failed to compile AVR bare-metal runtime"
        FAIL=$((FAIL + 1))
    fi
fi

# Helper: run an ATmega1280 ELF on QEMU -M mega, stopping immediately when USART1 emits 1-byte exit code
run_qemu_avr() {
    elf_path="$1"
    out_path="$2"
    ec_path="$3"
    python3 - "$QEMU_AVR" "$elf_path" "$out_path" "$ec_path" <<'PY'
import os, subprocess, sys, time
qemu, elf, out_f, ec_f = sys.argv[1:5]
for f in (out_f, ec_f):
    if os.path.exists(f):
        os.remove(f)
p = subprocess.Popen(
    [qemu, "-M", "mega", "-display", "none", "-no-reboot",
     "-serial", "file:" + out_f, "-serial", "file:" + ec_f, "-bios", elf],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
)
t0 = time.time()
ec = 255
while time.time() - t0 < 8.0:
    if os.path.exists(ec_f) and os.path.getsize(ec_f) > 0:
        time.sleep(0.008)
        with open(ec_f, "rb") as fh:
            ec = fh.read(1)[0]
        break
    time.sleep(0.004)
p.kill()
p.wait()
sys.exit(ec)
PY
}

select_rt_start() {
    src_file="$1"
    if grep -qE 'func[[:space:]]+@main\([^)]*\)[[:space:]]*->[[:space:]]*i32.*cc[[:space:]]+"c"' "$src_file"; then
        echo "$TMP_DIR/rt_s_i32.o"
    elif grep -qE 'func[[:space:]]+@main\([^)]*\)[[:space:]]*->[[:space:]]*i32' "$src_file"; then
        echo "$TMP_DIR/rt_s_sret.o"
    else
        echo "$TMP_DIR/rt_s_i16.o"
    fi
}

run_avr_suite() {
    dir="$1"
    echo "[AVR gate] Compiling & verifying $dir/*.pir..."
    for f in "$dir"/*.pir; do
        base="$(basename "$f" .pir)"

        # Reference interpreter run on avr-unknown-elf target (-O0)
        set +e
        "$BIN" --interp --target=avr-unknown-elf -O0 "$f" >"$TMP_DIR/ref.out" 2>"$TMP_DIR/ref.err"
        ref_rc=$?
        set -e

        rt_start="$(select_rt_start "$f")"

        for opt in -O0 -O2; do
            # Check -O2 interpreter parity against -O0
            if [ "$opt" = "-O2" ]; then
                set +e
                "$BIN" --interp --target=avr-unknown-elf -O2 "$f" >"$TMP_DIR/o2.out" 2>"$TMP_DIR/o2.err"
                o2_rc=$?
                set -e
                if [ "$o2_rc" -ne "$ref_rc" ] || ! cmp -s "$TMP_DIR/ref.out" "$TMP_DIR/o2.out"; then
                    echo "FAIL: AVR interp -O0 vs -O2 mismatch on $f (o0=$ref_rc o2=$o2_rc)"
                    FAIL=$((FAIL + 1))
                    continue
                fi
            fi

            asm="$TMP_DIR/${base}_${opt#-}.s"
            obj="$TMP_DIR/${base}_${opt#-}.o"
            elf="$TMP_DIR/${base}_${opt#-}.elf"

            if ! "$BIN" --target=avr-unknown-elf "$opt" -S "$f" -o "$asm" 2>"$TMP_DIR/cg.err"; then
                echo "FAIL: AVR codegen ($opt) failed on $f:"
                cat "$TMP_DIR/cg.err"
                FAIL=$((FAIL + 1))
                continue
            fi

            if [ -n "$AVRCC" ]; then
                if ! "$AVRCC" --target=avr -mmcu=atmega1280 -c "$asm" -o "$obj" 2>"$TMP_DIR/as.err"; then
                    echo "FAIL: AVR assemble ($opt) failed on $f:"
                    cat "$TMP_DIR/as.err"
                    FAIL=$((FAIL + 1))
                    continue
                fi
                if [ "$RT_OK" -eq 1 ]; then
                    if ! "$AVRLD" -T runtime/avr_mega1280.ld "$rt_start" "$TMP_DIR/rt_c.o" "$obj" -o "$elf" 2>"$TMP_DIR/ld.err"; then
                        echo "FAIL: AVR link ($opt) failed on $f:"
                        cat "$TMP_DIR/ld.err"
                        FAIL=$((FAIL + 1))
                        continue
                    fi
                    # Skip live QEMU only on tests with x86_64-hardcoded 64-bit `size_t = i64`
                    # extern C libc declarations (AVR C `size_t` is 16-bit!) or >8KB SRAM allocas.
                    if [ -n "$QEMU_AVR" ] && \
                       [ "$base" != "21_heap_libc_shims" ] && \
                       [ "$base" != "21_heap_malloc_realloc_free" ] && \
                       [ "$base" != "22_mem_and_str_shims" ]; then
                        set +e
                        run_qemu_avr "$elf" "$TMP_DIR/qemu.out" "$TMP_DIR/ec.bin"
                        q_rc=$?
                        set -e
                        if [ "$q_rc" -ne "$ref_rc" ] || ! cmp -s "$TMP_DIR/ref.out" "$TMP_DIR/qemu.out"; then
                            echo "FAIL: QEMU AVR vs interp mismatch ($opt) on $f (ref=$ref_rc qemu=$q_rc)"
                            echo "  ref_out:  $(cat "$TMP_DIR/ref.out")"
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

run_avr_suite "tests/avr_exec"
run_avr_suite "tests/interp_exec"
run_avr_suite "tests/hir_exec"
run_avr_suite "tests/hir_m4_exec"

# Backend stress tests (compile + assemble all; skip 64-bit-only >32KiB/large-alloca if any)
echo "[AVR gate] Compiling & assembling tests/backend_stress/*.pir..."
for f in tests/backend_stress/*.pir; do
    base="$(basename "$f" .pir)"
    for opt in -O0 -O2; do
        asm="$TMP_DIR/bs_${base}_${opt#-}.s"
        obj="$TMP_DIR/bs_${base}_${opt#-}.o"
        if ! "$BIN" --target=avr-unknown-elf "$opt" -S "$f" -o "$asm" 2>"$TMP_DIR/cg.err"; then
            echo "FAIL: AVR codegen ($opt) failed on $f:"
            cat "$TMP_DIR/cg.err"
            FAIL=$((FAIL + 1))
            continue
        fi
        if [ -n "$AVRCC" ]; then
            if ! "$AVRCC" --target=avr -mmcu=atmega1280 -c "$asm" -o "$obj" 2>"$TMP_DIR/as.err"; then
                echo "FAIL: AVR assemble ($opt) failed on $f:"
                cat "$TMP_DIR/as.err"
                FAIL=$((FAIL + 1))
                continue
            fi
        fi
        PASS=$((PASS + 1))
    done
done

# Check -c direct object compilation
if [ -n "$AVRCC" ]; then
    echo "[AVR gate] Checking -c direct object compilation for avr-unknown-elf..."
    if PIRC_AVR_CC="$AVRCC" "$BIN" --target=avr-unknown-elf -O2 -c tests/avr_exec/01_avr_16bit_comprehensive.pir -o "$TMP_DIR/direct.o"; then
        PASS=$((PASS + 1))
    else
        echo "FAIL: -c direct object compilation for avr-unknown-elf failed"
        FAIL=$((FAIL + 1))
    fi
fi

# Byte-identical determinism check
echo "[AVR gate] Checking byte-identical deterministic AVR assembly emission..."
"$BIN" --target=avr-unknown-elf -O2 -S tests/avr_exec/01_avr_16bit_comprehensive.pir -o "$TMP_DIR/det1.s"
"$BIN" --target=avr-unknown-elf -O2 -S tests/avr_exec/01_avr_16bit_comprehensive.pir -o "$TMP_DIR/det2.s"
if cmp -s "$TMP_DIR/det1.s" "$TMP_DIR/det2.s"; then
    echo "  DETERMINISM PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL: non-deterministic AVR assembly output"
    diff -u "$TMP_DIR/det1.s" "$TMP_DIR/det2.s"
    FAIL=$((FAIL + 1))
fi

echo "[AVR gate] Summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
