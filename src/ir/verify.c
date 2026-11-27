/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ir/verify.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analysis/cfg.h"
#include "analysis/dom.h"
#include "lower/lower.h"
#include "target/target.h"

typedef struct {
    const PirModule     *mod;
    PirVerifyMode        mode;
    const PirTargetSpec *target;
    PirDiag             *diag;
} Verifier;

static const char *type_str(const PirModule *mod, PirType ty, char *buf, size_t n) {
    switch (ty.kind) {
        case PIR_TYPE_SCALAR:
            switch (ty.u.scalar) {
                case PIR_SCALAR_VOID:   return "void";
                case PIR_SCALAR_I8:     return "i8";
                case PIR_SCALAR_I16:    return "i16";
                case PIR_SCALAR_I32:    return "i32";
                case PIR_SCALAR_I64:    return "i64";
                case PIR_SCALAR_I128:   return "i128";
                case PIR_SCALAR_F32:    return "f32";
                case PIR_SCALAR_F64:    return "f64";
                case PIR_SCALAR_PTR:    return "ptr";
                case PIR_SCALAR_VALIST: return "va_list";
            }
            return "void";
        case PIR_TYPE_VECTOR:
            switch (ty.u.vec) {
                case PIR_VEC_NONE:   return "none";
                case PIR_VEC_V8I8:   return "v8i8";
                case PIR_VEC_V4I16:  return "v4i16";
                case PIR_VEC_V2I32:  return "v2i32";
                case PIR_VEC_V2F32:  return "v2f32";
                case PIR_VEC_V16I8:  return "v16i8";
                case PIR_VEC_V8I16:  return "v8i16";
                case PIR_VEC_V4I32:  return "v4i32";
                case PIR_VEC_V2I64:  return "v2i64";
                case PIR_VEC_V4F32:  return "v4f32";
                case PIR_VEC_V2F64:  return "v2f64";
                case PIR_VEC_V32I8:  return "v32i8";
                case PIR_VEC_V16I16: return "v16i16";
                case PIR_VEC_V8I32:  return "v8i32";
                case PIR_VEC_V4I64:  return "v4i64";
                case PIR_VEC_V8F32:  return "v8f32";
                case PIR_VEC_V4F64:  return "v4f64";
            }
            return "none";
        case PIR_TYPE_NAMED_AGG:
            if (ty.u.agg_id < mod->agg_types.len && mod->agg_types.data[ty.u.agg_id].name) {
                snprintf(buf, n, "@%s", mod->agg_types.data[ty.u.agg_id].name);
            } else {
                snprintf(buf, n, "[array]");
            }
            return buf;
    }
    return "?";
}

static bool has_symbol(const PirModule *mod, const char *name) {
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        if (strcmp(mod->globals.data[i].name, name) == 0) return true;
    }
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        if (strcmp(mod->funcs.data[i].name, name) == 0) return true;
    }
    return false;
}

static bool check_lir_type(Verifier *v, PirType ty, uint32_t line, const char *ctx) {
    if (v->mode != PIR_VERIFY_LIR) return true;
    switch (ty.kind) {
        case PIR_TYPE_SCALAR:
            if (ty.u.scalar == PIR_SCALAR_I128) {
                pir_diag_error(v->diag, line, 0u, "LIR forbids 'i128' in %s", ctx);
                return false;
            }
            if (ty.u.scalar == PIR_SCALAR_VALIST) {
                pir_diag_error(v->diag, line, 0u, "LIR forbids 'va_list' in %s", ctx);
                return false;
            }
            return true;
        case PIR_TYPE_VECTOR:
            if (v->target && pir_vec_bits(ty.u.vec) > v->target->caps.max_simd_bits) {
                pir_diag_error(v->diag, line, 0u,
                               "LIR vector width %u exceeds target max_simd_bits %u in %s",
                               (unsigned)pir_vec_bits(ty.u.vec),
                               (unsigned)v->target->caps.max_simd_bits, ctx);
                return false;
            }
            return true;
        case PIR_TYPE_NAMED_AGG:
            pir_diag_error(v->diag, line, 0u, "LIR forbids aggregate type in %s", ctx);
            return false;
    }
    return true;
}

static bool check_ssa_type(Verifier *v, PirType ty, uint32_t line, const char *ctx) {
    if (!pir_type_is_ssa(ty)) {
        char b[64];
        pir_diag_error(v->diag, line, 0u, "type '%s' is not valid in SSA (%s)",
                       type_str(v->mod, ty, b, sizeof(b)), ctx);
        return false;
    }
    return check_lir_type(v, ty, line, ctx);
}

static bool check_agg_cycles(Verifier *v, uint32_t start_id, uint8_t *color) {
    if (color[start_id] == 2u) return true;
    typedef struct { uint32_t id; uint32_t next_field; } Frame;
    PIR_VEC(Frame) stk = {0};
    Frame init = { start_id, 0u };
    PIR_VEC_PUSH(&stk, init);
    color[start_id] = 1u;

    bool ok = true;
    while (stk.len > 0u) {
        Frame *top = &stk.data[stk.len - 1u];
        const PirAggDef *a = &v->mod->agg_types.data[top->id];
        uint32_t child = PIR_INVALID_ID;
        if (a->kind == PIR_AGG_PAD) {
            /* Leaf */
        } else if (a->kind == PIR_AGG_ARRAY) {
            if (top->next_field == 0u) {
                top->next_field = 1u;
                if (a->elem_type.kind == PIR_TYPE_NAMED_AGG &&
                    a->elem_type.u.agg_id < v->mod->agg_types.len) {
                    child = a->elem_type.u.agg_id;
                }
            }
        } else {
            while (top->next_field < a->field_count) {
                PirType ft = a->fields[top->next_field++];
                if (ft.kind == PIR_TYPE_NAMED_AGG && ft.u.agg_id < v->mod->agg_types.len) {
                    child = ft.u.agg_id;
                    break;
                }
            }
        }
        if (child != PIR_INVALID_ID) {
            if (color[child] == 1u) {
                pir_diag_error(v->diag, v->mod->agg_types.data[child].src_line, 0u,
                               "recursive/cyclic aggregate type '@%s'",
                               v->mod->agg_types.data[child].name
                                   ? v->mod->agg_types.data[child].name : "anon");
                ok = false;
                break;
            }
            if (color[child] == 0u) {
                color[child] = 1u;
                Frame nf = { child, 0u };
                PIR_VEC_PUSH(&stk, nf);
            }
        } else {
            color[top->id] = 2u;
            stk.len--;
        }
    }
    PIR_VEC_FREE(&stk);
    return ok;
}

static bool is_ptr_type(PirType t) {
    return t.kind == PIR_TYPE_SCALAR && t.u.scalar == PIR_SCALAR_PTR;
}

static bool align_valid_u32(uint32_t al) {
    return pir_is_pow2_u32(al) && al <= (1u << 28);
}

static uint64_t max_obj_bytes(const PirTargetSpec *target) {
    return (target && target->ptr_bytes == 4u) ? (UINT64_C(1) << 31) : (UINT64_C(1) << 40);
}

static bool is_pad_agg_type(const PirModule *mod, PirType t) {
    return t.kind == PIR_TYPE_NAMED_AGG && t.u.agg_id < mod->agg_types.len &&
           mod->agg_types.data[t.u.agg_id].kind == PIR_AGG_PAD;
}

static void verify_params_and_ret(Verifier *v, const char *owner, uint32_t line,
                                  PirCallingConv cc, uint32_t fn_attrs,
                                  const PirParamDecl *params,
                                  uint32_t param_count, bool is_variadic, PirType ret_type,
                                  const PirCabiRetInfo *cabi_ret) {
    uint64_t max_sz = max_obj_bytes(v->target);
    if (is_variadic && cc != PIR_CC_C) {
        pir_diag_error(v->diag, line, 0u, "'%s': variadic '...' requires cc \"c\"", owner);
    }
    if (!(ret_type.kind == PIR_TYPE_SCALAR && ret_type.u.scalar == PIR_SCALAR_VOID)) {
        check_ssa_type(v, ret_type, line, "return type");
    }
    if ((fn_attrs & (PIR_FATTR_RET_SIGNEXT | PIR_FATTR_RET_ZEROEXT)) != 0u) {
        if (ret_type.kind != PIR_TYPE_SCALAR ||
            (ret_type.u.scalar != PIR_SCALAR_I8 && ret_type.u.scalar != PIR_SCALAR_I16)) {
            pir_diag_error(v->diag, line, 0u, "'%s': signext/zeroext return only valid on i8/i16", owner);
        }
        if ((fn_attrs & PIR_FATTR_RET_SIGNEXT) != 0u && (fn_attrs & PIR_FATTR_RET_ZEROEXT) != 0u) {
            pir_diag_error(v->diag, line, 0u, "'%s': return cannot have both signext and zeroext", owner);
        }
    }
    for (uint32_t i = 0u; i < param_count; ++i) {
        const PirParamDecl *pd = &params[i];
        if (pd->name && pd->name[0] != '\0') {
            for (uint32_t j = 0u; j < i; ++j) {
                if (params[j].name && strcmp(params[j].name, pd->name) == 0) {
                    pir_diag_error(v->diag, line, 0u, "'%s': duplicate parameter name '%%%s'",
                                   owner, pd->name);
                    break;
                }
            }
        }
        if (pd->abi_kind != PIR_PARAM_NORMAL) {
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, line, 0u, "LIR forbids byval/retbuf parameter on '%s'", owner);
            }
            if (cc != PIR_CC_C) {
                pir_diag_error(v->diag, line, 0u, "'%s': byval/retbuf requires cc \"c\"", owner);
            }
            if (pd->abi_kind == PIR_PARAM_RETBUF) {
                if (i != 0u) {
                    pir_diag_error(v->diag, line, 0u, "'%s': retbuf must be parameter 0", owner);
                }
                if (!(ret_type.kind == PIR_TYPE_SCALAR && ret_type.u.scalar == PIR_SCALAR_VOID)) {
                    pir_diag_error(v->diag, line, 0u, "'%s': retbuf function must return void", owner);
                }
                if (pd->abi_type.kind != PIR_TYPE_NAMED_AGG || is_pad_agg_type(v->mod, pd->abi_type)) {
                    pir_diag_error(v->diag, line, 0u, "'%s': retbuf requires aggregate type", owner);
                }
            } else { /* BYVAL */
                bool ok = (pd->abi_type.kind == PIR_TYPE_NAMED_AGG && !is_pad_agg_type(v->mod, pd->abi_type)) ||
                          (pd->abi_type.kind == PIR_TYPE_SCALAR &&
                           pd->abi_type.u.scalar == PIR_SCALAR_VALIST);
                if (!ok) {
                    pir_diag_error(v->diag, line, 0u, "'%s': byval requires aggregate or va_list", owner);
                }
            }
        } else {
            check_ssa_type(v, pd->type, line, "parameter");
        }
        if (pd->abi_byval_size > 0u || pd->abi_byval_align != 0u ||
            pd->abi_stack_align != 0u || pd->abi_force_stack) {
            if (cc != PIR_CC_C) {
                pir_diag_error(v->diag, line, 0u, "'%s': lowered C ABI parameter metadata requires cc \"c\"", owner);
            }
        }
        if (pd->abi_byval_size > 0u) {
            if (pd->abi_kind != PIR_PARAM_NORMAL || !is_ptr_type(pd->type)) {
                pir_diag_error(v->diag, line, 0u, "'%s': byval_size only valid on normal ptr parameter", owner);
            } else if (pd->abi_byval_size > max_sz) {
                pir_diag_error(v->diag, line, 0u, "'%s': byval_size %" PRIu64 " exceeds maximum", owner, pd->abi_byval_size);
            }
        }
        if (pd->abi_byval_align != 0u) {
            if (pd->abi_byval_size == 0u || !align_valid_u32(pd->abi_byval_align)) {
                pir_diag_error(v->diag, line, 0u, "'%s': invalid byval_align %u", owner, pd->abi_byval_align);
            }
        }
        if (pd->abi_stack_align != 0u && !align_valid_u32(pd->abi_stack_align)) {
            pir_diag_error(v->diag, line, 0u, "'%s': invalid stack_align %u", owner, pd->abi_stack_align);
        }
        if ((pd->attrs & (PIR_PATTR_NOALIAS | PIR_PATTR_READONLY)) != 0u &&
            !(pd->type.kind == PIR_TYPE_SCALAR && pd->type.u.scalar == PIR_SCALAR_PTR)) {
            pir_diag_error(v->diag, line, 0u, "'%s': noalias/readonly only valid on ptr parameter", owner);
        }
        if ((pd->attrs & (PIR_PATTR_SIGNEXT | PIR_PATTR_ZEROEXT)) != 0u) {
            if (pd->type.kind != PIR_TYPE_SCALAR ||
                (pd->type.u.scalar != PIR_SCALAR_I8 && pd->type.u.scalar != PIR_SCALAR_I16)) {
                pir_diag_error(v->diag, line, 0u, "'%s': signext/zeroext only valid on i8/i16 parameter", owner);
            }
            if ((pd->attrs & PIR_PATTR_SIGNEXT) != 0u && (pd->attrs & PIR_PATTR_ZEROEXT) != 0u) {
                pir_diag_error(v->diag, line, 0u, "'%s': parameter cannot have both signext and zeroext", owner);
            }
        }
    }
    if (cabi_ret && cabi_ret->kind != PIR_CABI_RET_NONE) {
        if (cc != PIR_CC_C) {
            pir_diag_error(v->diag, line, 0u, "'%s': sret/multi_ret requires cc \"c\"", owner);
        }
        if (v->target) {
            if (cabi_ret->kind == PIR_CABI_RET_SRET_X8 && v->target->arch != PIR_ARCH_ARM64) {
                pir_diag_error(v->diag, line, 0u, "'%s': sret_x8 is only valid on ARM64 targets", owner);
            }
            if (cabi_ret->kind == PIR_CABI_RET_SRET_RAX && v->target->arch != PIR_ARCH_X86_64) {
                pir_diag_error(v->diag, line, 0u, "'%s': sret_rax is only valid on x86-64 targets", owner);
            }
            if (cabi_ret->kind == PIR_CABI_RET_MULTI_REG &&
                v->target->arch == PIR_ARCH_X86_64 && cabi_ret->reg_count > 2u) {
                pir_diag_error(v->diag, line, 0u, "'%s': multi_ret on x86-64 supports at most 2 registers", owner);
            }
        }
        if (!(ret_type.kind == PIR_TYPE_SCALAR && ret_type.u.scalar == PIR_SCALAR_VOID)) {
            pir_diag_error(v->diag, line, 0u, "'%s': sret/multi_ret requires void return type", owner);
        }
        if (param_count == 0u || params[0].abi_kind != PIR_PARAM_NORMAL || !is_ptr_type(params[0].type)) {
            pir_diag_error(v->diag, line, 0u, "'%s': sret/multi_ret requires normal ptr parameter 0", owner);
        } else if (params[0].abi_byval_size != 0u) {
            pir_diag_error(v->diag, line, 0u, "'%s': sret/multi_ret parameter 0 cannot have byval_size", owner);
        }
        if (cabi_ret->kind == PIR_CABI_RET_MULTI_REG) {
            if (cabi_ret->reg_count < 2u || cabi_ret->reg_count > 4u) {
                pir_diag_error(v->diag, line, 0u, "'%s': multi_ret requires 2..4 registers", owner);
            } else {
                for (uint32_t r = 0u; r < cabi_ret->reg_count; ++r) {
                    PirType rt = cabi_ret->regs[r];
                    check_ssa_type(v, rt, line, "multi_ret register");
                    if (rt.kind == PIR_TYPE_SCALAR && rt.u.scalar == PIR_SCALAR_I128) {
                        pir_diag_error(v->diag, line, 0u, "'%s': multi_ret register cannot be i128", owner);
                    }
                    if (rt.kind == PIR_TYPE_VECTOR && v->target &&
                        pir_vec_bits(rt.u.vec) > v->target->caps.max_simd_bits) {
                        pir_diag_error(v->diag, line, 0u,
                                       "'%s': multi_ret vector register exceeds target max_simd_bits", owner);
                    }
                }
            }
            uint64_t max_mr_sz = (uint64_t)cabi_ret->reg_count * 16u;
            if (cabi_ret->agg_size == 0u || cabi_ret->agg_size > max_mr_sz) {
                pir_diag_error(v->diag, line, 0u, "'%s': multi_ret size %" PRIu64 " is invalid",
                               owner, cabi_ret->agg_size);
            }
            if (!align_valid_u32(cabi_ret->agg_align)) {
                pir_diag_error(v->diag, line, 0u, "'%s': multi_ret align %u is invalid",
                               owner, cabi_ret->agg_align);
            }
        }
    }
}

static bool is_atomic_type(PirType t) {
    if (t.kind != PIR_TYPE_SCALAR) return false;
    switch (t.u.scalar) {
        case PIR_SCALAR_I8:
        case PIR_SCALAR_I16:
        case PIR_SCALAR_I32:
        case PIR_SCALAR_I64:
        case PIR_SCALAR_PTR:
            return true;
        case PIR_SCALAR_VOID: case PIR_SCALAR_I128:
        case PIR_SCALAR_F32:  case PIR_SCALAR_F64:
        case PIR_SCALAR_VALIST:
            return false;
    }
    return false;
}

static bool is_i8_type(PirType t) {
    return t.kind == PIR_TYPE_SCALAR && t.u.scalar == PIR_SCALAR_I8;
}

static bool is_i32_or_i64(PirType t) {
    return t.kind == PIR_TYPE_SCALAR &&
           (t.u.scalar == PIR_SCALAR_I32 || t.u.scalar == PIR_SCALAR_I64);
}

static uint32_t popcount_u32(uint32_t x) {
    uint32_t c = 0u;
    while (x) { c += (x & 1u); x >>= 1u; }
    return c;
}

static void check_flags(Verifier *v, const PirInst *inst) {
    uint32_t f = inst->flags;
    if (f == PIR_FLAG_NONE) return;
    uint32_t allowed = PIR_FLAG_NONE;
    switch (inst->op) {
        case PIR_OP_ADD: case PIR_OP_SUB: case PIR_OP_MUL:
            allowed = PIR_FLAG_WRAP | PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U |
                      PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U;
            if ((f & ~allowed) != 0u) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "invalid semantic flag for integer add/sub/mul");
                return;
            }
            /* Allowed combinations: single flag, or [unsafe_s, unsafe_u]. */
            if (popcount_u32(f) > 1u && f != (PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "conflicting semantic flags on add/sub/mul");
            }
            return;
        case PIR_OP_DIV_S: case PIR_OP_REM_S:
            allowed = PIR_FLAG_TRAP | PIR_FLAG_WRAP | PIR_FLAG_UNSAFE;
            break;
        case PIR_OP_DIV_U: case PIR_OP_REM_U:
            allowed = PIR_FLAG_TRAP | PIR_FLAG_UNSAFE;
            break;
        case PIR_OP_SHL: case PIR_OP_SHR: case PIR_OP_SAR:
            allowed = PIR_FLAG_MASK | PIR_FLAG_TRAP | PIR_FLAG_UNSAFE;
            break;
        case PIR_OP_CLZ: case PIR_OP_CTZ:
            allowed = PIR_FLAG_NONZERO;
            break;
        case PIR_OP_FADD: case PIR_OP_FSUB: case PIR_OP_FMUL: case PIR_OP_FDIV:
            allowed = PIR_FLAG_FAST;
            break;
        case PIR_OP_FPTOSI: case PIR_OP_FPTOUI:
            allowed = PIR_FLAG_SAT | PIR_FLAG_TRAP | PIR_FLAG_UNSAFE;
            break;
        case PIR_OP_PTR_ADD:
            allowed = PIR_FLAG_WRAP | PIR_FLAG_INBOUNDS;
            break;
        case PIR_OP_LOAD: case PIR_OP_STORE:
            allowed = PIR_FLAG_VOLATILE;
            break;
        case PIR_OP_ATOMIC_CMPXCHG:
            allowed = PIR_FLAG_WEAK;
            break;
        case PIR_OP_CALL: case PIR_OP_CALL_INDIRECT:
            allowed = PIR_FLAG_NOUNWIND;
            break;
        case PIR_OP_CONST: case PIR_OP_ADDR:
        case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
        case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
        case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U:
        case PIR_OP_MUL_WIDE_U: case PIR_OP_MUL_WIDE_S:
        case PIR_OP_ADD_CARRY: case PIR_OP_SUB_BORROW:
        case PIR_OP_AND: case PIR_OP_OR: case PIR_OP_XOR:
        case PIR_OP_POPCNT: case PIR_OP_BSWAP:
        case PIR_OP_FNEG:
        case PIR_OP_CMP_EQ: case PIR_OP_CMP_NE:
        case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LT_U: case PIR_OP_CMP_LE_S: case PIR_OP_CMP_LE_U:
        case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GT_U: case PIR_OP_CMP_GE_S: case PIR_OP_CMP_GE_U:
        case PIR_OP_FCMP_OEQ: case PIR_OP_FCMP_ONE: case PIR_OP_FCMP_OLT:
        case PIR_OP_FCMP_OLE: case PIR_OP_FCMP_OGT: case PIR_OP_FCMP_OGE:
        case PIR_OP_FCMP_UEQ: case PIR_OP_FCMP_UNE: case PIR_OP_FCMP_ULT:
        case PIR_OP_FCMP_ULE: case PIR_OP_FCMP_UGT: case PIR_OP_FCMP_UGE:
        case PIR_OP_SELECT:
        case PIR_OP_ZEXT: case PIR_OP_SEXT: case PIR_OP_TRUNC:
        case PIR_OP_SITOFP: case PIR_OP_UITOFP:
        case PIR_OP_FPEXT: case PIR_OP_FPTRUNC: case PIR_OP_BITCAST:
        case PIR_OP_PTRTOINT: case PIR_OP_INTTOPTR:
        case PIR_OP_SPLAT: case PIR_OP_EXTRACT: case PIR_OP_INSERT: case PIR_OP_SHUFFLE:
        case PIR_OP_ALLOCA: case PIR_OP_ALLOCA_PTR: case PIR_OP_ALLOCA_DYN:
        case PIR_OP_STACKSAVE: case PIR_OP_STACKRESTORE:
        case PIR_OP_FIELD_PTR: case PIR_OP_ELEM_PTR:
        case PIR_OP_MEMCPY: case PIR_OP_MEMMOVE: case PIR_OP_MEMSET:
        case PIR_OP_ATOMIC_LOAD: case PIR_OP_ATOMIC_STORE:
        case PIR_OP_ATOMIC_RMW: case PIR_OP_FENCE:
        case PIR_OP_VA_START: case PIR_OP_VA_ARG: case PIR_OP_VA_ARG_BUF:
        case PIR_OP_VA_COPY: case PIR_OP_VA_END:
        case PIR_OP_JMP: case PIR_OP_BR: case PIR_OP_SWITCH:
        case PIR_OP_INVOKE: case PIR_OP_INVOKE_INDIRECT: case PIR_OP_RESUME:
        case PIR_OP_RET: case PIR_OP_TRAP: case PIR_OP_UNREACHABLE:
            allowed = PIR_FLAG_NONE;
            break;
    }
    if ((f & ~allowed) != 0u) {
        pir_diag_error(v->diag, inst->src_line, 0u, "invalid semantic flag for opcode");
    } else if (popcount_u32(f) > 1u) {
        pir_diag_error(v->diag, inst->src_line, 0u, "conflicting semantic flags on instruction");
    }
}

static void check_operand_kind_and_type(Verifier *v, const PirFunction *fn,
                                        const PirOperand *op, PirType expected,
                                        uint32_t line, const char *ctx);

static bool operand_has_call_meta(const PirOperand *op);

static void check_branch_target(Verifier *v, const PirFunction *fn, const PirInst *inst,
                                const PirBranchTarget *bt, bool allow_ret, bool allow_exn,
                                PirType invoke_ret_ty) {
    if (bt->target >= fn->blocks.len) {
        pir_diag_error(v->diag, inst->src_line, 0u, "invalid branch target block");
        return;
    }
    if (bt->target == 0u) {
        pir_diag_error(v->diag, inst->src_line, 0u,
                       "cannot branch to entry block '.%s'", fn->blocks.data[0].label);
        return;
    }
    const PirBlock *dst = &fn->blocks.data[bt->target];
    if (bt->arg_count != dst->params.len) {
        pir_diag_error(v->diag, inst->src_line, 0u,
                       "block argument count mismatch for '.%s' (expected %u, got %u)",
                       dst->label, dst->params.len, bt->arg_count);
        return;
    }
    for (uint32_t k = 0u; k < bt->arg_count; ++k) {
        const PirOperand *arg = &bt->args[k];
        if (operand_has_call_meta(arg)) {
            pir_diag_error(v->diag, inst->src_line, 0u,
                           "call-site ABI metadata not allowed on branch argument");
        }
        PirType expected = fn->values.data[dst->params.data[k]].type;
        if (arg->kind == PIR_OPER_INVOKE_RET) {
            if (!allow_ret || (invoke_ret_ty.kind == PIR_TYPE_SCALAR &&
                               invoke_ret_ty.u.scalar == PIR_SCALAR_VOID)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "'ret' placeholder only valid in non-void invoke 'then' edge");
                continue;
            }
            if (!pir_type_eq(arg->type, expected)) {
                char b1[64], b2[64];
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "block argument %u type mismatch for '.%s' (expected %s, got %s)",
                               k, dst->label,
                               type_str(v->mod, expected, b1, sizeof(b1)),
                               type_str(v->mod, arg->type, b2, sizeof(b2)));
            }
            continue;
        } else if (arg->kind == PIR_OPER_INVOKE_EXN) {
            if (!allow_exn) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "'exn' placeholder only valid in invoke 'unwind' edge");
                continue;
            }
            if (!pir_type_eq(arg->type, expected)) {
                char b1[64], b2[64];
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "block argument %u type mismatch for '.%s' (expected %s, got %s)",
                               k, dst->label,
                               type_str(v->mod, expected, b1, sizeof(b1)),
                               type_str(v->mod, arg->type, b2, sizeof(b2)));
            }
            continue;
        }
        check_operand_kind_and_type(v, fn, arg, expected, inst->src_line, "branch argument");
    }
}

static void check_call_args(Verifier *v, const PirFunction *fn, const PirInst *inst,
                            const char *callee_name, PirCallingConv callee_cc,
                            const PirParamDecl *params, uint32_t param_count,
                            bool is_variadic, uint32_t first_arg_idx) {
    uint64_t max_sz = max_obj_bytes(v->target);
    uint32_t provided = inst->operand_count - first_arg_idx;
    if ((!is_variadic && provided != param_count) || (is_variadic && provided < param_count)) {
        pir_diag_error(v->diag, inst->src_line, 0u,
                       "call to '%s': expected %s%u arguments, got %u",
                       callee_name, is_variadic ? "at least " : "", param_count, provided);
        return;
    }
    for (uint32_t i = 0u; i < provided; ++i) {
        const PirOperand *arg = &inst->operands[first_arg_idx + i];
        if (callee_cc != PIR_CC_C && operand_has_call_meta(arg)) {
            pir_diag_error(v->diag, inst->src_line, 0u,
                           "call to '%s': C ABI call-site metadata requires cc \"c\"", callee_name);
        }
        if (arg->call_abi == PIR_PARAM_BYVAL && v->mode == PIR_VERIFY_LIR) {
            pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids byval at call site");
        }
        if (arg->call_abi == PIR_PARAM_BYVAL && arg->call_byval_size > 0u) {
            pir_diag_error(v->diag, inst->src_line, 0u,
                           "call to '%s': cannot combine byval(@T) with byval_size", callee_name);
        }
        if (arg->call_byval_size > 0u) {
            if (!is_ptr_type(arg->type) || arg->kind == PIR_OPER_IMM_INT || arg->kind == PIR_OPER_IMM_FLOAT) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "call to '%s': byval_size requires ptr value or symbol", callee_name);
            } else if (arg->call_byval_size > max_sz) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "call to '%s': byval_size %" PRIu64 " exceeds maximum",
                               callee_name, arg->call_byval_size);
            }
        }
        if (arg->call_byval_align != 0u) {
            if (arg->call_byval_size == 0u || !align_valid_u32(arg->call_byval_align)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "call to '%s': invalid byval_align %u", callee_name, arg->call_byval_align);
            }
        }
        if (arg->call_stack_align != 0u && !align_valid_u32(arg->call_stack_align)) {
            pir_diag_error(v->diag, inst->src_line, 0u,
                           "call to '%s': invalid stack_align %u", callee_name, arg->call_stack_align);
        }
        if (arg->call_no_f32_promote &&
            !(arg->type.kind == PIR_TYPE_SCALAR && arg->type.u.scalar == PIR_SCALAR_F32)) {
            pir_diag_error(v->diag, inst->src_line, 0u,
                           "call to '%s': no_f32_promote only valid on f32 argument", callee_name);
        }
        if (i < param_count) {
            if (arg->call_byval_size != params[i].abi_byval_size ||
                arg->call_byval_align != params[i].abi_byval_align ||
                arg->call_stack_align != params[i].abi_stack_align ||
                arg->call_force_stack != params[i].abi_force_stack) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "call to '%s': argument %u ABI metadata mismatches callee parameter",
                               callee_name, i);
            }
            if (arg->call_abi == PIR_PARAM_BYVAL) {
                if (params[i].abi_kind != PIR_PARAM_BYVAL ||
                    !pir_type_eq(arg->call_abi_type, params[i].abi_type)) {
                    pir_diag_error(v->diag, inst->src_line, 0u,
                                   "call to '%s': argument %u byval annotation mismatches callee parameter",
                                   callee_name, i);
                }
            }
            if ((params[i].abi_kind == PIR_PARAM_BYVAL || params[i].abi_kind == PIR_PARAM_RETBUF) &&
                (arg->kind == PIR_OPER_IMM_INT || arg->kind == PIR_OPER_IMM_FLOAT)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "call to '%s': byval/retbuf argument %u must be ptr value or symbol",
                               callee_name, i);
            } else if (!pir_type_eq(arg->type, params[i].type)) {
                char b1[64], b2[64];
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "call to '%s': argument %u type mismatch (expected %s, got %s)",
                               callee_name, i,
                               type_str(v->mod, params[i].type, b1, sizeof(b1)),
                               type_str(v->mod, arg->type, b2, sizeof(b2)));
            } else {
                check_operand_kind_and_type(v, fn, arg, params[i].type, inst->src_line, "call argument");
            }
        } else {
            if (arg->call_abi == PIR_PARAM_BYVAL) {
                bool ok_abi = (arg->call_abi_type.kind == PIR_TYPE_NAMED_AGG &&
                               !is_pad_agg_type(v->mod, arg->call_abi_type)) ||
                              (arg->call_abi_type.kind == PIR_TYPE_SCALAR &&
                               arg->call_abi_type.u.scalar == PIR_SCALAR_VALIST);
                if (!ok_abi) {
                    pir_diag_error(v->diag, inst->src_line, 0u,
                                   "byval call argument requires aggregate or va_list type");
                }
                if (!is_ptr_type(arg->type) || arg->kind == PIR_OPER_IMM_INT || arg->kind == PIR_OPER_IMM_FLOAT) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "byval call argument must be ptr value or symbol");
                } else {
                    check_operand_kind_and_type(v, fn, arg, pir_type_scalar(PIR_SCALAR_PTR),
                                                inst->src_line, "byval call argument");
                }
            } else {
                check_ssa_type(v, arg->type, inst->src_line, "variadic call argument");
                check_operand_kind_and_type(v, fn, arg, arg->type, inst->src_line, "variadic call argument");
            }
        }
    }
}

static void check_dom_operand(Verifier *v, const PirFunction *fn, const PirDom *dom,
                              PirBlockId use_blk, uint32_t use_pos, const uint32_t *inst_pos,
                              const PirBlockId *inst_blk, const PirOperand *op, uint32_t line) {
    if (op->kind != PIR_OPER_VAL || op->u.val >= fn->values.len) return;
    const PirValueDef *vd = &fn->values.data[op->u.val];
    if (vd->def_block == PIR_INVALID_ID) {
        return; /* Function parameter dominates entire function. */
    }
    if (vd->def_block >= fn->blocks.len) {
        pir_diag_error(v->diag, line, 0u, "use of '%%%s' references invalid defining block", vd->name);
        return;
    }
    if (vd->def_inst != PIR_INVALID_ID) {
        if (vd->def_inst >= fn->insts.len || inst_pos[vd->def_inst] == PIR_INVALID_ID ||
            inst_blk[vd->def_inst] != vd->def_block) {
            pir_diag_error(v->diag, line, 0u,
                           "use of '%%%s' references deleted or detached instruction", vd->name);
            return;
        }
    }
    if (vd->def_block == use_blk) {
        if (vd->def_inst == PIR_INVALID_ID) return; /* Block param of same block. */
        if (inst_pos[vd->def_inst] >= use_pos) {
            pir_diag_error(v->diag, line, 0u,
                           "use of '%%%s' in '.%s' precedes its definition",
                           vd->name, fn->blocks.data[use_blk].label);
        }
        return;
    }
    if (!pir_dom_dominates(dom, vd->def_block, use_blk)) {
        pir_diag_error(v->diag, line, 0u,
                       "use of '%%%s' in '.%s' is not dominated by definition in '.%s'",
                       vd->name, fn->blocks.data[use_blk].label,
                       fn->blocks.data[vd->def_block].label);
    }
}

static void check_result_count(Verifier *v, const PirInst *inst, uint32_t expected) {
    if (inst->results[0] == PIR_INVALID_ID && inst->results[1] != PIR_INVALID_ID) {
        pir_diag_error(v->diag, inst->src_line, 0u,
                       "instruction has results[1] set without results[0]");
        return;
    }
    uint32_t got = (inst->results[0] != PIR_INVALID_ID ? 1u : 0u) +
                   (inst->results[1] != PIR_INVALID_ID ? 1u : 0u);
    if (got != expected) {
        pir_diag_error(v->diag, inst->src_line, 0u,
                       "instruction expects %u result(s), got %u", expected, got);
    }
}

static void check_op_count(Verifier *v, const PirInst *inst, uint32_t expected) {
    if (inst->operand_count != expected) {
        pir_diag_error(v->diag, inst->src_line, 0u,
                       "instruction expects %u operand(s), got %u", expected, inst->operand_count);
    }
}

static bool fail_order_valid(PirMemOrder succ, PirMemOrder fail) {
    if (succ == PIR_ORDER_NONE || fail == PIR_ORDER_RELEASE ||
        fail == PIR_ORDER_ACQ_REL || fail == PIR_ORDER_NONE) {
        return false;
    }
    if (fail == PIR_ORDER_RELAXED) return true;
    if (fail == PIR_ORDER_ACQUIRE) {
        return succ == PIR_ORDER_ACQUIRE || succ == PIR_ORDER_ACQ_REL || succ == PIR_ORDER_SEQ_CST;
    }
    if (fail == PIR_ORDER_SEQ_CST) {
        return succ == PIR_ORDER_SEQ_CST;
    }
    return false;
}

static void check_int_imm_range(Verifier *v, uint32_t bits, uint64_t lo, uint64_t hi,
                                bool is_neg, uint32_t line, const char *ctx) {
    if (bits < 128u && hi != 0u) {
        pir_diag_error(v->diag, line, 0u, "128-bit integer immediate out of range for type in %s", ctx);
        return;
    }
    if (bits == 8u || bits == 16u || bits == 32u) {
        uint64_t max_pos = (UINT64_C(1) << bits) - 1u;
        uint64_t max_neg = UINT64_C(1) << (bits - 1u);
        if (is_neg ? (lo > max_neg) : (lo > max_pos)) {
            pir_diag_error(v->diag, line, 0u,
                           "integer immediate out of %u-bit range in %s", bits, ctx);
        }
    } else if (bits == 64u) {
        if (is_neg && lo > (UINT64_C(1) << 63)) {
            pir_diag_error(v->diag, line, 0u,
                           "integer immediate out of 64-bit range in %s", ctx);
        }
    } else if (bits == 128u) {
        if (is_neg && (hi > (UINT64_C(1) << 63) || (hi == (UINT64_C(1) << 63) && lo != 0u))) {
            pir_diag_error(v->diag, line, 0u,
                           "integer immediate out of 128-bit range in %s", ctx);
        }
    }
}

static void check_operand_kind_and_type(Verifier *v, const PirFunction *fn,
                                        const PirOperand *op, PirType expected,
                                        uint32_t line, const char *ctx) {
    if (op->kind == PIR_OPER_VAL) {
        if (op->u.val >= fn->values.len) {
            pir_diag_error(v->diag, line, 0u, "invalid value id in %s", ctx);
            return;
        }
        PirType actual = fn->values.data[op->u.val].type;
        if (!pir_type_eq(actual, expected) || !pir_type_eq(op->type, expected)) {
            pir_diag_error(v->diag, line, 0u, "operand type mismatch on %s", ctx);
        }
    } else if (op->kind == PIR_OPER_IMM_INT) {
        if (!(expected.kind == PIR_TYPE_SCALAR &&
              (pir_scalar_is_int(expected.u.scalar) || expected.u.scalar == PIR_SCALAR_PTR))) {
            pir_diag_error(v->diag, line, 0u, "integer immediate invalid for type in %s", ctx);
        } else if (expected.u.scalar == PIR_SCALAR_PTR) {
            if (op->u.imm_int.lo != 0u || op->u.imm_int.hi != 0u || op->u.imm_int.is_negative) {
                pir_diag_error(v->diag, line, 0u, "only null literal '0' is valid for ptr in %s", ctx);
            }
        } else {
            uint32_t bits = pir_type_bits(expected, v->target ? v->target->ptr_bytes : 8u);
            check_int_imm_range(v, bits, op->u.imm_int.lo, op->u.imm_int.hi,
                                op->u.imm_int.is_negative, line, ctx);
        }
    } else if (op->kind == PIR_OPER_IMM_FLOAT) {
        if (!(expected.kind == PIR_TYPE_SCALAR && pir_scalar_is_float(expected.u.scalar))) {
            pir_diag_error(v->diag, line, 0u, "float immediate invalid for type in %s", ctx);
        }
    } else if (op->kind == PIR_OPER_SYM) {
        if (!is_ptr_type(expected)) {
            pir_diag_error(v->diag, line, 0u, "symbol operand requires ptr type in %s", ctx);
        }
        if (!op->u.sym.name || !has_symbol(v->mod, op->u.sym.name)) {
            pir_diag_error(v->diag, line, 0u, "undefined symbol '@%s' in %s",
                           op->u.sym.name ? op->u.sym.name : "?", ctx);
        }
    }
}

static bool operand_has_call_meta(const PirOperand *op) {
    return op->call_abi != PIR_PARAM_NORMAL ||
           op->call_stack_align != 0u ||
           op->call_byval_size != 0u ||
           op->call_byval_align != 0u ||
           op->call_force_stack ||
           op->call_no_f32_promote;
}

static void verify_inst(Verifier *v, const PirFunction *fn, const PirInst *inst) {
    check_flags(v, inst);
    uint8_t ptr_bytes = v->target ? v->target->ptr_bytes : 8u;

    if (inst->op != PIR_OP_CALL && inst->op != PIR_OP_CALL_INDIRECT &&
        inst->op != PIR_OP_INVOKE && inst->op != PIR_OP_INVOKE_INDIRECT) {
        for (uint32_t k = 0u; k < inst->operand_count; ++k) {
            if (operand_has_call_meta(&inst->operands[k])) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "call-site ABI metadata not allowed on non-call instruction");
                break;
            }
        }
    } else if (inst->operand_count > 0u && operand_has_call_meta(&inst->operands[0])) {
        pir_diag_error(v->diag, inst->src_line, 0u,
                       "call-site ABI metadata not allowed on callee operand");
    }

    if (inst->loc_idx != 0u && inst->loc_idx >= fn->locs.len) {
        pir_diag_error(v->diag, inst->src_line, 0u, "instruction references out-of-bounds loc index %u",
                       inst->loc_idx);
    }

    /* Check LIR-legality of any defined result types. */
    if (inst->results[0] != PIR_INVALID_ID) {
        if (inst->results[0] >= fn->values.len) {
            pir_diag_error(v->diag, inst->src_line, 0u, "invalid result[0] value id");
        } else {
            check_ssa_type(v, fn->values.data[inst->results[0]].type, inst->src_line, "result");
        }
    }
    if (inst->results[1] != PIR_INVALID_ID) {
        if (inst->results[1] >= fn->values.len) {
            pir_diag_error(v->diag, inst->src_line, 0u, "invalid result[1] value id");
        } else {
            check_ssa_type(v, fn->values.data[inst->results[1]].type, inst->src_line, "result");
        }
    }

    switch (inst->op) {
        case PIR_OP_CONST:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            check_ssa_type(v, inst->ty, inst->src_line, "const");
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "const requires scalar type (use splat for vectors)");
            }
            if (inst->operand_count == 1u) {
                if (inst->operands[0].kind == PIR_OPER_VAL) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "const operand must be an immediate or @symbol");
                } else if (inst->operands[0].kind == PIR_OPER_SYM) {
                    if (!is_ptr_type(inst->ty)) {
                        pir_diag_error(v->diag, inst->src_line, 0u, "symbol const requires ptr type");
                    }
                    if (!has_symbol(v->mod, inst->operands[0].u.sym.name)) {
                        pir_diag_error(v->diag, inst->src_line, 0u,
                                       "undefined symbol '@%s'", inst->operands[0].u.sym.name);
                    }
                } else {
                    check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "const");
                }
            }
            break;

        case PIR_OP_ADDR:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (inst->operand_count == 1u && (inst->operands[0].kind != PIR_OPER_SYM ||
                !has_symbol(v->mod, inst->operands[0].u.sym.name))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "addr requires defined @symbol");
            }
            break;

        case PIR_OP_ADD: case PIR_OP_SUB: case PIR_OP_MUL:
        case PIR_OP_DIV_S: case PIR_OP_DIV_U: case PIR_OP_REM_S: case PIR_OP_REM_U:
        case PIR_OP_AND: case PIR_OP_OR: case PIR_OP_XOR:
        case PIR_OP_SHL: case PIR_OP_SHR: case PIR_OP_SAR:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (!pir_type_is_int(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "integer opcode requires integer type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "integer op");
            if (inst->operand_count == 2u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "integer op");
                check_operand_kind_and_type(v, fn, &inst->operands[1], inst->ty, inst->src_line, "integer op");
            }
            break;

        case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
        case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
        case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U:
            if (v->mode == PIR_VERIFY_LIR && v->target && !v->target->caps.has_ovf_ops) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR target does not support *_ovf_* ops");
            }
            check_result_count(v, inst, 2u);
            check_op_count(v, inst, 2u);
            if (!(inst->ty.kind == PIR_TYPE_SCALAR && pir_scalar_is_int(inst->ty.u.scalar))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "*_ovf_* requires scalar integer type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "ovf op");
            if (inst->operand_count == 2u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "*_ovf_*");
                check_operand_kind_and_type(v, fn, &inst->operands[1], inst->ty, inst->src_line, "*_ovf_*");
            }
            break;

        case PIR_OP_ADD_CARRY: case PIR_OP_SUB_BORROW:
            if (v->mode == PIR_VERIFY_LIR && v->target &&
                (!v->target->caps.has_ovf_ops || !v->target->caps.has_add_carry)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR target does not support add_carry/sub_borrow");
            }
            check_result_count(v, inst, 2u);
            check_op_count(v, inst, 3u);
            if (!(inst->ty.kind == PIR_TYPE_SCALAR && pir_scalar_is_int(inst->ty.u.scalar))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "add_carry/sub_borrow requires scalar integer");
            }
            check_lir_type(v, inst->ty, inst->src_line, "carry/borrow op");
            if (inst->operand_count == 3u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "add_carry/sub_borrow");
                check_operand_kind_and_type(v, fn, &inst->operands[1], inst->ty, inst->src_line, "add_carry/sub_borrow");
                check_operand_kind_and_type(v, fn, &inst->operands[2], pir_type_scalar(PIR_SCALAR_I8),
                                            inst->src_line, "add_carry/sub_borrow");
            }
            break;

        case PIR_OP_MUL_WIDE_U: case PIR_OP_MUL_WIDE_S:
            if (v->mode == PIR_VERIFY_LIR && v->target && !v->target->caps.has_mul_wide) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR target does not support mul_wide_*");
            }
            check_result_count(v, inst, 2u);
            check_op_count(v, inst, 2u);
            if (!is_i32_or_i64(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "mul_wide_* requires i32 or i64");
            }
            if (inst->operand_count == 2u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "mul_wide_*");
                check_operand_kind_and_type(v, fn, &inst->operands[1], inst->ty, inst->src_line, "mul_wide_*");
            }
            break;

        case PIR_OP_CLZ: case PIR_OP_CTZ: case PIR_OP_POPCNT: case PIR_OP_BSWAP:
            if (v->mode == PIR_VERIFY_LIR && v->target && !v->target->caps.has_bitops) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR target does not support bitops");
            }
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!(inst->ty.kind == PIR_TYPE_SCALAR && pir_scalar_is_int(inst->ty.u.scalar))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "bitop requires scalar integer");
            }
            if (inst->op == PIR_OP_BSWAP && is_i8_type(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "bswap is not valid on i8");
            }
            check_lir_type(v, inst->ty, inst->src_line, "bitop");
            if (inst->operand_count == 1u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "bitop");
            }
            break;

        case PIR_OP_FADD: case PIR_OP_FSUB: case PIR_OP_FMUL: case PIR_OP_FDIV:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (!pir_type_is_float(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "float op requires float type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "float op");
            if (inst->operand_count == 2u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "float op");
                check_operand_kind_and_type(v, fn, &inst->operands[1], inst->ty, inst->src_line, "float op");
            }
            break;

        case PIR_OP_FNEG:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!pir_type_is_float(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "fneg requires float type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "fneg");
            if (inst->operand_count == 1u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "fneg");
            }
            break;

        case PIR_OP_CMP_EQ: case PIR_OP_CMP_NE:
        case PIR_OP_CMP_LT_U: case PIR_OP_CMP_LE_U:
        case PIR_OP_CMP_GT_U: case PIR_OP_CMP_GE_U:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (!pir_type_is_int(inst->ty) && !is_ptr_type(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "cmp requires integer or ptr type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "cmp");
            if (inst->results[0] != PIR_INVALID_ID) {
                PirType exp_res = (inst->ty.kind == PIR_TYPE_VECTOR)
                                  ? pir_type_vec(pir_vec_mask_type(inst->ty.u.vec))
                                  : pir_type_scalar(PIR_SCALAR_I8);
                if (!pir_type_eq(fn->values.data[inst->results[0]].type, exp_res)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "cmp result type mismatch");
                }
            }
            if (inst->operand_count == 2u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "cmp");
                check_operand_kind_and_type(v, fn, &inst->operands[1], inst->ty, inst->src_line, "cmp");
            }
            break;

        case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LE_S:
        case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GE_S:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (!pir_type_is_int(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "signed cmp requires integer type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "signed cmp");
            if (inst->results[0] != PIR_INVALID_ID) {
                PirType exp_res = (inst->ty.kind == PIR_TYPE_VECTOR)
                                  ? pir_type_vec(pir_vec_mask_type(inst->ty.u.vec))
                                  : pir_type_scalar(PIR_SCALAR_I8);
                if (!pir_type_eq(fn->values.data[inst->results[0]].type, exp_res)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "signed cmp result type mismatch");
                }
            }
            if (inst->operand_count == 2u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "signed cmp");
                check_operand_kind_and_type(v, fn, &inst->operands[1], inst->ty, inst->src_line, "signed cmp");
            }
            break;

        case PIR_OP_FCMP_OEQ: case PIR_OP_FCMP_ONE: case PIR_OP_FCMP_OLT:
        case PIR_OP_FCMP_OLE: case PIR_OP_FCMP_OGT: case PIR_OP_FCMP_OGE:
        case PIR_OP_FCMP_UEQ: case PIR_OP_FCMP_UNE: case PIR_OP_FCMP_ULT:
        case PIR_OP_FCMP_ULE: case PIR_OP_FCMP_UGT: case PIR_OP_FCMP_UGE:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (!pir_type_is_float(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "fcmp requires float type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "fcmp");
            if (inst->results[0] != PIR_INVALID_ID) {
                PirType exp_res = (inst->ty.kind == PIR_TYPE_VECTOR)
                                  ? pir_type_vec(pir_vec_mask_type(inst->ty.u.vec))
                                  : pir_type_scalar(PIR_SCALAR_I8);
                if (!pir_type_eq(fn->values.data[inst->results[0]].type, exp_res)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "fcmp result type mismatch");
                }
            }
            if (inst->operand_count == 2u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "fcmp");
                check_operand_kind_and_type(v, fn, &inst->operands[1], inst->ty, inst->src_line, "fcmp");
            }
            break;

        case PIR_OP_SELECT:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 3u);
            check_ssa_type(v, inst->ty, inst->src_line, "select");
            if (inst->operand_count == 3u) {
                PirType cond_ty = (inst->ty.kind == PIR_TYPE_VECTOR)
                                  ? pir_type_vec(pir_vec_mask_type(inst->ty.u.vec))
                                  : pir_type_scalar(PIR_SCALAR_I8);
                check_operand_kind_and_type(v, fn, &inst->operands[0], cond_ty, inst->src_line, "select condition");
                check_operand_kind_and_type(v, fn, &inst->operands[1], inst->ty, inst->src_line, "select value");
                check_operand_kind_and_type(v, fn, &inst->operands[2], inst->ty, inst->src_line, "select value");
            }
            break;

        case PIR_OP_ZEXT: case PIR_OP_SEXT: case PIR_OP_TRUNC:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!pir_type_is_int(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "int cast requires integer dst type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "int cast");
            if (inst->operand_count == 1u) {
                PirType src = inst->operands[0].type;
                check_operand_kind_and_type(v, fn, &inst->operands[0], src, inst->src_line, "int cast");
                check_lir_type(v, src, inst->src_line, "int cast src");
                if (!pir_type_is_int(src) || src.kind != inst->ty.kind) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "int cast requires matching int src kind");
                } else {
                    uint32_t sb = (src.kind == PIR_TYPE_VECTOR)
                                  ? pir_scalar_bits(pir_vec_elem_scalar(src.u.vec), ptr_bytes)
                                  : pir_scalar_bits(src.u.scalar, ptr_bytes);
                    uint32_t db = (inst->ty.kind == PIR_TYPE_VECTOR)
                                  ? pir_scalar_bits(pir_vec_elem_scalar(inst->ty.u.vec), ptr_bytes)
                                  : pir_scalar_bits(inst->ty.u.scalar, ptr_bytes);
                    if (src.kind == PIR_TYPE_VECTOR &&
                        pir_vec_lanes(src.u.vec) != pir_vec_lanes(inst->ty.u.vec)) {
                        pir_diag_error(v->diag, inst->src_line, 0u, "vector cast lane count mismatch");
                    }
                    if (inst->op == PIR_OP_TRUNC ? (db >= sb) : (db <= sb)) {
                        pir_diag_error(v->diag, inst->src_line, 0u, "invalid integer cast bit-width direction");
                    }
                }
            }
            break;

        case PIR_OP_FPEXT: case PIR_OP_FPTRUNC:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!pir_type_is_float(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "fpext/fptrunc requires float dst");
            }
            check_lir_type(v, inst->ty, inst->src_line, "fp cast");
            if (inst->operand_count == 1u) {
                PirType src = inst->operands[0].type;
                check_operand_kind_and_type(v, fn, &inst->operands[0], src, inst->src_line, "fp cast");
                check_lir_type(v, src, inst->src_line, "fp cast src");
                if (!pir_type_is_float(src) || src.kind != inst->ty.kind) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "fpext/fptrunc requires float src");
                } else {
                    PirScalarType ss = (src.kind == PIR_TYPE_VECTOR) ? pir_vec_elem_scalar(src.u.vec) : src.u.scalar;
                    PirScalarType ds = (inst->ty.kind == PIR_TYPE_VECTOR) ? pir_vec_elem_scalar(inst->ty.u.vec) : inst->ty.u.scalar;
                    if (src.kind == PIR_TYPE_VECTOR &&
                        pir_vec_lanes(src.u.vec) != pir_vec_lanes(inst->ty.u.vec)) {
                        pir_diag_error(v->diag, inst->src_line, 0u, "vector fp cast lane mismatch");
                    }
                    if (inst->op == PIR_OP_FPEXT && !(ss == PIR_SCALAR_F32 && ds == PIR_SCALAR_F64)) {
                        pir_diag_error(v->diag, inst->src_line, 0u, "fpext requires f32 -> f64");
                    }
                    if (inst->op == PIR_OP_FPTRUNC && !(ss == PIR_SCALAR_F64 && ds == PIR_SCALAR_F32)) {
                        pir_diag_error(v->diag, inst->src_line, 0u, "fptrunc requires f64 -> f32");
                    }
                }
            }
            break;

        case PIR_OP_FPTOSI: case PIR_OP_FPTOUI:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!pir_type_is_int(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "fptosi/fptoui requires integer dst");
            }
            check_lir_type(v, inst->ty, inst->src_line, "fpto*");
            if (inst->operand_count == 1u) {
                PirType src = inst->operands[0].type;
                check_operand_kind_and_type(v, fn, &inst->operands[0], src, inst->src_line, "fpto*");
                check_lir_type(v, src, inst->src_line, "fpto* src");
                if (!pir_type_is_float(src) || src.kind != inst->ty.kind ||
                    (src.kind == PIR_TYPE_VECTOR &&
                     pir_vec_lanes(src.u.vec) != pir_vec_lanes(inst->ty.u.vec))) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "fptosi/fptoui requires matching float src");
                }
            }
            break;

        case PIR_OP_SITOFP: case PIR_OP_UITOFP:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!pir_type_is_float(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "sitofp/uitofp requires float dst");
            }
            check_lir_type(v, inst->ty, inst->src_line, "*tofp");
            if (inst->operand_count == 1u) {
                PirType src = (inst->operands[0].kind == PIR_OPER_VAL && inst->operands[0].u.val < fn->values.len)
                              ? fn->values.data[inst->operands[0].u.val].type
                              : inst->operands[0].type;
                check_operand_kind_and_type(v, fn, &inst->operands[0], src, inst->src_line, "*tofp");
                check_lir_type(v, src, inst->src_line, "*tofp src");
                if (!pir_type_is_int(src) || src.kind != inst->ty.kind ||
                    (src.kind == PIR_TYPE_VECTOR &&
                     pir_vec_lanes(src.u.vec) != pir_vec_lanes(inst->ty.u.vec))) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "sitofp/uitofp requires matching int src");
                }
            }
            break;

        case PIR_OP_PTRTOINT:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!is_i32_or_i64(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "ptrtoint dst must be i32 or i64");
            }
            if (inst->operand_count == 1u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "ptrtoint");
            }
            break;

        case PIR_OP_INTTOPTR:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!is_ptr_type(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "inttoptr dst must be ptr");
            }
            if (inst->operand_count == 1u) {
                if (!is_i32_or_i64(inst->operands[0].type)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "inttoptr src must be i32 or i64");
                } else {
                    check_operand_kind_and_type(v, fn, &inst->operands[0], inst->operands[0].type,
                                                inst->src_line, "inttoptr");
                }
            }
            break;

        case PIR_OP_BITCAST:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            check_ssa_type(v, inst->ty, inst->src_line, "bitcast");
            if (inst->operand_count == 1u) {
                const PirOperand *op0 = &inst->operands[0];
                PirType src = (op0->kind == PIR_OPER_VAL && op0->u.val < fn->values.len)
                              ? fn->values.data[op0->u.val].type
                              : op0->type;
                if (op0->kind == PIR_OPER_VAL) {
                    check_operand_kind_and_type(v, fn, op0, src, inst->src_line, "bitcast");
                } else if (op0->kind == PIR_OPER_SYM) {
                    if (!is_ptr_type(inst->ty)) {
                        pir_diag_error(v->diag, inst->src_line, 0u,
                                       "symbol operand in bitcast requires ptr destination type");
                    } else {
                        check_operand_kind_and_type(v, fn, op0, pir_type_scalar(PIR_SCALAR_PTR),
                                                    inst->src_line, "bitcast");
                    }
                } else if (op0->kind == PIR_OPER_IMM_INT) {
                    uint32_t db_imm = pir_type_bits(inst->ty, ptr_bytes);
                    if (inst->ty.kind != PIR_TYPE_SCALAR) {
                        pir_diag_error(v->diag, inst->src_line, 0u,
                                       "integer immediate invalid for non-scalar bitcast");
                    } else if (is_ptr_type(inst->ty) &&
                               (op0->u.imm_int.lo != 0u || op0->u.imm_int.hi != 0u || op0->u.imm_int.is_negative)) {
                        pir_diag_error(v->diag, inst->src_line, 0u, "only null literal '0' is valid for ptr bitcast");
                    } else {
                        check_int_imm_range(v, db_imm, op0->u.imm_int.lo, op0->u.imm_int.hi,
                                            op0->u.imm_int.is_negative, inst->src_line, "bitcast");
                    }
                } else if (op0->kind == PIR_OPER_IMM_FLOAT &&
                           !(src.kind == PIR_TYPE_SCALAR && pir_scalar_is_float(src.u.scalar))) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "float immediate invalid for type in bitcast");
                }
                check_lir_type(v, src, inst->src_line, "bitcast src");
                uint32_t sb = pir_type_bits(src, ptr_bytes);
                uint32_t db = pir_type_bits(inst->ty, ptr_bytes);
                if (sb == 0u || sb != db) {
                    pir_diag_error(v->diag, inst->src_line, 0u,
                                   "bitcast bit-width mismatch (%u vs %u)", sb, db);
                }
            }
            break;

        case PIR_OP_SPLAT:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (inst->ty.kind != PIR_TYPE_VECTOR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "splat requires vector type");
            } else {
                check_lir_type(v, inst->ty, inst->src_line, "splat");
                PirType elem = pir_type_scalar(pir_vec_elem_scalar(inst->ty.u.vec));
                if (inst->operand_count == 1u) {
                    check_operand_kind_and_type(v, fn, &inst->operands[0], elem, inst->src_line, "splat");
                }
            }
            break;

        case PIR_OP_EXTRACT:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (inst->ty.kind != PIR_TYPE_VECTOR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "extract requires vector type");
            } else {
                check_lir_type(v, inst->ty, inst->src_line, "extract");
                if (inst->operand_count == 1u) {
                    check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "extract");
                }
                if (inst->imm_u64 >= pir_vec_lanes(inst->ty.u.vec)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "extract lane index out of bounds");
                }
            }
            break;

        case PIR_OP_INSERT:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (inst->ty.kind != PIR_TYPE_VECTOR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "insert requires vector type");
            } else {
                check_lir_type(v, inst->ty, inst->src_line, "insert");
                PirType elem = pir_type_scalar(pir_vec_elem_scalar(inst->ty.u.vec));
                if (inst->operand_count == 2u) {
                    check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "insert");
                    check_operand_kind_and_type(v, fn, &inst->operands[1], elem, inst->src_line, "insert");
                }
                if (inst->imm_u64 >= pir_vec_lanes(inst->ty.u.vec)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "insert lane index out of bounds");
                }
            }
            break;

        case PIR_OP_SHUFFLE:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (inst->ty.kind != PIR_TYPE_VECTOR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "shuffle requires vector type");
            } else {
                check_lir_type(v, inst->ty, inst->src_line, "shuffle");
                uint32_t lanes = pir_vec_lanes(inst->ty.u.vec);
                if (inst->operand_count == 2u) {
                    check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "shuffle");
                    check_operand_kind_and_type(v, fn, &inst->operands[1], inst->ty, inst->src_line, "shuffle");
                }
                if (inst->shuffle_len != lanes) {
                    pir_diag_error(v->diag, inst->src_line, 0u,
                                   "shuffle mask length %u does not match %u lanes",
                                   inst->shuffle_len, lanes);
                }
                for (uint32_t k = 0u; k < inst->shuffle_len; ++k) {
                    if (inst->shuffle_mask[k] >= 2u * lanes) {
                        pir_diag_error(v->diag, inst->src_line, 0u,
                                       "shuffle mask index %u out of bounds (max %u)",
                                       (unsigned)inst->shuffle_mask[k], 2u * lanes - 1u);
                    }
                }
            }
            break;

        case PIR_OP_ALLOCA:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids typed 'alloca' (use alloca.ptr)");
            }
            check_result_count(v, inst, 1u);
            if (inst->aux_type.kind == PIR_TYPE_SCALAR && inst->aux_type.u.scalar == PIR_SCALAR_VOID) {
                pir_diag_error(v->diag, inst->src_line, 0u, "cannot alloca void");
            }
            if (is_pad_agg_type(v->mod, inst->aux_type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "cannot alloca standalone pad type");
            }
            if (inst->align != 0u && !align_valid_u32(inst->align)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "alignment %u is not a valid power of two", inst->align);
            }
            break;

        case PIR_OP_ALLOCA_PTR:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 0u);
            if (inst->imm_u64 == 0u) {
                pir_diag_error(v->diag, inst->src_line, 0u, "alloca.ptr byte size must be > 0");
            } else if (inst->imm_u64 > max_obj_bytes(v->target)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "alloca.ptr byte size %" PRIu64 " exceeds maximum",
                               inst->imm_u64);
            }
            if (!align_valid_u32(inst->align)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "alignment %u is not a valid power of two", inst->align);
            }
            break;

        case PIR_OP_ALLOCA_DYN:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (inst->operand_count == 1u) {
                if (!is_i32_or_i64(inst->operands[0].type)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "alloca_dyn size must be i32 or i64");
                } else {
                    check_operand_kind_and_type(v, fn, &inst->operands[0], inst->operands[0].type,
                                                inst->src_line, "alloca_dyn");
                }
            }
            if (!align_valid_u32(inst->align)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "alignment %u is not a valid power of two", inst->align);
            }
            break;

        case PIR_OP_STACKSAVE:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 0u);
            break;

        case PIR_OP_STACKRESTORE:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 1u);
            if (inst->operand_count == 1u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "stackrestore");
            }
            break;

        case PIR_OP_FIELD_PTR:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'field_ptr'");
                break;
            }
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (inst->operand_count == 1u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "field_ptr");
            }
            if (inst->aux_type.kind != PIR_TYPE_NAMED_AGG ||
                inst->aux_type.u.agg_id >= v->mod->agg_types.len) {
                pir_diag_error(v->diag, inst->src_line, 0u, "field_ptr requires struct/union @Type");
            } else {
                const PirAggDef *agg = &v->mod->agg_types.data[inst->aux_type.u.agg_id];
                if (agg->kind == PIR_AGG_ARRAY) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "field_ptr cannot index array type");
                } else if (inst->imm_u64 >= agg->field_count) {
                    pir_diag_error(v->diag, inst->src_line, 0u,
                                   "field_ptr index %" PRIu64 " out of bounds for '@%s' (%u fields)",
                                   inst->imm_u64, agg->name ? agg->name : "anon", agg->field_count);
                }
            }
            break;

        case PIR_OP_ELEM_PTR:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'elem_ptr'");
                break;
            }
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (inst->aux_type.kind == PIR_TYPE_SCALAR && inst->aux_type.u.scalar == PIR_SCALAR_VOID) {
                pir_diag_error(v->diag, inst->src_line, 0u, "elem_ptr element type cannot be void");
            }
            if (is_pad_agg_type(v->mod, inst->aux_type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "elem_ptr element type cannot be standalone pad");
            }
            if (inst->operand_count == 2u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "elem_ptr");
                if (!is_i32_or_i64(inst->operands[1].type)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "elem_ptr requires (ptr, i32|i64)");
                } else {
                    check_operand_kind_and_type(v, fn, &inst->operands[1], inst->operands[1].type,
                                                inst->src_line, "elem_ptr");
                }
            }
            break;

        case PIR_OP_PTR_ADD:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (!is_i32_or_i64(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "ptr_add index type must be i32 or i64");
            }
            if (inst->operand_count == 2u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "ptr_add");
                check_operand_kind_and_type(v, fn, &inst->operands[1], inst->ty, inst->src_line, "ptr_add");
            }
            break;

        case PIR_OP_LOAD:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            check_ssa_type(v, inst->ty, inst->src_line, "load");
            if (inst->operand_count == 1u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "load");
            }
            if (!align_valid_u32(inst->align)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "alignment %u is not a valid power of two", inst->align);
            }
            break;

        case PIR_OP_STORE:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 2u);
            check_ssa_type(v, inst->ty, inst->src_line, "store");
            if (inst->operand_count == 2u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "store");
                check_operand_kind_and_type(v, fn, &inst->operands[1], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "store");
            }
            if (!align_valid_u32(inst->align)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "alignment %u is not a valid power of two", inst->align);
            }
            break;

        case PIR_OP_MEMCPY: case PIR_OP_MEMMOVE: case PIR_OP_MEMSET:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only memcpy/memmove/memset");
                break;
            }
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 3u);
            if (inst->operand_count == 3u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "mem*");
                check_operand_kind_and_type(v, fn, &inst->operands[1],
                                            inst->op == PIR_OP_MEMSET ? pir_type_scalar(PIR_SCALAR_I8)
                                                                      : pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "mem*");
                if (!is_i32_or_i64(inst->operands[2].type)) {
                    pir_diag_error(v->diag, inst->src_line, 0u, "mem* operand type mismatch");
                } else {
                    check_operand_kind_and_type(v, fn, &inst->operands[2], inst->operands[2].type,
                                                inst->src_line, "mem*");
                }
            }
            if (!align_valid_u32(inst->align)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "alignment %u is not a valid power of two", inst->align);
            }
            break;

        case PIR_OP_ATOMIC_LOAD:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            if (!is_atomic_type(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invalid atomic_load type");
            }
            if (inst->operand_count == 1u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "atomic_load");
            }
            if (inst->order != PIR_ORDER_RELAXED && inst->order != PIR_ORDER_ACQUIRE &&
                inst->order != PIR_ORDER_SEQ_CST) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invalid memory ordering for atomic_load");
            }
            break;

        case PIR_OP_ATOMIC_STORE:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 2u);
            if (!is_atomic_type(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invalid atomic_store type");
            }
            if (inst->operand_count == 2u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "atomic_store");
                check_operand_kind_and_type(v, fn, &inst->operands[1], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "atomic_store");
            }
            if (inst->order != PIR_ORDER_RELAXED && inst->order != PIR_ORDER_RELEASE &&
                inst->order != PIR_ORDER_SEQ_CST) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invalid memory ordering for atomic_store");
            }
            break;

        case PIR_OP_ATOMIC_RMW:
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 2u);
            if (!is_atomic_type(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invalid atomic_rmw type");
            }
            if (is_ptr_type(inst->ty) && inst->rmw_op != PIR_RMW_XCHG) {
                pir_diag_error(v->diag, inst->src_line, 0u, "only atomic_rmw.xchg is valid on ptr");
            }
            if (inst->operand_count == 2u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "atomic_rmw");
                check_operand_kind_and_type(v, fn, &inst->operands[1], inst->ty, inst->src_line, "atomic_rmw");
            }
            if (inst->order == PIR_ORDER_NONE) {
                pir_diag_error(v->diag, inst->src_line, 0u, "missing memory ordering for atomic_rmw");
            }
            break;

        case PIR_OP_ATOMIC_CMPXCHG:
            check_result_count(v, inst, 2u);
            check_op_count(v, inst, 3u);
            if (!is_atomic_type(inst->ty)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invalid atomic_cmpxchg type");
            }
            if (inst->operand_count == 3u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "atomic_cmpxchg");
                check_operand_kind_and_type(v, fn, &inst->operands[1], inst->ty, inst->src_line, "atomic_cmpxchg");
                check_operand_kind_and_type(v, fn, &inst->operands[2], inst->ty, inst->src_line, "atomic_cmpxchg");
            }
            if (!fail_order_valid(inst->order, inst->fail_order)) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "invalid atomic_cmpxchg failure ordering (cannot be release/acq_rel or stronger than success)");
            }
            break;

        case PIR_OP_FENCE:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 0u);
            if (inst->order != PIR_ORDER_ACQUIRE && inst->order != PIR_ORDER_RELEASE &&
                inst->order != PIR_ORDER_ACQ_REL && inst->order != PIR_ORDER_SEQ_CST) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invalid memory ordering for fence");
            }
            break;

        case PIR_OP_VA_START:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 1u);
            if (!fn->is_variadic || fn->cc != PIR_CC_C) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "va_start is only valid inside a variadic cc \"c\" function");
            }
            if (inst->operand_count == 1u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "va_start");
            }
            break;

        case PIR_OP_VA_ARG:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'va_arg'");
                break;
            }
            check_result_count(v, inst, 1u);
            check_op_count(v, inst, 1u);
            check_ssa_type(v, inst->ty, inst->src_line, "va_arg");
            if (inst->operand_count == 1u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "va_arg");
            }
            break;

        case PIR_OP_VA_ARG_BUF:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'va_arg_buf'");
                break;
            }
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 2u);
            if (inst->aux_type.kind != PIR_TYPE_NAMED_AGG || is_pad_agg_type(v->mod, inst->aux_type)) {
                pir_diag_error(v->diag, inst->src_line, 0u, "va_arg_buf requires aggregate type");
            }
            if (inst->operand_count == 2u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "va_arg_buf");
                check_operand_kind_and_type(v, fn, &inst->operands[1], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "va_arg_buf");
            }
            break;

        case PIR_OP_VA_COPY:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'va_copy'");
                break;
            }
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 2u);
            if (inst->operand_count == 2u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "va_copy");
                check_operand_kind_and_type(v, fn, &inst->operands[1], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "va_copy");
            }
            break;

        case PIR_OP_VA_END:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'va_end'");
                break;
            }
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 1u);
            if (inst->operand_count == 1u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "va_end");
            }
            break;

        case PIR_OP_CALL: {
            if (inst->results[1] != PIR_INVALID_ID) {
                pir_diag_error(v->diag, inst->src_line, 0u, "call produces at most 1 result");
            }
            if (inst->operand_count < 1u || inst->operands[0].kind != PIR_OPER_SYM ||
                !inst->operands[0].u.sym.is_func ||
                inst->operands[0].u.sym.id >= v->mod->funcs.len) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "call target '@%s' is not a declared function",
                               (inst->operand_count >= 1u && inst->operands[0].kind == PIR_OPER_SYM &&
                                inst->operands[0].u.sym.name) ? inst->operands[0].u.sym.name : "?");
                break;
            }
            if (inst->operands[0].u.sym.offset != 0) {
                pir_diag_error(v->diag, inst->src_line, 0u, "call target cannot have non-zero offset");
            }
            const PirFunction *callee = &v->mod->funcs.data[inst->operands[0].u.sym.id];
            if (inst->results[0] != PIR_INVALID_ID &&
                callee->ret_type.kind == PIR_TYPE_SCALAR &&
                callee->ret_type.u.scalar == PIR_SCALAR_VOID) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "cannot assign result of void call '@%s'", callee->name);
            }
            check_call_args(v, fn, inst, callee->name, callee->cc, callee->params, callee->param_count,
                            callee->is_variadic, 1u);
            break;
        }

        case PIR_OP_CALL_INDIRECT: {
            if (inst->results[1] != PIR_INVALID_ID) {
                pir_diag_error(v->diag, inst->src_line, 0u, "call_indirect produces at most 1 result");
            }
            if (inst->sig_id >= v->mod->sigs.len) {
                pir_diag_error(v->diag, inst->src_line, 0u, "call_indirect references unknown signature");
                break;
            }
            if (inst->operand_count < 1u) {
                pir_diag_error(v->diag, inst->src_line, 0u, "call_indirect function pointer must be ptr");
            } else {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "call_indirect callee");
            }
            const PirSigDef *sig = &v->mod->sigs.data[inst->sig_id];
            if (inst->results[0] != PIR_INVALID_ID &&
                sig->ret_type.kind == PIR_TYPE_SCALAR &&
                sig->ret_type.u.scalar == PIR_SCALAR_VOID) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "cannot assign result of void call_indirect '@%s'", sig->name);
            }
            check_call_args(v, fn, inst, sig->name, sig->cc, sig->params, sig->param_count, sig->is_variadic, 1u);
            break;
        }

        case PIR_OP_JMP:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 0u);
            check_branch_target(v, fn, inst, &inst->targets[0], false, false,
                                pir_type_scalar(PIR_SCALAR_VOID));
            break;

        case PIR_OP_BR:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 1u);
            if (inst->operand_count == 1u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_I8),
                                            inst->src_line, "br condition");
            }
            check_branch_target(v, fn, inst, &inst->targets[0], false, false,
                                pir_type_scalar(PIR_SCALAR_VOID));
            check_branch_target(v, fn, inst, &inst->targets[1], false, false,
                                pir_type_scalar(PIR_SCALAR_VOID));
            break;

        case PIR_OP_SWITCH:
            if (v->mode == PIR_VERIFY_LIR && v->target && !v->target->caps.has_switch) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR target does not support switch");
            }
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 1u);
            if (!(inst->ty.kind == PIR_TYPE_SCALAR && pir_scalar_is_int(inst->ty.u.scalar))) {
                pir_diag_error(v->diag, inst->src_line, 0u, "switch requires scalar integer type");
            }
            check_lir_type(v, inst->ty, inst->src_line, "switch");
            if (inst->operand_count == 1u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], inst->ty, inst->src_line, "switch");
            }
            check_branch_target(v, fn, inst, &inst->targets[0], false, false,
                                pir_type_scalar(PIR_SCALAR_VOID));
            for (uint32_t c = 0u; c < inst->switch_case_count; ++c) {
                check_branch_target(v, fn, inst, &inst->switch_cases[c].target, false, false,
                                    pir_type_scalar(PIR_SCALAR_VOID));
                uint32_t sw_bits = (inst->ty.kind == PIR_TYPE_SCALAR &&
                                    pir_scalar_is_int(inst->ty.u.scalar))
                                   ? pir_scalar_bits(inst->ty.u.scalar, ptr_bytes) : 0u;
                if (sw_bits == 0u) continue;
                uint64_t mask = (sw_bits >= 64u) ? UINT64_MAX : ((UINT64_C(1) << sw_bits) - 1u);
                uint64_t clo = inst->switch_cases[c].case_lo;
                uint64_t chi = inst->switch_cases[c].case_hi;
                if (sw_bits < 64u) {
                    bool oob = (chi != 0u) ||
                               (inst->switch_cases[c].is_negative
                                ? (clo > (UINT64_C(1) << (sw_bits - 1u)))
                                : (clo > mask));
                    if (oob) {
                        pir_diag_error(v->diag, inst->src_line, 0u,
                                       "switch case constant out of range for %u-bit switch", sw_bits);
                    }
                } else if (sw_bits == 64u) {
                    if (chi != 0u) {
                        pir_diag_error(v->diag, inst->src_line, 0u,
                                       "128-bit switch case constant out of range for 64-bit switch");
                    } else if (inst->switch_cases[c].is_negative && clo > (UINT64_C(1) << 63)) {
                        pir_diag_error(v->diag, inst->src_line, 0u,
                                       "negative switch case constant out of range for 64-bit switch");
                    }
                } else if (sw_bits == 128u && inst->switch_cases[c].is_negative) {
                    if (chi > (UINT64_C(1) << 63) || (chi == (UINT64_C(1) << 63) && clo != 0u)) {
                        pir_diag_error(v->diag, inst->src_line, 0u,
                                       "negative switch case constant out of range for 128-bit switch");
                    }
                }
                if (inst->switch_cases[c].is_negative) {
                    uint64_t nlo = ~clo + 1u;
                    uint64_t cy = (nlo == 0u) ? 1u : 0u;
                    clo = nlo; chi = ~chi + cy;
                }
                clo &= mask;
                if (sw_bits <= 64u) chi = 0u;
                for (uint32_t d = 0u; d < c; ++d) {
                    uint64_t dlo = inst->switch_cases[d].case_lo;
                    uint64_t dhi = inst->switch_cases[d].case_hi;
                    if (inst->switch_cases[d].is_negative) {
                        uint64_t nlo = ~dlo + 1u;
                        uint64_t cy = (nlo == 0u) ? 1u : 0u;
                        dlo = nlo; dhi = ~dhi + cy;
                    }
                    dlo &= mask;
                    if (sw_bits <= 64u) dhi = 0u;
                    if (clo == dlo && chi == dhi) {
                        pir_diag_error(v->diag, inst->src_line, 0u, "duplicate switch case constant");
                        break;
                    }
                }
            }
            break;

        case PIR_OP_INVOKE: {
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'invoke'");
                break;
            }
            check_result_count(v, inst, 0u);
            if (inst->operand_count < 1u || inst->operands[0].kind != PIR_OPER_SYM ||
                !inst->operands[0].u.sym.is_func ||
                inst->operands[0].u.sym.id >= v->mod->funcs.len) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invoke target is not a declared function");
                break;
            }
            if (inst->operands[0].u.sym.offset != 0) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invoke target cannot have non-zero offset");
            }
            const PirFunction *callee = &v->mod->funcs.data[inst->operands[0].u.sym.id];
            check_call_args(v, fn, inst, callee->name, callee->cc, callee->params, callee->param_count,
                            callee->is_variadic, 1u);
            check_branch_target(v, fn, inst, &inst->targets[0], true, false, callee->ret_type);
            check_branch_target(v, fn, inst, &inst->targets[1], false, true, callee->ret_type);
            break;
        }

        case PIR_OP_INVOKE_INDIRECT: {
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'invoke_indirect'");
                break;
            }
            check_result_count(v, inst, 0u);
            if (inst->sig_id >= v->mod->sigs.len) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invoke_indirect references unknown signature");
                break;
            }
            if (inst->operand_count < 1u) {
                pir_diag_error(v->diag, inst->src_line, 0u, "invoke_indirect callee must be ptr");
            } else {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "invoke_indirect callee");
            }
            const PirSigDef *sig = &v->mod->sigs.data[inst->sig_id];
            check_call_args(v, fn, inst, sig->name, sig->cc, sig->params, sig->param_count, sig->is_variadic, 1u);
            check_branch_target(v, fn, inst, &inst->targets[0], true, false, sig->ret_type);
            check_branch_target(v, fn, inst, &inst->targets[1], false, true, sig->ret_type);
            break;
        }

        case PIR_OP_RESUME:
            if (v->mode == PIR_VERIFY_LIR) {
                pir_diag_error(v->diag, inst->src_line, 0u, "LIR forbids HIR-only 'resume'");
                break;
            }
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 1u);
            if (inst->operand_count == 1u) {
                check_operand_kind_and_type(v, fn, &inst->operands[0], pir_type_scalar(PIR_SCALAR_PTR),
                                            inst->src_line, "resume");
            }
            break;

        case PIR_OP_RET:
            check_result_count(v, inst, 0u);
            if (fn->ret_type.kind == PIR_TYPE_SCALAR && fn->ret_type.u.scalar == PIR_SCALAR_VOID) {
                check_op_count(v, inst, 0u);
            } else {
                check_op_count(v, inst, 1u);
                if (inst->operand_count == 1u) {
                    if (!pir_type_eq(inst->operands[0].type, fn->ret_type)) {
                        char b1[64], b2[64];
                        pir_diag_error(v->diag, inst->src_line, 0u,
                                       "ret value type mismatch (expected %s, got %s)",
                                       type_str(v->mod, fn->ret_type, b1, sizeof(b1)),
                                       type_str(v->mod, inst->operands[0].type, b2, sizeof(b2)));
                    } else {
                        check_operand_kind_and_type(v, fn, &inst->operands[0], fn->ret_type,
                                                    inst->src_line, "ret");
                    }
                }
            }
            break;

        case PIR_OP_TRAP:
        case PIR_OP_UNREACHABLE:
            check_result_count(v, inst, 0u);
            check_op_count(v, inst, 0u);
            break;
    }
}

static bool has_file_id(const PirModule *mod, PirFileId fid) {
    for (uint32_t i = 0u; i < mod->files.len; ++i) {
        if (mod->files.data[i].id == fid) return true;
    }
    return false;
}

static void check_unreachable_operand(Verifier *v, const PirFunction *fn, PirBlockId b,
                                      uint32_t j, const uint32_t *inst_pos,
                                      const PirBlockId *inst_blk, const PirOperand *op,
                                      uint32_t line) {
    if (op->kind != PIR_OPER_VAL || op->u.val >= fn->values.len) return;
    const PirValueDef *vd = &fn->values.data[op->u.val];
    if (vd->def_block == PIR_INVALID_ID) return;
    if (vd->def_block >= fn->blocks.len) {
        pir_diag_error(v->diag, line, 0u, "use of '%%%s' references invalid defining block", vd->name);
        return;
    }
    if (vd->def_inst != PIR_INVALID_ID) {
        if (vd->def_inst >= fn->insts.len || inst_pos[vd->def_inst] == PIR_INVALID_ID ||
            inst_blk[vd->def_inst] != vd->def_block) {
            pir_diag_error(v->diag, line, 0u,
                           "use of '%%%s' references deleted or detached instruction", vd->name);
            return;
        }
        if (vd->def_block == b && inst_pos[vd->def_inst] >= j) {
            pir_diag_error(v->diag, line, 0u,
                           "use of '%%%s' in '.%s' precedes its definition",
                           vd->name, fn->blocks.data[b].label);
        }
    }
}

static void verify_function(Verifier *v, const PirFunction *fn) {
    verify_params_and_ret(v, fn->name, fn->src_line, fn->cc, fn->attrs, fn->params,
                          fn->param_count, fn->is_variadic, fn->ret_type, &fn->cabi_ret);

    if (fn->linkage == PIR_LINK_EXTERN) {
        if (fn->is_defined) {
            pir_diag_error(v->diag, fn->src_line, 0u, "extern function '@%s' cannot have a body", fn->name);
        }
        return;
    }
    if (!fn->is_defined || fn->blocks.len == 0u) {
        pir_diag_error(v->diag, fn->src_line, 0u, "function '@%s' must define at least one block", fn->name);
        return;
    }

    /* Validate all loc directives reference declared file IDs. */
    for (uint32_t i = 1u; i < fn->locs.len; ++i) {
        if (!has_file_id(v->mod, fn->locs.data[i].file_id)) {
            pir_diag_error(v->diag, fn->src_line, 0u,
                           "loc references undeclared file id %u", fn->locs.data[i].file_id);
        }
    }

    /* Entry block must have 0 block parameters (PIR.md §7.1 #3). */
    if (fn->blocks.data[0].params.len != 0u) {
        pir_diag_error(v->diag, fn->blocks.data[0].src_line, 0u,
                       "entry block '.%s' must not declare block parameters",
                       fn->blocks.data[0].label);
    }

    /* Check block label uniqueness, params, terminators, and record instruction positions. */
    uint32_t   *inst_pos = (uint32_t *)malloc((fn->insts.len + 1u) * sizeof(uint32_t));
    PirBlockId *inst_blk = (PirBlockId *)malloc((fn->insts.len + 1u) * sizeof(PirBlockId));
    if (!inst_pos || !inst_blk) abort();
    for (uint32_t i = 0u; i <= fn->insts.len; ++i) {
        inst_pos[i] = PIR_INVALID_ID;
        inst_blk[i] = PIR_INVALID_ID;
    }
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t pb = 0u; pb < b; ++pb) {
            if (blk->label && fn->blocks.data[pb].label &&
                strcmp(blk->label, fn->blocks.data[pb].label) == 0) {
                pir_diag_error(v->diag, blk->src_line, 0u,
                               "duplicate block label '.%s' in function '@%s'",
                               blk->label, fn->name);
                break;
            }
        }
        for (uint32_t k = 0u; k < blk->params.len; ++k) {
            if (blk->params.data[k] >= fn->values.len) {
                pir_diag_error(v->diag, blk->src_line, 0u, "invalid block param value id");
            } else {
                check_ssa_type(v, fn->values.data[blk->params.data[k]].type, blk->src_line, "block param");
            }
        }
        if (blk->insts.len == 0u) {
            pir_diag_error(v->diag, blk->src_line, 0u,
                           "block '.%s' does not end with a terminator", blk->label);
            continue;
        }
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            PirInstId iid = blk->insts.data[j];
            if (iid >= fn->insts.len) {
                pir_diag_error(v->diag, blk->src_line, 0u, "invalid instruction id %u", iid);
                continue;
            }
            inst_pos[iid] = j;
            inst_blk[iid] = b;
            const PirInst *inst = &fn->insts.data[iid];
            bool is_term = pir_op_is_terminator(inst->op);
            if (j + 1u < blk->insts.len && is_term) {
                pir_diag_error(v->diag, inst->src_line, 0u,
                               "terminator appears in middle of block '.%s'", blk->label);
            } else if (j + 1u == blk->insts.len && !is_term) {
                pir_diag_error(v->diag, blk->src_line, 0u,
                               "block '.%s' does not end with a terminator", blk->label);
            }
            verify_inst(v, fn, inst);
        }
    }

    /* Build CFG & Dominators and verify strict SSA dominance. */
    PirCfg cfg;
    PirDom dom;
    pir_cfg_build(&cfg, fn);
    pir_dom_build(&dom, &cfg);

    if (cfg.preds[0].len != 0u) {
        pir_diag_error(v->diag, fn->blocks.data[0].src_line, 0u,
                       "entry block '.%s' cannot have predecessors", fn->blocks.data[0].label);
    }

    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        if (!cfg.reachable[b]) {
            /* Even in unreachable blocks, verify same-block def-before-use and valid defs. */
            for (uint32_t j = 0u; j < blk->insts.len; ++j) {
                if (blk->insts.data[j] >= fn->insts.len) continue;
                const PirInst *inst = &fn->insts.data[blk->insts.data[j]];
                for (uint32_t k = 0u; k < inst->operand_count; ++k) {
                    check_unreachable_operand(v, fn, b, j, inst_pos, inst_blk,
                                              &inst->operands[k], inst->src_line);
                }
                if (inst->op == PIR_OP_JMP || inst->op == PIR_OP_BR ||
                    inst->op == PIR_OP_INVOKE || inst->op == PIR_OP_INVOKE_INDIRECT ||
                    inst->op == PIR_OP_SWITCH) {
                    uint32_t nt = (inst->op == PIR_OP_BR || inst->op == PIR_OP_INVOKE ||
                                   inst->op == PIR_OP_INVOKE_INDIRECT) ? 2u : 1u;
                    for (uint32_t t = 0u; t < nt; ++t) {
                        for (uint32_t k = 0u; k < inst->targets[t].arg_count; ++k) {
                            check_unreachable_operand(v, fn, b, j, inst_pos, inst_blk,
                                                      &inst->targets[t].args[k], inst->src_line);
                        }
                    }
                    if (inst->op == PIR_OP_SWITCH) {
                        for (uint32_t c = 0u; c < inst->switch_case_count; ++c) {
                            const PirBranchTarget *bt = &inst->switch_cases[c].target;
                            for (uint32_t k = 0u; k < bt->arg_count; ++k) {
                                check_unreachable_operand(v, fn, b, j, inst_pos, inst_blk,
                                                          &bt->args[k], inst->src_line);
                            }
                        }
                    }
                }
            }
            continue;
        }
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            if (blk->insts.data[j] >= fn->insts.len) continue;
            const PirInst *inst = &fn->insts.data[blk->insts.data[j]];
            for (uint32_t k = 0u; k < inst->operand_count; ++k) {
                check_dom_operand(v, fn, &dom, b, j, inst_pos, inst_blk, &inst->operands[k], inst->src_line);
            }
            if (inst->op == PIR_OP_JMP || inst->op == PIR_OP_BR ||
                inst->op == PIR_OP_INVOKE || inst->op == PIR_OP_INVOKE_INDIRECT ||
                inst->op == PIR_OP_SWITCH) {
                uint32_t nt = (inst->op == PIR_OP_BR || inst->op == PIR_OP_INVOKE ||
                               inst->op == PIR_OP_INVOKE_INDIRECT) ? 2u : 1u;
                for (uint32_t t = 0u; t < nt; ++t) {
                    for (uint32_t k = 0u; k < inst->targets[t].arg_count; ++k) {
                        check_dom_operand(v, fn, &dom, b, j, inst_pos, inst_blk,
                                          &inst->targets[t].args[k], inst->src_line);
                    }
                }
                if (inst->op == PIR_OP_SWITCH) {
                    for (uint32_t c = 0u; c < inst->switch_case_count; ++c) {
                        const PirBranchTarget *bt = &inst->switch_cases[c].target;
                        for (uint32_t k = 0u; k < bt->arg_count; ++k) {
                            check_dom_operand(v, fn, &dom, b, j, inst_pos, inst_blk,
                                              &bt->args[k], inst->src_line);
                        }
                    }
                }
            }
        }
    }

    /* Reject cross-block SSA instruction definition cycles in unreachable blocks. */
    if (fn->values.len > 0u) {
        bool has_unreach = false;
        for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
            if (!cfg.reachable[b]) { has_unreach = true; break; }
        }
        if (has_unreach) {
            uint8_t *vcol = (uint8_t *)calloc(fn->values.len, 1u);
            typedef struct { PirValueId vid; uint32_t op_i; } UFrame;
            PIR_VEC(UFrame) ustk = {0};
            if (!vcol) abort();
            for (uint32_t sv = 0u; sv < fn->values.len; ++sv) {
                if (vcol[sv] != 0u) continue;
                const PirValueDef *svd = &fn->values.data[sv];
                if (svd->def_block == PIR_INVALID_ID || svd->def_block >= fn->blocks.len ||
                    cfg.reachable[svd->def_block] || svd->def_inst == PIR_INVALID_ID ||
                    svd->def_inst >= fn->insts.len) {
                    vcol[sv] = 2u;
                    continue;
                }
                vcol[sv] = 1u;
                UFrame uf = { sv, 0u };
                PIR_VEC_PUSH(&ustk, uf);
                while (ustk.len > 0u) {
                    UFrame *top = &ustk.data[ustk.len - 1u];
                    const PirValueDef *tvd = &fn->values.data[top->vid];
                    const PirInst *tins = &fn->insts.data[tvd->def_inst];
                    PirValueId nxt = PIR_INVALID_ID;
                    while (top->op_i < tins->operand_count) {
                        const PirOperand *op = &tins->operands[top->op_i++];
                        if (op->kind == PIR_OPER_VAL && op->u.val < fn->values.len) {
                            const PirValueDef *ovd = &fn->values.data[op->u.val];
                            if (ovd->def_block != PIR_INVALID_ID && ovd->def_block < fn->blocks.len &&
                                !cfg.reachable[ovd->def_block] && ovd->def_inst != PIR_INVALID_ID &&
                                ovd->def_inst < fn->insts.len) {
                                nxt = op->u.val;
                                break;
                            }
                        }
                    }
                    if (nxt != PIR_INVALID_ID) {
                        if (vcol[nxt] == 1u) {
                            pir_diag_error(v->diag, tins->src_line, 0u,
                                           "cyclic SSA definition involving '%%%s' in unreachable block",
                                           fn->values.data[nxt].name);
                            while (ustk.len > 0u) {
                                vcol[ustk.data[--ustk.len].vid] = 2u;
                            }
                            break;
                        }
                        if (vcol[nxt] == 0u) {
                            vcol[nxt] = 1u;
                            UFrame nf = { nxt, 0u };
                            PIR_VEC_PUSH(&ustk, nf);
                        }
                    } else {
                        vcol[top->vid] = 2u;
                        ustk.len--;
                    }
                }
            }
            PIR_VEC_FREE(&ustk);
            free(vcol);
        }
    }

    pir_dom_destroy(&dom);
    pir_cfg_destroy(&cfg);
    free(inst_blk);
    free(inst_pos);
}

static void verify_scalar_global_item(Verifier *v, const PirGlobal *g, PirScalarType sk,
                                      const PirGlobalItem *gi) {
    if (sk == PIR_SCALAR_VALIST) {
        pir_diag_error(v->diag, g->src_line, 0u,
                       "va_list field is not allowed in global '@%s'", g->name);
        return;
    }
    PirGlobalItemKind ik = gi->kind;
    uint8_t ptr_bytes = v->target ? v->target->ptr_bytes : 8u;
    uint64_t scalar_sz = (v->target)
                         ? pir_type_layout(v->mod, v->target, pir_type_scalar(sk)).size
                         : (uint64_t)(pir_type_bits(pir_type_scalar(sk), ptr_bytes) / 8u);
    if (ik == PIR_GITEM_ZERO) {
        if (gi->len != 0u && gi->len != scalar_sz) {
            pir_diag_error(v->diag, g->src_line, 0u,
                           "zeroinit size %" PRIu64 " mismatches scalar size %" PRIu64 " in global '@%s'",
                           gi->len, scalar_sz, g->name);
        }
    } else if (ik == PIR_GITEM_BYTES) {
        pir_diag_error(v->diag, g->src_line, 0u,
                       "string initializer invalid for scalar global '@%s'", g->name);
    } else if (ik == PIR_GITEM_RELOC) {
        if (sk != PIR_SCALAR_PTR) {
            pir_diag_error(v->diag, g->src_line, 0u,
                           "relocation initializer invalid for non-ptr global '@%s'", g->name);
        }
    } else if (ik == PIR_GITEM_FLOAT) {
        if (!pir_scalar_is_float(sk)) {
            pir_diag_error(v->diag, g->src_line, 0u,
                           "float initializer invalid for non-float global '@%s'", g->name);
        }
    } else if (ik == PIR_GITEM_INT) {
        if (sk == PIR_SCALAR_PTR) {
            if (gi->int_lo != 0u || gi->int_hi != 0u || gi->int_neg) {
                pir_diag_error(v->diag, g->src_line, 0u,
                               "only null literal '0' is valid for ptr global '@%s'", g->name);
            }
        } else if (pir_scalar_is_float(sk)) {
            if (gi->int_hi != 0u) {
                pir_diag_error(v->diag, g->src_line, 0u,
                               "128-bit integer initializer invalid for float global '@%s'", g->name);
            }
        } else if (pir_scalar_is_int(sk)) {
            uint32_t bits = pir_type_bits(pir_type_scalar(sk), ptr_bytes);
            check_int_imm_range(v, bits, gi->int_lo, gi->int_hi, gi->int_neg, g->src_line, "global initializer");
        }
    }
}

static bool agg_type_is_pad_only(const PirModule *mod, PirType ty, int8_t *cache) {
    return pir_agg_is_pad_only_cached(mod, ty, cache);
}

static bool agg_contains_valist_rec(const PirModule *mod, PirType ty, int8_t *cache, uint32_t depth) {
    if (depth > 256u) return false;
    if (ty.kind == PIR_TYPE_SCALAR) return ty.u.scalar == PIR_SCALAR_VALIST;
    if (ty.kind != PIR_TYPE_NAMED_AGG || ty.u.agg_id >= mod->agg_types.len) return false;
    uint32_t aid = ty.u.agg_id;
    if (cache) {
        if (cache[aid] == 1) return true;
        if (cache[aid] == 0 || cache[aid] == 2) return false;
        cache[aid] = 2;
    }
    const PirAggDef *ad = &mod->agg_types.data[aid];
    bool res = false;
    switch (ad->kind) {
        case PIR_AGG_PAD:
            res = false;
            break;
        case PIR_AGG_ARRAY:
            res = agg_contains_valist_rec(mod, ad->elem_type, cache, depth + 1u);
            break;
        case PIR_AGG_STRUCT:
        case PIR_AGG_PACKED_STRUCT:
        case PIR_AGG_UNION:
            for (uint32_t f = 0u; f < ad->field_count; ++f) {
                if (agg_contains_valist_rec(mod, ad->fields[f], cache, depth + 1u)) {
                    res = true;
                    break;
                }
            }
            break;
    }
    if (cache) cache[aid] = res ? 1 : 0;
    return res;
}

static void verify_agg_global_items(Verifier *v, const PirGlobal *g, PirType ty,
                                    uint32_t *idx, int8_t *pad_cache, uint32_t depth) {
    if (depth > 256u) {
        pir_diag_error(v->diag, g->src_line, 0u, "aggregate global '@%s' nesting exceeds maximum depth", g->name);
        return;
    }
    if (agg_type_is_pad_only(v->mod, ty, pad_cache)) {
        return;
    }
    if (*idx >= g->item_count) return;
    if (g->items[*idx].kind == PIR_GITEM_ZERO) {
        const PirGlobalItem *zi = &g->items[(*idx)++];
        if (zi->len != 0u && v->target) {
            PirTypeLayout ly = pir_type_layout(v->mod, v->target, ty);
            if (!ly.overflow && zi->len != ly.size) {
                pir_diag_error(v->diag, g->src_line, 0u,
                               "zeroinit size %" PRIu64 " mismatches element size %" PRIu64 " in global '@%s'",
                               zi->len, ly.size, g->name);
            }
        }
        return;
    }
    if (ty.kind == PIR_TYPE_SCALAR) {
        verify_scalar_global_item(v, g, ty.u.scalar, &g->items[(*idx)++]);
        return;
    }
    if (ty.kind == PIR_TYPE_VECTOR) {
        PirType ety = pir_type_scalar(pir_vec_elem_scalar(ty.u.vec));
        uint32_t lanes = pir_vec_lanes(ty.u.vec);
        for (uint32_t l = 0u; l < lanes && *idx < g->item_count; ++l) {
            verify_agg_global_items(v, g, ety, idx, pad_cache, depth + 1u);
        }
        return;
    }
    if (ty.u.agg_id >= v->mod->agg_types.len) return;
    const PirAggDef *ad = &v->mod->agg_types.data[ty.u.agg_id];
    switch (ad->kind) {
        case PIR_AGG_PAD:
            break;
        case PIR_AGG_ARRAY:
            if (agg_type_is_pad_only(v->mod, ad->elem_type, pad_cache)) {
                break;
            }
            if (g->items[*idx].kind == PIR_GITEM_BYTES &&
                ad->elem_type.kind == PIR_TYPE_SCALAR &&
                ad->elem_type.u.scalar == PIR_SCALAR_I8) {
                const PirGlobalItem *bi = &g->items[(*idx)++];
                if (bi->len > ad->array_len) {
                    pir_diag_error(v->diag, g->src_line, 0u,
                                   "string initializer for '@%s' exceeds array length (%" PRIu64 " > %" PRIu64 ")",
                                   g->name, bi->len, ad->array_len);
                }
            } else {
                for (uint64_t k = 0u; k < ad->array_len && *idx < g->item_count; ++k) {
                    verify_agg_global_items(v, g, ad->elem_type, idx, pad_cache, depth + 1u);
                }
            }
            break;
        case PIR_AGG_STRUCT:
        case PIR_AGG_PACKED_STRUCT:
            for (uint32_t f = 0u; f < ad->field_count && *idx < g->item_count; ++f) {
                verify_agg_global_items(v, g, ad->fields[f], idx, pad_cache, depth + 1u);
            }
            break;
        case PIR_AGG_UNION:
            if (ad->field_count > 0u) {
                if (g->items[*idx].kind == PIR_GITEM_RELOC) {
                    bool has_ptr_field = false;
                    for (uint32_t f = 0u; f < ad->field_count; ++f) {
                        if (is_ptr_type(ad->fields[f])) {
                            has_ptr_field = true;
                            break;
                        }
                    }
                    if (has_ptr_field) {
                        (*idx)++;
                        break;
                    }
                }
                verify_agg_global_items(v, g, ad->fields[0], idx, pad_cache, depth + 1u);
            }
            break;
    }
}

static void check_unique_symbol(Verifier *v, const char *name, uint32_t line,
                                const char **seen, uint32_t *seen_len) {
    for (uint32_t i = 0u; i < *seen_len; ++i) {
        if (strcmp(seen[i], name) == 0) {
            pir_diag_error(v->diag, line, 0u, "duplicate module symbol '@%s'", name);
            return;
        }
    }
    seen[(*seen_len)++] = name;
}

bool pir_verify_module(const PirModule *mod, PirVerifyMode mode,
                       const PirTargetSpec *target, PirDiag *diag) {
    PirTargetSpec parsed_target;
    if (!target) {
        if (mod->target_triple && pir_target_from_triple(mod->target_triple, &parsed_target)) {
            target = &parsed_target;
        }
    }

    Verifier v = { mod, mode, target, diag };

    if (!mod->target_triple || !target) {
        pir_diag_error(diag, 1u, 0u, "invalid or unsupported target triple '%s'",
                       mod->target_triple ? mod->target_triple : "(null)");
    }

    /* Unique file IDs. */
    for (uint32_t i = 0u; i < mod->files.len; ++i) {
        for (uint32_t j = 0u; j < i; ++j) {
            if (mod->files.data[i].id == mod->files.data[j].id) {
                pir_diag_error(diag, 0u, 0u, "duplicate file id %u", mod->files.data[i].id);
                break;
            }
        }
    }

    /* Unique module-level @symbols across types, sigs, globals, funcs. */
    uint32_t max_syms = mod->agg_types.len + mod->sigs.len + mod->globals.len + mod->funcs.len + 1u;
    const char **seen = (const char **)malloc(max_syms * sizeof(const char *));
    if (!seen) abort();
    uint32_t seen_len = 0u;

    /* Aggregates. */
    uint8_t *agg_color   = (uint8_t *)calloc(mod->agg_types.len + 1u, 1u);
    bool    *agg_acyclic = (bool *)calloc(mod->agg_types.len + 1u, sizeof(bool));
    int8_t  *pad_cache   = (int8_t *)malloc(mod->agg_types.len + 1u);
    int8_t  *vl_cache    = (int8_t *)malloc(mod->agg_types.len + 1u);
    if (!agg_color || !agg_acyclic || !pad_cache || !vl_cache) abort();
    memset(pad_cache, -1, mod->agg_types.len + 1u);
    memset(vl_cache,  -1, mod->agg_types.len + 1u);
    for (uint32_t i = 0u; i < mod->agg_types.len; ++i) {
        const PirAggDef *a = &mod->agg_types.data[i];
        if (a->name) {
            check_unique_symbol(&v, a->name, a->src_line, seen, &seen_len);
        }
        if (mode == PIR_VERIFY_LIR) {
            pir_diag_error(diag, a->src_line, 0u,
                           "LIR forbids aggregate type declarations%s%s%s",
                           a->name ? " '@" : "", a->name ? a->name : "",
                           a->name ? "'" : "");
            continue;
        }
        if (a->kind == PIR_AGG_PAD) {
            if (a->array_len == 0u) {
                pir_diag_error(diag, a->src_line, 0u, "pad type length must be > 0");
            }
        } else if (a->kind == PIR_AGG_ARRAY) {
            if (a->array_len == 0u) {
                pir_diag_error(diag, a->src_line, 0u, "array type length must be > 0");
            }
            if (a->elem_type.kind == PIR_TYPE_SCALAR && a->elem_type.u.scalar == PIR_SCALAR_VOID) {
                pir_diag_error(diag, a->src_line, 0u, "array element type cannot be void");
            }
        } else {
            if (a->field_count == 0u) {
                pir_diag_error(diag, a->src_line, 0u, "struct/union '@%s' must have at least one field",
                               a->name ? a->name : "anon");
            }
            for (uint32_t f = 0u; f < a->field_count; ++f) {
                if (a->fields[f].kind == PIR_TYPE_SCALAR && a->fields[f].u.scalar == PIR_SCALAR_VOID) {
                    pir_diag_error(diag, a->src_line, 0u, "struct/union field cannot have type void");
                }
            }
        }
        if (a->min_align != 0u && (!align_valid_u32(a->min_align) || a->min_align > 4096u)) {
            pir_diag_error(diag, a->src_line, 0u,
                           "aggregate type alignment %u is invalid", a->min_align);
        }
        bool acyclic = check_agg_cycles(&v, i, agg_color);
        agg_acyclic[i] = acyclic;
        if (acyclic && target) {
            PirTypeLayout ly = pir_type_layout(mod, target, pir_type_agg(i));
            if (ly.overflow) {
                pir_diag_error(diag, a->src_line, 0u, "aggregate type '@%s' layout size overflow",
                               a->name ? a->name : "anon");
            }
        }
    }
    free(agg_color);

    /* Signatures. */
    for (uint32_t i = 0u; i < mod->sigs.len; ++i) {
        const PirSigDef *s = &mod->sigs.data[i];
        check_unique_symbol(&v, s->name, s->src_line, seen, &seen_len);
        verify_params_and_ret(&v, s->name, s->src_line, s->cc, s->attrs, s->params,
                              s->param_count, s->is_variadic, s->ret_type, &s->cabi_ret);
    }

    /* Globals. */
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        const PirGlobal *g = &mod->globals.data[i];
        check_unique_symbol(&v, g->name, g->src_line, seen, &seen_len);
        if (g->align != 0u && !align_valid_u32(g->align)) {
            pir_diag_error(diag, g->src_line, 0u,
                           "alignment %u is not a valid power of two", g->align);
        }
        if (g->linkage == PIR_LINK_EXTERN) {
            if (g->item_count > 0u || g->is_zeroinit || g->is_braced) {
                pir_diag_error(diag, g->src_line, 0u, "extern global '@%s' cannot have an initializer", g->name);
            }
        } else {
            if (!g->is_zeroinit && g->item_count == 0u) {
                pir_diag_error(diag, g->src_line, 0u, "global '@%s' requires an initializer", g->name);
            }
            if (target && target->os != PIR_OS_MACOS && target->os != PIR_OS_WINDOWS &&
                g->section && (strcmp(g->section, ".bss") == 0 || strcmp(g->section, ".tbss") == 0)) {
                bool all_zero = g->is_zeroinit;
                if (!all_zero && g->item_count > 0u) {
                    all_zero = true;
                    for (uint32_t k = 0u; k < g->item_count && all_zero; ++k) {
                        const PirGlobalItem *gi = &g->items[k];
                        switch (gi->kind) {
                            case PIR_GITEM_ZERO: break;
                            case PIR_GITEM_INT:
                                if (gi->int_lo != 0u || gi->int_hi != 0u) all_zero = false;
                                break;
                            case PIR_GITEM_FLOAT:
                                if (gi->float_bits != 0u || (gi->has_f32_bits && gi->float32_bits != 0u)) all_zero = false;
                                break;
                            case PIR_GITEM_BYTES:
                                for (uint64_t bi = 0u; bi < gi->len; ++bi) {
                                    if (gi->bytes[bi] != 0u) { all_zero = false; break; }
                                }
                                break;
                            case PIR_GITEM_RELOC:
                                all_zero = false;
                                break;
                        }
                    }
                }
                if (!all_zero) {
                    pir_diag_error(diag, g->src_line, 0u,
                                   "global '@%s' in nobits section '%s' cannot have non-zero initializer",
                                   g->name, g->section);
                }
            }
        }
        if (g->has_type) {
            if (g->type.kind == PIR_TYPE_SCALAR && g->type.u.scalar == PIR_SCALAR_VOID) {
                pir_diag_error(diag, g->src_line, 0u, "global '@%s' cannot have type void", g->name);
            }
            if (g->type.kind == PIR_TYPE_SCALAR && g->type.u.scalar == PIR_SCALAR_VALIST) {
                pir_diag_error(diag, g->src_line, 0u, "global '@%s' cannot have type va_list", g->name);
            }
            if (is_pad_agg_type(mod, g->type)) {
                pir_diag_error(diag, g->src_line, 0u, "global '@%s' cannot have standalone pad type", g->name);
            }
            if (g->type.kind == PIR_TYPE_VECTOR && !g->is_braced && !g->is_zeroinit &&
                g->item_count > 0u) {
                pir_diag_error(diag, g->src_line, 0u,
                               "vector global '@%s' requires braced or zeroinit initializer", g->name);
            }
            if (g->type.kind == PIR_TYPE_SCALAR && g->item_count > 0u) {
                if (g->item_count != 1u) {
                    pir_diag_error(diag, g->src_line, 0u,
                                   "scalar global '@%s' expects 1 initializer item, got %u",
                                   g->name, g->item_count);
                } else {
                    verify_scalar_global_item(&v, g, g->type.u.scalar, &g->items[0]);
                }
            } else if (g->type.kind == PIR_TYPE_VECTOR && g->is_braced) {
                uint32_t lanes = pir_vec_lanes(g->type.u.vec);
                PirScalarType elem = pir_vec_elem_scalar(g->type.u.vec);
                uint64_t vec_bytes = (uint64_t)(pir_vec_bits(g->type.u.vec) / 8u);
                bool single_zero = (g->item_count == 1u && g->items[0].kind == PIR_GITEM_ZERO);
                if (single_zero) {
                    if (g->items[0].len != 0u && g->items[0].len != vec_bytes) {
                        pir_diag_error(diag, g->src_line, 0u,
                                       "zeroinit size %" PRIu64 " mismatches vector size %" PRIu64 " in '@%s'",
                                       g->items[0].len, vec_bytes, g->name);
                    }
                } else {
                    if (g->item_count != lanes) {
                        pir_diag_error(diag, g->src_line, 0u,
                                       "vector global '@%s' expects %u lane initializers, got %u",
                                       g->name, lanes, g->item_count);
                    }
                    for (uint32_t k = 0u; k < g->item_count; ++k) {
                        PirGlobalItemKind ik = g->items[k].kind;
                        if (ik == PIR_GITEM_ZERO) {
                            pir_diag_error(diag, g->src_line, 0u,
                                           "zeroinit cannot be mixed in vector lane list for '@%s'", g->name);
                        } else {
                            verify_scalar_global_item(&v, g, elem, &g->items[k]);
                        }
                    }
                }
            } else if (g->type.kind == PIR_TYPE_NAMED_AGG && g->type.u.agg_id < mod->agg_types.len) {
                if (agg_acyclic[g->type.u.agg_id] &&
                    agg_contains_valist_rec(mod, g->type, vl_cache, 0u)) {
                    pir_diag_error(diag, g->src_line, 0u,
                                   "global '@%s' aggregate type cannot contain va_list", g->name);
                }
                const PirAggDef *ad = &mod->agg_types.data[g->type.u.agg_id];
                if (!g->is_zeroinit && !g->is_braced && g->item_count > 0u) {
                    bool is_i8_str = (ad->kind == PIR_AGG_ARRAY &&
                                      ad->elem_type.kind == PIR_TYPE_SCALAR &&
                                      ad->elem_type.u.scalar == PIR_SCALAR_I8 &&
                                      g->item_count == 1u &&
                                      g->items[0].kind == PIR_GITEM_BYTES);
                    if (!is_i8_str) {
                        pir_diag_error(diag, g->src_line, 0u,
                                       "aggregate global '@%s' requires braced or zeroinit initializer", g->name);
                    } else if (g->items[0].len > ad->array_len) {
                        pir_diag_error(diag, g->src_line, 0u,
                                       "string initializer for '@%s' exceeds array length (%" PRIu64 " > %" PRIu64 ")",
                                       g->name, g->items[0].len, ad->array_len);
                    }
                } else if (g->is_braced && g->item_count > 0u && agg_acyclic[g->type.u.agg_id]) {
                    if (!(g->item_count == 1u && g->items[0].kind == PIR_GITEM_ZERO &&
                          (g->items[0].len == 0u ||
                           (target && g->items[0].len == pir_type_layout(mod, target, g->type).size)))) {
                        uint32_t idx = 0u;
                        verify_agg_global_items(&v, g, g->type, &idx, pad_cache, 0u);
                        if (idx < g->item_count) {
                            pir_diag_error(diag, g->src_line, 0u,
                                           "aggregate global '@%s' has too many initializer items (%u > %u)",
                                           g->name, g->item_count, idx);
                        }
                    }
                }
            }
            check_lir_type(&v, g->type, g->src_line, "global");
        } else if (g->is_zeroinit && g->item_count == 1u &&
                   g->items[0].kind == PIR_GITEM_ZERO) {
            if (g->items[0].len == 0u) {
                pir_diag_error(diag, g->src_line, 0u, "flattened zeroinit global '@%s' must have size > 0", g->name);
            } else if (g->items[0].len > max_obj_bytes(target)) {
                pir_diag_error(diag, g->src_line, 0u, "flattened zeroinit global '@%s' size %" PRIu64 " exceeds maximum",
                               g->name, g->items[0].len);
            }
        } else if (g->is_braced) {
            uint64_t total_bytes = 0u;
            uint64_t max_sz = max_obj_bytes(target);
            uint8_t ptr_b = target ? target->ptr_bytes : 8u;
            for (uint32_t k = 0u; k < g->item_count; ++k) {
                const PirGlobalItem *gi = &g->items[k];
                PirGlobalItemKind ik = gi->kind;
                uint64_t item_sz = 0u;
                if (ik == PIR_GITEM_INT) {
                    if (gi->int_hi != 0u || (gi->int_neg ? gi->int_lo > 128u : gi->int_lo > 255u)) {
                        pir_diag_error(diag, g->src_line, 0u,
                                       "integer byte item in untyped global '@%s' out of 8-bit range",
                                       g->name);
                    }
                    item_sz = 1u;
                } else if (ik == PIR_GITEM_BYTES) {
                    item_sz = gi->len;
                } else if (ik == PIR_GITEM_RELOC) {
                    item_sz = ptr_b;
                } else if (ik == PIR_GITEM_ZERO) {
                    if (gi->len == 0u) {
                        pir_diag_error(diag, g->src_line, 0u,
                                       "zeroinit item in untyped global '@%s' must have positive byte count", g->name);
                    }
                    item_sz = gi->len;
                } else {
                    pir_diag_error(diag, g->src_line, 0u,
                                   "untyped braced global '@%s' only allows byte strings, byte ints, zeroinit N, or ptr relocations",
                                   g->name);
                }
                if (item_sz > max_sz || total_bytes > max_sz - item_sz) {
                    pir_diag_error(diag, g->src_line, 0u,
                                   "untyped global '@%s' byte size exceeds maximum", g->name);
                    break;
                }
                total_bytes += item_sz;
            }
        }
        for (uint32_t k = 0u; k < g->item_count; ++k) {
            if (g->items[k].kind == PIR_GITEM_RELOC) {
                const char *rname = g->items[k].sym_name;
                if (!has_symbol(mod, rname)) {
                    pir_diag_error(diag, g->src_line, 0u,
                                   "global '@%s' references undefined symbol '@%s'",
                                   g->name, rname);
                } else {
                    for (uint32_t gi = 0u; gi < mod->globals.len; ++gi) {
                        if (strcmp(mod->globals.data[gi].name, rname) == 0 &&
                            mod->globals.data[gi].seg == PIR_GSEG_TLS) {
                            pir_diag_error(diag, g->src_line, 0u,
                                           "global '@%s' cannot take static address of tls global '@%s'",
                                           g->name, rname);
                            break;
                        }
                    }
                }
            }
        }
    }

    free(vl_cache);
    free(pad_cache);
    free(agg_acyclic);

    /* Functions. */
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        const PirFunction *fn = &mod->funcs.data[i];
        check_unique_symbol(&v, fn->name, fn->src_line, seen, &seen_len);
        verify_function(&v, fn);
    }

    free(seen);
    return !pir_diag_has_errors(diag);
}
