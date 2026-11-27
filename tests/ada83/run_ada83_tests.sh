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

run_neg_case() {
    local name="$1"
    local src="$2"
    local bin="$TMP/neg_${name}"
    local err="$TMP/neg_${name}.err"
    local rc=0
    "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O0 "$src" -o "$bin" >"$err" 2>&1 || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "FAIL [neg:$name] unexpectedly compiled cleanly!"
        FAIL=$((FAIL + 1))
        return
    fi
    if [ "$rc" -ge 128 ] || grep -q "ABORTING\|unhandled exception\|PROGRAM_ERROR\|CONSTRAINT_ERROR\|ASSERTION_ERROR" "$err"; then
        echo "FAIL [neg:$name] compiler crashed/ICE (rc=$rc):"
        cat "$err"
        FAIL=$((FAIL + 1))
        return
    fi
    PASS=$((PASS + 1))
}

run_multi_sep_case() {
    local name="multi_sep"
    for cfg in "aarch64-apple-darwin:-O0" "aarch64-apple-darwin:-O2" "x86_64-apple-darwin:-O2"; do
        local tgt="${cfg%%:*}"
        local opt="${cfg##*:}"
        local ldir="$TMP/${name}_lib_${tgt}_${opt#-}"
        local bin="$TMP/${name}_${tgt}_${opt#-}"
        local out="$TMP/${name}_${tgt}_${opt#-}.out"
        mkdir -p "$ldir"
        if ! "$ADA_CC" --pirc="$PIRC" --target="$tgt" "$opt" -L "$ldir" -c "$TDIR/multi/multi_sep_pkg.ads" 2>"$TMP/cc.err" ||
           ! "$ADA_CC" --pirc="$PIRC" --target="$tgt" "$opt" -L "$ldir" -c "$TDIR/multi/multi_sep_pkg.adb" 2>"$TMP/cc.err" ||
           ! "$ADA_CC" --pirc="$PIRC" --target="$tgt" "$opt" -L "$ldir" -c "$TDIR/multi/multi_sep_subunits.adb" 2>"$TMP/cc.err" ||
           ! "$ADA_CC" --pirc="$PIRC" --target="$tgt" "$opt" -L "$ldir" -c "$TDIR/multi/multi_sep_main.adb" 2>"$TMP/cc.err" ||
           ! "$ADA_CC" --pirc="$PIRC" --target="$tgt" "$opt" -L "$ldir" --bind multi_sep_main -o "$bin" 2>"$TMP/cc.err"; then
            echo "FAIL [$name $tgt $opt] compile/bind failed:"
            cat "$TMP/cc.err"
            FAIL=$((FAIL + 1))
            continue
        fi
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
        if ! grep -qx "MULTI_SEP_OK" "$out"; then
            echo "FAIL [$name $tgt $opt] unexpected output:"
            cat "$out"
            FAIL=$((FAIL + 1))
        else
            PASS=$((PASS + 1))
        fi
    done

    # Also test binder obsolescence rejection after spec recompilation (P3-5.13/14)
    local odir="$TMP/multi_obs_lib"
    mkdir -p "$odir"
    if "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O0 -L "$odir" -c "$TDIR/multi/multi_sep_pkg.ads" 2>/dev/null &&
       "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O0 -L "$odir" -c "$TDIR/multi/multi_sep_pkg.adb" 2>/dev/null &&
       "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O0 -L "$odir" -c "$TDIR/multi/multi_sep_subunits.adb" 2>/dev/null &&
       "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O0 -L "$odir" -c "$TDIR/multi/multi_sep_main.adb" 2>/dev/null &&
       "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O0 -L "$odir" -c "$TDIR/multi/multi_sep_pkg.ads" 2>/dev/null; then
        if "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O0 -L "$odir" --bind multi_sep_main -o "$TMP/obs_bin" 2>/dev/null; then
            echo "FAIL [multi_sep_obsolete] binder failed to reject obsolete client after spec recompile!"
            FAIL=$((FAIL + 1))
        else
            PASS=$((PASS + 1))
        fi
    else
        echo "FAIL [multi_sep_obsolete] setup failed"
        FAIL=$((FAIL + 1))
    fi
    echo "  PASS $name"
}

run_c_iface_case() {
    local name="22_c_iface"
    for cfg in "aarch64-apple-darwin:-O0" "aarch64-apple-darwin:-O2" "x86_64-apple-darwin:-O2"; do
        local tgt="${cfg%%:*}"
        local opt="${cfg##*:}"
        local arch_flag="arm64"
        if [ "$tgt" = "x86_64-apple-darwin" ]; then arch_flag="x86_64"; fi
        local cobj="$TMP/c_iface_${arch_flag}.o"
        local bin="$TMP/${name}_${tgt}_${opt#-}"
        local out="$TMP/${name}_${tgt}_${opt#-}.out"
        if ! cc -std=c99 -O2 -arch "$arch_flag" -c "$TDIR/c_iface_helper.c" -o "$cobj" 2>"$TMP/cc.err" ||
           ! "$ADA_CC" --pirc="$PIRC" --target="$tgt" "$opt" "$TDIR/22_c_interface_test.ada" "$cobj" -o "$bin" 2>"$TMP/cc.err"; then
            echo "FAIL [$name $tgt $opt] compile/link failed:"
            cat "$TMP/cc.err"
            FAIL=$((FAIL + 1))
            continue
        fi
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
        if ! grep -qx "C_INTERFACE_OK" "$out"; then
            echo "FAIL [$name $tgt $opt] unexpected output:"
            cat "$out"
            FAIL=$((FAIL + 1))
        else
            PASS=$((PASS + 1))
        fi
    done
    echo "  PASS $name"
}

run_p4_driver_case() {
    # P4-2: Test driver CLI hardening & atomic -L rollback on failed compilation
    if "$ADA_CC" --help >/dev/null 2>&1; then
        PASS=$((PASS + 1))
    else
        echo "FAIL [p4_driver_help] --help did not exit 0"
        FAIL=$((FAIL + 1))
    fi
    if "$ADA_CC" -o >/dev/null 2>&1; then
        echo "FAIL [p4_driver_missing_arg] -o with missing arg did not fail"
        FAIL=$((FAIL + 1))
    else
        PASS=$((PASS + 1))
    fi
    local rdir="$TMP/p4_rollback_lib"
    mkdir -p "$rdir"
    if "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O0 -L "$rdir" -c "$TDIR/multi/multi_sep_pkg.ads" 2>/dev/null &&
       "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O0 -L "$rdir" -c "$TDIR/multi/multi_sep_pkg.adb" 2>/dev/null &&
       "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O0 -L "$rdir" -c "$TDIR/multi/multi_sep_subunits.adb" 2>/dev/null &&
       "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O0 -L "$rdir" -c "$TDIR/multi/multi_sep_main.adb" 2>/dev/null; then
        "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O0 -L "$rdir" -c "$TDIR/neg/n53_multi_error_sigpipe.ada" >/dev/null 2>&1 || true
        if "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O0 -L "$rdir" --bind multi_sep_main -o "$TMP/rb_bin" 2>/dev/null &&
           "$TMP/rb_bin" | grep -qx "MULTI_SEP_OK"; then
            PASS=$((PASS + 1))
        else
            echo "FAIL [p4_lib_rollback] failed compile corrupted persistent -L library!"
            FAIL=$((FAIL + 1))
        fi
    else
        echo "FAIL [p4_lib_rollback] setup failed"
        FAIL=$((FAIL + 1))
    fi
    echo "  PASS p4_driver"
}

run_p5_driver_binder_case() {
    # P5-R6: Driver CLI hardening + >96-char library main symbol parity + user-named ACVC support unit
    if "$ADA_CC" --lib="" "$TDIR/01_checked_arith_and_exceptions.ada" >/dev/null 2>&1; then
        echo "FAIL [p5_driver_empty_lib] --lib=\"\" did not fail"
        FAIL=$((FAIL + 1))
    else
        PASS=$((PASS + 1))
    fi
    if "$ADA_CC" -c --emit-pir "$TDIR/01_checked_arith_and_exceptions.ada" >/dev/null 2>&1; then
        echo "FAIL [p5_driver_c_emit_pir] -c --emit-pir did not fail"
        FAIL=$((FAIL + 1))
    else
        PASS=$((PASS + 1))
    fi
    # >96-char library main procedure compiled with -L -c and bound with --bind
    local ldir="$TMP/p5_long_main_lib"
    local long_name="p5_very_long_library_main_procedure_identifier_exceeding_ninety_six_characters_for_fnv1a_symbol_hash_check"
    mkdir -p "$ldir"
    cat >"$TMP/long_main.ada" <<EOF
with Text_IO; use Text_IO;
procedure ${long_name} is
begin
   Put_Line ("LONG_MAIN_OK");
end ${long_name};
EOF
    if "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O2 -L "$ldir" -c "$TMP/long_main.ada" 2>"$TMP/cc.err" &&
       "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O2 -L "$ldir" --bind "$long_name" -o "$TMP/long_main_bin" 2>"$TMP/cc.err" &&
       "$TMP/long_main_bin" | grep -qx "LONG_MAIN_OK"; then
        PASS=$((PASS + 1))
    else
        echo "FAIL [p5_long_main_bind] >96-char main procedure bind/run failed:"
        cat "$TMP/cc.err"
        FAIL=$((FAIL + 1))
    fi
    # User-defined package named Check_File must be elaborated and linked cleanly
    local udir="$TMP/p5_user_checkfile_lib"
    mkdir -p "$udir"
    cat >"$TMP/check_file.ads" <<'EOF'
package Check_File is
   Val : Integer := 41;
end Check_File;
EOF
    cat >"$TMP/use_cf.adb" <<'EOF'
with Text_IO; use Text_IO;
with Check_File;
procedure Use_CF is
begin
   if Check_File.Val = 41 then
      Put_Line ("USER_CHECK_FILE_OK");
   end if;
end Use_CF;
EOF
    if "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O2 -L "$udir" -c "$TMP/check_file.ads" 2>"$TMP/cc.err" &&
       "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O2 -L "$udir" -c "$TMP/use_cf.adb" 2>"$TMP/cc.err" &&
       "$ADA_CC" --pirc="$PIRC" --target="aarch64-apple-darwin" -O2 -L "$udir" --bind use_cf -o "$TMP/use_cf_bin" 2>"$TMP/cc.err" &&
       "$TMP/use_cf_bin" | grep -qx "USER_CHECK_FILE_OK"; then
        PASS=$((PASS + 1))
    else
        echo "FAIL [p5_user_check_file] user-defined Check_File bind/run failed:"
        cat "$TMP/cc.err"
        FAIL=$((FAIL + 1))
    fi
    echo "  PASS p5_driver_binder"
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
run_case "10_p2s1"    "$TDIR/10_pass2_stage1_rt_task_io.ada"
run_case "11_p2s2"    "$TDIR/11_pass2_stage2_scalar_arr_arith.ada"
run_case "12_p2s3"    "$TDIR/12_pass2_stage3_generics_binder.ada"
run_case "13_p2s4"    "$TDIR/13_pass2_stage4_ch13_rep_unchk.ada"
run_case "14_p2s5"    "$TDIR/14_pass2_stage5_frontend_legality.ada"
run_case "15_p3s1"    "$TDIR/15_pass3_stage1_scalars_arrays_mem.ada"
run_case "16_p3s2"    "$TDIR/16_pass3_stage2_fixed_float_arith.ada"
run_case "17_p3s3"    "$TDIR/17_pass3_stage3_records_aggs_subps.ada"
run_case "18_p3s4"    "$TDIR/18_pass3_stage4_tasks_select_shared.ada"
run_case "19_p3s5"    "$TDIR/19_pass3_stage5_generics_exc.ada"
run_case "20_p3s6"    "$TDIR/20_pass3_stage6_rep_interface_io.ada"
run_case "21_p3s7"    "$TDIR/21_pass3_stage7_frontend_valid.ada"
run_case "23_p4_rt"   "$TDIR/23_pass4_rt_task_io_core.ada"
run_case "24_p4_expr" "$TDIR/24_pass4_expr_checks_rep.ada"
run_case "25_p4_stmt" "$TDIR/25_pass4_stmt_task.ada"
run_case "26_64bit"   "$TDIR/26_refactor_64bit_bounds.ada"
run_case "27_p5_sc"   "$TDIR/27_p5_scalar_array.ada"
run_case "28_p5_cmp"  "$TDIR/28_p5_composite_call_rtl.ada"
run_case "29_p5_stm"  "$TDIR/29_p5_stmt_task_rt.ada"
run_multi_sep_case
run_c_iface_case
run_p4_driver_case
run_p5_driver_binder_case

NEG_OK=0
NEG_FAIL_BEFORE=$FAIL
for nsrc in "$TDIR"/neg/*.ada; do
    nbase="$(basename "$nsrc" .ada)"
    run_neg_case "$nbase" "$nsrc"
    NEG_OK=$((NEG_OK + 1))
done
echo "  DONE negative legality/binder suite ($NEG_OK cases, new failures=$((FAIL - NEG_FAIL_BEFORE)))"

echo "=== Ada 83 Conformance Results: PASS=$PASS FAIL=$FAIL ==="
[ "$FAIL" -eq 0 ]
