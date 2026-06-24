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
  decl_t **structs;
  size_t count;
} typetab_t;

static decl_t *typetab_lookup(const typetab_t *tab, const char *name) {
  for (size_t i = 0; i < tab->count; i++) {
    if (strcmp(tab->structs[i]->name, name) == 0) {
      return tab->structs[i];
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
  const typetab_t *types;
  const source_t *src;
  scope_t *globals;
  scope_t *scope;
  arena_t *arena;
  unsigned int loop_depth;
  bool had_error;
} sema_t;

typedef struct {
  TypeKind kind;
  char *name;
  type_t *element;
  size_t array_length;
  bool ok;
} exprty_t;

static bool types_equal(type_t a, type_t b) {
  if (a.kind != b.kind) {
    return false;
  }
  if (a.kind == TYPE_STRUCT) {
    return a.name != NULL && b.name != NULL && strcmp(a.name, b.name) == 0;
  }
  if (a.kind == TYPE_ARRAY) {
    return a.array_length == b.array_length &&
           types_equal(*a.element, *b.element);
  }
  return true;
}

static bool assignable(type_t to, exprty_t from) {
  if (to.kind == TYPE_CSTR && from.kind == TYPE_STR) {
    return true;
  }
  if (to.kind != from.kind) {
    return false;
  }
  if (to.kind == TYPE_STRUCT) {
    return from.name != NULL && strcmp(to.name, from.name) == 0;
  }
  if (to.kind == TYPE_ARRAY) {
    return from.element != NULL && to.array_length == from.array_length &&
           types_equal(*to.element, *from.element);
  }
  return true;
}

static field_t *struct_field(const decl_t *strukt, const char *name) {
  for (field_t *field = strukt->strct.fields; field != NULL;
       field = field->next) {
    if (strcmp(field->name, name) == 0) {
      return field;
    }
  }
  return NULL;
}

static const char *lvalue_root(const expr_t *target) {
  while (target->kind == EXPR_FIELD || target->kind == EXPR_INDEX) {
    target = target->kind == EXPR_FIELD ? target->field.base
                                        : target->index.base;
  }
  return target->kind == EXPR_ID ? target->id.name : NULL;
}

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
      } else if (init.ok && !assignable(member->global.type, init)) {
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
      if (at.ok && !assignable(param->type, at)) {
        diag_error(sema->src, call->line, call->col,
                   "cannot pass %s as argument '%s' of '%s' (expected %s)",
                   type_to_str((type_t){.kind = at.kind, .name = at.name}),
                   param->name, name, type_to_str(param->type));
        sema->had_error = true;
      }
      param = param->next;
    }
  }

  return (exprty_t){.kind = fn->fn.return_type.kind,
                    .name = fn->fn.return_type.name,
                    .ok = true};
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

static bool number_is_float(const char *s) {
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    return false;
  }
  return strpbrk(s, ".eE") != NULL;
}

static decl_t *struct_method(const decl_t *strukt, const char *name) {
  for (decl_t *m = strukt->strct.members; m != NULL; m = m->next) {
    if (m->kind == DECL_FN && strcmp(m->name, name) == 0) {
      return m;
    }
  }
  return NULL;
}

static exprty_t check_method_call(sema_t *sema, expr_t *call) {
  expr_t *field = call->call.callee;
  exprty_t recv = check_expr(sema, field->field.base, TYPE_UNKNOWN);
  if (!recv.ok) {
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }
  if (recv.kind != TYPE_STRUCT) {
    diag_error(sema->src, field->line, field->col,
               "cannot call method '%s' on non-struct type %s",
               field->field.name,
               type_to_str((type_t){.kind = recv.kind, .name = recv.name}));
    sema->had_error = true;
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }

  decl_t *strukt = typetab_lookup(sema->types, recv.name);
  decl_t *method = strukt ? struct_method(strukt, field->field.name) : NULL;
  if (method == NULL) {
    diag_error(sema->src, field->line, field->col,
               "struct '%s' has no method '%s'", recv.name, field->field.name);
    sema->had_error = true;
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }

  param_t *param = method->fn.params;
  bool has_self = param != NULL && param->is_self;
  if (has_self) {
    param = param->next;
  }
  size_t expected = method->fn.params_count - (has_self ? 1 : 0);
  if (call->call.arg_count != expected) {
    diag_error(sema->src, call->line, call->col,
               "'%s' expects %zu argument%s but got %zu", field->field.name,
               expected, expected == 1 ? "" : "s", call->call.arg_count);
    sema->had_error = true;
  }

  for (expr_t *arg = call->call.args; arg != NULL; arg = arg->next) {
    exprty_t at =
        check_expr(sema, arg, param ? param->type.kind : TYPE_UNKNOWN);
    if (param != NULL) {
      if (at.ok && !assignable(param->type, at)) {
        diag_error(sema->src, call->line, call->col,
                   "cannot pass %s as argument '%s' of '%s' (expected %s)",
                   type_to_str((type_t){.kind = at.kind, .name = at.name}),
                   param->name, field->field.name, type_to_str(param->type));
        sema->had_error = true;
      }
      param = param->next;
    }
  }

  return (exprty_t){.kind = method->fn.return_type.kind,
                    .name = method->fn.return_type.name,
                    .ok = true};
}

static exprty_t check_expr(sema_t *sema, expr_t *expr, TypeKind expected) {
  exprty_t result;
  switch (expr->kind) {
  case EXPR_BOOLEAN:
    result = (exprty_t){.kind = TYPE_BOOL, .ok = true};
    break;
  case EXPR_NUMBER: {
    if (number_is_float(expr->number.value)) {
      TypeKind type = type_is_float(expected) ? expected : TYPE_F64;
      result = (exprty_t){.kind = type, .ok = true};
    } else {
      TypeKind type = type_is_integer(expected) ? expected : TYPE_I32;
      if (check_literal_fit(sema, expr, type)) {
        result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      } else {
        result = (exprty_t){.kind = type, .ok = true};
      }
    }
    break;
  }
  case EXPR_STRING:
    result = (exprty_t){.kind = TYPE_STR, .ok = true};
    break;
  case EXPR_CALL:
    result = expr->call.callee->kind == EXPR_FIELD
                 ? check_method_call(sema, expr)
                 : check_call(sema, expr);
    break;
  case EXPR_CAST: {
    exprty_t operand = check_expr(sema, expr->cast.operand, TYPE_UNKNOWN);
    TypeKind target = expr->cast.target.kind;
    if (operand.ok &&
        (!type_is_numeric(operand.kind) || !type_is_numeric(target))) {
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
      result = (exprty_t){.kind = local->type.kind,
                          .name = local->type.name,
                          .element = local->type.element,
                          .array_length = local->type.array_length,
                          .ok = true};
    }
    break;
  }
  case EXPR_BINARY: {
    bool lhs_lit = expr->binary.lhs->kind == EXPR_NUMBER;
    bool rhs_lit = expr->binary.rhs->kind == EXPR_NUMBER;

    exprty_t lhs, rhs;
    if (rhs_lit && !lhs_lit) {
      lhs = check_expr(sema, expr->binary.lhs, expected);
      TypeKind hint = type_is_numeric(lhs.kind) ? lhs.kind : expected;
      rhs = check_expr(sema, expr->binary.rhs, hint);
    } else if (lhs_lit && !rhs_lit) {
      rhs = check_expr(sema, expr->binary.rhs, expected);
      TypeKind hint = type_is_numeric(rhs.kind) ? rhs.kind : expected;
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
    } else if (!type_is_numeric(lhs.kind) || lhs.kind != rhs.kind ||
               (type_is_float(lhs.kind) &&
                !binop_is_arithmetic(expr->binary.op))) {
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
                        : type_is_signed_integer(operand.kind) ||
                              type_is_float(operand.kind);
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
  case EXPR_STRUCT_LITERAL: {
    decl_t *strukt =
        typetab_lookup(sema->types, expr->struct_literal.type_name);
    if (strukt == NULL) {
      diag_error(sema->src, expr->line, expr->col, "unknown type '%s'",
                 expr->struct_literal.type_name);
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }

    bool ok = true;
    for (field_init_t *init = expr->struct_literal.inits; init != NULL;
         init = init->next) {
      field_t *field = struct_field(strukt, init->name);
      exprty_t value = check_expr(sema, init->value,
                                  field ? field->type.kind : TYPE_UNKNOWN);
      if (field == NULL) {
        diag_error(sema->src, init->line, init->col,
                   "struct '%s' has no field '%s'", strukt->name, init->name);
        sema->had_error = true;
        ok = false;
        continue;
      }
      if (value.ok && !assignable(field->type, value)) {
        diag_error(sema->src, init->value->line, init->value->col,
                   "cannot assign %s to field '%s' of type %s",
                   type_kind_to_str(value.kind), init->name,
                   type_to_str(field->type));
        sema->had_error = true;
        ok = false;
      }
    }

    for (field_t *field = strukt->strct.fields; field != NULL;
         field = field->next) {
      bool found = false;
      for (field_init_t *init = expr->struct_literal.inits; init != NULL;
           init = init->next) {
        if (strcmp(init->name, field->name) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        diag_error(sema->src, expr->line, expr->col,
                   "missing field '%s' in initializer for '%s'", field->name,
                   strukt->name);
        sema->had_error = true;
        ok = false;
      }
    }

    result = (exprty_t){.kind = TYPE_STRUCT, .name = strukt->name, .ok = ok};
    break;
  }
  case EXPR_FIELD: {
    exprty_t base = check_expr(sema, expr->field.base, TYPE_UNKNOWN);
    if (!base.ok) {
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    if (base.kind == TYPE_STR) {
      if (strcmp(expr->field.name, "ptr") == 0) {
        result = (exprty_t){.kind = TYPE_CSTR, .ok = true};
      } else if (strcmp(expr->field.name, "len") == 0) {
        result = (exprty_t){.kind = TYPE_I64, .ok = true};
      } else {
        diag_error(sema->src, expr->line, expr->col, "str has no field '%s'",
                   expr->field.name);
        sema->had_error = true;
        result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      }
      break;
    }
    if (base.kind == TYPE_ARRAY) {
      if (strcmp(expr->field.name, "len") == 0) {
        result = (exprty_t){.kind = TYPE_I64, .ok = true};
      } else {
        diag_error(sema->src, expr->line, expr->col, "array has no field '%s'",
                   expr->field.name);
        sema->had_error = true;
        result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      }
      break;
    }
    if (base.kind != TYPE_STRUCT) {
      diag_error(sema->src, expr->line, expr->col,
                 "cannot access field '%s' of non-struct type %s",
                 expr->field.name, type_kind_to_str(base.kind));
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    decl_t *strukt = typetab_lookup(sema->types, base.name);
    field_t *field = strukt ? struct_field(strukt, expr->field.name) : NULL;
    if (field == NULL) {
      diag_error(sema->src, expr->line, expr->col,
                 "struct '%s' has no field '%s'", base.name, expr->field.name);
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    result = (exprty_t){.kind = field->type.kind,
                        .name = field->type.name,
                        .element = field->type.element,
                        .array_length = field->type.array_length,
                        .ok = true};
    break;
  }
  case EXPR_ARRAY: {
    type_t elem = {.kind = TYPE_UNKNOWN};
    size_t count = 0;
    bool ok = true;
    for (expr_t *e = expr->array.elements; e != NULL; e = e->next) {
      exprty_t et = check_expr(sema, e, TYPE_UNKNOWN);
      type_t ety = {.kind = et.kind,
                    .name = et.name,
                    .element = et.element,
                    .array_length = et.array_length};
      if (count == 0) {
        elem = ety;
      } else if (et.ok && !types_equal(elem, ety)) {
        diag_error(sema->src, e->line, e->col,
                   "array element has type %s, expected %s", type_to_str(ety),
                   type_to_str(elem));
        sema->had_error = true;
        ok = false;
      }
      if (!et.ok) {
        ok = false;
      }
      count++;
    }
    type_t *element = arena_alloc(sema->arena, sizeof(type_t));
    *element = elem;
    result = (exprty_t){.kind = TYPE_ARRAY,
                        .element = element,
                        .array_length = count,
                        .ok = ok};
    break;
  }
  case EXPR_INDEX: {
    exprty_t base = check_expr(sema, expr->index.base, TYPE_UNKNOWN);
    exprty_t idx = check_expr(sema, expr->index.index, TYPE_I32);
    if (!base.ok) {
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    if (base.kind != TYPE_ARRAY) {
      diag_error(sema->src, expr->line, expr->col,
                 "cannot index non-array type %s", type_kind_to_str(base.kind));
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    if (idx.ok && !type_is_integer(idx.kind)) {
      diag_error(sema->src, expr->index.index->line, expr->index.index->col,
                 "array index must be an integer, got %s",
                 type_kind_to_str(idx.kind));
      sema->had_error = true;
    }
    result = (exprty_t){.kind = base.element->kind,
                        .name = base.element->name,
                        .element = base.element->element,
                        .array_length = base.element->array_length,
                        .ok = true};
    break;
  }
  default:
    result = (exprty_t){.kind = TYPE_VOID, .ok = false};
    break;
  }

  expr->type.kind = result.kind;
  expr->type.name = result.name;
  expr->type.element = result.element;
  expr->type.array_length = result.array_length;
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

  if (value.ok && !assignable(fn->fn.return_type, value)) {
    diag_error(sema->src, stmt->line, stmt->col,
               "cannot return %s from function returning %s",
               type_to_str((type_t){.kind = value.kind, .name = value.name}),
               type_to_str(fn->fn.return_type));
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
        stmt->binding.type.name = init.name;
        stmt->binding.type.element = init.element;
        stmt->binding.type.array_length = init.array_length;
      } else {
        if (stmt->binding.type.kind == TYPE_ARRAY &&
            stmt->binding.type.array_length == 0 && init.kind == TYPE_ARRAY) {
          stmt->binding.type.array_length = init.array_length;
        }
        if (init.ok && !assignable(stmt->binding.type, init)) {
          type_t from = {.kind = init.kind,
                         .name = init.name,
                         .element = init.element,
                         .array_length = init.array_length};
          diag_error(sema->src, stmt->line, stmt->col,
                     "cannot assign %s to variable '%s' of type %s",
                     type_to_str(from), stmt->binding.name,
                     type_to_str(stmt->binding.type));
          sema->had_error = true;
        }
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
    expr_t *target = stmt->assign.target;
    exprty_t lhs = check_expr(sema, target, TYPE_UNKNOWN);

    // Mutability is a property of the binding the lvalue is rooted at.
    const char *root = lvalue_root(target);
    const local_t *local =
        (root && sema->scope) ? scope_lookup(sema->scope, root) : NULL;
    if (local == NULL && root && sema->globals != NULL) {
      local = scope_lookup(sema->globals, root);
    }

    if (local != NULL && !local->mutable) {
      diag_error(sema->src, stmt->line, stmt->col,
                 local->is_loop_var ? "cannot assign to loop variable '%s'"
                                    : "cannot assign to const '%s'",
                 root);
      sema->had_error = true;
      check_expr(sema, stmt->assign.value, TYPE_UNKNOWN);
      break;
    }

    exprty_t value = check_expr(sema, stmt->assign.value, lhs.kind);
    if (lhs.ok && value.ok) {
      type_t to = {.kind = lhs.kind, .name = lhs.name};
      if (!assignable(to, value)) {
        if (target->kind == EXPR_FIELD) {
          diag_error(sema->src, stmt->assign.value->line,
                     stmt->assign.value->col,
                     "cannot assign %s to field '%s' of type %s",
                     type_kind_to_str(value.kind), target->field.name,
                     type_to_str(to));
        } else {
          diag_error(sema->src, stmt->line, stmt->col,
                     "cannot assign %s to variable '%s' of type %s",
                     type_kind_to_str(value.kind), root, type_to_str(to));
        }
        sema->had_error = true;
      }
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

static bool check_type(sema_t *sema, type_t type) {
  if (type.kind == TYPE_ARRAY) {
    return check_type(sema, *type.element);
  }
  if (type.kind == TYPE_STRUCT &&
      typetab_lookup(sema->types, type.name) == NULL) {
    diag_error(sema->src, type.line, type.col, "unknown type '%s'", type.name);
    sema->had_error = true;
    return false;
  }
  return true;
}

static void check_fn(sema_t *sema, const decl_t *fn);

static void check_struct(sema_t *sema, const decl_t *strukt) {
  for (field_t *field = strukt->strct.fields; field != NULL;
       field = field->next) {
    check_type(sema, field->type);
    for (field_t *prev = strukt->strct.fields; prev != field;
         prev = prev->next) {
      if (strcmp(prev->name, field->name) == 0) {
        diag_error(sema->src, field->line, field->col, "duplicate field '%s'",
                   field->name);
        sema->had_error = true;
        break;
      }
    }
  }
  for (decl_t *member = strukt->strct.members; member != NULL;
       member = member->next) {
    check_fn(sema, member);
  }
}

static void check_fn(sema_t *sema, const decl_t *fn) {
  for (param_t *param = fn->fn.params; param != NULL; param = param->next) {
    check_type(sema, param->type);
  }
  check_type(sema, fn->fn.return_type);

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
  typetab_t types = {
      .structs =
          arena_alloc(arena, sizeof(decl_t *) * (capacity ? capacity : 1)),
      .count = 0,
  };
  for (decl_t *member = root->container.members; member != NULL;
       member = member->next) {
    switch (member->kind) {
    case DECL_FN: {
      if (symtab_lookup(&tab, member->name) != NULL) {
        diag_error(unit->src, member->line, member->col,
                   "redefinition of function '%s'", member->name);
        had_error = true;
        break;
      }
      tab.fns[tab.count++] = member;
      break;
    }
    case DECL_STRUCT: {
      if (typetab_lookup(&types, member->name)) {
        diag_error(unit->src, member->line, member->col,
                   "redefinition of type '%s'", member->name);
        had_error = true;
        break;
      }
      types.structs[types.count++] = member;
      break;
    }
    case DECL_CONTAINER:
    case DECL_GLOBAL:
      break;
    }
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
      .types = &types,
      .src = unit->src,
      .globals = &globals,
      .scope = NULL,
      .arena = arena,
      .had_error = false,
  };

  for (size_t i = 0; i < types.count; i++) {
    check_struct(&sema, types.structs[i]);
  }

  check_globals(&sema, root, &globals);

  for (size_t i = 0; i < tab.count; i++) {
    check_fn(&sema, tab.fns[i]);
  }
  if (sema.had_error) {
    had_error = true;
  }

  return !had_error;
}
