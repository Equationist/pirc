#!/bin/sh
# ARM Thumb-2 (ARMv7E-M, Cortex-M7 FPv5-D16) Verification & Live QEMU MPS2-AN500 Differential Test Gate
set -u

PIRC="${1:-./build/pirc-san}"
TARGET="thumbv7em-none-eabihf"
TMP_DIR="$(mktemp -d /tmp/pirc_thumb2_XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

# Locate LLVM clang + lld + qemu-system-arm
T2_CC="${PIRC_THUMB2_CC:-}"
if [ -z "$T2_CC" ]; then
    for cand in /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang clang; do
        if command -v "$cand" >/dev/null 2>&1 || [ -x "$cand" ]; then
            T2_CC="$cand"
            break
        fi
    done
fi
T2_LD="${PIRC_THUMB2_LD:-}"
if [ -z "$T2_LD" ]; then
    for cand in /opt/homebrew/opt/lld/bin/ld.lld /opt/homebrew/opt/llvm/bin/ld.lld /usr/local/opt/llvm@17/bin/ld.lld /usr/local/opt/llvm/bin/ld.lld ld.lld; do
        if command -v "$cand" >/dev/null 2>&1 || [ -x "$cand" ]; then
            T2_LD="$cand"
            break
        fi
    done
fi
T2_QEMU="${PIRC_THUMB2_QEMU:-}"
if [ -z "$T2_QEMU" ]; then
    for cand in /opt/homebrew/bin/qemu-system-arm /usr/local/bin/qemu-system-arm qemu-system-arm; do
        if command -v "$cand" >/dev/null 2>&1 || [ -x "$cand" ]; then
            T2_QEMU="$cand"
            break
        fi
    done
fi

echo "[THUMB2 gate] Toolchain: CC=$T2_CC LD=${T2_LD:-none} QEMU=${T2_QEMU:-none}"

# Pre-compile bare-metal Cortex-M7 runtime objects (startup + freestanding helpers)
RT_S_OBJ="$TMP_DIR/pir_rt_thumb2_s.o"
RT_C_OBJ="$TMP_DIR/pir_rt_thumb2_c.o"
HAVE_EXEC=0
if [ -n "$T2_CC" ] && [ -n "$T2_LD" ] && [ -n "$T2_QEMU" ]; then
    if "$T2_CC" --target="$TARGET" -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard \
            -c runtime/pir_rt_thumb2.S -o "$RT_S_OBJ" 2>"$TMP_DIR/rt_err.txt" && \
       "$T2_CC" --target="$TARGET" -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard \
            -ffreestanding -fno-builtin -O2 -c runtime/pir_rt_thumb2.c -o "$RT_C_OBJ" 2>>"$TMP_DIR/rt_err.txt"; then
        HAVE_EXEC=1
    else
        echo "FAIL: failed to build Thumb-2 bare-metal runtime:"
        cat "$TMP_DIR/rt_err.txt"
        exit 1
    fi
fi

PASS=0
FAIL=0

run_qemu_elf() {
    elf="$1"
    out_file="$2"
    "$T2_QEMU" -M mps2-an500 -nographic -monitor none -serial none \
        -semihosting-config enable=on,target=native \
        -kernel "$elf" >"$out_file" 2>&1 &
    qpid=$!
    # 10s watchdog so a hang never stalls the gate
    (
        sleep 10
        kill -9 "$qpid" 2>/dev/null
    ) >/dev/null 2>&1 &
    wpid=$!
    wait "$qpid" 2>/dev/null
    qrc=$?
    kill "$wpid" 2>/dev/null
    wait "$wpid" 2>/dev/null
    return $qrc
}

check_file() {
    f="$1"
    base="$(basename "$f" .pir)"

    # Interpreter reference on Thumb-2 target (0 = pass, 132 = expected trap)
    "$PIRC" --target="$TARGET" --interp "$f" >"$TMP_DIR/ref.out" 2>"$TMP_DIR/ref.err"
    ref_rc=$?
    "$PIRC" --target="$TARGET" -O2 --interp "$f" >"$TMP_DIR/ref_o2.out" 2>"$TMP_DIR/ref_o2.err"
    ref_o2_rc=$?
    if [ "$ref_rc" -ne "$ref_o2_rc" ] || ! diff -q "$TMP_DIR/ref.out" "$TMP_DIR/ref_o2.out" >/dev/null 2>&1; then
        echo "FAIL: Thumb-2 interp -O0 vs -O2 mismatch on $f (o0=$ref_rc o2=$ref_o2_rc)"
        FAIL=$((FAIL + 1))
        return
    fi

    for opt in -O0 -O2; do
        sfile="$TMP_DIR/${base}_${opt}.s"
        ofile="$TMP_DIR/${base}_${opt}.o"
        efile="$TMP_DIR/${base}_${opt}.elf"
        if ! "$PIRC" --target="$TARGET" "$opt" -S "$f" -o "$sfile" 2>"$TMP_DIR/err.txt"; then
            echo "FAIL: Thumb-2 codegen ($opt) failed on $f:"
            cat "$TMP_DIR/err.txt"
            FAIL=$((FAIL + 1))
            continue
        fi
        if ! "$T2_CC" --target="$TARGET" -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard \
                -c "$sfile" -o "$ofile" 2>"$TMP_DIR/err.txt"; then
            echo "FAIL: Thumb-2 assemble ($opt) failed on $f:"
            cat "$TMP_DIR/err.txt"
            FAIL=$((FAIL + 1))
            continue
        fi
        if [ "$HAVE_EXEC" -eq 1 ]; then
            if ! "$T2_LD" -T runtime/thumb2_mps2_an500.ld "$RT_S_OBJ" "$ofile" "$RT_C_OBJ" \
                    -o "$efile" 2>"$TMP_DIR/err.txt"; then
                echo "FAIL: Thumb-2 link ($opt) failed on $f:"
                cat "$TMP_DIR/err.txt"
                FAIL=$((FAIL + 1))
                continue
            fi
            run_qemu_elf "$efile" "$TMP_DIR/qemu.out"
            qrc=$?
            if [ "$qrc" -ne "$ref_rc" ] || ! diff -q "$TMP_DIR/ref.out" "$TMP_DIR/qemu.out" >/dev/null 2>&1; then
                echo "FAIL: Thumb-2 QEMU mismatch ($opt) on $f (ref=$ref_rc qemu=$qrc)"
                diff -u "$TMP_DIR/ref.out" "$TMP_DIR/qemu.out" | head -n 20
                FAIL=$((FAIL + 1))
                continue
            fi
        fi
        PASS=$((PASS + 1))
    done
}

echo "[THUMB2 gate] Compiling & verifying tests/thumb2_exec/*.pir..."
for f in tests/thumb2_exec/[0-9]*.pir; do
    [ -f "$f" ] && check_file "$f"
done

if [ "$HAVE_EXEC" -eq 1 ]; then
    echo "[THUMB2 gate] Checking bidirectional C <-> PIR AAPCS32-VFP ABI conformance (clang -O2 vs pirc)..."
    "$T2_CC" --target="$TARGET" -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard \
        -ffreestanding -fno-builtin -O2 -c tests/thumb2_exec/c_abi_check.c -o "$TMP_DIR/c_abi_c.o"
    for opt in -O0 -O2; do
        if "$PIRC" --target="$TARGET" "$opt" -S tests/thumb2_exec/c_abi_check.pir -o "$TMP_DIR/c_abi_pir_${opt}.s" && \
           "$T2_CC" --target="$TARGET" -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard \
                -c "$TMP_DIR/c_abi_pir_${opt}.s" -o "$TMP_DIR/c_abi_pir_${opt}.o" && \
           "$T2_LD" -T runtime/thumb2_mps2_an500.ld "$RT_S_OBJ" "$TMP_DIR/c_abi_c.o" \
                "$TMP_DIR/c_abi_pir_${opt}.o" "$RT_C_OBJ" -o "$TMP_DIR/c_abi_${opt}.elf"; then
            run_qemu_elf "$TMP_DIR/c_abi_${opt}.elf" "$TMP_DIR/c_abi.out"
            qrc=$?
            if [ "$qrc" -eq 0 ]; then
                echo "  AAPCS32-VFP C<->PIR ($opt) PASS"
                PASS=$((PASS + 1))
            else
                echo "FAIL: AAPCS32-VFP C<->PIR ($opt) exited with code $qrc"
                FAIL=$((FAIL + 1))
            fi
        else
            echo "FAIL: AAPCS32-VFP C<->PIR ($opt) build/link failed"
            FAIL=$((FAIL + 1))
        fi
    done
fi

echo "[THUMB2 gate] Compiling & verifying tests/interp_exec/*.pir..."
for f in tests/interp_exec/*.pir; do
    [ -f "$f" ] && check_file "$f"
done

echo "[THUMB2 gate] Compiling & verifying tests/hir_exec/*.pir..."
for f in tests/hir_exec/*.pir; do
    [ -f "$f" ] && check_file "$f"
done

echo "[THUMB2 gate] Compiling & verifying tests/hir_m4_exec/*.pir..."
for f in tests/hir_m4_exec/*.pir; do
    [ -f "$f" ] && check_file "$f"
done

echo "[THUMB2 gate] Compiling & assembling tests/backend_stress/*.pir..."
for f in tests/backend_stress/*.pir; do
    [ -f "$f" ] || continue
    base="$(basename "$f" .pir)"
    for opt in -O0 -O2; do
        sfile="$TMP_DIR/st_${base}_${opt}.s"
        ofile="$TMP_DIR/st_${base}_${opt}.o"
        if ! "$PIRC" --target="$TARGET" "$opt" -S "$f" -o "$sfile" 2>"$TMP_DIR/err.txt"; then
            echo "FAIL: Thumb-2 stress codegen ($opt) failed on $f:"
            cat "$TMP_DIR/err.txt"
            FAIL=$((FAIL + 1))
            continue
        fi
        if ! "$T2_CC" --target="$TARGET" -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard \
                -c "$sfile" -o "$ofile" 2>"$TMP_DIR/err.txt"; then
            echo "FAIL: Thumb-2 stress assemble ($opt) failed on $f:"
            cat "$TMP_DIR/err.txt"
            FAIL=$((FAIL + 1))
            continue
        fi
        PASS=$((PASS + 1))
    done
done

echo "[THUMB2 gate] Checking -c direct object compilation & soft-float thumbv7m-none-eabi..."
if "$PIRC" --target="$TARGET" -O2 -c tests/thumb2_exec/01_thumb2_aapcs_vfp_comprehensive.pir -o "$TMP_DIR/direct.o" 2>"$TMP_DIR/err.txt" && \
   [ -s "$TMP_DIR/direct.o" ]; then
    PASS=$((PASS + 1))
else
    echo "FAIL: -c direct object compilation for $TARGET failed:"
    cat "$TMP_DIR/err.txt"
    FAIL=$((FAIL + 1))
fi

if "$PIRC" --target=thumbv7m-none-eabi -O2 -c tests/thumb2_exec/01_thumb2_aapcs_vfp_comprehensive.pir -o "$TMP_DIR/soft.o" 2>"$TMP_DIR/err.txt" && \
   [ -s "$TMP_DIR/soft.o" ]; then
    echo "  SOFT-FLOAT thumbv7m-none-eabi (-c) PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL: soft-float thumbv7m-none-eabi -c failed:"
    cat "$TMP_DIR/err.txt"
    FAIL=$((FAIL + 1))
fi

echo "[THUMB2 gate] Checking byte-identical deterministic Thumb-2 assembly emission..."
"$PIRC" --target="$TARGET" -O2 -S tests/thumb2_exec/01_thumb2_aapcs_vfp_comprehensive.pir -o "$TMP_DIR/det1.s"
"$PIRC" --target="$TARGET" -O2 -S tests/thumb2_exec/01_thumb2_aapcs_vfp_comprehensive.pir -o "$TMP_DIR/det2.s"
if diff -u "$TMP_DIR/det1.s" "$TMP_DIR/det2.s"; then
    echo "  DETERMINISM PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL: non-deterministic Thumb-2 assembly output"
    FAIL=$((FAIL + 1))
fi

echo "[THUMB2 gate] Summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
