#include "codegen.h"
#include "arena.h"
#include "ast.h"
#include "debug.h"
#include "diag.h"
#include <stdbool.h>
#include <stdio.h>
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

  unsigned int reg;
  unsigned int label;
  unsigned int loop_label;
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
    if (collect_expr(ctx, expr->binary.lhs) != 0) {
      return 1;
    }
    return collect_expr(ctx, expr->binary.rhs);
  case EXPR_UNARY:
    return collect_expr(ctx, expr->unary.operand);
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
  case DECL_FN:
    for (stmt_t *stmt = decl->fn.body; stmt != NULL; stmt = stmt->next) {
      if (collect_stmt(ctx, stmt) != 0) {
        return 1;
      }
    }
    return 0;
  case DECL_GLOBAL:
    add_global(ctx, decl->name, decl->global.type);
    return 0;
  }
  return 0;
}

static value_t emit_call(ctx_t *ctx, expr_t *call);

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
    return 32;
  case TYPE_I64:
  case TYPE_U64:
    return 64;
  default:
    return 0;
  }
}

static value_t emit_logical(ctx_t *ctx, expr_t *expr);

static value_t emit_value(ctx_t *ctx, expr_t *expr) {
  switch (expr->kind) {
  case EXPR_BOOLEAN:
    return (value_t){
        .type = expr->type,
        .ref = expr->boolean.value ? "true" : "false",
    };
  case EXPR_NUMBER:
    return (value_t){
        .type = expr->type,
        .ref = expr->number.value,
    };
  case EXPR_STRING:
    return (value_t){
        .type = expr->type,
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
    return emit_call(ctx, expr);
  case EXPR_CAST: {
    value_t operand = emit_value(ctx, expr->cast.operand);
    TypeKind from = operand.type.kind;
    TypeKind to = expr->cast.target.kind;

    if (type_bits(to) == type_bits(from)) {
      return (value_t){.type = expr->cast.target, .ref = operand.ref};
    }

    const char *op = type_bits(to) < type_bits(from) ? "trunc"
                     : type_is_signed_integer(from)  ? "sext"
                                                     : "zext";
    unsigned int reg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = %s %s %s to %s\n", reg, op,
            type_kind_to_ir(from), operand.ref, type_kind_to_ir(to));
    return (value_t){
        .type = expr->cast.target,
        .ref = arena_format(ctx->arena, "%%%u", reg),
    };
  }
  case EXPR_BINARY: {
    if (binop_is_logical(expr->binary.op)) {
      return emit_logical(ctx, expr);
    }
    value_t left = emit_value(ctx, expr->binary.lhs);
    value_t right = emit_value(ctx, expr->binary.rhs);

    unsigned int reg = ctx->reg++;
    fprintf(
        ctx->out, "  %%%u = %s %s %s, %s\n", reg,
        binop_to_ir(expr->binary.op, type_is_signed_integer(left.type.kind)),
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
      // !x  ->  xor i1 x, true
      fprintf(ctx->out, "  %%%u = xor i1 %s, true\n", reg, operand.ref);
      return (value_t){
          .type = (type_t){.kind = TYPE_BOOL},
          .ref = arena_format(ctx->arena, "%%%u", reg),
      };
    }
    // -x  ->  sub <ty> 0, x
    fprintf(ctx->out, "  %%%u = sub %s 0, %s\n", reg,
            type_kind_to_ir(operand.type.kind), operand.ref);
    return (value_t){
        .type = operand.type,
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

static value_t emit_call(ctx_t *ctx, expr_t *call) {
  const char *name = call->call.callee->id.name;

  size_t n = call->call.arg_count;
  value_t *argv = arena_alloc(ctx->arena, sizeof(value_t) * (n ? n : 1));
  size_t i = 0;
  for (expr_t *arg = call->call.args; arg != NULL; arg = arg->next) {
    argv[i++] = emit_value(ctx, arg);
  }

  value_t result;
  if (call->type.kind == TYPE_VOID) {
    fprintf(ctx->out, "  call void @%s(", name);
    result = (value_t){
        .type = call->type,
        .ref = NULL,
    };
  } else {
    unsigned int reg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = call %s @%s(", reg,
            type_kind_to_ir(call->type.kind), name);
    result = (value_t){
        .type = call->type,
        .ref = arena_format(ctx->arena, "%%%u", reg),
    };
  }

  for (i = 0; i < n; i++) {
    fprintf(ctx->out, "%s%s %s", i ? ", " : "",
            type_kind_to_ir(argv[i].type.kind), argv[i].ref);
  }
  fprintf(ctx->out, ")\n");
  return result;
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

  value_t val = emit_value(ctx, value);
  fprintf(ctx->out, "  ret %s %s\n", type_kind_to_ir(type.kind), val.ref);
  return 0;
}

static int emit_expr(ctx_t *ctx, expr_t *expr) {
  if (expr == NULL) {
    return 1;
  }

  switch (expr->kind) {
  case EXPR_CALL:
    emit_call(ctx, expr);
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

static bool stmt_assigns(stmt_t *body, const char *name) {
  for (stmt_t *s = body; s != NULL; s = s->next) {
    switch (s->kind) {
    case STMT_ASSIGN:
      if (strcmp(s->assign.name, name) == 0) {
        return true;
      }
      break;
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
              type_kind_to_ir(stmt->binding.type.kind));
      value_t value = emit_value(ctx, stmt->binding.init);
      fprintf(ctx->out, "  store %s %s, ptr %s\n",
              type_kind_to_ir(stmt->binding.type.kind), value.ref, ptr);
      add_local(ctx, stmt->binding.name, stmt->binding.type, ptr);
      break;
    }
    case STMT_ASSIGN: {
      ctx_local_t *local = find_local(ctx, stmt->assign.name);
      if (local != NULL) {
        value_t value = emit_value(ctx, stmt->assign.value);
        fprintf(ctx->out, "  store %s %s, ptr %s\n",
                type_kind_to_ir(local->type.kind), value.ref, local->ptr);
        break;
      }

      ctx_global_t *global = find_global(ctx, stmt->assign.name);
      if (global != NULL) {
        value_t value = emit_value(ctx, stmt->assign.value);
        fprintf(ctx->out, "  store %s %s, ptr @%s\n",
                type_kind_to_ir(global->type.kind), value.ref, global->name);
        break;
      }
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

static int emit_extern_fn(ctx_t *ctx, decl_t *decl) {
  fprintf(ctx->out, "declare %s @%s(",
          type_kind_to_ir(decl->fn.return_type.kind), decl->name);

  for (const param_t *param = decl->fn.params; param != NULL;
       param = param->next) {
    fprintf(ctx->out, "%s%s", type_kind_to_ir(param->type.kind),
            param->next != NULL || decl->fn.variadic ? ", " : "");
    if (param->next == NULL && decl->fn.variadic) {
      fprintf(ctx->out, "...");
    }
  }

  fprintf(ctx->out, ")\n");
  return 0;
}

static int emit_fn(ctx_t *ctx, decl_t *decl) {
  ctx->reg = CTX_REG_START;
  ctx->label = CTX_LABEL_START;
  ctx->loop_label = 0;
  ctx->locals = NULL;
  ctx->locals_tail = NULL;
  fprintf(ctx->out, "define %s%s @%s(",
          decl->visibility == VISIBILITY_PRIVATE ? "internal " : "",
          type_kind_to_ir(decl->fn.return_type.kind), decl->name);
  for (const param_t *param = decl->fn.params; param != NULL;
       param = param->next) {
    fprintf(ctx->out, "%s %%%s%s", type_kind_to_ir(param->type.kind),
            param->name, param->next != NULL ? ", " : "");
  }

  fprintf(ctx->out, ") {\nentry:\n");

  for (const param_t *param = decl->fn.params; param != NULL;
       param = param->next) {
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
    return decl->fn.is_extern ? emit_extern_fn(ctx, decl) : emit_fn(ctx, decl);
  }
  case DECL_GLOBAL: {
    value_t value = emit_value(ctx, decl->global.init);
    fprintf(ctx->out, "@%s = %s%s %s %s\n", decl->name,
            decl->visibility == VISIBILITY_PRIVATE ? "internal " : "",
            decl->global.mutable ? "global" : "constant",
            type_kind_to_ir(decl->global.type.kind), value.ref);
    return 0;
  }
  }
  return 0;
}

int codegen_emit(FILE *out, unit_t *unit, arena_t *arena) {
  if (unit == NULL) {
    return 1;
  }

  ctx_t ctx = {
      .out = out,
      .arena = arena,
      .reg = CTX_REG_START,
      .label = CTX_LABEL_START,
      .loop_label = 0,
  };

  for (decl_t *decl = unit->root; decl != NULL; decl = decl->next) {
    if (collect_decl(&ctx, decl) != 0) {
      return 1;
    }
  }

  emit_string_globals(&ctx);

  for (decl_t *decl = unit->root; decl != NULL; decl = decl->next) {
    if (emit_decl(&ctx, decl) != 0) {
      return 1;
    }
  }

  return 0;
}
