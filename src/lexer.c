#include "lexer.h"
#include "arena.h"
#include "diag.h"
#include "token.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

const keyword_t keywords[] = {
    {"pub", TOKEN_PUB},
    {"fn", TOKEN_FN},
    {"return", TOKEN_RETURN},
    {"as", TOKEN_AS},
    {"if", TOKEN_IF},
    {"else", TOKEN_ELSE},
    {"let", TOKEN_LET},
    {"while", TOKEN_WHILE},
    {"break", TOKEN_BREAK},
    {"continue", TOKEN_CONTINUE},
    // Types
    {"i8", TOKEN_I8},
    {"i16", TOKEN_I16},
    {"i32", TOKEN_I32},
    {"i64", TOKEN_I64},
    {"void", TOKEN_VOID},
    {"bool", TOKEN_BOOL},
    {"true", TOKEN_TRUE},
    {"false", TOKEN_FALSE},
    {NULL, TOKEN_EOF},
};

static bool at_eof(lexer_t *lexer) { return *lexer->current == '\0'; }

static void advance(lexer_t *lexer) {
  if (at_eof(lexer)) {
    return;
  }

  if (*lexer->current == '\n') {
    lexer->line++;
    lexer->col = 1;
  } else {
    lexer->col++;
  }

  lexer->current++;
}

static void advance_n(lexer_t *lexer, unsigned int n) {
  for (unsigned int i = 0; i < n; i++) {
    advance(lexer);
  }
}

static token_t advance_with(lexer_t *lexer, token_t token) {
  advance(lexer);
  return token;
}

static char peek(lexer_t *lexer) {
  if (at_eof(lexer)) {
    return '\0';
  }

  return lexer->current[1];
}

static void skip_whitespace(lexer_t *lexer) {
  while (!at_eof(lexer) && (*lexer->current == ' ' || *lexer->current == '\t' ||
                            *lexer->current == '\n')) {
    advance(lexer);
  }
}

static void skip_comments(lexer_t *lexer) {
  skip_whitespace(lexer);
  if (*lexer->current != '/') {
    return;
  }

  char next = peek(lexer);
  switch (next) {
  case '/':
    while (!at_eof(lexer) && *lexer->current != '\n') {
      advance(lexer);
    }
    break;
  case '*':
    while (!at_eof(lexer) && !(*lexer->current == '*' && peek(lexer) == '/')) {
      advance(lexer);
    }
    advance_n(lexer, 2);
    break;
  default:
    return;
  }
  skip_comments(lexer);
}

static bool is_id_start(const char c) { return isalpha(c) || c == '_'; }

static bool is_id_char(const char c) { return is_id_start(c) || isdigit(c); }

static TokenKind id_to_token_kind(const char *id) {
  for (int i = 0; keywords[i].name != NULL; i++) {
    if (strcmp(keywords[i].name, id) == 0) {
      return keywords[i].kind;
    }
  }

  return TOKEN_ID;
}

static token_t id(lexer_t *lexer) {
  const char *start = lexer->current;
  unsigned int start_line = lexer->line;
  unsigned int start_col = lexer->col;

  while (is_id_char(*lexer->current)) {
    advance(lexer);
  }

  size_t len = (size_t)(lexer->current - start);
  char *id = arena_strndup(lexer->arena, start, len);

  return token_init(id_to_token_kind(id), .line = start_line, .col = start_col,
                    .value = id);
}

static token_t string_literal(lexer_t *lexer) {
  unsigned int start_line = lexer->line;
  unsigned int start_col = lexer->col;
  advance(lexer);
  const char *start = lexer->current;

  while (!at_eof(lexer) && *lexer->current != '"') {
    if (*lexer->current == '\\') {
      advance(lexer);
      if (at_eof(lexer)) {
        break;
      }
    }
    advance(lexer);
  }

  if (at_eof(lexer)) {
    diag_error(NULL, start_line, start_col, "unterminated string literal");
    return token_init(TOKEN_EOF);
  }

  size_t len = (size_t)(lexer->current - start);
  advance(lexer);

  char *id = arena_alloc(lexer->arena, len + 1);
  size_t out = 0;
  for (size_t i = 0; i < len;) {
    if (start[i] == '\\' && i + 1 < len) {
      switch (start[i + 1]) {
      case 'n':
        id[out++] = '\n';
        break;
      case 't':
        id[out++] = '\t';
        break;
      case 'r':
        id[out++] = '\r';
        break;
      case '0':
        id[out++] = '\0';
        break;
      case '\\':
        id[out++] = '\\';
        break;
      case '"':
        id[out++] = '"';
        break;
      default:
        diag_error(NULL, start_line, start_col + (unsigned)i,
                   "unknown escape '\\%c'", start[i + 1]);
        id[out++] = start[i + 1];
        break;
      }
      i += 2;
    } else {
      id[out++] = start[i++];
    }
  }

  return token_init(TOKEN_STRING, .line = start_line, .col = start_col,
                    .value = id, .length = out);
}

// TODO: Add support for floats
static token_t number_literal(lexer_t *lexer) {
  const char *start = lexer->current;
  unsigned int start_line = lexer->line;
  unsigned int start_col = lexer->col;

  char next = peek(lexer);
  if (*lexer->current == '0' && (next == 'x' || next == 'X')) {
    advance_n(lexer, 2);
    while (isxdigit(*lexer->current)) {
      advance(lexer);
    }
  } else {
    while (isdigit(*lexer->current)) {
      advance(lexer);
    }
  }

  size_t len = (size_t)(lexer->current - start);
  char *num = arena_strndup(lexer->arena, start, len);
  return token_init(TOKEN_NUMBER, .line = start_line, .col = start_col,
                    .value = num);
}

lexer_t lexer_init(const char *src, arena_t *arena) {
  lexer_t lexer = {
      .src = src,
      .current = src,
      .line = 1,
      .col = 1,
      .arena = arena,
  };

  return lexer;
}

token_t lexer_next_token(lexer_t *lexer) {
  skip_comments(lexer);

  if (at_eof(lexer)) {
    return (token_t){.kind = TOKEN_EOF, .line = lexer->line, .col = lexer->col};
  }

  if (is_id_start(*lexer->current)) {
    return id(lexer);
  }

  if (isdigit(*lexer->current)) {
    return number_literal(lexer);
  }

  switch (*lexer->current) {
  case '(':
    return advance_with(lexer, token_init(TOKEN_LPAREN, .line = lexer->line,
                                          .col = lexer->col));
  case ')':
    return advance_with(lexer, token_init(TOKEN_RPAREN, .line = lexer->line,
                                          .col = lexer->col));
  case '{':
    return advance_with(lexer, token_init(TOKEN_LBRACE, .line = lexer->line,
                                          .col = lexer->col));
  case '}':
    return advance_with(lexer, token_init(TOKEN_RBRACE, .line = lexer->line,
                                          .col = lexer->col));
  case ';':
    return advance_with(lexer, token_init(TOKEN_SEMICOLON, .line = lexer->line,
                                          .col = lexer->col));
  case ':':
    return advance_with(
        lexer, token_init(TOKEN_COLON, .line = lexer->line, .col = lexer->col));
  case ',':
    return advance_with(
        lexer, token_init(TOKEN_COMMA, .line = lexer->line, .col = lexer->col));
  case '"':
    return string_literal(lexer);
  case '+':
    return advance_with(
        lexer, token_init(TOKEN_PLUS, .line = lexer->line, .col = lexer->col));
  case '-':
    return advance_with(
        lexer, token_init(TOKEN_MINUS, .line = lexer->line, .col = lexer->col));
  case '*':
    return advance_with(
        lexer, token_init(TOKEN_STAR, .line = lexer->line, .col = lexer->col));
  case '/':
    return advance_with(
        lexer, token_init(TOKEN_SLASH, .line = lexer->line, .col = lexer->col));
  case '%':
    return advance_with(lexer, token_init(TOKEN_PERCENT, .line = lexer->line,
                                          .col = lexer->col));
  case '!':
    if (peek(lexer) == '=') {
      return advance_with(
          lexer,
          advance_with(lexer, token_init(TOKEN_BANG_EQUAL, .line = lexer->line,
                                         lexer->col)));
    }
    return advance_with(
        lexer, token_init(TOKEN_BANG, .line = lexer->line, .col = lexer->col));
  case '=':
    if (peek(lexer) == '=') {
      return advance_with(
          lexer,
          advance_with(lexer, token_init(TOKEN_EQUAL_EQUAL, .line = lexer->line,
                                         lexer->col)));
    }
    return advance_with(
        lexer, token_init(TOKEN_EQUAL, .line = lexer->line, .col = lexer->col));
  case '<':
    switch (peek(lexer)) {
    case '=':
      return advance_with(
          lexer,
          advance_with(lexer, token_init(TOKEN_LESS_EQUAL, .line = lexer->line,
                                         lexer->col)));
    case '<':
      return advance_with(
          lexer,
          advance_with(lexer, token_init(TOKEN_LESS_LESS, .line = lexer->line,
                                         lexer->col)));
    default:
      return advance_with(lexer, token_init(TOKEN_LESS, .line = lexer->line,
                                            .col = lexer->col));
    }
  case '>':
    switch (peek(lexer)) {
    case '=':
      return advance_with(
          lexer,
          advance_with(lexer, token_init(TOKEN_GREATER_EQUAL,
                                         .line = lexer->line, lexer->col)));
    case '>':
      return advance_with(
          lexer,
          advance_with(lexer, token_init(TOKEN_GREATER_GREATER,
                                         .line = lexer->line, lexer->col)));
    default:
      return advance_with(lexer, token_init(TOKEN_GREATER, .line = lexer->line,
                                            .col = lexer->col));
    }
  case '&':
    if (peek(lexer) == '&') {
      return advance_with(
          lexer,
          advance_with(lexer, token_init(TOKEN_AMPERSAND_AMPERSAND,
                                         .line = lexer->line, lexer->col)));
    }
    return advance_with(lexer, token_init(TOKEN_AMPERSAND, .line = lexer->line,
                                          .col = lexer->col));
  case '|':
    if (peek(lexer) == '|') {
      return advance_with(
          lexer,
          advance_with(lexer, token_init(TOKEN_PIPE_PIPE, .line = lexer->line,
                                         lexer->col)));
    }
    return advance_with(
        lexer, token_init(TOKEN_PIPE, .line = lexer->line, .col = lexer->col));
  case '^':
    return advance_with(
        lexer, token_init(TOKEN_CARET, .line = lexer->line, .col = lexer->col));
  default:
    diag_error(NULL, lexer->line, lexer->col, "unexpected '%c'",
               *lexer->current);
    return token_init(TOKEN_EOF);
  }
}
