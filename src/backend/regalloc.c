/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "backend/backend.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * ARM64 Physical Register Pools
 * =========================================================================
 * GP:
 *   Caller-saved allocatable: x0..x14 (15 regs)
 *   Callee-saved allocatable: x19..x28 (10 regs)
 *   Reserved: x15, x16, x17 (scratch), x18 (Darwin platform), x29 (FP), x30 (LR), sp/xzr
 *
 * FP/SIMD:
 *   Caller-saved allocatable: v0..v7, v16..v28 (21 regs)
 *   Callee-saved (low 64b d8..d15 ONLY!): v8..v15 (8 regs)
 *   Reserved: v29, v30, v31 (scratch)
 */

static const MPReg A64_GP_REGS[] = {
    /* Caller-saved first */
    A64_GP(0),  A64_GP(1),  A64_GP(2),  A64_GP(3),  A64_GP(4),
    A64_GP(5),  A64_GP(6),  A64_GP(7),  A64_GP(8),  A64_GP(9),
    A64_GP(10), A64_GP(11), A64_GP(12), A64_GP(13), A64_GP(14),
    /* Callee-saved */
    A64_GP(19), A64_GP(20), A64_GP(21), A64_GP(22), A64_GP(23),
    A64_GP(24), A64_GP(25), A64_GP(26), A64_GP(27), A64_GP(28)
};
#define NUM_A64_GP_REGS ((uint32_t)(sizeof(A64_GP_REGS) / sizeof(A64_GP_REGS[0])))

static const MPReg A64_FP_REGS[] = {
    /* Caller-saved first */
    A64_FP(0),  A64_FP(1),  A64_FP(2),  A64_FP(3),  A64_FP(4),
    A64_FP(5),  A64_FP(6),  A64_FP(7),
    A64_FP(16), A64_FP(17), A64_FP(18), A64_FP(19), A64_FP(20),
    A64_FP(21), A64_FP(22), A64_FP(23), A64_FP(24), A64_FP(25),
    A64_FP(26), A64_FP(27), A64_FP(28),
    /* Callee-saved (d8..d15 only) */
    A64_FP(8),  A64_FP(9),  A64_FP(10), A64_FP(11), A64_FP(12),
    A64_FP(13), A64_FP(14), A64_FP(15)
};
#define NUM_A64_FP_REGS ((uint32_t)(sizeof(A64_FP_REGS) / sizeof(A64_FP_REGS[0])))

static const MPReg X64_GP_REGS[] = {
    /* Caller-saved first (r10, r11 reserved scratch) */
    X64_RAX, X64_RCX, X64_RDX, X64_RSI, X64_RDI, X64_R8, X64_R9,
    /* Callee-saved (r15 reserved as 3rd spill scratch) */
    X64_RBX, X64_R12, X64_R13, X64_R14
};
#define NUM_X64_GP_REGS ((uint32_t)(sizeof(X64_GP_REGS) / sizeof(X64_GP_REGS[0])))

static const MPReg X64_FP_REGS[] = {
    /* All xmm0..xmm12 are caller-saved on SysV AMD64 (xmm13..xmm15 reserved scratch) */
    X64_XMM(0), X64_XMM(1), X64_XMM(2),  X64_XMM(3),  X64_XMM(4),
    X64_XMM(5), X64_XMM(6), X64_XMM(7),  X64_XMM(8),  X64_XMM(9),
    X64_XMM(10), X64_XMM(11), X64_XMM(12)
};
#define NUM_X64_FP_REGS ((uint32_t)(sizeof(X64_FP_REGS) / sizeof(X64_FP_REGS[0])))

static bool is_callee_saved_gp(PirArch arch, MPReg p) {
    if (arch == PIR_ARCH_X86_64) {
        return p == X64_RBX || p == X64_R12 || p == X64_R13 || p == X64_R14 || p == X64_R15;
    }
    return p >= A64_GP(19) && p <= A64_GP(28);
}

static bool is_callee_saved_fp(PirArch arch, MPReg p) {
    if (arch == PIR_ARCH_X86_64) {
        return false; /* SysV AMD64 has no callee-saved XMM registers */
    }
    return p >= A64_FP(8) && p <= A64_FP(15);
}

static void get_reg_pool(PirArch arch, MRegClass rc, const MPReg **out_pool, uint32_t *out_len) {
    if (arch == PIR_ARCH_X86_64) {
        *out_pool = (rc == MREG_CLASS_GP) ? X64_GP_REGS : X64_FP_REGS;
        *out_len  = (rc == MREG_CLASS_GP) ? NUM_X64_GP_REGS : NUM_X64_FP_REGS;
    } else {
        *out_pool = (rc == MREG_CLASS_GP) ? A64_GP_REGS : A64_FP_REGS;
        *out_len  = (rc == MREG_CLASS_GP) ? NUM_A64_GP_REGS : NUM_A64_FP_REGS;
    }
}

static bool is_allocatable_preg(PirArch arch, MRegClass rc, MPReg p) {
    const MPReg *pool = NULL;
    uint32_t len = 0u;
    get_reg_pool(arch, rc, &pool, &len);
    for (uint32_t i = 0u; i < len; ++i) {
        if (pool[i] == p) return true;
    }
    return false;
}

/* =========================================================================
 * Live Ranges & Intervals (Wimmer-Franz 2010 with Lifetime Holes)
 * ========================================================================= */

typedef struct {
    uint32_t start;
    uint32_t end;   /* Half-open [start, end) */
} LiveRange;

typedef struct {
    MVReg      vreg;
    MRegClass  rclass;
    uint8_t    byte_size;
    LiveRange *ranges;
    uint32_t   len;
    uint32_t   cap;
    MPReg      assigned;
    bool       spilled;
} LiveInterval;

static void iv_add_range(LiveInterval *iv, uint32_t start, uint32_t end) {
    if (start >= end) return;
    /* Insert keeping sorted & merged. */
    uint32_t i = 0u;
    while (i < iv->len && iv->ranges[i].end < start) {
        i++;
    }
    if (i < iv->len && iv->ranges[i].start <= end) {
        if (start < iv->ranges[i].start) iv->ranges[i].start = start;
        if (end > iv->ranges[i].end) iv->ranges[i].end = end;
        /* Merge with subsequent overlapping/adjacent ranges. */
        while (i + 1u < iv->len && iv->ranges[i + 1u].start <= iv->ranges[i].end) {
            if (iv->ranges[i + 1u].end > iv->ranges[i].end) {
                iv->ranges[i].end = iv->ranges[i + 1u].end;
            }
            memmove(&iv->ranges[i + 1u], &iv->ranges[i + 2u],
                    (size_t)(iv->len - i - 2u) * sizeof(LiveRange));
            iv->len--;
        }
        return;
    }
    if (iv->len == iv->cap) {
        uint32_t ncap = iv->cap ? iv->cap * 2u : 4u;
        iv->ranges = (LiveRange *)realloc(iv->ranges, (size_t)ncap * sizeof(LiveRange));
        if (!iv->ranges) abort();
        iv->cap = ncap;
    }
    if (i < iv->len) {
        memmove(&iv->ranges[i + 1u], &iv->ranges[i], (size_t)(iv->len - i) * sizeof(LiveRange));
    }
    iv->ranges[i].start = start;
    iv->ranges[i].end   = end;
    iv->len++;
}

static void iv_trim_start(LiveInterval *iv, uint32_t pos) {
    /* Def at pos: trim any range [s, e) with s < pos < e to [pos, e).
     * If no range covers pos, add [pos, pos + 1). */
    for (uint32_t i = 0u; i < iv->len; ++i) {
        if (iv->ranges[i].start <= pos && pos < iv->ranges[i].end) {
            iv->ranges[i].start = pos;
            return;
        }
    }
    iv_add_range(iv, pos, pos + 1u);
}

static uint32_t iv_start(const LiveInterval *iv) {
    return iv->len > 0u ? iv->ranges[0].start : UINT32_MAX;
}

static uint32_t iv_end(const LiveInterval *iv) {
    return iv->len > 0u ? iv->ranges[iv->len - 1u].end : 0u;
}

static bool iv_covers(const LiveInterval *iv, uint32_t pos) {
    for (uint32_t i = 0u; i < iv->len; ++i) {
        if (pos < iv->ranges[i].start) return false;
        if (pos < iv->ranges[i].end)   return true;
    }
    return false;
}

/* First position >= 0 where intervals a and b intersect, or UINT32_MAX if disjoint. */
static uint32_t iv_next_intersect(const LiveInterval *a, const LiveInterval *b) {
    uint32_t i = 0u, j = 0u;
    while (i < a->len && j < b->len) {
        uint32_t s = a->ranges[i].start > b->ranges[j].start ? a->ranges[i].start : b->ranges[j].start;
        uint32_t e = a->ranges[i].end   < b->ranges[j].end   ? a->ranges[i].end   : b->ranges[j].end;
        if (s < e) return s;
        if (a->ranges[i].end <= b->ranges[j].end) i++;
        else j++;
    }
    return UINT32_MAX;
}

/* =========================================================================
 * Bitset Helpers for Liveness Analysis
 * ========================================================================= */

static void bs_set(uint64_t *bs, uint32_t bit) {
    bs[bit >> 6u] |= ((uint64_t)1u << (bit & 63u));
}

static bool bs_test(const uint64_t *bs, uint32_t bit) {
    return (bs[bit >> 6u] & ((uint64_t)1u << (bit & 63u))) != 0u;
}

/* =========================================================================
 * Liveness & Interval Construction
 * ========================================================================= */

static void compute_intervals(const MFunc *mf, LiveInterval *v_ivs,
                              LiveInterval *fixed_ivs, LiveInterval *call_iv) {
    uint32_t nb = mf->blocks.len;
    uint32_t nv = mf->vregs.len;
    if (nb == 0u) return;
    uint32_t words = (nv + 63u) / 64u;
    if (words == 0u) words = 1u;

    uint32_t *blk_start = (uint32_t *)calloc((size_t)nb + 1u, sizeof(uint32_t));
    uint32_t *blk_end   = (uint32_t *)calloc((size_t)nb + 1u, sizeof(uint32_t));
    uint64_t *b_use     = (uint64_t *)calloc((size_t)nb * words, sizeof(uint64_t));
    uint64_t *b_def     = (uint64_t *)calloc((size_t)nb * words, sizeof(uint64_t));
    uint64_t *b_in      = (uint64_t *)calloc((size_t)nb * words, sizeof(uint64_t));
    uint64_t *b_out     = (uint64_t *)calloc((size_t)nb * words, sizeof(uint64_t));
    if (!blk_start || !blk_end || !b_use || !b_def || !b_in || !b_out) abort();

    /* 1. Assign dense even instruction positions across blocks. */
    uint32_t cur_pos = 2u;
    for (uint32_t b = 0u; b < nb; ++b) {
        blk_start[b] = cur_pos;
        cur_pos += mf->blocks.data[b].insts.len * 2u;
        if (mf->blocks.data[b].insts.len == 0u) cur_pos += 2u;
        blk_end[b] = cur_pos;
    }

    /* 2. Compute per-block use/def sets. */
    for (uint32_t b = 0u; b < nb; ++b) {
        uint64_t *use_s = &b_use[(size_t)b * words];
        uint64_t *def_s = &b_def[(size_t)b * words];
        const MBlock *blk = &mf->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const MInst *ins = &blk->insts.data[j];
            if (ins->op == MI_PCOPY) {
                const MPCopyBundle *pc = &mf->pcopies.data[ins->aux_id];
                for (uint32_t k = 0u; k < pc->count; ++k) {
                    if (!pc->entries[k].src_is_imm) {
                        MVReg sv = pc->entries[k].src;
                        if (sv < nv && !bs_test(def_s, sv)) bs_set(use_s, sv);
                    }
                }
                for (uint32_t k = 0u; k < pc->count; ++k) {
                    MVReg dv = pc->entries[k].dst;
                    if (dv < nv) bs_set(def_s, dv);
                }
                continue;
            }
            /* Uses first */
            for (uint8_t k = 0u; k < ins->op_count; ++k) {
                if (ins->ops[k].kind == MOP_VREG && ins->ops[k].is_use) {
                    MVReg v = ins->ops[k].u.vreg;
                    if (v < nv && !bs_test(def_s, v)) bs_set(use_s, v);
                }
            }
            /* Defs second */
            for (uint8_t k = 0u; k < ins->op_count; ++k) {
                if (ins->ops[k].kind == MOP_VREG && ins->ops[k].is_def) {
                    MVReg v = ins->ops[k].u.vreg;
                    if (v < nv) bs_set(def_s, v);
                }
            }
        }
    }

    /* 3. Backward liveness fixed point. */
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t bi = nb; bi-- > 0u; ) {
            uint64_t *out_s = &b_out[(size_t)bi * words];
            uint64_t *in_s  = &b_in[(size_t)bi * words];
            const uint64_t *use_s = &b_use[(size_t)bi * words];
            const uint64_t *def_s = &b_def[(size_t)bi * words];
            const MBlock *blk = &mf->blocks.data[bi];

            for (uint32_t si = 0u; si < blk->succs.len; ++si) {
                uint32_t sb = blk->succs.data[si];
                const uint64_t *sin = &b_in[(size_t)sb * words];
                for (uint32_t w = 0u; w < words; ++w) {
                    uint64_t nv_bits = out_s[w] | sin[w];
                    if (nv_bits != out_s[w]) {
                        out_s[w] = nv_bits;
                        changed = true;
                    }
                }
            }
            for (uint32_t w = 0u; w < words; ++w) {
                uint64_t nv_in = use_s[w] | (out_s[w] & ~def_s[w]);
                if (nv_in != in_s[w]) {
                    in_s[w] = nv_in;
                    changed = true;
                }
            }
        }
    }

    /* 4. Build Wimmer-Franz intervals by walking blocks in reverse order. */
    for (uint32_t bi = nb; bi-- > 0u; ) {
        const MBlock *blk = &mf->blocks.data[bi];
        uint32_t bs = blk_start[bi];
        uint32_t be = blk_end[bi];
        const uint64_t *out_s = &b_out[(size_t)bi * words];

        for (uint32_t v = 0u; v < nv; ++v) {
            if (bs_test(out_s, v)) {
                iv_add_range(&v_ivs[v], bs, be);
            }
        }

        /* Track intra-block live physical registers backward so fixed preg ranges span
         * continuously from def to use with zero holes. */
        uint64_t preg_live = 0u;
        uint32_t preg_use_end[64];
        memset(preg_use_end, 0, sizeof(preg_use_end));

        for (uint32_t j = blk->insts.len; j-- > 0u; ) {
            const MInst *ins = &blk->insts.data[j];
            uint32_t pos = bs + j * 2u;

            if (ins->op == MI_PCOPY) {
                const MPCopyBundle *pc = &mf->pcopies.data[ins->aux_id];
                /* Defs at pos */
                for (uint32_t k = 0u; k < pc->count; ++k) {
                    MVReg dv = pc->entries[k].dst;
                    if (dv < nv) iv_trim_start(&v_ivs[dv], pos);
                }
                /* Uses up to pos */
                for (uint32_t k = 0u; k < pc->count; ++k) {
                    if (!pc->entries[k].src_is_imm) {
                        MVReg sv = pc->entries[k].src;
                        if (sv < nv) iv_add_range(&v_ivs[sv], bs, pos > bs ? pos : pos + 1u);
                    }
                }
                continue;
            }

            /* Call clobbers: target-specific caller-saved GP & FP registers at [pos, pos+1). */
            if (ins->is_call) {
                iv_add_range(call_iv, pos, pos + 1u);
                if (mf->target->arch == PIR_ARCH_X86_64) {
                    static const MPReg X64_CALL_CLOB_GP[] = {
                        X64_RAX, X64_RCX, X64_RDX, X64_RSI, X64_RDI,
                        X64_R8,  X64_R9,  X64_R10, X64_R11
                    };
                    for (uint32_t ci = 0u; ci < sizeof(X64_CALL_CLOB_GP) / sizeof(X64_CALL_CLOB_GP[0]); ++ci) {
                        MPReg p = X64_CALL_CLOB_GP[ci];
                        if ((preg_live & (UINT64_C(1) << p)) != 0u) {
                            uint32_t end_p = preg_use_end[p] > pos ? preg_use_end[p] : (pos + 1u);
                            iv_add_range(&fixed_ivs[p], pos, end_p);
                            preg_live &= ~(UINT64_C(1) << p);
                        } else {
                            iv_add_range(&fixed_ivs[p], pos, pos + 1u);
                        }
                    }
                    for (uint32_t r = 0u; r < 16u; ++r) {
                        MPReg p = X64_XMM(r);
                        if ((preg_live & (UINT64_C(1) << p)) != 0u) {
                            uint32_t end_p = preg_use_end[p] > pos ? preg_use_end[p] : (pos + 1u);
                            iv_add_range(&fixed_ivs[p], pos, end_p);
                            preg_live &= ~(UINT64_C(1) << p);
                        } else {
                            iv_add_range(&fixed_ivs[p], pos, pos + 1u);
                        }
                    }
                } else {
                    for (uint32_t r = 0u; r <= 15u; ++r) {
                        MPReg p = A64_GP(r);
                        if ((preg_live & (UINT64_C(1) << p)) != 0u) {
                            uint32_t end_p = preg_use_end[p] > pos ? preg_use_end[p] : (pos + 1u);
                            iv_add_range(&fixed_ivs[p], pos, end_p);
                            preg_live &= ~(UINT64_C(1) << p);
                        } else {
                            iv_add_range(&fixed_ivs[p], pos, pos + 1u);
                        }
                    }
                    for (uint32_t r = 0u; r <= 7u; ++r) {
                        MPReg p = A64_FP(r);
                        if ((preg_live & (UINT64_C(1) << p)) != 0u) {
                            uint32_t end_p = preg_use_end[p] > pos ? preg_use_end[p] : (pos + 1u);
                            iv_add_range(&fixed_ivs[p], pos, end_p);
                            preg_live &= ~(UINT64_C(1) << p);
                        } else {
                            iv_add_range(&fixed_ivs[p], pos, pos + 1u);
                        }
                    }
                    for (uint32_t r = 16u; r <= 31u; ++r) {
                        MPReg p = A64_FP(r);
                        if ((preg_live & (UINT64_C(1) << p)) != 0u) {
                            uint32_t end_p = preg_use_end[p] > pos ? preg_use_end[p] : (pos + 1u);
                            iv_add_range(&fixed_ivs[p], pos, end_p);
                            preg_live &= ~(UINT64_C(1) << p);
                        } else {
                            iv_add_range(&fixed_ivs[p], pos, pos + 1u);
                        }
                    }
                }
            }

            /* Defs (and explicit fixed preg defs) */
            for (uint8_t k = 0u; k < ins->op_count; ++k) {
                if (ins->ops[k].kind == MOP_VREG && ins->ops[k].is_def) {
                    MVReg v = ins->ops[k].u.vreg;
                    if (v < nv) {
                        uint32_t def_pos = ins->ops[k].is_early_clobber ? (pos - 1u) : pos;
                        iv_trim_start(&v_ivs[v], def_pos);
                        if (ins->ops[k].is_early_clobber) {
                            /* Cover both [pos-1, pos) and [pos, pos+1) so even a dead
                             * early-clobber def at j==0 (pos==bs) conflicts with uses [bs, bs+1). */
                            iv_add_range(&v_ivs[v], def_pos, pos + 1u);
                        }
                    }
                } else if (ins->ops[k].kind == MOP_PREG && ins->ops[k].is_def) {
                    MPReg p = ins->ops[k].u.preg;
                    if (p < 64u) {
                        if ((preg_live & (UINT64_C(1) << p)) != 0u) {
                            uint32_t end_p = preg_use_end[p] > pos ? preg_use_end[p] : (pos + 1u);
                            iv_add_range(&fixed_ivs[p], pos, end_p);
                            preg_live &= ~(UINT64_C(1) << p);
                        } else {
                            iv_add_range(&fixed_ivs[p], pos, pos + 1u);
                        }
                    }
                }
            }

            /* Uses (and explicit fixed preg uses) */
            for (uint8_t k = 0u; k < ins->op_count; ++k) {
                if (ins->ops[k].kind == MOP_VREG && ins->ops[k].is_use) {
                    MVReg v = ins->ops[k].u.vreg;
                    if (v < nv) {
                        /* For single-instruction defuse or normal use, live from block start to pos.
                         * If pos == bs (first inst of block), add [bs, bs+1) so interval is non-empty! */
                        uint32_t end_p = (pos > bs) ? pos : (pos + 1u);
                        iv_add_range(&v_ivs[v], bs, end_p);
                    }
                } else if (ins->ops[k].kind == MOP_PREG && ins->ops[k].is_use) {
                    MPReg p = ins->ops[k].u.preg;
                    if (p < 64u) {
                        iv_add_range(&fixed_ivs[p], pos - 1u, pos);
                        if ((preg_live & (UINT64_C(1) << p)) == 0u) {
                            preg_live |= (UINT64_C(1) << p);
                            preg_use_end[p] = pos;
                        }
                    }
                }
            }
        }

        /* Any preg live into the top of this block (e.g. ABI args at entry) spans [bs, use_end). */
        if (preg_live != 0u) {
            for (uint32_t p = 0u; p < 64u; ++p) {
                if ((preg_live & (UINT64_C(1) << p)) != 0u) {
                    uint32_t end_p = preg_use_end[p] > bs ? preg_use_end[p] : (bs + 1u);
                    iv_add_range(&fixed_ivs[p], bs, end_p);
                }
            }
        }
    }

    /* Precolored virtual registers reserve their fixed physical register. */
    for (uint32_t v = 0u; v < nv; ++v) {
        MPReg fp = mf->vregs.data[v].fixed_preg;
        if (fp != MREG_NONE && fp < 64u) {
            for (uint32_t r = 0u; r < v_ivs[v].len; ++r) {
                iv_add_range(&fixed_ivs[fp], v_ivs[v].ranges[r].start, v_ivs[v].ranges[r].end);
            }
        }
    }

    free(blk_start);
    free(blk_end);
    free(b_use);
    free(b_def);
    free(b_in);
    free(b_out);
}

/* =========================================================================
 * Linear Scan Core (Per Register Class)
 * ========================================================================= */

static const LiveInterval *g_sort_ivs = NULL;

static int cmp_iv_order(const void *pa, const void *pb) {
    uint32_t a = *(const uint32_t *)pa;
    uint32_t b = *(const uint32_t *)pb;
    uint32_t sa = iv_start(&g_sort_ivs[a]);
    uint32_t sb = iv_start(&g_sort_ivs[b]);
    if (sa < sb) return -1;
    if (sa > sb) return 1;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

static void run_linear_scan_class(MFunc *mf, MRegClass rc, LiveInterval *v_ivs,
                                  const LiveInterval *fixed_ivs, const LiveInterval *call_iv) {
    PirArch arch = mf->target->arch;
    const MPReg *pool = NULL;
    uint32_t pool_len = 0u;
    get_reg_pool(arch, rc, &pool, &pool_len);
    uint32_t nv = mf->vregs.len;

    uint32_t *order = (uint32_t *)malloc(((size_t)nv + 1u) * sizeof(uint32_t));
    uint32_t *active = (uint32_t *)malloc(((size_t)nv + 1u) * sizeof(uint32_t));
    uint32_t *inactive = (uint32_t *)malloc(((size_t)nv + 1u) * sizeof(uint32_t));
    if (!order || !active || !inactive) abort();

    uint32_t norder = 0u;
    for (uint32_t v = 0u; v < nv; ++v) {
        if (mf->vregs.data[v].rclass != rc) continue;
        if (mf->vregs.data[v].fixed_preg != MREG_NONE) {
            v_ivs[v].assigned = mf->vregs.data[v].fixed_preg;
            continue;
        }
        if (v_ivs[v].len == 0u) {
            /* Unused dead vreg: assign first caller-saved register harmlessly. */
            v_ivs[v].assigned = pool[0];
            continue;
        }
        order[norder++] = v;
    }

    g_sort_ivs = v_ivs;
    qsort(order, norder, sizeof(uint32_t), cmp_iv_order);
    g_sort_ivs = NULL;

    uint32_t nactive = 0u, ninactive = 0u;

    for (uint32_t oi = 0u; oi < norder; ++oi) {
        uint32_t cur_v = order[oi];
        LiveInterval *cur = &v_ivs[cur_v];
        uint32_t cur_s = iv_start(cur);
        uint32_t cur_e = iv_end(cur);

        /* 1. Move expired / hole intervals out of active. */
        for (uint32_t i = 0u; i < nactive; ) {
            uint32_t av = active[i];
            if (iv_end(&v_ivs[av]) <= cur_s) {
                active[i] = active[--nactive];
            } else if (!iv_covers(&v_ivs[av], cur_s)) {
                inactive[ninactive++] = av;
                active[i] = active[--nactive];
            } else {
                i++;
            }
        }

        /* 2. Move expired / reactivated intervals out of inactive. */
        for (uint32_t i = 0u; i < ninactive; ) {
            uint32_t iv_v = inactive[i];
            if (iv_end(&v_ivs[iv_v]) <= cur_s) {
                inactive[i] = inactive[--ninactive];
            } else if (iv_covers(&v_ivs[iv_v], cur_s)) {
                active[nactive++] = iv_v;
                inactive[i] = inactive[--ninactive];
            } else {
                i++;
            }
        }

        /* 3. Compute free_until_pos for every register in pool. */
        uint32_t free_until[64];
        for (uint32_t r = 0u; r < 64u; ++r) free_until[r] = UINT32_MAX;

        for (uint32_t i = 0u; i < nactive; ++i) {
            MPReg p = v_ivs[active[i]].assigned;
            if (p < 64u) free_until[p] = 0u;
        }
        for (uint32_t i = 0u; i < ninactive; ++i) {
            uint32_t iv_v = inactive[i];
            MPReg p = v_ivs[iv_v].assigned;
            if (p < 64u) {
                uint32_t inter = iv_next_intersect(cur, &v_ivs[iv_v]);
                if (inter < free_until[p]) free_until[p] = inter;
            }
        }
        bool is_128_vec = (rc == MREG_CLASS_FP && cur->byte_size == 16u);
        uint32_t call_inter = is_128_vec ? iv_next_intersect(cur, call_iv) : UINT32_MAX;

        for (uint32_t pi = 0u; pi < pool_len; ++pi) {
            MPReg p = pool[pi];
            uint32_t inter = iv_next_intersect(cur, &fixed_ivs[p]);
            if (inter < free_until[p]) free_until[p] = inter;
            /* On AAPCS64, d8..d15 only preserve low 64 bits across calls!
             * 128-bit NEON vectors CANNOT survive calls in v8..v15. */
            if (is_128_vec && is_callee_saved_fp(arch, p) && call_inter < free_until[p]) {
                free_until[p] = call_inter;
            }
        }

        /* 4. Try hint register first, then first free register in pool order (caller-saved first). */
        MPReg chosen = MREG_NONE;
        MPReg hint = mf->vregs.data[cur_v].hint_preg;
        if (hint != MREG_NONE && is_allocatable_preg(arch, rc, hint) && free_until[hint] >= cur_e) {
            chosen = hint;
        }
        if (chosen == MREG_NONE) {
            for (uint32_t pi = 0u; pi < pool_len; ++pi) {
                MPReg p = pool[pi];
                if (free_until[p] >= cur_e) {
                    chosen = p;
                    break;
                }
            }
        }

        if (chosen != MREG_NONE) {
            cur->assigned = chosen;
            active[nactive++] = cur_v;
            continue;
        }

        /* 5. No register free for full interval: try evicting the furthest-ending active
         *    interval whose register has no fixed/inactive conflict with cur. */
        uint32_t evict_idx = UINT32_MAX;
        uint32_t max_end   = cur_e;
        for (uint32_t i = 0u; i < nactive; ++i) {
            uint32_t av = active[i];
            MPReg p = v_ivs[av].assigned;
            if (p >= 64u) continue;
            uint32_t av_end = iv_end(&v_ivs[av]);
            if (av_end <= max_end) continue;
            /* Verify p has no fixed/inactive conflict with cur */
            if (iv_next_intersect(cur, &fixed_ivs[p]) < cur_e) continue;
            if (is_128_vec && is_callee_saved_fp(arch, p) && call_inter < cur_e) continue;
            bool clash = false;
            for (uint32_t k = 0u; k < ninactive; ++k) {
                if (v_ivs[inactive[k]].assigned == p &&
                    iv_next_intersect(cur, &v_ivs[inactive[k]]) < cur_e) {
                    clash = true;
                    break;
                }
            }
            if (!clash) {
                max_end   = av_end;
                evict_idx = i;
            }
        }

        if (evict_idx != UINT32_MAX) {
            uint32_t ev = active[evict_idx];
            MPReg p = v_ivs[ev].assigned;
            v_ivs[ev].assigned = MREG_NONE;
            v_ivs[ev].spilled  = true;
            cur->assigned = p;
            active[evict_idx] = cur_v;
        } else {
            cur->assigned = MREG_NONE;
            cur->spilled  = true;
        }
    }

    free(order);
    free(active);
    free(inactive);
}

/* =========================================================================
 * Parallel Copy Sequentializer (Cycle-Safe with Dedicated Scratch Regs)
 * ========================================================================= */

typedef enum {
    PC_LOC_REG = 0,
    PC_LOC_SLOT
} PCLocKind;

typedef struct {
    PCLocKind kind;
    MPReg     preg;
    uint32_t  slot_id;
    MRegClass rclass;
    uint8_t   byte_size;
} PCLoc;

static bool pcloc_eq(PCLoc a, PCLoc b) {
    if (a.kind != b.kind) return false;
    if (a.kind == PC_LOC_REG) return a.preg == b.preg;
    return a.slot_id == b.slot_id;
}

static PCLoc vreg_to_pcloc(const MFunc *mf, MVReg v) {
    const MVRegInfo *vi = &mf->vregs.data[v];
    PCLoc loc;
    memset(&loc, 0, sizeof(loc));
    loc.rclass    = vi->rclass;
    loc.byte_size = vi->byte_size;
    if (vi->assigned_preg != MREG_NONE) {
        loc.kind = PC_LOC_REG;
        loc.preg = vi->assigned_preg;
    } else {
        loc.kind    = PC_LOC_SLOT;
        loc.slot_id = (uint32_t)vi->spill_slot;
    }
    return loc;
}

static void emit_mov_imm64(PirArch arch, MInstVec *out, MPReg dst, uint64_t val, uint8_t width, uint32_t loc) {
    if (arch == PIR_ARCH_X86_64) {
        MInst m = minst_make(MI_X64_MOV_RI, width <= 4u ? 4u : 8u, loc);
        m.op_count = 2u;
        m.ops[0]   = mop_preg_def(dst);
        m.ops[1]   = mop_imm((int64_t)(width <= 4u ? (val & 0xFFFFFFFFu) : val));
        PIR_VEC_PUSH(out, m);
        return;
    }
    if (width <= 4u) {
        val &= 0xFFFFFFFFu;
        uint16_t lo = (uint16_t)(val & 0xFFFFu);
        uint16_t hi = (uint16_t)((val >> 16u) & 0xFFFFu);
        MInst m = minst_make(MI_A64_MOVZ, 4u, loc);
        m.aux_width = 0u;
        m.op_count = 2u; m.ops[0] = mop_preg_def(dst); m.ops[1] = mop_imm(lo);
        PIR_VEC_PUSH(out, m);
        if (hi != 0u) {
            MInst k = minst_make(MI_A64_MOVK, 4u, loc);
            k.aux_width = 16u;
            k.op_count = 2u; k.ops[0] = mop_preg_defuse(dst); k.ops[1] = mop_imm(hi);
            PIR_VEC_PUSH(out, k);
        }
        return;
    }
    /* 64-bit immediate */
    uint16_t chunks[4] = {
        (uint16_t)(val & 0xFFFFu),
        (uint16_t)((val >> 16u) & 0xFFFFu),
        (uint16_t)((val >> 32u) & 0xFFFFu),
        (uint16_t)((val >> 48u) & 0xFFFFu)
    };
    uint32_t zcnt = 0u, fcnt = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        if (chunks[i] == 0u) zcnt++;
        if (chunks[i] == 0xFFFFu) fcnt++;
    }
    bool first = true;
    if (fcnt > zcnt) {
        for (uint32_t i = 0u; i < 4u; ++i) {
            if (chunks[i] == 0xFFFFu && !(first && i == 3u)) continue;
            if (first) {
                MInst m = minst_make(MI_A64_MOVN, 8u, loc);
                m.aux_width = (uint8_t)(i * 16u);
                m.op_count = 2u; m.ops[0] = mop_preg_def(dst); m.ops[1] = mop_imm((uint16_t)(~chunks[i]));
                PIR_VEC_PUSH(out, m);
                first = false;
            } else {
                MInst k = minst_make(MI_A64_MOVK, 8u, loc);
                k.aux_width = (uint8_t)(i * 16u);
                k.op_count = 2u; k.ops[0] = mop_preg_defuse(dst); k.ops[1] = mop_imm(chunks[i]);
                PIR_VEC_PUSH(out, k);
            }
        }
    } else {
        for (uint32_t i = 0u; i < 4u; ++i) {
            if (chunks[i] == 0u && !(first && i == 3u)) continue;
            if (first) {
                MInst m = minst_make(MI_A64_MOVZ, 8u, loc);
                m.aux_width = (uint8_t)(i * 16u);
                m.op_count = 2u; m.ops[0] = mop_preg_def(dst); m.ops[1] = mop_imm(chunks[i]);
                PIR_VEC_PUSH(out, m);
                first = false;
            } else {
                MInst k = minst_make(MI_A64_MOVK, 8u, loc);
                k.aux_width = (uint8_t)(i * 16u);
                k.op_count = 2u; k.ops[0] = mop_preg_defuse(dst); k.ops[1] = mop_imm(chunks[i]);
                PIR_VEC_PUSH(out, k);
            }
        }
    }
}

static void emit_pcloc_move(MInstVec *out, PCLoc dst, PCLoc src,
                            MPReg slot_tmp_gp, MPReg slot_tmp_fp, uint32_t loc) {
    if (pcloc_eq(dst, src)) return;
    uint8_t w = dst.byte_size;
    if (dst.kind == PC_LOC_REG && src.kind == PC_LOC_REG) {
        MInst c = minst_make(MI_COPY, w, loc);
        c.op_count = 2u;
        c.ops[0] = mop_preg_def(dst.preg);
        c.ops[1] = mop_preg_use(src.preg);
        PIR_VEC_PUSH(out, c);
    } else if (dst.kind == PC_LOC_REG && src.kind == PC_LOC_SLOT) {
        MInst ld = minst_make(MI_LOAD_SLOT, w, loc);
        ld.op_count = 2u;
        ld.ops[0] = mop_preg_def(dst.preg);
        ld.ops[1] = mop_slot(src.slot_id);
        PIR_VEC_PUSH(out, ld);
    } else if (dst.kind == PC_LOC_SLOT && src.kind == PC_LOC_REG) {
        MInst st = minst_make(MI_STORE_SLOT, w, loc);
        st.op_count = 2u;
        st.ops[0] = mop_preg_use(src.preg);
        st.ops[1] = mop_slot(dst.slot_id);
        PIR_VEC_PUSH(out, st);
    } else {
        /* Slot -> Slot via second scratch register (never clobbers cycle-breaker tmp!) */
        MPReg tmp = (dst.rclass == MREG_CLASS_GP) ? slot_tmp_gp : slot_tmp_fp;
        MInst ld = minst_make(MI_LOAD_SLOT, w, loc);
        ld.op_count = 2u; ld.ops[0] = mop_preg_def(tmp); ld.ops[1] = mop_slot(src.slot_id);
        PIR_VEC_PUSH(out, ld);
        MInst st = minst_make(MI_STORE_SLOT, w, loc);
        st.op_count = 2u; st.ops[0] = mop_preg_use(tmp); st.ops[1] = mop_slot(dst.slot_id);
        PIR_VEC_PUSH(out, st);
    }
}

typedef struct {
    PCLoc dst;
    PCLoc src;
    bool  done;
} PendingCopy;

static void lower_pcopy_bundle(const MFunc *mf, const MPCopyBundle *pc,
                               uint32_t loc, MInstVec *out) {
    if (pc->count == 0u) return;
    PirArch arch = mf->target->arch;
    MPReg cyc_gp  = (arch == PIR_ARCH_X86_64) ? X64_SCRATCH_GP1 : A64_SCRATCH_GP0;
    MPReg slot_gp = (arch == PIR_ARCH_X86_64) ? X64_SCRATCH_GP0 : A64_SCRATCH_GP2;
    MPReg cyc_fp  = (arch == PIR_ARCH_X86_64) ? X64_SCRATCH_FP2 : A64_SCRATCH_FP2;
    MPReg slot_fp = (arch == PIR_ARCH_X86_64) ? X64_SCRATCH_FP1 : A64_SCRATCH_FP1;

    PendingCopy *copies = (PendingCopy *)calloc(pc->count, sizeof(PendingCopy));
    if (!copies) abort();
    uint32_t ncopies = 0u;

    for (uint32_t i = 0u; i < pc->count; ++i) {
        if (pc->entries[i].src_is_imm) continue;
        PCLoc d = vreg_to_pcloc(mf, pc->entries[i].dst);
        PCLoc s = vreg_to_pcloc(mf, pc->entries[i].src);
        if (pcloc_eq(d, s)) continue;
        copies[ncopies].dst  = d;
        copies[ncopies].src  = s;
        copies[ncopies].done = false;
        ncopies++;
    }

    uint32_t remaining = ncopies;
    while (remaining > 0u) {
        bool progressed = false;
        for (uint32_t i = 0u; i < ncopies; ++i) {
            if (copies[i].done) continue;
            bool dst_is_read = false;
            for (uint32_t j = 0u; j < ncopies; ++j) {
                if (!copies[j].done && pcloc_eq(copies[j].src, copies[i].dst)) {
                    dst_is_read = true;
                    break;
                }
            }
            if (!dst_is_read) {
                emit_pcloc_move(out, copies[i].dst, copies[i].src,
                                slot_gp, slot_fp, loc);
                copies[i].done = true;
                remaining--;
                progressed = true;
            }
        }
        if (!progressed) {
            /* All remaining copies form pure cycles: break the first cycle via scratch reg. */
            uint32_t k = 0u;
            while (k < ncopies && copies[k].done) k++;
            PCLoc old_src = copies[k].src;
            PCLoc tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.kind      = PC_LOC_REG;
            tmp.rclass    = old_src.rclass;
            tmp.byte_size = old_src.byte_size;
            tmp.preg      = (old_src.rclass == MREG_CLASS_GP) ? cyc_gp : cyc_fp;

            emit_pcloc_move(out, tmp, old_src, slot_gp, slot_fp, loc);
            for (uint32_t j = 0u; j < ncopies; ++j) {
                if (!copies[j].done && pcloc_eq(copies[j].src, old_src)) {
                    copies[j].src = tmp;
                }
            }
        }
    }
    free(copies);

    /* Emit immediate-source copies LAST (they read no locations, so cannot clobber any src). */
    for (uint32_t i = 0u; i < pc->count; ++i) {
        if (!pc->entries[i].src_is_imm) continue;
        PCLoc d = vreg_to_pcloc(mf, pc->entries[i].dst);
        uint64_t bits = pc->entries[i].imm_bits;
        if (d.rclass == MREG_CLASS_GP) {
            MPReg target_reg = (d.kind == PC_LOC_REG) ? d.preg : cyc_gp;
            emit_mov_imm64(arch, out, target_reg, bits, d.byte_size, loc);
            if (d.kind == PC_LOC_SLOT) {
                MInst st = minst_make(MI_STORE_SLOT, d.byte_size, loc);
                st.op_count = 2u; st.ops[0] = mop_preg_use(target_reg); st.ops[1] = mop_slot(d.slot_id);
                PIR_VEC_PUSH(out, st);
            }
        } else {
            /* Float scalar immediate (f32 or f64) */
            MPReg gp_t = cyc_gp;
            MPReg fp_t = (d.kind == PC_LOC_REG) ? d.preg : cyc_fp;
            emit_mov_imm64(arch, out, gp_t, bits, d.byte_size, loc);
            MInst fm = minst_make(arch == PIR_ARCH_X86_64 ? MI_X64_MOV_G2X : MI_A64_FMOV_G2F,
                                  d.byte_size, loc);
            fm.op_count = 2u; fm.ops[0] = mop_preg_def(fp_t); fm.ops[1] = mop_preg_use(gp_t);
            PIR_VEC_PUSH(out, fm);
            if (d.kind == PC_LOC_SLOT) {
                MInst st = minst_make(MI_STORE_SLOT, d.byte_size, loc);
                st.op_count = 2u; st.ops[0] = mop_preg_use(fp_t); st.ops[1] = mop_slot(d.slot_id);
                PIR_VEC_PUSH(out, st);
            }
        }
    }
}

/* =========================================================================
 * Post-Allocation Rewriting (Spill Reloads/Stores + VReg -> PReg)
 * ========================================================================= */

static bool rewrite_blocks(MFunc *mf, PirDiag *diag) {
    static const MPReg A64_GP_SCRATCH[3] = { A64_SCRATCH_GP0, A64_SCRATCH_GP1, A64_SCRATCH_GP2 };
    static const MPReg A64_FP_SCRATCH[3] = { A64_SCRATCH_FP0, A64_SCRATCH_FP1, A64_SCRATCH_FP2 };
    static const MPReg X64_GP_SCRATCH[3] = { X64_SCRATCH_GP0, X64_SCRATCH_GP1, X64_SCRATCH_GP2 };
    static const MPReg X64_FP_SCRATCH[3] = { X64_SCRATCH_FP0, X64_SCRATCH_FP1, X64_SCRATCH_FP2 };
    bool is_x64 = (mf->target->arch == PIR_ARCH_X86_64);
    const MPReg *GP_SCRATCH = is_x64 ? X64_GP_SCRATCH : A64_GP_SCRATCH;
    const MPReg *FP_SCRATCH = is_x64 ? X64_FP_SCRATCH : A64_FP_SCRATCH;
    MPReg addr_tmp_gp = is_x64 ? X64_SCRATCH_GP1 : A64_SCRATCH_GP1;

    for (uint32_t b = 0u; b < mf->blocks.len; ++b) {
        MBlock *blk = &mf->blocks.data[b];
        MInstVec out = {0};

        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            MInst ins = blk->insts.data[j];
            if (ins.op == MI_PCOPY) {
                lower_pcopy_bundle(mf, &mf->pcopies.data[ins.aux_id], ins.loc_idx, &out);
                continue;
            }

            /* Collect unique spilled vregs in this instruction; duplicate vregs share one scratch. */
            MVReg mapped_v[MINST_MAX_OPS];
            MPReg mapped_p[MINST_MAX_OPS];
            bool  mapped_need_load[MINST_MAX_OPS];
            bool  mapped_need_store[MINST_MAX_OPS];
            bool  mapped_ec[MINST_MAX_OPS];
            uint32_t nmapped = 0u;

            for (uint8_t k = 0u; k < ins.op_count; ++k) {
                if (ins.ops[k].kind != MOP_VREG) continue;
                MVReg v = ins.ops[k].u.vreg;
                const MVRegInfo *vi = &mf->vregs.data[v];
                if (vi->assigned_preg != MREG_NONE) continue;

                uint32_t found = UINT32_MAX;
                for (uint32_t m = 0u; m < nmapped; ++m) {
                    if (mapped_v[m] == v) { found = m; break; }
                }
                if (found == UINT32_MAX) {
                    found = nmapped++;
                    mapped_v[found]          = v;
                    mapped_p[found]          = MREG_NONE;
                    mapped_need_load[found]  = false;
                    mapped_need_store[found] = false;
                    mapped_ec[found]         = false;
                }
                if (ins.ops[k].is_use)           mapped_need_load[found]  = true;
                if (ins.ops[k].is_def)           mapped_need_store[found] = true;
                if (ins.ops[k].is_early_clobber) mapped_ec[found]         = true;
            }

            /* Assign distinct scratch regs without modulo aliasing:
             * Pass 0: spilled defs/defuses that are uses or early-clobber defs get GP0/GP1 first
             *         so on ARM64 GP2 (x17) is never a spilled def (kept free as large-offset store tmp).
             * Pass 1: remaining pure uses take next free scratch.
             * Pass 2: pure non-early-clobber defs take a free scratch or reuse a pure-use scratch. */
            uint32_t gp_used = 0u, fp_used = 0u;
            uint32_t gp_def_mask = 0u, fp_def_mask = 0u;
            for (uint32_t pass = 0u; pass < 3u; ++pass) {
                for (uint32_t m = 0u; m < nmapped; ++m) {
                    bool is_p1 = mapped_need_load[m] || mapped_ec[m];
                    bool want = (pass == 0u) ? (is_p1 && mapped_need_store[m])
                              : (pass == 1u) ? (is_p1 && !mapped_need_store[m])
                                             : (!is_p1);
                    if (!want) continue;
                    const MVRegInfo *vi = &mf->vregs.data[mapped_v[m]];
                    bool is_gp = (vi->rclass == MREG_CLASS_GP);
                    uint32_t *used_cnt = is_gp ? &gp_used : &fp_used;
                    uint32_t *def_mask = is_gp ? &gp_def_mask : &fp_def_mask;
                    const MPReg *pool  = is_gp ? GP_SCRATCH : FP_SCRATCH;

                    uint32_t sidx = UINT32_MAX;
                    if (pass == 2u) {
                        /* Prefer GP0/GP1 (index < 2) for pure defs so GP2 (x17) stays free as tmp. */
                        for (uint32_t s = 0u; s < 3u; ++s) {
                            if ((*def_mask & (1u << s)) == 0u) {
                                sidx = s;
                                break;
                            }
                        }
                        if (sidx >= *used_cnt && sidx < 3u) *used_cnt = sidx + 1u;
                    } else if (*used_cnt < 3u) {
                        sidx = (*used_cnt)++;
                    }
                    if (sidx == UINT32_MAX) {
                        pir_diag_error(diag, 0u, 0u,
                                       "register allocator: instruction requires >3 %s scratch registers",
                                       is_gp ? "GP" : "FP");
                        PIR_VEC_FREE(&out);
                        return false;
                    }
                    if (mapped_need_store[m] || mapped_ec[m]) {
                        *def_mask |= (1u << sidx);
                    }
                    mapped_p[m] = pool[sidx];
                    if (is_x64 && is_gp && sidx == 2u) {
                        mf->used_callee_gp_mask |= (1u << mreg_num(mapped_p[m]));
                    }
                }
            }

            /* Rewrite operands from VREG to PREG. */
            for (uint8_t k = 0u; k < ins.op_count; ++k) {
                if (ins.ops[k].kind != MOP_VREG) continue;
                MVReg v = ins.ops[k].u.vreg;
                const MVRegInfo *vi = &mf->vregs.data[v];
                if (vi->assigned_preg != MREG_NONE) {
                    ins.ops[k].kind   = MOP_PREG;
                    ins.ops[k].u.preg = vi->assigned_preg;
                } else {
                    for (uint32_t m = 0u; m < nmapped; ++m) {
                        if (mapped_v[m] == v) {
                            ins.ops[k].kind   = MOP_PREG;
                            ins.ops[k].u.preg = mapped_p[m];
                            break;
                        }
                    }
                }
            }

            /* Emit FP reloads BEFORE GP reloads so frame pass has GP scratch free for FP slot addr!
             * Among GP reloads, emit addr_tmp_gp (x16 / r11) LAST so large-offset frame lowering
             * of earlier reloads cannot clobber an already-reloaded addr_tmp_gp. */
            for (uint32_t m = 0u; m < nmapped; ++m) {
                const MVRegInfo *vi = &mf->vregs.data[mapped_v[m]];
                if (mapped_need_load[m] && vi->rclass == MREG_CLASS_FP) {
                    MInst ld = minst_make(MI_LOAD_SLOT, vi->byte_size, ins.loc_idx);
                    ld.op_count = 2u;
                    ld.ops[0] = mop_preg_def(mapped_p[m]);
                    ld.ops[1] = mop_slot((uint32_t)vi->spill_slot);
                    PIR_VEC_PUSH(&out, ld);
                }
            }
            for (uint32_t phase = 0u; phase < 2u; ++phase) {
                for (uint32_t m = 0u; m < nmapped; ++m) {
                    const MVRegInfo *vi = &mf->vregs.data[mapped_v[m]];
                    if (!mapped_need_load[m] || vi->rclass != MREG_CLASS_GP) continue;
                    bool is_addr_tmp = (mapped_p[m] == addr_tmp_gp);
                    if ((phase == 1u) != is_addr_tmp) continue;
                    MInst ld = minst_make(MI_LOAD_SLOT, vi->byte_size, ins.loc_idx);
                    ld.op_count = 2u;
                    ld.ops[0] = mop_preg_def(mapped_p[m]);
                    ld.ops[1] = mop_slot((uint32_t)vi->spill_slot);
                    PIR_VEC_PUSH(&out, ld);
                }
            }

            /* Drop trivial self-copy */
            if (!(ins.op == MI_COPY && ins.op_count == 2u &&
                  ins.ops[0].kind == MOP_PREG && ins.ops[1].kind == MOP_PREG &&
                  ins.ops[0].u.preg == ins.ops[1].u.preg)) {
                PIR_VEC_PUSH(&out, ins);
            }

            /* Emit stores for spilled defs: GP stores first (with addr_tmp_gp FIRST so subsequent
             * large-offset frame lowering cannot clobber it), then FP stores. */
            for (uint32_t phase = 0u; phase < 2u; ++phase) {
                for (uint32_t m = 0u; m < nmapped; ++m) {
                    const MVRegInfo *vi = &mf->vregs.data[mapped_v[m]];
                    if (!mapped_need_store[m] || vi->rclass != MREG_CLASS_GP) continue;
                    bool is_addr_tmp = (mapped_p[m] == addr_tmp_gp);
                    if ((phase == 0u) != is_addr_tmp) continue;
                    MInst st = minst_make(MI_STORE_SLOT, vi->byte_size, ins.loc_idx);
                    st.op_count = 2u;
                    st.ops[0] = mop_preg_use(mapped_p[m]);
                    st.ops[1] = mop_slot((uint32_t)vi->spill_slot);
                    PIR_VEC_PUSH(&out, st);
                }
            }
            for (uint32_t m = 0u; m < nmapped; ++m) {
                const MVRegInfo *vi = &mf->vregs.data[mapped_v[m]];
                if (mapped_need_store[m] && vi->rclass == MREG_CLASS_FP) {
                    MInst st = minst_make(MI_STORE_SLOT, vi->byte_size, ins.loc_idx);
                    st.op_count = 2u;
                    st.ops[0] = mop_preg_use(mapped_p[m]);
                    st.ops[1] = mop_slot((uint32_t)vi->spill_slot);
                    PIR_VEC_PUSH(&out, st);
                }
            }
        }

        PIR_VEC_FREE(&blk->insts);
        blk->insts = out;
    }
    return true;
}

#ifdef PIR_VERIFY_ALL
static bool verify_mir_common(const MFunc *mf, bool post_ra, PirDiag *diag) {
    uint32_t nb = mf->blocks.len;
    uint32_t nv = mf->vregs.len;
    uint32_t ns = mf->slots.len;
    uint32_t nj = mf->jtables.len;
    uint32_t np = mf->pcopies.len;

    for (uint32_t b = 0u; b < nb; ++b) {
        const MBlock *blk = &mf->blocks.data[b];
        for (uint32_t s = 0u; s < blk->succs.len; ++s) {
            if (blk->succs.data[s] >= nb) {
                pir_diag_error(diag, 0u, 0u, "MIR verifier: invalid succ block %u", blk->succs.data[s]);
                return false;
            }
        }
        for (uint32_t p = 0u; p < blk->preds.len; ++p) {
            if (blk->preds.data[p] >= nb) {
                pir_diag_error(diag, 0u, 0u, "MIR verifier: invalid pred block %u", blk->preds.data[p]);
                return false;
            }
        }
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const MInst *ins = &blk->insts.data[j];
            if (ins->op_count > MINST_MAX_OPS) {
                pir_diag_error(diag, 0u, 0u, "MIR verifier: op_count %u exceeds max", (unsigned)ins->op_count);
                return false;
            }
            if (ins->op == MI_PCOPY) {
                if (post_ra) {
                    pir_diag_error(diag, 0u, 0u, "MIR verifier (post-RA): MI_PCOPY survived regalloc");
                    return false;
                }
                if (ins->aux_id >= np) {
                    pir_diag_error(diag, 0u, 0u, "MIR verifier: invalid pcopy bundle %u", ins->aux_id);
                    return false;
                }
                const MPCopyBundle *pc = &mf->pcopies.data[ins->aux_id];
                for (uint32_t k = 0u; k < pc->count; ++k) {
                    if (pc->entries[k].dst >= nv ||
                        (!pc->entries[k].src_is_imm && pc->entries[k].src >= nv)) {
                        pir_diag_error(diag, 0u, 0u, "MIR verifier: out-of-range vreg in MI_PCOPY");
                        return false;
                    }
                    for (uint32_t m = 0u; m < k; ++m) {
                        if (pc->entries[m].dst == pc->entries[k].dst) {
                            pir_diag_error(diag, 0u, 0u, "MIR verifier: duplicate dst vreg in MI_PCOPY");
                            return false;
                        }
                    }
                }
                continue;
            }
            for (uint8_t k = 0u; k < ins->op_count; ++k) {
                const MOperand *op = &ins->ops[k];
                switch (op->kind) {
                    case MOP_NONE:
                    case MOP_IMM:
                    case MOP_SYM:
                    case MOP_COND:
                        break;
                    case MOP_VREG:
                        if (post_ra) {
                            pir_diag_error(diag, 0u, 0u, "MIR verifier (post-RA): MOP_VREG survived regalloc");
                            return false;
                        }
                        if (op->u.vreg >= nv) {
                            pir_diag_error(diag, 0u, 0u, "MIR verifier: vreg %u out of range", op->u.vreg);
                            return false;
                        }
                        break;
                    case MOP_PREG:
                        if (op->u.preg >= 64u ||
                            (mf->target && mf->target->arch == PIR_ARCH_X86_64 &&
                             ((op->u.preg >= 16u && op->u.preg < 32u) || op->u.preg >= 48u))) {
                            pir_diag_error(diag, 0u, 0u, "MIR verifier: preg %u out of range", (unsigned)op->u.preg);
                            return false;
                        }
                        break;
                    case MOP_BLOCK:
                        if (op->u.block_id >= nb) {
                            pir_diag_error(diag, 0u, 0u, "MIR verifier: block %u out of range", op->u.block_id);
                            return false;
                        }
                        break;
                    case MOP_SLOT:
                        if (op->u.slot_id >= ns) {
                            pir_diag_error(diag, 0u, 0u, "MIR verifier: slot %u out of range", op->u.slot_id);
                            return false;
                        }
                        break;
                    case MOP_JTABLE:
                        if (op->u.jtable_id >= nj) {
                            pir_diag_error(diag, 0u, 0u, "MIR verifier: jtable %u out of range", op->u.jtable_id);
                            return false;
                        }
                        break;
                }
            }
        }
    }
    for (uint32_t j = 0u; j < nj; ++j) {
        const MJTable *jt = &mf->jtables.data[j];
        for (uint32_t k = 0u; k < jt->num_entries; ++k) {
            if (jt->targets[k] >= nb) {
                pir_diag_error(diag, 0u, 0u, "MIR verifier: jtable target block %u out of range",
                               jt->targets[k]);
                return false;
            }
        }
    }
    return true;
}

bool pir_mir_verify_pre_ra(const MFunc *mf, PirDiag *diag) {
    return verify_mir_common(mf, false, diag);
}

bool pir_mir_verify_post_ra(const MFunc *mf, PirDiag *diag) {
    return verify_mir_common(mf, true, diag);
}
#endif

/* =========================================================================
 * Public Entry Point
 * ========================================================================= */

bool pir_regalloc_run(MFunc *mf, PirDiag *diag) {
    uint32_t nv = mf->vregs.len;
    LiveInterval *v_ivs = (LiveInterval *)calloc((size_t)nv + 1u, sizeof(LiveInterval));
    LiveInterval fixed_ivs[64];
    LiveInterval call_iv;
    memset(fixed_ivs, 0, sizeof(fixed_ivs));
    memset(&call_iv, 0, sizeof(call_iv));
    if (!v_ivs) abort();

    for (uint32_t v = 0u; v < nv; ++v) {
        v_ivs[v].vreg      = v;
        v_ivs[v].rclass    = mf->vregs.data[v].rclass;
        v_ivs[v].byte_size = mf->vregs.data[v].byte_size;
        v_ivs[v].assigned  = MREG_NONE;
    }

    compute_intervals(mf, v_ivs, fixed_ivs, &call_iv);

    run_linear_scan_class(mf, MREG_CLASS_GP, v_ivs, fixed_ivs, &call_iv);
    run_linear_scan_class(mf, MREG_CLASS_FP, v_ivs, fixed_ivs, &call_iv);

    /* Commit assignments & allocate unique spill slots per spilled vreg (safe for setjmp/longjmp). */
    mf->used_callee_gp_mask = 0u;
    mf->used_callee_fp_mask = 0u;
    for (uint32_t v = 0u; v < nv; ++v) {
        MVRegInfo *vi = &mf->vregs.data[v];
        if (v_ivs[v].spilled || v_ivs[v].assigned == MREG_NONE) {
            vi->assigned_preg = MREG_NONE;
            uint32_t sz = vi->byte_size >= 4u ? vi->byte_size : 4u;
            vi->spill_slot = (int32_t)mfunc_add_slot(mf, sz, sz, true);
        } else {
            vi->assigned_preg = v_ivs[v].assigned;
            vi->spill_slot    = -1;
            if (is_callee_saved_gp(mf->target->arch, vi->assigned_preg)) {
                mf->used_callee_gp_mask |= (1u << mreg_num(vi->assigned_preg));
            } else if (is_callee_saved_fp(mf->target->arch, vi->assigned_preg)) {
                mf->used_callee_fp_mask |= (1u << mreg_num(vi->assigned_preg));
            }
        }
    }

    bool ok = rewrite_blocks(mf, diag);

    for (uint32_t v = 0u; v < nv; ++v) free(v_ivs[v].ranges);
    for (uint32_t r = 0u; r < 64u; ++r) free(fixed_ivs[r].ranges);
    free(call_iv.ranges);
    free(v_ivs);
    return ok;
}
