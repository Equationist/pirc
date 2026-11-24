# AGENTS.md — Coding Agent Operating Guide for `pirc`

This repository implements **`pirc`**, the strict C99 reference compiler, LIR interpreter/sanitizer, and self-hosting toolchain for the **Portable Intermediate Representation (PIR)**.

## 1. Source of Truth (Read Before Writing Code)
1. **`docs/PIR.md`** — Frozen v0.3.0 specification. Never invent syntax, opcodes, or flags outside this document. If you discover a genuine spec ambiguity, document it clearly and raise it rather than guessing silently.
2. **`IMPLEMENTATION_PLAN.md`** — Canonical architecture, data structures, pass ordering, and milestone gates (`M0` through `M9`). Follow milestones strictly in order.

---

## 2. Mandatory C99 Coding Standards

Every `.c` and `.h` file in `src/` must adhere to these rules without exception:

1. **Strict Standard & Warnings:** Code must compile cleanly under:
   ```sh
   -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wswitch-enum -Wconversion -Wshadow -Wstrict-prototypes -Wmissing-prototypes
   ```
   Never add `#pragma` suppressions or `default:` branches in `switch (inst->op)` / `switch (ty.kind)` statements—let `-Wswitch-enum` catch missing cases at compile time.
2. **Zero External Dependencies:** Use only the C99 standard library (plus minimal POSIX headers for file I/O / subprocess execution in `main.c` and test runners). No parser generators, no external libraries.
3. **Arena-Only IR Allocation:** Allocate IR strings, arrays, and nodes from the owning `Module`/`Function` `Arena` or a pass-local scratch `Arena`. Do not mix `malloc`/`free` with arena pointers.
4. **Index Handles Over Pointers:** Store `ValueId`, `BlockId`, `InstId`, `TypeId`, `SigId`, and `GlobalId` (`uint32_t`). Never retain pointers into dynamic arrays across push/resize operations.
5. **Determinism Everywhere:** Iteration order must never depend on raw pointer addresses (`uintptr_t` hashing is forbidden). Use interned `SymId` or `uint32_t` IDs for hash tables and sort output deterministically so `pirc` passes the 3-stage byte-identical bootstrap gate.
6. **Clean Under Sanitizers:** `make test-san` (`-fsanitize=address,undefined`) must pass with zero leaks, zero signed-overflow UB, zero unaligned host reads (use `memcpy` for type-punning/unaligned reads in C99).

---

## 3. Non-Negotiable Compiler Invariants

1. **Verify-Every-Pass Rule:** In debug/sanitizer builds (`-DPIR_VERIFY_ALL=1`), every lowering and optimization pass must end with `verify_module(...)`. Never bypass verification or defer fixing broken SSA/dominance to a later pass.
2. **Entry Block Invariant (`PIR.md` §7.1 #3):** `blocks[0]` (`.entry`) must have `param_count == 0` and zero predecessors. Function parameters are defined on `Function::params`, not as entry-block parameters.
3. **Strictly Scalar/Vector SSA:** No SSA `ValueId` may ever hold a `TY_AGGREGATE` or `TY_VA_LIST`. Aggregates live in memory and cross C ABI boundaries via `byval(@T)` and `retbuf(@T)` only.
4. **Semantic Honesty:**
   * Never delete or hoist reachable `trap`, `[volatile]` memory ops, or atomic operations/fences.
   * Never assume no-overflow unless `[unsafe_s]` / `[unsafe_u]` / `[unsafe]` is explicitly set.
   * Never reassociate floating-point math unless `[fast]` is explicitly set.
   * Never fold a `[trap]`/`[trap_s]`/`[trap_u]` arithmetic operation that overflows or divides by zero into a normal constant (fold it to `trap` or leave it un-folded).
5. **Zero Silent ABI Fallbacks:** If `abi_*.c` or `lower_cabi.c` encounters an unsupported target triple or aggregate shape, emit a hard diagnostic error and exit non-zero. Never guess register assignment or struct padding.
6. **Assembly-Agnostic `MFunc`:** Backend instruction selection (`*_isel.c`) and register allocation (`regalloc.c`) must produce structured `MInst` structs only—never formatted assembly strings. Both `*_asm.c` and `obj/{macho64,elf64}.c` must consume the identical post-regalloc `MFunc`.
7. **Differential Oracle Discipline:** Whenever `-O2` or a native backend disagrees with `pirc --interp -O0`, first minimize the reproducer into a permanent `.pir` regression test under `tests/`, verify it fails, then fix the root cause.

---

## 4. Milestone Execution Protocol

When assigned a milestone (e.g., `M1`, `M3`, `M6`):

1. **Inspect State:** Read `IMPLEMENTATION_PLAN.md` for the milestone's exact deliverables, pass ordering, and verification gate. Check existing tests and run `make test-san` to confirm a green baseline.
2. **Write Tests Alongside Code:** Add unit tests and `.pir` fixtures for both happy paths and negative edge cases *before* considering a component complete.
3. **Run Full Gates:**
   ```sh
   make clean && make test-san
   make clean && make test
   ```
   For `M5+`, also run differential `-O0` vs `-O2` checks across all `tests/interp_exec/*.pir`.  
   For `M6+`, also run native ARM64 macOS (`aarch64-apple-darwin`) execution checks comparing compiled binaries against `pirc --interp`.  
   For `M7.5+`, run `make bootstrap` on macOS ARM64 and confirm zero diff between Stage 2 and Stage 3 assembly.  
   For `M8+`, also run x86-64 execution checks (via Rosetta 2 `arch -x86_64` on `x86_64-apple-darwin` and/or `x86_64-unknown-linux-gnu`).
4. **Do Not Scope-Creep:** Do not start the next milestone or add out-of-spec features (inline assembly, GC safepoints, arbitrary bit-widths, scalable vectors) while current gates are incomplete.

---

## 5. High-Risk Pitfalls Checklist

Watch closely for these specific compiler traps during implementation:

* **Signedness on Overflow Flags:** `add`/`sub`/`mul` wrap identically for signed and unsigned (`[wrap]`), but `[trap_s]` vs `[trap_u]` and `[unsafe_s]` vs `[unsafe_u]` have completely different overflow conditions (`0x7fffffff + 1` vs `0xffffffff + 1`).
* **`invoke` Dominance:** `invoke` has no `%res =` assignment; `ret` is only valid in the `then` edge arguments and `exn` only in the `unwind` edge arguments.
* **Parallel Copy Cycles in RegAlloc:** Block arguments represent *simultaneous* assignment (`jmp .loop(%b, %a)` swaps `%a` and `%b`). Never emit sequential moves without cycle detection and a dedicated scratch register (`x16`/`v31` on ARM64; `r11`/`xmm15` on x64).
* **Critical Edges:** Always run `edge_split` before register allocation so conditional branches (`br`, `switch`) targeting a block with parameters have a dedicated empty edge block to hold parallel copies.
* **Darwin ARM64 (`aarch64-apple-darwin`) vs. Linux AAPCS64 Divergences:**
  * **`i128` register alignment:** Both require 16-byte alignment in memory, but **Linux AAPCS64** requires `i128` to start in an **even** register pair (`x0:x1`, `x2:x3`, `x4:x5`, `x6:x7`), skipping an odd register if necessary; **Darwin ARM64** does **not** require even-register alignment (`x1:x2` is valid).
  * **Reserved `x18`, Frame Pointer `x29`, & Stack Probing:** On Darwin ARM64, `x18` is reserved by the OS and must never be allocated or clobbered; keep the `x29`/`x30` frame pointer chain intact and `sp` 16-byte aligned at all times. For stack frames $> 4095$ bytes, materialize the offset (overflows `sub sp, sp, #imm12`), and for frames $\ge 16\text{ KB}$ emit a Darwin stack probe via `___chkstk_darwin` (size in `x9`) before decrementing `sp`.
  * **Sub-word stack args & Mach-O conventions:** Darwin packs non-variadic sub-word stack args to natural alignment (vs. 8-byte slots on Linux). Mach-O exported/extern/weak symbols carry a leading `_` (`_main`, `_printf`, `___divti3`, `___pir_exc_head`), local labels use `L...`, PC-relative addressing uses `@PAGE`/`@PAGEOFF` (and `@GOTPAGE`/`@GOTPAGEOFF` for externs), and ELF-style custom section names (e.g., `section ".rodata.vt"`) must be mapped to Mach-O `SEGMENT,section` syntax (`__TEXT,__const`, etc.) rather than emitted verbatim.
  * **Darwin TLS (`__DATA,__thread_vars` TLV Thunks):** Never emit Linux `:tprel_hi12:` / `tpidr_el0` on macOS. Darwin TLS descriptors live in `__DATA,__thread_vars` (backed by `__DATA,__thread_data` / `__DATA,__thread_bss`) and are accessed via `adrp x0, _sym@TLVPAGE` + `add x0, x0, _sym@TLVPAGEOFF` followed by calling the TLV thunk (`ldr x8, [x0]; blr x8`), which returns the thread's variable address in `x0` and **clobbers caller-saved registers** (regalloc must model TLS address materialization as a call clobber).
* **`va_list` Passing Asymmetry:** Darwin ARM64 passes an 8-byte `char *` pointer and passes all variadic arguments **strictly on the stack** (never in `x0`–`x7`/`v0`–`v7`); Linux AAPCS64 passes a 32-byte struct by value; SysV x86-64 decays `va_list` (array of 1 struct) to a pointer when passed to `vprintf`; Win64 passes an 8-byte pointer value. Handle all targets in `lower_varargs` before `lower_cabi`.
* **`slimcc-pir` Self-Hosting:** Build with `BOOTSTRAP_NO_LDOUBLE` so `long double` aliases `f64` (already native on `aarch64-apple-darwin`), point `slimcc-pir` at macOS SDK headers via `xcrun --show-sdk-path`, keep `slimcc`'s `sizeof`/`_Alignof` layout constants locked to `pirc`'s `TargetSpec`, and ensure macOS SDK / system headers do not expand to unsupported GNU inline `asm`.