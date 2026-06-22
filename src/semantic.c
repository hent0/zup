#include "semantic.h"
#include "arena.h"
#include "ast.h"
#include "diag.h"
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
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
  bool mutable;
  bool is_loop_var;
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
  scope_t *globals;
  scope_t *scope;
  arena_t *arena;
  unsigned int loop_depth;
  bool had_error;
} sema_t;

typedef struct {
  TypeKind kind;
  bool ok;
} exprty_t;

static bool type_assignable(TypeKind to, TypeKind from) { return to == from; }

static bool is_const_init(const expr_t *expr) {
  if (expr == NULL) {
    return false;
  }

  return expr->kind == EXPR_NUMBER || expr->kind == EXPR_BOOLEAN;
}

static exprty_t check_expr(sema_t *sema, expr_t *expr, TypeKind expected);

static void check_globals(sema_t *sema, decl_t *root, scope_t *globals) {
  for (decl_t *member = root->container.members; member != NULL;
       member = member->next) {
    if (member->kind != DECL_GLOBAL) {
      continue;
    }

    if (scope_lookup(globals, member->name) != NULL ||
        symtab_lookup(sema->tab, member->name)) {
      diag_error(sema->src, member->line, member->col, "redeclaration of '%s'",
                 member->name);
      sema->had_error = true;
      continue;
    }

    if (!is_const_init(member->global.init)) {
      const expr_t *expr = member->global.init;
      diag_error(sema->src, expr ? expr->line : member->line,
                 expr ? expr->col : member->col,
                 "global initializer must be a constant");
      sema->had_error = true;
    } else {
      exprty_t init =
          check_expr(sema, member->global.init, member->global.type.kind);
      if (member->global.type.kind == TYPE_UNKNOWN) {
        member->global.type.kind = init.kind;
      } else if (init.ok &&
                 !type_assignable(member->global.type.kind, init.kind)) {
        diag_error(sema->src, member->line, member->col,
                   "cannot assign %s to '%s' of type %s",
                   type_kind_to_str(init.kind), member->name,
                   type_kind_to_str(member->global.type.kind));
        sema->had_error = true;
      }
    }
    globals->items[globals->count++] = (local_t){
        .name = member->name,
        .type = member->global.type,
        .mutable = member->global.mutable,
    };
  }
}

static exprty_t check_call(sema_t *sema, expr_t *call) {
  expr_t *callee = call->call.callee;
  const char *name = callee->id.name;

  decl_t *fn = symtab_lookup(sema->tab, name);
  if (fn == NULL) {
    diag_error(sema->src, call->line, call->col,
               "call to undefined function '%s'", name);
    sema->had_error = true;
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }

  if (fn->fn.variadic) {
    if (call->call.arg_count < fn->fn.params_count) {
      diag_error(sema->src, call->line, call->col,
                 "'%s' expects at least %zu argument%s but got %zu", name,
                 fn->fn.params_count, fn->fn.params_count == 1 ? "" : "s",
                 call->call.arg_count);
      sema->had_error = true;
    }
  } else if (call->call.arg_count != fn->fn.params_count) {
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

static unsigned long long type_int_max(TypeKind k) {
  switch (k) {
  case TYPE_I8:
    return 127ULL;
  case TYPE_U8:
    return 255ULL;
  case TYPE_I16:
    return 32767ULL;
  case TYPE_U16:
    return 65535ULL;
  case TYPE_I32:
    return 2147483647ULL;
  case TYPE_U32:
    return 4294967295ULL;
  case TYPE_I64:
    return 9223372036854775807ULL;
  case TYPE_U64:
    return 18446744073709551615ULL;
  default:
    return 0;
  }
}

static bool check_literal_fit(sema_t *sema, expr_t *expr, TypeKind type) {
  errno = 0;
  unsigned long long v = strtoull(expr->number.value, NULL, 0);
  if (errno == ERANGE || v > type_int_max(type)) {
    diag_error(sema->src, expr->line, expr->col,
               "integer literal %s out of range for %s", expr->number.value,
               type_kind_to_str(type));
    sema->had_error = true;
    return true;
  }
  return false;
}

static exprty_t check_expr(sema_t *sema, expr_t *expr, TypeKind expected) {
  exprty_t result;
  switch (expr->kind) {
  case EXPR_BOOLEAN:
    result = (exprty_t){.kind = TYPE_BOOL, .ok = true};
    break;
  case EXPR_NUMBER: {
    TypeKind type = type_is_integer(expected) ? expected : TYPE_I32;
    if (check_literal_fit(sema, expr, type)) {
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
    } else {
      result = (exprty_t){.kind = type, .ok = true};
    }
    break;
  }
  case EXPR_STRING:
    result = (exprty_t){.kind = TYPE_STRING, .ok = true};
    break;
  case EXPR_CALL:
    result = check_call(sema, expr);
    break;
  case EXPR_CAST: {
    exprty_t operand = check_expr(sema, expr->cast.operand, TYPE_UNKNOWN);
    TypeKind target = expr->cast.target.kind;
    if (operand.ok &&
        (!type_is_integer(operand.kind) || !type_is_integer(target))) {
      diag_error(sema->src, expr->line, expr->col, "cannot cast %s to %s",
                 type_kind_to_str(operand.kind), type_kind_to_str(target));
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
    } else {
      result = (exprty_t){.kind = target, .ok = operand.ok};
    }
    break;
  }
  case EXPR_ID: {
    const local_t *local =
        sema->scope ? scope_lookup(sema->scope, expr->id.name) : NULL;

    if (local == NULL && sema->globals != NULL) {
      local = scope_lookup(sema->globals, expr->id.name);
    }

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
  case EXPR_BINARY: {
    bool lhs_lit = expr->binary.lhs->kind == EXPR_NUMBER;
    bool rhs_lit = expr->binary.rhs->kind == EXPR_NUMBER;

    exprty_t lhs, rhs;
    if (rhs_lit && !lhs_lit) {
      lhs = check_expr(sema, expr->binary.lhs, expected);
      TypeKind hint = type_is_integer(lhs.kind) ? lhs.kind : expected;
      rhs = check_expr(sema, expr->binary.rhs, hint);
    } else if (lhs_lit && !rhs_lit) {
      rhs = check_expr(sema, expr->binary.rhs, expected);
      TypeKind hint = type_is_integer(rhs.kind) ? rhs.kind : expected;
      lhs = check_expr(sema, expr->binary.lhs, hint);
    } else {
      lhs = check_expr(sema, expr->binary.lhs, expected);
      rhs = check_expr(sema, expr->binary.rhs, expected);
    }

    if (!lhs.ok || !rhs.ok) {
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }

    if (binop_is_logical(expr->binary.op)) {
      if (lhs.kind != TYPE_BOOL || rhs.kind != TYPE_BOOL) {
        diag_error(sema->src, expr->line, expr->col,
                   "cannot apply '%s' to %s and %s",
                   binop_to_str(expr->binary.op), type_kind_to_str(lhs.kind),
                   type_kind_to_str(rhs.kind));
        sema->had_error = true;
        result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      } else {
        result = (exprty_t){.kind = TYPE_BOOL, .ok = true};
      }
    } else if (!type_is_integer(lhs.kind) || lhs.kind != rhs.kind) {
      diag_error(sema->src, expr->line, expr->col,
                 "cannot apply '%s' to %s and %s",
                 binop_to_str(expr->binary.op), type_kind_to_str(lhs.kind),
                 type_kind_to_str(rhs.kind));
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
    } else {
      TypeKind kind =
          binop_is_comparison(expr->binary.op) ? TYPE_BOOL : lhs.kind;
      result = (exprty_t){.kind = kind, .ok = true};
    }
    break;
  }
  case EXPR_UNARY: {
    // !  : operand must be bool, result bool
    // -  : operand must be an integer, result the same integer type
    bool is_not = expr->unary.op == UNOP_NOT;
    TypeKind operand_expected = is_not ? TYPE_BOOL : expected;
    exprty_t operand = check_expr(sema, expr->unary.operand, operand_expected);

    bool valid = is_not ? operand.kind == TYPE_BOOL
                        : type_is_signed_integer(operand.kind);
    if (!operand.ok) {
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
    } else if (!valid) {
      diag_error(sema->src, expr->line, expr->col, "cannot apply '%s' to %s",
                 unop_to_str(expr->unary.op), type_kind_to_str(operand.kind));
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
    } else {
      result =
          (exprty_t){.kind = is_not ? TYPE_BOOL : operand.kind, .ok = true};
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
  case STMT_IF: {
    exprty_t cond = check_expr(sema, stmt->if_stmt.cond, TYPE_BOOL);
    if (cond.ok && cond.kind != TYPE_BOOL) {
      diag_error(sema->src, stmt->if_stmt.cond->line, stmt->if_stmt.cond->col,
                 "if condition must be bool, got %s",
                 type_kind_to_str(cond.kind));
      sema->had_error = true;
    }
    for (stmt_t *s = stmt->if_stmt.then_body; s != NULL; s = s->next) {
      check_stmt(sema, s, fn);
    }
    for (stmt_t *s = stmt->if_stmt.else_body; s != NULL; s = s->next) {
      check_stmt(sema, s, fn);
    }
    break;
  }
  case STMT_BINDING: {
    if (stmt->binding.init != NULL) {
      exprty_t init =
          check_expr(sema, stmt->binding.init, stmt->binding.type.kind);
      if (stmt->binding.type.kind == TYPE_UNKNOWN) {
        stmt->binding.type.kind = init.kind;
      } else if (init.ok &&
                 !type_assignable(stmt->binding.type.kind, init.kind)) {
        diag_error(sema->src, stmt->line, stmt->col,
                   "cannot assign %s to variable '%s' of type %s",
                   type_kind_to_str(init.kind), stmt->binding.name,
                   type_kind_to_str(stmt->binding.type.kind));
        sema->had_error = true;
      }
    }
    if (scope_lookup(sema->scope, stmt->binding.name) != NULL) {
      diag_error(sema->src, stmt->line, stmt->col, "redeclaration of '%s'",
                 stmt->binding.name);
      sema->had_error = true;
    } else {
      sema->scope->items[sema->scope->count++] = (local_t){
          .name = stmt->binding.name,
          .type = stmt->binding.type,
          .mutable = stmt->binding.mutable,
      };
    }
    break;
  }
  case STMT_ASSIGN: {
    const local_t *local =
        sema->scope ? scope_lookup(sema->scope, stmt->assign.name) : NULL;

    if (local == NULL && sema->globals != NULL) {
      local = scope_lookup(sema->globals, stmt->assign.name);
    }

    if (local == NULL) {
      diag_error(sema->src, stmt->line, stmt->col, "undefined name '%s'",
                 stmt->assign.name);
      sema->had_error = true;
      check_expr(sema, stmt->assign.value, TYPE_UNKNOWN);
      break;
    }

    if (!local->mutable) {
      diag_error(sema->src, stmt->line, stmt->col,
                 local->is_loop_var ? "cannot assign to loop variable '%s'"
                                    : "cannot assign to const '%s'",
                 stmt->assign.name);
      sema->had_error = true;
      check_expr(sema, stmt->assign.value, TYPE_UNKNOWN);
      break;
    }
    exprty_t value = check_expr(sema, stmt->assign.value, local->type.kind);
    if (value.ok && !type_assignable(local->type.kind, value.kind)) {
      diag_error(sema->src, stmt->line, stmt->col,
                 "cannot assign %s to variable '%s' of type %s",
                 type_kind_to_str(value.kind), stmt->assign.name,
                 type_kind_to_str(local->type.kind));
      sema->had_error = true;
    }
    break;
  }
  case STMT_WHILE: {
    exprty_t cond = check_expr(sema, stmt->while_loop.cond, TYPE_BOOL);
    if (cond.ok && cond.kind != TYPE_BOOL) {
      diag_error(
          sema->src, stmt->while_loop.cond->line, stmt->while_loop.cond->col,
          "while condition must be bool, got %s", type_kind_to_str(cond.kind));
      sema->had_error = true;
    }
    sema->loop_depth++;
    for (stmt_t *s = stmt->while_loop.body; s != NULL; s = s->next) {
      check_stmt(sema, s, fn);
    }
    sema->loop_depth--;
    break;
  }
  case STMT_FOR: {
    exprty_t start = check_expr(sema, stmt->for_loop.start, TYPE_UNKNOWN);
    exprty_t end = check_expr(sema, stmt->for_loop.end, TYPE_UNKNOWN);

    TypeKind var_type = TYPE_I32;
    if (start.ok && !type_is_integer(start.kind)) {
      diag_error(sema->src, stmt->for_loop.start->line,
                 stmt->for_loop.start->col,
                 "range bound must be an integer, got %s",
                 type_kind_to_str(start.kind));
      sema->had_error = true;
    } else if (end.ok && !type_is_integer(end.kind)) {
      diag_error(sema->src, stmt->for_loop.end->line, stmt->for_loop.end->col,
                 "range bound must be an integer, got %s",
                 type_kind_to_str(end.kind));
      sema->had_error = true;
    } else if (start.ok && end.ok && start.kind != end.kind) {
      diag_error(sema->src, stmt->for_loop.start->line,
                 stmt->for_loop.start->col,
                 "range bounds must have the same type, got %s and %s",
                 type_kind_to_str(start.kind), type_kind_to_str(end.kind));
      sema->had_error = true;
    } else if (start.ok) {
      var_type = start.kind;
    }

    size_t saved = sema->scope->count;
    sema->scope->items[sema->scope->count++] = (local_t){
        .name = stmt->for_loop.var,
        .type = (type_t){.kind = var_type},
        .mutable = false,
        .is_loop_var = true,
    };

    sema->loop_depth++;
    for (stmt_t *s = stmt->for_loop.body; s != NULL; s = s->next) {
      check_stmt(sema, s, fn);
    }
    sema->loop_depth--;

    sema->scope->count = saved;
    break;
  }
  case STMT_BREAK: {
    if (sema->loop_depth == 0) {
      diag_error(sema->src, stmt->line, stmt->col, "break outside of loop");
      sema->had_error = true;
    }
    break;
  }
  case STMT_CONTINUE: {
    if (sema->loop_depth == 0) {
      diag_error(sema->src, stmt->line, stmt->col, "continue outside of loop");
      sema->had_error = true;
    }
    break;
  }
  }
}

static bool block_returns(stmt_t *body);

static bool stmt_returns(stmt_t *stmt) {
  switch (stmt->kind) {
  case STMT_RETURN:
    return true;
  case STMT_IF:
    return stmt->if_stmt.else_body != NULL &&
           block_returns(stmt->if_stmt.then_body) &&
           block_returns(stmt->if_stmt.else_body);
  default:
    return false;
  }
}

static bool block_returns(stmt_t *body) {
  if (body == NULL) {
    return false;
  }
  stmt_t *last = body;
  while (last->next != NULL) {
    last = last->next;
  }
  return stmt_returns(last);
}

static size_t count_bindings(stmt_t *body) {
  size_t n = 0;
  for (stmt_t *s = body; s != NULL; s = s->next) {
    if (s->kind == STMT_BINDING) {
      n++;
    } else if (s->kind == STMT_IF) {
      n += count_bindings(s->if_stmt.then_body);
      n += count_bindings(s->if_stmt.else_body);
    } else if (s->kind == STMT_WHILE) {
      n += count_bindings(s->while_loop.body);
    } else if (s->kind == STMT_FOR) {
      n += 1 + count_bindings(s->for_loop.body);
    }
  }
  return n;
}

static void check_fn(sema_t *sema, const decl_t *fn) {
  if (fn->fn.is_extern) {
    return;
  }

  size_t capacity = fn->fn.params_count + count_bindings(fn->fn.body);
  scope_t scope = {
      .items =
          arena_alloc(sema->arena, sizeof(local_t) * (capacity ? capacity : 1)),
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
        .mutable = param->mutable,
    };
  }

  sema->scope = &scope;

  for (stmt_t *stmt = fn->fn.body; stmt != NULL; stmt = stmt->next) {
    check_stmt(sema, stmt, fn);
  }

  if (fn->fn.return_type.kind != TYPE_VOID && !block_returns(fn->fn.body)) {
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

  scope_t globals = {
      .items = arena_alloc(arena, sizeof(local_t) * (capacity ? capacity : 1)),
      .count = 0,
  };

  sema_t sema = {
      .tab = &tab,
      .src = unit->src,
      .globals = &globals,
      .scope = NULL,
      .arena = arena,
      .had_error = false,
  };

  check_globals(&sema, root, &globals);

  for (size_t i = 0; i < tab.count; i++) {
    check_fn(&sema, tab.fns[i]);
  }
  if (sema.had_error) {
    had_error = true;
  }

  return !had_error;
}
