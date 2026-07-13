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

static decl_t *module_pub_enum(module_t *mod, const char *name) {
  for (decl_t *m = mod->unit->root->container.members; m != NULL; m = m->next) {
    if (m->kind == DECL_ENUM && m->visibility == VISIBILITY_PUBLIC &&
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

typedef struct modnode modnode_t;
struct modnode {
  module_t *mod;
  modnode_t *next;
};

static bool modnode_contains(const modnode_t *head, const module_t *mod) {
  for (; head != NULL; head = head->next) {
    if (head->mod == mod) {
      return true;
    }
  }
  return false;
}

static modnode_t *collect_pub_modules(modnode_t *head, module_t *mod,
                                      arena_t *arena) {
  if (mod == NULL || modnode_contains(head, mod)) {
    return head;
  }
  modnode_t *node = arena_alloc(arena, sizeof(modnode_t));
  node->mod = mod;
  node->next = head;
  head = node;
  for (decl_t *m = mod->unit->root->container.members; m != NULL; m = m->next) {
    if (m->kind == DECL_IMPORT && m->visibility == VISIBILITY_PUBLIC) {
      head = collect_pub_modules(head, m->import.resolved, arena);
    }
  }
  return head;
}

typedef struct {
  const modtab_t *modules;
  const modnode_t *reachable;
  const symtab_t *tab;
  const typetab_t *types;
  const source_t *src;
  scope_t *globals;
  scope_t *scope;
  const decl_t *current_fn;
  const char *module_stem;
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

static bool enum_name_eq(const char *a, const char *b) {
  if (a == NULL || b == NULL) {
    return false;
  }
  if (strcmp(a, b) == 0) {
    return true;
  }
  const char *adot = strrchr(a, '.');
  const char *bdot = strrchr(b, '.');
  if ((adot != NULL) == (bdot != NULL)) {
    return false;
  }
  return strcmp(adot != NULL ? adot + 1 : a, bdot != NULL ? bdot + 1 : b) == 0;
}

static bool types_equal(type_t a, type_t b) {
  if (a.kind != b.kind) {
    return false;
  }
  if (a.kind == TYPE_ENUM) {
    return enum_name_eq(a.name, b.name);
  }
  if (a.kind == TYPE_STRUCT) {
    return enum_name_eq(a.name, b.name);
  }
  if (a.kind == TYPE_ARRAY) {
    return a.array_length == b.array_length &&
           types_equal(*a.element, *b.element);
  }
  if (a.kind == TYPE_SLICE || a.kind == TYPE_OPTIONAL) {
    return types_equal(*a.element, *b.element);
  }
  return true;
}

static bool assignable(type_t to, exprty_t from) {
  if (to.kind == TYPE_CSTR && from.kind == TYPE_STR) {
    return true;
  }
  if (to.kind == TYPE_OPTIONAL && from.kind != TYPE_OPTIONAL) {
    return assignable(*to.element, from);
  }
  if (to.kind == TYPE_OPTIONAL && from.kind == TYPE_OPTIONAL) {
    return from.element != NULL && types_equal(*to.element, *from.element);
  }
  if (to.kind == TYPE_SLICE &&
      (from.kind == TYPE_ARRAY || from.kind == TYPE_SLICE)) {
    return from.element != NULL && types_equal(*to.element, *from.element);
  }
  if (to.kind != from.kind) {
    return false;
  }
  if (to.kind == TYPE_ENUM) {
    return enum_name_eq(to.name, from.name);
  }
  if (to.kind == TYPE_STRUCT) {
    return enum_name_eq(to.name, from.name);
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

static decl_t *imports_pub_enum(const sema_t *sema, const char *name) {
  for (const modnode_t *n = sema->reachable; n != NULL; n = n->next) {
    for (decl_t *m = n->mod->unit->root->container.members; m != NULL;
         m = m->next) {
      if (m->kind == DECL_ENUM && m->visibility == VISIBILITY_PUBLIC &&
          strcmp(m->name, name) == 0) {
        return m;
      }
    }
  }
  return NULL;
}

static bool sema_enum_is_tagged(const sema_t *sema, const char *name) {
  if (name == NULL) {
    return false;
  }
  decl_t *enm = typetab_lookup(sema->types, name);
  if (enm == NULL) {
    enm = imports_pub_enum(sema, name);
  }
  return enm != NULL && enm->kind == DECL_ENUM && enm->enm.tagged;
}

static module_t *resolve_module_alias_path(const sema_t *sema,
                                           const char *path) {
  if (sema->modules == NULL) {
    return NULL;
  }
  module_t *mod = NULL;
  const char *seg = path;
  for (;;) {
    const char *dot = strchr(seg, '.');
    size_t len = dot != NULL ? (size_t)(dot - seg) : strlen(seg);
    char *name = arena_format(sema->arena, "%.*s", (int)len, seg);
    if (mod == NULL) {
      decl_t *imp = modtab_lookup(sema->modules, name);
      mod = imp != NULL ? imp->import.resolved : NULL;
    } else {
      module_t *next = NULL;
      for (decl_t *m = mod->unit->root->container.members; m != NULL;
           m = m->next) {
        if (m->kind == DECL_IMPORT && m->visibility == VISIBILITY_PUBLIC &&
            strcmp(m->name, name) == 0) {
          next = m->import.resolved;
          break;
        }
      }
      mod = next;
    }
    if (mod == NULL || dot == NULL) {
      return mod;
    }
    seg = dot + 1;
  }
}

static module_t *resolve_module_path(const sema_t *sema, const expr_t *expr) {
  if (expr->kind == EXPR_ID) {
    if (sema->modules == NULL) {
      return NULL;
    }
    decl_t *imp = modtab_lookup(sema->modules, expr->id.name);
    return imp != NULL ? imp->import.resolved : NULL;
  }
  if (expr->kind != EXPR_FIELD) {
    return NULL;
  }
  module_t *base = resolve_module_path(sema, expr->field.base);
  if (base == NULL) {
    return NULL;
  }
  for (decl_t *m = base->unit->root->container.members; m != NULL;
       m = m->next) {
    if (m->kind == DECL_IMPORT && m->visibility == VISIBILITY_PUBLIC &&
        strcmp(m->name, expr->field.name) == 0) {
      return m->import.resolved;
    }
  }
  return NULL;
}

static bool pattern_int_value(const expr_t *e, long long *out) {
  if (e->kind == EXPR_NUMBER) {
    *out = strtoll(e->number.value, NULL, 0);
    return true;
  }
  if (e->kind == EXPR_UNARY && e->unary.op == UNOP_NEG &&
      e->unary.operand->kind == EXPR_NUMBER) {
    *out = -strtoll(e->unary.operand->number.value, NULL, 0);
    return true;
  }
  return false;
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
         expr->kind == EXPR_STRING || expr->kind == EXPR_NULL;
}

static exprty_t check_expr(sema_t *sema, expr_t *expr, type_t expected);
static void check_stmt(sema_t *sema, stmt_t *stmt, const decl_t *fn);
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

static bool vararg_boxable(TypeKind kind) {
  return type_is_integer(kind) || type_is_float(kind) || kind == TYPE_BOOL ||
         kind == TYPE_STR || kind == TYPE_ENUM;
}

static type_t qualify_param_type(sema_t *sema, type_t t, const char *stem) {
  if (stem == NULL) {
    return t;
  }
  if ((t.kind == TYPE_STRUCT || t.kind == TYPE_ENUM) && t.name != NULL &&
      strchr(t.name, '.') == NULL) {
    t.name = arena_format(sema->arena, "%s.%s", stem, t.name);
    return t;
  }
  if ((t.kind == TYPE_SLICE || t.kind == TYPE_ARRAY ||
       t.kind == TYPE_OPTIONAL) &&
      t.element != NULL) {
    type_t *element = arena_alloc(sema->arena, sizeof(type_t));
    *element = qualify_param_type(sema, *t.element, stem);
    t.element = element;
  }
  return t;
}

static exprty_t qualify_result(sema_t *sema, exprty_t result,
                               const char *stem) {
  if (stem == NULL || !result.ok) {
    return result;
  }
  type_t t = qualify_param_type(sema, exprty_type(result), stem);
  result.name = t.name;
  result.element = t.element;
  return result;
}

static exprty_t check_fn_call(sema_t *sema, expr_t *call, decl_t *fn,
                              const char *name, const char *stem) {
  bool boxed_variadic = fn->fn.variadic && !fn->fn.is_extern;
  size_t fixed = boxed_variadic ? fn->fn.params_count - 1 : fn->fn.params_count;
  if (fn->fn.variadic) {
    if (call->call.arg_count < fixed) {
      diag_error(sema->src, call->line, call->col,
                 "'%s' expects at least %zu argument%s but got %zu", name,
                 fixed, fixed == 1 ? "" : "s", call->call.arg_count);
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
  size_t index = 0;
  for (expr_t *arg = call->call.args; arg != NULL; arg = arg->next, index++) {
    if (boxed_variadic && index >= fixed) {
      exprty_t at = check_expr(sema, arg, type_from_kind(TYPE_UNKNOWN));
      if (at.ok && !vararg_boxable(at.kind)) {
        diag_error(sema->src, arg->line, arg->col,
                   "cannot pass %s as a variadic argument",
                   type_to_str(exprty_type(at)));
        sema->had_error = true;
      }
      continue;
    }
    type_t ptype = param != NULL ? qualify_param_type(sema, param->type, stem)
                                 : type_from_kind(TYPE_UNKNOWN);
    exprty_t at = check_expr(sema, arg, ptype);
    if (param != NULL) {
      if (at.ok && !assignable(ptype, at)) {
        diag_error(sema->src, call->line, call->col,
                   "cannot pass %s as argument '%s' of '%s' (expected %s)",
                   type_to_str(exprty_type(at)), param->name, name,
                   type_to_str(ptype));
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
  return check_fn_call(sema, call, fn, name, NULL);
}

static exprty_t check_module_call(sema_t *sema, expr_t *call, module_t *mod,
                                  const char *display) {
  expr_t *field = call->call.callee;
  const char *fn_name = field->field.name;

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
               "module '%s' has no public function '%s'", display, fn_name);
    sema->had_error = true;
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }

  const char *stem = module_stem(mod->path, sema->arena);
  exprty_t result = check_fn_call(sema, call, fn, fn_name, stem);
  return qualify_result(sema, result, stem);
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

  module_t *mod = resolve_module_path(sema, field->field.base);
  if (mod != NULL) {
    const char *display = field->field.base->kind == EXPR_ID
                              ? field->field.base->id.name
                              : field->field.base->field.name;
    return check_module_call(sema, call, mod, display);
  }
  decl_t *ts_owner = NULL;
  const char *ts_stem = NULL;
  if (field->field.base->kind == EXPR_ID &&
      (sema->scope == NULL ||
       scope_lookup(sema->scope, field->field.base->id.name) == NULL) &&
      (sema->globals == NULL ||
       scope_lookup(sema->globals, field->field.base->id.name) == NULL)) {
    ts_owner = typetab_lookup(sema->types, field->field.base->id.name);
  } else if (field->field.base->kind == EXPR_FIELD) {
    module_t *owner_mod =
        resolve_module_path(sema, field->field.base->field.base);
    if (owner_mod != NULL) {
      ts_owner = module_pub_struct(owner_mod, field->field.base->field.name);
      if (ts_owner == NULL) {
        ts_owner = module_pub_enum(owner_mod, field->field.base->field.name);
      }
      if (ts_owner != NULL) {
        ts_stem = module_stem(owner_mod->path, sema->arena);
      }
    }
  }
  if (ts_owner != NULL && ts_owner->kind == DECL_ENUM && ts_owner->enm.tagged &&
      enum_member(ts_owner, field->field.name) != NULL) {
    if (call->call.arg_count > 1) {
      diag_error(sema->src, call->line, call->col,
                 "variant '%s' takes a single payload", field->field.name);
      sema->had_error = true;
      return (exprty_t){.kind = TYPE_VOID, .ok = false};
    }
    char *ename = ts_stem != NULL ? arena_format(sema->arena, "%s.%s", ts_stem,
                                                 ts_owner->name)
                                  : ts_owner->name;
    expr_t *payload = call->call.args;
    call->kind = EXPR_ENUM_LITERAL;
    call->enum_literal.name = field->field.name;
    call->enum_literal.payload = payload;
    return check_expr(sema, call, (type_t){.kind = TYPE_ENUM, .name = ename});
  }

  if (ts_owner != NULL &&
      (ts_owner->kind == DECL_STRUCT || ts_owner->kind == DECL_ENUM)) {
    const char *kind_str = ts_owner->kind == DECL_ENUM ? "enum" : "struct";
    decl_t *members = ts_owner->kind == DECL_STRUCT ? ts_owner->strct.members
                                                    : ts_owner->enm.methods;
    decl_t *fn = NULL;
    for (decl_t *m = members; m != NULL; m = m->next) {
      if (m->kind == DECL_FN && strcmp(m->name, field->field.name) == 0) {
        fn = m;
        break;
      }
    }
    if (fn == NULL) {
      if (ts_owner->kind == DECL_ENUM && field->field.base->kind == EXPR_ID) {
        diag_error(sema->src, field->line, field->col,
                   "enum '%s' has no function '%s'", ts_owner->name,
                   field->field.name);
      } else {
        diag_error(sema->src, field->line, field->col,
                   "%s '%s' has no function '%s'", kind_str, ts_owner->name,
                   field->field.name);
      }
      sema->had_error = true;
      return (exprty_t){.kind = TYPE_VOID, .ok = false};
    }
    if (fn->fn.params != NULL && fn->fn.params->is_self) {
      diag_error(sema->src, field->line, field->col,
                 "method '%s' of %s '%s' requires an instance",
                 field->field.name, kind_str, ts_owner->name);
      sema->had_error = true;
      return (exprty_t){.kind = TYPE_VOID, .ok = false};
    }
    if (ts_stem != NULL && fn->visibility != VISIBILITY_PUBLIC) {
      diag_error(sema->src, field->line, field->col,
                 "method '%s' of %s '%s' is not public", field->field.name,
                 kind_str, ts_owner->name);
      sema->had_error = true;
      return (exprty_t){.kind = TYPE_VOID, .ok = false};
    }
    const char *qualified =
        ts_stem != NULL
            ? arena_format(sema->arena, "%s.%s", ts_stem, ts_owner->name)
            : ts_owner->name;
    field->field.base->type =
        (type_t){.kind = ts_owner->kind == DECL_ENUM ? TYPE_ENUM : TYPE_STRUCT,
                 .name = (char *)qualified};
    call->call.type_scoped = true;
    exprty_t r = check_fn_call(sema, call, fn, field->field.name, ts_stem);
    return qualify_result(sema, r, ts_stem);
  }

  if (field->field.base->kind == EXPR_FIELD &&
      resolve_module_path(sema, field->field.base->field.base) != NULL) {
    expr_t *base = field->field.base;
    const char *display = base->field.base->kind == EXPR_ID
                              ? base->field.base->id.name
                              : base->field.base->field.name;
    diag_error(sema->src, base->line, base->col,
               "module '%s' has no public module '%s'", display,
               base->field.name);
    sema->had_error = true;
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }

  exprty_t recv =
      check_expr(sema, field->field.base, type_from_kind(TYPE_UNKNOWN));
  if (!recv.ok) {
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }
  decl_t *method = NULL;
  bool imported = is_imported_type(recv.name);
  if (recv.kind == TYPE_STRUCT) {
    decl_t *strct = typetab_lookup(sema->types, recv.name);
    method = strct != NULL && strct->kind == DECL_STRUCT
                 ? struct_method(strct, field->field.name)
                 : NULL;
    if (method == NULL) {
      diag_error(sema->src, field->line, field->col,
                 "struct '%s' has no method '%s'", recv.name,
                 field->field.name);
      sema->had_error = true;
      return (exprty_t){.kind = TYPE_VOID, .ok = false};
    }
  } else if (recv.kind == TYPE_ENUM && recv.name != NULL) {
    decl_t *enm = typetab_lookup(sema->types, recv.name);
    if (enm == NULL) {
      enm = imports_pub_enum(sema, recv.name);
      if (enm != NULL) {
        imported = true;
      }
    }
    if (enm != NULL && enm->kind == DECL_ENUM) {
      for (decl_t *m = enm->enm.methods; m != NULL; m = m->next) {
        if (m->kind == DECL_FN && strcmp(m->name, field->field.name) == 0) {
          method = m;
          break;
        }
      }
    }
    if (method == NULL) {
      diag_error(sema->src, field->line, field->col,
                 "enum '%s' has no method '%s'", recv.name, field->field.name);
      sema->had_error = true;
      return (exprty_t){.kind = TYPE_VOID, .ok = false};
    }
  } else {
    diag_error(sema->src, field->line, field->col,
               "cannot call method '%s' on non-struct type %s",
               field->field.name,
               type_to_str((type_t){.kind = recv.kind, .name = recv.name}));
    sema->had_error = true;
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }
  if (imported && method->visibility != VISIBILITY_PUBLIC) {
    diag_error(sema->src, field->line, field->col,
               "method '%s' of %s '%s' is not public", field->field.name,
               recv.kind == TYPE_ENUM ? "enum" : "struct",
               bare_type_name(recv.name));
    sema->had_error = true;
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }

  param_t *param = method->fn.params;
  bool has_self = param != NULL && param->is_self;
  if (!has_self) {
    diag_error(sema->src, field->line, field->col,
               "'%s' does not take 'self'; call it through the type",
               field->field.name);
    sema->had_error = true;
    return (exprty_t){.kind = TYPE_VOID, .ok = false};
  }
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
                   type_to_str(exprty_type(at)), param->name, field->field.name,
                   type_to_str(param->type));
        sema->had_error = true;
      }
      param = param->next;
    }
  }

  exprty_t r = (exprty_t){.kind = method->fn.return_type.kind,
                          .name = method->fn.return_type.name,
                          .element = method->fn.return_type.element,
                          .array_length = method->fn.return_type.array_length,
                          .ok = true};
  if (recv.name != NULL) {
    const char *dot = strrchr(recv.name, '.');
    if (dot != NULL) {
      const char *stem =
          arena_format(sema->arena, "%.*s", (int)(dot - recv.name), recv.name);
      r = qualify_result(sema, r, stem);
    }
  }
  return r;
}

static exprty_t check_expr(sema_t *sema, expr_t *expr, type_t expected) {
  exprty_t result;
  switch (expr->kind) {
  case EXPR_BOOLEAN:
    result = (exprty_t){.kind = TYPE_BOOL, .ok = true};
    break;
  case EXPR_NUMBER: {
    type_t want = expected;
    while (want.kind == TYPE_OPTIONAL && want.element != NULL) {
      want = *want.element;
    }
    if (number_is_float(expr->number.value)) {
      TypeKind type = type_is_float(want.kind) ? want.kind : TYPE_F64;
      result = (exprty_t){.kind = type, .ok = true};
    } else {
      TypeKind type = type_is_integer(want.kind) ? want.kind : TYPE_I32;
      if (check_literal_fit(sema, expr, type)) {
        result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      } else {
        result = (exprty_t){.kind = type, .ok = true};
      }
    }
    break;
  }
  case EXPR_NULL:
    if (expected.kind == TYPE_OPTIONAL && expected.element != NULL) {
      result = (exprty_t){
          .kind = TYPE_OPTIONAL, .element = expected.element, .ok = true};
    } else if (expected.kind == TYPE_CSTR) {
      result = (exprty_t){.kind = TYPE_CSTR, .ok = true};
    } else {
      diag_error(sema->src, expr->line, expr->col,
                 "cannot infer a type for 'null'");
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
    }
    break;
  case EXPR_TERNARY: {
    exprty_t cond =
        check_expr(sema, expr->ternary.cond, type_from_kind(TYPE_BOOL));
    if (cond.ok && cond.kind != TYPE_BOOL) {
      diag_error(sema->src, expr->ternary.cond->line, expr->ternary.cond->col,
                 "ternary condition must be bool, got %s",
                 type_kind_to_str(cond.kind));
      sema->had_error = true;
    }
    exprty_t then = check_expr(sema, expr->ternary.then, expected);
    type_t hint = then.ok ? exprty_type(then) : expected;
    exprty_t els = check_expr(sema, expr->ternary.els, hint);
    bool ok = cond.ok && cond.kind == TYPE_BOOL && then.ok && els.ok;
    if (then.ok && els.ok && !assignable(exprty_type(then), els)) {
      diag_error(sema->src, expr->ternary.els->line, expr->ternary.els->col,
                 "ternary branches must have the same type, got %s and %s",
                 type_to_str(exprty_type(then)), type_to_str(exprty_type(els)));
      sema->had_error = true;
      ok = false;
    }
    result = (exprty_t){.kind = then.kind,
                        .name = then.name,
                        .element = then.element,
                        .array_length = then.array_length,
                        .ok = ok};
    break;
  }
  case EXPR_COALESCE: {
    exprty_t lhs =
        check_expr(sema, expr->coalesce.lhs, type_from_kind(TYPE_UNKNOWN));
    if (!lhs.ok) {
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    if (lhs.kind != TYPE_OPTIONAL || lhs.element == NULL) {
      diag_error(sema->src, expr->coalesce.lhs->line, expr->coalesce.lhs->col,
                 "left side of '\?\?' must be an optional, got %s",
                 type_to_str(exprty_type(lhs)));
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    exprty_t rhs = check_expr(sema, expr->coalesce.rhs, *lhs.element);
    bool ok = rhs.ok;
    if (rhs.ok && !assignable(*lhs.element, rhs)) {
      diag_error(sema->src, expr->coalesce.rhs->line, expr->coalesce.rhs->col,
                 "'\?\?' fallback has type %s, expected %s",
                 type_to_str(exprty_type(rhs)), type_to_str(*lhs.element));
      sema->had_error = true;
      ok = false;
    }
    result = (exprty_t){.kind = lhs.element->kind,
                        .name = lhs.element->name,
                        .element = lhs.element->element,
                        .array_length = lhs.element->array_length,
                        .ok = ok};
    break;
  }
  case EXPR_UNWRAP: {
    exprty_t operand =
        check_expr(sema, expr->unwrap.operand, type_from_kind(TYPE_UNKNOWN));
    if (!operand.ok) {
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    if (operand.kind != TYPE_OPTIONAL || operand.element == NULL) {
      diag_error(sema->src, expr->unwrap.operand->line,
                 expr->unwrap.operand->col,
                 "operand of '!' must be an optional, got %s",
                 type_to_str(exprty_type(operand)));
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    result = (exprty_t){.kind = operand.element->kind,
                        .name = operand.element->name,
                        .element = operand.element->element,
                        .array_length = operand.element->array_length,
                        .ok = true};
    break;
  }
  case EXPR_PROPAGATE: {
    exprty_t operand =
        check_expr(sema, expr->propagate.operand, type_from_kind(TYPE_UNKNOWN));
    if (!operand.ok) {
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    if (operand.kind != TYPE_OPTIONAL || operand.element == NULL) {
      diag_error(sema->src, expr->propagate.operand->line,
                 expr->propagate.operand->col,
                 "operand of '?' must be an optional, got %s",
                 type_to_str(exprty_type(operand)));
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    if (sema->current_fn == NULL ||
        sema->current_fn->fn.return_type.kind != TYPE_OPTIONAL) {
      diag_error(sema->src, expr->line, expr->col,
                 "'?' can only be used in a function returning an optional");
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    result = (exprty_t){.kind = operand.element->kind,
                        .name = operand.element->name,
                        .element = operand.element->element,
                        .array_length = operand.element->array_length,
                        .ok = true};
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
    resolve_qualified_type(sema, &expr->cast.target);
    resolve_enum_type(sema, &expr->cast.target);
    TypeKind target = expr->cast.target.kind;
    bool bytes_to_str = operand.kind == TYPE_SLICE && operand.element != NULL &&
                        operand.element->kind == TYPE_U8 && target == TYPE_STR;
    bool str_to_bytes = operand.kind == TYPE_STR && target == TYPE_SLICE &&
                        expr->cast.target.element != NULL &&
                        expr->cast.target.element->kind == TYPE_U8;
    bool valid = (type_is_numeric(operand.kind) && type_is_numeric(target)) ||
                 (operand.kind == TYPE_ENUM && type_is_integer(target)) ||
                 (type_is_integer(operand.kind) && target == TYPE_ENUM) ||
                 bytes_to_str || str_to_bytes;
    if (operand.ok && !valid) {
      diag_error(sema->src, expr->line, expr->col, "cannot cast %s to %s",
                 type_to_str(exprty_type(operand)),
                 type_to_str(expr->cast.target));
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
    } else {
      result = (exprty_t){.kind = target,
                          .name = expr->cast.target.name,
                          .element = expr->cast.target.element,
                          .ok = operand.ok};
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
                   expr->binary.lhs->kind == EXPR_ENUM_LITERAL ||
                   expr->binary.lhs->kind == EXPR_NULL;
    bool rhs_lit = expr->binary.rhs->kind == EXPR_NUMBER ||
                   expr->binary.rhs->kind == EXPR_ENUM_LITERAL ||
                   expr->binary.rhs->kind == EXPR_NULL;

    exprty_t lhs, rhs;
    if (rhs_lit && !lhs_lit) {
      lhs = check_expr(sema, expr->binary.lhs, expected);
      type_t hint = type_is_numeric(lhs.kind) || lhs.kind == TYPE_ENUM ||
                            lhs.kind == TYPE_OPTIONAL || lhs.kind == TYPE_CSTR
                        ? exprty_type(lhs)
                        : expected;
      rhs = check_expr(sema, expr->binary.rhs, hint);
    } else if (lhs_lit && !rhs_lit) {
      rhs = check_expr(sema, expr->binary.rhs, expected);
      type_t hint = type_is_numeric(rhs.kind) || rhs.kind == TYPE_ENUM ||
                            rhs.kind == TYPE_OPTIONAL || rhs.kind == TYPE_CSTR
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
               enum_name_eq(lhs.name, rhs.name) &&
               (expr->binary.op == BINOP_EQ || expr->binary.op == BINOP_NE)) {
      if (sema_enum_is_tagged(sema, lhs.name)) {
        diag_error(sema->src, expr->line, expr->col,
                   "cannot compare tagged union '%s' with '==' or '!='; "
                   "use match",
                   bare_type_name(lhs.name));
        sema->had_error = true;
        result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      } else {
        result = (exprty_t){.kind = TYPE_BOOL, .ok = true};
      }
    } else if ((expr->binary.lhs->kind == EXPR_NULL ||
                expr->binary.rhs->kind == EXPR_NULL) &&
               lhs.kind == rhs.kind &&
               (lhs.kind == TYPE_OPTIONAL || lhs.kind == TYPE_CSTR) &&
               (expr->binary.op == BINOP_EQ || expr->binary.op == BINOP_NE)) {
      result = (exprty_t){.kind = TYPE_BOOL, .ok = true};
    } else if ((lhs.kind == TYPE_OPTIONAL) != (rhs.kind == TYPE_OPTIONAL) &&
               (expr->binary.op == BINOP_EQ || expr->binary.op == BINOP_NE) &&
               (lhs.kind == TYPE_OPTIONAL ? lhs : rhs).element != NULL &&
               (type_is_numeric((lhs.kind == TYPE_OPTIONAL ? rhs : lhs).kind) ||
                (lhs.kind == TYPE_OPTIONAL ? rhs : lhs).kind == TYPE_ENUM ||
                (lhs.kind == TYPE_OPTIONAL ? rhs : lhs).kind == TYPE_BOOL) &&
               types_equal(
                   *(lhs.kind == TYPE_OPTIONAL ? lhs : rhs).element,
                   exprty_type(lhs.kind == TYPE_OPTIONAL ? rhs : lhs))) {
      result = (exprty_t){.kind = TYPE_BOOL, .ok = true};
    } else if (!type_is_numeric(lhs.kind) || lhs.kind != rhs.kind ||
               (type_is_float(lhs.kind) &&
                !binop_is_arithmetic(expr->binary.op) &&
                !binop_is_comparison(expr->binary.op))) {
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
    if (expr->struct_literal.type_name == NULL) {
      type_t want = expected;
      if (want.kind == TYPE_OPTIONAL && want.element != NULL) {
        want = *want.element;
      }
      if (want.kind == TYPE_STRUCT && want.name != NULL) {
        expr->struct_literal.type_name = want.name;
      } else {
        diag_error(sema->src, expr->line, expr->col,
                   "cannot infer struct type for literal");
        sema->had_error = true;
        result = (exprty_t){.kind = TYPE_VOID, .ok = false};
        break;
      }
    }
    if (expr->struct_literal.module != NULL) {
      module_t *mod =
          resolve_module_alias_path(sema, expr->struct_literal.module);
      if (mod == NULL) {
        diag_error(sema->src, expr->line, expr->col, "'%s' is not a module",
                   expr->struct_literal.module);
        sema->had_error = true;
        result = (exprty_t){.kind = TYPE_VOID, .ok = false};
        break;
      }
      if (module_pub_struct(mod, expr->struct_literal.type_name) == NULL) {
        diag_error(sema->src, expr->line, expr->col,
                   "module '%s' has no public struct '%s'",
                   expr->struct_literal.module, expr->struct_literal.type_name);
        sema->had_error = true;
        result = (exprty_t){.kind = TYPE_VOID, .ok = false};
        break;
      }
      expr->struct_literal.type_name = arena_format(
          sema->arena, "%s.%s", module_stem(mod->path, sema->arena),
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
      exprty_t value =
          check_expr(sema, init->value,
                     field ? field->type : type_from_kind(TYPE_UNKNOWN));
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
        if (enm->enm.tagged) {
          expr->kind = EXPR_ENUM_LITERAL;
          expr->enum_literal.name = member->name;
          expr->enum_literal.payload = NULL;
          result = check_expr(sema, expr,
                              (type_t){.kind = TYPE_ENUM, .name = enm->name});
          break;
        }
        expr->kind = EXPR_NUMBER;
        expr->number.value = arena_format(sema->arena, "%lld", member->value);
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
    while (want.kind == TYPE_OPTIONAL && want.element != NULL) {
      want = *want.element;
    }
    resolve_enum_type(sema, &want);
    decl_t *enm =
        want.name != NULL ? typetab_lookup(sema->types, want.name) : NULL;
    if (enm == NULL && want.kind == TYPE_ENUM && want.name != NULL) {
      enm = imports_pub_enum(sema, want.name);
    }
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
    if (member->has_payload) {
      if (expr->enum_literal.payload == NULL) {
        diag_error(sema->src, expr->line, expr->col,
                   "variant '%s' of '%s' requires a payload of type %s",
                   member->name, bare_type_name(want.name),
                   type_to_str(member->payload));
        sema->had_error = true;
        result = (exprty_t){.kind = TYPE_VOID, .ok = false};
        break;
      }
      exprty_t pl =
          check_expr(sema, expr->enum_literal.payload, member->payload);
      if (pl.ok && !assignable(member->payload, pl)) {
        diag_error(sema->src, expr->enum_literal.payload->line,
                   expr->enum_literal.payload->col,
                   "cannot pass %s as payload of variant '%s' (expected %s)",
                   type_to_str(exprty_type(pl)), member->name,
                   type_to_str(member->payload));
        sema->had_error = true;
      }
    } else if (expr->enum_literal.payload != NULL) {
      diag_error(sema->src, expr->enum_literal.payload->line,
                 expr->enum_literal.payload->col,
                 "variant '%s' of '%s' takes no payload", member->name,
                 bare_type_name(want.name));
      sema->had_error = true;
    }
    if (!enm->enm.tagged) {
      expr->kind = EXPR_NUMBER;
      expr->number.value = arena_format(sema->arena, "%lld", member->value);
    }
    result = (exprty_t){.kind = TYPE_ENUM, .name = want.name, .ok = true};
    break;
  }
  case EXPR_SLICE_RANGE: {
    exprty_t base =
        check_expr(sema, expr->slice_range.base, type_from_kind(TYPE_UNKNOWN));
    exprty_t start =
        check_expr(sema, expr->slice_range.start, type_from_kind(TYPE_I64));
    exprty_t end =
        check_expr(sema, expr->slice_range.end, type_from_kind(TYPE_I64));
    if (!base.ok) {
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    if (base.kind != TYPE_ARRAY && base.kind != TYPE_SLICE &&
        base.kind != TYPE_STR && base.kind != TYPE_CSTR) {
      diag_error(sema->src, expr->line, expr->col, "cannot slice %s",
                 type_to_str(exprty_type(base)));
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    if (start.ok && !type_is_integer(start.kind)) {
      diag_error(sema->src, expr->slice_range.start->line,
                 expr->slice_range.start->col,
                 "slice bound must be an integer, got %s",
                 type_kind_to_str(start.kind));
      sema->had_error = true;
    }
    if (end.ok && !type_is_integer(end.kind)) {
      diag_error(
          sema->src, expr->slice_range.end->line, expr->slice_range.end->col,
          "slice bound must be an integer, got %s", type_kind_to_str(end.kind));
      sema->had_error = true;
    }
    if (base.kind == TYPE_STR) {
      result = (exprty_t){.kind = TYPE_STR, .ok = start.ok && end.ok};
    } else if (base.kind == TYPE_CSTR) {
      type_t *elem = arena_alloc(sema->arena, sizeof(type_t));
      *elem = (type_t){.kind = TYPE_U8};
      result = (exprty_t){
          .kind = TYPE_SLICE, .element = elem, .ok = start.ok && end.ok};
    } else {
      result = (exprty_t){.kind = TYPE_SLICE,
                          .element = base.element,
                          .ok = start.ok && end.ok};
    }
    break;
  }
  case EXPR_MATCH: {
    exprty_t scrut = check_expr(sema, expr->match_expr.scrutinee,
                                type_from_kind(TYPE_UNKNOWN));
    if (!scrut.ok) {
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    bool opt_scrut = scrut.kind == TYPE_OPTIONAL && scrut.element != NULL;
    exprty_t inner = scrut;
    if (opt_scrut) {
      inner = (exprty_t){.kind = scrut.element->kind,
                         .name = scrut.element->name,
                         .element = scrut.element->element,
                         .array_length = scrut.element->array_length,
                         .ok = true};
    }
    decl_t *enm = NULL;
    if (inner.kind == TYPE_ENUM && inner.name != NULL) {
      enm = typetab_lookup(sema->types, inner.name);
      if (enm == NULL) {
        enm = imports_pub_enum(sema, inner.name);
      }
      if (enm == NULL || enm->kind != DECL_ENUM) {
        diag_error(sema->src, expr->match_expr.scrutinee->line,
                   expr->match_expr.scrutinee->col, "unknown enum '%s'",
                   inner.name);
        sema->had_error = true;
        result = (exprty_t){.kind = TYPE_VOID, .ok = false};
        break;
      }
    } else if (inner.kind != TYPE_BOOL && !type_is_integer(inner.kind)) {
      diag_error(sema->src, expr->match_expr.scrutinee->line,
                 expr->match_expr.scrutinee->col,
                 "match requires an enum, bool, or integer value, got %s",
                 type_to_str(exprty_type(scrut)));
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }
    bool int_scrut = type_is_integer(inner.kind);
    bool tagged = enm != NULL && enm->enm.tagged;
    if (expr->match_expr.arms == NULL) {
      diag_error(sema->src, expr->line, expr->col,
                 "match must have at least one arm");
      sema->had_error = true;
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
      break;
    }

    size_t member_count = enm != NULL ? enm->enm.member_count : 2;
    bool *covered = arena_alloc(
        sema->arena, sizeof(bool) * (member_count ? member_count : 1));
    long long *seen = arena_alloc(sema->arena, sizeof(long long) *
                                                   expr->match_expr.arm_count);
    size_t seen_count = 0;
    bool seen_null = false;
    bool has_wildcard = false;
    bool has_condition = false;
    bool has_block = false;
    bool have_rtype = false;
    bool ok = true;
    exprty_t rtype = {.kind = TYPE_UNKNOWN};

    for (match_arm_t *arm = expr->match_expr.arms; arm != NULL;
         arm = arm->next) {
      char *bind_name = NULL;
      type_t bind_type = {.kind = TYPE_UNKNOWN};
      if (arm->pattern == NULL) {
        has_wildcard = true;
        if (arm->next != NULL) {
          diag_error(sema->src, arm->line, arm->col,
                     "'_' must be the last match arm");
          sema->had_error = true;
          ok = false;
        }
      } else if (tagged) {
        if (arm->pattern->kind != EXPR_ENUM_LITERAL) {
          diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                     "match pattern must be a variant of enum '%s'",
                     bare_type_name(inner.name));
          sema->had_error = true;
          ok = false;
        } else {
          const char *vname = arm->pattern->enum_literal.name;
          enum_member_t *member = NULL;
          size_t index = 0;
          size_t i = 0;
          for (enum_member_t *m = enm->enm.members; m != NULL;
               m = m->next, i++) {
            if (strcmp(m->name, vname) == 0) {
              member = m;
              index = i;
              break;
            }
          }
          if (member == NULL) {
            diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                       "enum '%s' has no member '%s'",
                       bare_type_name(inner.name), vname);
            sema->had_error = true;
            ok = false;
          } else {
            if (covered[index]) {
              diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                         "duplicate match arm for '%s'", vname);
              sema->had_error = true;
              ok = false;
            }
            covered[index] = true;
            if (arm->or_next && arm->binding != NULL) {
              diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                         "grouped variant patterns cannot bind a payload");
              sema->had_error = true;
              ok = false;
            } else if (member->has_payload) {
              if (arm->binding == NULL) {
                diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                           "variant '%s' binds its payload: write .%s(name)",
                           vname, vname);
                sema->had_error = true;
                ok = false;
              } else {
                bind_name = arm->binding;
                bind_type = member->payload;

                if (inner.name != NULL) {
                  const char *dot = strrchr(inner.name, '.');
                  if (dot != NULL) {
                    const char *stem =
                        arena_format(sema->arena, "%.*s",
                                     (int)(dot - inner.name), inner.name);
                    bind_type = qualify_param_type(sema, bind_type, stem);
                  }
                }
              }
            } else if (arm->binding != NULL) {
              diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                         "variant '%s' has no payload to bind", vname);
              sema->had_error = true;
              ok = false;
            }
          }
        }
      } else if (arm->pattern->kind == EXPR_NULL) {
        if (!opt_scrut) {
          diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                     "a 'null' pattern requires an optional scrutinee");
          sema->had_error = true;
          ok = false;
        } else {
          check_expr(sema, arm->pattern, exprty_type(scrut));
          if (seen_null) {
            diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                       "duplicate match arm for 'null'");
            sema->had_error = true;
            ok = false;
          }
          seen_null = true;
        }
      } else {
        exprty_t pat = check_expr(sema, arm->pattern, exprty_type(inner));
        if (arm->pattern_end != NULL && !int_scrut) {
          diag_error(sema->src, arm->pattern_end->line, arm->pattern_end->col,
                     "range patterns require an integer scrutinee");
          sema->had_error = true;
          ok = false;
        } else if (!pat.ok) {
          ok = false;
        } else if (int_scrut) {
          long long lo = 0;
          long long hi = 0;
          if (!pattern_int_value(arm->pattern, &lo)) {
            diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                       "match pattern must be an integer constant");
            sema->had_error = true;
            ok = false;
          } else if (arm->pattern_end != NULL) {
            exprty_t pe =
                check_expr(sema, arm->pattern_end, exprty_type(inner));
            if (!pe.ok) {
              ok = false;
            } else if (!pattern_int_value(arm->pattern_end, &hi)) {
              diag_error(sema->src, arm->pattern_end->line,
                         arm->pattern_end->col,
                         "match pattern must be an integer constant");
              sema->had_error = true;
              ok = false;
            } else if (lo > hi) {
              diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                         "range pattern is empty");
              sema->had_error = true;
              ok = false;
            }
          } else {
            long long value = lo;
            for (size_t i = 0; i < seen_count; i++) {
              if (seen[i] == value) {
                diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                           "duplicate match arm for %lld", value);
                sema->had_error = true;
                ok = false;
                break;
              }
            }
            seen[seen_count++] = value;
          }
        } else if (enm == NULL) {
          if (arm->pattern->kind == EXPR_BOOLEAN) {
            size_t index = arm->pattern->boolean.value ? 1 : 0;
            if (covered[index]) {
              diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                         "duplicate match arm for '%s'",
                         arm->pattern->boolean.value ? "true" : "false");
              sema->had_error = true;
              ok = false;
            }
            covered[index] = true;
          } else if (pat.kind != TYPE_BOOL) {
            diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                       "match pattern must be a bool expression, got %s",
                       type_to_str(exprty_type(pat)));
            sema->had_error = true;
            ok = false;
          } else {
            has_condition = true;
          }
        } else if (pat.kind != TYPE_ENUM || pat.name == NULL ||
                   strcmp(pat.name, inner.name) != 0 ||
                   arm->pattern->kind != EXPR_NUMBER) {
          diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                     "match pattern must be a member of enum '%s'",
                     bare_type_name(inner.name));
          sema->had_error = true;
          ok = false;
        } else {
          long long value = strtoll(arm->pattern->number.value, NULL, 10);
          size_t index = 0;
          for (enum_member_t *m = enm->enm.members; m != NULL;
               m = m->next, index++) {
            if (m->value == value) {
              if (covered[index]) {
                diag_error(sema->src, arm->pattern->line, arm->pattern->col,
                           "duplicate match arm for '%s'", m->name);
                sema->had_error = true;
                ok = false;
              }
              covered[index] = true;
              break;
            }
          }
        }
      }

      if (arm->or_next) {
        continue;
      }

      size_t bind_saved = sema->scope != NULL ? sema->scope->count : 0;
      if (bind_name != NULL && sema->scope != NULL) {
        sema->scope->items[sema->scope->count++] = (local_t){
            .name = bind_name,
            .type = bind_type,
            .mutable = false,
        };
      }

      if (arm->is_block) {
        has_block = true;
        if (sema->current_fn == NULL) {
          diag_error(sema->src, arm->line, arm->col,
                     "a match arm block is only allowed inside a function");
          sema->had_error = true;
          ok = false;
        } else {
          for (stmt_t *s = arm->body; s != NULL; s = s->next) {
            check_stmt(sema, s, sema->current_fn);
          }
        }
        if (sema->scope != NULL) {
          sema->scope->count = bind_saved;
        }
        continue;
      }

      type_t hint = expected;
      if (expected.kind == TYPE_UNKNOWN && have_rtype) {
        hint = exprty_type(rtype);
      }
      if (arm->value->kind == EXPR_NULL && hint.kind != TYPE_OPTIONAL &&
          hint.kind != TYPE_CSTR && have_rtype && rtype.kind != TYPE_OPTIONAL &&
          rtype.kind != TYPE_VOID) {
        type_t *element = arena_alloc(sema->arena, sizeof(type_t));
        *element = exprty_type(rtype);
        hint = (type_t){.kind = TYPE_OPTIONAL, .element = element};
      }
      exprty_t value = check_expr(sema, arm->value, hint);
      if (sema->scope != NULL) {
        sema->scope->count = bind_saved;
      }
      if (!value.ok) {
        ok = false;
      } else if (!have_rtype) {
        rtype = value;
        have_rtype = true;
      } else if (!assignable(exprty_type(rtype), value)) {
        if (value.kind == TYPE_OPTIONAL && rtype.kind != TYPE_OPTIONAL &&
            value.element != NULL &&
            types_equal(*value.element, exprty_type(rtype))) {
          rtype = value;
        } else {
          diag_error(sema->src, arm->value->line, arm->value->col,
                     "match arm has type %s, expected %s",
                     type_to_str(exprty_type(value)),
                     type_to_str(exprty_type(rtype)));
          sema->had_error = true;
          ok = false;
        }
      }
    }

    if (!has_wildcard) {
      if (opt_scrut || int_scrut) {
        diag_error(sema->src, expr->line, expr->col,
                   "match on %s must have a '_' arm",
                   type_to_str(exprty_type(scrut)));
        sema->had_error = true;
        ok = false;
      } else if (enm == NULL) {
        if (has_condition) {
          if (!(covered[0] && covered[1])) {
            diag_error(sema->src, expr->line, expr->col,
                       "match with condition arms must end with a '_' arm");
            sema->had_error = true;
            ok = false;
          }
        } else {
          if (!covered[1]) {
            diag_error(sema->src, expr->line, expr->col,
                       "match does not cover 'true'");
            sema->had_error = true;
            ok = false;
          }
          if (!covered[0]) {
            diag_error(sema->src, expr->line, expr->col,
                       "match does not cover 'false'");
            sema->had_error = true;
            ok = false;
          }
        }
      } else {
        size_t index = 0;
        for (enum_member_t *m = enm->enm.members; m != NULL;
             m = m->next, index++) {
          if (!covered[index]) {
            diag_error(sema->src, expr->line, expr->col,
                       "match does not cover enum member '%s'", m->name);
            sema->had_error = true;
            ok = false;
          }
        }
      }
    }

    if (has_block) {
      result = (exprty_t){.kind = TYPE_VOID, .ok = ok};
    } else if (!have_rtype) {
      result = (exprty_t){.kind = TYPE_VOID, .ok = false};
    } else {
      result = (exprty_t){.kind = rtype.kind,
                          .name = rtype.name,
                          .element = rtype.element,
                          .array_length = rtype.array_length,
                          .ok = ok};
    }
    break;
  }
  case EXPR_ARRAY: {
    type_t elem_hint =
        (expected.kind == TYPE_ARRAY || expected.kind == TYPE_SLICE) &&
                expected.element != NULL
            ? *expected.element
            : type_from_kind(TYPE_UNKNOWN);
    type_t elem = {.kind = TYPE_UNKNOWN};
    size_t count = 0;
    bool ok = true;
    for (expr_t *e = expr->array.elements; e != NULL; e = e->next) {
      exprty_t et = check_expr(sema, e, elem_hint);
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
    if (stmt->binding.init == NULL && stmt->binding.type.kind == TYPE_UNKNOWN) {
      diag_error(sema->src, stmt->line, stmt->col,
                 "binding '%s' without an initializer needs a type",
                 stmt->binding.name);
      sema->had_error = true;
    }
    if (stmt->binding.init != NULL) {
      exprty_t init = check_expr(sema, stmt->binding.init, stmt->binding.type);
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

    if (stmt->assign.coalesce) {
      if (lhs.ok && (lhs.kind != TYPE_OPTIONAL || lhs.element == NULL)) {
        diag_error(sema->src, stmt->line, stmt->col,
                   "left side of '\?\?=' must be an optional, got %s",
                   type_to_str(exprty_type(lhs)));
        sema->had_error = true;
        check_expr(sema, stmt->assign.value, type_from_kind(TYPE_UNKNOWN));
        break;
      }
      type_t elem = lhs.ok ? *lhs.element : type_from_kind(TYPE_UNKNOWN);
      exprty_t fallback = check_expr(sema, stmt->assign.value, elem);
      if (lhs.ok && fallback.ok && !assignable(elem, fallback)) {
        diag_error(sema->src, stmt->assign.value->line, stmt->assign.value->col,
                   "'\?\?=' fallback has type %s, expected %s",
                   type_to_str(exprty_type(fallback)), type_to_str(elem));
        sema->had_error = true;
      }
      break;
    }
    exprty_t value = check_expr(sema, stmt->assign.value, exprty_type(lhs));
    if (stmt->assign.compound) {
      bool bitwise = !binop_is_arithmetic(stmt->assign.op);
      bool valid =
          lhs.kind == value.kind &&
          (bitwise ? type_is_integer(lhs.kind) : type_is_numeric(lhs.kind));
      if (lhs.ok && value.ok && !valid) {
        diag_error(sema->src, stmt->line, stmt->col,
                   "cannot apply '%s=' to %s and %s",
                   binop_to_str(stmt->assign.op), type_to_str(exprty_type(lhs)),
                   type_to_str(exprty_type(value)));
        sema->had_error = true;
      }
      break;
    }
    if (lhs.ok && value.ok) {
      type_t to = exprty_type(lhs);
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
      exprty_t iter = check_expr(sema, stmt->for_loop.iterable,
                                 type_from_kind(TYPE_UNKNOWN));
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
  case STMT_DEFER: {
    if (stmt->defer_stmt.expr->kind != EXPR_CALL) {
      diag_error(sema->src, stmt->defer_stmt.expr->line,
                 stmt->defer_stmt.expr->col,
                 "defer expects a function or method call");
      sema->had_error = true;
      break;
    }
    check_expr(sema, stmt->defer_stmt.expr, type_from_kind(TYPE_UNKNOWN));
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

static size_t count_bindings(stmt_t *body);

static size_t count_expr_bindings(const expr_t *e) {
  if (e == NULL) {
    return 0;
  }
  size_t n = 0;
  switch (e->kind) {
  case EXPR_MATCH:
    n += count_expr_bindings(e->match_expr.scrutinee);
    for (const match_arm_t *arm = e->match_expr.arms; arm != NULL;
         arm = arm->next) {
      if (arm->binding != NULL) {
        n++;
      }
      n += count_bindings(arm->body);
      n += count_expr_bindings(arm->value);
    }
    break;
  case EXPR_CALL:
    n += count_expr_bindings(e->call.callee);
    for (const expr_t *a = e->call.args; a != NULL; a = a->next) {
      n += count_expr_bindings(a);
    }
    break;
  case EXPR_CAST:
    n += count_expr_bindings(e->cast.operand);
    break;
  case EXPR_BINARY:
    n += count_expr_bindings(e->binary.lhs);
    n += count_expr_bindings(e->binary.rhs);
    break;
  case EXPR_UNARY:
    n += count_expr_bindings(e->unary.operand);
    break;
  case EXPR_FIELD:
    n += count_expr_bindings(e->field.base);
    break;
  case EXPR_ENUM_LITERAL:
    n += count_expr_bindings(e->enum_literal.payload);
    break;
  case EXPR_INDEX:
    n += count_expr_bindings(e->index.base);
    n += count_expr_bindings(e->index.index);
    break;
  case EXPR_SLICE_RANGE:
    n += count_expr_bindings(e->slice_range.base);
    n += count_expr_bindings(e->slice_range.start);
    n += count_expr_bindings(e->slice_range.end);
    break;
  case EXPR_COALESCE:
    n += count_expr_bindings(e->coalesce.lhs);
    n += count_expr_bindings(e->coalesce.rhs);
    break;
  case EXPR_TERNARY:
    n += count_expr_bindings(e->ternary.cond);
    n += count_expr_bindings(e->ternary.then);
    n += count_expr_bindings(e->ternary.els);
    break;
  case EXPR_ARRAY:
    for (const expr_t *el = e->array.elements; el != NULL; el = el->next) {
      n += count_expr_bindings(el);
    }
    break;
  case EXPR_STRUCT_LITERAL:
    for (const field_init_t *fi = e->struct_literal.inits; fi != NULL;
         fi = fi->next) {
      n += count_expr_bindings(fi->value);
    }
    break;
  default:
    break;
  }
  return n;
}

static size_t count_bindings(stmt_t *body) {
  size_t n = 0;
  for (stmt_t *s = body; s != NULL; s = s->next) {
    switch (s->kind) {
    case STMT_BINDING:
      n += 1 + count_expr_bindings(s->binding.init);
      break;
    case STMT_IF:
      n += count_expr_bindings(s->if_stmt.cond);
      n += count_bindings(s->if_stmt.then_body);
      n += count_bindings(s->if_stmt.else_body);
      break;
    case STMT_WHILE:
      n += count_expr_bindings(s->while_loop.cond);
      n += count_bindings(s->while_loop.body);
      break;
    case STMT_FOR:
      n += 1 + count_bindings(s->for_loop.body);
      break;
    case STMT_RETURN:
      n += count_expr_bindings(s->ret.value);
      break;
    case STMT_EXPR:
      n += count_expr_bindings(s->expr_stmt.expr);
      break;
    case STMT_ASSIGN:
      n += count_expr_bindings(s->assign.value);
      break;
    case STMT_DEFER:
      n += count_expr_bindings(s->defer_stmt.expr);
      break;
    default:
      break;
    }
  }
  return n;
}

static void resolve_qualified_type(sema_t *sema, type_t *t) {
  if (t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE ||
      t->kind == TYPE_OPTIONAL) {
    resolve_qualified_type(sema, t->element);
    return;
  }
  if (t->kind != TYPE_STRUCT || t->module == NULL) {
    return;
  }
  module_t *mod = resolve_module_alias_path(sema, t->module);
  if (mod == NULL) {
    diag_error(sema->src, t->line, t->col, "'%s' is not a module", t->module);
    sema->had_error = true;
    t->kind = TYPE_UNKNOWN;
    t->module = NULL;
    return;
  }
  if (module_pub_struct(mod, t->name) != NULL) {
    t->name = arena_format(sema->arena, "%s.%s",
                           module_stem(mod->path, sema->arena), t->name);
    t->module = NULL;
    return;
  }
  if (module_pub_enum(mod, t->name) != NULL) {
    t->kind = TYPE_ENUM;
    t->name = arena_format(sema->arena, "%s.%s",
                           module_stem(mod->path, sema->arena), t->name);
    t->module = NULL;
    return;
  }
  diag_error(sema->src, t->line, t->col,
             "module '%s' has no public struct '%s'", t->module, t->name);
  sema->had_error = true;
  t->kind = TYPE_UNKNOWN;
  t->module = NULL;
}

static void resolve_enum_type(sema_t *sema, type_t *t) {
  if (t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE ||
      t->kind == TYPE_OPTIONAL) {
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
  if (type->kind == TYPE_ARRAY || type->kind == TYPE_SLICE ||
      type->kind == TYPE_OPTIONAL) {
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

    if (fn->fn.variadic && !fn->fn.is_extern && param->next == NULL &&
        param->type.kind == TYPE_SLICE &&
        param->type.element->kind == TYPE_STRUCT &&
        typetab_lookup(sema->types, param->type.element->name) == NULL) {
      for (const modnode_t *node = sema->reachable; node != NULL;
           node = node->next) {
        if (module_pub_struct(node->mod, param->type.element->name) != NULL) {
          param->type.element->name = arena_format(
              sema->arena, "%s.%s", module_stem(node->mod->path, sema->arena),
              param->type.element->name);
          break;
        }
      }
    }
  }
  resolve_qualified_type(sema, &fn->fn.return_type);
  resolve_enum_type(sema, &fn->fn.return_type);
}

static void check_fn(sema_t *sema, decl_t *fn);

static void check_struct(sema_t *sema, const decl_t *strct) {
  for (field_t *field = strct->strct.fields; field != NULL;
       field = field->next) {
    resolve_qualified_type(sema, &field->type);
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

static bool payload_stores_enum(const char *ename, const type_t *t) {
  if (t == NULL) {
    return false;
  }
  if (t->kind == TYPE_ENUM) {
    return enum_name_eq(ename, t->name);
  }
  if (t->kind == TYPE_ARRAY || t->kind == TYPE_OPTIONAL) {
    return payload_stores_enum(ename, t->element);
  }
  return false;
}

static void check_enum(sema_t *sema, const decl_t *enm) {
  for (enum_member_t *member = enm->enm.members; member != NULL;
       member = member->next) {
    if (member->has_payload) {
      resolve_qualified_type(sema, &member->payload);
      resolve_enum_type(sema, &member->payload);
      check_type(sema, &member->payload);
      if (payload_stores_enum(enm->name, &member->payload)) {
        diag_error(sema->src, member->line, member->col,
                   "variant '%s' stores '%s' by value, making it infinitely "
                   "sized; use a slice or pointer",
                   member->name, enm->name);
        sema->had_error = true;
      }
    }
  }
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
      if (prev->value == member->value) {
        diag_error(
            sema->src, member->line, member->col,
            "duplicate enum value %lld for member '%s' (already used by '%s')",
            member->value, member->name, prev->name);
        sema->had_error = true;
        break;
      }
    }
  }
  for (decl_t *method = enm->enm.methods; method != NULL;
       method = method->next) {
    check_fn(sema, method);
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
      if (!is_const_init(param->default_value) &&
          param->default_value->kind != EXPR_STRUCT_LITERAL) {
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
        if (sema->module_stem != NULL &&
            param->default_value->kind == EXPR_STRUCT_LITERAL &&
            param->default_value->struct_literal.type_name != NULL &&
            strchr(param->default_value->struct_literal.type_name, '.') ==
                NULL) {
          char *qualified =
              arena_format(sema->arena, "%s.%s", sema->module_stem,
                           param->default_value->struct_literal.type_name);
          param->default_value->struct_literal.type_name = qualified;
          param->default_value->type.name = qualified;
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
  sema->current_fn = fn;

  for (stmt_t *stmt = fn->fn.body; stmt != NULL; stmt = stmt->next) {
    check_stmt(sema, stmt, fn);
  }

  if (fn->fn.return_type.kind != TYPE_VOID && !block_returns(fn->fn.body)) {
    diag_error(sema->src, fn->line, fn->col,
               "non-void function '%s' must return a value", fn->name);
    sema->had_error = true;
  }

  sema->scope = NULL;
  sema->current_fn = NULL;
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
  modnode_t *reachable = NULL;
  for (decl_t *m = root->container.members; m != NULL; m = m->next) {
    if (m->kind == DECL_IMPORT && m->import.resolved != NULL) {
      reachable = collect_pub_modules(reachable, m->import.resolved, arena);
    }
  }
  size_t type_capacity = capacity;
  for (modnode_t *n = reachable; n != NULL; n = n->next) {
    type_capacity += n->mod->unit->root->container.member_count;
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
      .reachable = reachable,
      .tab = &tab,
      .types = &types,
      .src = unit->src,
      .globals = &globals,
      .scope = NULL,
      .module_stem = require_main ? NULL : module_stem(unit->src->path, arena),
      .arena = arena,
      .had_error = false,
  };

  size_t local_type_count = types.count;
  for (modnode_t *n = reachable; n != NULL; n = n->next) {
    const char *stem = module_stem(n->mod->path, arena);
    for (decl_t *m = n->mod->unit->root->container.members; m != NULL;
         m = m->next) {
      if ((m->kind == DECL_STRUCT || m->kind == DECL_ENUM) &&
          m->visibility == VISIBILITY_PUBLIC) {
        types.structs[types.count++] = (typeent_t){
            .name = arena_format(arena, "%s.%s", stem, m->name), .decl = m};
      }
    }
  }

  for (size_t i = 0; i < tab.count; i++) {
    resolve_fn_signature(&sema, tab.fns[i]);
  }
  for (size_t i = 0; i < local_type_count; i++) {
    decl_t *owner = types.structs[i].decl;
    decl_t *members = owner->kind == DECL_STRUCT ? owner->strct.members
                      : owner->kind == DECL_ENUM ? owner->enm.methods
                                                 : NULL;
    for (decl_t *m = members; m != NULL; m = m->next) {
      resolve_fn_signature(&sema, m);
    }
  }

  check_globals(&sema, root, &globals);

  for (size_t i = 0; i < local_type_count; i++) {
    if (types.structs[i].decl->kind == DECL_ENUM) {
      check_enum(&sema, types.structs[i].decl);
    } else {
      check_struct(&sema, types.structs[i].decl);
    }
  }

  for (size_t i = 0; i < tab.count; i++) {
    check_fn(&sema, tab.fns[i]);
  }
  if (sema.had_error) {
    had_error = true;
  }

  return !had_error;
}
