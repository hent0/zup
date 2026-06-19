#include "codegen.h"
#include "arena.h"
#include "ast.h"
#include "debug.h"
#include "diag.h"
#include <stdio.h>

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

typedef struct {
  FILE *out;
  arena_t *arena;
  ctx_str_t *strings;
  ctx_str_t *strings_tail;
  int strings_count;

  unsigned int reg;
  unsigned int label;
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
  }
  return 0;
}

static int emit_std(ctx_t *ctx) {
  fprintf(ctx->out, "declare i32 @printf(ptr, ...)\n");
  emit_string_globals(ctx);
  fprintf(ctx->out, "\n");
  return 0;
}

static value_t emit_call(ctx_t *ctx, expr_t *call);

static unsigned type_bits(TypeKind kind) {
  switch (kind) {
  case TYPE_I8:
    return 8;
  case TYPE_I16:
    return 16;
  case TYPE_I32:
    return 32;
  case TYPE_I64:
    return 64;
  default:
    return 0;
  }
}

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
  case EXPR_ID:
    return (value_t){
        .type = expr->type,
        .ref = arena_format(ctx->arena, "%%%s", expr->id.name),
    };
  case EXPR_CALL:
    return emit_call(ctx, expr);
  case EXPR_CAST: {
    value_t operand = emit_value(ctx, expr->cast.operand);
    TypeKind from = operand.type.kind;
    TypeKind to = expr->cast.target.kind;

    if (type_bits(to) == type_bits(from)) {
      return (value_t){.type = expr->cast.target, .ref = operand.ref};
    }

    const char *op = type_bits(to) < type_bits(from) ? "trunc" : "sext";
    unsigned int reg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = %s %s %s to %s\n", reg, op,
            type_kind_to_ir(from), operand.ref, type_kind_to_ir(to));
    return (value_t){
        .type = expr->cast.target,
        .ref = arena_format(ctx->arena, "%%%u", reg),
    };
  }
  case EXPR_BINARY: {
    value_t left = emit_value(ctx, expr->binary.lhs);
    value_t right = emit_value(ctx, expr->binary.rhs);

    unsigned int reg = ctx->reg++;
    fprintf(ctx->out, "  %%%u = %s %s %s, %s\n", reg,
            binop_to_ir(expr->binary.op), type_kind_to_ir(left.type.kind),
            left.ref, right.ref);
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

    return 0; // emit_decl appends the trailing `ret void`
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
    }
  }

  return 0;
}

static int emit_condition(ctx_t *ctx, stmt_t *stmt, type_t ret,
                          const char *fn) {
  if (stmt == NULL) {
    return 1;
  }

  value_t cond = emit_value(ctx, stmt->if_stmt.cond);
  unsigned int label = ctx->label++;
  fprintf(ctx->out, "  br i1 %s, label %%then.%d, label %%%s.%d\n", cond.ref,
          label, stmt->if_stmt.else_body != NULL ? "else" : "endif", label);
  fprintf(ctx->out, "then.%d:\n", label);
  if (emit_block(ctx, stmt->if_stmt.then_body, ret, fn) != 0) {
    return 1;
  }

  if (stmt->if_stmt.else_body != NULL) {
    fprintf(ctx->out, "else.%d:\n", label);
    if (emit_block(ctx, stmt->if_stmt.else_body, ret, fn) != 0) {
      return 1;
    }
  } else {
    fprintf(ctx->out, "endif.%d:\n", label);
  }

  return 0;
}

static int emit_decl(ctx_t *ctx, decl_t *decl) {
  if (decl == NULL) {
    return 1;
  }

  switch (decl->kind) {
  case DECL_CONTAINER:
    for (decl_t *member = decl->container.members; member != NULL;
         member = member->next) {
      if (emit_decl(ctx, member) != 0) {
        return 1;
      }
    }
    return 0;
  case DECL_FN:
    ctx->reg = CTX_REG_START;
    ctx->label = CTX_LABEL_START;
    fprintf(ctx->out, "define %s%s @%s(",
            decl->visibility == VISIBILITY_PRIVATE ? "internal " : "",
            type_kind_to_ir(decl->fn.return_type.kind), decl->name);
    for (const param_t *param = decl->fn.params; param != NULL;
         param = param->next) {
      fprintf(ctx->out, "%s %%%s%s", type_kind_to_ir(param->type.kind),
              param->name, param->next != NULL ? ", " : "");
    }

    fprintf(ctx->out, ") {\nentry:\n");

    if (emit_block(ctx, decl->fn.body, decl->fn.return_type, decl->name) != 0) {
      return 1;
    }

    if (decl->fn.return_type.kind == TYPE_VOID) {
      fprintf(ctx->out, "  ret void\n");
    }

    fprintf(ctx->out, "}\n\n");
    return 0;
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
  };

  for (decl_t *decl = unit->root; decl != NULL; decl = decl->next) {
    if (collect_decl(&ctx, decl) != 0) {
      return 1;
    }
  }

  if (emit_std(&ctx) != 0) {
    return 1;
  }

  for (decl_t *decl = unit->root; decl != NULL; decl = decl->next) {
    if (emit_decl(&ctx, decl) != 0) {
      return 1;
    }
  }

  return 0;
}
