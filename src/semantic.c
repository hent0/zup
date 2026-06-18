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
  const char *name;
  type_t type;
} local_t;

typedef struct {
  local_t *items;
  size_t count;
} scope_t;

static const local_t *scope_lookup(const scope_t *scope, const char *name) {
  for (size_t i = 0; i < scope->count; i++) {
    if (strcmp(scope->items[i].name, name) == 0) {
      return &scope->items[i];
    }
  }
  return NULL;
}

typedef struct {
  const symtab_t *tab;
  const source_t *src;
  const scope_t *scope;
  arena_t *arena;
  bool had_error;
} sema_t;

typedef struct {
  TypeKind kind;
  bool ok;
} exprty_t;

static bool is_integer(TypeKind k) {
  return k == TYPE_I8 || k == TYPE_I16 || k == TYPE_I32 || k == TYPE_I64;
}

static bool type_assignable(TypeKind to, TypeKind from) { return to == from; }

static exprty_t check_expr(sema_t *sema, expr_t *expr, TypeKind expected);

// TODO: Refactor out later
static exprty_t check_printf_call(sema_t *sema, expr_t *call) {
  expr_t *first = call->call.args;
  if (first == NULL || first->kind != EXPR_STRING) {
    diag_error(sema->src, call->line, call->col,
               "'printf' expects a string literal as its first argument");
    sema->had_error = true;
  }

  for (expr_t *arg = first; arg != NULL; arg = arg->next) {
    check_expr(sema, arg, TYPE_UNKNOWN);
  }
  return (exprty_t){.kind = TYPE_I32, .ok = true};
}

static exprty_t check_call(sema_t *sema, expr_t *call) {
  expr_t *callee = call->call.callee;
  const char *name = callee->id.name;

  // TODO: Refactor out later
  if (strcmp(name, "printf") == 0) {
    return check_printf_call(sema, call);
  }

  decl_t *fn = symtab_lookup(sema->tab, name);
  if (fn == NULL) {
    diag_error(sema->src, call->line, call->col,
               "call to undefined function '%s'", name);
    sema->had_error = true;
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }

  if (call->call.arg_count != fn->fn.params_count) {
    diag_error(sema->src, call->line, call->col,
               "'%s' expects %zu argument%s but got %zu", name,
               fn->fn.params_count, fn->fn.params_count == 1 ? "" : "s",
               call->call.arg_count);
    sema->had_error = true;
  }

  param_t *param = fn->fn.params;
  for (expr_t *arg = call->call.args; arg != NULL; arg = arg->next) {
    exprty_t at =
        check_expr(sema, arg, param ? param->type.kind : TYPE_UNKNOWN);
    if (param != NULL) {
      if (at.ok && !type_assignable(param->type.kind, at.kind)) {
        diag_error(sema->src, call->line, call->col,
                   "cannot pass %s as argument '%s' of '%s' (expected %s)",
                   type_kind_to_str(at.kind), param->name, name,
                   type_kind_to_str(param->type.kind));
        sema->had_error = true;
      }
      param = param->next;
    }
  }

  return (exprty_t){.kind = fn->fn.return_type.kind, .ok = true};
}

static exprty_t check_expr(sema_t *sema, expr_t *expr, TypeKind expected) {
  exprty_t result;
  switch (expr->kind) {
  case EXPR_NUMBER:
    result = (exprty_t){.kind = is_integer(expected) ? expected : TYPE_I32,
                        .ok = true};
    break;
  case EXPR_STRING:
    result = (exprty_t){.kind = TYPE_STRING, .ok = true};
    break;
  case EXPR_CALL:
    result = check_call(sema, expr);
    break;
  case EXPR_ID: {
    const local_t *local =
        sema->scope ? scope_lookup(sema->scope, expr->id.name) : NULL;
    if (local == NULL) {
      diag_error(sema->src, expr->line, expr->col, "undefined name '%s'",
                 expr->id.name);
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
    } else {
      result = (exprty_t){.kind = local->type.kind, .ok = true};
    }
    break;
  }
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

  exprty_t value = check_expr(sema, stmt->ret.value, ret);

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
    check_expr(sema, stmt->expr_stmt.expr, TYPE_UNKNOWN);
    break;
  }
}

static void check_fn(sema_t *sema, const decl_t *fn) {
  scope_t scope = {
      .items = arena_alloc(sema->arena, sizeof(local_t) * fn->fn.params_count),
      .count = 0};

  for (param_t *param = fn->fn.params; param != NULL; param = param->next) {
    if (scope_lookup(&scope, param->name) != NULL) {
      diag_error(sema->src, fn->line, fn->col, "duplicate parameter name '%s'",
                 param->name);
      sema->had_error = true;
      continue;
    }
    scope.items[scope.count++] = (local_t){
        .name = param->name,
        .type = param->type,
    };
  }

  sema->scope = &scope;

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

  sema->scope = NULL;
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

  sema_t sema = {
      .tab = &tab,
      .src = unit->src,
      .arena = arena,
      .had_error = false,
  };
  for (size_t i = 0; i < tab.count; i++) {
    check_fn(&sema, tab.fns[i]);
  }
  if (sema.had_error) {
    had_error = true;
  }

  return !had_error;
}
