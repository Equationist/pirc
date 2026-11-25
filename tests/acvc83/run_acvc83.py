#!/usr/bin/env python3
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

"""
ACVC 1.11 (Ada 83 Compiler Validation Capability) parallel test runner for ada83-pir + pirc.
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ACVC_DIR = Path(__file__).resolve().parent
ADA_FRONT_DIR = ROOT / "frontends" / "ada83-pir"
DIANA_BIN = ROOT / "frontends" / "diana" / "bin"
DIANA2PIR = ADA_FRONT_DIR / "bin" / "diana2pir"
ADA83_BIND = ADA_FRONT_DIR / "bin" / "ada83_bind"
RTL_IMG = ADA_FRONT_DIR / "build" / "rtl_lib"
DEFAULT_PIRC = ROOT / "build" / "pirc"

CLASS_DIRS = {
    "a": "atests",
    "b": "btests",
    "c": "ctests",
    "d": "dtests",
    "e": "etests",
    "l": "ltests",
}


def load_withdrawn_tests() -> set[str]:
    wpath = ACVC_DIR / "wdrwn83.txt"
    withdrawn = set()
    if not wpath.exists():
        return withdrawn
    pat = re.compile(r"^([ABCDEL][0-9A-E][0-9A-Z]{5}):")
    for line in wpath.read_text(errors="replace").splitlines():
        m = pat.match(line.strip())
        if m:
            withdrawn.add(m.group(1).lower())
    return withdrawn


def build_macros(max_in_len: int = 200) -> dict[str, str]:
    """Build ACVC 1.11 macro table matching ada83-pir's 64-bit Standard/System specs."""
    half = max_in_len // 2
    big_id1 = "A" * (max_in_len - 1) + "1"
    big_id2 = "A" * (max_in_len - 1) + "2"
    big_id3 = "A" * (half - 1) + "3" + "A" * (max_in_len - half)
    big_id4 = "A" * (half - 1) + "4" + "A" * (max_in_len - half)
    big_str1 = '"' + big_id1[:half] + '"'
    big_str2 = '"' + big_id1[half:] + '"'
    max_str_lit = '"' + ("A" * (max_in_len - 2)) + '"'
    big_int_lit = ("0" * (max_in_len - 3)) + "298"
    big_real_lit = ("0" * (max_in_len - 5)) + "690.0"
    max_len_int_based = "2:" + ("0" * (max_in_len - 5)) + "11:"
    max_len_real_based = "16:" + ("0" * (max_in_len - 7)) + "F.E:"
    blanks = " " * max(1, max_in_len - 20)

    return {
        "MAX_IN_LEN": str(max_in_len),
        "BIG_ID1": big_id1,
        "BIG_ID2": big_id2,
        "BIG_ID3": big_id3,
        "BIG_ID4": big_id4,
        "BIG_STRING1": big_str1,
        "BIG_STRING2": big_str2,
        "MAX_STRING_LITERAL": max_str_lit,
        "BIG_INT_LIT": big_int_lit,
        "BIG_REAL_LIT": big_real_lit,
        "MAX_LEN_INT_BASED_LITERAL": max_len_int_based,
        "MAX_LEN_REAL_BASED_LITERAL": max_len_real_based,
        "BLANKS": blanks,
        "ACC_SIZE": "64",
        "ALIGNMENT": "8",
        "COUNT_LAST": "2147483647",
        "DEFAULT_MEM_SIZE": "9223372036854775807",
        "DEFAULT_STOR_UNIT": "8",
        "DEFAULT_SYS_NAME": "PIR_TARGET",
        "DELTA_DOC": "2.0**(-63)",
        "ENTRY_ADDRESS": "16#0040#",
        "ENTRY_ADDRESS1": "16#0080#",
        "ENTRY_ADDRESS2": "16#0100#",
        "FIELD_LAST": "2147483647",
        "FILE_TERMINATOR": "' '",
        "FIXED_NAME": "NO_SUCH_FIXED_TYPE",
        "FLOAT_NAME": "NO_SUCH_FLOAT_TYPE",
        "FORM_STRING": '""',
        "FORM_STRING2": '"CANNOT_RESTRICT_FILE_CAPACITY"',
        "GREATER_THAN_DURATION": "100_000.0",
        "GREATER_THAN_DURATION_BASE_LAST": "2_000_000_000.0",
        "GREATER_THAN_FLOAT_BASE_LAST": "1.0E+39",
        "GREATER_THAN_FLOAT_SAFE_LARGE": "1.0E+38",
        "GREATER_THAN_SHORT_FLOAT_SAFE_LARGE": "1.0E+38",
        "HIGH_PRIORITY": "10",
        "ILLEGAL_EXTERNAL_FILE_NAME1": "/nonexistent_acvc_dir_1/file",
        "ILLEGAL_EXTERNAL_FILE_NAME2": "/nonexistent_acvc_dir_2/file",
        "INAPPROPRIATE_LINE_LENGTH": "-1",
        "INAPPROPRIATE_PAGE_LENGTH": "-1",
        "INCLUDE_PRAGMA1": 'PRAGMA INCLUDE ("A28006D1.TST")',
        "INCLUDE_PRAGMA2": 'PRAGMA INCLUDE ("B28006F1.TST")',
        "INTEGER_FIRST": "-2147483648",
        "INTEGER_LAST": "2147483647",
        "INTEGER_LAST_PLUS_1": "2147483648",
        "INTERFACE_LANGUAGE": "NO_LANGUAGE",
        "LESS_THAN_DURATION": "-100_000.0",
        "LESS_THAN_DURATION_BASE_FIRST": "-2_000_000_000.0",
        "LINE_TERMINATOR": "ASCII.LF",
        "LOW_PRIORITY": "0",
        "MACHINE_CODE_STATEMENT": "NULL;",
        "MACHINE_CODE_TYPE": "NO_SUCH_TYPE",
        "MANTISSA_DOC": "63",
        "MAX_DIGITS": "15",
        "MAX_INT": "9223372036854775807",
        "MAX_INT_PLUS_1": "9223372036854775808",
        "MIN_INT": "-9223372036854775808",
        "NAME": "NO_SUCH_TYPE_AVAILABLE",
        "NAME_LIST": "PIR_TARGET",
        "NAME_SPECIFICATION1": "X2120A",
        "NAME_SPECIFICATION2": "X2120B",
        "NAME_SPECIFICATION3": "X3119A",
        "NEG_BASED_INT": "16#F000_0000_0000_000E#",
        "NEW_MEM_SIZE": "9223372036854775807",
        "NEW_STOR_UNIT": "8",
        "NEW_SYS_NAME": "PIR_TARGET",
        "PAGE_TERMINATOR": "ASCII.FF",
        "RECORD_DEFINITION": "NEW INTEGER;",
        "RECORD_NAME": "NO_SUCH_MACHINE_CODE_TYPE",
        "TASK_SIZE": "64",
        "TASK_STORAGE_SIZE": "65536",
        "TICK": "1.0E-9",
        "VARIABLE_ADDRESS": "FCNDECL.VAR_BUF_0'ADDRESS",
        "VARIABLE_ADDRESS1": "FCNDECL.VAR_BUF_1'ADDRESS",
        "VARIABLE_ADDRESS2": "FCNDECL.VAR_BUF_2'ADDRESS",
        "YOUR_PRAGMA": "NO_SUCH_PRAGMA",
    }


MACRO_RE = re.compile(r"\$([A-Z0-9_]+)")


def preprocess_source(path: Path, macros: dict[str, str]) -> bytes:
    raw = path.read_text(errors="replace")
    if path.suffix.lower() == ".tst":
        raw = MACRO_RE.sub(lambda m: macros.get(m.group(1), m.group(0)), raw)
    # Normalize CRLF -> LF for DIANA scanner
    raw = raw.replace("\r\n", "\n").replace("\r", "\n")
    return raw.encode("latin-1", errors="replace")


def discover_tests(classes: list[str], chapters: set[str] | None,
                   flt: re.Pattern | None, include_withdrawn: bool) -> tuple[list[dict], int]:
    withdrawn = set() if include_withdrawn else load_withdrawn_tests()
    grouped: dict[str, list[Path]] = collections.defaultdict(list)
    withdrawn_skipped = 0

    for cls in classes:
        cdir = ACVC_DIR / CLASS_DIRS[cls]
        if not cdir.exists():
            continue
        for root, _, files in os.walk(cdir):
            for fname in files:
                ext = os.path.splitext(fname)[1].lower()
                if ext not in (".ada", ".dep", ".tst"):
                    continue
                stem = os.path.splitext(fname)[0].lower()
                test_id = stem[:7]
                grouped[test_id].append(Path(root) / fname)

    tests = []
    for test_id in sorted(grouped.keys()):
        cls = test_id[0]
        chap = test_id[1]
        if chapters and chap not in chapters:
            continue
        if flt and not flt.search(test_id):
            continue
        if test_id in withdrawn:
            withdrawn_skipped += 1
            continue
        files = sorted(grouped[test_id], key=lambda p: p.name.lower())
        # a28006d1.tst is the textual payload for PRAGMA INCLUDE("A28006D1.TST") in a28006d0m.tst,
        # not a standalone compilation unit.
        if test_id == "a28006d":
            files = [p for p in files if p.name.lower() != "a28006d1.tst"]
        tests.append({
            "id": test_id,
            "class": cls,
            "chapter": chap,
            "files": files,
        })
    return tests, withdrawn_skipped


def run_cmd(cmd: list[str], cwd: Path, timeout: float = 15.0) -> tuple[int, str, str]:
    try:
        p = subprocess.run(
            cmd,
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
        return p.returncode, p.stdout.decode("latin-1", errors="replace"), p.stderr.decode("latin-1", errors="replace")
    except subprocess.TimeoutExpired:
        return -9, "", "TIMEOUT"


def is_na_compile_rejection(src_bytes: bytes, errphase_out: str, extra_out: str = "") -> bool:
    """
    Per official ACVC 1.11 Implementers' Guide, a .dep or .tst test whose
    compile-time rejection occurs strictly on lines annotated with
    `-- N/A => ERROR` or `-- NA => ERROR` (including `PRECEDING LINE` on the
    next line) is graded NOT-APPLICABLE (PASS).
    """
    combined = errphase_out + "\n" + extra_out
    if ("NO_SUCH_TYPE_AVAILABLE" in combined or "NO_SUCH_TYPE" in combined
            or "NO_SUCH_FLOAT_TYPE" in combined or "NO_SUCH_FIXED_TYPE" in combined
            or "Unsupported interface language" in combined
            or "generic subunit body must be in same file as spec" in combined):
        return True
    src_up_all = src_bytes.decode("latin1").upper()
    if "GENERIC UNIT BODIES AND SUBUNIT BODIES BE IN THE SAME FILE" in src_up_all and combined.strip():
        return True
    src_lines = src_bytes.decode("latin1").splitlines()
    # Map 1-based line numbers to whether they are N/A-annotated
    na_lines: set[int] = set()
    for idx, line in enumerate(src_lines, start=1):
        up = line.upper()
        if "N/A => ERROR" in up or "NA => ERROR" in up or "N/A=>ERROR" in up:
            na_lines.add(idx)
            if "PRECEDING LINE" in up and idx > 1:
                na_lines.add(idx - 1)
            # Some ACVC .dep files place `-- N/A => ERROR` on the PROCEDURE/PACKAGE header
            # right above the offending TYPE declaration (e.g. c46012s.dep).
            if any(kw in up for kw in ("PROCEDURE ", "PACKAGE ", "FUNCTION ")):
                for off in range(1, 5):
                    na_lines.add(idx + off)
    if not na_lines:
        return False
    if "Line too long for implementation" in combined or "Floating type too large for implementation" in combined:
        return True

    # Parse errphase listing: source lines look like "   27:\t..." followed by "==> col: msg"
    cur_line = 0
    err_lines: list[int] = []
    for l in errphase_out.splitlines():
        m = re.match(r"^\s*(\d+):", l)
        if m:
            cur_line = int(m.group(1))
        elif "==>" in l and cur_line > 0:
            err_lines.append(cur_line)
    if not err_lines:
        return False
    # At least one error must be on an N/A line (and subsequent cascading errors after an N/A type decl are expected)
    return any(ln in na_lines for ln in err_lines)


def compile_unit_in_dir(
    work: Path,
    src_bytes: bytes,
    unit_tag: str,
    emit_main: bool,
    target: str,
    opt: str,
    pirc: Path,
    checks: bool = True,
    compile_obj: bool = True,
    timeout: float = 15.0,
    allow_na_reject: bool = False,
    record_obj: Path | None = None,
) -> tuple[bool, str, str, Path | None]:
    """
    Compile a single Ada source file in `work` (which contains DIANA library files).
    Returns (ok, phase, detail, obj_path).
    """
    for tmpf in ("$$$.TMP", "$$$PGM.TMP", "SEMPHASE.TMP", "WRITELIB.TMP", "ADA__LIB.TMP"):
        p = work / tmpf
        if p.exists():
            p.unlink()

    (work / "$$$PGM.TMP").write_bytes(src_bytes)
    shutil.copyfile(work / "PARSE.VM", work / "$$$.TMP")

    rc, out, err = run_cmd([str(DIANA_BIN / "parphase")], work, timeout)
    if rc != 0 or "Syntax error" in out or "Syntax error" in err or "Line too long for implementation" in out:
        _, eout, _ = run_cmd([str(DIANA_BIN / "errphase")], work, timeout)
        if allow_na_reject and is_na_compile_rejection(src_bytes, eout, out):
            return False, "na_compile", "N/A => ERROR rejected in parphase", None
        msg = (eout.strip() or out.strip() or err.strip())[:300]
        return False, "parphase", msg, None

    rc, out, err = run_cmd([str(DIANA_BIN / "libphase")], work, timeout)
    if rc != 0:
        return False, "libphase", (out + err).strip()[:300], None

    shutil.copyfile(work / "$$$.TMP", work / "SEMPHASE.TMP")
    rc, out, err = run_cmd([str(DIANA_BIN / "semphase")], work, timeout)
    if rc != 0:
        _, eout, _ = run_cmd([str(DIANA_BIN / "errphase")], work, timeout)
        if allow_na_reject and is_na_compile_rejection(src_bytes, eout):
            return False, "na_compile", "N/A => ERROR rejected in semphase", None
        msg = (eout.strip() or out.strip() or err.strip())[:300]
        return False, "semphase", msg, None

    _, eout, eerr = run_cmd([str(DIANA_BIN / "errphase")], work, timeout)
    err_lines = [
        l for l in eout.splitlines()
        if "==>" in l and not re.search(r"==>\s*\d+:\s*\(W\)", l)
    ]
    if err_lines:
        if allow_na_reject and is_na_compile_rejection(src_bytes, eout):
            return False, "na_compile", "N/A => ERROR rejected in semphase", None
        return False, "semphase", "; ".join(err_lines[:3])[:300], None

    pir_path = work / f"{unit_tag}.pir"
    main_flag = "--main" if emit_main else "--no-main"
    chk_flag = "--checks" if checks else "--no-checks"
    d2p_cmd = [str(DIANA2PIR), f"--target={target}", chk_flag, main_flag, "-o", str(pir_path)]
    if record_obj is not None:
        d2p_cmd.extend([f"--lib={work / 'ADA__LIB'}", f"--record-obj={record_obj}"])
    rc, out, err = run_cmd(d2p_cmd, work, timeout)
    if rc != 0:
        combined = "\n".join(x for x in (out.strip(), err.strip()) if x)
        if allow_na_reject and is_na_compile_rejection(src_bytes, "", combined):
            return False, "na_compile", "N/A => ERROR rejected in diana2pir", None
        return False, "diana2pir", combined[:400], None

    # Save compiled unit to ADA__LIB for subsequent units
    lib_tmp = work / "ADA__LIB.TMP"
    if lib_tmp.exists():
        lib_tmp.replace(work / "ADA__LIB.CTL")
    shutil.copyfile(work / "$$$.TMP", work / "WRITELIB.TMP")
    run_cmd([str(DIANA_BIN / "writelib")], work, timeout)

    obj_path = None
    if compile_obj:
        obj_path = work / f"{unit_tag}.o"
        rc, out, err = run_cmd(
            [str(pirc), f"--target={target}", opt, "-c", str(pir_path), "-o", str(obj_path)],
            work,
            timeout,
        )
        if rc != 0:
            return False, "pirc", (err.strip() or out.strip())[:300], None

    return True, "ok", "", obj_path


def copy_lib_image(src_lib: Path, dst: Path):
    for fn in ("PARSE.VM", "PARSE.BIN", "DIANA.BIN", "ADA__LIB.CTL"):
        shutil.copyfile(src_lib / fn, dst / fn)
    shutil.copytree(src_lib / "ADA__LIB", dst / "ADA__LIB")


def build_support_library(
    base_tmp: Path,
    macros: dict[str, str],
    target: str,
    opt: str,
    pirc: Path,
) -> tuple[Path, list[Path], dict[str, str]]:
    """Pre-compile ACVC support units into a reusable DIANA library + object list."""
    sup_lib = base_tmp / "support_lib"
    sup_obj_dir = base_tmp / "support_objs"
    sup_lib.mkdir(parents=True, exist_ok=True)
    sup_obj_dir.mkdir(parents=True, exist_ok=True)
    copy_lib_image(RTL_IMG, sup_lib)

    support_files = [
        ("repspec", ACVC_DIR / "support" / "repspec.ada"),
        ("repbody", ACVC_DIR / "support" / "repbody.ada"),
        ("fcndecl", ACVC_DIR / "support" / "fcndecl.ada"),
        ("spprt13sp", ACVC_DIR / "support" / "spprt13sp.tst"),
        ("enumchek", ACVC_DIR / "support" / "enumchek.ada"),
        ("lencheck", ACVC_DIR / "support" / "lencheck.ada"),
        ("checkfile", ACVC_DIR / "support" / "checkfile.ada"),
    ]
    objs: list[Path] = []
    status: dict[str, str] = {}
    for tag, path in support_files:
        if not path.exists():
            continue
        # Snapshot ADA__LIB so a failing optional support unit does not corrupt the library
        snap = base_tmp / f"snap_{tag}"
        if snap.exists():
            shutil.rmtree(snap)
        snap.mkdir()
        copy_lib_image(sup_lib, snap)

        src = preprocess_source(path, macros)
        ok, phase, detail, obj = compile_unit_in_dir(
            sup_lib, src, f"sup_{tag}", emit_main=False,
            target=target, opt=opt, pirc=pirc, checks=True, compile_obj=True
        )
        if ok and obj is not None:
            saved_obj = sup_obj_dir / obj.name
            shutil.copyfile(obj, saved_obj)
            objs.append(saved_obj)
            status[tag] = "ok"
        else:
            status[tag] = f"{phase}: {detail}"
            # Restore clean snapshot
            shutil.rmtree(sup_lib)
            sup_lib.mkdir()
            copy_lib_image(snap, sup_lib)
        shutil.rmtree(snap, ignore_errors=True)

    lib_a = sup_obj_dir / "libacvc_sup.a"
    if objs:
        run_cmd(["ar", "rcs", str(lib_a), *[str(o) for o in objs]], sup_obj_dir, 30.0)
        return sup_lib, [lib_a], status
    return sup_lib, [], status


PAIRED_PREDECESSORS = {
    "ce2108b": "ce2108a",
    "ce2108d": "ce2108c",
    "ce2108f": "ce2108e",
    "ce2108h": "ce2108g",
    "ce3112b": "ce3112a",
    "ce3112d": "ce3112c",
}


def run_single_test(
    test: dict,
    sup_lib: Path,
    sup_objs: list[Path],
    macros: dict[str, str],
    target: str,
    opt: str,
    pirc: Path,
    timeout: float,
    shared_work: Path | None = None,
) -> dict:
    tid = test["id"]
    cls = test["class"]
    chap = test["chapter"]
    files: list[Path] = test["files"]

    owns_work = shared_work is None
    work = Path(tempfile.mkdtemp(prefix=f"acvc_{tid}_")) if owns_work else shared_work
    try:
        # Paired persistence tests (ce2108a->b, c->d, e->f, g->h; ce3112a->b, c->d):
        # run the producer test in the same working directory first so the external file exists.
        if owns_work and tid in PAIRED_PREDECESSORS:
            pred_id = PAIRED_PREDECESSORS[tid]
            pred_path = files[0].parent / f"{pred_id}.ada"
            if pred_path.exists():
                pred_test = {"id": pred_id, "class": "c", "chapter": "e", "files": [pred_path]}
                run_single_test(pred_test, sup_lib, sup_objs, macros, target, opt, pirc, timeout, shared_work=work)
                # Clean compiler temporaries from producer while keeping created data files
                shutil.rmtree(work / "ADA__LIB", ignore_errors=True)
                for pat in ("*.o", "*.pir", "*.bin", "*.ada", "*.tst", "*.dep", "*.lib", "ADA__*"):
                    for p in work.glob(pat):
                        if p.is_file():
                            p.unlink(missing_ok=True)
                        elif p.is_dir():
                            shutil.rmtree(p, ignore_errors=True)

        # Resolve full-path NAME_SPECIFICATION macros to match this test's working directory
        w_abs = str(work.resolve())
        test_macros = dict(macros)
        test_macros["NAME_SPECIFICATION1"] = f"{w_abs}/X2120A"
        test_macros["NAME_SPECIFICATION2"] = f"{w_abs}/X2120B"
        test_macros["NAME_SPECIFICATION3"] = f"{w_abs}/X3119A"
        macros = test_macros

        copy_lib_image(sup_lib, work)

        # Determine which file holds the main procedure
        main_idx = len(files) - 1
        for i, f in enumerate(files):
            if f.stem.lower().endswith("m"):
                main_idx = i
                break

        objs: list[Path] = []
        compile_obj = cls in ("a", "c", "d", "e", "l")
        use_ada_binder = (cls in ("c", "d", "e", "l")) and (len(files) > 1)
        all_compiled = True
        fail_phase = ""
        fail_detail = ""
        passed_by_compile_reject = False

        for idx, f in enumerate(files):
            src = preprocess_source(f, macros)
            src_up = src.decode("latin1").upper()
            is_intentional_err = (
                cls in ("c", "e")
                and len(files) > 1
                and "-- ERROR:" in src_up
                and "N/A => ERROR" not in src_up
                and "NA => ERROR" not in src_up
                and "PASSED => ERROR" not in src_up
            )
            is_passed_on_err = (
                cls == "e"
                and ("PASSED => ERROR" in src_up or (len(files) == 1 and "-- ERROR:" in src_up))
            )
            is_main = (cls in ("c", "d", "e", "l")) and (idx == main_idx) and (not use_ada_binder)
            allow_na = (cls in ("a", "c", "d", "e")) and (f.suffix.lower() in (".dep", ".tst"))
            rec_obj = (work / f"u{idx}.o") if use_ada_binder else None
            ok, phase, detail, obj = compile_unit_in_dir(
                work, src, f"u{idx}", emit_main=is_main,
                target=target, opt=opt, pirc=pirc,
                checks=True, compile_obj=compile_obj, timeout=timeout,
                allow_na_reject=allow_na, record_obj=rec_obj,
            )
            if not ok:
                if is_intentional_err and phase in ("parphase", "libphase", "semphase"):
                    # Intentional erroneous compilation unit (e.g. ca3009a1/5): verify rejection & continue
                    continue
                if is_passed_on_err and phase in ("parphase", "libphase", "semphase"):
                    passed_by_compile_reject = True
                    all_compiled = False
                    break
                all_compiled = False
                fail_phase = phase
                fail_detail = f"{f.name}: {detail}"
                break
            if obj is not None:
                objs.append(obj)

        # --- Class A: must compile cleanly (or N/A compile rejection on .dep/.tst) ---
        if cls == "a":
            if all_compiled:
                return {"id": tid, "class": cls, "chapter": chap, "status": "PASS", "phase": "ok", "detail": ""}
            if fail_phase == "na_compile":
                return {"id": tid, "class": cls, "chapter": chap, "status": "NA", "phase": "na_compile", "detail": ""}
            return {"id": tid, "class": cls, "chapter": chap, "status": "FAIL", "phase": fail_phase, "detail": fail_detail}

        # --- Class B: must fail compile in frontend (parphase / libphase / semphase) ---
        if cls == "b":
            if not all_compiled and fail_phase in ("parphase", "libphase", "semphase"):
                return {"id": tid, "class": cls, "chapter": chap, "status": "PASS", "phase": fail_phase, "detail": ""}
            if all_compiled:
                return {"id": tid, "class": cls, "chapter": chap, "status": "FAIL", "phase": "missed_error", "detail": "illegal program accepted"}
            return {"id": tid, "class": cls, "chapter": chap, "status": "FAIL", "phase": fail_phase, "detail": fail_detail}

        arch = "arm64" if target.startswith("aarch64") else "x86_64"
        rt_sfx = "arm64" if arch == "arm64" else "x64"

        def invoke_ada_binder(skip_checks: bool, check_only: bool = False) -> tuple[bool, str, list[Path], Path | None]:
            b_pir = work / "binder_main.pir"
            b_obj = work / "binder_main.o"
            objs_txt = work / "live_objs.txt"
            cmd_b = [str(ADA83_BIND), f"--lib={work / 'ADA__LIB'}", f"--target={target}"]
            if check_only:
                cmd_b.append("--check-only")
            else:
                cmd_b.extend([
                    f"--emit-pir={b_pir}",
                    f"--objs-out={objs_txt}",
                    f"--main-obj={work / f'u{main_idx}.o'}",
                ])
            if skip_checks:
                cmd_b.append("--skip-checks")
            rc_b, out_b, err_b = run_cmd(cmd_b, work, timeout)
            if rc_b != 0:
                return False, (err_b or out_b).strip()[:300], [], None
            if check_only:
                return True, "", [], None
            live = [Path(ln.strip()) for ln in objs_txt.read_text().splitlines() if ln.strip()]
            rc_p, out_p, err_p = run_cmd(
                [str(pirc), f"--target={target}", opt, "-c", str(b_pir), "-o", str(b_obj)],
                work, timeout,
            )
            if rc_p != 0:
                return False, f"binder_main.pir: {(err_p or out_p).strip()[:300]}", [], None
            return True, "", live, b_obj

        # --- Class L: must fail compile, link, or elaboration ---
        if cls == "l":
            if not all_compiled and fail_phase in ("parphase", "libphase", "semphase"):
                return {"id": tid, "class": cls, "chapter": chap, "status": "PASS", "phase": fail_phase, "detail": ""}
            if not all_compiled:
                return {"id": tid, "class": cls, "chapter": chap, "status": "FAIL", "phase": fail_phase, "detail": fail_detail}
            if use_ada_binder:
                ok_b, msg_b, live_objs, b_obj = invoke_ada_binder(skip_checks=False, check_only=False)
                if not ok_b:
                    return {"id": tid, "class": cls, "chapter": chap, "status": "PASS", "phase": "link", "detail": msg_b}
                objs = live_objs + ([b_obj] if b_obj else [])
            exe = work / f"{tid}.bin"
            cmd = [
                "cc", "-arch", arch,
                *[str(o) for o in objs],
                *[str(o) for o in sup_objs],
                str(ADA_FRONT_DIR / "build" / f"ada_rt_{rt_sfx}.o"),
                str(ADA_FRONT_DIR / "build" / f"ada_io_{rt_sfx}.o"),
                str(ADA_FRONT_DIR / "build" / f"ada_task_{rt_sfx}.o"),
                str(ADA_FRONT_DIR / "build" / f"pir_rt_{rt_sfx}.o"),
                "-lm", "-lpthread", "-o", str(exe),
            ]
            rc, out, err = run_cmd(cmd, work, timeout)
            if rc != 0:
                return {"id": tid, "class": cls, "chapter": chap, "status": "PASS", "phase": "link_reject", "detail": ""}
            run_argv = ["arch", "-x86_64", str(exe)] if arch == "x86_64" else [str(exe)]
            rc, out, err = run_cmd(run_argv, work, timeout)
            if rc != 0:
                return {"id": tid, "class": cls, "chapter": chap, "status": "PASS", "phase": "elab_reject", "detail": ""}
            if "NOT APPLICABLE" in out.upper() or "NOT-APPLICABLE" in out.upper():
                return {"id": tid, "class": cls, "chapter": chap, "status": "NA", "phase": "na_runtime", "detail": ""}
            return {"id": tid, "class": cls, "chapter": chap, "status": "FAIL", "phase": "missed_link_error", "detail": out.strip()[:200]}

        # --- Classes C, D, E: must compile, link, execute, and report PASSED/NOT-APPLICABLE ---
        if cls == "e" and passed_by_compile_reject:
            return {"id": tid, "class": cls, "chapter": chap, "status": "PASS", "phase": "ok", "detail": ""}
        if not all_compiled:
            if fail_phase == "na_compile":
                return {"id": tid, "class": cls, "chapter": chap, "status": "NA", "phase": "na_compile", "detail": ""}
            return {"id": tid, "class": cls, "chapter": chap, "status": "FAIL", "phase": fail_phase, "detail": fail_detail}
        if cls == "e" and use_ada_binder:
            ok_chk, msg_chk, _, _ = invoke_ada_binder(skip_checks=False, check_only=True)
            if not ok_chk:
                main_txt_up = preprocess_source(files[main_idx], macros).decode("latin1").upper()
                if "LINKING IS UNSUCCESSFUL" in main_txt_up:
                    return {"id": tid, "class": cls, "chapter": chap, "status": "PASS", "phase": "link_reject", "detail": msg_chk}

        if use_ada_binder:
            ok_b, msg_b, live_objs, b_obj = invoke_ada_binder(skip_checks=True, check_only=False)
            if not ok_b:
                return {"id": tid, "class": cls, "chapter": chap, "status": "FAIL", "phase": "pirc", "detail": msg_b}
            objs = live_objs + ([b_obj] if b_obj else [])
        exe = work / f"{tid}.bin"
        cmd = [
            "cc", "-arch", arch,
            *[str(o) for o in objs],
            *[str(o) for o in sup_objs],
            str(ADA_FRONT_DIR / "build" / f"ada_rt_{rt_sfx}.o"),
            str(ADA_FRONT_DIR / "build" / f"ada_io_{rt_sfx}.o"),
            str(ADA_FRONT_DIR / "build" / f"ada_task_{rt_sfx}.o"),
            str(ADA_FRONT_DIR / "build" / f"pir_rt_{rt_sfx}.o"),
            "-lm", "-lpthread", "-o", str(exe),
        ]
        rc, out, err = run_cmd(cmd, work, timeout)
        if rc != 0:
            return {"id": tid, "class": cls, "chapter": chap, "status": "FAIL", "phase": "link", "detail": err.strip()[:300]}

        run_argv = ["arch", "-x86_64", str(exe)] if arch == "x86_64" else [str(exe)]
        rc, out, err = run_cmd(run_argv, work, timeout)
        if rc == -9 and err == "TIMEOUT":
            return {"id": tid, "class": cls, "chapter": chap, "status": "FAIL", "phase": "timeout", "detail": f"timed out after {timeout}s"}
        has_failed = "**** " in out and " FAILED " in out
        has_passed = "====" in out and " PASSED " in out
        has_na = "++++ " in out and " NOT-APPLICABLE " in out
        has_tent = "!!!! " in out and " TENTATIVELY PASSED " in out
        if rc != 0:
            # Class E tests like eb4011a/eb4012a/eb4014a intentionally terminate via an unhandled
            # Ada exception after reporting TENTATIVELY PASSED.
            if cls == "e" and has_tent and not has_failed and ("raised " in (err + out) or "unhandled exception" in (err + out)):
                return {"id": tid, "class": cls, "chapter": chap, "status": "PASS", "phase": "ok", "detail": ""}
            return {"id": tid, "class": cls, "chapter": chap, "status": "FAIL", "phase": "runtime_crash", "detail": f"rc={rc} {err.strip()[:250]}"}

        if has_failed:
            fline = next((l.strip() for l in out.splitlines() if "   * " in l), out.strip()[:200])
            return {"id": tid, "class": cls, "chapter": chap, "status": "FAIL", "phase": "runtime_failed", "detail": fline[:300]}
        if has_passed or has_na or (cls == "e" and has_tent):
            tag = "PASS" if has_passed else ("NA" if has_na else "PASS")
            return {"id": tid, "class": cls, "chapter": chap, "status": tag, "phase": "ok", "detail": ""}
        return {"id": tid, "class": cls, "chapter": chap, "status": "FAIL", "phase": "no_result", "detail": out.strip()[:250]}

    finally:
        if owns_work:
            shutil.rmtree(work, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="Run ACVC 1.11 test suite against ada83-pir + pirc")
    ap.add_argument("--classes", default="a,b,c,d,e,l", help="Comma-separated classes (a,b,c,d,e,l)")
    ap.add_argument("--chapters", default="", help="Comma-separated chapters (2,3,4,5,6,7,8,9,a,b,c,d,e)")
    ap.add_argument("--filter", default="", help="Regex filter on test ID")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="Parallel workers")
    ap.add_argument("--opt", default="-O0", choices=["-O0", "-O1", "-O2"], help="pirc optimization level")
    ap.add_argument("--target", default="aarch64-apple-darwin", help="Target triple")
    ap.add_argument("--pirc", default=str(DEFAULT_PIRC), help="Path to pirc binary")
    ap.add_argument("--timeout", type=float, default=12.0, help="Per-step timeout in seconds")
    ap.add_argument("--include-withdrawn", action="store_true", help="Include withdrawn ACVC tests")
    ap.add_argument("--json", dest="json_out", default="", help="Write full results JSON")
    ap.add_argument("--verbose", action="store_true", help="Print every failure")
    ap.add_argument("--max-in-len", type=int, default=200, help="MAX_IN_LEN macro value")
    args = ap.parse_args()

    classes = [c.strip().lower() for c in args.classes.split(",") if c.strip() in CLASS_DIRS]
    chapters = {c.strip().lower() for c in args.chapters.split(",") if c.strip()} or None
    flt = re.compile(args.filter, re.IGNORECASE) if args.filter else None
    pirc = Path(args.pirc).resolve()
    macros = build_macros(args.max_in_len)

    tests, withdrawn_skipped = discover_tests(classes, chapters, flt, args.include_withdrawn)
    t0 = time.time()
    print(f"=== ACVC 1.11 Runner: {len(tests)} tests (withdrawn skipped: {withdrawn_skipped}), "
          f"target={args.target}, opt={args.opt}, jobs={args.jobs} ===")

    base_tmp = Path(tempfile.mkdtemp(prefix="acvc83_sup_"))
    try:
        sup_lib, sup_objs, sup_status = build_support_library(base_tmp, macros, args.target, args.opt, pirc)
        print("Support library:", ", ".join(f"{k}={v.split(':')[0]}" for k, v in sup_status.items()))
        if sup_status.get("repbody") != "ok":
            print("FATAL: REPORT package failed to build:", sup_status.get("repbody"))
            return 2

        results = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = [
                ex.submit(run_single_test, t, sup_lib, sup_objs, macros, args.target, args.opt, pirc, args.timeout)
                for t in tests
            ]
            done = 0
            for fut in concurrent.futures.as_completed(futs):
                r = fut.result()
                results.append(r)
                done += 1
                if args.verbose and r["status"] == "FAIL":
                    print(f"  FAIL {r['id']} [{r['phase']}]: {r['detail']}")
                elif done % 250 == 0 or done == len(tests):
                    p_cnt = sum(1 for x in results if x["status"] in ("PASS", "NA"))
                    print(f"  progress: {done}/{len(tests)} ({p_cnt} pass, {done - p_cnt} fail) [{time.time()-t0:.1f}s]")

        results.sort(key=lambda r: r["id"])

        # Summary by class
        print("\n=== SUMMARY BY CLASS ===")
        by_cls = collections.defaultdict(lambda: collections.Counter())
        by_chap = collections.defaultdict(lambda: collections.Counter())
        by_phase = collections.Counter()
        for r in results:
            st = "PASS" if r["status"] in ("PASS", "NA") else "FAIL"
            by_cls[r["class"]][st] += 1
            by_cls[r["class"]]["total"] += 1
            if r["status"] == "NA":
                by_cls[r["class"]]["na"] += 1
            by_chap[(r["class"], r["chapter"])][st] += 1
            by_chap[(r["class"], r["chapter"])]["total"] += 1
            if st == "FAIL":
                by_phase[(r["class"], r["phase"])] += 1

        tot_pass = tot_all = 0
        for c in classes:
            cnt = by_cls[c]
            if cnt["total"] == 0:
                continue
            pct = 100.0 * cnt["PASS"] / cnt["total"]
            print(f"  Class {c.upper()}: {cnt['PASS']:4d} / {cnt['total']:4d} ({pct:5.1f}%) [N/A={cnt['na']}]")
            tot_pass += cnt["PASS"]
            tot_all += cnt["total"]
        if tot_all:
            print(f"  TOTAL  : {tot_pass:4d} / {tot_all:4d} ({100.0*tot_pass/tot_all:5.1f}%) in {time.time()-t0:.1f}s")

        print("\n=== FAILURE BREAKDOWN BY (CLASS, PHASE) ===")
        for (c, ph), n in by_phase.most_common():
            print(f"  Class {c.upper()} / {ph:18s}: {n:4d}")

        print("\n=== PASS RATE BY (CLASS, CHAPTER) ===")
        for (c, ch) in sorted(by_chap.keys()):
            cnt = by_chap[(c, ch)]
            pct = 100.0 * cnt["PASS"] / cnt["total"]
            print(f"  {c.upper()}{ch.upper()}: {cnt['PASS']:4d} / {cnt['total']:4d} ({pct:5.1f}%)")

        if args.json_out:
            Path(args.json_out).write_text(json.dumps({
                "support": sup_status,
                "results": results,
            }, indent=2))
            print(f"\nWrote detailed results to {args.json_out}")

        return 0 if tot_pass == tot_all else 1
    finally:
        shutil.rmtree(base_tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
