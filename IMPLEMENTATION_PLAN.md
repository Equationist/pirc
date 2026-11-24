# PIR Reference Compiler (`pirc`) — C99 Implementation Plan

**Specification Target:** `docs/PIR.md` (v0.3.0-draft)  
**Implementation Language:** Strict C99 (`-std=c99`), zero third-party dependencies  
**Primary Host & First Native Target:** Apple Silicon macOS (`aarch64-apple-darwin`), followed by `aarch64-unknown-linux-gnu` and `x86_64` (`x86_64-apple-darwin` / `x86_64-unknown-linux-gnu`)  
**Self-Hosting Frontend:** Adapted `slimcc` (`frontends/slimcc-pir/`)  
**Performance Target:** $\ge 80\%$ of host `clang -O2` (macOS) / `gcc -O2` (Linux) throughput on scalar, `i128`, and fixed-width SIMD workloads

---

## 1. Architectural Strategy & Build Philosophy

`pirc` is written in clean, portable **C99** so that it compiles in under two seconds on any POSIX system, requires no external libraries or parser generators, and can **self-host** via an adapted C frontend (`slimcc-pir`).

### Core Engineering Principles
1. **Single-Pass Bump Arenas:** Every `Module`, `Function`, and compiler pass allocates from contiguous `Arena` blocks. Individual IR nodes are never freed piecemeal, eliminating use-after-free bugs and allocator overhead.
2. **Dense Integer Handles:** All IR references (`ValueId`, `BlockId`, `InstId`, `TypeId`, `SigId`, `GlobalId`) are 32-bit indices into flat arrays—never raw pointers between IR nodes. This makes pass mutations, cloning, printing, and serialization simple and cache-friendly.
3. **Exhaustive Switch Enforcement:** Compiling with `-Wswitch-enum -Werror` guarantees that adding an opcode, type, or flag forces every pass (verifier, printer, interpreter, lowerer, backend) to handle it explicitly.
4. **Verify-After-Every-Pass:** Both `verify_hir()` and `verify_lir()` execute automatically after every pass in debug/sanitizer builds.
5. **Differential Oracle Testing:** A provenance-checking LIR interpreter (`pir-interp`) is built **before** any machine-code backend. Every lowering pass, `-O2` pass, and native backend is differentially tested against it.

---

## 2. Repository & Build Layout

```text
pirc/
├── Makefile                      # Targets: all, pirc, test, test-san, bootstrap, bench, clean
├── AGENTS.md                     # Agent operating rules & coding standards
├── IMPLEMENTATION_PLAN.md        # This document
├── docs/
│   └── PIR.md                    # Frozen v0.3.0 master specification
├── src/
│   ├── main.c                    # CLI driver: parse, verify, lower, opt, interp, codegen
│   ├── common/
│   │   ├── arena.h / arena.c     # Bump allocator + temporary scratch checkpoints
│   │   ├── vec.h                 # Type-safe contiguous dynamic array macros
│   │   ├── strtab.h / strtab.c   # Interned string table (deterministic FNV-1a)
│   │   └── diag.h / diag.c       # Structured diagnostics (file:line:col + error codes)
│   ├── ir/
│   │   ├── ir.h                  # Core enums, IDs, Module, Function, Block, Inst, Type, Sig
│   │   ├── lexer.c               # Zero-lookahead sigil lexer (%, @, ., quoted ids, literals)
│   │   ├── parser.c              # Recursive-descent parser -> Module (HIR or LIR)
│   │   ├── printer.c             # Canonical deterministic text printer (round-trip safe)
│   │   └── verify.c              # Strict HIR & LIR verifiers (§7.1 & §7.2 of PIR.md)
│   ├── analysis/
│   │   ├── cfg.h / cfg.c         # Predecessors, RPO order, reachability, critical edges
│   │   ├── dom.h / dom.c         # Cooper-Harvey-Kennedy dominators + dominance frontiers
│   │   ├── loops.h / loops.c     # Natural loop detection + preheader insertion
│   │   └── alias.h / alias.c     # Provenance-based alias analysis (alloca/global/noalias)
│   ├── target/
│   │   ├── target.h / target.c   # TargetSpec, DataLayout, BackendCaps registry
│   │   ├── abi_aapcs64.c         # ARM64 AAPCS64 (Darwin & Linux) ABI + va_list layout
│   │   ├── abi_sysv_x64.c        # System V AMD64 (macOS & Linux) ABI + va_list layout
│   │   └── abi_win64.c           # Windows x64 ABI classifier + va_list layout
│   ├── lower/
│   │   ├── lower.h / lower.c     # Mandatory HIR -> LIR progressive lowering orchestrator
│   │   ├── exceptions.c          # invoke/invoke_indirect/resume -> TLS shadow stack
│   │   ├── varargs.c             # va_list/va_arg/va_arg_buf/va_copy/va_end -> LIR + va_start
│   │   ├── cabi.c                # cc "c", byval/retbuf, i128 & SIMD ABI lowering
│   │   ├── aggregates.c          # alloca @T, field_ptr, elem_ptr, memcpy/memmove/memset
│   │   ├── i128.c                # i128 -> (lo, hi) i64 pairs + mul_wide/add_carry/__divti3
│   │   ├── simd.c                # v256 splitting & full scalarization per BackendCaps
│   │   └── legalize.c            # Optional software expansion (ovf, bitops, switch, mul_wide)
│   ├── opt/
│   │   ├── opt.h / opt.c         # -O0 / -O1 / -O2 pass manager
│   │   ├── simplify_cfg.c        # Dead blocks, const br, block merge, jump threading, EdgeSplit
│   │   ├── mem2reg.c             # Entry-block scalar/vector alloca -> SSA block arguments
│   │   ├── gvn.c                 # Dominator-scoped GVN + flag-aware constant fold + peepholes
│   │   ├── licm.c                # Loop-invariant code motion (pure ops & alias-safe loads)
│   │   ├── dse.c                 # Intra-block load forwarding & dead store elimination
│   │   └── dce.c                 # Worklist dead code & unused block-param elimination
│   ├── interp/
│   │   ├── interp.h / interp.c   # Reference LIR interpreter + provenance/UB/alignment sanitizer
│   │   └── runtime_shims.c       # Interpreter libc shims (malloc, free, printf, memcpy, etc.)
│   └── backend/
│       ├── backend.h             # Backend & CodeEmitter vtables (AsmEmitter vs ObjEmitter)
│       ├── mir.h / mir.c         # Machine IR (VReg/PReg, RegClass, fixed-reg constraints)
│       ├── regalloc.c            # Wimmer-Franz SSA linear scan + parallel copy sequentializer
│       ├── arm64/                # Primary backend (M6)
│       │   ├── arm64_isel.c      # ARM64 ISel (AAPCS64, NEON v128, adds/cset)
│       │   ├── arm64_frame.c     # Frame layout, callee-saved pairs (stp/ldp), Darwin FP rules
│       │   └── arm64_asm.c       # UAL asm emitter (Apple Mach-O primary + Linux ELF)
│       ├── x64/                  # Secondary backend (M8)
│       │   ├── x64_isel.c        # Maximal-munch ISel (addressing modes, cmp+br, SSE2 v128)
│       │   ├── x64_frame.c       # Stack layout, 16-byte call alignment, prologue/epilogue
│       │   └── x64_asm.c         # Intel-syntax asm emitter (Mach-O & ELF; .file/.loc/TLS)
│       └── obj/
│           ├── macho64.c         # Phase 2 (M9): direct Mach-O 64-bit relocatable .o emitter
│           ├── arm64_encode.c    # AArch64 machine-code instruction encoder from MInst
│           ├── elf64.c           # Optional/follow-on ELF64 relocatable .o emitter
│           └── x64_encode.c      # Optional/follow-on x86-64 machine-code encoder
├── runtime/
│   └── pir_rt.c                  # Minimal native shadow-stack exception runtime (__pir_setjmp / __pir_unwind_resume)
├── frontends/
│   └── slimcc-pir/               # Adapted slimcc with codegen_pir.c emitting PIR HIR
└── tests/
    ├── run_tests.sh              # Master test harness invoked by `make test`
    ├── roundtrip/                # .pir parse -> verify -> print -> parse equivalence
    ├── verifier_neg/             # Invalid HIR/LIR fixtures that MUST fail with error codes
    ├── lowering/                 # Snapshot tests (HIR -> LIR) across arm64/x86_64/scalar
    ├── opt/                      # Pass-firing proof snapshots for M5 (-O2)
    ├── backend_stress/           # High reg pressure, copy cycles, >8 args, large frames, TLS (M6)
    ├── interp_exec/              # Executable PIR programs verified by pir-interp
    ├── c_abi_conformance/        # Bidirectional C <-> PIR tests linked with host cc
    ├── bootstrap/                # 3-stage self-hosting bootstrap verification script
    └── benchmarks/               # Perf suite compared against host `clang -O2` / `gcc -O2`
```

### Standard Compiler Flags (`Makefile`)
```makefile
CFLAGS_BASE = -std=c99 -D_POSIX_C_SOURCE=200809L \
              -Wall -Wextra -Werror -Wswitch-enum -Wconversion \
              -Wshadow -Wstrict-prototypes -Wmissing-prototypes
CFLAGS_DBG  = $(CFLAGS_BASE) -g -O1 -DPIR_VERIFY_ALL=1
CFLAGS_SAN  = $(CFLAGS_DBG) -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS_REL  = $(CFLAGS_BASE) -O2 -DNDEBUG
```

### CLI Surface (`pirc`)
```text
pirc [options] <input.pir>
  --verify-only             Parse and verify HIR (or LIR with --lir) without lowering
  --emit-hir                Print canonical HIR after parsing
  --emit-lir                Lower HIR -> LIR, verify LIR, and print canonical LIR
  --interp                  Lower to LIR and execute @main in the reference sanitizer
  --target=<triple>         Override target triple (also accepts `--target <triple>`)
  --caps=<profile>          Override backend caps (default | scalar | avx2 | minimal)
  -O0 | -O1 | -O2           Select optimization level (default: -O2)
  -S -o <out.s>             Emit target assembly text
  -c -o <out.o>             Emit native object file (via `cc`/`as` in M6-M8, native Mach-O 64-bit in M9)
```

---

## 3. Core C99 Data Structures (`src/ir/ir.h`)

All IR structures must stay flat, POD-like, and strictly aligned with `docs/PIR.md` v0.3.0.

```c
typedef uint32_t ValueId;
typedef uint32_t BlockId;
typedef uint32_t InstId;
typedef uint32_t TypeId;
typedef uint32_t SigId;
typedef uint32_t GlobalId;
typedef uint32_t SymId;      /* Interned string handle */

#define PIR_INVALID_ID UINT32_MAX

typedef enum {
    TY_VOID,
    TY_I8, TY_I16, TY_I32, TY_I64,
    TY_I128,                 /* HIR only */
    TY_F32, TY_F64,
    TY_PTR,
    /* Fixed-width vectors (HIR; capability-gated in LIR) */
    TY_V8I8,  TY_V4I16, TY_V2I32, TY_V2F32,            /* 64-bit  */
    TY_V16I8, TY_V8I16, TY_V4I32, TY_V2I64, TY_V4F32, TY_V2F64, /* 128-bit */
    TY_V32I8, TY_V16I16, TY_V8I32, TY_V4I64, TY_V8F32, TY_V4F64,/* 256-bit */
    TY_VA_LIST,              /* HIR only (memory-only aggregate) */
    TY_AGGREGATE             /* HIR only (references TypeId for struct/packed/union/array) */
} TypeKind;

typedef struct {
    TypeKind kind;
    TypeId   agg_id;         /* Valid iff kind == TY_AGGREGATE */
} PirType;

typedef enum {
    OP_CONST, OP_ADDR,
    /* Conversions */
    OP_ZEXT, OP_SEXT, OP_TRUNC, OP_FPEXT, OP_FPTRUNC,
    OP_FPTOSI, OP_FPTOUI, OP_SITOFP, OP_UITOFP,
    OP_PTRTOINT, OP_INTTOPTR, OP_BITCAST,
    /* Integer math & bitwise */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV_S, OP_DIV_U, OP_REM_S, OP_REM_U,
    OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR, OP_SAR,
    OP_CLZ, OP_CTZ, OP_POPCNT, OP_BSWAP,
    /* Multi-result checked/widening math */
    OP_ADD_OVF_S, OP_ADD_OVF_U, OP_SUB_OVF_S, OP_SUB_OVF_U,
    OP_MUL_OVF_S, OP_MUL_OVF_U,
    OP_ADD_CARRY, OP_SUB_BORROW, OP_MUL_WIDE_U, OP_MUL_WIDE_S,
    /* Float math */
    OP_FADD, OP_FSUB, OP_FMUL, OP_FDIV, OP_FNEG,
    /* Comparisons & select */
    OP_CMP_EQ, OP_CMP_NE, OP_CMP_LT_S, OP_CMP_LT_U, OP_CMP_LE_S, OP_CMP_LE_U,
    OP_CMP_GT_S, OP_CMP_GT_U, OP_CMP_GE_S, OP_CMP_GE_U,
    OP_FCMP_OEQ, OP_FCMP_ONE, OP_FCMP_OLT, OP_FCMP_OLE, OP_FCMP_OGT, OP_FCMP_OGE,
    OP_FCMP_UEQ, OP_FCMP_UNE, OP_FCMP_ULT, OP_FCMP_ULE, OP_FCMP_UGT, OP_FCMP_UGE,
    OP_SELECT,
    /* Vectors */
    OP_SPLAT, OP_EXTRACT, OP_INSERT, OP_SHUFFLE,
    /* Memory & stack */
    OP_ALLOCA,               /* HIR: typed alloca; LIR: alloca.ptr <bytes>, align <n> */
    OP_ALLOCA_DYN, OP_STACKSAVE, OP_STACKRESTORE,
    OP_FIELD_PTR, OP_ELEM_PTR, /* HIR only */
    OP_PTR_ADD,
    OP_LOAD, OP_STORE,
    OP_MEMCPY, OP_MEMMOVE, OP_MEMSET, /* HIR only */
    /* Calls & varargs */
    OP_CALL, OP_CALL_INDIRECT,
    OP_VA_START,             /* HIR & LIR */
    OP_VA_ARG, OP_VA_ARG_BUF, OP_VA_COPY, OP_VA_END, /* HIR only */
    /* Atomics */
    OP_ATOMIC_LOAD, OP_ATOMIC_STORE, OP_ATOMIC_RMW, OP_ATOMIC_CMPXCHG, OP_FENCE,
    /* Terminators */
    OP_JMP, OP_BR, OP_SWITCH, OP_RET, OP_TRAP, OP_UNREACHABLE,
    OP_INVOKE, OP_INVOKE_INDIRECT, OP_RESUME /* HIR only */
} Opcode;

typedef enum {
    FLAG_NONE      = 0,
    FLAG_WRAP      = 1u << 0,
    FLAG_TRAP_S    = 1u << 1,
    FLAG_TRAP_U    = 1u << 2,
    FLAG_TRAP      = 1u << 3, /* div/rem/shift/fpto* */
    FLAG_UNSAFE_S  = 1u << 4,
    FLAG_UNSAFE_U  = 1u << 5,
    FLAG_UNSAFE    = 1u << 6, /* div/rem/shift/fpto* */
    FLAG_MASK      = 1u << 7,
    FLAG_NONZERO   = 1u << 8,
    FLAG_FAST      = 1u << 9,
    FLAG_SAT       = 1u << 10,
    FLAG_INBOUNDS  = 1u << 11,
    FLAG_VOLATILE  = 1u << 12,
    FLAG_WEAK      = 1u << 13,
    FLAG_NOUNWIND  = 1u << 14
} SemanticFlags;

typedef struct {
    Opcode        op;
    PirType       ty;            /* Primary opcode type suffix */
    uint32_t      flags;         /* SemanticFlags bitmask */
    uint32_t      align;         /* Power-of-two alignment for load/store/alloca/mem* */
    uint32_t      extra_idx;     /* Side-table index for switch/shuffle/call/invoke/atomic */
    uint32_t      op_start;      /* Slice start into Function::operand_pool */
    uint16_t      op_count;
    ValueId       results[2];    /* PIR_INVALID_ID if unused */
    uint32_t      loc_id;        /* Index into Function::locs (0 = none) */
} Inst;

typedef struct {
    SymId         label;
    uint32_t      param_start;   /* Slice into Function::block_param_pool */
    uint16_t      param_count;   /* Always 0 for entry block (blocks[0]) per §7.1 #3 */
    InstId       *insts;         /* Dynamic array of InstIds in execution order */
    uint32_t      inst_count;
    uint32_t      inst_cap;
} Block;
```

**Important Invariant:** Per `PIR.md` §7.1 rule 3, function parameters live on `Function::params` (each bound to a dedicated `ValueId` defined at function entry), while `blocks[0]` (`.entry`) has `param_count == 0` and zero incoming edges.

---

## 4. Target Specification & Capability Profile (`src/target/`)

```c
typedef enum { ARCH_X86_64, ARCH_ARM64, ARCH_CUSTOM } TargetArch;
typedef enum { OS_LINUX, OS_MACOS, OS_WINDOWS, OS_BAREMETAL } TargetOs;
typedef enum { UNWIND_SHADOW_STACK, UNWIND_TABLE } UnwindMode;

typedef struct {
    uint16_t   max_simd_bits;  /* 0 (scalarize), 128 (SSE2/NEON), 256 (AVX2) */
    bool       has_mul_wide;   /* Native 64x64->128 multiply */
    bool       has_ovf_ops;    /* Native *_ovf_* and add_carry/sub_borrow */
    bool       has_bitops;     /* Native clz/ctz/popcnt/bswap */
    bool       has_switch;     /* Native jump tables (else middle-end binary tree) */
    UnwindMode unwind_mode;    /* UNWIND_SHADOW_STACK in v0.3 */
} BackendCaps;

typedef struct {
    const char  *triple;
    TargetArch   arch;
    TargetOs     os;
    uint8_t      ptr_bytes;    /* Strictly 4 or 8 */
    bool         little_endian;
    uint32_t     va_list_size;
    uint32_t     va_list_align;
    BackendCaps  caps;
} TargetSpec;
```

Built-in presets for v0.3:
* `aarch64-apple-darwin` *(primary dev host & first native target)*: 64-bit, LE, Darwin AAPCS64 ABI (stack-only varargs, `va_list` = 8B/8B, no even-register requirement for `i128`, packed sub-word stack args, Mach-O leading `_` symbols & TLV), `max_simd_bits = 128` (NEON).
* `aarch64-unknown-linux-gnu`: 64-bit, LE, Linux AAPCS64 ABI (`va_list` = 32B/8B, even-register pairing for `i128`), `max_simd_bits = 128` (NEON).
* `x86_64-apple-darwin` / `x86_64-unknown-linux-gnu`: 64-bit, LE, SysV AMD64 ABI, `va_list` = 24B/8B, `max_simd_bits = 128` (256 with `--caps=avx2`).
* `x86_64-pc-windows-msvc`: 64-bit, LE, Win64 ABI, `va_list` = 8B/8B, `max_simd_bits = 128`.
* `--caps=minimal`: forces `max_simd_bits = 0`, `has_mul_wide = false`, `has_ovf_ops = false`, `has_bitops = false`, `has_switch = false` (exercises every legalization pass!).

---

## 5. Exact Middle-End Pipeline & Algorithms

### Phase A: Mandatory HIR → LIR Lowering (Strict Order)

Changing the order of these seven passes will corrupt ABI classification or alignment. Verify after each pass when `PIR_VERIFY_ALL=1`.

1. **`lower_exceptions` (`src/lower/exceptions.c`)**
   * Downgrade `invoke`/`invoke_indirect` targeting a `nounwind` callee directly to `call`/`call_indirect` + `jmp .ok(...)`.
   * For remaining throwing sites under `UNWIND_SHADOW_STACK`:
     * Reference shared TLS head `@__pir_exc_head` (`weak @__pir_exc_head = tls ptr = 0, align 8`, also resolved against `runtime/pir_rt.c` when linking native binaries).
     * Allocate a canonical frame record `{ prev: ptr (off 0), exn: ptr (off ptr_bytes), jmp_buf: [256 x i8] (off 2*ptr_bytes) }` on entry stack (`alloca`), push onto `@__pir_exc_head`, call `@__pir_setjmp(&frame.jmp_buf)`, branch on return value (`0` → perform call, pop frame, `jmp .ok(...)`; non-zero → pop frame, load `frame.exn`, `jmp .catch(...)`). Placing `exn` before `jmp_buf` keeps its offset target-independent across macOS (`sizeof(jmp_buf) == 192`) and Linux.
     * Lower `resume %exn` to `call @__pir_unwind_resume(%exn)` + `unreachable`.
   * Keep the lowering behind an `ExceptionLowerer` function pointer so `UNWIND_TABLE` (DWARF `.eh_frame` + `.rodata` stub) can be plugged in later without altering HIR or backends.

2. **`lower_varargs` (`src/lower/varargs.c`)**
   * Runs **before** C ABI lowering so `byval(va_list)` uses target-accurate passing semantics (8B `char *` pointer copy on Darwin ARM64 and Win64, 32B struct copy on Linux AAPCS64, pointer decay on SysV x64).
   * Lower `alloca va_list` → `alloca.ptr <va_list_size>, align <va_list_align>`.
   * Lower `va_copy` → `memcpy`; delete `va_end`.
   * Expand `va_arg.<T>` and `va_arg_buf @Type` into target-specific LIR branches and loads (e.g., on `aarch64-apple-darwin`, simple 8-byte aligned pointer bump from the variadic stack area since all variadic args are passed on the stack; on Linux AAPCS64 / SysV x64, register-save-area checks vs. overflow stack area).
   * Keep `va_start %ap_ptr` as the lone LIR varargs primitive.

3. **`lower_cabi` (`src/lower/cabi.c`)**
   * Runs **before** aggregate and `i128` flattening because ABI classification depends on field types (on ARM64 AAPCS64: Homogeneous Floating-point/Short-Vector Aggregates like `{ f32, f32, f32, f32 }` pass in `v0`–`v3`, `i128` requires 16-byte memory alignment plus even-register pairing `x0:x1` on Linux AAPCS64 vs. any consecutive pair `x1:x2` on Darwin ARM64, and Darwin passes variadic args strictly on the stack and packs non-variadic sub-word stack args; on SysV x64: `{ f32, f32 }` → SSE `xmm` vs `{ i32, f32 }` → GP `rax`).
   * Symmetrically transforms both `call`/`call_indirect` sites and `export`/`weak` `cc "c"` function definitions:
     * Small `byval(@T)` fitting in registers (or HFA/HVA on ARM64) → unpacked scalar/vector args at caller; spilled into a local `alloca` at callee entry (later eliminated by `mem2reg`).
     * Large `byval(@T)` → caller-allocated stack copy or ABI stack slot (on AAPCS64, >16B non-HFA structs pass a pointer to a caller-side copy).
     * `retbuf(@T)` fitting in registers → register return at callee + stores into caller's `%out` buffer (note: multi-register returns such as 16B `x0:x1` / HFA `v0`–`v3` on ARM64 or 16B SysV returns unpack via a clean multi-register ABI carrier handled by the backend).
     * Large `retbuf(@T)` → becomes the hidden indirect-result / `sret` pointer parameter (`x8` on AAPCS64, `rdi` on SysV x64, `rcx` on Win64).
   * Strip `byval(...)` and `retbuf(...)` completely.

4. **`lower_aggregates` (`src/lower/aggregates.c`)**
   * Compute target `(size, align)` and field offsets for every struct, packed struct, union, and array.
   * `alloca <Type>` → `alloca.ptr <size>, align <align>`.
   * `field_ptr @T, %base, <idx>` → `ptr_add.<I> %base, <offset> [inbounds]` (offset `0` simplifies directly to `%base`).
   * `elem_ptr <ElemT>, %base, %idx` → `mul.<I> %idx, <stride>` (or `shl` for power-of-2 stride) + `ptr_add.<I> %base, %off [inbounds]`.
   * `memcpy`/`memmove`/`memset`: unroll constant lengths $\le 64$ bytes into scalar/vector `load`/`store` sequences; lower larger or dynamic lengths to `extern` C calls (`@memcpy`, `@memmove`, `@memset`).
   * Flatten typed globals to raw byte arrays or byte-count `zeroinit`; remove all `type @Name` declarations.

5. **`legalize_i128` (`src/lower/i128.c`)**
   * Split every `i128` SSA value into `(lo: i64, hi: i64)` across instructions, block parameters, and branch edges.
   * Lower `add`/`sub` via `add_carry.i64`/`sub_borrow.i64`; `mul` via `mul_wide_u.i64` + cross-term `mul`/`add`; shifts via constant/dynamic masks + `select`; `div_s`/`div_u`/`rem_s`/`rem_u` via standard libcalls (`__divti3`, `__udivti3`, `__modti3`, `__umodti3`).

6. **`legalize_simd` (`src/lower/simd.c`)**
   * If `caps.max_simd_bits == 0`: scalarize every vector value into $N$ scalar `ValueId`s (updating block params/args and unrolling all lane ops, `splat`, `extract`, `insert`, `shuffle`).
   * If `caps.max_simd_bits == 128`: split any 256-bit vector value into two 128-bit halves.

7. **`legalize_optional_ops` (`src/lower/legalize.c`)**
   * Conditionally expand `*_ovf_*`, `add_carry`/`sub_borrow`, `mul_wide_*`, `clz`/`ctz`/`popcnt`/`bswap`, or `switch` into primitive LIR whenever the target's `BackendCaps` sets the corresponding capability to `false`.

8. **LIR Verification Gate:** Call `verify_module(mod, VERIFY_LIR, target)`. Abort compilation on any failure.

---

### Phase B: LIR Optimization Pipeline (`-O1` / `-O2`)

Run strictly on verified LIR:

1. **`simplify_cfg`**: Drop unreachable blocks, fold constant `br`/`switch`, merge single-predecessor blocks `A -> B` (substituting `B`'s block parameters with `A`'s jump arguments), and thread empty forwarding blocks.
2. **`mem2reg`**: Promote non-escaping, non-volatile, entry-block `alloca.ptr` slots whose loads/stores share one scalar or vector type. Insert SSA block parameters on the iterated dominance frontier (`analysis/dom.c`) and perform one dominator-tree preorder renaming pass.
3. **`gvn` (with Constant Folding & Peepholes)**: Dominator-scoped hash table over `(op, ty, flags, canonical_operands)`. Fold constants strictly honoring semantic flags (never fold a `[trap]` overflow/div0 away; convert guaranteed traps into `trap`). Apply identities (`x + 0`, `x * 2^k -> x << k`, `x ^ x -> 0`, `select 1/0`, redundant casts, and algebraic reassociation *only* when `[unsafe_s]`/`[unsafe_u]` or `[fast]` permits).
4. **`licm`**: Detect natural loops from backedges, ensure single preheaders, hoist pure non-trapping loop-invariant instructions, and hoist non-volatile `load`s proven invariant by `analysis/alias.c` that dominate all loop exits (or read from proven-dereferenceable `alloca`s).
5. **`dse` & Local Load Forwarding**: Per-block store table keyed by `(base_ptr, const_offset)` using `alias.c` (`alloca` vs `alloca` = NoAlias, `alloca` vs global = NoAlias, `noalias` param vs other ptrs = NoAlias). Forward store-to-load and delete dead stores before aliasing barriers.
6. **`dce` + Final `simplify_cfg` + `edge_split`**: Mark-and-sweep live instructions and block params; run `simplify_cfg`; then split every critical edge (`succ_count(A) > 1 && pred_count(B) > 1`) with an empty trampoline block so out-of-SSA parallel copies always have a clean edge home.

---

## 6. Reference LIR Interpreter & Sanitizer (`src/interp/`)

Implemented in **Milestone M2** before any backend exists:

* **Fat Provenance Pointers:** Every `alloca.ptr`, global, and `malloc` allocation gets a unique `AllocId` with `{ uint8_t *bytes; uint8_t *init_bits; size_t size; uint32_t align; bool alive; bool readonly; }`. A `ptr` value is `(AllocId, int64_t offset)`.
* **Automatic Sanitizer Checks:**
  * Use-after-free (including dangling stack pointers after function return or `stackrestore`).
  * Out-of-bounds memory access and `ptr_add [inbounds]` provenance escapes.
  * Alignment violations (`offset % align != 0`).
  * Reading uninitialized memory (`init_bits == 0`).
  * Immediate failure on `[unsafe_s]`/`[unsafe_u]`/`[unsafe]` UB triggers; deterministic trap exit code on `trap` or `[trap_*]`.
  * Strict `br`/`select` boolean check (`0` or `1` only for scalar `i8`).
* **Built-in Runtime Shims (`runtime_shims.c`):** Implements `@malloc`, `@calloc`, `@realloc`, `@free`, `@memcpy`, `@memmove`, `@memset`, `@memcmp`, `@strlen`, `@puts`, `@putchar`, `@printf`, `@abort`, `@exit`, `@__pir_setjmp`, `@__pir_unwind_resume`, and `__divti3`/`__udivti3`/`__modti3`/`__umodti3` so complete programs execute inside `pir-interp`.

---

## 7. Backend Architecture (`src/backend/`)

All backends target a shared **Machine IR (`MFunc`)** with virtual registers (`VReg`), physical registers (`PReg`), register classes (`RC_GP`, `RC_FP`), fixed-register operand constraints, and caller-saved clobber bitmasks. **ARM64 (`aarch64-apple-darwin` first, plus `aarch64-unknown-linux-gnu`) is built first in M6**, followed by **x86-64 in M8**.

1. **Instruction Selection (`arm64_isel.c` / `x64_isel.c`):**
   * Structured `MInst` enums only (**never** formatted assembly strings during ISel or RegAlloc).
   * Fuse `ptr_add` + scaled `mul` + `load`/`store` into native addressing modes (`[base, idx, lsl #scale]` / `[base, #imm]` on ARM64; `[base + idx*scale + disp]` on x64).
   * Fuse `cmp_*` + `br` into flag-setting compare + conditional jump (`cmp` + `b.cond` on ARM64; `cmp` + `jcc` on x64).
   * Map `add_ovf_*` → `adds` + `cset vs/cs` (ARM64) or `add` + `seto`/`setc` (x64).
   * Propagate `loc_id` from every LIR `Inst` to its generated `MInst`s.
2. **SSA Linear Scan Register Allocator (`src/backend/regalloc.c`):**
   * Wimmer & Franz (2010) linear scan with lifetime holes, separate `RC_GP` and `RC_FP` pools, platform-reserved register exclusions (never allocate `x18` on Darwin ARM64), and fixed physical-register reservation intervals for ABI args, returns (`x0`–`x7`/`x8`/`v0`–`v7` on ARM64; `idiv`/`shl`/ABI regs on x64), and call clobbers.
   * **Parallel Copy Sequentializer:** Resolves block-argument assignments on every edge (guaranteed non-critical by `edge_split`) in topological order, breaking cycles via a reserved scratch register (`x16`/`v31` on ARM64; `r11`/`xmm15` on x64).
3. **Frame Layout & Calling Convention (`*_frame.c`):**
   * Assign stack offsets for `alloca.ptr` slots (respecting alignment), callee-saved register pairs (`stp`/`ldp` on ARM64), spill slots, and outgoing stack arguments while preserving 16-byte stack alignment at every `call` (and continuously for `sp` on ARM64).
   * On `aarch64-apple-darwin`, handle large stack frames cleanly: materialize offsets $> 4095$ bytes into a scratch register (since `sub sp, sp, #imm12` only encodes 12-bit immediates) and emit a call to `___chkstk_darwin` (with frame size in `x9`) for frames $\ge 16\text{ KB}$.
   * Maintain the `x29`/`x30` frame pointer chain on `aarch64-apple-darwin` (per Apple ABI / unwinding / LLDB requirements); on Linux/x64 omit frame pointer by default and force it whenever `alloca_dyn`, `stacksave`, or `va_start` is present.
4. **Dual Code Emitters (`CodeEmitter` vtable):**
   * **Phase 1 (`*_asm.c`):**
     * **`arm64_asm.c` (M6, primary):** Emits UAL ARM64 assembly targeting **Mach-O (`aarch64-apple-darwin`)** (leading `_` symbol prefix, `L...` local labels, `__TEXT,__text` / `__TEXT,__const` / `__DATA,__data` / `.zerofill __DATA,__bss`, `.weak_definition`, `.p2align`, `@PAGE`/`@PAGEOFF`/`@GOTPAGE`/`@GOTPAGEOFF`, Darwin TLV descriptors in `__DATA,__thread_vars` backed by `__DATA,__thread_data`/`__DATA,__thread_bss` accessed via `@TLVPAGE`/`@TLVPAGEOFF` + thunk call `ldr x8, [x0]; blr x8` returning address in `x0` and modeled as a caller-save clobber in regalloc, ELF-style custom section mapping such as `section ".rodata.*"` → `__TEXT,__const` so Apple `as` accepts portable fixtures, PC-relative jump tables in `__TEXT` without absolute relocations, `.subsections_via_symbols`, and `.file`/`.loc` DWARF directives) and **ELF (`aarch64-unknown-linux-gnu`)**; assembled via host `cc`/`as`.
     * **`x64_asm.c` (M8, secondary):** Emits Intel-syntax x86-64 assembly targeting Mach-O (`x86_64-apple-darwin`) and ELF (`x86_64-unknown-linux-gnu`) with `.file`, `.loc`, sections, `.globl`/`.weak`, and TLS directives; assembled via system `cc`/`as`.
   * **Phase 2 (`obj/macho64.c` + `obj/arm64_encode.c`, M9):** Directly encodes the exact same post-regalloc `MFunc` into native **Mach-O 64-bit** `.o` binary files (`MH_OBJECT`, `CPU_TYPE_ARM64`, `__TEXT`/`__DATA` sections, `ARM64_RELOC_*` relocations, `LC_SYMTAB`) without calling `as` (with `obj/elf64.c` + `obj/x64_encode.c` as optional follow-on Linux/x64 direct object emission).

---

## 8. Adapting `slimcc` as the Self-Hosting C Frontend (`frontends/slimcc-pir/`)

To make `pirc` fully self-compilable and exercise real C codebases, fork `slimcc` into `frontends/slimcc-pir/` with a clean, isolated PIR backend:

### Integration Architecture
1. **Replace `codegen.c` with `codegen_pir.c`:**
   * Reuse `slimcc`'s preprocessor, lexer, parser, and typechecker unchanged.
   * Walk `slimcc`'s AST (`Node`, `Obj`, `Type`) and emit **unoptimized PIR HIR text**:
     * Every local variable is emitted as an `alloca` in `.entry` (`pirc`'s `mem2reg` promotes scalars to SSA block arguments).
     * C structs/unions/arrays emit `type @Name` declarations + `field_ptr`/`elem_ptr`.
     * Struct-by-value parameters and returns emit `byval(@T)` / `retbuf(@T)` on `cc "c"` functions.
     * C signed arithmetic emits `[unsafe_s]` (or `[wrap]` under `-fwrapv`); unsigned arithmetic emits `[wrap]`; bitfields lower in `codegen_pir.c` to shifts and masks.
     * C `_Atomic` operations emit PIR `atomic_load`, `atomic_store`, `atomic_rmw`, `atomic_cmpxchg`, and `fence`.
     * C varargs emit `alloca va_list`, `va_start`, `va_arg`, `va_arg_buf`, `va_copy`, `va_end`.
     * Emit `file` and sticky `loc` directives from `slimcc` token source locations.
2. **Floating-Point & `long double` Alignment:**
   * On `aarch64-apple-darwin`, `long double` is natively 64-bit IEEE `double` (`f64`). Build `slimcc-pir` with `BOOTSTRAP_NO_LDOUBLE` enabled so `long double` consistently maps to `f64` across all targets and rejects 80-bit x87 / 128-bit IEEE quad extensions cleanly if requested.
3. **Target Layout Synchronization:**
   * Parameterize `slimcc`'s type size/alignment table via a shared `target_config.h` matching `pirc`'s `TargetSpec` (pointer size, natural alignment, `va_list` size/align) so C `sizeof`/`_Alignof` constant expressions match `pirc`'s `lower_aggregates` byte-for-byte.
4. **System Header Hygiene:**
   * Locate macOS SDK headers via `xcrun --show-sdk-path` (`-isysroot $(xcrun --show-sdk-path)`) and define target & feature macros in `slimcc-pir`'s built-in headers (`__APPLE__`, `__MACH__`, `__aarch64__`, `__arm64__` on macOS ARM64) so macOS SDK headers (e.g., `<libkern/OSByteOrder.h>`, `<sys/cdefs.h>`) and glibc headers do not expand to unsupported GNU inline-asm macros (routing `__builtin_bswap*` directly to PIR `bswap`), and emit a clear diagnostic if user code contains unsupported `asm(...)` statements.

### 3-Stage Self-Hosting Bootstrap Protocol (`tests/bootstrap/run_bootstrap.sh`)
1. **Stage 0:** Compile `pirc-stage0` and `slimcc-pir-stage0` with host `cc` (Apple `clang` on macOS, or `gcc`/`clang` on Linux).
2. **Stage 1:** Compile every `.c` file of `pirc` and `slimcc-pir` via `slimcc-pir-stage0 -> pirc-stage0 -O2 -> as/ld` to build `pirc-stage1` and `slimcc-pir-stage1`.
3. **Stage 2:** Compile every `.c` file of `pirc` again via `slimcc-pir-stage1 -> pirc-stage1 -O2` to emit Stage 2 `.s` files and link `pirc-stage2`.
4. **Stage 3 (Fixed-Point Check):** Compile `pirc` once more via `slimcc-pir-stage1 -> pirc-stage2 -O2` to emit Stage 3 `.s` files.
5. **Gate:** `diff -ru stage2_asm/ stage3_asm/` must be **empty (byte-for-byte identical)**, and `pirc-stage2` must pass the full `make test` suite.

---

## 9. Phased Milestones & Automated Verification Gates

An implementing agent must pass every gate in a milestone (under both default and `make test-san` ASan/UBSan builds) before moving to the next milestone.

| Milestone | Deliverables | Verification Gate (Must Pass 100%) |
| :--- | :--- | :--- |
| **M0: Repo, Common Lib & IR Core** | `Makefile`, `common/{arena,vec,strtab,diag}.*`, `ir/ir.h`, `target/target.*`, `main.c` skeleton. | `make && make test-san` builds cleanly with zero warnings under `-Werror -Wswitch-enum`; unit tests construct a valid HIR `Module` in memory and free cleanly under ASan. |
| **M1: Lexer, Parser, Printer & Verifier** | `ir/{lexer,parser,printer,verify}.c`, `analysis/{cfg,dom}.c`. | 30+ `tests/roundtrip/*.pir` fixtures pass `parse -> verify -> print -> parse` with byte-identical canonical output; 25+ `tests/verifier_neg/*.pir` fixtures fail with exact diagnostic codes (bad dominance, arity mismatch, entry block param, conflicting flags, invalid LIR op). |
| **M2: LIR Interpreter & Sanitizer** | `interp/{interp,runtime_shims}.c`. | 25+ hand-written LIR programs execute via `pirc --interp` (recursion, loops, `select`, `switch`, `v4f32`, `mul_wide`, `add_ovf`, `atomic_*`, `alloca_dyn`+`stacksave`); 10+ negative UB tests (OOB, UAF, uninit read, unaligned load, `[unsafe_s]` overflow) are caught and rejected by the sanitizer. |
| **M3: Core Lowering (HIR → LIR)** | `lower/{lower,aggregates,i128,simd,legalize}.c`. | All HIR test programs (structs, packed structs, unions, dynamic `elem_ptr`, `memcpy`/`memmove`/`memset`, `i128` arithmetic/div, `v4f32`/`v8f32`) pass `verify_lir` AND produce identical results in `pir-interp` under `--caps=default`, `--caps=scalar`, and `--caps=minimal`. |
| **M4: C ABI, Varargs & Exceptions** | `target/abi_{aapcs64,sysv_x64,win64}.c`, `lower/{cabi,varargs,exceptions}.c`. | Snapshot tests verify Darwin AAPCS64, Linux AAPCS64, SysV x64, and Win64 ABI register/stack splits (`byval`, `retbuf`, HFA, `i128`, `v4f32`, Darwin stack-only varargs); shadow-stack `invoke`/`resume` tests execute end-to-end in `pir-interp`; `va_start`/`va_arg`/`va_copy`/`vprintf` pass `verify_lir`. |
| **M5: LIR Optimizer (`-O2`)** | `analysis/{loops,alias}.c`, `opt/{opt,simplify_cfg,mem2reg,gvn,licm,dse,dce}.c`. | Differential gate: every test in `tests/interp_exec/`, `tests/hir_exec/`, and `tests/hir_m4_exec/` produces **bit-identical** output at `-O0`, `-O1`, and `-O2`, passes `verify_lir` after every single pass (`PIR_VERIFY_ALL=1`), and all pass-firing snapshots in `tests/opt/` prove `mem2reg`, `simplify_cfg`, `gvn`, `licm`, `dse`, `dce`, and `edge_split` actually fire while preserving trap/volatile/alias/IEEE safety. |
| **M6: Shared Backend & ARM64 (AArch64) Asm** | `backend/{mir,regalloc}.c`, `backend/arm64/{arm64_isel,arm64_frame,arm64_asm}.c`, `runtime/pir_rt.c`. | Every test in `tests/interp_exec/`, `tests/hir_exec/`, `tests/hir_m4_exec/` (linked with `runtime/pir_rt.c`), and `tests/backend_stress/` compiles to a native macOS ARM64 (`aarch64-apple-darwin`) executable via `--target=aarch64-apple-darwin -S` + host `cc` and matches `pir-interp` output; `lldb` steps cleanly through a `.loc`-annotated PIR binary (and Linux `aarch64-unknown-linux-gnu` assembly snapshots verify cleanly). |
| **M7: C ABI Conformance & Perf Gate** | `tests/c_abi_conformance/` (bidirectional C $\leftrightarrow$ PIR: 1–64B structs, HFA float/vector structs, mixed int/float structs, unions, `i128`, `v4f32`, Darwin stack varargs, function pointers, `weak`, custom sections) + `tests/benchmarks/`. | 100% pass on bidirectional C $\leftrightarrow$ PIR conformance tests against host Apple `clang` on `aarch64-apple-darwin`; benchmark suite (`matmul`, `quicksort`, `wyhash`/`i128`, `simd_clamp`, `switch` bytecode VM) achieves $\ge 80\%$ of host `clang -O2` throughput. |
| **M7.5: `slimcc-pir` & Self-Hosting Bootstrap** | `frontends/slimcc-pir/codegen_pir.c`, `tests/bootstrap/run_bootstrap.sh`. | `slimcc-pir` passes `slimcc`'s core C test suite through `pirc` on `aarch64-apple-darwin`; 3-stage bootstrap compiles `pirc` with itself on macOS ARM64 and produces **byte-identical** Stage 2 vs. Stage 3 assembly; `pirc-stage2` passes `make test`. |
| **M8: x86-64 Asm Backend** | `backend/x64/{x64_isel,x64_frame,x64_asm}.c`. | Full `interp_exec` and `c_abi_conformance` suites pass on x86-64 via macOS Rosetta 2 (`x86_64-apple-darwin` with `cc -arch x86_64` and `arch -x86_64`) and/or on `x86_64-unknown-linux-gnu` (Docker/QEMU/Linux host). |
| **M9: Direct Native Mach-O 64-bit Object Emitter (AArch64)** | `backend/obj/{macho64,arm64_encode}.c` (plus optional `elf64.c`/`x64_encode.c`). | `pirc -c` emits native Mach-O 64-bit ARM64 `.o` files directly on macOS without calling `as`; linked binaries pass the entire `interp_exec`, `c_abi_conformance`, and 3-stage bootstrap suites identically to `-S`. |