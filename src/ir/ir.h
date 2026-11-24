/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_IR_IR_H
#define PIR_IR_IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/arena.h"
#include "common/vec.h"

/* =========================================================================
 * 1. Dense Arena Handles
 * ========================================================================= */

typedef uint32_t PirValueId;
typedef uint32_t PirBlockId;
typedef uint32_t PirInstId;
typedef uint32_t PirTypeId;
typedef uint32_t PirSigId;
typedef uint32_t PirGlobalId;
typedef uint32_t PirFuncId;
typedef uint32_t PirFileId;

#define PIR_INVALID_ID ((uint32_t)0xFFFFFFFFu)

typedef PIR_VEC(PirValueId) PirValueIdVec;
typedef PIR_VEC(PirInstId)  PirInstIdVec;
typedef PIR_VEC(PirBlockId) PirBlockIdVec;

/* =========================================================================
 * 2. Type System (PIR v0.3.0)
 * ========================================================================= */

typedef enum {
    PIR_SCALAR_VOID = 0,
    PIR_SCALAR_I8,
    PIR_SCALAR_I16,
    PIR_SCALAR_I32,
    PIR_SCALAR_I64,
    PIR_SCALAR_I128,   /* HIR only; legalized to i64 pairs in LIR */
    PIR_SCALAR_F32,
    PIR_SCALAR_F64,
    PIR_SCALAR_PTR,
    PIR_SCALAR_VALIST  /* HIR only; opaque target va_list */
} PirScalarType;

typedef enum {
    PIR_VEC_NONE = 0,
    /* 64-bit vectors */
    PIR_VEC_V8I8,
    PIR_VEC_V4I16,
    PIR_VEC_V2I32,
    PIR_VEC_V2F32,
    /* 128-bit baseline vectors (SSE2 / NEON / WASM) */
    PIR_VEC_V16I8,
    PIR_VEC_V8I16,
    PIR_VEC_V4I32,
    PIR_VEC_V2I64,
    PIR_VEC_V4F32,
    PIR_VEC_V2F64,
    /* 256-bit vectors (AVX2; split to 128-bit or scalarized in LIR) */
    PIR_VEC_V32I8,
    PIR_VEC_V16I16,
    PIR_VEC_V8I32,
    PIR_VEC_V4I64,
    PIR_VEC_V8F32,
    PIR_VEC_V4F64
} PirVecType;

typedef enum {
    PIR_TYPE_SCALAR = 0,
    PIR_TYPE_VECTOR,
    PIR_TYPE_NAMED_AGG /* HIR only: references Module.agg_types[agg_id] */
} PirTypeKind;

typedef struct {
    PirTypeKind kind;
    union {
        PirScalarType scalar;
        PirVecType    vec;
        PirTypeId     agg_id;
    } u;
} PirType;

static inline PirType pir_type_scalar(PirScalarType s) {
    PirType t;
    t.kind     = PIR_TYPE_SCALAR;
    t.u.scalar = s;
    return t;
}

static inline PirType pir_type_vec(PirVecType v) {
    PirType t;
    t.kind  = PIR_TYPE_VECTOR;
    t.u.vec = v;
    return t;
}

static inline PirType pir_type_agg(PirTypeId id) {
    PirType t;
    t.kind     = PIR_TYPE_NAMED_AGG;
    t.u.agg_id = id;
    return t;
}

typedef enum {
    PIR_AGG_STRUCT = 0,
    PIR_AGG_PACKED_STRUCT,
    PIR_AGG_UNION,
    PIR_AGG_ARRAY
} PirAggKind;

typedef struct {
    const char *name;         /* Without '@' sigil; NULL for anonymous array */
    uint32_t    src_line;
    PirAggKind  kind;
    /* For STRUCT / PACKED_STRUCT / UNION */
    PirType    *fields;
    uint32_t    field_count;
    /* For ARRAY */
    PirType     elem_type;
    uint64_t    array_len;
} PirAggDef;

/* =========================================================================
 * 3. Linkage, Calling Conventions, Attributes & Semantic Flags
 * ========================================================================= */

typedef enum {
    PIR_LINK_INTERNAL = 0,
    PIR_LINK_EXPORT,
    PIR_LINK_EXTERN,
    PIR_LINK_WEAK
} PirLinkage;

typedef enum {
    PIR_CC_INTERNAL = 0,
    PIR_CC_C
} PirCallingConv;

typedef enum {
    PIR_PARAM_NORMAL = 0,
    PIR_PARAM_BYVAL,          /* HIR only: ptr to caller struct passed by C value */
    PIR_PARAM_RETBUF          /* HIR only: ptr to caller return buffer (sret/regs) */
} PirParamAbiKind;

typedef enum {
    PIR_PATTR_NONE     = 0u,
    PIR_PATTR_NOALIAS  = 1u << 0,
    PIR_PATTR_READONLY = 1u << 1
} PirParamAttrs;

typedef enum {
    PIR_FATTR_NONE     = 0u,
    PIR_FATTR_NOUNWIND = 1u << 0,
    PIR_FATTR_NORETURN = 1u << 1,
    PIR_FATTR_READONLY = 1u << 2
} PirFuncAttrs;

typedef struct {
    const char     *name;       /* Optional in sig/extern */
    PirType         type;       /* Machine type (must be ptr for BYVAL/RETBUF) */
    PirParamAbiKind abi_kind;
    PirType         abi_type;   /* Underlying aggregate or va_list for BYVAL/RETBUF */
    uint32_t        attrs;      /* PirParamAttrs bitmask */
    PirValueId      val_id;     /* Bound ValueId in defined functions */
    uint32_t        abi_stack_align; /* Lowered stack alignment override (0 = default) */
    uint64_t        abi_byval_size;  /* SysV x64 MEMORY-class inline stack byval size (0 = none) */
    uint32_t        abi_byval_align; /* SysV x64 MEMORY-class inline stack byval alignment */
    bool            abi_force_stack; /* Force parameter onto stack (e.g., SysV i128 spill) */
} PirParamDecl;

typedef enum {
    PIR_CABI_RET_NONE = 0,      /* Normal scalar/vec/void return in x0/v0 or rax/xmm0 */
    PIR_CABI_RET_SRET_X8,       /* Param 0 is hidden indirect return ptr passed in x8 (ARM64) */
    PIR_CABI_RET_SRET_RAX,      /* Param 0 is hidden sret ptr in rdi; callee returns it in rax (x64) */
    PIR_CABI_RET_MULTI_REG      /* Param 0 is caller buffer; callee returns in 2..4 regs */
} PirCabiRetKind;

typedef struct {
    PirCabiRetKind kind;
    uint32_t       reg_count;   /* 2..4 for MULTI_REG */
    PirType        regs[4];     /* Machine register types for MULTI_REG */
    uint64_t       agg_size;    /* Exact byte size of aggregate */
    uint32_t       agg_align;   /* Natural alignment of aggregate */
} PirCabiRetInfo;

typedef struct {
    const char     *name;       /* Without '@' sigil */
    uint32_t        src_line;
    PirCallingConv  cc;
    uint32_t        attrs;      /* PirFuncAttrs bitmask */
    PirParamDecl   *params;
    uint32_t        param_count;
    bool            is_variadic;
    PirType         ret_type;
    PirCabiRetInfo  cabi_ret;
} PirSigDef;

typedef enum {
    PIR_FLAG_NONE     = 0u,
    PIR_FLAG_WRAP     = 1u << 0,
    PIR_FLAG_TRAP     = 1u << 1, /* div/rem, shifts, fptosi/fptoui */
    PIR_FLAG_TRAP_S   = 1u << 2, /* add/sub/mul signed trap */
    PIR_FLAG_TRAP_U   = 1u << 3, /* add/sub/mul unsigned trap */
    PIR_FLAG_UNSAFE   = 1u << 4, /* div/rem, shifts, fptosi/fptoui */
    PIR_FLAG_UNSAFE_S = 1u << 5, /* add/sub/mul signed UB */
    PIR_FLAG_UNSAFE_U = 1u << 6, /* add/sub/mul unsigned UB */
    PIR_FLAG_MASK     = 1u << 7, /* shifts: mod bit-width */
    PIR_FLAG_FAST     = 1u << 8, /* float fast-math */
    PIR_FLAG_SAT      = 1u << 9, /* fptosi/fptoui saturating */
    PIR_FLAG_INBOUNDS = 1u << 10,/* ptr_add inbounds provenance */
    PIR_FLAG_VOLATILE = 1u << 11,/* load/store volatile */
    PIR_FLAG_WEAK     = 1u << 12,/* atomic_cmpxchg weak */
    PIR_FLAG_NOUNWIND = 1u << 13,/* call-site nounwind */
    PIR_FLAG_NONZERO  = 1u << 14 /* clz/ctz nonzero input */
} PirSemFlags;

typedef enum {
    PIR_ORDER_NONE    = 0,
    PIR_ORDER_RELAXED,
    PIR_ORDER_ACQUIRE,
    PIR_ORDER_RELEASE,
    PIR_ORDER_ACQ_REL,
    PIR_ORDER_SEQ_CST
} PirMemOrder;

typedef enum {
    PIR_RMW_ADD = 0,
    PIR_RMW_SUB,
    PIR_RMW_AND,
    PIR_RMW_OR,
    PIR_RMW_XOR,
    PIR_RMW_XCHG
} PirRmwOp;

/* =========================================================================
 * 4. Opcodes (Exhaustive enum for -Werror=switch-enum)
 * ========================================================================= */

typedef enum {
    /* Constants & Addresses */
    PIR_OP_CONST = 0,
    PIR_OP_ADDR,

    /* Integer arithmetic (scalar & vector) */
    PIR_OP_ADD,
    PIR_OP_SUB,
    PIR_OP_MUL,
    PIR_OP_DIV_S,
    PIR_OP_DIV_U,
    PIR_OP_REM_S,
    PIR_OP_REM_U,

    /* Checked overflow arithmetic (2 results: %val, %ovf: i8) */
    PIR_OP_ADD_OVF_S,
    PIR_OP_ADD_OVF_U,
    PIR_OP_SUB_OVF_S,
    PIR_OP_SUB_OVF_U,
    PIR_OP_MUL_OVF_S,
    PIR_OP_MUL_OVF_U,

    /* LIR i128-legalization arithmetic primitives (2 results) */
    PIR_OP_MUL_WIDE_U,        /* %lo, %hi */
    PIR_OP_MUL_WIDE_S,        /* %lo, %hi */
    PIR_OP_ADD_CARRY,         /* %sum, %cout */
    PIR_OP_SUB_BORROW,        /* %dif, %bout */

    /* Bitwise & shifts (scalar & vector) */
    PIR_OP_AND,
    PIR_OP_OR,
    PIR_OP_XOR,
    PIR_OP_SHL,
    PIR_OP_SHR,
    PIR_OP_SAR,

    /* Bit manipulation */
    PIR_OP_CLZ,
    PIR_OP_CTZ,
    PIR_OP_POPCNT,
    PIR_OP_BSWAP,

    /* Floating-point arithmetic (scalar & vector) */
    PIR_OP_FADD,
    PIR_OP_FSUB,
    PIR_OP_FMUL,
    PIR_OP_FDIV,
    PIR_OP_FNEG,

    /* Integer comparisons */
    PIR_OP_CMP_EQ,
    PIR_OP_CMP_NE,
    PIR_OP_CMP_LT_S,
    PIR_OP_CMP_LT_U,
    PIR_OP_CMP_LE_S,
    PIR_OP_CMP_LE_U,
    PIR_OP_CMP_GT_S,
    PIR_OP_CMP_GT_U,
    PIR_OP_CMP_GE_S,
    PIR_OP_CMP_GE_U,

    /* Float comparisons (Ordered & Unordered) */
    PIR_OP_FCMP_OEQ,
    PIR_OP_FCMP_ONE,
    PIR_OP_FCMP_OLT,
    PIR_OP_FCMP_OLE,
    PIR_OP_FCMP_OGT,
    PIR_OP_FCMP_OGE,
    PIR_OP_FCMP_UEQ,
    PIR_OP_FCMP_UNE,
    PIR_OP_FCMP_ULT,
    PIR_OP_FCMP_ULE,
    PIR_OP_FCMP_UGT,
    PIR_OP_FCMP_UGE,

    /* Selection */
    PIR_OP_SELECT,

    /* Conversions & bitcasts */
    PIR_OP_ZEXT,
    PIR_OP_SEXT,
    PIR_OP_TRUNC,
    PIR_OP_FPTOSI,
    PIR_OP_FPTOUI,
    PIR_OP_SITOFP,
    PIR_OP_UITOFP,
    PIR_OP_FPEXT,
    PIR_OP_FPTRUNC,
    PIR_OP_BITCAST,
    PIR_OP_PTRTOINT,
    PIR_OP_INTTOPTR,

    /* SIMD lane operations */
    PIR_OP_SPLAT,
    PIR_OP_EXTRACT,
    PIR_OP_INSERT,
    PIR_OP_SHUFFLE,

    /* Stack & memory operations */
    PIR_OP_ALLOCA,            /* HIR: alloca <type> [, align N] */
    PIR_OP_ALLOCA_PTR,        /* LIR: alloca.ptr <bytes>, align <N> */
    PIR_OP_ALLOCA_DYN,        /* Dynamic stack alloc: %bytes, align <N> */
    PIR_OP_STACKSAVE,
    PIR_OP_STACKRESTORE,
    PIR_OP_FIELD_PTR,         /* HIR only */
    PIR_OP_ELEM_PTR,          /* HIR only */
    PIR_OP_PTR_ADD,           /* HIR & LIR */
    PIR_OP_LOAD,
    PIR_OP_STORE,
    PIR_OP_MEMCPY,            /* HIR only */
    PIR_OP_MEMMOVE,           /* HIR only */
    PIR_OP_MEMSET,            /* HIR only */

    /* Atomics & fences */
    PIR_OP_ATOMIC_LOAD,
    PIR_OP_ATOMIC_STORE,
    PIR_OP_ATOMIC_RMW,
    PIR_OP_ATOMIC_CMPXCHG,    /* 2 results: %old, %ok: i8 */
    PIR_OP_FENCE,

    /* Varargs */
    PIR_OP_VA_START,          /* HIR & LIR */
    PIR_OP_VA_ARG,            /* HIR only */
    PIR_OP_VA_ARG_BUF,        /* HIR only */
    PIR_OP_VA_COPY,           /* HIR only */
    PIR_OP_VA_END,            /* HIR only */

    /* Calls */
    PIR_OP_CALL,
    PIR_OP_CALL_INDIRECT,

    /* Terminators */
    PIR_OP_JMP,
    PIR_OP_BR,
    PIR_OP_SWITCH,
    PIR_OP_INVOKE,            /* HIR only */
    PIR_OP_INVOKE_INDIRECT,   /* HIR only */
    PIR_OP_RESUME,            /* HIR only */
    PIR_OP_RET,
    PIR_OP_TRAP,
    PIR_OP_UNREACHABLE
} PirOpcode;

/* =========================================================================
 * 5. Operands, Values, Instructions, Blocks, Functions, Globals, Module
 * ========================================================================= */

typedef enum {
    PIR_OPER_VAL = 0,
    PIR_OPER_IMM_INT,         /* 128-bit integer split into lo/hi for C99 portability */
    PIR_OPER_IMM_FLOAT,       /* IEEE 754 bit pattern (f32 in low 32 bits, f64 in 64) */
    PIR_OPER_SYM,             /* Global/Function symbol + constant byte offset */
    PIR_OPER_INVOKE_RET,      /* Reserved `ret` placeholder on invoke `then` edge */
    PIR_OPER_INVOKE_EXN       /* Reserved `exn` placeholder on invoke `unwind` edge */
} PirOperandKind;

typedef struct {
    PirOperandKind kind;
    PirType        type;      /* Resolved type (including variadic typed literals) */
    bool           has_explicit_type; /* Variadic explicit `: <type>` tag */
    PirParamAbiKind call_abi; /* Optional byval(@T) tag at variadic call sites */
    PirType         call_abi_type;
    uint64_t        call_byval_size;  /* SysV x64 variadic inline stack byval size (0 = none) */
    uint32_t        call_byval_align; /* SysV x64 variadic inline stack byval alignment */
    union {
        PirValueId val;
        struct {
            uint64_t lo;
            uint64_t hi;
            bool     is_negative;
        } imm_int;
        struct {
            uint64_t bits;    /* IEEE 754 bits (f32 in low 32, f64 in 64) */
            double   raw_val; /* Parsed double before f32/f64 narrowing */
        } imm_float;
        struct {
            const char *name;
            int64_t     offset;
            bool        is_func;
            uint32_t    id;
        } sym;
    } u;
} PirOperand;

typedef struct {
    PirBlockId  target;
    PirOperand *args;
    uint32_t    arg_count;
} PirBranchTarget;

typedef struct {
    uint64_t        case_lo;
    uint64_t        case_hi;
    bool            is_negative;
    PirBranchTarget target;
} PirSwitchCase;

typedef struct {
    PirFileId file_id;
    uint32_t  line;
    uint32_t  col;
} PirSourceLoc;

typedef struct {
    const char *name;         /* Local name without '%' */
    PirType     type;
    PirBlockId  def_block;
    PirInstId   def_inst;     /* PIR_INVALID_ID if defined as a block parameter */
    uint32_t    param_index;  /* Block param index or result slot (0 or 1) */
} PirValueDef;

typedef struct {
    PirOpcode      op;
    PirType        ty;        /* Primary opcode type suffix */
    uint32_t       flags;     /* PirSemFlags bitmask */
    PirOperand    *operands;
    uint32_t       operand_count;
    PirValueId     results[2];/* PIR_INVALID_ID if unused */
    uint32_t       align;     /* Byte alignment (0 if unspecified) */
    uint64_t       imm_u64;   /* Static field index, alloca byte size, or lane index */
    PirType        aux_type;  /* Aggregate type for alloca/field_ptr/elem_ptr/va_arg_buf */
    PirSigId       sig_id;    /* Signature for call_indirect / invoke indirect */
    PirRmwOp       rmw_op;    /* Sub-op for atomic_rmw */
    PirMemOrder    order;     /* Primary/success memory ordering */
    PirMemOrder    fail_order;/* Failure ordering for atomic_cmpxchg */
    uint8_t       *shuffle_mask;
    uint32_t       shuffle_len;
    /* Branch / terminator payload */
    PirBranchTarget targets[2]; /* jmp:[0], br:[0]=true,[1]=false, invoke:[0]=then,[1]=unwind */
    PirSwitchCase  *switch_cases;
    uint32_t        switch_case_count;
    /* Source debug location index (0 = none) */
    uint32_t       loc_idx;
    uint32_t       src_line;  /* .pir file line for diagnostics */
} PirInst;

typedef struct {
    const char   *label;      /* Without '.' sigil */
    uint32_t      src_line;
    PirValueIdVec params;
    PirInstIdVec  insts;
} PirBlock;

typedef struct {
    const char    *name;
    uint32_t       src_line;
    PirLinkage     linkage;
    const char    *section;   /* NULL if default */
    PirCallingConv cc;
    uint32_t       attrs;     /* PirFuncAttrs */
    PirParamDecl  *params;
    uint32_t       param_count;
    bool           is_variadic;
    PirType        ret_type;
    PirCabiRetInfo cabi_ret;
    bool           is_defined;

    PIR_VEC(PirBlock)     blocks;
    PIR_VEC(PirInst)      insts;
    PIR_VEC(PirValueDef)  values;
    PIR_VEC(PirSourceLoc) locs;
} PirFunction;

typedef enum {
    PIR_GSEG_RODATA = 0,      /* `data` */
    PIR_GSEG_DATA,            /* `global` */
    PIR_GSEG_TLS              /* `tls` */
} PirGlobalSeg;

typedef enum {
    PIR_GITEM_BYTES = 0,
    PIR_GITEM_ZERO,
    PIR_GITEM_RELOC,          /* ptr @sym [+ offset] */
    PIR_GITEM_INT,            /* Integer literal */
    PIR_GITEM_FLOAT           /* Float literal */
} PirGlobalItemKind;

typedef struct {
    PirGlobalItemKind kind;
    const uint8_t    *bytes;
    uint64_t          len;
    const char       *sym_name;
    int64_t           sym_offset;
    uint64_t          int_lo;
    uint64_t          int_hi;
    bool              int_neg;
    uint64_t          float_bits;
} PirGlobalItem;

typedef struct {
    const char    *name;
    uint32_t       src_line;
    PirLinkage     linkage;
    PirGlobalSeg   seg;
    const char    *section;
    uint32_t       align;
    PirType        type;      /* HIR global type; flattened in LIR */
    bool           has_type;
    bool           is_zeroinit;
    bool           is_braced;
    PirGlobalItem *items;
    uint32_t       item_count;
} PirGlobal;

typedef struct {
    PirFileId   id;
    const char *path;
} PirFileDecl;

typedef struct {
    PirArena              arena;
    const char           *target_triple;
    PIR_VEC(PirFileDecl)  files;
    PIR_VEC(PirAggDef)    agg_types;
    PIR_VEC(PirSigDef)    sigs;
    PIR_VEC(PirGlobal)    globals;
    PIR_VEC(PirFunction)  funcs;
} PirModule;

/* =========================================================================
 * 6. Target & Backend Capabilities
 * ========================================================================= */

typedef enum {
    PIR_ARCH_UNKNOWN = 0,
    PIR_ARCH_X86_64,
    PIR_ARCH_ARM64,
    PIR_ARCH_CUSTOM
} PirArch;

typedef enum {
    PIR_OS_UNKNOWN = 0,
    PIR_OS_LINUX,
    PIR_OS_MACOS,
    PIR_OS_WINDOWS,
    PIR_OS_BAREMETAL
} PirOs;

typedef struct {
    uint16_t max_simd_bits;   /* 0, 128, or 256 */
    bool     has_mul_wide;
    bool     has_add_carry;
    bool     has_ovf_ops;
    bool     has_bitops;
    bool     has_switch;
} PirBackendCaps;

typedef struct {
    const char    *triple;
    PirArch        arch;
    PirOs          os;
    uint8_t        ptr_bytes;     /* 4 or 8 */
    bool           is_big_endian;
    uint32_t       va_list_size;
    uint32_t       va_list_align;
    PirBackendCaps caps;
} PirTargetSpec;

/* =========================================================================
 * 7. Module Lifecycle & Verifier API (Implemented across M0-M1)
 * ========================================================================= */

typedef enum {
    PIR_VERIFY_HIR = 0,
    PIR_VERIFY_LIR
} PirVerifyMode;

static inline void pir_module_init(PirModule *m) {
    pir_arena_init(&m->arena, 65536u);
    m->target_triple = NULL;
    m->files.data     = NULL; m->files.len     = 0u; m->files.cap     = 0u;
    m->agg_types.data = NULL; m->agg_types.len = 0u; m->agg_types.cap = 0u;
    m->sigs.data      = NULL; m->sigs.len      = 0u; m->sigs.cap      = 0u;
    m->globals.data   = NULL; m->globals.len   = 0u; m->globals.cap   = 0u;
    m->funcs.data     = NULL; m->funcs.len     = 0u; m->funcs.cap     = 0u;
}

static inline void pir_module_destroy(PirModule *m) {
    for (uint32_t i = 0u; i < m->funcs.len; ++i) {
        PirFunction *fn = &m->funcs.data[i];
        for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
            PIR_VEC_FREE(&fn->blocks.data[b].params);
            PIR_VEC_FREE(&fn->blocks.data[b].insts);
        }
        PIR_VEC_FREE(&fn->blocks);
        PIR_VEC_FREE(&fn->insts);
        PIR_VEC_FREE(&fn->values);
        PIR_VEC_FREE(&fn->locs);
    }
    PIR_VEC_FREE(&m->files);
    PIR_VEC_FREE(&m->agg_types);
    PIR_VEC_FREE(&m->sigs);
    PIR_VEC_FREE(&m->globals);
    PIR_VEC_FREE(&m->funcs);
    pir_arena_destroy(&m->arena);
}

/* =========================================================================
 * 8. Compact Type & Opcode Predicates
 * ========================================================================= */

static inline bool pir_type_eq(PirType a, PirType b) {
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
        case PIR_TYPE_SCALAR:    return a.u.scalar == b.u.scalar;
        case PIR_TYPE_VECTOR:    return a.u.vec    == b.u.vec;
        case PIR_TYPE_NAMED_AGG: return a.u.agg_id == b.u.agg_id;
    }
    return false;
}

static inline bool pir_scalar_is_int(PirScalarType s) {
    switch (s) {
        case PIR_SCALAR_I8:
        case PIR_SCALAR_I16:
        case PIR_SCALAR_I32:
        case PIR_SCALAR_I64:
        case PIR_SCALAR_I128:
            return true;
        case PIR_SCALAR_VOID:
        case PIR_SCALAR_F32:
        case PIR_SCALAR_F64:
        case PIR_SCALAR_PTR:
        case PIR_SCALAR_VALIST:
            return false;
    }
    return false;
}

static inline bool pir_scalar_is_float(PirScalarType s) {
    return s == PIR_SCALAR_F32 || s == PIR_SCALAR_F64;
}

static inline uint32_t pir_scalar_bits(PirScalarType s, uint8_t ptr_bytes) {
    switch (s) {
        case PIR_SCALAR_I8:     return 8u;
        case PIR_SCALAR_I16:    return 16u;
        case PIR_SCALAR_I32:
        case PIR_SCALAR_F32:    return 32u;
        case PIR_SCALAR_I64:
        case PIR_SCALAR_F64:    return 64u;
        case PIR_SCALAR_I128:   return 128u;
        case PIR_SCALAR_PTR:    return (uint32_t)ptr_bytes * 8u;
        case PIR_SCALAR_VOID:
        case PIR_SCALAR_VALIST: return 0u;
    }
    return 0u;
}

static inline PirScalarType pir_vec_elem_scalar(PirVecType v) {
    switch (v) {
        case PIR_VEC_V8I8:  case PIR_VEC_V16I8:  case PIR_VEC_V32I8:  return PIR_SCALAR_I8;
        case PIR_VEC_V4I16: case PIR_VEC_V8I16:  case PIR_VEC_V16I16: return PIR_SCALAR_I16;
        case PIR_VEC_V2I32: case PIR_VEC_V4I32:  case PIR_VEC_V8I32:  return PIR_SCALAR_I32;
        case PIR_VEC_V2I64: case PIR_VEC_V4I64:                       return PIR_SCALAR_I64;
        case PIR_VEC_V2F32: case PIR_VEC_V4F32:  case PIR_VEC_V8F32:  return PIR_SCALAR_F32;
        case PIR_VEC_V2F64: case PIR_VEC_V4F64:                       return PIR_SCALAR_F64;
        case PIR_VEC_NONE:                                            return PIR_SCALAR_VOID;
    }
    return PIR_SCALAR_VOID;
}

static inline uint32_t pir_vec_lanes(PirVecType v) {
    switch (v) {
        case PIR_VEC_V2I32: case PIR_VEC_V2F32: case PIR_VEC_V2I64: case PIR_VEC_V2F64: return 2u;
        case PIR_VEC_V4I16: case PIR_VEC_V4I32: case PIR_VEC_V4F32: case PIR_VEC_V4I64:
        case PIR_VEC_V4F64: return 4u;
        case PIR_VEC_V8I8:  case PIR_VEC_V8I16: case PIR_VEC_V8I32: case PIR_VEC_V8F32: return 8u;
        case PIR_VEC_V16I8: case PIR_VEC_V16I16: return 16u;
        case PIR_VEC_V32I8: return 32u;
        case PIR_VEC_NONE:  return 0u;
    }
    return 0u;
}

static inline uint16_t pir_vec_bits(PirVecType v) {
    switch (v) {
        case PIR_VEC_V8I8:  case PIR_VEC_V4I16: case PIR_VEC_V2I32: case PIR_VEC_V2F32:
            return 64u;
        case PIR_VEC_V16I8: case PIR_VEC_V8I16: case PIR_VEC_V4I32: case PIR_VEC_V2I64:
        case PIR_VEC_V4F32: case PIR_VEC_V2F64:
            return 128u;
        case PIR_VEC_V32I8: case PIR_VEC_V16I16: case PIR_VEC_V8I32: case PIR_VEC_V4I64:
        case PIR_VEC_V8F32: case PIR_VEC_V4F64:
            return 256u;
        case PIR_VEC_NONE:
            return 0u;
    }
    return 0u;
}

static inline PirVecType pir_vec_mask_type(PirVecType v) {
    switch (v) {
        case PIR_VEC_V2F32: return PIR_VEC_V2I32;
        case PIR_VEC_V4F32: return PIR_VEC_V4I32;
        case PIR_VEC_V2F64: return PIR_VEC_V2I64;
        case PIR_VEC_V8F32: return PIR_VEC_V8I32;
        case PIR_VEC_V4F64: return PIR_VEC_V4I64;
        case PIR_VEC_V8I8:  case PIR_VEC_V4I16:  case PIR_VEC_V2I32:
        case PIR_VEC_V16I8: case PIR_VEC_V8I16:  case PIR_VEC_V4I32: case PIR_VEC_V2I64:
        case PIR_VEC_V32I8: case PIR_VEC_V16I16: case PIR_VEC_V8I32: case PIR_VEC_V4I64:
            return v;
        case PIR_VEC_NONE:
            return PIR_VEC_NONE;
    }
    return PIR_VEC_NONE;
}

static inline bool pir_type_is_ssa(PirType t) {
    switch (t.kind) {
        case PIR_TYPE_SCALAR:
            return t.u.scalar != PIR_SCALAR_VOID && t.u.scalar != PIR_SCALAR_VALIST;
        case PIR_TYPE_VECTOR:
            return t.u.vec != PIR_VEC_NONE;
        case PIR_TYPE_NAMED_AGG:
            return false;
    }
    return false;
}

static inline bool pir_type_is_int(PirType t) {
    if (t.kind == PIR_TYPE_SCALAR) {
        return pir_scalar_is_int(t.u.scalar);
    }
    if (t.kind == PIR_TYPE_VECTOR) {
        return pir_scalar_is_int(pir_vec_elem_scalar(t.u.vec));
    }
    return false;
}

static inline bool pir_type_is_float(PirType t) {
    if (t.kind == PIR_TYPE_SCALAR) {
        return pir_scalar_is_float(t.u.scalar);
    }
    if (t.kind == PIR_TYPE_VECTOR) {
        return pir_scalar_is_float(pir_vec_elem_scalar(t.u.vec));
    }
    return false;
}

static inline uint32_t pir_type_bits(PirType t, uint8_t ptr_bytes) {
    switch (t.kind) {
        case PIR_TYPE_SCALAR:    return pir_scalar_bits(t.u.scalar, ptr_bytes);
        case PIR_TYPE_VECTOR:    return pir_vec_bits(t.u.vec);
        case PIR_TYPE_NAMED_AGG: return 0u;
    }
    return 0u;
}

static inline bool pir_op_is_terminator(PirOpcode op) {
    switch (op) {
        case PIR_OP_JMP:
        case PIR_OP_BR:
        case PIR_OP_SWITCH:
        case PIR_OP_INVOKE:
        case PIR_OP_INVOKE_INDIRECT:
        case PIR_OP_RESUME:
        case PIR_OP_RET:
        case PIR_OP_TRAP:
        case PIR_OP_UNREACHABLE:
            return true;
        case PIR_OP_CONST: case PIR_OP_ADDR:
        case PIR_OP_ADD: case PIR_OP_SUB: case PIR_OP_MUL:
        case PIR_OP_DIV_S: case PIR_OP_DIV_U: case PIR_OP_REM_S: case PIR_OP_REM_U:
        case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
        case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
        case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U:
        case PIR_OP_MUL_WIDE_U: case PIR_OP_MUL_WIDE_S:
        case PIR_OP_ADD_CARRY: case PIR_OP_SUB_BORROW:
        case PIR_OP_AND: case PIR_OP_OR: case PIR_OP_XOR:
        case PIR_OP_SHL: case PIR_OP_SHR: case PIR_OP_SAR:
        case PIR_OP_CLZ: case PIR_OP_CTZ: case PIR_OP_POPCNT: case PIR_OP_BSWAP:
        case PIR_OP_FADD: case PIR_OP_FSUB: case PIR_OP_FMUL: case PIR_OP_FDIV: case PIR_OP_FNEG:
        case PIR_OP_CMP_EQ: case PIR_OP_CMP_NE:
        case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LT_U: case PIR_OP_CMP_LE_S: case PIR_OP_CMP_LE_U:
        case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GT_U: case PIR_OP_CMP_GE_S: case PIR_OP_CMP_GE_U:
        case PIR_OP_FCMP_OEQ: case PIR_OP_FCMP_ONE: case PIR_OP_FCMP_OLT:
        case PIR_OP_FCMP_OLE: case PIR_OP_FCMP_OGT: case PIR_OP_FCMP_OGE:
        case PIR_OP_FCMP_UEQ: case PIR_OP_FCMP_UNE: case PIR_OP_FCMP_ULT:
        case PIR_OP_FCMP_ULE: case PIR_OP_FCMP_UGT: case PIR_OP_FCMP_UGE:
        case PIR_OP_SELECT:
        case PIR_OP_ZEXT: case PIR_OP_SEXT: case PIR_OP_TRUNC:
        case PIR_OP_FPTOSI: case PIR_OP_FPTOUI: case PIR_OP_SITOFP: case PIR_OP_UITOFP:
        case PIR_OP_FPEXT: case PIR_OP_FPTRUNC: case PIR_OP_BITCAST:
        case PIR_OP_PTRTOINT: case PIR_OP_INTTOPTR:
        case PIR_OP_SPLAT: case PIR_OP_EXTRACT: case PIR_OP_INSERT: case PIR_OP_SHUFFLE:
        case PIR_OP_ALLOCA: case PIR_OP_ALLOCA_PTR: case PIR_OP_ALLOCA_DYN:
        case PIR_OP_STACKSAVE: case PIR_OP_STACKRESTORE:
        case PIR_OP_FIELD_PTR: case PIR_OP_ELEM_PTR: case PIR_OP_PTR_ADD:
        case PIR_OP_LOAD: case PIR_OP_STORE:
        case PIR_OP_MEMCPY: case PIR_OP_MEMMOVE: case PIR_OP_MEMSET:
        case PIR_OP_ATOMIC_LOAD: case PIR_OP_ATOMIC_STORE:
        case PIR_OP_ATOMIC_RMW: case PIR_OP_ATOMIC_CMPXCHG: case PIR_OP_FENCE:
        case PIR_OP_VA_START: case PIR_OP_VA_ARG: case PIR_OP_VA_ARG_BUF:
        case PIR_OP_VA_COPY: case PIR_OP_VA_END:
        case PIR_OP_CALL: case PIR_OP_CALL_INDIRECT:
            return false;
    }
    return false;
}

static inline bool pir_is_pow2_u32(uint32_t v) {
    return v != 0u && (v & (v - 1u)) == 0u;
}

#endif /* PIR_IR_IR_H */
