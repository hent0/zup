#include "semantic.h"
#include "arena.h"
#include "ast.h"
#include "diag.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct {
  decl_t **fns;
  size_t count;
} symtab_t;

static decl_t *symtab_lookup(const symtab_t *tab, const char *name) {
  for (size_t i = 0; i < tab->count; i++) {
    if (strcmp(tab->fns[i]->name, name) == 0) {
      return tab->fns[i];
    }
  }
  return NULL;
}

typedef struct {
  const symtab_t *tab;
  const source_t *src;
  bool had_error;
} sema_t;

typedef struct {
  TypeKind kind;
  bool ok;
} exprty_t;

static bool is_integer(TypeKind kind) {
  return kind == TYPE_I8 || kind == TYPE_I16 || kind == TYPE_I32 ||
         kind == TYPE_I64;
}

static bool type_assignable(TypeKind to, TypeKind from) {
  if (is_integer(to)) {
    return is_integer(from);
  }
  return to == from;
}

static exprty_t check_expr(sema_t *sema, expr_t *expr);

static exprty_t check_call(sema_t *sema, expr_t *call) {
  expr_t *callee = call->call.callee;
  const char *name = callee->id.name;

  if (strcmp(name, "printf") == 0) {
    expr_t *first = call->call.args;
    if (first == NULL || first->kind != EXPR_STRING) {
      diag_error(sema->src, call->line, call->col,
                 "'printf' expects a string literal as its first argument");
      sema->had_error = true;
    }
    for (expr_t *arg = first != NULL ? first->next : NULL; arg != NULL;
         arg = arg->next) {
      check_expr(sema, arg);
    }
    return (exprty_t){.kind = TYPE_I32, .ok = true};
  }

  decl_t *fn = symtab_lookup(sema->tab, name);
  if (fn == NULL) {
    diag_error(sema->src, call->line, call->col,
               "call to undefined function '%s'", name);
    sema->had_error = true;
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }

  if (call->call.arg_count > 0) {
    diag_error(sema->src, call->line, call->col,
               "'%s' takes no arguments (parameters not supported yet)", name);
    sema->had_error = true;
  }
  for (expr_t *arg = call->call.args; arg != NULL; arg = arg->next) {
    check_expr(sema, arg);
  }

  return (exprty_t){.kind = fn->fn.return_type.kind, .ok = true};
}

static exprty_t check_expr(sema_t *sema, expr_t *expr) {
  exprty_t result;
  switch (expr->kind) {
  case EXPR_NUMBER:
    result = (exprty_t){.kind = TYPE_I32, .ok = true};
    break;
  case EXPR_STRING:
    result = (exprty_t){.kind = TYPE_STRING, .ok = true};
    break;
  case EXPR_CALL:
    result = check_call(sema, expr);
    break;
  case EXPR_ID:
    diag_error(sema->src, expr->line, expr->col, "undefined name '%s'",
               expr->id.name);
    sema->had_error = true;
    result = (exprty_t){.kind = TYPE_VOID, .ok = false};
    break;
  default:
    result = (exprty_t){.kind = TYPE_VOID, .ok = false};
    break;
  }

  expr->type.kind = result.kind;
  return result;
}

static void check_return(sema_t *sema, stmt_t *stmt, const decl_t *fn) {
  TypeKind ret = fn->fn.return_type.kind;

  if (stmt->ret.value == NULL) {
    if (ret != TYPE_VOID) {
      diag_error(sema->src, stmt->line, stmt->col,
                 "non-void function '%s' must return a value", fn->name);
      sema->had_error = true;
    }
    return;
  }

  exprty_t value = check_expr(sema, stmt->ret.value);

  if (ret == TYPE_VOID) {
    diag_error(sema->src, stmt->line, stmt->col,
               "void function '%s' cannot return a value", fn->name);
    sema->had_error = true;
    return;
  }

  if (value.ok && !type_assignable(ret, value.kind)) {
    diag_error(sema->src, stmt->line, stmt->col,
               "cannot return %s from function returning %s",
               type_kind_to_str(value.kind), type_kind_to_str(ret));
    sema->had_error = true;
  }
}

static void check_stmt(sema_t *sema, stmt_t *stmt, const decl_t *fn) {
  switch (stmt->kind) {
  case STMT_RETURN:
    check_return(sema, stmt, fn);
    break;
  case STMT_EXPR:
    check_expr(sema, stmt->expr_stmt.expr);
    break;
  }
}

static void check_fn(sema_t *sema, const decl_t *fn) {
  stmt_t *last = NULL;
  for (stmt_t *stmt = fn->fn.body; stmt != NULL; stmt = stmt->next) {
    check_stmt(sema, stmt, fn);
    last = stmt;
  }

  if (fn->fn.return_type.kind != TYPE_VOID &&
      (last == NULL || last->kind != STMT_RETURN)) {
    diag_error(sema->src, fn->line, fn->col,
               "non-void function '%s' must return a value", fn->name);
    sema->had_error = true;
  }
}

static bool check_entry_point(const symtab_t *tab, const source_t *src) {
  decl_t *main_fn = symtab_lookup(tab, "main");
  if (main_fn == NULL) {
    diag_error_nofile("no entry point: 'main' function is required");
    return false;
  }

  bool ok = true;
  if (main_fn->visibility != VISIBILITY_PUBLIC) {
    diag_error(src, main_fn->line, main_fn->col,
               "'main' must be public (declared with 'pub')");
    ok = false;
  }
  if (main_fn->fn.return_type.kind != TYPE_I32) {
    diag_error(src, main_fn->line, main_fn->col, "'main' must return i32");
    ok = false;
  }
  if (main_fn->fn.params_count != 0) {
    diag_error(src, main_fn->line, main_fn->col,
               "'main' must take no parameters");
    ok = false;
  }
  return ok;
}

int semantic_check(unit_t *unit, arena_t *arena) {
  if (unit == NULL) {
    return false;
  }

  decl_t *root = unit->root;
  bool had_error = false;

  size_t capacity = root->container.member_count;
  symtab_t tab = {
      .fns = arena_alloc(arena, sizeof(decl_t *) * (capacity ? capacity : 1)),
      .count = 0,
  };
  for (decl_t *member = root->container.members; member != NULL;
       member = member->next) {
    if (member->kind != DECL_FN) {
      continue;
    }
    if (symtab_lookup(&tab, member->name) != NULL) {
      diag_error(unit->src, member->line, member->col,
                 "redefinition of function '%s'", member->name);
      had_error = true;
      continue;
    }
    tab.fns[tab.count++] = member;
  }

  if (!check_entry_point(&tab, unit->src)) {
    had_error = true;
  }

  sema_t sema = {.tab = &tab, .src = unit->src, .had_error = false};
  for (size_t i = 0; i < tab.count; i++) {
    check_fn(&sema, tab.fns[i]);
  }
  if (sema.had_error) {
    had_error = true;
  }

  return !had_error;
}
