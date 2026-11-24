#!/bin/sh
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

set -eu

BIN="${1:-./build/pirc-san}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/pirc_m5.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

PASS=0
FAIL=0

check_pattern() {
    haystack="$1"
    pat="$2"
    if printf '%s' "$pat" | grep -Eq '^[A-Za-z0-9_]+$'; then
        grep -Eq "(^|[^A-Za-z0-9_.])${pat}([^A-Za-z0-9_]|$)" "$haystack"
    else
        grep -Fq -- "$pat" "$haystack"
    fi
}

echo "[M5 gate] Running optimizer fixture tests in tests/opt/*.pir..."
for f in tests/opt/*.pir; do
    opt_level="$(sed -n 's|^// OPT_LEVEL: ||p' "$f" | head -n 1)"
    if [ -z "$opt_level" ]; then opt_level="-O2"; fi
    exp_ret="$(sed -n 's|^// EXPECT_RET: ||p' "$f" | head -n 1)"
    no_crit="$(sed -n 's|^// EXPECT_NO_CRITICAL_EDGES: ||p' "$f" | head -n 1)"

    lir_out="$TMP_DIR/out.lir"
    if ! "$BIN" --emit-lir "$opt_level" "$f" >"$lir_out" 2>"$TMP_DIR/err.txt"; then
        echo "FAIL: --emit-lir $opt_level failed on $f:"
        cat "$TMP_DIR/err.txt"
        FAIL=$((FAIL + 1))
        continue
    fi

    if ! "$BIN" --verify-lir "$lir_out" >/dev/null 2>"$TMP_DIR/err.txt"; then
        echo "FAIL: emitted LIR failed --verify-lir on $f:"
        cat "$TMP_DIR/err.txt"
        FAIL=$((FAIL + 1))
        continue
    fi

    ok=1

    sed -n 's|^// EXPECT_LIR_CONTAINS: ||p' "$f" >"$TMP_DIR/contains.txt"
    while IFS= read -r pat; do
        [ -z "$pat" ] && continue
        if ! check_pattern "$lir_out" "$pat"; then
            echo "FAIL: $f missing expected LIR pattern: [$pat]"
            ok=0
        fi
    done <"$TMP_DIR/contains.txt"

    sed -n 's|^// EXPECT_LIR_FORBIDS: ||p' "$f" >"$TMP_DIR/forbids.txt"
    while IFS= read -r pat; do
        [ -z "$pat" ] && continue
        if check_pattern "$lir_out" "$pat"; then
            echo "FAIL: $f contains forbidden LIR pattern: [$pat]"
            ok=0
        fi
    done <"$TMP_DIR/forbids.txt"

    sed -n 's|^// EXPECT_HOIST_BEFORE: ||p' "$f" >"$TMP_DIR/hoist.txt"
    while IFS= read -r spec; do
        [ -z "$spec" ] && continue
        lbl="$(printf '%s' "$spec" | awk '{print $1}')"
        op="$(printf '%s' "$spec" | awk '{print $2}')"
        if ! awk -v lbl="$lbl" -v op="$op" '
            BEGIN { op_line = 0; lbl_line = 0 }
            $0 ~ ("^" lbl "[:(]") { if (lbl_line == 0) lbl_line = NR }
            index($0, op) > 0 { if (op_line == 0) op_line = NR }
            END { exit !(op_line > 0 && lbl_line > 0 && op_line < lbl_line) }
        ' "$lir_out"; then
            echo "FAIL: $f did not hoist [$op] before [$lbl]"
            ok=0
        fi
    done <"$TMP_DIR/hoist.txt"

    sed -n 's|^// EXPECT_IN_LOOP: ||p' "$f" >"$TMP_DIR/inloop.txt"
    while IFS= read -r op; do
        [ -z "$op" ] && continue
        if ! awk -v op="$op" '
            BEGIN { in_loop = 0; found = 0 }
            /^\.loop[:(]/ { in_loop = 1 }
            /^\.exit[:(]/ || /^\}/ { in_loop = 0 }
            in_loop && index($0, op) > 0 { found = 1 }
            END { exit !found }
        ' "$lir_out"; then
            echo "FAIL: $f expected [$op] to remain inside loop"
            ok=0
        fi
    done <"$TMP_DIR/inloop.txt"

    if [ "$no_crit" = "true" ]; then
        if ! awk '
            function check_fn() {
                for (e in edge_from) {
                    a = edge_from[e]; b = edge_to[e];
                    if (succs[a] > 1 && preds[b] > 1) {
                        printf("critical edge %s -> %s (succs=%d preds=%d)\n", a, b, succs[a], preds[b]) > "/dev/stderr";
                        bad = 1;
                    }
                }
                delete succs; delete preds; delete edge_from; delete edge_to; ne = 0;
            }
            BEGIN { cur = ""; ne = 0; bad = 0 }
            /^\}/ { check_fn(); cur = ""; next }
            /^\.[A-Za-z0-9_]+[:(]/ {
                sub(/[:(].*/, "", $0);
                cur = $0;
                next;
            }
            cur != "" && ($1 == "jmp" || $1 == "br" || $1 == "switch") {
                line = $0;
                while (match(line, /\.[A-Za-z0-9_]+/)) {
                    tgt = substr(line, RSTART, RLENGTH);
                    ne++;
                    edge_from[ne] = cur;
                    edge_to[ne] = tgt;
                    succs[cur]++;
                    preds[tgt]++;
                    line = substr(line, RSTART + RLENGTH);
                }
            }
            END { exit bad }
        ' "$lir_out"; then
            echo "FAIL: $f still contains a critical edge"
            ok=0
        fi
    fi

    if [ -n "$exp_ret" ]; then
        for lvl in -O0 -O1 -O2; do
            set +e
            "$BIN" --interp "$lvl" "$f" >"$TMP_DIR/run.out" 2>"$TMP_DIR/run.err"
            rc=$?
            set -e
            if [ "$rc" -ne "$exp_ret" ]; then
                echo "FAIL: $f ($lvl) returned $rc, expected $exp_ret"
                cat "$TMP_DIR/run.err"
                ok=0
            fi
        done
    fi

    if [ "$ok" -eq 1 ]; then
        echo "  OPT PASS $f"
        PASS=$((PASS + 1))
    else
        echo "  --- emitted LIR for $f ---"
        cat "$lir_out"
        FAIL=$((FAIL + 1))
    fi
done

echo "[M5 gate] Running differential -O0 vs -O1 vs -O2 execution tests..."
for dir in tests/interp_exec tests/hir_exec tests/hir_m4_exec; do
    for f in "$dir"/*.pir; do
        set +e
        "$BIN" --interp -O0 "$f" >"$TMP_DIR/o0.out" 2>"$TMP_DIR/o0.err"
        rc0=$?
        "$BIN" --interp -O1 "$f" >"$TMP_DIR/o1.out" 2>"$TMP_DIR/o1.err"
        rc1=$?
        "$BIN" --interp -O2 "$f" >"$TMP_DIR/o2.out" 2>"$TMP_DIR/o2.err"
        rc2=$?
        set -e

        if [ "$rc0" -ne "$rc1" ] || [ "$rc0" -ne "$rc2" ] || \
           ! cmp -s "$TMP_DIR/o0.out" "$TMP_DIR/o1.out" || \
           ! cmp -s "$TMP_DIR/o0.out" "$TMP_DIR/o2.out"; then
            echo "FAIL: differential mismatch on $f (rc: O0=$rc0 O1=$rc1 O2=$rc2)"
            [ -s "$TMP_DIR/o1.err" ] && cat "$TMP_DIR/o1.err"
            [ -s "$TMP_DIR/o2.err" ] && cat "$TMP_DIR/o2.err"
            FAIL=$((FAIL + 1))
        else
            PASS=$((PASS + 1))
        fi
    done
done

echo "[M5 gate] Summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
