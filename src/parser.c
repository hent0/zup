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

static expr_t *parse_expr(parser_t *parser) {
  // TODO: Expand
  return parse_primary(parser);
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

static stmt_t *parse_stmt(parser_t *parser) {
  if (check(parser, TOKEN_RETURN)) {
    return parse_return(parser);
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

  expect(parser, TOKEN_LBRACE, "expected '{'");
  stmt_t *s_tail = NULL;
  while (!check(parser, TOKEN_RBRACE) && !check(parser, TOKEN_EOF)) {
    stmt_t *stmt = parse_stmt(parser);
    if (stmt == NULL) {
      break;
    }

    if (s_tail == NULL) {
      fn->fn.body = stmt;
    } else {
      s_tail->next = stmt;
    }

    s_tail = stmt;
    fn->fn.stmt_count++;
  }

  expect(parser, TOKEN_RBRACE, "expected '}'");
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
