#include "ast.h"
#include "arena.h"
#include "token.h"
#include "utils.h"
#include <stdio.h>

char *type_kind_to_ir(TypeKind kind) {
  switch (kind) {
  case TYPE_VOID:
    return "void";
  case TYPE_BOOL:
    return "i1";
  case TYPE_I8:
  case TYPE_U8:
    return "i8";
  case TYPE_I16:
  case TYPE_U16:
    return "i16";
  case TYPE_I32:
  case TYPE_U32:
    return "i32";
  case TYPE_I64:
  case TYPE_U64:
    return "i64";
  case TYPE_F32:
    return "float";
  case TYPE_F64:
    return "double";
  case TYPE_CSTR:
    return "ptr";
  case TYPE_STR:
  case TYPE_SLICE:
    return "{ ptr, i64 }";
  default:
    return "?";
  }
}

char *type_kind_to_str(TypeKind kind) {
  switch (kind) {
  case TYPE_VOID:
    return "void";
  case TYPE_BOOL:
    return "bool";
  case TYPE_I8:
    return "i8";
  case TYPE_U8:
    return "u8";
  case TYPE_I16:
    return "i16";
  case TYPE_U16:
    return "u16";
  case TYPE_I32:
    return "i32";
  case TYPE_U32:
    return "u32";
  case TYPE_I64:
    return "i64";
  case TYPE_U64:
    return "u64";
  case TYPE_F32:
    return "f32";
  case TYPE_F64:
    return "f64";
  case TYPE_CSTR:
    return "cstr";
  case TYPE_STR:
    return "str";
  case TYPE_SLICE:
    return "[]str";
  default:
    return "?";
  }
}

char *type_to_str(type_t type) {
  switch (type.kind) {
  case TYPE_STRUCT:
    return type.name;
  case TYPE_ARRAY:
  case TYPE_SLICE: {
    static char bufs[8][64];
    static unsigned next = 0;
    char *buf = bufs[next++ % 8];
    if (type.kind == TYPE_SLICE) {
      snprintf(buf, sizeof(bufs[0]), "[]%s", type_to_str(*type.element));
    } else {
      snprintf(buf, sizeof(bufs[0]), "[%zu]%s", type.array_length,
               type_to_str(*type.element));
    }
    return buf;
  }
  default:
    return type_kind_to_str(type.kind);
  }
}

bool type_is_integer(TypeKind kind) {
  switch (kind) {
  case TYPE_I8:
  case TYPE_U8:
  case TYPE_I16:
  case TYPE_U16:
  case TYPE_I32:
  case TYPE_U32:
  case TYPE_I64:
  case TYPE_U64:
    return true;
  default:
    return false;
  }
}

bool type_is_signed_integer(TypeKind kind) {
  switch (kind) {
  case TYPE_I8:
  case TYPE_I16:
  case TYPE_I32:
  case TYPE_I64:
    return true;
  default:
    return false;
  }
}

bool type_is_float(TypeKind kind) {
  return kind == TYPE_F32 || kind == TYPE_F64;
}

bool type_is_numeric(TypeKind kind) {
  return type_is_integer(kind) || type_is_float(kind);
}

bool binop_is_arithmetic(BinaryOp op) {
  switch (op) {
  case BINOP_ADD:
  case BINOP_SUB:
  case BINOP_MUL:
  case BINOP_DIV:
  case BINOP_REM:
    return true;
  default:
    return false;
  }
}

char *binop_to_ir_float(BinaryOp op) {
  switch (op) {
  case BINOP_ADD:
    return "fadd";
  case BINOP_SUB:
    return "fsub";
  case BINOP_MUL:
    return "fmul";
  case BINOP_DIV:
    return "fdiv";
  case BINOP_REM:
    return "frem";
  default:
    return "?";
  }
}

char *binop_to_ir(BinaryOp op, bool is_signed) {
  switch (op) {
  case BINOP_ADD:
    return "add";
  case BINOP_SUB:
    return "sub";
  case BINOP_MUL:
    return "mul";
  case BINOP_DIV:
    return is_signed ? "sdiv" : "udiv";
  case BINOP_REM:
    return is_signed ? "srem" : "urem";
  case BINOP_EQ:
    return "icmp eq";
  case BINOP_NE:
    return "icmp ne";
  case BINOP_LT:
    return is_signed ? "icmp slt" : "icmp ult";
  case BINOP_LE:
    return is_signed ? "icmp sle" : "icmp ule";
  case BINOP_GT:
    return is_signed ? "icmp sgt" : "icmp ugt";
  case BINOP_GE:
    return is_signed ? "icmp sge" : "icmp uge";
  case BINOP_AND:
    return "and";
  case BINOP_OR:
    return "or";
  case BINOP_BITAND:
    return "and";
  case BINOP_BITOR:
    return "or";
  case BINOP_BITXOR:
    return "xor";
  case BINOP_SHL:
    return "shl";
  case BINOP_SHR:
    return is_signed ? "ashr" : "lshr";
  }
  return "?";
}

char *binop_to_str(BinaryOp op) {
  switch (op) {
  case BINOP_ADD:
    return "+";
  case BINOP_SUB:
    return "-";
  case BINOP_MUL:
    return "*";
  case BINOP_DIV:
    return "/";
  case BINOP_REM:
    return "%";
  case BINOP_EQ:
    return "=";
  case BINOP_NE:
    return "!=";
  case BINOP_LT:
    return "<";
  case BINOP_LE:
    return "<=";
  case BINOP_GT:
    return ">";
  case BINOP_GE:
    return ">=";
  case BINOP_AND:
    return "&&";
  case BINOP_OR:
    return "||";
  case BINOP_BITAND:
    return "&";
  case BINOP_BITOR:
    return "|";
  case BINOP_BITXOR:
    return "^";
  case BINOP_SHL:
    return "<<";
  case BINOP_SHR:
    return ">>";
  }
  return "?";
}

bool binop_is_comparison(BinaryOp op) {
  switch (op) {
  case BINOP_EQ:
  case BINOP_NE:
  case BINOP_LT:
  case BINOP_GT:
  case BINOP_LE:
  case BINOP_GE:
    return true;
  default:
    return false;
  }
}

bool binop_is_logical(BinaryOp op) { return op == BINOP_AND || op == BINOP_OR; }

char *unop_to_ir(UnaryOp op) {
  switch (op) {
  case UNOP_NOT:
    return "xor";
  case UNOP_NEG:
    return "sub";
  default:
    return "?";
  }
}

char *unop_to_str(UnaryOp op) {
  switch (op) {
  case UNOP_NOT:
    return "!";
  case UNOP_NEG:
    return "-";
  default:
    return "?";
  }
}

expr_t *ast_binary_init(BinaryOp op, expr_t *lhs, expr_t *rhs, arena_t *arena) {
  expr_t *expr = arena_alloc(arena, sizeof(expr_t));
  expr->kind = EXPR_BINARY;
  expr->line = lhs->line;
  expr->col = lhs->col;
  expr->type = (type_t){.kind = TYPE_UNKNOWN};
  expr->next = NULL;
  expr->binary.op = op;
  expr->binary.lhs = lhs;
  expr->binary.rhs = rhs;
  return expr;
}

expr_t *ast_unary_init(UnaryOp op, expr_t *operand, arena_t *arena) {
  expr_t *expr = arena_alloc(arena, sizeof(expr_t));
  expr->kind = EXPR_UNARY;
  expr->line = operand->line;
  expr->col = operand->col;
  expr->type = (type_t){.kind = TYPE_UNKNOWN};
  expr->next = NULL;
  expr->unary.op = op;
  expr->unary.operand = operand;
  return expr;
}

expr_t *ast_boolean_init(bool value, token_t token, arena_t *arena) {
  expr_t *expr = arena_alloc(arena, sizeof(expr_t));
  expr->kind = EXPR_BOOLEAN;
  expr->line = token.line;
  expr->col = token.col;
  expr->type = (type_t){.kind = TYPE_BOOL};
  expr->next = NULL;
  expr->boolean.value = value;
  return expr;
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

expr_t *ast_cast_init(expr_t *operand, type_t target, arena_t *arena) {
  expr_t *expr = arena_alloc(arena, sizeof(expr_t));
  expr->kind = EXPR_CAST;
  expr->line = operand->line;
  expr->col = operand->col;
  expr->type = (type_t){.kind = TYPE_UNKNOWN};
  expr->next = NULL;
  expr->cast.operand = operand;
  expr->cast.target = target;
  return expr;
}

expr_t *ast_struct_literal_init(token_t token, arena_t *arena) {
  expr_t *expr = arena_alloc(arena, sizeof(expr_t));
  expr->kind = EXPR_STRUCT_LITERAL;
  expr->line = token.line;
  expr->col = token.col;
  expr->type = (type_t){.kind = TYPE_UNKNOWN};
  expr->next = NULL;
  expr->struct_literal.type_name = token.value;
  expr->struct_literal.inits = NULL;
  expr->struct_literal.init_count = 0;
  return expr;
}

expr_t *ast_field_access_init(expr_t *base, token_t name, arena_t *arena) {
  expr_t *expr = arena_alloc(arena, sizeof(expr_t));
  expr->kind = EXPR_FIELD;
  expr->line = name.line;
  expr->col = name.col;
  expr->type = (type_t){.kind = TYPE_UNKNOWN};
  expr->next = NULL;
  expr->field.base = base;
  expr->field.name = name.value;
  return expr;
}

expr_t *ast_array_init(token_t token, arena_t *arena) {
  expr_t *expr = arena_alloc(arena, sizeof(expr_t));
  expr->kind = EXPR_ARRAY;
  expr->line = token.line;
  expr->col = token.col;
  expr->type = (type_t){.kind = TYPE_UNKNOWN};
  expr->next = NULL;
  expr->array.elements = NULL;
  expr->array.element_count = 0;
  return expr;
}
expr_t *ast_index_init(expr_t *base, expr_t *index, arena_t *arena) {
  expr_t *expr = arena_alloc(arena, sizeof(expr_t));
  expr->kind = EXPR_INDEX;
  expr->line = base->line;
  expr->col = base->col;
  expr->type = (type_t){.kind = TYPE_UNKNOWN};
  expr->next = NULL;
  expr->index.base = base;
  expr->index.index = index;
  return expr;
}

stmt_t *ast_stmt_init(token_t token, StmtKind kind, arena_t *arena) {
  stmt_t *stmt = arena_alloc(arena, sizeof(stmt_t));
  stmt->kind = kind;
  stmt->line = token.line;
  stmt->col = token.col;
  stmt->next = NULL;
  return stmt;
}

stmt_t *ast_return_init(token_t token, expr_t *value, arena_t *arena) {
  stmt_t *stmt = ast_stmt_init(token, STMT_RETURN, arena);
  stmt->ret.value = value;
  return stmt;
}

stmt_t *ast_expr_stmt_init(token_t token, expr_t *value, arena_t *arena) {
  stmt_t *stmt = ast_stmt_init(token, STMT_EXPR, arena);
  stmt->expr_stmt.expr = value;
  return stmt;
}

stmt_t *ast_if_init(token_t token, expr_t *cond, stmt_t *then_body,
                    stmt_t *else_body, arena_t *arena) {
  stmt_t *stmt = ast_stmt_init(token, STMT_IF, arena);
  stmt->if_stmt.cond = cond;
  stmt->if_stmt.then_body = then_body;
  stmt->if_stmt.else_body = else_body;
  return stmt;
}

stmt_t *ast_binding_init(token_t token, char *name, type_t type, bool mutable,
                         expr_t *init, arena_t *arena) {
  stmt_t *stmt = ast_stmt_init(token, STMT_BINDING, arena);
  stmt->binding.name = name;
  stmt->binding.type = type;
  stmt->binding.mutable = mutable;
  stmt->binding.init = init;
  return stmt;
}

stmt_t *ast_assign_init(token_t token, expr_t *target, expr_t *value,
                        arena_t *arena) {
  stmt_t *stmt = ast_stmt_init(token, STMT_ASSIGN, arena);
  stmt->assign.target = target;
  stmt->assign.value = value;
  return stmt;
}

stmt_t *ast_while_init(token_t token, expr_t *cond, stmt_t *body,
                       arena_t *arena) {
  stmt_t *stmt = ast_stmt_init(token, STMT_WHILE, arena);
  stmt->while_loop.cond = cond;
  stmt->while_loop.body = body;
  return stmt;
}

stmt_t *ast_for_init(token_t token, char *var, expr_t *start, expr_t *end,
                     stmt_t *body, arena_t *arena) {
  stmt_t *stmt = ast_stmt_init(token, STMT_FOR, arena);
  stmt->for_loop.var = var;
  stmt->for_loop.start = start;
  stmt->for_loop.end = end;
  stmt->for_loop.body = body;
  return stmt;
}

param_t *ast_param_init(arena_t *arena) {
  param_t *param = arena_alloc(arena, sizeof(param_t));
  param->name = NULL;
  param->type = (type_t){.kind = TYPE_UNKNOWN};
  param->mutable = true;
  param->is_self = false;
  param->next = NULL;
  return param;
}

field_t *ast_field_init(arena_t *arena) {
  field_t *field = arena_alloc(arena, sizeof(field_t));
  field->name = NULL;
  field->type = (type_t){.kind = TYPE_UNKNOWN};
  field->next = NULL;
  return field;
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
  fn->fn.is_extern = false;
  fn->fn.variadic = false;
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

decl_t *ast_struct_init(char *name, arena_t *arena) {
  decl_t *decl = arena_alloc(arena, sizeof(decl_t));
  decl->kind = DECL_STRUCT;
  decl->visibility = VISIBILITY_PRIVATE;
  decl->name = name;
  decl->line = 0;
  decl->col = 0;
  decl->next = NULL;
  decl->strct.fields = NULL;
  decl->strct.field_count = 0;
  decl->strct.members = NULL;
  decl->strct.member_count = 0;
  return decl;
}

decl_t *ast_global_init(token_t token, Visibility visibility, char *name,
                        type_t type, bool mutable, expr_t *init,
                        arena_t *arena) {
  decl_t *global = arena_alloc(arena, sizeof(decl_t));
  global->kind = DECL_GLOBAL;
  global->visibility = visibility;
  global->name = name;
  global->line = token.line;
  global->col = token.col;
  global->next = NULL;
  global->global.type = type;
  global->global.mutable = mutable;
  global->global.init = init;
  return global;
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

static void dump_expr(const expr_t *expr, int depth);

static void dump_binary(const expr_t *expr, int depth) {
  printf("Binary %s\n", binop_to_str(expr->binary.op));
  dump_expr(expr->binary.lhs, depth + 1);
  dump_expr(expr->binary.rhs, depth + 1);
}

static void dump_unary(const expr_t *expr, int depth) {
  printf("Unary %s\n", unop_to_str(expr->unary.op));
  dump_expr(expr->unary.operand, depth + 1);
}

static void dump_expr(const expr_t *expr, int depth) {
  print_indent(depth);
  switch (expr->kind) {
  case EXPR_BOOLEAN:
    printf("Boolean literal %s\n", expr->boolean.value ? "true" : "false");
    break;
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
    if (expr->call.callee->kind == EXPR_FIELD) {
      printf("MethodCall '%s' (%zu args)\n", expr->call.callee->field.name,
             expr->call.arg_count);
      dump_expr(expr->call.callee->field.base, depth + 1);
    } else {
      printf("Call '%s' (%zu args)\n", expr->call.callee->id.name,
             expr->call.arg_count);
    }
    for (const expr_t *arg = expr->call.args; arg != NULL; arg = arg->next) {
      dump_expr(arg, depth + 1);
    }
    break;
  case EXPR_CAST:
    printf("Cast to %s\n", type_kind_to_str(expr->cast.target.kind));
    dump_expr(expr->cast.operand, depth + 1);
    break;
  case EXPR_BINARY:
    dump_binary(expr, depth);
    break;
  case EXPR_UNARY:
    dump_unary(expr, depth);
    break;
  case EXPR_STRUCT_LITERAL:
    printf("StructLit '%s'\n", expr->struct_literal.type_name);
    for (const field_init_t *init = expr->struct_literal.inits; init != NULL;
         init = init->next) {
      print_indent(depth + 1);
      printf("field %s\n", init->name);
      dump_expr(init->value, depth + 2);
    }
    break;
  case EXPR_FIELD:
    printf("Field %s\n", expr->field.name);
    dump_expr(expr->field.base, depth + 1);
    break;
  case EXPR_ARRAY:
    printf("Array (%zu elements)\n", expr->array.element_count);
    for (const expr_t *element = expr->array.elements; element != NULL;
         element = element->next) {
      dump_expr(element, depth + 1);
    }
    break;
  case EXPR_INDEX:
    printf("Index\n");
    dump_expr(expr->index.base, depth + 1);
    dump_expr(expr->index.index, depth + 1);
    break;
  }
}

static void dump_stmt(const stmt_t *stmt, int depth);

static void dump_block(const stmt_t *block, int depth) {
  for (const stmt_t *stmt = block; stmt != NULL; stmt = stmt->next) {
    dump_stmt(stmt, depth + 1);
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
  case STMT_IF:
    printf("If\n");
    dump_expr(stmt->if_stmt.cond, depth + 1);
    print_indent(depth + 1);
    printf("Then\n");
    dump_block(stmt->if_stmt.then_body, depth + 1);
    if (stmt->if_stmt.else_body != NULL) {
      print_indent(depth + 1);
      printf("Else\n");
      dump_block(stmt->if_stmt.else_body, depth + 1);
    }
    break;
  case STMT_BINDING:
    printf("%s %s: %s\n", stmt->binding.mutable ? "Let" : "Const",
           stmt->binding.name, type_to_str(stmt->binding.type));
    dump_expr(stmt->binding.init, depth + 1);
    break;
  case STMT_ASSIGN:
    if (stmt->assign.target->kind == EXPR_ID) {
      printf("Assign %s\n", stmt->assign.target->id.name);
    } else {
      printf("Assign\n");
      dump_expr(stmt->assign.target, depth + 1);
    }
    dump_expr(stmt->assign.value, depth + 1);
    break;
  case STMT_WHILE:
    printf("While\n");
    dump_expr(stmt->while_loop.cond, depth + 1);
    print_indent(depth + 1);
    printf("Do\n");
    dump_block(stmt->while_loop.body, depth + 1);
    break;
  case STMT_FOR:
    printf("For %s\n", stmt->for_loop.var);
    print_indent(depth + 1);
    printf("Range\n");
    dump_expr(stmt->for_loop.start, depth + 2);
    dump_expr(stmt->for_loop.end, depth + 2);
    print_indent(depth + 1);
    printf("Do\n");
    dump_block(stmt->for_loop.body, depth + 1);
    break;
  case STMT_BREAK:
    printf("Break\n");
    break;
  case STMT_CONTINUE:
    printf("Continue\n");
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
    if (decl->fn.is_extern) {
      printf("ExternFn '%s' -> %s%s\n", decl->name,
             type_to_str(decl->fn.return_type),
             decl->fn.variadic ? " (variadic)" : "");
    } else {
      printf("FnDecl %s '%s' -> %s\n", visibility_to_str(decl->visibility),
             decl->name ? decl->name : "(anonymous)",
             type_to_str(decl->fn.return_type));
    }
    for (const param_t *param = decl->fn.params; param != NULL;
         param = param->next) {
      print_indent(depth + 1);
      printf("param %s%s: %s\n", !param->mutable ? "const " : "", param->name,
             type_to_str(param->type));
    }

    if (!decl->fn.is_extern) {
      dump_block(decl->fn.body, depth);
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
  case DECL_STRUCT:
    printf("Struct %s '%s'\n", visibility_to_str(decl->visibility), decl->name);
    for (const field_t *field = decl->strct.fields; field != NULL;
         field = field->next) {
      print_indent(depth + 1);
      printf("field %s: %s\n", field->name, type_to_str(field->type));
    }
    for (const decl_t *member = decl->strct.members; member != NULL;
         member = member->next) {
      dump_decl(member, depth + 1);
    }
    break;
  case DECL_GLOBAL:
    printf("GlobalDecl %s %s '%s': %s\n", visibility_to_str(decl->visibility),
           decl->global.mutable ? "let" : "const", decl->name,
           type_to_str(decl->global.type));
    dump_expr(decl->global.init, depth + 1);
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
