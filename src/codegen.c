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
  ctx_struct_t *structs;
  ctx_fn_t *fns;

  unsigned int reg;
  unsigned int label;
  unsigned int loop_label;
  bool uses_str_eq;

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
  local->next = NULL;
  if (ctx->locals_tail == NULL) {
    ctx->locals = local;
  } else {
    ctx->locals_tail->next = local;
  }
  ctx->locals_tail = local;
}

static bool is_aggregate(TypeKind kind) {
  return kind == TYPE_STRUCT || kind == TYPE_STR || kind == TYPE_ARRAY ||
         kind == TYPE_SLICE;
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
  case EXPR_IMPORT:
    NOT_IMPLEMENTED;
    return 1;
  case EXPR_NUMBER:
  case EXPR_ID:
  case EXPR_BOOLEAN:
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
    return 0;
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
  case DECL_IMPORT:
    return 0;
  }
  return 0;
}

static value_t emit_call(ctx_t *ctx, expr_t *call, const char *sret_dest);
static void emit_struct_into(ctx_t *ctx, const char *dest, expr_t *expr);
static void emit_slice_from_array(ctx_t *ctx, const char *dest, expr_t *arr);

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
    value_t idx = emit_value(ctx, expr->index.index);
    if (expr->index.base->type.kind == TYPE_SLICE) {
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
  default:
    return NULL;
  }
}

static value_t emit_value(ctx_t *ctx, expr_t *expr);

// TODO: Probably needs refatoring
static value_t emit_cast(ctx_t *ctx, expr_t *expr) {
  value_t operand = emit_value(ctx, expr->cast.operand);
  TypeKind from = operand.type.kind;
  TypeKind to = expr->cast.target.kind;
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
  case EXPR_BINARY: {
    if (binop_is_logical(expr->binary.op)) {
      return emit_logical(ctx, expr);
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
  bool is_method = call->call.callee->kind == EXPR_FIELD;
  const char *name;
  const char *self = NULL;
  const decl_t *fn = NULL;
  if (is_method) {
    expr_t *recv = call->call.callee->field.base;
    const char *member = call->call.callee->field.name;
    decl_t *import =
        recv->kind == EXPR_ID ? find_import(ctx, recv->id.name) : NULL;
    if (import != NULL) {
      const char *prefix = import->import.resolved->prefix;
      name = (prefix && prefix[0])
                 ? arena_format(ctx->arena, "%s.%s", prefix, member)
                 : member;
      fn = module_fn(import->import.resolved, member);
      is_method = false;
    } else {
      name = mangle(ctx,
                    arena_format(ctx->arena, "%s.%s", recv->type.name, member));
      self = emit_addr(ctx, recv);
    }
  } else {
    const char *callee = call->call.callee->id.name;
    fn = find_fn(ctx, callee);
    name = (fn != NULL && fn->fn.is_extern) ? callee : mangle(ctx, callee);
  }
  bool ret_struct = is_aggregate(call->type.kind);

  const param_t *param = NULL;
  size_t param_count = 0;
  if (is_method) {
    expr_t *recv = call->call.callee->field.base;
    const decl_t *strct = find_struct(ctx, struct_ref(ctx, recv->type.name));
    if (strct != NULL) {
      for (decl_t *m = strct->strct.members; m != NULL; m = m->next) {
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
  const char **argref =
      arena_alloc(ctx->arena, sizeof(char *) * (cap ? cap : 1));
  type_t *argtype = arena_alloc(ctx->arena, sizeof(type_t) * (cap ? cap : 1));
  bool *argbyval = arena_alloc(ctx->arena, sizeof(bool) * (cap ? cap : 1));
  size_t i = 0;
  for (expr_t *arg = call->call.args; arg != NULL; arg = arg->next) {
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
    } else if (is_aggregate(arg->type.kind) && arg->kind != EXPR_STRING) {
      argref[i] = emit_addr(ctx, arg);
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
    if (param->type.kind == TYPE_STR && def->kind == EXPR_STRING) {
      unsigned int slot = ctx->reg++;
      fprintf(ctx->out, "  %%%u = alloca { ptr, i64 }\n", slot);
      const char *tmp = arena_format(ctx->arena, "%%%u", slot);
      emit_struct_into(ctx, tmp, def);
      argref[i] = tmp;
      argtype[i] = (type_t){.kind = TYPE_STR};
      argbyval[i] = true;
    } else {
      value_t v = emit_value(ctx, def);
      argref[i] = v.ref;
      argtype[i] = v.type;
      argbyval[i] = false;
    }
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
  if (ret_struct || call->type.kind == TYPE_VOID) {
    fprintf(ctx->out, "  call void @%s(", name);
    result = (value_t){.type = call->type, .ref = ret_struct ? dest : NULL};
  } else {
    unsigned int reg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = call %s @%s(", reg, ir_type(ctx, call->type),
            name);
    result = (value_t){.type = call->type,
                       .ref = arena_format(ctx->arena, "%%%u", reg)};
  }

  const char *sep = "";
  if (ret_struct) {
    fprintf(ctx->out, "ptr sret(%s) %s", ir_type(ctx, call->type), dest);
    sep = ", ";
  }
  if (is_method) {
    fprintf(ctx->out, "%sptr %s", sep, self);
    sep = ", ";
  }
  for (i = 0; i < n; i++) {
    if (argbyval[i]) {
      fprintf(ctx->out, "%sptr byval(%s) %s", sep, ir_type(ctx, argtype[i]),
              argref[i]);
    } else {
      fprintf(ctx->out, "%s%s %s", sep, ir_type(ctx, argtype[i]), argref[i]);
    }
    sep = ", ";
  }
  fprintf(ctx->out, ")\n");
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

static void emit_aggregate_into(ctx_t *ctx, const char *dest, type_t target,
                                expr_t *expr) {
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
      if (is_aggregate(e->type.kind)) {
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
      if (is_aggregate(field->type.kind)) {
        emit_struct_into(ctx, slot, fi->value);
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
      if (is_aggregate(field->type.kind)) {
        emit_struct_into(ctx, slot, field->default_value);
      } else {
        value_t value = emit_value(ctx, field->default_value);
        fprintf(ctx->out, "  store %s %s, ptr %s\n", ir_type(ctx, value.type),
                value.ref, slot);
      }
    }
    return;
  }
  if (expr->kind == EXPR_CALL) {
    emit_call(ctx, expr, dest);
    return;
  }
  const char *src = emit_addr(ctx, expr);
  unsigned int reg = ctx->reg++;
  fprintf(ctx->out, "  %%%u = load %s, ptr %s\n", reg, ir_type(ctx, expr->type),
          src);
  fprintf(ctx->out, "  store %s %%%u, ptr %s\n", ir_type(ctx, expr->type), reg,
          dest);
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

    fprintf(ctx->out, "  ret void\n");
    return 0;
  }

  if (is_aggregate(type.kind)) {
    emit_aggregate_into(ctx, "%sret", type, value);
    fprintf(ctx->out, "  ret void\n");
    return 0;
  }

  value_t val = emit_value(ctx, value);
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
      const char *ptr = arena_format(ctx->arena, "%%%s", stmt->binding.name);
      fprintf(ctx->out, "  %s = alloca %s\n", ptr,
              ir_type(ctx, stmt->binding.type));
      add_local(ctx, stmt->binding.name, stmt->binding.type, ptr);

      expr_t *init = stmt->binding.init;
      if (is_aggregate(stmt->binding.type.kind)) {
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
      if (is_aggregate(stmt->assign.target->type.kind)) {
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
      fprintf(ctx->out, "  br label %%endwhile.%d\n", ctx->loop_label);
      break;
    }
    case STMT_CONTINUE: {
      fprintf(ctx->out, "  br label %%cond.%d\n", ctx->loop_label);
      break;
    }
    }

    if (stmt_terminates(stmt)) {
      break;
    }
  }

  return 0;
}

static int emit_while(ctx_t *ctx, stmt_t *stmt, type_t ret, const char *fn) {
  if (stmt == NULL) {
    return 1;
  }

  unsigned int label = ctx->label++;
  unsigned int outer = ctx->loop_label;
  ctx->loop_label = label;

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
  const char *slot = arena_format(ctx->arena, "%%%s", stmt->for_loop.var);
  fprintf(ctx->out, "  %s = alloca %s\n", slot, type);
  value_t start = emit_value(ctx, stmt->for_loop.start);
  fprintf(ctx->out, "  store %s %s, ptr %s\n", type, start.ref, slot);
  add_local(ctx, stmt->for_loop.var,
            (type_t){.kind = stmt->for_loop.start->type.kind}, slot);

  unsigned int label = ctx->label++;
  unsigned int outer = ctx->loop_label;
  ctx->loop_label = label;

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

  const char *xslot = arena_format(ctx->arena, "%%%s", stmt->for_loop.var);
  fprintf(ctx->out, "  %s = alloca %s\n", xslot, elem_ir);
  add_local(ctx, stmt->for_loop.var, elem, xslot);

  unsigned int index = ctx->reg++;
  fprintf(ctx->out, "  %%%u = alloca i64\n", index);
  fprintf(ctx->out, "  store i64 0, ptr %%%u\n", index);

  unsigned int label = ctx->label++;
  unsigned int outer = ctx->loop_label;
  ctx->loop_label = label;

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
  if (type.kind == TYPE_STRUCT) {
    return arena_format(ctx->arena, "%%%s", struct_ref(ctx, type.name));
  }
  if (type.kind == TYPE_ARRAY) {
    return arena_format(ctx->arena, "[%zu x %s]", type.array_length,
                        ir_type(ctx, *type.element));
  }
  return type_kind_to_ir(type.kind);
}

static int emit_extern_fn(ctx_t *ctx, decl_t *decl) {
  bool sret = is_aggregate(decl->fn.return_type.kind);
  fprintf(ctx->out, "declare %s @%s(",
          sret ? "void" : ir_type(ctx, decl->fn.return_type), decl->name);

  const char *sep = "";
  if (sret) {
    fprintf(ctx->out, "ptr sret(%s)", ir_type(ctx, decl->fn.return_type));
    sep = ", ";
  }
  for (const param_t *param = decl->fn.params; param != NULL;
       param = param->next) {
    if (is_aggregate(param->type.kind)) {
      fprintf(ctx->out, "%sptr byval(%s)", sep, ir_type(ctx, param->type));
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

  bool sret = is_aggregate(decl->fn.return_type.kind);
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
    } else if (is_aggregate(param->type.kind)) {
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
    if (is_aggregate(param->type.kind)) {
      add_local(ctx, param->name, param->type,
                arena_format(ctx->arena, "%%%s", param->name));
      continue;
    }
    if (!param->mutable || !stmt_assigns(decl->fn.body, param->name)) {
      continue;
    }
    const char *slot = arena_format(ctx->arena, "%%%s.addr", param->name);
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

  if (find_fn(ctx, "strlen") == NULL) {
    fprintf(ctx->out, "declare i64 @strlen(ptr)\n");
  }
  fprintf(ctx->out, "define i32 @main(i32 %%argc, ptr %%argv) {\n");

  fprintf(ctx->out, "entry:\n");
  fprintf(ctx->out, "  %%%s = alloca { ptr, i64 }\n", args_name);
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
          "  %%%u = getelementptr { ptr, i64 }, ptr %%%s, i32 0, i32 0\n",
          args_ptr, args_name);
  fprintf(ctx->out, "  store ptr %%%u, ptr %%%u\n", strs, args_ptr);
  unsigned int args_len = ctx->reg++;
  fprintf(ctx->out,
          "  %%%u = getelementptr { ptr, i64 }, ptr %%%s, i32 0, i32 1\n",
          args_len, args_name);
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
    value_t value = emit_value(ctx, decl->global.init);
    fprintf(ctx->out, "@%s = %s%s %s %s\n", decl->name,
            decl->visibility == VISIBILITY_PRIVATE ? "internal " : "",
            decl->global.mutable ? "global" : "constant",
            type_kind_to_ir(decl->global.type.kind), value.ref);
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

  return 0;
}
