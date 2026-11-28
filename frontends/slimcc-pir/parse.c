#include "slimcc.h"

typedef struct {
  Obj *var;
  Type *var_ty;
  Type *type_def;
  EnumVal *enum_val;
} VarScope;

typedef enum {
  PCD_MUL,
  PCD_ADD,
  PCD_SHFT,
  PCD_CMP,
  PCD_EQ,
  PCD_BITAND,
  PCD_XOR,
  PCD_BITOR,
  PCD_LOGAND,
  PCD_LOGOR,
} Preced;

typedef enum {
  SC_NONE = 0,
  SC_AUTO = 1,
  SC_CONSTEXPR = 1 << 1,
  SC_EXTERN = 1 << 2,
  SC_REGISTER = 1 << 3,
  SC_STATIC = 1 << 4,
  SC_THREAD = 1 << 5,
  SC_TYPEDEF = 1 << 6,
  SC_ALL = (1 << 7) - 1,
  SC_INLINE = 1 << 7,
} StorageClass;

typedef struct {
  Node *typeof_vm_expr;
  StorageClass strg : 16;
  bool is_weak;
  bool is_packed;
  bool is_gnu_inline;
  bool is_always_inline;
  bool is_noreturn;
  bool has_noreturn_kw;
  bool is_returns_twice;
  bool is_used;
  bool has_alignas;
  Obj *cleanup_fn;
  char *alias;
  char *section;
  int align;
  int alignas_val;
} VarAttr;

typedef struct Initializer Initializer;
struct Initializer {
  Type *ty;

  enum {
    INIT_NONE = 0,
    INIT_FLEX,
    INIT_FLEX_NESTED,
    INIT_COPY,
    INIT_LIST,
    INIT_STR_ARRAY,
    INIT_EXPR,
    INIT_TOK,
    INIT_NUM_SEQ
  } kind;

  bool is_root;
  bool has_copy;
  bool is_zero;

  ANON_UNION_START
  Token *tok;
  Node *expr;

  struct {
    Initializer *data;
    Node *base_expr;
    int32_t cnt;
    int32_t union_idx;
  } list;

  struct {
    Obj *var;
    Initializer *init;
  } cpy;

  struct {
    Token *start;
    int32_t len;
  } numseq;
  ANON_UNION_END
};

// For local variable initializer.
typedef struct InitDesg InitDesg;
struct InitDesg {
  InitDesg *parent;
  int idx;
  Member *member;
  Obj *var;
};

typedef struct {
  Initializer *init;
  Obj *var;
  Token *end;
  int lvl;
  int init_lvl;
} DesgContext;

struct LocalLabel {
  LocalLabel *next;
  Token *name;
  Node *label;
};

typedef enum {
  EV_CONST,     // constant expression
  EV_LABEL,     // relocation label
  EV_AGGREGATE, // struct/union/array
} EvalKind;

typedef enum {
  EV_ARRAY = 1,
  EV_VOLATILE = 1 << 1,
  EV_ATOMIC = 1 << 2,
  EV_CONSTEXPR = 1 << 3,
} EvalVarSpec;

typedef struct {
  EvalKind kind;
  Obj *var;
  int deref_cnt;
  EvalVarSpec vspec;
} EvalContext;

typedef struct JumpContext JumpContext;
struct JumpContext {
  JumpContext *next;
  DeferStmt *dfr_lvl;
  Token *dfr_ctx;
  Token *labels;
  Node *node;
} *jump_ctx;

typedef struct {
  int32_t val;
  const char *id;
  int id_len;
} PackEntry;

struct {
  PackEntry *data;
  int32_t capacity;
  int32_t cnt;
} pack_stk;

typedef struct FuncContext FuncContext;
struct FuncContext {
  Obj *fn;
  Obj *fnname;
  Node *gotos;
  Node *labels;
  DeferStmt *defr;
  bool use_vla;
  bool dont_dealloc_vla;
  bool is_static_init_context;
  Token *defr_ctx;
};

static Obj *globals = &(Obj){0};
static Scope *scope = &(Scope){0};
static HashMap symbols;
static FuncContext *fnctx;
static bool *eval_recover;
static bool eval_strict;
static int parse_depth = 0;
int uneval_depth = 0;
int void_deref_ok_depth = 0;
static bool is_lvalue_expr(Node *n);
static bool is_ice_expr_syntax(Node *n);
static void strip_proto_vla_exprs(Type *ty);

static bool is_type_kw(TokenKind kind);
static bool is_typename(Token *tok);
static bool comma_list(Token **rest, Token **tok_rest, TokenKind end, bool skip_comma);
static bool braced_list(Token **rest, Token **tok_rest, bool skip_comma);
static Token *skip_bracket(Token *tok);
static Type *typename(Token **rest, Token *tok);
static Type *typename2(Token **rest, Token *tok, VarAttr *attr);
static Type *enum_specifier(Token **rest, Token *tok);
static Type *typeof_specifier(Token **rest, Token *tok, VarAttr *attr);
static Type *declarator(Token **rest, Token *tok, Type *ty, Token **name_tok);
static Type *declarator2(Token **rest, Token *tok, Type *ty, Token **name_tok, bool allow_abstract);
static void declarator3(Type *basety, Type *ty, bool let_star, bool is_fndef);
static QualMask pointer_qualifiers(Token **rest, Token *tok);
static void list_initializer(Token **rest, Token *tok, Initializer *init, int i);
static void initializer2(Token **rest, Token *tok, Initializer *init);
static void initializer3(Token **rest, Token *tok, Initializer *init, Node *expr);
static Node *lvar_initializer(Token **rest, Token *tok, Obj *var, bool try_promote);
static void gvar_initializer(Token **rest, Token *tok, Obj *var);
static void write_gvar_data(Relocation *rel_head, Relocation **cur, Initializer *init, char *buf,
                            int64_t offset, EvalKind ev_kind);
static void constexpr_initializer(Token **rest, Token *tok, Obj *init_var, Obj *var);
static void constexpr_initializer2(Initializer *init, Obj *init_var, Obj *var);
static Node *constexpr_lvar_init(Obj *init_var, Obj *var, Token *tok);
static Node *compound_stmt(Token **rest, Token *tok, NodeKind kind);
static Node *stmt(Token **rest, Token *tok, Token *label_list);
static Node *expr_stmt(Token **rest, Token *tok);
static Node *expression(Token **rest, Token *tok);
static int64_t align_expr(Token **rest, Token *tok);
static int64_t eval(Node *node);
static int64_t eval2(Node *node, EvalContext *ctx);
static Obj *eval_var(Node *node, int64_t *ofs, bool let_volatile);
static bool is_static_const_var(Obj *var);
static Node *assign(Token **rest, Token *tok);
static long_double_t eval_double(Node *node);
static BitBuf *eval_bitint(Node *node);
static void clean_bitint(BitBuf *val, Type *ty);
static bool is_const_bitint_strict(Node *node, BitBuf **result);
static Node *conditional(Token **rest, Token *tok);
static bool addsub_overload(Node **lhs, Node **rhs);
static Node *new_add(Node *lhs, Node *rhs, Token *tok);
static Node *new_sub(Node *lhs, Node *rhs, Token *tok);
static Node *binary(Token **rest, Token *tok, Preced stop);
static Member *get_struct_member(Type *ty, Token *tok);
static Type *struct_union_decl(Token **rest, Token *tok, TypeKind kind);
static Type *struct_decl(Type *ty, int alt_align, int pack_align);
static Type *union_decl(Type *ty, int alt_align, int pack_align);
static Node *postfix(Node *node, Token **rest, Token *tok);
static Node *funcall(Token **rest, Token *tok, Node *node);
static Node *unary(Token **rest, Token *tok);
static Node *primary(Token **rest, Token *tok);
static Node *compound_literal(Token **rest, Token *tok);
static Node *parse_typedef(Token **rest, Token *tok, Type *basety, VarAttr *attr);
static Obj *func_prototype_implicit(Token *name);
static Obj *func_prototype(Token *tok, Token *name, Type *ty, VarAttr *attr, bool is_def);
static void global_declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr);
static Node *calc_vla(Type *ty, Token *tok);
static Node *calc_vla2(Type *ty, Token *tok, VarAttr *attr);
static int64_t const_expr2(Token **rest, Token *tok, Type **ty);
static bool is_const_bitint(Node *node, BitBuf **result);
static bool int_or_trunc_bitint(Node **node, bool chk_narrow);
static Node *resolve_local_gotos(void);
static void push_goto(Node *node);

static bool is_const_context(void) {
  if (fnctx)
    return fnctx->is_static_init_context;
  for (Scope *sc = scope; sc; sc = sc->parent) {
    if (sc->is_fn_base)
      return false;
  }
  return true;
}

static void enter_scope(void) {
  Scope *sc = arena_calloc(&ast_arena, sizeof(Scope));
  sc->parent = scope;
  sc->sibling_next = scope->children;
  scope = scope->children = sc;
}

static void enter_isolated_scope(void) {
  Scope *sc = arena_calloc(&ast_arena, sizeof(Scope));
  sc->parent = scope;
  scope = sc;
}

static void enter_fn_scope(Type *fn_ty) {
  enter_isolated_scope();
  scope->is_fn_base = true;
  fn_ty->scopes = scope;
}

static void enter_tmp_scope(void) {
  enter_scope();
  scope->is_temporary = true;
}

static bool leave_scope(void) {
  if (fnctx) {
    free(scope->vars.buckets);
    free(scope->tags.buckets);
  }
  bool has_label = scope->has_label;
  bool has_alloca = scope->has_alloca;
  scope = scope->parent;
  scope->has_label |= has_label;
  scope->has_alloca |= has_alloca;
  return !has_label;
}

static DeferStmt *enter_stmt_scope(void) {
  enter_scope();
  scope->is_stmt = true;
  return fnctx->defr;
}

static Node *leave_stmt_scope(DeferStmt *defr, Node *stmt_node) {
  bool no_label = leave_scope();

  if (stmt_node->kind == ND_RETURN || stmt_node->kind == ND_GOTO)
    fnctx->defr = defr;

  if (defr == fnctx->defr && !stmt_node->next) {
    stmt_node->no_label = no_label;
    return stmt_node;
  }
  Node *blk = new_node(ND_BLOCK, stmt_node->tok);
  blk->dfr_from = fnctx->defr;
  blk->dfr_dest = fnctx->defr = defr;
  blk->blk.body = stmt_node;
  blk->no_label = no_label;
  return blk;
}

static Scope *base_scope(void) {
  Scope *sc = scope;
  while (sc->parent && !sc->is_fn_base)
    sc = sc->parent;
  if (!sc->is_fn_base)
    internal_error();
  return sc;
}

static Scope *decl_scope(void) {
  Scope *sc = scope;
  while (sc->is_temporary || (sc->is_stmt && opt_std < STD_C99))
    sc = sc->parent;
  return sc;
}

static VarScope *find_var(Token *tok) {
  for (Scope *sc = scope; sc; sc = sc->parent) {
    VarScope *sc2 = hashmap_get2(&sc->vars, tok->loc, tok->len);
    if (sc2)
      return sc2;
  }
  return NULL;
}

static Type *find_tag(Token *tok) {
  for (Scope *sc = scope; sc; sc = sc->parent) {
    Type *ty = hashmap_get2(&sc->tags, tok->loc, tok->len);
    if (ty)
      return ty;
  }
  return NULL;
}

static Type *find_tag_in_scope(Token *tok) {
  return hashmap_get2(&decl_scope()->tags, tok->loc, tok->len);
}

static void push_tag_scope(Token *tag, Type *ty) {
  tag->is_live = true;
  ty->tag = tag;
  hashmap_put2(&decl_scope()->tags, tag->loc, tag->len, ty);
}

Obj *get_symbol_var(const char *name) {
  return hashmap_get(&symbols, name);
}

static bool less_eq(Type *ty, int64_t lhs, int64_t rhs) {
  if (ty->is_unsigned && ty->size == 8)
    return (uint64_t)lhs <= rhs;
  return lhs <= rhs;
}

static bool is_vm_ty(Type *ty) {
  do {
    if (ty->kind == TY_VLA)
      return true;
  } while (vmty_iter(&ty));

  return false;
}

bool equal_tok(Token *a, Token *b) {
  return a->len == b->len && !memcmp(a->loc, b->loc, b->len);
}

static bool equal_substr(const char *loc, size_t len, const char *op) {
  return strlen(op) == len && !memcmp(loc, op, len);
}

Node *new_node(NodeKind kind, Token *tok) {
  Node *node = arena_calloc(&ast_arena, sizeof(Node));
  node->kind = kind;
  node->tok = tok;
  return node;
}

static Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  Node *node = new_node(kind, tok);
  node->m.lhs = lhs;
  node->m.rhs = rhs;
  return node;
}

static Node *new_unary(NodeKind kind, Node *expr, Token *tok) {
  Node *node = new_node(kind, tok);
  node->m.lhs = expr;
  return node;
}

static Node *new_num(int64_t val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->num.val = val;
  return node;
}

static Node *new_size_t(int64_t val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->num.val = val;
  node->ty = ty_size_t;
  return node;
}

static Node *new_boolean(bool val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->num.val = val;
  node->ty = ty_bool;
  return node;
}

Node *new_var_node(Obj *var, Token *tok) {
  Node *node = new_node(ND_VAR, tok);
  node->m.var = var;
  return node;
}

static Node *new_label(NodeKind kind, Token *tok) {
  Node *node = new_node(kind, tok);
  node->dfr_from = fnctx->defr;
  scope->has_label = true;
  return node;
}

static Node *new_unknown(Type *ty, Token *tok) {
  Node *node = new_node(ND_UNKNOWN, tok);
  node->ty = ty;
  return node;
}

static bool invalid_cast(Node *node, Type *to) {
  if (is_decay_ty(to))
    return true;
  if (node->ty->size < 0 && node->ty->kind != TY_ARRAY)
    return true;

  if (to->kind == TY_NULLPTR)
    return !is_null_ptr_constant(node);

  if (node->ty->kind == TY_NULLPTR) {
    switch (to->kind) {
    case TY_VOID:
    case TY_BOOL:
    case TY_PTR:  return false;
    }
    return true;
  }

  if (to->kind != TY_VOID) {
    if (node->ty->kind == TY_VOID)
      return true;
    if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION)
      return is_iso_std ? true : !is_compatible(node->ty, to);
    if (to->kind == TY_STRUCT || to->kind == TY_UNION)
      return true;
  }
  if ((node->ty->base && is_flonum(to)) || (is_flonum(node->ty) && to->base))
    return true;

  return false;
}

Node *new_cast(Node *expr, Type *ty) {
  add_type(expr);
  chk_not_register_array(expr);
  ty = unqual(ty);

  if (invalid_cast(expr, ty))
    error_tok(expr->tok, "invalid cast");

  Node tmp_node = {.kind = ND_CAST, .tok = expr->tok, .m.lhs = expr, .ty = ty};
  int64_t val = 0;
  if (is_integer(ty) && is_const_expr_strict(&tmp_node, &val)) {
    expr->kind = ND_NUM;
    expr->num.val = val;
    expr->ty = ty;
    return expr;
  }

  if (is_redundant_cast(expr, ty)) {
    expr->ty = ty;
    return expr;
  }
  Node *node = arena_malloc(&ast_arena, sizeof(Node));
  *node = tmp_node;
  return node;
}

static Node *cond_cast(Node *expr) {
  switch (expr->kind) {
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE:
  case ND_LOGAND:
  case ND_LOGOR:
  case ND_NOT:    return expr;
  }
  ptr_convert(&expr);
  if (is_flonum(expr->ty)) {
    Node *node = arena_calloc(&ast_arena, sizeof(Node));
    node->kind = ND_CAST;
    node->tok = expr->tok;
    node->m.lhs = expr;
    node->ty = ty_bool;
    node->is_implicit_cast = true;
    return node;
  }
  return new_cast(expr, ty_bool);
}

static void free_initializers(Initializer *init) {
  if (init->kind == INIT_LIST && init->list.cnt) {
    for (int i = 0; i < init->list.cnt; i++)
      free_initializers(&init->list.data[i]);
    free(init->list.data);
  }
}

static void set_init(Initializer *init, int kind) {
  if (init->kind == kind)
    return;
  free_initializers(init);
  init->kind = kind;
}

static void prepare_array_init(Initializer *init, Type *ty) {
  if (init->kind == INIT_LIST)
    return;
  if (ty->array_len > 64LL * 1024 * 1024)
    error("array initializer is too large");

  if (init->kind == INIT_NUM_SEQ) {
    Token *t = init->numseq.start;

    init->kind = INIT_LIST;
    init->list.cnt = (int32_t)ty->array_len;
    init->list.data = calloc(init->list.cnt, sizeof(Initializer));

    int32_t i = 0;
    for (; braced_list(&t, &t, i); i++) {
      if (i >= init->list.cnt)
        break;
      init->list.data[i].kind = INIT_TOK;
      init->list.data[i].tok = t;
      init->list.data[i].ty = ty->base;
      t = t->next;
    }
    for (; i < init->list.cnt; i++)
      init->list.data[i].ty = ty->base;
    return;
  }

  if (init->kind == INIT_STR_ARRAY) {
    Token *tok = init->tok;

    init->kind = INIT_LIST;
    init->list.cnt = ty->array_len;
    init->list.data = calloc(init->list.cnt, sizeof(Initializer));

    int32_t len = MIN(init->ty->array_len, tok->ty->array_len);
    int32_t i = 0;
    for (; i < len; i++) {
      int64_t val;
      switch (init->ty->base->size) {
      case 1:  val = ((uint8_t *)tok->str)[i]; break;
      case 2:  val = ((uint16_t *)tok->str)[i]; break;
      case 4:  val = ((uint32_t *)tok->str)[i]; break;
      default: internal_error();
      }
      init->list.data[i].kind = INIT_EXPR;
      init->list.data[i].expr = new_num(val, tok);
      init->list.data[i].ty = ty->base;
    }
    for (; i < init->list.cnt; i++)
      init->list.data[i].ty = ty->base;
    return;
  }

  init->kind = INIT_LIST;
  if (!(init->list.cnt = ty->array_len))
    return;
  init->list.data = calloc(init->list.cnt, sizeof(Initializer));

  for (int i = 0; i < init->list.cnt; i++)
    init->list.data[i].ty = ty->base;
}

static void prepare_struct_init(Initializer *init, Type *ty) {
  if (init->kind == INIT_LIST)
    return;

  Node *prev_expr = (init->kind == INIT_EXPR) ? init->expr : NULL;

  int len = 0;
  for (Member *mem = ty->members; mem_iter(&mem); mem = mem->next)
    mem->idx = len++;

  init->kind = INIT_LIST;
  init->list.base_expr = prev_expr;
  init->list.union_idx = 0;
  if (!(init->list.cnt = len))
    return;
  init->list.data = calloc(init->list.cnt, sizeof(Initializer));

  for (Member *mem = ty->members; mem_iter(&mem); mem = mem->next) {
    Initializer *child = &init->list.data[mem->idx];

    if (!mem->next && ty->is_flexible) {
      child->ty = mem->ty;
      child->kind = init->is_root ? INIT_FLEX : INIT_FLEX_NESTED;
      break;
    }
    child->ty = mem->ty;
  }
}

static VarScope *push_var_scope(const char *key, int keylen, Obj *var) {
  HashEntry *ent = hashmap_get_or_insert(&decl_scope()->vars, key, keylen);
  VarScope *vsc = ent->val;
  if (vsc)
    return vsc;
  vsc = ent->val = arena_calloc(fnctx ? &ast_arena : &cc1_arena, sizeof(VarScope));
  vsc->var = var;
  return NULL;
}

static void push_var_name2(const char *key, int keylen, Token *tok, Obj *var) {
  VarScope *vsc = push_var_scope(key, keylen, var);
  if (vsc)
    error_tok(tok, "redeclaration of '%.*s'", keylen, key);
}

static void push_var_name(Token *name, Obj *var) {
  push_var_name2(name->loc, name->len, name, var);
}

static VarScope *push_gvar_name2(Token *name, Obj *var) {
  VarScope *vsc = push_var_scope(name->loc, name->len, var);
  if (vsc && var != vsc->var)
    error_tok(name, "invalid redefinition of '%.*s'", name->len, name->loc);
  return vsc ? vsc : hashmap_get2(&decl_scope()->vars, name->loc, name->len);
}

static void push_gvar_name(Token *name, Obj *var) {
  (void)push_gvar_name2(name, var);
}

static Obj *alloc_ast_var(Type *ty) {
  Obj *var = arena_calloc(&ast_arena, sizeof(Obj));
  var->ty = ty;
  return var;
}

static Obj *new_lvar2(Type *ty, Scope *sc) {
  Obj *var = alloc_ast_var(ty);
  var->kind = OBJ_LOCAL;
  var->next = sc->locals;
  sc->locals = var;
  return var;
}

Obj *new_lvar(Type *ty) {
  return new_lvar2(ty, scope);
}

static Obj *alloc_var(char *name, Type *ty) {
  Obj *var = calloc(1, sizeof(Obj));
  var->ty = ty;
  var->name = name;
  return var;
}

static Obj *new_param(char *name, Type *ty) {
  Obj *var = alloc_var(name, ty);
  var->kind = OBJ_LOCAL;
  var->next = scope->locals;
  scope->locals = var;
  return var;
}

static Obj *new_gvar(char *name, Type *ty) {
  return globals = globals->next = alloc_var(name, ty);
}

static Obj *new_static_lvar(Type *ty) {
  static int64_t id = 0;
  char *name = arena_format(&ast_arena, ".L.pv.%" PRIi64, id++);

  Obj *var = alloc_ast_var(ty);
  var->name = name;
  var->is_definition = true;
  var->is_static = true;
  var->is_static_lvar = true;
  var->next = fnctx->fn->static_lvars;
  fnctx->fn->static_lvars = var;
  return var;
}

static Obj *new_anon_gvar(Type *ty) {
  if (fnctx)
    return new_static_lvar(ty);

  static int64_t id = 0;
  char *name = arena_format(&cc1_arena, ".L.gv.%" PRIi64, id++);

  Obj *var = new_gvar(name, ty);
  var->is_definition = true;
  var->is_static = true;
  return var;
}

static DeferStmt *new_defr(DeferKind kind) {
  DeferStmt *defr = arena_calloc(&ast_arena, sizeof(DeferStmt));
  defr->kind = kind;
  if (fnctx->defr) {
    defr->vla = fnctx->defr->vla;
    defr->next = fnctx->defr;
  }
  fnctx->defr = defr;
  return defr;
}

static char *get_ident(Arena *arena, Token *tok) {
  if (tok->kind != TK_IDENT)
    error_tok(tok, "expected an identifier");
  return arena_copy_string(arena, tok->loc, tok->len);
}

static Type *find_typedef(Token *tok) {
  if (tok->kind == TK_IDENT) {
    VarScope *sc = find_var(tok);
    if (sc)
      return sc->type_def;
  }
  return NULL;
}

static Token *ident_tok(Token **rest, Token *tok) {
  if (tok->kind != TK_IDENT)
    error_tok(tok, "expected an identifier");
  *rest = tok->next;
  return tok;
}

static Token *str_tok(Token **rest, Token *tok) {
  if (tok->kind != TK_STR)
    error_tok(tok, "expected string literal");
  *rest = tok->next;
  return tok;
}

static void assembler_name(Token **rest, Token *tok, Obj *var) {
  if (tok->kind == TK_asm) {
    char *str = str_tok(&tok, skip_tk(tok->next, TK_LPAREN))->str;
    *rest = skip_tk(tok, TK_RPAREN);
    if (var->is_definition && var->ty && var->ty->kind == TY_FUNC)
      return;
    if (var->asm_name && strcmp(var->asm_name, str))
      error_tok(tok, "conflict of asm name");
    var->asm_name = str;
  }
}

static void chain_expr(Node **lhs, Node *rhs) {
  if (rhs)
    *lhs = !*lhs ? rhs : new_binary(ND_CHAIN, *lhs, rhs, rhs->tok);
}

static bool comma_list(Token **rest, Token **tok_rest, TokenKind end, bool skip_comma) {
  Token *tok = *tok_rest;
  if (consume_tk(rest, tok, end))
    return false;
  if (skip_comma)
    *tok_rest = skip_tk(tok, TK_COMMA);
  return true;
}

static bool braced_list(Token **rest, Token **tok_rest, bool skip_comma) {
  Token *tok = *tok_rest;
  if (consume_tk(rest, tok, TK_RCURLY))
    return false;

  if (skip_comma) {
    tok = skip_tk(tok, TK_COMMA);

    if (consume_tk(rest, tok, TK_RCURLY))
      return false;

    *tok_rest = tok;
  }
  return true;
}

static void pragma_pack_push(const char *id, int id_len) {
  bool init = !pack_stk.cnt;

  if (pack_stk.cnt == pack_stk.capacity)
    GrowArr(pack_stk.data, &pack_stk.capacity);

  int idx = pack_stk.cnt++;
  pack_stk.data[idx].val = idx ? pack_stk.data[idx - 1].val : 0;
  pack_stk.data[idx].id = idx ? id : NULL;
  pack_stk.data[idx].id_len = idx ? id_len : 0;

  if (init)
    pragma_pack_push(id, id_len);
}

static void pragma_pack_pop(Token *tok, Token *id_tok) {
  if (pack_stk.cnt <= 1) {
    warn_tok(tok, "#pragma pack() stack empty");
    return;
  }
  if (id_tok) {
    for (int i = pack_stk.cnt - 1; i >= 1; i--) {
      if (pack_stk.data[i].id && pack_stk.data[i].id_len == id_tok->len &&
          memcmp(pack_stk.data[i].id, id_tok->loc, (size_t)id_tok->len) == 0) {
        pack_stk.cnt = i;
        return;
      }
    }
    warn_tok(id_tok, "#pragma pack(pop, %.*s) identifier not found", id_tok->len, id_tok->loc);
    return;
  }
  pack_stk.cnt--;
}

static void pragma_pack_set(int val) {
  if (!pack_stk.cnt) {
    pragma_pack_push(NULL, 0);
    pack_stk.cnt = 1;
  }
  pack_stk.data[pack_stk.cnt - 1].val = val;
}

static bool pragma_pack_val(Token **rest, Token *tok, int *out_val) {
  Token *vtok = tok;
  int64_t val = const_expr(rest, tok);
  if (val == 0 || val == 1 || val == 2 || val == 4 || val == 8 || val == 16) {
    *out_val = (int)val;
    return true;
  }
  warn_tok(vtok, "expected #pragma pack parameter to be '1', '2', '4', '8', or '16'");
  return false;
}

static void pragma_pack(Token **rest, Token *tok) {
  if (consume(&tok, tok, "pack")) {
    tok = skip_tk(tok, TK_LPAREN);
    if (equal(tok, "pop")) {
      Token *pop_tok = tok;
      tok = tok->next;
      if (consume_tk(rest, tok, TK_RPAREN)) {
        pragma_pack_pop(pop_tok, NULL);
        return;
      }
      tok = skip_tk(tok, TK_COMMA);
      Token *id_tok = NULL;
      if (tok->kind == TK_IDENT) {
        id_tok = tok;
        tok = tok->next;
        pragma_pack_pop(pop_tok, id_tok);
        if (consume_tk(rest, tok, TK_RPAREN))
          return;
        tok = skip_tk(tok, TK_COMMA);
        int pv = 0;
        if (pragma_pack_val(&tok, tok, &pv))
          pragma_pack_set(pv);
        *rest = skip_tk(tok, TK_RPAREN);
        return;
      }
      pragma_pack_pop(pop_tok, NULL);
      int pv = 0;
      if (pragma_pack_val(&tok, tok, &pv))
        pragma_pack_set(pv);
      *rest = skip_tk(tok, TK_RPAREN);
      return;
    }
    if (equal(tok, "push")) {
      tok = tok->next;
      if (consume_tk(rest, tok, TK_RPAREN)) {
        pragma_pack_push(NULL, 0);
        return;
      }
      tok = skip_tk(tok, TK_COMMA);
      if (tok->kind == TK_IDENT) {
        Token *id_tok = tok;
        tok = tok->next;
        pragma_pack_push(id_tok->loc, id_tok->len);
        if (consume_tk(rest, tok, TK_RPAREN))
          return;
        tok = skip_tk(tok, TK_COMMA);
        int pv = 0;
        if (pragma_pack_val(&tok, tok, &pv))
          pragma_pack_set(pv);
        *rest = skip_tk(tok, TK_RPAREN);
        return;
      }
      pragma_pack_push(NULL, 0);
    }
    if (tok->kind == TK_RPAREN) {
      pragma_pack_set(0);
    } else {
      int pv = 0;
      if (pragma_pack_val(&tok, tok, &pv))
        pragma_pack_set(pv);
    }
    *rest = skip_tk(tok, TK_RPAREN);
    return;
  }
  internal_error();
}

static void attr_aligned(Token *loc, TokenKind kind, int *align) {
  for (Token *tok = loc->attr_next; tok; tok = tok->attr_next) {
    if (tok->kind != kind)
      continue;
    if (equal_ext(tok, "aligned")) {
      Token *tok2;
      if (consume_tk(&tok2, tok->next, TK_LPAREN)) {
        int align2 = align_expr(&tok2, tok2);
        *align = MAX(*align, align2);
        continue;
      }
      *align = MAX(*align, 16);
    }
  }
}

static void attr_cleanup(Token *loc, TokenKind kind, Obj **fn) {
  if (*fn)
    return;
  for (Token *tok = loc->attr_next; tok; tok = tok->attr_next) {
    if (tok->kind != kind)
      continue;
    if (equal_ext(tok, "cleanup")) {
      VarScope *sc = find_var(skip_tk(tok->next, TK_LPAREN));
      if (!(sc && sc->var && sc->var->ty->kind == TY_FUNC))
        error_tok(tok, "cleanup function not found");
      *fn = sc->var;
      return;
    }
  }
}

static void bool_attr(Token *loc, TokenKind kind, const char *name, bool *b) {
  for (Token *tok = loc->attr_next; tok; tok = tok->attr_next) {
    if (tok->kind == kind && equal_ext(tok, name)) {
      *b = true;
      return;
    }
  }
}

static void apply_str_attr(const char *attr_name, Token *tok, char **var_str,
                           char *attr_str) {
  if (!*var_str) {
    *var_str = attr_str;
    return;
  }
  if (attr_str && strcmp(*var_str, attr_str))
    error_tok(tok, "conflict of attribute \'%s\'", attr_name);
}

static void str_attr(Token *loc, TokenKind kind, const char *name, char **str) {
  for (Token *tok = loc->attr_next; tok; tok = tok->attr_next) {
    if (tok->kind == kind && equal_ext(tok, name)) {
      Token *t;
      apply_str_attr(name, tok, str, str_tok(&t, skip_tk(tok->next, TK_LPAREN))->str);
      skip_tk(t, TK_RPAREN);
      return;
    }
  }
}

static void tyspec_attr(Token *tok, VarAttr *attr, TokenKind kind) {
  attr_aligned(tok, kind, &attr->align);
  attr_cleanup(tok, kind, &attr->cleanup_fn);
  bool_attr(tok, kind, "weak", &attr->is_weak);
  bool_attr(tok, kind, "packed", &attr->is_packed);
  bool_attr(tok, kind, "gnu_inline", &attr->is_gnu_inline);
  bool_attr(tok, kind, "always_inline", &attr->is_always_inline);
  bool_attr(tok, kind, "noreturn", &attr->is_noreturn);
  bool_attr(tok, kind, "returns_twice", &attr->is_returns_twice);
  bool_attr(tok, kind, "used", &attr->is_used);
  str_attr(tok, kind, "alias", &attr->alias);
  str_attr(tok, kind, "section", &attr->section);
}

#define DeclAttr(Fn, ...)                    \
  do {                                       \
    if (name) {                              \
      Fn(name, TK_ATTR, __VA_ARGS__);        \
      Fn(name->next, TK_BATTR, __VA_ARGS__); \
    }                                        \
    Fn(tok, TK_ATTR, __VA_ARGS__);           \
  } while (0)

static void symbol_attr(Token *name, Token *tok, VarAttr *attr, Obj *var) {
  if (var->asm_name && (attr->strg & SC_REGISTER))
    error_tok(tok, "global register variables not supported");

  apply_str_attr("alias", name, &var->alias_name, attr->alias);
  DeclAttr(str_attr, "alias", &var->alias_name);

  apply_str_attr("section", name, &var->section_name, attr->section);
  DeclAttr(str_attr, "section", &var->section_name);

  var->is_used |= attr->is_used;
  DeclAttr(bool_attr, "used", &var->is_used);

  var->is_weak |= attr->is_weak;
  DeclAttr(bool_attr, "weak", &var->is_weak);

  if (var->is_weak && var->is_static)
    error_tok(name, "weak declaration cannot be `static`");
}

static void func_attr(Token *name, Token *tok, VarAttr *attr, Obj *fn) {
  fn->is_noreturn |= attr->is_noreturn;
  DeclAttr(bool_attr, "noreturn", &fn->is_noreturn);
  if (is_iso_std && fn->is_noreturn && equal(name, "main"))
    error_tok(name, "'main' cannot be declared '_Noreturn'");

  fn->returns_twice |= attr->is_returns_twice;
  DeclAttr(bool_attr, "returns_twice", &fn->returns_twice);

  fn->is_always_inline |= attr->is_always_inline;
  DeclAttr(bool_attr, "always_inline", &fn->is_always_inline);
}

static void mem_attr(Token *name, Token *tok, VarAttr *attr, Member *mem) {
  mem->is_packed |= attr->is_packed;
  DeclAttr(bool_attr, "packed", &mem->is_packed);
}

static void aligned_attr(Token *name, Token *tok, VarAttr *attr, int *align) {
  DeclAttr(attr_aligned, align);
  *align = MAX(*align, attr->align);
}

static void cleanup_attr(Token *name, Token *tok, VarAttr *attr, Obj *var) {
  Obj *fn = attr->cleanup_fn;
  DeclAttr(attr_cleanup, &fn);

  if (fn) {
    Node *n = new_node(ND_FUNCALL, tok);
    n->call.expr = new_var_node(fn, tok);
    n->call.expr->ty = fn->ty;
    n->ty = ty_void;

    Node *arg = new_unary(ND_ADDR, new_var_node(var, tok), tok);
    add_type(arg);

    n->call.args = alloc_ast_var(arg->ty);
    n->call.args->arg_expr = arg;

    DeferStmt *defr2 = new_defr(DF_CLEANUP_FN);
    defr2->cleanup_fn = n;
  }
}

static bool has_lcurly_before(Token *start, Token *end) {
  for (Token *t = start; t && t != end; t = t->next) {
    if (t->kind == TK_LCURLY)
      return true;
  }
  return false;
}

static void chk_vardecl(Type *ty, Token *name, Token *tok, bool allow_void) {
  if (!name)
    error_tok(tok, "variable name omitted");
  if (ty->kind == TY_VOID && !allow_void)
    error_tok(tok, "variable declared void");
}

typedef enum {
  DECL_CTX_PARAM,
  DECL_CTX_MEMBER,
  DECL_CTX_TYPENAME,
  DECL_CTX_COMPLIT,
  DECL_CTX_TYPEDEF,
  DECL_CTX_FUNC,
} DeclCtxKind;

static void validate_decl_attrs(const VarAttr *attr, DeclCtxKind ctx, Token *tok) {
  switch (ctx) {
  case DECL_CTX_PARAM:
    if (attr->has_alignas)
      error_tok(tok, "_Alignas cannot be applied to a parameter");
    if (attr->has_noreturn_kw)
      error_tok(tok, "'_Noreturn' cannot be applied to a parameter");
    break;
  case DECL_CTX_MEMBER:
    if (attr->has_noreturn_kw)
      error_tok(tok, "'_Noreturn' is not allowed in struct or union member");
    break;
  case DECL_CTX_TYPENAME:
    if (attr->has_alignas)
      error_tok(tok, "'_Alignas' is not allowed in a type name");
    if (attr->has_noreturn_kw)
      error_tok(tok, "'_Noreturn' is not allowed in a type name");
    break;
  case DECL_CTX_COMPLIT:
    if (attr->has_alignas)
      error_tok(tok, "'_Alignas' is not allowed in a compound literal");
    if (attr->has_noreturn_kw)
      error_tok(tok, "'_Noreturn' is not allowed in a compound literal");
    break;
  case DECL_CTX_TYPEDEF:
    if (attr->has_alignas)
      error_tok(tok, "_Alignas cannot appear in a typedef");
    if (attr->has_noreturn_kw)
      error_tok(tok, "'_Noreturn' cannot appear in a typedef");
    break;
  case DECL_CTX_FUNC:
    if (attr->has_alignas)
      error_tok(tok, "_Alignas cannot be applied to a function");
    break;
  }
}

static void chk_alignas_natural(const VarAttr *attr, const Type *ty, Token *tok) {
  if (attr->alignas_val > 0 && ty->align > 0 && attr->alignas_val < ty->align)
    error_tok(tok, "_Alignas cannot reduce natural alignment");
}

typedef enum {
  ICE_I64_STRICT64,
  ICE_I64_CASE,
  ICE_BOOL,
} IceMode;

static void chk_bitint_within_64bit(BitBuf *data, Type *ty, Token *tok);

static int64_t eval_ice_node(Node *node, IceMode mode, Token *err_tok, const char *non_int_msg) {
  bool prv_strict = eval_strict;
  if (is_iso_std && (!node->tok || !node->tok->file || !node->tok->file->is_syshdr))
    eval_strict = true;

  if (is_integer(node->ty)) {
    int64_t v = eval(node);
    eval_strict = prv_strict;
    return (mode == ICE_BOOL) ? (v != 0) : v;
  }
  if (node->ty->kind == TY_BITINT) {
    BitBuf *data = eval_bitint(node);
    eval_strict = prv_strict;
    int64_t res;
    if (mode == ICE_BOOL) {
      res = eval_bitint_to_bool(node->ty->bit_cnt, data) ? 1 : 0;
    } else if (mode == ICE_I64_CASE) {
      clean_bitint(data, node->ty);
      res = (int64_t)data->as64;
    } else {
      chk_bitint_within_64bit(data, node->ty, node->tok);
      res = (int64_t)data->as64;
    }
    free(data);
    return res;
  }
  eval_strict = prv_strict;
  error_tok(err_tok, "%s", non_int_msg);
}

static void chk_inline(VarAttr *attr, Token *tok) {
  if (attr->strg & SC_INLINE)
    error_tok(tok, "only function declarations can be 'inline'");
  if (attr->has_noreturn_kw)
    error_tok(tok, "only function declarations can be '_Noreturn'");
}

static void chk_local_tls(VarAttr *attr, Token *tok) {
  if (attr->strg & SC_THREAD)
    error_tok(tok, "thread local cannot be local");
}

static bool chk_storage_class(StorageClass msk, StorageClass allow) {
  if (msk == SC_NONE)
    return false;
  if (msk & ~allow)
    return true;
  if (msk & SC_AUTO)
    return msk & ~(SC_AUTO | (opt_std >= STD_C23 ? SC_CONSTEXPR : SC_NONE));
  if (msk & SC_CONSTEXPR)
    return msk & ~(SC_CONSTEXPR | SC_AUTO | SC_REGISTER | SC_STATIC);
  if (msk & SC_THREAD)
    return msk & ~(SC_THREAD | SC_STATIC | SC_EXTERN);
  return 1 < Pop64(msk & (SC_EXTERN | SC_REGISTER | SC_STATIC | SC_TYPEDEF));
}

static Type *declspec(Token **rest, Token *tok, VarAttr *attr, StorageClass ctx) {
  enum {
    VOID = 1 << 0,
    BOOL = 1 << 2,
    CHAR = 1 << 4,
    SHORT = 1 << 6,
    INT = 1 << 8,
    LONG = 1 << 10,
    FLOAT = 1 << 12,
    DOUBLE = 1 << 14,
    SIGNED = 1 << 16,
    UNSIGNED = 1 << 18,
    OTHER = 1 << 20,
    BITINT = 1 << 21,
  };

  Type *ty = NULL;
  int32_t counter = 0;
  QualMask qual = 0;

  if (tok->attr_next)
    tyspec_attr(tok, attr, TK_BATTR);

  for (;;) {
    if (tok->kind == TK_PRAGMA) {
      pragma_pack(&tok, tok);
      continue;
    }
    if (tok->attr_next)
      tyspec_attr(tok, attr, TK_ATTR);

    TokenKind tk_kind = tok->kind;
    if (!is_type_kw(tk_kind)) {
      if (!ty && (ty = find_typedef(tok))) {
        tok = tok->next;
        counter |= OTHER;
        continue;
      }
      break;
    }
    Token *kw_tok = tok;
    tok = tok->next;

    switch (tk_kind) {
    case TK_auto:
      if (is_iso_std && (attr->strg & SC_AUTO) && !kw_tok->file->is_syshdr)
        error_tok(kw_tok, "duplicate 'auto' storage class specifier");
      attr->strg |= SC_AUTO; continue;
    case TK_extern:
      if (is_iso_std && (attr->strg & SC_EXTERN) && !kw_tok->file->is_syshdr)
        error_tok(kw_tok, "duplicate 'extern' storage class specifier");
      attr->strg |= SC_EXTERN; continue;
    case TK_register:
      if (is_iso_std && (attr->strg & SC_REGISTER) && !kw_tok->file->is_syshdr)
        error_tok(kw_tok, "duplicate 'register' storage class specifier");
      attr->strg |= SC_REGISTER; continue;
    case TK_static:
      if (is_iso_std && (attr->strg & SC_STATIC) && !kw_tok->file->is_syshdr)
        error_tok(kw_tok, "duplicate 'static' storage class specifier");
      attr->strg |= SC_STATIC; continue;
    case TK_typedef:
      if (is_iso_std && (attr->strg & SC_TYPEDEF) && !kw_tok->file->is_syshdr)
        error_tok(kw_tok, "duplicate 'typedef' storage class specifier");
      attr->strg |= SC_TYPEDEF; continue;
    case TK_thread_local:
      if (is_iso_std && (attr->strg & SC_THREAD) && !kw_tok->file->is_syshdr)
        error_tok(kw_tok, "duplicate '_Thread_local' storage class specifier");
      attr->strg |= SC_THREAD; continue;
    case TK_inline:       attr->strg |= SC_INLINE; continue;
    case TK_constexpr:    attr->strg |= SC_CONSTEXPR; continue;
    case TK_Noreturn:     attr->is_noreturn = true; attr->has_noreturn_kw = true; continue;
    case TK_alignas: {
      tok = skip_tk(tok, TK_LPAREN);
      int align;
      if (is_typename(tok)) {
        Token *at_tok = tok;
        VarAttr attr2 = {0};
        Type *ty2 = typename2(&tok, tok, &attr2);
        if (ty2->kind == TY_VOID || ty2->kind == TY_FUNC || ty2->size < 0)
          error_tok(at_tok, "invalid application of '_Alignas' to incomplete or function type");
        align = attr2.align ? attr2.align : ty2->align;
      } else {
        Token *ae_tok = tok;
        int64_t val = const_expr2(&tok, tok, NULL);
        if (val < 0 || val >= (1LL << 31) || (val != 0 && !is_pow_of_two(val)))
          error_tok(ae_tok, "invalid alignment");
        align = (int)val;
      }
      attr->has_alignas = true;
      if (align > 0) {
        attr->alignas_val = MAX(attr->alignas_val, align);
        attr->align = MAX(attr->align, align);
      }
      tok = skip_tk(tok, TK_RPAREN);
      continue;
    }
    }

    switch (tk_kind) {
    case TK_const:    qual |= Q_CONST; continue;
    case TK_restrict: qual |= Q_RESTRICT; continue;
    case TK_volatile: qual |= Q_VOLATILE; continue;
    case TK_Atomic:
      qual |= Q_ATOMIC;
      if (consume_tk(&tok, tok, TK_LPAREN)) {
        if (ty)
          error_tok(tok, "invalid type");
        Token *at_tok = tok;
        ty = typename(&tok, tok);
        if (is_array(ty) || ty->kind == TY_FUNC || ty->kind == TY_VOID || ty->qual || ty->size < 0)
          error_tok(at_tok, "invalid type for '_Atomic'");
        tok = skip_tk(tok, TK_RPAREN);
        counter |= OTHER;
      }
      continue;
    }

    if (!ty) {
      switch (tk_kind) {
      case TK_struct:        ty = struct_union_decl(&tok, tok, TY_STRUCT); break;
      case TK_union:         ty = struct_union_decl(&tok, tok, TY_UNION); break;
      case TK_enum:          ty = enum_specifier(&tok, tok); break;
      case TK_typeof:        ty = typeof_specifier(&tok, tok, attr); break;
      case TK_typeof_unqual: ty = tyof_unqual(typeof_specifier(&tok, tok, attr)); break;
      case TK_auto_type:
        if (is_iso_std && !kw_tok->file->is_syshdr)
          error_tok(kw_tok, "'__auto_type' is a GNU extension");
        ty = new_type(TY_AUTO, -1, 0);
        break;
      }
      if (ty) {
        counter |= OTHER;
        continue;
      }
    }

    switch (tk_kind) {
    case TK_void:     counter += VOID; break;
    case TK_bool:     counter += BOOL; break;
    case TK_char:     counter += CHAR; break;
    case TK_short:    counter += SHORT; break;
    case TK_int:      counter += INT; break;
    case TK_long:     counter += LONG; break;
    case TK_float:    counter += FLOAT; break;
    case TK_double:   counter += DOUBLE; break;
    case TK_signed:   counter += SIGNED; break;
    case TK_unsigned: counter += UNSIGNED; break;
    case TK_BitInt:
      if (is_iso_std && opt_std < STD_C23 && !kw_tok->file->is_syshdr)
        error_tok(kw_tok, "'_BitInt' is a C23 extension");
      counter += BITINT;
      ty = new_bitint(const_expr(&tok, skip_tk(tok, TK_LPAREN)), tok);
      tok = skip_tk(tok, TK_RPAREN);
      break;
    default: error_tok(tok, "invalid type");
    }

    switch (counter) {
    case VOID:                         ty = ty_void; break;
    case BOOL:                         ty = ty_bool; break;
    case CHAR:                         ty = ty_pchar; break;
    case SIGNED + CHAR:                ty = ty_char; break;
    case UNSIGNED + CHAR:              ty = ty_uchar; break;
    case SHORT:
    case SHORT + INT:
    case SIGNED + SHORT:
    case SIGNED + SHORT + INT:         ty = ty_short; break;
    case UNSIGNED + SHORT:
    case UNSIGNED + SHORT + INT:       ty = ty_ushort; break;
    case INT:
    case SIGNED:
    case SIGNED + INT:                 ty = ty_int; break;
    case UNSIGNED:
    case UNSIGNED + INT:               ty = ty_uint; break;
    case LONG:
    case LONG + INT:
    case SIGNED + LONG:
    case SIGNED + LONG + INT:          ty = ty_long; break;
    case LONG + LONG:
    case LONG + LONG + INT:
    case SIGNED + LONG + LONG:
    case SIGNED + LONG + LONG + INT:   ty = ty_llong; break;
    case UNSIGNED + LONG:
    case UNSIGNED + LONG + INT:        ty = ty_ulong; break;
    case UNSIGNED + LONG + LONG:
    case UNSIGNED + LONG + LONG + INT: ty = ty_ullong; break;
    case FLOAT:                        ty = ty_float; break;
    case DOUBLE:                       ty = ty_double; break;
    case LONG + DOUBLE:                ty = ty_ldouble; break;
    case BITINT:
    case BITINT + SIGNED:              break;
    case BITINT + UNSIGNED:            ty->is_unsigned = true; break;
    default:                           error_tok(tok, "invalid type");
    }
  }
  *rest = tok;

  if (ty && ty->kind == TY_BITINT && !ty->is_unsigned && ty->bit_cnt < 2)
    error_tok(tok, "signed _BitInt requires width >= 2");

  if (chk_storage_class(attr->strg, ctx)) {
    if ((attr->strg & SC_INLINE) > (ctx & SC_INLINE))
      error_tok(tok, "'inline' not allowed here");
    error_tok(tok, "invalid storage class");
  }

  if (!ty) {
    if (opt_std < STD_C99) {
      ty = ty_int;
    } else if (opt_std >= STD_C23 && (attr->strg & SC_AUTO)) {
      ty = new_type(TY_AUTO, -1, 0);
    } else {
      if (tok->kind == TK_IDENT)
        error_tok(tok, "unknown type name");
      error_tok(tok, "implicit int only supported under -std=c89");
    }
  }

  if (ty->kind == TY_AUTO)
    if (tok->kind != TK_IDENT || tok->next->kind != TK_EQ)
      error_tok(tok, "unsupported form for type inference");

  return add_qual(qual, ty, tok);
}

static Type *qual_constexpr(Type *ty, VarAttr *attr, Token *tok) {
  if (attr->strg & SC_CONSTEXPR) {
    if (eff_qual(ty) & (Q_VOLATILE | Q_ATOMIC | Q_RESTRICT))
      error_tok(tok, "invalid qualifier for 'constexpr'");
    return add_qual(Q_CONST, ty, tok);
  }
  return ty;
}

static Type *param_declarator(Type *basety, Type *ty, Token *tok, bool is_fndef) {
  QualMask arr_qual = 0;
  bool has_static = false;
  if (ty->kind == TY_ARRAY_INCMP) {
    Token *t = ty->tag;

    has_static = consume_tk(&t, t, TK_static);
    if (is_type_kw(t->kind)) {
      arr_qual = pointer_qualifiers(&t, t);
      if (!has_static)
        has_static = consume_tk(&t, t, TK_static);
    }
    ty->tag = t;
  }

  declarator3(basety, ty, !is_fndef, false);

  if (has_static)
    if ((ty->kind == TY_ARRAY && ty->array_len < 0) ||
        (ty->kind == TY_VLA && ty->vla_len_expr && ty->vla_len_expr->kind == ND_UNKNOWN))
      error_tok(tok, "'static' requires an array size");

  ty = add_qual(arr_qual & ~Q_ATOMIC, ptr_decay(ty), tok);

  if (ty->kind == TY_VOID || (ty->size < 0 && is_fndef))
    error_tok(tok, "invalid parameter type");

  return ty;
}

static void func_params(Token *tok, Type *fn_ty, bool is_def) {
  fn_ty->kind = TY_FUNC;

  if (tok->kind == TK_DOT3 && tok->next->kind == TK_RPAREN) {
    if (is_iso_std && opt_std < STD_C23 && !tok->file->is_syshdr)
      error_tok(tok, "ISO C requires a named argument before '...'");
    fn_ty->is_variadic = true;
    return;
  }
  if (tok->kind == TK_void && tok->next->kind == TK_RPAREN)
    return;

  if (tok->kind == TK_RPAREN) {
    if (opt_std < STD_C23)
      fn_ty->is_oldstyle = true;
    return;
  }

  if (is_typename(tok)) {
    enter_fn_scope(fn_ty);

    Obj head = {0};
    Obj *cur = &head;
    Node *expr = NULL;

    while (comma_list(&tok, &tok, TK_RPAREN, cur != &head)) {
      if (tok->kind == TK_DOT3) {
        fn_ty->is_variadic = true;
        skip_tk(tok->next, TK_RPAREN);
        break;
      }
      VarAttr attr = {0};
      Type *basety = declspec(&tok, tok, &attr, SC_REGISTER);
      validate_decl_attrs(&attr, DECL_CTX_PARAM, tok);

      if (is_def)
        chain_expr(&expr, calc_vla2(basety, tok, &attr));

      Token *name = NULL;
      Type *raw_ty = declarator2(&tok, tok, basety, &name, true);
      if (cur == &head && !name && raw_ty == basety && basety->kind == TY_VOID &&
          !basety->qual && attr.strg == SC_NONE && tok->kind == TK_RPAREN)
        break;
      if (is_def && !name && is_iso_std && opt_std < STD_C23 && !tok->file->is_syshdr)
        error_tok(tok, "parameter name omitted in function definition");
      Type *param_ty = param_declarator(basety, raw_ty, tok, is_def);

      if (is_def)
        chain_expr(&expr, calc_vla(raw_ty, tok));

      cur = cur->param_next = new_param(NULL, param_ty);
      cur->is_register = !!(attr.strg & SC_REGISTER);
      aligned_attr(name, tok, &attr, &cur->alt_align);
      if (name)
        push_var_name(name, cur);
    }
    fn_ty->pre_calc = expr;
    fn_ty->param_list = head.param_next;
    leave_scope();
    return;
  }

  if (tok->kind == TK_IDENT) {
    fn_ty->is_oldstyle = true;

    Token *start = tok;
    if (!is_def) {
      if (is_iso_std && !tok->file->is_syshdr)
        error_tok(tok, "identifier list in function declarator that is not a definition");
      while (comma_list(&tok, &tok, TK_RPAREN, tok != start))
        ident_tok(&tok, tok);
      return;
    }
    enter_fn_scope(fn_ty);

    Obj head = {0};
    Obj *cur = &head;
    while (comma_list(&tok, &tok, TK_RPAREN, tok != start)) {
      Token *name = ident_tok(&tok, tok);
      cur = cur->param_next = new_param(get_ident(&ast_arena, name), NULL);
    }
    fn_ty->param_list = head.param_next;
    leave_scope();
    return;
  }

  error_tok(tok, "expected function parameter");
}

static Node *array_dimension(Token *tok, bool let_star) {
  if (consume_tk(&tok, tok, TK_RBRACK))
    return NULL;

  if (tok->kind == TK_MUL && tok->next->kind == TK_RBRACK) {
    if (!let_star)
      error_tok(tok, "`[*]` not allowed here");
    return new_unknown(ty_size_t, tok);
  }
  Node *expr = assign(&tok, tok);
  skip_tk(tok, TK_RBRACK);
  return expr;
}

static void array_dimension2(Type *ty, Token *tok, Node *expr) {
  Type *basety = ty->base;
  if (basety->size < 0 || basety->kind == TY_VOID || basety->kind == TY_FUNC)
    error_tok(tok, "invalid array element type");
  if (is_iso_std && basety->is_flexible && !tok->file->is_syshdr)
    error_tok(tok, "array of structs with flexible array member is not allowed in ISO C");

  if (!expr) {
    array_setty(ty, basety, -1);
    return;
  }

  if (!int_or_trunc_bitint(&expr, true))
    error_tok(expr->tok, "size of array not integer");

  int64_t array_len;
  if (is_const_expr_strict(expr, &array_len)) {
    if (array_len < 0)
      error_tok(expr->tok, "size of array is negative");
    if (is_iso_std && array_len == 0 && !expr->tok->file->is_syshdr)
      error_tok(expr->tok, "zero-size array is not allowed in ISO C");
    if (basety->kind == TY_VLA) {
      vla_setty(ty, NULL, array_len);
      return;
    }
    array_setty(ty, basety, array_len);
    return;
  }
  if (is_iso_std && !expr->tok->file->is_syshdr && is_ice_expr_syntax(expr))
    error_tok(expr->tok, "invalid constant expression for array size");
  vla_setty(ty, expr, 0);
  return;
}

static QualMask pointer_qualifiers(Token **rest, Token *tok) {
  QualMask qual = 0;
  for (;; tok = tok->next) {
    switch (tok->kind) {
    case TK_Atomic:   qual |= Q_ATOMIC; continue;
    case TK_const:    qual |= Q_CONST; continue;
    case TK_restrict: qual |= Q_RESTRICT; continue;
    case TK_volatile: qual |= Q_VOLATILE; continue;
    }
    break;
  }
  *rest = tok;
  return qual;
}

static Token *skip_bracket(Token *tok) {
  int level = 0;
  Token *start = tok;
  for (;;) {
    if (level == 0 && tok->kind == TK_RBRACK)
      break;

    if (tok->kind == TK_EOF)
      error_tok(start, "unterminated list");

    if (tok->kind == TK_LBRACK)
      level++;
    else if (tok->kind == TK_RBRACK)
      level--;

    tok = tok->next;
  }
  return tok->next;
}

Token *skip_paren(Token *tok) {
  int level = 0;
  Token *start = tok;
  for (;;) {
    if (level == 0 && tok->kind == TK_RPAREN)
      break;

    if (tok->kind == TK_EOF)
      error_tok(start, "unterminated list");

    if (tok->kind == TK_LPAREN)
      level++;
    else if (tok->kind == TK_RPAREN)
      level--;

    tok = tok->next;
  }
  return tok->next;
}

static Type *array_suffix(Token **rest, Token *tok, Type *ty) {
  Token *end = skip_bracket(tok);

  if (end->kind == TK_LBRACK)
    ty = array_suffix(&end, end->next, ty);
  *rest = end;

  Type *arr_ty = array_type(ty);
  arr_ty->kind = TY_ARRAY_INCMP;
  arr_ty->tag = tok;
  return arr_ty;
}

static Type *func_suffix(Token **rest, Token *tok, Type *ty) {
  *rest = skip_paren(tok);

  Type *fn_ty = func_type(TY_FUNC_INCMP, ty);
  fn_ty->tag = tok;
  return fn_ty;
}

static Type *type_suffix(Token **rest, Token *tok, Type *ty) {
  if (consume_tk(&tok, tok, TK_LBRACK))
    return array_suffix(rest, tok, ty);

  if (tok->kind == TK_LPAREN)
    return func_suffix(rest, tok->next, ty);

  *rest = tok;
  return ty;
}

static Type *declarator2(Token **rest, Token *tok, Type *ty, Token **name_tok, bool allow_abstract) {
  if (++parse_depth > 256)
    error_tok(tok, "declarator nesting depth exceeded");
  while (tok->kind == TK_PRAGMA)
    pragma_pack(&tok, tok);
  while (consume_tk(&tok, tok, TK_MUL)) {
    QualMask q = pointer_qualifiers(&tok, tok);
    ty = add_qual(q, pointer_to(ty), tok);
  }

  if (consume_tk(&tok, tok, TK_LPAREN)) {
    if (allow_abstract && (is_typename(tok) || tok->kind == TK_DOT3 || tok->kind == TK_RPAREN)) {
      Type *r = func_suffix(rest, tok, ty);
      parse_depth--;
      return r;
    }

    ty = type_suffix(rest, skip_paren(tok), ty);
    Token *inner_end = NULL;
    ty = declarator2(&inner_end, tok, ty, name_tok, allow_abstract);
    skip_tk(inner_end, TK_RPAREN);
    parse_depth--;
    return ty;
  }

  if (name_tok && tok->kind == TK_IDENT) {
    *name_tok = tok;
    tok = tok->next;
  }
  Type *r = type_suffix(rest, tok, ty);
  parse_depth--;
  return r;
}

static void declarator3(Type *basety, Type *ty, bool let_star, bool is_fndef) {
  for (;;) {
    if (ty == basety)
      return;

    if (ty->kind == TY_PTR) {
      ty = ty->base;
      continue;
    }
    if (ty->kind == TY_FUNC_INCMP) {
      Token *tok = ty->tag;
      ty->tag = NULL;
      func_params(tok, ty, is_fndef);

      declarator3(basety, ty->return_ty, let_star, false);

      unqual_rtn_ty(ty, tok);
      return;
    }
    if (ty->kind == TY_ARRAY_INCMP) {
      Token *tok = ty->tag;
      ty->tag = NULL;
      Node *expr = array_dimension(tok, let_star);

      declarator3(basety, ty->base, let_star, false);

      array_dimension2(ty, tok, expr);
      return;
    }
    internal_error();
  }
}

static Type *declarator(Token **rest, Token *tok, Type *basety, Token **name_tok) {
  Type *ty = declarator2(rest, tok, basety, name_tok, name_tok == NULL);
  declarator3(basety, ty, false, false);
  return ty;
}

static Type *typename2(Token **rest, Token *tok, VarAttr *attr) {
  Token *start = tok;
  Type *ty = declspec(&tok, tok, attr, SC_NONE);
  validate_decl_attrs(attr, DECL_CTX_TYPENAME, start);
  return declarator(rest, tok, ty, NULL);
}

static Type *typename(Token **rest, Token *tok) {
  return typename2(rest, tok, &(VarAttr){0});
}

static uint64_t enum_ovf_val(Type *ty) {
  int64_t shft = bit_size(ty) - !ty->is_unsigned;
  return shft < 64 ? ((uint64_t)1 << shft) : 0;
}

static bool chk_enum_fixed_range(bool is_neg, Type *ty, uint64_t val, uint64_t ovf_val) {
  if (is_neg) {
    if (ty->is_unsigned)
      return true;

    // ovf_val == 2^(n-1), valid negative range of val is [-2^(n-1), -1]
    return (int64_t)(ovf_val + val) < 0;
  }
  return val > (ovf_val - 1);
}

static Type *enum_ty_tag(Token *tag, Type *tag_ty) {
  if (tag_ty) {
    if (!tag_ty->is_enum)
      error_tok(tag, "mismatched tag type");
    return tag_ty;
  }

  Type *ty = new_type(TY_ENUM_INCMP, -1, 0);
  ty->is_enum = true;

  if (tag)
    push_tag_scope(tag, ty);
  return ty;
}

static Type *enum_specifier(Token **rest, Token *tok) {
  bool is_packed = false;
  bool_attr(tok, TK_ATTR, "packed", &is_packed);
  bool_attr(tok, TK_BATTR, "packed", &is_packed);

  Token *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  bool is_fixed = tok->kind == TK_COLON && is_typename(tok->next);

  Type *tag_ty = NULL;
  if (tag) {
    if (tok->kind != TK_LCURLY && !is_fixed) {
      *rest = tok;
      Type *found = find_tag(tag);
      if (is_iso_std && !tag->file->is_syshdr &&
          (!found || found->kind == TY_ENUM_INCMP))
        error_tok(tag, "ISO C forbids forward references to 'enum' types");
      return enum_ty_tag(tag, found);
    }

    if ((tag_ty = find_tag_in_scope(tag)))
      if (tag_ty->is_fixed_enum != is_fixed)
        error_tok(tag, "enum redeclared with%s fixed underlying type",
                  is_fixed ? "" : "out");
  }

  Type *ty = enum_ty_tag(tag, tag_ty);

  if (is_fixed) {
    if (is_iso_std && opt_std < STD_C23 && !tok->file->is_syshdr)
      error_tok(tok, "enum with fixed underlying type is a C23 extension");
    Type *fixed_ty = unqual(typename(&tok, tok->next));

    if (fixed_ty->is_enum || !is_int_class(fixed_ty))
      error_tok(tok, "invalid enum underlying type");
    if (fixed_ty->kind == TY_BITINT && fixed_ty->bit_cnt > 64)
      error_tok(tok, "large _BitInt unsupported here");

    if (tag_ty) {
      if (fixed_ty->kind != tag_ty->kind ||
          fixed_ty->is_unsigned != tag_ty->is_unsigned ||
          (fixed_ty->kind == TY_BITINT && fixed_ty->bit_cnt != tag_ty->bit_cnt))
        error_tok(tag, "enum redeclared with mismatched fixed underlying type");
    } else {
      *ty = *fixed_ty;
      ty->is_enum = ty->is_fixed_enum = true;
      ty->tag = tag;
    }

    if (tag && tok->kind == TK_SEMI) {
      *rest = tok;
      return ty;
    }
  }

  tok = skip_tk(tok, TK_LCURLY);

  bool is_redecl = false;
  if (tag_ty && tag_ty->kind != TY_ENUM_INCMP) {
    if (opt_std < STD_C23)
      error_tok(tag, "redefinition of '%.*s'", tag->len, tag->loc);
    else if (tag_ty->enums)
      is_redecl = true;
  }

  int64_t min_val = 0;
  uint64_t max_val = 0;
  HashMap redecl_map = {0};
  EnumVal head = {0};
  EnumVal *cur = &head;

  Type *dyn_ty;
  uint64_t ovf_val;
  if (ty->is_fixed_enum) {
    dyn_ty = NULL;
    ovf_val = enum_ovf_val(ty);
  } else {
    // assume 32-bit int
    dyn_ty = ty_int;
    ovf_val = (uint64_t)INT32_MAX + 1;
  }

  uint64_t val = 0;
  bool is_neg = false;
  bool first = true;
  for (; braced_list(&tok, &tok, !first); first = false, val++) {
    Token *name = ident_tok(&tok, tok);

    if (consume_tk(&tok, tok, TK_EQ)) {
      Type *val_ty;
      val = const_expr2(&tok, tok, &val_ty);

      is_neg = (!val_ty->is_unsigned && (int64_t)val < 0);

      if (dyn_ty) {
        // assume 32-bit int
        if (is_neg ? (int64_t)val >= INT32_MIN : val <= INT32_MAX) {
          dyn_ty = ty_int;
          ovf_val = (uint64_t)INT32_MAX + 1;
        } else {
          if (is_iso_std && opt_std < STD_C23 && !name->file->is_syshdr)
            error_tok(name, "enumerator value is not representable as 'int'");
          dyn_ty = unqual(val_ty);
          ovf_val = enum_ovf_val(dyn_ty);
        }
      } else {
        if (chk_enum_fixed_range(is_neg, ty, val, ovf_val))
          error_tok(name, "enum value out of underlying type range");
      }
    } else {
      if (val == 0)
        is_neg = false;

      if (val == ovf_val) {
        if (dyn_ty) {
          if (is_iso_std && opt_std < STD_C23 && !name->file->is_syshdr)
            error_tok(name, "enumerator value overflows 'int'");
          // assume 32-bit int
          if (!dyn_ty->is_unsigned && (ovf_val - 1 < INT64_MAX))
            dyn_ty = enum_ty[ETY_I64], ovf_val = (uint64_t)INT64_MAX + 1;
          else if (dyn_ty->is_unsigned && (ovf_val - 1 < UINT64_MAX))
            dyn_ty = enum_ty[ETY_U64], ovf_val = (uint64_t)UINT64_MAX + 1;
          else
            error_tok(name, "enum value overflowed");
        } else {
          if (!first)
            error_tok(name, "enum value overflowed");
        }
      }
    }

    HashEntry *ent = hashmap_get_or_insert(&decl_scope()->vars, name->loc, name->len);
    VarScope *vsc = ent->val;
    if (vsc) {
      if (is_redecl && vsc->enum_val && vsc->enum_val->ty == ty && vsc->enum_val->val == val) {
        HashEntry *re_ent = hashmap_get_or_insert(&redecl_map, name->loc, name->len);
        if (!re_ent->val) {
          re_ent->val = (void *)1;
          continue;
        }
      }
      error_tok(name, "redefinition of '%.*s'", name->len, name->loc);
    } else {
      cur = cur->next = arena_malloc(&cc1_arena, sizeof(EnumVal));
      cur->val = val;
      cur->ty = dyn_ty ? dyn_ty : ty;
      cur->name = name;
      name->is_live = true;

      vsc = ent->val = arena_calloc(fnctx ? &ast_arena : &cc1_arena, sizeof(VarScope));
      vsc->enum_val = cur;

      if (is_neg)
        min_val = MIN(min_val, (int64_t)val);
      else
        max_val = MAX(max_val, val);
    }
  }
  cur->next = NULL;

  bool_attr(tok, TK_ATTR, "packed", &is_packed);
  *rest = tok;

  if (is_redecl) {
    free(redecl_map.buckets);

    int64_t cnt = 0;
    for (EnumVal *ev = ty->enums; ev; ev = ev->next)
      cnt++;
    if (cnt != redecl_map.used || head.next)
      error_tok(tag, "enum redeclaration mismatch");
    return ty;
  }

  if (!(ty->enums = head.next))
    error_tok(tok, "empty enum specifier");

  if (ty->is_fixed_enum)
    return ty;

  for (EnumVal *ev = ty->enums; ev; ev = ev->next)
    ev->ty = ty;

  EnumType ety = ETY_I8;
  if (min_val < INT8_MIN) {
    if (min_val >= INT16_MIN)
      ety = MAX(ety, ETY_I16);
    else if (min_val >= INT32_MIN)
      ety = MAX(ety, ETY_I32);
    else
      ety = MAX(ety, ETY_I64);
  }

  if (max_val > INT8_MAX) {
    if (max_val <= UINT8_MAX)
      ety = MAX(ety, ETY_U8);
    else if (max_val <= INT16_MAX)
      ety = MAX(ety, ETY_I16);
    else if (max_val <= UINT16_MAX)
      ety = MAX(ety, ETY_U16);
    else if (max_val <= INT32_MAX)
      ety = MAX(ety, ETY_I32);
    else if (max_val <= UINT32_MAX)
      ety = MAX(ety, ETY_U32);
    else if (max_val <= INT64_MAX)
      ety = MAX(ety, ETY_I64);
    else
      ety = ETY_U64;
  }

  if (!(is_packed || opt_short_enums))
    ety = MAX(ety, ety_of_int);

  bool is_int = ety <= ety_of_int;
  if ((min_val && (ety & 1)) || (!min_val && !(ety & 1))) {
    ety++;
    if (ety > ETY_U64)
      error_tok(tok, "unsupported enum value range");
  }

  Type *base_ty = enum_ty[ety];

  for (Type *t = ty; t; t = t->decl_next) {
    t->kind = base_ty->kind;
    t->is_unsigned = base_ty->is_unsigned;
    t->size = base_ty->size;
    t->align = MAX(t->align, base_ty->align);
    t->is_int_enum = is_int;
  }
  return ty;
}

static Type *typeof_specifier(Token **rest, Token *tok, VarAttr *attr) {
  tok = skip_tk(tok, TK_LPAREN);

  Type *ty;
  if (is_typename(tok)) {
    VarAttr attr2 = {0};
    ty = declspec(&tok, tok, &attr2, 0);
    ty = declarator(&tok, tok, ty, NULL);
  } else {
    Node *node = expression(&tok, tok);
    add_type(node);
    ty = node->ty;

    if (is_vm_ty(ty))
      attr->typeof_vm_expr = node;
  }
  *rest = skip_tk(tok, TK_RPAREN);
  return ty;
}

static Node *vla_count(Type *ty, Token *tok) {
  if (!ty->vla_len_expr)
    return new_size_t(ty->array_len, tok);
  if (ty->vla_len_val)
    return new_var_node(ty->vla_len_val, tok);
  return ty->vla_len_expr;
}

static Node *vla_size(Type *ty, Token *tok) {
  Node *base_sz;
  if (ty->base->kind == TY_VLA)
    base_sz = vla_size(ty->base, tok);
  else
    base_sz = new_size_t(ty->base->size, tok);

  return new_binary(ND_MUL, vla_count(ty, tok), base_sz, tok);
}

static Node *ptr_base_size(Type *ty, Token *tok) {
  if (ty->base->kind == TY_VLA)
    return vla_size(ty->base, tok);
  if (is_iso_std && (ty->base->kind == TY_VOID || ty->base->kind == TY_FUNC))
    error_tok(tok, "arithmetic on pointer to void or function type");
  if (ty->base->size < 0)
    error_tok(tok, "pointer has incomplete type");
  return new_size_t(ty->base->size, tok);
}

static Node *calc_vla(Type *ty, Token *tok) {
  Node *n = NULL;
  do {
    if (ty->kind == TY_VLA && ty->vla_len_expr && !ty->vla_len_val) {
      if (is_const_context()) {
        chain_expr(&n, new_unknown(ty_size_t, ty->vla_len_expr->tok));
        continue;
      }
      ty->vla_len_val = new_lvar2(ty_size_t, base_scope());
      chain_expr(&n, new_binary(ND_ASSIGN, new_var_node(ty->vla_len_val, tok),
                                ty->vla_len_expr, tok));
    }
  } while (vmty_iter(&ty));
  return n;
}

static Node *calc_vla2(Type *ty, Token *tok, VarAttr *attr) {
  Node *n = attr->typeof_vm_expr;
  chain_expr(&n, calc_vla(ty, tok));
  return n;
}

static void chk_incomplete(Type *ty, Token *tok) {
  if (ty->size < 0)
    error_tok(tok, "variable has incomplete type");
}

static Node *declaration2(Token **rest, Token *tok, Type *basety, VarAttr *attr,
                          Obj **cond_var, bool *declared_obj) {
  Token *name = NULL;
  Type *ty = declarator(&tok, tok, basety, &name);

  if (ty->kind == TY_FUNC) {
    if (attr->strg & SC_STATIC)
      error_tok(tok, "static function not in file scope");
    validate_decl_attrs(attr, DECL_CTX_FUNC, name ? name : tok);
    strip_proto_vla_exprs(ty);
    Obj *fn = func_prototype(tok, name, ty, attr, false);
    if (!fn->is_definition && !(fnctx && fnctx->fn == fn))
      strip_proto_vla_exprs(fn->ty);
    assembler_name(&tok, tok, fn);
    if (declared_obj)
      *declared_obj = false;
    *rest = tok;
    return NULL;
  }

  if (declared_obj)
    *declared_obj = true;
  chk_vardecl(ty, name, tok, false);
  chk_inline(attr, tok);
  ty = qual_constexpr(ty, attr, tok);

  Node *expr = calc_vla(ty, tok);

  if (attr->strg & SC_STATIC) {
    if (ty->kind == TY_VLA)
      error_tok(tok, "variable length arrays cannot be 'static'");
    if (is_iso_std && fnctx && fnctx->fn->is_ext_inline && !fnctx->fn->is_static &&
        !fnctx->fn->export_fn && !(eff_qual(ty) & Q_CONST) && !tok->file->is_syshdr)
      error_tok(name, "non-const static local variable in external inline function");

    Obj *var = new_static_lvar(ty);
    if (is_vm_ty(ty))
      new_defr(DF_VM_BARRIER);
    push_var_name(name, var);

    if (attr->strg & SC_THREAD)
      var->kind = OBJ_TLS;
    assembler_name(&tok, tok, var);
    chk_alignas_natural(attr, ty, name);
    aligned_attr(name, tok, attr, &var->alt_align);
    symbol_attr(name, tok, attr, var);

    if (attr->strg & SC_CONSTEXPR) {
      constexpr_initializer(&tok, skip_tk(tok, TK_EQ), var, var);
      *cond_var = var;
    } else if (tok->kind == TK_EQ) {
      gvar_initializer(&tok, tok->next, var);
      *cond_var = var;
    } else {
      chk_incomplete(var->ty, name);
    }
    *rest = tok;
    return expr;
  }

  chk_local_tls(attr, tok);

  if ((attr->strg & SC_REGISTER) && attr->has_alignas)
    error_tok(name, "_Alignas cannot be applied to a register variable");

  if (ty->kind == TY_VLA) {
    fnctx->use_vla = true;

    Obj *var = alloc_ast_var(ty);
    var->vptr = new_lvar2(ty_ptrdiff_t, decl_scope());
    var->kind = OBJ_INDIR;
    var->is_register = !!(attr->strg & SC_REGISTER);
    push_var_name(name, var);

    Node *node = vla_size(ty, name);
    node = new_unary(ND_ALLOCA, node, name);
    node->m.var = var;

    new_defr(DF_VLA_DEALLOC)->vla = var->vptr;

    chk_alignas_natural(attr, ty, name);
    aligned_attr(name, tok, attr, &var->alt_align);
    cleanup_attr(name, tok, attr, var);

    if (tok->kind == TK_EQ) {
      if (is_iso_std && opt_std < STD_C23 && !tok->file->is_syshdr)
        error_tok(tok, "variable-sized object may not be initialized");
      tok = skip_tk(tok->next, TK_LCURLY);
      tok = skip_tk(tok, TK_RCURLY);
      node->kind = ND_ALLOCA_ZINIT;
      *cond_var = var;
    }
    *rest = tok;
    chain_expr(&expr, node);
    return expr;
  }

  Obj *var = new_lvar2(ty, decl_scope());
  if (is_vm_ty(ty))
    new_defr(DF_VM_BARRIER);
  var->is_register = !!(attr->strg & SC_REGISTER);
  push_var_name(name, var);
  assembler_name(&tok, tok, var);
  chk_alignas_natural(attr, ty, name);
  aligned_attr(name, tok, attr, &var->alt_align);
  cleanup_attr(name, tok, attr, var);

  if (attr->strg & SC_CONSTEXPR) {
    Obj *init_var = new_static_lvar(ty);
    constexpr_initializer(&tok, skip_tk(tok, TK_EQ), init_var, var);
    chain_expr(&expr, constexpr_lvar_init(init_var, var, tok));
    *cond_var = var;
  } else if (tok->kind == TK_EQ) {
    chain_expr(&expr, lvar_initializer(&tok, tok->next, var, true));
    *cond_var = var;
  } else {
    chk_incomplete(var->ty, name);
  }

  *rest = tok;
  return expr;
}

static Node *cond_declaration(Token **rest, Token *tok, TokenKind stopper, int clause) {
  Node *n = NULL;

  for (;; clause++) {
    if (!is_typename(tok)) {
      chain_expr(&n, expression(&tok, tok));
      *rest = skip_tk(tok, stopper);
      return n;
    }
    if (is_iso_std && opt_std <= STD_C23 && !tok->file->is_syshdr)
      error_tok(tok, "declaration in condition is a C2y extension");

    VarAttr attr = {0};
    StorageClass msk = (clause == 0) ? SC_ALL : (SC_AUTO | SC_CONSTEXPR | SC_REGISTER);
    Type *basety = declspec(&tok, tok, &attr, msk);

    if (attr.strg & SC_EXTERN) {
      global_declaration(&tok, tok, basety, &attr);
      continue;
    }

    chain_expr(&n, calc_vla2(basety, tok, &attr));

    if (attr.strg & SC_TYPEDEF) {
      chain_expr(&n, parse_typedef(&tok, tok, basety, &attr));
      continue;
    }

    Obj *var = NULL;
    chain_expr(&n, declaration2(&tok, tok, basety, &attr, &var, NULL));

    while (consume_tk(&tok, tok, TK_COMMA)) {
      chain_expr(&n, declaration2(&tok, tok, basety, &attr, &var, NULL));
      var = NULL;
    }

    if (clause == 0 && consume_tk(&tok, tok, TK_SEMI))
      continue;

    if (!var)
      error_tok(tok, "invalid condition");

    chain_expr(&n, new_var_node(var, tok));
    *rest = skip_tk(tok, stopper);
    return n;
  }
}

static Node *declaration_sc(Token **rest, Token *tok, StorageClass allow_sc, bool for_init) {
  Token *start = tok;
  VarAttr attr = {0};
  Type *basety = declspec(&tok, tok, &attr, allow_sc);

  if (attr.strg & SC_EXTERN) {
    if (for_init && is_iso_std)
      error_tok(start, "declaration in 'for' loop initial declaration cannot be 'extern'");
    if (tok->kind == TK_SEMI && is_iso_std && !tok->file->is_syshdr &&
        basety->is_enum && !has_lcurly_before(start, tok))
      error_tok(start, "declaration does not declare anything");
    global_declaration(rest, tok, basety, &attr);
    return NULL;
  }

  Node *expr = calc_vla2(basety, tok, &attr);

  if (tok->kind == TK_SEMI) {
    chk_inline(&attr, tok);
    if (for_init && is_iso_user_tok(tok))
      error_tok(start, "'for' loop initial declaration must declare an object");
    if ((attr.strg & SC_TYPEDEF) && is_iso_user_tok(tok))
      error_tok(start, "typedef requires a name");
    if (is_iso_user_tok(tok) &&
        basety->kind != TY_STRUCT && basety->kind != TY_UNION &&
        (!basety->is_enum || !has_lcurly_before(start, tok)))
      error_tok(start, "declaration does not declare anything");
    *rest = tok->next;
    return expr;
  }

  if (attr.strg & SC_TYPEDEF) {
    if (for_init && is_iso_std && !tok->file->is_syshdr)
      error_tok(start, "'for' loop initial declaration cannot be a typedef");
    chk_inline(&attr, tok);
    chain_expr(&expr, parse_typedef(rest, tok, basety, &attr));
    return expr;
  }

  do {
    Obj *decl_var = NULL;
    bool is_obj = false;
    chain_expr(&expr, declaration2(&tok, tok, basety, &attr, &decl_var, &is_obj));
    if (for_init && is_iso_std && !tok->file->is_syshdr && !is_obj)
      error_tok(start, "'for' loop initial declaration must declare an object");
  } while (comma_list(rest, &tok, TK_SEMI, true));

  return expr;
}

static Node *declaration(Token **rest, Token *tok) {
  return declaration_sc(rest, tok, SC_ALL | SC_INLINE, false);
}

static Token *skip_excess_element(Token *tok) {
  if (is_iso_std && !tok->file->is_syshdr)
    error_tok(tok, "excess elements in initializer");
  if (tok->kind == TK_LCURLY) {
    tok = tok->next;

    bool first = true;
    for (; braced_list(&tok, &tok, !first); first = false)
      tok = skip_excess_element(tok);

    return tok;
  }

  assign(&tok, tok);
  return tok;
}

static Token *skip_excess_scalar(Token *tok) {
  while (consume_tk(&tok, tok, TK_COMMA)) {
    if (tok->kind != TK_RCURLY)
      tok = skip_excess_element(tok);
  }
  return skip_tk(tok, TK_RCURLY);
}

static void string_initializer(Token *tok, Initializer *init) {
  if (tok->ty->base->size != init->ty->base->size)
    error_tok(tok, "array initialization with string of incompatible size");
  if (is_iso_std && tok->ty->base->size > 1 &&
      tok->ty->base->is_unsigned != init->ty->base->is_unsigned && !tok->file->is_syshdr)
    error_tok(tok, "array initialization with string of incompatible type");

  if (init->kind == INIT_FLEX)
    init->ty = array_of(init->ty->base, tok->ty->array_len);
  else if (is_iso_std && tok->ty->array_len - 1 > init->ty->array_len && !tok->file->is_syshdr)
    error_tok(tok, "initializer-string for array is too long");

  set_init(init, INIT_STR_ARRAY);
  init->tok = tok;
}

static void array_designator(Token **rest, Token *tok, Type *ty, int *begin, int *end) {
  int64_t b = const_expr(&tok, tok->next);

  if (b < 0 || b >= ty->array_len || b > INT32_MAX)
    error_tok(tok, "array designator index out of bounds");
  *begin = (int)b;

  if (tok->kind == TK_DOT3) {
    if (is_iso_std && !tok->file->is_syshdr)
      error_tok(tok, "GNU array designator range is not allowed in ISO C");
    int64_t e = const_expr(&tok, tok->next);
    if (e < 0 || e >= ty->array_len || e > INT32_MAX)
      error_tok(tok, "array designator index out of bounds");
    if (e < b)
      error_tok(tok, "array designator range [%" PRIi64 ", %" PRIi64 "] is empty", b, e);
    *end = (int)e;
  } else {
    *end = *begin;
  }

  *rest = skip_tk(tok, TK_RBRACK);
}

static Member *struct_designator(Token **rest, Token *tok, Type *ty) {
  if (tok->kind != TK_IDENT)
    error_tok(tok, "expected a field designator");

  Member *mem = get_struct_member(ty, tok);
  if (!mem)
    error_tok(tok, "struct has no such member");
  if (mem->name)
    *rest = tok->next;
  return mem;
}

static void designation(Token **rest, Token *tok, Initializer *init, bool post_bracket,
                        DesgContext *ctx) {
  if (tok->kind == TK_LBRACK) {
    if (init->ty->kind != TY_ARRAY)
      error_tok(tok, "array index not in array initializer");

    int begin, end;
    array_designator(&tok, tok, init->ty, &begin, &end);
    prepare_array_init(init, init->ty);

    Token *start = tok;
    if (begin == end || is_const_context()) {
      for (int i = begin; i <= end; i++)
        designation(&tok, start, &init->list.data[i], true, ctx);
    } else {
      ctx = ctx ? ctx : &(DesgContext){0};
      ctx->lvl++;

      for (int i = begin; i <= end; i++) {
        if (ctx->init_lvl != ctx->lvl) {
          designation(&tok, start, &init->list.data[i], true, ctx);

          if (ctx->init)
            continue;
          ctx->init = arena_malloc(&ast_arena, sizeof(Initializer));
          *ctx->init = init->list.data[i];

          ctx->var = new_lvar(init->list.data[i].ty);
          ctx->end = tok;
          ctx->init_lvl = ctx->lvl;
        }
        init->list.data[i].kind = INIT_COPY;
        init->list.data[i].cpy.init = ctx->init;
        init->list.data[i].cpy.var = ctx->var;
      }
      ctx->lvl--;
      tok = ctx->end;
    }

    list_initializer(rest, tok, init, end + 1);
    return;
  }

  if (tok->kind == TK_DOT) {
    if (!(init->ty->kind == TY_STRUCT || init->ty->kind == TY_UNION))
      error_tok(tok, "field designator not in struct or union initializer");

    Member *mem = struct_designator(&tok, tok->next, init->ty);
    prepare_struct_init(init, init->ty);

    if (init->ty->kind == TY_UNION) {
      if (init->list.union_idx != mem->idx) {
        free_initializers(&init->list.data[mem->idx]);
        init->list.data[mem->idx] = (Initializer){.ty = mem->ty};
        init->list.union_idx = mem->idx;
      }
      if (tok->kind == TK_EQ)
        init->list.base_expr = NULL;
      designation(rest, tok, &init->list.data[mem->idx], false, ctx);
    } else {
      designation(&tok, tok, &init->list.data[mem->idx], false, ctx);
      list_initializer(rest, tok, init, mem->idx + 1);
    }
    return;
  }

  if (post_bracket) {
    if (!consume_tk(&tok, tok, TK_EQ) && is_iso_std && !tok->file->is_syshdr)
      error_tok(tok, "GNU array designator without '=' is not allowed in ISO C");
  } else {
    tok = skip_tk(tok, TK_EQ);
  }

  initializer2(rest, tok, init);
}

static int count_array_init_elements(Token *tok, Type *ty, int i) {
  enter_isolated_scope();

  Initializer dummy = {.ty = ty->base};
  int64_t idx = i;
  int64_t max = i;

  while (braced_list(&tok, &tok, (int)idx)) {
    if (tok->kind == TK_LBRACK) {
      idx = const_expr(&tok, tok->next);
      if (idx < 0 || idx >= INT32_MAX)
        error_tok(tok, "array designator index out of bounds");
      if (tok->kind == TK_DOT3) {
        idx = const_expr(&tok, tok->next);
        if (idx < 0 || idx >= INT32_MAX)
          error_tok(tok, "array designator index out of bounds");
      }
      tok = skip_tk(tok, TK_RBRACK);
      designation(&tok, tok, &dummy, true, NULL);
    } else {
      initializer2(&tok, tok, &dummy);
    }

    if (idx >= INT32_MAX)
      error_tok(tok, "array initializer is too large");
    idx++;
    if (idx > max)
      max = idx;
  }
  free_initializers(&dummy);
  leave_scope();
  return (int)max;
}

static bool has_init_subobject(Type *ty) {
  if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
    return true;
  for (Member *m = ty->members; m; m = m->next) {
    if (m->is_bitfield && !m->name)
      continue;
    if (has_init_subobject(m->ty))
      return true;
  }
  return false;
}

static void aggregate_initializer(Token **rest, Token *tok, Initializer *init, Node *expr,
                                  bool has_brace) {
  if (!has_brace && init->is_root)
    error_tok(tok, "expected initializer list");

  Token *start;
  if (!expr) {
    start = tok;
    list_initializer(&tok, tok, init, 0);
  } else {
    start = NULL;
    int start_idx = 0;
    if (init->ty->kind == TY_STRUCT) {
      add_type(expr);
      while (start_idx < init->list.cnt &&
             !has_init_subobject(init->list.data[start_idx].ty) &&
             !is_compatible(expr->ty, init->list.data[start_idx].ty))
        start_idx++;
    }
    if (start_idx < init->list.cnt) {
      initializer3(&tok, tok, &init->list.data[start_idx], expr);

      if (init->ty->kind == TY_UNION)
        init->list.union_idx = start_idx;
      else
        list_initializer(&tok, tok, init, start_idx + 1);
    }
  }

  if (has_brace) {
    while (braced_list(&tok, &tok, start != tok)) {
      if (tok->kind == TK_DOT || tok->kind == TK_LBRACK) {
        designation(&tok, tok, init, false, NULL);
        continue;
      }
      tok = skip_excess_element(tok);
    }
  }
  *rest = tok;
}

static void list_initializer(Token **rest, Token *tok, Initializer *init, int idx) {
  bool need_comma = (idx > 0);
  for (; idx < init->list.cnt && tok->kind != TK_RCURLY; idx++) {
    Token *tok2 = need_comma ? skip_tk(tok, TK_COMMA) : tok;

    if (tok2->kind == TK_RCURLY || tok2->kind == TK_LBRACK || tok2->kind == TK_DOT)
      break;

    if (tok2->kind != TK_LCURLY && !has_init_subobject(init->list.data[idx].ty)) {
      Token *tmp = tok2;
      Node *probe = assign(&tmp, tok2);
      add_type(probe);
      if (!is_compatible(probe->ty, init->list.data[idx].ty)) {
        tok = tok2;
        need_comma = false;
        continue;
      }
    }

    initializer2(&tok, tok2, &init->list.data[idx]);
    need_comma = true;

    if (init->ty->kind == TY_UNION) {
      init->list.union_idx = idx;
      break;
    }
  }
  *rest = tok;
}

static bool is_num_seq(Token **rest, Token *tok, int32_t *val) {
  int32_t cnt = 0;
  for (;;) {
    if (tok->kind == TK_PP_NUM || tok->kind == TK_CHAR_LIT) {
      cnt++;
      tok = tok->next;

      if (consume_tk(rest, tok, TK_RCURLY) ||
          (tok->kind == TK_COMMA && consume_tk(rest, tok->next, TK_RCURLY))) {
        *val = cnt;
        return true;
      }

      if (consume_tk(&tok, tok, TK_COMMA))
        continue;
    }
    return false;
  }
}

static void initializer3(Token **rest, Token *tok, Initializer *init, Node *expr) {
  bool has_brace = !expr && consume_tk(&tok, tok, TK_LCURLY);
  if (has_brace && consume_tk(rest, tok, TK_RCURLY)) {
    if (is_iso_std && opt_std < STD_C23 && !tok->file->is_syshdr)
      error_tok(tok, "empty initializer braces are a C23 extension");
    if (init->kind == INIT_FLEX)
      init->ty = array_of(init->ty->base, 0);
    set_init(init, INIT_NONE);
    init->is_zero = true;
    return;
  }

  if (init->ty->kind == TY_ARRAY) {
    if (init->kind == INIT_FLEX_NESTED)
      error_tok(tok, "nested initialization of flexible array member");

    if (has_brace && init->kind != INIT_FLEX) {
      set_init(init, INIT_NONE);
      init->is_zero = true;
    }

    if (has_brace && is_numeric(init->ty->base)) {
      int32_t len;
      if (is_num_seq(rest, tok, &len)) {
        if (init->kind == INIT_FLEX)
          init->ty = array_of(init->ty->base, len);
        else if (is_iso_std && len > init->ty->array_len && !tok->file->is_syshdr)
          error_tok(tok, "excess elements in array initializer");

        init->kind = INIT_NUM_SEQ;
        init->numseq.start = tok;
        init->numseq.len = len;
        return;
      }
    }

    if (is_integer(init->ty->base) && init->ty->base->kind != TY_BOOL &&
        !(init->ty->base->qual & Q_ATOMIC)) {
      if (!expr &&
          !(tok->kind == TK_LBRACK || tok->kind == TK_DOT || tok->kind == TK_LCURLY))
        expr = assign(&tok, tok);

      if (expr && expr->kind == ND_VAR && expr->m.var->is_string_lit) {
        string_initializer(expr->tok, init);
        if (has_brace) {
          consume_tk(&tok, tok, TK_COMMA);
          tok = skip_tk(tok, TK_RCURLY);
        }
        *rest = tok;
        return;
      }
    }
    if (init->kind == INIT_FLEX) {
      int len = count_array_init_elements(tok, init->ty, !!expr);
      init->ty = array_of(init->ty->base, len);
    }
    prepare_array_init(init, init->ty);
    aggregate_initializer(rest, tok, init, expr, has_brace);
    return;
  }

  if (init->ty->kind == TY_STRUCT || init->ty->kind == TY_UNION) {
    if (has_brace) {
      if ((init->ty->qual & Q_ATOMIC) && is_iso_std && !tok->file->is_syshdr)
        error_tok(tok, "cannot initialize '_Atomic' struct or union with brace-enclosed member list");
      set_init(init, INIT_NONE);
      init->is_zero = true;
    } else {
      if (!expr)
        expr = assign(&tok, tok);

      add_type(expr);
      if (is_compatible(expr->ty, init->ty)) {
        set_init(init, INIT_EXPR);
        init->expr = expr;
        *rest = tok;
        return;
      } else if (init->is_root) {
        error_tok(tok, "incompatible types");
      }
    }
    prepare_struct_init(init, init->ty);
    aggregate_initializer(rest, tok, init, expr, has_brace);
    return;
  }

  if (!expr &&
      (tok->kind == TK_PP_NUM || tok->kind == TK_CHAR_LIT) &&
      (tok->next->kind == TK_COMMA ||
       tok->next->kind == TK_RCURLY ||
       tok->next->kind == TK_SEMI)) {
    init->kind = INIT_TOK;
    init->tok = tok;
    tok = tok->next;
  } else {
    int64_t cnt = 0;
    while (consume_tk(&tok, tok, TK_LCURLY))
      cnt++;
    if (is_iso_std && !tok->file->is_syshdr && (has_brace ? cnt > 0 : cnt > 1))
      error_tok(tok, "excess braces around scalar initializer");

    init->kind = INIT_EXPR;
    init->expr = expr ? expr : assign(&tok, tok);

    while (cnt--)
      tok = skip_excess_scalar(tok);
  }
  if (has_brace)
    tok = skip_excess_scalar(tok);

  *rest = tok;
}

static void initializer2(Token **rest, Token *tok, Initializer *init) {
  initializer3(rest, tok, init, NULL);
}

static void initializer(Token **rest, Token *tok, Initializer *init, Obj *var) {
  Token *start = tok;
  Type *ty = var->ty;

  if (ty->kind == TY_AUTO) {
    init->kind = INIT_EXPR;
    init->expr = assign(&tok, tok);
    if (tok->kind == TK_COMMA)
      error_tok(tok, "multiple declarators unsupported for type inference");
    *rest = tok;
    ptr_convert(&init->expr);

    if (chk_qual_type(ty->qual, init->expr->ty))
      error_tok(tok, "invalid qualifier for type");
    new_derived_type(ty, ty->qual, init->expr->ty);
    init->ty = ty;
    return;
  }

  init->ty = ty;
  init->is_root = true;
  if (ty->kind == TY_ARRAY && ty->size < 0)
    init->kind = INIT_FLEX;

  initializer2(rest, tok, init);

  chk_incomplete(init->ty, start);
  var->ty = init->ty;

  if (ty->is_flexible && ty->kind == TY_STRUCT && init->kind == INIT_LIST) {
    ty = new_derived_type(NULL, ty->qual, ty);

    Initializer *i = &init->list.data[init->list.cnt - 1];
    if (i->kind != INIT_FLEX && i->kind != INIT_FLEX_NESTED &&
        is_iso_std && !start->file->is_syshdr)
      error_tok(start, "initialization of flexible array member is not allowed in ISO C");
    if (i->ty->size > 0)
      ty->size += i->ty->size;
    var->ty = ty;
  }
}

static Node *init_desg_expr(InitDesg *desg, Token *tok) {
  if (desg->var)
    return new_var_node(desg->var, tok);

  if (desg->member) {
    Node *node = new_unary(ND_MEMBER, init_desg_expr(desg->parent, tok), tok);
    node->m.member = desg->member;
    return node;
  }

  Node *lhs = init_desg_expr(desg->parent, tok);
  Node *rhs = new_num(desg->idx, tok);
  return new_unary(ND_DEREF, new_add(lhs, rhs, tok), tok);
}

static Node *init_num_tok(Initializer *init, Node *node) {
  if (init->kind == INIT_TOK) {
    convert_pp_number(init->tok, node);
    return node;
  }
  add_type(init->expr);
  return init->expr;
}

static char *checked_gvar_alloc(int64_t sz, bool zero, Token *tok) {
  if (sz < 0 || sz > (1LL << 30))
    error_tok(tok, "initializer size too large");
  size_t n = sz > 0 ? (size_t)sz : 1;
  char *buf = zero ? calloc(1, n) : malloc(n);
  if (!buf)
    error_tok(tok, "initializer size too large");
  return buf;
}

static void create_lvar_init_ex(Node **cur, Initializer *init, InitDesg *desg, Token *tok,
                                bool has_base, bool clear_zero) {
  switch (init->kind) {
  case INIT_NONE: {
    if (clear_zero || (has_base && init->is_zero)) {
      if ((desg->member && desg->member->is_bitfield) ||
          is_numeric(init->ty) || is_ptr(init->ty) ||
          init->ty->kind == TY_BITINT || init->ty->kind == TY_NULLPTR) {
        Node *n = new_binary(ND_ASSIGN, init_desg_expr(desg, tok), new_num(0, tok), tok);
        add_type(n);
        (*cur) = (*cur)->next = n;
      } else if (init->ty->size > 0) {
        Obj *zvar = new_anon_gvar(init->ty);
        Node *n = new_binary(ND_ASSIGN, init_desg_expr(desg, tok), new_var_node(zvar, tok), tok);
        add_type(n->m.lhs);
        add_type(n->m.rhs);
        n->ty = ty_void;
        (*cur) = (*cur)->next = n;
      }
    }
    return;
  }
  case INIT_FLEX:
  case INIT_FLEX_NESTED: {
    return;
  }
  case INIT_TOK:
  case INIT_EXPR: {
    Node *node = init_num_tok(init, new_node(ND_NUM, init->tok));

    Node *n = new_binary(ND_ASSIGN, init_desg_expr(desg, tok), node, tok);
    add_type(n);
    (*cur) = (*cur)->next = n;
    return;
  }
  case INIT_STR_ARRAY: {
    Token *str = init->tok;
    Obj *var = new_anon_gvar(init->ty);
    var->is_string_lit = true;
    var->init_data = checked_gvar_alloc(init->ty->size, false, tok);

    if (init->ty->size <= str->ty->size) {
      memcpy(var->init_data, str->str, init->ty->size);
    } else {
      memcpy(var->init_data, str->str, str->ty->size);
      memset(var->init_data + str->ty->size, 0, init->ty->size - str->ty->size);
    }

    Node *n = new_binary(ND_ASSIGN, init_desg_expr(desg, tok), new_var_node(var, tok), tok);
    add_type(n->m.lhs);
    add_type(n->m.rhs);
    n->ty = ty_void;

    // non-static struct with flexible array member, supported as extension
    if (n->m.lhs->ty->size < 0)
      n->m.lhs->ty = init->ty;

    (*cur) = (*cur)->next = n;
    return;
  }
  case INIT_NUM_SEQ: {
    Obj *var = new_anon_gvar(init->ty);
    int64_t fill_sz = init->numseq.len * init->ty->base->size;
    var->is_string_lit = true;
    var->init_data = checked_gvar_alloc(init->ty->size, false, tok);

    write_gvar_data(NULL, NULL, init, var->init_data, 0, EV_CONST);

    if (init->ty->size > fill_sz)
      memset(var->init_data + fill_sz, 0, init->ty->size - fill_sz);

    Node *n = new_binary(ND_ASSIGN, init_desg_expr(desg, tok), new_var_node(var, tok), tok);
    add_type(n->m.lhs);
    add_type(n->m.rhs);
    n->ty = ty_void;
    if (n->m.lhs->ty->size < 0)
      n->m.lhs->ty = init->ty;
    (*cur) = (*cur)->next = n;
    return;
  }
  case INIT_LIST: {
    if (init->ty->kind == TY_ARRAY) {
      bool sub_clear = clear_zero || (has_base && init->is_zero);
      for (int i = 0; i < init->list.cnt; i++) {
        InitDesg desg2 = {.parent = desg, .idx = i};
        create_lvar_init_ex(cur, &init->list.data[i], &desg2, tok, has_base, sub_clear);
      }
      return;
    }
    if (init->ty->kind == TY_STRUCT || init->ty->kind == TY_UNION) {
      bool sub_base = has_base;
      bool sub_clear = clear_zero;
      if (init->list.base_expr) {
        Node *n = new_binary(ND_ASSIGN, init_desg_expr(desg, tok), init->list.base_expr, tok);
        add_type(n);
        (*cur) = (*cur)->next = n;
        sub_base = true;
        sub_clear = false;
      } else if (has_base && init->is_zero) {
        sub_clear = true;
      }
      for (Member *mem = init->ty->members; mem_iter(&mem); mem = mem->next) {
        if (init->ty->kind == TY_UNION && init->list.union_idx != mem->idx)
          continue;

        InitDesg desg2 = {.parent = desg, .member = mem};
        create_lvar_init_ex(cur, &init->list.data[mem->idx], &desg2, tok, sub_base, sub_clear);
      }
      return;
    }
    break;
  }
  case INIT_COPY: {
    Node *n;
    if (!init->cpy.init->has_copy) {
      init->cpy.init->has_copy = true;
      create_lvar_init_ex(cur, init->cpy.init, desg, tok, has_base, clear_zero);

      n = new_binary(ND_ASSIGN, new_var_node(init->cpy.var, tok),
                     init_desg_expr(desg, tok), tok);
    } else {
      n = new_binary(ND_ASSIGN, init_desg_expr(desg, tok),
                     new_var_node(init->cpy.var, tok), tok);
    }
    add_type(n);
    (*cur) = (*cur)->next = n;
    return;
  }
  }
  internal_error();
}

static void create_lvar_init(Node **cur, Initializer *init, InitDesg *desg, Token *tok) {
  create_lvar_init_ex(cur, init, desg, tok, false, false);
}

static bool promote_int_constexpr(Initializer *init, Type *ty) {
  if (opt_std <= STD_C23 ||
      !(ty->qual & Q_CONST) ||
      (ty->qual & Q_VOLATILE) ||
      !is_int_class(ty))
    return false;

  switch (init->kind) {
  case INIT_NONE: // C23 "={}"
    return true;
  case INIT_TOK:
    if (init->tok->kind == TK_PP_NUM)
      return is_pp_token_int(init->tok);
    if (init->tok->kind == TK_CHAR_LIT)
      return true;
    break;
  case INIT_EXPR: {
    Node *exp = init->expr;
    add_type(exp);

    if (is_integer(exp->ty))
      return is_const_expr_strict(exp, NULL);
    if (exp->ty->kind == TY_BITINT)
      return is_const_bitint_strict(exp, NULL);
  }
  }
  return false;
}

static Node *lvar_initializer(Token **rest, Token *tok, Obj *var, bool try_promote) {
  Initializer init = {0};
  initializer(rest, tok, &init, var);

  if (try_promote && promote_int_constexpr(&init, var->ty)) {
    Obj *init_var = new_static_lvar(var->ty);

    constexpr_initializer2(&init, init_var, var);

    free_initializers(&init);
    return constexpr_lvar_init(init_var, var, tok);
  }

  Node head = {0};
  InitDesg desg = {.var = var};

  create_lvar_init(&(Node *){&head}, &init, &desg, tok);

  free_initializers(&init);

  if (init.kind == INIT_EXPR || init.kind == INIT_TOK)
    return head.next;

  Node *node = new_node(ND_INIT_SEQ, tok);
  node->m.var = var;
  node->m.lhs = head.next;
  node->ty = ty_void;
  return node;
}

static uint64_t read_buf(char *buf, int sz) {
  if (sz == 1) {
    uint8_t v;
    memcpy(&v, buf, sizeof(v));
    return v;
  }
  if (sz == 2) {
    uint16_t v;
    memcpy(&v, buf, sizeof(v));
    return v;
  }
  if (sz == 4) {
    uint32_t v;
    memcpy(&v, buf, sizeof(v));
    return v;
  }
  if (sz == 8) {
    uint64_t v;
    memcpy(&v, buf, sizeof(v));
    return v;
  }
  internal_error();
}

static long_double_t read_double_buf(char *buf, Type *ty) {
  if (ty->kind == TY_FLOAT) {
    float v;
    memcpy(&v, buf, sizeof(v));
    return v;
  }
  if (ty->kind == TY_DOUBLE) {
    double v;
    memcpy(&v, buf, sizeof(v));
    return v;
  }
  if (ty->kind == TY_LDOUBLE) {
    long_double_t v;
    memcpy(&v, buf, sizeof(v));
    return v;
  }
  internal_error();
}

static void remove_relocs_in_range(Relocation *head, Relocation **cur, int64_t start, int64_t end) {
  if (!head || end <= start)
    return;
  Relocation *prev = head;
  while (prev->next) {
    Relocation *r = prev->next;
    if (r->offset >= start && r->offset < end) {
      prev->next = r->next;
    } else {
      prev = r;
    }
  }
  if (cur)
    *cur = prev;
}

static void sort_relocs(Relocation *head) {
  Relocation *sorted = NULL;
  Relocation *r = head->next;
  while (r) {
    Relocation *next = r->next;
    Relocation **pp = &sorted;
    while (*pp && (*pp)->offset < r->offset)
      pp = &(*pp)->next;
    r->next = *pp;
    *pp = r;
    r = next;
  }
  head->next = sorted;
}

static void write_gvar_data(Relocation *rel_head, Relocation **cur, Initializer *init, char *buf,
                            int64_t offset, EvalKind ev_kind) {
  switch (init->kind) {
  case INIT_NONE:
  case INIT_FLEX:
  case INIT_FLEX_NESTED: {
    return;
  }
  case INIT_TOK:
  case INIT_EXPR: {
    Node *node = init_num_tok(init, &(Node){.kind = ND_NUM, .tok = init->tok});
    node = assign_cast(init->ty, node);

    if (init->ty->kind == TY_ARRAY)
      error_tok(node->tok, "array initializer must be an initializer list");

    // Direct initialization with a const variable
    if (init->kind == INIT_EXPR) {
      int64_t sofs;
      Obj *var = NULL;
      if (is_compatible(init->ty, init->expr->ty) && !is_bitfield(init->expr))
        var = eval_var(init->expr, &sofs, false);

      if (var && (var->is_compound_lit || is_static_const_var(var)) &&
          sofs >= 0 && sofs <= var->ty->size) {
        Relocation *srel = var->rel;
        while (srel && srel->offset < sofs)
          srel = srel->next;

        int64_t end = MIN((int64_t)init->ty->size, (int64_t)var->ty->size - sofs);
        char *data = var->init_data ? var->init_data : var->constexpr_data;
        for (int64_t pos = 0; pos < end;) {
          if (srel && srel->offset == pos + sofs) {
            Relocation *rel = arena_calloc(fnctx ? &ast_arena : &cc1_arena,
                                           sizeof(Relocation));
            rel->offset = pos + offset;
            rel->var = srel->var;
            rel->addend = srel->addend;
            (*cur) = (*cur)->next = rel;
            srel = srel->next;
            pos += 8;
          } else {
            buf[pos + offset] = data ? data[pos + sofs] : 0;
            pos++;
          }
        }
        return;
      }
    }

    if (is_integer(init->ty) || init->ty->kind == TY_PTR || init->ty->kind == TY_NULLPTR) {
      EvalContext ctx = {.kind = ev_kind};
      int64_t val = eval2(node, &ctx);
      if (ctx.var) {
        if (init->ty->size != ty_nullptr->size)
          error_tok(node->tok, "initializer element is not constant");
        Relocation *rel = arena_calloc(fnctx ? &ast_arena : &cc1_arena, sizeof(Relocation));
        rel->offset = offset;
        rel->var = ctx.var;
        rel->addend = val;
        (*cur) = (*cur)->next = rel;
        return;
      }
      memcpy(buf + offset, &val, init->ty->size);
      return;
    }

    if (is_flonum(init->ty)) {
      FPVal fval = {0};
      eval_fp(node, &fval);
      memcpy(buf + offset, &fval, init->ty->size);
      return;
    }

    if (init->ty->kind == TY_BITINT) {
      BitBuf *val = eval_bitint(node);
      clean_bitint(val, node->ty);
      memcpy(buf + offset, val, init->ty->size);
      free(val);
      return;
    }

    error_tok(node->tok, "invalid initializer");
  }
  case INIT_STR_ARRAY: {
    size_t len = MIN(init->ty->array_len, init->tok->ty->array_len);
    memcpy(buf + offset, init->tok->str, init->ty->base->size * len);
    return;
  }
  case INIT_NUM_SEQ: {
    Initializer tmp = {.kind = INIT_TOK, .ty = init->ty->base};
    int64_t sz = init->ty->base->size;
    int64_t cnt = 0;
    for (Token *t = init->numseq.start; braced_list(&t, &t, cnt); t = t->next) {
      if (cnt >= init->ty->array_len)
        break;
      tmp.tok = t;
      write_gvar_data(rel_head, cur, &tmp, buf, offset + sz * cnt, ev_kind);
      cnt++;
    }
    return;
  }
  case INIT_LIST:
    if (init->ty->kind == TY_ARRAY) {
      int64_t sz = init->ty->base->size;
      for (int i = 0; i < init->list.cnt; i++)
        write_gvar_data(rel_head, cur, &init->list.data[i], buf, offset + sz * i, ev_kind);
      return;
    }

    if (init->ty->kind == TY_STRUCT || init->ty->kind == TY_UNION) {
      if (init->list.base_expr) {
        Initializer base_init = {.ty = init->ty, .kind = INIT_EXPR, .expr = init->list.base_expr};
        write_gvar_data(rel_head, cur, &base_init, buf, offset, ev_kind);
      }
      for (Member *mem = init->ty->members; mem_iter(&mem); mem = mem->next) {
        if (init->ty->kind == TY_UNION && init->list.union_idx != mem->idx)
          continue;

        Initializer *init2 = &init->list.data[mem->idx];
        if (init->list.base_expr && !mem->is_bitfield &&
            ((init2->kind != INIT_NONE && init2->kind != INIT_LIST) || init2->is_zero) &&
            mem->ty->size > 0) {
          remove_relocs_in_range(rel_head, cur, offset + mem->offset,
                                 offset + mem->offset + mem->ty->size);
          memset(buf + offset + mem->offset, 0, (size_t)mem->ty->size);
        }
        if (mem->is_bitfield &&
            (init2->kind == INIT_EXPR || init2->kind == INIT_TOK ||
             (init->list.base_expr && init2->kind == INIT_NONE && init2->is_zero))) {
          Node *node = (init2->kind == INIT_NONE)
                       ? new_num(0, mem->name ? mem->name : &(Token){0})
                       : init_num_tok(init2, &(Node){.kind = ND_NUM, .tok = init2->tok});
          node = assign_cast(mem->ty, node);

          char *loc = buf + offset + mem->offset;

          if (mem->ty->kind == TY_BITINT) {
            BitBuf *val = eval_bitint(node);
            eval_bitint_bitfield_save(mem->ty->bit_cnt, val, (uint8_t *)loc, mem->bit_width,
                                      mem->bit_offset);
            free(val);
            continue;
          }
          uint64_t val = eval(node);
          if (mem->is_aligned_bitfield) {
            int sz = next_pow_of_two(mem->bit_offset + mem->bit_width) / 8;
            uint64_t oldval = read_buf(loc, sz);
            uint64_t mask = (mem->bit_width >= 64) ? ~UINT64_C(0) : (((uint64_t)1 << mem->bit_width) - 1);
            uint64_t combined = (oldval & ~(mask << mem->bit_offset)) | ((val & mask) << mem->bit_offset);
            memcpy(loc, &combined, sz);
            continue;
          }
          int rem = mem->bit_offset + mem->bit_width;
          if (mem->bit_offset) {
            int shft = 8 - mem->bit_offset;
            uint8_t cur_b = (uint8_t)*loc;
            *loc = (char)((uint8_t)((uint8_t)(cur_b << shft) >> shft) |
                          (uint8_t)((val & 0xFFu) << mem->bit_offset));
            val >>= shft;
            rem -= 8;
            loc++;
          }
          for (int start = rem; rem > 0; rem -= 8, loc++) {
            if (rem != start)
              val >>= 8;
            if (rem >= 8) {
              *loc = (char)val;
              continue;
            }
            val &= (1u << rem) - 1u;
            uint8_t cur_b = (uint8_t)*loc;
            *loc = (char)((uint8_t)((cur_b >> rem) << rem) | (uint8_t)val);
            break;
          }
          continue;
        }
        write_gvar_data(rel_head, cur, init2, buf, offset + mem->offset, ev_kind);
      }
      return;
    }
  }
  internal_error();
}

static void gvar_initializer(Token **rest, Token *tok, Obj *var) {
  bool ctx = false;
  if (fnctx) {
    ctx = fnctx->is_static_init_context;
    fnctx->is_static_init_context = true;
  }
  Initializer init = {0};
  initializer(rest, tok, &init, var);

  Relocation head = {0};
  char *buf = checked_gvar_alloc(var->ty->size, true, tok);

  write_gvar_data(&head, &(Relocation *){&head}, &init, buf, 0, EV_LABEL);
  sort_relocs(&head);

  var->rel = head.next;
  var->init_data = buf;

  free_initializers(&init);

  if (fnctx)
    fnctx->is_static_init_context = ctx;
}

static void constexpr_initializer(Token **rest, Token *tok, Obj *init_var, Obj *var) {
  bool ctx = false;
  if (fnctx) {
    ctx = fnctx->is_static_init_context;
    fnctx->is_static_init_context = true;
  }
  Initializer init = {0};
  initializer(rest, tok, &init, init_var);

  constexpr_initializer2(&init, init_var, var);

  free_initializers(&init);

  if (fnctx)
    fnctx->is_static_init_context = ctx;
}

static void constexpr_initializer2(Initializer *init, Obj *init_var, Obj *var) {
  Relocation head = {0};
  char *buf = checked_gvar_alloc(init_var->ty->size, true, init->tok ? init->tok : &(Token){0});

  write_gvar_data(&head, &(Relocation *){&head}, init, buf, 0, EV_CONST);
  sort_relocs(&head);

  init_var->init_data = var->constexpr_data = buf;
  init_var->rel = head.next;
  var->ty = init_var->ty;
}

static Node *constexpr_lvar_init(Obj *init_var, Obj *var, Token *tok) {
  return new_binary(ND_ASSIGN, new_var_node(var, tok), new_var_node(init_var, tok), tok);
}

static bool is_type_kw(TokenKind kind) {
  return kind >= TK_TYPEKW && kind < TK_TYPEKW_END;
}

static bool is_typename(Token *tok) {
  return is_type_kw(tok->kind) || find_typedef(tok);
}

static bool is_typename_paren2(Token **rest, Token *tok, Type **ty, VarAttr *attr) {
  if (skip_paren(tok->next)->kind != TK_LCURLY) {
    *ty = typename2(&tok, tok, attr);
    *rest = skip_tk(tok, TK_RPAREN);
    return true;
  }
  return false;
}

static bool is_typename_paren(Token **rest, Token *tok, Type **ty, VarAttr *attr) {
  if (tok->kind == TK_LPAREN && is_typename(tok->next))
    return is_typename_paren2(rest, tok->next, ty, attr);
  return false;
}

static void eval_static_assert(Token **rest, Token *tok) {
  Node *node = conditional(&tok, skip_tk(tok, TK_LPAREN));
  add_type(node);

  if (!eval_ice_node(node, ICE_BOOL, tok, "static_assert argument not integer"))
    error_tok(tok, "static assertion failed");

  if (tok->kind == TK_COMMA) {
    if (tok->next->kind != TK_STR)
      error_tok(tok, "expected string literal");
    tok = tok->next->next;
  } else if (is_iso_std && opt_std < STD_C23 && !tok->file->is_syshdr) {
    error_tok(tok, "_Static_assert with no message is a C23 extension");
  }
  *rest = skip_tk(tok, TK_RPAREN);
}

static LocalLabel *find_local_label(Scope *sc, Token *tok) {
  for (LocalLabel *ll = sc->labels; ll; ll = ll->next)
    if (equal_tok(ll->name, tok))
      return ll;
  return NULL;
}

static void push_goto(Node *node) {
  for (Scope *sc = scope; sc; sc = sc->parent) {
    if (find_local_label(sc, node->tok)) {
      node->lbl.next = sc->gotos;
      sc->gotos = node;
      node->dfr_from = fnctx->defr;
      return;
    }
  }
  node->lbl.next = fnctx->gotos;
  fnctx->gotos = node;

  node->dfr_from = fnctx->defr;
}

static int64_t eval_case_const_expr(Token **rest, Token *tok) {
  Node *node = conditional(rest, tok);
  add_type(node);
  return eval_ice_node(node, ICE_I64_CASE, node->tok, "expression not integer");
}

static void case_range(Token **rest, Token *tok, Node *sw, Node *case_node) {
  int64_t lo = eval_case_const_expr(&tok, tok);
  int64_t hi;
  if (consume_tk(&tok, tok, TK_DOT3)) {
    if (is_iso_std && !tok->file->is_syshdr)
      error_tok(tok, "GNU case range is not allowed in ISO C");
    hi = eval_case_const_expr(&tok, tok);
  } else {
    hi = lo;
  }

  Type *ty = sw->ctrl.cond->ty;
  Type *orig_ty = (sw->ctrl.sw_orig_ty && sw->ctrl.sw_orig_ty->kind == TY_BITINT &&
                   sw->ctrl.sw_orig_ty->bit_cnt <= 64)
                  ? sw->ctrl.sw_orig_ty
                  : ((sw->ctrl.cond->kind == ND_CAST &&
                      sw->ctrl.cond->m.lhs->ty->kind == TY_BITINT &&
                      sw->ctrl.cond->m.lhs->ty->bit_cnt <= 64)
                     ? sw->ctrl.cond->m.lhs->ty : ty);
  if (orig_ty->kind == TY_BITINT) {
    int sh = 64 - (int)orig_ty->bit_cnt;
    if (orig_ty->is_unsigned) {
      lo = (int64_t)(((uint64_t)lo << sh) >> sh);
      hi = (int64_t)(((uint64_t)hi << sh) >> sh);
    } else {
      lo = (int64_t)((uint64_t)lo << sh) >> sh;
      hi = (int64_t)((uint64_t)hi << sh) >> sh;
    }
  } else if (ty->size <= 4) {
    if (!ty->is_unsigned)
      lo = (int32_t)lo, hi = (int32_t)hi;
    else
      lo = (uint32_t)lo, hi = (uint32_t)hi;
  }
  if (hi != lo && less_eq(ty, hi, lo))
    error_tok(tok, "empty case range specified");

  for (CaseRange *cr = sw->ctrl.sw_cases; cr; cr = cr->next)
    if ((less_eq(ty, cr->lo, lo) && less_eq(ty, lo, cr->hi)) ||
        (less_eq(ty, lo, cr->lo) && less_eq(ty, cr->lo, hi)))
      error_tok(tok, "duplicated case");

  *rest = skip_tk(tok, TK_COLON);

  // Merge adjacent ranges
  CaseRange *lo_adj = NULL;
  CaseRange *hi_adj = NULL;
  for (CaseRange *cr = sw->ctrl.sw_cases; cr; cr = cr->next) {
    if (cr->label != case_node)
      break;
    if (less_eq(ty, cr->hi, lo) && (uint64_t)cr->hi + 1 == lo)
      lo_adj = cr;
    else if (less_eq(ty, hi, cr->lo) && (uint64_t)hi + 1 == cr->lo)
      hi_adj = cr;
  }

  if (lo_adj && hi_adj) {
    if (lo_adj->next)
      hi_adj->lo = lo_adj->lo, *lo_adj = *lo_adj->next;
    else
      lo_adj->hi = hi_adj->hi, *hi_adj = *hi_adj->next;
    return;
  }
  if (lo_adj) {
    lo_adj->hi = hi;
    return;
  }
  if (hi_adj) {
    hi_adj->lo = lo;
    return;
  }

  CaseRange *cr = arena_malloc(&ast_arena, sizeof(CaseRange));
  cr->label = case_node;
  cr->lo = lo;
  cr->hi = hi;
  cr->next = sw->ctrl.sw_cases;
  sw->ctrl.sw_cases = cr;
}

static Token *label_stmt(Token **rest, Token *tok, Node **stmt) {
  Node *case_node = NULL;

  Token head = {0};
  Token *cur = &head;

  for (;;) {
    if (tok->kind == TK_IDENT && tok->next->kind == TK_COLON) {
      Token *start = tok;
      cur = cur->label_next = tok;
      tok = tok->next->next;

      LocalLabel *ll = NULL;
      for (Scope *sc = scope; sc; sc = sc->parent)
        if ((ll = find_local_label(sc, start)))
          break;

      if (ll) {
        if (ll->label)
          error_tok(start, "duplicated label");
        (*stmt) = (*stmt)->next = ll->label = new_label(ND_LABEL, start);
        continue;
      }

      if (!fnctx->defr_ctx) {
        for (Node *lbl = fnctx->labels; lbl; lbl = lbl->lbl.next)
          if (equal_tok(lbl->tok, start))
            error_tok(start, "duplicated label");
        Node *node = new_label(ND_LABEL, start);
        node->lbl.next = fnctx->labels;
        (*stmt) = (*stmt)->next = fnctx->labels = node;
      }
      continue;
    }

    if (tok->kind == TK_case || tok->kind == TK_default) {
      if (!case_node) {
        (*stmt) = (*stmt)->next = case_node = new_label(ND_CASE, tok);

        JumpContext *ctx = jump_ctx;
        for (; ctx; ctx = ctx->next)
          if (ctx->node->kind == ND_SWITCH)
            break;
        if (!ctx)
          error_tok(tok, "stray case");
        if (ctx->dfr_lvl != fnctx->defr || ctx->dfr_ctx != fnctx->defr_ctx)
          error_tok(tok, "illegal jump");
        case_node->cases.parent_sw = ctx->node;
      }

      if (tok->kind == TK_default) {
        if (case_node->cases.parent_sw->ctrl.sw_default)
          error_tok(tok, "duplicated default");
        case_node->cases.parent_sw->ctrl.sw_default = case_node;

        case_node->kind = ND_DEFAULT;
        tok = skip_tk(tok->next, TK_COLON);
        continue;
      }

      case_range(&tok, tok->next, case_node->cases.parent_sw, case_node);
      continue;
    }
    break;
  }

  *rest = tok;

  cur->label_next = NULL;
  return head.label_next;
}

static Node *secondary_block(Token **rest, Token *tok) {
  if (tok->kind == TK_LCURLY)
    return compound_stmt(rest, tok->next, ND_BLOCK);

  DeferStmt *dfr = enter_stmt_scope();

  Node head = {0};
  Node *cur = &head;

  Token *label_list = label_stmt(&tok, tok, &cur);
  cur->next = stmt(rest, tok, label_list);

  return leave_stmt_scope(dfr, head.next);
}

static void loop_body(Token **rest, Token *tok, Node *node, Token *label_list) {
  JumpContext ctx = {.next = jump_ctx};
  jump_ctx = &ctx;

  ctx.labels = label_list;
  ctx.node = node;
  ctx.dfr_lvl = fnctx->defr;
  ctx.dfr_ctx = fnctx->defr_ctx;

  node->ctrl.then = secondary_block(rest, tok);

  jump_ctx = ctx.next;
}

static JumpContext *resolve_labeled_jump(Token **rest, Token *tok) {
  bool is_cont = tok->kind == TK_continue;

  Token *name = NULL;
  if (tok->next->kind == TK_IDENT) {
    if (is_iso_std && !tok->file->is_syshdr)
      error_tok(tok->next, "labeled break/continue is not allowed in ISO C");
    name = tok = tok->next;
  }
  *rest = skip_tk(tok->next, TK_SEMI);

  for (JumpContext *ctx = jump_ctx; ctx; ctx = ctx->next) {
    if (is_cont && ctx->node->kind == ND_SWITCH)
      continue;
    if (!name)
      return ctx;
    for (Token *t = ctx->labels; t; t = t->label_next)
      if (equal_tok(t, name))
        return ctx;
  }
  error_tok(tok, "cannot resolve jump");
}

static Node *stmt_inner(Token **rest, Token *tok, Token *label_list);

static Node *stmt(Token **rest, Token *tok, Token *label_list) {
  if (++parse_depth > 256)
    error_tok(tok, "statement nesting depth exceeded");
  Node *r = stmt_inner(rest, tok, label_list);
  parse_depth--;
  return r;
}

static Node *stmt_inner(Token **rest, Token *tok, Token *label_list) {
  while (tok->kind == TK_PRAGMA)
    pragma_pack(&tok, tok);
  if (tok->kind == TK_return) {
    if (fnctx->defr_ctx)
      error_tok(tok, "return in defer block");

    Node *node = new_node(ND_RETURN, tok);
    node->dfr_from = fnctx->defr;
    if (consume_tk(rest, tok->next, TK_SEMI)) {
      if (is_iso_std && fnctx->fn->ty->return_ty->kind != TY_VOID)
        error_tok(tok, "non-void function should return a value");
      return node;
    }

    Node *n = expression(&tok, tok->next);
    add_type(n);

    if (fnctx->fn->ty->return_ty->kind == TY_VOID) {
      if (is_iso_std || n->ty->kind != TY_VOID)
        error_tok(tok, "void function should not return a value");
    } else {
      n = assign_cast(fnctx->fn->ty->return_ty, n);
    }

    node->m.lhs = n;
    *rest = skip_tk(tok, TK_SEMI);
    return node;
  }

  if (tok->kind == TK_if) {
    DeferStmt *dfr = enter_stmt_scope();

    Node *node = new_node(ND_IF, tok);
    tok = skip_tk(tok->next, TK_LPAREN);
    node->ctrl.cond = cond_cast(cond_declaration(&tok, tok, TK_RPAREN, 0));
    node->ctrl.then = secondary_block(&tok, tok);
    if (tok->kind == TK_else)
      node->ctrl.els = secondary_block(&tok, tok->next);

    *rest = tok;
    return leave_stmt_scope(dfr, node);
  }

  if (tok->kind == TK_switch) {
    DeferStmt *dfr = enter_stmt_scope();

    Node *node = new_node(ND_SWITCH, tok);
    tok = skip_tk(tok->next, TK_LPAREN);
    node->ctrl.cond = cond_declaration(&tok, tok, TK_RPAREN, 0);
    add_type(node->ctrl.cond);
    node->ctrl.sw_orig_ty = node->ctrl.cond->ty;

    if (!int_or_trunc_bitint(&node->ctrl.cond, true))
      error_tok(tok, "controlling expression not integer");
    if (node->ctrl.cond->ty->kind != TY_BITINT)
      int_promotion(&node->ctrl.cond);

    JumpContext ctx = {.next = jump_ctx, .node = node};
    jump_ctx = &ctx;

    ctx.labels = label_list;
    ctx.dfr_lvl = fnctx->defr;
    ctx.dfr_ctx = fnctx->defr_ctx;

    node->ctrl.then = secondary_block(rest, tok);

    jump_ctx = ctx.next;
    return leave_stmt_scope(dfr, node);
  }

  if (tok->kind == TK_for) {
    DeferStmt *dfr = enter_stmt_scope();

    Node *node = new_node(ND_FOR, tok);
    tok = skip_tk(tok->next, TK_LPAREN);

    if (tok->kind == TK_static_assert) {
      eval_static_assert(&tok, tok->next);
      tok = skip_tk(tok, TK_SEMI);
    } else if (is_typename(tok)) {
      StorageClass for_sc = is_iso_std
          ? (SC_AUTO | SC_REGISTER | (opt_std >= STD_C23 ? SC_CONSTEXPR : SC_NONE))
          : (SC_ALL | SC_INLINE);
      Node *expr = declaration_sc(&tok, tok, for_sc, true);
      if (expr)
        node->ctrl.for_init = new_unary(ND_EXPR_STMT, expr, tok);
    } else {
      node->ctrl.for_init = expr_stmt(&tok, tok);
    }

    if (!consume_tk(&tok, tok, TK_SEMI)) {
      node->dfr_dest = fnctx->defr;
      node->ctrl.cond = cond_cast(cond_declaration(&tok, tok, TK_SEMI, 1));
      node->dfr_from = fnctx->defr;
    }

    if (tok->kind != TK_RPAREN)
      node->ctrl.for_inc = expression(&tok, tok);
    tok = skip_tk(tok, TK_RPAREN);

    loop_body(rest, tok, node, label_list);

    return leave_stmt_scope(dfr, node);
  }

  if (tok->kind == TK_while) {
    DeferStmt *dfr = enter_stmt_scope();

    Node *node = new_node(ND_FOR, tok);
    tok = skip_tk(tok->next, TK_LPAREN);
    node->dfr_dest = fnctx->defr;
    node->ctrl.cond = cond_cast(cond_declaration(&tok, tok, TK_RPAREN, 1));
    node->dfr_from = fnctx->defr;

    loop_body(rest, tok, node, label_list);

    return leave_stmt_scope(dfr, node);
  }

  if (tok->kind == TK_do) {
    DeferStmt *dfr = enter_stmt_scope();

    Node *node = new_node(ND_DO, tok);

    loop_body(&tok, tok->next, node, label_list);

    tok = skip_tk(tok, TK_while);
    tok = skip_tk(tok, TK_LPAREN);
    node->ctrl.cond = cond_cast(expression(&tok, tok));
    tok = skip_tk(tok, TK_RPAREN);
    *rest = skip_tk(tok, TK_SEMI);
    return leave_stmt_scope(dfr, node);
  }

  if (tok->kind == TK_asm)
    error_tok(tok, "inline assembly is not supported by PIR backend");

  if (tok->kind == TK_goto) {
    if (tok->next->kind == TK_MUL)
      error_tok(tok, "computed goto is not supported by PIR backend");
    Node *node = new_node(ND_GOTO, ident_tok(&tok, tok->next));
    push_goto(node);

    *rest = skip_tk(tok, TK_SEMI);
    return node;
  }

  if (tok->kind == TK_break || tok->kind == TK_continue) {
    JumpContext *ctx = resolve_labeled_jump(rest, tok);
    if (ctx->dfr_ctx != fnctx->defr_ctx)
      error_tok(tok, "illegal jump");

    Node *node = new_node(tok->kind == TK_continue ? ND_CONT : ND_BREAK, tok);
    node->jmp.parent_loop = ctx->node;
    node->dfr_dest = ctx->dfr_lvl;
    node->dfr_from = fnctx->defr;
    return node;
  }

  if (tok->kind == TK_defer) {
    Token *prev = fnctx->defr_ctx;
    fnctx->defr_ctx = tok;
    Node *node = secondary_block(rest, tok->next);
    add_type(node);
    fnctx->defr_ctx = prev;
    new_defr(DF_DEFER_STMT)->stmt = node;
    return new_node(ND_NULL_STMT, tok);
  }

  if (tok->kind == TK_LCURLY)
    return compound_stmt(rest, tok->next, ND_BLOCK);

  return expr_stmt(rest, tok);
}

static Node *compound_stmt2(Token **rest, Token *tok, NodeKind kind) {
  Node *node = new_node(kind, tok);
  node->dfr_dest = fnctx->defr;

  while (consume_tk(&tok, tok, TK_GNU_label)) {
    bool first = true;
    for (; comma_list(&tok, &tok, TK_SEMI, !first); first = false) {
      LocalLabel *ll = arena_calloc(&ast_arena, sizeof(LocalLabel));
      ll->name = ident_tok(&tok, tok);
      ll->next = scope->labels;
      scope->labels = ll;
    }
  }

  Node head = {0};
  Node *cur = &head;
  Node *result = NULL;
  for (;;) {
    Node *prev_cur = cur;
    Token *label_list = label_stmt(&tok, tok, &cur);

    if (tok->kind == TK_RCURLY) {
      if (cur != prev_cur && opt_std < STD_C23)
        error_tok(tok, "label at end of compound statement requires C23");
      break;
    }
    if (consume_tk(&tok, tok, TK_SEMI))
      continue;
    if (tok->kind == TK_PRAGMA) {
      pragma_pack(&tok, tok);
      continue;
    }
    if (tok->kind == TK_static_assert) {
      if (cur != prev_cur && is_iso_std && opt_std < STD_C23 && !tok->file->is_syshdr)
        error_tok(tok, "label followed by a declaration is a C23 extension");
      eval_static_assert(&tok, tok->next);
      tok = skip_tk(tok, TK_SEMI);
      continue;
    }

    Node *node;
    if (is_typename(tok)) {
      if (cur != prev_cur && is_iso_std && opt_std < STD_C23 && !tok->file->is_syshdr)
        error_tok(tok, "label followed by a declaration is a C23 extension");
      Node *expr = declaration(&tok, tok);
      if (!expr)
        continue;
      node = new_unary(ND_EXPR_STMT, expr, tok);
    } else {
      node = result = stmt(&tok, tok, label_list);
    }
    cur = cur->next = node;
  }

  if (cur->kind == ND_RETURN || cur->kind == ND_GOTO)
    fnctx->defr = node->dfr_dest;

  node->dfr_from = fnctx->defr;
  fnctx->defr = node->dfr_dest;
  node->blk.local_labels = resolve_local_gotos();
  node->no_label = leave_scope();

  if (kind == ND_STMT_EXPR && result == cur && result->kind == ND_EXPR_STMT) {
    node->blk.result = result;
    add_type(result);
    Type *ty = result->m.lhs->ty;
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION ||
        (ty->kind == TY_BITINT && bitint_rtn_need_copy(ty->bit_cnt))) {
      Obj *var = new_lvar((ty->qual & Q_ATOMIC) ? ty : unqual(ty));
      Node *n = new_binary(ND_ASSIGN, new_var_node(var, tok), result->m.lhs, tok);
      result->m.lhs = new_binary(ND_COMMA, n, new_var_node(var, tok), tok);
    }
  }

  node->blk.body = head.next;
  *rest = tok->next;
  return node;
}

static Node *compound_stmt(Token **rest, Token *tok, NodeKind kind) {
  if (++parse_depth > 256)
    error_tok(tok, "statement nesting depth exceeded");
  enter_scope();
  Node *r = compound_stmt2(rest, tok, kind);
  parse_depth--;
  return r;
}

static Node *expr_stmt(Token **rest, Token *tok) {
  if (consume_tk(rest, tok, TK_SEMI))
    return new_node(ND_NULL_STMT, tok);

  Node *n = expression(&tok, tok);
  add_type(n);
  if (n->ty->size < 0 && n->ty->kind != TY_ARRAY)
    error_tok(n->tok, "expression has incomplete type");

  Node *node = new_node(ND_EXPR_STMT, tok);
  node->m.lhs = n;
  *rest = skip_tk(tok, TK_SEMI);
  return node;
}

static Node *expression(Token **rest, Token *tok) {
  Node *node = assign(&tok, tok);

  if (tok->kind == TK_COMMA) {
    node = new_binary(ND_COMMA, node, expression(&tok, tok->next), tok);
    node->is_nonlval = true;
  }
  *rest = tok;
  return node;
}

static int64_t eval_error2(Node *node, const char *fmt, ...) {
  if (eval_recover) {
    *eval_recover = true;
    return 0;
  }
  va_list ap;
  va_start(ap, fmt);
  verror_at_tok(node->tok, fmt, ap);
  va_end(ap);
  cleanup_exit(1);
}

static int64_t eval_error(Node *node) {
  return eval_error2(node, "not a compile-time constant");
}

static bool eval_ctx(Node *node, EvalContext *ctx, int64_t *val) {
  bool failed = false;
  bool *prev = eval_recover;
  eval_recover = &failed;

  int64_t v = eval2(node, ctx);
  if (val)
    *val = v;
  eval_recover = prev;
  return !failed;
}

static bool eval_non_var_ofs(Node *node, int64_t *ofs) {
  if (node->kind == ND_MEMBER || node->kind == ND_DEREF) {
    EvalContext ctx = {.kind = EV_AGGREGATE, .vspec = EV_ARRAY | EV_ATOMIC | EV_VOLATILE};
    if (eval_ctx(node, &ctx, ofs) && !ctx.var)
      return true;
  }
  return false;
}

static Obj *eval_var_ofs(Node *node, int64_t *ofs, EvalVarSpec vspec) {
  if (node->kind == ND_VAR) {
    if (((vspec & EV_VOLATILE) || !(node->ty->qual & Q_VOLATILE)) &&
        ((vspec & EV_ATOMIC) || !(node->ty->qual & Q_ATOMIC))) {
      *ofs = 0;
      return node->m.var;
    }
    return NULL;
  }
  if (node->kind == ND_MEMBER || node->kind == ND_DEREF) {
    int64_t offset;
    EvalContext ctx = {.kind = EV_AGGREGATE, .vspec = vspec};
    if (eval_ctx(node, &ctx, &offset) && ctx.var) {
      *ofs = offset;
      return ctx.var;
    }
  }
  return NULL;
}

static Obj *eval_var(Node *node, int64_t *ofs, bool let_volatile) {
  EvalVarSpec vkind = EV_ARRAY | EV_ATOMIC;
  if (let_volatile)
    vkind |= EV_VOLATILE;
  return eval_var_ofs(node, ofs, vkind);
}

static bool is_static_const_var(Obj *var) {
  return var->constexpr_data ||
         (var->init_data && !var->is_weak && is_const_var(var) && !eval_strict);
}

static bool is_constexpr_complit(Node *node, Obj **var) {
  if (node->kind == ND_CHAIN && node->m.rhs->kind == ND_VAR) {
    Obj *v = node->m.rhs->m.var;
    if (v->is_compound_lit && v->constexpr_data) {
      *var = v;
      return true;
    }
  }
  return false;
}

static bool is_non_relo_read(Obj *var, int64_t ofs, int64_t read_sz) {
  for (Relocation *rel = var->rel; rel; rel = rel->next) {
    if ((int64_t)rel->offset + ty_nullptr->size <= ofs)
      continue;
    if (ofs <= (int64_t)rel->offset - read_sz)
      break;
    return false;
  }
  return true;
}

static char *eval_constexpr_data(Node *node) {
  int64_t ofs = 0;
  Obj *var = NULL;

  if (!is_constexpr_complit(node, &var))
    var = eval_var_ofs(node, &ofs, EV_CONSTEXPR);

  if (var && (var->is_string_lit ||
              (is_static_const_var(var) && is_non_relo_read(var, ofs, node->ty->size)))) {
    int64_t access_sz = !is_bitfield(node) ? node->ty->size
                                           : bitfield_footprint(node->m.member);

    if (ofs < 0 || ofs > var->ty->size || access_sz > var->ty->size - ofs)
      return (char *)eval_error2(node, "constexpr access out of bounds");

    char *data = var->init_data ? var->init_data : var->constexpr_data;
    return data + ofs;
  }
  return (char *)eval_error(node);
}

int64_t eval_sign_extend(Type *ty, int64_t val) {
  switch (ty->size) {
  case 1: return ty->is_unsigned ? (uint8_t)val : (int64_t)(int8_t)val;
  case 2: return ty->is_unsigned ? (uint16_t)val : (int64_t)(int16_t)val;
  case 4: return ty->is_unsigned ? (uint32_t)val : (int64_t)(int32_t)val;
  }
  return val;
}

static int64_t eval_cmp(Node *node) {
  Node *lhs = node->m.lhs;
  Node *rhs = node->m.rhs;

  if (lhs->ty->kind == TY_BITINT) {
    BitBuf *lval = eval_bitint(lhs);
    BitBuf *rval = eval_bitint(rhs);
    if (eval_recover && *eval_recover)
      return free(lval), free(rval), 0;

    int res = eval_bitint_cmp(lhs->ty->bit_cnt, lval, rval, lhs->ty->is_unsigned);
    free(lval), free(rval);

    switch (node->kind) {
    case ND_EQ: return res == 0;
    case ND_NE: return res != 0;
    case ND_LT: return res == 1;
    case ND_LE: return res != 2;
    case ND_GT: return res == 2;
    case ND_GE: return res != 1;
    }
  } else if (is_flonum(lhs->ty)) {
    if (eval_strict && is_iso_std && (!node->tok || !node->tok->file || !node->tok->file->is_syshdr))
      return eval_error(node);
    switch (node->kind) {
    case ND_EQ: return eval_double(lhs) == eval_double(rhs);
    case ND_NE: return eval_double(lhs) != eval_double(rhs);
    case ND_LT: return eval_double(lhs) < eval_double(rhs);
    case ND_LE: return eval_double(lhs) <= eval_double(rhs);
    case ND_GT: return eval_double(lhs) > eval_double(rhs);
    case ND_GE: return eval_double(lhs) >= eval_double(rhs);
    }
  } else if (is_ptr(lhs->ty) || is_ptr(rhs->ty)) {
    if (eval_strict && is_iso_std && (!node->tok || !node->tok->file || !node->tok->file->is_syshdr))
      return eval_error(node);
    EvalContext lctx = {.kind = EV_LABEL};
    EvalContext rctx = {.kind = EV_LABEL};
    int64_t lv = eval2(lhs, &lctx);
    int64_t rv = eval2(rhs, &rctx);
    if (eval_recover && *eval_recover)
      return 0;
    if (lctx.var == rctx.var) {
      if (!lctx.var) {
        switch (node->kind) {
        case ND_EQ: return (uint64_t)lv == (uint64_t)rv;
        case ND_NE: return (uint64_t)lv != (uint64_t)rv;
        case ND_LT: return (uint64_t)lv < (uint64_t)rv;
        case ND_LE: return (uint64_t)lv <= (uint64_t)rv;
        case ND_GT: return (uint64_t)lv > (uint64_t)rv;
        case ND_GE: return (uint64_t)lv >= (uint64_t)rv;
        }
      }
      switch (node->kind) {
      case ND_EQ: return lv == rv;
      case ND_NE: return lv != rv;
      case ND_LT: return lv < rv;
      case ND_LE: return lv <= rv;
      case ND_GT: return lv > rv;
      case ND_GE: return lv >= rv;
      }
    }
    if ((node->kind == ND_EQ || node->kind == ND_NE) &&
        ((lctx.var && !lctx.var->is_weak && !rctx.var && rv == 0) ||
         (rctx.var && !rctx.var->is_weak && !lctx.var && lv == 0) ||
         (lctx.var && rctx.var && !lctx.var->is_weak && !rctx.var->is_weak &&
          lctx.var != rctx.var))) {
      return node->kind == ND_NE;
    }
    return eval_error(node);
  } else if (lhs->ty->is_unsigned) {
    switch (node->kind) {
    case ND_EQ: return (uint64_t)eval(lhs) == (uint64_t)eval(rhs);
    case ND_NE: return (uint64_t)eval(lhs) != (uint64_t)eval(rhs);
    case ND_LT: return (uint64_t)eval(lhs) < (uint64_t)eval(rhs);
    case ND_LE: return (uint64_t)eval(lhs) <= (uint64_t)eval(rhs);
    case ND_GT: return (uint64_t)eval(lhs) > (uint64_t)eval(rhs);
    case ND_GE: return (uint64_t)eval(lhs) >= (uint64_t)eval(rhs);
    }
  } else {
    switch (node->kind) {
    case ND_EQ: return eval(lhs) == eval(rhs);
    case ND_NE: return eval(lhs) != eval(rhs);
    case ND_LT: return eval(lhs) < eval(rhs);
    case ND_LE: return eval(lhs) <= eval(rhs);
    case ND_GT: return eval(lhs) > eval(rhs);
    case ND_GE: return eval(lhs) >= eval(rhs);
    }
  }
  internal_error();
}

static int64_t eval(Node *node) {
  return eval2(node, &(EvalContext){.kind = EV_CONST});
}

static bool is_ice_expr_syntax(Node *n) {
  if (!n)
    return true;
  if (n->ty && is_ptr(n->ty))
    return false;
  switch (n->kind) {
  case ND_NUM:
    return !n->ty || !is_flonum(n->ty);
  case ND_NULL_EXPR:
    return true;
  case ND_POS:
  case ND_NEG:
  case ND_NOT:
  case ND_BITNOT:
    if (n->m.lhs && n->m.lhs->ty && (is_flonum(n->m.lhs->ty) || is_ptr(n->m.lhs->ty)))
      return false;
    return is_ice_expr_syntax(n->m.lhs);
  case ND_CAST:
    if (!n->ty || !is_integer(n->ty))
      return false;
    if (n->m.lhs && n->m.lhs->ty && is_ptr(n->m.lhs->ty))
      return false;
    if (n->m.lhs && n->m.lhs->ty && is_flonum(n->m.lhs->ty))
      return !n->is_implicit_cast && n->m.lhs->kind == ND_NUM;
    return is_ice_expr_syntax(n->m.lhs);
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
  case ND_GE:
  case ND_LOGAND:
  case ND_LOGOR:
  case ND_COMMA:
    if ((n->m.lhs && n->m.lhs->ty && (is_flonum(n->m.lhs->ty) || is_ptr(n->m.lhs->ty))) ||
        (n->m.rhs && n->m.rhs->ty && (is_flonum(n->m.rhs->ty) || is_ptr(n->m.rhs->ty))))
      return false;
    return is_ice_expr_syntax(n->m.lhs) && is_ice_expr_syntax(n->m.rhs);
  case ND_COND:
    return is_ice_expr_syntax(n->ctrl.cond) && is_ice_expr_syntax(n->ctrl.then) &&
           is_ice_expr_syntax(n->ctrl.els);
  default:
    return false;
  }
}

static int eval2_depth = 0;

static int64_t eval2_inner(Node *node, EvalContext *ctx);

static int64_t eval2(Node *node, EvalContext *ctx) {
  if (eval_recover && *eval_recover)
    return 0;
  if (++eval2_depth > 512) {
    eval2_depth--;
    return eval_error2(node, "constant expression nesting depth exceeded");
  }
  int64_t r = eval2_inner(node, ctx);
  eval2_depth--;
  return r;
}

static int64_t eval_sub_no_label(Node *n, EvalContext *ctx) {
  if (ctx->kind != EV_LABEL)
    return eval(n);
  EvalContext sub = {.kind = EV_LABEL};
  int64_t v = eval2(n, &sub);
  if (sub.var)
    return eval_error(n);
  return v;
}

static int64_t eval2_inner(Node *node, EvalContext *ctx) {
  if (eval_recover && *eval_recover)
    return 0;

  Type *ty = node->ty;
  Node *lhs = node->m.lhs;
  Node *rhs = node->m.rhs;
  bool strict_iso = eval_strict && is_iso_std &&
                    (!node->tok || !node->tok->file || !node->tok->file->is_syshdr);

  switch (node->kind) {
  case ND_ADD: {
    int64_t lval = eval2(lhs, ctx);
    int64_t rval = eval2(rhs, ctx);
    if (strict_iso && is_integer(ty) && !ty->is_unsigned) {
      if (ty->size == 4) {
        int32_t res;
        if (__builtin_add_overflow((int32_t)lval, (int32_t)rval, &res))
          return eval_error2(node, "integer overflow in constant expression");
      } else if (ty->size == 8) {
        int64_t res;
        if (__builtin_add_overflow(lval, rval, &res))
          return eval_error2(node, "integer overflow in constant expression");
      }
    }
    return eval_sign_extend(ty, (uint64_t)lval + (uint64_t)rval);
  }
  case ND_SUB: {
    if (ctx->kind == EV_LABEL) {
      EvalContext lctx = {.kind = EV_LABEL};
      EvalContext rctx = {.kind = EV_LABEL};
      int64_t lv = eval2(lhs, &lctx);
      int64_t rv = eval2(rhs, &rctx);
      if (lctx.var && rctx.var) {
        if (lctx.var != rctx.var)
          return eval_error(node);
        return eval_sign_extend(ty, (uint64_t)lv - (uint64_t)rv);
      }
      if (rctx.var)
        return eval_error(rhs);
      if (lctx.var) {
        if (ctx->var)
          return eval_error(lhs);
        ctx->var = lctx.var;
      }
      return eval_sign_extend(ty, (uint64_t)lv - (uint64_t)rv);
    }
    int64_t lval = eval2(lhs, ctx);
    int64_t rval = eval(rhs);
    if (strict_iso && is_integer(ty) && !ty->is_unsigned) {
      if (ty->size == 4) {
        int32_t res;
        if (__builtin_sub_overflow((int32_t)lval, (int32_t)rval, &res))
          return eval_error2(node, "integer overflow in constant expression");
      } else if (ty->size == 8) {
        int64_t res;
        if (__builtin_sub_overflow(lval, rval, &res))
          return eval_error2(node, "integer overflow in constant expression");
      }
    }
    return eval_sign_extend(ty, (uint64_t)lval - (uint64_t)rval);
  }
  case ND_MUL: {
    int64_t lval = eval_sub_no_label(lhs, ctx);
    int64_t rval = eval_sub_no_label(rhs, ctx);
    if (strict_iso && is_integer(ty) && !ty->is_unsigned) {
      if (ty->size == 4) {
        int32_t res;
        if (__builtin_mul_overflow((int32_t)lval, (int32_t)rval, &res))
          return eval_error2(node, "integer overflow in constant expression");
      } else if (ty->size == 8) {
        int64_t res;
        if (__builtin_mul_overflow(lval, rval, &res))
          return eval_error2(node, "integer overflow in constant expression");
      }
    }
    return eval_sign_extend(ty, (uint64_t)lval * (uint64_t)rval);
  }
  case ND_DIV: {
    int64_t lval = eval_sub_no_label(lhs, ctx);
    int64_t rval = eval_sub_no_label(rhs, ctx);
    if (!rval)
      return eval_error2(rhs, "division by zero during constant evaluation");
    if (ty->is_unsigned)
      return (uint64_t)lval / rval;
    if (((ty->size == 8 && lval == INT64_MIN) || (ty->size == 4 && lval == INT32_MIN)) &&
        rval == -1) {
      if (eval_strict && is_iso_std && (!node->tok || !node->tok->file || !node->tok->file->is_syshdr))
        return eval_error2(node, "integer overflow in constant expression");
      return lval;
    }
    return lval / rval;
  }
  case ND_REM: {
    int64_t lval = eval_sub_no_label(lhs, ctx);
    int64_t rval = eval_sub_no_label(rhs, ctx);
    if (!rval)
      return eval_error2(rhs, "remainder by zero during constant evaluation");
    if (ty->is_unsigned)
      return (uint64_t)lval % rval;
    if (((ty->size == 8 && lval == INT64_MIN) || (ty->size == 4 && lval == INT32_MIN)) &&
        rval == -1) {
      if (eval_strict && is_iso_std && (!node->tok || !node->tok->file || !node->tok->file->is_syshdr))
        return eval_error2(node, "integer overflow in constant expression");
      return 0;
    }
    return lval % rval;
  }
  case ND_POS:    return eval_sub_no_label(lhs, ctx);
  case ND_NEG: {
    int64_t lval = eval_sub_no_label(lhs, ctx);
    if (strict_iso && is_integer(ty) && !ty->is_unsigned &&
        ((ty->size == 4 && lval == INT32_MIN) || (ty->size == 8 && lval == INT64_MIN)))
      return eval_error2(node, "integer overflow in constant expression");
    return eval_sign_extend(ty, -(uint64_t)lval);
  }
  case ND_BITAND: return eval_sub_no_label(lhs, ctx) & eval_sub_no_label(rhs, ctx);
  case ND_BITOR:  return eval_sub_no_label(lhs, ctx) | eval_sub_no_label(rhs, ctx);
  case ND_BITXOR: return eval_sub_no_label(lhs, ctx) ^ eval_sub_no_label(rhs, ctx);
  case ND_SHL:
  case ND_SHR:
  case ND_SAR: {
    int64_t lval = eval_sub_no_label(lhs, ctx);
    int64_t rval = 0;
    bool r_oob = false;
    if (rhs->kind == ND_CAST && rhs->m.lhs->ty->kind == TY_BITINT &&
        rhs->m.lhs->ty->bit_cnt > 64) {
      BitBuf *rb = eval_bitint(rhs->m.lhs);
      if (eval_recover && *eval_recover)
        return free(rb), 0;
      clean_bitint(rb, rhs->m.lhs->ty);
      int64_t rcnt = bitint_buffer_size(rhs->m.lhs->ty) / 8;
      if (!rhs->m.lhs->ty->is_unsigned && ((int64_t)(&rb->as64)[rcnt - 1] < 0))
        r_oob = true;
      for (int64_t i = 1; i < rcnt && !r_oob; i++)
        if ((&rb->as64)[i] != 0)
          r_oob = true;
      if ((uint64_t)rb->as64 >= (uint64_t)(ty->size * 8))
        r_oob = true;
      rval = (int64_t)rb->as64;
      free(rb);
    } else {
      rval = eval_sub_no_label(rhs, ctx);
      if (rval < 0 || rval >= ty->size * 8)
        r_oob = true;
    }
    if (r_oob) {
      if (strict_iso)
        return eval_error2(node, "shift count out of range in constant expression");
      return 0;
    }
    if (node->kind == ND_SHL) {
      if (strict_iso && is_integer(ty) && !ty->is_unsigned) {
        int bits = ty->size * 8;
        if (lval < 0 || (rval > 0 && ((uint64_t)lval >> (bits - 1 - rval)) != 0))
          return eval_error2(node, "signed left shift overflow in constant expression");
      }
      return eval_sign_extend(ty, (uint64_t)lval << rval);
    }
    if (node->kind == ND_SHR) {
      if (ty->size == 4)
        return (uint32_t)lval >> rval;
      return (uint64_t)lval >> rval;
    }
    if (ty->size == 4)
      return (int32_t)lval >> rval;
    return lval >> rval;
  }
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_GT:
  case ND_GE: {
    return eval_cmp(node);
  }
  case ND_COND: {
    bool cond = eval(node->ctrl.cond);
    if (strict_iso) {
      Node *uneval = cond ? node->ctrl.els : node->ctrl.then;
      if (!is_ice_expr_syntax(uneval))
        return eval_error(uneval);
    }
    return cond ? eval2(node->ctrl.then, ctx) : eval2(node->ctrl.els, ctx);
  }
  case ND_NOT: {
    if (strict_iso && (is_flonum(lhs->ty) || is_ptr(lhs->ty)))
      return eval_error(node);
    if (!eval_strict) {
      bool rec = false;
      bool *prev_rec = eval_recover;
      eval_recover = &rec;
      EvalContext lctx = {.kind = EV_LABEL};
      int64_t lv = eval2(lhs, &lctx);
      eval_recover = prev_rec;
      if (!rec && lctx.var) {
        if (!lctx.var->is_weak)
          return 0;
        return eval_error(node);
      }
      if (!rec)
        return !lv;
    }
    return !eval(lhs);
  }
  case ND_BITNOT: return eval_sign_extend(ty, ~eval(lhs));
  case ND_LOGAND: {
    if (strict_iso && (is_flonum(lhs->ty) || is_ptr(lhs->ty)))
      return eval_error(node);
    int64_t lval = eval(lhs);
    if (!lval) {
      if (strict_iso && !is_ice_expr_syntax(rhs))
        return eval_error(rhs);
      return 0;
    }
    if (strict_iso && (is_flonum(rhs->ty) || is_ptr(rhs->ty)))
      return eval_error(node);
    return eval(rhs);
  }
  case ND_LOGOR: {
    if (strict_iso && (is_flonum(lhs->ty) || is_ptr(lhs->ty)))
      return eval_error(node);
    int64_t lval = eval(lhs);
    if (lval) {
      if (strict_iso && !is_ice_expr_syntax(rhs))
        return eval_error(rhs);
      return 1;
    }
    if (strict_iso && (is_flonum(rhs->ty) || is_ptr(rhs->ty)))
      return eval_error(node);
    return eval(rhs);
  }
  case ND_CAST: {
    if (strict_iso && is_ptr(lhs->ty))
      return eval_error(node);
    if (lhs->ty->kind == TY_BITINT) {
      BitBuf *data = eval_bitint(lhs);
      if (eval_recover && *eval_recover)
        return free(data), 0;

      if (ty->kind == TY_BOOL) {
        bool res = eval_bitint_to_bool(lhs->ty->bit_cnt, data);
        free(data);
        return res;
      }
      int64_t ival = data->as64;
      free(data);
      if (lhs->ty->bit_cnt < ty->size * 8) {
        int shft = 64 - lhs->ty->bit_cnt;
        if (lhs->ty->is_unsigned)
          ival = ((uint64_t)ival << shft) >> shft;
        else
          ival = (int64_t)((uint64_t)ival << shft) >> shft;
      }
      return eval_sign_extend(ty, ival);
    }

    if (is_flonum(lhs->ty)) {
      if (strict_iso && (node->is_implicit_cast || lhs->kind != ND_NUM))
        return eval_error(lhs);
      long_double_t d = eval_double(lhs);
      if (ty->kind == TY_BOOL)
        return d != 0;
      if (!(d == d)) {
        if (strict_iso)
          return eval_error2(node, "float-to-integer conversion out of range in constant expression");
        return 0;
      }
      if (ty->size == 8 && ty->is_unsigned) {
        if (d < 0.0 || d >= 18446744073709551616.0) {
          if (strict_iso)
            return eval_error2(node, "float-to-integer conversion out of range in constant expression");
          return 0;
        }
        return (int64_t)(uint64_t)d;
      }
      if (d < -9223372036854775808.0 || d >= 9223372036854775808.0) {
        if (strict_iso)
          return eval_error2(node, "float-to-integer conversion out of range in constant expression");
        return 0;
      }
      int64_t iv = (int64_t)d;
      if (strict_iso && is_integer(ty) && ty->size < 8) {
        int bits = ty->size * 8;
        if (ty->is_unsigned) {
          if (d < 0.0 || (uint64_t)iv >= (UINT64_C(1) << bits))
            return eval_error2(node, "float-to-integer conversion out of range in constant expression");
        } else {
          int64_t minv = -(INT64_C(1) << (bits - 1));
          int64_t maxv = (INT64_C(1) << (bits - 1)) - 1;
          if (iv < minv || iv > maxv)
            return eval_error2(node, "float-to-integer conversion out of range in constant expression");
        }
      }
      return eval_sign_extend(ty, iv);
    }

    if (ty->kind == TY_BOOL || (is_integer(ty) && ty->size < ty_nullptr->size)) {
      if (ctx->kind == EV_LABEL || (ty->kind == TY_BOOL && !eval_strict)) {
        bool rec = false;
        bool *prev_rec = eval_recover;
        if (ty->kind == TY_BOOL && !eval_strict)
          eval_recover = &rec;
        EvalContext lctx = {.kind = EV_LABEL};
        int64_t val = eval2(lhs, &lctx);
        eval_recover = prev_rec;
        if (!rec && lctx.var) {
          if (ty->kind == TY_BOOL && !eval_strict && !lctx.var->is_weak)
            return 1;
          return eval_error(node);
        }
        if (!rec) {
          if (ty->kind == TY_BOOL)
            return !!val;
          return eval_sign_extend(ty, val);
        }
      }
    }

    int64_t val = eval2(lhs, ctx);
    if (ty->kind == TY_BOOL)
      return !!val;
    if (is_integer(ty))
      return eval_sign_extend(ty, val);
    return val;
  }
  case ND_NUM: return node->num.val;
  }

  if (ctx->kind == EV_AGGREGATE) {
    if (((ty->qual & Q_ATOMIC) && !(ctx->vspec & EV_ATOMIC)) ||
        ((ty->qual & Q_VOLATILE) && !(ctx->vspec & EV_VOLATILE)))
      return eval_error(node);

    if (node->kind == ND_DEREF) {
      ctx->deref_cnt++;
      return eval2(lhs, ctx);
    }

    if (node->kind == ND_ADDR && ctx->deref_cnt > 0) {
      ctx->deref_cnt--;
      return eval2(lhs, ctx);
    }

    for (Type *t = ty; t && t->kind == TY_ARRAY; t = t->base)
      ctx->deref_cnt--;

    if (ctx->deref_cnt < 0) {
      if (!(ctx->vspec & EV_ARRAY))
        return eval_error(node);
      ctx->vspec -= EV_ARRAY;
      ctx->deref_cnt = 0;
    }

    bool is_agg = ty->kind == TY_ARRAY || ty->kind == TY_STRUCT || ty->kind == TY_UNION;

    if (node->kind == ND_MEMBER && (!ctx->deref_cnt || is_agg))
      return (int64_t)((uint64_t)eval2(lhs, ctx) + (uint64_t)node->m.member->offset);

    if (node->kind == ND_VAR && (!ctx->deref_cnt && is_agg)) {
      ctx->var = node->m.var;
      return 0;
    }

    if ((ctx->vspec & EV_CONSTEXPR) && is_constexpr_complit(node, &ctx->var))
      return 0;
  }

  if (ctx->kind == EV_LABEL) {
    if (!ctx->var) {
      int64_t ofs;
      Obj *var = NULL;
      if (node->kind == ND_ADDR)
        var = eval_var(lhs, &ofs, true);
      else if (node->ty->kind == TY_ARRAY || node->ty->kind == TY_FUNC)
        var = eval_var(node, &ofs, true);

      if (var && var->kind == OBJ_GLOBAL) {
        ctx->var = var;
        return ofs;
      }
    }
    return eval(node);
  }

  if (ctx->kind == EV_CONST) {
    int64_t ofs;
    if (node->kind == ND_ADDR) {
      if (strict_iso)
        return eval_error(node);
      if (eval_non_var_ofs(lhs, &ofs))
        return ofs;
    } else if (node->ty->kind == TY_ARRAY) {
      if (strict_iso)
        return eval_error(node);
      if (eval_non_var_ofs(node, &ofs))
        return ofs;
    }
    if (is_integer(ty) || ty->kind == TY_PTR || ty->kind == TY_NULLPTR) {
      if (strict_iso && node->kind == ND_DEREF && opt_std < STD_C23)
        return eval_error(node);
      char *data = eval_constexpr_data(node);
      if (data) {
        if (!is_bitfield(node))
          return eval_sign_extend(ty, read_buf(data, ty->size));

        Member *mem = node->m.member;
        uint64_t val = 0;
        if (mem->is_aligned_bitfield) {
          val = read_buf(data, next_pow_of_two(mem->bit_offset + mem->bit_width) / 8);
          val <<= (64 - mem->bit_width - mem->bit_offset);
        } else {
          int pofs = bitfield_footprint(mem) - 1;
          for (int start = pofs; pofs >= !!mem->bit_offset; pofs--) {
            if (pofs != start)
              val <<= 8;
            val |= (uint8_t)data[pofs];
          }
          if (mem->bit_offset) {
            val <<= 8 - mem->bit_offset;
            val |= (uint8_t)*data >> mem->bit_offset;
          }
          val <<= (64 - mem->bit_width);
        }
        if (ty->is_unsigned)
          return (uint64_t)val >> (64 - mem->bit_width);
        return (int64_t)val >> (64 - mem->bit_width);
      }
    }
  }
  return eval_error(node);
}

static void chk_bitint_within_64bit(BitBuf *data, Type *ty, Token *tok) {
  clean_bitint(data, ty);

  int64_t cnt = bitint_buffer_size(ty) / 8;

  uint64_t chunk;
  if (!ty->is_unsigned && ((int64_t)(&data->as64)[cnt - 1] < 0))
    chunk = -1;
  else
    chunk = 0;

  for (int64_t i = 1; i < cnt; i++)
    if (chunk != (&data->as64)[i])
      error_tok(tok, "_BitInt constant out of range");
  if (!ty->is_unsigned && (((int64_t)(&data->as64)[0] < 0) != (chunk != 0)))
    error_tok(tok, "_BitInt constant out of range");
}

static bool int_or_trunc_bitint(Node **node, bool chk_narrow) {
  add_type(*node);

  if (is_integer((*node)->ty))
    return true;

  if ((*node)->ty->kind == TY_BITINT) {
    if (chk_narrow && (*node)->ty->bit_cnt > 64) {
      BitBuf *data = NULL;
      if (!is_const_bitint(*node, &data))
        error_tok((*node)->tok, "large _BitInt unsupported here");

      chk_bitint_within_64bit(data, (*node)->ty, (*node)->tok);
      free(data);
    }
    if ((*node)->ty->is_unsigned)
      *node = new_cast(*node, enum_ty[ETY_U64]);
    else
      *node = new_cast(*node, enum_ty[ETY_I64]);
    return true;
  }
  return false;
}

bool is_const_expr(Node *node, int64_t *val) {
  add_type(node);
  bool failed = false;
  bool *prev = eval_recover;
  eval_recover = &failed;

  int64_t v = eval(node);
  if (val)
    *val = v;
  eval_recover = prev;
  return !failed;
}

bool is_const_expr_strict(Node *node, int64_t *val) {
  bool prv = eval_strict;
  eval_strict = true;

  bool res = is_const_expr(node, val);

  eval_strict = prv;
  return res;
}

bool is_const_fp(Node *node, FPVal *fval) {
  bool failed = false;
  bool *prev = eval_recover;
  eval_recover = &failed;

  eval_fp(node, fval);

  eval_recover = prev;
  return !failed;
}

static bool is_const_bitint(Node *node, BitBuf **result) {
  bool failed = false;
  bool *prev = eval_recover;
  eval_recover = &failed;

  BitBuf *data = eval_bitint(node);

  eval_recover = prev;

  if (result && !failed)
    *result = data;
  else
    free(data);
  return !failed;
}

static bool is_const_bitint_strict(Node *node, BitBuf **result) {
  bool prv = eval_strict;
  eval_strict = true;

  bool res = is_const_bitint(node, result);

  eval_strict = prv;
  return res;
}

bool is_const_zero_bitint(Node *node) {
  BitBuf *val = NULL;
  if (is_const_bitint_strict(node, &val)) {
    bool res = eval_bitint_to_bool(node->ty->bit_cnt, val);
    free(val);
    return !res;
  }
  return false;
}

static int64_t const_expr2(Token **rest, Token *tok, Type **ty) {
  Node *node = conditional(rest, tok);
  add_type(node);
  if (ty)
    *ty = node->ty;
  return eval_ice_node(node, ICE_I64_STRICT64, node->tok, "expression not integer");
}

int64_t const_expr(Token **rest, Token *tok) {
  return const_expr2(rest, tok, NULL);
}

static int64_t align_expr(Token **rest, Token *tok) {
  int64_t val = const_expr2(rest, tok, NULL);
  if (val <= 0 || val >= (1LL << 31) || !is_pow_of_two(val))
    error_tok(tok, "invalid alignment");
  return val;
}

static long_double_t eval_fp_cast(long_double_t fval, Type *ty) {
  switch (ty->kind) {
  case TY_FLOAT:   return (float)fval;
  case TY_DOUBLE:  return (double)fval;
  case TY_LDOUBLE: return fval;
  }
  internal_error();
}

static void build_math_constant(Node *node, FPVal *fval) {
  switch (node->num.constant) {
  case MATH_CONSTANT_NANF:  fval->chunk[0] = 0x7FC00000; return;
  case MATH_CONSTANT_INFF:  fval->chunk[0] = 0x7F800000; return;
  case MATH_CONSTANT_NANSF: fval->chunk[0] = 0x7FA00000; return;
  case MATH_CONSTANT_NANS:  fval->chunk[0] = 0x7FF4000000000000; return;
  case MATH_CONSTANT_NANSL:
#ifdef BOOTSTRAP_NO_LDOUBLE
    fval->chunk[0] = 0x7FF4000000000000;
#else
    fval->chunk[0] = 0xA000000000000000;
    fval->chunk[1] = 0x7FFF;
#endif
    return;
  }
  internal_error();
}

void eval_fp(Node *node, FPVal *fval) {
  while (node->kind == ND_CAST && node->ty->kind == node->m.lhs->ty->kind)
    node = node->m.lhs;

  if (node->kind == ND_NUM && node->num.constant) {
    if (fval)
      build_math_constant(node, fval);
    return;
  }

  long_double_t v = eval_double(node);
  if (fval) {
    switch (node->ty->kind) {
    case TY_FLOAT:   fval->f = (float)v; return;
    case TY_DOUBLE:  fval->d = (double)v; return;
    case TY_LDOUBLE: fval->ld = v; return;
    }
    internal_error();
  }
}

static long_double_t eval_double(Node *node) {
  if (eval_recover && *eval_recover)
    return false;

  Type *ty = node->ty;
  Node *lhs = node->m.lhs;
  Node *rhs = node->m.rhs;

  switch (node->kind) {
  case ND_ADD: return eval_fp_cast(eval_double(lhs) + eval_double(rhs), ty);
  case ND_SUB: return eval_fp_cast(eval_double(lhs) - eval_double(rhs), ty);
  case ND_MUL: return eval_fp_cast(eval_double(lhs) * eval_double(rhs), ty);
  case ND_DIV: {
    long_double_t lval = eval_double(lhs);
    long_double_t rval = eval_double(rhs);
    if (rval == 0 && !is_const_context())
      break;
    return eval_fp_cast(lval / rval, ty);
  }
  case ND_POS:  return eval_double(lhs);
  case ND_NEG:  return -eval_double(lhs);
  case ND_COND: {
    bool cond = eval(node->ctrl.cond);
    if (eval_strict) {
      long_double_t lval = eval_double(node->ctrl.then);
      long_double_t rval = eval_double(node->ctrl.els);
      return cond ? lval : rval;
    }
    return cond ? eval_double(node->ctrl.then) : eval_double(node->ctrl.els);
  }
  case ND_CAST:
    if (is_flonum(lhs->ty))
      return eval_fp_cast(eval_double(lhs), ty);

    if (is_integer(lhs->ty)) {
      int64_t val = eval(lhs);
      if (lhs->ty->size == 8 && lhs->ty->is_unsigned)
        return eval_fp_cast((uint64_t)val, ty);
      return eval_fp_cast(val, ty);
    }
    if (lhs->ty->kind == TY_BITINT) {
      BitBuf *data = eval_bitint(lhs);
      if (eval_recover && *eval_recover)
        return free(data), 0;
      clean_bitint(data, lhs->ty);
      int64_t cnt = bitint_buffer_size(lhs->ty) / 8;
      bool is_neg = !lhs->ty->is_unsigned && ((int64_t)(&data->as64)[cnt - 1] < 0);
      if (is_neg)
        eval_bitint_neg(lhs->ty->bit_cnt, data);
      long_double_t acc = 0.0;
      for (int64_t i = cnt - 1; i >= 0; i--)
        acc = acc * 18446744073709551616.0 + (long_double_t)(&data->as64)[i];
      free(data);
      return eval_fp_cast(is_neg ? -acc : acc, ty);
    }

    return eval_error2(node, "unimplemented cast");
  case ND_NUM:
    if (node->num.constant) {
      FPVal fval = {0};
      build_math_constant(node, &fval);

      switch (node->ty->kind) {
      case TY_FLOAT:   return fval.f;
      case TY_DOUBLE:  return fval.d;
      case TY_LDOUBLE: return fval.ld;
      }
      internal_error();
    }
    return node->num.fval;
  }

  char *data = eval_constexpr_data(node);
  if (data)
    return read_double_buf(data, ty);

  return eval_error(node);
}

static bool bitint_sign_bit(const BitBuf *v, int64_t bits) {
  int64_t idx = (bits - 1) / 64;
  return (((&v->as64)[idx] >> ((bits - 1) % 64)) & 1u) != 0u;
}

static bool bitint_is_signed_min(const BitBuf *v, Type *ty) {
  int64_t top_idx = (ty->bit_cnt - 1) / 64;
  uint64_t top_bit = UINT64_C(1) << ((ty->bit_cnt - 1) % 64);
  for (int64_t i = 0; i < top_idx; i++)
    if ((&v->as64)[i] != 0)
      return false;
  uint64_t mask = (ty->bit_cnt % 64 == 0) ? UINT64_MAX : ((top_bit << 1) - 1u);
  return ((&v->as64)[top_idx] & mask) == top_bit;
}

static BitBuf *eval_bitint(Node *node) {
  if (eval_recover && *eval_recover)
    return NULL;

  Type *ty = node->ty;
  Node *lhs = node->m.lhs;
  Node *rhs = node->m.rhs;

  switch (node->kind) {
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
  case ND_REM: {
    BitBuf *lval = eval_bitint(lhs);
    BitBuf *rval = eval_bitint(rhs);
    if (eval_recover && *eval_recover)
      return free(lval), free(rval), NULL;

    bool sa = (!ty->is_unsigned && eval_strict) ? bitint_sign_bit(lval, ty->bit_cnt) : false;
    bool sb = (!ty->is_unsigned && eval_strict) ? bitint_sign_bit(rval, ty->bit_cnt) : false;

    switch (node->kind) {
    case ND_BITAND: eval_bitint_bitand(ty->bit_cnt, lval, rval); break;
    case ND_BITOR:  eval_bitint_bitor(ty->bit_cnt, lval, rval); break;
    case ND_BITXOR: eval_bitint_bitxor(ty->bit_cnt, lval, rval); break;
    case ND_ADD:
      eval_bitint_add(ty->bit_cnt, lval, rval);
      if (!ty->is_unsigned && eval_strict && sa == sb && bitint_sign_bit(rval, ty->bit_cnt) != sa) {
        free(lval); free(rval);
        eval_error2(node, "integer overflow in constant expression");
        return NULL;
      }
      break;
    case ND_SUB:
      eval_bitint_sub(ty->bit_cnt, lval, rval);
      if (!ty->is_unsigned && eval_strict && sa != sb && bitint_sign_bit(rval, ty->bit_cnt) != sa) {
        free(lval); free(rval);
        eval_error2(node, "integer overflow in constant expression");
        return NULL;
      }
      break;
    case ND_MUL: {
      if (!ty->is_unsigned && eval_strict) {
        size_t bsz = (size_t)bitint_buffer_size(ty);
        bool l_nz = eval_bitint_to_bool(ty->bit_cnt, lval);
        bool r_nz = eval_bitint_to_bool(ty->bit_cnt, rval);
        BitBuf *orig_r = NULL;
        if (l_nz && r_nz) {
          orig_r = malloc(bsz);
          memcpy(orig_r, rval, bsz);
        }
        eval_bitint_mul(ty->bit_cnt, lval, rval);
        if (orig_r) {
          BitBuf *chk_a = malloc(bsz);
          BitBuf *chk_b = malloc(bsz);
          memcpy(chk_a, rval, bsz);
          memcpy(chk_b, lval, bsz);
          clean_bitint(chk_a, ty);
          clean_bitint(chk_b, ty);
          bool ovf = false;
          if (bitint_is_signed_min(chk_a, ty)) {
            int64_t cnt = (int64_t)(bsz / 8);
            bool b_m1 = true;
            for (int64_t i = 0; i < cnt; i++)
              if ((&chk_b->as64)[i] != UINT64_MAX) { b_m1 = false; break; }
            if (b_m1) ovf = true;
          }
          if (!ovf) {
            eval_bitint_div(ty->bit_cnt, chk_a, chk_b, false, true);
            clean_bitint(chk_b, ty);
            clean_bitint(orig_r, ty);
            if (memcmp(chk_b, orig_r, bsz) != 0)
              ovf = true;
          }
          free(chk_a);
          free(chk_b);
          free(orig_r);
          if (ovf) {
            free(lval); free(rval);
            eval_error2(node, "integer overflow in constant expression");
            return NULL;
          }
        }
      } else {
        eval_bitint_mul(ty->bit_cnt, lval, rval);
      }
      break;
    }
    case ND_DIV:
    case ND_REM: {
      bool res = eval_bitint_to_bool(ty->bit_cnt, rval);
      if (!res) {
        free(lval);
        free(rval);
        eval_error2(node, "division by zero during constant evaluation");
        return NULL;
      }
      if (!ty->is_unsigned) {
        int64_t cnt = bitint_buffer_size(ty) / 8;
        clean_bitint(lval, ty);
        clean_bitint(rval, ty);
        bool r_m1 = true;
        for (int64_t i = 0; i < cnt; i++)
          if ((&rval->as64)[i] != UINT64_MAX) { r_m1 = false; break; }
        if (r_m1) {
          int64_t top_idx = (ty->bit_cnt - 1) / 64;
          uint64_t top_bit = UINT64_C(1) << ((ty->bit_cnt - 1) % 64);
          bool l_min = true;
          for (int64_t i = 0; i < top_idx; i++)
            if ((&lval->as64)[i] != 0) { l_min = false; break; }
          if ((&lval->as64)[top_idx] != ~(top_bit - 1))
            l_min = false;
          for (int64_t i = top_idx + 1; i < cnt; i++)
            if ((&lval->as64)[i] != UINT64_MAX) { l_min = false; break; }
          if (l_min) {
            free(lval);
            free(rval);
            eval_error2(node, "integer overflow in constant expression");
            return NULL;
          }
        }
      }
      eval_bitint_div(ty->bit_cnt, lval, rval, ty->is_unsigned, node->kind == ND_DIV);
      break;
    }
    }
    return free(lval), rval;
  }
  case ND_SHL:
  case ND_SHR:
  case ND_SAR: {
    BitBuf *val = eval_bitint(lhs);
    uint64_t amount;
    if (rhs->kind == ND_CAST && rhs->m.lhs->ty->kind == TY_BITINT &&
        rhs->m.lhs->ty->bit_cnt > 64) {
      BitBuf *rb = eval_bitint(rhs->m.lhs);
      if (eval_recover && *eval_recover)
        return free(val), free(rb), NULL;
      clean_bitint(rb, rhs->m.lhs->ty);
      int64_t rcnt = bitint_buffer_size(rhs->m.lhs->ty) / 8;
      bool oob = (!rhs->m.lhs->ty->is_unsigned && ((int64_t)(&rb->as64)[rcnt - 1] < 0));
      for (int64_t i = 1; i < rcnt && !oob; i++)
        if ((&rb->as64)[i] != 0)
          oob = true;
      amount = rb->as64;
      free(rb);
      if (oob) {
        free(val);
        eval_error2(rhs, "invalid shift amount");
        return NULL;
      }
    } else {
      amount = eval(rhs);
      if (eval_recover && *eval_recover)
        return free(val), NULL;
    }

    if (amount >= (uint64_t)ty->bit_cnt) {
      free(val);
      eval_error2(rhs, "invalid shift amount");
      return NULL;
    }

    if (node->kind == ND_SHL) {
      if (!ty->is_unsigned && eval_strict && amount > 0) {
        if (bitint_sign_bit(val, ty->bit_cnt)) {
          free(val);
          eval_error2(node, "integer overflow in constant expression");
          return NULL;
        }
        int64_t first_lost = ty->bit_cnt - 1 - (int64_t)amount;
        for (int64_t bi = first_lost; bi < ty->bit_cnt; bi++) {
          if (((&val->as64)[bi / 64] >> (bi % 64)) & 1u) {
            free(val);
            eval_error2(node, "integer overflow in constant expression");
            return NULL;
          }
        }
      }
      eval_bitint_shl(ty->bit_cnt, val, val, amount);
    } else
      eval_bitint_shr(ty->bit_cnt, val, val, amount, ty->is_unsigned);
    return val;
  }
  case ND_BITNOT:
  case ND_POS:
  case ND_NEG: {
    BitBuf *val = eval_bitint(lhs);
    if (eval_recover && *eval_recover)
      return free(val), NULL;

    switch (node->kind) {
    case ND_BITNOT: eval_bitint_bitnot(ty->bit_cnt, val); break;
    case ND_NEG:
      if (!ty->is_unsigned && eval_strict && bitint_is_signed_min(val, ty)) {
        free(val);
        eval_error2(node, "integer overflow in constant expression");
        return NULL;
      }
      eval_bitint_neg(ty->bit_cnt, val);
      break;
    }
    return val;
  }
  case ND_COND: {
    bool cond = eval(node->ctrl.cond);
    return cond ? eval_bitint(node->ctrl.then) : eval_bitint(node->ctrl.els);
  }
  case ND_CAST:
    if (lhs->ty->kind == TY_BITINT) {
      BitBuf *val = eval_bitint(lhs);
      if (eval_recover && *eval_recover)
        return free(val), NULL;

      val = realloc(val, bitint_buffer_size(ty));
      if (ty->bit_cnt > lhs->ty->bit_cnt)
        eval_bitint_sign_ext(lhs->ty->bit_cnt, val, ty->bit_cnt, lhs->ty->is_unsigned);
      return val;
    }
    if (is_integer(lhs->ty)) {
      BitBuf *val = calloc(1, bitint_buffer_size(ty));
      val->as64 = eval(lhs);

      if (ty->bit_cnt > lhs->ty->size * 8)
        eval_bitint_sign_ext(lhs->ty->size * 8, val, ty->bit_cnt, lhs->ty->is_unsigned);
      return val;
    }
    if (is_flonum(lhs->ty)) {
      long_double_t d = eval_double(lhs);
      if (eval_recover && *eval_recover)
        return NULL;
      if (!(d == d) || (ty->is_unsigned && d <= -1.0)) {
        eval_error2(node, "float-to-_BitInt conversion out of range in constant expression");
        return NULL;
      }
      bool is_neg = (!ty->is_unsigned && d < 0.0);
      long_double_t mag = (ty->is_unsigned && d < 0.0) ? 0.0 : (is_neg ? -d : d);
      BitBuf *val = calloc(1, bitint_buffer_size(ty));
      int64_t cnt = bitint_buffer_size(ty) / 8;
      for (int64_t i = 0; i < cnt && mag >= 1.0; i++) {
        long_double_t q = mag / 18446744073709551616.0;
        long_double_t q_int = (q < 1.0) ? 0.0
                            : (q < 18446744073709551616.0) ? (long_double_t)(uint64_t)q : q;
        long_double_t rem = mag - q_int * 18446744073709551616.0;
        if (rem < 0.0) rem = 0.0;
        if (rem >= 18446744073709551616.0) { rem -= 18446744073709551616.0; q_int += 1.0; }
        (&val->as64)[i] = (uint64_t)rem;
        mag = q_int;
      }
      if (is_neg)
        eval_bitint_neg(ty->bit_cnt, val);
      return val;
    }
    eval_error2(node, "unimplemented cast");
    return NULL;
  case ND_NUM: {
    BitBuf *val = calloc(1, bitint_buffer_size(ty));
    memcpy(val, node->num.bitint_data, ty->size);
    return val;
  }
  }

  char *data = eval_constexpr_data(node);
  if (data) {
    size_t bsz = bitint_buffer_size(ty);
    if (is_bitfield(node)) {
      size_t fsz = (size_t)bitfield_footprint(node->m.member);
      if (fsz > bsz)
        bsz = fsz;
    }
    BitBuf *val = calloc(1, bsz);
    if (is_bitfield(node)) {
      memcpy(val, data, bitfield_footprint(node->m.member));
      eval_bitint_bitfield_load(ty->bit_cnt, val, val, node->m.member->bit_width,
                                node->m.member->bit_offset, ty->is_unsigned);
      return val;
    }
    memcpy(val, data, ty->size);
    return val;
  }

  return (void *)eval_error(node);
}

static void clean_bitint(BitBuf *val, Type *ty) {
  int64_t buf_bits = bitint_buffer_size(ty) * 8;
  if (ty->bit_cnt < buf_bits)
    eval_bitint_sign_ext(ty->bit_cnt, val, buf_bits, ty->is_unsigned);
}

static Node *atomic_op(NodeKind kind, Node *lhs, Node *rhs, Token *tok, bool return_old) {
  // ({
  //   T *addr = &obj; T old = *addr; T new;
  //   do {
  //    new = old op val;
  //   } while (!atomic_compare_exchange_strong(addr, &old, new));
  //
  //   return_old ? old : new;
  // })
  Node head = {0};
  Node *cur = &head;

  add_type(lhs);
  add_type(rhs);
  Obj *addr = new_lvar(pointer_to(lhs->ty));
  Obj *val = new_lvar(rhs->ty);
  Obj *old = new_lvar(lhs->ty);
  Obj *new = new_lvar(lhs->ty);

  cur = cur->next = new_unary(ND_EXPR_STMT,
                              new_binary(ND_ASSIGN, new_var_node(addr, tok),
                                         new_unary(ND_ADDR, lhs, tok), tok),
                              tok);

  cur = cur->next = new_unary(ND_EXPR_STMT,
                              new_binary(ND_ASSIGN, new_var_node(val, tok), rhs, tok), tok);

  cur = cur->next = new_unary(ND_EXPR_STMT,
                              new_binary(ND_ASSIGN, new_var_node(old, tok),
                                         new_unary(ND_DEREF, new_var_node(addr, tok), tok),
                                         tok),
                              tok);

  Node *loop = new_node(ND_DO, tok);
  loop->ctrl.then = new_unary(ND_EXPR_STMT,
                              new_binary(ND_ASSIGN, new_var_node(new, tok),
                                         new_binary(kind, new_var_node(old, tok),
                                                    new_var_node(val, tok), tok),
                                         tok),
                              tok);

  Node *cas = new_node(ND_CAS, tok);
  cas->cas.addr = new_var_node(addr, tok);
  cas->cas.old_val = new_unary(ND_ADDR, new_var_node(old, tok), tok);
  cas->cas.new_val = new_var_node(new, tok);

  loop->ctrl.cond = new_unary(ND_NOT, cas, tok);
  cur = cur->next = loop;

  if (return_old)
    cur = cur->next = new_unary(ND_EXPR_STMT, new_var_node(old, tok), tok);
  else
    cur = cur->next = new_unary(ND_EXPR_STMT, new_var_node(new, tok), tok);

  Node *node = new_node(ND_STMT_EXPR, tok);
  node->blk.body = head.next;
  node->blk.result = cur;
  return node;
}

static Node *nonvoid_ptr(Token **rest, Token *tok) {
  Node *node = assign(rest, tok);
  ptr_convert(&node);
  if (!node->ty->base || node->ty->base->kind == TY_VOID)
    error_tok(tok, "expected pointer to non-void type");
  return node;
}

static Node *atomic_builtin_op(Token **rest, Token *tok, bool return_old) {
  Token *start = tok;
  tok = skip_tk(tok->next, TK_LPAREN);
  Node *obj = new_unary(ND_DEREF, nonvoid_ptr(&tok, tok), start);
  tok = skip_tk(tok, TK_COMMA);
  Node *val = assign(&tok, tok);
  if (consume_tk(&tok, tok, TK_COMMA))
    ident_tok(&tok, tok);
  *rest = skip_tk(tok, TK_RPAREN);

  const char *loc = start->loc + 23;
  int len = start->len - 23;

  NodeKind kind;
  if (equal_substr(loc, len, "add") && addsub_overload(&obj, &val))
    kind = ND_ADD;
  else if (equal_substr(loc, len, "sub") && addsub_overload(&obj, &val))
    kind = ND_SUB;
  else if (equal_substr(loc, len, "and"))
    kind = ND_BITAND;
  else if (equal_substr(loc, len, "or"))
    kind = ND_BITOR;
  else if (equal_substr(loc, len, "xor"))
    kind = ND_BITXOR;
  else
    error_tok(start, "unsupported atomic op");

  return atomic_op(kind, obj, val, start, return_old);
}

static Node *arith_assign(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  if (lhs->ty->qual & Q_ATOMIC) {
    Node *node = atomic_op(kind, lhs, rhs, tok, false);
    node->is_nonlval = true;
    return node;
  }
  Node *node = new_binary(ND_ARITH_ASSIGN, lhs, rhs, tok);
  node->arith_kind = kind;
  node->is_nonlval = true;
  return node;
}

static Node *arith_assign_addsub(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  if (!addsub_overload(&lhs, &rhs))
    error_tok(rhs->tok, "invalid operand");

  return arith_assign(kind, lhs, rhs, tok);
}

static Node *new_inc_dec(Node *lhs, Token *tok, int addend) {
  if (!(lhs->ty->qual & Q_ATOMIC) &&
      lhs->ty->kind != TY_BOOL &&
      !is_bitfield(lhs) &&
      !is_flonum(lhs->ty)) {
    Type *ty = lhs->ty;
    Node *node = arith_assign_addsub(ND_ADD, lhs, new_num(addend, tok), tok);
    return new_cast(new_add(node, new_num(-addend, tok), tok), ty);
  }

  Node *rhs = new_num(addend, tok);
  if (!addsub_overload(&lhs, &rhs))
    error_tok(tok, "invalid operand");

  if (lhs->ty->qual & Q_ATOMIC)
    return atomic_op(ND_ADD, lhs, rhs, tok, true);

  return new_binary(ND_POST_INCDEC, lhs, rhs, tok);
}

static void chk_assignable(Node *node) {
  add_type(node);
  if (node->is_nonlval ||
      !is_lvalue_expr(node) ||
      (eff_qual(node->ty) & Q_CONST) ||
      node->ty->has_const_member ||
      is_decay_ty(node->ty))
    error_tok(node->tok, "operand unassignable");
}

static Node *assign_inner(Token **rest, Token *tok);

static Node *assign(Token **rest, Token *tok) {
  if (++parse_depth > 256)
    error_tok(tok, "expression nesting depth exceeded");
  Node *r = assign_inner(rest, tok);
  parse_depth--;
  return r;
}

static Node *assign_inner(Token **rest, Token *tok) {
  Node *node = conditional(&tok, tok);

  TokenKind tk = tok->kind;
  if (TK_EQ <= tk && tk <= TK_RANGLE2_EQ) {
    chk_assignable(node);

    Node *rhs = assign(rest, tok->next);

    switch (tk) {
    case TK_EQ:
      node = new_binary(ND_ASSIGN, node, rhs, tok);
      node->is_nonlval = true;
      return node;
    case TK_ADD_EQ:     return arith_assign_addsub(ND_ADD, node, rhs, tok);
    case TK_SUB_EQ:     return arith_assign_addsub(ND_SUB, node, rhs, tok);
    case TK_MUL_EQ:     return arith_assign(ND_MUL, node, rhs, tok);
    case TK_DIV_EQ:     return arith_assign(ND_DIV, node, rhs, tok);
    case TK_REM_EQ:     return arith_assign(ND_REM, node, rhs, tok);
    case TK_AND_EQ:     return arith_assign(ND_BITAND, node, rhs, tok);
    case TK_OR_EQ:      return arith_assign(ND_BITOR, node, rhs, tok);
    case TK_XOR_EQ:     return arith_assign(ND_BITXOR, node, rhs, tok);
    case TK_LANGLE2_EQ: return arith_assign(ND_SHL, node, rhs, tok);
    case TK_RANGLE2_EQ:
      return arith_assign(node->ty->is_unsigned ? ND_SHR : ND_SAR, node, rhs, tok);
    }
    internal_error();
  }

  *rest = tok;
  return node;
}

static Node *vla_cond_result_len2(Type *ty) {
  if (!ty->vla_len_expr)
    return NULL;
  if (ty->vla_len_val)
    return new_var_node(ty->vla_len_val, ty->vla_len_expr->tok);
  return ty->vla_len_expr;
}

Type *vla_cond_result_len(Type *ty1, Type *ty2, Type *base, Node **cond, Obj **cond_var) {
  if (is_const_context())
    return vla_of(base, new_unknown(ty_size_t, (*cond)->tok), 0);

  Node *len1 = vla_cond_result_len2(ty1);
  Node *len2 = vla_cond_result_len2(ty2);

  if (!len1 && !len2)
    return array_of(base, -1);
  if (!len1 + !len2 == 1)
    return vla_of(base, len1 ? len1 : len2, 0);

  if (!*cond_var) {
    int64_t val;
    if (is_const_expr(*cond, &val))
      return vla_of(base, val ? len1 : len2, 0);

    *cond_var = new_lvar2(ty_bool, base_scope());
    *cond = new_binary(ND_ASSIGN, new_var_node(*cond_var, (*cond)->tok), *cond,
                       (*cond)->tok);
    add_type(*cond);
  }
  Node *node = new_node(ND_COND, (*cond)->tok);
  node->ctrl.cond = new_var_node(*cond_var, (*cond)->tok);
  node->ctrl.then = len1;
  node->ctrl.els = len2;
  return vla_of(base, node, 0);
}

static Node *conditional_inner(Token **rest, Token *tok);

static Node *conditional(Token **rest, Token *tok) {
  if (++parse_depth > 256)
    error_tok(tok, "expression nesting depth exceeded");
  Node *r = conditional_inner(rest, tok);
  parse_depth--;
  return r;
}

static Node *conditional_inner(Token **rest, Token *tok) {
  Node *cond = binary(&tok, tok, PCD_LOGOR);

  if (tok->kind != TK_QMARK) {
    *rest = tok;
    return cond;
  }
  Node *node = new_node(ND_COND, tok);
  node->is_nonlval = true;

  if (!consume_tk(&tok, tok->next, TK_COLON)) {
    node->ctrl.then = expression(&tok, tok->next);
    tok = skip_tk(tok, TK_COLON);
  } else if (is_iso_std && !tok->file->is_syshdr) {
    error_tok(tok, "GNU omitted conditional operand is not allowed in ISO C");
  }
  node->ctrl.els = conditional(rest, tok);

  if (node->ctrl.then) {
    node->ctrl.cond = cond_cast(cond);
    return node;
  }
  int64_t val;
  Node n = *cond;
  if (is_const_expr(cond_cast(&n), &val)) {
    node->ctrl.cond = new_boolean(!!val, cond->tok);
    node->ctrl.then = cond;
    return node;
  }
  ptr_convert(&cond);
  enter_tmp_scope();
  Obj *var = new_lvar(cond->ty);
  node->ctrl.cond = cond_cast(new_binary(ND_ASSIGN, new_var_node(var, cond->tok), cond,
                                         tok));
  node->ctrl.then = new_var_node(var, cond->tok);
  leave_scope();
  return node;
}

static Node *binary(Token **rest, Token *tok, Preced stop) {
  Node *node = unary(&tok, tok);

  for (;;) {
    Token *start = tok;
    if (tok->kind == TK_MUL) {
      node = new_binary(ND_MUL, node, unary(&tok, tok->next), start);
      continue;
    }
    if (tok->kind == TK_DIV) {
      node = new_binary(ND_DIV, node, unary(&tok, tok->next), start);
      continue;
    }
    if (tok->kind == TK_REM) {
      node = new_binary(ND_REM, node, unary(&tok, tok->next), start);
      continue;
    }
    if (stop == PCD_MUL)
      return *rest = tok, node;
    break;
  }

  for (;;) {
    Token *start = tok;
    if (tok->kind == TK_ADD) {
      node = new_add(node, binary(&tok, tok->next, PCD_MUL), start);
      continue;
    }
    if (tok->kind == TK_SUB) {
      node = new_sub(node, binary(&tok, tok->next, PCD_MUL), start);
      continue;
    }
    if (stop == PCD_ADD)
      return *rest = tok, node;
    break;
  }

  for (;;) {
    Token *start = tok;
    if (tok->kind == TK_LANGLE2) {
      node = new_binary(ND_SHL, node, binary(&tok, tok->next, PCD_ADD), start);
      continue;
    }
    if (tok->kind == TK_RANGLE2) {
      add_type(node);
      if (node->ty->is_unsigned)
        node = new_binary(ND_SHR, node, binary(&tok, tok->next, PCD_ADD), start);
      else
        node = new_binary(ND_SAR, node, binary(&tok, tok->next, PCD_ADD), start);
      continue;
    }
    if (stop == PCD_SHFT)
      return *rest = tok, node;
    break;
  }

  for (;;) {
    Token *start = tok;
    if (tok->kind == TK_LANGLE) {
      node = new_binary(ND_LT, node, binary(&tok, tok->next, PCD_SHFT), start);
      continue;
    }
    if (tok->kind == TK_LANGLE_EQ) {
      node = new_binary(ND_LE, node, binary(&tok, tok->next, PCD_SHFT), start);
      continue;
    }
    if (tok->kind == TK_RANGLE) {
      node = new_binary(ND_GT, node, binary(&tok, tok->next, PCD_SHFT), start);
      continue;
    }
    if (tok->kind == TK_RANGLE_EQ) {
      node = new_binary(ND_GE, node, binary(&tok, tok->next, PCD_SHFT), start);
      continue;
    }
    if (stop == PCD_CMP)
      return *rest = tok, node;
    break;
  }

  for (;;) {
    Token *start = tok;
    if (tok->kind == TK_EQ2) {
      node = new_binary(ND_EQ, node, binary(&tok, tok->next, PCD_CMP), start);
      continue;
    }
    if (tok->kind == TK_NOT_EQ) {
      node = new_binary(ND_NE, node, binary(&tok, tok->next, PCD_CMP), start);
      continue;
    }
    if (stop == PCD_EQ)
      return *rest = tok, node;
    break;
  }

  for (;;) {
    Token *start = tok;
    if (tok->kind == TK_AND) {
      node = new_binary(ND_BITAND, node, binary(&tok, tok->next, PCD_EQ), start);
      continue;
    }
    if (stop == PCD_BITAND)
      return *rest = tok, node;
    break;
  }

  for (;;) {
    Token *start = tok;
    if (tok->kind == TK_XOR) {
      node = new_binary(ND_BITXOR, node, binary(&tok, tok->next, PCD_BITAND), start);
      continue;
    }
    if (stop == PCD_XOR)
      return *rest = tok, node;
    break;
  }

  for (;;) {
    Token *start = tok;
    if (tok->kind == TK_OR) {
      node = new_binary(ND_BITOR, node, binary(&tok, tok->next, PCD_XOR), start);
      continue;
    }
    if (stop == PCD_BITOR)
      return *rest = tok, node;
    break;
  }

  for (;;) {
    Token *start = tok;
    if (tok->kind == TK_AND2) {
      node = new_binary(ND_LOGAND, cond_cast(node),
                        cond_cast(binary(&tok, tok->next, PCD_BITOR)), start);
      continue;
    }
    if (stop == PCD_LOGAND)
      return *rest = tok, node;
    break;
  }

  for (;;) {
    Token *start = tok;
    if (tok->kind == TK_OR2) {
      node = new_binary(ND_LOGOR, cond_cast(node),
                        cond_cast(binary(&tok, tok->next, PCD_LOGAND)), start);
      continue;
    }
    break;
  }
  *rest = tok;
  return node;
}

static bool addsub_overload(Node **lhs, Node **rhs) {
  ptr_convert(lhs);
  ptr_convert(rhs);

  if (is_numeric((*lhs)->ty) && is_numeric((*rhs)->ty))
    return true;

  if ((*lhs)->ty->base && int_or_trunc_bitint(rhs, false)) {
    Node *sz = ptr_base_size((*lhs)->ty, (*lhs)->tok);
    *rhs = new_binary(ND_MUL, sz, *rhs, (*rhs)->tok);
    return true;
  }

  return false;
}

static Node *new_add(Node *lhs, Node *rhs, Token *tok) {
  if (addsub_overload(&lhs, &rhs))
    return new_binary(ND_ADD, lhs, rhs, tok);

  if (int_or_trunc_bitint(&lhs, false) && rhs->ty->base) {
    Node *sz = ptr_base_size(rhs->ty, tok);
    if (rhs->ty->base->kind == TY_VLA) {
      lhs = new_binary(ND_MUL, sz, lhs, tok);
      return new_binary(ND_ADD, rhs, lhs, tok);
    }
    lhs = new_binary(ND_MUL, lhs, sz, tok);
    return new_binary(ND_ADD, lhs, rhs, tok);
  }

  error_tok(tok, "invalid operands");
}

static Node *new_sub(Node *lhs, Node *rhs, Token *tok) {
  if (addsub_overload(&lhs, &rhs))
    return new_binary(ND_SUB, lhs, rhs, tok);

  if (lhs->ty->base && rhs->ty->base &&
      lhs->ty->base->size >= 0 && rhs->ty->base->size >= 0 &&
      is_compatible_unqual_ptr_target(lhs->ty->base, rhs->ty->base)) {
    Node *sz = new_cast(ptr_base_size(lhs->ty, tok), ty_ptrdiff_t);
    lhs = new_cast(lhs, ty_ptrdiff_t);
    rhs = new_cast(rhs, ty_ptrdiff_t);
    return new_binary(ND_DIV, new_binary(ND_SUB, lhs, rhs, tok), sz, tok);
  }

  error_tok(tok, "invalid operands");
}

static Type *sizeof_arg(Token **rest, Token *tok, Node **expr) {
  Type *ty;
  VarAttr attr = {0};
  if (is_typename_paren(rest, tok, &ty, &attr)) {
    *expr = attr.typeof_vm_expr;
    return ty;
  }
  uneval_depth++;
  *expr = unary(rest, tok);
  uneval_depth--;
  add_type(*expr);
  return (*expr)->ty;
}

static bool is_lvalue_expr(Node *n) {
  if (!n || n->is_nonlval)
    return false;
  switch (n->kind) {
  case ND_VAR:
  case ND_DEREF:
    return true;
  case ND_MEMBER:
    return is_lvalue_expr(n->m.lhs);
  case ND_CHAIN:
    return is_lvalue_expr(n->m.rhs);
  default:
    return false;
  }
}

static Node *unary_inner(Token **rest, Token *tok);

static Node *unary(Token **rest, Token *tok) {
  if (++parse_depth > 256)
    error_tok(tok, "expression nesting depth exceeded");
  Node *r = unary_inner(rest, tok);
  parse_depth--;
  return r;
}

static Node *unary_inner(Token **rest, Token *tok) {
  if (tok->kind == TK_ADD)
    return new_unary(ND_POS, unary(rest, tok->next), tok);

  if (tok->kind == TK_SUB)
    return new_unary(ND_NEG, unary(rest, tok->next), tok);

  if (tok->kind == TK_AND) {
    Node *lhs = unary(rest, tok->next);
    if (lhs->kind == ND_DEREF) {
      add_type(lhs->m.lhs);
      if (lhs->m.lhs->ty->kind == TY_PTR && lhs->m.lhs->ty->base->kind == TY_VOID) {
        Node *r = new_cast(lhs->m.lhs, lhs->m.lhs->ty);
        r->is_nonlval = true;
        return r;
      }
    }
    add_type(lhs);
    if (lhs->ty->kind != TY_FUNC && !is_lvalue_expr(lhs))
      error_tok(tok, "cannot take address of non-lvalue");
    if (is_bitfield(lhs))
      error_tok(tok, "cannot take address of bitfield");
    Node *base = lhs;
    while (base->kind == ND_MEMBER)
      base = base->m.lhs;
    if (base->kind == ND_VAR && base->m.var->is_register)
      error_tok(tok, "cannot take address of register variable");
    return new_unary(ND_ADDR, lhs, tok);
  }

  if (tok->kind == TK_MUL) {
    // [https://www.sigbus.info/n1570#6.5.3.2p4] This is an oddity
    // in the C spec, but dereferencing a function shouldn't do
    // anything. If foo is a function, `*foo`, `**foo` or `*****foo`
    // are all equivalent to just `foo`.
    Node *node = unary(rest, tok->next);
    add_type(node);
    if (node->ty->kind == TY_FUNC)
      return node;
    return new_unary(ND_DEREF, node, tok);
  }

  if (tok->kind == TK_NOT)
    return new_unary(ND_NOT, cond_cast(unary(rest, tok->next)), tok);

  if (tok->kind == TK_BITNOT)
    return new_unary(ND_BITNOT, unary(rest, tok->next), tok);

  // Read ++i as i+=1
  if (tok->kind == TK_ADD2) {
    Node *node = unary(rest, tok->next);
    chk_assignable(node);
    return arith_assign_addsub(ND_ADD, node, new_num(1, tok), tok);
  }

  // Read --i as i-=1
  if (tok->kind == TK_SUB2) {
    Node *node = unary(rest, tok->next);
    chk_assignable(node);
    return arith_assign_addsub(ND_SUB, node, new_num(1, tok), tok);
  }

  // [GNU] labels-as-values
  if (tok->kind == TK_AND2)
    error_tok(tok, "labels-as-values is not supported by PIR backend");

  if (tok->kind == TK_alignof) {
    Type *ty;
    VarAttr attr = {0};
    int attr_align = 0;
    if (is_typename_paren(rest, tok->next, &ty, &attr)) {
      attr_align = attr.align;
    } else {
      if (is_iso_std && !tok->file->is_syshdr &&
          (equal(tok, "_Alignof") || equal(tok, "alignof")))
        error_tok(tok, "'_Alignof' applied to an expression is a GNU extension");
      Node *node = unary(rest, tok->next);
      switch (node->kind) {
      case ND_MEMBER:
        return new_size_t(MAX(node->m.member->ty->align, node->m.member->alt_align), tok);
      case ND_VAR:
        return new_size_t(node->m.var->alt_align ? node->m.var->alt_align
                                                 : node->m.var->ty->align,
                          tok);
      }
      add_type(node);
      ty = node->ty;
    }
    if (ty->kind == TY_VOID || ty->kind == TY_FUNC ||
        (ty->kind != TY_VLA && ty->size < 0))
      error_tok(tok, "alignof applied to invalid or incomplete type");
    ty = get_elem(ty);
    if (ty->size < 0)
      error_tok(tok, "alignof applied to incomplete type");
    return new_size_t(MAX(ty->align, attr_align), tok);
  }

  if (tok->kind == TK_Countof) {
    if (is_iso_std && opt_std < STD_C23 && !tok->file->is_syshdr)
      error_tok(tok, "'_Countof' is a C2y extension");
    Node *expr = NULL;
    Type *ty = sizeof_arg(rest, tok->next, &expr);

    if (ty->kind == TY_VLA) {
      if (!ty->vla_len_expr)
        expr = NULL;
      chain_expr(&expr, vla_count(ty, tok));
      return expr;
    }
    if (ty->kind == TY_ARRAY) {
      if (ty->size < 0)
        error_tok(tok, "countof applied to incomplete array");
      return new_size_t(ty->array_len, tok);
    }
    error_tok(tok, "countof applied to non-array type");
  }

  if (tok->kind == TK_sizeof) {
    Node *expr = NULL;
    Type *ty = sizeof_arg(rest, tok->next, &expr);

    if (expr && is_bitfield(expr))
      error_tok(tok, "sizeof applied to bitfield");
    if (is_iso_std && (ty->kind == TY_VOID || ty->kind == TY_FUNC))
      error_tok(tok, "invalid application of 'sizeof' to void or function type");
    if (ty->kind == TY_VLA) {
      chain_expr(&expr, vla_size(ty, tok));
      return expr;
    }
    if (ty->is_flexible && ty->origin)
      ty = ty->origin;
    if (ty->size < 0)
      error_tok(tok, "sizeof applied to incomplete type");
    return new_size_t(ty->size, tok);
  }

  if (tok->kind == TK_static_assert) {
    if (is_iso_std && !tok->file->is_syshdr)
      error_tok(tok, "_Static_assert is not allowed in an expression");
    eval_static_assert(rest, tok->next);
    return new_node(ND_NULL_EXPR, tok);
  }

  if (tok->kind == TK_LPAREN) {
    tok = tok->next;

    if (is_typename(tok)) {
      Type *ty;
      VarAttr attr = {0};
      if (is_typename_paren2(&tok, tok, &ty, &attr)) {
        // Casts
        Node *calc = calc_vla2(ty, tok, &attr);
        Node *node = new_cast(unary(rest, tok), ty);
        node->is_nonlval = true;
        if (calc)
          return new_binary(ND_COMMA, calc, node, tok);
        return node;
      }

      Node *node = compound_literal(&tok, tok);
      return postfix(node, rest, tok);
    }

    if (tok->kind == TK_LCURLY) {
      if (is_iso_std && !tok->file->is_syshdr)
        error_tok(tok, "GNU statement expression is not allowed in ISO C");
      if (!fnctx)
        error_tok(tok, "statement expression not in a function");

      Node *node = compound_stmt(&tok, tok->next, ND_STMT_EXPR);
      return postfix(node, rest, skip_tk(tok, TK_RPAREN));
    }

    Node *node = expression(&tok, tok);
    return postfix(node, rest, skip_tk(tok, TK_RPAREN));
  }

  Node *node = primary(&tok, tok);
  return postfix(node, rest, tok);
}

static void chk_mem_name2(HashMap *map, Token *name) {
  HashEntry *ent = hashmap_get_or_insert(map, name->loc, name->len);
  Token *prv = ent->val;
  if (prv) {
    notice_tok(name, "duplicated member name");
    error_tok(prv, "previously declared here");
  }
  ent->val = name;
}

static void chk_mem_name(HashMap *map, Member *members) {
  for (Member *mem = members; mem; mem = mem->next) {
    if (mem->name)
      chk_mem_name2(map, mem->name);
    else if (mem->ty->kind == TY_STRUCT || mem->ty->kind == TY_UNION)
      chk_mem_name(map, mem->ty->members);
  }
}

static bool chk_bitfield_width(int64_t width, Member *mem) {
  if (width < 0)
    return true;
  if (width == 0)
    return mem->name;
  return width > bit_size(mem->ty);
}

static bool member_has_named(Member *m) {
  if (m->name)
    return true;
  if (!m->is_bitfield && (m->ty->kind == TY_STRUCT || m->ty->kind == TY_UNION)) {
    for (Member *sub = m->ty->members; sub; sub = sub->next)
      if (member_has_named(sub))
        return true;
  }
  return false;
}

static void struct_members(Token **rest, Token *tok, Type *ty) {
  Member head = {0};
  Member *cur = &head;
  Token *flex_tok = NULL;
  HashMap names = {0};
  bool has_const = false;

  while (tok->kind != TK_RCURLY) {
    if (tok->kind == TK_SEMI) {
      if (is_iso_std && !tok->file->is_syshdr)
        error_tok(tok, "extra ';' inside struct or union is not allowed in ISO C");
      tok = tok->next;
      continue;
    }
    if (tok->kind == TK_PRAGMA) {
      pragma_pack(&tok, tok);
      continue;
    }
    if (tok->kind == TK_static_assert) {
      eval_static_assert(&tok, tok->next);
      tok = skip_tk(tok, TK_SEMI);
      continue;
    }

    VarAttr attr = {0};
    Token *decl_start = tok;
    Type *basety = declspec(&tok, tok, &attr, SC_NONE);
    validate_decl_attrs(&attr, DECL_CTX_MEMBER, decl_start);

    // Anonymous struct member
    if (tok->kind == TK_SEMI && (basety->kind == TY_STRUCT || basety->kind == TY_UNION)) {
      if (basety->size < 0)
        error_tok(tok, "member has incomplete type");
      if (is_iso_std && ty->kind != TY_UNION && basety->is_flexible && !tok->file->is_syshdr)
        error_tok(tok, "struct with flexible array member cannot be a member in ISO C");
      bool has_struct_kw = false;
      for (Token *t = decl_start; t && t != tok; t = t->next) {
        if (t->kind == TK_struct || t->kind == TK_union) {
          has_struct_kw = true;
          break;
        }
      }
      if ((basety->tag || (is_iso_std && !has_struct_kw && !tok->file->is_syshdr)) &&
          !opt_ms_anon_struct)
        error_tok(tok,
                  "enable MSVC anonymous struct extension with `-fms-anonymous-structs`");
      chk_mem_name(&names, basety->members);

      Member *mem = calloc(1, sizeof(Member));
      mem->ty = basety;
      chk_alignas_natural(&attr, mem->ty, tok);
      aligned_attr(NULL, tok, &attr, &mem->alt_align);
      mem_attr(NULL, tok, &attr, mem);

      tok = tok->next;
      cur = cur->next = mem;
      has_const |= (eff_qual(mem->ty) & Q_CONST) | mem->ty->has_const_member;
      continue;
    }

    if (tok->kind == TK_SEMI) {
      if (is_iso_std && (!tok->file || !tok->file->is_syshdr))
        error_tok(decl_start, "declaration does not declare anything");
      tok = tok->next;
      continue;
    }

    // Regular struct members
    bool first = true;
    for (; comma_list(&tok, &tok, TK_SEMI, !first); first = false) {
      Member *mem = calloc(1, sizeof(Member));
      mem->ty = declarator(&tok, tok, basety, &mem->name);
      if (mem->name) {
        chk_mem_name2(&names, mem->name);
        mem->name->is_live = true;
      }
      if (is_vm_ty(mem->ty) || mem->ty->kind == TY_FUNC || mem->ty->kind == TY_VOID)
        error_tok(tok, "invalid member type");
      if (is_iso_std && ty->kind != TY_UNION && mem->ty->is_flexible && !tok->file->is_syshdr)
        error_tok(tok, "struct with flexible array member cannot be a member in ISO C");

      if (consume_tk(&tok, tok, TK_COLON)) {
        if (!is_int_class(mem->ty))
          error_tok(tok, "bit-field not integer");
        if (mem->ty->qual & Q_ATOMIC)
          error_tok(tok, "bit-field cannot have '_Atomic' type");

        int64_t width = const_expr(&tok, tok);
        if (chk_bitfield_width(width, mem))
          error_tok(tok, "invalid bit-field width");

        mem->bit_width = width;
        mem->is_bitfield = true;
      } else if (!mem->name) {
        error_tok(tok, "expected member name");
      }

      if (mem->is_bitfield && attr.has_alignas)
        error_tok(tok, "_Alignas cannot be applied to a bit-field");
      chk_alignas_natural(&attr, mem->ty, tok);
      aligned_attr(mem->name, tok, &attr, &mem->alt_align);
      mem_attr(mem->name, tok, &attr, mem);
      cur = cur->next = mem;

      Type *elemty = get_elem(mem->ty);
      has_const |= (elemty->qual & Q_CONST) | elemty->has_const_member;

      if (mem->ty->size < 0) {
        if (mem->ty->kind == TY_ARRAY && ty->kind == TY_STRUCT && !flex_tok) {
          flex_tok = tok;
          continue;
        }
        error_tok(flex_tok ? flex_tok : tok, "member has incomplete type");
      }
    }
  }
  free(names.buckets);
  *rest = tok->next;

  if (flex_tok) {
    if (cur->ty->size >= 0)
      error_tok(flex_tok, "member has incomplete type");
    if (is_iso_std) {
      bool has_named = false;
      for (Member *m = head.next; m != cur; m = m->next) {
        if (member_has_named(m)) {
          has_named = true;
          break;
        }
      }
      if (!has_named)
        error_tok(flex_tok, "flexible array member in struct with no named members");
    }
    ty->is_flexible = true;
  }
  if (is_iso_std && !tok->file->is_syshdr) {
    if (!head.next)
      error_tok(tok, "empty struct/union is not allowed in ISO C");
    bool has_named = false;
    for (Member *m = head.next; m; m = m->next) {
      if (member_has_named(m)) {
        has_named = true;
        break;
      }
    }
    if (!has_named)
      error_tok(tok, "struct/union with no named members is not allowed in ISO C");
  }
  ty->members = head.next;
  ty->has_const_member = has_const;
}

static Type *struct_ty_tag(TypeKind kind, Token *tag, Type *tag_ty) {
  if (tag_ty) {
    if (tag_ty->kind != kind)
      error_tok(tag, "mismatched tag type");
    return tag_ty;
  }

  Type *ty = new_type(kind, -1, 0);

  if (tag)
    push_tag_scope(tag, ty);
  return ty;
}

static Type *struct_union_decl(Token **rest, Token *tok, TypeKind kind) {
  bool is_packed = false;
  bool_attr(tok, TK_ATTR, "packed", &is_packed);
  bool_attr(tok, TK_BATTR, "packed", &is_packed);

  int alt_align = 0;
  attr_aligned(tok, TK_ATTR, &alt_align);
  attr_aligned(tok, TK_BATTR, &alt_align);

  Token *tag = NULL;
  if (tok->kind == TK_IDENT) {
    tag = tok;
    tok = tok->next;
  }

  Type *tag_ty = NULL;
  if (tag) {
    if (tok->kind != TK_LCURLY) {
      if (tok->kind == TK_SEMI)
        tag_ty = find_tag_in_scope(tag);
      else
        tag_ty = find_tag(tag);

      *rest = tok;
      return struct_ty_tag(kind, tag, tag_ty);
    }
    tag_ty = find_tag_in_scope(tag);
  }

  tok = skip_tk(tok, TK_LCURLY);

  bool is_redecl = false;
  if (tag_ty) {
    if (tag_ty->is_constructing)
      error_tok(tag, "nested redefinition of '%.*s'", tag->len, tag->loc);

    if (tag_ty->size >= 0) {
      if (opt_std < STD_C23)
        error_tok(tag, "redefinition of '%.*s'", tag->len, tag->loc);
      is_redecl = true;
    }
  }

  Type *ty;
  if (is_redecl) {
    ty = new_type(kind, -1, 0);
  } else {
    ty = struct_ty_tag(kind, tag, tag_ty);
    if (tag)
      ty->is_constructing = true;
  }

  /* Snapshot #pragma pack state BEFORE parsing body so directives inside the
   * struct/union definition do not retroactively alter this type's layout. */
  int cur_pack_align = pack_stk.cnt ? pack_stk.data[pack_stk.cnt - 1].val : 0;

  struct_members(&tok, tok, ty);

  attr_aligned(tok, TK_ATTR, &alt_align);
  bool_attr(tok, TK_ATTR, "packed", &is_packed);
  *rest = tok;

  int pack_align = is_packed ? 1 : cur_pack_align;

  if (kind == TY_STRUCT)
    ty = struct_decl(ty, alt_align, pack_align);
  else
    ty = union_decl(ty, alt_align, pack_align);

  if (is_redecl) {
    if (!is_record_compat(ty, tag_ty, true))
      error_tok(tag, "struct/union redeclaration mismatch");
    return tag_ty;
  }

  if (tag)
    ty->is_constructing = false;

  for (Type *t = ty->decl_next; t;) {
    Type *t2 = t;
    t = t->decl_next;

    int align = t2->align;
    new_derived_type(t2, t2->qual, ty);
    t2->align = MAX(t2->align, align);
    if ((t2->qual & Q_ATOMIC) &&
        (t2->size == 2 || t2->size == 4 || t2->size == 8 || t2->size == 16) &&
        t2->align < t2->size)
      t2->align = (int32_t)t2->size;
  }
  return ty;
}

static int64_t member_align(Member *mem, int pack_align) {
  if (mem->is_packed)
    return 1;
  if (pack_align > 0 && pack_align < mem->ty->align)
    return pack_align;
  return mem->ty->align;
}

static int64_t checked_align_to(int64_t n, int64_t align) {
  if (align <= 1)
    return n;
  if (n > INT64_MAX - (align - 1))
    error("struct size is too large");
  return align_to(n, align);
}

static int64_t member_alt_align(Member *mem, int pack_align) {
  int64_t alt = mem->alt_align;
  if (pack_align > 0 && alt > pack_align)
    alt = pack_align;
  return alt;
}

static Type *struct_decl(Type *ty, int align, int pack_align) {
  int64_t bits = 0;

  for (Member *mem = ty->members; mem; mem = mem->next) {
    int64_t mem_align = member_align(mem, pack_align);
    int64_t eff_alt = member_alt_align(mem, pack_align);
    int64_t eff_align = MAX(mem_align, eff_alt);

    if (!mem->is_bitfield || mem->name)
      align = MAX(align, eff_align);

    if (mem->is_bitfield) {
      if (eff_alt > 0)
        bits = checked_align_to(bits, eff_alt * 8);
      if (mem->bit_width == 0) {
        bits = checked_align_to(bits, mem->ty->size * 8);
        continue;
      }
      if (bits > INT64_MAX - mem->bit_width)
        error("struct size is too large");
      if (mem->is_packed || pack_align) {
        int64_t ceil = checked_align_to(bits + mem->bit_width, 8);
        for (int64_t rsz = 8; rsz <= 64; rsz *= 2) {
          if (ceil % rsz != 0)
            break;
          int64_t mofs = (ceil - rsz) / 8;
          if (mofs * 8 > bits)
            continue;
          mem->offset = mofs;
          mem->is_aligned_bitfield = true;
          break;
        }
        if (!mem->is_aligned_bitfield)
          mem->offset = bits / 8;
      } else {
        int64_t bsz = mem->ty->size * 8;
        if (bits / bsz != (bits + mem->bit_width - 1) / bsz)
          bits = checked_align_to(bits, bsz);
        if (bits > INT64_MAX - mem->bit_width)
          error("struct size is too large");
        mem->offset = bits / bsz * mem->ty->size;
        mem->is_aligned_bitfield = true;
      }

      mem->bit_offset = bits - mem->offset * 8;
      bits += mem->bit_width;
      continue;
    }
    bits = checked_align_to(bits, eff_align * 8);
    mem->offset = bits / 8;

    if (mem->ty->size < 0) {
      if (!mem->next && ty->is_flexible)
        break;
      internal_error();
    }
    if (mem->ty->size > (INT64_MAX - bits) / 8)
      error("struct size is too large");
    bits += mem->ty->size * 8;
  }

  if (!align && pack_align > 0) {
    ty->align = 1;
    ty->size = checked_align_to(bits, 8) / 8;
  } else {
    ty->align = MAX(align, 1);
    int64_t al_bits = (int64_t)ty->align * 8;
    ty->size = checked_align_to(bits, al_bits) / 8;
  }
  return ty;
}

static Type *union_decl(Type *ty, int align, int pack_align) {
  for (Member *mem = ty->members; mem; mem = mem->next) {
    int64_t mem_align = member_align(mem, pack_align);
    int64_t eff_alt = member_alt_align(mem, pack_align);
    int64_t eff_align = MAX(mem_align, eff_alt);

    if (!mem->is_bitfield || mem->name)
      align = MAX(align, eff_align);
    if (mem->ty->is_flexible)
      ty->is_flexible = true;
    int64_t sz;
    if (mem->is_bitfield)
      sz = checked_align_to(mem->bit_width, 8) / 8;
    else
      sz = mem->ty->size;
    ty->size = MAX(ty->size, sz);
  }
  ty->align = MAX(align, 1);
  ty->size = checked_align_to(MAX(ty->size, 0), ty->align);
  return ty;
}

static Member *get_struct_member(Type *ty, Token *tok) {
  for (Member *mem = ty->members; mem; mem = mem->next) {
    if (mem->name) {
      if (equal_tok(mem->name, tok))
        return mem;
    } else {
      if ((mem->ty->kind == TY_STRUCT || mem->ty->kind == TY_UNION) &&
          get_struct_member(mem->ty, tok))
        return mem;
    }
  }
  return NULL;
}

static Node *struct_ref(Node *node, Token *tok) {
  add_type(node);
  if (node->ty->kind != TY_STRUCT && node->ty->kind != TY_UNION)
    error_tok(node->tok, "not a struct nor a union");
  if (is_iso_std && (node->ty->qual & Q_ATOMIC) && !tok->file->is_syshdr)
    error_tok(tok, "cannot access member of '_Atomic' struct or union");

  Type *ty = node->ty;
  bool is_nonlval = node->is_nonlval;

  for (;;) {
    Member *mem = get_struct_member(ty, tok);
    if (!mem)
      error_tok(tok, "no such member");
    node = new_unary(ND_MEMBER, node, tok);
    node->m.member = mem;

    ty = node->ty = add_qual(ty->qual & (Q_CONST | Q_VOLATILE), mem->ty, NULL);

    if (mem->name)
      break;
  }
  node->is_nonlval = is_nonlval;
  return node;
}

static Node *postfix(Node *node, Token **rest, Token *tok) {
  for (;;) {
    if (tok->kind == TK_LPAREN) {
      node = funcall(&tok, tok->next, node);
      continue;
    }

    if (tok->kind == TK_LBRACK) {
      // x[y] is short for *(x+y)
      Token *start = tok;
      Node *idx = expression(&tok, tok->next);
      tok = skip_tk(tok, TK_RBRACK);
      node = new_unary(ND_DEREF, new_add(node, idx, start), start);
      continue;
    }

    if (tok->kind == TK_DOT) {
      node = struct_ref(node, ident_tok(&tok, tok->next));
      continue;
    }

    if (tok->kind == TK_ARROW) {
      // x->y is short for (*x).y
      node = new_unary(ND_DEREF, node, tok);
      node = struct_ref(node, ident_tok(&tok, tok->next));
      continue;
    }

    if (tok->kind == TK_ADD2) {
      chk_assignable(node);
      node = new_inc_dec(node, tok, 1);
      tok = tok->next;
      continue;
    }

    if (tok->kind == TK_SUB2) {
      chk_assignable(node);
      node = new_inc_dec(node, tok, -1);
      tok = tok->next;
      continue;
    }

    *rest = tok;
    return node;
  }
}

static Node *funcall(Token **rest, Token *tok, Node *fn) {
  Type *ty = get_func_ty(fn);

  if (fnctx && fn->kind == ND_VAR && fn->m.var->returns_twice)
    fnctx->fn->dont_reuse_stk = true;

  Obj *param = ty->is_oldstyle ? NULL : ty->param_list;

  Obj head = {0};
  Obj *cur = &head;

  enter_tmp_scope();

  while (comma_list(rest, &tok, TK_RPAREN, cur != &head)) {
    Node *arg = assign(&tok, tok);
    add_type(arg);

    bool is_fixed = (param != NULL);
    if (param) {
      arg = assign_cast(param->ty, arg);

      param = param->param_next;
    } else {
      if (!ty->is_variadic && !ty->is_oldstyle)
        error_tok(tok, "too many arguments");
      if (arg->ty->kind == TY_VOID || (arg->ty->size < 0 && !is_array(arg->ty)))
        error_tok(arg->tok, "invalid argument of void or incomplete type");

      if (is_integer(arg->ty))
        int_promotion(&arg);
      else if (arg->ty->kind == TY_FLOAT)
        arg = new_cast(arg, ty_double);
      else
        ptr_convert(&arg);
    }
    cur = cur->param_next = alloc_ast_var(is_fixed ? arg->ty : lval_conv_type(arg->ty));
    cur->arg_expr = arg;
  }
  if (param)
    error_tok(tok, "too few arguments");

  Node *node = new_node(ND_FUNCALL, tok);
  node->call.expr = fn;
  node->ty = unqual(ty->return_ty);
  node->call.args = head.param_next;
  node->is_nonlval = true;

  leave_scope();

  // If a function returns a struct, it is caller's responsibility
  // to allocate a space for the return value.
  if (node->ty->kind == TY_STRUCT ||
      node->ty->kind == TY_UNION ||
      (node->ty->kind == TY_BITINT && bitint_rtn_need_copy(node->ty->bit_cnt)))
    node->call.rtn_buf = new_lvar((ty->return_ty->qual & Q_ATOMIC) ? ty->return_ty : node->ty);
  return node;
}

static bool is_variably_modified(Type *ty) {
  for (; ty; ty = ty->base) {
    if (ty->kind == TY_VLA)
      return true;
    if (ty->kind == TY_FUNC) {
      if (is_variably_modified(ty->return_ty))
        return true;
      for (Obj *p = ty->param_list; p; p = p->param_next)
        if (is_variably_modified(p->ty))
          return true;
      return false;
    }
    if (ty->kind != TY_PTR && ty->kind != TY_ARRAY)
      break;
  }
  return false;
}

static Node *generic_selection(Token **rest, Token *tok) {
  Token *start = tok;
  tok = skip_tk(tok, TK_LPAREN);

  Type *t1;
  if (opt_std >= STD_C23 && is_typename(tok)) {
    t1 = typename(&tok, tok);
  } else {
    uneval_depth++;
    void_deref_ok_depth++;
    Node *ctrl = assign(&tok, tok);
    add_type(ctrl);
    void_deref_ok_depth--;
    uneval_depth--;
    chk_not_register_array(ctrl);
    t1 = unqual(ptr_decay(ctrl->ty));
  }
  Node *ret = NULL;
  Node *def = NULL;
  bool has_def = false;

  struct {
    Type **data;
    int32_t cap;
    int32_t len;
  } types = {0};

  while (comma_list(rest, &tok, TK_RPAREN, true)) {
    if (tok->kind == TK_default) {
      if (has_def)
        error_tok(tok, "duplicate default generic association");
      has_def = true;

      /* Parse default: in lexical order (exactly once) so tag/enum declarations
       * enter scope at their lexical position (C17 6.2.1p7). */
      bool spec_uneval = (ret != NULL) || (is_iso_std && fnctx && is_const_context());
      if (spec_uneval)
        uneval_depth++;
      def = assign(&tok, skip_tk(tok->next, TK_COLON));
      add_type(def);
      if (spec_uneval)
        uneval_depth--;
      continue;
    }

    Token *ty_tok = tok;
    VarAttr attr = {0};
    Type *basety = declspec(&tok, tok, &attr, SC_NONE);
    validate_decl_attrs(&attr, DECL_CTX_TYPENAME, ty_tok);
    Type *t2 = declarator2(&tok, tok, basety, NULL, true);
    declarator3(basety, t2, true, false);

    if (t2->kind == TY_VOID || t2->kind == TY_FUNC || t2->size < 0 ||
        is_vm_ty(t2))
      error_tok(ty_tok, "'_Generic' association type must be a complete object type");

    for (int32_t i = 0; i < types.len; i++)
      if (is_compatible2(types.data[i], t2))
        error_tok(ty_tok, "'_Generic' association compatible with a previous type");

    if (types.len == types.cap)
      GrowArr(types.data, &types.cap);
    types.data[types.len++] = t2;

    bool matched = is_compatible2(t1, t2);
    if (!matched)
      uneval_depth++;
    Node *node = assign(&tok, skip_tk(tok, TK_COLON));
    add_type(node);
    if (!matched)
      uneval_depth--;
    if (matched) {
      if (ret)
        error_tok(start, "controlling expression compatible with multiple generic associations");
      ret = node;
    }
  }

  free(types.data);

  if (ret)
    return ret;
  if (def)
    return def;
  error_tok(start, "controlling type not compatible with any association type");
}

static Node *checked_arith(Token **rest, Token *tok, NodeKind kind) {
  Node *node = new_node(ND_CKD_ARITH, tok);
  node->arith_kind = kind;
  tok = skip_tk(tok->next, TK_LPAREN);
  node->m.lhs = assign(&tok, tok);
  tok = skip_tk(tok, TK_COMMA);
  node->m.rhs = assign(&tok, tok);
  tok = skip_tk(tok, TK_COMMA);
  node->m.target = assign(&tok, tok);
  *rest = skip_tk(tok, TK_RPAREN);
  add_type(node);

  Token *bad_tok = NULL;
  if (node->m.target->ty->kind != TY_PTR ||
      node->m.target->ty->base->kind == TY_BOOL ||
      !is_int_class(node->m.target->ty->base))
    bad_tok = node->m.target->tok;
  else if (!is_int_class(node->m.lhs->ty))
    bad_tok = node->m.lhs->tok;
  else if (!is_int_class(node->m.rhs->ty))
    bad_tok = node->m.rhs->tok;
  if (bad_tok)
    error_tok(bad_tok, "operand invalid for integer overflow arithmetic");

  return node;
}

static Node *compound_literal(Token **rest, Token *tok) {
  Token *start = tok;
  VarAttr attr = {0};
  StorageClass cl_sc = (opt_std >= STD_C23)
      ? (SC_CONSTEXPR | SC_REGISTER | SC_STATIC | SC_THREAD)
      : SC_NONE;
  Type *ty = declspec(&tok, tok, &attr, cl_sc);
  validate_decl_attrs(&attr, DECL_CTX_COMPLIT, start);

  ty = declarator(&tok, tok, ty, NULL);

  if (ty->kind == TY_VOID ||
      ty->kind == TY_FUNC ||
      ty->kind == TY_VLA ||
      (ty->size < 0 && ty->kind != TY_ARRAY))
    error_tok(tok, "invalid compound literal type");

  ty = qual_constexpr(ty, &attr, tok);

  Node *expr = NULL;
  chain_expr(&expr, calc_vla2(ty, tok, &attr));

  tok = skip_tk(tok, TK_RPAREN);

  if (!fnctx || (!uneval_depth && is_const_context()) || (attr.strg & SC_STATIC)) {
    if (is_iso_std && fnctx && !uneval_depth && is_const_context() && !(attr.strg & SC_STATIC) &&
        !start->file->is_syshdr)
      error_tok(start, "block-scope compound literal is not a constant expression");
    Obj *var = new_anon_gvar(ty);
    var->is_compound_lit = true;
    var->alt_align = attr.align;
    if (attr.strg & SC_THREAD)
      var->kind = OBJ_TLS;

    if (attr.strg & SC_CONSTEXPR)
      constexpr_initializer(rest, tok, var, var);
    else
      gvar_initializer(rest, tok, var);

    chain_expr(&expr, new_var_node(var, start));
    return expr;
  }

  chk_local_tls(&attr, tok);

  Obj *var = new_lvar2(ty, decl_scope());
  var->is_compound_lit = true;
  var->alt_align = attr.align;

  if (attr.strg & SC_CONSTEXPR) {
    Obj *init_var = new_anon_gvar(ty);
    constexpr_initializer(&tok, tok, init_var, var);
    chain_expr(&expr, constexpr_lvar_init(init_var, var, tok));
  } else {
    chain_expr(&expr, lvar_initializer(&tok, tok, var, false));
  }
  chain_expr(&expr, new_var_node(var, start));
  *rest = tok;
  return expr;
}

static Node *builtin_functions(Token **rest, Token *tok) {
  if (equal(tok, "__builtin_alloca")) {
    Node *node = assign(&tok, skip_tk(tok->next, TK_LPAREN));
    node = new_unary(ND_ALLOCA, node, tok);
    *rest = skip_tk(tok, TK_RPAREN);

    scope->has_alloca = true;
    if (fnctx)
      fnctx->dont_dealloc_vla = true;
    return node;
  }

  if (equal(tok, "__builtin_frame_address") ||
      equal(tok, "__builtin_return_address"))
    error_tok(tok, "%.*s is not supported by PIR backend", tok->len, tok->loc);

  if (equal(tok, "__builtin_atomic_chk")) {
    tok = skip_tk(tok->next, TK_LPAREN);
    Node *node = nonvoid_ptr(&tok, tok);
    *rest = skip_tk(tok, TK_RPAREN);
    if (!(node->ty->base->qual & Q_ATOMIC))
      node->ty = pointer_to(add_qual(Q_ATOMIC, node->ty->base, node->tok));
    return node;
  }

  if (!strncmp(tok->loc, "__builtin_atomic_arith_", 23))
    return atomic_builtin_op(rest, tok, false);

  if (!strncmp(tok->loc, "__builtin_atomic_fetch_", 23))
    return atomic_builtin_op(rest, tok, true);

  if (equal(tok, "__builtin_atomic_thread_fence")) {
    Node *node = new_node(ND_THREAD_FENCE, tok);
    tok = skip_tk(tok->next, TK_LPAREN);
    *rest = skip_tk(tok, TK_RPAREN);
    return node;
  }

  if (equal(tok, "__builtin_constant_p")) {
    Node *node = new_node(ND_NUM, tok);
    tok = skip_tk(tok->next, TK_LPAREN);
    Node *exp = conditional(&tok, tok);
    add_type(exp);

    if (((is_integer(exp->ty) || is_ptr(exp->ty)) && is_const_expr(exp, NULL)) ||
        (is_flonum(exp->ty) && is_const_fp(exp, NULL)) ||
        (exp->ty->kind == TY_BITINT && is_const_bitint(exp, NULL)) ||
        (exp->kind == ND_VAR && exp->m.var->is_string_lit))
      node->num.val = 1;

    node->ty = ty_int;
    *rest = skip_tk(tok, TK_RPAREN);
    return node;
  }

  if (equal(tok, "__builtin_expect")) {
    tok = skip_tk(tok->next, TK_LPAREN);
    Node *node = new_cast(assign(&tok, tok), ty_long);
    tok = skip_tk(tok, TK_COMMA);
    assign(&tok, tok);
    *rest = skip_tk(tok, TK_RPAREN);
    return node;
  }

  if (equal(tok, "__builtin_expect_with_probability")) {
    tok = skip_tk(tok->next, TK_LPAREN);
    Node *node = new_cast(assign(&tok, tok), ty_long);
    tok = skip_tk(tok, TK_COMMA);
    assign(&tok, tok);
    tok = skip_tk(tok, TK_COMMA);
    assign(&tok, tok);
    *rest = skip_tk(tok, TK_RPAREN);
    return node;
  }

  if (equal(tok, "__builtin_offsetof")) {
    tok = skip_tk(tok->next, TK_LPAREN);
    Token *ty_start = tok;
    Type *ty = typename(&tok, tok);
    if (is_iso_std && opt_std < STD_C23 && !ty_start->file->is_syshdr &&
        has_lcurly_before(ty_start, tok))
      error_tok(ty_start, "defining a type within 'offsetof' is a C23 extension");
    Node *node = new_unary(ND_DEREF, new_cast(new_num(0, tok), pointer_to(ty)), tok);
    tok = skip_tk(tok, TK_COMMA);
    node = struct_ref(node, ident_tok(&tok, tok));
    for (;;) {
      if (tok->kind == TK_DOT) {
        node = struct_ref(node, ident_tok(&tok, tok->next));
        continue;
      }
      if (tok->kind == TK_LBRACK) {
        Token *start = tok;
        Node *idx = expression(&tok, tok->next);
        tok = skip_tk(tok, TK_RBRACK);
        node = new_unary(ND_DEREF, new_add(node, idx, start), start);
        continue;
      }
      break;
    }
    add_type(node);
    if (is_bitfield(node))
      error_tok(tok, "cannot apply offsetof to bit-field");
    *rest = skip_tk(tok, TK_RPAREN);

    int64_t ofs;
    if (eval_non_var_ofs(node, &ofs))
      return new_size_t(ofs, tok);
    return new_cast(new_unary(ND_ADDR, node, tok), ty_size_t);
  }

  if (equal(tok, "__builtin_add_overflow"))
    return checked_arith(rest, tok, ND_ADD);

  if (equal(tok, "__builtin_sub_overflow"))
    return checked_arith(rest, tok, ND_SUB);

  if (equal(tok, "__builtin_mul_overflow"))
    return checked_arith(rest, tok, ND_MUL);

  if (equal(tok, "__builtin_types_compatible_p")) {
    tok = skip_tk(tok->next, TK_LPAREN);
    Type *t1 = typename(&tok, tok);
    tok = skip_tk(tok, TK_COMMA);
    Type *t2 = typename(&tok, tok);
    *rest = skip_tk(tok, TK_RPAREN);
    return new_num(is_compatible(t1, t2), tok);
  }

  if (equal(tok, "__builtin_unreachable")) {
    Node *node = new_node(ND_UNREACHABLE, tok);
    tok = skip_tk(tok->next, TK_LPAREN);
    *rest = skip_tk(tok, TK_RPAREN);
    return node;
  }

  if (equal(tok, "__builtin_c23_va_start")) {
    Node *node = new_node(ND_VA_START, tok);
    tok = skip_tk(tok->next, TK_LPAREN);
    node->m.lhs = conditional(&tok, tok);
    if (tok->kind == TK_COMMA)
      assign(&tok, tok->next);
    *rest = skip_tk(tok, TK_RPAREN);
    return node;
  }

  if (equal(tok, "__builtin_va_start")) {
    Node *node = new_node(ND_VA_START, tok);
    tok = skip_tk(tok->next, TK_LPAREN);
    node->m.lhs = conditional(&tok, tok);
    assign(&tok, skip_tk(tok, TK_COMMA));
    *rest = skip_tk(tok, TK_RPAREN);
    return node;
  }

  if (equal(tok, "__builtin_va_copy")) {
    Node *node = new_node(ND_VA_COPY, tok);
    tok = skip_tk(tok->next, TK_LPAREN);
    node->m.lhs = conditional(&tok, tok);
    tok = skip_tk(tok, TK_COMMA);
    node->m.rhs = conditional(&tok, tok);
    *rest = skip_tk(tok, TK_RPAREN);
    return node;
  }

  if (equal(tok, "__builtin_va_end")) {
    tok = skip_tk(tok->next, TK_LPAREN);
    Node *node = conditional(&tok, tok);
    *rest = skip_tk(tok, TK_RPAREN);
    return node;
  }

  if (equal(tok, "__builtin_va_arg")) {
    Node *node = new_node(ND_VA_ARG, tok);
    tok = skip_tk(tok->next, TK_LPAREN);

    Node *ap_arg = conditional(&tok, tok);
    add_type(ap_arg);
    node->m.lhs = ap_arg;
    tok = skip_tk(tok, TK_COMMA);

    VarAttr attr = {0};
    Type *ty = typename2(&tok, tok, &attr);
    *rest = skip_tk(tok, TK_RPAREN);

    node->ty = pointer_to(ty);
    node = new_unary(ND_DEREF, node, tok);

    Node *calc = calc_vla2(ty, tok, &attr);
    if (calc)
      return new_binary(ND_COMMA, calc, node, tok);
    return node;
  }

  if (equal(tok, "__builtin_compare_and_swap")) {
    Node *node = new_node(ND_CAS, tok);
    tok = skip_tk(tok->next, TK_LPAREN);
    node->cas.addr = nonvoid_ptr(&tok, tok);
    tok = skip_tk(tok, TK_COMMA);
    node->cas.old_val = nonvoid_ptr(&tok, tok);
    tok = skip_tk(tok, TK_COMMA);
    node->cas.new_val = assign(&tok, tok);
    *rest = skip_tk(tok, TK_RPAREN);
    return node;
  }

  if (equal(tok, "__builtin_atomic_exchange")) {
    Node *node = new_node(ND_EXCH, tok);
    tok = skip_tk(tok->next, TK_LPAREN);
    node->m.lhs = nonvoid_ptr(&tok, tok);
    tok = skip_tk(tok, TK_COMMA);
    node->m.rhs = assign(&tok, tok);
    *rest = skip_tk(tok, TK_RPAREN);
    return node;
  }

  if (!strncmp(tok->loc, "__builtin_math_constant_", 24)) {
    Node *node = new_node(ND_NUM, tok);
    const char *loc = tok->loc + 24;
    int len = tok->len - 24;

    if (equal_substr(loc, len, "nanf")) {
      node->num.constant = MATH_CONSTANT_NANF;
      node->ty = ty_float;
    } else if (equal_substr(loc, len, "inff")) {
      node->num.constant = MATH_CONSTANT_INFF;
      node->ty = ty_float;
    } else if (equal_substr(loc, len, "nansf")) {
      node->num.constant = MATH_CONSTANT_NANSF;
      node->ty = ty_float;
    } else if (equal_substr(loc, len, "nans")) {
      node->num.constant = MATH_CONSTANT_NANS;
      node->ty = ty_double;
    } else if (equal_substr(loc, len, "nansl")) {
      node->num.constant = MATH_CONSTANT_NANSL;
      node->ty = ty_ldouble;
    } else {
      error_tok(tok, "unknown math constant");
    }
    tok = skip_tk(tok->next, TK_LPAREN);
    *rest = skip_tk(tok, TK_RPAREN);
    return node;
  }
  return NULL;
}

static Node *primary(Token **rest, Token *tok) {
  if (tok->kind == TK_Generic)
    return generic_selection(rest, tok->next);

  if (tok->kind == TK_IDENT) {
    VarScope *sc = find_var(tok);
    *rest = tok->next;

    if (sc) {
      if (sc->var) {
        if (is_iso_std && fnctx && fnctx->fn->is_ext_inline && !fnctx->fn->is_static &&
            !tok->file->is_syshdr && sc->var->is_static &&
            (sc->var->kind == OBJ_GLOBAL || sc->var->kind == OBJ_TLS) &&
            !sc->var->is_static_lvar)
          error_tok(tok, "inline function with external linkage cannot reference static identifier");
        Node *n = new_var_node(sc->var, tok);
        if (sc->var_ty)
          n->ty = sc->var_ty;
        return n;
      }
      if (sc->enum_val) {
        if (sc->enum_val->ty->kind == TY_BITINT) {
          Node *n = new_node(ND_NUM, tok);
          n->num.bitint_data = calloc(1, bitint_buffer_size(sc->enum_val->ty));
          n->num.bitint_data->as64 = sc->enum_val->val;
          n->ty = sc->enum_val->ty;
          return n;
        }
        Node *n = new_num(sc->enum_val->val, tok);
        n->ty = (sc->enum_val->ty->is_int_enum) ? ty_int : sc->enum_val->ty;
        return n;
      }
    }

    if (tok->next->kind == TK_LPAREN) {
      Node *node = builtin_functions(rest, tok);
      if (node)
        return node;

      if (opt_std < STD_C99)
        return new_var_node(func_prototype_implicit(tok), tok);

      error_tok(tok, "implicit declaration of a function");
    }
    error_tok(tok, "undefined variable");
  }

  if (tok->kind == TK_STR) {
    Obj *var = new_anon_gvar(tok->ty);
    var->init_data = tok->str;
    var->is_string_lit = true;
    *rest = tok->next;
    return new_var_node(var, tok);
  }

  if (tok->kind == TK_true || tok->kind == TK_false) {
    *rest = tok->next;
    return new_boolean(tok->kind == TK_true, tok);
  }

  if (tok->kind == TK_nullptr) {
    *rest = tok->next;
    Node *node = new_node(ND_NUM, tok);
    node->ty = ty_nullptr;
    node->tok = tok;
    return node;
  }

  if (tok->kind == TK_PP_NUM || tok->kind == TK_CHAR_LIT) {
    Node *node = new_node(ND_NUM, tok);
    convert_pp_number(tok, node);
    *rest = tok->next;
    return node;
  }

  if (tok->kind == TK_PP_NUM_PPEV || tok->kind == TK_CHAR_LIT_PPEV) {
    bool is_num = (tok->kind == TK_PP_NUM_PPEV);
    tok->kind--;
    Node *node = new_node(ND_NUM, tok);
    convert_pp_number(tok, node);
    *rest = tok->next;

    if (is_integer(node->ty)) {
      if (is_num) {
        bool has_u = false;
        for (int i = 0; i < tok->len; i++) {
          if (tok->loc[i] == 'u' || tok->loc[i] == 'U') {
            has_u = true;
            break;
          }
        }
        node->ty = (has_u || (uint64_t)node->num.val > (uint64_t)INT64_MAX)
                       ? ty_uintmax_t : ty_intmax_t;
      } else {
        node->ty = node->ty->is_unsigned ? ty_uintmax_t : ty_intmax_t;
      }
    } else if (node->ty->kind == TY_BITINT) {
      int_or_trunc_bitint(&node, true);
    } else {
      error_tok(tok, "non-integer numeric literal in preprocessor expression");
    }
    return node;
  }

  if (tok->kind == TK_INTMAX_NUM) {
    Node *node = new_node(ND_NUM, tok);
    node->num.val = tok->ival;
    node->ty = ty_intmax_t;
    *rest = tok->next;
    return node;
  }

  if (tok->kind == TK_FUNCTION) {
    if (!fnctx) {
      static Obj *empty_name;
      if (!empty_name) {
        empty_name = new_anon_gvar(array_of(add_qual(Q_CONST, ty_pchar, tok), 1));
        empty_name->init_data = arena_strdup(&cc1_arena, "");
      }
      *rest = tok->next;
      return new_var_node(empty_name, tok);
    }
    if (!fnctx->fnname) {
      char *name = fnctx->fn->name;
      fnctx->fnname = new_static_lvar(array_of(add_qual(Q_CONST, ty_pchar, tok),
                                               strlen(name) + 1));
      fnctx->fnname->init_data = name;
    }
    *rest = tok->next;
    return new_var_node(fnctx->fnname, tok);
  }

  error_tok(tok, "expected an expression");
}

static bool is_same_typedef_ty(Type *a, Type *b) {
  if (!is_compatible2(a, b))
    return false;
  if (a->origin) a = a->origin;
  if (b->origin) b = b->origin;
  if (a->is_enum != b->is_enum)
    return false;
  if (is_array(a) && is_array(b)) {
    if ((a->size < 0) != (b->size < 0))
      return false;
    return is_same_typedef_ty(a->base, b->base);
  }
  if (a->kind == TY_PTR && b->kind == TY_PTR)
    return is_same_typedef_ty(a->base, b->base);
  if (a->kind == TY_FUNC && b->kind == TY_FUNC) {
    if (a->is_oldstyle != b->is_oldstyle)
      return false;
    if (!is_same_typedef_ty(a->return_ty, b->return_ty))
      return false;
    for (Obj *p1 = a->param_list, *p2 = b->param_list; p1 && p2;
         p1 = p1->param_next, p2 = p2->param_next)
      if (!is_same_typedef_ty(unqual(p1->ty), unqual(p2->ty)))
        return false;
  }
  return true;
}

static Node *parse_typedef(Token **rest, Token *tok, Type *basety, VarAttr *attr) {
  Node *node = NULL;
  bool first = true;
  for (; comma_list(rest, &tok, TK_SEMI, !first); first = false) {
    Token *name = NULL;
    Type *ty = declarator(&tok, tok, basety, &name);

    if (!name)
      error_tok(tok, "typedef name omitted");
    if (!scope->parent && is_vm_ty(ty))
      error_tok(tok, "variably-modified type at file scope");

    int align = 0;
    validate_decl_attrs(attr, DECL_CTX_TYPEDEF, name);
    aligned_attr(name, tok, attr, &align);

    HashEntry *ent = hashmap_get_or_insert(&decl_scope()->vars, name->loc, name->len);
    VarScope *vsc = ent->val;
    if (vsc) {
      if (!vsc->type_def)
        error_tok(name, "redeclaration of '%.*s'", name->len, name->loc);
      if (is_iso_std ? !is_same_typedef_ty(ty, vsc->type_def) : !is_compatible2(ty, vsc->type_def))
        error_tok(name, "incompatible redeclaration");
      if (is_variably_modified(ty) || is_variably_modified(vsc->type_def))
        error_tok(name, "redefining typedef of variably-modified type");
      if (align > vsc->type_def->align)
        vsc->type_def = aligned_type(align, vsc->type_def);
    } else {
      vsc = ent->val = arena_calloc(fnctx ? &ast_arena : &cc1_arena, sizeof(VarScope));
      if (align)
        vsc->type_def = aligned_type(align, ty);
      else
        vsc->type_def = ty;
      chain_expr(&node, calc_vla(ty, tok));
      if (fnctx && is_vm_ty(ty))
        new_defr(DF_VM_BARRIER);
    }
  }
  return node;
}

static void set_goto(Node *node, Node *dest) {
  node->jmp.target = dest;

  if (!dest->dfr_from)
    return;

  DeferStmt *dfr = node->dfr_from;
  for (; dfr; dfr = dfr->next)
    if (dfr == dest->dfr_from)
      break;
  if (!dfr)
    error_tok(node->tok->next, "illegal jump");

  node->dfr_dest = dfr;
  return;
}

static void resolve_gotos(Node *fn_body) {
  Node *search_list = fnctx->labels;
  Node live_head = {0};
  Node *live_cur = &live_head;
  HashMap label_cache = {0};
  for (Node *x = fnctx->gotos; x; x = x->lbl.next) {
    HashEntry *ent = hashmap_get_or_insert(&label_cache, x->tok->loc, x->tok->len);
    Node *dest = ent->val;
    if (!dest) {
      Node head = {0};
      Node *cur = &head;
      for (Node *lbl = search_list; lbl; lbl = lbl->lbl.next) {
        if (!equal_tok(lbl->tok, x->tok)) {
          cur = cur->lbl.next = lbl;
          continue;
        }
        if (dest)
          error_tok(x->tok, "duplicated label");
        dest = ent->val = lbl;
        live_cur = live_cur->lbl.next = lbl;
      }
      if (!dest)
        error_tok(x->tok, "use of undeclared label");
      cur->lbl.next = NULL;
      search_list = head.lbl.next;
    }

    set_goto(x, dest);
  }
  free(label_cache.buckets);

  live_cur->lbl.next = fn_body->blk.local_labels;
  fn_body->blk.local_labels = live_head.lbl.next;
}

static Node *resolve_local_gotos(void) {
  for (Node *x = scope->gotos; x; x = x->lbl.next) {
    LocalLabel *ll = find_local_label(scope, x->tok);
    if (!ll || !ll->label)
      error_tok(x->tok, "use of undeclared local label");

    set_goto(x, ll->label);
  }

  Node head = {0};
  Node *cur = &head;
  for (LocalLabel *ll = scope->labels; ll; ll = ll->next)
    if (ll->label)
      cur = cur->lbl.next = ll->label;
  return head.lbl.next;
}

static void chk_param_ty(Type *fn, Type *ty, int idx, Token *tok) {
  int i = 0;
  for (Obj *p = fn->param_list; p; p = p->param_next, i++) {
    if (i == idx) {
      if (!!(p->ty->qual & Q_ATOMIC) != !!(ty->qual & Q_ATOMIC) ||
          !is_compatible(p->ty, ty))
        error_tok(tok, "mismatched parameter type");
      return;
    }
  }
  internal_error();
}

static Node *func_old_style_param(Token **rest, Token *tok, Type *prot_ty, Type *def_ty) {
  if (!prot_ty->is_oldstyle) {
    def_ty->is_oldstyle = false;

    Obj *p1 = prot_ty->param_list;
    Obj *p2 = def_ty->param_list;
    while (p1 && p2) {
      p1 = p1->param_next;
      p2 = p2->param_next;
    }
    if (!p1 != !p2)
      error_tok(tok, "parameter count mismatch");
  }

  Node *expr = NULL;
  while (is_typename(tok)) {
    VarAttr attr = {0};
    Type *basety = declspec(&tok, tok, &attr, SC_REGISTER);
    validate_decl_attrs(&attr, DECL_CTX_PARAM, tok);

    chain_expr(&expr, calc_vla2(basety, tok, &attr));

    do {
      Token *name = NULL;
      Type *raw_ty = declarator2(&tok, tok, basety, &name, false);
      Type *ty = param_declarator(basety, raw_ty, tok, true);

      chain_expr(&expr, calc_vla(raw_ty, tok));

      if (!name)
        error_tok(tok, "expected identifier");

      int idx = 0;
      Obj *var = def_ty->param_list;
      for (; var; var = var->param_next, idx++)
        if (equal(name, var->name))
          break;

      if (!var)
        error_tok(name, "no such parameter");

      Type *promoted = NULL;
      if (is_integer(ty) && ty->size < ty_int->size)
        promoted = (ty->qual & Q_ATOMIC) ? add_qual(Q_ATOMIC, ty_int, NULL) : ty_int;
      else if (ty->kind == TY_FLOAT)
        promoted = (ty->qual & Q_ATOMIC) ? add_qual(Q_ATOMIC, ty_double, NULL) : ty_double;

      if (!prot_ty->is_oldstyle)
        chk_param_ty(prot_ty, promoted ? promoted : ty, idx, name);

      if (!promoted) {
        var->ty = ty;
        var->is_register = !!(attr.strg & SC_REGISTER);
        aligned_attr(name, tok, &attr, &var->alt_align);
        push_var_name(name, var);
      } else {
        var->ty = (ty->qual & Q_ATOMIC) ? promoted : unqual(promoted);
        Node *lhs = new_var_node(new_lvar2(ty, decl_scope()), name);
        lhs->m.var->is_register = !!(attr.strg & SC_REGISTER);
        aligned_attr(name, tok, &attr, &lhs->m.var->alt_align);
        push_var_name(name, lhs->m.var);

        Node *rhs = new_var_node(var, name);
        if (ty->kind == TY_BOOL)
          rhs = new_binary(ND_BITAND, rhs, new_num(1, name), name);
        chain_expr(&expr, new_binary(ND_ASSIGN, lhs, rhs, name));
      }
    } while (comma_list(&tok, &tok, TK_SEMI, true));
  }

  int idx = 0;
  Obj *var = def_ty->param_list;
  for (; var; var = var->param_next, idx++) {
    if (var->ty)
      continue;

    if (is_iso_std && opt_std >= STD_C99 && !tok->file->is_syshdr)
      error_tok(tok, "type of K&R parameter defaults to 'int'");
    if (!prot_ty->is_oldstyle)
      chk_param_ty(prot_ty, ty_int, idx, tok);

    var->ty = ty_int;
    push_var_name2(var->name, strlen(var->name), tok, var);
  }

  *rest = tok;
  return expr;
}

static void strip_proto_vla_exprs(Type *ty) {
  for (; ty; ty = ty->base) {
    if (ty->kind == TY_VLA) {
      ty->vla_len_expr = NULL;
      ty->array_len = -1;
    } else if (ty->kind == TY_FUNC) {
      strip_proto_vla_exprs(ty->return_ty);
      for (Obj *p = ty->param_list; p; p = p->param_next)
        strip_proto_vla_exprs(p->ty);
      return;
    } else if (ty->kind != TY_PTR && ty->kind != TY_ARRAY) {
      return;
    }
  }
}

static void func_definition(Token **rest, Token *tok, Obj *fn, Type *ty) {
  if (scope->parent)
    error_tok(tok, "nested function not supported");
  if (ty->return_ty->size < 0)
    error_tok(tok, "incomplete return type");
  if (is_iso_std && ty->return_ty->kind == TY_VOID && ty->return_ty->qual != 0 &&
      !tok->file->is_syshdr)
    error_tok(tok, "function cannot have qualified 'void' return type");
  fn->is_definition = true;

  Type *vis_ty = fn->ty;
  Type *compat_prot = fn->compat_ty ? fn->compat_ty : vis_ty;
  if (vis_ty && vis_ty != ty && vis_ty->kind == TY_FUNC) {
    ty->return_ty = composite_type(vis_ty->return_ty, ty->return_ty);
  }
  if (compat_prot && compat_prot != ty && compat_prot->kind == TY_FUNC &&
      !compat_prot->is_oldstyle && ty->is_oldstyle && !ty->scopes) {
    if (compat_prot->param_list)
      error_tok(tok, "parameter count mismatch");
    if (vis_ty && !vis_ty->is_oldstyle)
      ty->is_oldstyle = false;
  }
  bool keep_vis_in_body = (!ty->is_oldstyle && vis_ty && vis_ty != ty);
  fn->ty = keep_vis_in_body ? vis_ty : ty;

  fnctx = &(FuncContext){.fn = fn};

  Node *precalc = NULL;

  if (!ty->scopes) {
    enter_fn_scope(ty);
    if (ty->is_oldstyle)
      ty->is_oldstyle_def = true;
  } else {
    scope = ty->scopes;

    if (ty->is_oldstyle) {
      bool vis_was_old = (!vis_ty || vis_ty->is_oldstyle);
      precalc = func_old_style_param(&tok, tok, compat_prot, ty);
      if (vis_was_old)
        ty->is_oldstyle = true;
      ty->is_oldstyle_def = true;
    } else {
      precalc = ty->pre_calc;
    }
  }
  fn->compat_ty = (compat_prot && compat_prot != ty) ? composite_type(compat_prot, ty) : ty;

  fn->body = compound_stmt2(rest, skip_tk(tok, TK_LCURLY), ND_BLOCK);

  if (precalc) {
    precalc = new_unary(ND_EXPR_STMT, precalc, tok);
    precalc->next = fn->body->blk.body;
    fn->body->blk.body = precalc;
  }
  add_type(fn->body);

  if (fnctx->use_vla &&
      !fnctx->dont_dealloc_vla &&
      (opt_reuse_stack && !fnctx->fn->dont_reuse_stk))
    fn->dealloc_vla = true;

  if (fnctx->gotos)
    resolve_gotos(fn->body);

  fn->ty = ty;
  emit_text(fn);

  if (keep_vis_in_body)
    fn->ty = vis_ty;

  fnctx = NULL;
}

static void set_returns_twice(Obj *fn) {
  if (strstr(fn->name, "setjmp") ||
      strstr(fn->name, "savectx") ||
      strstr(fn->name, "vfork") ||
      strstr(fn->name, "getcontext"))
    fn->returns_twice = true;
}

static Obj *func_prototype_implicit(Token *name) {
  HashEntry *ent = hashmap_get_or_insert(&symbols, name->loc, name->len);
  Obj *fn = ent->val;
  if (!fn) {
    Type *ty = func_type(TY_FUNC, ty_int);
    ty->is_oldstyle = true;

    fn = ent->val = new_gvar(get_ident(&cc1_arena, name), ty);
    set_returns_twice(fn);
  }
  push_gvar_name(name, fn);
  return fn;
}

static void clear_fn_obj_state(Obj *fn) {
  fn->dont_reuse_stk = false;
  fn->dealloc_vla = false;
}

static Type *merge_external_decl_type(Token *name, Obj *obj, Type *ty,
                                      bool is_func, bool is_def) {
  Type *cty = obj->compat_ty ? obj->compat_ty : obj->ty;
  bool compat = is_func ? is_compatible(cty, ty) : is_compatible2(cty, ty);
  if (!compat)
    error_tok(name, "incompatible redeclaration");
  obj->compat_ty = composite_type(cty, ty);
  if (scope->parent && !is_def)
    return ty;
  obj->ty = obj->has_file_decl ? composite_type(obj->ty, ty) : ty;
  obj->has_file_decl = true;
  return NULL;
}

static void bind_external_decl_scope(Token *name, Obj *obj, Type *scoped_ty,
                                     VarScope *prev_vsc, bool is_func) {
  VarScope *vsc = push_gvar_name2(name, obj);
  if (scoped_ty && vsc) {
    if (prev_vsc && prev_vsc->var == obj) {
      Type *prev_ty = prev_vsc->var_ty ? prev_vsc->var_ty : obj->ty;
      bool compat = is_func ? is_compatible(prev_ty, scoped_ty)
                            : is_compatible2(prev_ty, scoped_ty);
      if (!compat)
        error_tok(name, "incompatible redeclaration");
      scoped_ty = composite_type(prev_ty, scoped_ty);
    }
    vsc->var_ty = scoped_ty;
  }
}

static Obj *func_prototype(Token *tok, Token *name, Type *ty, VarAttr *attr, bool is_def) {
  if (!name)
    error_tok(tok, "function name omitted");
  if (is_vm_ty(ty->return_ty))
    error_tok(name, "cannot return variably-modified type");
  if (attr->strg & (SC_AUTO | SC_CONSTEXPR | SC_REGISTER | SC_THREAD))
    error_tok(name, "invalid storage class for function");

  bool is_gnu_inline = false;
  if (attr->strg & SC_INLINE) {
    if (equal(name, "main"))
      error_tok(name, "'main' cannot be declared 'inline'");
    is_gnu_inline = attr->is_gnu_inline || opt_gnu89_inline || opt_std < STD_C99;
    DeclAttr(bool_attr, "gnu_inline", &is_gnu_inline);
  }

  HashEntry *ent = hashmap_get_or_insert(&symbols, name->loc, name->len);
  Obj *fn = ent->val;
  Type *scoped_fn_ty = NULL;
  VarScope *prev_vsc = scope->parent ? find_var(name) : NULL;
  if (fn) {
    scoped_fn_ty = merge_external_decl_type(name, fn, ty, true, is_def);

    if (fn->is_gnu_inline && !fn->export_fn && !fn->is_static) {
      if (attr->strg & SC_STATIC) {
        fn->is_static = true;
        fn->is_definition = false;

        clear_fn_obj_state(fn);
      } else if (is_def && fn->is_definition) {
        if (is_gnu_inline && (attr->strg & SC_EXTERN))
          error_tok(name, "redefinition of %s", fn->name);

        clear_fn_obj_state(fn);
      }
    } else {
      if (attr->strg & SC_STATIC && !fn->is_static)
        error_tok(name, "static declaration follows a non-static declaration");
      if (is_def && fn->is_definition)
        error_tok(name, "redefinition of %s", fn->name);

      if (!fn->is_definition && !fn->is_gnu_inline)
        if (is_gnu_inline && (attr->strg & SC_EXTERN))
          fn->export_fn = false;
    }
  } else {
    fn = ent->val = new_gvar(get_ident(&cc1_arena, name), ty);
    fn->compat_ty = ty;
    fn->is_static = attr->strg & SC_STATIC;
    fn->has_file_decl = !scope->parent;
    if (scope->parent && !is_def)
      scoped_fn_ty = ty;
    set_returns_twice(fn);
  }
  bind_external_decl_scope(name, fn, scoped_fn_ty, prev_vsc, true);

  fn->is_gnu_inline |= is_gnu_inline;

  aligned_attr(name, tok, attr, &fn->alt_align);
  symbol_attr(name, tok, attr, fn);
  func_attr(name, tok, attr, fn);
  return fn;
}

static void func_exportness(Obj *fn, VarAttr *attr, bool is_def) {
  if (!fn->is_static && !fn->export_fn && !scope->parent) {
    if (fn->is_gnu_inline) {
      if (is_def)
        fn->export_fn = !(attr->strg & SC_INLINE) || !(attr->strg & SC_EXTERN);
      else
        fn->export_fn = (attr->strg & SC_INLINE) && !(attr->strg & SC_EXTERN);
    } else {
      fn->export_fn = !(attr->strg & SC_INLINE) || (attr->strg & SC_EXTERN);
    }
  }
}

static void global_declaration(Token **rest, Token *tok, Type *basety, VarAttr *attr) {
  if (tok->kind == TK_SEMI) {
    chk_inline(attr, tok);
    if (is_iso_user_tok(tok) && scope->parent &&
        basety->kind != TY_STRUCT && basety->kind != TY_UNION && !basety->is_enum)
      error_tok(tok, "declaration does not declare anything");
    *rest = tok->next;
    return;
  }
  bool first = true;
  for (; comma_list(&tok, &tok, TK_SEMI, !first); first = false) {
    Token *name = NULL;
    Type *ty = declarator2(&tok, tok, basety, &name, false);

    bool is_fndef = first &&
                    ty->kind == TY_FUNC_INCMP &&
                    (tok->kind == TK_LCURLY || is_typename(tok));

    declarator3(basety, ty, false, is_fndef);

    if (ty->kind == TY_FUNC) {
      validate_decl_attrs(attr, DECL_CTX_FUNC, name ? name : tok);
      if (!is_fndef)
        strip_proto_vla_exprs(ty);
      Obj *fn = func_prototype(tok, name, ty, attr, is_fndef);
      func_exportness(fn, attr, is_fndef);
      if (is_fndef && (attr->strg & SC_INLINE) && !fn->is_static && !fn->is_gnu_inline)
        fn->is_ext_inline = true;

      if (is_fndef) {
        func_definition(rest, tok, fn, ty);
        return;
      }
      if (!fn->is_definition && !(fnctx && fnctx->fn == fn))
        strip_proto_vla_exprs(fn->ty);
      assembler_name(&tok, tok, fn);
      continue;
    }

    chk_vardecl(ty, name, tok, (attr->strg & SC_EXTERN) && tok->kind != TK_EQ);
    chk_inline(attr, tok);
    ty = qual_constexpr(ty, attr, tok);

    if (is_vm_ty(ty))
      error_tok(name, "variably-modified type cannot be 'extern' or at file scope");

    bool is_definition = !(attr->strg & SC_EXTERN);
    if (!is_definition) {
      if (tok->kind == TK_EQ) {
        if (scope->parent)
          error_tok(tok, "extern variable cannot be initialized");
        is_definition = true;
      }
    }

    HashEntry *ent = hashmap_get_or_insert(&symbols, name->loc, name->len);
    Obj *var = ent->val;
    Type *scoped_ty = NULL;
    VarScope *prev_vsc = scope->parent ? find_var(name) : NULL;
    bool existed = (var != NULL);
    if (var) {
      if ((!var->is_static && !!(attr->strg & SC_STATIC)) ||
          (var->is_static && !(attr->strg & (SC_STATIC | SC_EXTERN))))
        error_tok(name, "redeclaration has inconsistent 'static'");
      if ((var->kind == OBJ_TLS) != !!(attr->strg & SC_THREAD))
        error_tok(name, "redeclaration with%s thread local",
                  var->kind == OBJ_TLS ? "out" : "");
      scoped_ty = merge_external_decl_type(name, var, ty, false, false);
    } else {
      var = ent->val = new_gvar(get_ident(&cc1_arena, name), ty);
      var->compat_ty = ty;
      var->is_static = (attr->strg & SC_STATIC) || (attr->strg & SC_CONSTEXPR);
      var->has_file_decl = !scope->parent;
      if (scope->parent)
        scoped_ty = ty;
      if (attr->strg & SC_THREAD)
        var->kind = OBJ_TLS;
    }
    bind_external_decl_scope(name, var, scoped_ty, prev_vsc, false);

    assembler_name(&tok, tok, var);
    chk_alignas_natural(attr, ty, name);
    if (is_iso_std && !tok->file->is_syshdr) {
      int cur_al = attr->has_alignas ? MAX(attr->alignas_val, ty->align) : 0;
      if (existed) {
        if (var->has_alignas && attr->has_alignas && cur_al != var->alignas_val)
          error_tok(name, "_Alignas redeclaration mismatch");
        if (var->has_alignas && !attr->has_alignas && is_definition)
          error_tok(name, "definition missing '_Alignas' from previous declaration");
        if (!var->has_alignas && attr->has_alignas && var->has_real_def)
          error_tok(name, "'_Alignas' cannot be added after definition");
      }
      if (attr->has_alignas) {
        var->has_alignas = true;
        var->alignas_val = cur_al;
      }
    }
    aligned_attr(name, tok, attr, &var->alt_align);
    symbol_attr(name, tok, attr, var);

    if (!is_definition)
      continue;
    if (var->init_data) {
      if (tok->kind == TK_EQ)
        error_tok(tok, "redefinition of '%.*s'", name->len, name->loc);
      continue;
    }
    var->is_definition = true;

    if (attr->strg & SC_CONSTEXPR) {
      var->has_real_def = true;
      constexpr_initializer(&tok, skip_tk(tok, TK_EQ), var, var);
      if (var->compat_ty && var->compat_ty != var->ty) {
        if (!is_compatible2(var->compat_ty, var->ty))
          error_tok(name, "incompatible redeclaration");
        var->compat_ty = composite_type(var->compat_ty, var->ty);
      } else {
        var->compat_ty = var->ty;
      }
    } else if (tok->kind == TK_EQ) {
      var->has_real_def = true;
      gvar_initializer(&tok, tok->next, var);
      if (var->compat_ty && var->compat_ty != var->ty) {
        if (!is_compatible2(var->compat_ty, var->ty))
          error_tok(name, "incompatible redeclaration");
        var->compat_ty = composite_type(var->compat_ty, var->ty);
      } else {
        var->compat_ty = var->ty;
      }
    } else if (is_iso_std && var->is_static && var->ty->size < 0)
      error_tok(name, "tentative definition with internal linkage has incomplete type");
  }
  *rest = tok;
}

static Token *free_parsed_tok(Token *tok, Token *end) {
  while (tok != end) {
    for (Token *t = tok->attr_next; t;) {
      Token *t2 = t;
      t = t->attr_next;

      while (t2) {
        Token *tmp = t2;
        t2 = t2->next;
        free(tmp);
      }
    }
    Token *tmp = tok;
    tok = tok->next;

    if (!tmp->is_live)
      free(tmp);
  }
  return end;
}

Obj *parse(Token *tok) {
  ty_eval_int = ty_int;

  Obj *glb_head = globals;

  bool has_decl = false;
  Token *free_head = tok;
  while (tok->kind != TK_EOF) {
    if (free_alloc)
      free_head = free_parsed_tok(free_head, tok);

    if (tok->kind == TK_SEMI) {
      if (is_iso_std && !tok->file->is_syshdr)
        error_tok(tok, "extra ';' outside of a function is not allowed in ISO C");
      tok = tok->next;
      continue;
    }

    if (tok->kind == TK_asm)
      error_tok(tok, "top-level inline assembly is not supported by PIR backend");

    arena_on(&ast_arena);

    if (tok->kind == TK_PRAGMA) {
      pragma_pack(&tok, tok);
      arena_off(&ast_arena);
      continue;
    }

    if (tok->kind == TK_static_assert) {
      if (!tok->file || strcmp(tok->file->name, "slimcc_builtins") != 0)
        has_decl = true;
      eval_static_assert(&tok, tok->next);
      tok = skip_tk(tok, TK_SEMI);
      arena_off(&ast_arena);
      continue;
    }

    if (!tok->file || strcmp(tok->file->name, "slimcc_builtins") != 0)
      has_decl = true;
    Token *decl_start = tok;
    VarAttr attr = {0};
    StorageClass file_sc = SC_EXTERN | SC_STATIC | SC_THREAD | SC_TYPEDEF | SC_INLINE | SC_CONSTEXPR;
    if (opt_std >= STD_C23)
      file_sc |= SC_AUTO;
    if (!is_iso_std)
      file_sc |= SC_REGISTER;
    Type *basety = declspec(&tok, tok, &attr, file_sc);

    if (tok->kind == TK_SEMI) {
      chk_inline(&attr, tok);
      if ((attr.strg & SC_TYPEDEF) && is_iso_user_tok(decl_start))
        error_tok(decl_start, "typedef requires a name");
      if (is_iso_user_tok(decl_start) &&
          basety->kind != TY_STRUCT && basety->kind != TY_UNION &&
          (!basety->is_enum || !has_lcurly_before(decl_start, tok)))
        error_tok(decl_start, "declaration does not declare anything");
      tok = tok->next;
      arena_off(&ast_arena);
      continue;
    }

    if (attr.strg & SC_TYPEDEF) {
      chk_inline(&attr, tok);
      parse_typedef(&tok, tok, basety, &attr);
      arena_off(&ast_arena);
      continue;
    }

    global_declaration(&tok, tok, basety, &attr);
    arena_off(&ast_arena);
  }

  if (is_iso_std && !has_decl && (!tok->file || !tok->file->is_syshdr))
    error_tok(tok, "ISO C requires a translation unit to contain at least one declaration");

  for (Obj *v = glb_head->next; v; v = v->next) {
    if (v->is_definition && v->ty->kind != TY_FUNC && v->ty->size < 0) {
      if (v->ty->kind == TY_ARRAY) {
        if (v->ty->base && v->ty->base->size > 0)
          v->ty = array_of(v->ty->base, 1);
        else
          error("tentative definition of '%s' has incomplete element type", v->name);
      } else {
        error("tentative definition of '%s' has incomplete type", v->name);
      }
    }
  }

  return glb_head->next;
}
