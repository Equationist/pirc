# Portable Intermediate Representation (PIR) — Master Specification

**Version:** 0.3.0

## 1. Architectural Philosophy & Machine Model

PIR is an opinionated, two-tiered Intermediate Representation stored in a single human-readable text format. It sits in the pragmatic middle ground between raw assembly and monolithic frameworks like LLVM:

* **Frontend-friendly:** Frontends emit portable **High-Level IR (HIR)** with structural layouts, symmetric platform C ABI support, varargs, `i128`, fixed-width SIMD, and structured unwinding—without encoding target-specific padding or calling conventions.
* **Backend-friendly:** Backends consume strictly lowered **Low-Level IR (LIR)** consisting only of flat memory offsets, scalar registers (plus opt-in SIMD), and simple control flow.
* **Middle-end optimizable:** A small, clean middle-end can reach $\ge 80\%$ of GCC `-O2` performance using standard SSA passes (`mem2reg`, GVN, LICM, DSE, DCE, linear-scan register allocation) enabled by explicit overflow semantics, `inbounds` pointers, `noalias`, and `nounwind`.

### 1.1. Opinionated Target Assumptions

To avoid carrying complexity for exotic hardware, PIR assumes every target architecture satisfies:

1. **8-bit bytes** (`1 byte = 8 bits`) and flat, byte-addressed memory.
2. **Power-of-two pointer widths:** strictly 32-bit or 64-bit pointers (declared by the target triple).
3. **Two's-complement signed integers** in power-of-two widths only (`i8`, `i16`, `i32`, `i64`, plus `i128` in HIR).
4. **IEEE 754 binary floating-point** (`f32` binary32, `f64` binary64). 80-bit x87 `long double` is intentionally omitted.
5. **Uniform byte order:** strictly little-endian or big-endian (per target triple; no mixed-endian).

### 1.2. Two-Tier Progressive Pipeline

Both HIR and LIR share one parser and AST, separated by strict verification gates:

1. **Frontend** emits HIR text.
2. **HIR Verifier** validates syntax, types, and SSA invariants.
3. **Middle-End** runs target-specific lowering/legalization passes and target-independent LIR optimizations.
4. **LIR Verifier** rejects any remaining HIR-only types, attributes, or instructions.
5. **Backend** emits target assembly (`.s`) or native object files (`.o`).

### 1.3. Explicit Non-Goals (Core v0.3)

The following are intentionally excluded from core PIR:

* Garbage collection safepoints, stack maps, or managed object models.
* Inline assembly blocks and target-specific constraint strings.
* Arbitrary non-power-of-two integer widths (e.g., `i1`, `i7`, `i24`) and struct bitfields (frontends lower bitfields via shifts/masks).
* Scalable/predicated vector architectures (ARM SVE, RISC-V RVV).
* Full DWARF type/variable metadata graphs (core PIR provides sticky source line tables only).

---

## 2. Lexical Structure & Formatting

### 2.1. Module Header

Every `.pir` file must begin with a `target` triple so layout, alignment, `va_list`, and ABI lowering are reproducible. Optional `file` directives register source paths for debug line tables:

```text
target "x86_64-unknown-linux-gnu"
file 0 "src/main.c"
file 1 "src/math.c"
```

### 2.2. Sigils & Identifiers

Sigils eliminate lookahead and symbol-table lookups during lexing and parsing:

* `%` — Local SSA values / virtual registers (`%tmp`, `%x_1`, `%0`).
* `@` — Module-level symbols: functions, globals, structural types, and signatures (`@main`, `@Point`).
* `.` — Basic block labels (`.entry`, `.loop_body`).
* **Barewords** — Keywords, opcodes, built-in types, and flags (`add`, `i32`, `trap_s`).

**Identifier Grammar:**

* Standard identifiers match `[A-Za-z_][A-Za-z0-9_]*`.
* Locals (`%`) may also be pure non-negative integers (`%0`, `%1`, `%42`).
* Global symbols (`@`) may be double-quoted to represent mangled or non-ASCII names (e.g., `@"_ZN4core3ops3Add3addE"`).

### 2.3. Delimiters & Comments

* **Statement Termination:** Instructions and declarations are terminated by **newlines**. Semicolons are not used. Multi-line `switch` tables and `data` blocks may span newlines inside `[ ... ]` or `{ ... }`.
* **Operands:** Separated by commas (`,`).
* **Scope:** Function bodies are enclosed in `{ ... }`. Blocks are strictly flat and cannot be nested.
* **Comments:** `//` starts a line comment extending to the newline.

### 2.4. Literals & Inline Immediates

Literals may appear directly as instruction operands (e.g., `add.i32 %x, 4`) whenever the instruction's opcode or signature uniquely determines the type. Where type cannot be inferred (specifically variadic arguments after `...`), literals and by-value structs must carry an explicit type annotation (`42: i32`, `%pt: byval(@Point)`).

* **Integers:** Decimal (`42`, `-1`), hexadecimal (`0x2A`, `-0x10`), binary (`0b1010`).
* **Floats:** Decimal with fractional or exponent part (`3.14`, `1.0`, `1e-5`), hexadecimal floats (`0x1.8p+1`), and special values (`nan`, `inf`, `-inf`).
* **Null Pointer:** `0` in any `ptr` operand position (or `const.ptr 0`).
* **Strings (in `data` / `global`):** Double-quoted UTF-8 bytes supporting standard escapes (`\n`, `\r`, `\t`, `\\`, `\"`, `\0`, `\xNN`).

---

## 3. Type System

**Strictly Scalar/Vector SSA Rule:** SSA values (`%reg`) may only hold **Core Machine Types**, **`i128` (HIR)**, or **Fixed-Width Vector Types**. Aggregate types (structs, unions, arrays, `va_list`) live exclusively in memory and are manipulated via pointers (`ptr`), `alloca`, `byval`, and `retbuf`.

### 3.1. Core Machine Types (HIR & LIR)

| Type | Description |
| :--- | :--- |
| `i8`, `i16`, `i32`, `i64` | Two's-complement integers (8, 16, 32, 64 bits). |
| `f32`, `f64` | IEEE 754 single- and double-precision floats. |
| `ptr` | Opaque untyped memory address (32-bit or 64-bit per `target`). |
| `void` | Absence of value (valid **only** as a function/signature return type). |

### 3.2. Extended Integer Type (HIR Only)

* `i128` — 128-bit integer. Legalized by the middle-end into `i64` pairs and runtime division helpers while preserving target C ABI alignment and register pairing rules.

### 3.3. Fixed-Width Vector Types (HIR; Capability-Gated in LIR)

Only power-of-two lane counts totaling 64, 128, or 256 bits are permitted:

| Total Width | Legal Vector Types | Typical Hardware Baseline |
| :--- | :--- | :--- |
| **64-bit** | `v8i8`, `v4i16`, `v2i32`, `v2f32` | Sub-register / NEON 64-bit |
| **128-bit** | `v16i8`, `v8i16`, `v4i32`, `v2i64`, `v4f32`, `v2f64` | x86-64 SSE2, ARM64 NEON, WASM `v128` |
| **256-bit** | `v32i8`, `v16i16`, `v8i32`, `v4i64`, `v8f32`, `v4f64` | x86-64 AVX2 |

### 3.4. Structural, Signature & ABI Types (HIR Only)

Defined at module scope:

* **Struct:** `type @Point = { i32, i32, f64 }` — Fields laid out sequentially using the target's standard C alignment and padding rules.
* **Packed Struct:** `type @NetHdr = packed { i8, i32, i16 }` — Zero padding between fields; alignment is `1`.
* **Union:** `type @Value = union { i64, f64, @Point }` — All fields start at offset `0`; alignment is $\max(\text{field alignments})$; size is $\max(\text{field sizes})$ rounded up to alignment.
* **Array:** `type @Buffer = [256 x i8]` — Contiguous elements with stride equal to element size (padded to element alignment).
* **Signature:** `sig @Callback = (ptr, i32) -> void cc "c"` — Function type descriptor required for `call_indirect` and `invoke_indirect`. Retained in flattened machine-type form in LIR for indirect calls.
* **Varargs State:** `va_list` — Opaque built-in aggregate representing the target platform's C `va_list`.

---

## 4. Module Declarations, Linkage & ABI

### 4.1. Linkage & Section Modifiers

Symbols (globals and functions) default to **module-private** linkage unless prefixed with:

* `export` — Public external linkage; visible to the linker and preserved by dead-code elimination.
* `weak` — Weak/linkonce linkage; the linker deduplicates identical definitions across translation units (supports C++ inline/templates, Rust generics, weak hooks).
* `extern` — Forward declaration of a symbol defined in another object file or library (no body/initializer).

Any global or function definition may specify a custom linker section via `section "<name>"` (e.g., `section ".init_array"`, `section ".pir_tests"`).

### 4.2. Global Data & Thread-Local Storage

* **Read-only data (`data`):** Immutable bytes/values, optionally containing symbol relocations with constant byte offsets:

```text
@msg    = data align 1 { "Hello\n", 0 }
@vtable = data align 8 { ptr @draw_fn, ptr @base_table + 16 }
```

* **Mutable globals (`global`):** Placed in `.data` (initialized) or `.bss` (`zeroinit`):

```text
export @counter = global i32 = 42, align 4
@arena          = global [65536 x i8] = zeroinit, align 16
export @init_fn = data section ".init_array" align 8 { ptr @module_init }
```

* **Thread-local globals (`tls`):** Per-thread storage (valid in both HIR and LIR):

```text
@exc_stack = tls ptr = 0, align 8
```

### 4.3. Functions, Calling Conventions & Attributes

```text
extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
export func @compute(%out: retbuf(@Point), %in: byval(@Point), %p: ptr noalias) -> void cc "c" nounwind { ... }
```

* **Calling Conventions (`cc`):**
  * `cc "internal"` (default if omitted): Backend-defined fast calling convention. May not use `...`, `byval`, or `retbuf`.
  * `cc "c"`: Target platform's standard C ABI (SysV x86-64, Win64, AAPCS64, etc.). Applied symmetrically to both callers and `export`/`weak` callees.
* **Parameter Attributes:**
  * `byval(@Type)` / `byval(va_list)` *(HIR only, `cc "c"` only)*: Parameter has type `ptr` in IR; the middle-end passes/receives the pointed-to aggregate by value according to the C ABI.
  * `retbuf(@Type)` *(HIR only, `cc "c"` only, parameter 0 only, return type must be `void`)*: Parameter has type `ptr` in IR; represents a C ABI struct return value. Lowered transparently to either return registers or a hidden `sret` pointer.
  * `noalias` *(on `ptr` params)*: Memory reachable via this pointer does not alias any other pointer accessed during the call.
  * `readonly` *(on `ptr` params)*: Callee does not write through this pointer.
* **Function Attributes:**
  * `nounwind`: Function will never unwind/throw an exception (enables downgrading `invoke` to `call`).
  * `noreturn`: Function never returns control to its caller (e.g., `abort`, `exit`, panic).
  * `readonly`: Function does not mutate observable memory.

---

## 5. Instruction Set Reference

### 5.1. Semantic Flags & Defaults Summary

Every operation class has an explicit, deterministic default when no `[flag]` is specified:

| Operation Class | Instructions | Default (No Flag) | Optional Flags |
| :--- | :--- | :--- | :--- |
| **Int Add / Sub / Mul** | `add`, `sub`, `mul` | `[wrap]` (two's-complement wrap) | `[wrap]`, `[trap_s]`, `[trap_u]`, `[unsafe_s]`, `[unsafe_u]` |
| **Int Div / Rem** | `div_s`, `div_u`, `rem_s`, `rem_u` | `[trap]` (traps on `/0` and `INT_MIN / -1`) | `[trap]`, `[wrap]` (`INT_MIN / -1` wraps to `INT_MIN` / rem `0`; `/0` still traps), `[unsafe]` (UB on both) |
| **Bitwise Shifts** | `shl`, `shr`, `sar` | `[mask]` (shift amount mod bit-width) | `[mask]`, `[trap]` (traps if shift $\ge$ width), `[unsafe]` (UB if shift $\ge$ width) |
| **Zero-Count Bitops** | `clz`, `ctz` | Returns bit-width on input `0` | `[nonzero]` (UB if input is `0`; enables raw x86 `bsr`/`bsf`) |
| **Floating Point** | `fadd`, `fsub`, `fmul`, `fdiv` | Strict IEEE 754 (round-to-nearest-even) | `[fast]` (no NaN/Inf, reassociation & FMA permitted) |
| **Float-to-Int Cast** | `fptosi`, `fptoui` | `[sat]` (saturates out-of-range; `NaN` becomes `0`) | `[sat]`, `[trap]` (traps on NaN/out-of-range), `[unsafe]` (UB on NaN/out-of-range) |
| **Pointer Math** | `ptr_add` | `[wrap]` (modular address arithmetic) | `[wrap]`, `[inbounds]` (UB if result leaves allocated object or one-past-end) |
| **Memory Access** | `load`, `store` | Non-volatile, aligned per `align` | `[volatile]` (no reordering or elision; for MMIO) |
| **Compare-Exchange** | `atomic_cmpxchg` | Strong (no spurious failure) | `[weak]` (may spuriously fail on LL/SC targets) |
| **Calls** | `call`, `call_indirect` | Inherits callee `nounwind` | `[nounwind]` (call-site override) |

*(Note: `[unsafe_s, unsafe_u]` may be combined on `add`/`sub`/`mul`. Conflicting flags such as `[wrap, trap_s]` are verifier errors.)*

### 5.2. Constants, Addresses & Conversions

* `%r = const.<T> <literal>` — Materializes a literal constant of type `<T>` into a register.
* `%p = addr @sym` — Produces the `ptr` address of a global variable, `data` block, `tls` variable, or function.
* `%r = zext.<dst_int> %val` / `%r = sext.<dst_int> %val` — Zero-extend or sign-extend an integer (or integer vector) to a wider integer type.
* `%r = trunc.<dst_int> %val` — Truncates an integer (or integer vector) to a narrower integer type.
* `%r = fpext.<dst_fp> %val` / `%r = fptrunc.<dst_fp> %val` — Widens (`f32` to `f64`) or narrows (`f64` to `f32`) floats (scalar or equal-lane vector).
* `%r = fptosi.<dst_int> %val [sat | trap | unsafe]` / `%r = fptoui.<dst_int> %val [sat | trap | unsafe]` — Float to signed/unsigned integer.
* `%r = sitofp.<dst_fp> %val` / `%r = uitofp.<dst_fp> %val` — Signed/unsigned integer to float.
* `%r = ptrtoint.<dst_int> %ptr` / `%r = inttoptr.ptr %int` — Converts between `ptr` and `i32`/`i64` (zero-extending or truncating relative to target pointer width).
* `%r = bitcast.<dst_T> %val` — Zero-cost bitwise reinterpretation between equal-bit-width scalar or vector types (e.g., `i32` and `f32`, `v4f32` and `v4i32`).

### 5.3. Integer Arithmetic, Bitwise & Bit-Manipulation

Supported on integer types (`i8`..`i64`, `i128` in HIR, and integer vectors lane-wise):

* **Arithmetic:** `add.<T>`, `sub.<T>`, `mul.<T>`, `div_s.<T>`, `div_u.<T>`, `rem_s.<T>`, `rem_u.<T>`
* **Bitwise:** `and.<T>`, `or.<T>`, `xor.<T>`
* **Shifts:** `shl.<T>` (left), `shr.<T>` (logical right), `sar.<T>` (arithmetic right)
* **Bit-Manipulation (scalar integers):**
  * `%r = clz.<I> %a [nonzero]` — Count leading zeros.
  * `%r = ctz.<I> %a [nonzero]` — Count trailing zeros.
  * `%r = popcnt.<I> %a` — Count set bits.
  * `%r = bswap.<I> %a` — Reverse byte order (`i16`, `i32`, `i64`, `i128`).

### 5.4. Checked Overflow & Widening Arithmetic (Multi-Result)

Used for Rust/Zig/Ada checked math and `i128` legalization without introducing tuple types:

* `%res, %ovf = add_ovf_s.<I> %a, %b` / `add_ovf_u.<I>` — Returns wrapped result `<I>` and overflow flag `i8` (`0` or `1`).
* `%res, %ovf = sub_ovf_s.<I> %a, %b` / `sub_ovf_u.<I>`
* `%res, %ovf = mul_ovf_s.<I> %a, %b` / `mul_ovf_u.<I>`
* `%sum, %cout = add_carry.<I> %a, %b, %cin_i8` — Adds `%a + %b + (%cin & 1)`; returns wrapped sum `<I>` and carry-out `i8`.
* `%dif, %bout = sub_borrow.<I> %a, %b, %bin_i8` — Computes `%a - %b - (%bin & 1)`; returns wrapped diff `<I>` and borrow-out `i8`.
* `%lo, %hi = mul_wide_u.<I> %a, %b` / `mul_wide_s.<I>` — Full $N \times N \to 2N$ widening multiply (`i32` or `i64`), returning low and high halves of type `<I>`.

### 5.5. Floating-Point Arithmetic

Supported on `f32`, `f64`, and float vectors (`v2f32`, `v4f32`, `v2f64`, `v8f32`, `v4f64`):

* `fadd.<F> %a, %b [fast]`
* `fsub.<F> %a, %b [fast]`
* `fmul.<F> %a, %b [fast]`
* `fdiv.<F> %a, %b [fast]`
* `fneg.<F> %a` — Negates sign bit (exact IEEE 754 flip).

### 5.6. Comparisons & Selection

* **Integer Comparisons:** `cmp_eq`, `cmp_ne`, `cmp_lt_s`, `cmp_lt_u`, `cmp_le_s`, `cmp_le_u`, `cmp_gt_s`, `cmp_gt_u`, `cmp_ge_s`, `cmp_ge_u`.
* **Pointer Comparisons:** `cmp_eq.ptr`, `cmp_ne.ptr`, `cmp_lt_u.ptr`, `cmp_le_u.ptr`, `cmp_gt_u.ptr`, `cmp_ge_u.ptr`.
* **Float Comparisons:** Explicitly distinguish **Ordered** (`o`: false if either operand is NaN) vs. **Unordered** (`u`: true if either operand is NaN):
  * Ordered: `fcmp_oeq`, `fcmp_one`, `fcmp_olt`, `fcmp_ole`, `fcmp_ogt`, `fcmp_oge`
  * Unordered: `fcmp_ueq`, `fcmp_une`, `fcmp_ult`, `fcmp_ule`, `fcmp_ugt`, `fcmp_uge`
* **Comparison Result Type:**
  * Scalar comparisons return `i8` (`0` = false, `1` = true).
  * Vector comparisons return an equal-width integer mask vector (e.g., `fcmp_olt.v4f32` returns `v4i32`) where each lane is `0` (false) or `-1` / all-1-bits (true).
* **Selection:** `%r = select.<T> %cond, %t_val, %f_val`
  * For scalar `<T>`: `%cond` must be `i8` (`0` selects `%f_val`, `1` selects `%t_val`; other values are UB).
  * For vector `<V>`: `%cond` must be the matching-width integer mask vector (`0` selects `%f_val` lane, `-1` selects `%t_val` lane).

### 5.7. Fixed-Width Vector Operations

In addition to lane-wise arithmetic, bitwise, comparison, conversion, selection, and `load`/`store`:

* `%v = splat.<V> %scalar` — Broadcasts `%scalar` to every lane of `<V>`.
* `%s = extract.<V> %v, <const_lane>` — Extracts constant lane index $0 \le \text{idx} < \text{lanes}$.
* `%v2 = insert.<V> %v, %scalar, <const_lane>` — Produces a new vector with one lane replaced.
* `%v3 = shuffle.<V> %v1, %v2 [<idx_0>, <idx_1>, ...]` — Permutes lanes from the concatenation of `%v1` ($0 \dots N-1$) and `%v2` ($N \dots 2N-1$) using constant indices.

### 5.8. Memory, Stack & Pointer Operations

* `%p = alloca <Type> [, align <n>]` *(HIR)* — Allocates fixed stack space for any scalar, vector, structural `@Type`, or `va_list`. Fixed `alloca`s in the entry block are promotable to SSA registers (`mem2reg`).
* `%p = alloca.ptr <bytes>, align <n>` *(LIR)* — Lowered fixed stack slot.
* `%p = alloca_dyn %bytes, align <n>` — Dynamic stack allocation (C VLAs); `%bytes` is `i32` or `i64`.
* `%sp = stacksave` / `stackrestore %sp` — Saves and restores the current stack pointer to reclaim `alloca_dyn` frames inside loops/scopes.
* `%p = field_ptr @StructOrUnion, %base, <const_idx>` *(HIR only)* — Computes the address of field `<const_idx>` in a struct, packed struct, or union.
* `%p = elem_ptr <ElemType>, %base, %idx` *(HIR only)* — Computes `%base + %idx * sizeof(<ElemType>)` where `%idx` is an integer register or literal (`i32` or `i64`).
* `%p = ptr_add.<I> %base, %offset [wrap | inbounds]` *(HIR & LIR)* — Byte-level pointer offset; `<I>` is the offset integer type (`i32` or `i64`).
* `%v = load.<T> %ptr, align <n> [volatile]` — Reads `<T>` from `%ptr`. `align` must be a non-zero power of two.
* `store.<T> %val, %ptr, align <n> [volatile]` — Writes `<T>` to `%ptr`.
* **Memory Intrinsics *(HIR only)*:**
  * `memcpy %dst, %src, %len, align <n>` — Non-overlapping byte copy (`%len` is `i32`/`i64` or constant).
  * `memmove %dst, %src, %len, align <n>` — Overlapping-safe byte copy.
  * `memset %dst, %byte_i8, %len, align <n>` — Fills `%len` bytes with `%byte_i8`.

### 5.9. Control Flow, Block Arguments, Calls & Exceptions

PIR uses **Block Arguments** exclusively (no `phi` nodes):

```text
.loop(%i: i32, %acc: f32):
```

**Terminators** (every basic block must end with exactly one):

* `jmp .target(%args...)` — Unconditional jump.
* `br %cond_i8, .true_blk(%args...), .false_blk(%args...)` — Conditional branch; `%cond_i8` must be `i8` (`0` = false, `1` = true).
* `switch.<I> %val, default .def(%args...) [ <c0> -> .b0(%args...), <c1> -> .b1(%args...) ]` — Multi-way branch on integer `<I>` with unique constant case values.
* `ret %val` / `ret` — Returns from function (`ret` with no operand for `void` functions).
* `trap` — Defined immediate runtime abort (e.g., `ud2` / `brk`); optimizer must never remove reachable `trap`s.
* `unreachable` — Asserts execution can never reach this point (UB if reached; optimizer may eliminate code leading exclusively to it).
* `invoke @fn(%args...) then .ok(<args...>) unwind .catch(<args...>)` *(HIR only)* — Throwing direct call.
* `invoke_indirect @Sig, %fn_ptr(%args...) then .ok(<args...>) unwind .catch(<args...>)` *(HIR only)* — Throwing indirect call.
* `resume %exn_ptr` *(HIR only)* — Continues unwinding the exception payload `%exn_ptr: ptr` to the caller frame.

> **Invoke Result & Exception Binding:** `invoke` has no left-hand `%res =` assignment (preserving standard SSA dominance). Instead, the reserved bareword **`ret`** may be passed in the `.ok(...)` argument list to forward the call's return value (for non-`void` callees), and **`exn`** may be passed in the `.catch(...)` argument list to forward the caught exception `ptr`:
>
> ```text
> invoke @parse(%buf) then .parsed(ret, %ctx) unwind .on_err(exn, %ctx)
> ```

**Non-Terminator Calls:**

* `[%res =] call @fn(%args...) [nounwind]` (omit `%res =` for `void` or discarded results).
* `[%res =] call_indirect @Sig, %fn_ptr(%args...) [nounwind]`.
* At variadic call sites (`...`), any argument past the fixed parameters that is a literal or `byval` struct must carry an explicit type tag: `call @printf(%fmt, 42: i32, %pt: byval(@Point))`.

### 5.10. Variadic Arguments (HIR; `va_start` in LIR)

`va_start` is only valid in `cc "c"` functions whose parameter list ends in `...`. `va_arg`/`va_arg_buf`/`va_copy`/`va_end` may be called on any `va_list` pointer:

* `va_start %ap_ptr` — Initializes the `va_list` at `%ap_ptr`.
* `%val = va_arg.<T> %ap_ptr` *(HIR only)* — Reads next scalar/vector argument of type `<T>`.
* `va_arg_buf @Type, %dst_ptr, %ap_ptr` *(HIR only)* — Reads next by-value aggregate argument into `%dst_ptr`.
* `va_copy %dst_ap, %src_ap` *(HIR only)* — Duplicates a `va_list`.
* `va_end %ap_ptr` *(HIR only)* — Ends lifetime of `%ap_ptr`.

### 5.11. Atomics & Memory Model

Valid on `i8`, `i16`, `i32`, `i64`, and `ptr`. Memory pointers **must be naturally aligned** to `sizeof(<T>)` (unaligned atomic access is UB). Orderings follow the C11/C++11 memory model: `[relaxed]`, `[acquire]`, `[release]`, `[acq_rel]`, `[seq_cst]`.

* `%val = atomic_load.<T> %ptr [ord]` — Legal orderings: `relaxed`, `acquire`, `seq_cst`.
* `atomic_store.<T> %val, %ptr [ord]` — Legal orderings: `relaxed`, `release`, `seq_cst`.
* `%old = atomic_rmw.<op>.<T> %ptr, %val [ord]` — `op` $\in \{\text{add}, \text{sub}, \text{and}, \text{or}, \text{xor}, \text{xchg}\}$; returns previous value. Any of the 5 orderings is legal.
* `%old, %ok = atomic_cmpxchg.<T> %ptr, %expected, %new [succ_ord, fail_ord] [weak]` — Strong CAS by default (no spurious failures); add `[weak]` for LL/SC loops. Returns previous value `<T>` and success flag `i8` (`0`/`1`). `fail_ord` may not be `release` or `acq_rel`, and must not be stronger than `succ_ord`.
* `fence [ord]` — Standalone memory barrier. Legal orderings: `acquire`, `release`, `acq_rel`, `seq_cst`.

### 5.12. Debug Source Locations

* `loc <file_id>, <line>, <col>` — Sticky directive inside a basic block associating subsequent instructions with a registered `file` ID, 1-based line, and 1-based column until the next `loc` or block end. Lowered directly to assembler `.loc` directives (or DWARF `.debug_line` entries).

---

## 6. Middle-End Lowering & Backend Capabilities (HIR to LIR)

### 6.1. Backend Capability Profile

To keep experimental backends minimal without penalizing production targets, each backend declares a compact capability profile that guides the middle-end's final legalization passes:

| Capability | Values | Middle-End Behavior When Unsupported |
| :--- | :--- | :--- |
| `ptr_bits` | `32` or `64` | Sets `ptr` size, alignment, and `target` layout. |
| `simd` | `0`, `128`, or `256` | Splits `v256` into `v128` pairs, or fully scalarizes all vectors when `0`. |
| `has_mul_wide` | `bool` | Expands `mul_wide_*` to 32-bit half-word multiplies when `false`. |
| `has_ovf_ops` | `bool` | Expands `*_ovf_*` and `add_carry`/`sub_borrow` to standard arithmetic + comparisons. |
| `has_bitops` | `bool` | Expands `clz`/`ctz`/`popcnt`/`bswap` to standard bitwise sequences. |
| `has_switch` | `bool` | Lowers `switch` to a balanced binary tree of `cmp` + `br`. |
| `unwind` | `shadow_stack` or `table` | Lowers `invoke`/`resume` to TLS shadow stack or `.rodata` unwind tables. |

### 6.2. Mandatory HIR to LIR Lowering Rules (Strict Order)

All of the following transformations **must** complete in order before LIR reaches any backend:

1. **Exception Lowering:**
   * Downgrade `invoke`/`invoke_indirect` on `nounwind` callees to `call`/`call_indirect` + `jmp`.
   * Under `unwind = shadow_stack`: inject thread-local (`tls`) frame push/pop around standard `call` instructions and branch to landing pads on unwind.
   * Under `unwind = table`: emit standard `call` instructions alongside middle-end-generated `.rodata` unwind tables and a shared personality stub.
2. **Varargs Lowering:**
   * `alloca va_list` lowers to `alloca.ptr <abi_size>, align <abi_align>`.
   * `va_copy` lowers to `memcpy`; `va_end` is deleted.
   * `va_arg` and `va_arg_buf` lower to explicit LIR loads, register-save-area bounds checks, branches, and pointer increments per target ABI.
   * Only `va_start %ap_ptr` remains as an LIR primitive for the backend.
3. **Symmetric C ABI Lowering (`cc "c"`):**
   * `byval(@Type)` and `retbuf(@Type)` are eliminated on both call sites and `export`/`weak` function definitions.
   * Small aggregates fitting in ABI registers are unpacked into scalar/vector machine-type arguments or returns (`load`/`store` around the call or prologue/epilogue).
   * Large aggregates are passed via caller-allocated stack copies or hidden `sret` pointers per the target ABI (SysV x86-64, Win64, AAPCS64).
   * `sig @Name` retains only flattened machine types for `call_indirect`.
4. **Structural & Memory Flattening:**
   * All `type @Name` declarations are removed.
   * `alloca <Type>` lowers to `alloca.ptr <size_bytes>, align <align_bytes>`.
   * `field_ptr @T, %base, <idx>` lowers to `ptr_add.i64 %base, <byte_offset> [inbounds]` (`i32` on 32-bit targets; offset `0` for unions).
   * `elem_ptr <ElemT>, %base, %idx` lowers to `mul.<I> %idx, <stride>` followed by `ptr_add.<I> %base, %off [inbounds]`.
   * Small constant-length `memcpy`/`memmove`/`memset` lower to unrolled `load`/`store` sequences; large or dynamic lengths lower to calls to `@memcpy`/`@memmove`/`@memset`.
   * Typed global arrays/structs flatten to raw byte sequences or `global [<bytes>] = zeroinit, align <n>`.
5. **`i128` Legalization:**
   * All `i128` registers, loads, stores, and ops are split into `(lo: i64, hi: i64)` pairs using `add_carry.i64`, `sub_borrow.i64`, `mul_wide_*.i64`, and shifts.
   * `div_s.i128`, `div_u.i128`, `rem_s.i128`, `rem_u.i128` lower to calls to standard runtime helpers (`__divti3`, `__udivti3`, `__modti3`, `__umodti3`).
6. **Vector Legalization:**
   * Any vector wider than the backend's `simd` capability is split into narrower vectors or unrolled into scalar registers and operations.
7. **Optional Hardware-Op Legalization:**
   * Any disabled optional opcodes (`mul_wide_*`, `*_ovf_*`, `add_carry`/`sub_borrow`, bitops, `switch`) are expanded into primitive LIR.

---

## 7. Formal Verification Rules

### 7.1. HIR Verification Invariants

A module is valid HIR if and only if:

1. A valid `target` header appears first; all `loc` directives reference declared `file` IDs.
2. Every symbol (`@name`), block label (`.lbl` per function), and SSA value (`%v` per function) is defined **at most once**.
3. The first basic block of every function is its entry block (`blocks[0]`): it declares **no block parameters** (`param_count == 0`) and cannot be targeted by any `jmp`, `br`, `switch`, or `invoke`. Function parameters are bound on the function signature itself.
4. Every basic block ends with **exactly one terminator**; no terminator appears mid-block.
5. **Strict Dominance:** Every use of `%v` is strictly dominated by its definition (either a function parameter, a block parameter, or an earlier instruction in a dominating block).
6. **Type & Arity Soundness:** Operand types match opcode rules; `br` condition is strictly `i8`; `switch` case constants are unique; jump/branch argument counts and types match the destination block's parameter list 1-to-1.
7. **Aggregate & ABI Validity:** SSA registers never hold structural types or `va_list`; `byval`/`retbuf`/`...` appear only on `cc "c"` functions; `retbuf` appears only as parameter 0 of a `void` function; `field_ptr` index is within bounds of `@Type`; `va_start` appears only inside variadic functions; all alignments are non-zero powers of two.
8. **Flag & Ordering Validity:** No conflicting semantic flags; `atomic_load`/`atomic_store`/`atomic_cmpxchg`/`fence` use only legal memory orderings (`fail_ord` is neither `release` nor `acq_rel` and no stronger than `succ_ord`).
9. **Invoke Binding:** `ret` appears only in the `then` target of a non-`void` `invoke`/`invoke_indirect`; `exn` appears only in the `unwind` target of an `invoke`/`invoke_indirect` and matches a `ptr` block parameter.

### 7.2. LIR Verification Gate (Backend Precondition)

A module is valid LIR if and only if it passes **all HIR invariants**, plus:

1. **Zero HIR Types:** No `type @Name`, `i128`, or `va_list` remains anywhere in the module. Any retained `sig @Name` declarations contain only flattened machine/vector types and no `byval`/`retbuf` tags.
2. **Zero HIR Attributes:** No `byval(...)` or `retbuf(...)` remains on any parameter or call site.
3. **Zero HIR Instructions:** None of `field_ptr`, `elem_ptr`, `invoke`, `invoke_indirect`, `resume`, `va_arg`, `va_arg_buf`, `va_copy`, `va_end`, `memcpy`, `memmove`, `memset`, or typed `alloca <Type>` remain (only `alloca.ptr <bytes>, align <n>` with positive byte count and power-of-two alignment).
4. **Capability Compliance:** All vector widths are $\le \text{backend.simd}$ (none if `simd == 0`), and any disabled optional opcodes (`mul_wide_*`, `*_ovf_*`, `add_carry`/`sub_borrow`, bitops, `switch`) have been expanded.

---

## 8. Concise Reference Grammar (EBNF)

```ebnf
module       = target_decl { file_decl | top_decl } ;
target_decl  = "target" STRING NEWLINE ;
file_decl    = "file" INT STRING NEWLINE ;

top_decl     = type_decl | sig_decl | global_decl | func_decl ;
type_decl    = "type" GLOBAL "=" ( struct_ty | packed_ty | union_ty | array_ty ) NEWLINE ;
struct_ty    = "{" type { "," type } "}" ;
packed_ty    = "packed" "{" type { "," type } "}" ;
union_ty     = "union" "{" type { "," type } "}" ;
array_ty     = "[" INT "x" type "]" ;

sig_decl     = "sig" GLOBAL "=" "(" [ param_ty_list ] ")" "->" ret_ty [ cc_spec ] NEWLINE ;
global_decl  = [ linkage ] GLOBAL "=" ( "data" | "global" | "tls" ) [ sec_spec ] ... NEWLINE ;
func_decl    = [ linkage ] "func" GLOBAL "(" [ param_list ] ")" "->" ret_ty
               [ cc_spec ] { fn_attr } ( NEWLINE | "{" NEWLINE { block } "}" NEWLINE ) ;

linkage      = "export" | "weak" | "extern" ;
cc_spec      = "cc" ( "\"internal\"" | "\"c\"" ) ;
sec_spec     = "section" STRING ;
fn_attr      = "nounwind" | "noreturn" | "readonly" | sec_spec ;

block        = LABEL [ "(" block_params ")" ] ":" NEWLINE { stmt } terminator ;
stmt         = ( loc_dir | assign_inst | void_inst ) NEWLINE ;
assign_inst  = LOCAL { "," LOCAL } "=" opcode [ "." type ] [ operands ] [ flags ] ;
void_inst    = opcode [ "." type ] [ operands ] [ flags ] ;
flags        = "[" IDENT { "," IDENT } "]" ;
```

---

## 9. End-to-End Progressive Lowering Examples

### Example 1: Symmetric C ABI (`byval` + `retbuf`) & Checked Math

**Frontend Output (HIR):**

```text
target "x86_64-unknown-linux-gnu"
file 0 "src/rect.rs"

type @Rect = { i32, i32 }

export func @grow_rect(%out: retbuf(@Rect), %r: byval(@Rect), %delta: i32) -> void cc "c" nounwind {
.entry:
    loc 0, 10, 5
    %w_ptr = field_ptr @Rect, %r, 0
    %h_ptr = field_ptr @Rect, %r, 1
    %w     = load.i32 %w_ptr, align 4
    %h     = load.i32 %h_ptr, align 4

    %nw, %ovf1 = add_ovf_s.i32 %w, %delta
    %nh, %ovf2 = add_ovf_s.i32 %h, %delta
    %any_ovf   = or.i8 %ovf1, %ovf2
    br %any_ovf, .panic, .write(%nw, %nh)

.panic:
    loc 0, 12, 9
    trap

.write(%w2: i32, %h2: i32):
    loc 0, 14, 5
    %ow = field_ptr @Rect, %out, 0
    %oh = field_ptr @Rect, %out, 1
    store.i32 %w2, %ow, align 4
    store.i32 %h2, %oh, align 4
    ret
}
```

**Backend Input (LIR on x86-64 SysV — 8-byte `@Rect` packed into a single `i64` register):**

```text
target "x86_64-unknown-linux-gnu"
file 0 "src/rect.rs"

export func @grow_rect(%r_packed: i64, %delta: i32) -> i64 cc "c" nounwind {
.entry:
    loc 0, 10, 5
    %w    = trunc.i32 %r_packed
    %r_hi = shr.i64 %r_packed, 32
    %h    = trunc.i32 %r_hi

    %nw, %ovf1 = add_ovf_s.i32 %w, %delta
    %nh, %ovf2 = add_ovf_s.i32 %h, %delta
    %any_ovf   = or.i8 %ovf1, %ovf2
    br %any_ovf, .panic, .write(%nw, %nh)

.panic:
    loc 0, 12, 9
    trap

.write(%w2: i32, %h2: i32):
    loc 0, 14, 5
    %lo  = zext.i64 %w2
    %hi0 = zext.i64 %h2
    %hi  = shl.i64 %hi0, 32
    %res = or.i64 %lo, %hi
    ret %res
}
```

### Example 2: Fixed-Width SIMD (`v4f32`) & Dynamic Array Indexing

**Frontend Output (HIR):**

```text
target "x86_64-unknown-linux-gnu"

export func @scale_add(%dst: ptr noalias, %src: ptr readonly, %n: i64, %k: f32) -> void cc "c" nounwind {
.entry:
    %vk = splat.v4f32 %k
    jmp .loop(0)

.loop(%i: i64):
    %done = cmp_ge_u.i64 %i, %n
    br %done, .exit, .body(%i)

.body(%i: i64):
    %sp  = elem_ptr f32, %src, %i
    %dp  = elem_ptr f32, %dst, %i
    %v   = load.v4f32 %sp, align 16
    %out = fmul.v4f32 %v, %vk [fast]
    store.v4f32 %out, %dp, align 16
    %i1  = add.i64 %i, 4 [unsafe_u]
    jmp .loop(%i1)

.exit:
    ret
}
```

**Backend Input (LIR targeting `simd = 128`, e.g., x86-64 SSE2 / ARM64 NEON):**

```text
target "x86_64-unknown-linux-gnu"

export func @scale_add(%dst: ptr noalias, %src: ptr readonly, %n: i64, %k: f32) -> void cc "c" nounwind {
.entry:
    %vk = splat.v4f32 %k
    jmp .loop(0)

.loop(%i: i64):
    %done = cmp_ge_u.i64 %i, %n
    br %done, .exit, .body(%i)

.body(%i: i64):
    %off = mul.i64 %i, 4
    %sp  = ptr_add.i64 %src, %off [inbounds]
    %dp  = ptr_add.i64 %dst, %off [inbounds]
    %v   = load.v4f32 %sp, align 16
    %out = fmul.v4f32 %v, %vk [fast]
    store.v4f32 %out, %dp, align 16
    %i1  = add.i64 %i, 4 [unsafe_u]
    jmp .loop(%i1)

.exit:
    ret
}
```