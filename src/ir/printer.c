/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ir/printer.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

static bool is_bare_sym(const char *s) {
    if (!s || !s[0]) return false;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) {
        return false;
    }
    for (size_t i = 1u; s[i]; ++i) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

static void print_escaped_string(const uint8_t *bytes, size_t len, FILE *out) {
    fputc('"', out);
    for (size_t i = 0u; i < len; ++i) {
        uint8_t b = bytes[i];
        switch (b) {
            case '\n': fputs("\\n", out);  break;
            case '\r': fputs("\\r", out);  break;
            case '\t': fputs("\\t", out);  break;
            case '\\': fputs("\\\\", out); break;
            case '"':  fputs("\\\"", out); break;
            case '\0': fputs("\\0", out);  break;
            default:
                if (b >= 0x20u && b <= 0x7Eu) {
                    fputc((int)b, out);
                } else {
                    fprintf(out, "\\x%02x", (unsigned)b);
                }
                break;
        }
    }
    fputc('"', out);
}

static void print_global_sym(const char *name, FILE *out) {
    fputc('@', out);
    if (is_bare_sym(name)) {
        fputs(name, out);
    } else {
        print_escaped_string((const uint8_t *)name, strlen(name), out);
    }
}

static void print_u128(uint64_t hi, uint64_t lo, bool neg, FILE *out) {
    if (neg) fputc('-', out);
    if (hi == 0u) {
        fprintf(out, "%" PRIu64, lo);
    } else {
        fprintf(out, "0x%" PRIx64 "%016" PRIx64, hi, lo);
    }
}

static void print_float_bits(uint64_t bits, PirType ty, FILE *out) {
    PirScalarType sc = (ty.kind == PIR_TYPE_VECTOR)
                       ? pir_vec_elem_scalar(ty.u.vec)
                       : (ty.kind == PIR_TYPE_SCALAR ? ty.u.scalar : PIR_SCALAR_F64);
    double val = 0.0;
    if (sc == PIR_SCALAR_F32) {
        uint32_t b32 = (uint32_t)bits;
        float f = 0.0f;
        memcpy(&f, &b32, sizeof(f));
        val = (double)f;
    } else {
        memcpy(&val, &bits, sizeof(val));
    }

    if (isnan(val)) {
        bool neg = (sc == PIR_SCALAR_F32)
                   ? ((bits & UINT64_C(0x80000000)) != 0u)
                   : ((bits >> 63u) != 0u);
        fputs(neg ? "-nan" : "nan", out);
        return;
    }
    if (isinf(val)) {
        fputs(val < 0.0 ? "-inf" : "inf", out);
        return;
    }
    char buf[64];
    if (sc == PIR_SCALAR_F32) {
        snprintf(buf, sizeof(buf), "%.9g", val);
    } else {
        snprintf(buf, sizeof(buf), "%.17g", val);
    }
    fputs(buf, out);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
        fputs(".0", out);
    }
}

static const char *scalar_name(PirScalarType s) {
    switch (s) {
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
}

static const char *vec_name(PirVecType v) {
    switch (v) {
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
}

void pir_print_type(const PirModule *mod, PirType ty, FILE *out) {
    switch (ty.kind) {
        case PIR_TYPE_SCALAR:
            fputs(scalar_name(ty.u.scalar), out);
            break;
        case PIR_TYPE_VECTOR:
            fputs(vec_name(ty.u.vec), out);
            break;
        case PIR_TYPE_NAMED_AGG: {
            if (ty.u.agg_id >= mod->agg_types.len) {
                fputs("@<invalid>", out);
                break;
            }
            const PirAggDef *agg = &mod->agg_types.data[ty.u.agg_id];
            if (agg->name) {
                print_global_sym(agg->name, out);
            } else if (agg->kind == PIR_AGG_PAD) {
                fprintf(out, "pad %" PRIu64, agg->array_len);
            } else {
                fprintf(out, "[%" PRIu64 " x ", agg->array_len);
                pir_print_type(mod, agg->elem_type, out);
                fputc(']', out);
            }
            break;
        }
    }
}

static const char *order_name(PirMemOrder o) {
    switch (o) {
        case PIR_ORDER_NONE:    return "none";
        case PIR_ORDER_RELAXED: return "relaxed";
        case PIR_ORDER_ACQUIRE: return "acquire";
        case PIR_ORDER_RELEASE: return "release";
        case PIR_ORDER_ACQ_REL: return "acq_rel";
        case PIR_ORDER_SEQ_CST: return "seq_cst";
    }
    return "none";
}

static const char *rmw_name(PirRmwOp r) {
    switch (r) {
        case PIR_RMW_ADD:  return "add";
        case PIR_RMW_SUB:  return "sub";
        case PIR_RMW_AND:  return "and";
        case PIR_RMW_OR:   return "or";
        case PIR_RMW_XOR:  return "xor";
        case PIR_RMW_XCHG: return "xchg";
    }
    return "add";
}

static const char *opcode_name(PirOpcode op) {
    switch (op) {
        case PIR_OP_CONST:           return "const";
        case PIR_OP_ADDR:            return "addr";
        case PIR_OP_ADD:             return "add";
        case PIR_OP_SUB:             return "sub";
        case PIR_OP_MUL:             return "mul";
        case PIR_OP_DIV_S:           return "div_s";
        case PIR_OP_DIV_U:           return "div_u";
        case PIR_OP_REM_S:           return "rem_s";
        case PIR_OP_REM_U:           return "rem_u";
        case PIR_OP_ADD_OVF_S:       return "add_ovf_s";
        case PIR_OP_ADD_OVF_U:       return "add_ovf_u";
        case PIR_OP_SUB_OVF_S:       return "sub_ovf_s";
        case PIR_OP_SUB_OVF_U:       return "sub_ovf_u";
        case PIR_OP_MUL_OVF_S:       return "mul_ovf_s";
        case PIR_OP_MUL_OVF_U:       return "mul_ovf_u";
        case PIR_OP_MUL_WIDE_U:      return "mul_wide_u";
        case PIR_OP_MUL_WIDE_S:      return "mul_wide_s";
        case PIR_OP_ADD_CARRY:       return "add_carry";
        case PIR_OP_SUB_BORROW:      return "sub_borrow";
        case PIR_OP_AND:             return "and";
        case PIR_OP_OR:              return "or";
        case PIR_OP_XOR:             return "xor";
        case PIR_OP_SHL:             return "shl";
        case PIR_OP_SHR:             return "shr";
        case PIR_OP_SAR:             return "sar";
        case PIR_OP_CLZ:             return "clz";
        case PIR_OP_CTZ:             return "ctz";
        case PIR_OP_POPCNT:          return "popcnt";
        case PIR_OP_BSWAP:           return "bswap";
        case PIR_OP_FADD:            return "fadd";
        case PIR_OP_FSUB:            return "fsub";
        case PIR_OP_FMUL:            return "fmul";
        case PIR_OP_FDIV:            return "fdiv";
        case PIR_OP_FNEG:            return "fneg";
        case PIR_OP_CMP_EQ:          return "cmp_eq";
        case PIR_OP_CMP_NE:          return "cmp_ne";
        case PIR_OP_CMP_LT_S:        return "cmp_lt_s";
        case PIR_OP_CMP_LT_U:        return "cmp_lt_u";
        case PIR_OP_CMP_LE_S:        return "cmp_le_s";
        case PIR_OP_CMP_LE_U:        return "cmp_le_u";
        case PIR_OP_CMP_GT_S:        return "cmp_gt_s";
        case PIR_OP_CMP_GT_U:        return "cmp_gt_u";
        case PIR_OP_CMP_GE_S:        return "cmp_ge_s";
        case PIR_OP_CMP_GE_U:        return "cmp_ge_u";
        case PIR_OP_FCMP_OEQ:        return "fcmp_oeq";
        case PIR_OP_FCMP_ONE:        return "fcmp_one";
        case PIR_OP_FCMP_OLT:        return "fcmp_olt";
        case PIR_OP_FCMP_OLE:        return "fcmp_ole";
        case PIR_OP_FCMP_OGT:        return "fcmp_ogt";
        case PIR_OP_FCMP_OGE:        return "fcmp_oge";
        case PIR_OP_FCMP_UEQ:        return "fcmp_ueq";
        case PIR_OP_FCMP_UNE:        return "fcmp_une";
        case PIR_OP_FCMP_ULT:        return "fcmp_ult";
        case PIR_OP_FCMP_ULE:        return "fcmp_ule";
        case PIR_OP_FCMP_UGT:        return "fcmp_ugt";
        case PIR_OP_FCMP_UGE:        return "fcmp_uge";
        case PIR_OP_SELECT:          return "select";
        case PIR_OP_ZEXT:            return "zext";
        case PIR_OP_SEXT:            return "sext";
        case PIR_OP_TRUNC:           return "trunc";
        case PIR_OP_FPTOSI:          return "fptosi";
        case PIR_OP_FPTOUI:          return "fptoui";
        case PIR_OP_SITOFP:          return "sitofp";
        case PIR_OP_UITOFP:          return "uitofp";
        case PIR_OP_FPEXT:           return "fpext";
        case PIR_OP_FPTRUNC:         return "fptrunc";
        case PIR_OP_BITCAST:         return "bitcast";
        case PIR_OP_PTRTOINT:        return "ptrtoint";
        case PIR_OP_INTTOPTR:        return "inttoptr";
        case PIR_OP_SPLAT:           return "splat";
        case PIR_OP_EXTRACT:         return "extract";
        case PIR_OP_INSERT:          return "insert";
        case PIR_OP_SHUFFLE:         return "shuffle";
        case PIR_OP_ALLOCA:          return "alloca";
        case PIR_OP_ALLOCA_PTR:      return "alloca.ptr";
        case PIR_OP_ALLOCA_DYN:      return "alloca_dyn";
        case PIR_OP_STACKSAVE:       return "stacksave";
        case PIR_OP_STACKRESTORE:    return "stackrestore";
        case PIR_OP_FIELD_PTR:       return "field_ptr";
        case PIR_OP_ELEM_PTR:        return "elem_ptr";
        case PIR_OP_PTR_ADD:         return "ptr_add";
        case PIR_OP_LOAD:            return "load";
        case PIR_OP_STORE:           return "store";
        case PIR_OP_MEMCPY:          return "memcpy";
        case PIR_OP_MEMMOVE:         return "memmove";
        case PIR_OP_MEMSET:          return "memset";
        case PIR_OP_ATOMIC_LOAD:     return "atomic_load";
        case PIR_OP_ATOMIC_STORE:    return "atomic_store";
        case PIR_OP_ATOMIC_RMW:      return "atomic_rmw";
        case PIR_OP_ATOMIC_CMPXCHG:  return "atomic_cmpxchg";
        case PIR_OP_FENCE:           return "fence";
        case PIR_OP_VA_START:        return "va_start";
        case PIR_OP_VA_ARG:          return "va_arg";
        case PIR_OP_VA_ARG_BUF:      return "va_arg_buf";
        case PIR_OP_VA_COPY:         return "va_copy";
        case PIR_OP_VA_END:          return "va_end";
        case PIR_OP_CALL:            return "call";
        case PIR_OP_CALL_INDIRECT:   return "call_indirect";
        case PIR_OP_JMP:             return "jmp";
        case PIR_OP_BR:              return "br";
        case PIR_OP_SWITCH:          return "switch";
        case PIR_OP_INVOKE:          return "invoke";
        case PIR_OP_INVOKE_INDIRECT: return "invoke_indirect";
        case PIR_OP_RESUME:          return "resume";
        case PIR_OP_RET:             return "ret";
        case PIR_OP_TRAP:            return "trap";
        case PIR_OP_UNREACHABLE:     return "unreachable";
    }
    return "unknown";
}

static void print_flags(uint32_t flags, FILE *out) {
    if (flags == PIR_FLAG_NONE) return;
    static const struct { uint32_t bit; const char *name; } tbl[] = {
        { PIR_FLAG_WRAP,     "wrap"     },
        { PIR_FLAG_TRAP,     "trap"     },
        { PIR_FLAG_TRAP_S,   "trap_s"   },
        { PIR_FLAG_TRAP_U,   "trap_u"   },
        { PIR_FLAG_UNSAFE,   "unsafe"   },
        { PIR_FLAG_UNSAFE_S, "unsafe_s" },
        { PIR_FLAG_UNSAFE_U, "unsafe_u" },
        { PIR_FLAG_MASK,     "mask"     },
        { PIR_FLAG_FAST,     "fast"     },
        { PIR_FLAG_SAT,      "sat"      },
        { PIR_FLAG_INBOUNDS, "inbounds" },
        { PIR_FLAG_VOLATILE, "volatile" },
        { PIR_FLAG_WEAK,     "weak"     },
        { PIR_FLAG_NOUNWIND, "nounwind" },
        { PIR_FLAG_NONZERO,  "nonzero"  }
    };
    fputs(" [", out);
    bool first = true;
    for (size_t i = 0u; i < sizeof(tbl) / sizeof(tbl[0]); ++i) {
        if (flags & tbl[i].bit) {
            if (!first) fputs(", ", out);
            fputs(tbl[i].name, out);
            first = false;
        }
    }
    fputc(']', out);
}

static void print_operand(const PirModule *mod, const PirFunction *fn,
                          const PirOperand *op, FILE *out) {
    switch (op->kind) {
        case PIR_OPER_VAL:
            if (op->u.val < fn->values.len) {
                fprintf(out, "%%%s", fn->values.data[op->u.val].name);
            } else {
                fprintf(out, "%%<invalid:%u>", op->u.val);
            }
            break;
        case PIR_OPER_IMM_INT:
            print_u128(op->u.imm_int.hi, op->u.imm_int.lo, op->u.imm_int.is_negative, out);
            break;
        case PIR_OPER_IMM_FLOAT:
            print_float_bits(op->u.imm_float.bits, op->type, out);
            break;
        case PIR_OPER_SYM:
            print_global_sym(op->u.sym.name, out);
            if (op->u.sym.offset > 0)      fprintf(out, " + %" PRId64, op->u.sym.offset);
            else if (op->u.sym.offset < 0) fprintf(out, " - %" PRIu64, UINT64_C(0) - (uint64_t)op->u.sym.offset);
            break;
        case PIR_OPER_INVOKE_RET:
            fputs("ret", out);
            break;
        case PIR_OPER_INVOKE_EXN:
            fputs("exn", out);
            break;
    }
    if (op->has_explicit_type) {
        fputs(": ", out);
        if (op->call_abi == PIR_PARAM_BYVAL) {
            fputs("byval(", out);
            pir_print_type(mod, op->call_abi_type, out);
            fputc(')', out);
        } else {
            pir_print_type(mod, op->type, out);
        }
    }
    if (op->call_byval_size > 0u) {
        fprintf(out, " byval_size %" PRIu64, op->call_byval_size);
    }
    if (op->call_byval_align > 0u) {
        fprintf(out, " byval_align %u", op->call_byval_align);
    }
    if (op->call_stack_align > 0u) {
        fprintf(out, " stack_align %u", op->call_stack_align);
    }
    if (op->call_force_stack) {
        fputs(" force_stack", out);
    }
    if (op->call_no_f32_promote) {
        fputs(" no_f32_promote", out);
    }
}

static void print_branch_target(const PirModule *mod, const PirFunction *fn,
                                const PirBranchTarget *bt, FILE *out) {
    if (bt->target < fn->blocks.len) {
        fprintf(out, ".%s", fn->blocks.data[bt->target].label);
    } else {
        fprintf(out, ".<invalid:%u>", bt->target);
    }
    if (bt->arg_count > 0u) {
        fputc('(', out);
        for (uint32_t i = 0u; i < bt->arg_count; ++i) {
            if (i > 0u) fputs(", ", out);
            print_operand(mod, fn, &bt->args[i], out);
        }
        fputc(')', out);
    }
}

static void print_param_list(const PirModule *mod, const PirParamDecl *params,
                             uint32_t count, bool is_variadic, FILE *out) {
    fputc('(', out);
    for (uint32_t i = 0u; i < count; ++i) {
        if (i > 0u) fputs(", ", out);
        if (params[i].name) {
            fprintf(out, "%%%s: ", params[i].name);
        }
        switch (params[i].abi_kind) {
            case PIR_PARAM_NORMAL:
                pir_print_type(mod, params[i].type, out);
                break;
            case PIR_PARAM_BYVAL:
                fputs("byval(", out);
                pir_print_type(mod, params[i].abi_type, out);
                fputc(')', out);
                break;
            case PIR_PARAM_RETBUF:
                fputs("retbuf(", out);
                pir_print_type(mod, params[i].abi_type, out);
                fputc(')', out);
                break;
        }
        if (params[i].attrs & PIR_PATTR_NOALIAS)  fputs(" noalias", out);
        if (params[i].attrs & PIR_PATTR_READONLY) fputs(" readonly", out);
        if (params[i].attrs & PIR_PATTR_SIGNEXT)  fputs(" signext", out);
        if (params[i].attrs & PIR_PATTR_ZEROEXT)  fputs(" zeroext", out);
        if (params[i].abi_byval_size > 0u) {
            fprintf(out, " byval_size %" PRIu64, params[i].abi_byval_size);
        }
        if (params[i].abi_byval_align > 0u) {
            fprintf(out, " byval_align %u", params[i].abi_byval_align);
        }
        if (params[i].abi_stack_align > 0u) {
            fprintf(out, " stack_align %u", params[i].abi_stack_align);
        }
        if (params[i].abi_force_stack) {
            fputs(" force_stack", out);
        }
    }
    if (is_variadic) {
        if (count > 0u) fputs(", ", out);
        fputs("...", out);
    }
    fputc(')', out);
}

static void print_linkage(PirLinkage l, bool is_weak_extern, FILE *out) {
    switch (l) {
        case PIR_LINK_INTERNAL: break;
        case PIR_LINK_EXPORT:   fputs("export ", out); break;
        case PIR_LINK_EXTERN:
            fputs(is_weak_extern ? "weak extern " : "extern ", out);
            break;
        case PIR_LINK_WEAK:     fputs("weak ", out);   break;
    }
}

static void print_fn_cc_attrs(const PirModule *mod, PirCallingConv cc, uint32_t attrs,
                              const char *sec, const PirCabiRetInfo *cabi_ret, FILE *out) {
    switch (cc) {
        case PIR_CC_INTERNAL: break;
        case PIR_CC_C:        fputs(" cc \"c\"", out); break;
    }
    if (sec) {
        fputs(" section ", out);
        print_escaped_string((const uint8_t *)sec, strlen(sec), out);
    }
    if (attrs & PIR_FATTR_NOUNWIND)    fputs(" nounwind", out);
    if (attrs & PIR_FATTR_NORETURN)    fputs(" noreturn", out);
    if (attrs & PIR_FATTR_READONLY)    fputs(" readonly", out);
    if (attrs & PIR_FATTR_RET_SIGNEXT) fputs(" signext", out);
    if (attrs & PIR_FATTR_RET_ZEROEXT) fputs(" zeroext", out);
    if (cabi_ret) {
        switch (cabi_ret->kind) {
            case PIR_CABI_RET_NONE:
                break;
            case PIR_CABI_RET_SRET_X8:
                fputs(" sret_x8", out);
                break;
            case PIR_CABI_RET_SRET_RAX:
                fputs(" sret_rax", out);
                break;
            case PIR_CABI_RET_MULTI_REG:
                fputs(" multi_ret(", out);
                for (uint32_t r = 0u; r < cabi_ret->reg_count && r < 4u; ++r) {
                    if (r > 0u) fputs(", ", out);
                    pir_print_type(mod, cabi_ret->regs[r], out);
                }
                fprintf(out, ", size %" PRIu64 ", align %u)",
                        cabi_ret->agg_size, cabi_ret->agg_align);
                break;
        }
    }
}

static void print_global_item(const PirGlobalItem *it, PirType hint_ty, FILE *out) {
    switch (it->kind) {
        case PIR_GITEM_BYTES:
            print_escaped_string(it->bytes, (size_t)it->len, out);
            break;
        case PIR_GITEM_ZERO:
            if (it->len > 0u) {
                fprintf(out, "zeroinit %" PRIu64, it->len);
            } else {
                fputs("zeroinit", out);
            }
            break;
        case PIR_GITEM_INT:
            print_u128(it->int_hi, it->int_lo, it->int_neg, out);
            break;
        case PIR_GITEM_FLOAT:
            print_float_bits(it->float_bits, hint_ty, out);
            break;
        case PIR_GITEM_RELOC:
            fputs("ptr ", out);
            print_global_sym(it->sym_name, out);
            if (it->sym_offset > 0)      fprintf(out, " + %" PRId64, it->sym_offset);
            else if (it->sym_offset < 0) fprintf(out, " - %" PRIu64, UINT64_C(0) - (uint64_t)it->sym_offset);
            break;
    }
}

static void print_inst(const PirModule *mod, const PirFunction *fn,
                       const PirInst *inst, FILE *out) {
    fputs("    ", out);
    if (inst->results[0] != PIR_INVALID_ID) {
        fprintf(out, "%%%s", fn->values.data[inst->results[0]].name);
        if (inst->results[1] != PIR_INVALID_ID) {
            fprintf(out, ", %%%s", fn->values.data[inst->results[1]].name);
        }
        fputs(" = ", out);
    }

    fputs(opcode_name(inst->op), out);

    /* Print opcode sub-op / type suffix where syntactically expected. */
    switch (inst->op) {
        case PIR_OP_ATOMIC_RMW:
            fprintf(out, ".%s.", rmw_name(inst->rmw_op));
            pir_print_type(mod, inst->ty, out);
            break;
        case PIR_OP_CONST:
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
        case PIR_OP_PTR_ADD:
        case PIR_OP_LOAD: case PIR_OP_STORE:
        case PIR_OP_ATOMIC_LOAD: case PIR_OP_ATOMIC_STORE: case PIR_OP_ATOMIC_CMPXCHG:
        case PIR_OP_VA_ARG:
        case PIR_OP_SWITCH:
            fputc('.', out);
            pir_print_type(mod, inst->ty, out);
            break;
        case PIR_OP_ADDR:
        case PIR_OP_ALLOCA: case PIR_OP_ALLOCA_PTR: case PIR_OP_ALLOCA_DYN:
        case PIR_OP_STACKSAVE: case PIR_OP_STACKRESTORE:
        case PIR_OP_FIELD_PTR: case PIR_OP_ELEM_PTR:
        case PIR_OP_MEMCPY: case PIR_OP_MEMMOVE: case PIR_OP_MEMSET:
        case PIR_OP_FENCE:
        case PIR_OP_VA_START: case PIR_OP_VA_ARG_BUF: case PIR_OP_VA_COPY: case PIR_OP_VA_END:
        case PIR_OP_CALL: case PIR_OP_CALL_INDIRECT:
        case PIR_OP_JMP: case PIR_OP_BR:
        case PIR_OP_INVOKE: case PIR_OP_INVOKE_INDIRECT: case PIR_OP_RESUME:
        case PIR_OP_RET: case PIR_OP_TRAP: case PIR_OP_UNREACHABLE:
            break;
    }

    /* Print instruction-specific operands & metadata. */
    switch (inst->op) {
        case PIR_OP_ALLOCA:
            fputc(' ', out);
            pir_print_type(mod, inst->aux_type, out);
            if (inst->align > 0u) fprintf(out, ", align %u", inst->align);
            break;
        case PIR_OP_ALLOCA_PTR:
            fprintf(out, " %" PRIu64 ", align %u", inst->imm_u64, inst->align);
            break;
        case PIR_OP_ALLOCA_DYN:
            fputc(' ', out);
            print_operand(mod, fn, &inst->operands[0], out);
            fprintf(out, ", align %u", inst->align);
            break;
        case PIR_OP_FIELD_PTR:
            fputc(' ', out);
            pir_print_type(mod, inst->aux_type, out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[0], out);
            fprintf(out, ", %" PRIu64, inst->imm_u64);
            break;
        case PIR_OP_ELEM_PTR:
            fputc(' ', out);
            pir_print_type(mod, inst->aux_type, out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[0], out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[1], out);
            break;
        case PIR_OP_LOAD:
            fputc(' ', out);
            print_operand(mod, fn, &inst->operands[0], out);
            fprintf(out, ", align %u", inst->align);
            print_flags(inst->flags, out);
            break;
        case PIR_OP_STORE:
            fputc(' ', out);
            print_operand(mod, fn, &inst->operands[0], out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[1], out);
            fprintf(out, ", align %u", inst->align);
            print_flags(inst->flags, out);
            break;
        case PIR_OP_MEMCPY:
        case PIR_OP_MEMMOVE:
        case PIR_OP_MEMSET:
            fputc(' ', out);
            print_operand(mod, fn, &inst->operands[0], out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[1], out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[2], out);
            fprintf(out, ", align %u", inst->align);
            break;
        case PIR_OP_ATOMIC_LOAD:
            fputc(' ', out);
            print_operand(mod, fn, &inst->operands[0], out);
            fprintf(out, " [%s]", order_name(inst->order));
            break;
        case PIR_OP_ATOMIC_STORE:
        case PIR_OP_ATOMIC_RMW:
            fputc(' ', out);
            print_operand(mod, fn, &inst->operands[0], out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[1], out);
            fprintf(out, " [%s]", order_name(inst->order));
            break;
        case PIR_OP_ATOMIC_CMPXCHG:
            fputc(' ', out);
            print_operand(mod, fn, &inst->operands[0], out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[1], out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[2], out);
            fprintf(out, " [%s, %s]", order_name(inst->order), order_name(inst->fail_order));
            print_flags(inst->flags, out);
            break;
        case PIR_OP_FENCE:
            fprintf(out, " [%s]", order_name(inst->order));
            break;
        case PIR_OP_VA_ARG_BUF:
            fputc(' ', out);
            pir_print_type(mod, inst->aux_type, out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[0], out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[1], out);
            break;
        case PIR_OP_EXTRACT:
            fputc(' ', out);
            print_operand(mod, fn, &inst->operands[0], out);
            fprintf(out, ", %" PRIu64, inst->imm_u64);
            break;
        case PIR_OP_INSERT:
            fputc(' ', out);
            print_operand(mod, fn, &inst->operands[0], out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[1], out);
            fprintf(out, ", %" PRIu64, inst->imm_u64);
            break;
        case PIR_OP_SHUFFLE:
            fputc(' ', out);
            print_operand(mod, fn, &inst->operands[0], out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[1], out);
            fputs(" [", out);
            for (uint32_t k = 0u; k < inst->shuffle_len; ++k) {
                if (k > 0u) fputs(", ", out);
                fprintf(out, "%u", (unsigned)inst->shuffle_mask[k]);
            }
            fputc(']', out);
            break;
        case PIR_OP_CALL:
            fputc(' ', out);
            print_operand(mod, fn, &inst->operands[0], out);
            fputc('(', out);
            for (uint32_t k = 1u; k < inst->operand_count; ++k) {
                if (k > 1u) fputs(", ", out);
                print_operand(mod, fn, &inst->operands[k], out);
            }
            fputc(')', out);
            print_flags(inst->flags, out);
            break;
        case PIR_OP_CALL_INDIRECT:
            fputc(' ', out);
            print_global_sym(mod->sigs.data[inst->sig_id].name, out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[0], out);
            fputc('(', out);
            for (uint32_t k = 1u; k < inst->operand_count; ++k) {
                if (k > 1u) fputs(", ", out);
                print_operand(mod, fn, &inst->operands[k], out);
            }
            fputc(')', out);
            print_flags(inst->flags, out);
            break;
        case PIR_OP_INVOKE:
            fputc(' ', out);
            print_operand(mod, fn, &inst->operands[0], out);
            fputc('(', out);
            for (uint32_t k = 1u; k < inst->operand_count; ++k) {
                if (k > 1u) fputs(", ", out);
                print_operand(mod, fn, &inst->operands[k], out);
            }
            fputs(") then ", out);
            print_branch_target(mod, fn, &inst->targets[0], out);
            fputs(" unwind ", out);
            print_branch_target(mod, fn, &inst->targets[1], out);
            break;
        case PIR_OP_INVOKE_INDIRECT:
            fputc(' ', out);
            print_global_sym(mod->sigs.data[inst->sig_id].name, out);
            fputs(", ", out);
            print_operand(mod, fn, &inst->operands[0], out);
            fputc('(', out);
            for (uint32_t k = 1u; k < inst->operand_count; ++k) {
                if (k > 1u) fputs(", ", out);
                print_operand(mod, fn, &inst->operands[k], out);
            }
            fputs(") then ", out);
            print_branch_target(mod, fn, &inst->targets[0], out);
            fputs(" unwind ", out);
            print_branch_target(mod, fn, &inst->targets[1], out);
            break;
        case PIR_OP_JMP:
            fputc(' ', out);
            print_branch_target(mod, fn, &inst->targets[0], out);
            break;
        case PIR_OP_BR:
            fputc(' ', out);
            print_operand(mod, fn, &inst->operands[0], out);
            fputs(", ", out);
            print_branch_target(mod, fn, &inst->targets[0], out);
            fputs(", ", out);
            print_branch_target(mod, fn, &inst->targets[1], out);
            break;
        case PIR_OP_SWITCH:
            fputc(' ', out);
            print_operand(mod, fn, &inst->operands[0], out);
            fputs(", default ", out);
            print_branch_target(mod, fn, &inst->targets[0], out);
            fputs(" [\n", out);
            for (uint32_t c = 0u; c < inst->switch_case_count; ++c) {
                fputs("        ", out);
                print_u128(inst->switch_cases[c].case_hi, inst->switch_cases[c].case_lo,
                           inst->switch_cases[c].is_negative, out);
                fputs(" -> ", out);
                print_branch_target(mod, fn, &inst->switch_cases[c].target, out);
                if (c + 1u < inst->switch_case_count) fputc(',', out);
                fputc('\n', out);
            }
            fputs("    ]", out);
            break;
        case PIR_OP_RET:
            if (inst->operand_count > 0u) {
                fputc(' ', out);
                print_operand(mod, fn, &inst->operands[0], out);
            }
            break;
        case PIR_OP_STACKSAVE:
        case PIR_OP_TRAP:
        case PIR_OP_UNREACHABLE:
            break;
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
        case PIR_OP_SPLAT:
        case PIR_OP_STACKRESTORE:
        case PIR_OP_PTR_ADD:
        case PIR_OP_VA_START: case PIR_OP_VA_ARG: case PIR_OP_VA_COPY: case PIR_OP_VA_END:
        case PIR_OP_RESUME:
            for (uint32_t k = 0u; k < inst->operand_count; ++k) {
                fputs(k == 0u ? " " : ", ", out);
                print_operand(mod, fn, &inst->operands[k], out);
            }
            print_flags(inst->flags, out);
            break;
    }
    fputc('\n', out);
}

void pir_print_module(const PirModule *mod, FILE *out) {
    fputs("target ", out);
    print_escaped_string((const uint8_t *)mod->target_triple, strlen(mod->target_triple), out);
    fputc('\n', out);

    for (uint32_t i = 0u; i < mod->files.len; ++i) {
        fprintf(out, "file %u ", mod->files.data[i].id);
        print_escaped_string((const uint8_t *)mod->files.data[i].path,
                             strlen(mod->files.data[i].path), out);
        fputc('\n', out);
    }

    bool had_section = (mod->files.len > 0u);
    for (uint32_t i = 0u; i < mod->agg_types.len; ++i) {
        const PirAggDef *a = &mod->agg_types.data[i];
        if (!a->name) continue;
        if (!had_section) { fputc('\n', out); had_section = true; }
        fputs("type ", out);
        print_global_sym(a->name, out);
        fputs(" = ", out);
        switch (a->kind) {
            case PIR_AGG_STRUCT:        fputs("{ ", out); break;
            case PIR_AGG_PACKED_STRUCT: fputs("packed { ", out); break;
            case PIR_AGG_UNION:         fputs("union { ", out); break;
            case PIR_AGG_ARRAY:
                fprintf(out, "[%" PRIu64 " x ", a->array_len);
                pir_print_type(mod, a->elem_type, out);
                fputc(']', out);
                if (a->min_align > 0u) fprintf(out, ", align %u", a->min_align);
                if (a->abi_memory) fputs(", memory", out);
                fputc('\n', out);
                continue;
            case PIR_AGG_PAD:
                fprintf(out, "pad %" PRIu64, a->array_len);
                if (a->min_align > 0u) fprintf(out, ", align %u", a->min_align);
                if (a->abi_memory) fputs(", memory", out);
                fputc('\n', out);
                continue;
        }
        for (uint32_t f = 0u; f < a->field_count; ++f) {
            if (f > 0u) fputs(", ", out);
            pir_print_type(mod, a->fields[f], out);
        }
        fputs(" }", out);
        if (a->min_align > 0u) fprintf(out, ", align %u", a->min_align);
        if (a->abi_memory) fputs(", memory", out);
        fputc('\n', out);
    }

    if (mod->sigs.len > 0u) fputc('\n', out);
    for (uint32_t i = 0u; i < mod->sigs.len; ++i) {
        const PirSigDef *s = &mod->sigs.data[i];
        fputs("sig ", out);
        print_global_sym(s->name, out);
        fputs(" = ", out);
        print_param_list(mod, s->params, s->param_count, s->is_variadic, out);
        fputs(" -> ", out);
        pir_print_type(mod, s->ret_type, out);
        print_fn_cc_attrs(mod, s->cc, s->attrs, NULL, &s->cabi_ret, out);
        fputc('\n', out);
    }

    if (mod->globals.len > 0u) fputc('\n', out);
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        const PirGlobal *g = &mod->globals.data[i];
        print_linkage(g->linkage, g->is_weak_extern, out);
        print_global_sym(g->name, out);
        fputs(" = ", out);
        switch (g->seg) {
            case PIR_GSEG_RODATA: fputs("data", out);   break;
            case PIR_GSEG_DATA:   fputs("global", out); break;
            case PIR_GSEG_TLS:    fputs("tls", out);    break;
        }
        if (g->section) {
            fputs(" section ", out);
            print_escaped_string((const uint8_t *)g->section, strlen(g->section), out);
        }
        if (g->has_type) {
            fputc(' ', out);
            pir_print_type(mod, g->type, out);
            if (g->is_zeroinit) {
                fputs(" = zeroinit", out);
            } else if (g->is_braced) {
                fputs(" = { ", out);
                for (uint32_t k = 0u; k < g->item_count; ++k) {
                    if (k > 0u) fputs(", ", out);
                    print_global_item(&g->items[k], g->type, out);
                }
                fputs(" }", out);
            } else if (g->item_count == 1u) {
                fputs(" = ", out);
                print_global_item(&g->items[0], g->type, out);
            }
            if (g->align > 0u) fprintf(out, ", align %u", g->align);
        } else if (g->is_zeroinit && g->item_count == 1u && g->items[0].kind == PIR_GITEM_ZERO) {
            fprintf(out, " [%" PRIu64 "] = zeroinit", g->items[0].len);
            if (g->align > 0u) fprintf(out, ", align %u", g->align);
        } else {
            if (g->align > 0u) fprintf(out, " align %u", g->align);
            fputs(" { ", out);
            for (uint32_t k = 0u; k < g->item_count; ++k) {
                if (k > 0u) fputs(", ", out);
                print_global_item(&g->items[k], pir_type_scalar(PIR_SCALAR_F64), out);
            }
            fputs(" }", out);
        }
        fputc('\n', out);
    }

    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        const PirFunction *fn = &mod->funcs.data[i];
        fputc('\n', out);
        print_linkage(fn->linkage, fn->is_weak_extern, out);
        fputs("func ", out);
        print_global_sym(fn->name, out);
        print_param_list(mod, fn->params, fn->param_count, fn->is_variadic, out);
        fputs(" -> ", out);
        pir_print_type(mod, fn->ret_type, out);
        print_fn_cc_attrs(mod, fn->cc, fn->attrs, fn->section, &fn->cabi_ret, out);
        if (!fn->is_defined) {
            fputc('\n', out);
            continue;
        }
        fputs(" {\n", out);
        for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
            const PirBlock *blk = &fn->blocks.data[b];
            if (b > 0u) fputc('\n', out);
            fprintf(out, ".%s", blk->label);
            if (blk->params.len > 0u) {
                fputc('(', out);
                for (uint32_t k = 0u; k < blk->params.len; ++k) {
                    if (k > 0u) fputs(", ", out);
                    const PirValueDef *vd = &fn->values.data[blk->params.data[k]];
                    fprintf(out, "%%%s: ", vd->name);
                    pir_print_type(mod, vd->type, out);
                }
                fputc(')', out);
            }
            fputs(":\n", out);
            uint32_t last_loc = 0u;
            for (uint32_t j = 0u; j < blk->insts.len; ++j) {
                const PirInst *inst = &fn->insts.data[blk->insts.data[j]];
                if (inst->loc_idx != 0u && inst->loc_idx != last_loc) {
                    const PirSourceLoc *loc = &fn->locs.data[inst->loc_idx];
                    fprintf(out, "    loc %u, %u, %u\n", loc->file_id, loc->line, loc->col);
                    last_loc = inst->loc_idx;
                }
                print_inst(mod, fn, inst, out);
            }
        }
        fputs("}\n", out);
    }
}
