#include "ast.h"
#include "arena.h"
#include "token.h"
#include "utils.h"
#include <stdio.h>

char *type_kind_to_str(TypeKind kind) {
  switch (kind) {
  case TYPE_VOID:
    return "void";
  case TYPE_I8:
    return "i8";
  case TYPE_I16:
    return "i16";
  case TYPE_I32:
    return "i32";
  case TYPE_I64:
    return "i64";
  case TYPE_STRING:
    return "i8[]";
  default:
    return "?";
  }
}

expr_t *ast_number_init(token_t token, arena_t *arena) {
  expr_t *expr = arena_alloc(arena, sizeof(expr_t));
  expr->kind = EXPR_NUMBER;
  expr->line = token.line;
  expr->col = token.col;
  expr->type = (type_t){.kind = TYPE_UNKNOWN};
  expr->next = NULL;
  expr->number.value = token.value;
  return expr;
}

expr_t *ast_string_init(token_t token, arena_t *arena) {
  expr_t *expr = arena_alloc(arena, sizeof(expr_t));
  expr->kind = EXPR_STRING;
  expr->line = token.line;
  expr->col = token.col;
  expr->type = (type_t){.kind = TYPE_UNKNOWN};
  expr->next = NULL;
  expr->string.value = token.value;
  expr->string.length = token.length;
  return expr;
}

expr_t *ast_id_init(token_t token, arena_t *arena) {
  expr_t *expr = arena_alloc(arena, sizeof(expr_t));
  expr->kind = EXPR_ID;
  expr->line = token.line;
  expr->col = token.col;
  expr->type = (type_t){.kind = TYPE_UNKNOWN};
  expr->next = NULL;
  expr->id.name = token.value;
  return expr;
}

expr_t *ast_call_init(expr_t *callee, arena_t *arena) {
  expr_t *expr = arena_alloc(arena, sizeof(expr_t));
  expr->kind = EXPR_CALL;
  expr->line = callee->line;
  expr->col = callee->col;
  expr->type = (type_t){.kind = TYPE_UNKNOWN};
  expr->next = NULL;
  expr->call.callee = callee;
  expr->call.args = NULL;
  expr->call.arg_count = 0;
  return expr;
}

stmt_t *ast_return_init(token_t token, expr_t *value, arena_t *arena) {
  stmt_t *stmt = arena_alloc(arena, sizeof(stmt_t));
  stmt->kind = STMT_RETURN;
  stmt->line = token.line;
  stmt->col = token.col;
  stmt->next = NULL;
  stmt->ret.value = value;
  return stmt;
}

stmt_t *ast_expr_stmt_init(token_t token, expr_t *value, arena_t *arena) {
  stmt_t *stmt = arena_alloc(arena, sizeof(stmt_t));
  stmt->kind = STMT_EXPR;
  stmt->line = token.line;
  stmt->col = token.col;
  stmt->next = NULL;
  stmt->expr_stmt.expr = value;
  return stmt;
}

param_t *ast_param_init(arena_t *arena) {
  param_t *param = arena_alloc(arena, sizeof(param_t));
  param->name = NULL;
  param->type = (type_t){.kind = TYPE_UNKNOWN};
  param->next = NULL;
  return param;
}

decl_t *ast_fn_init(arena_t *arena) {
  decl_t *fn = arena_alloc(arena, sizeof(decl_t));
  fn->kind = DECL_FN;
  fn->visibility = VISIBILITY_PRIVATE;
  fn->name = NULL;
  fn->line = 0;
  fn->col = 0;
  fn->next = NULL;
  fn->fn.params = NULL;
  fn->fn.params_count = 0;
  fn->fn.body = NULL;
  fn->fn.stmt_count = 0;
  return fn;
}

decl_t *ast_container_init(char *name, arena_t *arena) {
  decl_t *container = arena_alloc(arena, sizeof(decl_t));
  container->kind = DECL_CONTAINER;
  container->visibility = VISIBILITY_PRIVATE;
  container->name = name;
  container->line = 0;
  container->col = 0;
  container->next = NULL;
  container->container.members = NULL;
  container->container.member_count = 0;
  return container;
}

unit_t *ast_unit_init(source_t *src, arena_t *arena) {
  unit_t *unit = arena_alloc(arena, sizeof(unit_t));
  unit->src = src;
  unit->root = ast_container_init(NULL, arena);
  return unit;
}

// -----------------------------------------------------------------
// Dumping
// -----------------------------------------------------------------

static void print_indent(int depth) {
  for (int i = 0; i < depth; i++) {
    fputs("  ", stdout);
  }
}

static void dump_expr(const expr_t *expr, int depth) {
  print_indent(depth);
  switch (expr->kind) {
  case EXPR_NUMBER:
    printf("Number literal %s\n", expr->number.value);
    break;
  case EXPR_STRING:
    printf("String literal \"");
    print_escaped(expr->string.value, expr->string.length);
    printf("\"\n");
    break;
  case EXPR_ID:
    printf("Id %s\n", expr->id.name);
    break;
  case EXPR_CALL:
    printf("Call '%s' (%zu args)\n", expr->call.callee->id.name,
           expr->call.arg_count);
    for (const expr_t *arg = expr->call.args; arg != NULL; arg = arg->next) {
      dump_expr(arg, depth + 1);
    }
    break;
  }
}

static void dump_stmt(const stmt_t *stmt, int depth) {
  print_indent(depth);
  switch (stmt->kind) {
  case STMT_RETURN:
    printf("Return\n");
    if (stmt->ret.value != NULL) {
      dump_expr(stmt->ret.value, depth + 1);
    }
    break;
  case STMT_EXPR:
    printf("ExprStmt\n");
    dump_expr(stmt->expr_stmt.expr, depth + 1);
    break;
  }
}

static char *visibility_to_str(Visibility visibility) {
  switch (visibility) {
  case VISIBILITY_PRIVATE:
    return "priv";
  case VISIBILITY_PUBLIC:
    return "pub";
  }
  return "?";
}

static void dump_decl(const decl_t *decl, int depth) {
  print_indent(depth);
  switch (decl->kind) {
  case DECL_FN:
    printf("FnDecl %s '%s' -> %s\n", visibility_to_str(decl->visibility),
           decl->name ? decl->name : "(anonymous)",
           type_kind_to_str(decl->fn.return_type.kind));
    for (const param_t *param = decl->fn.params; param != NULL;
         param = param->next) {
      print_indent(depth + 1);
      printf("param %s: %s\n", param->name, type_kind_to_str(param->type.kind));
    }

    for (const stmt_t *stmt = decl->fn.body; stmt != NULL; stmt = stmt->next) {
      dump_stmt(stmt, depth + 1);
    }
    break;
  case DECL_CONTAINER:
    printf("Container %s (%zu members)\n", decl->name ? decl->name : "(file)",
           decl->container.member_count);
    for (const decl_t *member = decl->container.members; member != NULL;
         member = member->next) {
      dump_decl(member, depth + 1);
    }
    break;
  }
}

int ast_dump(unit_t *unit) {
  if (unit == NULL) {
    return 1;
  }

  dump_decl(unit->root, 0);
  return 0;
}
