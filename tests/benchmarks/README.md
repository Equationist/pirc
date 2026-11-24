# PIR v0.3.0 Milestone 7 — Performance Gate Benchmarks (`aarch64-apple-darwin`)

Compares `pirc --target aarch64-apple-darwin -O2` against host Apple `clang -O2` across five
representative kernels from the M7 implementation plan:

| Benchmark | Workload | Primary Compiler Subsystem Stressed |
| :--- | :--- | :--- |
| `01_matmul` | 128x128 `f32` matrix multiply (`C = A * B`) | Nested loops, LICM, GVN, FP register allocation |
| `02_quicksort` | In-place Lomuto quicksort over 65,536 `i32` elements | Recursion, memory addressing modes, branching |
| `03_wyhash_i128` | 2,000,000 iterations of 64x64->128 `wyhash` `wymix` | `LegalizeI128`, `mul_wide_u.i64`, `umulh` codegen |
| `04_simd_clamp` | 4-wide `v4f32` scale + clamp over 262,144 floats (x64 reps) | 128-bit NEON SIMD (`fmul`, `fcmp_ogt`, `select`), `inbounds` ptr math |
| `05_switch_vm` | 2,000,000-step bytecode VM (`switch.i32` with 8 opcodes) | Jump-table / switch lowering, SSA block-argument register allocation |

## Gate Criterion

1. **Bit-Exact Correctness:** Every benchmark's `impl.pir` must produce the **exact same 64-bit checksum** as `ref.c`.
2. **Throughput Gate:** `pirc -O2` must achieve **>= 80%** of host `clang -O2` throughput (`c_time / pir_time >= 0.80`).

## Running

```bash
./tests/benchmarks/run_benchmarks.sh
```
