#include "parser.h"
#include "arena.h"
#include "ast.h"
#include "compiler.h"
#include "diag.h"
#include "lexer.h"
#include "token.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  token_t token;
  char *name;
  type_t type;
  bool mutable;
  expr_t *init;
} binding_t;

static void parse_error(parser_t *parser, const char *msg) {
  diag_error(parser->src, parser->current.line, parser->current.col,
             "%s (got %s)", msg, token_kind_to_str(parser->current.kind));
  parser->had_error = true;
}

static token_t advance(parser_t *parser) {
  token_t prev = parser->current;
  parser->current = lexer_next_token(parser->lexer);
  return prev;
}

static bool check(parser_t *parser, TokenKind kind) {
  return parser->current.kind == kind;
}

static bool match(parser_t *parser, TokenKind kind) {
  if (!check(parser, kind)) {
    return false;
  }
  advance(parser);
  return true;
}

static token_t expect(parser_t *parser, TokenKind kind, const char *msg) {
  if (check(parser, kind)) {
    return advance(parser);
  }
  parse_error(parser, msg);
  return parser->current;
}

static type_t parse_type(parser_t *parser) {
  token_t token = parser->current;
  type_t type = {
      .kind = TYPE_UNKNOWN,
      .line = token.line,
      .col = token.col,
  };

  if (match(parser, TOKEN_LBRACKET)) {
    switch (parser->current.kind) {
    case TOKEN_RBRACKET:
      type.kind = TYPE_SLICE;
      break;
    case TOKEN_NUMBER:
      type.kind = TYPE_ARRAY;
      type.array_length = (size_t)atoi(parser->current.value);
      advance(parser);
      break;
    case TOKEN_UNDERSCORE:
      type.kind = TYPE_ARRAY;
      type.array_length = 0;
      advance(parser);
      break;
    default:
      parse_error(parser, "expected an array size");
      return type;
    }

    expect(parser, TOKEN_RBRACKET, "expected ']'");
    type_t *element = arena_alloc(parser->arena, sizeof(type_t));
    *element = parse_type(parser);
    type.element = element;
    return type;
  }

  switch (parser->current.kind) {
  case TOKEN_VOID:
    advance(parser);
    type.kind = TYPE_VOID;
    break;
  case TOKEN_BOOL:
    advance(parser);
    type.kind = TYPE_BOOL;
    break;
  case TOKEN_I8:
    advance(parser);
    type.kind = TYPE_I8;
    break;
  case TOKEN_U8:
    advance(parser);
    type.kind = TYPE_U8;
    break;
  case TOKEN_I16:
    advance(parser);
    type.kind = TYPE_I16;
    break;
  case TOKEN_U16:
    advance(parser);
    type.kind = TYPE_U16;
    break;
  case TOKEN_I32:
    advance(parser);
    type.kind = TYPE_I32;
    break;
  case TOKEN_U32:
    advance(parser);
    type.kind = TYPE_U32;
    break;
  case TOKEN_I64:
    advance(parser);
    type.kind = TYPE_I64;
    break;
  case TOKEN_U64:
    advance(parser);
    type.kind = TYPE_U64;
    break;
  case TOKEN_F32:
    advance(parser);
    type.kind = TYPE_F32;
    break;
  case TOKEN_F64:
    advance(parser);
    type.kind = TYPE_F64;
    break;
  case TOKEN_CSTR:
    advance(parser);
    type.kind = TYPE_CSTR;
    break;
  case TOKEN_STR:
    advance(parser);
    type.kind = TYPE_STR;
    break;
  case TOKEN_ID:
    advance(parser);
    type.kind = TYPE_STRUCT;
    type.name = token.value;
    if (match(parser, TOKEN_DOT)) {
      token_t member =
          expect(parser, TOKEN_ID, "expected struct name after '.'");
      type.module = token.value;
      type.name = member.value;
    }
    break;
  default:
    parse_error(parser, "expected a type");
    return type;
  }

  return type;
}

static expr_t *parse_expr(parser_t *parser);

static bool starts_expr(TokenKind kind) {
  return kind == TOKEN_NUMBER || kind == TOKEN_STRING || kind == TOKEN_ID;
}

static expr_t *parse_call_args(parser_t *parser, expr_t *callee) {
  expr_t *call = ast_call_init(callee, parser->arena);
  expect(parser, TOKEN_LPAREN, "expected '(' before arguments");

  if (!check(parser, TOKEN_RPAREN)) {
    expr_t *tail = NULL;
    do {
      expr_t *arg = parse_expr(parser);
      if (arg == NULL) {
        break;
      }

      if (tail == NULL) {
        call->call.args = arg;
      } else {
        tail->next = arg;
      }

      tail = arg;
      call->call.arg_count++;
    } while (match(parser, TOKEN_COMMA));
  }

  expect(parser, TOKEN_RPAREN, "expected ')' after arguments");
  return call;
}

static expr_t *parse_struct_body(parser_t *parser, expr_t *lit) {
  expect(parser, TOKEN_LBRACE, "expected '{' after struct name");

  field_init_t *tail = NULL;
  if (!check(parser, TOKEN_RBRACE)) {
    do {
      if (check(parser, TOKEN_RBRACE)) {
        break;
      }
      token_t name = expect(parser, TOKEN_ID, "expected field name");
      expect(parser, TOKEN_COLON, "expected ':' after field name");
      expr_t *value = parse_expr(parser);
      if (value == NULL) {
        return NULL;
      }

      field_init_t *init = arena_alloc(parser->arena, sizeof(field_init_t));
      init->name = name.value;
      init->value = value;
      init->line = name.line;
      init->col = name.col;
      init->next = NULL;

      if (tail == NULL) {
        lit->struct_literal.inits = init;
      } else {
        tail->next = init;
      }
      tail = init;
      lit->struct_literal.init_count++;
    } while (match(parser, TOKEN_COMMA));
  }

  expect(parser, TOKEN_RBRACE, "expected '}' after struct fields");
  return lit;
}

static expr_t *parse_struct_literal(parser_t *parser, token_t type_name) {
  return parse_struct_body(parser,
                           ast_struct_literal_init(type_name, parser->arena));
}

static expr_t *parse_match(parser_t *parser) {
  token_t kw = parser->current;
  advance(parser);

  bool saved = parser->no_struct_literal;
  parser->no_struct_literal = true;
  expr_t *scrutinee = parse_expr(parser);
  parser->no_struct_literal = saved;

  expr_t *expr = ast_match_init(kw, scrutinee, parser->arena);
  expect(parser, TOKEN_LBRACE, "expected '{' after match value");

  match_arm_t *tail = NULL;
  while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
    match_arm_t *arm = ast_match_arm_init(parser->arena);
    arm->line = parser->current.line;
    arm->col = parser->current.col;

    if (check(parser, TOKEN_UNDERSCORE)) {
      advance(parser);
    } else {
      arm->pattern = parse_expr(parser);
    }
    expect(parser, TOKEN_FAT_ARROW, "expected '=>' after match pattern");
    arm->value = parse_expr(parser);

    if (tail == NULL) {
      expr->match_expr.arms = arm;
    } else {
      tail->next = arm;
    }
    tail = arm;
    expr->match_expr.arm_count++;

    if (!match(parser, TOKEN_COMMA)) {
      break;
    }
  }

  expect(parser, TOKEN_RBRACE, "expected '}' after match arms");
  return expr;
}

static expr_t *parse_primary(parser_t *parser) {
  token_t token = parser->current;
  switch (parser->current.kind) {
  case TOKEN_TRUE:
    advance(parser);
    return ast_boolean_init(true, token, parser->arena);
  case TOKEN_FALSE:
    advance(parser);
    return ast_boolean_init(false, token, parser->arena);
  case TOKEN_NUMBER:
    advance(parser);
    return ast_number_init(token, parser->arena);
  case TOKEN_STRING:
    advance(parser);
    return ast_string_init(token, parser->arena);
  case TOKEN_IMPORT:
    advance(parser);
    expect(parser, TOKEN_LPAREN, "expected '(' after 'import'");
    token_t path =
        expect(parser, TOKEN_STRING, "expected a module path string");
    expect(parser, TOKEN_RPAREN, "expected ')' after import path");
    return ast_import_init(token, path.value, parser->arena);
  case TOKEN_ID: {
    advance(parser);
    if (check(parser, TOKEN_LPAREN)) {
      return parse_call_args(parser, ast_id_init(token, parser->arena));
    }
    if (check(parser, TOKEN_LBRACE) && !parser->no_struct_literal) {
      return parse_struct_literal(parser, token);
    }
    return ast_id_init(token, parser->arena);
  }
  case TOKEN_DOT: {
    advance(parser);
    token_t name =
        expect(parser, TOKEN_ID, "expected enum member name after '.'");
    return ast_enum_literal_init(name, parser->arena);
  }
  case TOKEN_MATCH:
    return parse_match(parser);
  case TOKEN_LPAREN:
    advance(parser);
    expr_t *inner = parse_expr(parser);
    expect(parser, TOKEN_RPAREN, "expected ')'");
    return inner;
  case TOKEN_LBRACKET: {
    advance(parser);
    expr_t *array = ast_array_init(token, parser->arena);
    expr_t *tail = NULL;
    if (!check(parser, TOKEN_RBRACKET)) {
      do {
        expr_t *elem = parse_expr(parser);
        if (elem == NULL) {
          break;
        }
        if (tail == NULL) {
          array->array.elements = elem;
        } else {
          tail->next = elem;
        }
        tail = elem;
        array->array.element_count++;
      } while (match(parser, TOKEN_COMMA));
    }
    expect(parser, TOKEN_RBRACKET, "expected ']' after array elements");
    return array;
  }
  default:
    parse_error(parser, "expected an expression");
    return NULL;
  }
}

static expr_t *parse_postfix(parser_t *parser) {
  expr_t *expr = parse_primary(parser);
  if (expr == NULL) {
    return NULL;
  }
  for (;;) {
    if (check(parser, TOKEN_DOT)) {
      advance(parser);
      token_t name = expect(parser, TOKEN_ID, "expected field name after '.'");
      expr = ast_field_access_init(expr, name, parser->arena);
    } else if (check(parser, TOKEN_LPAREN) && expr->kind == EXPR_FIELD) {
      expr = parse_call_args(parser, expr);
    } else if (check(parser, TOKEN_LBRACE) && !parser->no_struct_literal &&
               expr->kind == EXPR_FIELD && expr->field.base->kind == EXPR_ID) {
      token_t name = {
          .value = expr->field.name, .line = expr->line, .col = expr->col};
      expr_t *lit = ast_struct_literal_init(name, parser->arena);
      lit->struct_literal.module = expr->field.base->id.name;
      expr = parse_struct_body(parser, lit);
    } else if (check(parser, TOKEN_LBRACKET)) {
      advance(parser);
      expr_t *index = parse_expr(parser);
      expect(parser, TOKEN_RBRACKET, "expected ']' after index");
      expr = ast_index_init(expr, index, parser->arena);
    } else {
      break;
    }
  }
  return expr;
}

static expr_t *parse_unary(parser_t *parser) {
  switch (parser->current.kind) {
  case TOKEN_BANG:
    advance(parser);
    return ast_unary_init(UNOP_NOT, parse_unary(parser), parser->arena);
  case TOKEN_MINUS:
    advance(parser);
    return ast_unary_init(UNOP_NEG, parse_unary(parser), parser->arena);
  default:
    return parse_postfix(parser);
  }
}

static expr_t *parse_cast(parser_t *parser) {
  expr_t *expr = parse_unary(parser);
  if (expr == NULL) {
    return NULL;
  }
  while (match(parser, TOKEN_AS)) {
    type_t target = parse_type(parser);
    if (target.kind == TYPE_UNKNOWN) {
      return NULL;
    }
    expr = ast_cast_init(expr, target, parser->arena);
  }
  return expr;
}

static expr_t *parse_multiplicative(parser_t *parser) {
  expr_t *left = parse_cast(parser);
  if (left == NULL) {
    return NULL;
  }

  for (;;) {
    BinaryOp op;
    if (check(parser, TOKEN_STAR)) {
      op = BINOP_MUL;
    } else if (check(parser, TOKEN_SLASH)) {
      op = BINOP_DIV;
    } else if (check(parser, TOKEN_PERCENT)) {
      op = BINOP_REM;
    } else {
      break;
    }

    advance(parser);
    expr_t *right = parse_cast(parser);
    if (right == NULL) {
      return NULL;
    }

    left = ast_binary_init(op, left, right, parser->arena);
  }

  return left;
}

static expr_t *parse_additive(parser_t *parser) {
  expr_t *left = parse_multiplicative(parser);
  if (left == NULL) {
    return NULL;
  }

  for (;;) {
    BinaryOp op;
    if (check(parser, TOKEN_PLUS)) {
      op = BINOP_ADD;
    } else if (check(parser, TOKEN_MINUS)) {
      op = BINOP_SUB;
    } else {
      break;
    }

    advance(parser);
    expr_t *right = parse_multiplicative(parser);
    if (right == NULL) {
      return NULL;
    }

    left = ast_binary_init(op, left, right, parser->arena);
  }

  return left;
}

static expr_t *parse_shift(parser_t *parser) {
  expr_t *left = parse_additive(parser);
  if (left == NULL) {
    return NULL;
  }

  for (;;) {
    BinaryOp op;
    if (check(parser, TOKEN_LESS_LESS)) {
      op = BINOP_SHL;
    } else if (check(parser, TOKEN_GREATER_GREATER)) {
      op = BINOP_SHR;
    } else {
      break;
    }

    advance(parser);
    expr_t *right = parse_additive(parser);
    if (right == NULL) {
      return NULL;
    }

    left = ast_binary_init(op, left, right, parser->arena);
  }

  return left;
}

static expr_t *parse_comparison(parser_t *parser) {
  expr_t *left = parse_shift(parser);
  if (left == NULL) {
    return NULL;
  }

  for (;;) {
    BinaryOp op;
    if (check(parser, TOKEN_EQUAL_EQUAL)) {
      op = BINOP_EQ;
    } else if (check(parser, TOKEN_BANG_EQUAL)) {
      op = BINOP_NE;
    } else if (check(parser, TOKEN_LESS)) {
      op = BINOP_LT;
    } else if (check(parser, TOKEN_LESS_EQUAL)) {
      op = BINOP_LE;
    } else if (check(parser, TOKEN_GREATER)) {
      op = BINOP_GT;
    } else if (check(parser, TOKEN_GREATER_EQUAL)) {
      op = BINOP_GE;
    } else {
      break;
    }

    advance(parser);
    expr_t *right = parse_shift(parser);
    if (right == NULL) {
      return NULL;
    }

    left = ast_binary_init(op, left, right, parser->arena);
  }

  return left;
}

static expr_t *parse_bitand(parser_t *parser) {
  expr_t *left = parse_comparison(parser);
  if (left == NULL) {
    return NULL;
  }

  while (match(parser, TOKEN_AMPERSAND)) {
    expr_t *right = parse_comparison(parser);
    if (right == NULL) {
      return NULL;
    }
    left = ast_binary_init(BINOP_BITAND, left, right, parser->arena);
  }
  return left;
}

static expr_t *parse_bitxor(parser_t *parser) {
  expr_t *left = parse_bitand(parser);
  if (left == NULL) {
    return NULL;
  }

  while (match(parser, TOKEN_CARET)) {
    expr_t *right = parse_bitand(parser);
    if (right == NULL) {
      return NULL;
    }
    left = ast_binary_init(BINOP_BITXOR, left, right, parser->arena);
  }
  return left;
}

static expr_t *parse_bitor(parser_t *parser) {
  expr_t *left = parse_bitxor(parser);
  if (left == NULL) {
    return NULL;
  }

  while (match(parser, TOKEN_PIPE)) {
    expr_t *right = parse_bitxor(parser);
    if (right == NULL) {
      return NULL;
    }
    left = ast_binary_init(BINOP_BITOR, left, right, parser->arena);
  }
  return left;
}

static expr_t *parse_and(parser_t *parser) {
  expr_t *left = parse_bitor(parser);
  if (left == NULL) {
    return NULL;
  }

  while (match(parser, TOKEN_AMPERSAND_AMPERSAND)) {
    expr_t *right = parse_bitor(parser);
    if (right == NULL) {
      return NULL;
    }
    left = ast_binary_init(BINOP_AND, left, right, parser->arena);
  }
  return left;
}

static expr_t *parse_or(parser_t *parser) {
  expr_t *left = parse_and(parser);
  if (left == NULL) {
    return NULL;
  }

  while (match(parser, TOKEN_PIPE_PIPE)) {
    expr_t *right = parse_and(parser);
    if (right == NULL) {
      return NULL;
    }
    left = ast_binary_init(BINOP_OR, left, right, parser->arena);
  }
  return left;
}

static expr_t *parse_expr(parser_t *parser) { return parse_or(parser); }

static stmt_t *parse_stmt(parser_t *parser);

static stmt_t *parse_block(parser_t *parser) {
  expect(parser, TOKEN_LBRACE, "expected '{'");
  stmt_t *head = NULL;
  stmt_t *tail = NULL;
  while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
    stmt_t *stmt = parse_stmt(parser);
    if (stmt == NULL) {
      break;
    }

    if (tail == NULL) {
      head = stmt;
    } else {
      tail->next = stmt;
    }

    tail = stmt;
  }

  expect(parser, TOKEN_RBRACE, "expected '}'");
  return head;
}

static stmt_t *parse_return(parser_t *parser) {
  token_t token = parser->current;
  expect(parser, TOKEN_RETURN, "expected 'return'");
  expr_t *value = NULL;
  if (!check(parser, TOKEN_SEMICOLON)) {
    value = parse_expr(parser);
  }
  expect(parser, TOKEN_SEMICOLON, "expected ';' after return statement");
  return ast_return_init(token, value, parser->arena);
}

static stmt_t *parse_if(parser_t *parser) {
  token_t token = expect(parser, TOKEN_IF, "expected 'if'");
  bool saved = parser->no_struct_literal;
  parser->no_struct_literal = true;
  expr_t *cond = parse_expr(parser);
  parser->no_struct_literal = saved;
  stmt_t *then_body = parse_block(parser);
  stmt_t *else_body = NULL;
  if (match(parser, TOKEN_ELSE)) {
    else_body =
        check(parser, TOKEN_IF) ? parse_if(parser) : parse_block(parser);
  }
  return ast_if_init(token, cond, then_body, else_body, parser->arena);
}

static binding_t parse_binding(parser_t *parser) {
  bool mutable = check(parser, TOKEN_LET);
  token_t token = mutable ? expect(parser, TOKEN_LET, "expected 'let'")
                          : expect(parser, TOKEN_CONST, "expected 'const'");
  token_t id = expect(parser, TOKEN_ID, "expected identifier");

  type_t type = (type_t){.kind = TYPE_UNKNOWN};
  if (match(parser, TOKEN_COLON)) {
    type = parse_type(parser);
  }

  expr_t *init = NULL;
  if (match(parser, TOKEN_EQUAL)) {
    init = parse_expr(parser);
  } else if (!mutable) {
    parse_error(parser, "const requires an initializer");
  }

  expect(parser, TOKEN_SEMICOLON, "expected ';' after binding");
  return (binding_t){
      .token = token,
      .name = id.value,
      .type = type,
      .mutable = mutable,
      .init = init,
  };
}

static decl_t *parse_global_binding(parser_t *parser, Visibility visibility) {
  binding_t binding = parse_binding(parser);

  if (binding.init != NULL && binding.init->kind == EXPR_IMPORT) {
    return ast_import_decl_init(binding.token, binding.name,
                                binding.init->import.path, parser->arena);
  }

  return ast_global_init(binding.token, visibility, binding.name, binding.type,
                         binding.mutable, binding.init, parser->arena);
}

static stmt_t *parse_local_binding(parser_t *parser) {
  binding_t binding = parse_binding(parser);
  return ast_binding_init(binding.token, binding.name, binding.type,
                          binding.mutable, binding.init, parser->arena);
}

static stmt_t *parse_while(parser_t *parser) {
  token_t token = expect(parser, TOKEN_WHILE, "expected 'while'");
  bool saved = parser->no_struct_literal;
  parser->no_struct_literal = true;
  expr_t *cond = parse_expr(parser);
  parser->no_struct_literal = saved;
  stmt_t *body = parse_block(parser);
  return ast_while_init(token, cond, body, parser->arena);
}

static stmt_t *parse_for(parser_t *parser) {
  token_t token = expect(parser, TOKEN_FOR, "expected 'for'");
  token_t id = expect(parser, TOKEN_ID, "expected identifier");
  expect(parser, TOKEN_IN, "expected 'in'");

  bool saved = parser->no_struct_literal;
  parser->no_struct_literal = true;
  expr_t *start = parse_expr(parser);

  if (match(parser, TOKEN_DOT_DOT)) {
    expr_t *end = parse_expr(parser);
    parser->no_struct_literal = saved;
    stmt_t *body = parse_block(parser);
    return ast_for_init(token, id.value, start, end, body, parser->arena);
  }

  parser->no_struct_literal = saved;
  stmt_t *body = parse_block(parser);
  return ast_foreach_init(token, id.value, start, body, parser->arena);
}

static stmt_t *parse_stmt(parser_t *parser) {
  if (check(parser, TOKEN_RETURN)) {
    return parse_return(parser);
  }

  if (check(parser, TOKEN_IF)) {
    return parse_if(parser);
  }

  if (check(parser, TOKEN_LET) || check(parser, TOKEN_CONST)) {
    return parse_local_binding(parser);
  }

  if (check(parser, TOKEN_WHILE)) {
    return parse_while(parser);
  }

  if (check(parser, TOKEN_FOR)) {
    return parse_for(parser);
  }

  if (check(parser, TOKEN_BREAK)) {
    token_t token = expect(parser, TOKEN_BREAK, "expected 'break'");
    expect(parser, TOKEN_SEMICOLON, "expected ';'");
    return ast_stmt_init(token, STMT_BREAK, parser->arena);
  }

  if (check(parser, TOKEN_CONTINUE)) {
    token_t token = expect(parser, TOKEN_CONTINUE, "expected 'continue'");
    expect(parser, TOKEN_SEMICOLON, "expected ';'");
    return ast_stmt_init(token, STMT_CONTINUE, parser->arena);
  }

  if (!starts_expr(parser->current.kind)) {
    parse_error(parser, "expected statement");
    return NULL;
  }

  token_t start = parser->current;
  expr_t *expr = parse_expr(parser);
  if (expr == NULL) {
    return NULL;
  }

  if (match(parser, TOKEN_EQUAL)) {
    if (expr->kind != EXPR_ID && expr->kind != EXPR_FIELD &&
        expr->kind != EXPR_INDEX) {
      parse_error(parser, "invalid assignment target");
      return NULL;
    }
    expr_t *value = parse_expr(parser);
    if (value == NULL) {
      return NULL;
    }
    expect(parser, TOKEN_SEMICOLON, "expected ';' after assignment");
    return ast_assign_init(start, expr, value, parser->arena);
  }

  expect(parser, TOKEN_SEMICOLON, "expected ';' after expression statement");
  return ast_expr_stmt_init(start, expr, parser->arena);
}

static type_t parse_return_type(parser_t *parser) {
  if (match(parser, TOKEN_COLON)) {
    return parse_type(parser);
  }

  return (type_t){.kind = TYPE_VOID};
}

static param_t *parse_param(parser_t *parser) {
  bool mutable = !match(parser, TOKEN_CONST);
  token_t id = expect(parser, TOKEN_ID, "expected identifier");
  expect(parser, TOKEN_COLON, "expected ':' after identifier");
  type_t type = parse_type(parser);
  if (type.kind == TYPE_UNKNOWN) {
    return NULL;
  }

  expr_t *default_value = NULL;
  if (match(parser, TOKEN_EQUAL)) {
    default_value = parse_expr(parser);
  }

  param_t *param = ast_param_init(parser->arena);
  param->name = id.value;
  param->type = type;
  param->default_value = default_value;
  param->mutable = mutable;
  return param;
}

static void parse_param_list(parser_t *parser, decl_t *fn,
                             bool allow_variadic) {
  expect(parser, TOKEN_LPAREN, "expected '(' after function name");
  param_t *p_tail = NULL;
  while (!check(parser, TOKEN_RPAREN)) {
    if (p_tail != NULL) {
      expect(parser, TOKEN_COMMA, "expected ',' between arguments");
    }

    if (check(parser, TOKEN_DOT_DOT_DOT)) {
      token_t dots = advance(parser);
      if (!allow_variadic) {
        diag_error(parser->src, dots.line, dots.col,
                   "variadic '...' is only allowed in extern declarations");
        parser->had_error = true;
      } else if (fn->fn.params_count == 0) {
        diag_error(parser->src, dots.line, dots.col,
                   "a variadic function requires at least one fixed parameter");
        parser->had_error = true;
      }
      fn->fn.variadic = true;
      break;
    }

    param_t *param = parse_param(parser);
    if (param == NULL) {
      break;
    }

    if (p_tail == NULL) {
      fn->fn.params = param;
    } else {
      p_tail->next = param;
    }
    p_tail = param;
    fn->fn.params_count++;
  }
  expect(parser, TOKEN_RPAREN, "expected ')' after parameters");
}

static decl_t *parse_fn(parser_t *parser, Visibility visibility) {
  token_t kw = expect(parser, TOKEN_FN, "expected 'fn'");
  token_t name = expect(parser, TOKEN_ID, "expected function name");

  decl_t *fn = ast_fn_init(parser->arena);
  fn->visibility = visibility;
  fn->name = name.value;
  fn->line = kw.line;
  fn->col = kw.col;

  parse_param_list(parser, fn, false);

  fn->fn.return_type = parse_return_type(parser);
  fn->fn.body = parse_block(parser);
  return fn;
}

static decl_t *parse_extern(parser_t *parser, Visibility visibility) {
  token_t kw = expect(parser, TOKEN_EXTERN, "expected 'extern'");
  expect(parser, TOKEN_FN, "expected 'fn' after 'extern'");
  token_t name = expect(parser, TOKEN_ID, "expected function name");

  decl_t *fn = ast_fn_init(parser->arena);
  fn->visibility = visibility;
  fn->name = name.value;
  fn->line = kw.line;
  fn->col = kw.col;
  fn->fn.is_extern = true;

  parse_param_list(parser, fn, true);

  fn->fn.return_type = parse_return_type(parser);
  expect(parser, TOKEN_SEMICOLON, "expected ';' after extern declaration");
  if (check(parser, TOKEN_LBRACE)) {
    parse_block(parser);
  }
  return fn;
}

static decl_t *parse_method(parser_t *parser, char *struct_name,
                            Visibility visibility) {
  token_t kw = expect(parser, TOKEN_FN, "expected 'fn'");
  token_t name = expect(parser, TOKEN_ID, "expected method name");

  decl_t *fn = ast_fn_init(parser->arena);
  fn->visibility = visibility;
  fn->name = name.value;
  fn->line = kw.line;
  fn->col = kw.col;

  expect(parser, TOKEN_LPAREN, "expected '(' after method name");

  param_t *tail = NULL;
  if (check(parser, TOKEN_ID) && strcmp(parser->current.value, "self") == 0) {
    token_t s = advance(parser);
    param_t *self = ast_param_init(parser->arena);
    self->name = s.value;
    self->type = (type_t){
        .kind = TYPE_STRUCT, .name = struct_name, .line = s.line, .col = s.col};
    self->is_self = true;
    fn->fn.params = self;
    tail = self;
    fn->fn.params_count++;
    if (!check(parser, TOKEN_RPAREN)) {
      expect(parser, TOKEN_COMMA, "expected ',' after self");
    }
  }

  while (!check(parser, TOKEN_RPAREN)) {
    param_t *p = parse_param(parser);
    if (p == NULL) {
      break;
    }
    if (tail == NULL) {
      fn->fn.params = p;
    } else {
      tail->next = p;
    }
    tail = p;
    fn->fn.params_count++;
    if (!match(parser, TOKEN_COMMA)) {
      break;
    }
  }
  expect(parser, TOKEN_RPAREN, "expected ')' after parameters");

  fn->fn.return_type = parse_return_type(parser);
  fn->fn.body = parse_block(parser);
  return fn;
}

static decl_t *parse_struct(parser_t *parser, Visibility visibility) {
  token_t kw = expect(parser, TOKEN_STRUCT, "expected 'struct'");
  token_t name = expect(parser, TOKEN_ID, "expected struct name");

  decl_t *decl = ast_struct_init(name.value, parser->arena);
  decl->visibility = visibility;
  decl->line = kw.line;
  decl->col = kw.col;

  expect(parser, TOKEN_LBRACE, "expected '{' after struct name");

  field_t *f_tail = NULL;
  decl_t *m_tail = NULL;
  while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
    Visibility member_vis =
        match(parser, TOKEN_PUB) ? VISIBILITY_PUBLIC : VISIBILITY_PRIVATE;

    if (check(parser, TOKEN_FN)) {
      decl_t *method = parse_method(parser, decl->name, member_vis);
      if (method == NULL) {
        break;
      }
      if (m_tail == NULL) {
        decl->strct.members = method;
      } else {
        m_tail->next = method;
      }
      m_tail = method;
      decl->strct.member_count++;
      continue;
    }

    token_t fname = expect(parser, TOKEN_ID, "expected field name");
    expect(parser, TOKEN_COLON, "expected ':' after field name");
    type_t ftype = parse_type(parser);

    expr_t *default_value = NULL;
    if (match(parser, TOKEN_EQUAL)) {
      default_value = parse_expr(parser);
    }

    field_t *field = ast_field_init(parser->arena);
    field->name = fname.value;
    field->type = ftype;
    field->default_value = default_value;
    field->visibility = member_vis;
    field->line = fname.line;
    field->col = fname.col;

    if (f_tail == NULL) {
      decl->strct.fields = field;
    } else {
      f_tail->next = field;
    }
    f_tail = field;
    decl->strct.field_count++;
    match(parser, TOKEN_COMMA);
  }

  expect(parser, TOKEN_RBRACE, "expected '}' after struct body");
  return decl;
}

static decl_t *parse_enum(parser_t *parser, Visibility visibility) {
  token_t kw = expect(parser, TOKEN_ENUM, "expected 'enum'");
  token_t name = expect(parser, TOKEN_ID, "expected enum name");

  decl_t *decl = ast_enum_init(name.value, parser->arena);
  decl->visibility = visibility;
  decl->line = kw.line;
  decl->col = kw.col;

  expect(parser, TOKEN_LBRACE, "expected '{' after struct name");

  enum_member_t *tail = NULL;
  long long next_value = 0;
  while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
    token_t member_name = expect(parser, TOKEN_ID, "expected enum member name");
    enum_member_t *member = ast_enum_member_init(parser->arena);
    member->name = member_name.value;
    member->value = next_value++;
    member->line = member_name.line;
    member->col = member_name.col;

    if (tail == NULL) {
      decl->enm.members = member;
    } else {
      tail->next = member;
    }
    tail = member;
    decl->enm.member_count++;

    match(parser, TOKEN_COMMA);
  }

  expect(parser, TOKEN_RBRACE, "expected '}'");
  return decl;
}

static Visibility parse_visibility(parser_t *parser) {
  if (match(parser, TOKEN_PUB)) {
    return VISIBILITY_PUBLIC;
  }
  return VISIBILITY_PRIVATE;
}

static decl_t *parse_decl(parser_t *parser) {
  Visibility visibility = parse_visibility(parser);

  if (check(parser, TOKEN_EXTERN)) {
    return parse_extern(parser, visibility);
  }

  switch (parser->current.kind) {
  case TOKEN_FN:
    return parse_fn(parser, visibility);
  case TOKEN_CONST:
  case TOKEN_LET:
    return parse_global_binding(parser, visibility);
  case TOKEN_STRUCT:
    return parse_struct(parser, visibility);
  case TOKEN_ENUM:
    return parse_enum(parser, visibility);
  default:
    parse_error(parser, "expected a declaration");
    return NULL;
  }
}

parser_t parser_init(lexer_t *lexer, source_t *src, arena_t *arena) {
  parser_t parser = {
      .lexer = lexer,
      .src = src,
      .arena = arena,
      .current = lexer_next_token(lexer),
      .had_error = false,
  };

  return parser;
}

unit_t *parser_parse(parser_t *parser) {
  unit_t *unit = ast_unit_init(parser->src, parser->arena);
  decl_t *root = unit->root;
  decl_t *tail = NULL;
  while (parser->current.kind != TOKEN_EOF) {
    decl_t *decl = parse_decl(parser);
    if (decl == NULL) {
      break;
    }

    if (tail == NULL) {
      root->container.members = decl;
    } else {
      tail->next = decl;
    }

    tail = decl;
    root->container.member_count++;
  }

  if (parser->had_error) {
    return NULL;
  }

  return unit;
}
