/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_INTERP_INTERP_H
#define PIR_INTERP_INTERP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ir/ir.h"

typedef enum {
    PIR_INTERP_OK = 0,
    PIR_INTERP_TRAP,
    PIR_INTERP_UB,
    PIR_INTERP_ERR
} PirInterpStatus;

typedef struct {
    uint32_t alloc_id; /* 0 = null; 1..N = index into PirInterp.allocs */
    int64_t  offset;
} PirPtr;

typedef struct {
    uint64_t byval_size;
    uint32_t byval_align;
    uint32_t stack_align;
    bool     force_stack;
    bool     no_f32_promote;
} PirCallArgMeta;

static inline bool pir_ptr_is_null(PirPtr p) {
    return p.alloc_id == 0u && p.offset == 0;
}

typedef struct {
    PirType ty;
    uint8_t bytes[32]; /* Up to 256-bit scalar or vector value (little-endian) */
    PirPtr  ptr;       /* Provenance pointer when ty == PIR_SCALAR_PTR */
} PirVal;

typedef struct {
    size_t byte_off;
    PirPtr ptr;
} PirPtrTag;

typedef struct {
    uint8_t  *bytes;
    uint8_t  *init_bits; /* 1 byte per memory byte: 0=uninit, 1=init */
    size_t    size;
    uint32_t  align;
    uint64_t  base_addr;
    bool      alive;
    bool      readonly;
    bool      is_heap;
    bool      is_func;
    bool      is_dyn_stack;
    PirFuncId func_id;
    PIR_VEC(PirPtrTag) ptr_tags;
} PirAlloc;

typedef struct {
    const PirModule     *mod;
    const PirTargetSpec *target;
    uint8_t              ptr_bytes;
    PIR_VEC(PirAlloc)    allocs;
    uint64_t             next_addr;
    uint32_t            *global_allocs; /* GlobalId -> AllocId */
    uint32_t            *func_allocs;   /* FuncId   -> AllocId */
    PirInterpStatus      status;
    bool                 exited;
    int                  exit_code;
    char                 err_msg[256];
    uint32_t             call_depth;
    uint32_t             next_frame_id;
    /* Shadow-stack exception unwinding state (M4) */
    bool                 unwinding;
    PirPtr               unwind_target_jb;
    PirPtr               unwind_exn;
} PirInterp;

/* Public entry point */
PirInterpStatus pir_interp_run(const PirModule *mod, const PirTargetSpec *target,
                               int *out_exit_code);

/* Shared helpers used by runtime_shims.c */
void     pir_interp_fail(PirInterp *it, PirInterpStatus st, const char *fmt, ...);
uint32_t pir_interp_alloc_new(PirInterp *it, size_t size, uint32_t align,
                              bool readonly, bool is_heap, bool is_dyn, bool zero_init);
bool     pir_interp_check_access(PirInterp *it, PirPtr p, size_t size,
                                 uint32_t align, bool is_write, bool check_init);
bool     pir_interp_mem_read(PirInterp *it, PirPtr p, void *dst, size_t size, uint32_t align);
bool     pir_interp_mem_write(PirInterp *it, PirPtr p, const void *src, size_t size, uint32_t align);
bool     pir_interp_mem_read_ptr(PirInterp *it, PirPtr p, PirPtr *out, uint32_t align);
bool     pir_interp_mem_write_ptr(PirInterp *it, PirPtr p, PirPtr val, uint32_t align);
bool     pir_interp_mem_copy(PirInterp *it, PirPtr dst, PirPtr src, size_t len, bool overlap_ok);
void     pir_interp_clear_ptr_tags(PirAlloc *a, size_t off, size_t size, uint8_t ptr_bytes);
bool     pir_ptr_offset_checked(PirInterp *it, PirPtr base, int64_t delta, PirPtr *out);
PirPtr   pir_interp_int_to_ptr(PirInterp *it, uint64_t addr);
uint64_t pir_interp_ptr_to_int(const PirInterp *it, PirPtr p);
uint64_t pir_val_get_u64(const PirVal *v);
void     pir_val_set_u64(PirVal *v, PirType ty, uint64_t x);

/* Runtime libc / compiler-rt shims */
bool pir_interp_try_shim(PirInterp *it, const char *name,
                         const PirVal *args, uint32_t arg_count,
                         PirType ret_ty, PirVal *out_ret);

#endif /* PIR_INTERP_INTERP_H */
