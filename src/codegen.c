#include "codegen.h"
#include "arena.h"
#include "ast.h"
#include "debug.h"
#include "diag.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTX_REG_START 1
#define CTX_LABEL_START 1

typedef struct {
  type_t type;
  const char *ref;
} value_t;

typedef struct ctx_str ctx_str_t;
struct ctx_str {
  expr_t *node;
  int id;
  ctx_str_t *next;
};

typedef struct ctx_global ctx_global_t;
struct ctx_global {
  const char *name;
  type_t type;
  ctx_global_t *next;
};

typedef struct ctx_local ctx_local_t;
struct ctx_local {
  const char *name;
  type_t type;
  const char *ptr;
  ctx_local_t *next;
};

typedef struct ctx_struct ctx_struct_t;
struct ctx_struct {
  const char *name;
  const decl_t *decl;
  ctx_struct_t *next;
};

typedef struct ctx_fn ctx_fn_t;
struct ctx_fn {
  const decl_t *decl;
  ctx_fn_t *next;
};

typedef struct ctx_extern ctx_extern_t;
struct ctx_extern {
  const char *name;
  ctx_extern_t *next;
};

typedef struct ctx_defer ctx_defer_t;
struct ctx_defer {
  expr_t *expr;
  ctx_defer_t *next;
};

typedef struct ctx_reg_name ctx_reg_name_t;
struct ctx_reg_name {
  const char *ref;
  ctx_reg_name_t *next;
};

typedef struct {
  FILE *out;
  arena_t *arena;
  ctx_global_t *globals;
  ctx_global_t *globals_tail;
  ctx_str_t *strings;
  ctx_str_t *strings_tail;
  int strings_count;
  ctx_local_t *locals;
  ctx_local_t *locals_tail;
  ctx_reg_name_t *fn_regs;
  ctx_struct_t *structs;
  ctx_fn_t *fns;
  ctx_extern_t *externs;
  ctx_defer_t *defers;
  size_t defer_count;
  size_t loop_defer_base;

  unsigned int reg;
  unsigned int label;
  unsigned int loop_label;
  bool uses_str_eq;
  bool uses_memcpy;
  bool uses_memset;
  bool uses_abort;
  type_t fn_ret;
  const char *fn_name;

  const char *prefix;
  module_t *module;
} ctx_t;

static int intern_string(ctx_t *ctx, expr_t *node) {
  ctx_str_t *str = arena_alloc(ctx->arena, sizeof(ctx_str_t));
  str->node = node;
  str->id = ctx->strings_count++;
  str->next = NULL;
  if (ctx->strings_tail == NULL) {
    ctx->strings = str;
  } else {
    ctx->strings_tail->next = str;
  }
  ctx->strings_tail = str;
  return str->id;
}

static int string_index(ctx_t *ctx, expr_t *node) {
  for (ctx_str_t *str = ctx->strings; str != NULL; str = str->next) {
    if (str->node == node) {
      return str->id;
    }
  }

  return -1;
}

static ctx_global_t *find_global(ctx_t *ctx, const char *name) {
  for (ctx_global_t *global = ctx->globals; global != NULL;
       global = global->next) {
    if (strcmp(global->name, name) == 0) {
      return global;
    }
  }
  return NULL;
}

static void add_global(ctx_t *ctx, const char *name, type_t type) {
  ctx_global_t *global = arena_alloc(ctx->arena, sizeof(ctx_global_t));
  global->name = name;
  global->type = type;
  global->next = NULL;
  if (ctx->globals_tail == NULL) {
    ctx->globals = global;
  } else {
    ctx->globals_tail->next = global;
  }
  ctx->globals_tail = global;
}

static ctx_local_t *find_local(ctx_t *ctx, const char *name) {
  for (ctx_local_t *local = ctx->locals; local != NULL; local = local->next) {
    if (strcmp(local->name, name) == 0) {
      return local;
    }
  }
  return NULL;
}

static void add_local(ctx_t *ctx, const char *name, type_t type,
                      const char *ptr) {
  ctx_local_t *local = arena_alloc(ctx->arena, sizeof(ctx_local_t));
  local->name = name;
  local->type = type;
  local->ptr = ptr;
  local->next = ctx->locals;
  ctx->locals = local;
  if (ctx->locals_tail == NULL) {
    ctx->locals_tail = local;
  }
}

static void record_fn_reg(ctx_t *ctx, const char *ref) {
  ctx_reg_name_t *node = arena_alloc(ctx->arena, sizeof(ctx_reg_name_t));
  node->ref = ref;
  node->next = ctx->fn_regs;
  ctx->fn_regs = node;
}

static bool fn_reg_taken(ctx_t *ctx, const char *ref) {
  for (ctx_reg_name_t *node = ctx->fn_regs; node != NULL; node = node->next) {
    if (strcmp(node->ref, ref) == 0) {
      return true;
    }
  }
  return false;
}

static const char *claim_fn_reg(ctx_t *ctx, const char *name) {
  const char *ref = arena_format(ctx->arena, "%%%s", name);
  unsigned int n = 1;
  while (fn_reg_taken(ctx, ref)) {
    ref = arena_format(ctx->arena, "%%%s.%u", name, n++);
  }
  record_fn_reg(ctx, ref);
  return ref;
}

static bool is_aggregate(TypeKind kind) {
  return kind == TYPE_STRUCT || kind == TYPE_STR || kind == TYPE_ARRAY ||
         kind == TYPE_SLICE || kind == TYPE_OPTIONAL;
}

static const decl_t *find_struct(ctx_t *ctx, const char *name) {
  for (ctx_struct_t *s = ctx->structs; s != NULL; s = s->next) {
    if (strcmp(s->name, name) == 0) {
      return s->decl;
    }
  }
  return NULL;
}

static const decl_t *find_fn(ctx_t *ctx, const char *name) {
  for (ctx_fn_t *f = ctx->fns; f != NULL; f = f->next) {
    if (strcmp(f->decl->name, name) == 0) {
      return f->decl;
    }
  }
  return NULL;
}

static const char *module_stem(const char *path, arena_t *arena) {
  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  const char *dot = strrchr(base, '.');
  size_t len = dot ? (size_t)(dot - base) : strlen(base);
  char *out = arena_alloc(arena, len + 1);
  memcpy(out, base, len);
  out[len] = '\0';
  return out;
}

static const char *mangle(ctx_t *ctx, const char *name) {
  if (ctx->prefix == NULL || ctx->prefix[0] == '\0') {
    return name;
  }
  return arena_format(ctx->arena, "%s.%s", ctx->prefix, name);
}

static const char *struct_ref(ctx_t *ctx, const char *name) {
  return strchr(name, '.') != NULL ? name : mangle(ctx, name);
}

static bool is_tagged_enum(ctx_t *ctx, type_t type) {
  if (type.kind != TYPE_ENUM || type.name == NULL) {
    return false;
  }
  const decl_t *d = find_struct(ctx, struct_ref(ctx, type.name));
  return d != NULL && d->kind == DECL_ENUM && d->enm.tagged;
}

static bool is_agg(ctx_t *ctx, type_t type) {
  return is_aggregate(type.kind) || is_tagged_enum(ctx, type);
}

static size_t round_up8(size_t n) { return (n + 7) / 8 * 8; }

static size_t type_size(ctx_t *ctx, type_t type) {
  switch (type.kind) {
  case TYPE_BOOL:
  case TYPE_I8:
  case TYPE_U8:
    return 1;
  case TYPE_I16:
  case TYPE_U16:
    return 2;
  case TYPE_I32:
  case TYPE_U32:
  case TYPE_F32:
    return 4;
  case TYPE_I64:
  case TYPE_U64:
  case TYPE_F64:
  case TYPE_CSTR:
    return 8;
  case TYPE_STR:
  case TYPE_SLICE:
    return 16;
  case TYPE_ARRAY:
    return type.array_length * type_size(ctx, *type.element);
  case TYPE_OPTIONAL:
    return 8 + round_up8(type_size(ctx, *type.element));
  case TYPE_STRUCT: {
    const decl_t *s = find_struct(ctx, struct_ref(ctx, type.name));
    if (s == NULL) {
      return 8;
    }
    size_t n = 0;
    for (field_t *f = s->strct.fields; f != NULL; f = f->next) {
      n += round_up8(type_size(ctx, f->type));
    }
    return n ? n : 8;
  }
  case TYPE_ENUM: {
    if (!is_tagged_enum(ctx, type)) {
      return 8;
    }
    const decl_t *e = find_struct(ctx, struct_ref(ctx, type.name));
    size_t max = 0;
    for (enum_member_t *m = e->enm.members; m != NULL; m = m->next) {
      if (m->has_payload) {
        size_t s = type_size(ctx, m->payload);
        if (s > max) {
          max = s;
        }
      }
    }
    return 8 + round_up8(max);
  }
  default:
    return 8;
  }
}

static size_t enum_payload_slots(ctx_t *ctx, const decl_t *enm);

static size_t c_abi_align(ctx_t *ctx, type_t type) {
  switch (type.kind) {
  case TYPE_BOOL:
  case TYPE_I8:
  case TYPE_U8:
    return 1;
  case TYPE_I16:
  case TYPE_U16:
    return 2;
  case TYPE_I32:
  case TYPE_U32:
  case TYPE_F32:
    return 4;
  case TYPE_ARRAY:
    return c_abi_align(ctx, *type.element);
  case TYPE_OPTIONAL: {
    size_t a = c_abi_align(ctx, *type.element);
    return a > 1 ? a : 1;
  }
  case TYPE_STRUCT: {
    const decl_t *s = find_struct(ctx, struct_ref(ctx, type.name));
    if (s == NULL) {
      return 8;
    }
    size_t align = 1;
    for (field_t *f = s->strct.fields; f != NULL; f = f->next) {
      size_t fa = c_abi_align(ctx, f->type);
      if (fa > align) {
        align = fa;
      }
    }
    return align;
  }
  default:
    return 8;
  }
}

static size_t c_abi_size(ctx_t *ctx, type_t type) {
  switch (type.kind) {
  case TYPE_BOOL:
  case TYPE_I8:
  case TYPE_U8:
    return 1;
  case TYPE_I16:
  case TYPE_U16:
    return 2;
  case TYPE_I32:
  case TYPE_U32:
  case TYPE_F32:
    return 4;
  case TYPE_STR:
  case TYPE_SLICE:
    return 16;
  case TYPE_ARRAY:
    return type.array_length * c_abi_size(ctx, *type.element);
  case TYPE_OPTIONAL: {
    size_t a = c_abi_align(ctx, type);
    size_t n = c_abi_align(ctx, *type.element) + c_abi_size(ctx, *type.element);
    return (n + a - 1) / a * a;
  }
  case TYPE_STRUCT: {
    const decl_t *s = find_struct(ctx, struct_ref(ctx, type.name));
    if (s == NULL) {
      return 8;
    }
    size_t off = 0;
    size_t align = 1;
    for (field_t *f = s->strct.fields; f != NULL; f = f->next) {
      size_t fa = c_abi_align(ctx, f->type);
      off = (off + fa - 1) / fa * fa;
      off += c_abi_size(ctx, f->type);
      if (fa > align) {
        align = fa;
      }
    }
    return (off + align - 1) / align * align;
  }
  case TYPE_ENUM:
    if (!is_tagged_enum(ctx, type)) {
      return 8;
    }
    return 8 + 8 * enum_payload_slots(
                       ctx, find_struct(ctx, struct_ref(ctx, type.name)));
  default:
    return 8;
  }
}

typedef struct {
  bool present[2];
  bool has_int[2];
  size_t end[2];
} cabi_scan_t;

static void cabi_mark(cabi_scan_t *s, size_t off, size_t size, bool is_float) {
  for (int k = 0; k < 2; k++) {
    size_t lo = (size_t)k * 8;
    size_t hi = lo + 8;
    if (off < hi && off + size > lo) {
      s->present[k] = true;
      if (!is_float) {
        s->has_int[k] = true;
      }
      size_t end = off + size < hi ? off + size : hi;
      if (end - lo > s->end[k]) {
        s->end[k] = end - lo;
      }
    }
  }
}

static bool cabi_scan_type(ctx_t *ctx, type_t type, size_t off,
                           cabi_scan_t *s) {
  switch (type.kind) {
  case TYPE_F32:
  case TYPE_F64:
    cabi_mark(s, off, c_abi_size(ctx, type), true);
    return true;
  case TYPE_BOOL:
  case TYPE_I8:
  case TYPE_U8:
  case TYPE_I16:
  case TYPE_U16:
  case TYPE_I32:
  case TYPE_U32:
  case TYPE_I64:
  case TYPE_U64:
  case TYPE_CSTR:
    cabi_mark(s, off, c_abi_size(ctx, type), false);
    return true;
  case TYPE_STR:
  case TYPE_SLICE:
    cabi_mark(s, off, 8, false);
    cabi_mark(s, off + 8, 8, false);
    return true;
  case TYPE_ARRAY: {
    size_t es = c_abi_size(ctx, *type.element);
    for (size_t i = 0; i < type.array_length; i++) {
      if (!cabi_scan_type(ctx, *type.element, off + i * es, s)) {
        return false;
      }
    }
    return true;
  }
  case TYPE_OPTIONAL:
    cabi_mark(s, off, 1, false);
    return cabi_scan_type(ctx, *type.element,
                          off + c_abi_align(ctx, *type.element), s);
  case TYPE_STRUCT: {
    const decl_t *st = find_struct(ctx, struct_ref(ctx, type.name));
    if (st == NULL) {
      return false;
    }
    size_t o = 0;
    for (field_t *f = st->strct.fields; f != NULL; f = f->next) {
      size_t fa = c_abi_align(ctx, f->type);
      o = (o + fa - 1) / fa * fa;
      if (!cabi_scan_type(ctx, f->type, off + o, s)) {
        return false;
      }
      o += c_abi_size(ctx, f->type);
    }
    return true;
  }
  case TYPE_ENUM:
    cabi_mark(s, off, c_abi_size(ctx, type), false);
    return true;
  default:
    return false;
  }
}

typedef struct {
  int n;
  const char *ty[2];
} cabi_t;

static cabi_t cabi_classify(ctx_t *ctx, type_t type) {
  cabi_t r = {0};
  if (type.kind != TYPE_STRUCT) {
    return r;
  }
  size_t size = c_abi_size(ctx, type);
  if (size == 0 || size > 16) {
    return r;
  }
  cabi_scan_t s = {0};
  if (!cabi_scan_type(ctx, type, 0, &s)) {
    return r;
  }
  r.n = size > 8 ? 2 : 1;
  for (int k = 0; k < r.n; k++) {
    if (s.present[k] && !s.has_int[k]) {
      r.ty[k] = s.end[k] <= 4 ? "float" : "double";
    } else {
      size_t used = s.present[k] ? s.end[k] : 8;
      r.ty[k] = arena_format(ctx->arena, "i%zu", used * 8);
    }
  }
  return r;
}

static const char *cabi_ret_type(ctx_t *ctx, cabi_t c) {
  if (c.n == 2) {
    return arena_format(ctx->arena, "{ %s, %s }", c.ty[0], c.ty[1]);
  }
  return c.ty[0];
}

static const char *cabi_load_args(ctx_t *ctx, const char *addr, cabi_t c) {
  unsigned int lo = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load %s, ptr %s, align 1\n", lo, c.ty[0], addr);
  if (c.n == 1) {
    return arena_format(ctx->arena, "%s %%%u", c.ty[0], lo);
  }
  unsigned int p = ctx->reg++;
  fprintf(ctx->out, "  %%%u = getelementptr i8, ptr %s, i64 8\n", p, addr);
  unsigned int hi = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load %s, ptr %%%u, align 1\n", hi, c.ty[1], p);
  return arena_format(ctx->arena, "%s %%%u, %s %%%u", c.ty[0], lo, c.ty[1], hi);
}

static size_t enum_payload_slots(ctx_t *ctx, const decl_t *enm) {
  size_t max = 0;
  for (enum_member_t *m = enm->enm.members; m != NULL; m = m->next) {
    if (m->has_payload) {
      size_t s = type_size(ctx, m->payload);
      if (s > max) {
        max = s;
      }
    }
  }
  size_t slots = round_up8(max) / 8;
  return slots ? slots : 1;
}

static decl_t *find_import(ctx_t *ctx, const char *alias) {
  if (ctx->module == NULL) {
    return NULL;
  }
  for (decl_t *decl = ctx->module->unit->root->container.members; decl != NULL;
       decl = decl->next) {
    if (decl->kind == DECL_IMPORT && strcmp(decl->import.alias, alias) == 0) {
      return decl;
    }
  }
  return NULL;
}

static const decl_t *module_fn(module_t *mod, const char *name) {
  for (decl_t *m = mod->unit->root->container.members; m != NULL; m = m->next) {
    if (m->kind == DECL_FN && m->visibility == VISIBILITY_PUBLIC &&
        strcmp(m->name, name) == 0) {
      return m;
    }
  }
  return NULL;
}

static module_t *module_path(ctx_t *ctx, expr_t *expr) {
  if (expr->kind == EXPR_ID) {
    decl_t *import = find_import(ctx, expr->id.name);
    return import != NULL ? import->import.resolved : NULL;
  }
  if (expr->kind != EXPR_FIELD) {
    return NULL;
  }
  module_t *base = module_path(ctx, expr->field.base);
  if (base == NULL) {
    return NULL;
  }
  for (decl_t *m = base->unit->root->container.members; m != NULL;
       m = m->next) {
    if (m->kind == DECL_IMPORT && m->visibility == VISIBILITY_PUBLIC &&
        strcmp(m->name, expr->field.name) == 0) {
      return m->import.resolved;
    }
  }
  return NULL;
}

static int field_index(const decl_t *strct, const char *name) {
  int i = 0;
  for (field_t *field = strct->strct.fields; field != NULL;
       field = field->next, i++) {
    if (strcmp(field->name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static field_t *find_field(const decl_t *strct, const char *name) {
  for (field_t *field = strct->strct.fields; field != NULL;
       field = field->next) {
    if (strcmp(field->name, name) == 0) {
      return field;
    }
  }
  return NULL;
}

// Re-encode decoded bytes into LLVM's c"..." form.
static void emit_escaped(FILE *out, const char *bytes, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char byte = (unsigned char)bytes[i];
    if (byte >= 0x20 && byte < 0x7F && byte != '"' && byte != '\\') {
      fputc(byte, out);
    } else {
      fprintf(out, "\\%02X", byte);
    }
  }
}

static void emit_string_globals(ctx_t *ctx) {
  for (ctx_str_t *str = ctx->strings; str != NULL; str = str->next) {
    size_t len = str->node->string.length + 1;
    fprintf(ctx->out, "@.str.%d = private unnamed_addr constant [%zu x i8] c\"",
            str->id, len);
    emit_escaped(ctx->out, str->node->string.value, str->node->string.length);
    fprintf(ctx->out, "\\00\"\n");
  }
}

static int collect_stmt(ctx_t *ctx, stmt_t *stmt);

static int collect_expr(ctx_t *ctx, expr_t *expr) {
  if (expr == NULL) {
    return 0;
  }
  switch (expr->kind) {
  case EXPR_STRING:
    intern_string(ctx, expr);
    return 0;
  case EXPR_CALL:
    if (collect_expr(ctx, expr->call.callee) != 0) {
      return 1;
    }
    for (expr_t *arg = expr->call.args; arg != NULL; arg = arg->next) {
      if (collect_expr(ctx, arg) != 0) {
        return 1;
      }
    }
    return 0;
  case EXPR_CAST:
    return collect_expr(ctx, expr->cast.operand);
  case EXPR_BINARY:
    if (expr->binary.lhs->type.kind == TYPE_STR &&
        (expr->binary.op == BINOP_EQ || expr->binary.op == BINOP_NE)) {
      ctx->uses_str_eq = true;
    }
    if (collect_expr(ctx, expr->binary.lhs) != 0) {
      return 1;
    }
    return collect_expr(ctx, expr->binary.rhs);
  case EXPR_UNARY:
    return collect_expr(ctx, expr->unary.operand);
  case EXPR_STRUCT_LITERAL:
    for (field_init_t *init = expr->struct_literal.inits; init != NULL;
         init = init->next) {
      if (collect_expr(ctx, init->value) != 0) {
        return 1;
      }
    }
    return 0;
  case EXPR_FIELD:
    return collect_expr(ctx, expr->field.base);
  case EXPR_ARRAY:
    for (expr_t *element = expr->array.elements; element != NULL;
         element = element->next) {
      if (collect_expr(ctx, element) != 0) {
        return 1;
      }
    }
    return 0;
  case EXPR_INDEX:
    if (collect_expr(ctx, expr->index.base) != 0) {
      return 1;
    }
    return collect_expr(ctx, expr->index.index);
  case EXPR_SLICE_RANGE:
    if (collect_expr(ctx, expr->slice_range.base) != 0 ||
        collect_expr(ctx, expr->slice_range.start) != 0) {
      return 1;
    }
    return collect_expr(ctx, expr->slice_range.end);
  case EXPR_IMPORT:
    NOT_IMPLEMENTED;
    return 1;
  case EXPR_MATCH:
    if (collect_expr(ctx, expr->match_expr.scrutinee) != 0) {
      return 1;
    }
    for (match_arm_t *arm = expr->match_expr.arms; arm != NULL;
         arm = arm->next) {
      if (arm->pattern != NULL && collect_expr(ctx, arm->pattern) != 0) {
        return 1;
      }
      if (arm->pattern_end != NULL &&
          collect_expr(ctx, arm->pattern_end) != 0) {
        return 1;
      }
      if (arm->value != NULL && collect_expr(ctx, arm->value) != 0) {
        return 1;
      }
      for (stmt_t *s = arm->body; s != NULL; s = s->next) {
        if (collect_stmt(ctx, s) != 0) {
          return 1;
        }
      }
    }
    return 0;
  case EXPR_COALESCE:
    if (collect_expr(ctx, expr->coalesce.lhs) != 0) {
      return 1;
    }
    return collect_expr(ctx, expr->coalesce.rhs);
  case EXPR_UNWRAP:
    return collect_expr(ctx, expr->unwrap.operand);
  case EXPR_PROPAGATE:
    return collect_expr(ctx, expr->propagate.operand);
  case EXPR_TERNARY:
    if (collect_expr(ctx, expr->ternary.cond) != 0 ||
        collect_expr(ctx, expr->ternary.then) != 0) {
      return 1;
    }
    return collect_expr(ctx, expr->ternary.els);
  case EXPR_ENUM_LITERAL:
    return collect_expr(ctx, expr->enum_literal.payload);
  case EXPR_NUMBER:
  case EXPR_ID:
  case EXPR_BOOLEAN:
  case EXPR_NULL:
    return 0;
  }
  return 0;
}

static int collect_stmt(ctx_t *ctx, stmt_t *stmt) {
  switch (stmt->kind) {
  case STMT_RETURN:
    return collect_expr(ctx, stmt->ret.value);
  case STMT_EXPR:
    return collect_expr(ctx, stmt->expr_stmt.expr);
  case STMT_IF:
    collect_expr(ctx, stmt->if_stmt.cond);
    for (stmt_t *s = stmt->if_stmt.then_body; s != NULL; s = s->next) {
      if (collect_stmt(ctx, s) != 0) {
        return 1;
      }
    }

    for (stmt_t *s = stmt->if_stmt.else_body; s != NULL; s = s->next) {
      if (collect_stmt(ctx, s) != 0) {
        return 1;
      }
    }
    return 0;
  case STMT_BINDING:
    return collect_expr(ctx, stmt->binding.init);
  case STMT_ASSIGN:
    if (collect_expr(ctx, stmt->assign.target) != 0) {
      return 1;
    }
    return collect_expr(ctx, stmt->assign.value);
  case STMT_WHILE:
    collect_expr(ctx, stmt->while_loop.cond);
    for (stmt_t *s = stmt->while_loop.body; s != NULL; s = s->next) {
      if (collect_stmt(ctx, s) != 0) {
        return 1;
      }
    }
    return 0;
  case STMT_FOR:
    collect_expr(ctx, stmt->for_loop.start);
    collect_expr(ctx, stmt->for_loop.end);
    collect_expr(ctx, stmt->for_loop.iterable);
    for (stmt_t *s = stmt->for_loop.body; s != NULL; s = s->next) {
      if (collect_stmt(ctx, s) != 0) {
        return 1;
      }
    }
    return 0;
  case STMT_DEFER:
    return collect_expr(ctx, stmt->defer_stmt.expr);
  case STMT_BREAK:
  case STMT_CONTINUE:
    return 0;
  }
  return 0;
}

static int collect_decl(ctx_t *ctx, decl_t *decl) {
  if (decl == NULL) {
    return 1;
  }
  switch (decl->kind) {
  case DECL_CONTAINER:
    for (decl_t *member = decl->container.members; member != NULL;
         member = member->next) {
      if (collect_decl(ctx, member) != 0) {
        return 1;
      }
    }
    return 0;
  case DECL_FN: {
    ctx_fn_t *f = arena_alloc(ctx->arena, sizeof(ctx_fn_t));
    f->decl = decl;
    f->next = ctx->fns;
    ctx->fns = f;
    for (param_t *param = decl->fn.params; param != NULL; param = param->next) {
      collect_expr(ctx, param->default_value);
    }
    for (stmt_t *stmt = decl->fn.body; stmt != NULL; stmt = stmt->next) {
      if (collect_stmt(ctx, stmt) != 0) {
        return 1;
      }
    }
    return 0;
  }
  case DECL_GLOBAL:
    add_global(ctx, decl->name, decl->global.type);
    return collect_expr(ctx, decl->global.init);
  case DECL_STRUCT: {
    ctx_struct_t *s = arena_alloc(ctx->arena, sizeof(ctx_struct_t));
    s->name = mangle(ctx, decl->name);
    s->decl = decl;
    s->next = ctx->structs;
    ctx->structs = s;
    for (field_t *field = decl->strct.fields; field != NULL;
         field = field->next) {
      collect_expr(ctx, field->default_value);
    }
    for (decl_t *member = decl->strct.members; member != NULL;
         member = member->next) {
      for (param_t *param = member->fn.params; param != NULL;
           param = param->next) {
        collect_expr(ctx, param->default_value);
      }
      for (stmt_t *stmt = member->fn.body; stmt != NULL; stmt = stmt->next) {
        if (collect_stmt(ctx, stmt) != 0) {
          return 1;
        }
      }
    }
    return 0;
  }
  case DECL_ENUM: {
    ctx_struct_t *s = arena_alloc(ctx->arena, sizeof(ctx_struct_t));
    s->name = mangle(ctx, decl->name);
    s->decl = decl;
    s->next = ctx->structs;
    ctx->structs = s;
    for (decl_t *member = decl->enm.methods; member != NULL;
         member = member->next) {
      for (param_t *param = member->fn.params; param != NULL;
           param = param->next) {
        collect_expr(ctx, param->default_value);
      }
      for (stmt_t *stmt = member->fn.body; stmt != NULL; stmt = stmt->next) {
        if (collect_stmt(ctx, stmt) != 0) {
          return 1;
        }
      }
    }
    return 0;
  }
  case DECL_IMPORT:
    return 0;
  }
  return 0;
}

static value_t emit_call(ctx_t *ctx, expr_t *call, const char *sret_dest);
static void emit_struct_into(ctx_t *ctx, const char *dest, expr_t *expr);
static void emit_slice_from_array(ctx_t *ctx, const char *dest, expr_t *arr);
static void emit_aggregate_into(ctx_t *ctx, const char *dest, type_t target,
                                expr_t *expr);
static void emit_enum_literal_into(ctx_t *ctx, const char *dest, expr_t *expr);

static const char *zero_value(TypeKind kind) {
  switch (kind) {
  case TYPE_BOOL:
    return "false";
  case TYPE_F32:
  case TYPE_F64:
    return "0.0";
  case TYPE_CSTR:
    return "null";
  case TYPE_STR:
  case TYPE_SLICE:
  case TYPE_ARRAY:
  case TYPE_STRUCT:
  case TYPE_OPTIONAL:
    return "zeroinitializer";
  default:
    return "0";
  }
}

static unsigned type_bits(TypeKind kind) {
  switch (kind) {
  case TYPE_I8:
  case TYPE_U8:
    return 8;
  case TYPE_I16:
  case TYPE_U16:
    return 16;
  case TYPE_I32:
  case TYPE_U32:
  case TYPE_F32:
    return 32;
  case TYPE_I64:
  case TYPE_U64:
  case TYPE_F64:
    return 64;
  default:
    return 0;
  }
}

static value_t emit_logical(ctx_t *ctx, expr_t *expr);
static const char *ir_type(ctx_t *ctx, type_t type);
static value_t emit_value(ctx_t *ctx, expr_t *expr);
static value_t emit_match(ctx_t *ctx, expr_t *expr, const char *dest);
static void emit_defers(ctx_t *ctx, size_t base);

static const char *widen_index(ctx_t *ctx, value_t v) {
  if (type_bits(v.type.kind) >= 64) {
    return v.ref;
  }
  unsigned int reg = ctx->reg++;
  fprintf(ctx->out, "  %%%u = %s %s %s to i64\n", reg,
          type_is_signed_integer(v.type.kind) ? "sext" : "zext",
          type_kind_to_ir(v.type.kind), v.ref);
  return arena_format(ctx->arena, "%%%u", reg);
}

static const char *emit_addr(ctx_t *ctx, expr_t *expr) {
  switch (expr->kind) {
  case EXPR_ID: {
    ctx_local_t *local = find_local(ctx, expr->id.name);
    if (local != NULL) {
      return local->ptr;
    }
    ctx_global_t *global = find_global(ctx, expr->id.name);
    if (global != NULL) {
      return arena_format(ctx->arena, "@%s", global->name);
    }
    return arena_format(ctx->arena, "%%%s", expr->id.name);
  }
  case EXPR_FIELD: {
    const char *base = emit_addr(ctx, expr->field.base);
    if (base == NULL && is_aggregate(expr->field.base->type.kind)) {
      base = emit_value(ctx, expr->field.base).ref;
    }
    type_t base_type = expr->field.base->type;
    if (base_type.kind == TYPE_STR || base_type.kind == TYPE_SLICE) {
      int index = strcmp(expr->field.name, "ptr") == 0 ? 0 : 1;
      unsigned int reg = ctx->reg++;
      fprintf(ctx->out,
              "  %%%u = getelementptr { ptr, i64 }, ptr %s, i32 0, i32 %d\n",
              reg, base, index);
      return arena_format(ctx->arena, "%%%u", reg);
    }
    const char *sname = struct_ref(ctx, base_type.name);
    const decl_t *strct = find_struct(ctx, sname);
    int index = field_index(strct, expr->field.name);
    unsigned int reg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = getelementptr %%%s, ptr %s, i32 0, i32 %d\n",
            reg, sname, base, index);
    return arena_format(ctx->arena, "%%%u", reg);
  }
  case EXPR_INDEX: {
    const char *base = emit_addr(ctx, expr->index.base);
    if (base == NULL && is_aggregate(expr->index.base->type.kind)) {
      base = emit_value(ctx, expr->index.base).ref;
    }
    value_t idx = emit_value(ctx, expr->index.index);
    if (expr->index.base->type.kind == TYPE_SLICE ||
        expr->index.base->type.kind == TYPE_STR) {
      unsigned int g = ctx->reg++;
      fprintf(ctx->out,
              "  %%%u = getelementptr { ptr, i64 }, ptr %s, i32 0, i32 0\n", g,
              base);
      unsigned int p = ctx->reg++;
      fprintf(ctx->out, "  %%%u = load ptr, ptr %%%u\n", p, g);
      unsigned int reg = ctx->reg++;
      fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %%%u, %s %s\n", reg,
              ir_type(ctx, expr->type), p, type_kind_to_ir(idx.type.kind),
              idx.ref);
      return arena_format(ctx->arena, "%%%u", reg);
    }
    unsigned int reg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, %s %s\n", reg,
            ir_type(ctx, expr->index.base->type), base,
            type_kind_to_ir(idx.type.kind), idx.ref);
    return arena_format(ctx->arena, "%%%u", reg);
  }
  case EXPR_UNWRAP: {
    expr_t *inner = expr->unwrap.operand;
    const char *addr = emit_addr(ctx, inner);
    if (addr == NULL) {
      addr = emit_value(ctx, inner).ref;
    }
    const char *opt_ir = ir_type(ctx, inner->type);
    unsigned int id = ctx->label++;
    unsigned int f = ctx->reg++;
    fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i32 0\n", f,
            opt_ir, addr);
    unsigned int flag = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load i1, ptr %%%u\n", flag, f);
    fprintf(ctx->out,
            "  br i1 %%%u, label %%unwrap.some.%u, label %%unwrap.none.%u\n",
            flag, id, id);
    fprintf(ctx->out, "unwrap.none.%u:\n", id);
    ctx->uses_abort = true;
    fprintf(ctx->out, "  call void @abort()\n");
    fprintf(ctx->out, "  unreachable\n");
    fprintf(ctx->out, "unwrap.some.%u:\n", id);
    unsigned int pp = ctx->reg++;
    fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i32 1\n", pp,
            opt_ir, addr);
    return arena_format(ctx->arena, "%%%u", pp);
  }
  case EXPR_PROPAGATE: {
    expr_t *inner = expr->propagate.operand;
    const char *addr = emit_addr(ctx, inner);
    if (addr == NULL) {
      addr = emit_value(ctx, inner).ref;
    }
    const char *opt_ir = ir_type(ctx, inner->type);
    unsigned int id = ctx->label++;
    unsigned int f = ctx->reg++;
    fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i32 0\n", f,
            opt_ir, addr);
    unsigned int flag = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load i1, ptr %%%u\n", flag, f);
    fprintf(ctx->out,
            "  br i1 %%%u, label %%propagate.some.%u, label "
            "%%propagate.none.%u\n",
            flag, id, id);
    fprintf(ctx->out, "propagate.none.%u:\n", id);
    ctx->uses_memset = true;
    fprintf(ctx->out,
            "  call void @llvm.memset.p0.i64(ptr %%sret, i8 0, i64 ptrtoint "
            "(ptr getelementptr (%s, ptr null, i32 1) to i64), i1 false)\n",
            ir_type(ctx, ctx->fn_ret));
    emit_defers(ctx, 0);
    fprintf(ctx->out, "  ret void\n");
    fprintf(ctx->out, "propagate.some.%u:\n", id);
    unsigned int pp = ctx->reg++;
    fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i32 1\n", pp,
            opt_ir, addr);
    return arena_format(ctx->arena, "%%%u", pp);
  }
  default:
    return NULL;
  }
}

static value_t emit_value(ctx_t *ctx, expr_t *expr);

// TODO: Probably needs refatoring
static value_t emit_cast(ctx_t *ctx, expr_t *expr) {
  if (is_aggregate(expr->cast.operand->type.kind) &&
      is_aggregate(expr->cast.target.kind)) {
    const char *addr = emit_addr(ctx, expr->cast.operand);
    if (addr == NULL) {
      addr = emit_value(ctx, expr->cast.operand).ref;
    }
    return (value_t){.type = expr->cast.target, .ref = addr};
  }
  value_t operand = emit_value(ctx, expr->cast.operand);
  TypeKind from = operand.type.kind;
  TypeKind to = expr->cast.target.kind;
  if (from == TYPE_ENUM) {
    from = TYPE_I32;
  }
  if (to == TYPE_ENUM) {
    to = TYPE_I32;
  }
  int from_bits = type_bits(from);
  int to_bits = type_bits(to);

  if (type_is_float(from)) {
    if (type_is_integer(to)) {
      unsigned int reg = ctx->reg++;
      const char *op = type_is_signed_integer(to) ? "fptosi" : "fptoui";
      fprintf(ctx->out, "  %%%u = %s %s %s to %s\n", reg, op,
              type_kind_to_ir(from), operand.ref, type_kind_to_ir(to));
      return (value_t){.type = expr->cast.target,
                       .ref = arena_format(ctx->arena, "%%%u", reg)};
    }

    if (type_is_float(to)) {
      if (type_bits(from) == type_bits(to)) {
        return (value_t){.type = expr->cast.target, .ref = operand.ref};
      }

      const char *op = from_bits > to_bits ? "fptrunc" : "fpext";
      unsigned int reg = ctx->reg++;
      fprintf(ctx->out, "  %%%u = %s %s %s to %s\n", reg, op,
              type_kind_to_ir(from), operand.ref, type_kind_to_ir(to));
      return (value_t){
          .type = expr->cast.target,
          .ref = arena_format(ctx->arena, "%%%u", reg),
      };
    }
  } else if (type_is_numeric(from)) {
    if (type_is_float(to)) {
      unsigned int reg = ctx->reg++;
      const char *op = type_is_signed_integer(from) ? "sitofp" : "uitofp";
      fprintf(ctx->out, "  %%%u = %s %s %s to %s\n", reg, op,
              type_kind_to_ir(from), operand.ref, type_kind_to_ir(to));
      return (value_t){
          .type = expr->cast.target,
          .ref = arena_format(ctx->arena, "%%%u", reg),
      };
    }

    if (from_bits == to_bits) {
      return (value_t){.type = expr->cast.target, .ref = operand.ref};
    }

    const char *op = from_bits > to_bits            ? "trunc"
                     : type_is_signed_integer(from) ? "sext"
                                                    : "zext";
    unsigned int reg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = %s %s %s to %s\n", reg, op,
            type_kind_to_ir(from), operand.ref, type_kind_to_ir(to));
    return (value_t){
        .type = expr->cast.target,
        .ref = arena_format(ctx->arena, "%%%u", reg),
    };
  }
  return (value_t){.type = expr->cast.target, .ref = operand.ref};
}

static const char *str_len(ctx_t *ctx, expr_t *op, const char *addr) {
  if (op->kind == EXPR_STRING) {
    return arena_format(ctx->arena, "%zu", op->string.length);
  }

  unsigned int len = ctx->reg++;
  fprintf(ctx->out,
          "  %%%u = getelementptr { ptr, i64 }, ptr %s, i32 0, i32 1\n", len,
          addr);
  unsigned int value = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load i64, ptr %%%u\n", value, len);
  return arena_format(ctx->arena, "%%%u", value);
}

static const char *str_ptr(ctx_t *ctx, expr_t *op, const char *addr) {
  if (op->kind == EXPR_STRING) {
    return arena_format(ctx->arena, "@.str.%d", string_index(ctx, op));
  }

  unsigned int len = ctx->reg++;
  fprintf(ctx->out,
          "  %%%u = getelementptr { ptr, i64 }, ptr %s, i32 0, i32 0\n", len,
          addr);
  unsigned int value = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load ptr, ptr %%%u\n", value, len);
  return arena_format(ctx->arena, "%%%u", value);
}

static value_t emit_str_equality(ctx_t *ctx, expr_t *expr) {
  expr_t *lhs = expr->binary.lhs;
  expr_t *rhs = expr->binary.rhs;

  const char *lhs_addr = lhs->kind == EXPR_STRING ? NULL : emit_addr(ctx, lhs);
  const char *rhs_addr = rhs->kind == EXPR_STRING ? NULL : emit_addr(ctx, rhs);

  const char *lhs_len = str_len(ctx, lhs, lhs_addr);
  const char *rhs_len = str_len(ctx, rhs, rhs_addr);
  unsigned int equal = ctx->reg++;
  fprintf(ctx->out, "  %%%u = icmp eq i64 %s, %s\n", equal, lhs_len, rhs_len);

  unsigned int result = ctx->reg++;
  fprintf(ctx->out, "  %%%u = alloca i1\n", result);
  fprintf(ctx->out, "  store i1 %%%u, ptr %%%u\n", equal, result);

  unsigned int label = ctx->label++;
  fprintf(ctx->out,
          "  br i1 %%%u, label %%streq.cmp.%u, label %%streq.end.%u\n", equal,
          label, label);

  fprintf(ctx->out, "streq.cmp.%u:\n", label);
  const char *lhs_ptr = str_ptr(ctx, lhs, lhs_addr);
  const char *rhs_ptr = str_ptr(ctx, rhs, rhs_addr);

  unsigned int cmp = ctx->reg++;
  fprintf(ctx->out, "  %%%u = call i32 @memcmp(ptr %s, ptr %s, i64 %s)\n", cmp,
          lhs_ptr, rhs_ptr, lhs_len);

  unsigned int zero = ctx->reg++;
  fprintf(ctx->out, "  %%%u = icmp eq i32 %%%u, 0\n", zero, cmp);
  fprintf(ctx->out, "  store i1 %%%u, ptr %%%u\n", zero, result);
  fprintf(ctx->out, "  br label %%streq.end.%u\n", label);

  fprintf(ctx->out, "streq.end.%u:\n", label);
  unsigned int loaded = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load i1, ptr %%%u\n", loaded, result);

  const char *ref = arena_format(ctx->arena, "%%%u", loaded);
  if (expr->binary.op == BINOP_NE) {
    unsigned int neg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = xor i1 %s, true\n", neg, ref);
    ref = arena_format(ctx->arena, "%%%u", neg);
  }

  return (value_t){
      .type = (type_t){.kind = TYPE_BOOL},
      .ref = ref,
  };
}

static value_t emit_value(ctx_t *ctx, expr_t *expr) {
  switch (expr->kind) {
  case EXPR_BOOLEAN:
    return (value_t){
        .type = expr->type,
        .ref = expr->boolean.value ? "true" : "false",
    };
  case EXPR_NUMBER:
    if (type_is_float(expr->type.kind)) {
      double d = strtod(expr->number.value, NULL);
      unsigned long long bits;
      memcpy(&bits, &d, sizeof(bits));
      return (value_t){
          .type = expr->type,
          .ref = arena_format(ctx->arena, "0x%016llX", bits),
      };
    }
    if (expr->number.value[0] == '0' &&
        (expr->number.value[1] == 'x' || expr->number.value[1] == 'X')) {
      return (value_t){
          .type = expr->type,
          .ref = arena_format(ctx->arena, "%llu",
                              strtoull(expr->number.value, NULL, 16)),
      };
    }
    return (value_t){
        .type = expr->type,
        .ref = expr->number.value,
    };
  case EXPR_STRING:
    return (value_t){
        .type = (type_t){.kind = TYPE_CSTR},
        .ref = arena_format(ctx->arena, "@.str.%d", string_index(ctx, expr)),
    };
  case EXPR_ID: {
    ctx_local_t *local = find_local(ctx, expr->id.name);
    if (local != NULL) {
      unsigned int reg = ctx->reg++;
      fprintf(ctx->out, "  %%%u = load %s, ptr %s\n", reg,
              type_kind_to_ir(local->type.kind), local->ptr);
      return (value_t){
          .type = local->type,
          .ref = arena_format(ctx->arena, "%%%u", reg),
      };
    }

    ctx_global_t *global = find_global(ctx, expr->id.name);
    if (global != NULL) {
      unsigned int reg = ctx->reg++;
      fprintf(ctx->out, "  %%%u = load %s, ptr @%s\n", reg,
              type_kind_to_ir(global->type.kind), global->name);
      return (value_t){
          .type = global->type,
          .ref = arena_format(ctx->arena, "%%%u", reg),
      };
    }

    return (value_t){
        .type = expr->type,
        .ref = arena_format(ctx->arena, "%%%s", expr->id.name),
    };
  }
  case EXPR_CALL:
    return emit_call(ctx, expr, NULL);
  case EXPR_CAST:
    return emit_cast(ctx, expr);
  case EXPR_NULL: {
    if (expr->type.kind == TYPE_CSTR) {
      return (value_t){.type = expr->type, .ref = "null"};
    }
    unsigned int slot = ctx->reg++;
    fprintf(ctx->out, "  %%%u = alloca %s\n", slot, ir_type(ctx, expr->type));
    fprintf(ctx->out, "  store %s zeroinitializer, ptr %%%u\n",
            ir_type(ctx, expr->type), slot);
    return (value_t){.type = expr->type,
                     .ref = arena_format(ctx->arena, "%%%u", slot)};
  }
  case EXPR_UNWRAP:
  case EXPR_PROPAGATE: {
    const char *addr = emit_addr(ctx, expr);
    if (is_agg(ctx, expr->type)) {
      return (value_t){.type = expr->type, .ref = addr};
    }
    unsigned int reg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load %s, ptr %s\n", reg,
            ir_type(ctx, expr->type), addr);
    return (value_t){.type = expr->type,
                     .ref = arena_format(ctx->arena, "%%%u", reg)};
  }
  case EXPR_COALESCE: {
    expr_t *lhs = expr->coalesce.lhs;
    const char *addr = emit_addr(ctx, lhs);
    if (addr == NULL) {
      addr = emit_value(ctx, lhs).ref;
    }
    unsigned int id = ctx->label++;
    const char *opt_ir = ir_type(ctx, lhs->type);
    const char *out_ir = ir_type(ctx, expr->type);
    bool agg = is_agg(ctx, expr->type);

    unsigned int slot = ctx->reg++;
    fprintf(ctx->out, "  %%%u = alloca %s\n", slot, out_ir);
    const char *dest = arena_format(ctx->arena, "%%%u", slot);

    unsigned int f = ctx->reg++;
    fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i32 0\n", f,
            opt_ir, addr);
    unsigned int flag = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load i1, ptr %%%u\n", flag, f);
    fprintf(
        ctx->out,
        "  br i1 %%%u, label %%coalesce.some.%u, label %%coalesce.none.%u\n",
        flag, id, id);

    fprintf(ctx->out, "coalesce.some.%u:\n", id);
    unsigned int pf = ctx->reg++;
    fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i32 1\n", pf,
            opt_ir, addr);
    unsigned int pv = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load %s, ptr %%%u\n", pv, out_ir, pf);
    fprintf(ctx->out, "  store %s %%%u, ptr %s\n", out_ir, pv, dest);
    fprintf(ctx->out, "  br label %%coalesce.end.%u\n", id);

    fprintf(ctx->out, "coalesce.none.%u:\n", id);
    if (agg) {
      emit_aggregate_into(ctx, dest, expr->type, expr->coalesce.rhs);
    } else {
      value_t rhs = emit_value(ctx, expr->coalesce.rhs);
      fprintf(ctx->out, "  store %s %s, ptr %s\n", out_ir, rhs.ref, dest);
    }
    fprintf(ctx->out, "  br label %%coalesce.end.%u\n", id);

    fprintf(ctx->out, "coalesce.end.%u:\n", id);
    if (agg) {
      return (value_t){.type = expr->type, .ref = dest};
    }
    unsigned int loaded = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load %s, ptr %s\n", loaded, out_ir, dest);
    return (value_t){.type = expr->type,
                     .ref = arena_format(ctx->arena, "%%%u", loaded)};
  }
  case EXPR_BINARY: {
    if (binop_is_logical(expr->binary.op)) {
      return emit_logical(ctx, expr);
    }

    if (expr->binary.lhs->type.kind == TYPE_OPTIONAL ||
        expr->binary.rhs->type.kind == TYPE_OPTIONAL) {
      expr_t *l = expr->binary.lhs;
      expr_t *r = expr->binary.rhs;
      expr_t *opt = l->kind == EXPR_NULL            ? r
                    : r->kind == EXPR_NULL          ? l
                    : l->type.kind == TYPE_OPTIONAL ? l
                                                    : r;
      expr_t *other = opt == l ? r : l;
      const char *addr = emit_addr(ctx, opt);
      if (addr == NULL) {
        addr = emit_value(ctx, opt).ref;
      }
      unsigned int f = ctx->reg++;
      fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i32 0\n", f,
              ir_type(ctx, opt->type), addr);
      unsigned int flag = ctx->reg++;
      fprintf(ctx->out, "  %%%u = load i1, ptr %%%u\n", flag, f);

      if (other->kind != EXPR_NULL) {
        const char *elem_ir = ir_type(ctx, *opt->type.element);
        unsigned int pp = ctx->reg++;
        fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i32 1\n",
                pp, ir_type(ctx, opt->type), addr);
        unsigned int pv = ctx->reg++;
        fprintf(ctx->out, "  %%%u = load %s, ptr %%%u\n", pv, elem_ir, pp);
        value_t o = emit_value(ctx, other);
        unsigned int eq = ctx->reg++;
        fprintf(ctx->out, "  %%%u = icmp eq %s %%%u, %s\n", eq, elem_ir, pv,
                o.ref);
        unsigned int both = ctx->reg++;
        fprintf(ctx->out, "  %%%u = and i1 %%%u, %%%u\n", both, flag, eq);
        if (expr->binary.op == BINOP_NE) {
          unsigned int inv = ctx->reg++;
          fprintf(ctx->out, "  %%%u = xor i1 %%%u, true\n", inv, both);
          return (value_t){.type = (type_t){.kind = TYPE_BOOL},
                           .ref = arena_format(ctx->arena, "%%%u", inv)};
        }
        return (value_t){.type = (type_t){.kind = TYPE_BOOL},
                         .ref = arena_format(ctx->arena, "%%%u", both)};
      }

      if (expr->binary.op == BINOP_EQ) {
        unsigned int inv = ctx->reg++;
        fprintf(ctx->out, "  %%%u = xor i1 %%%u, true\n", inv, flag);
        return (value_t){.type = (type_t){.kind = TYPE_BOOL},
                         .ref = arena_format(ctx->arena, "%%%u", inv)};
      }
      return (value_t){.type = (type_t){.kind = TYPE_BOOL},
                       .ref = arena_format(ctx->arena, "%%%u", flag)};
    }

    if (expr->binary.lhs->type.kind == TYPE_STR) {
      return emit_str_equality(ctx, expr);
    }

    value_t left = emit_value(ctx, expr->binary.lhs);
    value_t right = emit_value(ctx, expr->binary.rhs);

    const char *opcode =
        type_is_float(left.type.kind)
            ? binop_to_ir_float(expr->binary.op)
            : binop_to_ir(expr->binary.op,
                          type_is_signed_integer(left.type.kind));
    unsigned int reg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = %s %s %s, %s\n", reg, opcode,
            type_kind_to_ir(left.type.kind), left.ref, right.ref);
    return (value_t){
        .type = expr->type,
        .ref = arena_format(ctx->arena, "%%%u", reg),
    };
  }
  case EXPR_UNARY: {
    value_t operand = emit_value(ctx, expr->unary.operand);
    unsigned int reg = ctx->reg++;
    if (expr->unary.op == UNOP_NOT) {
      fprintf(ctx->out, "  %%%u = xor i1 %s, true\n", reg, operand.ref);
      return (value_t){
          .type = (type_t){.kind = TYPE_BOOL},
          .ref = arena_format(ctx->arena, "%%%u", reg),
      };
    }
    if (type_is_float(operand.type.kind)) {
      fprintf(ctx->out, "  %%%u = fneg %s %s\n", reg,
              type_kind_to_ir(operand.type.kind), operand.ref);
      return (value_t){
          .type = operand.type,
          .ref = arena_format(ctx->arena, "%%%u", reg),
      };
    }
    fprintf(ctx->out, "  %%%u = sub %s 0, %s\n", reg,
            type_kind_to_ir(operand.type.kind), operand.ref);
    return (value_t){
        .type = operand.type,
        .ref = arena_format(ctx->arena, "%%%u", reg),
    };
  }
  case EXPR_FIELD: {
    if (expr->field.base->type.kind == TYPE_ARRAY) {
      return (value_t){
          .type = (type_t){.kind = TYPE_I64},
          .ref = arena_format(ctx->arena, "%zu",
                              expr->field.base->type.array_length),
      };
    }
    const char *addr = emit_addr(ctx, expr);
    unsigned int reg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load %s, ptr %s\n", reg,
            ir_type(ctx, expr->type), addr);
    return (value_t){
        .type = expr->type,
        .ref = arena_format(ctx->arena, "%%%u", reg),
    };
  }
  case EXPR_ARRAY: {
    NOT_IMPLEMENTED;
    return (value_t){
        .type = (type_t){.kind = TYPE_UNKNOWN},
        .ref = NULL,
    };
  }
  case EXPR_INDEX: {
    const char *addr = emit_addr(ctx, expr);
    unsigned int reg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load %s, ptr %s\n", reg,
            ir_type(ctx, expr->type), addr);
    return (value_t){
        .type = expr->type,
        .ref = arena_format(ctx->arena, "%%%u", reg),
    };
  }
  case EXPR_SLICE_RANGE: {
    expr_t *base = expr->slice_range.base;
    const char *base_addr = NULL;
    const char *dataref = NULL;
    if (base->kind == EXPR_STRING) {
      dataref = arena_format(ctx->arena, "@.str.%d", string_index(ctx, base));
    } else if (base->type.kind == TYPE_CSTR) {
      dataref = emit_value(ctx, base).ref;
    } else {
      base_addr = emit_addr(ctx, base);
      if (base_addr == NULL) {
        base_addr = emit_value(ctx, base).ref;
      }
    }

    value_t start = emit_value(ctx, expr->slice_range.start);
    value_t end = emit_value(ctx, expr->slice_range.end);
    const char *s = widen_index(ctx, start);
    const char *e = widen_index(ctx, end);

    const char *elem_ir =
        base->type.kind == TYPE_STR || base->type.kind == TYPE_CSTR
            ? "i8"
            : ir_type(ctx, *base->type.element);
    if (base->type.kind == TYPE_ARRAY) {
      unsigned int p = ctx->reg++;
      fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i64 0, i64 %s\n", p,
              ir_type(ctx, base->type), base_addr, s);
      dataref = arena_format(ctx->arena, "%%%u", p);
    } else if (dataref == NULL) {
      unsigned int pfield = ctx->reg++;
      fprintf(ctx->out,
              "  %%%u = getelementptr { ptr, i64 }, ptr %s, i32 0, i32 0\n",
              pfield, base_addr);
      unsigned int loaded = ctx->reg++;
      fprintf(ctx->out, "  %%%u = load ptr, ptr %%%u\n", loaded, pfield);
      unsigned int p = ctx->reg++;
      fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %%%u, i64 %s\n", p,
              elem_ir, loaded, s);
      dataref = arena_format(ctx->arena, "%%%u", p);
    } else {
      unsigned int p = ctx->reg++;
      fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i64 %s\n", p,
              elem_ir, dataref, s);
      dataref = arena_format(ctx->arena, "%%%u", p);
    }

    unsigned int len = ctx->reg++;
    fprintf(ctx->out, "  %%%u = sub i64 %s, %s\n", len, e, s);
    unsigned int slot = ctx->reg++;
    fprintf(ctx->out, "  %%%u = alloca { ptr, i64 }\n", slot);
    unsigned int f0 = ctx->reg++;
    fprintf(ctx->out,
            "  %%%u = getelementptr { ptr, i64 }, ptr %%%u, i32 0, i32 0\n", f0,
            slot);
    fprintf(ctx->out, "  store ptr %s, ptr %%%u\n", dataref, f0);
    unsigned int f1 = ctx->reg++;
    fprintf(ctx->out,
            "  %%%u = getelementptr { ptr, i64 }, ptr %%%u, i32 0, i32 1\n", f1,
            slot);
    fprintf(ctx->out, "  store i64 %%%u, ptr %%%u\n", len, f1);
    return (value_t){.type = expr->type,
                     .ref = arena_format(ctx->arena, "%%%u", slot)};
  }
  case EXPR_MATCH:
    return emit_match(ctx, expr, NULL);
  case EXPR_TERNARY: {
    value_t cond = emit_value(ctx, expr->ternary.cond);
    unsigned int id = ctx->label++;
    bool is_void = expr->type.kind == TYPE_VOID;
    bool agg = is_agg(ctx, expr->type);
    const char *dest = NULL;
    if (!is_void) {
      unsigned int slot = ctx->reg++;
      fprintf(ctx->out, "  %%%u = alloca %s\n", slot, ir_type(ctx, expr->type));
      dest = arena_format(ctx->arena, "%%%u", slot);
    }
    fprintf(ctx->out,
            "  br i1 %s, label %%tern.then.%u, label %%tern.else.%u\n",
            cond.ref, id, id);

    fprintf(ctx->out, "tern.then.%u:\n", id);
    if (is_void) {
      emit_value(ctx, expr->ternary.then);
    } else if (agg) {
      emit_aggregate_into(ctx, dest, expr->type, expr->ternary.then);
    } else {
      value_t v = emit_value(ctx, expr->ternary.then);
      fprintf(ctx->out, "  store %s %s, ptr %s\n", ir_type(ctx, expr->type),
              v.ref, dest);
    }
    fprintf(ctx->out, "  br label %%tern.end.%u\n", id);

    fprintf(ctx->out, "tern.else.%u:\n", id);
    if (is_void) {
      emit_value(ctx, expr->ternary.els);
    } else if (agg) {
      emit_aggregate_into(ctx, dest, expr->type, expr->ternary.els);
    } else {
      value_t v = emit_value(ctx, expr->ternary.els);
      fprintf(ctx->out, "  store %s %s, ptr %s\n", ir_type(ctx, expr->type),
              v.ref, dest);
    }
    fprintf(ctx->out, "  br label %%tern.end.%u\n", id);

    fprintf(ctx->out, "tern.end.%u:\n", id);
    if (is_void) {
      return (value_t){.type = expr->type, .ref = NULL};
    }
    if (agg) {
      return (value_t){.type = expr->type, .ref = dest};
    }
    unsigned int loaded = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load %s, ptr %s\n", loaded,
            ir_type(ctx, expr->type), dest);
    return (value_t){.type = expr->type,
                     .ref = arena_format(ctx->arena, "%%%u", loaded)};
  }
  default:
    return (value_t){
        .type = (type_t){.kind = TYPE_UNKNOWN},
        .ref = NULL,
    };
  }
}

static value_t emit_logical(ctx_t *ctx, expr_t *expr) {
  unsigned int id = ctx->label++;
  unsigned int slot = ctx->reg++;
  const char *kw = binop_to_ir(
      expr->binary.op, type_is_signed_integer(expr->binary.lhs->type.kind));

  fprintf(ctx->out, "  %%%u = alloca i1\n", slot);

  value_t lhs = emit_value(ctx, expr->binary.lhs);
  fprintf(ctx->out, "  store i1 %s, ptr %%%u\n", lhs.ref, slot);

  if (expr->binary.op == BINOP_AND) {
    fprintf(ctx->out, "  br i1 %s, label %%%s.rhs.%u, label %%%s.end.%u\n",
            lhs.ref, kw, id, kw, id);
  } else {
    fprintf(ctx->out, "  br i1 %s, label %%%s.end.%u, label %%%s.rhs.%u\n",
            lhs.ref, kw, id, kw, id);
  }

  fprintf(ctx->out, "%s.rhs.%u:\n", kw, id);
  value_t rhs = emit_value(ctx, expr->binary.rhs);
  fprintf(ctx->out, "  store i1 %s, ptr %%%u\n", rhs.ref, slot);
  fprintf(ctx->out, "  br label %%%s.end.%u\n", kw, id);

  fprintf(ctx->out, "%s.end.%u:\n", kw, id);
  unsigned int result = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load i1, ptr %%%u\n", result, slot);

  return (value_t){
      .type = (type_t){.kind = TYPE_BOOL},
      .ref = arena_format(ctx->arena, "%%%u", result),
  };
}

static value_t emit_call(ctx_t *ctx, expr_t *call, const char *sret_dest) {
  bool is_method =
      call->call.callee->kind == EXPR_FIELD && !call->call.type_scoped;
  const char *name;
  const char *self = NULL;
  const decl_t *fn = NULL;
  if (call->call.type_scoped) {
    const char *tname = call->call.callee->field.base->type.name;
    const char *member = call->call.callee->field.name;
    const char *ref = struct_ref(ctx, tname);
    name = NULL;
    for (ctx_struct_t *s = ctx->structs; s != NULL; s = s->next) {
      if (strcmp(s->name, ref) == 0) {
        name = arena_format(ctx->arena, "%s.%s", s->name, member);
        decl_t *members = s->decl->kind == DECL_STRUCT ? s->decl->strct.members
                                                       : s->decl->enm.methods;
        for (decl_t *m = members; m != NULL; m = m->next) {
          if (m->kind == DECL_FN && strcmp(m->name, member) == 0) {
            fn = m;
            break;
          }
        }
        break;
      }
    }
    if (name == NULL) {
      name = mangle(ctx, arena_format(ctx->arena, "%s.%s", tname, member));
    }
  } else if (is_method) {
    expr_t *recv = call->call.callee->field.base;
    const char *member = call->call.callee->field.name;
    module_t *mod = module_path(ctx, recv);
    if (mod != NULL) {
      fn = module_fn(mod, member);
      const char *prefix = mod->prefix;
      if (fn != NULL && fn->fn.is_extern) {
        name = member;
      } else {
        name = (prefix && prefix[0])
                   ? arena_format(ctx->arena, "%s.%s", prefix, member)
                   : member;
      }
      is_method = false;
    } else {
      if (recv->type.kind == TYPE_ENUM && recv->type.name != NULL) {
        name = NULL;
        for (ctx_struct_t *s = ctx->structs; s != NULL; s = s->next) {
          if (s->decl->kind == DECL_ENUM &&
              strcmp(s->decl->name, recv->type.name) == 0) {
            name = arena_format(ctx->arena, "%s.%s", s->name, member);
            break;
          }
        }
        if (name == NULL) {
          name = mangle(
              ctx, arena_format(ctx->arena, "%s.%s", recv->type.name, member));
        }
      } else {
        name = arena_format(ctx->arena, "%s.%s",
                            struct_ref(ctx, recv->type.name), member);
      }
      self = emit_addr(ctx, recv);
      if (self == NULL) {
        value_t rv = emit_value(ctx, recv);
        if (is_agg(ctx, rv.type)) {
          self = rv.ref;
        } else {
          unsigned int slot = ctx->reg++;
          fprintf(ctx->out, "  %%%u = alloca %s\n", slot,
                  ir_type(ctx, rv.type));
          fprintf(ctx->out, "  store %s %s, ptr %%%u\n", ir_type(ctx, rv.type),
                  rv.ref, slot);
          self = arena_format(ctx->arena, "%%%u", slot);
        }
      }
    }
  } else {
    const char *callee = call->call.callee->id.name;
    fn = find_fn(ctx, callee);
    name = (fn != NULL && fn->fn.is_extern) ? callee : mangle(ctx, callee);
  }
  bool ret_struct = is_agg(ctx, call->type);
  bool extern_c = fn != NULL && fn->fn.is_extern;
  cabi_t retc = extern_c ? cabi_classify(ctx, call->type) : (cabi_t){0};

  const param_t *param = NULL;
  size_t param_count = 0;
  if (is_method) {
    expr_t *recv = call->call.callee->field.base;
    const decl_t *owner = NULL;
    if (recv->type.kind == TYPE_ENUM && recv->type.name != NULL) {
      for (ctx_struct_t *s = ctx->structs; s != NULL; s = s->next) {
        if (s->decl->kind == DECL_ENUM &&
            strcmp(s->decl->name, recv->type.name) == 0) {
          owner = s->decl;
          break;
        }
      }
    } else {
      owner = find_struct(ctx, struct_ref(ctx, recv->type.name));
    }
    if (owner != NULL) {
      decl_t *methods = owner->kind == DECL_STRUCT ? owner->strct.members
                                                   : owner->enm.methods;
      for (decl_t *m = methods; m != NULL; m = m->next) {
        if (m->kind == DECL_FN &&
            strcmp(m->name, call->call.callee->field.name) == 0) {
          param = m->fn.params;
          param_count = m->fn.params_count;
          if (param != NULL && param->is_self) {
            param = param->next;
            param_count--;
          }
          break;
        }
      }
    }
  } else if (fn != NULL) {
    param = fn->fn.params;
    param_count = fn->fn.params_count;
  }

  size_t n = call->call.arg_count;
  size_t cap = n > param_count ? n : param_count;
  if (cap < n + 1) {
    cap = n + 1;
  }
  const char **argref =
      arena_alloc(ctx->arena, sizeof(char *) * (cap ? cap : 1));
  type_t *argtype = arena_alloc(ctx->arena, sizeof(type_t) * (cap ? cap : 1));
  bool *argbyval = arena_alloc(ctx->arena, sizeof(bool) * (cap ? cap : 1));
  const char **argcoerce =
      arena_alloc(ctx->arena, sizeof(char *) * (cap ? cap : 1));
  for (size_t z = 0; z < (cap ? cap : 1); z++) {
    argcoerce[z] = NULL;
  }
  bool boxed_variadic = fn != NULL && fn->fn.variadic && !fn->fn.is_extern;
  size_t fixed = boxed_variadic ? fn->fn.params_count - 1 : 0;
  expr_t *rest = NULL;
  size_t i = 0;
  for (expr_t *arg = call->call.args; arg != NULL; arg = arg->next) {
    if (boxed_variadic && i == fixed) {
      rest = arg;
      break;
    }
    type_t ptype = param ? param->type : (type_t){.kind = TYPE_UNKNOWN};
    if (ptype.kind == TYPE_STR && arg->kind == EXPR_STRING) {
      unsigned int slot = ctx->reg++;
      fprintf(ctx->out, "  %%%u = alloca { ptr, i64 }\n", slot);
      const char *tmp = arena_format(ctx->arena, "%%%u", slot);
      emit_struct_into(ctx, tmp, arg);
      argref[i] = tmp;
      argtype[i] = (type_t){.kind = TYPE_STR};
      argbyval[i] = true;
    } else if (arg->type.kind == TYPE_STR && arg->kind != EXPR_STRING &&
               (ptype.kind == TYPE_CSTR ||
                (fn != NULL && fn->fn.variadic && param == NULL))) {
      const char *addr = emit_addr(ctx, arg);
      if (addr == NULL) {
        addr = emit_value(ctx, arg).ref;
      }
      unsigned int field_addr = ctx->reg++;
      fprintf(ctx->out,
              "  %%%u = getelementptr { ptr, i64 }, ptr %s, i32 0, i32 0\n",
              field_addr, addr);
      unsigned int cstr = ctx->reg++;
      fprintf(ctx->out, "  %%%u = load ptr, ptr %%%u\n", cstr, field_addr);
      argref[i] = arena_format(ctx->arena, "%%%u", cstr);
      argtype[i] = (type_t){.kind = TYPE_CSTR};
      argbyval[i] = false;
    } else if (ptype.kind == TYPE_SLICE && arg->type.kind == TYPE_ARRAY) {
      unsigned int slot = ctx->reg++;
      fprintf(ctx->out, "  %%%u = alloca { ptr, i64 }\n", slot);
      const char *tmp = arena_format(ctx->arena, "%%%u", slot);
      emit_slice_from_array(ctx, tmp, arg);
      argref[i] = tmp;
      argtype[i] = ptype;
      argbyval[i] = true;
    } else if (ptype.kind == TYPE_OPTIONAL && arg->type.kind != TYPE_OPTIONAL) {
      unsigned int slot = ctx->reg++;
      fprintf(ctx->out, "  %%%u = alloca %s\n", slot, ir_type(ctx, ptype));
      const char *tmp = arena_format(ctx->arena, "%%%u", slot);
      emit_aggregate_into(ctx, tmp, ptype, arg);
      argref[i] = tmp;
      argtype[i] = ptype;
      argbyval[i] = true;
    } else if (is_agg(ctx, arg->type) && arg->kind != EXPR_STRING) {
      const char *addr = emit_addr(ctx, arg);
      if (addr == NULL) {
        unsigned int slot = ctx->reg++;
        fprintf(ctx->out, "  %%%u = alloca %s\n", slot,
                ir_type(ctx, arg->type));
        addr = arena_format(ctx->arena, "%%%u", slot);
        emit_aggregate_into(ctx, addr, arg->type, arg);
      }
      cabi_t pc = extern_c ? cabi_classify(ctx, arg->type) : (cabi_t){0};
      if (pc.n > 0) {
        argcoerce[i] = cabi_load_args(ctx, addr, pc);
      }
      argref[i] = addr;
      argtype[i] = arg->type;
      argbyval[i] = true;
    } else {
      value_t v = emit_value(ctx, arg);
      argref[i] = v.ref;
      argtype[i] = v.type;
      argbyval[i] = false;
    }
    if (param != NULL) {
      param = param->next;
    }
    i++;
  }

  for (; param != NULL && param->default_value != NULL; param = param->next) {
    expr_t *def = param->default_value;
    if (is_agg(ctx, param->type)) {
      type_t dt = def->type.kind != TYPE_UNKNOWN ? def->type : param->type;
      unsigned int slot = ctx->reg++;
      fprintf(ctx->out, "  %%%u = alloca %s\n", slot, ir_type(ctx, dt));
      const char *tmp = arena_format(ctx->arena, "%%%u", slot);
      emit_aggregate_into(ctx, tmp, dt, def);
      cabi_t pc = extern_c ? cabi_classify(ctx, dt) : (cabi_t){0};
      if (pc.n > 0) {
        argcoerce[i] = cabi_load_args(ctx, tmp, pc);
      }
      argref[i] = tmp;
      argtype[i] = dt;
      argbyval[i] = true;
    } else {
      value_t v = emit_value(ctx, def);
      argref[i] = v.ref;
      argtype[i] = v.type;
      argbyval[i] = false;
    }
    i++;
  }

  if (boxed_variadic) {
    const char *box = "{ i32, i64, double, { ptr, i64 } }";
    size_t vcount =
        call->call.arg_count > fixed ? call->call.arg_count - fixed : 0;
    unsigned int arr = ctx->reg++;
    fprintf(ctx->out, "  %%%u = alloca [%zu x %s]\n", arr, vcount, box);

    size_t j = 0;
    for (expr_t *arg = rest; arg != NULL; arg = arg->next, j++) {
      unsigned int el = ctx->reg++;
      fprintf(ctx->out,
              "  %%%u = getelementptr [%zu x %s], ptr %%%u, i64 0, i64 %zu\n",
              el, vcount, box, arr, j);

      TypeKind kind = arg->type.kind;
      int tag;
      if (kind == TYPE_STR) {
        tag = 3;
      } else if (kind == TYPE_BOOL) {
        tag = 4;
      } else if (type_is_float(kind)) {
        tag = 2;
      } else if (type_is_signed_integer(kind) || kind == TYPE_ENUM) {
        tag = 0;
      } else {
        tag = 1;
      }
      unsigned int kf = ctx->reg++;
      fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %%%u, i32 0, i32 0\n",
              kf, box, el);
      fprintf(ctx->out, "  store i32 %d, ptr %%%u\n", tag, kf);

      if (kind == TYPE_STR) {
        unsigned int sf = ctx->reg++;
        fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %%%u, i32 0, i32 3\n",
                sf, box, el);
        emit_aggregate_into(ctx, arena_format(ctx->arena, "%%%u", sf),
                            (type_t){.kind = TYPE_STR}, arg);
      } else if (type_is_float(kind)) {
        value_t v = emit_value(ctx, arg);
        const char *ref = v.ref;
        if (kind == TYPE_F32) {
          unsigned int ext = ctx->reg++;
          fprintf(ctx->out, "  %%%u = fpext float %s to double\n", ext, ref);
          ref = arena_format(ctx->arena, "%%%u", ext);
        }
        unsigned int ff = ctx->reg++;
        fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %%%u, i32 0, i32 2\n",
                ff, box, el);
        fprintf(ctx->out, "  store double %s, ptr %%%u\n", ref, ff);
      } else {
        value_t v = emit_value(ctx, arg);
        const char *ref = widen_index(ctx, v);
        unsigned int inf = ctx->reg++;
        fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %%%u, i32 0, i32 1\n",
                inf, box, el);
        fprintf(ctx->out, "  store i64 %s, ptr %%%u\n", ref, inf);
      }
    }

    unsigned int sl = ctx->reg++;
    fprintf(ctx->out, "  %%%u = alloca { ptr, i64 }\n", sl);
    unsigned int p0 = ctx->reg++;
    fprintf(ctx->out,
            "  %%%u = getelementptr { ptr, i64 }, ptr %%%u, i32 0, i32 0\n", p0,
            sl);
    fprintf(ctx->out, "  store ptr %%%u, ptr %%%u\n", arr, p0);
    unsigned int p1 = ctx->reg++;
    fprintf(ctx->out,
            "  %%%u = getelementptr { ptr, i64 }, ptr %%%u, i32 0, i32 1\n", p1,
            sl);
    fprintf(ctx->out, "  store i64 %zu, ptr %%%u\n", vcount, p1);

    argref[i] = arena_format(ctx->arena, "%%%u", sl);
    argtype[i] = (type_t){.kind = TYPE_SLICE};
    argbyval[i] = true;
    i++;
  }
  n = i;

  const char *dest = sret_dest;
  if (ret_struct && dest == NULL) {
    unsigned int slot = ctx->reg++;
    fprintf(ctx->out, "  %%%u = alloca %s\n", slot, ir_type(ctx, call->type));
    dest = arena_format(ctx->arena, "%%%u", slot);
  }

  value_t result;
  const char *ret_coerce = retc.n > 0 ? cabi_ret_type(ctx, retc) : NULL;
  const char *callee_type = NULL;
  if (fn != NULL && fn->fn.is_extern && fn->fn.variadic) {
    const char *params = "";
    const char *psep = "";
    for (const param_t *p = fn->fn.params; p != NULL; p = p->next) {
      const char *pt;
      if (is_agg(ctx, p->type)) {
        cabi_t pc = cabi_classify(ctx, p->type);
        if (pc.n == 1) {
          pt = pc.ty[0];
        } else if (pc.n == 2) {
          pt = arena_format(ctx->arena, "%s, %s", pc.ty[0], pc.ty[1]);
        } else {
          pt = "ptr";
        }
      } else {
        pt = ir_type(ctx, p->type);
      }
      params = arena_format(ctx->arena, "%s%s%s", params, psep, pt);
      psep = ", ";
    }
    callee_type =
        arena_format(ctx->arena, "%s (%s%s...)",
                     ret_coerce                     ? ret_coerce
                     : call->type.kind == TYPE_VOID ? "void"
                                                    : ir_type(ctx, call->type),
                     params, fn->fn.params_count > 0 ? ", " : "");
  }

  unsigned int retreg = 0;
  if (ret_struct && ret_coerce != NULL) {
    retreg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = call %s @%s(", retreg,
            callee_type ? callee_type : ret_coerce, name);
    result = (value_t){.type = call->type, .ref = dest};
  } else if (ret_struct || call->type.kind == TYPE_VOID) {
    fprintf(ctx->out, "  call %s @%s(", callee_type ? callee_type : "void",
            name);
    result = (value_t){.type = call->type, .ref = ret_struct ? dest : NULL};
  } else {
    unsigned int reg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = call %s @%s(", reg,
            callee_type ? callee_type : ir_type(ctx, call->type), name);
    result = (value_t){.type = call->type,
                       .ref = arena_format(ctx->arena, "%%%u", reg)};
  }

  const char *sep = "";
  if (ret_struct && ret_coerce == NULL) {
    fprintf(ctx->out, "ptr sret(%s) %s", ir_type(ctx, call->type), dest);
    sep = ", ";
  }
  if (is_method) {
    fprintf(ctx->out, "%sptr %s", sep, self);
    sep = ", ";
  }
  for (i = 0; i < n; i++) {
    if (argcoerce[i] != NULL) {
      fprintf(ctx->out, "%s%s", sep, argcoerce[i]);
    } else if (argbyval[i]) {
      fprintf(ctx->out, "%sptr byval(%s) %s", sep, ir_type(ctx, argtype[i]),
              argref[i]);
    } else {
      fprintf(ctx->out, "%s%s %s", sep, ir_type(ctx, argtype[i]), argref[i]);
    }
    sep = ", ";
  }
  fprintf(ctx->out, ")\n");

  if (ret_struct && ret_coerce != NULL) {
    if (retc.n == 1) {
      fprintf(ctx->out, "  store %s %%%u, ptr %s, align 1\n", retc.ty[0],
              retreg, dest);
    } else {
      unsigned int e0 = ctx->reg++;
      fprintf(ctx->out, "  %%%u = extractvalue %s %%%u, 0\n", e0, ret_coerce,
              retreg);
      fprintf(ctx->out, "  store %s %%%u, ptr %s, align 1\n", retc.ty[0], e0,
              dest);
      unsigned int p = ctx->reg++;
      fprintf(ctx->out, "  %%%u = getelementptr i8, ptr %s, i64 8\n", p, dest);
      unsigned int e1 = ctx->reg++;
      fprintf(ctx->out, "  %%%u = extractvalue %s %%%u, 1\n", e1, ret_coerce,
              retreg);
      fprintf(ctx->out, "  store %s %%%u, ptr %%%u, align 1\n", retc.ty[1], e1,
              p);
    }
  }
  return result;
}

static void emit_struct_into(ctx_t *ctx, const char *dest, expr_t *expr);

static void emit_slice_from_array(ctx_t *ctx, const char *dest, expr_t *arr) {
  const char *base;
  if (arr->kind == EXPR_ARRAY) {
    unsigned int slot = ctx->reg++;
    fprintf(ctx->out, "  %%%u = alloca %s\n", slot, ir_type(ctx, arr->type));
    base = arena_format(ctx->arena, "%%%u", slot);
    emit_struct_into(ctx, base, arr);
  } else {
    base = emit_addr(ctx, arr);
  }
  unsigned int p = ctx->reg++;
  fprintf(ctx->out,
          "  %%%u = getelementptr { ptr, i64 }, ptr %s, i32 0, i32 0\n", p,
          dest);
  fprintf(ctx->out, "  store ptr %s, ptr %%%u\n", base, p);
  unsigned int l = ctx->reg++;
  fprintf(ctx->out,
          "  %%%u = getelementptr { ptr, i64 }, ptr %s, i32 0, i32 1\n", l,
          dest);
  fprintf(ctx->out, "  store i64 %zu, ptr %%%u\n", arr->type.array_length, l);
}

static void emit_optional_wrap_into(ctx_t *ctx, const char *dest, type_t target,
                                    expr_t *expr) {
  const char *ir = ir_type(ctx, target);
  unsigned int flag = ctx->reg++;
  fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i32 0\n", flag,
          ir, dest);
  fprintf(ctx->out, "  store i1 true, ptr %%%u\n", flag);
  unsigned int payload = ctx->reg++;
  fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i32 1\n",
          payload, ir, dest);
  const char *pdest = arena_format(ctx->arena, "%%%u", payload);
  if (is_agg(ctx, *target.element)) {
    emit_aggregate_into(ctx, pdest, *target.element, expr);
  } else {
    value_t value = emit_value(ctx, expr);
    fprintf(ctx->out, "  store %s %s, ptr %s\n", ir_type(ctx, *target.element),
            value.ref, pdest);
  }
}

static void emit_aggregate_into(ctx_t *ctx, const char *dest, type_t target,
                                expr_t *expr) {
  if (target.kind == TYPE_OPTIONAL && expr->type.kind != TYPE_OPTIONAL) {
    emit_optional_wrap_into(ctx, dest, target, expr);
    return;
  }
  if (target.kind == TYPE_SLICE && expr->type.kind == TYPE_ARRAY) {
    emit_slice_from_array(ctx, dest, expr);
    return;
  }
  emit_struct_into(ctx, dest, expr);
}

static void emit_struct_into(ctx_t *ctx, const char *dest, expr_t *expr) {
  if (expr->kind == EXPR_STRING) {
    unsigned int p = ctx->reg++;
    fprintf(ctx->out,
            "  %%%u = getelementptr { ptr, i64 }, ptr %s, i32 0, i32 0\n", p,
            dest);
    fprintf(ctx->out, "  store ptr @.str.%d, ptr %%%u\n",
            string_index(ctx, expr), p);
    unsigned int l = ctx->reg++;
    fprintf(ctx->out,
            "  %%%u = getelementptr { ptr, i64 }, ptr %s, i32 0, i32 1\n", l,
            dest);
    fprintf(ctx->out, "  store i64 %zu, ptr %%%u\n", expr->string.length, l);
    return;
  }
  if (expr->kind == EXPR_ARRAY) {
    const char *arr = ir_type(ctx, expr->type);
    size_t i = 0;
    for (expr_t *e = expr->array.elements; e != NULL; e = e->next, i++) {
      unsigned int reg = ctx->reg++;
      fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i32 %zu\n",
              reg, arr, dest, i);
      const char *slot = arena_format(ctx->arena, "%%%u", reg);
      if (is_agg(ctx, e->type)) {
        emit_struct_into(ctx, slot, e);
      } else {
        value_t value = emit_value(ctx, e);
        fprintf(ctx->out, "  store %s %s, ptr %s\n", ir_type(ctx, value.type),
                value.ref, slot);
      }
    }
    return;
  }
  if (expr->kind == EXPR_STRUCT_LITERAL) {
    const char *sname = struct_ref(ctx, expr->type.name);
    const decl_t *strct = find_struct(ctx, sname);
    for (field_init_t *fi = expr->struct_literal.inits; fi != NULL;
         fi = fi->next) {
      field_t *field = find_field(strct, fi->name);
      int index = field_index(strct, fi->name);
      unsigned int reg = ctx->reg++;
      fprintf(ctx->out, "  %%%u = getelementptr %%%s, ptr %s, i32 0, i32 %d\n",
              reg, sname, dest, index);
      const char *slot = arena_format(ctx->arena, "%%%u", reg);
      if (is_agg(ctx, field->type)) {
        emit_aggregate_into(ctx, slot, field->type, fi->value);
      } else {
        value_t value = emit_value(ctx, fi->value);
        fprintf(ctx->out, "  store %s %s, ptr %s\n", ir_type(ctx, value.type),
                value.ref, slot);
      }
    }

    for (field_t *field = strct->strct.fields; field != NULL;
         field = field->next) {
      if (field->default_value == NULL) {
        continue;
      }
      bool provided = false;
      for (field_init_t *fi = expr->struct_literal.inits; fi != NULL;
           fi = fi->next) {
        if (strcmp(fi->name, field->name) == 0) {
          provided = true;
          break;
        }
      }
      if (provided) {
        continue;
      }
      int index = field_index(strct, field->name);
      unsigned int reg = ctx->reg++;
      fprintf(ctx->out, "  %%%u = getelementptr %%%s, ptr %s, i32 0, i32 %d\n",
              reg, sname, dest, index);
      const char *slot = arena_format(ctx->arena, "%%%u", reg);
      if (is_agg(ctx, field->type)) {
        emit_aggregate_into(ctx, slot, field->type, field->default_value);
      } else {
        value_t value = emit_value(ctx, field->default_value);
        fprintf(ctx->out, "  store %s %s, ptr %s\n", ir_type(ctx, value.type),
                value.ref, slot);
      }
    }
    return;
  }
  if (expr->kind == EXPR_ENUM_LITERAL) {
    emit_enum_literal_into(ctx, dest, expr);
    return;
  }
  if (expr->kind == EXPR_CALL) {
    emit_call(ctx, expr, dest);
    return;
  }
  if (expr->kind == EXPR_MATCH) {
    emit_match(ctx, expr, dest);
    return;
  }
  const char *src = emit_addr(ctx, expr);
  if (src == NULL) {
    src = emit_value(ctx, expr).ref;
  }
  if (expr->type.kind == TYPE_STRUCT || expr->type.kind == TYPE_ARRAY) {
    ctx->uses_memcpy = true;
    fprintf(ctx->out,
            "  call void @llvm.memcpy.p0.p0.i64(ptr %s, ptr %s, i64 ptrtoint "
            "(ptr getelementptr (%s, ptr null, i32 1) to i64), i1 false)\n",
            dest, src, ir_type(ctx, expr->type));
    return;
  }
  unsigned int reg = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load %s, ptr %s\n", reg, ir_type(ctx, expr->type),
          src);
  fprintf(ctx->out, "  store %s %%%u, ptr %s\n", ir_type(ctx, expr->type), reg,
          dest);
}

static enum_member_t *find_enum_member(const decl_t *enm, const char *name) {
  for (enum_member_t *m = enm->enm.members; m != NULL; m = m->next) {
    if (strcmp(m->name, name) == 0) {
      return m;
    }
  }
  return NULL;
}

static void emit_enum_literal_into(ctx_t *ctx, const char *dest, expr_t *expr) {
  const char *ename = struct_ref(ctx, expr->type.name);
  const decl_t *enm = find_struct(ctx, ename);
  enum_member_t *member = find_enum_member(enm, expr->enum_literal.name);

  unsigned int tagp = ctx->reg++;
  fprintf(ctx->out, "  %%%u = getelementptr %%%s, ptr %s, i32 0, i32 0\n", tagp,
          ename, dest);
  fprintf(ctx->out, "  store i32 %lld, ptr %%%u\n", member->value, tagp);

  if (expr->enum_literal.payload == NULL) {
    return;
  }
  unsigned int payp = ctx->reg++;
  fprintf(ctx->out, "  %%%u = getelementptr %%%s, ptr %s, i32 0, i32 1\n", payp,
          ename, dest);
  const char *pdest = arena_format(ctx->arena, "%%%u", payp);
  if (is_agg(ctx, member->payload)) {
    emit_aggregate_into(ctx, pdest, member->payload,
                        expr->enum_literal.payload);
  } else {
    value_t v = emit_value(ctx, expr->enum_literal.payload);
    fprintf(ctx->out, "  store %s %s, ptr %s\n", ir_type(ctx, member->payload),
            v.ref, pdest);
  }
}

static int emit_block(ctx_t *ctx, stmt_t *body, type_t ret, const char *fn);
static bool block_terminates(stmt_t *body);

static value_t emit_match(ctx_t *ctx, expr_t *expr, const char *dest) {
  unsigned int id = ctx->label++;
  bool is_void = expr->type.kind == TYPE_VOID;
  expr_t *scrutinee = expr->match_expr.scrutinee;

  const char *scrut_ref;
  const char *scrut_ir;
  const char *flag_ref = NULL;
  const char *tagged_addr = NULL;
  const decl_t *tagged_enm = NULL;
  const char *tagged_ename = NULL;
  TypeKind scrut_kind = scrutinee->type.kind == TYPE_OPTIONAL
                            ? scrutinee->type.element->kind
                            : scrutinee->type.kind;
  if (is_tagged_enum(ctx, scrutinee->type)) {
    tagged_ename = struct_ref(ctx, scrutinee->type.name);
    tagged_enm = find_struct(ctx, tagged_ename);
    tagged_addr = emit_addr(ctx, scrutinee);
    if (tagged_addr == NULL) {
      tagged_addr = emit_value(ctx, scrutinee).ref;
    }
    unsigned int tp = ctx->reg++;
    fprintf(ctx->out, "  %%%u = getelementptr %%%s, ptr %s, i32 0, i32 0\n", tp,
            tagged_ename, tagged_addr);
    unsigned int tv = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load i32, ptr %%%u\n", tv, tp);
    scrut_ref = arena_format(ctx->arena, "%%%u", tv);
    scrut_ir = "i32";
    scrut_kind = TYPE_I32;
  } else if (scrutinee->type.kind == TYPE_OPTIONAL) {
    const char *opt_ir = ir_type(ctx, scrutinee->type);
    const char *addr = emit_addr(ctx, scrutinee);
    if (addr == NULL) {
      addr = emit_value(ctx, scrutinee).ref;
    }
    unsigned int fp = ctx->reg++;
    fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i32 0\n", fp,
            opt_ir, addr);
    unsigned int fl = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load i1, ptr %%%u\n", fl, fp);
    unsigned int pp = ctx->reg++;
    fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i32 1\n", pp,
            opt_ir, addr);
    const char *elem_ir = ir_type(ctx, *scrutinee->type.element);
    unsigned int pv = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load %s, ptr %%%u\n", pv, elem_ir, pp);
    flag_ref = arena_format(ctx->arena, "%%%u", fl);
    scrut_ref = arena_format(ctx->arena, "%%%u", pv);
    scrut_ir = elem_ir;
  } else {
    value_t scrut = emit_value(ctx, scrutinee);
    scrut_ref = scrut.ref;
    scrut_ir = ir_type(ctx, scrut.type);
  }

  if (!is_void && dest == NULL) {
    unsigned int slot = ctx->reg++;
    fprintf(ctx->out, "  %%%u = alloca %s\n", slot, ir_type(ctx, expr->type));
    dest = arena_format(ctx->arena, "%%%u", slot);
  }

  fprintf(ctx->out, "  br label %%match.arm.%u.0\n", id);

  size_t i = 0;
  bool group_pending = false;
  for (match_arm_t *arm = expr->match_expr.arms; arm != NULL;
       arm = arm->next, i++) {
    fprintf(ctx->out, "match.arm.%u.%zu:\n", id, i);
    if (arm->or_next || (arm->pattern != NULL && arm->next != NULL)) {
      unsigned int cmp;
      if (arm->pattern->kind == EXPR_NULL) {
        cmp = ctx->reg++;
        fprintf(ctx->out, "  %%%u = xor i1 %s, true\n", cmp, flag_ref);
      } else if (arm->pattern_end != NULL) {
        value_t lo = emit_value(ctx, arm->pattern);
        value_t hi = emit_value(ctx, arm->pattern_end);
        bool sgn = type_is_signed_integer(scrut_kind);
        unsigned int ge = ctx->reg++;
        fprintf(ctx->out, "  %%%u = icmp %s %s %s, %s\n", ge,
                sgn ? "sge" : "uge", scrut_ir, scrut_ref, lo.ref);
        unsigned int le = ctx->reg++;
        fprintf(ctx->out, "  %%%u = icmp %s %s %s, %s\n", le,
                sgn ? "sle" : "ule", scrut_ir, scrut_ref, hi.ref);
        unsigned int within = ctx->reg++;
        fprintf(ctx->out, "  %%%u = and i1 %%%u, %%%u\n", within, ge, le);
        if (flag_ref != NULL) {
          cmp = ctx->reg++;
          fprintf(ctx->out, "  %%%u = and i1 %s, %%%u\n", cmp, flag_ref,
                  within);
        } else {
          cmp = within;
        }
      } else {
        const char *pat_ref;
        if (arm->pattern->kind == EXPR_ENUM_LITERAL) {
          enum_member_t *m =
              find_enum_member(tagged_enm, arm->pattern->enum_literal.name);
          pat_ref = arena_format(ctx->arena, "%lld", m->value);
        } else {
          pat_ref = emit_value(ctx, arm->pattern).ref;
        }
        unsigned int eq = ctx->reg++;
        fprintf(ctx->out, "  %%%u = icmp eq %s %s, %s\n", eq, scrut_ir,
                scrut_ref, pat_ref);
        if (flag_ref != NULL) {
          cmp = ctx->reg++;
          fprintf(ctx->out, "  %%%u = and i1 %s, %%%u\n", cmp, flag_ref, eq);
        } else {
          cmp = eq;
        }
      }
      size_t body_index = i;
      for (const match_arm_t *g = arm; g->or_next; g = g->next) {
        body_index++;
      }
      fprintf(ctx->out,
              "  br i1 %%%u, label %%match.body.%u.%zu, label "
              "%%match.arm.%u.%zu\n",
              cmp, id, body_index, id, i + 1);
      if (arm->or_next) {
        group_pending = true;
        continue;
      }
      fprintf(ctx->out, "match.body.%u.%zu:\n", id, i);
    } else if (group_pending) {
      fprintf(ctx->out, "  br label %%match.body.%u.%zu\n", id, i);
      fprintf(ctx->out, "match.body.%u.%zu:\n", id, i);
    }
    group_pending = false;

    ctx_local_t *saved_head = ctx->locals;
    ctx_local_t *saved_tail = ctx->locals_tail;
    if (arm->binding != NULL && tagged_enm != NULL) {
      enum_member_t *m =
          find_enum_member(tagged_enm, arm->pattern->enum_literal.name);
      unsigned int pp = ctx->reg++;
      fprintf(ctx->out, "  %%%u = getelementptr %%%s, ptr %s, i32 0, i32 1\n",
              pp, tagged_ename, tagged_addr);
      add_local(ctx, arm->binding, m->payload,
                arena_format(ctx->arena, "%%%u", pp));
    }

    if (arm->is_block) {
      emit_block(ctx, arm->body, ctx->fn_ret, ctx->fn_name);
      ctx->locals = saved_head;
      ctx->locals_tail = saved_tail;
      if (!block_terminates(arm->body)) {
        fprintf(ctx->out, "  br label %%match.end.%u\n", id);
      }
      continue;
    }
    if (is_void) {
      emit_value(ctx, arm->value);
    } else if (is_agg(ctx, expr->type)) {
      emit_aggregate_into(ctx, dest, expr->type, arm->value);
    } else {
      value_t value = emit_value(ctx, arm->value);
      fprintf(ctx->out, "  store %s %s, ptr %s\n", ir_type(ctx, expr->type),
              value.ref, dest);
    }
    ctx->locals = saved_head;
    ctx->locals_tail = saved_tail;
    fprintf(ctx->out, "  br label %%match.end.%u\n", id);
  }

  fprintf(ctx->out, "match.end.%u:\n", id);
  if (is_void) {
    return (value_t){.type = expr->type, .ref = NULL};
  }
  if (is_agg(ctx, expr->type)) {
    return (value_t){.type = expr->type, .ref = dest};
  }
  unsigned int reg = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load %s, ptr %s\n", reg, ir_type(ctx, expr->type),
          dest);
  return (value_t){.type = expr->type,
                   .ref = arena_format(ctx->arena, "%%%u", reg)};
}

static int emit_expr(ctx_t *ctx, expr_t *expr);

static void emit_defers(ctx_t *ctx, size_t base) {
  size_t remaining = ctx->defer_count - base;
  for (ctx_defer_t *d = ctx->defers; d != NULL && remaining > 0;
       d = d->next, remaining--) {
    emit_expr(ctx, d->expr);
  }
}

static void pop_defers(ctx_t *ctx, size_t base) {
  while (ctx->defer_count > base) {
    ctx->defers = ctx->defers->next;
    ctx->defer_count--;
  }
}

static int emit_return(ctx_t *ctx, stmt_t *stmt, type_t type,
                       const char *fn_name) {
  expr_t *value = stmt->ret.value;
  if (value == NULL) {
    if (type.kind != TYPE_VOID) {
      diag_error(NULL, stmt->line, stmt->col,
                 "non-void function '%s' must return a value", fn_name);
      return 1;
    }

    emit_defers(ctx, 0);
    fprintf(ctx->out, "  ret void\n");
    return 0;
  }

  if (is_agg(ctx, type)) {
    emit_aggregate_into(ctx, "%sret", type, value);
    emit_defers(ctx, 0);
    fprintf(ctx->out, "  ret void\n");
    return 0;
  }

  value_t val = emit_value(ctx, value);
  emit_defers(ctx, 0);
  fprintf(ctx->out, "  ret %s %s\n", ir_type(ctx, type), val.ref);
  return 0;
}

static int emit_expr(ctx_t *ctx, expr_t *expr) {
  if (expr == NULL) {
    return 1;
  }

  switch (expr->kind) {
  case EXPR_CALL:
    emit_call(ctx, expr, NULL);
    return 0;
  case EXPR_MATCH:
    emit_match(ctx, expr, NULL);
    return 0;
  case EXPR_TERNARY:
    emit_value(ctx, expr);
    return 0;
  default:
    NOT_IMPLEMENTED;
    return 1;
  }
}

static int emit_condition(ctx_t *ctx, stmt_t *stmt, type_t ret, const char *fn);

static bool block_terminates(stmt_t *body);

static bool stmt_terminates(stmt_t *stmt) {
  switch (stmt->kind) {
  case STMT_RETURN:
  case STMT_BREAK:
  case STMT_CONTINUE:
    return true;
  case STMT_IF:
    return stmt->if_stmt.else_body != NULL &&
           block_terminates(stmt->if_stmt.then_body) &&
           block_terminates(stmt->if_stmt.else_body);
  default:
    return false;
  }
}

static bool block_terminates(stmt_t *body) {
  for (stmt_t *s = body; s != NULL; s = s->next) {
    if (stmt_terminates(s)) {
      return true;
    }
  }
  return false;
}

static int emit_while(ctx_t *ctx, stmt_t *stmt, type_t ret, const char *fn);
static int emit_for(ctx_t *ctx, stmt_t *stmt, type_t ret, const char *fn);
static int emit_foreach(ctx_t *ctx, stmt_t *stmt, type_t ret, const char *fn);

static const char *lvalue_root_name(const expr_t *target) {
  while (target->kind == EXPR_FIELD) {
    target = target->field.base;
  }
  return target->kind == EXPR_ID ? target->id.name : NULL;
}

static bool stmt_assigns(stmt_t *body, const char *name) {
  for (stmt_t *s = body; s != NULL; s = s->next) {
    switch (s->kind) {
    case STMT_ASSIGN: {
      const char *root = lvalue_root_name(s->assign.target);
      if (root != NULL && strcmp(root, name) == 0) {
        return true;
      }
      break;
    }
    case STMT_IF:
      if (stmt_assigns(s->if_stmt.then_body, name) ||
          stmt_assigns(s->if_stmt.else_body, name)) {
        return true;
      }
      break;
    case STMT_WHILE:
      if (stmt_assigns(s->while_loop.body, name)) {
        return true;
      }
      break;
    case STMT_FOR:
      if (stmt_assigns(s->for_loop.body, name)) {
        return true;
      }
      break;
    default:
      break;
    }
  }
  return false;
}

static int emit_block(ctx_t *ctx, stmt_t *body, type_t ret, const char *fn) {
  size_t defer_base = ctx->defer_count;
  bool terminated = false;
  for (stmt_t *stmt = body; stmt != NULL; stmt = stmt->next) {
    switch (stmt->kind) {
    case STMT_RETURN:
      if (emit_return(ctx, stmt, ret, fn) != 0) {
        return 1;
      }
      break;
    case STMT_EXPR:
      if (emit_expr(ctx, stmt->expr_stmt.expr) != 0) {
        return 1;
      }
      break;
    case STMT_IF:
      if (emit_condition(ctx, stmt, ret, fn) != 0) {
        return 1;
      }
      break;
    case STMT_BINDING: {
      const char *ptr = claim_fn_reg(ctx, stmt->binding.name);
      fprintf(ctx->out, "  %s = alloca %s\n", ptr,
              ir_type(ctx, stmt->binding.type));
      add_local(ctx, stmt->binding.name, stmt->binding.type, ptr);

      expr_t *init = stmt->binding.init;
      if (init == NULL) {
        if (stmt->binding.type.kind == TYPE_STRUCT ||
            stmt->binding.type.kind == TYPE_ARRAY) {
          ctx->uses_memset = true;
          fprintf(ctx->out,
                  "  call void @llvm.memset.p0.i64(ptr %s, i8 0, i64 ptrtoint "
                  "(ptr getelementptr (%s, ptr null, i32 1) to i64), i1 "
                  "false)\n",
                  ptr, ir_type(ctx, stmt->binding.type));
        } else {
          fprintf(ctx->out, "  store %s %s, ptr %s\n",
                  ir_type(ctx, stmt->binding.type),
                  is_agg(ctx, stmt->binding.type)
                      ? "zeroinitializer"
                      : zero_value(stmt->binding.type.kind),
                  ptr);
        }
      } else if (is_agg(ctx, stmt->binding.type)) {
        emit_aggregate_into(ctx, ptr, stmt->binding.type, init);
      } else {
        value_t value = emit_value(ctx, init);
        fprintf(ctx->out, "  store %s %s, ptr %s\n",
                type_kind_to_ir(stmt->binding.type.kind), value.ref, ptr);
      }
      break;
    }
    case STMT_ASSIGN: {
      const char *addr = emit_addr(ctx, stmt->assign.target);
      if (stmt->assign.coalesce) {
        unsigned int id = ctx->label++;
        const char *ir = ir_type(ctx, stmt->assign.target->type);
        unsigned int fp = ctx->reg++;
        fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i32 0\n",
                fp, ir, addr);
        unsigned int flag = ctx->reg++;
        fprintf(ctx->out, "  %%%u = load i1, ptr %%%u\n", flag, fp);
        fprintf(ctx->out,
                "  br i1 %%%u, label %%coal.end.%u, label %%coal.set.%u\n",
                flag, id, id);
        fprintf(ctx->out, "coal.set.%u:\n", id);
        emit_optional_wrap_into(ctx, addr, stmt->assign.target->type,
                                stmt->assign.value);
        fprintf(ctx->out, "  br label %%coal.end.%u\n", id);
        fprintf(ctx->out, "coal.end.%u:\n", id);
        break;
      }
      if (stmt->assign.compound) {
        TypeKind kind = stmt->assign.target->type.kind;
        const char *ir = ir_type(ctx, stmt->assign.target->type);
        unsigned int old = ctx->reg++;
        fprintf(ctx->out, "  %%%u = load %s, ptr %s\n", old, ir, addr);
        value_t value = emit_value(ctx, stmt->assign.value);
        const char *op =
            type_is_float(kind)
                ? binop_to_ir_float(stmt->assign.op)
                : binop_to_ir(stmt->assign.op, type_is_signed_integer(kind));
        unsigned int result = ctx->reg++;
        fprintf(ctx->out, "  %%%u = %s %s %%%u, %s\n", result, op, ir, old,
                value.ref);
        fprintf(ctx->out, "  store %s %%%u, ptr %s\n", ir, result, addr);
        break;
      }
      if (is_agg(ctx, stmt->assign.target->type)) {
        emit_aggregate_into(ctx, addr, stmt->assign.target->type,
                            stmt->assign.value);
        break;
      }
      value_t value = emit_value(ctx, stmt->assign.value);
      fprintf(ctx->out, "  store %s %s, ptr %s\n", ir_type(ctx, value.type),
              value.ref, addr);
      break;
    }
    case STMT_WHILE: {
      if (emit_while(ctx, stmt, ret, fn) != 0) {
        return 1;
      }
      break;
    }
    case STMT_FOR: {
      if (emit_for(ctx, stmt, ret, fn) != 0) {
        return 1;
      }
      break;
    }
    case STMT_BREAK: {
      emit_defers(ctx, ctx->loop_defer_base);
      fprintf(ctx->out, "  br label %%endwhile.%d\n", ctx->loop_label);
      break;
    }
    case STMT_CONTINUE: {
      emit_defers(ctx, ctx->loop_defer_base);
      fprintf(ctx->out, "  br label %%cond.%d\n", ctx->loop_label);
      break;
    }
    case STMT_DEFER: {
      ctx_defer_t *node = arena_alloc(ctx->arena, sizeof(ctx_defer_t));
      node->expr = stmt->defer_stmt.expr;
      node->next = ctx->defers;
      ctx->defers = node;
      ctx->defer_count++;
      break;
    }
    }

    if (stmt_terminates(stmt)) {
      terminated = true;
      break;
    }
  }

  if (!terminated) {
    emit_defers(ctx, defer_base);
  }
  pop_defers(ctx, defer_base);

  return 0;
}

static int emit_while(ctx_t *ctx, stmt_t *stmt, type_t ret, const char *fn) {
  if (stmt == NULL) {
    return 1;
  }

  unsigned int label = ctx->label++;
  unsigned int outer = ctx->loop_label;
  size_t outer_defer_base = ctx->loop_defer_base;
  ctx->loop_label = label;
  ctx->loop_defer_base = ctx->defer_count;

  fprintf(ctx->out, "  br label %%cond.%d\n", label);
  fprintf(ctx->out, "cond.%d:\n", label);
  value_t cond = emit_value(ctx, stmt->while_loop.cond);
  fprintf(ctx->out, "  br i1 %s, label %%body.%d, label %%endwhile.%d\n",
          cond.ref, label, label);
  fprintf(ctx->out, "body.%d:\n", label);
  if (emit_block(ctx, stmt->while_loop.body, ret, fn) != 0) {
    return 1;
  }
  if (!block_terminates(stmt->while_loop.body)) {
    fprintf(ctx->out, "  br label %%cond.%d\n", label);
  }
  fprintf(ctx->out, "endwhile.%d:\n", label);

  ctx->loop_label = outer;
  ctx->loop_defer_base = outer_defer_base;
  return 0;
}

static int emit_for(ctx_t *ctx, stmt_t *stmt, type_t ret, const char *fn) {
  if (stmt == NULL) {
    return 1;
  }

  if (stmt->for_loop.iterable != NULL) {
    return emit_foreach(ctx, stmt, ret, fn);
  }

  const char *type = type_kind_to_ir(stmt->for_loop.start->type.kind);
  const char *slot = claim_fn_reg(ctx, stmt->for_loop.var);
  fprintf(ctx->out, "  %s = alloca %s\n", slot, type);
  value_t start = emit_value(ctx, stmt->for_loop.start);
  fprintf(ctx->out, "  store %s %s, ptr %s\n", type, start.ref, slot);
  add_local(ctx, stmt->for_loop.var,
            (type_t){.kind = stmt->for_loop.start->type.kind}, slot);

  unsigned int label = ctx->label++;
  unsigned int outer = ctx->loop_label;
  size_t outer_defer_base = ctx->loop_defer_base;
  ctx->loop_label = label;
  ctx->loop_defer_base = ctx->defer_count;

  fprintf(ctx->out, "  br label %%cond.%d\n", label);
  fprintf(ctx->out, "cond.%d:\n", label);
  unsigned int current = ctx->reg++;
  fprintf(ctx->out, "  %%%d = load %s, ptr %s\n", current, type, slot);
  value_t end = emit_value(ctx, stmt->for_loop.end);
  unsigned int cmp = ctx->reg++;
  fprintf(ctx->out, "  %%%d = icmp slt %s %%%d, %s\n", cmp, type, current,
          end.ref);
  fprintf(ctx->out, "  br i1 %%%d, label %%body.%d, label %%endwhile.%d\n", cmp,
          label, label);

  fprintf(ctx->out, "body.%d:\n", label);
  if (emit_block(ctx, stmt->for_loop.body, ret, fn) != 0) {
    return 1;
  }

  if (!block_terminates(stmt->for_loop.body)) {
    unsigned int load = ctx->reg++;
    fprintf(ctx->out, "  %%%d = load %s, ptr %s\n", load, type, slot);
    unsigned int inc = ctx->reg++;
    fprintf(ctx->out, "  %%%d = add %s %%%d, 1\n", inc, type, load);
    fprintf(ctx->out, "  store %s %%%d, ptr %s\n", type, inc, slot);
    fprintf(ctx->out, "  br label %%cond.%d\n", label);
  }

  fprintf(ctx->out, "endwhile.%d:\n", label);
  ctx->loop_label = outer;
  ctx->loop_defer_base = outer_defer_base;
  return 0;
}

static int emit_foreach(ctx_t *ctx, stmt_t *stmt, type_t ret, const char *fn) {
  expr_t *iter = stmt->for_loop.iterable;
  bool is_slice = iter->type.kind == TYPE_SLICE;

  type_t elem = *iter->type.element;
  const char *elem_ir = ir_type(ctx, elem);

  const char *base;
  if (is_slice) {
    base = emit_addr(ctx, iter);
  } else if (iter->kind == EXPR_ARRAY) {
    unsigned int slot = ctx->reg++;
    fprintf(ctx->out, "  %%%u = alloca %s\n", slot, ir_type(ctx, iter->type));
    base = arena_format(ctx->arena, "%%%u", slot);
    emit_struct_into(ctx, base, iter);
  } else {
    base = emit_addr(ctx, iter);
  }

  const char *xslot = claim_fn_reg(ctx, stmt->for_loop.var);
  fprintf(ctx->out, "  %s = alloca %s\n", xslot, elem_ir);
  add_local(ctx, stmt->for_loop.var, elem, xslot);

  unsigned int index = ctx->reg++;
  fprintf(ctx->out, "  %%%u = alloca i64\n", index);
  fprintf(ctx->out, "  store i64 0, ptr %%%u\n", index);

  unsigned int label = ctx->label++;
  unsigned int outer = ctx->loop_label;
  size_t outer_defer_base = ctx->loop_defer_base;
  ctx->loop_label = label;
  ctx->loop_defer_base = ctx->defer_count;

  fprintf(ctx->out, "  br label %%cond.%u\n", label);
  fprintf(ctx->out, "cond.%u:\n", label);
  unsigned int cur = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load i64, ptr %%%u\n", cur, index);

  const char *len;
  if (is_slice) {
    unsigned int len_ptr = ctx->reg++;
    fprintf(ctx->out,
            "  %%%u = getelementptr { ptr, i64 }, ptr %s, i32 0, i32 1\n",
            len_ptr, base);
    unsigned int len_val = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load i64, ptr %%%u\n", len_val, len_ptr);
    len = arena_format(ctx->arena, "%%%u", len_val);
  } else {
    len = arena_format(ctx->arena, "%zu", iter->type.array_length);
  }

  unsigned int cmp = ctx->reg++;
  fprintf(ctx->out, "  %%%u = icmp slt i64 %%%u, %s\n", cmp, cur, len);
  fprintf(ctx->out, "  br i1 %%%u, label %%body.%u, label %%endwhile.%u\n", cmp,
          label, label);

  fprintf(ctx->out, "body.%u:\n", label);
  unsigned int bidx = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load i64, ptr %%%u\n", bidx, index);

  unsigned int elem_ptr;
  if (is_slice) {
    unsigned int data_ptr = ctx->reg++;
    fprintf(ctx->out,
            "  %%%u = getelementptr { ptr, i64 }, ptr %s, i32 0, i32 0\n",
            data_ptr, base);
    unsigned int data = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load ptr, ptr %%%u\n", data, data_ptr);
    elem_ptr = ctx->reg++;
    fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %%%u, i64 %%%u\n",
            elem_ptr, elem_ir, data, bidx);
  } else {
    elem_ptr = ctx->reg++;
    fprintf(ctx->out, "  %%%u = getelementptr %s, ptr %s, i32 0, i64 %%%u\n",
            elem_ptr, ir_type(ctx, iter->type), base, bidx);
  }

  unsigned int elem_val = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load %s, ptr %%%u\n", elem_val, elem_ir,
          elem_ptr);
  fprintf(ctx->out, "  store %s %%%u, ptr %s\n", elem_ir, elem_val, xslot);

  if (emit_block(ctx, stmt->for_loop.body, ret, fn) != 0) {
    return 1;
  }

  if (!block_terminates(stmt->for_loop.body)) {
    unsigned int load = ctx->reg++;
    fprintf(ctx->out, "  %%%u = load i64, ptr %%%u\n", load, index);
    unsigned int inc = ctx->reg++;
    fprintf(ctx->out, "  %%%u = add i64 %%%u, 1\n", inc, load);
    fprintf(ctx->out, "  store i64 %%%u, ptr %%%u\n", inc, index);
    fprintf(ctx->out, "  br label %%cond.%u\n", label);
  }

  fprintf(ctx->out, "endwhile.%u:\n", label);
  ctx->loop_label = outer;
  ctx->loop_defer_base = outer_defer_base;
  return 0;
}

static int emit_condition(ctx_t *ctx, stmt_t *stmt, type_t ret,
                          const char *fn) {
  if (stmt == NULL) {
    return 1;
  }

  bool has_else = stmt->if_stmt.else_body != NULL;
  bool then_term = block_terminates(stmt->if_stmt.then_body);
  bool else_term = has_else && block_terminates(stmt->if_stmt.else_body);
  bool merge = !has_else || !then_term || !else_term;

  value_t cond = emit_value(ctx, stmt->if_stmt.cond);
  unsigned int label = ctx->label++;
  fprintf(ctx->out, "  br i1 %s, label %%then.%u, label %%%s.%u\n", cond.ref,
          label, has_else ? "else" : "endif", label);

  fprintf(ctx->out, "then.%u:\n", label);
  if (emit_block(ctx, stmt->if_stmt.then_body, ret, fn) != 0) {
    return 1;
  }
  if (!then_term) {
    fprintf(ctx->out, "  br label %%endif.%u\n", label);
  }

  if (has_else) {
    fprintf(ctx->out, "else.%u:\n", label);
    if (emit_block(ctx, stmt->if_stmt.else_body, ret, fn) != 0) {
      return 1;
    }
    if (!else_term) {
      fprintf(ctx->out, "  br label %%endif.%u\n", label);
    }
  }

  if (merge) {
    fprintf(ctx->out, "endif.%u:\n", label);
  }

  return 0;
}

static const char *ir_type(ctx_t *ctx, type_t type) {
  if (type.kind == TYPE_STRUCT || is_tagged_enum(ctx, type)) {
    return arena_format(ctx->arena, "%%%s", struct_ref(ctx, type.name));
  }
  if (type.kind == TYPE_ARRAY) {
    return arena_format(ctx->arena, "[%zu x %s]", type.array_length,
                        ir_type(ctx, *type.element));
  }
  if (type.kind == TYPE_OPTIONAL) {
    return arena_format(ctx->arena, "{ i1, %s }", ir_type(ctx, *type.element));
  }
  return type_kind_to_ir(type.kind);
}

static int emit_extern_fn(ctx_t *ctx, decl_t *decl) {
  for (ctx_extern_t *e = ctx->externs; e != NULL; e = e->next) {
    if (strcmp(e->name, decl->name) == 0) {
      return 0;
    }
  }
  ctx_extern_t *node = arena_alloc(ctx->arena, sizeof(ctx_extern_t));
  node->name = decl->name;
  node->next = ctx->externs;
  ctx->externs = node;

  cabi_t retc = cabi_classify(ctx, decl->fn.return_type);
  bool sret = is_agg(ctx, decl->fn.return_type) && retc.n == 0;
  const char *ret_ty = retc.n > 0 ? cabi_ret_type(ctx, retc)
                       : sret     ? "void"
                                  : ir_type(ctx, decl->fn.return_type);
  fprintf(ctx->out, "declare %s @%s(", ret_ty, decl->name);

  const char *sep = "";
  if (sret) {
    fprintf(ctx->out, "ptr sret(%s)", ir_type(ctx, decl->fn.return_type));
    sep = ", ";
  }
  for (const param_t *param = decl->fn.params; param != NULL;
       param = param->next) {
    if (is_agg(ctx, param->type)) {
      cabi_t pc = cabi_classify(ctx, param->type);
      if (pc.n == 1) {
        fprintf(ctx->out, "%s%s", sep, pc.ty[0]);
      } else if (pc.n == 2) {
        fprintf(ctx->out, "%s%s, %s", sep, pc.ty[0], pc.ty[1]);
      } else {
        fprintf(ctx->out, "%sptr byval(%s)", sep, ir_type(ctx, param->type));
      }
    } else {
      fprintf(ctx->out, "%s%s", sep, ir_type(ctx, param->type));
    }
    sep = ", ";
  }
  if (decl->fn.variadic) {
    fprintf(ctx->out, "%s...", sep);
  }

  fprintf(ctx->out, ")\n");
  return 0;
}

static int emit_fn(ctx_t *ctx, decl_t *decl, const char *name) {
  ctx->reg = CTX_REG_START;
  ctx->label = CTX_LABEL_START;
  ctx->loop_label = 0;
  ctx->locals = NULL;
  ctx->locals_tail = NULL;
  ctx->fn_regs = NULL;
  ctx->defers = NULL;
  ctx->defer_count = 0;
  ctx->loop_defer_base = 0;
  ctx->fn_ret = decl->fn.return_type;
  ctx->fn_name = name;

  bool sret = is_agg(ctx, decl->fn.return_type);
  if (sret) {
    record_fn_reg(ctx, "%sret");
  }
  for (const param_t *param = decl->fn.params; param != NULL;
       param = param->next) {
    record_fn_reg(ctx, arena_format(ctx->arena, "%%%s", param->name));
  }
  fprintf(ctx->out, "define %s%s @%s(",
          decl->visibility == VISIBILITY_PRIVATE ? "internal " : "",
          sret ? "void" : ir_type(ctx, decl->fn.return_type),
          mangle(ctx, name));

  const char *sep = "";
  if (sret) {
    fprintf(ctx->out, "ptr sret(%s) %%sret",
            ir_type(ctx, decl->fn.return_type));
    sep = ", ";
  }
  for (const param_t *param = decl->fn.params; param != NULL;
       param = param->next) {
    if (param->is_self) {
      fprintf(ctx->out, "%sptr %%%s", sep, param->name);
    } else if (is_agg(ctx, param->type)) {
      fprintf(ctx->out, "%sptr byval(%s) %%%s", sep, ir_type(ctx, param->type),
              param->name);
    } else {
      fprintf(ctx->out, "%s%s %%%s", sep, ir_type(ctx, param->type),
              param->name);
    }
    sep = ", ";
  }

  fprintf(ctx->out, ") {\nentry:\n");

  for (const param_t *param = decl->fn.params; param != NULL;
       param = param->next) {
    if (param->is_self || is_agg(ctx, param->type)) {
      add_local(ctx, param->name, param->type,
                arena_format(ctx->arena, "%%%s", param->name));
      continue;
    }
    if (!param->mutable || !stmt_assigns(decl->fn.body, param->name)) {
      continue;
    }
    const char *slot = arena_format(ctx->arena, "%%%s.addr", param->name);
    record_fn_reg(ctx, slot);
    const char *type = type_kind_to_ir(param->type.kind);
    fprintf(ctx->out, "  %s = alloca %s\n", slot, type);
    fprintf(ctx->out, "  store %s %%%s, ptr %s\n", type, param->name, slot);
    add_local(ctx, param->name, param->type, slot);
  }

  if (emit_block(ctx, decl->fn.body, decl->fn.return_type, decl->name) != 0) {
    return 1;
  }

  if (decl->fn.return_type.kind == TYPE_VOID &&
      !block_terminates(decl->fn.body)) {
    fprintf(ctx->out, "  ret void\n");
  }

  fprintf(ctx->out, "}\n\n");
  return 0;
}

static int emit_entry_point(ctx_t *ctx, decl_t *decl) {
  if (decl->fn.params_count == 0) {
    return emit_fn(ctx, decl, decl->name);
  }

  const char *args_name = decl->fn.params->name;

  ctx->locals = NULL;
  ctx->locals_tail = NULL;
  ctx->fn_regs = NULL;
  ctx->defers = NULL;
  ctx->defer_count = 0;
  ctx->loop_defer_base = 0;
  ctx->fn_ret = decl->fn.return_type;
  ctx->fn_name = decl->name;
  record_fn_reg(ctx, "%argc");
  record_fn_reg(ctx, "%argv");
  const char *args_slot = claim_fn_reg(ctx, args_name);
  add_local(ctx, args_name, decl->fn.params->type, args_slot);

  if (find_fn(ctx, "strlen") == NULL) {
    fprintf(ctx->out, "declare i64 @strlen(ptr)\n");
  }
  fprintf(ctx->out, "define i32 @main(i32 %%argc, ptr %%argv) {\n");

  fprintf(ctx->out, "entry:\n");
  fprintf(ctx->out, "  %s = alloca { ptr, i64 }\n", args_slot);
  unsigned int argc = ctx->reg++;
  fprintf(ctx->out, "  %%%u = sext i32 %%argc to i64\n", argc);
  unsigned int strs = ctx->reg++;
  fprintf(ctx->out, "  %%%u = alloca { ptr, i64 }, i64 %%%u\n", strs, argc);
  unsigned int loop_index = ctx->reg++;
  fprintf(ctx->out, "  %%%u = alloca i64\n", loop_index);
  fprintf(ctx->out, "  store i64 0, ptr %%%u\n", loop_index);
  fprintf(ctx->out, "  br label %%args.cond\n");

  fprintf(ctx->out, "args.cond:\n");
  unsigned int idx = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load i64, ptr %%%u\n", idx, loop_index);
  unsigned int cond = ctx->reg++;
  fprintf(ctx->out, "  %%%u = icmp slt i64 %%%u, %%%u\n", cond, idx, argc);
  fprintf(ctx->out, "  br i1 %%%u, label %%args.body, label %%args.done\n",
          cond);

  fprintf(ctx->out, "args.body:\n");
  idx = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load i64, ptr %%%u\n", idx, loop_index);
  unsigned int argv_slot = ctx->reg++;
  fprintf(ctx->out, "  %%%u = getelementptr ptr, ptr %%argv, i64 %%%u\n",
          argv_slot, idx);
  unsigned int cstr = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load ptr, ptr %%%u\n", cstr, argv_slot);
  unsigned int len = ctx->reg++;
  fprintf(ctx->out, "  %%%u = call i64 @strlen(ptr %%%u)\n", len, cstr);
  unsigned int element = ctx->reg++;
  fprintf(ctx->out, "  %%%u = getelementptr { ptr, i64 }, ptr %%%u, i64 %%%u\n",
          element, strs, idx);
  unsigned int element_ptr = ctx->reg++;
  fprintf(ctx->out,
          "  %%%u = getelementptr { ptr, i64 }, ptr %%%u, i32 0, i32 0\n",
          element_ptr, element);
  fprintf(ctx->out, "  store ptr %%%u, ptr %%%u\n", cstr, element_ptr);
  unsigned int element_len = ctx->reg++;
  fprintf(ctx->out,
          "  %%%u = getelementptr { ptr, i64 }, ptr %%%u, i32 0, i32 1\n",
          element_len, element);
  fprintf(ctx->out, "  store i64 %%%u, ptr %%%u\n", len, element_len);
  idx = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load i64, ptr %%%u\n", idx, loop_index);
  unsigned int next = ctx->reg++;
  fprintf(ctx->out, "  %%%u = add i64 %%%u, 1\n", next, idx);
  fprintf(ctx->out, "  store i64 %%%u, ptr %%%u\n", next, loop_index);
  fprintf(ctx->out, "  br label %%args.cond\n");
  fprintf(ctx->out, "args.done:\n");

  unsigned int args_ptr = ctx->reg++;
  fprintf(ctx->out,
          "  %%%u = getelementptr { ptr, i64 }, ptr %s, i32 0, i32 0\n",
          args_ptr, args_slot);
  fprintf(ctx->out, "  store ptr %%%u, ptr %%%u\n", strs, args_ptr);
  unsigned int args_len = ctx->reg++;
  fprintf(ctx->out,
          "  %%%u = getelementptr { ptr, i64 }, ptr %s, i32 0, i32 1\n",
          args_len, args_slot);
  fprintf(ctx->out, "  store i64 %%%u, ptr %%%u\n", argc, args_len);

  if (emit_block(ctx, decl->fn.body, decl->fn.return_type, decl->name) != 0) {
    return 1;
  }

  fprintf(ctx->out, "}\n\n");
  return 0;
}

static int emit_decl(ctx_t *ctx, decl_t *decl) {
  if (decl == NULL) {
    return 1;
  }

  switch (decl->kind) {
  case DECL_CONTAINER: {
    for (decl_t *member = decl->container.members; member != NULL;
         member = member->next) {
      if (emit_decl(ctx, member) != 0) {
        return 1;
      }
    }
    return 0;
  }
  case DECL_FN: {
    if (decl->fn.is_extern) {
      return emit_extern_fn(ctx, decl);
    }
    return strcmp(decl->name, "main") == 0 ? emit_entry_point(ctx, decl)
                                           : emit_fn(ctx, decl, decl->name);
  }
  case DECL_STRUCT: {
    for (decl_t *member = decl->strct.members; member != NULL;
         member = member->next) {
      if (emit_fn(ctx, member,
                  arena_format(ctx->arena, "%s.%s", decl->name,
                               member->name)) != 0) {
        return 1;
      }
    }
    return 0;
  }
  case DECL_GLOBAL: {
    if (decl->global.init->kind == EXPR_NULL) {
      fprintf(ctx->out, "@%s = %s%s %s zeroinitializer\n", decl->name,
              decl->visibility == VISIBILITY_PRIVATE ? "internal " : "",
              decl->global.mutable ? "global" : "constant",
              ir_type(ctx, decl->global.type));
      return 0;
    }
    if (decl->global.init->kind == EXPR_STRING &&
        decl->global.type.kind == TYPE_STR) {
      fprintf(ctx->out, "@%s = %s%s { ptr, i64 } { ptr @.str.%d, i64 %zu }\n",
              decl->name,
              decl->visibility == VISIBILITY_PRIVATE ? "internal " : "",
              decl->global.mutable ? "global" : "constant",
              string_index(ctx, decl->global.init),
              decl->global.init->string.length);
      return 0;
    }
    value_t value = emit_value(ctx, decl->global.init);
    fprintf(ctx->out, "@%s = %s%s %s %s\n", decl->name,
            decl->visibility == VISIBILITY_PRIVATE ? "internal " : "",
            decl->global.mutable ? "global" : "constant",
            type_kind_to_ir(decl->global.type.kind), value.ref);
    return 0;
  }
  case DECL_ENUM: {
    for (decl_t *member = decl->enm.methods; member != NULL;
         member = member->next) {
      if (emit_fn(ctx, member,
                  arena_format(ctx->arena, "%s.%s", decl->name,
                               member->name)) != 0) {
        return 1;
      }
    }
    return 0;
  }
  case DECL_IMPORT:
    return 0;
  }
  return 0;
}

static void emit_struct_types(ctx_t *ctx, decl_t *decl) {
  if (decl->kind == DECL_CONTAINER) {
    for (decl_t *m = decl->container.members; m != NULL; m = m->next) {
      emit_struct_types(ctx, m);
    }
    return;
  }
  if (decl->kind == DECL_ENUM) {
    if (!decl->enm.tagged) {
      return;
    }
    fprintf(ctx->out, "%%%s = type { i32, [%zu x i64] }\n",
            mangle(ctx, decl->name), enum_payload_slots(ctx, decl));
    return;
  }
  if (decl->kind != DECL_STRUCT) {
    return;
  }
  fprintf(ctx->out, "%%%s = type {", mangle(ctx, decl->name));
  bool first = true;
  for (field_t *field = decl->strct.fields; field != NULL;
       field = field->next) {
    fprintf(ctx->out, "%s%s", first ? " " : ", ", ir_type(ctx, field->type));
    first = false;
  }
  fprintf(ctx->out, first ? "}\n" : " }\n");
}

int codegen_emit(FILE *out, compilation_t *compilation, arena_t *arena) {
  if (compilation == NULL) {
    return 1;
  }

  ctx_t ctx = {
      .out = out,
      .arena = arena,
      .reg = CTX_REG_START,
      .label = CTX_LABEL_START,
      .loop_label = 0,
  };

  for (module_t *module = compilation->modules; module != NULL;
       module = module->next) {
    module->prefix =
        (module == compilation->entry) ? "" : module_stem(module->path, arena);
  }

  for (module_t *module = compilation->modules; module != NULL;
       module = module->next) {
    ctx.prefix = module->prefix;
    if (collect_decl(&ctx, module->unit->root) != 0) {
      return 1;
    }
  }
  ctx.prefix = NULL;

  emit_string_globals(&ctx);

  if (ctx.uses_str_eq && find_fn(&ctx, "memcmp") == NULL) {
    fprintf(ctx.out, "declare i32 @memcmp(ptr, ptr, i64)\n");
  }

  for (module_t *module = compilation->modules; module != NULL;
       module = module->next) {
    ctx.prefix = module->prefix;
    emit_struct_types(&ctx, module->unit->root);
  }

  for (module_t *module = compilation->modules; module != NULL;
       module = module->next) {
    ctx.prefix = module->prefix;
    ctx.module = module;
    if (emit_decl(&ctx, module->unit->root) != 0) {
      return 1;
    }
  }

  if (ctx.uses_memcpy) {
    fprintf(ctx.out,
            "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n");
  }
  if (ctx.uses_memset) {
    fprintf(ctx.out, "declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)\n");
  }
  if (ctx.uses_abort && find_fn(&ctx, "abort") == NULL) {
    fprintf(ctx.out, "declare void @abort()\n");
  }

  return 0;
}
