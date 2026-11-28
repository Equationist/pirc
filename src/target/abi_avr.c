/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * AVR (ATmega1280, avr-unknown-elf) C ABI Classifier.
 *
 * Rules (AVR-GCC / Clang --target=avr -mmcu=atmega1280 -mdouble=64):
 *   - 9 GP argument register pairs (18 bytes), allocated top-down from r25:
 *       r24:r25, r22:r23, r20:r21, r18:r19, r16:r17, r14:r15, r12:r13, r10:r11, r8:r9.
 *   - No hardware floating-point registers (f32 -> i32, f64 -> i64 via soft-float).
 *   - All scalar alignments are 1 byte; pointers are 2 bytes (16-bit).
 *   - Multi-word scalar/struct args of N 16-bit words (N = 2..4) allocate 2*N registers
 *     as an atomic top-down group (LSW in lowest reg) or spill all N words to stack.
 *   - Small aggregates (byval(@T), size <= 8B): DIRECT in 1..4 16-bit register words
 *     (1B -> i8, 2B -> i16, 3..4B -> 2x i16, 5..6B -> 3x i16, 7..8B -> 4x i16).
 *   - Large aggregates (> 8B) and i128: INDIRECT via 16-bit pointer.
 *   - Aggregate return (retbuf(@T)):
 *       * Empty (size == 0): DIRECT 0 regs.
 *       * size == 1B: DIRECT 1 reg (i8 in r24).
 *       * size == 2B: DIRECT 1 reg (i16 in r24:r25).
 *       * 3..4B:      DIRECT 2 regs ({ i16, i16 } in r22:r25).
 *       * 5..6B:      DIRECT 3 regs ({ i16, i16, i16 } in r20:r25).
 *       * 7..8B:      DIRECT 4 regs ({ i16, i16, i16, i16 } in r18:r25).
 *       * > 8B:       INDIRECT hidden sret pointer in r24:r25.
 */

#include "target/abi.h"
#include "lower/lower.h"

#include <string.h>

static void pack_avr_words(uint64_t size, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    out->kind = PIR_ABI_DIRECT;
    if (size == 0u) {
        out->reg_count = 0u;
        return;
    }
    if (size == 1u) {
        out->reg_count = 1u;
        out->regs[0]   = pir_type_scalar(PIR_SCALAR_I8);
        out->gp_used   = 1u;
        return;
    }
    uint32_t nw = (uint32_t)((size + 1u) / 2u);
    if (nw > 4u) nw = 4u;
    out->reg_count = nw;
    out->gp_used   = nw;
    for (uint32_t k = 0u; k < nw; ++k) {
        out->regs[k] = pir_type_scalar(PIR_SCALAR_I16);
    }
}

bool pir_abi_avr_param(const PirModule *mod, const PirTargetSpec *target,
                       const PirParamDecl *pd, PirAbiRegState *st, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));

    if (pd->abi_kind == PIR_PARAM_BYVAL) {
        PirTypeLayout ly = pir_type_layout(mod, target, pd->abi_type);
        if (ly.size == 0u) {
            out->kind      = PIR_ABI_DIRECT;
            out->reg_count = 0u;
            return true;
        }
        if (ly.size <= 8u) {
            pack_avr_words(ly.size, out);
            if (st->gp_used + out->gp_used <= 9u) {
                st->gp_used += out->gp_used;
            } else {
                st->gp_used = 9u;
                out->gp_used = 0u;
                out->force_stack = true;
            }
            return true;
        }
        out->kind    = PIR_ABI_INDIRECT;
        out->gp_used = 1u;
        if (st->gp_used < 9u) st->gp_used++;
        return true;
    }

    PirType ty = pd->type;
    if (ty.kind == PIR_TYPE_SCALAR) {
        switch (ty.u.scalar) {
            case PIR_SCALAR_I128:
                out->kind    = PIR_ABI_INDIRECT;
                out->gp_used = 1u;
                if (st->gp_used < 9u) st->gp_used++;
                return true;

            case PIR_SCALAR_I64:
            case PIR_SCALAR_F64:
                out->kind      = PIR_ABI_DIRECT;
                out->reg_count = 1u;
                out->regs[0]   = ty;
                if (st->gp_used + 4u <= 9u) {
                    out->gp_used = 4u;
                    st->gp_used += 4u;
                } else {
                    st->gp_used = 9u;
                }
                return true;

            case PIR_SCALAR_I32:
            case PIR_SCALAR_F32:
                out->kind      = PIR_ABI_DIRECT;
                out->reg_count = 1u;
                out->regs[0]   = ty;
                if (st->gp_used + 2u <= 9u) {
                    out->gp_used = 2u;
                    st->gp_used += 2u;
                } else {
                    st->gp_used = 9u;
                }
                return true;

            case PIR_SCALAR_I8:
            case PIR_SCALAR_I16:
            case PIR_SCALAR_PTR:
            case PIR_SCALAR_VALIST:
            case PIR_SCALAR_VOID:
                out->kind      = PIR_ABI_DIRECT;
                out->reg_count = 1u;
                out->regs[0]   = ty;
                if (st->gp_used < 9u) {
                    out->gp_used = 1u;
                    st->gp_used++;
                }
                return true;
        }
    }

    PirTypeLayout ly = pir_type_layout(mod, target, ty);
    if (ly.size > 8u) {
        out->kind    = PIR_ABI_INDIRECT;
        out->gp_used = 1u;
        if (st->gp_used < 9u) st->gp_used++;
        return true;
    }
    out->kind      = PIR_ABI_DIRECT;
    out->reg_count = 1u;
    out->regs[0]   = ty;
    uint32_t pairs = (uint32_t)((ly.size + 1u) / 2u);
    if (pairs == 0u) pairs = 1u;
    if (st->gp_used + pairs <= 9u) {
        out->gp_used = pairs;
        st->gp_used += pairs;
    } else {
        st->gp_used = 9u;
    }
    return true;
}

bool pir_abi_avr_retbuf(const PirModule *mod, const PirTargetSpec *target,
                        PirType agg_ty, PirAbiArgInfo *out) {
    memset(out, 0, sizeof(*out));
    PirTypeLayout ly = pir_type_layout(mod, target, agg_ty);
    if (ly.size == 0u) {
        out->kind      = PIR_ABI_DIRECT;
        out->reg_count = 0u;
        return true;
    }
    if (ly.size <= 8u) {
        pack_avr_words(ly.size, out);
        return true;
    }
    /* > 8 bytes: hidden sret pointer in r24:r25 (normal param 0). */
    out->kind    = PIR_ABI_INDIRECT;
    out->gp_used = 1u;
    return true;
}
