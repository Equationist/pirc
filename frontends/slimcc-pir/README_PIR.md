# `slimcc-pir` — PIR HIR Frontend Adaptation of `slimcc`

This directory contains a PIR v0.3.0 HIR frontend adaptation of [`slimcc`](https://github.com/fuhsnn/slimcc) (upstream commit `2388c39989886fee05df045b6a669d684aee3946`).

## License & Copyright

Original `slimcc` code is licensed under the MIT License (see [`LICENSE`](./LICENSE)):

- Copyright (c) 2019 Rui Ueyama
- Copyright (c) 2023-2026 Hsiang-Ying Fu

All original copyright notices and license terms are preserved intact.

## PIR Backend Adaptation

- `codegen_pir.c` replaces `codegen.c` when built via `Makefile.pir` (`-DSLIMCC_PIR -DBOOTSTRAP_NO_LDOUBLE`), walking `slimcc`'s AST (`Obj`, `Node`, `Type`, `Member`) and emitting portable PIR v0.3.0 HIR text.
- `platform/macos-arm64-pir.c` configures `aarch64-apple-darwin` target macros, macOS SDK include discovery, `pirc` lowering/codegen invocation, and native linking.
- Build with `make -f Makefile.pir` (or from repo root: `make slimcc-pir`).
