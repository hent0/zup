#include "parser.h"
#include "ast.h"
#include "compiler.h"
#include "diag.h"
#include "lexer.h"
#include "token.h"
#include <stdlib.h>

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
  switch (parser->current.kind) {
  case TOKEN_VOID:
    advance(parser);
    return (type_t){.kind = TYPE_VOID};
  case TOKEN_BOOL:
    advance(parser);
    return (type_t){.kind = TYPE_BOOL};
  case TOKEN_I8:
    advance(parser);
    return (type_t){.kind = TYPE_I8};
  case TOKEN_I16:
    advance(parser);
    return (type_t){.kind = TYPE_I16};
  case TOKEN_I32:
    advance(parser);
    return (type_t){.kind = TYPE_I32};
  case TOKEN_I64:
    advance(parser);
    return (type_t){.kind = TYPE_I64};
  case TOKEN_STRING:
    advance(parser);
    return (type_t){.kind = TYPE_STRING};
  default:
    parse_error(parser, "expected a type");
    return (type_t){.kind = TYPE_UNKNOWN};
  }
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
  case TOKEN_ID: {
    advance(parser);
    expr_t *id = ast_id_init(token, parser->arena);
    if (check(parser, TOKEN_LPAREN)) {
      return parse_call_args(parser, id);
    }
    return id;
  }
  default:
    parse_error(parser, "expected an expression");
    return NULL;
  }
}

static expr_t *parse_cast(parser_t *parser) {
  expr_t *expr = parse_primary(parser);
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

static expr_t *parse_comparison(parser_t *parser) {
  expr_t *left = parse_additive(parser);
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
    expr_t *right = parse_additive(parser);
    if (right == NULL) {
      return NULL;
    }

    left = ast_binary_init(op, left, right, parser->arena);
  }

  return left;
}

static expr_t *parse_and(parser_t *parser) {
  expr_t *left = parse_comparison(parser);
  if (left == NULL) {
    return NULL;
  }

  while (match(parser, TOKEN_AMPERSAND_AMPERSAND)) {
    expr_t *right = parse_comparison(parser);
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
  expr_t *cond = parse_expr(parser);
  stmt_t *then_body = parse_block(parser);
  stmt_t *else_body = NULL;
  if (match(parser, TOKEN_ELSE)) {
    else_body =
        check(parser, TOKEN_IF) ? parse_if(parser) : parse_block(parser);
  }
  return ast_if_init(token, cond, then_body, else_body, parser->arena);
}

static stmt_t *parse_let(parser_t *parser) {
  token_t token = expect(parser, TOKEN_LET, "expected 'let'");
  token_t id = expect(parser, TOKEN_ID, "expected identifier");
  stmt_t *let = ast_let_init(token, id.value, (type_t){.kind = TYPE_UNKNOWN},
                             NULL, parser->arena);

  if (match(parser, TOKEN_COLON)) {
    let->let.type = parse_type(parser);
  }

  if (match(parser, TOKEN_SEMICOLON)) {
    if (let->let.type.kind == TYPE_UNKNOWN) {
      parse_error(parser, "expected type");
      return NULL;
    }
    return let;
  } else if (match(parser, TOKEN_EQUAL)) {
    let->let.init = parse_expr(parser);
    expect(parser, TOKEN_SEMICOLON, "expected ';' after let statement");
    return let;
  }

  parse_error(parser, "expected '=' or ';'");
  return NULL;
}

static stmt_t *parse_while(parser_t *parser) {
  token_t token = expect(parser, TOKEN_WHILE, "expected 'while'");
  expr_t *cond = parse_expr(parser);
  stmt_t *body = parse_block(parser);
  return ast_while_init(token, cond, body, parser->arena);
}

static stmt_t *parse_stmt(parser_t *parser) {
  if (check(parser, TOKEN_RETURN)) {
    return parse_return(parser);
  }

  if (check(parser, TOKEN_IF)) {
    return parse_if(parser);
  }

  if (check(parser, TOKEN_LET)) {
    return parse_let(parser);
  }

  if (check(parser, TOKEN_WHILE)) {
    return parse_while(parser);
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
    if (expr->kind != EXPR_ID) {
      parse_error(parser, "invalid assignment target");
      return NULL;
    }
    expr_t *value = parse_expr(parser);
    if (value == NULL) {
      return NULL;
    }
    expect(parser, TOKEN_SEMICOLON, "expected ';' after assignment");
    return ast_assign_init(start, expr->id.name, value, parser->arena);
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
  token_t id = expect(parser, TOKEN_ID, "expected identifier");
  expect(parser, TOKEN_COLON, "expected ':' after identifier");
  type_t type = parse_type(parser);
  if (type.kind == TYPE_UNKNOWN) {
    return NULL;
  }

  param_t *param = ast_param_init(parser->arena);
  param->name = id.value;
  param->type = type;
  return param;
}

static decl_t *parse_fn(parser_t *parser, Visibility visibility) {
  token_t kw = expect(parser, TOKEN_FN, "expected 'fn'");
  token_t name = expect(parser, TOKEN_ID, "expected function name");

  decl_t *fn = ast_fn_init(parser->arena);
  fn->visibility = visibility;
  fn->name = name.value;
  fn->line = kw.line;
  fn->col = kw.col;

  expect(parser, TOKEN_LPAREN, "expected '(' after function name");
  param_t *p_tail = NULL;
  while (!check(parser, TOKEN_RPAREN)) {
    if (p_tail != NULL) {
      expect(parser, TOKEN_COMMA, "expected ',' between arguments");
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

  fn->fn.return_type = parse_return_type(parser);
  fn->fn.body = parse_block(parser);
  return fn;
}

static Visibility parse_visibility(parser_t *parser) {
  if (match(parser, TOKEN_PUB)) {
    return VISIBILITY_PUBLIC;
  }
  return VISIBILITY_PRIVATE;
}

static decl_t *parse_decl(parser_t *parser) {
  Visibility visibility = parse_visibility(parser);
  if (check(parser, TOKEN_FN)) {
    return parse_fn(parser, visibility);
  }
  parse_error(parser, "expected 'fn'");
  return NULL;
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
      // Could not start a declaration; the stuck-token diagnostic has been
      // reported. Stop rather than re-reporting it forever.
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
