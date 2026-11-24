#!/usr/bin/env bash
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
SUITE_DIR="$ROOT_DIR/tests/c_abi_conformance"
PIRC="${PIRC:-$ROOT_DIR/build/pirc}"
CC="${CC:-clang}"
TARGET="${TARGET:-aarch64-apple-darwin}"
if [ "$TARGET" = "x86_64-apple-darwin" ]; then
    BUILD_DIR="$ROOT_DIR/build/m8_c_abi_x64"
    ARCH_FLAGS="-arch x86_64"
else
    BUILD_DIR="$ROOT_DIR/build/m7_c_abi"
    ARCH_FLAGS=""
fi

mkdir -p "$BUILD_DIR"

if [ ! -x "$PIRC" ]; then
    echo "Error: pirc binary not found at $PIRC" >&2
    exit 1
fi

SUITES=(
    01_structs_1_to_64b
    02_hfa_and_hva_structs
    03_mixed_packed_unions
    04_i128_and_simd_v128
    05_darwin_stack_varargs
    06_darwin_stack_packing
    07_fn_ptrs_and_callbacks
    08_weak_sections_and_tls
)

passed=0
total=0

for opt in -O0 -O2; do
    echo "=== Running M7 C ABI Conformance ($TARGET, pirc $opt vs $CC -O2) ==="
    for suite in "${SUITES[@]}"; do
        total=$((total + 1))
        sdir="$SUITE_DIR/$suite"
        asm="$BUILD_DIR/${suite}_${opt#-}.s"
        bin="$BUILD_DIR/${suite}_${opt#-}"
        extra_ldflags=""
        if [ "$suite" = "08_weak_sections_and_tls" ]; then
            extra_ldflags="-lpthread"
        fi

        echo -n "  [$opt] $suite ... "
        "$PIRC" --target "$TARGET" "$opt" --emit-asm "$sdir/pir_side.pir" -o "$asm"
        "$CC" $ARCH_FLAGS -std=c99 -O2 -Wall -Wextra -Werror -I"$SUITE_DIR" \
            "$sdir/c_side.c" "$asm" -o "$bin" $extra_ldflags
        if [ "$TARGET" = "x86_64-apple-darwin" ]; then
            arch -x86_64 "$bin"
        else
            "$bin"
        fi
        passed=$((passed + 1))
    done
done

echo ""
echo "ALL M7 C ABI CONFORMANCE TESTS PASSED ($passed/$total)."
