#include "semantic.h"
#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "loader.h"
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  decl_t **imports;
  size_t count;
} modtab_t;

static decl_t *modtab_lookup(const modtab_t *tab, const char *alias) {
  for (size_t i = 0; i < tab->count; i++) {
    if (strcmp(tab->imports[i]->name, alias) == 0) {
      return tab->imports[i];
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

static decl_t *module_pub_struct(module_t *mod, const char *name) {
  for (decl_t *m = mod->unit->root->container.members; m != NULL; m = m->next) {
    if (m->kind == DECL_STRUCT && m->visibility == VISIBILITY_PUBLIC &&
        strcmp(m->name, name) == 0) {
      return m;
    }
  }
  return NULL;
}

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
  decl_t *decl;
} typeent_t;

typedef struct {
  typeent_t *structs;
  size_t count;
} typetab_t;

static decl_t *typetab_lookup(const typetab_t *tab, const char *name) {
  for (size_t i = 0; i < tab->count; i++) {
    if (strcmp(tab->structs[i].name, name) == 0) {
      return tab->structs[i].decl;
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
  const modtab_t *modules;
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
  if (a.kind == TYPE_STRUCT || a.kind == TYPE_ENUM) {
    return a.name != NULL && b.name != NULL && strcmp(a.name, b.name) == 0;
  }
  if (a.kind == TYPE_ARRAY) {
    return a.array_length == b.array_length &&
           types_equal(*a.element, *b.element);
  }
  if (a.kind == TYPE_SLICE) {
    return types_equal(*a.element, *b.element);
  }
  return true;
}

static bool assignable(type_t to, exprty_t from) {
  if (to.kind == TYPE_CSTR && from.kind == TYPE_STR) {
    return true;
  }
  if (to.kind == TYPE_SLICE &&
      (from.kind == TYPE_ARRAY || from.kind == TYPE_SLICE)) {
    return from.element != NULL && types_equal(*to.element, *from.element);
  }
  if (to.kind != from.kind) {
    return false;
  }
  if (to.kind == TYPE_STRUCT || to.kind == TYPE_ENUM) {
    return from.name != NULL && to.name != NULL &&
           strcmp(to.name, from.name) == 0;
  }
  if (to.kind == TYPE_ARRAY) {
    return from.element != NULL && to.array_length == from.array_length &&
           types_equal(*to.element, *from.element);
  }
  return true;
}

static type_t exprty_type(exprty_t e) {
  return (type_t){.kind = e.kind,
                  .name = e.name,
                  .element = e.element,
                  .array_length = e.array_length};
}

static type_t type_from_kind(TypeKind kind) { return (type_t){.kind = kind}; }

static field_t *struct_field(const decl_t *strct, const char *name) {
  for (field_t *field = strct->strct.fields; field != NULL;
       field = field->next) {
    if (strcmp(field->name, name) == 0) {
      return field;
    }
  }
  return NULL;
}

static enum_member_t *enum_member(const decl_t *enm, const char *name) {
  for (enum_member_t *member = enm->enm.members; member != NULL;
       member = member->next) {
    if (strcmp(member->name, name) == 0) {
      return member;
    }
  }
  return NULL;
}

static bool is_imported_type(const char *name) {
  return name != NULL && strchr(name, '.') != NULL;
}

static const char *bare_type_name(const char *name) {
  const char *dot = name != NULL ? strrchr(name, '.') : NULL;
  return dot != NULL ? dot + 1 : name;
}

static const char *lvalue_root(const expr_t *target) {
  while (target->kind == EXPR_FIELD || target->kind == EXPR_INDEX) {
    target =
        target->kind == EXPR_FIELD ? target->field.base : target->index.base;
  }
  return target->kind == EXPR_ID ? target->id.name : NULL;
}

static bool is_const_init(const expr_t *expr) {
  if (expr == NULL) {
    return false;
  }

  return expr->kind == EXPR_NUMBER || expr->kind == EXPR_BOOLEAN ||
         expr->kind == EXPR_STRING;
}

static exprty_t check_expr(sema_t *sema, expr_t *expr, type_t expected);
static void resolve_qualified_type(sema_t *sema, type_t *t);
static void resolve_enum_type(sema_t *sema, type_t *t);

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
          check_expr(sema, member->global.init, member->global.type);
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

static exprty_t check_fn_call(sema_t *sema, expr_t *call, decl_t *fn,
                              const char *name) {
  if (fn->fn.variadic) {
    if (call->call.arg_count < fn->fn.params_count) {
      diag_error(sema->src, call->line, call->col,
                 "'%s' expects at least %zu argument%s but got %zu", name,
                 fn->fn.params_count, fn->fn.params_count == 1 ? "" : "s",
                 call->call.arg_count);
      sema->had_error = true;
    }
  } else {
    size_t required = 0;
    for (param_t *p = fn->fn.params; p != NULL; p = p->next) {
      if (p->default_value == NULL) {
        required++;
      }
    }
    if (required == fn->fn.params_count) {
      if (call->call.arg_count != fn->fn.params_count) {
        diag_error(sema->src, call->line, call->col,
                   "'%s' expects %zu argument%s but got %zu", name,
                   fn->fn.params_count, fn->fn.params_count == 1 ? "" : "s",
                   call->call.arg_count);
        sema->had_error = true;
      }
    } else if (call->call.arg_count < required ||
               call->call.arg_count > fn->fn.params_count) {
      diag_error(sema->src, call->line, call->col,
                 "'%s' expects between %zu and %zu arguments but got %zu", name,
                 required, fn->fn.params_count, call->call.arg_count);
      sema->had_error = true;
    }
  }

  param_t *param = fn->fn.params;
  for (expr_t *arg = call->call.args; arg != NULL; arg = arg->next) {
    exprty_t at = check_expr(
        sema, arg, param ? param->type : type_from_kind(TYPE_UNKNOWN));
    if (param != NULL) {
      if (at.ok && !assignable(param->type, at)) {
        diag_error(sema->src, call->line, call->col,
                   "cannot pass %s as argument '%s' of '%s' (expected %s)",
                   type_to_str(exprty_type(at)), param->name, name,
                   type_to_str(param->type));
        sema->had_error = true;
      }
      param = param->next;
    }
  }

  return (exprty_t){.kind = fn->fn.return_type.kind,
                    .name = fn->fn.return_type.name,
                    .element = fn->fn.return_type.element,
                    .array_length = fn->fn.return_type.array_length,
                    .ok = true};
}

static exprty_t check_call(sema_t *sema, expr_t *call) {
  const char *name = call->call.callee->id.name;
  decl_t *fn = symtab_lookup(sema->tab, name);
  if (fn == NULL) {
    diag_error(sema->src, call->line, call->col,
               "call to undefined function '%s'", name);
    sema->had_error = true;
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }
  return check_fn_call(sema, call, fn, name);
}

static exprty_t check_module_call(sema_t *sema, expr_t *call, decl_t *import) {
  expr_t *field = call->call.callee;
  const char *fn_name = field->field.name;
  module_t *mod = import->import.resolved;

  decl_t *fn = NULL;
  for (decl_t *m = mod->unit->root->container.members; m != NULL; m = m->next) {
    if (m->kind == DECL_FN && m->visibility == VISIBILITY_PUBLIC &&
        strcmp(m->name, fn_name) == 0) {
      fn = m;
      break;
    }
  }
  if (fn == NULL) {
    diag_error(sema->src, field->line, field->col,
               "module '%s' has no public function '%s'", import->import.alias,
               fn_name);
    sema->had_error = true;
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }

  exprty_t result = check_fn_call(sema, call, fn, fn_name);
  if (result.kind == TYPE_STRUCT && result.name != NULL &&
      strchr(result.name, '.') == NULL) {
    result.name = arena_format(
        sema->arena, "%s.%s", module_stem(mod->path, sema->arena), result.name);
  }
  return result;
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

static decl_t *struct_method(const decl_t *strct, const char *name) {
  for (decl_t *m = strct->strct.members; m != NULL; m = m->next) {
    if (m->kind == DECL_FN && strcmp(m->name, name) == 0) {
      return m;
    }
  }
  return NULL;
}

static exprty_t check_method_call(sema_t *sema, expr_t *call) {
  expr_t *field = call->call.callee;

  if (field->field.base->kind == EXPR_ID && sema->modules != NULL) {
    decl_t *import = modtab_lookup(sema->modules, field->field.base->id.name);
    if (import != NULL) {
      return check_module_call(sema, call, import);
    }
  }

  exprty_t recv =
      check_expr(sema, field->field.base, type_from_kind(TYPE_UNKNOWN));
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

  decl_t *strct = typetab_lookup(sema->types, recv.name);
  decl_t *method = strct != NULL && strct->kind == DECL_STRUCT
                       ? struct_method(strct, field->field.name)
                       : NULL;
  if (method == NULL) {
    diag_error(sema->src, field->line, field->col,
               "struct '%s' has no method '%s'", recv.name, field->field.name);
    sema->had_error = true;
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }
  if (is_imported_type(recv.name) && method->visibility != VISIBILITY_PUBLIC) {
    diag_error(sema->src, field->line, field->col,
               "method '%s' of struct '%s' is not public", field->field.name,
               bare_type_name(recv.name));
    sema->had_error = true;
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }

  param_t *param = method->fn.params;
  bool has_self = param != NULL && param->is_self;
  if (has_self) {
    param = param->next;
  }
  size_t expected = method->fn.params_count - (has_self ? 1 : 0);
  size_t required = 0;
  for (param_t *p = param; p != NULL; p = p->next) {
    if (p->default_value == NULL) {
      required++;
    }
  }
  if (required == expected) {
    if (call->call.arg_count != expected) {
      diag_error(sema->src, call->line, call->col,
                 "'%s' expects %zu argument%s but got %zu", field->field.name,
                 expected, expected == 1 ? "" : "s", call->call.arg_count);
      sema->had_error = true;
    }
  } else if (call->call.arg_count < required ||
             call->call.arg_count > expected) {
    diag_error(sema->src, call->line, call->col,
               "'%s' expects between %zu and %zu arguments but got %zu",
               field->field.name, required, expected, call->call.arg_count);
    sema->had_error = true;
  }

  for (expr_t *arg = call->call.args; arg != NULL; arg = arg->next) {
    exprty_t at = check_expr(
        sema, arg, param ? param->type : type_from_kind(TYPE_UNKNOWN));
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

static exprty_t check_expr(sema_t *sema, expr_t *expr, type_t expected) {
  exprty_t result;
  switch (expr->kind) {
  case EXPR_BOOLEAN:
    result = (exprty_t){.kind = TYPE_BOOL, .ok = true};
    break;
  case EXPR_NUMBER: {
    if (number_is_float(expr->number.value)) {
      TypeKind type = type_is_float(expected.kind) ? expected.kind : TYPE_F64;
      result = (exprty_t){.kind = type, .ok = true};
    } else {
      TypeKind type =
          type_is_integer(expected.kind) ? expected.kind : TYPE_I32;
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
    exprty_t operand =
        check_expr(sema, expr->cast.operand, type_from_kind(TYPE_UNKNOWN));
    resolve_enum_type(sema, &expr->cast.target);
    TypeKind target = expr->cast.target.kind;
    bool valid = (type_is_numeric(operand.kind) && type_is_numeric(target)) ||
                 (operand.kind == TYPE_ENUM && type_is_integer(target)) ||
                 (type_is_integer(operand.kind) && target == TYPE_ENUM);
    if (operand.ok && !valid) {
      diag_error(sema->src, expr->line, expr->col, "cannot cast %s to %s",
                 type_to_str(exprty_type(operand)),
                 type_to_str(expr->cast.target));
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
    } else {
      result = (exprty_t){
          .kind = target, .name = expr->cast.target.name, .ok = operand.ok};
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
      if (sema->modules != NULL &&
          modtab_lookup(sema->modules, expr->id.name) != NULL) {
        diag_error(sema->src, expr->line, expr->col,
                   "'%s' is a module, not a value", expr->id.name);
      } else if (sema->types != NULL &&
                 typetab_lookup(sema->types, expr->id.name) != NULL) {
        diag_error(sema->src, expr->line, expr->col,
                   "'%s' is a type, not a value", expr->id.name);
      } else {
        diag_error(sema->src, expr->line, expr->col, "undefined name '%s'",
                   expr->id.name);
      }
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
    bool lhs_lit = expr->binary.lhs->kind == EXPR_NUMBER ||
                   expr->binary.lhs->kind == EXPR_ENUM_LITERAL;
    bool rhs_lit = expr->binary.rhs->kind == EXPR_NUMBER ||
                   expr->binary.rhs->kind == EXPR_ENUM_LITERAL;

    exprty_t lhs, rhs;
    if (rhs_lit && !lhs_lit) {
      lhs = check_expr(sema, expr->binary.lhs, expected);
      type_t hint = type_is_numeric(lhs.kind) || lhs.kind == TYPE_ENUM
                        ? exprty_type(lhs)
                        : expected;
      rhs = check_expr(sema, expr->binary.rhs, hint);
    } else if (lhs_lit && !rhs_lit) {
      rhs = check_expr(sema, expr->binary.rhs, expected);
      type_t hint = type_is_numeric(rhs.kind) || rhs.kind == TYPE_ENUM
                        ? exprty_type(rhs)
                        : expected;
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
    } else if (lhs.kind == TYPE_STR && rhs.kind == TYPE_STR &&
               (expr->binary.op == BINOP_EQ || expr->binary.op == BINOP_NE)) {
      result = (exprty_t){.kind = TYPE_BOOL, .ok = true};
    } else if (lhs.kind == TYPE_ENUM && rhs.kind == TYPE_ENUM &&
               lhs.name != NULL && rhs.name != NULL &&
               strcmp(lhs.name, rhs.name) == 0 &&
               (expr->binary.op == BINOP_EQ || expr->binary.op == BINOP_NE)) {
      result = (exprty_t){.kind = TYPE_BOOL, .ok = true};
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
    bool is_not = expr->unary.op == UNOP_NOT;
    type_t operand_expected = is_not ? type_from_kind(TYPE_BOOL) : expected;
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
    if (expr->struct_literal.module != NULL) {
      decl_t *import =
          modtab_lookup(sema->modules, expr->struct_literal.module);
      if (import == NULL || import->import.resolved == NULL) {
        diag_error(sema->src, expr->line, expr->col, "'%s' is not a module",
                   expr->struct_literal.module);
        sema->had_error = true;
        result = (exprty_t){.kind = TYPE_VOID, .ok = false};
        break;
      }
      if (module_pub_struct(import->import.resolved,
                            expr->struct_literal.type_name) == NULL) {
        diag_error(sema->src, expr->line, expr->col,
                   "module '%s' has no public struct '%s'",
                   expr->struct_literal.module, expr->struct_literal.type_name);
        sema->had_error = true;
        result = (exprty_t){.kind = TYPE_VOID, .ok = false};
        break;
      }
      expr->struct_literal.type_name =
          arena_format(sema->arena, "%s.%s",
                       module_stem(import->import.resolved->path, sema->arena),
                       expr->struct_literal.type_name);
      expr->struct_literal.module = NULL;
    }
    decl_t *strct = typetab_lookup(sema->types, expr->struct_literal.type_name);
    if (strct == NULL) {
      diag_error(sema->src, expr->line, expr->col, "unknown type '%s'",
                 expr->struct_literal.type_name);
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    if (strct->kind != DECL_STRUCT) {
      diag_error(sema->src, expr->line, expr->col, "'%s' is not a struct",
                 expr->struct_literal.type_name);
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }

    bool ok = true;
    for (field_init_t *init = expr->struct_literal.inits; init != NULL;
         init = init->next) {
      field_t *field = struct_field(strct, init->name);
      exprty_t value = check_expr(
          sema, init->value, field ? field->type : type_from_kind(TYPE_UNKNOWN));
      if (field == NULL) {
        diag_error(sema->src, init->line, init->col,
                   "struct '%s' has no field '%s'", strct->name, init->name);
        sema->had_error = true;
        ok = false;
        continue;
      }
      if (is_imported_type(expr->struct_literal.type_name) &&
          field->visibility != VISIBILITY_PUBLIC) {
        diag_error(sema->src, init->line, init->col,
                   "field '%s' of struct '%s' is not public", init->name,
                   bare_type_name(expr->struct_literal.type_name));
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

    for (field_t *field = strct->strct.fields; field != NULL;
         field = field->next) {
      bool found = false;
      for (field_init_t *init = expr->struct_literal.inits; init != NULL;
           init = init->next) {
        if (strcmp(init->name, field->name) == 0) {
          found = true;
          break;
        }
      }
      if (!found && field->default_value == NULL) {
        diag_error(sema->src, expr->line, expr->col,
                   "missing field '%s' in initializer for '%s'", field->name,
                   strct->name);
        sema->had_error = true;
        ok = false;
      }
    }

    result = (exprty_t){
        .kind = TYPE_STRUCT, .name = expr->struct_literal.type_name, .ok = ok};
    break;
  }
  case EXPR_FIELD: {
    if (expr->field.base->kind == EXPR_ID &&
        (sema->scope == NULL ||
         scope_lookup(sema->scope, expr->field.base->id.name) == NULL) &&
        (sema->globals == NULL ||
         scope_lookup(sema->globals, expr->field.base->id.name) == NULL)) {
      decl_t *enm = typetab_lookup(sema->types, expr->field.base->id.name);
      if (enm != NULL && enm->kind == DECL_ENUM) {
        enum_member_t *member = enum_member(enm, expr->field.name);
        if (member == NULL) {
          diag_error(sema->src, expr->line, expr->col,
                     "enum '%s' has no member '%s'", enm->name,
                     expr->field.name);
          sema->had_error = true;
          result = (exprty_t){.kind = TYPE_VOID, .ok = false};
          break;
        }
        expr->kind = EXPR_NUMBER;
        expr->number.value =
            arena_format(sema->arena, "%lld", member->value);
        result = (exprty_t){.kind = TYPE_ENUM, .name = enm->name, .ok = true};
        break;
      }
    }
    exprty_t base =
        check_expr(sema, expr->field.base, type_from_kind(TYPE_UNKNOWN));
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
    if (base.kind == TYPE_SLICE) {
      if (strcmp(expr->field.name, "len") == 0) {
        result = (exprty_t){.kind = TYPE_I64, .ok = true};
      } else if (strcmp(expr->field.name, "ptr") == 0) {
        result = (exprty_t){.kind = TYPE_CSTR, .ok = true};
      } else {
        diag_error(sema->src, expr->line, expr->col, "slice has no field '%s'",
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
    decl_t *strct = typetab_lookup(sema->types, base.name);
    field_t *field = strct != NULL && strct->kind == DECL_STRUCT
                         ? struct_field(strct, expr->field.name)
                         : NULL;
    if (field == NULL) {
      diag_error(sema->src, expr->line, expr->col,
                 "struct '%s' has no field '%s'", base.name, expr->field.name);
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    if (is_imported_type(base.name) && field->visibility != VISIBILITY_PUBLIC) {
      diag_error(sema->src, expr->line, expr->col,
                 "field '%s' of struct '%s' is not public", expr->field.name,
                 bare_type_name(base.name));
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
  case EXPR_ENUM_LITERAL: {
    type_t want = expected;
    resolve_enum_type(sema, &want);
    decl_t *enm =
        want.name != NULL ? typetab_lookup(sema->types, want.name) : NULL;
    if (want.kind != TYPE_ENUM || enm == NULL || enm->kind != DECL_ENUM) {
      diag_error(sema->src, expr->line, expr->col,
                 "cannot infer enum type for '.%s'", expr->enum_literal.name);
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    enum_member_t *member = enum_member(enm, expr->enum_literal.name);
    if (member == NULL) {
      diag_error(sema->src, expr->line, expr->col,
                 "enum '%s' has no member '%s'", enm->name,
                 expr->enum_literal.name);
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    expr->kind = EXPR_NUMBER;
    expr->number.value = arena_format(sema->arena, "%lld", member->value);
    result = (exprty_t){.kind = TYPE_ENUM, .name = enm->name, .ok = true};
    break;
  }
  case EXPR_ARRAY: {
    type_t elem = {.kind = TYPE_UNKNOWN};
    size_t count = 0;
    bool ok = true;
    for (expr_t *e = expr->array.elements; e != NULL; e = e->next) {
      exprty_t et = check_expr(sema, e, type_from_kind(TYPE_UNKNOWN));
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
    exprty_t base =
        check_expr(sema, expr->index.base, type_from_kind(TYPE_UNKNOWN));
    exprty_t idx =
        check_expr(sema, expr->index.index, type_from_kind(TYPE_I32));
    if (!base.ok) {
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    if (base.kind != TYPE_ARRAY && base.kind != TYPE_SLICE &&
        base.kind != TYPE_STR) {
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
    if (base.kind == TYPE_STR) {
      result = (exprty_t){.kind = TYPE_U8, .ok = true};
      break;
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

  exprty_t value = check_expr(sema, stmt->ret.value, fn->fn.return_type);

  if (ret == TYPE_VOID) {
    diag_error(sema->src, stmt->line, stmt->col,
               "void function '%s' cannot return a value", fn->name);
    sema->had_error = true;
    return;
  }

  if (value.ok && !assignable(fn->fn.return_type, value)) {
    diag_error(sema->src, stmt->line, stmt->col,
               "cannot return %s from function returning %s",
               type_to_str(exprty_type(value)),
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
    check_expr(sema, stmt->expr_stmt.expr, type_from_kind(TYPE_UNKNOWN));
    break;
  case STMT_IF: {
    exprty_t cond =
        check_expr(sema, stmt->if_stmt.cond, type_from_kind(TYPE_BOOL));
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
    resolve_qualified_type(sema, &stmt->binding.type);
    resolve_enum_type(sema, &stmt->binding.type);
    if (stmt->binding.init != NULL) {
      exprty_t init =
          check_expr(sema, stmt->binding.init, stmt->binding.type);
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
    exprty_t lhs = check_expr(sema, target, type_from_kind(TYPE_UNKNOWN));

    if (target->kind == EXPR_INDEX &&
        target->index.base->type.kind == TYPE_STR) {
      diag_error(
          sema->src, stmt->line, stmt->col,
          "cannot assign through index into str (strings are immutable)");
      sema->had_error = true;
      check_expr(sema, stmt->assign.value, type_from_kind(TYPE_UNKNOWN));
      break;
    }

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
      check_expr(sema, stmt->assign.value, type_from_kind(TYPE_UNKNOWN));
      break;
    }

    exprty_t value = check_expr(sema, stmt->assign.value, exprty_type(lhs));
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
    exprty_t cond =
        check_expr(sema, stmt->while_loop.cond, type_from_kind(TYPE_BOOL));
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
    type_t var_type;
    if (stmt->for_loop.iterable != NULL) {
      exprty_t iter =
          check_expr(sema, stmt->for_loop.iterable, type_from_kind(TYPE_UNKNOWN));
      if (iter.ok && iter.kind != TYPE_SLICE && iter.kind != TYPE_ARRAY) {
        diag_error(sema->src, stmt->for_loop.iterable->line,
                   stmt->for_loop.iterable->col,
                   "for-in loop requires a slice or array, got %s",
                   type_kind_to_str(iter.kind));
        sema->had_error = true;
        var_type = (type_t){.kind = TYPE_I32};
      } else {
        var_type = iter.element ? *iter.element : (type_t){.kind = TYPE_I32};
      }
    } else {
      exprty_t start =
          check_expr(sema, stmt->for_loop.start, type_from_kind(TYPE_UNKNOWN));
      exprty_t end =
          check_expr(sema, stmt->for_loop.end, type_from_kind(TYPE_UNKNOWN));

      TypeKind kind = TYPE_I32;
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
        kind = start.kind;
      }
      var_type = (type_t){.kind = kind};
    }

    size_t saved = sema->scope->count;
    sema->scope->items[sema->scope->count++] = (local_t){
        .name = stmt->for_loop.var,
        .type = var_type,
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

static void resolve_qualified_type(sema_t *sema, type_t *t) {
  if (t->kind != TYPE_STRUCT || t->module == NULL) {
    return;
  }
  decl_t *import = modtab_lookup(sema->modules, t->module);
  if (import == NULL || import->import.resolved == NULL) {
    diag_error(sema->src, t->line, t->col, "'%s' is not a module", t->module);
    sema->had_error = true;
    t->kind = TYPE_UNKNOWN;
    t->module = NULL;
    return;
  }
  if (module_pub_struct(import->import.resolved, t->name) == NULL) {
    diag_error(sema->src, t->line, t->col,
               "module '%s' has no public struct '%s'", t->module, t->name);
    sema->had_error = true;
    t->kind = TYPE_UNKNOWN;
    t->module = NULL;
    return;
  }
  t->name = arena_format(
      sema->arena, "%s.%s",
      module_stem(import->import.resolved->path, sema->arena), t->name);
  t->module = NULL;
}

static void resolve_enum_type(sema_t *sema, type_t *t) {
  if (t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE) {
    resolve_enum_type(sema, t->element);
    return;
  }
  if (t->kind != TYPE_STRUCT || t->module != NULL) {
    return;
  }
  decl_t *decl = typetab_lookup(sema->types, t->name);
  if (decl != NULL && decl->kind == DECL_ENUM) {
    t->kind = TYPE_ENUM;
  }
}

static bool check_type(sema_t *sema, type_t *type) {
  if (type->kind == TYPE_ARRAY || type->kind == TYPE_SLICE) {
    return check_type(sema, type->element);
  }
  if (type->kind == TYPE_STRUCT) {
    decl_t *decl = typetab_lookup(sema->types, type->name);
    if (decl == NULL) {
      diag_error(sema->src, type->line, type->col, "unknown type '%s'",
                 type->name);
      sema->had_error = true;
      return false;
    }
    if (decl->kind == DECL_ENUM) {
      type->kind = TYPE_ENUM;
    }
  }
  return true;
}

static void resolve_fn_signature(sema_t *sema, decl_t *fn) {
  for (param_t *param = fn->fn.params; param != NULL; param = param->next) {
    resolve_qualified_type(sema, &param->type);
    resolve_enum_type(sema, &param->type);
  }
  resolve_enum_type(sema, &fn->fn.return_type);
}

static void check_fn(sema_t *sema, decl_t *fn);

static void check_struct(sema_t *sema, const decl_t *strct) {
  for (field_t *field = strct->strct.fields; field != NULL;
       field = field->next) {
    check_type(sema, &field->type);

    for (field_t *prev = strct->strct.fields; prev != field;
         prev = prev->next) {
      if (strcmp(prev->name, field->name) == 0) {
        diag_error(sema->src, field->line, field->col, "duplicate field '%s'",
                   field->name);
        sema->had_error = true;
        break;
      }
    }

    if (field->default_value != NULL) {
      if (!is_const_init(field->default_value)) {
        diag_error(sema->src, field->default_value->line,
                   field->default_value->col,
                   "struct field default must be a constant");
        sema->had_error = true;
      } else {
        exprty_t dty = check_expr(sema, field->default_value, field->type);
        if (dty.ok && !assignable(field->type, dty)) {
          diag_error(sema->src, field->default_value->line,
                     field->default_value->col,
                     "cannot assign %s to field '%s' of type %s",
                     type_kind_to_str(dty.kind), field->name,
                     type_to_str(field->type));
          sema->had_error = true;
        }
      }
    }
  }
  for (decl_t *member = strct->strct.members; member != NULL;
       member = member->next) {
    check_fn(sema, member);
  }
}

static void check_enum(sema_t *sema, const decl_t *enm) {
  for (enum_member_t *member = enm->enm.members; member != NULL;
       member = member->next) {
    for (enum_member_t *prev = enm->enm.members; prev != member;
         prev = prev->next) {
      if (strcmp(prev->name, member->name) == 0) {
        diag_error(sema->src, member->line, member->col,
                   "duplicate enum member '%s'", member->name);
        sema->had_error = true;
        break;
      }
    }
  }
}

static void check_fn(sema_t *sema, decl_t *fn) {
  bool seen_default = false;
  for (param_t *param = fn->fn.params; param != NULL; param = param->next) {
    resolve_qualified_type(sema, &param->type);
    check_type(sema, &param->type);

    if (param->default_value != NULL) {
      if (fn->fn.variadic) {
        diag_error(sema->src, fn->line, fn->col,
                   "default parameters cannot be combined with variadic '...'");
        sema->had_error = true;
      }
      if (!is_const_init(param->default_value)) {
        diag_error(sema->src, param->default_value->line,
                   param->default_value->col,
                   "parameter default must be a constant");
        sema->had_error = true;
      } else {
        exprty_t dty = check_expr(sema, param->default_value, param->type);
        if (dty.ok && !assignable(param->type, dty)) {
          diag_error(sema->src, param->default_value->line,
                     param->default_value->col,
                     "cannot assign %s to parameter '%s' of type %s",
                     type_kind_to_str(dty.kind), param->name,
                     type_to_str(param->type));
          sema->had_error = true;
        }
      }
      seen_default = true;
    } else if (seen_default && !param->is_self) {
      diag_error(sema->src, fn->line, fn->col,
                 "parameter '%s' without a default cannot follow a defaulted "
                 "parameter",
                 param->name);
      sema->had_error = true;
    }
  }
  check_type(sema, &fn->fn.return_type);

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
  decl_t *main = symtab_lookup(tab, "main");
  if (main == NULL) {
    diag_error_nofile("no entry point: 'main' function is required");
    return false;
  }

  bool ok = true;
  if (main->visibility != VISIBILITY_PUBLIC) {
    diag_error(src, main->line, main->col,
               "'main' must be public (declared with 'pub')");
    ok = false;
  }
  if (main->fn.return_type.kind != TYPE_I32) {
    diag_error(src, main->line, main->col, "'main' must return i32");
    ok = false;
  }

  if (main->fn.params_count > 1) {
    diag_error(
        src, main->line, main->col,
        "'main' takes at most one parameter (the argument list as '[]str')");
    return false;
  }

  if (main->fn.params == 0) {
    return ok;
  }

  if (main->fn.params->type.kind != TYPE_SLICE ||
      main->fn.params->type.element->kind != TYPE_STR) {
    diag_error(src, main->line, main->col,
               "'main' parameter must be of type '[]str'");
    return false;
  }

  return ok;
}

int semantic_check(unit_t *unit, arena_t *arena, bool require_main) {
  if (unit == NULL) {
    return false;
  }

  decl_t *root = unit->root;
  bool had_error = false;

  size_t capacity = root->container.member_count;
  size_t type_capacity = capacity;
  for (decl_t *m = root->container.members; m != NULL; m = m->next) {
    if (m->kind == DECL_IMPORT && m->import.resolved != NULL) {
      type_capacity += m->import.resolved->unit->root->container.member_count;
    }
  }

  modtab_t modules = {
      .imports =
          arena_alloc(arena, sizeof(decl_t *) * (capacity ? capacity : 1)),
      .count = 0,
  };
  symtab_t tab = {
      .fns = arena_alloc(arena, sizeof(decl_t *) * (capacity ? capacity : 1)),
      .count = 0,
  };
  typetab_t types = {
      .structs = arena_alloc(arena, sizeof(typeent_t) *
                                        (type_capacity ? type_capacity : 1)),
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
      types.structs[types.count++] =
          (typeent_t){.name = member->name, .decl = member};
      break;
    }
    case DECL_ENUM:
      if (typetab_lookup(&types, member->name)) {
        diag_error(unit->src, member->line, member->col,
                   "redefinition of type '%s'", member->name);
        had_error = true;
        break;
      }
      types.structs[types.count++] =
          (typeent_t){.name = member->name, .decl = member};
      break;
    case DECL_IMPORT:
      modules.imports[modules.count++] = member;
      break;
    case DECL_CONTAINER:
    case DECL_GLOBAL:
      break;
    }
  }

  if (require_main && !check_entry_point(&tab, unit->src)) {
    had_error = true;
  }

  scope_t globals = {
      .items = arena_alloc(arena, sizeof(local_t) * (capacity ? capacity : 1)),
      .count = 0,
  };

  sema_t sema = {
      .modules = &modules,
      .tab = &tab,
      .types = &types,
      .src = unit->src,
      .globals = &globals,
      .scope = NULL,
      .arena = arena,
      .had_error = false,
  };

  for (size_t i = 0; i < tab.count; i++) {
    resolve_fn_signature(&sema, tab.fns[i]);
  }
  for (size_t i = 0; i < types.count; i++) {
    if (types.structs[i].decl->kind != DECL_STRUCT) {
      continue;
    }
    for (decl_t *m = types.structs[i].decl->strct.members; m != NULL;
         m = m->next) {
      resolve_fn_signature(&sema, m);
    }
  }

  for (size_t i = 0; i < types.count; i++) {
    if (types.structs[i].decl->kind == DECL_ENUM) {
      check_enum(&sema, types.structs[i].decl);
    } else {
      check_struct(&sema, types.structs[i].decl);
    }
  }

  for (size_t i = 0; i < modules.count; i++) {
    decl_t *imp = modules.imports[i];
    if (imp->import.resolved == NULL) {
      continue;
    }
    const char *stem = module_stem(imp->import.resolved->path, arena);
    for (decl_t *m = imp->import.resolved->unit->root->container.members;
         m != NULL; m = m->next) {
      if (m->kind == DECL_STRUCT && m->visibility == VISIBILITY_PUBLIC) {
        types.structs[types.count++] = (typeent_t){
            .name = arena_format(arena, "%s.%s", stem, m->name), .decl = m};
      }
    }
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
