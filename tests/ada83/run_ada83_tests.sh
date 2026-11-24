#!/usr/bin/env bash
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TDIR="$ROOT/tests/ada83"
ADA_CC="$ROOT/frontends/ada83-pir/ada83-pir"
PIRC="${PIRC:-$ROOT/build/pirc}"
WITH_GNAT="${WITH_GNAT:-0}"
SDK15="/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/ada83_tests.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0

run_case() {
    local name="$1"
    shift
    local srcs=("$@")

    local ref_out=""
    if [ "$WITH_GNAT" = "1" ]; then
        local gdir="$TMP/gnat_${name}"
        mkdir -p "$gdir"
        local last_src=""
        for s in "${srcs[@]}"; do
            local b="$(basename "$s")"
            # GNAT expects .adb for body/main unless -gnatf; copy .ada -> <unit>.adb
            if [[ "$b" == *.ada ]]; then
                local unit
                unit="$(awk '/^procedure / {print tolower($2); exit}' "$s")"
                cp "$s" "$gdir/${unit}.adb"
                last_src="${unit}.adb"
            else
                cp "$s" "$gdir/$b"
                last_src="$b"
            fi
        done
        if ! (cd "$gdir" && gnatmake -q -gnat83 -O2 "$last_src" -o "$gdir/gnat_bin" -largs -B/usr/bin -Wl,-syslibroot,"$SDK15") 2>"$TMP/gnat.err"; then
            echo "FAIL [$name] GNAT compile error:"
            cat "$TMP/gnat.err"
            FAIL=$((FAIL + 1))
            return
        fi
        "$gdir/gnat_bin" >"$TMP/${name}_gnat.out"
        ref_out="$TMP/${name}_gnat.out"
    fi

    for cfg in "aarch64-apple-darwin:-O0" "aarch64-apple-darwin:-O2" "x86_64-apple-darwin:-O2"; do
        local tgt="${cfg%%:*}"
        local opt="${cfg##*:}"
        local bin="$TMP/${name}_${tgt}_${opt#-}"
        if ! "$ADA_CC" --pirc="$PIRC" --target="$tgt" "$opt" "${srcs[@]}" -o "$bin" 2>"$TMP/cc.err"; then
            echo "FAIL [$name $tgt $opt] compile failed:"
            cat "$TMP/cc.err"
            FAIL=$((FAIL + 1))
            continue
        fi
        local out="$TMP/${name}_${tgt}_${opt#-}.out"
        if [ "$tgt" = "x86_64-apple-darwin" ]; then
            if ! arch -x86_64 "$bin" >"$out" 2>"$TMP/run.err"; then
                echo "FAIL [$name $tgt $opt] runtime crash:"
                cat "$TMP/run.err"
                FAIL=$((FAIL + 1))
                continue
            fi
        else
            if ! "$bin" >"$out" 2>"$TMP/run.err"; then
                echo "FAIL [$name $tgt $opt] runtime crash:"
                cat "$TMP/run.err"
                FAIL=$((FAIL + 1))
                continue
            fi
        fi

        if [ -z "$ref_out" ]; then
            ref_out="$out"
            PASS=$((PASS + 1))
        elif ! cmp -s "$ref_out" "$out"; then
            echo "FAIL [$name $tgt $opt] output mismatch vs reference!"
            diff -u "$ref_out" "$out" || true
            FAIL=$((FAIL + 1))
        else
            PASS=$((PASS + 1))
        fi
    done
    echo "  PASS $name"
}

echo "=== Ada 83 -> DIANA -> PIR Conformance Suite (PIRC=$PIRC, WITH_GNAT=$WITH_GNAT) ==="
run_case "01_exc"     "$TDIR/01_checked_arith_and_exceptions.ada"
run_case "02_nested"  "$TDIR/02_nested_subprograms_static_link.ada"
run_case "03_arrays"  "$TDIR/03_arrays_slices_and_bounds.ada"
run_case "04_records" "$TDIR/04_records_variants_and_access.ada"
run_case "05_genctrl" "$TDIR/05_generics_and_control_flow.ada"
run_case "06_pkg"     "$TDIR/math_pkg.ads" "$TDIR/math_pkg.adb" "$TDIR/06_use_math.ada"
run_case "07_strings" "$TDIR/07_strings_and_fixed.ada"
run_case "08_stress"  "$TDIR/08_non_c_semantics_stress.ada"
run_case "09_retarr"  "$TDIR/09_returned_arrays_and_lifetimes.ada"

echo "=== Ada 83 Conformance Results: PASS=$PASS FAIL=$FAIL ==="
[ "$FAIL" -eq 0 ]
