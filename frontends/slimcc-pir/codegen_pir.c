/*
 * PIR v0.3.0 HIR Code Generator for slimcc-pir.
 *
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause AND MIT
 *
 * Adapted from slimcc:
 *   Copyright (c) 2019 Rui Ueyama
 *   Copyright (c) 2023-2026 Hsiang-Ying Fu
 *   MIT License (see LICENSE).
 */
#include "slimcc.h"

#include <ctype.h>
#include <math.h>

/* =========================================================================
 * 1. Dynamic String Buffer & Module-Level Registries
 * ========================================================================= */

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} SBuf;

static void sb_init(SBuf *sb) {
  sb->buf = NULL;
  sb->len = 0;
  sb->cap = 0;
}

static void sb_reserve(SBuf *sb, size_t extra) {
  if (sb->len + extra + 1 <= sb->cap)
    return;
  size_t ncap = sb->cap ? sb->cap * 2 : 1024;
  while (ncap < sb->len + extra + 1)
    ncap *= 2;
  sb->buf = realloc(sb->buf, ncap);
  if (!sb->buf)
    abort();
  sb->cap = ncap;
}

static void sb_append(SBuf *sb, const char *s, size_t n) {
  sb_reserve(sb, n);
  memcpy(sb->buf + sb->len, s, n);
  sb->len += n;
  sb->buf[sb->len] = '\0';
}

static void sb_printf(SBuf *sb, const char *fmt, ...) FMTCHK(2, 3);
static void sb_printf(SBuf *sb, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    abort();
  }
  sb_reserve(sb, (size_t)n);
  vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  sb->len += (size_t)n;
}

static void sb_free(SBuf *sb) {
  free(sb->buf);
  sb->buf = NULL;
  sb->len = 0;
  sb->cap = 0;
}

typedef struct {
  const char *sym_name;
  char *buf;
  size_t len;
} StaticLvarOut;

/* Backend per-function output stored on Obj::output */
struct FuncObj {
  char *buf;
  size_t buflen;
  StaticLvarOut *static_lvars;
  int static_lvar_cnt;
  Obj **refs;
  int ref_cnt;
  int ref_cap;
};

static void emit_global_def(SBuf *sb, Obj *var);

/* Registered PIR structural type declarations (`type @T_N = ...`) */
typedef struct {
  char *name;
  char *def_str;
} PirTypeEntry;

static PirTypeEntry *pir_types;
static int pir_type_cnt;
static int pir_type_cap;
static HashMap pir_type_by_def;

/* Registered PIR indirect call signatures (`sig @S_N = ...`) */
typedef struct {
  char *name;
  char *def_str;
} PirSigEntry;

static PirSigEntry *pir_sigs;
static int pir_sig_cnt;
static int pir_sig_cap;
static HashMap pir_sig_by_def;

/* Debug file table */
static bool *debug_file_used;
static int *debug_file_id;
static int debug_file_cap;
static StringArray debug_files;

static bool need_atomic_lock_helpers;

/* =========================================================================
 * 2. Alignment & Interface Helpers
 * ========================================================================= */

static void *pir_xrealloc(void *p, size_t sz) {
  void *res = realloc(p, sz);
  if (!res && sz > 0)
    error("out of memory");
  return res;
}

static int32_t align_floor_pow2(int32_t al) {
  if (al <= 1)
    return 1;
  int32_t p2 = 1;
  while (p2 < (1 << 28) && p2 * 2 <= al)
    p2 *= 2;
  return p2;
}

static int32_t align_ceil_pow2(int32_t al, int32_t max_al) {
  if (al <= 1)
    return 1;
  if (max_al <= 0 || max_al > (1 << 28))
    max_al = (1 << 28);
  int32_t p2 = 1;
  while (p2 < al && p2 < max_al)
    p2 <<= 1;
  return p2;
}

static void ensure_debug_file_cap(int need) {
  if (need < debug_file_cap)
    return;
  int old_cap = debug_file_cap;
  int new_cap = old_cap ? old_cap * 2 : 16;
  while (new_cap <= need)
    new_cap *= 2;
  debug_file_used = pir_xrealloc(debug_file_used, (size_t)new_cap * sizeof(bool));
  debug_file_id = pir_xrealloc(debug_file_id, (size_t)new_cap * sizeof(int));
  memset(debug_file_used + old_cap, 0, (size_t)(new_cap - old_cap) * sizeof(bool));
  debug_file_cap = new_cap;
}

int64_t align_to(int64_t n, int64_t align) {
  if (align <= 1)
    return n;
  return (n + align - 1) / align * align;
}

bool bitint_rtn_need_copy(size_t width) {
  return width > 64;
}

/* =========================================================================
 * 3. Symbol Name Mangling & Escaping
 * ========================================================================= */

static bool is_bare_pir_ident(const char *s) {
  if (!s || !s[0])
    return false;
  if (!(isalpha((unsigned char)s[0]) || s[0] == '_'))
    return false;
  for (const char *p = s + 1; *p; p++) {
    if (!(isalnum((unsigned char)*p) || *p == '_'))
      return false;
  }
  return true;
}

static const char *get_raw_c_sym(Obj *var) {
  const char *raw = var->asm_name ? var->asm_name : var->name;
  if (!raw || !raw[0])
    return "";
  if (var->asm_name) {
    if (raw[0] == '\x01')
      raw++;
    /* On Mach-O (aarch64-apple-darwin), pirc prepends '_' automatically;
     * macOS SDK __DARWIN_ALIAS macros emit __asm("_sym"). Strip 1 leading '_'. */
    if (opt_pir_target && strstr(opt_pir_target, "darwin") && raw[0] == '_')
      raw++;
  }
  return raw;
}

static const char *pir_sym_name(Obj *var) {
  const char *raw = get_raw_c_sym(var);
  if (is_bare_pir_ident(raw)) {
    return raw;
  }

  /* Check if we already assigned a clean name for this exact Obj* via var->ptr
   * (var->ptr is unused on globals/functions/static_lvars in PIR mode!). */
  if (var->kind != OBJ_LOCAL && var->kind != OBJ_INDIR && var->ptr)
    return var->ptr;

  SBuf sb;
  sb_init(&sb);
  if (raw[0] == '.' && !var->asm_name) {
    /* Internal compiler label like .L..0 or .L.data.3 -> __pir_L_... */
    sb_printf(&sb, "__pir_");
    for (const char *p = raw; *p; p++) {
      if (isalnum((unsigned char)*p) || *p == '_')
        sb_printf(&sb, "%c", *p);
      else
        sb_printf(&sb, "_");
    }
  } else {
    /* External/user symbol with special chars: keep raw, quoted at emit. */
    sb_printf(&sb, "%s", raw[0] ? raw : "__pir_anon");
  }
  char *res = arena_strdup(&cc1_arena, sb.buf ? sb.buf : "__pir_anon");
  sb_free(&sb);
  if (var->kind != OBJ_LOCAL && var->kind != OBJ_INDIR)
    var->ptr = res;
  return res;
}

static void sb_emit_sym(SBuf *sb, const char *name) {
  if (is_bare_pir_ident(name)) {
    sb_printf(sb, "@%s", name);
  } else {
    sb_printf(sb, "@\"");
    for (const char *p = name; *p; p++) {
      unsigned char c = (unsigned char)*p;
      if (c == '"' || c == '\\')
        sb_printf(sb, "\\%c", (char)c);
      else if (c == '\n')
        sb_printf(sb, "\\n");
      else if (c == '\r')
        sb_printf(sb, "\\r");
      else if (c == '\t')
        sb_printf(sb, "\\t");
      else if (c < 32 || c >= 127)
        sb_printf(sb, "\\x%02x", c);
      else
        sb_printf(sb, "%c", (char)c);
    }
    sb_printf(sb, "\"");
  }
}

static void sb_emit_escaped_bytes(SBuf *sb, const char *s, size_t len, bool escape_nul) {
  sb_printf(sb, "\"");
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\\')
      sb_printf(sb, "\\\\");
    else if (c == '"')
      sb_printf(sb, "\\\"");
    else if (c == '\n')
      sb_printf(sb, "\\n");
    else if (c == '\r')
      sb_printf(sb, "\\r");
    else if (c == '\t')
      sb_printf(sb, "\\t");
    else if (escape_nul && c == 0)
      sb_printf(sb, "\\0");
    else if (c >= 32 && c < 127)
      sb_printf(sb, "%c", (char)c);
    else
      sb_printf(sb, "\\x%02x", c);
  }
  sb_printf(sb, "\"");
}

static void sb_emit_escaped_str(SBuf *sb, const char *s) {
  sb_emit_escaped_bytes(sb, s, strlen(s), false);
}

/* =========================================================================
 * 4. Type Classification & PIR Structural Type / Signature Registration
 * ========================================================================= */

static bool is_agg_ty(Type *ty) {
  return ty->kind == TY_STRUCT || ty->kind == TY_UNION;
}

static bool is_bitint_mem(Type *ty) {
  return ty->kind == TY_BITINT && ty->bit_cnt > 64;
}

static bool is_mem_val_ty(Type *ty) {
  return is_agg_ty(ty) || is_bitint_mem(ty);
}

static int64_t agg_copy_size(Type *ty) {
  if (ty && ty->is_flexible && ty->origin && ty->origin->size > 0)
    return ty->origin->size;
  return ty ? ty->size : 0;
}

static const char *pir_scalar_ty_name(Type *ty) {
  switch (ty->kind) {
  case TY_VOID:     return "void";
  case TY_BOOL:
  case TY_CHAR:
  case TY_PCHAR:    return "i8";
  case TY_SHORT:    return "i16";
  case TY_INT:      return "i32";
  case TY_LONG:
  case TY_LONGLONG: return "i64";
  case TY_FLOAT:    return "f32";
  case TY_DOUBLE:
  case TY_LDOUBLE:  return "f64";
  case TY_PTR:
  case TY_NULLPTR:
  case TY_ARRAY:
  case TY_VLA:
  case TY_FUNC:     return "ptr";
  case TY_BITINT:
    if (ty->bit_cnt <= 8) return "i8";
    if (ty->bit_cnt <= 16) return "i16";
    if (ty->bit_cnt <= 32) return "i32";
    if (ty->bit_cnt <= 64) return "i64";
    return "ptr";
  case TY_STRUCT:
  case TY_UNION:
    return "ptr";
  default:
    return "i64";
  }
}

static bool is_darwin_a64_bitint_i32_abi(Type *ty) {
  if (!ty || ty->kind != TY_BITINT)
    return false;
  if (is_x64_pir_target())
    return false;
  if (opt_pir_target && strstr(opt_pir_target, "darwin") == NULL)
    return false;
  return ty->bit_cnt < 32 && ty->bit_cnt != 1 && ty->bit_cnt != 8 && ty->bit_cnt != 16;
}

static const char *pir_abi_scalar_ty_name(Type *ty) {
  if (is_darwin_a64_bitint_i32_abi(ty))
    return "i32";
  return pir_scalar_ty_name(ty);
}

static const char *pir_ret_ty_name(Type *rty) {
  if (!rty || rty->kind == TY_VOID || (is_agg_ty(rty) && rty->size <= 0))
    return "void";
  return pir_abi_scalar_ty_name(rty);
}

static int pir_scalar_align(Type *ty) {
  if (ty->align > 0 && (ty->align & (ty->align - 1)) == 0)
    return ty->align;
  switch (ty->kind) {
  case TY_BOOL:
  case TY_CHAR:
  case TY_PCHAR:    return 1;
  case TY_SHORT:    return 2;
  case TY_INT:
  case TY_FLOAT:    return 4;
  default:          return 8;
  }
}

/* Detect AAPCS64 HFA (1..4 homogeneous float or double elements, no padding/bitfields). */
typedef struct {
  bool valid;
  TypeKind elem_kind; /* TY_FLOAT or TY_DOUBLE */
  int count;
} HfaCheck;

static bool is_unnamed_bf_only_agg_rec(Type *ty, int depth) {
  if (depth > 256 || !ty || (ty->kind != TY_STRUCT && ty->kind != TY_UNION) || ty->size <= 0)
    return false;
  for (Member *m = ty->members; m; m = m->next) {
    if (m->is_bitfield) {
      if (m->name && m->bit_width > 0)
        return false;
      continue;
    }
    if (m->ty->size <= 0)
      continue;
    if (m->ty->kind == TY_ARRAY) {
      Type *elem = m->ty;
      int ad = depth;
      while (elem && elem->kind == TY_ARRAY && ad++ < 256)
        elem = elem->base;
      if (!elem || elem->size <= 0 || is_unnamed_bf_only_agg_rec(elem, ad + 1))
        continue;
      return false;
    }
    if ((m->ty->kind == TY_STRUCT || m->ty->kind == TY_UNION) &&
        is_unnamed_bf_only_agg_rec(m->ty, depth + 1))
      continue;
    return false;
  }
  return true;
}

static bool is_unnamed_bf_only_agg(Type *ty) {
  return is_unnamed_bf_only_agg_rec(ty, 0);
}

static int32_t c_agg_member_natural_align_rec(Type *ty, int depth) {
  if (!ty || depth > 256)
    return 1;
  if (ty->kind == TY_ARRAY)
    return c_agg_member_natural_align_rec(ty->base, depth + 1);
  if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
    return ty->align > 0 ? ty->align : 1;
  int32_t max_al = 1;
  for (Member *m = ty->members; m; m = m->next) {
    if (m->is_bitfield && !m->name)
      continue;
    int32_t ma = m->is_packed ? 1 : (m->ty->align > 0 ? m->ty->align : 1);
    if (m->alt_align > ma)
      ma = m->alt_align;
    if (ma > max_al)
      max_al = ma;
  }
  return max_al;
}

static int32_t c_agg_member_natural_align(Type *ty) {
  return c_agg_member_natural_align_rec(ty, 0);
}

static void check_hfa_walk_rec(Type *ty, int64_t base_off, HfaCheck *hc, int depth) {
  if (!hc->valid)
    return;
  if (depth > 256) {
    hc->valid = false;
    return;
  }
  if (ty->qual & Q_ATOMIC) {
    hc->valid = false;
    return;
  }
  if (ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE || ty->kind == TY_LDOUBLE) {
    TypeKind ek = (ty->kind == TY_FLOAT) ? TY_FLOAT : TY_DOUBLE;
    int64_t al = (ek == TY_FLOAT) ? 4 : 8;
    if ((base_off % al) != 0) {
      hc->valid = false;
      return;
    }
    if (hc->count == 0)
      hc->elem_kind = ek;
    else if (hc->elem_kind != ek) {
      hc->valid = false;
      return;
    }
    hc->count++;
    if (hc->count > 4)
      hc->valid = false;
    return;
  }
  if (ty->kind == TY_ARRAY) {
    if (ty->array_len > 0 && ty->size == 0)
      return;
    if (ty->array_len <= 0) {
      hc->valid = false;
      return;
    }
    for (int64_t i = 0; i < ty->array_len && hc->valid; i++)
      check_hfa_walk_rec(ty->base, base_off + i * ty->base->size, hc, depth + 1);
    return;
  }
  if (ty->kind == TY_STRUCT) {
    for (Member *m = ty->members; m && hc->valid; m = m->next) {
      if (m->is_bitfield && m->bit_width == 0)
        continue;
      if (m->is_bitfield) {
        hc->valid = false;
        return;
      }
      if ((m->ty->kind == TY_STRUCT || m->ty->kind == TY_UNION) && m->ty->size == 0)
        continue;
      check_hfa_walk_rec(m->ty, base_off + m->offset, hc, depth + 1);
    }
    return;
  }
  if (ty->kind == TY_UNION) {
    if (!ty->members) {
      hc->valid = false;
      return;
    }
    int max_cnt = 0;
    for (Member *m = ty->members; m && hc->valid; m = m->next) {
      if (m->is_bitfield && m->bit_width == 0)
        continue;
      if (m->is_bitfield) {
        hc->valid = false;
        return;
      }
      if (m->ty->size <= 0 || is_unnamed_bf_only_agg(m->ty))
        continue;
      HfaCheck mc = {.valid = true, .elem_kind = TY_VOID, .count = 0};
      check_hfa_walk_rec(m->ty, base_off + m->offset, &mc, depth + 1);
      int64_t esz = (mc.elem_kind == TY_FLOAT) ? 4 : 8;
      if (!mc.valid || mc.count < 1 || mc.count > 4 || m->ty->size != mc.count * esz) {
        hc->valid = false;
        return;
      }
      if (hc->count == 0 && max_cnt == 0)
        hc->elem_kind = mc.elem_kind;
      else if (hc->elem_kind != mc.elem_kind) {
        hc->valid = false;
        return;
      }
      if (mc.count > max_cnt)
        max_cnt = mc.count;
    }
    if (max_cnt == 0) {
      hc->valid = false;
      return;
    }
    if (hc->valid) {
      hc->count += max_cnt;
      if (hc->count > 4)
        hc->valid = false;
    }
    return;
  }
  hc->valid = false;
}

static void check_hfa_walk(Type *ty, int64_t base_off, HfaCheck *hc) {
  check_hfa_walk_rec(ty, base_off, hc, 0);
}

static const char *register_pir_type_def(const char *def_str) {
  const char *existing = hashmap_get(&pir_type_by_def, def_str);
  if (existing)
    return existing;

  if (pir_type_cnt == pir_type_cap) {
    pir_type_cap = pir_type_cap ? pir_type_cap * 2 : 32;
    pir_types = realloc(pir_types, (size_t)pir_type_cap * sizeof(PirTypeEntry));
  }
  int id = pir_type_cnt++;
  char *name = arena_format(&cc1_arena, "T_%d", id);
  char *def = arena_strdup(&cc1_arena, def_str);
  pir_types[id].name = name;
  pir_types[id].def_str = def;
  hashmap_put(&pir_type_by_def, def, name);
  return name;
}

/* SysV AMD64 §3.2.3 eightbyte classification for <=16B structs/unions. */
typedef struct {
  int cls[2];     /* 0 = NO_CLASS, 1 = SSE, 2 = INTEGER */
  int f32_cnt[2];
  int f64_cnt[2];
  bool unaligned;
} SysvX64Walk;

static int sysv_merge_cls(int a, int b) {
  if (a == b) return a;
  if (a == 0) return b;
  if (b == 0) return a;
  if (a == 2 || b == 2) return 2;
  return 1;
}

static void walk_sysv_x64_agg_rec(Type *ty, int64_t base_off, SysvX64Walk *w, int depth);
static void walk_sysv_x64_agg(Type *ty, int64_t base_off, SysvX64Walk *w) {
  walk_sysv_x64_agg_rec(ty, base_off, w, 0);
}
static void walk_sysv_x64_agg_rec(Type *ty, int64_t base_off, SysvX64Walk *w, int depth) {
  if (w->unaligned || !ty)
    return;
  if (depth > 256) {
    w->unaligned = true;
    return;
  }
  if (ty->qual & Q_ATOMIC) {
    w->unaligned = true;
    return;
  }
  if (ty->is_flexible) {
    w->unaligned = true;
    return;
  }
  if (ty->align > 1 && (base_off % ty->align) != 0) {
    w->unaligned = true;
    return;
  }
  if (ty->kind == TY_FLOAT) {
    if ((base_off % 4) != 0) {
      w->unaligned = true;
      return;
    }
    int64_t eb = base_off / 8;
    if (eb < 2) {
      w->cls[eb] = sysv_merge_cls(w->cls[eb], 1);
      w->f32_cnt[eb]++;
    }
    return;
  }
  if (ty->kind == TY_DOUBLE || ty->kind == TY_LDOUBLE) {
    if ((base_off % 8) != 0) {
      w->unaligned = true;
      return;
    }
    int64_t eb = base_off / 8;
    if (eb < 2) {
      w->cls[eb] = sysv_merge_cls(w->cls[eb], 1);
      w->f64_cnt[eb]++;
    }
    return;
  }
  if (ty->kind == TY_ARRAY) {
    if (ty->array_len <= 0 || !ty->base || ty->base->size <= 0)
      return;
    for (int64_t i = 0; i < ty->array_len && !w->unaligned; i++)
      walk_sysv_x64_agg_rec(ty->base, base_off + i * ty->base->size, w, depth + 1);
    return;
  }
  if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
    for (Member *m = ty->members; m && !w->unaligned; m = m->next) {
      if (m->is_bitfield) {
        if (m->bit_width > 0 && m->name) {
          int64_t b_start = base_off + m->offset + (m->bit_offset / 8);
          int64_t b_end = base_off + m->offset + ((m->bit_offset + m->bit_width - 1) / 8);
          for (int64_t eb = b_start / 8; eb <= b_end / 8 && eb < 2; eb++)
            w->cls[eb] = sysv_merge_cls(w->cls[eb], 2);
        }
        continue;
      }
      if (m->alt_align > 1 && ((base_off + m->offset) % m->alt_align) != 0) {
        w->unaligned = true;
        return;
      }
      walk_sysv_x64_agg_rec(m->ty, base_off + m->offset, w, depth + 1);
    }
    return;
  }
  /* Scalar integer / pointer / bool / enum */
  int64_t sz = ty->size > 0 ? ty->size : 1;
  int64_t al = ty->align > 0 ? ty->align : 1;
  if (al > 1 && (base_off % al) != 0) {
    w->unaligned = true;
    return;
  }
  int64_t eb0 = base_off / 8;
  int64_t eb1 = (base_off + sz - 1) / 8;
  for (int64_t eb = eb0; eb <= eb1 && eb < 2; eb++)
    w->cls[eb] = sysv_merge_cls(w->cls[eb], 2);
}

/* Build a PIR structural type `@T_N` whose size, alignment, and target C ABI
 * classification (AAPCS64 HFA or SysV AMD64 eightbytes) match C's `Type *ty`. */
static const char *get_pir_agg_type_name(Type *ty) {
  int64_t csz = agg_copy_size(ty);
  int64_t sz = csz > 0 ? csz : 1;
  int32_t al = ty->align > 0 ? ty->align : 1;

  /* 0. On SysV AMD64, <=16B aggregates with any SSE or NO_CLASS eightbyte
   *    (including mixed [INTEGER, SSE], [SSE, INTEGER], [NO_CLASS, SSE],
   *    [SSE, NO_CLASS], [INTEGER, NO_CLASS], and unnamed bitfield padding)
   *    must preserve exact eightbyte classification, size, and alignment. */
  if (is_x64_pir_target() && (ty->kind == TY_STRUCT || ty->kind == TY_UNION) &&
      csz > 0 && csz <= 16) {
    SysvX64Walk w = {0};
    walk_sysv_x64_agg(ty, 0, &w);
    if (w.unaligned) {
      /* SysV AMD64 §3.2.3: unaligned fields, _Atomic, or flexible array members
       * force the entire aggregate to MEMORY class (stack / sret). */
      SBuf sb;
      sb_init(&sb);
      sb_printf(&sb, "[%" PRIi64 " x i8]", sz);
      if (al > 1)
        sb_printf(&sb, ", align %d", align_ceil_pow2(al, 4096));
      sb_printf(&sb, ", memory");
      const char *res = register_pir_type_def(sb.buf);
      sb_free(&sb);
      return res;
    }
    bool has_special_eb = (w.cls[0] == 1 || w.cls[1] == 1 ||
                           w.cls[0] == 0 || (sz > 8 && w.cls[1] == 0));
    if (!w.unaligned && has_special_eb) {
      SBuf fields;
      sb_init(&fields);
      int32_t nat_al = 1;
      if (sz <= 8) {
        if (w.cls[0] == 0) {
          sb_printf(&fields, "pad %" PRIi64, sz);
        } else if (w.cls[0] == 1) {
          if (sz == 4) {
            sb_printf(&fields, "f32");
            nat_al = 4;
          } else if (al < 8) {
            if (sz == 8)
              sb_printf(&fields, "f32, f32");
            else
              sb_printf(&fields, "f32, pad %" PRIi64, sz - 4);
            nat_al = 4;
          } else {
            sb_printf(&fields, "f64");
            nat_al = 8;
          }
        } else {
          if (sz == 8 && al >= 8) { sb_printf(&fields, "i64"); nat_al = 8; }
          else if (sz == 8 && al >= 4) { sb_printf(&fields, "i32, i32"); nat_al = 4; }
          else if (sz == 4 && al >= 4) { sb_printf(&fields, "i32"); nat_al = 4; }
          else if (sz == 2 && al >= 2) { sb_printf(&fields, "i16"); nat_al = 2; }
          else { sb_printf(&fields, "[%" PRIi64 " x i8]", sz); }
        }
      } else {
        /* Eightbyte 0: occupies exact bytes 0..7 */
        if (w.cls[0] == 0) {
          sb_printf(&fields, "pad 8");
        } else if (w.cls[0] == 1) {
          if (al < 8) {
            sb_printf(&fields, "f32, f32");
            if (4 > nat_al) nat_al = 4;
          } else {
            sb_printf(&fields, "f64");
            if (8 > nat_al) nat_al = 8;
          }
        } else {
          if (al >= 8) {
            sb_printf(&fields, "i64");
            if (8 > nat_al) nat_al = 8;
          } else if (al >= 4) {
            sb_printf(&fields, "i32, i32");
            if (4 > nat_al) nat_al = 4;
          } else if (al >= 2) {
            sb_printf(&fields, "[4 x i16]");
            if (2 > nat_al) nat_al = 2;
          } else {
            sb_printf(&fields, "[8 x i8]");
          }
        }
        sb_printf(&fields, ", ");
        /* Eightbyte 1: occupies exact remaining bytes 8..(sz-1) */
        int64_t rem = sz - 8;
        if (w.cls[1] == 0) {
          sb_printf(&fields, "pad %" PRIi64, rem);
        } else if (w.cls[1] == 1) {
          if (rem == 4) {
            sb_printf(&fields, "f32");
            if (4 > nat_al) nat_al = 4;
          } else if (al < 8) {
            if (rem == 8)
              sb_printf(&fields, "f32, f32");
            else
              sb_printf(&fields, "f32, pad %" PRIi64, rem - 4);
            if (4 > nat_al) nat_al = 4;
          } else {
            sb_printf(&fields, "f64");
            if (8 > nat_al) nat_al = 8;
          }
        } else {
          if (rem == 8 && al >= 8) { sb_printf(&fields, "i64"); if (8 > nat_al) nat_al = 8; }
          else if (rem == 8 && al >= 4) { sb_printf(&fields, "i32, i32"); if (4 > nat_al) nat_al = 4; }
          else if (rem == 4 && al >= 4) { sb_printf(&fields, "i32"); if (4 > nat_al) nat_al = 4; }
          else if (rem == 2 && al >= 2) { sb_printf(&fields, "i16"); if (2 > nat_al) nat_al = 2; }
          else { sb_printf(&fields, "[%" PRIi64 " x i8]", rem); }
        }
      }
      SBuf sb;
      sb_init(&sb);
      bool need_packed = (al < nat_al);
      sb_printf(&sb, "%s{ %s }", need_packed ? "packed " : "", fields.buf ? fields.buf : "");
      int32_t eff_nat = need_packed ? 1 : nat_al;
      if (al > eff_nat)
        sb_printf(&sb, ", align %d", align_ceil_pow2(al, 4096));
      const char *res = register_pir_type_def(sb.buf);
      sb_free(&fields);
      sb_free(&sb);
      return res;
    }
  }

  /* 0b. On ARM64, aggregates containing only unnamed bitfields (and/or empty
   *     aggregates) are pure padding; Darwin AAPCS64 passes them in 0 registers. */
  if (!is_x64_pir_target() && is_unnamed_bf_only_agg(ty)) {
    SBuf sb;
    sb_init(&sb);
    sb_printf(&sb, "{ pad %" PRIi64 " }", sz);
    if (al > 1)
      sb_printf(&sb, ", align %d", align_ceil_pow2(al, 4096));
    const char *res = register_pir_type_def(sb.buf);
    sb_free(&sb);
    return res;
  }

  /* 1. Check for HFA (1..4 float or double elements, exact packed size) */
  int32_t mem_nat_al = c_agg_member_natural_align(ty);
  if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && csz > 0) {
    HfaCheck hc = {.valid = true, .elem_kind = TY_VOID, .count = 0};
    check_hfa_walk(ty, 0, &hc);
    int64_t esz = (hc.elem_kind == TY_FLOAT) ? 4 : 8;
    if (hc.valid && hc.count >= 1 && hc.count <= 4 && sz == hc.count * esz) {
      const char *et = (hc.elem_kind == TY_FLOAT) ? "f32" : "f64";
      bool need_packed = (al < esz);
      bool is_arm64_linux = !is_x64_pir_target() && opt_pir_target &&
                            strstr(opt_pir_target, "linux") != NULL;
      SBuf sb;
      sb_init(&sb);
      sb_printf(&sb, "%s{ ", need_packed ? "packed " : "");
      for (int i = 0; i < hc.count; i++) {
        if (i > 0)
          sb_printf(&sb, ", ");
        sb_printf(&sb, "%s", et);
      }
      sb_printf(&sb, " }");
      int64_t eff_nat = need_packed ? 1 : esz;
      int32_t emit_al = is_arm64_linux ? mem_nat_al : al;
      if (emit_al > eff_nat)
        sb_printf(&sb, ", align %d", align_ceil_pow2(emit_al, 4096));
      const char *res = register_pir_type_def(sb.buf);
      sb_free(&sb);
      return res;
    }
  }

  /* 2. Non-HFA aggregate: represent with exact (size, align) integer array/struct,
   *    using natural member alignment (NOT outer __attribute__((aligned(N)))) for
   *    element selection so Linux AAPCS64 natural alignment matches GCC/Clang! */
  SBuf sb;
  sb_init(&sb);
  int32_t elem_al = (al < mem_nat_al) ? al : mem_nat_al;
  int32_t nat_al = 1;
  if (elem_al >= 16 && (sz % 16) == 0) {
    sb_printf(&sb, "[%" PRIi64 " x i128]", sz / 16);
    nat_al = 16;
  } else if (elem_al >= 8 && (sz % 8) == 0) {
    sb_printf(&sb, "[%" PRIi64 " x i64]", sz / 8);
    nat_al = 8;
  } else if (elem_al >= 4 && (sz % 4) == 0) {
    sb_printf(&sb, "[%" PRIi64 " x i32]", sz / 4);
    nat_al = 4;
  } else if (elem_al >= 2 && (sz % 2) == 0) {
    sb_printf(&sb, "[%" PRIi64 " x i16]", sz / 2);
    nat_al = 2;
  } else {
    sb_printf(&sb, "[%" PRIi64 " x i8]", sz);
    nat_al = 1;
  }
  if (al > nat_al)
    sb_printf(&sb, ", align %d", align_ceil_pow2(al, 4096));
  const char *res = register_pir_type_def(sb.buf);
  sb_free(&sb);
  return res;
}

static const char *register_pir_sig(const char *def_str) {
  const char *existing = hashmap_get(&pir_sig_by_def, def_str);
  if (existing)
    return existing;

  if (pir_sig_cnt == pir_sig_cap) {
    pir_sig_cap = pir_sig_cap ? pir_sig_cap * 2 : 32;
    pir_sigs = realloc(pir_sigs, (size_t)pir_sig_cap * sizeof(PirSigEntry));
  }
  int id = pir_sig_cnt++;
  char *name = arena_format(&cc1_arena, "S_%d", id);
  char *def = arena_strdup(&cc1_arena, def_str);
  pir_sigs[id].name = name;
  pir_sigs[id].def_str = def;
  hashmap_put(&pir_sig_by_def, def, name);
  return name;
}

/* Format parameter type list + return type for `sig` declaration. */
static const char *pir_int_ext_attr(Type *ty) {
  if (!ty || !is_int_class(ty) || ty->size >= 4 || is_darwin_a64_bitint_i32_abi(ty))
    return "";
  if (ty->kind == TY_BOOL || ty->is_unsigned)
    return " zeroext";
  return " signext";
}

static const char *get_pir_sig_for_call(Type *fnty, Obj *call_args) {
  SBuf sb;
  sb_init(&sb);
  sb_printf(&sb, "(");
  bool first = true;

  Type *rty = fnty->return_ty;
  bool has_retbuf = is_mem_val_ty(rty) && rty->size > 0;
  if (has_retbuf) {
    sb_printf(&sb, "retbuf(@%s)", get_pir_agg_type_name(rty));
    first = false;
  }

  if (fnty->is_oldstyle) {
    /* Old-style K&R function pointer: derive fixed params from call-site args.
     * Append `, ...` on SysV x86-64 so the backend sets %al = fp_used per ABI. */
    for (Obj *a = call_args; a; a = a->param_next) {
      if (a->ty->size <= 0 && is_agg_ty(a->ty))
        continue;
      if (!first)
        sb_printf(&sb, ", ");
      first = false;
      if (is_mem_val_ty(a->ty))
        sb_printf(&sb, "byval(@%s)", get_pir_agg_type_name(unqual(a->ty)));
      else
        sb_printf(&sb, "%s%s", pir_abi_scalar_ty_name(a->ty), pir_int_ext_attr(a->ty));
    }
    if (is_x64_pir_target()) {
      if (!first)
        sb_printf(&sb, ", ");
      sb_printf(&sb, "...");
    }
  } else {
    for (Obj *p = fnty->param_list; p; p = p->param_next) {
      if (p->ty->size <= 0 && is_agg_ty(p->ty))
        continue;
      if (!first)
        sb_printf(&sb, ", ");
      first = false;
      if (is_mem_val_ty(p->ty))
        sb_printf(&sb, "byval(@%s)", get_pir_agg_type_name(p->ty));
      else
        sb_printf(&sb, "%s%s", pir_abi_scalar_ty_name(p->ty), pir_int_ext_attr(p->ty));
    }
    if (fnty->is_variadic) {
      if (!first)
        sb_printf(&sb, ", ");
      sb_printf(&sb, "...");
    }
  }

  sb_printf(&sb, ") -> %s cc \"c\"%s",
            has_retbuf ? "void" : pir_ret_ty_name(rty),
            has_retbuf ? "" : pir_int_ext_attr(rty));
  const char *res = register_pir_sig(sb.buf);
  sb_free(&sb);
  return res;
}

/* =========================================================================
 * 5. Per-Function Emitter State
 * ========================================================================= */

typedef struct {
  int id;          /* %t<id>, or -1 if none */
  const char *ty;  /* PIR type name */
} PirVal;

typedef struct {
  Obj *obj;
  int slot_id;       /* %loc_<slot_id> or %p_<slot_id> */
  int vla_save_slot; /* %loc_<vla_save_slot> for stacksave before VLA alloca_dyn, or -1 */
  bool is_byval_param;
  const char *pir_ty;
  int64_t size;
  int32_t align;
} LocalInfo;

typedef struct {
  Obj *fn;
  SBuf entry_sb;   /* Entry-block allocas & param stores (strictly dominating!) */
  SBuf body_sb;    /* Function body blocks */
  int next_val;
  int next_blk;
  int next_loc_id;
  bool terminated;

  bool has_retbuf;
  const char *ret_pir_ty;
  int vla_sp_slot; /* Entry slot storing stacksave ptr if fn->dealloc_vla */

  /* Local variable map: Obj* -> LocalInfo* (in current function's linear array) */
  LocalInfo *locals;
  int local_cnt;
  int local_cap;

  /* Pending LHS value for ND_NULL_EXPR inside ND_ARITH_ASSIGN / ND_POST_INCDEC */
  PirVal null_expr_val;
  bool has_null_expr_val;

  /* Local variable currently being brace-initialized by ND_INIT_SEQ */
  Obj *init_seq_var;

  /* Referenced module-level Obj*s (for liveness & extern emission) */
  Obj **refs;
  int ref_cnt;
  int ref_cap;

  /* Debug loc tracking */
  int last_file_no;
  int last_line_no;
} FnCtx;

static FnCtx *fc;

static PirVal pir_void_val(void) {
  PirVal v = {-1, "void"};
  return v;
}

static int new_tid(void) {
  return fc->next_val++;
}

static int new_bid(void) {
  return fc->next_blk++;
}

static void fc_add_ref(Obj *obj) {
  if (!obj)
    return;
  if (obj->is_static_lvar) {
    obj->is_live = true;
    return;
  }
  for (int i = 0; i < fc->ref_cnt; i++) {
    if (fc->refs[i] == obj)
      return;
  }
  if (fc->ref_cnt == fc->ref_cap) {
    fc->ref_cap = fc->ref_cap ? fc->ref_cap * 2 : 32;
    fc->refs = pir_xrealloc(fc->refs, (size_t)fc->ref_cap * sizeof(Obj *));
  }
  fc->refs[fc->ref_cnt++] = obj;
}

static void emit_loc_if_needed(Token *tok) {
  if (!opt_g || !tok || !tok->file || fc->terminated)
    return;
  int fno = tok->display_file_no;
  int lno = tok->display_line_no;
  if (fno < 0 || fno >= display_files.len || lno <= 0)
    return;
  if (fno == fc->last_file_no && lno == fc->last_line_no)
    return;
  fc->last_file_no = fno;
  fc->last_line_no = lno;
  ensure_debug_file_cap(fno);
  if (!debug_file_used[fno]) {
    debug_file_used[fno] = true;
    debug_file_id[fno] = debug_files.len;
    strarray_push(&debug_files, display_files.data[fno]);
  }
  sb_printf(&fc->body_sb, "    loc %d, %d, 1\n", debug_file_id[fno], lno);
}

static void ensure_open_block(void) {
  if (fc->terminated) {
    int b = new_bid();
    sb_printf(&fc->body_sb, "\n.dead_%d:\n", b);
    fc->terminated = false;
  }
}

static void emit_label(const char *lbl) {
  if (!fc->terminated)
    sb_printf(&fc->body_sb, "    jmp .%s\n", lbl);
  sb_printf(&fc->body_sb, "\n.%s:\n", lbl);
  fc->terminated = false;
}

static void emit_label_id(const char *pfx, int64_t id) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%s_%" PRIi64, pfx, id);
  emit_label(buf);
}

static void emit_jmp(const char *lbl) {
  if (!fc->terminated) {
    sb_printf(&fc->body_sb, "    jmp .%s\n", lbl);
    fc->terminated = true;
  }
}

static void emit_jmp_id(const char *pfx, int64_t id) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%s_%" PRIi64, pfx, id);
  emit_jmp(buf);
}

/* Allocate a fresh entry-block stack slot (strictly dominates entire function!). */
static int emit_entry_alloca(int64_t bytes, int32_t align) {
  if (bytes <= 0)
    bytes = 1;
  int32_t al = align_ceil_pow2(align, 1 << 20);
  int id = fc->next_loc_id++;
  if (al > 16) {
    /* Runtime over-alignment beyond 16-byte ABI stack alignment */
    int64_t total = bytes + (int64_t)al - 1;
    uint64_t mask = ~(uint64_t)(al - 1);
    sb_printf(&fc->entry_sb, "    %%loc_raw_%d = alloca [%" PRIi64 " x i8], align 16\n", id, total);
    sb_printf(&fc->entry_sb, "    %%loc_pi_%d = ptrtoint.i64 %%loc_raw_%d\n", id, id);
    sb_printf(&fc->entry_sb, "    %%loc_pa_%d = add.i64 %%loc_pi_%d, %d [wrap]\n", id, id, al - 1);
    sb_printf(&fc->entry_sb, "    %%loc_pm_%d = and.i64 %%loc_pa_%d, 0x%" PRIx64 "\n", id, id, mask);
    sb_printf(&fc->entry_sb, "    %%loc_%d = inttoptr.ptr %%loc_pm_%d\n", id, id);
  } else {
    sb_printf(&fc->entry_sb, "    %%loc_%d = alloca [%" PRIi64 " x i8], align %d\n", id, bytes, al);
  }
  return id;
}

static int emit_entry_scalar_alloca(const char *pir_ty, int64_t bytes, int32_t align) {
  int32_t al = align_ceil_pow2(align, 1 << 20);
  if (al > 16)
    return emit_entry_alloca(bytes > 0 ? bytes : 8, al);
  int id = fc->next_loc_id++;
  sb_printf(&fc->entry_sb, "    %%loc_%d = alloca %s, align %d\n", id, pir_ty, al);
  return id;
}

static LocalInfo *find_local(Obj *var) {
  for (int i = 0; i < fc->local_cnt; i++) {
    if (fc->locals[i].obj == var)
      return &fc->locals[i];
  }
  return NULL;
}

static LocalInfo *register_local(Obj *var, bool is_param) {
  LocalInfo *ex = find_local(var);
  if (ex)
    return ex;

  if (fc->local_cnt == fc->local_cap) {
    fc->local_cap = fc->local_cap ? fc->local_cap * 2 : 64;
    fc->locals = realloc(fc->locals, (size_t)fc->local_cap * sizeof(LocalInfo));
  }
  LocalInfo *li = &fc->locals[fc->local_cnt++];
  memset(li, 0, sizeof(*li));
  li->obj = var;
  li->vla_save_slot = -1;
  int32_t al = var->alt_align ? var->alt_align : (var->ty->align > 0 ? var->ty->align : 1);
  li->align = al;
  li->size = var->ty->size > 0 ? var->ty->size : 1;

  if (is_param && is_mem_val_ty(var->ty) && var->ty->size > 0) {
    li->is_byval_param = true;
    li->slot_id = fc->next_loc_id++;
    li->pir_ty = "ptr";
    return li;
  }

  if (is_mem_val_ty(var->ty) || var->ty->kind == TY_ARRAY || var->ty->kind == TY_VLA) {
    if (var->ty->kind == TY_ARRAY && var->ty->size >= 16 && al < 16)
      al = 16;
    li->align = al;
    li->slot_id = emit_entry_alloca(li->size, al);
    li->pir_ty = "ptr";
  } else {
    li->pir_ty = pir_scalar_ty_name(var->ty);
    int32_t min_al = pir_scalar_align(var->ty);
    if (al < min_al)
      al = min_al;
    li->align = al;
    li->slot_id = emit_entry_scalar_alloca(li->pir_ty, li->size, al);
  }
  return li;
}

static void collect_scope_locals(Scope *sc) {
  if (!sc)
    return;
  for (Obj *v = sc->locals; v; v = v->next) {
    if (v->kind == OBJ_LOCAL)
      register_local(v, false);
  }
  for (Scope *ch = sc->children; ch; ch = ch->sibling_next)
    collect_scope_locals(ch);
}

/* =========================================================================
 * 6. Expression & Statement Forward Declarations
 * ========================================================================= */

static PirVal gen_expr(Node *node);
static PirVal gen_addr(Node *node);
static void gen_stmt(Node *node);
static void gen_defr(Node *node);

/* =========================================================================
 * 7. Core Scalar / Cast / Memory / Bitfield Helpers
 * ========================================================================= */

static PirVal emit_const_int(const char *ty, uint64_t val) {
  ensure_open_block();
  if (!strcmp(ty, "i8"))
    val &= 0xffULL;
  else if (!strcmp(ty, "i16"))
    val &= 0xffffULL;
  else if (!strcmp(ty, "i32"))
    val &= 0xffffffffULL;
  int r = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = const.%s 0x%" PRIx64 "\n", r, ty, val);
  PirVal v = {r, ty};
  return v;
}

static PirVal emit_const_float(Type *ty, FPVal *fval) {
  ensure_open_block();
  if (ty->kind == TY_FLOAT) {
    if (fval->chunk32[0] == 0u) {
      int r = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = const.f32 0.0\n", r);
      PirVal v = {r, "f32"};
      return v;
    }
    int g = new_tid();
    int r = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = const.i32 0x%" PRIx32 "\n", g, fval->chunk32[0]);
    sb_printf(&fc->body_sb, "    %%t%d = bitcast.f32 %%t%d\n", r, g);
    PirVal v = {r, "f32"};
    return v;
  } else {
    uint64_t bits = 0;
    if (ty->kind == TY_LDOUBLE) {
      double d = (double)fval->ld;
      memcpy(&bits, &d, sizeof(bits));
    } else {
      bits = fval->chunk[0];
    }
    if (bits == 0u) {
      int r = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = const.f64 0.0\n", r);
      PirVal v = {r, "f64"};
      return v;
    }
    int g = new_tid();
    int r = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = const.i64 0x%" PRIx64 "\n", g, bits);
    sb_printf(&fc->body_sb, "    %%t%d = bitcast.f64 %%t%d\n", r, g);
    PirVal v = {r, "f64"};
    return v;
  }
}

static PirVal emit_to_bool_i8(PirVal v, Type *from_ty) {
  ensure_open_block();
  int r = new_tid();
  if (is_flonum(from_ty)) {
    const char *ft = (from_ty->kind == TY_FLOAT) ? "f32" : "f64";
    sb_printf(&fc->body_sb, "    %%t%d = fcmp_une.%s %%t%d, 0.0\n", r, ft, v.id);
  } else if (is_ptr(from_ty) || is_decay_ty(from_ty)) {
    sb_printf(&fc->body_sb, "    %%t%d = cmp_ne.ptr %%t%d, 0\n", r, v.id);
  } else {
    sb_printf(&fc->body_sb, "    %%t%d = cmp_ne.%s %%t%d, 0\n", r, v.ty, v.id);
  }
  PirVal res = {r, "i8"};
  return res;
}

static int pir_ty_bits(const char *t) {
  if (!strcmp(t, "i8")) return 8;
  if (!strcmp(t, "i16")) return 16;
  if (!strcmp(t, "i32") || !strcmp(t, "f32")) return 32;
  if (!strcmp(t, "i64") || !strcmp(t, "f64") || !strcmp(t, "ptr")) return 64;
  return 64;
}

static PirVal emit_bitint_norm(PirVal v, Type *ty) {
  if (!ty || ty->kind != TY_BITINT || ty->bit_cnt > 64)
    return v;
  int db = pir_ty_bits(v.ty);
  if ((int)ty->bit_cnt >= db)
    return v;
  int sh = db - (int)ty->bit_cnt;
  int t1 = new_tid();
  int t2 = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = shl.%s %%t%d, %d [mask]\n", t1, v.ty, v.id, sh);
  sb_printf(&fc->body_sb, "    %%t%d = %s.%s %%t%d, %d [mask]\n",
            t2, ty->is_unsigned ? "shr" : "sar", v.ty, t1, sh);
  PirVal res = {t2, v.ty};
  return res;
}

static PirVal emit_cast_val(PirVal v, Type *from_ty, Type *to_ty) {
  ensure_open_block();
  if (to_ty->kind == TY_VOID)
    return pir_void_val();
  if (is_bitint_mem(from_ty) || is_bitint_mem(to_ty)) {
    if (is_bitint_mem(from_ty) && is_bitint_mem(to_ty) &&
        from_ty->bit_cnt == to_ty->bit_cnt)
      return v;
    error("wide _BitInt(>64) conversions are not yet supported in PIR backend");
  }
  if (is_mem_val_ty(to_ty))
    return v;
  if (to_ty->kind == TY_BOOL)
    return emit_to_bool_i8(v, from_ty);

  const char *src_t = v.ty;
  const char *dst_t = pir_scalar_ty_name(to_ty);

  /* Pointer <-> Integer / Pointer */
  if (is_ptr(to_ty) || is_decay_ty(to_ty)) {
    if (!strcmp(src_t, "ptr"))
      return v;
    /* Int -> Ptr */
    int sb = pir_ty_bits(src_t);
    int cur = v.id;
    if (sb < 64) {
      int w = new_tid();
      bool is_u = from_ty->is_unsigned || from_ty->kind == TY_BOOL;
      sb_printf(&fc->body_sb, "    %%t%d = %s.i64 %%t%d\n", w, is_u ? "zext" : "sext", cur);
      cur = w;
    }
    int r = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = inttoptr.ptr %%t%d\n", r, cur);
    PirVal res = {r, "ptr"};
    return res;
  }

  if (!strcmp(src_t, "ptr")) {
    /* Ptr -> Int */
    int p64 = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = ptrtoint.i64 %%t%d\n", p64, v.id);
    if (!strcmp(dst_t, "i64")) {
      PirVal res = {p64, "i64"};
      return emit_bitint_norm(res, to_ty);
    }
    int r = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = trunc.%s %%t%d\n", r, dst_t, p64);
    PirVal res = {r, dst_t};
    return emit_bitint_norm(res, to_ty);
  }

  /* Float <-> Float */
  if (is_flonum(from_ty) && is_flonum(to_ty)) {
    if (!strcmp(src_t, dst_t))
      return v;
    int r = new_tid();
    if (!strcmp(src_t, "f32") && !strcmp(dst_t, "f64"))
      sb_printf(&fc->body_sb, "    %%t%d = fpext.f64 %%t%d\n", r, v.id);
    else
      sb_printf(&fc->body_sb, "    %%t%d = fptrunc.f32 %%t%d\n", r, v.id);
    PirVal res = {r, dst_t};
    return res;
  }

  /* Int -> Float */
  if (!is_flonum(from_ty) && is_flonum(to_ty)) {
    int r = new_tid();
    bool is_u = from_ty->is_unsigned || from_ty->kind == TY_BOOL;
    sb_printf(&fc->body_sb, "    %%t%d = %s.%s %%t%d\n", r, is_u ? "uitofp" : "sitofp", dst_t, v.id);
    PirVal res = {r, dst_t};
    return res;
  }

  /* Float -> Int */
  if (is_flonum(from_ty) && !is_flonum(to_ty)) {
    bool is_u = to_ty->is_unsigned;
    /* For i8/i16, convert to i32 first then truncate (matches C/ARM64/x86 fcvtzu w + uxtb) */
    if (!strcmp(dst_t, "i8") || !strcmp(dst_t, "i16")) {
      int w = new_tid();
      int r = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = %s.i32 %%t%d [sat]\n",
                w, is_u ? "fptoui" : "fptosi", v.id);
      sb_printf(&fc->body_sb, "    %%t%d = trunc.%s %%t%d\n", r, dst_t, w);
      PirVal res = {r, dst_t};
      return emit_bitint_norm(res, to_ty);
    }
    int r = new_tid();
    /* C standard float->int conversion (use [sat] for deterministic safe lowering) */
    sb_printf(&fc->body_sb, "    %%t%d = %s.%s %%t%d [sat]\n",
              r, is_u ? "fptoui" : "fptosi", dst_t, v.id);
    PirVal res = {r, dst_t};
    return emit_bitint_norm(res, to_ty);
  }

  /* Int -> Int */
  int sb = pir_ty_bits(src_t);
  int db = pir_ty_bits(dst_t);
  PirVal cur = v;
  if (sb < db) {
    int r = new_tid();
    bool is_u = from_ty->is_unsigned || from_ty->kind == TY_BOOL;
    sb_printf(&fc->body_sb, "    %%t%d = %s.%s %%t%d\n", r, is_u ? "zext" : "sext", dst_t, cur.id);
    cur.id = r;
    cur.ty = dst_t;
  } else if (sb > db) {
    int r = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = trunc.%s %%t%d\n", r, dst_t, cur.id);
    cur.id = r;
    cur.ty = dst_t;
  }

  /* Narrow _BitInt normalization if destination is sub-storage _BitInt(N) */
  return emit_bitint_norm(cur, to_ty);
}

typedef struct {
  int b_fast;
  int b_slow;
  int b_done;
} AtomicAlignBlocks;

static AtomicAlignBlocks emit_atomic_align_begin(PirVal addr, int64_t sz, const char *pfx) {
  need_atomic_lock_helpers = true;
  AtomicAlignBlocks ab = {new_bid(), new_bid(), new_bid()};
  int pi = new_tid(), mis = new_tid(), is_mis = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = ptrtoint.i64 %%t%d\n", pi, addr.id);
  sb_printf(&fc->body_sb, "    %%t%d = and.i64 %%t%d, %" PRIi64 "\n", mis, pi, sz - 1);
  sb_printf(&fc->body_sb, "    %%t%d = cmp_ne.i64 %%t%d, 0\n", is_mis, mis);
  sb_printf(&fc->body_sb, "    br %%t%d, .%s_slow_%d, .%s_fast_%d\n",
            is_mis, pfx, ab.b_slow, pfx, ab.b_fast);
  fc->terminated = true;
  sb_printf(&fc->body_sb, "\n.%s_fast_%d:\n", pfx, ab.b_fast);
  fc->terminated = false;
  return ab;
}

static void emit_atomic_align_slow(const AtomicAlignBlocks *ab, const char *pfx, int fast_val) {
  if (fast_val >= 0)
    sb_printf(&fc->body_sb, "    jmp .%s_done_%d(%%t%d)\n", pfx, ab->b_done, fast_val);
  else
    sb_printf(&fc->body_sb, "    jmp .%s_done_%d\n", pfx, ab->b_done);
  fc->terminated = true;
  sb_printf(&fc->body_sb, "\n.%s_slow_%d:\n", pfx, ab->b_slow);
  fc->terminated = false;
}

static void emit_atomic_align_end(const AtomicAlignBlocks *ab, const char *pfx,
                                  int slow_val, int done_val, const char *done_ty) {
  if (slow_val >= 0)
    sb_printf(&fc->body_sb, "    jmp .%s_done_%d(%%t%d)\n", pfx, ab->b_done, slow_val);
  else
    sb_printf(&fc->body_sb, "    jmp .%s_done_%d\n", pfx, ab->b_done);
  fc->terminated = true;
  if (done_val >= 0)
    sb_printf(&fc->body_sb, "\n.%s_done_%d(%%t%d: %s):\n", pfx, ab->b_done, done_val, done_ty);
  else
    sb_printf(&fc->body_sb, "\n.%s_done_%d:\n", pfx, ab->b_done);
  fc->terminated = false;
}

static PirVal emit_load_scalar(PirVal addr, Type *ty, int32_t align_hint) {
  ensure_open_block();
  const char *pt = pir_scalar_ty_name(ty);
  int32_t al = align_hint > 0 ? align_hint : pir_scalar_align(ty);
  int32_t p2 = align_floor_pow2(al);
  int r = new_tid();
  if (ty->qual & Q_ATOMIC) {
    int64_t sz = ty->size > 0 ? ty->size : (pir_ty_bits(pt) / 8);
    if (sz > 1 && sz <= 8) {
      AtomicAlignBlocks ab = emit_atomic_align_begin(addr, sz, "at");
      int rf = new_tid();
      if (!strcmp(pt, "f32") || !strcmp(pt, "f64")) {
        const char *it = !strcmp(pt, "f32") ? "i32" : "i64";
        int raw = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = atomic_load.%s %%t%d [seq_cst]\n", raw, it, addr.id);
        sb_printf(&fc->body_sb, "    %%t%d = bitcast.%s %%t%d\n", rf, pt, raw);
      } else {
        sb_printf(&fc->body_sb, "    %%t%d = atomic_load.%s %%t%d [seq_cst]\n", rf, pt, addr.id);
      }
      emit_atomic_align_slow(&ab, "at", rf);
      int slot = emit_entry_scalar_alloca(pt, sz, (int32_t)sz);
      int tp = new_tid(), sz_id = new_tid(), rs = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", tp, slot);
      sb_printf(&fc->body_sb, "    %%t%d = const.i64 %" PRIi64 "\n", sz_id, sz);
      sb_printf(&fc->body_sb, "    call @__pir_atomic_load_n(%%t%d, %%t%d, %%t%d)\n", tp, addr.id, sz_id);
      sb_printf(&fc->body_sb, "    %%t%d = load.%s %%t%d, align %" PRIi64 "\n", rs, pt, tp, sz);
      emit_atomic_align_end(&ab, "at", rs, r, pt);
    } else if (!strcmp(pt, "f32") || !strcmp(pt, "f64")) {
      const char *it = !strcmp(pt, "f32") ? "i32" : "i64";
      int raw = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = atomic_load.%s %%t%d [seq_cst]\n", raw, it, addr.id);
      sb_printf(&fc->body_sb, "    %%t%d = bitcast.%s %%t%d\n", r, pt, raw);
    } else {
      sb_printf(&fc->body_sb, "    %%t%d = atomic_load.%s %%t%d [seq_cst]\n", r, pt, addr.id);
    }
  } else if (ty->qual & Q_VOLATILE) {
    sb_printf(&fc->body_sb, "    %%t%d = load.%s %%t%d, align %d [volatile]\n", r, pt, addr.id, p2);
  } else {
    sb_printf(&fc->body_sb, "    %%t%d = load.%s %%t%d, align %d\n", r, pt, addr.id, p2);
  }
  PirVal res = {r, pt};
  return emit_bitint_norm(res, ty);
}

static void emit_store_scalar(PirVal val, PirVal addr, Type *ty, int32_t align_hint) {
  ensure_open_block();
  const char *pt = pir_scalar_ty_name(ty);
  int32_t al = align_hint > 0 ? align_hint : pir_scalar_align(ty);
  int32_t p2 = align_floor_pow2(al);
  if (ty->qual & Q_ATOMIC) {
    int64_t sz = ty->size > 0 ? ty->size : (pir_ty_bits(pt) / 8);
    if (sz > 1 && sz <= 8) {
      AtomicAlignBlocks ab = emit_atomic_align_begin(addr, sz, "at");
      if (!strcmp(pt, "f32") || !strcmp(pt, "f64")) {
        const char *it = !strcmp(pt, "f32") ? "i32" : "i64";
        int raw = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = bitcast.%s %%t%d\n", raw, it, val.id);
        sb_printf(&fc->body_sb, "    atomic_store.%s %%t%d, %%t%d [seq_cst]\n", it, raw, addr.id);
      } else {
        sb_printf(&fc->body_sb, "    atomic_store.%s %%t%d, %%t%d [seq_cst]\n", pt, val.id, addr.id);
      }
      emit_atomic_align_slow(&ab, "at", -1);
      int slot = emit_entry_scalar_alloca(pt, sz, (int32_t)sz);
      int tp = new_tid(), sz_id = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", tp, slot);
      sb_printf(&fc->body_sb, "    store.%s %%t%d, %%t%d, align %" PRIi64 "\n", pt, val.id, tp, sz);
      sb_printf(&fc->body_sb, "    %%t%d = const.i64 %" PRIi64 "\n", sz_id, sz);
      sb_printf(&fc->body_sb, "    call @__pir_atomic_store_n(%%t%d, %%t%d, %%t%d)\n", addr.id, tp, sz_id);
      emit_atomic_align_end(&ab, "at", -1, -1, NULL);
    } else if (!strcmp(pt, "f32") || !strcmp(pt, "f64")) {
      const char *it = !strcmp(pt, "f32") ? "i32" : "i64";
      int raw = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = bitcast.%s %%t%d\n", raw, it, val.id);
      sb_printf(&fc->body_sb, "    atomic_store.%s %%t%d, %%t%d [seq_cst]\n", it, raw, addr.id);
    } else {
      sb_printf(&fc->body_sb, "    atomic_store.%s %%t%d, %%t%d [seq_cst]\n", pt, val.id, addr.id);
    }
  } else if (ty->qual & Q_VOLATILE) {
    sb_printf(&fc->body_sb, "    store.%s %%t%d, %%t%d, align %d [volatile]\n",
              pt, val.id, addr.id, p2);
  } else {
    sb_printf(&fc->body_sb, "    store.%s %%t%d, %%t%d, align %d\n",
              pt, val.id, addr.id, p2);
  }
}

static PirVal emit_ptr_add_imm(PirVal base, int64_t off) {
  if (off == 0)
    return base;
  ensure_open_block();
  int r = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%t%d, %" PRIi64 " [inbounds]\n",
            r, base.id, off);
  PirVal res = {r, "ptr"};
  return res;
}

/* =========================================================================
 * 8. Bitfield Load / Store Lowering (Aligned & Unaligned)
 * ========================================================================= */

static PirVal emit_bitfield_load(PirVal addr, Member *mem, bool is_vol) {
  if (is_bitint_mem(mem->ty) || mem->bit_width > 64)
    error("wide _BitInt(>64) bit-field is not yet supported in PIR backend");
  ensure_open_block();
  const char *vsuf = is_vol ? " [volatile]" : "";
  int32_t fp = bitfield_footprint(mem);
  int32_t lo_bytes = fp > 8 ? 8 : fp;
  int acc = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = const.i64 0\n", acc);

  /* Read first min(fp, 8) bytes little-endian into an i64 accumulator */
  for (int32_t i = 0; i < lo_bytes; i++) {
    PirVal bp = emit_ptr_add_imm(addr, i);
    int b = new_tid();
    int b64 = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = load.i8 %%t%d, align 1%s\n", b, bp.id, vsuf);
    sb_printf(&fc->body_sb, "    %%t%d = zext.i64 %%t%d\n", b64, b);
    int sh = b64;
    if (i > 0) {
      sh = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = shl.i64 %%t%d, %d [mask]\n", sh, b64, i * 8);
    }
    int nacc = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = or.i64 %%t%d, %%t%d\n", nacc, acc, sh);
    acc = nacc;
  }

  /* Shift out bit_offset, merge 9th byte if bitfield crosses 64-bit boundary */
  if (mem->bit_offset > 0) {
    int s = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = shr.i64 %%t%d, %d [mask]\n", s, acc, mem->bit_offset);
    acc = s;
  }
  if (fp > 8) {
    PirVal bp8 = emit_ptr_add_imm(addr, 8);
    int b = new_tid();
    int b64 = new_tid();
    int sh = new_tid();
    int nacc = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = load.i8 %%t%d, align 1%s\n", b, bp8.id, vsuf);
    sb_printf(&fc->body_sb, "    %%t%d = zext.i64 %%t%d\n", b64, b);
    sb_printf(&fc->body_sb, "    %%t%d = shl.i64 %%t%d, %d [mask]\n", sh, b64, 64 - mem->bit_offset);
    sb_printf(&fc->body_sb, "    %%t%d = or.i64 %%t%d, %%t%d\n", nacc, acc, sh);
    acc = nacc;
  }
  if (mem->bit_width < 64) {
    int sh = 64 - mem->bit_width;
    int s1 = new_tid();
    int s2 = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = shl.i64 %%t%d, %d [mask]\n", s1, acc, sh);
    bool is_u = mem->ty->is_unsigned || mem->ty->kind == TY_BOOL;
    sb_printf(&fc->body_sb, "    %%t%d = %s.i64 %%t%d, %d [mask]\n",
              s2, is_u ? "shr" : "sar", s1, sh);
    acc = s2;
  }

  const char *dst_t = pir_scalar_ty_name(mem->ty);
  if (!strcmp(dst_t, "i64")) {
    PirVal res = {acc, "i64"};
    return res;
  }
  int tr = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = trunc.%s %%t%d\n", tr, dst_t, acc);
  PirVal res = {tr, dst_t};
  return res;
}

static PirVal emit_bitfield_store(PirVal val, PirVal addr, Member *mem, bool is_vol) {
  if (is_bitint_mem(mem->ty) || mem->bit_width > 64)
    error("wide _BitInt(>64) bit-field is not yet supported in PIR backend");
  ensure_open_block();
  const char *vsuf = is_vol ? " [volatile]" : "";
  /* Widen val to i64 */
  int v64 = val.id;
  if (strcmp(val.ty, "i64") != 0) {
    v64 = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = zext.i64 %%t%d\n", v64, val.id);
  }
  if (mem->ty->kind == TY_BOOL) {
    int nb = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = cmp_ne.i64 %%t%d, 0\n", nb, v64);
    v64 = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = zext.i64 %%t%d\n", v64, nb);
  }

  int32_t fp = bitfield_footprint(mem);
  int32_t lo_bytes = fp > 8 ? 8 : fp;
  /* Read current low bytes (0..lo_bytes-1) into i64 */
  int old = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = const.i64 0\n", old);
  for (int32_t i = 0; i < lo_bytes; i++) {
    PirVal bp = emit_ptr_add_imm(addr, i);
    int b = new_tid();
    int b64 = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = load.i8 %%t%d, align 1%s\n", b, bp.id, vsuf);
    sb_printf(&fc->body_sb, "    %%t%d = zext.i64 %%t%d\n", b64, b);
    int sh = b64;
    if (i > 0) {
      sh = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = shl.i64 %%t%d, %d [mask]\n", sh, b64, i * 8);
    }
    int nold = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = or.i64 %%t%d, %%t%d\n", nold, old, sh);
    old = nold;
  }

  uint64_t val_mask = (mem->bit_width >= 64) ? ~UINT64_C(0) : ((UINT64_C(1) << mem->bit_width) - 1);
  uint64_t field_mask = val_mask << mem->bit_offset;
  uint64_t clear_mask = ~field_mask;

  int cleared = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = and.i64 %%t%d, 0x%" PRIx64 "\n", cleared, old, clear_mask);
  int masked = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = and.i64 %%t%d, 0x%" PRIx64 "\n", masked, v64, val_mask);
  int shifted = masked;
  if (mem->bit_offset > 0) {
    shifted = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = shl.i64 %%t%d, %d [mask]\n",
              shifted, masked, mem->bit_offset);
  }
  int merged = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = or.i64 %%t%d, %%t%d\n", merged, cleared, shifted);

  /* Store `lo_bytes` bytes back */
  for (int32_t i = 0; i < lo_bytes; i++) {
    PirVal bp = emit_ptr_add_imm(addr, i);
    int src = merged;
    if (i > 0) {
      src = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = shr.i64 %%t%d, %d [mask]\n", src, merged, i * 8);
    }
    int b8 = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = trunc.i8 %%t%d\n", b8, src);
    sb_printf(&fc->body_sb, "    store.i8 %%t%d, %%t%d, align 1%s\n", b8, bp.id, vsuf);
  }

  /* If bitfield spans into 9th byte, update byte 8 */
  if (fp > 8) {
    int hi_bits = mem->bit_offset + mem->bit_width - 64;
    uint32_t hi_mask = (1u << hi_bits) - 1u;
    PirVal bp8 = emit_ptr_add_imm(addr, 8);
    int ob8 = new_tid();
    int cb8 = new_tid();
    int vhi = new_tid();
    int vhi8 = new_tid();
    int mhi8 = new_tid();
    int nb8 = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = load.i8 %%t%d, align 1%s\n", ob8, bp8.id, vsuf);
    sb_printf(&fc->body_sb, "    %%t%d = and.i8 %%t%d, 0x%x\n", cb8, ob8, (~hi_mask) & 0xffu);
    sb_printf(&fc->body_sb, "    %%t%d = shr.i64 %%t%d, %d [mask]\n", vhi, masked, 64 - mem->bit_offset);
    sb_printf(&fc->body_sb, "    %%t%d = trunc.i8 %%t%d\n", vhi8, vhi);
    sb_printf(&fc->body_sb, "    %%t%d = and.i8 %%t%d, 0x%x\n", mhi8, vhi8, hi_mask & 0xffu);
    sb_printf(&fc->body_sb, "    %%t%d = or.i8 %%t%d, %%t%d\n", nb8, cb8, mhi8);
    sb_printf(&fc->body_sb, "    store.i8 %%t%d, %%t%d, align 1%s\n", nb8, bp8.id, vsuf);
  }

  /* Return bitfield-truncated value (signed or unsigned per mem->ty) */
  int ret64 = v64;
  if (mem->bit_width < 64) {
    int sh = 64 - mem->bit_width;
    int s1 = new_tid();
    int s2 = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = shl.i64 %%t%d, %d [mask]\n", s1, v64, sh);
    bool is_u = mem->ty->is_unsigned || mem->ty->kind == TY_BOOL;
    sb_printf(&fc->body_sb, "    %%t%d = %s.i64 %%t%d, %d [mask]\n",
              s2, is_u ? "shr" : "sar", s1, sh);
    ret64 = s2;
  }
  const char *dst_t = pir_scalar_ty_name(mem->ty);
  if (!strcmp(dst_t, "i64")) {
    PirVal res = {ret64, "i64"};
    return res;
  }
  int tr = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = trunc.%s %%t%d\n", tr, dst_t, ret64);
  PirVal res = {tr, dst_t};
  return res;
}

static const char *atomic_int_ty_for_size(int64_t sz) {
  switch (sz) {
  case 1: return "i8";
  case 2: return "i16";
  case 4: return "i32";
  case 8: return "i64";
  default: return NULL;
  }
}

static int32_t effective_lval_align(Node *n) {
  if (!n || !n->ty)
    return 1;
  int32_t al = n->ty->align > 0 ? n->ty->align : 1;
  switch (n->kind) {
  case ND_MEMBER: {
    Member *m = n->m.member;
    if (m->is_packed)
      return 1;
    int32_t base_al = effective_lval_align(n->m.lhs);
    if (base_al < al)
      al = base_al;
    if (m->offset != 0) {
      int64_t low = m->offset & -m->offset;
      if (low < al)
        al = (int32_t)low;
    }
    return align_floor_pow2(al);
  }
  case ND_DEREF: {
    Node *p = n->m.lhs;
    while (p && p->kind == ND_CAST && is_ptr(p->ty))
      p = p->m.lhs;
    if (p && p->kind == ND_ADDR)
      return effective_lval_align(p->m.lhs);
    if (p && (p->kind == ND_ADD || p->kind == ND_SUB)) {
      Node *ptr_op = is_ptr(p->m.lhs->ty) ? p->m.lhs : (is_ptr(p->m.rhs->ty) ? p->m.rhs : NULL);
      if (ptr_op) {
        Node *base = ptr_op;
        while (base && base->kind == ND_CAST && is_ptr(base->ty))
          base = base->m.lhs;
        if (base && base->kind == ND_ADDR)
          base = base->m.lhs;
        int32_t base_al = effective_lval_align(base);
        if (base_al < al)
          al = base_al;
      }
    }
    return align_floor_pow2(al);
  }
  case ND_COND: {
    int32_t ta = effective_lval_align(n->ctrl.then);
    int32_t ea = effective_lval_align(n->ctrl.els);
    int32_t ma = ta < ea ? ta : ea;
    return align_floor_pow2(ma < al ? ma : al);
  }
  case ND_COMMA:
  case ND_CHAIN:
    return effective_lval_align(n->m.rhs);
  case ND_ASSIGN:
    return effective_lval_align(n->m.lhs);
  case ND_CAST:
    if (is_mem_val_ty(n->ty))
      return effective_lval_align(n->m.lhs);
    return align_floor_pow2(al);
  default:
    return align_floor_pow2(al);
  }
}

static int64_t atomic_unqual_size(Type *atomic_ty) {
  if (atomic_ty && atomic_ty->origin && atomic_ty->origin->size > 0 &&
      atomic_ty->origin->size < atomic_ty->size)
    return atomic_ty->origin->size;
  return atomic_ty ? atomic_ty->size : 0;
}

static PirVal emit_stage_padded_atomic_src(PirVal src, int64_t src_sz, int64_t dst_sz,
                                           int32_t dst_al) {
  int slot = emit_entry_alloca(dst_sz, dst_al);
  int p = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", p, slot);
  sb_printf(&fc->body_sb, "    memset %%t%d, 0, %" PRIi64 ", align %d\n", p, dst_sz,
            align_floor_pow2(dst_al));
  if (src_sz > 0) {
    sb_printf(&fc->body_sb, "    memcpy %%t%d, %%t%d, %" PRIi64 ", align 1\n", p, src.id, src_sz);
  }
  PirVal v = {p, "ptr"};
  return v;
}

static PirVal emit_load_node(Node *node, PirVal addr) {
  if (is_bitfield(node)) {
    bool is_vol = ((node->ty->qual & Q_VOLATILE) != 0) ||
                  ((node->m.member->ty->qual & Q_VOLATILE) != 0);
    return emit_bitfield_load(addr, node->m.member, is_vol);
  }
  if (is_mem_val_ty(node->ty) && (node->ty->qual & Q_ATOMIC)) {
    const char *it = atomic_int_ty_for_size(node->ty->size);
    if (it) {
      ensure_open_block();
      int64_t sz = node->ty->size;
      int32_t al = (int32_t)sz;
      int slot = emit_entry_alloca(sz, al);
      int tmp = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", tmp, slot);
      if (sz > 1) {
        AtomicAlignBlocks ab = emit_atomic_align_begin(addr, sz, "at");
        int raw = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = atomic_load.%s %%t%d [seq_cst]\n", raw, it, addr.id);
        sb_printf(&fc->body_sb, "    store.%s %%t%d, %%t%d, align %d\n", it, raw, tmp, al);
        emit_atomic_align_slow(&ab, "at", -1);
        int sz_id = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = const.i64 %" PRIi64 "\n", sz_id, sz);
        sb_printf(&fc->body_sb, "    call @__pir_atomic_load_n(%%t%d, %%t%d, %%t%d)\n",
                  tmp, addr.id, sz_id);
        emit_atomic_align_end(&ab, "at", -1, -1, NULL);
      } else {
        int raw = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = atomic_load.%s %%t%d [seq_cst]\n", raw, it, addr.id);
        sb_printf(&fc->body_sb, "    store.%s %%t%d, %%t%d, align %d\n", it, raw, tmp, al);
      }
      PirVal v = {tmp, "ptr"};
      return v;
    }
    if (node->ty->size > 0) {
      need_atomic_lock_helpers = true;
      ensure_open_block();
      int32_t al = node->ty->align > 0 ? node->ty->align : 1;
      int slot = emit_entry_alloca(node->ty->size, al);
      int tmp = new_tid();
      int sz_id = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", tmp, slot);
      sb_printf(&fc->body_sb, "    %%t%d = const.i64 %" PRIi64 "\n", sz_id, node->ty->size);
      sb_printf(&fc->body_sb, "    call @__pir_atomic_load_n(%%t%d, %%t%d, %%t%d)\n",
                tmp, addr.id, sz_id);
      PirVal v = {tmp, "ptr"};
      return v;
    }
  }
  if (is_mem_val_ty(node->ty) && (node->ty->qual & Q_VOLATILE) && node->ty->size > 0) {
    ensure_open_block();
    int32_t al = effective_lval_align(node);
    int slot = emit_entry_alloca(node->ty->size, al);
    int tmp = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", tmp, slot);
    int64_t off = 0;
    while (off < node->ty->size) {
      int64_t rem = node->ty->size - off;
      int32_t step = (rem >= 8 && al >= 8) ? 8
                   : (rem >= 4 && al >= 4) ? 4
                   : (rem >= 2 && al >= 2) ? 2 : 1;
      const char *ct = (step == 8) ? "i64" : (step == 4) ? "i32" : (step == 2) ? "i16" : "i8";
      PirVal sp = emit_ptr_add_imm(addr, off);
      PirVal dp = emit_ptr_add_imm((PirVal){tmp, "ptr"}, off);
      int lv = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = load.%s %%t%d, align %d [volatile]\n", lv, ct, sp.id, step);
      sb_printf(&fc->body_sb, "    store.%s %%t%d, %%t%d, align %d\n", ct, lv, dp.id, step);
      off += step;
    }
    PirVal v = {tmp, "ptr"};
    return v;
  }
  if (is_mem_val_ty(node->ty) || node->ty->kind == TY_ARRAY ||
      node->ty->kind == TY_VLA || node->ty->kind == TY_FUNC)
    return addr;
  int32_t al = effective_lval_align(node);
  return emit_load_scalar(addr, node->ty, al);
}

static PirVal emit_store_node(Node *lhs, PirVal val, PirVal addr, Node *rhs_node) {
  ensure_open_block();
  if (is_bitfield(lhs)) {
    bool is_vol = ((lhs->ty->qual & Q_VOLATILE) != 0) ||
                  ((lhs->m.member->ty->qual & Q_VOLATILE) != 0);
    return emit_bitfield_store(val, addr, lhs->m.member, is_vol);
  }
  if (is_mem_val_ty(lhs->ty) && (lhs->ty->qual & Q_ATOMIC)) {
    const char *it = atomic_int_ty_for_size(lhs->ty->size);
    int64_t orig_sz = atomic_unqual_size(lhs->ty);
    if (rhs_node && rhs_node->ty && rhs_node->ty->size > 0 && rhs_node->ty->size < orig_sz)
      orig_sz = rhs_node->ty->size;
    PirVal src_ptr = val;
    int32_t src_al = rhs_node ? effective_lval_align(rhs_node)
                    : ((lhs->ty->origin && lhs->ty->origin->align > 0)
                           ? align_floor_pow2(lhs->ty->origin->align)
                           : 1);
    if (orig_sz < lhs->ty->size) {
      int32_t stage_al = lhs->ty->align > 0 ? lhs->ty->align : 1;
      src_ptr = emit_stage_padded_atomic_src(val, orig_sz, lhs->ty->size, stage_al);
      src_al = align_floor_pow2(stage_al);
    }
    if (it) {
      int64_t sz = lhs->ty->size;
      if (sz > 1) {
        AtomicAlignBlocks ab = emit_atomic_align_begin(addr, sz, "at");
        int raw = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = load.%s %%t%d, align %d\n", raw, it, src_ptr.id, src_al);
        sb_printf(&fc->body_sb, "    atomic_store.%s %%t%d, %%t%d [seq_cst]\n", it, raw, addr.id);
        emit_atomic_align_slow(&ab, "at", -1);
        int sz_id = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = const.i64 %" PRIi64 "\n", sz_id, sz);
        sb_printf(&fc->body_sb, "    call @__pir_atomic_store_n(%%t%d, %%t%d, %%t%d)\n",
                  addr.id, src_ptr.id, sz_id);
        emit_atomic_align_end(&ab, "at", -1, -1, NULL);
      } else {
        int raw = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = load.%s %%t%d, align %d\n", raw, it, src_ptr.id, src_al);
        sb_printf(&fc->body_sb, "    atomic_store.%s %%t%d, %%t%d [seq_cst]\n", it, raw, addr.id);
      }
      return val;
    }
    if (lhs->ty->size > 0) {
      need_atomic_lock_helpers = true;
      int sz_id = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = const.i64 %" PRIi64 "\n", sz_id, lhs->ty->size);
      sb_printf(&fc->body_sb, "    call @__pir_atomic_store_n(%%t%d, %%t%d, %%t%d)\n",
                addr.id, src_ptr.id, sz_id);
      return val;
    }
  }
  if (is_mem_val_ty(lhs->ty) || lhs->ty->kind == TY_ARRAY) {
    bool is_vol = ((get_elem(lhs->ty)->qual & Q_VOLATILE) != 0);
    int64_t copy_sz = is_agg_ty(lhs->ty) ? agg_copy_size(lhs->ty) : lhs->ty->size;
    if (copy_sz > 0) {
      int32_t dst_al = effective_lval_align(lhs);
      int32_t src_al = rhs_node ? effective_lval_align(rhs_node) : dst_al;
      int32_t p2 = dst_al < src_al ? dst_al : src_al;
      if (is_vol) {
        int64_t off = 0;
        while (off < copy_sz) {
          int64_t rem = copy_sz - off;
          int32_t step = (rem >= 8 && p2 >= 8) ? 8
                       : (rem >= 4 && p2 >= 4) ? 4
                       : (rem >= 2 && p2 >= 2) ? 2 : 1;
          const char *ct = (step == 8) ? "i64" : (step == 4) ? "i32" : (step == 2) ? "i16" : "i8";
          PirVal sp = emit_ptr_add_imm(val, off);
          PirVal dp = emit_ptr_add_imm(addr, off);
          int lv = new_tid();
          sb_printf(&fc->body_sb, "    %%t%d = load.%s %%t%d, align %d\n", lv, ct, sp.id, step);
          sb_printf(&fc->body_sb, "    store.%s %%t%d, %%t%d, align %d [volatile]\n", ct, lv, dp.id, step);
          off += step;
        }
      } else {
        sb_printf(&fc->body_sb, "    memmove %%t%d, %%t%d, %" PRIi64 ", align %d\n",
                  addr.id, val.id, copy_sz, p2);
      }
    }
    return is_vol ? val : addr;
  }
  int32_t al = effective_lval_align(lhs);
  emit_store_scalar(val, addr, lhs->ty, al);
  return val;
}

/* =========================================================================
 * 9. Address Evaluation (`gen_addr`)
 * ========================================================================= */

static bool is_under_init_seq_var(Node *n, Obj *target) {
  if (!n || !target)
    return false;
  for (;;) {
    if (n->kind == ND_VAR)
      return n->m.var == target;
    if (n->kind == ND_MEMBER || n->kind == ND_DEREF || n->kind == ND_CAST || n->kind == ND_ADDR) {
      n = n->m.lhs;
      continue;
    }
    if (n->kind == ND_ADD || n->kind == ND_SUB) {
      if (is_under_init_seq_var(n->m.lhs, target))
        return true;
      n = n->m.rhs;
      continue;
    }
    return false;
  }
}

static PirVal gen_addr(Node *node) {
  ensure_open_block();
  switch (node->kind) {
  case ND_VAR: {
    Obj *var = node->m.var;
    if (var->kind == OBJ_INDIR) {
      LocalInfo *li = register_local(var->vptr, false);
      int r = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = load.ptr %%loc_%d, align 8\n", r, li->slot_id);
      PirVal v = {r, "ptr"};
      return v;
    }
    if (var->kind == OBJ_LOCAL) {
      LocalInfo *li = register_local(var, false);
      int r = new_tid();
      if (li->is_byval_param)
        sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%p_%d, 0\n", r, li->slot_id);
      else
        sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", r, li->slot_id);
      PirVal v = {r, "ptr"};
      return v;
    }
    /* OBJ_GLOBAL or OBJ_TLS */
    fc_add_ref(var);
    int r = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = addr ", r);
    sb_emit_sym(&fc->body_sb, pir_sym_name(var));
    sb_printf(&fc->body_sb, "\n");
    PirVal v = {r, "ptr"};
    return v;
  }
  case ND_DEREF:
    return gen_expr(node->m.lhs);
  case ND_COMMA:
    gen_expr(node->m.lhs);
    if (is_mem_val_ty(node->m.rhs->ty) && (node->m.rhs->ty->qual & (Q_ATOMIC | Q_VOLATILE)))
      return gen_expr(node->m.rhs);
    return gen_addr(node->m.rhs);
  case ND_CHAIN:
    gen_expr(node->m.lhs);
    return gen_addr(node->m.rhs);
  case ND_MEMBER: {
    bool is_init_target = is_under_init_seq_var(node->m.lhs, fc->init_seq_var);
    PirVal base = (!is_init_target && is_mem_val_ty(node->m.lhs->ty) &&
                   (node->m.lhs->ty->qual & Q_ATOMIC))
                      ? gen_expr(node->m.lhs)
                      : gen_addr(node->m.lhs);
    return emit_ptr_add_imm(base, node->m.member->offset);
  }
  case ND_ASSIGN:
  case ND_COND:
  case ND_CAST:
  case ND_FUNCALL:
  case ND_EXCH:
  case ND_STMT_EXPR:
    if (is_mem_val_ty(node->ty))
      return gen_expr(node);
    break;
  default:
    break;
  }
  error_tok(node->tok, "not an lvalue");
}

/* =========================================================================
 * 10. Expression Evaluation (`gen_expr`)
 * ========================================================================= */

static Obj *unwrap_direct_callee(Node *expr) {
  while (expr->kind == ND_CAST && is_ptr(expr->ty))
    expr = expr->m.lhs;
  if (expr->kind == ND_ADDR)
    expr = expr->m.lhs;
  if (expr->kind == ND_VAR && expr->m.var->ty->kind == TY_FUNC)
    return expr->m.var;
  return NULL;
}

static PirVal gen_funcall(Node *node) {
  ensure_open_block();
  Type *fnty = get_func_ty(node->call.expr);
  Obj *direct = unwrap_direct_callee(node->call.expr);

  /* Determine whether we can use direct `call @fn` or need `call_indirect @Sig`.
   * Use `call_indirect` if calling through a function pointer OR calling a K&R
   * old-style function with arguments (to match PIR's strict parameter arity). */
  bool use_direct = (direct != NULL) && !direct->asm_name &&
                    !direct->ty->is_oldstyle && !fnty->is_oldstyle &&
                    (direct->ty->is_variadic == fnty->is_variadic);
  if (use_direct) {
    /* Also verify call-site arg count matches fixed params (or >= if variadic) */
    int pcnt = 0;
    for (Obj *p = direct->ty->param_list; p; p = p->param_next)
      if (!(p->ty->size <= 0 && is_agg_ty(p->ty)))
        pcnt++;
    int acnt = 0;
    for (Obj *a = node->call.args; a; a = a->param_next)
      if (!(a->ty->size <= 0 && is_agg_ty(a->ty)))
        acnt++;
    if (direct->ty->is_variadic ? (acnt < pcnt) : (acnt != pcnt))
      use_direct = false;
    /* Also verify return-type & fixed parameter PIR types + signext/zeroext match */
    bool d_retbuf = is_mem_val_ty(direct->ty->return_ty) && direct->ty->return_ty->size > 0;
    bool c_retbuf = is_mem_val_ty(fnty->return_ty) && fnty->return_ty->size > 0;
    if (d_retbuf != c_retbuf) {
      use_direct = false;
    } else if (d_retbuf) {
      if (strcmp(get_pir_agg_type_name(direct->ty->return_ty),
                 get_pir_agg_type_name(fnty->return_ty)) != 0)
        use_direct = false;
    } else {
      if (strcmp(pir_ret_ty_name(direct->ty->return_ty),
                 pir_ret_ty_name(fnty->return_ty)) != 0 ||
          strcmp(pir_int_ext_attr(direct->ty->return_ty),
                 pir_int_ext_attr(fnty->return_ty)) != 0)
        use_direct = false;
    }
    if (use_direct) {
      Obj *dp = direct->ty->param_list;
      Obj *ca = node->call.args;
      while (dp && ca) {
        while (dp && dp->ty->size <= 0 && is_agg_ty(dp->ty)) dp = dp->param_next;
        while (ca && ca->ty->size <= 0 && is_agg_ty(ca->ty)) ca = ca->param_next;
        if (!dp || !ca) break;
        bool d_mem = is_mem_val_ty(dp->ty);
        bool c_mem = is_mem_val_ty(ca->ty);
        if (d_mem != c_mem) {
          use_direct = false;
          break;
        }
        if (d_mem) {
          if (strcmp(get_pir_agg_type_name(dp->ty), get_pir_agg_type_name(ca->ty)) != 0 &&
              !((dp->ty->qual & Q_ATOMIC) && is_compatible(unqual(dp->ty), unqual(ca->ty)))) {
            use_direct = false;
            break;
          }
        } else {
          if (strcmp(pir_abi_scalar_ty_name(dp->ty), pir_abi_scalar_ty_name(ca->ty)) != 0 ||
              strcmp(pir_int_ext_attr(dp->ty), pir_int_ext_attr(ca->ty)) != 0) {
            use_direct = false;
            break;
          }
        }
        dp = dp->param_next;
        ca = ca->param_next;
      }
    }
  }

  PirVal fn_ptr = pir_void_val();
  if (use_direct) {
    fc_add_ref(direct);
  } else {
    fn_ptr = gen_expr(node->call.expr);
  }

  /* Count & evaluate all arguments into SSA values BEFORE emitting the call. */
  int arg_cnt = 0;
  for (Obj *a = node->call.args; a; a = a->param_next)
    arg_cnt++;

  Type *sig_fnty = use_direct ? direct->ty : fnty;
  PirVal *arg_vals = arg_cnt ? calloc((size_t)arg_cnt, sizeof(PirVal)) : NULL;
  Type **arg_tys = arg_cnt ? calloc((size_t)arg_cnt, sizeof(Type *)) : NULL;
  Obj *fp_cur = (!sig_fnty->is_oldstyle) ? sig_fnty->param_list : NULL;
  int idx = 0;
  for (Obj *a = node->call.args; a; a = a->param_next, idx++) {
    arg_tys[idx] = a->ty;
    if (a->ty->size <= 0 && is_agg_ty(a->ty)) {
      gen_expr(a->arg_expr);
      arg_vals[idx] = pir_void_val();
    } else {
      while (fp_cur && fp_cur->ty->size <= 0 && is_agg_ty(fp_cur->ty))
        fp_cur = fp_cur->param_next;
      Obj *dp = fp_cur;
      if (fp_cur)
        fp_cur = fp_cur->param_next;

      PirVal av = gen_expr(a->arg_expr);
      /* Stage under-aligned or smaller non-atomic aggregate byval args into an aligned stack slot */
      if (is_mem_val_ty(a->ty)) {
        Type *dst_ty = (dp && is_mem_val_ty(dp->ty)) ? dp->ty : a->ty;
        int64_t asz = agg_copy_size(a->ty);
        int32_t req_al = align_floor_pow2(dst_ty->align > 0 ? dst_ty->align : 1);
        int32_t eff_al = effective_lval_align(a->arg_expr);
        if ((dst_ty->qual & Q_ATOMIC) && (asz < dst_ty->size || eff_al < req_al)) {
          av = emit_stage_padded_atomic_src(av, asz, dst_ty->size, req_al);
        } else if (asz > 0 && eff_al < req_al) {
          int slot = emit_entry_alloca(asz, req_al);
          int sp = new_tid();
          sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", sp, slot);
          sb_printf(&fc->body_sb, "    memcpy %%t%d, %%t%d, %" PRIi64 ", align %d\n",
                    sp, av.id, asz, eff_al);
          av.id = sp;
          av.ty = "ptr";
        }
      } else if (strcmp(av.ty, pir_scalar_ty_name(a->ty)) != 0) {
        av = emit_cast_val(av, a->arg_expr->ty, a->ty);
      }
      arg_vals[idx] = av;
    }
  }

  /* Count fixed parameters for variadic tagging */
  int fixed_cnt = 0;
  if (!sig_fnty->is_oldstyle) {
    for (Obj *p = sig_fnty->param_list; p; p = p->param_next)
      if (!(p->ty->size <= 0 && is_agg_ty(p->ty)))
        fixed_cnt++;
  } else {
    fixed_cnt = arg_cnt;
  }

  /* Promote narrow _BitInt arguments to i32 when required by Darwin ARM64 ABI
   * or when passed as variadic arguments (where PIR carries no signext tag). */
  {
    int p_idx = 0;
    for (int i = 0; i < arg_cnt; i++) {
      if (arg_tys[i]->size <= 0 && is_agg_ty(arg_tys[i]))
        continue;
      bool is_va_extra = sig_fnty->is_variadic && p_idx >= fixed_cnt;
      if (!is_mem_val_ty(arg_tys[i]) &&
          (is_darwin_a64_bitint_i32_abi(arg_tys[i]) ||
           (is_va_extra && arg_tys[i]->kind == TY_BITINT && arg_tys[i]->bit_cnt < 32)) &&
          strcmp(arg_vals[i].ty, "i32") != 0) {
        ensure_open_block();
        int ext = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = %s.i32 %%t%d\n", ext,
                  arg_tys[i]->is_unsigned ? "zext" : "sext", arg_vals[i].id);
        arg_vals[i].id = ext;
        arg_vals[i].ty = "i32";
      }
      p_idx++;
    }
  }

  Type *rty = node->ty;
  bool has_retbuf = is_mem_val_ty(rty) && rty->size > 0;
  LocalInfo *rb_li = NULL;
  if (has_retbuf) {
    if (!node->call.rtn_buf)
      internal_error();
    rb_li = register_local(node->call.rtn_buf, false);
  }

  bool has_scalar_ret = !has_retbuf && rty->kind != TY_VOID && !(is_agg_ty(rty) && rty->size == 0);
  int res_id = has_scalar_ret ? new_tid() : -1;

  ensure_open_block();
  sb_printf(&fc->body_sb, "    ");
  if (has_scalar_ret)
    sb_printf(&fc->body_sb, "%%t%d = ", res_id);

  if (use_direct) {
    sb_printf(&fc->body_sb, "call ");
    sb_emit_sym(&fc->body_sb, pir_sym_name(direct));
    sb_printf(&fc->body_sb, "(");
  } else {
    const char *sig_name = get_pir_sig_for_call(fnty, node->call.args);
    sb_printf(&fc->body_sb, "call_indirect @%s, %%t%d(", sig_name, fn_ptr.id);
  }

  bool first = true;
  if (has_retbuf) {
    sb_printf(&fc->body_sb, "%%loc_%d", rb_li->slot_id);
    first = false;
  }
  int passed = 0;
  for (int i = 0; i < arg_cnt; i++) {
    if (arg_tys[i]->size <= 0 && is_agg_ty(arg_tys[i]))
      continue;
    if (!first)
      sb_printf(&fc->body_sb, ", ");
    first = false;
    bool is_va_extra = sig_fnty->is_variadic && passed >= fixed_cnt;
    if (is_mem_val_ty(arg_tys[i])) {
      if (is_va_extra)
        sb_printf(&fc->body_sb, "%%t%d: byval(@%s)", arg_vals[i].id,
                  get_pir_agg_type_name(unqual(arg_tys[i])));
      else
        sb_printf(&fc->body_sb, "%%t%d", arg_vals[i].id);
    } else {
      sb_printf(&fc->body_sb, "%%t%d", arg_vals[i].id);
    }
    passed++;
  }
  sb_printf(&fc->body_sb, ")\n");

  free(arg_vals);
  free(arg_tys);

  if (has_retbuf) {
    int rp = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", rp, rb_li->slot_id);
    PirVal v = {rp, "ptr"};
    return v;
  }
  if (has_scalar_ret) {
    const char *st = pir_scalar_ty_name(rty);
    if (is_darwin_a64_bitint_i32_abi(rty) && strcmp(st, "i32") != 0) {
      int tr = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = trunc.%s %%t%d\n", tr, st, res_id);
      PirVal v = {tr, st};
      return emit_bitint_norm(v, rty);
    }
    PirVal v = {res_id, st};
    return emit_bitint_norm(v, rty);
  }
  return pir_void_val();
}

static PirVal gen_expr_with_null_lhs(NodeKind kind, Node *orig_lhs, Type *ty, Node *rhs, PirVal lhs_val) {
  Type *null_ty = ty;
  PirVal promoted_val = lhs_val;
  if (orig_lhs && is_bitfield(orig_lhs) && ty->kind != TY_BITINT) {
    int bw = orig_lhs->m.member->bit_width;
    if (bw < 32) {
      null_ty = ty_int;
    } else if (bw == 32) {
      null_ty = ty->is_unsigned ? ty_uint : ty_int;
    }
    if (null_ty != ty && strcmp(lhs_val.ty, pir_scalar_ty_name(null_ty)) != 0)
      promoted_val = emit_cast_val(lhs_val, ty, null_ty);
  }

  PirVal saved = fc->null_expr_val;
  bool saved_has = fc->has_null_expr_val;
  fc->null_expr_val = promoted_val;
  fc->has_null_expr_val = true;

  Node null = {.kind = ND_NULL_EXPR, .ty = null_ty, .tok = rhs->tok};
  Node expr = {.kind = kind, .m.lhs = &null, .m.rhs = rhs, .tok = rhs->tok};
  add_type(&expr);
  Node *casted = new_cast(&expr, ty);
  PirVal res = gen_expr(casted);

  fc->null_expr_val = saved;
  fc->has_null_expr_val = saved_has;
  return res;
}

static PirVal gen_ckd_arith(Node *node) {
  /* Evaluate checked overflow arithmetic using PIR HIR i128 arithmetic:
   * widen lhs & rhs to i128, perform exact op, truncate to res_ty,
   * re-extend back to i128, overflow = (reextended != exact). */
  ensure_open_block();
  Type *res_ty = node->m.target->ty->base;
  if (is_bitint_mem(res_ty) || is_bitint_mem(node->m.lhs->ty) || is_bitint_mem(node->m.rhs->ty))
    error_tok(node->tok, "wide _BitInt(>64) is not supported in checked arithmetic");
  PirVal dst_ptr = gen_expr(node->m.target);
  PirVal lv = gen_expr(node->m.lhs);
  PirVal rv = gen_expr(node->m.rhs);

  int l128 = new_tid();
  int r128 = new_tid();
  bool lu = node->m.lhs->ty->is_unsigned || node->m.lhs->ty->kind == TY_BOOL;
  bool ru = node->m.rhs->ty->is_unsigned || node->m.rhs->ty->kind == TY_BOOL;
  sb_printf(&fc->body_sb, "    %%t%d = %s.i128 %%t%d\n", l128, lu ? "zext" : "sext", lv.id);
  sb_printf(&fc->body_sb, "    %%t%d = %s.i128 %%t%d\n", r128, ru ? "zext" : "sext", rv.id);

  const char *op = (node->arith_kind == ND_ADD) ? "add"
                 : (node->arith_kind == ND_SUB) ? "sub" : "mul";
  int exact = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = %s.i128 %%t%d, %%t%d [wrap]\n", exact, op, l128, r128);

  const char *rt = pir_scalar_ty_name(res_ty);
  int trunc_v = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = trunc.%s %%t%d\n", trunc_v, rt, exact);
  PirVal store_v = emit_bitint_norm((PirVal){trunc_v, rt}, res_ty);
  emit_store_scalar(store_v, dst_ptr, res_ty, res_ty->align);

  int back128 = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = %s.i128 %%t%d\n",
            back128, res_ty->is_unsigned ? "zext" : "sext", store_v.id);
  int ovf = new_tid();
  sb_printf(&fc->body_sb, "    %%t%d = cmp_ne.i128 %%t%d, %%t%d\n", ovf, back128, exact);
  PirVal res = {ovf, "i8"};
  return res;
}

static bool is_direct_scalar_local(Node *node, LocalInfo **out_li) {
  if (!node || node->kind != ND_VAR)
    return false;
  Obj *var = node->m.var;
  if (var->kind != OBJ_LOCAL)
    return false;
  if (is_mem_val_ty(node->ty) || node->ty->kind == TY_ARRAY ||
      node->ty->kind == TY_VLA || node->ty->kind == TY_FUNC)
    return false;
  LocalInfo *li = register_local(var, false);
  if (li->is_byval_param)
    return false;
  *out_li = li;
  return true;
}

static PirVal emit_load_local_slot(LocalInfo *li, Type *ty) {
  ensure_open_block();
  const char *pt = pir_scalar_ty_name(ty);
  int32_t al = li->align > 0 ? li->align : pir_scalar_align(ty);
  int32_t p2 = align_floor_pow2(al);
  int r = new_tid();
  if (ty->qual & Q_ATOMIC) {
    if (!strcmp(pt, "f32") || !strcmp(pt, "f64")) {
      const char *it = !strcmp(pt, "f32") ? "i32" : "i64";
      int raw = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = atomic_load.%s %%loc_%d [seq_cst]\n", raw, it, li->slot_id);
      sb_printf(&fc->body_sb, "    %%t%d = bitcast.%s %%t%d\n", r, pt, raw);
    } else {
      sb_printf(&fc->body_sb, "    %%t%d = atomic_load.%s %%loc_%d [seq_cst]\n", r, pt, li->slot_id);
    }
  } else if (ty->qual & Q_VOLATILE) {
    sb_printf(&fc->body_sb, "    %%t%d = load.%s %%loc_%d, align %d [volatile]\n", r, pt, li->slot_id, p2);
  } else {
    sb_printf(&fc->body_sb, "    %%t%d = load.%s %%loc_%d, align %d\n", r, pt, li->slot_id, p2);
  }
  PirVal res = {r, pt};
  return emit_bitint_norm(res, ty);
}

static void emit_store_local_slot(PirVal val, LocalInfo *li, Type *ty) {
  ensure_open_block();
  const char *pt = pir_scalar_ty_name(ty);
  int32_t al = li->align > 0 ? li->align : pir_scalar_align(ty);
  int32_t p2 = align_floor_pow2(al);
  if (ty->qual & Q_ATOMIC) {
    if (!strcmp(pt, "f32") || !strcmp(pt, "f64")) {
      const char *it = !strcmp(pt, "f32") ? "i32" : "i64";
      int raw = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = bitcast.%s %%t%d\n", raw, it, val.id);
      sb_printf(&fc->body_sb, "    atomic_store.%s %%t%d, %%loc_%d [seq_cst]\n", it, raw, li->slot_id);
    } else {
      sb_printf(&fc->body_sb, "    atomic_store.%s %%t%d, %%loc_%d [seq_cst]\n", pt, val.id, li->slot_id);
    }
  } else if (ty->qual & Q_VOLATILE) {
    sb_printf(&fc->body_sb, "    store.%s %%t%d, %%loc_%d, align %d [volatile]\n", pt, val.id, li->slot_id, p2);
  } else {
    sb_printf(&fc->body_sb, "    store.%s %%t%d, %%loc_%d, align %d\n", pt, val.id, li->slot_id, p2);
  }
}

static void name_labels(Node *lbl);

static PirVal gen_expr(Node *node) {
  if (!node)
    return pir_void_val();
  emit_loc_if_needed(node->tok);
  ensure_open_block();

  switch (node->kind) {
  case ND_NULL_EXPR:
    if (fc->has_null_expr_val) {
      fc->has_null_expr_val = false;
      return fc->null_expr_val;
    }
    return pir_void_val();

  case ND_UNREACHABLE:
    sb_printf(&fc->body_sb, "    unreachable\n");
    fc->terminated = true;
    return pir_void_val();

  case ND_NUM: {
    if (is_flonum(node->ty)) {
      FPVal fval = {0};
      eval_fp(node, &fval);
      return emit_const_float(node->ty, &fval);
    }
    if (is_ptr(node->ty)) {
      int r = new_tid();
      if (node->num.val == 0) {
        sb_printf(&fc->body_sb, "    %%t%d = const.ptr 0\n", r);
      } else {
        int iv = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = const.i64 0x%" PRIx64 "\n", iv, (uint64_t)node->num.val);
        sb_printf(&fc->body_sb, "    %%t%d = inttoptr.ptr %%t%d\n", r, iv);
      }
      PirVal v = {r, "ptr"};
      return v;
    }
    if (node->ty->kind == TY_BITINT) {
      if (node->ty->bit_cnt <= 64) {
        uint64_t val = node->num.bitint_data ? node->num.bitint_data->as64 : (uint64_t)node->num.val;
        return emit_const_int(pir_scalar_ty_name(node->ty), val);
      }
      int slot = emit_entry_alloca(node->ty->size, node->ty->align);
      int32_t cnt = (int32_t)((node->ty->bit_cnt + 63) / 64);
      int p = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", p, slot);
      for (int32_t i = 0; i < cnt; i++) {
        uint64_t w = node->num.bitint_data ? (&node->num.bitint_data->as64)[i]
                                           : (i == 0 ? (uint64_t)node->num.val : 0);
        PirVal cp = emit_ptr_add_imm((PirVal){p, "ptr"}, i * 8);
        PirVal cw = emit_const_int("i64", w);
        sb_printf(&fc->body_sb, "    store.i64 %%t%d, %%t%d, align 8\n", cw.id, cp.id);
      }
      PirVal v = {p, "ptr"};
      return v;
    }
    return emit_const_int(pir_scalar_ty_name(node->ty), (uint64_t)node->num.val);
  }

  case ND_POS:
    return gen_expr(node->m.lhs);

  case ND_NEG: {
    if (is_bitint_mem(node->ty))
      error_tok(node->tok, "wide _BitInt(>64) arithmetic is not yet supported in PIR backend");
    PirVal lv = gen_expr(node->m.lhs);
    ensure_open_block();
    int r = new_tid();
    if (is_flonum(node->ty)) {
      sb_printf(&fc->body_sb, "    %%t%d = fneg.%s %%t%d\n", r, lv.ty, lv.id);
    } else {
      sb_printf(&fc->body_sb, "    %%t%d = sub.%s 0, %%t%d [wrap]\n", r, lv.ty, lv.id);
    }
    PirVal v = {r, lv.ty};
    return emit_bitint_norm(v, node->ty);
  }

  case ND_BITNOT: {
    if (is_bitint_mem(node->ty))
      error_tok(node->tok, "wide _BitInt(>64) arithmetic is not yet supported in PIR backend");
    PirVal lv = gen_expr(node->m.lhs);
    ensure_open_block();
    int r = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = xor.%s %%t%d, -1\n", r, lv.ty, lv.id);
    PirVal v = {r, lv.ty};
    return emit_bitint_norm(v, node->ty);
  }

  case ND_NOT: {
    if (is_bitint_mem(node->m.lhs->ty))
      error_tok(node->tok, "wide _BitInt(>64) boolean conversion is not yet supported in PIR backend");
    PirVal lv = gen_expr(node->m.lhs);
    PirVal b = emit_to_bool_i8(lv, node->m.lhs->ty);
    int inv = new_tid();
    int r = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = cmp_eq.i8 %%t%d, 0\n", inv, b.id);
    sb_printf(&fc->body_sb, "    %%t%d = zext.i32 %%t%d\n", r, inv);
    PirVal v = {r, "i32"};
    return v;
  }

  case ND_VAR: {
    LocalInfo *li = NULL;
    if (is_direct_scalar_local(node, &li))
      return emit_load_local_slot(li, node->ty);
    PirVal a = gen_addr(node);
    return emit_load_node(node, a);
  }
  case ND_MEMBER: {
    PirVal a = gen_addr(node);
    return emit_load_node(node, a);
  }

  case ND_DEREF: {
    PirVal a = gen_expr(node->m.lhs);
    return emit_load_node(node, a);
  }

  case ND_ADDR:
    return gen_addr(node->m.lhs);

  case ND_ASSIGN: {
    LocalInfo *li = NULL;
    if (is_direct_scalar_local(node->m.lhs, &li)) {
      PirVal rhs = gen_expr(node->m.rhs);
      if (strcmp(rhs.ty, pir_scalar_ty_name(node->m.lhs->ty)) != 0)
        rhs = emit_cast_val(rhs, node->m.rhs->ty, node->m.lhs->ty);
      emit_store_local_slot(rhs, li, node->m.lhs->ty);
      return rhs;
    }
    PirVal addr = gen_addr(node->m.lhs);
    PirVal rhs = gen_expr(node->m.rhs);
    if (!is_mem_val_ty(node->m.lhs->ty) && node->m.lhs->ty->kind != TY_ARRAY &&
        strcmp(rhs.ty, pir_scalar_ty_name(node->m.lhs->ty)) != 0)
      rhs = emit_cast_val(rhs, node->m.rhs->ty, node->m.lhs->ty);
    return emit_store_node(node->m.lhs, rhs, addr, node->m.rhs);
  }

  case ND_ARITH_ASSIGN: {
    LocalInfo *li = NULL;
    if (is_direct_scalar_local(node->m.lhs, &li)) {
      PirVal old = emit_load_local_slot(li, node->m.lhs->ty);
      PirVal nv = gen_expr_with_null_lhs(node->arith_kind, node->m.lhs, node->m.lhs->ty, node->m.rhs, old);
      emit_store_local_slot(nv, li, node->m.lhs->ty);
      return nv;
    }
    PirVal addr = gen_addr(node->m.lhs);
    PirVal old = emit_load_node(node->m.lhs, addr);
    PirVal nv = gen_expr_with_null_lhs(node->arith_kind, node->m.lhs, node->m.lhs->ty, node->m.rhs, old);
    return emit_store_node(node->m.lhs, nv, addr, NULL);
  }

  case ND_POST_INCDEC: {
    LocalInfo *li = NULL;
    if (is_direct_scalar_local(node->m.lhs, &li)) {
      PirVal old = emit_load_local_slot(li, node->m.lhs->ty);
      PirVal nv = gen_expr_with_null_lhs(ND_ADD, node->m.lhs, node->m.lhs->ty, node->m.rhs, old);
      emit_store_local_slot(nv, li, node->m.lhs->ty);
      return old;
    }
    PirVal addr = gen_addr(node->m.lhs);
    PirVal old = emit_load_node(node->m.lhs, addr);
    PirVal nv = gen_expr_with_null_lhs(ND_ADD, node->m.lhs, node->m.lhs->ty, node->m.rhs, old);
    emit_store_node(node->m.lhs, nv, addr, NULL);
    return old;
  }

  case ND_CKD_ARITH:
    return gen_ckd_arith(node);

  case ND_COMMA:
  case ND_CHAIN:
    gen_expr(node->m.lhs);
    return gen_expr(node->m.rhs);

  case ND_CAST: {
    PirVal lv = gen_expr(node->m.lhs);
    return emit_cast_val(lv, node->m.lhs->ty, node->ty);
  }

  case ND_INIT_SEQ: {
    LocalInfo *li = register_local(node->m.var, false);
    ensure_open_block();
    int32_t al = li->align > 0 ? li->align : 1;
    int32_t p2 = align_floor_pow2(al);
    if (node->m.var->ty->size > 0) {
      if (get_elem(node->m.var->ty)->qual & Q_VOLATILE) {
        int base_p = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", base_p, li->slot_id);
        int64_t off = 0;
        while (off < node->m.var->ty->size) {
          int64_t rem = node->m.var->ty->size - off;
          int32_t step = (rem >= 8 && p2 >= 8) ? 8
                       : (rem >= 4 && p2 >= 4) ? 4
                       : (rem >= 2 && p2 >= 2) ? 2 : 1;
          const char *ct = (step == 8) ? "i64" : (step == 4) ? "i32" : (step == 2) ? "i16" : "i8";
          PirVal dp = emit_ptr_add_imm((PirVal){base_p, "ptr"}, off);
          sb_printf(&fc->body_sb, "    store.%s 0, %%t%d, align %d [volatile]\n", ct, dp.id, step);
          off += step;
        }
      } else {
        sb_printf(&fc->body_sb, "    memset %%loc_%d, 0, %" PRIi64 ", align %d\n",
                  li->slot_id, node->m.var->ty->size, p2);
      }
    }
    Obj *saved_init_var = fc->init_seq_var;
    fc->init_seq_var = node->m.var;
    for (Node *n = node->m.lhs; n; n = n->next)
      gen_expr(n);
    fc->init_seq_var = saved_init_var;
    return pir_void_val();
  }

  case ND_COND: {
    int b_then = new_bid();
    int b_else = new_bid();
    int b_end = new_bid();
    PirVal cv = gen_expr(node->ctrl.cond);
    PirVal cb = emit_to_bool_i8(cv, node->ctrl.cond->ty);
    sb_printf(&fc->body_sb, "    br %%t%d, .cond_t_%d, .cond_e_%d\n", cb.id, b_then, b_else);
    fc->terminated = true;

    bool is_void = (node->ty->kind == TY_VOID) || (is_agg_ty(node->ty) && node->ty->size == 0);
    const char *rty = is_void ? "void" : pir_scalar_ty_name(node->ty);

    sb_printf(&fc->body_sb, "\n.cond_t_%d:\n", b_then);
    fc->terminated = false;
    PirVal tv = gen_expr(node->ctrl.then);
    if (!fc->terminated) {
      if (is_void)
        sb_printf(&fc->body_sb, "    jmp .cond_end_%d\n", b_end);
      else
        sb_printf(&fc->body_sb, "    jmp .cond_end_%d(%%t%d)\n", b_end, tv.id);
      fc->terminated = true;
    }

    sb_printf(&fc->body_sb, "\n.cond_e_%d:\n", b_else);
    fc->terminated = false;
    PirVal ev = gen_expr(node->ctrl.els);
    if (!fc->terminated) {
      if (is_void)
        sb_printf(&fc->body_sb, "    jmp .cond_end_%d\n", b_end);
      else
        sb_printf(&fc->body_sb, "    jmp .cond_end_%d(%%t%d)\n", b_end, ev.id);
      fc->terminated = true;
    }

    if (is_void) {
      sb_printf(&fc->body_sb, "\n.cond_end_%d:\n", b_end);
      fc->terminated = false;
      return pir_void_val();
    }
    int res = new_tid();
    sb_printf(&fc->body_sb, "\n.cond_end_%d(%%t%d: %s):\n", b_end, res, rty);
    fc->terminated = false;
    PirVal v = {res, rty};
    return v;
  }

  case ND_LOGAND: {
    int b_rhs = new_bid();
    int b_end = new_bid();
    PirVal lv = gen_expr(node->m.lhs);
    PirVal lb = emit_to_bool_i8(lv, node->m.lhs->ty);
    sb_printf(&fc->body_sb, "    br %%t%d, .land_r_%d, .land_end_%d(0)\n", lb.id, b_rhs, b_end);
    fc->terminated = true;

    sb_printf(&fc->body_sb, "\n.land_r_%d:\n", b_rhs);
    fc->terminated = false;
    PirVal rv = gen_expr(node->m.rhs);
    PirVal rb = emit_to_bool_i8(rv, node->m.rhs->ty);
    if (!fc->terminated) {
      sb_printf(&fc->body_sb, "    jmp .land_end_%d(%%t%d)\n", b_end, rb.id);
      fc->terminated = true;
    }

    int bi = new_tid();
    int res = new_tid();
    sb_printf(&fc->body_sb, "\n.land_end_%d(%%t%d: i8):\n", b_end, bi);
    fc->terminated = false;
    sb_printf(&fc->body_sb, "    %%t%d = zext.i32 %%t%d\n", res, bi);
    PirVal v = {res, "i32"};
    return v;
  }

  case ND_LOGOR: {
    int b_rhs = new_bid();
    int b_end = new_bid();
    PirVal lv = gen_expr(node->m.lhs);
    PirVal lb = emit_to_bool_i8(lv, node->m.lhs->ty);
    sb_printf(&fc->body_sb, "    br %%t%d, .lor_end_%d(1), .lor_r_%d\n", lb.id, b_end, b_rhs);
    fc->terminated = true;

    sb_printf(&fc->body_sb, "\n.lor_r_%d:\n", b_rhs);
    fc->terminated = false;
    PirVal rv = gen_expr(node->m.rhs);
    PirVal rb = emit_to_bool_i8(rv, node->m.rhs->ty);
    if (!fc->terminated) {
      sb_printf(&fc->body_sb, "    jmp .lor_end_%d(%%t%d)\n", b_end, rb.id);
      fc->terminated = true;
    }

    int bi = new_tid();
    int res = new_tid();
    sb_printf(&fc->body_sb, "\n.lor_end_%d(%%t%d: i8):\n", b_end, bi);
    fc->terminated = false;
    sb_printf(&fc->body_sb, "    %%t%d = zext.i32 %%t%d\n", res, bi);
    PirVal v = {res, "i32"};
    return v;
  }

  case ND_STMT_EXPR: {
    name_labels(node->blk.local_labels);
    for (Node *n = node->blk.body; n != node->blk.result; n = n->next)
      gen_stmt(n);
    PirVal res = pir_void_val();
    if (node->blk.result)
      res = gen_expr(node->blk.result->m.lhs);
    gen_defr(node);
    return res;
  }

  case ND_FUNCALL:
    return gen_funcall(node);

  case ND_ALLOCA:
  case ND_ALLOCA_ZINIT: {
    PirVal sz = gen_expr(node->m.lhs);
    if (strcmp(sz.ty, "i64") != 0)
      sz = emit_cast_val(sz, node->m.lhs->ty, ty_ulong);
    ensure_open_block();
    int32_t al = 16;
    if (node->m.var) {
      if (node->m.var->alt_align > al)
        al = node->m.var->alt_align;
      Type *et = node->m.var->ty;
      while (et) {
        if (et->align > al)
          al = et->align;
        if (et->kind == TY_VLA || et->kind == TY_ARRAY)
          et = et->base;
        else
          break;
      }
    }
    int32_t p2 = align_ceil_pow2(al, 1 << 20);
    if (p2 < 16)
      p2 = 16;
    if (fc->fn->dealloc_vla && node->m.var && node->m.var->vptr) {
      LocalInfo *li = register_local(node->m.var->vptr, false);
      if (li->vla_save_slot < 0)
        li->vla_save_slot = emit_entry_scalar_alloca("ptr", 8, 8);
      int sp = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = stacksave\n", sp);
      sb_printf(&fc->body_sb, "    store.ptr %%t%d, %%loc_%d, align 8\n", sp, li->vla_save_slot);
    }
    int r = new_tid();
    if (p2 > 16) {
      int extra = new_tid();
      int raw = new_tid();
      int pi = new_tid();
      int pa = new_tid();
      int pm = new_tid();
      uint64_t mask = ~(uint64_t)(p2 - 1);
      sb_printf(&fc->body_sb, "    %%t%d = add.i64 %%t%d, %d [wrap]\n", extra, sz.id, p2 - 1);
      sb_printf(&fc->body_sb, "    %%t%d = alloca_dyn %%t%d, align 16\n", raw, extra);
      sb_printf(&fc->body_sb, "    %%t%d = ptrtoint.i64 %%t%d\n", pi, raw);
      sb_printf(&fc->body_sb, "    %%t%d = add.i64 %%t%d, %d [wrap]\n", pa, pi, p2 - 1);
      sb_printf(&fc->body_sb, "    %%t%d = and.i64 %%t%d, 0x%" PRIx64 "\n", pm, pa, mask);
      sb_printf(&fc->body_sb, "    %%t%d = inttoptr.ptr %%t%d\n", r, pm);
    } else {
      sb_printf(&fc->body_sb, "    %%t%d = alloca_dyn %%t%d, align 16\n", r, sz.id);
    }
    if (node->kind == ND_ALLOCA_ZINIT) {
      sb_printf(&fc->body_sb, "    memset %%t%d, 0, %%t%d, align 16\n", r, sz.id);
    }
    if (node->m.var && node->m.var->vptr) {
      LocalInfo *li = register_local(node->m.var->vptr, false);
      sb_printf(&fc->body_sb, "    store.ptr %%t%d, %%loc_%d, align 8\n", r, li->slot_id);
    }
    PirVal v = {r, "ptr"};
    return v;
  }

  case ND_THREAD_FENCE:
    sb_printf(&fc->body_sb, "    fence [seq_cst]\n");
    return pir_void_val();

  case ND_EXCH: {
    PirVal addr = gen_expr(node->m.lhs);
    PirVal val = gen_expr(node->m.rhs);
    Type *ty = node->ty;
    Type *atom_ty = (node->m.lhs->ty && node->m.lhs->ty->base) ? node->m.lhs->ty->base : ty;
    ensure_open_block();
    if (is_mem_val_ty(atom_ty)) {
      const char *it = atomic_int_ty_for_size(atom_ty->size);
      int64_t rhs_sz = node->m.rhs->ty->size;
      PirVal src_ptr = val;
      int32_t p2 = effective_lval_align(node->m.rhs);
      if (rhs_sz > 0 && rhs_sz < atom_ty->size) {
        int32_t stage_al = atom_ty->align > 0 ? atom_ty->align : 1;
        src_ptr = emit_stage_padded_atomic_src(val, rhs_sz, atom_ty->size, stage_al);
        p2 = align_floor_pow2(stage_al);
      }
      if (it) {
        int64_t sz = atom_ty->size;
        int32_t tmp_al = (int32_t)sz;
        int slot = emit_entry_alloca(sz, tmp_al);
        int tmp = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", tmp, slot);
        if (sz > 1) {
          AtomicAlignBlocks ab = emit_atomic_align_begin(addr, sz, "at");
          int new_raw = new_tid();
          int old_raw = new_tid();
          sb_printf(&fc->body_sb, "    %%t%d = load.%s %%t%d, align %d\n", new_raw, it, src_ptr.id, p2);
          sb_printf(&fc->body_sb, "    %%t%d = atomic_rmw.xchg.%s %%t%d, %%t%d [seq_cst]\n",
                    old_raw, it, addr.id, new_raw);
          sb_printf(&fc->body_sb, "    store.%s %%t%d, %%t%d, align %d\n", it, old_raw, tmp, tmp_al);
          emit_atomic_align_slow(&ab, "at", -1);
          int sz_id = new_tid();
          sb_printf(&fc->body_sb, "    %%t%d = const.i64 %" PRIi64 "\n", sz_id, sz);
          sb_printf(&fc->body_sb, "    call @__pir_atomic_exch_n(%%t%d, %%t%d, %%t%d, %%t%d)\n",
                    addr.id, src_ptr.id, tmp, sz_id);
          emit_atomic_align_end(&ab, "at", -1, -1, NULL);
        } else {
          int new_raw = new_tid();
          int old_raw = new_tid();
          sb_printf(&fc->body_sb, "    %%t%d = load.%s %%t%d, align %d\n", new_raw, it, src_ptr.id, p2);
          sb_printf(&fc->body_sb, "    %%t%d = atomic_rmw.xchg.%s %%t%d, %%t%d [seq_cst]\n",
                    old_raw, it, addr.id, new_raw);
          sb_printf(&fc->body_sb, "    store.%s %%t%d, %%t%d, align %d\n", it, old_raw, tmp, tmp_al);
        }
        PirVal v = {tmp, "ptr"};
        return v;
      }
      need_atomic_lock_helpers = true;
      int64_t sz = atom_ty->size > 0 ? atom_ty->size : 1;
      int32_t tmp_al = atom_ty->align > 0 ? atom_ty->align : 1;
      int slot = emit_entry_alloca(sz, tmp_al);
      int tmp = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", tmp, slot);
      if (atom_ty->size > 0) {
        int sz_id = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = const.i64 %" PRIi64 "\n", sz_id, atom_ty->size);
        sb_printf(&fc->body_sb, "    call @__pir_atomic_exch_n(%%t%d, %%t%d, %%t%d, %%t%d)\n",
                  addr.id, src_ptr.id, tmp, sz_id);
      }
      PirVal v = {tmp, "ptr"};
      return v;
    }
    const char *pt = is_flonum(ty) ? ((ty->kind == TY_FLOAT) ? "f32" : "f64") : pir_scalar_ty_name(ty);
    int64_t sz = ty->size > 0 ? ty->size : (pir_ty_bits(pt) / 8);
    if (sz > 1 && sz <= 8) {
      AtomicAlignBlocks ab = emit_atomic_align_begin(addr, sz, "at");
      int rf = new_tid();
      if (is_flonum(ty)) {
        const char *it = (ty->kind == TY_FLOAT) ? "i32" : "i64";
        int iv = new_tid(), oldi = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = bitcast.%s %%t%d\n", iv, it, val.id);
        sb_printf(&fc->body_sb, "    %%t%d = atomic_rmw.xchg.%s %%t%d, %%t%d [seq_cst]\n",
                  oldi, it, addr.id, iv);
        sb_printf(&fc->body_sb, "    %%t%d = bitcast.%s %%t%d\n", rf, pt, oldi);
      } else {
        sb_printf(&fc->body_sb, "    %%t%d = atomic_rmw.xchg.%s %%t%d, %%t%d [seq_cst]\n",
                  rf, pt, addr.id, val.id);
      }
      emit_atomic_align_slow(&ab, "at", rf);
      int new_slot = emit_entry_scalar_alloca(pt, sz, (int32_t)sz);
      int old_slot = emit_entry_scalar_alloca(pt, sz, (int32_t)sz);
      int np = new_tid(), op = new_tid(), sz_id = new_tid(), rs = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", np, new_slot);
      sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", op, old_slot);
      sb_printf(&fc->body_sb, "    store.%s %%t%d, %%t%d, align %" PRIi64 "\n", pt, val.id, np, sz);
      sb_printf(&fc->body_sb, "    %%t%d = const.i64 %" PRIi64 "\n", sz_id, sz);
      sb_printf(&fc->body_sb, "    call @__pir_atomic_exch_n(%%t%d, %%t%d, %%t%d, %%t%d)\n",
                addr.id, np, op, sz_id);
      sb_printf(&fc->body_sb, "    %%t%d = load.%s %%t%d, align %" PRIi64 "\n", rs, pt, op, sz);
      int r = new_tid();
      emit_atomic_align_end(&ab, "at", rs, r, pt);
      PirVal v = {r, pt};
      return is_flonum(ty) ? v : emit_bitint_norm(v, node->ty);
    }
    int r = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = atomic_rmw.xchg.%s %%t%d, %%t%d [seq_cst]\n",
              r, pt, addr.id, val.id);
    PirVal v = {r, pt};
    return emit_bitint_norm(v, node->ty);
  }

  case ND_CAS: {
    PirVal addr = gen_expr(node->cas.addr);
    PirVal old_ptr = gen_expr(node->cas.old_val);
    PirVal new_v = gen_expr(node->cas.new_val);
    Type *ty = node->cas.addr->ty->base;
    ensure_open_block();

    bool is_agg_cas = is_mem_val_ty(ty);
    bool is_fp = is_flonum(ty);
    const char *it = is_agg_cas ? atomic_int_ty_for_size(ty->size)
                   : is_fp ? ((ty->kind == TY_FLOAT) ? "i32" : "i64")
                   : pir_scalar_ty_name(ty);
    int64_t old_sz = (is_agg_cas && node->cas.old_val->ty->base)
                     ? node->cas.old_val->ty->base->size : ty->size;
    int64_t new_sz = is_agg_cas ? node->cas.new_val->ty->size : ty->size;
    bool pad_old = is_agg_cas && old_sz > 0 && old_sz < ty->size;
    bool pad_new = is_agg_cas && new_sz > 0 && new_sz < ty->size;
    int32_t stage_al = ty->align > 0 ? ty->align : 1;
    PirVal eff_old = pad_old ? emit_stage_padded_atomic_src(old_ptr, old_sz, ty->size, stage_al)
                             : old_ptr;
    PirVal eff_new = pad_new ? emit_stage_padded_atomic_src(new_v, new_sz, ty->size, stage_al)
                             : new_v;

    if (is_agg_cas && !it) {
      need_atomic_lock_helpers = true;
      int ok = new_tid();
      if (ty->size > 0) {
        int sz_id = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = const.i64 %" PRIi64 "\n", sz_id, ty->size);
        sb_printf(&fc->body_sb,
                  "    %%t%d = call @__pir_atomic_cas_n(%%t%d, %%t%d, %%t%d, %%t%d)\n",
                  ok, addr.id, eff_old.id, eff_new.id, sz_id);
        if (pad_old) {
          int b_fail = new_bid();
          int b_done = new_bid();
          sb_printf(&fc->body_sb, "    br %%t%d, .cas_ok_%d, .cas_fail_%d\n", ok, b_done, b_fail);
          fc->terminated = true;
          sb_printf(&fc->body_sb, "\n.cas_fail_%d:\n", b_fail);
          fc->terminated = false;
          sb_printf(&fc->body_sb, "    memcpy %%t%d, %%t%d, %" PRIi64 ", align 1\n",
                    old_ptr.id, eff_old.id, old_sz);
          sb_printf(&fc->body_sb, "    jmp .cas_ok_%d\n", b_done);
          fc->terminated = true;
          sb_printf(&fc->body_sb, "\n.cas_ok_%d:\n", b_done);
          fc->terminated = false;
        }
      } else {
        sb_printf(&fc->body_sb, "    %%t%d = const.i8 1\n", ok);
      }
      PirVal v = {ok, "i8"};
      return v;
    }
    if (!it)
      it = "i64";
    int32_t old_al = pad_old ? stage_al
                   : (is_agg_cas
                      ? (node->cas.old_val->ty->base->align > 0 ? node->cas.old_val->ty->base->align : 1)
                      : pir_scalar_align(ty));
    int32_t p2_old = align_floor_pow2(old_al);

    int64_t cas_sz = ty->size > 0 ? ty->size : (pir_ty_bits(it) / 8);
    if (cas_sz > 1 && cas_sz <= 8) {
      AtomicAlignBlocks ab = emit_atomic_align_begin(addr, cas_sz, "cas");
      int b_slow = ab.b_slow, b_done = ab.b_done;
      int exp_id = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = load.%s %%t%d, align %d\n", exp_id, it, eff_old.id, p2_old);
      int new_id = eff_new.id;
      if (is_agg_cas) {
        int32_t new_al = pad_new ? stage_al
                       : (node->cas.new_val->ty->align > 0 ? node->cas.new_val->ty->align : 1);
        int32_t p2_new = align_floor_pow2(new_al);
        new_id = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = load.%s %%t%d, align %d\n", new_id, it, eff_new.id, p2_new);
      } else if (is_fp) {
        new_id = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = bitcast.%s %%t%d\n", new_id, it, eff_new.id);
      }
      int actual = new_tid(), ok_fast = new_tid();
      sb_printf(&fc->body_sb,
                "    %%t%d, %%t%d = atomic_cmpxchg.%s %%t%d, %%t%d, %%t%d [seq_cst, seq_cst]\n",
                actual, ok_fast, it, addr.id, exp_id, new_id);
      int b_fail = new_bid();
      sb_printf(&fc->body_sb, "    br %%t%d, .cas_merge_%d(%%t%d), .cas_fail_%d\n",
                ok_fast, b_done, ok_fast, b_fail);
      fc->terminated = true;

      sb_printf(&fc->body_sb, "\n.cas_fail_%d:\n", b_fail);
      fc->terminated = false;
      sb_printf(&fc->body_sb, "    store.%s %%t%d, %%t%d, align %d\n", it, actual, eff_old.id, p2_old);
      if (pad_old) {
        sb_printf(&fc->body_sb, "    memcpy %%t%d, %%t%d, %" PRIi64 ", align 1\n",
                  old_ptr.id, eff_old.id, old_sz);
      }
      sb_printf(&fc->body_sb, "    jmp .cas_merge_%d(%%t%d)\n", b_done, ok_fast);
      fc->terminated = true;

      sb_printf(&fc->body_sb, "\n.cas_slow_%d:\n", b_slow);
      fc->terminated = false;
      PirVal des_p = eff_new;
      if (!is_agg_cas) {
        const char *st_pt = is_fp ? ((ty->kind == TY_FLOAT) ? "f32" : "f64") : it;
        int nslot = emit_entry_scalar_alloca(st_pt, cas_sz, (int32_t)cas_sz);
        int np = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", np, nslot);
        sb_printf(&fc->body_sb, "    store.%s %%t%d, %%t%d, align %" PRIi64 "\n",
                  st_pt, eff_new.id, np, cas_sz);
        des_p.id = np;
        des_p.ty = "ptr";
      }
      int sz_id = new_tid(), ok_slow = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = const.i64 %" PRIi64 "\n", sz_id, cas_sz);
      sb_printf(&fc->body_sb,
                "    %%t%d = call @__pir_atomic_cas_n(%%t%d, %%t%d, %%t%d, %%t%d)\n",
                ok_slow, addr.id, eff_old.id, des_p.id, sz_id);
      if (pad_old) {
        int b_sf = new_bid();
        sb_printf(&fc->body_sb, "    br %%t%d, .cas_merge_%d(%%t%d), .cas_sfail_%d\n",
                  ok_slow, b_done, ok_slow, b_sf);
        fc->terminated = true;
        sb_printf(&fc->body_sb, "\n.cas_sfail_%d:\n", b_sf);
        fc->terminated = false;
        sb_printf(&fc->body_sb, "    memcpy %%t%d, %%t%d, %" PRIi64 ", align 1\n",
                  old_ptr.id, eff_old.id, old_sz);
        sb_printf(&fc->body_sb, "    jmp .cas_merge_%d(%%t%d)\n", b_done, ok_slow);
        fc->terminated = true;
      } else {
        sb_printf(&fc->body_sb, "    jmp .cas_merge_%d(%%t%d)\n", b_done, ok_slow);
        fc->terminated = true;
      }

      int ok = new_tid();
      sb_printf(&fc->body_sb, "\n.cas_merge_%d(%%t%d: i8):\n", b_done, ok);
      fc->terminated = false;
      PirVal v = {ok, "i8"};
      return v;
    }

    int exp_id = new_tid();
    sb_printf(&fc->body_sb, "    %%t%d = load.%s %%t%d, align %d\n", exp_id, it, eff_old.id, p2_old);
    int new_id = eff_new.id;
    if (is_agg_cas) {
      int32_t new_al = pad_new ? stage_al
                     : (node->cas.new_val->ty->align > 0 ? node->cas.new_val->ty->align : 1);
      int32_t p2_new = align_floor_pow2(new_al);
      new_id = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = load.%s %%t%d, align %d\n", new_id, it, eff_new.id, p2_new);
    } else if (is_fp) {
      new_id = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = bitcast.%s %%t%d\n", new_id, it, eff_new.id);
    }

    int actual = new_tid();
    int ok = new_tid();
    sb_printf(&fc->body_sb,
              "    %%t%d, %%t%d = atomic_cmpxchg.%s %%t%d, %%t%d, %%t%d [seq_cst, seq_cst]\n",
              actual, ok, it, addr.id, exp_id, new_id);

    int b_fail = new_bid();
    int b_done = new_bid();
    sb_printf(&fc->body_sb, "    br %%t%d, .cas_ok_%d, .cas_fail_%d\n", ok, b_done, b_fail);
    fc->terminated = true;

    sb_printf(&fc->body_sb, "\n.cas_fail_%d:\n", b_fail);
    fc->terminated = false;
    sb_printf(&fc->body_sb, "    store.%s %%t%d, %%t%d, align %d\n", it, actual, eff_old.id, p2_old);
    if (pad_old) {
      sb_printf(&fc->body_sb, "    memcpy %%t%d, %%t%d, %" PRIi64 ", align 1\n",
                old_ptr.id, eff_old.id, old_sz);
    }
    sb_printf(&fc->body_sb, "    jmp .cas_ok_%d\n", b_done);
    fc->terminated = true;

    sb_printf(&fc->body_sb, "\n.cas_ok_%d:\n", b_done);
    fc->terminated = false;
    PirVal v = {ok, "i8"};
    return v;
  }

  case ND_VA_START: {
    PirVal ap_ptr = (is_x64_pir_target() && node->m.lhs->ty->kind == TY_PTR)
                    ? gen_expr(node->m.lhs)
                    : gen_addr(node->m.lhs);
    sb_printf(&fc->body_sb, "    va_start %%t%d\n", ap_ptr.id);
    return pir_void_val();
  }

  case ND_VA_COPY: {
    PirVal dst_ptr = (is_x64_pir_target() && node->m.lhs->ty->kind == TY_PTR)
                     ? gen_expr(node->m.lhs)
                     : gen_addr(node->m.lhs);
    PirVal src_ptr = (is_x64_pir_target() && node->m.rhs->ty->kind == TY_PTR)
                     ? gen_expr(node->m.rhs)
                     : gen_addr(node->m.rhs);
    sb_printf(&fc->body_sb, "    va_copy %%t%d, %%t%d\n", dst_ptr.id, src_ptr.id);
    return pir_void_val();
  }

  case ND_VA_ARG: {
    /* ND_VA_ARG has type `T*` (wrapped by an outer ND_DEREF in parse.c).
     * Allocate a local buffer for T, fill it via va_arg / va_arg_buf, return ptr. */
    PirVal ap_ptr = (is_x64_pir_target() && node->m.lhs->ty->kind == TY_PTR)
                    ? gen_expr(node->m.lhs)
                    : gen_addr(node->m.lhs);
    Type *ty = node->ty->base;
    ensure_open_block();
    if (is_mem_val_ty(ty)) {
      int slot = emit_entry_alloca(ty->size > 0 ? ty->size : 1, ty->align);
      int p = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", p, slot);
      if (ty->size > 0) {
        sb_printf(&fc->body_sb, "    va_arg_buf @%s, %%t%d, %%t%d\n",
                  get_pir_agg_type_name(ty), p, ap_ptr.id);
      }
      PirVal v = {p, "ptr"};
      return v;
    } else {
      const char *pt = pir_scalar_ty_name(ty);
      int32_t al = pir_scalar_align(ty);
      int slot = emit_entry_scalar_alloca(pt, ty->size > 0 ? ty->size : al, al);
      int val = new_tid();
      int p = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = va_arg.%s %%t%d\n", val, pt, ap_ptr.id);
      sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%loc_%d, 0\n", p, slot);
      sb_printf(&fc->body_sb, "    store.%s %%t%d, %%t%d, align %d\n", pt, val, p, al);
      PirVal v = {p, "ptr"};
      return v;
    }
  }

  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
  case ND_REM:
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
  case ND_SHL:
  case ND_SHR:
  case ND_SAR:
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE: {
    if (is_bitint_mem(node->ty) || is_bitint_mem(node->m.lhs->ty) ||
        is_bitint_mem(node->m.rhs->ty))
      error_tok(node->tok, "wide _BitInt(>64) arithmetic/comparison is not yet supported in PIR backend");
    PirVal lv = gen_expr(node->m.lhs);
    PirVal rv = gen_expr(node->m.rhs);
    ensure_open_block();

    Type *lty = node->m.lhs->ty;
    Type *rty = node->m.rhs->ty;

    /* Pointer arithmetic */
    if (node->kind == ND_ADD && (is_ptr(lty) || is_decay_ty(lty))) {
      if (strcmp(rv.ty, "i64") != 0)
        rv = emit_cast_val(rv, rty, ty_long);
      int r = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%t%d, %%t%d\n", r, lv.id, rv.id);
      PirVal v = {r, "ptr"};
      return v;
    }
    if (node->kind == ND_ADD && (is_ptr(rty) || is_decay_ty(rty))) {
      if (strcmp(lv.ty, "i64") != 0)
        lv = emit_cast_val(lv, lty, ty_long);
      int r = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%t%d, %%t%d\n", r, rv.id, lv.id);
      PirVal v = {r, "ptr"};
      return v;
    }
    if (node->kind == ND_SUB && (is_ptr(lty) || is_decay_ty(lty))) {
      if (is_ptr(rty) || is_decay_ty(rty)) {
        int a = new_tid();
        int b = new_tid();
        int r = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = ptrtoint.i64 %%t%d\n", a, lv.id);
        sb_printf(&fc->body_sb, "    %%t%d = ptrtoint.i64 %%t%d\n", b, rv.id);
        sb_printf(&fc->body_sb, "    %%t%d = sub.i64 %%t%d, %%t%d [wrap]\n", r, a, b);
        PirVal v = {r, "i64"};
        return v;
      }
      if (strcmp(rv.ty, "i64") != 0)
        rv = emit_cast_val(rv, rty, ty_long);
      int neg = new_tid();
      int r = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = sub.i64 0, %%t%d [wrap]\n", neg, rv.id);
      sb_printf(&fc->body_sb, "    %%t%d = ptr_add.i64 %%t%d, %%t%d\n", r, lv.id, neg);
      PirVal v = {r, "ptr"};
      return v;
    }

    /* Comparisons */
    if (node->kind >= ND_EQ && node->kind <= ND_GE) {
      int cmp = new_tid();
      int res = new_tid();
      if (is_flonum(lty)) {
        const char *op = (node->kind == ND_EQ) ? "fcmp_oeq"
                       : (node->kind == ND_NE) ? "fcmp_une"
                       : (node->kind == ND_LT) ? "fcmp_olt"
                       : (node->kind == ND_LE) ? "fcmp_ole"
                       : (node->kind == ND_GT) ? "fcmp_ogt" : "fcmp_oge";
        sb_printf(&fc->body_sb, "    %%t%d = %s.%s %%t%d, %%t%d\n", cmp, op, lv.ty, lv.id, rv.id);
      } else if (!strcmp(lv.ty, "ptr") || !strcmp(rv.ty, "ptr")) {
        if (strcmp(lv.ty, "ptr") != 0)
          lv = emit_cast_val(lv, lty, pointer_to(ty_void));
        if (strcmp(rv.ty, "ptr") != 0)
          rv = emit_cast_val(rv, rty, pointer_to(ty_void));
        const char *op = (node->kind == ND_EQ) ? "cmp_eq"
                       : (node->kind == ND_NE) ? "cmp_ne"
                       : (node->kind == ND_LT) ? "cmp_lt_u"
                       : (node->kind == ND_LE) ? "cmp_le_u"
                       : (node->kind == ND_GT) ? "cmp_gt_u" : "cmp_ge_u";
        sb_printf(&fc->body_sb, "    %%t%d = %s.ptr %%t%d, %%t%d\n", cmp, op, lv.id, rv.id);
      } else {
        if (strcmp(rv.ty, lv.ty) != 0)
          rv = emit_cast_val(rv, rty, lty);
        bool is_u = lty->is_unsigned || lty->kind == TY_BOOL;
        const char *op = (node->kind == ND_EQ) ? "cmp_eq"
                       : (node->kind == ND_NE) ? "cmp_ne"
                       : (node->kind == ND_LT) ? (is_u ? "cmp_lt_u" : "cmp_lt_s")
                       : (node->kind == ND_LE) ? (is_u ? "cmp_le_u" : "cmp_le_s")
                       : (node->kind == ND_GT) ? (is_u ? "cmp_gt_u" : "cmp_gt_s")
                                               : (is_u ? "cmp_ge_u" : "cmp_ge_s");
        sb_printf(&fc->body_sb, "    %%t%d = %s.%s %%t%d, %%t%d\n", cmp, op, lv.ty, lv.id, rv.id);
      }
      sb_printf(&fc->body_sb, "    %%t%d = zext.i32 %%t%d\n", res, cmp);
      PirVal v = {res, "i32"};
      return v;
    }

    /* Floating-point arithmetic */
    if (is_flonum(node->ty)) {
      const char *op = (node->kind == ND_ADD) ? "fadd"
                     : (node->kind == ND_SUB) ? "fsub"
                     : (node->kind == ND_MUL) ? "fmul" : "fdiv";
      int r = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = %s.%s %%t%d, %%t%d\n", r, op, lv.ty, lv.id, rv.id);
      PirVal v = {r, lv.ty};
      return v;
    }

    /* Integer arithmetic, bitwise, and shifts */
    const char *pt = pir_scalar_ty_name(node->ty);
    if (strcmp(lv.ty, pt) != 0)
      lv = emit_cast_val(lv, lty, node->ty);
    if (strcmp(rv.ty, pt) != 0)
      rv = emit_cast_val(rv, rty, node->ty);

    int r = new_tid();
    switch (node->kind) {
    case ND_ADD:
      sb_printf(&fc->body_sb, "    %%t%d = add.%s %%t%d, %%t%d [wrap]\n", r, pt, lv.id, rv.id);
      break;
    case ND_SUB:
      sb_printf(&fc->body_sb, "    %%t%d = sub.%s %%t%d, %%t%d [wrap]\n", r, pt, lv.id, rv.id);
      break;
    case ND_MUL:
      sb_printf(&fc->body_sb, "    %%t%d = mul.%s %%t%d, %%t%d [wrap]\n", r, pt, lv.id, rv.id);
      break;
    case ND_DIV:
      sb_printf(&fc->body_sb, "    %%t%d = %s.%s %%t%d, %%t%d [unsafe]\n",
                r, node->ty->is_unsigned ? "div_u" : "div_s", pt, lv.id, rv.id);
      break;
    case ND_REM:
      sb_printf(&fc->body_sb, "    %%t%d = %s.%s %%t%d, %%t%d [unsafe]\n",
                r, node->ty->is_unsigned ? "rem_u" : "rem_s", pt, lv.id, rv.id);
      break;
    case ND_BITAND:
      sb_printf(&fc->body_sb, "    %%t%d = and.%s %%t%d, %%t%d\n", r, pt, lv.id, rv.id);
      break;
    case ND_BITOR:
      sb_printf(&fc->body_sb, "    %%t%d = or.%s %%t%d, %%t%d\n", r, pt, lv.id, rv.id);
      break;
    case ND_BITXOR:
      sb_printf(&fc->body_sb, "    %%t%d = xor.%s %%t%d, %%t%d\n", r, pt, lv.id, rv.id);
      break;
    case ND_SHL:
      sb_printf(&fc->body_sb, "    %%t%d = shl.%s %%t%d, %%t%d [mask]\n", r, pt, lv.id, rv.id);
      break;
    case ND_SHR:
      sb_printf(&fc->body_sb, "    %%t%d = shr.%s %%t%d, %%t%d [mask]\n", r, pt, lv.id, rv.id);
      break;
    case ND_SAR:
      sb_printf(&fc->body_sb, "    %%t%d = sar.%s %%t%d, %%t%d [mask]\n", r, pt, lv.id, rv.id);
      break;
    default:
      internal_error();
    }
    PirVal v = {r, pt};
    return emit_bitint_norm(v, node->ty);
  }

  default:
    error_tok(node->tok, "unsupported expression kind %d in PIR backend", (int)node->kind);
  }
}

/* =========================================================================
 * 11. Statement & Defer Generation
 * ========================================================================= */

static void gen_defr(Node *node) {
  DeferStmt *defr = node->dfr_from;
  DeferStmt *end = node->dfr_dest;
  while (defr != end) {
    if (defr->kind == DF_VLA_DEALLOC) {
      while (defr->next != end && defr->next->kind == DF_VLA_DEALLOC)
        defr = defr->next;
      if (fc->fn->dealloc_vla && !fc->terminated) {
        int sp = new_tid();
        int save_slot = fc->vla_sp_slot;
        if (defr->vla) {
          LocalInfo *li = register_local(defr->vla, false);
          if (li->vla_save_slot < 0)
            li->vla_save_slot = emit_entry_scalar_alloca("ptr", 8, 8);
          save_slot = li->vla_save_slot;
        }
        sb_printf(&fc->body_sb, "    %%t%d = load.ptr %%loc_%d, align 8\n", sp, save_slot);
        sb_printf(&fc->body_sb, "    stackrestore %%t%d\n", sp);
      }
      defr = defr->next;
      continue;
    }
    if (defr->kind == DF_VM_BARRIER) {
      defr = defr->next;
      continue;
    }
    if (defr->kind == DF_CLEANUP_FN) {
      gen_expr(defr->cleanup_fn);
      defr = defr->next;
      continue;
    }
    if (defr->kind == DF_DEFER_STMT) {
      gen_stmt(defr->stmt);
      defr = defr->next;
      continue;
    }
    internal_error();
  }
}

static int64_t get_lbl_id(Node *lbl) {
  if (!lbl->lbl.id)
    lbl->lbl.id = ++fc->next_blk;
  return lbl->lbl.id;
}

static void name_labels(Node *lbl) {
  for (; lbl; lbl = lbl->lbl.next)
    lbl->lbl.id = ++fc->next_blk;
}

static void gen_stmt(Node *node) {
  if (!node)
    return;
  emit_loc_if_needed(node->tok);

  switch (node->kind) {
  case ND_NULL_STMT:
    return;

  case ND_EXPR_STMT:
    gen_expr(node->m.lhs);
    return;

  case ND_BLOCK:
    name_labels(node->blk.local_labels);
    for (Node *n = node->blk.body; n; n = n->next)
      gen_stmt(n);
    gen_defr(node);
    return;

  case ND_IF: {
    int c = new_bid();
    PirVal cv = gen_expr(node->ctrl.cond);
    PirVal cb = emit_to_bool_i8(cv, node->ctrl.cond->ty);
    if (node->ctrl.els) {
      sb_printf(&fc->body_sb, "    br %%t%d, .if_t_%d, .if_e_%d\n", cb.id, c, c);
      fc->terminated = true;
      sb_printf(&fc->body_sb, "\n.if_t_%d:\n", c);
      fc->terminated = false;
      gen_stmt(node->ctrl.then);
      emit_jmp_id("if_end", c);
      sb_printf(&fc->body_sb, "\n.if_e_%d:\n", c);
      fc->terminated = false;
      gen_stmt(node->ctrl.els);
      emit_label_id("if_end", c);
    } else {
      sb_printf(&fc->body_sb, "    br %%t%d, .if_t_%d, .if_end_%d\n", cb.id, c, c);
      fc->terminated = true;
      sb_printf(&fc->body_sb, "\n.if_t_%d:\n", c);
      fc->terminated = false;
      gen_stmt(node->ctrl.then);
      emit_label_id("if_end", c);
    }
    return;
  }

  case ND_FOR: {
    int c = new_bid();
    node->ctrl.id = c;
    if (node->ctrl.for_init)
      gen_stmt(node->ctrl.for_init);
    emit_label_id("for_cond", c);
    if (node->ctrl.cond) {
      PirVal cv = gen_expr(node->ctrl.cond);
      PirVal cb = emit_to_bool_i8(cv, node->ctrl.cond->ty);
      /* Branch to .brk when condition is false so .for_body is the fallthrough block! */
      int done = new_tid();
      sb_printf(&fc->body_sb, "    %%t%d = cmp_eq.i8 %%t%d, 0\n", done, cb.id);
      sb_printf(&fc->body_sb, "    br %%t%d, .brk_%d, .for_body_%d\n", done, c, c);
      fc->terminated = true;
      sb_printf(&fc->body_sb, "\n.for_body_%d:\n", c);
      fc->terminated = false;
    }
    gen_stmt(node->ctrl.then);
    emit_label_id("cont", c);
    if (node->ctrl.for_inc)
      gen_expr(node->ctrl.for_inc);
    gen_defr(node);
    emit_jmp_id("for_cond", c);
    emit_label_id("brk", c);
    return;
  }

  case ND_DO: {
    int c = new_bid();
    node->ctrl.id = c;
    emit_label_id("do_body", c);
    gen_stmt(node->ctrl.then);
    emit_label_id("cont", c);
    PirVal cv = gen_expr(node->ctrl.cond);
    PirVal cb = emit_to_bool_i8(cv, node->ctrl.cond->ty);
    sb_printf(&fc->body_sb, "    br %%t%d, .do_body_%d, .brk_%d\n", cb.id, c, c);
    fc->terminated = true;
    emit_label_id("brk", c);
    return;
  }

  case ND_SWITCH: {
    int c = new_bid();
    node->ctrl.id = c;
    PirVal val = gen_expr(node->ctrl.cond);
    ensure_open_block();

    /* Assign case IDs & check if ranges are small enough to expand into native PIR switch. */
    bool is_u = node->ctrl.cond->ty->is_unsigned;
    bool can_native_switch = true;
    uint64_t expanded_cases = 0;
    int case_cnt = 0;
    for (CaseRange *cr = node->ctrl.sw_cases; cr; cr = cr->next) {
      if (cr->label->kind != ND_DEFAULT && !cr->label->cases.id)
        cr->label->cases.id = ++case_cnt;
      bool bad_order = is_u ? ((uint64_t)cr->hi < (uint64_t)cr->lo) : (cr->hi < cr->lo);
      if (bad_order) {
        can_native_switch = false;
      } else {
        uint64_t diff = (uint64_t)cr->hi - (uint64_t)cr->lo;
        if (diff >= 64 || expanded_cases + diff + 1 > 256)
          can_native_switch = false;
        else
          expanded_cases += diff + 1;
      }
    }

    char def_lbl[64];
    if (node->ctrl.sw_default)
      snprintf(def_lbl, sizeof(def_lbl), "sw_def_%d", c);
    else
      snprintf(def_lbl, sizeof(def_lbl), "brk_%d", c);

    if (can_native_switch) {
      /* Emit native PIR switch instruction (expanding small consecutive case ranges)! */
      sb_printf(&fc->body_sb, "    switch.%s %%t%d, default .%s [\n", val.ty, val.id, def_lbl);
      bool first_case = true;
      for (CaseRange *cr = node->ctrl.sw_cases; cr; cr = cr->next) {
        char tgt_lbl[64];
        if (cr->label->kind == ND_DEFAULT)
          snprintf(tgt_lbl, sizeof(tgt_lbl), "sw_def_%d", c);
        else
          snprintf(tgt_lbl, sizeof(tgt_lbl), "sw_c_%d_%" PRIi64, c, cr->label->cases.id);
        uint64_t diff = (uint64_t)cr->hi - (uint64_t)cr->lo;
        for (uint64_t off = 0; off <= diff; off++) {
          int64_t v = (int64_t)((uint64_t)cr->lo + off);
          if (!first_case) sb_printf(&fc->body_sb, ",\n");
          sb_printf(&fc->body_sb, "        %" PRIi64 " -> .%s", v, tgt_lbl);
          first_case = false;
        }
      }
      if (!first_case) sb_printf(&fc->body_sb, "\n");
      sb_printf(&fc->body_sb, "    ]\n");
      fc->terminated = true;
    } else {
      /* Emit range-supporting compare/branch chain */
      for (CaseRange *cr = node->ctrl.sw_cases; cr; cr = cr->next) {
        char tgt_lbl[64];
        if (cr->label->kind == ND_DEFAULT)
          snprintf(tgt_lbl, sizeof(tgt_lbl), "sw_def_%d", c);
        else
          snprintf(tgt_lbl, sizeof(tgt_lbl), "sw_c_%d_%" PRIi64, c, cr->label->cases.id);
        int nxt = new_bid();
        if (cr->lo == cr->hi) {
          int eq = new_tid();
          sb_printf(&fc->body_sb, "    %%t%d = cmp_eq.%s %%t%d, %" PRIi64 "\n",
                    eq, val.ty, val.id, cr->lo);
          sb_printf(&fc->body_sb, "    br %%t%d, .%s, .sw_chk_%d\n",
                    eq, tgt_lbl, nxt);
        } else {
          int c1 = new_tid();
          int c2 = new_tid();
          int both = new_tid();
          sb_printf(&fc->body_sb, "    %%t%d = %s.%s %%t%d, %" PRIi64 "\n",
                    c1, is_u ? "cmp_ge_u" : "cmp_ge_s", val.ty, val.id, cr->lo);
          sb_printf(&fc->body_sb, "    %%t%d = %s.%s %%t%d, %" PRIi64 "\n",
                    c2, is_u ? "cmp_le_u" : "cmp_le_s", val.ty, val.id, cr->hi);
          sb_printf(&fc->body_sb, "    %%t%d = and.i8 %%t%d, %%t%d\n", both, c1, c2);
          sb_printf(&fc->body_sb, "    br %%t%d, .%s, .sw_chk_%d\n",
                    both, tgt_lbl, nxt);
        }
        fc->terminated = true;
        sb_printf(&fc->body_sb, "\n.sw_chk_%d:\n", nxt);
        fc->terminated = false;
      }
      emit_jmp(def_lbl);
    }

    gen_stmt(node->ctrl.then);
    emit_label_id("brk", c);
    return;
  }

  case ND_CASE: {
    char buf[64];
    snprintf(buf, sizeof(buf), "sw_c_%" PRIi64 "_%" PRIi64,
             node->cases.parent_sw->ctrl.id, node->cases.id);
    emit_label(buf);
    return;
  }

  case ND_DEFAULT:
    emit_label_id("sw_def", node->cases.parent_sw->ctrl.id);
    return;

  case ND_LABEL:
    emit_label_id("lbl", get_lbl_id(node));
    return;

  case ND_BREAK:
    gen_defr(node);
    emit_jmp_id("brk", node->jmp.parent_loop->ctrl.id);
    return;

  case ND_CONT:
    gen_defr(node);
    emit_jmp_id("cont", node->jmp.parent_loop->ctrl.id);
    return;

  case ND_GOTO:
    gen_defr(node);
    emit_jmp_id("lbl", get_lbl_id(node->jmp.target));
    return;

  case ND_RETURN: {
    if (!node->m.lhs) {
      gen_defr(node);
      ensure_open_block();
      sb_printf(&fc->body_sb, "    ret\n");
      fc->terminated = true;
      return;
    }
    Type *rty = fc->fn->ty->return_ty;
    if (rty->kind == TY_VOID || (is_agg_ty(rty) && rty->size == 0)) {
      gen_expr(node->m.lhs);
      gen_defr(node);
      ensure_open_block();
      sb_printf(&fc->body_sb, "    ret\n");
      fc->terminated = true;
      return;
    }
    PirVal rv = gen_expr(node->m.lhs);
    if (fc->has_retbuf) {
      int32_t dst_al = align_floor_pow2(rty->align > 0 ? rty->align : 1);
      int32_t src_al = effective_lval_align(node->m.lhs);
      int32_t p2 = dst_al < src_al ? dst_al : src_al;
      int64_t rsz = is_agg_ty(rty) ? agg_copy_size(rty) : rty->size;
      ensure_open_block();
      sb_printf(&fc->body_sb, "    memcpy %%__retbuf, %%t%d, %" PRIi64 ", align %d\n",
                rv.id, rsz, p2);
      gen_defr(node);
      ensure_open_block();
      sb_printf(&fc->body_sb, "    ret\n");
      fc->terminated = true;
      return;
    }
    if (is_darwin_a64_bitint_i32_abi(rty)) {
      const char *st = pir_scalar_ty_name(rty);
      if (strcmp(rv.ty, st) != 0)
        rv = emit_cast_val(rv, node->m.lhs->ty, rty);
      if (strcmp(rv.ty, "i32") != 0) {
        int ext = new_tid();
        sb_printf(&fc->body_sb, "    %%t%d = %s.i32 %%t%d\n", ext,
                  rty->is_unsigned ? "zext" : "sext", rv.id);
        rv.id = ext;
        rv.ty = "i32";
      }
    } else if (strcmp(rv.ty, fc->ret_pir_ty) != 0) {
      rv = emit_cast_val(rv, node->m.lhs->ty, rty);
    }
    gen_defr(node);
    ensure_open_block();
    sb_printf(&fc->body_sb, "    ret %%t%d\n", rv.id);
    fc->terminated = true;
    return;
  }

  default:
    error_tok(node->tok, "unsupported statement kind %d in PIR backend", (int)node->kind);
  }
}

/* =========================================================================
 * 12. Function Emission (`emit_text`)
 * ========================================================================= */

void emit_text(Obj *fn) {
  FnCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.fn = fn;
  ctx.last_file_no = -1;
  ctx.last_line_no = -1;
  ctx.vla_sp_slot = -1;
  fc = &ctx;

  Type *rty = fn->ty->return_ty;
  ctx.has_retbuf = is_mem_val_ty(rty) && rty->size > 0;
  ctx.ret_pir_ty = ctx.has_retbuf ? "void" : pir_ret_ty_name(rty);

  /* Register parameters first so param order matches signature cleanly */
  SBuf hdr;
  sb_init(&hdr);

  if (fn->is_weak)
    sb_printf(&hdr, "weak ");
  else if (!fn->is_static)
    sb_printf(&hdr, "export ");
  sb_printf(&hdr, "func ");
  sb_emit_sym(&hdr, pir_sym_name(fn));
  sb_printf(&hdr, "(");

  bool first_p = true;
  if (ctx.has_retbuf) {
    sb_printf(&hdr, "%%__retbuf: retbuf(@%s)", get_pir_agg_type_name(rty));
    first_p = false;
  }

  for (Obj *p = fn->ty->param_list; p; p = p->param_next) {
    if (p->ty->size <= 0 && is_agg_ty(p->ty))
      continue;
    LocalInfo *li = register_local(p, true);
    if (!first_p)
      sb_printf(&hdr, ", ");
    first_p = false;
    if (li->is_byval_param) {
      sb_printf(&hdr, "%%p_%d: byval(@%s)", li->slot_id, get_pir_agg_type_name(p->ty));
    } else {
      const char *abi_ty = pir_abi_scalar_ty_name(p->ty);
      sb_printf(&hdr, "%%p_%d: %s%s", li->slot_id, abi_ty, pir_int_ext_attr(p->ty));
      if (p->ty->kind == TY_PTR && (p->ty->qual & Q_RESTRICT))
        sb_printf(&hdr, " noalias");
      int32_t p2 = align_floor_pow2(li->align);
      if (strcmp(abi_ty, li->pir_ty) != 0) {
        int tr = new_tid();
        sb_printf(&ctx.entry_sb, "    %%t%d = trunc.%s %%p_%d\n", tr, li->pir_ty, li->slot_id);
        if (p->ty->kind == TY_BITINT && p->ty->bit_cnt < pir_ty_bits(li->pir_ty)) {
          int sh = pir_ty_bits(li->pir_ty) - (int)p->ty->bit_cnt;
          int s1 = new_tid(), s2 = new_tid();
          sb_printf(&ctx.entry_sb, "    %%t%d = shl.%s %%t%d, %d [mask]\n",
                    s1, li->pir_ty, tr, sh);
          sb_printf(&ctx.entry_sb, "    %%t%d = %s.%s %%t%d, %d [mask]\n",
                    s2, p->ty->is_unsigned ? "shr" : "sar", li->pir_ty, s1, sh);
          sb_printf(&ctx.entry_sb, "    store.%s %%t%d, %%loc_%d, align %d\n",
                    li->pir_ty, s2, li->slot_id, p2);
        } else {
          sb_printf(&ctx.entry_sb, "    store.%s %%t%d, %%loc_%d, align %d\n",
                    li->pir_ty, tr, li->slot_id, p2);
        }
      } else if (p->ty->kind == TY_BITINT && p->ty->bit_cnt < pir_ty_bits(li->pir_ty)) {
        int sh = pir_ty_bits(li->pir_ty) - (int)p->ty->bit_cnt;
        int s1 = new_tid(), s2 = new_tid();
        sb_printf(&ctx.entry_sb, "    %%t%d = shl.%s %%p_%d, %d [mask]\n",
                  s1, li->pir_ty, li->slot_id, sh);
        sb_printf(&ctx.entry_sb, "    %%t%d = %s.%s %%t%d, %d [mask]\n",
                  s2, p->ty->is_unsigned ? "shr" : "sar", li->pir_ty, s1, sh);
        sb_printf(&ctx.entry_sb, "    store.%s %%t%d, %%loc_%d, align %d\n",
                  li->pir_ty, s2, li->slot_id, p2);
      } else {
        sb_printf(&ctx.entry_sb, "    store.%s %%p_%d, %%loc_%d, align %d\n",
                  li->pir_ty, li->slot_id, li->slot_id, p2);
      }
    }
  }

  if (fn->ty->is_variadic) {
    if (!first_p)
      sb_printf(&hdr, ", ");
    sb_printf(&hdr, "...");
  }

  sb_printf(&hdr, ") -> %s cc \"c\"%s", ctx.ret_pir_ty,
            ctx.has_retbuf ? "" : pir_int_ext_attr(rty));
  if (fn->is_noreturn)
    sb_printf(&hdr, " noreturn");
  if (fn->section_name) {
    sb_printf(&hdr, " section ");
    sb_emit_escaped_str(&hdr, fn->section_name);
  }
  sb_printf(&hdr, " {\n.entry:\n");

  /* Collect all remaining local variables in scopes into .entry allocas */
  collect_scope_locals(fn->ty->scopes);

  /* Save stack pointer in entry if function deallocates VLAs */
  if (fn->dealloc_vla) {
    ctx.vla_sp_slot = emit_entry_scalar_alloca("ptr", 8, 8);
    int sp = new_tid();
    sb_printf(&ctx.entry_sb, "    %%t%d = stacksave\n", sp);
    sb_printf(&ctx.entry_sb, "    store.ptr %%t%d, %%loc_%d, align 8\n", sp, ctx.vla_sp_slot);
  }

  /* Emit function body */
  gen_stmt(fn->body);

  /* Close fallthrough block cleanly */
  if (!ctx.terminated) {
    if (fn->is_noreturn) {
      sb_printf(&ctx.body_sb, "    unreachable\n");
    } else if (!strcmp(ctx.ret_pir_ty, "void")) {
      sb_printf(&ctx.body_sb, "    ret\n");
    } else if (!strcmp(fn->name, "main") && !strcmp(ctx.ret_pir_ty, "i32")) {
      sb_printf(&ctx.body_sb, "    ret 0\n");
    } else {
      int z = new_tid();
      if (!strcmp(ctx.ret_pir_ty, "f32") || !strcmp(ctx.ret_pir_ty, "f64"))
        sb_printf(&ctx.body_sb, "    %%t%d = const.%s 0.0\n", z, ctx.ret_pir_ty);
      else
        sb_printf(&ctx.body_sb, "    %%t%d = const.%s 0\n", z, ctx.ret_pir_ty);
      sb_printf(&ctx.body_sb, "    ret %%t%d\n", z);
    }
    ctx.terminated = true;
  }

  /* Assemble final function text */
  SBuf full;
  sb_init(&full);
  if (hdr.len)
    sb_append(&full, hdr.buf, hdr.len);
  if (ctx.entry_sb.len)
    sb_append(&full, ctx.entry_sb.buf, ctx.entry_sb.len);
  if (ctx.body_sb.len)
    sb_append(&full, ctx.body_sb.buf, ctx.body_sb.len);
  sb_printf(&full, "}\n\n");

  fn->output = calloc(1, sizeof(FuncObj));
  fn->output->buf = full.buf;
  fn->output->buflen = full.len;
  /* Emit live static local variables NOW while ast_arena is still alive!
   * Store each entry with its PIR symbol name so codegen() deduplicates it
   * against top-level definitions and extern declarations. */
  StaticLvarOut *sl_list = NULL;
  int sl_cnt = 0, sl_cap = 0;
  {
    Obj head = {.next = fn->static_lvars};
    bool has_change;
    do {
      has_change = false;
      Obj *cur = &head;
      for (Obj *sv = head.next; sv; sv = sv->next) {
        if (sv->is_live || sv->is_used) {
          for (Relocation *rel = sv->rel; rel; rel = rel->next) {
            if (rel->var)
              fc_add_ref(rel->var);
          }
          SBuf one_sb;
          sb_init(&one_sb);
          emit_global_def(&one_sb, sv);
          if (sl_cnt == sl_cap) {
            sl_cap = sl_cap ? sl_cap * 2 : 8;
            sl_list = pir_xrealloc(sl_list, (size_t)sl_cap * sizeof(StaticLvarOut));
          }
          sl_list[sl_cnt].sym_name = arena_strdup(&cc1_arena, pir_sym_name(sv));
          sl_list[sl_cnt].buf = one_sb.buf;
          sl_list[sl_cnt].len = one_sb.len;
          sl_cnt++;
          has_change = true;
          continue;
        }
        cur = cur->next = sv;
      }
      cur->next = NULL;
    } while (has_change);
  }

  fn->output->refs = ctx.refs;
  fn->output->ref_cnt = ctx.ref_cnt;
  fn->output->ref_cap = ctx.ref_cap;
  fn->output->static_lvars = sl_list;
  fn->output->static_lvar_cnt = sl_cnt;

  sb_free(&hdr);
  sb_free(&ctx.entry_sb);
  sb_free(&ctx.body_sb);
  free(ctx.locals);
  fc = NULL;
}

/* =========================================================================
 * 13. Global Data, Extern Declarations & Final Module Emission (`codegen`)
 * ========================================================================= */

static void mark_live(Obj *var) {
  if (!var || var->is_live)
    return;
  var->is_live = true;

  if (!var->is_definition) {
    if (var->alias_name) {
      Obj *alias = get_symbol_var(var->alias_name);
      if (alias && alias->is_definition)
        mark_live(alias);
    }
    return;
  }

  if (var->ty->kind == TY_FUNC) {
    if (var->output) {
      for (int i = 0; i < var->output->ref_cnt; i++)
        mark_live(var->output->refs[i]);
    }
  } else {
    for (Relocation *rel = var->rel; rel; rel = rel->next) {
      if (rel->var)
        mark_live(rel->var);
    }
  }
}

static void emit_extern_decl(SBuf *sb, Obj *var) {
  const char *wpfx = var->is_weak ? "weak " : "";
  if (var->ty->kind == TY_FUNC) {
    sb_printf(sb, "%sextern func ", wpfx);
    sb_emit_sym(sb, pir_sym_name(var));
    sb_printf(sb, "(");
    bool first = true;
    Type *rty = var->ty->return_ty;
    bool has_rb = is_mem_val_ty(rty) && rty->size > 0;
    if (has_rb) {
      sb_printf(sb, "retbuf(@%s)", get_pir_agg_type_name(rty));
      first = false;
    }
    for (Obj *p = var->ty->param_list; p; p = p->param_next) {
      if (p->ty->size <= 0 && is_agg_ty(p->ty))
        continue;
      if (!first)
        sb_printf(sb, ", ");
      first = false;
      if (is_mem_val_ty(p->ty))
        sb_printf(sb, "byval(@%s)", get_pir_agg_type_name(p->ty));
      else
        sb_printf(sb, "%s%s", pir_abi_scalar_ty_name(p->ty), pir_int_ext_attr(p->ty));
    }
    if (var->ty->is_variadic) {
      if (!first)
        sb_printf(sb, ", ");
      sb_printf(sb, "...");
    }
    const char *rt = has_rb ? "void" : pir_ret_ty_name(rty);
    sb_printf(sb, ") -> %s cc \"c\"%s", rt, has_rb ? "" : pir_int_ext_attr(rty));
    if (var->is_noreturn)
      sb_printf(sb, " noreturn");
    sb_printf(sb, "\n");
  } else {
    int32_t al = var->alt_align ? var->alt_align : (var->ty->align > 0 ? var->ty->align : 1);
    int32_t p2 = align_floor_pow2(al);
    int64_t sz = var->ty->size > 0 ? var->ty->size : 1;
    const char *seg = (var->kind == OBJ_TLS) ? "tls" : "global";
    sb_printf(sb, "%sextern ", wpfx);
    sb_emit_sym(sb, pir_sym_name(var));
    sb_printf(sb, " = %s [%" PRIi64 " x i8], align %d\n", seg, sz, p2);
  }
}

static void emit_global_def(SBuf *sb, Obj *var) {
  int64_t sz = var->ty->size > 0 ? var->ty->size : 1;
  int32_t al = var->alt_align ? var->alt_align : (var->ty->align > 0 ? var->ty->align : 1);
  int32_t p2 = align_floor_pow2(al);

  const char *link = var->is_weak ? "weak " : (!var->is_static ? "export " : "");
  bool is_ro = (var->kind != OBJ_TLS) && (var->is_string_lit || is_const_var(var)) &&
               (var->init_data != NULL);
  const char *seg = (var->kind == OBJ_TLS) ? "tls" : (is_ro ? "data" : "global");

  sb_printf(sb, "%s", link);
  sb_emit_sym(sb, pir_sym_name(var));
  sb_printf(sb, " = %s ", seg);
  if (var->section_name) {
    sb_printf(sb, "section ");
    sb_emit_escaped_str(sb, var->section_name);
    sb_printf(sb, " ");
  }

  if (!var->init_data || var->ty->size <= 0) {
    sb_printf(sb, "[%" PRIi64 " x i8] = zeroinit, align %d\n", sz, p2);
    return;
  }

  /* Fast-path all-zero initialized globals with no relocations into compact zeroinit. */
  if (!var->rel) {
    bool all_zero = true;
    for (int64_t i = 0; i < sz; i++) {
      if (var->init_data[i] != 0) {
        all_zero = false;
        break;
      }
    }
    if (all_zero) {
      sb_printf(sb, "[%" PRIi64 " x i8] = zeroinit, align %d\n", sz, p2);
      return;
    }
  }

  /* Emit braced byte/string + zeroinit + relocation initializer */
  sb_printf(sb, "align %d { ", p2);
  Relocation *rel = var->rel;
  int64_t pos = 0;
  bool first = true;

  while (pos < sz) {
    while (rel && rel->offset < pos)
      rel = rel->next;
    if (rel && rel->offset == pos) {
      if (!first)
        sb_printf(sb, ", ");
      first = false;
      sb_printf(sb, "ptr ");
      sb_emit_sym(sb, pir_sym_name(rel->var));
      if (rel->addend > 0)
        sb_printf(sb, " + %" PRIi64, rel->addend);
      else if (rel->addend < 0)
        sb_printf(sb, " - %" PRIu64, -(uint64_t)rel->addend);
      rel = rel->next;
      pos += 8;
      continue;
    }

    int64_t limit = (rel && rel->offset > pos && rel->offset < sz) ? rel->offset : sz;
    if (var->init_data[pos] == 0) {
      int64_t zend = pos;
      while (zend < limit && var->init_data[zend] == 0)
        zend++;
      if (zend - pos >= 32) {
        if (!first)
          sb_printf(sb, ", ");
        first = false;
        sb_printf(sb, "zeroinit %" PRIi64, zend - pos);
        pos = zend;
        continue;
      }
    }
    /* Emit printable ASCII runs as string chunks, stopping before long zero runs. */
    int64_t run = limit - pos;
    if (run > 64)
      run = 64;
    if (!first)
      sb_printf(sb, ", ");
    first = false;
    sb_emit_escaped_bytes(sb, var->init_data + pos, (size_t)run, true);
    pos += run;
  }
  sb_printf(sb, " }\n");
}

int codegen(Obj *prog, FILE *out) {
  /* 1. Handle inline-only functions (same semantics as slimcc x64 codegen). */
  for (Obj *var = prog; var; var = var->next) {
    if (var->is_definition && var->ty->kind == TY_FUNC && !var->is_static && !var->export_fn) {
      if (var->is_always_inline && opt_fake_always_inline) {
        var->is_static = true;
        continue;
      }
      var->is_definition = false;
      var->output = NULL;
      continue;
    }
    if (var->alias_name)
      error("attribute 'alias' is not supported in PIR backend");
  }

  /* 2. Mark all live globals, functions, and extern references. */
  for (Obj *var = prog; var; var = var->next) {
    if ((!var->is_static || var->is_used) && (var->is_definition || var->alias_name))
      mark_live(var);
  }

  /* 3. Build definitions FIRST (so an extern sharing an __asm__ name with a
   *    definition never suppresses the definition), then extern declarations. */
  SBuf ext_sb, glob_sb;
  sb_init(&ext_sb);
  sb_init(&glob_sb);

  HashMap emitted_syms = {0};

  for (Obj *var = prog; var; var = var->next) {
    if (!var->is_live || !var->is_definition)
      continue;
    const char *sname = pir_sym_name(var);
    if (hashmap_get(&emitted_syms, sname))
      continue;
    hashmap_put(&emitted_syms, sname, (void *)1);
    if (var->ty->kind != TY_FUNC)
      emit_global_def(&glob_sb, var);
  }

  /* Emit live static local variables BEFORE extern declarations so a static
   * local definition sharing an __asm__ name wins over an extern declaration. */
  for (Obj *var = prog; var; var = var->next) {
    if (!var->is_live || !var->is_definition || var->ty->kind != TY_FUNC || !var->output)
      continue;
    for (int i = 0; i < var->output->static_lvar_cnt; i++) {
      StaticLvarOut *sl = &var->output->static_lvars[i];
      if (hashmap_get(&emitted_syms, sl->sym_name))
        continue;
      hashmap_put(&emitted_syms, sl->sym_name, (void *)1);
      if (sl->len > 0)
        sb_append(&glob_sb, sl->buf, sl->len);
    }
  }

  for (Obj *var = prog; var; var = var->next) {
    if (!var->is_live || var->is_definition)
      continue;
    const char *sname = pir_sym_name(var);
    if (hashmap_get(&emitted_syms, sname))
      continue;
    hashmap_put(&emitted_syms, sname, (void *)1);
    emit_extern_decl(&ext_sb, var);
  }

  /* Also emit any extern symbols referenced only via function refs/relocs (not in `prog` list) */
  for (Obj *var = prog; var; var = var->next) {
    if (!var->is_live || !var->is_definition || var->ty->kind != TY_FUNC || !var->output)
      continue;
    for (int i = 0; i < var->output->ref_cnt; i++) {
      Obj *r = var->output->refs[i];
      if (!r || r->is_definition)
        continue;
      const char *sname = pir_sym_name(r);
      if (hashmap_get(&emitted_syms, sname))
        continue;
      hashmap_put(&emitted_syms, sname, (void *)1);
      emit_extern_decl(&ext_sb, r);
    }
  }

  if (need_atomic_lock_helpers) {
    sb_printf(&glob_sb, "weak @__pir_atomic_locks = global [256 x i8] = zeroinit, align 64\n");
  }

  /* 4. Emit final PIR HIR module in canonical order! */
  fprintf(out, "target \"%s\"\n", opt_pir_target ? opt_pir_target : "aarch64-apple-darwin");

  if (opt_g) {
    for (int i = 0; i < debug_files.len; i++) {
      SBuf fsb;
      sb_init(&fsb);
      sb_emit_escaped_str(&fsb, debug_files.data[i]);
      fprintf(out, "file %d %s\n", i, fsb.buf);
      sb_free(&fsb);
    }
  }
  fprintf(out, "\n");

  for (int i = 0; i < pir_type_cnt; i++) {
    fprintf(out, "type @%s = %s\n", pir_types[i].name, pir_types[i].def_str);
  }
  if (pir_type_cnt > 0)
    fprintf(out, "\n");

  for (int i = 0; i < pir_sig_cnt; i++) {
    fprintf(out, "sig @%s = %s\n", pir_sigs[i].name, pir_sigs[i].def_str);
  }
  if (pir_sig_cnt > 0)
    fprintf(out, "\n");

  if (ext_sb.len > 0) {
    fwrite(ext_sb.buf, 1, ext_sb.len, out);
    fprintf(out, "\n");
  }

  if (glob_sb.len > 0) {
    fwrite(glob_sb.buf, 1, glob_sb.len, out);
    fprintf(out, "\n");
  }

  for (Obj *var = prog; var; var = var->next) {
    if (!var->is_live || !var->is_definition || var->ty->kind != TY_FUNC || !var->output)
      continue;
    fwrite(var->output->buf, 1, var->output->buflen, out);
  }

  if (need_atomic_lock_helpers) {
    static const char *atomic_helpers =
      "\n"
      "func @__pir_atomic_lock(%addr: ptr) -> ptr {\n"
      ".entry:\n"
      "    %pi = ptrtoint.i64 %addr\n"
      "    %sh = shr.i64 %pi, 4 [mask]\n"
      "    %idx = and.i64 %sh, 63\n"
      "    %off = shl.i64 %idx, 2 [mask]\n"
      "    %tbl = addr @__pir_atomic_locks\n"
      "    %lp = ptr_add.i64 %tbl, %off\n"
      "    %c1 = const.i32 1\n"
      "    jmp .spin\n"
      ".spin:\n"
      "    %old = atomic_rmw.xchg.i32 %lp, %c1 [acquire]\n"
      "    %busy = cmp_ne.i32 %old, 0\n"
      "    br %busy, .spin, .done\n"
      ".done:\n"
      "    ret %lp\n"
      "}\n"
      "\n"
      "func @__pir_atomic_load_n(%dst: ptr, %src: ptr, %sz: i64) -> void {\n"
      ".entry:\n"
      "    %lp = call @__pir_atomic_lock(%src)\n"
      "    %z = const.i64 0\n"
      "    jmp .loop(%z)\n"
      ".loop(%i: i64):\n"
      "    %done = cmp_ge_u.i64 %i, %sz\n"
      "    br %done, .unlock, .step\n"
      ".step:\n"
      "    %ps = ptr_add.i64 %src, %i\n"
      "    %pd = ptr_add.i64 %dst, %i\n"
      "    %b = load.i8 %ps, align 1\n"
      "    store.i8 %b, %pd, align 1\n"
      "    %ni = add.i64 %i, 1 [wrap]\n"
      "    jmp .loop(%ni)\n"
      ".unlock:\n"
      "    %c0 = const.i32 0\n"
      "    atomic_store.i32 %c0, %lp [release]\n"
      "    ret\n"
      "}\n"
      "\n"
      "func @__pir_atomic_store_n(%dst: ptr, %src: ptr, %sz: i64) -> void {\n"
      ".entry:\n"
      "    %lp = call @__pir_atomic_lock(%dst)\n"
      "    %z = const.i64 0\n"
      "    jmp .loop(%z)\n"
      ".loop(%i: i64):\n"
      "    %done = cmp_ge_u.i64 %i, %sz\n"
      "    br %done, .unlock, .step\n"
      ".step:\n"
      "    %ps = ptr_add.i64 %src, %i\n"
      "    %pd = ptr_add.i64 %dst, %i\n"
      "    %b = load.i8 %ps, align 1\n"
      "    store.i8 %b, %pd, align 1\n"
      "    %ni = add.i64 %i, 1 [wrap]\n"
      "    jmp .loop(%ni)\n"
      ".unlock:\n"
      "    %c0 = const.i32 0\n"
      "    atomic_store.i32 %c0, %lp [release]\n"
      "    ret\n"
      "}\n"
      "\n"
      "func @__pir_atomic_exch_n(%addr: ptr, %newp: ptr, %oldp: ptr, %sz: i64) -> void {\n"
      ".entry:\n"
      "    %lp = call @__pir_atomic_lock(%addr)\n"
      "    %z = const.i64 0\n"
      "    jmp .loop(%z)\n"
      ".loop(%i: i64):\n"
      "    %done = cmp_ge_u.i64 %i, %sz\n"
      "    br %done, .unlock, .step\n"
      ".step:\n"
      "    %pa = ptr_add.i64 %addr, %i\n"
      "    %pn = ptr_add.i64 %newp, %i\n"
      "    %po = ptr_add.i64 %oldp, %i\n"
      "    %bo = load.i8 %pa, align 1\n"
      "    %bn = load.i8 %pn, align 1\n"
      "    store.i8 %bo, %po, align 1\n"
      "    store.i8 %bn, %pa, align 1\n"
      "    %ni = add.i64 %i, 1 [wrap]\n"
      "    jmp .loop(%ni)\n"
      ".unlock:\n"
      "    %c0 = const.i32 0\n"
      "    atomic_store.i32 %c0, %lp [release]\n"
      "    ret\n"
      "}\n"
      "\n"
      "func @__pir_atomic_cas_n(%addr: ptr, %exp: ptr, %des: ptr, %sz: i64) -> i8 {\n"
      ".entry:\n"
      "    %lp = call @__pir_atomic_lock(%addr)\n"
      "    %z = const.i64 0\n"
      "    jmp .cmploop(%z)\n"
      ".cmploop(%i: i64):\n"
      "    %done = cmp_ge_u.i64 %i, %sz\n"
      "    br %done, .eq_init, .chk\n"
      ".chk:\n"
      "    %pa = ptr_add.i64 %addr, %i\n"
      "    %pe = ptr_add.i64 %exp, %i\n"
      "    %ba = load.i8 %pa, align 1\n"
      "    %be = load.i8 %pe, align 1\n"
      "    %same = cmp_eq.i8 %ba, %be\n"
      "    %ni = add.i64 %i, 1 [wrap]\n"
      "    br %same, .cmploop(%ni), .ne_init\n"
      ".eq_init:\n"
      "    jmp .eqloop(%z)\n"
      ".eqloop(%j: i64):\n"
      "    %edone = cmp_ge_u.i64 %j, %sz\n"
      "    br %edone, .ok_ret, .eqstep\n"
      ".eqstep:\n"
      "    %pd = ptr_add.i64 %des, %j\n"
      "    %pda = ptr_add.i64 %addr, %j\n"
      "    %bd = load.i8 %pd, align 1\n"
      "    store.i8 %bd, %pda, align 1\n"
      "    %nj = add.i64 %j, 1 [wrap]\n"
      "    jmp .eqloop(%nj)\n"
      ".ok_ret:\n"
      "    %c0a = const.i32 0\n"
      "    atomic_store.i32 %c0a, %lp [release]\n"
      "    %r1 = const.i8 1\n"
      "    ret %r1\n"
      ".ne_init:\n"
      "    jmp .neloop(%z)\n"
      ".neloop(%k: i64):\n"
      "    %ndone = cmp_ge_u.i64 %k, %sz\n"
      "    br %ndone, .fail_ret, .nestep\n"
      ".nestep:\n"
      "    %pna = ptr_add.i64 %addr, %k\n"
      "    %pne = ptr_add.i64 %exp, %k\n"
      "    %bna = load.i8 %pna, align 1\n"
      "    store.i8 %bna, %pne, align 1\n"
      "    %nk = add.i64 %k, 1 [wrap]\n"
      "    jmp .neloop(%nk)\n"
      ".fail_ret:\n"
      "    %c0b = const.i32 0\n"
      "    atomic_store.i32 %c0b, %lp [release]\n"
      "    %r0 = const.i8 0\n"
      "    ret %r0\n"
      "}\n";
    fputs(atomic_helpers, out);
  }

  sb_free(&ext_sb);
  sb_free(&glob_sb);
  free(emitted_syms.buckets);
  return 0;
}
