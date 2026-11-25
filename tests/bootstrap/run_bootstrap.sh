#!/bin/sh
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

# Milestone M7.5: 3-Stage Self-Hosting Bootstrap Verification Gate (aarch64-apple-darwin)
#
#   Stage 0: Host CC builds pirc-stage0 and slimcc-pir-stage0.
#   Stage 1: slimcc-pir-stage0 -> pirc-stage0 -O2 compiles pirc & slimcc-pir
#            into pirc-stage1 and slimcc-pir-stage1.
#   Stage 2: slimcc-pir-stage1 -> pirc-stage1 -O2 emits stage2_asm/*.s for all
#            pirc sources and links pirc-stage2.
#   Stage 3: slimcc-pir-stage1 -> pirc-stage2 -O2 emits stage3_asm/*.s for all
#            pirc sources.
#   Gate 1:  diff -ru stage2_asm/ stage3_asm/ MUST be empty (byte-identical).
#   Gate 2:  pirc-stage2 MUST pass the full pirc test suite (M1-M7).
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CC="${CC:-cc}"
BOOT_DIR="${BOOT_DIR:-$ROOT/build/bootstrap}"

PIRC_SRCS="
src/main.c
src/common/arena.c
src/common/diag.c
src/target/target.c
src/target/abi_sysv_x64.c
src/target/abi_aapcs64.c
src/target/abi_win64.c
src/analysis/cfg.c
src/analysis/dom.c
src/analysis/loops.c
src/analysis/alias.c
src/ir/lexer.c
src/ir/parser.c
src/ir/printer.c
src/ir/verify.c
src/lower/lower.c
src/lower/exceptions.c
src/lower/varargs.c
src/lower/cabi.c
src/lower/aggregates.c
src/lower/i128.c
src/lower/simd.c
src/lower/legalize.c
src/opt/opt.c
src/opt/inline.c
src/opt/simplify_cfg.c
src/opt/mem2reg.c
src/opt/gvn.c
src/opt/licm.c
src/opt/dse.c
src/opt/dce.c
src/opt/loop_vec.c
src/interp/interp.c
src/interp/runtime_shims.c
src/backend/mir.c
src/backend/regalloc.c
src/backend/arm64/arm64_isel.c
src/backend/arm64/arm64_frame.c
src/backend/arm64/arm64_asm.c
src/backend/x64/x64_isel.c
src/backend/x64/x64_frame.c
src/backend/x64/x64_asm.c
"

SLIMCC_SRCS="
alloc.c
bitint.c
codegen_pir.c
hashmap.c
main.c
parse.c
platform/macos-arm64-pir.c
preprocess.c
strings.c
tokenize.c
type.c
unicode.c
"

rm -rf "$BOOT_DIR"
mkdir -p "$BOOT_DIR/stage1_pirc_obj" \
         "$BOOT_DIR/stage1_slimcc_obj" \
         "$BOOT_DIR/stage2_asm" \
         "$BOOT_DIR/stage2_obj" \
         "$BOOT_DIR/stage3_asm"

cd "$ROOT"

echo "=== [Bootstrap Stage 0] Building pirc-stage0 and slimcc-pir-stage0 with host $CC ==="
make build/pirc >/dev/null
make -C frontends/slimcc-pir -f Makefile.pir >/dev/null
cp build/pirc "$BOOT_DIR/pirc-stage0"
cp frontends/slimcc-pir/slimcc-pir "$BOOT_DIR/slimcc-pir-stage0"

echo "=== [Bootstrap Stage 1] Compiling pirc (37 TUs) with slimcc-pir-stage0 -> pirc-stage0 -O2 ==="
for f in $PIRC_SRCS; do
    tag="$(printf '%s' "$f" | tr '/' '_' | sed 's/\.c$//')"
    "$BOOT_DIR/slimcc-pir-stage0" --pirc="$BOOT_DIR/pirc-stage0" -O2 \
        -Isrc -D_POSIX_C_SOURCE=200809L -DBOOTSTRAP_NO_LDOUBLE \
        -c "$f" -o "$BOOT_DIR/stage1_pirc_obj/${tag}.o"
done
"$CC" "$BOOT_DIR"/stage1_pirc_obj/*.o -o "$BOOT_DIR/pirc-stage1"
"$BOOT_DIR/pirc-stage1" --version

echo "=== [Bootstrap Stage 1] Compiling slimcc-pir (12 TUs) with slimcc-pir-stage0 -> pirc-stage0 -O2 ==="
for f in $SLIMCC_SRCS; do
    tag="$(printf '%s' "$f" | tr '/' '_' | sed 's/\.c$//')"
    "$BOOT_DIR/slimcc-pir-stage0" --pirc="$BOOT_DIR/pirc-stage0" -O2 \
        -Ifrontends/slimcc-pir -DSLIMCC_PIR -DBOOTSTRAP_NO_LDOUBLE \
        -c "frontends/slimcc-pir/$f" -o "$BOOT_DIR/stage1_slimcc_obj/${tag}.o"
done
"$CC" "$BOOT_DIR"/stage1_slimcc_obj/*.o -o "$BOOT_DIR/slimcc-pir-stage1"

echo "=== [Bootstrap Stage 2] Emitting stage2_asm/ & linking pirc-stage2 via slimcc-pir-stage1 -> pirc-stage1 -O2 ==="
for f in $PIRC_SRCS; do
    tag="$(printf '%s' "$f" | tr '/' '_' | sed 's/\.c$//')"
    "$BOOT_DIR/slimcc-pir-stage1" --pirc="$BOOT_DIR/pirc-stage1" -O2 \
        -Isrc -D_POSIX_C_SOURCE=200809L -DBOOTSTRAP_NO_LDOUBLE \
        -S "$f" -o "$BOOT_DIR/stage2_asm/${tag}.s"
    "$CC" -c "$BOOT_DIR/stage2_asm/${tag}.s" -o "$BOOT_DIR/stage2_obj/${tag}.o"
done
"$CC" "$BOOT_DIR"/stage2_obj/*.o -o "$BOOT_DIR/pirc-stage2"
"$BOOT_DIR/pirc-stage2" --version

echo "=== [Bootstrap Stage 3] Emitting stage3_asm/ via slimcc-pir-stage1 -> pirc-stage2 -O2 ==="
for f in $PIRC_SRCS; do
    tag="$(printf '%s' "$f" | tr '/' '_' | sed 's/\.c$//')"
    "$BOOT_DIR/slimcc-pir-stage1" --pirc="$BOOT_DIR/pirc-stage2" -O2 \
        -Isrc -D_POSIX_C_SOURCE=200809L -DBOOTSTRAP_NO_LDOUBLE \
        -S "$f" -o "$BOOT_DIR/stage3_asm/${tag}.s"
done

echo "=== [Bootstrap Gate 1] Comparing stage2_asm/ vs stage3_asm/ (fixed-point check) ==="
diff -ru "$BOOT_DIR/stage2_asm" "$BOOT_DIR/stage3_asm"
echo "PASS: stage2_asm/ and stage3_asm/ are 100% byte-identical across all 37 translation units!"

echo "=== [Bootstrap Gate 2] Running full pirc test suite (M1-M7) with pirc-stage2 ==="
sh tests/run_interp_tests.sh "$BOOT_DIR/pirc-stage2"
sh tests/run_m3_tests.sh "$BOOT_DIR/pirc-stage2"
sh tests/run_m4_tests.sh "$BOOT_DIR/pirc-stage2"
sh tests/run_m5_tests.sh "$BOOT_DIR/pirc-stage2"
sh tests/run_m6_tests.sh "$BOOT_DIR/pirc-stage2"
PIRC="$BOOT_DIR/pirc-stage2" bash tests/c_abi_conformance/run_c_abi.sh

echo ""
echo "===================================================================="
echo "ALL 3-STAGE SELF-HOSTING BOOTSTRAP GATES PASSED (BYTE-IDENTICAL)!"
echo "===================================================================="
