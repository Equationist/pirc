#!/usr/bin/env python3
# Copyright (c) 2026 Ummon Karpe
# SPDX-License-Identifier: BSD-3-Clause

"""
ISO/IEC 9899:2018 (C17) Conformance Test Suite runner for slimcc-pir + pirc.

Inspired by the ACVC 1.11 runner (tests/acvc83/run_acvc83.py):
  - Class A (atests/): Compile-only conformance & translation-limits tests
  - Class B (btests/): Negative compile-time syntax & constraint-violation tests
  - Class C (ctests/): Executable runtime conformance tests (incl. optional N/A)
  - Class L (ltests/): Multi-TU compilation, linkage, TLS, atomics, & ABI interop
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SUITE_DIR = Path(__file__).resolve().parent
DEFAULT_PIRC = ROOT / "build" / "pirc"
DEFAULT_SLIMCC = ROOT / "frontends" / "slimcc-pir" / "slimcc-pir"

CLASS_DIRS = {
    "a": "atests",
    "b": "btests",
    "c": "ctests",
    "l": "ltests",
}

DIRECTIVE_RE = re.compile(r"/\*\s*C(?:17|99)-([A-Z0-9_-]+):\s*(.*?)\s*\*/")
SECTION_RE = re.compile(r"^[abcl]([0-9]+)_")


def parse_directives(paths: list[Path]) -> dict[str, list[str]]:
    dirs: dict[str, list[str]] = collections.defaultdict(list)
    for p in paths:
        try:
            head = p.read_text(errors="replace")[:4096]
        except OSError:
            continue
        for m in DIRECTIVE_RE.finditer(head):
            dirs[m.group(1)].append(m.group(2).strip())
    return dirs


def infer_section(name: str) -> str:
    m = SECTION_RE.match(name)
    if not m:
        return "other"
    num = m.group(1)
    if num == "7":
        return "7"
    if len(num) >= 2:
        return f"{num[0]}.{num[1:]}"
    return num


def discover_tests(
    classes: list[str],
    sections: set[str] | None,
    flt: re.Pattern[str] | None,
) -> list[dict]:
    tests: list[dict] = []
    for cls in classes:
        cdir = SUITE_DIR / CLASS_DIRS[cls]
        if not cdir.exists():
            continue
        if cls == "l":
            for entry in sorted(cdir.iterdir()):
                if not entry.is_dir():
                    continue
                srcs = sorted(entry.glob("*.c"))
                if not srcs:
                    continue
                tid = entry.name
                sec = infer_section(tid)
                if sections and sec not in sections:
                    continue
                if flt and not flt.search(tid):
                    continue
                tests.append({
                    "id": tid,
                    "class": cls,
                    "section": sec,
                    "files": srcs,
                    "dir": entry,
                    "directives": parse_directives(srcs),
                })
        else:
            for p in sorted(cdir.rglob("*.c")):
                tid = p.stem
                sec = infer_section(tid)
                if sections and sec not in sections:
                    continue
                if flt and not flt.search(tid):
                    continue
                tests.append({
                    "id": tid,
                    "class": cls,
                    "section": sec,
                    "files": [p],
                    "dir": p.parent,
                    "directives": parse_directives([p]),
                })
    tests.sort(key=lambda t: (t["class"], t["id"]))
    return tests


def run_cmd(cmd: list[str], cwd: Path, timeout: float) -> tuple[int, str, str]:
    try:
        p = subprocess.run(
            cmd,
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
        return (
            p.returncode,
            p.stdout.decode("utf-8", errors="replace"),
            p.stderr.decode("utf-8", errors="replace"),
        )
    except subprocess.TimeoutExpired:
        return -9, "", "TIMEOUT"


def build_compile_cmd(
    compiler: str,
    slimcc: Path,
    pirc: Path,
    target: str,
    std: str,
    opt: str,
    src: Path,
    out: Path,
    include_dirs: list[Path],
    extra_flags: list[str],
    mode: str,
) -> list[str]:
    if compiler == "slimcc":
        cmd = [str(slimcc), f"--pirc={pirc}", f"--target={target}", f"-std={std}", opt]
        for inc in include_dirs:
            cmd.extend(["-I", str(inc)])
        cmd.extend(extra_flags)
        if mode == "pir":
            cmd.extend(["--emit-pir", "-S", str(src), "-o", str(out)])
        elif mode == "obj":
            cmd.extend(["-c", str(src), "-o", str(out)])
        else:
            cmd.extend([str(src), "-o", str(out)])
        return cmd

    cmd = [compiler, f"-std={std}", "-pedantic-errors", opt]
    if "clang" in Path(compiler).name:
        cmd.append("-Wno-unused-function")
    for inc in include_dirs:
        cmd.extend(["-I", str(inc)])
    cmd.extend(extra_flags)
    if mode == "obj":
        cmd.extend(["-c", str(src), "-o", str(out)])
    else:
        cmd.extend([str(src), "-o", str(out)])
    return cmd


def first_err_line(text: str) -> str:
    for line in text.splitlines():
        s = line.strip()
        if s:
            return s[:260]
    return ""


def run_single_test(
    test: dict,
    compiler: str,
    slimcc: Path,
    pirc: Path,
    target: str,
    std: str,
    opt: str,
    timeout: float,
    verify_pir: bool,
) -> dict:
    tid = test["id"]
    cls = test["class"]
    sec = test["section"]
    files: list[Path] = test["files"]
    dirs = test["directives"]

    extra_cflags: list[str] = []
    for f in dirs.get("CFLAGS", []):
        extra_cflags.extend(shlex.split(f))
    if compiler != "slimcc":
        for f in dirs.get("REF-CFLAGS", []):
            extra_cflags.extend(shlex.split(f))

    extra_ldflags: list[str] = []
    for f in dirs.get("LDFLAGS", []):
        extra_ldflags.extend(shlex.split(f))

    expect_list = dirs.get("EXPECT", [])
    expect = expect_list[0].lower() if expect_list else ""

    include_dirs = [SUITE_DIR, test["dir"]]
    work = Path(tempfile.mkdtemp(prefix=f"c17_{tid}_"))
    try:
        if verify_pir and compiler == "slimcc" and cls in ("a", "c", "l"):
            for idx, src in enumerate(files):
                if "clang_tu" in src.name:
                    continue
                pir_out = work / f"tu_{idx}.pir"
                cmd = build_compile_cmd(
                    compiler, slimcc, pirc, target, std, opt, src, pir_out,
                    include_dirs, extra_cflags, mode="pir",
                )
                rc, out, err = run_cmd(cmd, work, timeout)
                if rc != 0:
                    return {
                        "id": tid, "class": cls, "section": sec, "opt": opt,
                        "status": "FAIL", "phase": "emit_pir",
                        "detail": first_err_line(err or out),
                    }
                vrc, vout, verr = run_cmd([str(pirc), "--verify-hir", str(pir_out)], work, timeout)
                if vrc != 0:
                    return {
                        "id": tid, "class": cls, "section": sec, "opt": opt,
                        "status": "FAIL", "phase": "verify_hir",
                        "detail": first_err_line(verr or vout),
                    }

        # --- Class A: compile-only ---
        if cls == "a":
            obj = work / f"{tid}.o"
            cmd = build_compile_cmd(
                compiler, slimcc, pirc, target, std, opt, files[0], obj,
                include_dirs, extra_cflags, mode="obj",
            )
            rc, out, err = run_cmd(cmd, work, timeout)
            if rc == 0:
                return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "PASS", "phase": "ok", "detail": ""}
            phase = "timeout" if rc == -9 and err == "TIMEOUT" else "compile"
            return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": phase, "detail": first_err_line(err or out)}

        # --- Class B: compile-reject ---
        if cls == "b":
            obj = work / f"{tid}.o"
            cmd = build_compile_cmd(
                compiler, slimcc, pirc, target, std, opt, files[0], obj,
                include_dirs, extra_cflags, mode="obj",
            )
            rc, out, err = run_cmd(cmd, work, timeout)
            if rc == -9 and err == "TIMEOUT":
                return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": "timeout", "detail": "TIMEOUT"}
            if rc < 0:
                return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": "compiler_crash", "detail": f"signal {-rc}: {first_err_line(err or out)}"}
            if rc == 0:
                return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": "missed_error", "detail": f"illegal {std.upper()} program accepted"}
            return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "PASS", "phase": "rejected", "detail": ""}

        # --- Class C: executable ---
        if cls == "c":
            obj = work / f"{tid}.o"
            cmd = build_compile_cmd(
                compiler, slimcc, pirc, target, std, opt, files[0], obj,
                include_dirs, extra_cflags, mode="obj",
            )
            rc, out, err = run_cmd(cmd, work, timeout)
            if rc != 0:
                phase = "timeout" if rc == -9 and err == "TIMEOUT" else "compile"
                return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": phase, "detail": first_err_line(err or out)}
            exe = work / f"{tid}.bin"
            arch = "arm64" if target.startswith("aarch64") else "x86_64"
            link_cmd = ["cc", "-arch", arch, str(obj), "-lm", *extra_ldflags, "-o", str(exe)]
            lrc, lout, lerr = run_cmd(link_cmd, work, timeout)
            if lrc != 0:
                return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": "link", "detail": first_err_line(lerr or lout)}
            run_argv = ["arch", "-x86_64", str(exe)] if arch == "x86_64" else [str(exe)]
            rrc, rout, rerr = run_cmd(run_argv, work, timeout)
            if rrc == -9 and rerr == "TIMEOUT":
                return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": "timeout", "detail": "runtime TIMEOUT"}
            if expect == "exit-zero":
                if rrc == 0:
                    return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "PASS", "phase": "ok", "detail": ""}
                return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": "runtime_failed", "detail": f"rc={rrc} {first_err_line(rout or rerr)}"}
            if rrc < 0:
                return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": "runtime_crash", "detail": f"signal {-rrc}: {first_err_line(rerr or rout)}"}
            if rrc != 0 or "**** " in rout:
                fail_line = next((ln.strip() for ln in rout.splitlines() if "   * FAIL" in ln), first_err_line(rout or rerr))
                return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": "runtime_failed", "detail": f"rc={rrc} {fail_line[:240]}"}
            if "++++ " in rout and " NOT-APPLICABLE " in rout:
                return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "NA", "phase": "not_applicable", "detail": ""}
            if "==== " in rout and " PASSED " in rout:
                return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "PASS", "phase": "ok", "detail": ""}
            return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": "no_banner", "detail": first_err_line(rout or rerr)}

        # --- Class L: multi-TU ---
        objs: list[Path] = []
        for idx, src in enumerate(files):
            obj = work / f"tu_{idx}.o"
            tu_compiler = "clang" if ("clang_tu" in src.name and compiler == "slimcc") else compiler
            cmd = build_compile_cmd(
                tu_compiler, slimcc, pirc, target, std, opt, src, obj,
                include_dirs, extra_cflags, mode="obj",
            )
            rc, out, err = run_cmd(cmd, work, timeout)
            if rc != 0:
                return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": "compile", "detail": f"{src.name}: {first_err_line(err or out)}"}
            objs.append(obj)

        exe = work / f"{tid}.bin"
        arch = "arm64" if target.startswith("aarch64") else "x86_64"
        link_cmd = ["cc", "-arch", arch, *[str(o) for o in objs], "-lm", *extra_ldflags, "-o", str(exe)]
        lrc, lout, lerr = run_cmd(link_cmd, work, timeout)

        if expect == "fail-link" or "_neg_" in tid:
            if lrc != 0:
                return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "PASS", "phase": "link_rejected", "detail": ""}
            return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": "missed_link_error", "detail": "illegal multi-TU program linked cleanly"}

        if lrc != 0:
            return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": "link", "detail": first_err_line(lerr or lout)}

        run_argv = ["arch", "-x86_64", str(exe)] if arch == "x86_64" else [str(exe)]
        rrc, rout, rerr = run_cmd(run_argv, work, timeout)
        if rrc == 0 and "==== " in rout and " PASSED " in rout:
            return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "PASS", "phase": "ok", "detail": ""}
        fail_line = next((ln.strip() for ln in rout.splitlines() if "   * FAIL" in ln), first_err_line(rout or rerr))
        return {"id": tid, "class": cls, "section": sec, "opt": opt, "status": "FAIL", "phase": "runtime_failed" if rrc >= 0 else "runtime_crash", "detail": f"rc={rrc} {fail_line[:240]}"}
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main() -> int:
    ap = argparse.ArgumentParser(description="Run ISO C17 Conformance Suite on slimcc-pir + pirc")
    ap.add_argument("--classes", default="a,b,c,l", help="Comma-separated classes (a,b,c,l)")
    ap.add_argument("--sections", default="", help="Comma-separated ISO C sections (e.g. 5.1,6.5,6.7,7)")
    ap.add_argument("--filter", default="", help="Regex filter on test ID")
    ap.add_argument("--std", default="c17", choices=["c17", "c11", "c99"], help="C standard mode (default: c17)")
    ap.add_argument("--opt", default="O2", choices=["O0", "O1", "O2", "-O0", "-O1", "-O2", "all"], help="Optimization level")
    ap.add_argument("--target", default="aarch64-apple-darwin", help="Target triple")
    ap.add_argument("--compiler", default="slimcc", help="'slimcc' or reference host C compiler (e.g. 'clang')")
    ap.add_argument("--slimcc", default=str(DEFAULT_SLIMCC), help="Path to slimcc-pir binary")
    ap.add_argument("--pirc", default=str(DEFAULT_PIRC), help="Path to pirc binary")
    ap.add_argument("--verify-pir", action="store_true", help="Also verify emitted PIR HIR with pirc --verify-hir")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="Parallel worker count")
    ap.add_argument("--timeout", type=float, default=15.0, help="Per-command timeout in seconds")
    ap.add_argument("--json", dest="json_out", default="", help="Write JSON report to file")
    ap.add_argument("--verbose", action="store_true", help="Print every failure detail")
    args = ap.parse_args()

    classes = [c.strip().lower() for c in args.classes.split(",") if c.strip() in CLASS_DIRS]
    sections = {s.strip() for s in args.sections.split(",") if s.strip()} or None
    flt = re.compile(args.filter, re.IGNORECASE) if args.filter else None
    slimcc = Path(args.slimcc).resolve()
    pirc = Path(args.pirc).resolve()
    if args.opt == "all":
        opt_levels = ["-O0", "-O1", "-O2"]
    else:
        opt_levels = [args.opt if args.opt.startswith("-") else f"-{args.opt}"]

    tests = discover_tests(classes, sections, flt)
    work_items = [(t, opt) for opt in opt_levels for t in tests]
    t0 = time.time()
    print(
        f"=== C17 Conformance Runner: {len(tests)} tests x {len(opt_levels)} opt ({len(work_items)} runs), "
        f"compiler={args.compiler}, std={args.std}, target={args.target}, opt={args.opt}, verify_pir={args.verify_pir}, jobs={args.jobs} ==="
    )

    results: list[dict] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [
            ex.submit(
                run_single_test, t, args.compiler, slimcc, pirc,
                args.target, args.std, opt, args.timeout, args.verify_pir,
            )
            for (t, opt) in work_items
        ]
        for fut in concurrent.futures.as_completed(futs):
            r = fut.result()
            results.append(r)
            if args.verbose and r["status"] == "FAIL":
                print(f"  FAIL [{r['opt']}] {r['id']} ({r['phase']}): {r['detail']}")

    results.sort(key=lambda r: (r["opt"], r["class"], r["id"]))

    by_cls = collections.defaultdict(lambda: collections.Counter())
    by_sec = collections.defaultdict(lambda: collections.Counter())
    by_phase = collections.Counter()
    for r in results:
        st = "PASS" if r["status"] in ("PASS", "NA") else "FAIL"
        by_cls[r["class"]][st] += 1
        by_cls[r["class"]]["total"] += 1
        if r["status"] == "NA":
            by_cls[r["class"]]["na"] += 1
        by_sec[(r["class"], r["section"])][st] += 1
        by_sec[(r["class"], r["section"])]["total"] += 1
        if st == "FAIL":
            by_phase[(r["class"], r["phase"])] += 1

    print("\n=== SUMMARY BY CLASS ===")
    tot_pass = 0
    tot_all = 0
    for c in classes:
        cnt = by_cls[c]
        if cnt["total"] == 0:
            continue
        pct = 100.0 * cnt["PASS"] / cnt["total"]
        na_str = f" [N/A={cnt['na']}]" if cnt["na"] else ""
        print(f"  Class {c.upper()}: {cnt['PASS']:4d} / {cnt['total']:4d} ({pct:5.1f}%){na_str}")
        tot_pass += cnt["PASS"]
        tot_all += cnt["total"]
    if tot_all:
        print(f"  TOTAL  : {tot_pass:4d} / {tot_all:4d} ({100.0 * tot_pass / tot_all:5.1f}%) in {time.time() - t0:.1f}s")

    if by_phase:
        print("\n=== FAILURE BREAKDOWN BY (CLASS, PHASE) ===")
        for (c, ph), n in by_phase.most_common():
            print(f"  Class {c.upper()} / {ph:18s}: {n:4d}")

    print("\n=== PASS RATE BY (CLASS, ISO C SECTION) ===")
    for (c, sec) in sorted(by_sec.keys()):
        cnt = by_sec[(c, sec)]
        pct = 100.0 * cnt["PASS"] / cnt["total"]
        print(f"  {c.upper()} §{sec:5s}: {cnt['PASS']:4d} / {cnt['total']:4d} ({pct:5.1f}%)")

    if args.json_out:
        Path(args.json_out).write_text(json.dumps({
            "compiler": args.compiler,
            "std": args.std,
            "target": args.target,
            "opt_levels": opt_levels,
            "verify_pir": args.verify_pir,
            "total": tot_all,
            "passed": tot_pass,
            "results": results,
        }, indent=2))
        print(f"\nWrote detailed results to {args.json_out}")

    return 0 if tot_pass == tot_all else 1


if __name__ == "__main__":
    sys.exit(main())
