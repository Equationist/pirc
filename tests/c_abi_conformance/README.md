# PIR v0.3.0 Milestone 7 — C ABI Conformance Suite (`aarch64-apple-darwin`)

Bidirectional **C <-> PIR** ABI conformance tests compiled against host Apple `clang`
on macOS ARM64 (`aarch64-apple-darwin`). Every suite tests both directions:

* **C -> PIR:** Apple `clang` (`-O2`) calls `export func ... cc "c"` implemented in PIR.
* **PIR -> C:** PIR calls `extern func ... cc "c"` implemented in C.

## Suites Included

| Suite | Coverage & Apple ARM64 Divergences Tested |
| :--- | :--- |
| `01_structs_1_to_64b` | `1, 2, 3, 4, 5, 7, 8, 12, 16, 17, 24, 32, 64B` structs (`byval` + `retbuf`), copy isolation, 7 GP regs + 16B struct all-or-nothing spill |
| `02_hfa_and_hva_structs` | HFA (`2/3/4 x f32`, `2/4 x f64`, nested HFA), HVA (`2/4 x v4f32`), >4-element non-HFA/non-HVA (`5 x f32`, `5 x v4f32`), 6 FP regs + 4-float HFA spill |
| `03_mixed_packed_unions` | Mixed int/float structs (`{i32,f32}`, `{i64,f64}` GP not HFA, 24B padded), `packed {i8,i64}` (9B), `union` (8B, 16B, 24B) |
| `04_i128_and_simd_v128` | Direct `i128`, **Darwin odd-register-pair rule** `(i32, i128, i64, i128)` (`x1:x2`, `x4:x5`), 7 GP + `i128` 16B stack spill, `v4f32`/`v2f64`/`v4i32`/`v16i8` (>8 SIMD args) |
| `05_darwin_stack_varargs` | Apple ARM64 stack-only varargs: PIR -> `snprintf` & custom C varargs (`i32`, `i64`, `f64`, `byval` structs); C -> PIR `...` (`va_arg`, `va_arg_buf`, `va_copy`, `vsnprintf` forwarding) |
| `06_darwin_stack_packing` | 12 GP + 12 FP args; **Darwin sub-8-byte stack packing** (`8 x i64` + `i8, i8, i16, i32, i8, i64`); negative `i8`/`i16` values across C <-> PIR |
| `07_fn_ptrs_and_callbacks` | PIR comparator passed to host libc `qsort()`; C fn ptr invoked via PIR `call_indirect @Sig` (`byval`/`retbuf`); PIR returning a fn ptr to C |
| `08_weak_sections_and_tls` | `weak` override (C strong beats PIR weak), `weak` fallback (PIR only), `weak` dedup (both weak), Mach-O `section "__DATA,__pir_reg"` verified via `getsectiondata()`, `pthread` Darwin TLS isolation |

## Running

```bash
# Default: builds with ./build/pirc at both -O0 and -O2 against host clang -O2
./tests/c_abi_conformance/run_c_abi.sh

# Custom compiler path:
PIRC=./build/pirc-san ./tests/c_abi_conformance/run_c_abi.sh
```
