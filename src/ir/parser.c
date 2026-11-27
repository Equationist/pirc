/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ir/parser.h"
#include "ir/lexer.h"
#include "analysis/cfg.h"
#include "analysis/dom.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Pending Fixup Records (for forward block/value/call/sig refs)
 * ========================================================================= */

typedef struct {
    PirFuncId   func_idx;
    PirInstId   inst_idx;
    int         which_target; /* 0, 1, or >= 2 for switch_cases[which-2] */
    const char *label;
    uint32_t    line;
} PendingTarget;

typedef struct {
    PirFuncId   func_idx;
    PirBlockId  use_block;
    PirInstId   inst_idx;
    int         which_target; /* -1 = inst->operands, else target args */
    uint32_t    op_idx;
    const char *name;
    uint32_t    line;
} PendingValRef;

typedef struct {
    PirFuncId   func_idx;
    PirInstId   inst_idx;
    const char *sig_name;
    uint32_t    line;
} PendingSigRef;

typedef struct {
    PirLexer   lex;
    PirToken   cur;
    PirModule *mod;
    PirDiag   *diag;
    bool      *agg_defined;     /* Dynamically sized bitmap of defined named aggregates */
    uint32_t   agg_defined_cap;
    uint32_t   type_depth;
    PIR_VEC(PendingTarget) pending_targets;
    PIR_VEC(PendingValRef) pending_vals;
    PIR_VEC(PendingSigRef) pending_sigs;
} Parser;

static bool is_agg_defined(const Parser *p, PirTypeId id) {
    return id < p->agg_defined_cap && p->agg_defined[id];
}

static void mark_agg_defined(Parser *p, PirTypeId id) {
    if (id >= p->agg_defined_cap) {
        uint32_t ncap = p->agg_defined_cap ? p->agg_defined_cap : 64u;
        while (ncap <= id) ncap *= 2u;
        bool *nb = (bool *)realloc(p->agg_defined, (size_t)ncap * sizeof(bool));
        if (!nb) abort();
        memset(nb + p->agg_defined_cap, 0, (size_t)(ncap - p->agg_defined_cap) * sizeof(bool));
        p->agg_defined = nb;
        p->agg_defined_cap = ncap;
    }
    p->agg_defined[id] = true;
}

static void advance_tok(Parser *p) {
    p->cur = pir_lexer_next(&p->lex);
}

static void skip_newlines(Parser *p) {
    while (p->cur.kind == TOK_NEWLINE) {
        advance_tok(p);
    }
}

static bool match_tok(Parser *p, PirTokKind k) {
    if (p->cur.kind == k) {
        advance_tok(p);
        return true;
    }
    return false;
}

static bool expect_tok(Parser *p, PirTokKind k, const char *what) {
    if (p->cur.kind == k) {
        advance_tok(p);
        return true;
    }
    pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected %s", what);
    return false;
}

static bool is_ident(const Parser *p, const char *s) {
    return p->cur.kind == TOK_IDENT && p->cur.text && strcmp(p->cur.text, s) == 0;
}

static uint64_t encode_float_bits_tok(double v, float vf, bool has_f32, PirType ty) {
    PirScalarType sc = (ty.kind == PIR_TYPE_VECTOR)
                       ? pir_vec_elem_scalar(ty.u.vec)
                       : (ty.kind == PIR_TYPE_SCALAR ? ty.u.scalar : PIR_SCALAR_F64);
    if (sc == PIR_SCALAR_F32) {
        float f = has_f32 ? vf : (float)v;
        uint32_t bits = 0u;
        memcpy(&bits, &f, sizeof(bits));
        return (uint64_t)bits;
    }
    uint64_t bits = 0u;
    memcpy(&bits, &v, sizeof(bits));
    return bits;
}

static uint64_t encode_float_bits(double v, PirType ty) {
    return encode_float_bits_tok(v, 0.0f, false, ty);
}

/* =========================================================================
 * Type & Keyword Lookup Tables
 * ========================================================================= */

static bool parse_builtin_type_name(const char *s, PirType *out) {
    static const struct { const char *name; PirScalarType sc; } scalars[] = {
        { "void",    PIR_SCALAR_VOID   },
        { "i8",      PIR_SCALAR_I8     },
        { "i16",     PIR_SCALAR_I16    },
        { "i32",     PIR_SCALAR_I32    },
        { "i64",     PIR_SCALAR_I64    },
        { "i128",    PIR_SCALAR_I128   },
        { "f32",     PIR_SCALAR_F32    },
        { "f64",     PIR_SCALAR_F64    },
        { "ptr",     PIR_SCALAR_PTR    },
        { "va_list", PIR_SCALAR_VALIST }
    };
    for (size_t i = 0u; i < sizeof(scalars) / sizeof(scalars[0]); ++i) {
        if (strcmp(s, scalars[i].name) == 0) {
            *out = pir_type_scalar(scalars[i].sc);
            return true;
        }
    }
    static const struct { const char *name; PirVecType vec; } vecs[] = {
        { "v8i8",   PIR_VEC_V8I8   }, { "v4i16",  PIR_VEC_V4I16  },
        { "v2i32",  PIR_VEC_V2I32  }, { "v2f32",  PIR_VEC_V2F32  },
        { "v16i8",  PIR_VEC_V16I8  }, { "v8i16",  PIR_VEC_V8I16  },
        { "v4i32",  PIR_VEC_V4I32  }, { "v2i64",  PIR_VEC_V2I64  },
        { "v4f32",  PIR_VEC_V4F32  }, { "v2f64",  PIR_VEC_V2F64  },
        { "v32i8",  PIR_VEC_V32I8  }, { "v16i16", PIR_VEC_V16I16 },
        { "v8i32",  PIR_VEC_V8I32  }, { "v4i64",  PIR_VEC_V4I64  },
        { "v8f32",  PIR_VEC_V8F32  }, { "v4f64",  PIR_VEC_V4F64  }
    };
    for (size_t i = 0u; i < sizeof(vecs) / sizeof(vecs[0]); ++i) {
        if (strcmp(s, vecs[i].name) == 0) {
            *out = pir_type_vec(vecs[i].vec);
            return true;
        }
    }
    return false;
}

static PirTypeId intern_named_agg(Parser *p, const char *name, uint32_t line) {
    for (uint32_t i = 0u; i < p->mod->agg_types.len; ++i) {
        if (p->mod->agg_types.data[i].name && strcmp(p->mod->agg_types.data[i].name, name) == 0) {
            return i;
        }
    }
    PirAggDef def;
    memset(&def, 0, sizeof(def));
    def.name     = name;
    def.src_line = line;
    PirTypeId id = p->mod->agg_types.len;
    PIR_VEC_PUSH(&p->mod->agg_types, def);
    return id;
}

static PirTypeId intern_anon_array(Parser *p, uint64_t len, PirType elem, uint32_t line) {
    for (uint32_t i = 0u; i < p->mod->agg_types.len; ++i) {
        const PirAggDef *d = &p->mod->agg_types.data[i];
        if (!d->name && d->kind == PIR_AGG_ARRAY && d->array_len == len &&
            pir_type_eq(d->elem_type, elem)) {
            return i;
        }
    }
    PirAggDef def;
    memset(&def, 0, sizeof(def));
    def.name      = NULL;
    def.src_line  = line;
    def.kind      = PIR_AGG_ARRAY;
    def.elem_type = elem;
    def.array_len = len;
    PirTypeId id  = p->mod->agg_types.len;
    PIR_VEC_PUSH(&p->mod->agg_types, def);
    mark_agg_defined(p, id);
    return id;
}

static PirTypeId intern_anon_pad(Parser *p, uint64_t len, uint32_t line) {
    for (uint32_t i = 0u; i < p->mod->agg_types.len; ++i) {
        const PirAggDef *d = &p->mod->agg_types.data[i];
        if (!d->name && d->kind == PIR_AGG_PAD && d->array_len == len) {
            return i;
        }
    }
    PirAggDef def;
    memset(&def, 0, sizeof(def));
    def.name      = NULL;
    def.src_line  = line;
    def.kind      = PIR_AGG_PAD;
    def.array_len = len;
    PirTypeId id  = p->mod->agg_types.len;
    PIR_VEC_PUSH(&p->mod->agg_types, def);
    mark_agg_defined(p, id);
    return id;
}

static bool parse_type(Parser *p, PirType *out) {
    if (p->type_depth >= 256u) {
        pir_diag_error(p->diag, p->cur.line, p->cur.col, "type nesting depth exceeds limit (256)");
        return false;
    }
    p->type_depth++;
    bool ok = false;
    if (p->cur.kind == TOK_IDENT) {
        if (strcmp(p->cur.text, "pad") == 0) {
            uint32_t line = p->cur.line;
            advance_tok(p);
            if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u || p->cur.int_lo == 0u) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected positive integer byte count after 'pad'");
                goto done;
            }
            uint64_t plen = p->cur.int_lo;
            advance_tok(p);
            *out = pir_type_agg(intern_anon_pad(p, plen, line));
            ok = true;
            goto done;
        }
        if (parse_builtin_type_name(p->cur.text, out)) {
            advance_tok(p);
            ok = true;
            goto done;
        }
        pir_diag_error(p->diag, p->cur.line, p->cur.col, "unknown type '%s'", p->cur.text);
        goto done;
    }
    if (p->cur.kind == TOK_GLOBAL) {
        PirTypeId id = intern_named_agg(p, p->cur.text, p->cur.line);
        advance_tok(p);
        *out = pir_type_agg(id);
        ok = true;
        goto done;
    }
    if (p->cur.kind == TOK_LBRACKET) {
        uint32_t line = p->cur.line;
        advance_tok(p);
        if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u) {
            pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected array length integer");
            goto done;
        }
        uint64_t alen = p->cur.int_lo;
        advance_tok(p);
        if (!is_ident(p, "x")) {
            pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'x' in array type");
            goto done;
        }
        advance_tok(p);
        PirType elem;
        if (!parse_type(p, &elem)) {
            goto done;
        }
        if (!expect_tok(p, TOK_RBRACKET, "']'")) {
            goto done;
        }
        *out = pir_type_agg(intern_anon_array(p, alen, elem, line));
        ok = true;
        goto done;
    }
    pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected type");
done:
    p->type_depth--;
    return ok;
}

static bool parse_opcode_name(const char *s, PirOpcode *out) {
    static const struct { const char *name; PirOpcode op; } tbl[] = {
        { "const",           PIR_OP_CONST           },
        { "addr",            PIR_OP_ADDR            },
        { "add",             PIR_OP_ADD             },
        { "sub",             PIR_OP_SUB             },
        { "mul",             PIR_OP_MUL             },
        { "div_s",           PIR_OP_DIV_S           },
        { "div_u",           PIR_OP_DIV_U           },
        { "rem_s",           PIR_OP_REM_S           },
        { "rem_u",           PIR_OP_REM_U           },
        { "add_ovf_s",       PIR_OP_ADD_OVF_S       },
        { "add_ovf_u",       PIR_OP_ADD_OVF_U       },
        { "sub_ovf_s",       PIR_OP_SUB_OVF_S       },
        { "sub_ovf_u",       PIR_OP_SUB_OVF_U       },
        { "mul_ovf_s",       PIR_OP_MUL_OVF_S       },
        { "mul_ovf_u",       PIR_OP_MUL_OVF_U       },
        { "mul_wide_u",      PIR_OP_MUL_WIDE_U      },
        { "mul_wide_s",      PIR_OP_MUL_WIDE_S      },
        { "add_carry",       PIR_OP_ADD_CARRY       },
        { "sub_borrow",      PIR_OP_SUB_BORROW      },
        { "and",             PIR_OP_AND             },
        { "or",              PIR_OP_OR              },
        { "xor",             PIR_OP_XOR             },
        { "shl",             PIR_OP_SHL             },
        { "shr",             PIR_OP_SHR             },
        { "sar",             PIR_OP_SAR             },
        { "clz",             PIR_OP_CLZ             },
        { "ctz",             PIR_OP_CTZ             },
        { "popcnt",          PIR_OP_POPCNT          },
        { "bswap",           PIR_OP_BSWAP           },
        { "fadd",            PIR_OP_FADD            },
        { "fsub",            PIR_OP_FSUB            },
        { "fmul",            PIR_OP_FMUL            },
        { "fdiv",            PIR_OP_FDIV            },
        { "fneg",            PIR_OP_FNEG            },
        { "cmp_eq",          PIR_OP_CMP_EQ          },
        { "cmp_ne",          PIR_OP_CMP_NE          },
        { "cmp_lt_s",        PIR_OP_CMP_LT_S        },
        { "cmp_lt_u",        PIR_OP_CMP_LT_U        },
        { "cmp_le_s",        PIR_OP_CMP_LE_S        },
        { "cmp_le_u",        PIR_OP_CMP_LE_U        },
        { "cmp_gt_s",        PIR_OP_CMP_GT_S        },
        { "cmp_gt_u",        PIR_OP_CMP_GT_U        },
        { "cmp_ge_s",        PIR_OP_CMP_GE_S        },
        { "cmp_ge_u",        PIR_OP_CMP_GE_U        },
        { "fcmp_oeq",        PIR_OP_FCMP_OEQ        },
        { "fcmp_one",        PIR_OP_FCMP_ONE        },
        { "fcmp_olt",        PIR_OP_FCMP_OLT        },
        { "fcmp_ole",        PIR_OP_FCMP_OLE        },
        { "fcmp_ogt",        PIR_OP_FCMP_OGT        },
        { "fcmp_oge",        PIR_OP_FCMP_OGE        },
        { "fcmp_ueq",        PIR_OP_FCMP_UEQ        },
        { "fcmp_une",        PIR_OP_FCMP_UNE        },
        { "fcmp_ult",        PIR_OP_FCMP_ULT        },
        { "fcmp_ule",        PIR_OP_FCMP_ULE        },
        { "fcmp_ugt",        PIR_OP_FCMP_UGT        },
        { "fcmp_uge",        PIR_OP_FCMP_UGE        },
        { "select",          PIR_OP_SELECT          },
        { "zext",            PIR_OP_ZEXT            },
        { "sext",            PIR_OP_SEXT            },
        { "trunc",           PIR_OP_TRUNC           },
        { "fptosi",          PIR_OP_FPTOSI          },
        { "fptoui",          PIR_OP_FPTOUI          },
        { "sitofp",          PIR_OP_SITOFP          },
        { "uitofp",          PIR_OP_UITOFP          },
        { "fpext",           PIR_OP_FPEXT           },
        { "fptrunc",         PIR_OP_FPTRUNC         },
        { "bitcast",         PIR_OP_BITCAST         },
        { "ptrtoint",        PIR_OP_PTRTOINT        },
        { "inttoptr",        PIR_OP_INTTOPTR        },
        { "splat",           PIR_OP_SPLAT           },
        { "extract",         PIR_OP_EXTRACT         },
        { "insert",          PIR_OP_INSERT          },
        { "shuffle",         PIR_OP_SHUFFLE         },
        { "alloca",          PIR_OP_ALLOCA          },
        { "alloca_dyn",      PIR_OP_ALLOCA_DYN      },
        { "stacksave",       PIR_OP_STACKSAVE       },
        { "stackrestore",    PIR_OP_STACKRESTORE    },
        { "field_ptr",       PIR_OP_FIELD_PTR       },
        { "elem_ptr",        PIR_OP_ELEM_PTR        },
        { "ptr_add",         PIR_OP_PTR_ADD         },
        { "load",            PIR_OP_LOAD            },
        { "store",           PIR_OP_STORE           },
        { "memcpy",          PIR_OP_MEMCPY          },
        { "memmove",         PIR_OP_MEMMOVE         },
        { "memset",          PIR_OP_MEMSET          },
        { "atomic_load",     PIR_OP_ATOMIC_LOAD     },
        { "atomic_store",    PIR_OP_ATOMIC_STORE    },
        { "atomic_rmw",      PIR_OP_ATOMIC_RMW      },
        { "atomic_cmpxchg",  PIR_OP_ATOMIC_CMPXCHG  },
        { "fence",           PIR_OP_FENCE           },
        { "va_start",        PIR_OP_VA_START        },
        { "va_arg",          PIR_OP_VA_ARG          },
        { "va_arg_buf",      PIR_OP_VA_ARG_BUF      },
        { "va_copy",         PIR_OP_VA_COPY         },
        { "va_end",          PIR_OP_VA_END          },
        { "call",            PIR_OP_CALL            },
        { "call_indirect",   PIR_OP_CALL_INDIRECT   },
        { "jmp",             PIR_OP_JMP             },
        { "br",              PIR_OP_BR              },
        { "switch",          PIR_OP_SWITCH          },
        { "invoke",          PIR_OP_INVOKE          },
        { "invoke_indirect", PIR_OP_INVOKE_INDIRECT },
        { "resume",          PIR_OP_RESUME          },
        { "ret",             PIR_OP_RET             },
        { "trap",            PIR_OP_TRAP            },
        { "unreachable",     PIR_OP_UNREACHABLE     }
    };
    for (size_t i = 0u; i < sizeof(tbl) / sizeof(tbl[0]); ++i) {
        if (strcmp(s, tbl[i].name) == 0) {
            *out = tbl[i].op;
            return true;
        }
    }
    return false;
}

static bool parse_mem_order(const char *s, PirMemOrder *out) {
    if (strcmp(s, "relaxed") == 0) { *out = PIR_ORDER_RELAXED; return true; }
    if (strcmp(s, "acquire") == 0) { *out = PIR_ORDER_ACQUIRE; return true; }
    if (strcmp(s, "release") == 0) { *out = PIR_ORDER_RELEASE; return true; }
    if (strcmp(s, "acq_rel") == 0) { *out = PIR_ORDER_ACQ_REL; return true; }
    if (strcmp(s, "seq_cst") == 0) { *out = PIR_ORDER_SEQ_CST; return true; }
    return false;
}

static bool parse_rmw_op(const char *s, PirRmwOp *out) {
    if (strcmp(s, "add")  == 0) { *out = PIR_RMW_ADD;  return true; }
    if (strcmp(s, "sub")  == 0) { *out = PIR_RMW_SUB;  return true; }
    if (strcmp(s, "and")  == 0) { *out = PIR_RMW_AND;  return true; }
    if (strcmp(s, "or")   == 0) { *out = PIR_RMW_OR;   return true; }
    if (strcmp(s, "xor")  == 0) { *out = PIR_RMW_XOR;  return true; }
    if (strcmp(s, "xchg") == 0) { *out = PIR_RMW_XCHG; return true; }
    return false;
}

static bool parse_sem_flag(const char *s, uint32_t *out) {
    static const struct { const char *name; uint32_t bit; } tbl[] = {
        { "wrap",     PIR_FLAG_WRAP     },
        { "trap",     PIR_FLAG_TRAP     },
        { "trap_s",   PIR_FLAG_TRAP_S   },
        { "trap_u",   PIR_FLAG_TRAP_U   },
        { "unsafe",   PIR_FLAG_UNSAFE   },
        { "unsafe_s", PIR_FLAG_UNSAFE_S },
        { "unsafe_u", PIR_FLAG_UNSAFE_U },
        { "mask",     PIR_FLAG_MASK     },
        { "fast",     PIR_FLAG_FAST     },
        { "sat",      PIR_FLAG_SAT      },
        { "inbounds", PIR_FLAG_INBOUNDS },
        { "volatile", PIR_FLAG_VOLATILE },
        { "weak",     PIR_FLAG_WEAK     },
        { "nounwind", PIR_FLAG_NOUNWIND },
        { "nonzero",  PIR_FLAG_NONZERO  }
    };
    for (size_t i = 0u; i < sizeof(tbl) / sizeof(tbl[0]); ++i) {
        if (strcmp(s, tbl[i].name) == 0) {
            *out = tbl[i].bit;
            return true;
        }
    }
    return false;
}

/* =========================================================================
 * Operands, Flags & Branch Targets
 * ========================================================================= */

static bool parse_flags_brackets(Parser *p, uint32_t *flags_out) {
    while (p->cur.kind == TOK_LBRACKET) {
        advance_tok(p);
        do {
            if (p->cur.kind != TOK_IDENT) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected flag identifier");
                return false;
            }
            uint32_t bit = 0u;
            if (!parse_sem_flag(p->cur.text, &bit)) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "unknown flag '%s'", p->cur.text);
                return false;
            }
            if ((*flags_out & bit) != 0u) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate flag '%s'", p->cur.text);
                return false;
            }
            *flags_out |= bit;
            advance_tok(p);
        } while (match_tok(p, TOK_COMMA));
        if (!expect_tok(p, TOK_RBRACKET, "']'")) {
            return false;
        }
    }
    return true;
}

static bool parse_align_clause(Parser *p, uint32_t *align_out) {
    if (!is_ident(p, "align")) {
        pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'align'");
        return false;
    }
    advance_tok(p);
    if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u ||
        p->cur.int_lo == 0u || p->cur.int_lo > UINT32_MAX) {
        pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected non-zero integer alignment");
        return false;
    }
    *align_out = (uint32_t)p->cur.int_lo;
    advance_tok(p);
    return true;
}

static bool token_has_nul(const PirToken *t) {
    return t->len > 0u && memchr(t->text, '\0', t->len) != NULL;
}

static bool token_has_newline(const PirToken *t) {
    return t->len > 0u &&
           (memchr(t->text, '\n', t->len) != NULL || memchr(t->text, '\r', t->len) != NULL);
}

static bool parse_u64_positive(Parser *p, uint64_t *out, const char *what) {
    if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u || p->cur.int_lo == 0u) {
        pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected positive integer for %s", what);
        return false;
    }
    *out = p->cur.int_lo;
    advance_tok(p);
    return true;
}

static bool parse_u32_positive(Parser *p, uint32_t *out, const char *what) {
    uint64_t v = 0u;
    if (!parse_u64_positive(p, &v, what)) return false;
    if (v > UINT32_MAX) {
        pir_diag_error(p->diag, p->cur.line, p->cur.col, "integer out of 32-bit range for %s", what);
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool parse_operand(Parser *p, PirFuncId func_idx, PirBlockId blk_idx,
                          PirInstId inst_idx, int which_target, uint32_t op_idx,
                          bool allow_invoke_kw, bool allow_type_tag,
                          PirOperand *out) {
    memset(out, 0, sizeof(*out));
    out->type = pir_type_scalar(PIR_SCALAR_VOID);

    if (p->cur.kind == TOK_LOCAL) {
        out->kind  = PIR_OPER_VAL;
        out->u.val = PIR_INVALID_ID;
        PendingValRef ref = { func_idx, blk_idx, inst_idx, which_target, op_idx,
                              p->cur.text, p->cur.line };
        PIR_VEC_PUSH(&p->pending_vals, ref);
        advance_tok(p);
    } else if (p->cur.kind == TOK_INT) {
        out->kind                 = PIR_OPER_IMM_INT;
        out->u.imm_int.lo         = p->cur.int_lo;
        out->u.imm_int.hi         = p->cur.int_hi;
        out->u.imm_int.is_negative = p->cur.int_neg;
        advance_tok(p);
    } else if (p->cur.kind == TOK_FLOAT) {
        uint32_t f32b = 0u;
        memcpy(&f32b, &p->cur.float_val_f32, sizeof(f32b));
        out->kind                     = PIR_OPER_IMM_FLOAT;
        out->u.imm_float.raw_val      = p->cur.float_val;
        out->u.imm_float.f32_bits     = f32b;
        out->u.imm_float.has_f32_bits = true;
        out->u.imm_float.bits         = encode_float_bits(p->cur.float_val, pir_type_scalar(PIR_SCALAR_F64));
        advance_tok(p);
    } else if (p->cur.kind == TOK_GLOBAL) {
        out->kind       = PIR_OPER_SYM;
        out->type       = pir_type_scalar(PIR_SCALAR_PTR);
        out->u.sym.name = p->cur.text;
        out->u.sym.id   = PIR_INVALID_ID;
        advance_tok(p);
        if (p->cur.kind == TOK_PLUS || p->cur.kind == TOK_MINUS) {
            bool sub = (p->cur.kind == TOK_MINUS);
            advance_tok(p);
            if (p->cur.kind != TOK_INT || p->cur.int_hi != 0u) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected integer offset");
                return false;
            }
            uint64_t mag = p->cur.int_lo;
            bool neg = (sub != p->cur.int_neg);
            if (neg ? (mag > (uint64_t)INT64_MAX + 1u) : (mag > (uint64_t)INT64_MAX)) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "symbol offset out of signed 64-bit range");
                return false;
            }
            out->u.sym.offset = neg ? (int64_t)(UINT64_C(0) - mag) : (int64_t)mag;
            advance_tok(p);
        }
    } else if (p->cur.kind == TOK_IDENT && strcmp(p->cur.text, "nan") == 0) {
        double nv = (double)NAN;
        out->kind                = PIR_OPER_IMM_FLOAT;
        out->u.imm_float.raw_val = nv;
        out->u.imm_float.bits    = encode_float_bits(nv, pir_type_scalar(PIR_SCALAR_F64));
        advance_tok(p);
    } else if (p->cur.kind == TOK_IDENT && strcmp(p->cur.text, "inf") == 0) {
        double iv = HUGE_VAL;
        out->kind                = PIR_OPER_IMM_FLOAT;
        out->u.imm_float.raw_val = iv;
        out->u.imm_float.bits    = encode_float_bits(iv, pir_type_scalar(PIR_SCALAR_F64));
        advance_tok(p);
    } else if (allow_invoke_kw && is_ident(p, "ret")) {
        out->kind = PIR_OPER_INVOKE_RET;
        advance_tok(p);
    } else if (allow_invoke_kw && is_ident(p, "exn")) {
        out->kind = PIR_OPER_INVOKE_EXN;
        out->type = pir_type_scalar(PIR_SCALAR_PTR);
        advance_tok(p);
    } else {
        pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected operand");
        return false;
    }

    if (allow_type_tag && match_tok(p, TOK_COLON)) {
        out->has_explicit_type = true;
        if (is_ident(p, "byval")) {
            advance_tok(p);
            if (!expect_tok(p, TOK_LPAREN, "'('")) return false;
            out->call_abi = PIR_PARAM_BYVAL;
            if (!parse_type(p, &out->call_abi_type)) return false;
            if (!expect_tok(p, TOK_RPAREN, "')'")) return false;
            out->type = pir_type_scalar(PIR_SCALAR_PTR);
        } else {
            if (!parse_type(p, &out->type)) return false;
            if (out->kind == PIR_OPER_IMM_FLOAT) {
                float f32v = 0.0f;
                if (out->u.imm_float.has_f32_bits) {
                    memcpy(&f32v, &out->u.imm_float.f32_bits, sizeof(f32v));
                }
                out->u.imm_float.bits = encode_float_bits_tok(out->u.imm_float.raw_val, f32v,
                                                              out->u.imm_float.has_f32_bits, out->type);
            }
        }
    }
    if (allow_type_tag) {
        while (p->cur.kind == TOK_IDENT) {
            if (strcmp(p->cur.text, "byval_size") == 0) {
                if (out->call_byval_size != 0u) {
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'byval_size' attribute");
                    return false;
                }
                advance_tok(p);
                if (!parse_u64_positive(p, &out->call_byval_size, "byval_size")) return false;
            } else if (strcmp(p->cur.text, "byval_align") == 0) {
                if (out->call_byval_align != 0u) {
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'byval_align' attribute");
                    return false;
                }
                advance_tok(p);
                if (!parse_u32_positive(p, &out->call_byval_align, "byval_align")) return false;
            } else if (strcmp(p->cur.text, "stack_align") == 0) {
                if (out->call_stack_align != 0u) {
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'stack_align' attribute");
                    return false;
                }
                advance_tok(p);
                if (!parse_u32_positive(p, &out->call_stack_align, "stack_align")) return false;
            } else if (strcmp(p->cur.text, "force_stack") == 0) {
                if (out->call_force_stack) {
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'force_stack' attribute");
                    return false;
                }
                out->call_force_stack = true;
                advance_tok(p);
            } else if (strcmp(p->cur.text, "no_f32_promote") == 0) {
                if (out->call_no_f32_promote) {
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'no_f32_promote' attribute");
                    return false;
                }
                out->call_no_f32_promote = true;
                advance_tok(p);
            } else {
                break;
            }
        }
    }
    return true;
}

static bool parse_branch_target(Parser *p, PirFuncId func_idx, PirBlockId blk_idx,
                                PirInstId inst_idx, int which_target,
                                bool allow_invoke_kw, PirBranchTarget *out) {
    memset(out, 0, sizeof(*out));
    out->target = PIR_INVALID_ID;
    if (p->cur.kind != TOK_LABEL) {
        pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected block label");
        return false;
    }
    PendingTarget pt = { func_idx, inst_idx, which_target, p->cur.text, p->cur.line };
    PIR_VEC_PUSH(&p->pending_targets, pt);
    advance_tok(p);

    if (match_tok(p, TOK_LPAREN)) {
        PIR_VEC(PirOperand) args = {0};
        if (p->cur.kind != TOK_RPAREN) {
            do {
                PirOperand arg;
                if (!parse_operand(p, func_idx, blk_idx, inst_idx, which_target,
                                   args.len, allow_invoke_kw, false, &arg)) {
                    PIR_VEC_FREE(&args);
                    return false;
                }
                PIR_VEC_PUSH(&args, arg);
            } while (match_tok(p, TOK_COMMA));
        }
        if (!expect_tok(p, TOK_RPAREN, "')'")) {
            PIR_VEC_FREE(&args);
            return false;
        }
        if (args.len > 0u) {
            out->arg_count = args.len;
            out->args = PIR_ARENA_NEW_ARRAY(&p->mod->arena, PirOperand, args.len);
            memcpy(out->args, args.data, args.len * sizeof(PirOperand));
        }
        PIR_VEC_FREE(&args);
    }
    return true;
}

/* =========================================================================
 * Instruction Parsing
 * ========================================================================= */

static PirType infer_result0_type(PirOpcode op, PirType ty) {
    switch (op) {
        case PIR_OP_CMP_EQ: case PIR_OP_CMP_NE:
        case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LT_U:
        case PIR_OP_CMP_LE_S: case PIR_OP_CMP_LE_U:
        case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GT_U:
        case PIR_OP_CMP_GE_S: case PIR_OP_CMP_GE_U:
        case PIR_OP_FCMP_OEQ: case PIR_OP_FCMP_ONE: case PIR_OP_FCMP_OLT:
        case PIR_OP_FCMP_OLE: case PIR_OP_FCMP_OGT: case PIR_OP_FCMP_OGE:
        case PIR_OP_FCMP_UEQ: case PIR_OP_FCMP_UNE: case PIR_OP_FCMP_ULT:
        case PIR_OP_FCMP_ULE: case PIR_OP_FCMP_UGT: case PIR_OP_FCMP_UGE:
            if (ty.kind == PIR_TYPE_VECTOR) {
                return pir_type_vec(pir_vec_mask_type(ty.u.vec));
            }
            return pir_type_scalar(PIR_SCALAR_I8);
        case PIR_OP_EXTRACT:
            if (ty.kind == PIR_TYPE_VECTOR) {
                return pir_type_scalar(pir_vec_elem_scalar(ty.u.vec));
            }
            return ty;
        case PIR_OP_ADDR:
        case PIR_OP_INTTOPTR:
        case PIR_OP_ALLOCA:
        case PIR_OP_ALLOCA_PTR:
        case PIR_OP_ALLOCA_DYN:
        case PIR_OP_STACKSAVE:
        case PIR_OP_FIELD_PTR:
        case PIR_OP_ELEM_PTR:
        case PIR_OP_PTR_ADD:
            return pir_type_scalar(PIR_SCALAR_PTR);
        case PIR_OP_CALL:
        case PIR_OP_CALL_INDIRECT:
            return pir_type_scalar(PIR_SCALAR_VOID); /* Filled in module post-pass */
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
        case PIR_OP_SELECT:
        case PIR_OP_ZEXT: case PIR_OP_SEXT: case PIR_OP_TRUNC:
        case PIR_OP_FPTOSI: case PIR_OP_FPTOUI: case PIR_OP_SITOFP: case PIR_OP_UITOFP:
        case PIR_OP_FPEXT: case PIR_OP_FPTRUNC: case PIR_OP_BITCAST:
        case PIR_OP_PTRTOINT:
        case PIR_OP_SPLAT: case PIR_OP_INSERT: case PIR_OP_SHUFFLE:
        case PIR_OP_LOAD:
        case PIR_OP_ATOMIC_LOAD: case PIR_OP_ATOMIC_RMW: case PIR_OP_ATOMIC_CMPXCHG:
        case PIR_OP_VA_ARG:
            return ty;
        case PIR_OP_STACKRESTORE: case PIR_OP_STORE:
        case PIR_OP_MEMCPY: case PIR_OP_MEMMOVE: case PIR_OP_MEMSET:
        case PIR_OP_ATOMIC_STORE: case PIR_OP_FENCE:
        case PIR_OP_VA_START: case PIR_OP_VA_ARG_BUF: case PIR_OP_VA_COPY: case PIR_OP_VA_END:
        case PIR_OP_JMP: case PIR_OP_BR: case PIR_OP_SWITCH:
        case PIR_OP_INVOKE: case PIR_OP_INVOKE_INDIRECT: case PIR_OP_RESUME:
        case PIR_OP_RET: case PIR_OP_TRAP: case PIR_OP_UNREACHABLE:
            return pir_type_scalar(PIR_SCALAR_VOID);
    }
    return pir_type_scalar(PIR_SCALAR_VOID);
}

static PirType infer_result1_type(PirOpcode op, PirType ty) {
    if (op == PIR_OP_MUL_WIDE_U || op == PIR_OP_MUL_WIDE_S) {
        return ty;
    }
    return pir_type_scalar(PIR_SCALAR_I8);
}

static PirValueId define_value(Parser *p, PirFunction *fn, const char *name, PirType ty,
                               PirBlockId def_block, PirInstId def_inst,
                               uint32_t param_idx, uint32_t line) {
    /* Duplicate check:
     * - Duplicate function param -> error
     * - Duplicate within same block (param or inst) -> error
     * - Instruction result colliding with any func param or other inst result -> error
     * - Block param in block B sharing a name with block param in block A (A != B) is allowed
     *   (matches PIR.md Example 2 and roundtrip/03_block_args_loop.pir). */
    bool is_blk_param = (def_block != PIR_INVALID_ID && def_inst == PIR_INVALID_ID);
    for (uint32_t i = 0u; i < fn->values.len; ++i) {
        const PirValueDef *v = &fn->values.data[i];
        if (strcmp(v->name, name) != 0) continue;
        bool other_blk_param = (v->def_block != PIR_INVALID_ID && v->def_inst == PIR_INVALID_ID);
        if (is_blk_param && other_blk_param && v->def_block != def_block) {
            continue;
        }
        pir_diag_error(p->diag, line, 0u, "SSA value '%%%s' defined more than once", name);
        break;
    }
    PirValueDef vd;
    vd.name        = name;
    vd.type        = ty;
    vd.def_block   = def_block;
    vd.def_inst    = def_inst;
    vd.param_index = param_idx;
    PirValueId id  = fn->values.len;
    PIR_VEC_PUSH(&fn->values, vd);
    return id;
}

static void assign_literal_type(PirOperand *op, PirType ty) {
    if (op->has_explicit_type) {
        return;
    }
    if (op->kind == PIR_OPER_IMM_INT) {
        if (op->type.kind == PIR_TYPE_SCALAR && op->type.u.scalar == PIR_SCALAR_VOID) {
            op->type = ty;
        }
    } else if (op->kind == PIR_OPER_IMM_FLOAT) {
        if (op->type.kind == PIR_TYPE_SCALAR && op->type.u.scalar == PIR_SCALAR_VOID) {
            op->type = ty;
        }
        float f32v = 0.0f;
        if (op->u.imm_float.has_f32_bits) {
            memcpy(&f32v, &op->u.imm_float.f32_bits, sizeof(f32v));
        }
        op->u.imm_float.bits = encode_float_bits_tok(op->u.imm_float.raw_val, f32v,
                                                     op->u.imm_float.has_f32_bits, op->type);
    }
}

typedef PIR_VEC(PirOperand) PirOperandVec;

static bool parse_call_args(Parser *p, PirFuncId func_idx, PirBlockId blk_idx,
                            PirInstId inst_idx, PirOperandVec *ops) {
    if (!expect_tok(p, TOK_LPAREN, "'('")) return false;
    if (p->cur.kind != TOK_RPAREN) {
        do {
            PirOperand arg;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, ops->len,
                               false, true, &arg)) {
                return false;
            }
            PIR_VEC_PUSH(ops, arg);
        } while (match_tok(p, TOK_COMMA));
    }
    return expect_tok(p, TOK_RPAREN, "')'");
}

static bool parse_instruction(Parser *p, PirFuncId func_idx, PirBlockId blk_idx,
                              uint32_t current_loc_idx) {
    PirFunction *fn = &p->mod->funcs.data[func_idx];
    uint32_t line = p->cur.line;
    const char *res_names[2] = { NULL, NULL };
    uint32_t res_count = 0u;

    if (p->cur.kind == TOK_LOCAL) {
        res_names[0] = p->cur.text;
        res_count    = 1u;
        advance_tok(p);
        if (match_tok(p, TOK_COMMA)) {
            if (p->cur.kind != TOK_LOCAL) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected second result register");
                return false;
            }
            res_names[1] = p->cur.text;
            res_count    = 2u;
            advance_tok(p);
        }
        if (!expect_tok(p, TOK_EQ, "'='")) return false;
    }

    if (p->cur.kind != TOK_IDENT) {
        pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected opcode");
        return false;
    }
    const char *op_name = p->cur.text;
    uint32_t op_col = p->cur.col;
    PirOpcode op = PIR_OP_CONST;
    if (!parse_opcode_name(op_name, &op)) {
        pir_diag_error(p->diag, line, op_col, "unknown opcode '%s'", op_name);
        return false;
    }
    advance_tok(p);

    PirInst inst;
    memset(&inst, 0, sizeof(inst));
    inst.op          = op;
    inst.ty          = pir_type_scalar(PIR_SCALAR_VOID);
    inst.aux_type    = pir_type_scalar(PIR_SCALAR_VOID);
    inst.results[0]  = PIR_INVALID_ID;
    inst.results[1]  = PIR_INVALID_ID;
    inst.sig_id      = PIR_INVALID_ID;
    inst.targets[0].target = PIR_INVALID_ID;
    inst.targets[1].target = PIR_INVALID_ID;
    inst.loc_idx     = current_loc_idx;
    inst.src_line    = line;

    /* Handle attached suffixes: `.op` for atomic_rmw, `.ptr` for alloca.ptr, `.type` otherwise. */
    if (op == PIR_OP_ATOMIC_RMW) {
        if (p->cur.kind != TOK_LABEL || !p->cur.attached ||
            !parse_rmw_op(p->cur.text, &inst.rmw_op)) {
            pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected atomic_rmw sub-op (.add/.sub/.and/.or/.xor/.xchg)");
            return false;
        }
        advance_tok(p);
    }
    bool had_type_suffix = false;
    if (op == PIR_OP_ALLOCA && p->cur.kind == TOK_LABEL && p->cur.attached &&
        strcmp(p->cur.text, "ptr") == 0) {
        inst.op = PIR_OP_ALLOCA_PTR;
        inst.ty = pir_type_scalar(PIR_SCALAR_PTR);
        advance_tok(p);
    } else if (p->cur.kind == TOK_LABEL && p->cur.attached) {
        if (!parse_builtin_type_name(p->cur.text, &inst.ty)) {
            pir_diag_error(p->diag, p->cur.line, p->cur.col, "invalid opcode type suffix '.%s'", p->cur.text);
            return false;
        }
        had_type_suffix = true;
        advance_tok(p);
    }
    if (had_type_suffix) {
        bool forbids_suffix = false;
        switch (inst.op) {
            case PIR_OP_FENCE:
            case PIR_OP_JMP:
            case PIR_OP_BR:
            case PIR_OP_TRAP:
            case PIR_OP_UNREACHABLE:
            case PIR_OP_RESUME:
            case PIR_OP_RET:
            case PIR_OP_ADDR:
            case PIR_OP_ALLOCA:
            case PIR_OP_ALLOCA_DYN:
            case PIR_OP_STACKSAVE:
            case PIR_OP_STACKRESTORE:
            case PIR_OP_FIELD_PTR:
            case PIR_OP_ELEM_PTR:
            case PIR_OP_MEMCPY:
            case PIR_OP_MEMMOVE:
            case PIR_OP_MEMSET:
            case PIR_OP_VA_START:
            case PIR_OP_VA_ARG_BUF:
            case PIR_OP_VA_COPY:
            case PIR_OP_VA_END:
                forbids_suffix = true;
                break;
            case PIR_OP_CONST:
            case PIR_OP_ADD:
            case PIR_OP_SUB:
            case PIR_OP_MUL:
            case PIR_OP_DIV_S:
            case PIR_OP_DIV_U:
            case PIR_OP_REM_S:
            case PIR_OP_REM_U:
            case PIR_OP_AND:
            case PIR_OP_OR:
            case PIR_OP_XOR:
            case PIR_OP_SHL:
            case PIR_OP_SHR:
            case PIR_OP_SAR:
            case PIR_OP_ADD_OVF_S:
            case PIR_OP_ADD_OVF_U:
            case PIR_OP_SUB_OVF_S:
            case PIR_OP_SUB_OVF_U:
            case PIR_OP_MUL_OVF_S:
            case PIR_OP_MUL_OVF_U:
            case PIR_OP_ADD_CARRY:
            case PIR_OP_SUB_BORROW:
            case PIR_OP_MUL_WIDE_S:
            case PIR_OP_MUL_WIDE_U:
            case PIR_OP_CLZ:
            case PIR_OP_CTZ:
            case PIR_OP_POPCNT:
            case PIR_OP_BSWAP:
            case PIR_OP_FADD:
            case PIR_OP_FSUB:
            case PIR_OP_FMUL:
            case PIR_OP_FDIV:
            case PIR_OP_FNEG:
            case PIR_OP_CMP_EQ:
            case PIR_OP_CMP_NE:
            case PIR_OP_CMP_LT_S:
            case PIR_OP_CMP_LE_S:
            case PIR_OP_CMP_GT_S:
            case PIR_OP_CMP_GE_S:
            case PIR_OP_CMP_LT_U:
            case PIR_OP_CMP_LE_U:
            case PIR_OP_CMP_GT_U:
            case PIR_OP_CMP_GE_U:
            case PIR_OP_FCMP_OEQ:
            case PIR_OP_FCMP_ONE:
            case PIR_OP_FCMP_OLT:
            case PIR_OP_FCMP_OLE:
            case PIR_OP_FCMP_OGT:
            case PIR_OP_FCMP_OGE:
            case PIR_OP_FCMP_UEQ:
            case PIR_OP_FCMP_UNE:
            case PIR_OP_FCMP_ULT:
            case PIR_OP_FCMP_ULE:
            case PIR_OP_FCMP_UGT:
            case PIR_OP_FCMP_UGE:
            case PIR_OP_TRUNC:
            case PIR_OP_SEXT:
            case PIR_OP_ZEXT:
            case PIR_OP_FPTRUNC:
            case PIR_OP_FPEXT:
            case PIR_OP_SITOFP:
            case PIR_OP_UITOFP:
            case PIR_OP_FPTOSI:
            case PIR_OP_FPTOUI:
            case PIR_OP_PTRTOINT:
            case PIR_OP_INTTOPTR:
            case PIR_OP_BITCAST:
            case PIR_OP_SPLAT:
            case PIR_OP_EXTRACT:
            case PIR_OP_INSERT:
            case PIR_OP_SHUFFLE:
            case PIR_OP_SELECT:
            case PIR_OP_ALLOCA_PTR:
            case PIR_OP_PTR_ADD:
            case PIR_OP_LOAD:
            case PIR_OP_STORE:
            case PIR_OP_ATOMIC_LOAD:
            case PIR_OP_ATOMIC_STORE:
            case PIR_OP_ATOMIC_RMW:
            case PIR_OP_ATOMIC_CMPXCHG:
            case PIR_OP_VA_ARG:
            case PIR_OP_SWITCH:
                break;
            case PIR_OP_CALL:
            case PIR_OP_CALL_INDIRECT:
            case PIR_OP_INVOKE:
            case PIR_OP_INVOKE_INDIRECT:
                forbids_suffix = true;
                break;
        }
        if (forbids_suffix) {
            pir_diag_error(p->diag, line, 0u, "opcode does not take a type suffix");
            return false;
        }
    }

    PirInstId inst_idx = fn->insts.len;
    PirOperandVec ops = {0};

    switch (inst.op) {
        case PIR_OP_ALLOCA: {
            inst.ty = pir_type_scalar(PIR_SCALAR_PTR);
            if (!parse_type(p, &inst.aux_type)) goto fail;
            if (match_tok(p, TOK_COMMA)) {
                if (!parse_align_clause(p, &inst.align)) goto fail;
            }
            break;
        }
        case PIR_OP_ALLOCA_PTR: {
            if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected byte count for alloca.ptr");
                goto fail;
            }
            inst.imm_u64 = p->cur.int_lo;
            advance_tok(p);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_align_clause(p, &inst.align)) goto fail;
            break;
        }
        case PIR_OP_ALLOCA_DYN: {
            inst.ty = pir_type_scalar(PIR_SCALAR_PTR);
            PirOperand sz;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &sz)) goto fail;
            assign_literal_type(&sz, pir_type_scalar(PIR_SCALAR_I64));
            PIR_VEC_PUSH(&ops, sz);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_align_clause(p, &inst.align)) goto fail;
            break;
        }
        case PIR_OP_STACKSAVE:
            inst.ty = pir_type_scalar(PIR_SCALAR_PTR);
            break;
        case PIR_OP_STACKRESTORE: {
            inst.ty = pir_type_scalar(PIR_SCALAR_PTR);
            PirOperand sp;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &sp)) goto fail;
            assign_literal_type(&sp, pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, sp);
            break;
        }
        case PIR_OP_FIELD_PTR: {
            inst.ty = pir_type_scalar(PIR_SCALAR_PTR);
            if (!parse_type(p, &inst.aux_type)) goto fail;
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            PirOperand base;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &base)) goto fail;
            assign_literal_type(&base, pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, base);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected constant field index");
                goto fail;
            }
            inst.imm_u64 = p->cur.int_lo;
            advance_tok(p);
            break;
        }
        case PIR_OP_ELEM_PTR: {
            inst.ty = pir_type_scalar(PIR_SCALAR_PTR);
            if (!parse_type(p, &inst.aux_type)) goto fail;
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            PirOperand base, idx;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &base)) goto fail;
            assign_literal_type(&base, pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, base);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 1u, false, false, &idx)) goto fail;
            assign_literal_type(&idx, pir_type_scalar(PIR_SCALAR_I64));
            PIR_VEC_PUSH(&ops, idx);
            break;
        }
        case PIR_OP_LOAD: {
            PirOperand ptr;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &ptr)) goto fail;
            assign_literal_type(&ptr, pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, ptr);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_align_clause(p, &inst.align)) goto fail;
            if (!parse_flags_brackets(p, &inst.flags)) goto fail;
            break;
        }
        case PIR_OP_STORE: {
            PirOperand val, ptr;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &val)) goto fail;
            assign_literal_type(&val, inst.ty);
            PIR_VEC_PUSH(&ops, val);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 1u, false, false, &ptr)) goto fail;
            assign_literal_type(&ptr, pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, ptr);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_align_clause(p, &inst.align)) goto fail;
            if (!parse_flags_brackets(p, &inst.flags)) goto fail;
            break;
        }
        case PIR_OP_MEMCPY:
        case PIR_OP_MEMMOVE:
        case PIR_OP_MEMSET: {
            PirOperand a0, a1, a2;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &a0)) goto fail;
            assign_literal_type(&a0, pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, a0);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 1u, false, false, &a1)) goto fail;
            assign_literal_type(&a1, inst.op == PIR_OP_MEMSET
                                     ? pir_type_scalar(PIR_SCALAR_I8)
                                     : pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, a1);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 2u, false, false, &a2)) goto fail;
            assign_literal_type(&a2, pir_type_scalar(PIR_SCALAR_I64));
            PIR_VEC_PUSH(&ops, a2);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_align_clause(p, &inst.align)) goto fail;
            break;
        }
        case PIR_OP_ATOMIC_LOAD: {
            PirOperand ptr;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &ptr)) goto fail;
            assign_literal_type(&ptr, pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, ptr);
            if (!expect_tok(p, TOK_LBRACKET, "'['")) goto fail;
            if (p->cur.kind != TOK_IDENT || !parse_mem_order(p->cur.text, &inst.order)) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected memory ordering");
                goto fail;
            }
            advance_tok(p);
            if (!expect_tok(p, TOK_RBRACKET, "']'")) goto fail;
            break;
        }
        case PIR_OP_ATOMIC_STORE: {
            PirOperand val, ptr;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &val)) goto fail;
            assign_literal_type(&val, inst.ty);
            PIR_VEC_PUSH(&ops, val);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 1u, false, false, &ptr)) goto fail;
            assign_literal_type(&ptr, pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, ptr);
            if (!expect_tok(p, TOK_LBRACKET, "'['")) goto fail;
            if (p->cur.kind != TOK_IDENT || !parse_mem_order(p->cur.text, &inst.order)) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected memory ordering");
                goto fail;
            }
            advance_tok(p);
            if (!expect_tok(p, TOK_RBRACKET, "']'")) goto fail;
            break;
        }
        case PIR_OP_ATOMIC_RMW: {
            PirOperand ptr, val;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &ptr)) goto fail;
            assign_literal_type(&ptr, pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, ptr);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 1u, false, false, &val)) goto fail;
            assign_literal_type(&val, inst.ty);
            PIR_VEC_PUSH(&ops, val);
            if (!expect_tok(p, TOK_LBRACKET, "'['")) goto fail;
            if (p->cur.kind != TOK_IDENT || !parse_mem_order(p->cur.text, &inst.order)) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected memory ordering");
                goto fail;
            }
            advance_tok(p);
            if (!expect_tok(p, TOK_RBRACKET, "']'")) goto fail;
            break;
        }
        case PIR_OP_ATOMIC_CMPXCHG: {
            PirOperand ptr, exp_v, new_v;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &ptr)) goto fail;
            assign_literal_type(&ptr, pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, ptr);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 1u, false, false, &exp_v)) goto fail;
            assign_literal_type(&exp_v, inst.ty);
            PIR_VEC_PUSH(&ops, exp_v);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 2u, false, false, &new_v)) goto fail;
            assign_literal_type(&new_v, inst.ty);
            PIR_VEC_PUSH(&ops, new_v);
            if (!expect_tok(p, TOK_LBRACKET, "'['")) goto fail;
            if (p->cur.kind != TOK_IDENT || !parse_mem_order(p->cur.text, &inst.order)) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected success memory ordering");
                goto fail;
            }
            advance_tok(p);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (p->cur.kind != TOK_IDENT || !parse_mem_order(p->cur.text, &inst.fail_order)) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected failure memory ordering");
                goto fail;
            }
            advance_tok(p);
            if (!expect_tok(p, TOK_RBRACKET, "']'")) goto fail;
            if (!parse_flags_brackets(p, &inst.flags)) goto fail;
            break;
        }
        case PIR_OP_FENCE: {
            if (!expect_tok(p, TOK_LBRACKET, "'['")) goto fail;
            if (p->cur.kind != TOK_IDENT || !parse_mem_order(p->cur.text, &inst.order)) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected fence memory ordering");
                goto fail;
            }
            advance_tok(p);
            if (!expect_tok(p, TOK_RBRACKET, "']'")) goto fail;
            break;
        }
        case PIR_OP_VA_ARG_BUF: {
            if (!parse_type(p, &inst.aux_type)) goto fail;
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            PirOperand dst, ap;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &dst)) goto fail;
            assign_literal_type(&dst, pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, dst);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 1u, false, false, &ap)) goto fail;
            assign_literal_type(&ap, pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, ap);
            break;
        }
        case PIR_OP_EXTRACT: {
            PirOperand vec;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &vec)) goto fail;
            PIR_VEC_PUSH(&ops, vec);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected lane index");
                goto fail;
            }
            inst.imm_u64 = p->cur.int_lo;
            advance_tok(p);
            break;
        }
        case PIR_OP_INSERT: {
            PirOperand vec, sc;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &vec)) goto fail;
            PIR_VEC_PUSH(&ops, vec);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 1u, false, false, &sc)) goto fail;
            if (inst.ty.kind == PIR_TYPE_VECTOR) {
                assign_literal_type(&sc, pir_type_scalar(pir_vec_elem_scalar(inst.ty.u.vec)));
            }
            PIR_VEC_PUSH(&ops, sc);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected lane index");
                goto fail;
            }
            inst.imm_u64 = p->cur.int_lo;
            advance_tok(p);
            break;
        }
        case PIR_OP_SHUFFLE: {
            PirOperand v1, v2;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &v1)) goto fail;
            PIR_VEC_PUSH(&ops, v1);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 1u, false, false, &v2)) goto fail;
            PIR_VEC_PUSH(&ops, v2);
            if (!expect_tok(p, TOK_LBRACKET, "'['")) goto fail;
            PIR_VEC(uint8_t) mask = {0};
            do {
                if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u ||
                    p->cur.int_lo > 255u) {
                    PIR_VEC_FREE(&mask);
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected shuffle lane index");
                    goto fail;
                }
                PIR_VEC_PUSH(&mask, (uint8_t)p->cur.int_lo);
                advance_tok(p);
            } while (match_tok(p, TOK_COMMA));
            if (!expect_tok(p, TOK_RBRACKET, "']'")) {
                PIR_VEC_FREE(&mask);
                goto fail;
            }
            inst.shuffle_len  = mask.len;
            inst.shuffle_mask = PIR_ARENA_NEW_ARRAY(&p->mod->arena, uint8_t, mask.len);
            memcpy(inst.shuffle_mask, mask.data, mask.len);
            PIR_VEC_FREE(&mask);
            break;
        }
        case PIR_OP_CALL: {
            PirOperand callee;
            if (p->cur.kind != TOK_GLOBAL) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected callee @symbol");
                goto fail;
            }
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &callee)) goto fail;
            PIR_VEC_PUSH(&ops, callee);
            if (!parse_call_args(p, func_idx, blk_idx, inst_idx, &ops)) goto fail;
            if (!parse_flags_brackets(p, &inst.flags)) goto fail;
            break;
        }
        case PIR_OP_CALL_INDIRECT: {
            if (p->cur.kind != TOK_GLOBAL) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected signature @Sig");
                goto fail;
            }
            PendingSigRef sr = { func_idx, inst_idx, p->cur.text, p->cur.line };
            PIR_VEC_PUSH(&p->pending_sigs, sr);
            advance_tok(p);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            PirOperand fn_ptr;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &fn_ptr)) goto fail;
            assign_literal_type(&fn_ptr, pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, fn_ptr);
            if (!parse_call_args(p, func_idx, blk_idx, inst_idx, &ops)) goto fail;
            if (!parse_flags_brackets(p, &inst.flags)) goto fail;
            break;
        }
        case PIR_OP_INVOKE: {
            PirOperand callee;
            if (p->cur.kind != TOK_GLOBAL) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected callee @symbol");
                goto fail;
            }
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &callee)) goto fail;
            PIR_VEC_PUSH(&ops, callee);
            if (!parse_call_args(p, func_idx, blk_idx, inst_idx, &ops)) goto fail;
            if (!is_ident(p, "then")) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'then' in invoke");
                goto fail;
            }
            advance_tok(p);
            if (!parse_branch_target(p, func_idx, blk_idx, inst_idx, 0, true, &inst.targets[0])) goto fail;
            if (!is_ident(p, "unwind")) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'unwind' in invoke");
                goto fail;
            }
            advance_tok(p);
            if (!parse_branch_target(p, func_idx, blk_idx, inst_idx, 1, true, &inst.targets[1])) goto fail;
            break;
        }
        case PIR_OP_INVOKE_INDIRECT: {
            if (p->cur.kind != TOK_GLOBAL) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected signature @Sig");
                goto fail;
            }
            PendingSigRef sr = { func_idx, inst_idx, p->cur.text, p->cur.line };
            PIR_VEC_PUSH(&p->pending_sigs, sr);
            advance_tok(p);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            PirOperand fn_ptr;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &fn_ptr)) goto fail;
            assign_literal_type(&fn_ptr, pir_type_scalar(PIR_SCALAR_PTR));
            PIR_VEC_PUSH(&ops, fn_ptr);
            if (!parse_call_args(p, func_idx, blk_idx, inst_idx, &ops)) goto fail;
            if (!is_ident(p, "then")) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'then' in invoke_indirect");
                goto fail;
            }
            advance_tok(p);
            if (!parse_branch_target(p, func_idx, blk_idx, inst_idx, 0, true, &inst.targets[0])) goto fail;
            if (!is_ident(p, "unwind")) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'unwind' in invoke_indirect");
                goto fail;
            }
            advance_tok(p);
            if (!parse_branch_target(p, func_idx, blk_idx, inst_idx, 1, true, &inst.targets[1])) goto fail;
            break;
        }
        case PIR_OP_JMP: {
            if (!parse_branch_target(p, func_idx, blk_idx, inst_idx, 0, false, &inst.targets[0])) goto fail;
            break;
        }
        case PIR_OP_BR: {
            PirOperand cond;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &cond)) goto fail;
            assign_literal_type(&cond, pir_type_scalar(PIR_SCALAR_I8));
            PIR_VEC_PUSH(&ops, cond);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_branch_target(p, func_idx, blk_idx, inst_idx, 0, false, &inst.targets[0])) goto fail;
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!parse_branch_target(p, func_idx, blk_idx, inst_idx, 1, false, &inst.targets[1])) goto fail;
            break;
        }
        case PIR_OP_SWITCH: {
            PirOperand val;
            if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &val)) goto fail;
            assign_literal_type(&val, inst.ty);
            PIR_VEC_PUSH(&ops, val);
            if (!expect_tok(p, TOK_COMMA, "','")) goto fail;
            if (!is_ident(p, "default")) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'default' in switch");
                goto fail;
            }
            advance_tok(p);
            if (!parse_branch_target(p, func_idx, blk_idx, inst_idx, 0, false, &inst.targets[0])) goto fail;
            if (!expect_tok(p, TOK_LBRACKET, "'['")) goto fail;
            skip_newlines(p);
            PIR_VEC(PirSwitchCase) cases = {0};
            while (p->cur.kind != TOK_RBRACKET && p->cur.kind != TOK_EOF) {
                if (p->cur.kind != TOK_INT) {
                    PIR_VEC_FREE(&cases);
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected switch case integer");
                    goto fail;
                }
                PirSwitchCase sc;
                memset(&sc, 0, sizeof(sc));
                sc.case_lo     = p->cur.int_lo;
                sc.case_hi     = p->cur.int_hi;
                sc.is_negative = p->cur.int_neg;
                advance_tok(p);
                if (!expect_tok(p, TOK_ARROW, "'->'")) { PIR_VEC_FREE(&cases); goto fail; }
                int which = 2 + (int)cases.len;
                if (!parse_branch_target(p, func_idx, blk_idx, inst_idx, which, false, &sc.target)) {
                    PIR_VEC_FREE(&cases);
                    goto fail;
                }
                PIR_VEC_PUSH(&cases, sc);
                skip_newlines(p);
                if (match_tok(p, TOK_COMMA)) {
                    skip_newlines(p);
                } else if (p->cur.kind != TOK_RBRACKET) {
                    PIR_VEC_FREE(&cases);
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected ',' or ']' in switch");
                    goto fail;
                }
            }
            if (!expect_tok(p, TOK_RBRACKET, "']'")) { PIR_VEC_FREE(&cases); goto fail; }
            if (cases.len > 0u) {
                inst.switch_case_count = cases.len;
                inst.switch_cases = PIR_ARENA_NEW_ARRAY(&p->mod->arena, PirSwitchCase, cases.len);
                memcpy(inst.switch_cases, cases.data, cases.len * sizeof(PirSwitchCase));
            }
            PIR_VEC_FREE(&cases);
            break;
        }
        case PIR_OP_RET: {
            inst.ty = fn->ret_type;
            if (p->cur.kind != TOK_NEWLINE && p->cur.kind != TOK_RBRACE && p->cur.kind != TOK_EOF) {
                PirOperand rv;
                if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, 0u, false, false, &rv)) goto fail;
                assign_literal_type(&rv, fn->ret_type);
                PIR_VEC_PUSH(&ops, rv);
            }
            break;
        }
        case PIR_OP_TRAP:
        case PIR_OP_UNREACHABLE:
            break;
        case PIR_OP_ADDR:
            inst.ty = pir_type_scalar(PIR_SCALAR_PTR);
            /* fallthrough */
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
        case PIR_OP_SPLAT:
        case PIR_OP_PTR_ADD:
        case PIR_OP_VA_START: case PIR_OP_VA_ARG: case PIR_OP_VA_COPY: case PIR_OP_VA_END:
        case PIR_OP_RESUME: {
            if (p->cur.kind != TOK_NEWLINE && p->cur.kind != TOK_LBRACKET &&
                p->cur.kind != TOK_RBRACE && p->cur.kind != TOK_EOF) {
                do {
                    PirOperand o;
                    if (!parse_operand(p, func_idx, blk_idx, inst_idx, -1, ops.len,
                                       false, false, &o)) {
                        goto fail;
                    }
                    PIR_VEC_PUSH(&ops, o);
                } while (match_tok(p, TOK_COMMA));
            }
            if (!parse_flags_brackets(p, &inst.flags)) goto fail;

            /* Assign inferred literal types for standard ops. */
            for (uint32_t k = 0u; k < ops.len; ++k) {
                PirType expected = inst.ty;
                if (inst.op == PIR_OP_SPLAT && inst.ty.kind == PIR_TYPE_VECTOR) {
                    expected = pir_type_scalar(pir_vec_elem_scalar(inst.ty.u.vec));
                } else if ((inst.op == PIR_OP_ADD_CARRY || inst.op == PIR_OP_SUB_BORROW) && k == 2u) {
                    expected = pir_type_scalar(PIR_SCALAR_I8);
                } else if (inst.op == PIR_OP_SELECT && k == 0u) {
                    expected = (inst.ty.kind == PIR_TYPE_VECTOR)
                               ? pir_type_vec(pir_vec_mask_type(inst.ty.u.vec))
                               : pir_type_scalar(PIR_SCALAR_I8);
                } else if (inst.op == PIR_OP_PTR_ADD && k == 0u) {
                    expected = pir_type_scalar(PIR_SCALAR_PTR);
                } else if (inst.op == PIR_OP_VA_START || inst.op == PIR_OP_VA_ARG ||
                           inst.op == PIR_OP_VA_COPY || inst.op == PIR_OP_VA_END ||
                           inst.op == PIR_OP_RESUME || inst.op == PIR_OP_PTRTOINT) {
                    expected = pir_type_scalar(PIR_SCALAR_PTR);
                } else if (inst.op == PIR_OP_INTTOPTR ||
                           inst.op == PIR_OP_SITOFP || inst.op == PIR_OP_UITOFP) {
                    expected = pir_type_scalar(PIR_SCALAR_I64);
                } else if (inst.op == PIR_OP_FPTOSI || inst.op == PIR_OP_FPTOUI ||
                           inst.op == PIR_OP_FPTRUNC) {
                    expected = pir_type_scalar(PIR_SCALAR_F64);
                } else if (inst.op == PIR_OP_FPEXT) {
                    expected = pir_type_scalar(PIR_SCALAR_F32);
                } else if (inst.op == PIR_OP_BITCAST && ops.data[k].kind == PIR_OPER_IMM_FLOAT &&
                           inst.ty.kind == PIR_TYPE_SCALAR) {
                    if (inst.ty.u.scalar == PIR_SCALAR_I32) {
                        expected = pir_type_scalar(PIR_SCALAR_F32);
                    } else if (inst.ty.u.scalar == PIR_SCALAR_I64) {
                        expected = pir_type_scalar(PIR_SCALAR_F64);
                    }
                }
                assign_literal_type(&ops.data[k], expected);
            }
            break;
        }
    }

    if (ops.len > 0u) {
        inst.operand_count = ops.len;
        inst.operands = PIR_ARENA_NEW_ARRAY(&p->mod->arena, PirOperand, ops.len);
        memcpy(inst.operands, ops.data, ops.len * sizeof(PirOperand));
    }
    PIR_VEC_FREE(&ops);

    if (res_count >= 1u) {
        inst.results[0] = define_value(p, fn, res_names[0], infer_result0_type(inst.op, inst.ty),
                                       blk_idx, inst_idx, 0u, line);
    }
    if (res_count == 2u) {
        inst.results[1] = define_value(p, fn, res_names[1], infer_result1_type(inst.op, inst.ty),
                                       blk_idx, inst_idx, 1u, line);
    }

    PIR_VEC_PUSH(&fn->insts, inst);
    PIR_VEC_PUSH(&fn->blocks.data[blk_idx].insts, inst_idx);

    if (p->cur.kind != TOK_NEWLINE && p->cur.kind != TOK_RBRACE && p->cur.kind != TOK_EOF) {
        pir_diag_error(p->diag, p->cur.line, p->cur.col, "unexpected trailing token on instruction");
        return false;
    }
    return true;

fail:
    PIR_VEC_FREE(&ops);
    return false;
}

/* =========================================================================
 * Declarations: type, sig, global/data/tls, func
 * ========================================================================= */

static bool parse_param_list(Parser *p, bool require_names,
                             PirParamDecl **out_params, uint32_t *out_count,
                             bool *out_variadic) {
    *out_params   = NULL;
    *out_count    = 0u;
    *out_variadic = false;
    if (!expect_tok(p, TOK_LPAREN, "'('")) return false;
    PIR_VEC(PirParamDecl) params = {0};

    if (p->cur.kind != TOK_RPAREN) {
        do {
            if (match_tok(p, TOK_ELLIPSIS)) {
                *out_variadic = true;
                break;
            }
            PirParamDecl pd;
            memset(&pd, 0, sizeof(pd));
            pd.val_id   = PIR_INVALID_ID;
            pd.abi_type = pir_type_scalar(PIR_SCALAR_VOID);

            if (p->cur.kind == TOK_LOCAL) {
                pd.name = p->cur.text;
                advance_tok(p);
                if (!expect_tok(p, TOK_COLON, "':'")) { PIR_VEC_FREE(&params); return false; }
            } else if (require_names) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected parameter name '%%name'");
                PIR_VEC_FREE(&params);
                return false;
            }

            if (is_ident(p, "byval")) {
                advance_tok(p);
                if (!expect_tok(p, TOK_LPAREN, "'('")) { PIR_VEC_FREE(&params); return false; }
                pd.abi_kind = PIR_PARAM_BYVAL;
                pd.type     = pir_type_scalar(PIR_SCALAR_PTR);
                if (!parse_type(p, &pd.abi_type)) { PIR_VEC_FREE(&params); return false; }
                if (!expect_tok(p, TOK_RPAREN, "')'")) { PIR_VEC_FREE(&params); return false; }
            } else if (is_ident(p, "retbuf")) {
                advance_tok(p);
                if (!expect_tok(p, TOK_LPAREN, "'('")) { PIR_VEC_FREE(&params); return false; }
                pd.abi_kind = PIR_PARAM_RETBUF;
                pd.type     = pir_type_scalar(PIR_SCALAR_PTR);
                if (!parse_type(p, &pd.abi_type)) { PIR_VEC_FREE(&params); return false; }
                if (!expect_tok(p, TOK_RPAREN, "')'")) { PIR_VEC_FREE(&params); return false; }
            } else {
                if (!parse_type(p, &pd.type)) { PIR_VEC_FREE(&params); return false; }
            }

            while (p->cur.kind == TOK_IDENT) {
                if (strcmp(p->cur.text, "noalias") == 0) {
                    if ((pd.attrs & PIR_PATTR_NOALIAS) != 0u) {
                        pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'noalias' attribute");
                        PIR_VEC_FREE(&params);
                        return false;
                    }
                    pd.attrs |= PIR_PATTR_NOALIAS;
                    advance_tok(p);
                } else if (strcmp(p->cur.text, "readonly") == 0) {
                    if ((pd.attrs & PIR_PATTR_READONLY) != 0u) {
                        pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'readonly' attribute");
                        PIR_VEC_FREE(&params);
                        return false;
                    }
                    pd.attrs |= PIR_PATTR_READONLY;
                    advance_tok(p);
                } else if (strcmp(p->cur.text, "signext") == 0) {
                    if ((pd.attrs & PIR_PATTR_SIGNEXT) != 0u) {
                        pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'signext' attribute");
                        PIR_VEC_FREE(&params);
                        return false;
                    }
                    pd.attrs |= PIR_PATTR_SIGNEXT;
                    advance_tok(p);
                } else if (strcmp(p->cur.text, "zeroext") == 0) {
                    if ((pd.attrs & PIR_PATTR_ZEROEXT) != 0u) {
                        pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'zeroext' attribute");
                        PIR_VEC_FREE(&params);
                        return false;
                    }
                    pd.attrs |= PIR_PATTR_ZEROEXT;
                    advance_tok(p);
                } else if (strcmp(p->cur.text, "byval_size") == 0) {
                    if (pd.abi_byval_size != 0u) {
                        pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'byval_size' attribute");
                        PIR_VEC_FREE(&params);
                        return false;
                    }
                    advance_tok(p);
                    if (!parse_u64_positive(p, &pd.abi_byval_size, "byval_size")) {
                        PIR_VEC_FREE(&params);
                        return false;
                    }
                } else if (strcmp(p->cur.text, "byval_align") == 0) {
                    if (pd.abi_byval_align != 0u) {
                        pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'byval_align' attribute");
                        PIR_VEC_FREE(&params);
                        return false;
                    }
                    advance_tok(p);
                    if (!parse_u32_positive(p, &pd.abi_byval_align, "byval_align")) {
                        PIR_VEC_FREE(&params);
                        return false;
                    }
                } else if (strcmp(p->cur.text, "stack_align") == 0) {
                    if (pd.abi_stack_align != 0u) {
                        pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'stack_align' attribute");
                        PIR_VEC_FREE(&params);
                        return false;
                    }
                    advance_tok(p);
                    if (!parse_u32_positive(p, &pd.abi_stack_align, "stack_align")) {
                        PIR_VEC_FREE(&params);
                        return false;
                    }
                } else if (strcmp(p->cur.text, "force_stack") == 0) {
                    if (pd.abi_force_stack) {
                        pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'force_stack' attribute");
                        PIR_VEC_FREE(&params);
                        return false;
                    }
                    pd.abi_force_stack = true;
                    advance_tok(p);
                } else {
                    break;
                }
            }
            PIR_VEC_PUSH(&params, pd);
        } while (match_tok(p, TOK_COMMA));
    }

    if (!expect_tok(p, TOK_RPAREN, "')'")) {
        PIR_VEC_FREE(&params);
        return false;
    }
    if (params.len > 0u) {
        *out_count  = params.len;
        *out_params = PIR_ARENA_NEW_ARRAY(&p->mod->arena, PirParamDecl, params.len);
        memcpy(*out_params, params.data, params.len * sizeof(PirParamDecl));
    }
    PIR_VEC_FREE(&params);
    return true;
}

static bool parse_fn_cc_and_attrs(Parser *p, PirCallingConv *cc_out,
                                  uint32_t *attrs_out, const char **sec_out,
                                  PirCabiRetInfo *cabi_ret_out) {
    *cc_out    = PIR_CC_INTERNAL;
    *attrs_out = PIR_FATTR_NONE;
    if (sec_out) *sec_out = NULL;
    if (cabi_ret_out) memset(cabi_ret_out, 0, sizeof(*cabi_ret_out));

    bool saw_cc = false;
    for (;;) {
        if (is_ident(p, "cc")) {
            if (saw_cc) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'cc' clause");
                return false;
            }
            saw_cc = true;
            advance_tok(p);
            if (p->cur.kind != TOK_STRING || token_has_nul(&p->cur)) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected calling convention string");
                return false;
            }
            if (strcmp(p->cur.text, "c") == 0) {
                *cc_out = PIR_CC_C;
            } else if (strcmp(p->cur.text, "internal") == 0) {
                *cc_out = PIR_CC_INTERNAL;
            } else {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "unknown calling convention '%s'", p->cur.text);
                return false;
            }
            advance_tok(p);
        } else if (is_ident(p, "section") && sec_out) {
            if (*sec_out != NULL) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'section' clause");
                return false;
            }
            advance_tok(p);
            if (p->cur.kind != TOK_STRING || token_has_nul(&p->cur) || token_has_newline(&p->cur)) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected valid section name string");
                return false;
            }
            *sec_out = p->cur.text;
            advance_tok(p);
        } else if (is_ident(p, "nounwind")) {
            *attrs_out |= PIR_FATTR_NOUNWIND;
            advance_tok(p);
        } else if (is_ident(p, "noreturn")) {
            *attrs_out |= PIR_FATTR_NORETURN;
            advance_tok(p);
        } else if (is_ident(p, "readonly")) {
            *attrs_out |= PIR_FATTR_READONLY;
            advance_tok(p);
        } else if (is_ident(p, "signext")) {
            *attrs_out |= PIR_FATTR_RET_SIGNEXT;
            advance_tok(p);
        } else if (is_ident(p, "zeroext")) {
            *attrs_out |= PIR_FATTR_RET_ZEROEXT;
            advance_tok(p);
        } else if (cabi_ret_out && is_ident(p, "sret_x8")) {
            if (cabi_ret_out->kind != PIR_CABI_RET_NONE) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate or conflicting return ABI attribute");
                return false;
            }
            cabi_ret_out->kind = PIR_CABI_RET_SRET_X8;
            advance_tok(p);
        } else if (cabi_ret_out && is_ident(p, "sret_rax")) {
            if (cabi_ret_out->kind != PIR_CABI_RET_NONE) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate or conflicting return ABI attribute");
                return false;
            }
            cabi_ret_out->kind = PIR_CABI_RET_SRET_RAX;
            advance_tok(p);
        } else if (cabi_ret_out && is_ident(p, "multi_ret")) {
            if (cabi_ret_out->kind != PIR_CABI_RET_NONE) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate or conflicting return ABI attribute");
                return false;
            }
            advance_tok(p);
            if (!expect_tok(p, TOK_LPAREN, "'('")) return false;
            cabi_ret_out->kind = PIR_CABI_RET_MULTI_REG;
            cabi_ret_out->reg_count = 0u;
            for (;;) {
                if (cabi_ret_out->reg_count >= 4u) {
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "too many registers in multi_ret");
                    return false;
                }
                if (!parse_type(p, &cabi_ret_out->regs[cabi_ret_out->reg_count++])) return false;
                if (!expect_tok(p, TOK_COMMA, "','")) return false;
                if (is_ident(p, "size")) break;
            }
            if (cabi_ret_out->reg_count < 2u) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "multi_ret requires at least 2 register types");
                return false;
            }
            if (!is_ident(p, "size")) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'size' in multi_ret");
                return false;
            }
            advance_tok(p);
            if (!parse_u64_positive(p, &cabi_ret_out->agg_size, "multi_ret size")) return false;
            if (!expect_tok(p, TOK_COMMA, "','")) return false;
            if (!is_ident(p, "align")) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'align' in multi_ret");
                return false;
            }
            advance_tok(p);
            if (!parse_u32_positive(p, &cabi_ret_out->agg_align, "multi_ret align")) return false;
            if (!expect_tok(p, TOK_RPAREN, "')'")) return false;
        } else if (p->cur.kind == TOK_LBRACKET) {
            advance_tok(p);
            do {
                if (is_ident(p, "nounwind"))      *attrs_out |= PIR_FATTR_NOUNWIND;
                else if (is_ident(p, "noreturn")) *attrs_out |= PIR_FATTR_NORETURN;
                else if (is_ident(p, "readonly")) *attrs_out |= PIR_FATTR_READONLY;
                else if (is_ident(p, "signext"))  *attrs_out |= PIR_FATTR_RET_SIGNEXT;
                else if (is_ident(p, "zeroext"))  *attrs_out |= PIR_FATTR_RET_ZEROEXT;
                else {
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "unknown function attribute '%s'",
                                   p->cur.text ? p->cur.text : "");
                    return false;
                }
                advance_tok(p);
            } while (match_tok(p, TOK_COMMA));
            if (!expect_tok(p, TOK_RBRACKET, "']'")) return false;
        } else {
            break;
        }
    }
    return true;
}

static bool parse_type_decl(Parser *p) {
    uint32_t line = p->cur.line;
    advance_tok(p); /* consume 'type' */
    if (p->cur.kind != TOK_GLOBAL) {
        pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected @Type name");
        return false;
    }
    const char *name = p->cur.text;
    advance_tok(p);
    if (!expect_tok(p, TOK_EQ, "'='")) return false;

    PirTypeId id = intern_named_agg(p, name, line);
    if (is_agg_defined(p, id)) {
        pir_diag_error(p->diag, line, 0u, "duplicate type definition '@%s'", name);
        return false;
    }

    PirAggKind kind = PIR_AGG_STRUCT;
    if (is_ident(p, "packed")) {
        kind = PIR_AGG_PACKED_STRUCT;
        advance_tok(p);
    } else if (is_ident(p, "union")) {
        kind = PIR_AGG_UNION;
        advance_tok(p);
    } else if (p->cur.kind == TOK_LBRACKET) {
        kind = PIR_AGG_ARRAY;
    }

    if (kind == PIR_AGG_ARRAY) {
        advance_tok(p); /* '[' */
        if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u) {
            pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected array length");
            return false;
        }
        uint64_t alen = p->cur.int_lo;
        advance_tok(p);
        if (!is_ident(p, "x")) {
            pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'x' in array type");
            return false;
        }
        advance_tok(p);
        PirType elem;
        if (!parse_type(p, &elem)) return false;
        if (!expect_tok(p, TOK_RBRACKET, "']'")) return false;
        PirAggDef *def = &p->mod->agg_types.data[id];
        def->src_line  = line;
        def->kind      = PIR_AGG_ARRAY;
        def->elem_type = elem;
        def->array_len = alen;
    } else {
        if (!expect_tok(p, TOK_LBRACE, "'{'")) return false;
        PIR_VEC(PirType) fields = {0};
        if (p->cur.kind != TOK_RBRACE) {
            do {
                PirType ft;
                if (!parse_type(p, &ft)) { PIR_VEC_FREE(&fields); return false; }
                PIR_VEC_PUSH(&fields, ft);
            } while (match_tok(p, TOK_COMMA));
        }
        if (!expect_tok(p, TOK_RBRACE, "'}'")) { PIR_VEC_FREE(&fields); return false; }
        PirAggDef *def = &p->mod->agg_types.data[id];
        def->src_line    = line;
        def->kind        = kind;
        def->field_count = fields.len;
        if (fields.len > 0u) {
            def->fields = PIR_ARENA_NEW_ARRAY(&p->mod->arena, PirType, fields.len);
            memcpy(def->fields, fields.data, fields.len * sizeof(PirType));
        }
        PIR_VEC_FREE(&fields);
    }

    while (match_tok(p, TOK_COMMA)) {
        if (is_ident(p, "align")) {
            if (p->mod->agg_types.data[id].min_align != 0u) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col,
                               "duplicate 'align' in type declaration");
                return false;
            }
            advance_tok(p);
            if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u ||
                p->cur.int_lo == 0u || (p->cur.int_lo & (p->cur.int_lo - 1u)) != 0u ||
                p->cur.int_lo > 4096u) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col,
                               "expected power-of-two alignment in type declaration");
                return false;
            }
            p->mod->agg_types.data[id].min_align = (uint32_t)p->cur.int_lo;
            advance_tok(p);
        } else if (is_ident(p, "memory")) {
            if (p->mod->agg_types.data[id].abi_memory) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col,
                               "duplicate 'memory' in type declaration");
                return false;
            }
            p->mod->agg_types.data[id].abi_memory = true;
            advance_tok(p);
        } else {
            pir_diag_error(p->diag, p->cur.line, p->cur.col,
                           "expected 'align' or 'memory' after ',' in type declaration");
            return false;
        }
    }

    mark_agg_defined(p, id);
    return true;
}

static bool parse_sig_decl(Parser *p) {
    uint32_t line = p->cur.line;
    advance_tok(p); /* consume 'sig' */
    if (p->cur.kind != TOK_GLOBAL) {
        pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected @Sig name");
        return false;
    }
    PirSigDef sig;
    memset(&sig, 0, sizeof(sig));
    sig.name     = p->cur.text;
    sig.src_line = line;
    advance_tok(p);
    if (!expect_tok(p, TOK_EQ, "'='")) return false;
    if (!parse_param_list(p, false, &sig.params, &sig.param_count, &sig.is_variadic)) return false;
    if (!expect_tok(p, TOK_ARROW, "'->'")) return false;
    if (!parse_type(p, &sig.ret_type)) return false;
    if (!parse_fn_cc_and_attrs(p, &sig.cc, &sig.attrs, NULL, &sig.cabi_ret)) return false;
    PIR_VEC_PUSH(&p->mod->sigs, sig);
    return true;
}

static bool parse_global_item(Parser *p, PirType hint_ty, PirGlobalItem *out) {
    memset(out, 0, sizeof(*out));
    if (p->cur.kind == TOK_STRING) {
        out->kind  = PIR_GITEM_BYTES;
        out->bytes = (const uint8_t *)p->cur.text;
        out->len   = (uint64_t)p->cur.len;
        advance_tok(p);
        return true;
    }
    if (p->cur.kind == TOK_INT) {
        out->kind    = PIR_GITEM_INT;
        out->int_lo  = p->cur.int_lo;
        out->int_hi  = p->cur.int_hi;
        out->int_neg = p->cur.int_neg;
        advance_tok(p);
        return true;
    }
    if (p->cur.kind == TOK_FLOAT || is_ident(p, "nan") || is_ident(p, "inf")) {
        double v = 0.0;
        float vf = 0.0f;
        bool has_f32 = false;
        if (p->cur.kind == TOK_FLOAT) {
            v = p->cur.float_val;
            vf = p->cur.float_val_f32;
            has_f32 = true;
        } else if (strcmp(p->cur.text, "nan") == 0) {
            v = (double)NAN;
            vf = (float)NAN;
            has_f32 = true;
        } else {
            v = HUGE_VAL;
            vf = HUGE_VALF;
            has_f32 = true;
        }
        uint32_t f32b = 0u;
        memcpy(&f32b, &vf, sizeof(f32b));
        out->kind         = PIR_GITEM_FLOAT;
        out->float32_bits = f32b;
        out->has_f32_bits = has_f32;
        out->float_bits   = encode_float_bits_tok(v, vf, has_f32, hint_ty);
        advance_tok(p);
        return true;
    }
    if (is_ident(p, "zeroinit")) {
        out->kind = PIR_GITEM_ZERO;
        advance_tok(p);
        if (p->cur.kind == TOK_INT && !p->cur.int_neg && p->cur.int_hi == 0u) {
            out->len = p->cur.int_lo;
            advance_tok(p);
        }
        return true;
    }
    if (is_ident(p, "ptr")) {
        advance_tok(p);
        if (p->cur.kind != TOK_GLOBAL) {
            pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected @symbol after 'ptr' in relocation");
            return false;
        }
        out->kind     = PIR_GITEM_RELOC;
        out->sym_name = p->cur.text;
        advance_tok(p);
        if (p->cur.kind == TOK_PLUS || p->cur.kind == TOK_MINUS) {
            bool sub = (p->cur.kind == TOK_MINUS);
            advance_tok(p);
            if (p->cur.kind != TOK_INT || p->cur.int_hi != 0u) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected integer relocation offset");
                return false;
            }
            uint64_t mag = p->cur.int_lo;
            bool neg = (sub != p->cur.int_neg);
            if (neg ? (mag > (uint64_t)INT64_MAX + 1u) : (mag > (uint64_t)INT64_MAX)) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "relocation offset out of signed 64-bit range");
                return false;
            }
            out->sym_offset = neg ? (int64_t)(UINT64_C(0) - mag) : (int64_t)mag;
            advance_tok(p);
        }
        return true;
    }
    if (p->cur.kind == TOK_GLOBAL) {
        out->kind     = PIR_GITEM_RELOC;
        out->sym_name = p->cur.text;
        advance_tok(p);
        return true;
    }
    pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected global initializer item");
    return false;
}

static bool parse_global_decl(Parser *p, PirLinkage linkage, bool is_weak_extern) {
    uint32_t line = p->cur.line;
    PirGlobal g;
    memset(&g, 0, sizeof(g));
    g.name           = p->cur.text;
    g.src_line       = line;
    g.linkage        = linkage;
    g.is_weak_extern = is_weak_extern;
    g.type           = pir_type_scalar(PIR_SCALAR_VOID);
    advance_tok(p); /* @name */
    if (!expect_tok(p, TOK_EQ, "'='")) return false;

    if (is_ident(p, "data"))        g.seg = PIR_GSEG_RODATA;
    else if (is_ident(p, "global")) g.seg = PIR_GSEG_DATA;
    else if (is_ident(p, "tls"))    g.seg = PIR_GSEG_TLS;
    else {
        pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'data', 'global', or 'tls'");
        return false;
    }
    advance_tok(p);

    for (;;) {
        if (is_ident(p, "align")) {
            if (g.align != 0u) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'align' clause");
                return false;
            }
            if (!parse_align_clause(p, &g.align)) return false;
        } else if (is_ident(p, "section")) {
            if (g.section != NULL) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'section' clause");
                return false;
            }
            advance_tok(p);
            if (p->cur.kind != TOK_STRING || token_has_nul(&p->cur) || token_has_newline(&p->cur)) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected valid section string");
                return false;
            }
            g.section = p->cur.text;
            advance_tok(p);
        } else {
            break;
        }
    }

    PIR_VEC(PirGlobalItem) items = {0};
    if (p->cur.kind == TOK_LBRACE) {
        g.is_braced = true;
        advance_tok(p);
        skip_newlines(p);
        while (p->cur.kind != TOK_RBRACE && p->cur.kind != TOK_EOF) {
            PirGlobalItem it;
            if (!parse_global_item(p, pir_type_scalar(PIR_SCALAR_F64), &it)) {
                PIR_VEC_FREE(&items);
                return false;
            }
            PIR_VEC_PUSH(&items, it);
            skip_newlines(p);
            if (match_tok(p, TOK_COMMA)) {
                skip_newlines(p);
            } else if (p->cur.kind != TOK_RBRACE) {
                PIR_VEC_FREE(&items);
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected ',' or '}' in global initializer");
                return false;
            }
        }
        if (!expect_tok(p, TOK_RBRACE, "'}'")) { PIR_VEC_FREE(&items); return false; }
    } else if (p->cur.kind == TOK_LBRACKET) {
        /* Distinguish flattened LIR `global [<bytes>] = zeroinit` from HIR `[N x T]`. */
        uint32_t bline = p->cur.line;
        advance_tok(p);
        if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u) {
            PIR_VEC_FREE(&items);
            pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected array length integer");
            return false;
        }
        uint64_t alen = p->cur.int_lo;
        advance_tok(p);
        if (match_tok(p, TOK_RBRACKET)) {
            /* Flattened LIR byte-count zeroinit global: `[<bytes>] = zeroinit, align <n>` */
            if (alen == 0u) {
                PIR_VEC_FREE(&items);
                pir_diag_error(p->diag, bline, 0u, "flattened zeroinit global size must be > 0");
                return false;
            }
            g.has_type    = false;
            g.is_zeroinit = true;
            if (!expect_tok(p, TOK_EQ, "'='")) { PIR_VEC_FREE(&items); return false; }
            if (!is_ident(p, "zeroinit")) {
                PIR_VEC_FREE(&items);
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'zeroinit'");
                return false;
            }
            advance_tok(p);
            PirGlobalItem zi;
            memset(&zi, 0, sizeof(zi));
            zi.kind = PIR_GITEM_ZERO;
            zi.len  = alen;
            PIR_VEC_PUSH(&items, zi);
        } else {
            if (!is_ident(p, "x")) {
                PIR_VEC_FREE(&items);
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'x' or ']'");
                return false;
            }
            advance_tok(p);
            PirType elem;
            if (!parse_type(p, &elem)) { PIR_VEC_FREE(&items); return false; }
            if (!expect_tok(p, TOK_RBRACKET, "']'")) { PIR_VEC_FREE(&items); return false; }
            g.has_type = true;
            g.type = pir_type_agg(intern_anon_array(p, alen, elem, bline));
            if (match_tok(p, TOK_EQ)) {
                if (is_ident(p, "zeroinit")) {
                    g.is_zeroinit = true;
                    advance_tok(p);
                } else if (p->cur.kind == TOK_LBRACE) {
                    g.is_braced = true;
                    advance_tok(p);
                    skip_newlines(p);
                    while (p->cur.kind != TOK_RBRACE && p->cur.kind != TOK_EOF) {
                        PirGlobalItem it;
                        if (!parse_global_item(p, g.type, &it)) { PIR_VEC_FREE(&items); return false; }
                        PIR_VEC_PUSH(&items, it);
                        skip_newlines(p);
                        if (match_tok(p, TOK_COMMA)) {
                            skip_newlines(p);
                        } else if (p->cur.kind != TOK_RBRACE) {
                            PIR_VEC_FREE(&items);
                            pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected ',' or '}' in global initializer");
                            return false;
                        }
                    }
                    if (!expect_tok(p, TOK_RBRACE, "'}'")) { PIR_VEC_FREE(&items); return false; }
                } else {
                    PirGlobalItem it;
                    if (!parse_global_item(p, g.type, &it)) { PIR_VEC_FREE(&items); return false; }
                    PIR_VEC_PUSH(&items, it);
                }
            }
        }
        for (;;) {
            bool had_comma = match_tok(p, TOK_COMMA);
            if (is_ident(p, "align")) {
                if (g.align != 0u) {
                    PIR_VEC_FREE(&items);
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'align' clause");
                    return false;
                }
                if (!parse_align_clause(p, &g.align)) { PIR_VEC_FREE(&items); return false; }
            } else if (is_ident(p, "section")) {
                if (g.section != NULL) {
                    PIR_VEC_FREE(&items);
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'section' clause");
                    return false;
                }
                advance_tok(p);
                if (p->cur.kind != TOK_STRING || token_has_nul(&p->cur) || token_has_newline(&p->cur)) {
                    PIR_VEC_FREE(&items);
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected valid section string");
                    return false;
                }
                g.section = p->cur.text;
                advance_tok(p);
            } else {
                if (had_comma) {
                    PIR_VEC_FREE(&items);
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'align' or 'section' after ','");
                    return false;
                }
                break;
            }
        }
    } else {
        g.has_type = true;
        if (!parse_type(p, &g.type)) { PIR_VEC_FREE(&items); return false; }
        if (match_tok(p, TOK_EQ)) {
            if (is_ident(p, "zeroinit")) {
                g.is_zeroinit = true;
                advance_tok(p);
            } else if (p->cur.kind == TOK_LBRACE) {
                g.is_braced = true;
                advance_tok(p);
                skip_newlines(p);
                while (p->cur.kind != TOK_RBRACE && p->cur.kind != TOK_EOF) {
                    PirGlobalItem it;
                    if (!parse_global_item(p, g.type, &it)) { PIR_VEC_FREE(&items); return false; }
                    PIR_VEC_PUSH(&items, it);
                    skip_newlines(p);
                    if (match_tok(p, TOK_COMMA)) {
                        skip_newlines(p);
                    } else if (p->cur.kind != TOK_RBRACE) {
                        PIR_VEC_FREE(&items);
                        pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected ',' or '}' in global initializer");
                        return false;
                    }
                }
                if (!expect_tok(p, TOK_RBRACE, "'}'")) { PIR_VEC_FREE(&items); return false; }
            } else {
                PirGlobalItem it;
                if (!parse_global_item(p, g.type, &it)) { PIR_VEC_FREE(&items); return false; }
                PIR_VEC_PUSH(&items, it);
            }
        }
        for (;;) {
            bool had_comma = match_tok(p, TOK_COMMA);
            if (is_ident(p, "align")) {
                if (g.align != 0u) {
                    PIR_VEC_FREE(&items);
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'align' clause");
                    return false;
                }
                if (!parse_align_clause(p, &g.align)) { PIR_VEC_FREE(&items); return false; }
            } else if (is_ident(p, "section")) {
                if (g.section != NULL) {
                    PIR_VEC_FREE(&items);
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "duplicate 'section' clause");
                    return false;
                }
                advance_tok(p);
                if (p->cur.kind != TOK_STRING || token_has_nul(&p->cur) || token_has_newline(&p->cur)) {
                    PIR_VEC_FREE(&items);
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected valid section string");
                    return false;
                }
                g.section = p->cur.text;
                advance_tok(p);
            } else {
                if (had_comma) {
                    PIR_VEC_FREE(&items);
                    pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 'align' or 'section' after ','");
                    return false;
                }
                break;
            }
        }
    }

    if (items.len > 0u) {
        g.item_count = items.len;
        g.items = PIR_ARENA_NEW_ARRAY(&p->mod->arena, PirGlobalItem, items.len);
        memcpy(g.items, items.data, items.len * sizeof(PirGlobalItem));
    }
    PIR_VEC_FREE(&items);
    PIR_VEC_PUSH(&p->mod->globals, g);
    return true;
}

static bool parse_func_decl(Parser *p, PirLinkage linkage, bool is_weak_extern) {
    uint32_t line = p->cur.line;
    advance_tok(p); /* consume 'func' */
    if (p->cur.kind != TOK_GLOBAL) {
        pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected @function name");
        return false;
    }

    PirFunction fn;
    memset(&fn, 0, sizeof(fn));
    fn.name           = p->cur.text;
    fn.src_line       = line;
    fn.linkage        = linkage;
    fn.is_weak_extern = is_weak_extern;
    advance_tok(p);

    bool is_extern = (linkage == PIR_LINK_EXTERN);
    if (!parse_param_list(p, !is_extern, &fn.params, &fn.param_count, &fn.is_variadic)) {
        return false;
    }
    if (!expect_tok(p, TOK_ARROW, "'->'")) return false;
    if (!parse_type(p, &fn.ret_type)) return false;
    if (!parse_fn_cc_and_attrs(p, &fn.cc, &fn.attrs, &fn.section, &fn.cabi_ret)) return false;

    PirFuncId func_idx = p->mod->funcs.len;
    PIR_VEC_PUSH(&p->mod->funcs, fn);
    PirFunction *f = &p->mod->funcs.data[func_idx];

    if (match_tok(p, TOK_LBRACE)) {
        f->is_defined = true;
        /* Index 0 in locs is sentinel "no location". */
        PirSourceLoc zero_loc = { 0u, 0u, 0u };
        PIR_VEC_PUSH(&f->locs, zero_loc);

        /* Bind function parameters as ValueIds with def_block == PIR_INVALID_ID. */
        for (uint32_t i = 0u; i < f->param_count; ++i) {
            if (f->params[i].name) {
                f->params[i].val_id = define_value(p, f, f->params[i].name, f->params[i].type,
                                                   PIR_INVALID_ID, PIR_INVALID_ID, i, f->src_line);
            }
        }

        skip_newlines(p);
        while (p->cur.kind != TOK_RBRACE && p->cur.kind != TOK_EOF) {
            if (p->cur.kind != TOK_LABEL) {
                pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected basic block label '.name:'");
                return false;
            }
            const char *lbl = p->cur.text;
            uint32_t bline  = p->cur.line;
            for (uint32_t b = 0u; b < f->blocks.len; ++b) {
                if (strcmp(f->blocks.data[b].label, lbl) == 0) {
                    pir_diag_error(p->diag, bline, 0u, "duplicate block label '.%s'", lbl);
                    break;
                }
            }
            PirBlock blk;
            memset(&blk, 0, sizeof(blk));
            blk.label    = lbl;
            blk.src_line = bline;
            PirBlockId blk_idx = f->blocks.len;
            PIR_VEC_PUSH(&f->blocks, blk);
            advance_tok(p);

            if (match_tok(p, TOK_LPAREN)) {
                if (p->cur.kind != TOK_RPAREN) {
                    do {
                        if (p->cur.kind != TOK_LOCAL) {
                            pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected block param '%%name'");
                            return false;
                        }
                        const char *pname = p->cur.text;
                        uint32_t pline    = p->cur.line;
                        advance_tok(p);
                        if (!expect_tok(p, TOK_COLON, "':'")) return false;
                        PirType pty;
                        if (!parse_type(p, &pty)) return false;
                        uint32_t pidx = f->blocks.data[blk_idx].params.len;
                        PirValueId vid = define_value(p, f, pname, pty, blk_idx,
                                                      PIR_INVALID_ID, pidx, pline);
                        PIR_VEC_PUSH(&f->blocks.data[blk_idx].params, vid);
                    } while (match_tok(p, TOK_COMMA));
                }
                if (!expect_tok(p, TOK_RPAREN, "')'")) return false;
            }
            if (!expect_tok(p, TOK_COLON, "':'")) return false;
            skip_newlines(p);

            uint32_t cur_loc = 0u;
            while (p->cur.kind != TOK_LABEL && p->cur.kind != TOK_RBRACE && p->cur.kind != TOK_EOF) {
                if (is_ident(p, "loc")) {
                    advance_tok(p);
                    if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u ||
                        p->cur.int_lo > UINT32_MAX) {
                        pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 32-bit file id in loc");
                        return false;
                    }
                    PirSourceLoc loc;
                    loc.file_id = (uint32_t)p->cur.int_lo;
                    advance_tok(p);
                    if (!expect_tok(p, TOK_COMMA, "','")) return false;
                    if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u ||
                        p->cur.int_lo > UINT32_MAX) {
                        pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 32-bit line in loc");
                        return false;
                    }
                    loc.line = (uint32_t)p->cur.int_lo;
                    advance_tok(p);
                    if (!expect_tok(p, TOK_COMMA, "','")) return false;
                    if (p->cur.kind != TOK_INT || p->cur.int_neg || p->cur.int_hi != 0u ||
                        p->cur.int_lo > UINT32_MAX) {
                        pir_diag_error(p->diag, p->cur.line, p->cur.col, "expected 32-bit col in loc");
                        return false;
                    }
                    loc.col = (uint32_t)p->cur.int_lo;
                    advance_tok(p);
                    if (p->cur.kind != TOK_NEWLINE && p->cur.kind != TOK_EOF) {
                        pir_diag_error(p->diag, p->cur.line, p->cur.col,
                                       "expected newline after loc directive");
                        return false;
                    }
                    cur_loc = f->locs.len;
                    PIR_VEC_PUSH(&f->locs, loc);
                    skip_newlines(p);
                    continue;
                }
                if (!parse_instruction(p, func_idx, blk_idx, cur_loc)) {
                    return false;
                }
                skip_newlines(p);
            }
        }
        if (!expect_tok(p, TOK_RBRACE, "'}'")) return false;
    }
    return true;
}

/* =========================================================================
 * Post-Parse Name & Type Resolution
 * ========================================================================= */

static PirOperand *get_operand_slot(PirInst *inst, int which_target, uint32_t op_idx) {
    if (which_target < 0) {
        return &inst->operands[op_idx];
    }
    if (which_target == 0) return &inst->targets[0].args[op_idx];
    if (which_target == 1) return &inst->targets[1].args[op_idx];
    return &inst->switch_cases[which_target - 2].target.args[op_idx];
}

static void resolve_branch_arg_literals(PirFunction *fn, PirBranchTarget *bt) {
    if (bt->target >= fn->blocks.len) return;
    const PirBlock *dst = &fn->blocks.data[bt->target];
    for (uint32_t k = 0u; k < bt->arg_count && k < dst->params.len; ++k) {
        PirType pty = fn->values.data[dst->params.data[k]].type;
        assign_literal_type(&bt->args[k], pty);
    }
}

static bool resolve_module(Parser *p) {
    PirModule *mod = p->mod;

    /* 1. Check that all referenced named aggregates were defined. */
    for (uint32_t i = 0u; i < mod->agg_types.len; ++i) {
        if (mod->agg_types.data[i].name && !is_agg_defined(p, i)) {
            pir_diag_error(p->diag, mod->agg_types.data[i].src_line, 0u,
                           "undefined aggregate type '@%s'", mod->agg_types.data[i].name);
        }
    }

    /* 2. Resolve branch target labels -> PirBlockId. */
    for (uint32_t i = 0u; i < p->pending_targets.len; ++i) {
        PendingTarget *pt = &p->pending_targets.data[i];
        PirFunction *fn = &mod->funcs.data[pt->func_idx];
        PirBlockId bid = PIR_INVALID_ID;
        for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
            if (strcmp(fn->blocks.data[b].label, pt->label) == 0) {
                bid = b;
                break;
            }
        }
        if (bid == PIR_INVALID_ID) {
            pir_diag_error(p->diag, pt->line, 0u, "undefined block label '.%s'", pt->label);
            continue;
        }
        PirInst *inst = &fn->insts.data[pt->inst_idx];
        if (pt->which_target == 0)      inst->targets[0].target = bid;
        else if (pt->which_target == 1) inst->targets[1].target = bid;
        else inst->switch_cases[pt->which_target - 2].target.target = bid;
    }

    /* 3. Resolve @Sig references on call_indirect / invoke_indirect. */
    for (uint32_t i = 0u; i < p->pending_sigs.len; ++i) {
        PendingSigRef *sr = &p->pending_sigs.data[i];
        PirSigId sid = PIR_INVALID_ID;
        for (uint32_t s = 0u; s < mod->sigs.len; ++s) {
            if (strcmp(mod->sigs.data[s].name, sr->sig_name) == 0) {
                sid = s;
                break;
            }
        }
        if (sid == PIR_INVALID_ID) {
            pir_diag_error(p->diag, sr->line, 0u, "undefined signature '@%s'", sr->sig_name);
            continue;
        }
        PirFunction *fn = &mod->funcs.data[sr->func_idx];
        PirInst *inst = &fn->insts.data[sr->inst_idx];
        inst->sig_id = sid;
        const PirSigDef *sig = &mod->sigs.data[sid];
        inst->ty = sig->ret_type;
        if (inst->results[0] != PIR_INVALID_ID) {
            fn->values.data[inst->results[0]].type = sig->ret_type;
        }
    }

    /* 4. Resolve direct call/invoke callee symbols & result/literal types. */
    for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
        PirFunction *fn = &mod->funcs.data[fi];
        for (uint32_t ii = 0u; ii < fn->insts.len; ++ii) {
            PirInst *inst = &fn->insts.data[ii];
            if (inst->op == PIR_OP_CALL || inst->op == PIR_OP_INVOKE) {
                const char *cname = inst->operands[0].u.sym.name;
                PirFuncId cid = PIR_INVALID_ID;
                for (uint32_t c = 0u; c < mod->funcs.len; ++c) {
                    if (strcmp(mod->funcs.data[c].name, cname) == 0) {
                        cid = c;
                        break;
                    }
                }
                if (cid != PIR_INVALID_ID) {
                    inst->operands[0].u.sym.is_func = true;
                    inst->operands[0].u.sym.id      = cid;
                    const PirFunction *callee = &mod->funcs.data[cid];
                    inst->ty = callee->ret_type;
                    if (inst->results[0] != PIR_INVALID_ID) {
                        fn->values.data[inst->results[0]].type = callee->ret_type;
                    }
                    for (uint32_t a = 1u; a < inst->operand_count && (a - 1u) < callee->param_count; ++a) {
                        assign_literal_type(&inst->operands[a], callee->params[a - 1u].type);
                    }
                    if (inst->op == PIR_OP_INVOKE) {
                        for (uint32_t k = 0u; k < inst->targets[0].arg_count; ++k) {
                            if (inst->targets[0].args[k].kind == PIR_OPER_INVOKE_RET) {
                                inst->targets[0].args[k].type = callee->ret_type;
                            }
                        }
                    }
                }
            } else if ((inst->op == PIR_OP_CALL_INDIRECT || inst->op == PIR_OP_INVOKE_INDIRECT) &&
                       inst->sig_id < mod->sigs.len) {
                const PirSigDef *sig = &mod->sigs.data[inst->sig_id];
                for (uint32_t a = 1u; a < inst->operand_count && (a - 1u) < sig->param_count; ++a) {
                    assign_literal_type(&inst->operands[a], sig->params[a - 1u].type);
                }
                if (inst->op == PIR_OP_INVOKE_INDIRECT) {
                    for (uint32_t k = 0u; k < inst->targets[0].arg_count; ++k) {
                        if (inst->targets[0].args[k].kind == PIR_OPER_INVOKE_RET) {
                            inst->targets[0].args[k].type = sig->ret_type;
                        }
                    }
                }
            }

            /* Resolve branch target literal types. */
            if (inst->op == PIR_OP_JMP) {
                resolve_branch_arg_literals(fn, &inst->targets[0]);
            } else if (inst->op == PIR_OP_BR || inst->op == PIR_OP_INVOKE ||
                       inst->op == PIR_OP_INVOKE_INDIRECT) {
                resolve_branch_arg_literals(fn, &inst->targets[0]);
                resolve_branch_arg_literals(fn, &inst->targets[1]);
            } else if (inst->op == PIR_OP_SWITCH) {
                resolve_branch_arg_literals(fn, &inst->targets[0]);
                for (uint32_t c = 0u; c < inst->switch_case_count; ++c) {
                    resolve_branch_arg_literals(fn, &inst->switch_cases[c].target);
                }
            }
        }
    }

    /* 5. Resolve %val references using O(1) hash-indexed name buckets + dominator lookup. */
    PirFuncId idx_fn_id = PIR_INVALID_ID;
    uint32_t *ht_head   = NULL;
    uint32_t *ht_next   = NULL;
    uint32_t  ht_mask   = 0u;
    bool      dom_built = false;
    PirCfg    dom_cfg;
    PirDom    dom_tree;
    memset(&dom_cfg, 0, sizeof(dom_cfg));
    memset(&dom_tree, 0, sizeof(dom_tree));

    for (uint32_t i = 0u; i < p->pending_vals.len; ++i) {
        PendingValRef *vr = &p->pending_vals.data[i];
        PirFunction *fn = &mod->funcs.data[vr->func_idx];
        if (idx_fn_id != vr->func_idx) {
            if (dom_built) {
                pir_dom_destroy(&dom_tree);
                pir_cfg_destroy(&dom_cfg);
                dom_built = false;
            }
            free(ht_head);
            free(ht_next);
            uint32_t nv = fn->values.len;
            uint32_t nbuck = 64u;
            while (nbuck < nv * 2u && nbuck < (1u << 24)) nbuck <<= 1u;
            ht_mask = nbuck - 1u;
            ht_head = (uint32_t *)malloc(nbuck * sizeof(uint32_t));
            ht_next = (uint32_t *)malloc((nv + 1u) * sizeof(uint32_t));
            if (!ht_head || !ht_next) abort();
            for (uint32_t b = 0u; b < nbuck; ++b) ht_head[b] = PIR_INVALID_ID;
            /* Insert in reverse so traversing each bucket visits lower ValueIds first. */
            for (uint32_t v = nv; v > 0u; --v) {
                uint32_t vid = v - 1u;
                const char *s = fn->values.data[vid].name;
                uint32_t h = 2166136261u;
                for (const unsigned char *q = (const unsigned char *)s; *q; ++q) {
                    h ^= (uint32_t)*q;
                    h *= 16777619u;
                }
                uint32_t bkt = h & ht_mask;
                ht_next[vid] = ht_head[bkt];
                ht_head[bkt] = vid;
            }
            idx_fn_id = vr->func_idx;
        }

        uint32_t vh = 2166136261u;
        for (const unsigned char *q = (const unsigned char *)vr->name; *q; ++q) {
            vh ^= (uint32_t)*q;
            vh *= 16777619u;
        }
        uint32_t bkt = vh & ht_mask;
        PirValueId found = PIR_INVALID_ID;

        /* Prefer definition in the same block (supports same-named block params in nested blocks). */
        for (uint32_t v = ht_head[bkt]; v != PIR_INVALID_ID; v = ht_next[v]) {
            if (fn->values.data[v].def_block == vr->use_block &&
                strcmp(fn->values.data[v].name, vr->name) == 0) {
                found = v;
                break;
            }
        }
        if (found == PIR_INVALID_ID) {
            /* Next prefer non-block-param defs (function params or instruction results). */
            for (uint32_t v = ht_head[bkt]; v != PIR_INVALID_ID; v = ht_next[v]) {
                if ((fn->values.data[v].def_block == PIR_INVALID_ID ||
                     fn->values.data[v].def_inst != PIR_INVALID_ID) &&
                    strcmp(fn->values.data[v].name, vr->name) == 0) {
                    found = v;
                    break;
                }
            }
        }
        if (found == PIR_INVALID_ID) {
            /* Collect same-named block params in this bucket. */
            uint32_t bp_cnt = 0u;
            PirValueId single_bp = PIR_INVALID_ID;
            for (uint32_t v = ht_head[bkt]; v != PIR_INVALID_ID; v = ht_next[v]) {
                if (fn->values.data[v].def_block != PIR_INVALID_ID &&
                    fn->values.data[v].def_inst == PIR_INVALID_ID &&
                    strcmp(fn->values.data[v].name, vr->name) == 0) {
                    single_bp = v;
                    bp_cnt++;
                }
            }
            if (bp_cnt == 1u) {
                found = single_bp;
            } else if (bp_cnt > 1u) {
                if (!dom_built) {
                    pir_cfg_build(&dom_cfg, fn);
                    pir_dom_build(&dom_tree, &dom_cfg);
                    dom_built = true;
                }
                if (vr->use_block < dom_cfg.num_blocks && dom_cfg.reachable[vr->use_block]) {
                    uint32_t cur_b = dom_tree.idom[vr->use_block];
                    while (cur_b != UINT32_MAX && cur_b != vr->use_block) {
                        for (uint32_t v = ht_head[bkt]; v != PIR_INVALID_ID; v = ht_next[v]) {
                            if (fn->values.data[v].def_block == cur_b &&
                                fn->values.data[v].def_inst == PIR_INVALID_ID &&
                                strcmp(fn->values.data[v].name, vr->name) == 0) {
                                found = v;
                                break;
                            }
                        }
                        if (found != PIR_INVALID_ID) break;
                        uint32_t next_b = dom_tree.idom[cur_b];
                        if (next_b == cur_b) break;
                        cur_b = next_b;
                    }
                }
            }
        }
        if (found == PIR_INVALID_ID) {
            for (uint32_t v = ht_head[bkt]; v != PIR_INVALID_ID; v = ht_next[v]) {
                if (strcmp(fn->values.data[v].name, vr->name) == 0) {
                    found = v;
                    break;
                }
            }
        }
        if (found == PIR_INVALID_ID) {
            pir_diag_error(p->diag, vr->line, 0u, "undefined SSA value '%%%s'", vr->name);
            continue;
        }
        PirInst *inst = &fn->insts.data[vr->inst_idx];
        PirOperand *slot = get_operand_slot(inst, vr->which_target, vr->op_idx);
        slot->u.val = found;
        if (!slot->has_explicit_type) {
            slot->type = fn->values.data[found].type;
        }
    }
    if (dom_built) {
        pir_dom_destroy(&dom_tree);
        pir_cfg_destroy(&dom_cfg);
    }
    free(ht_head);
    free(ht_next);

    return !pir_diag_has_errors(p->diag);
}

bool pir_parse_module(const char *src, size_t len, const char *filename,
                      PirModule *mod, PirDiag *diag) {
    (void)filename;
    Parser p;
    memset(&p, 0, sizeof(p));
    p.mod  = mod;
    p.diag = diag;
    pir_lexer_init(&p.lex, src, len, &mod->arena, diag);
    advance_tok(&p);
    skip_newlines(&p);

    /* Mandatory leading `target "<triple>"`. */
    if (!is_ident(&p, "target")) {
        pir_diag_error(diag, p.cur.line, p.cur.col, "module must begin with 'target \"<triple>\"'");
        goto done;
    }
    advance_tok(&p);
    if (p.cur.kind != TOK_STRING || token_has_nul(&p.cur)) {
        pir_diag_error(diag, p.cur.line, p.cur.col, "expected target triple string");
        goto done;
    }
    mod->target_triple = p.cur.text;
    advance_tok(&p);
    skip_newlines(&p);

    while (p.cur.kind != TOK_EOF && !pir_diag_has_errors(diag)) {
        if (is_ident(&p, "file")) {
            advance_tok(&p);
            if (p.cur.kind != TOK_INT || p.cur.int_neg || p.cur.int_hi != 0u ||
                p.cur.int_lo > UINT32_MAX) {
                pir_diag_error(diag, p.cur.line, p.cur.col, "expected 32-bit file id integer");
                break;
            }
            PirFileDecl fd;
            fd.id = (uint32_t)p.cur.int_lo;
            advance_tok(&p);
            if (p.cur.kind != TOK_STRING || token_has_nul(&p.cur)) {
                pir_diag_error(diag, p.cur.line, p.cur.col, "expected file path string");
                break;
            }
            fd.path = p.cur.text;
            advance_tok(&p);
            PIR_VEC_PUSH(&mod->files, fd);
        } else if (is_ident(&p, "type")) {
            if (!parse_type_decl(&p)) break;
        } else if (is_ident(&p, "sig")) {
            if (!parse_sig_decl(&p)) break;
        } else {
            PirLinkage linkage = PIR_LINK_INTERNAL;
            bool is_weak_extern = false;
            if (is_ident(&p, "export")) {
                linkage = PIR_LINK_EXPORT;
                advance_tok(&p);
            } else if (is_ident(&p, "weak")) {
                linkage = PIR_LINK_WEAK;
                advance_tok(&p);
                if (is_ident(&p, "extern")) {
                    linkage = PIR_LINK_EXTERN;
                    is_weak_extern = true;
                    advance_tok(&p);
                }
            } else if (is_ident(&p, "extern")) {
                linkage = PIR_LINK_EXTERN;
                advance_tok(&p);
            }

            if (is_ident(&p, "func")) {
                if (!parse_func_decl(&p, linkage, is_weak_extern)) break;
            } else if (p.cur.kind == TOK_GLOBAL) {
                if (!parse_global_decl(&p, linkage, is_weak_extern)) break;
            } else {
                pir_diag_error(diag, p.cur.line, p.cur.col, "unexpected top-level token");
                break;
            }
        }
        skip_newlines(&p);
    }

    if (!pir_diag_has_errors(diag)) {
        resolve_module(&p);
    }

done:
    free(p.agg_defined);
    PIR_VEC_FREE(&p.pending_targets);
    PIR_VEC_FREE(&p.pending_vals);
    PIR_VEC_FREE(&p.pending_sigs);
    return !pir_diag_has_errors(diag);
}
