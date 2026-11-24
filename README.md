# pirc — Portable Intermediate Representation (PIR) Reference Compiler

`pirc` is a strict C99 reference compiler, verifier, optimizer, LIR interpreter/sanitizer, and native code generator for the **Portable Intermediate Representation (PIR)** v0.3.0 (`docs/PIR.md`).

## Features

- **Strict C99, Zero External Dependencies:** Compiles cleanly with `-std=c99 -Wall -Wextra -Werror -Wswitch-enum -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes` using only the C99 standard library and minimal POSIX APIs.
- **Two-Tier Progressive IR (`HIR` → `LIR`):** Single human-readable text format with strict HIR and LIR verifiers (`--verify-hir`, `--verify-lir`).
- **Progressive Target Lowering:** Lowers structured exceptions (shadow-stack TLS unwind), `va_list`/varargs, platform C ABIs (`aarch64-apple-darwin`, `aarch64-unknown-linux-gnu`, `x86_64-apple-darwin`, `x86_64-unknown-linux-gnu`, `x86_64-pc-windows-msvc`), aggregates, `i128`, and fixed-width SIMD.
- **SSA Optimizer (`-O0` / `-O1` / `-O2`):** CFG simplification, `mem2reg`, dominator-scoped GVN + constant folding, LICM, DSE, DCE, inlining, and loop vectorization.
- **Reference LIR Interpreter (`--interp`):** Provenance- and alignment-checking interpreter used as a differential oracle against `-O2` and native codegen.
- **Native Backends:** ARM64 (`aarch64`) and x86-64 (`x86_64`) instruction selection, linear-scan register allocation, frame layout, and assembly emission.
- **Frontends:**
  - `frontends/slimcc-pir/`: PIR HIR C frontend adapted from [`slimcc`](https://github.com/fuhsnn/slimcc) with 3-stage self-hosting bootstrap support.
  - `frontends/ada83-pir/`: Ada 83 → DIANA → PIR frontend (`diana2pir` + C99 runtime) using the vendored [`diana`](https://github.com/reznikmm/diana) Ada 83 frontend in `frontends/diana/`.

## Quick Start

```sh
# Build release binary (build/pirc)
make

# Run full sanitizer test suite (ASan/UBSan + verify-every-pass)
make test-san

# Run release test suite (M0-M8)
make test

# Build C frontend and run 3-stage self-hosting bootstrap
make slimcc-pir
make bootstrap

# Build Ada 83 frontend and run Ada 83 tests (requires GNAT/gprbuild + built DIANA)
make ada83-pir
make test-ada83
```

## Repository Layout

- `docs/PIR.md` — PIR v0.3.0 specification
- `IMPLEMENTATION_PLAN.md` — Architecture and milestone plan
- `src/` — `pirc` compiler (`common/`, `ir/`, `analysis/`, `target/`, `lower/`, `opt/`, `interp/`, `backend/`)
- `runtime/` — Minimal native shadow-stack exception runtime (`pir_rt.c`)
- `frontends/slimcc-pir/` — C → PIR frontend (adapted from `slimcc`)
- `frontends/ada83-pir/` — Ada 83 → PIR frontend (`diana2pir`) and runtime
- `frontends/diana/` — Vendored upstream DIANA Ada 83 frontend
- `tests/` — Unit, round-trip, negative verifier, interpreter, lowering, optimization, C ABI conformance, bootstrap, Ada 83, and benchmark suites

## Licensing

Original `pirc` source code, runtime, `ada83-pir` frontend, documentation, and test suites are licensed under the **BSD 3-Clause License** (see [`LICENSE`](./LICENSE)).

Third-party components retain their original licenses:
- `frontends/slimcc-pir/`: Upstream [`slimcc`](https://github.com/fuhsnn/slimcc) is licensed under the **MIT License** (see [`frontends/slimcc-pir/LICENSE`](./frontends/slimcc-pir/LICENSE) and [`frontends/slimcc-pir/README_PIR.md`](./frontends/slimcc-pir/README_PIR.md)).
- `frontends/diana/`: Vendored from [`reznikmm/diana`](https://github.com/reznikmm/diana), released into the public domain under **The Unlicense** (see [`frontends/diana/LICENSE`](./frontends/diana/LICENSE)).
