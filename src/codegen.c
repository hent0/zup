#include "codegen.h"
#include "arena.h"
#include "ast.h"
#include "debug.h"
#include "diag.h"
#include <stdio.h>

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

static int cg_string_index(ctx_t *ctx, expr_t *node) {
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
  case EXPR_NUMBER:
  case EXPR_ID:
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

static int emit_call(ctx_t *ctx, expr_t *expr) {
  if (expr == NULL) {
    return 1;
  }

  switch (expr->type.kind) {
  case TYPE_VOID:
    fprintf(ctx->out, "  call void @%s()\n", expr->call.callee->id.name);
    return 0;
  default:
    fprintf(ctx->out, "  %%%d = call %s @%s(", ctx->reg++,
            type_kind_to_str(expr->type.kind), expr->call.callee->id.name);
    for (expr_t *arg = expr->call.args; arg != NULL; arg = arg->next) {
      if (arg != expr->call.args) {
        fprintf(ctx->out, ", ");
      }
      switch (arg->kind) {
      case EXPR_STRING:
        fprintf(ctx->out, "ptr @.str.%d", cg_string_index(ctx, arg));
        break;
      default:
        NOT_IMPLEMENTED;
        return 1;
      }
    }
    fprintf(ctx->out, ")\n");
    return 0;
  }

  return 0;
}

static int emit_return(ctx_t *ctx, stmt_t *stmt, type_t type,
                       const char *fn_name) {
  expr_t *value = stmt->ret.value;

  if (value == NULL && type.kind != TYPE_VOID) {
    // TODO: Add source
    diag_error(NULL, stmt->line, stmt->col,
               "non-void function '%s' must return a value", fn_name);
    return 1;
  }

  switch (value->kind) {
  case EXPR_NUMBER:
    fprintf(ctx->out, "  ret %s %s\n", type_kind_to_str(type.kind),
            value->number.value);
    return 0;
  default:
    return 1;
  }
}

static int emit_expr(ctx_t *ctx, expr_t *expr) {
  if (expr == NULL) {
    return 1;
  }
  switch (expr->kind) {
  case EXPR_CALL:
    return emit_call(ctx, expr);
  default:
    NOT_IMPLEMENTED;
    return 1;
  }
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
    fprintf(ctx->out, "define %s%s @%s() {\n",
            decl->visibility == VISIBILITY_PRIVATE ? "internal " : "",
            type_kind_to_str(decl->fn.return_type.kind), decl->name);
    fprintf(ctx->out, "entry:\n");

    for (stmt_t *stmt = decl->fn.body; stmt != NULL; stmt = stmt->next) {
      switch (stmt->kind) {
      case STMT_RETURN:
        if (emit_return(ctx, stmt, decl->fn.return_type, decl->name) != 0) {
          return 1;
        }
        break;
      case STMT_EXPR:
        if (emit_expr(ctx, stmt->expr_stmt.expr) != 0) {
          return 1;
        }
        break;
      }
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
      .reg = 1,
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
