#ifndef TOKEN_H
#define TOKEN_H

#include "arena.h"
#include "compiler.h"
typedef enum {
  TOKEN_EOF,
  TOKEN_ID,
  TOKEN_PUB,
  TOKEN_FN,
  TOKEN_LPAREN,
  TOKEN_RPAREN,
  TOKEN_LBRACE,
  TOKEN_RBRACE,
  TOKEN_RETURN,
  TOKEN_NUMBER,
  TOKEN_COLON,
  TOKEN_SEMICOLON,
  TOKEN_COMMA,
  TOKEN_AS,
  TOKEN_PLUS,
  TOKEN_MINUS,
  TOKEN_STAR,
  TOKEN_SLASH,
  TOKEN_PERCENT,
  // Types
  TOKEN_VOID,
  TOKEN_I8,
  TOKEN_I16,
  TOKEN_I32,
  TOKEN_I64,
  TOKEN_STRING,
  TOKEN_BOOL,
  TOKEN_TRUE,
  TOKEN_FALSE,
} TokenKind;

typedef struct {
  char *name;
  TokenKind kind;
} keyword_t;

typedef struct {
  TokenKind kind;
  unsigned int line;
  unsigned int col;
  char *value;
  size_t length;
} token_t;

typedef struct {
  unsigned int line;
  unsigned int col;
  char *value;
  size_t length;
} token_opt_t;

token_t _token_init(TokenKind kind, token_opt_t opt);

#define token_init(kind, ...) _token_init((kind), (token_opt_t){__VA_ARGS__})

char *token_kind_to_str(TokenKind kind);
int tokens_dump(source_t *src, arena_t *arena);

#endif
