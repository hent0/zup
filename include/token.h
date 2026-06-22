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
  TOKEN_EQUAL,
  TOKEN_EQUAL_EQUAL,
  TOKEN_BANG,
  TOKEN_BANG_EQUAL,
  TOKEN_LESS,
  TOKEN_LESS_LESS,
  TOKEN_LESS_EQUAL,
  TOKEN_GREATER,
  TOKEN_GREATER_GREATER,
  TOKEN_GREATER_EQUAL,
  TOKEN_IF,
  TOKEN_ELSE,
  TOKEN_CONST,
  TOKEN_LET,
  TOKEN_WHILE,
  TOKEN_FOR,
  TOKEN_BREAK,
  TOKEN_CONTINUE,
  TOKEN_IN,
  TOKEN_DOT,
  TOKEN_DOT_DOT,
  TOKEN_DOT_DOT_DOT,
  TOKEN_AMPERSAND,
  TOKEN_AMPERSAND_AMPERSAND,
  TOKEN_PIPE,
  TOKEN_PIPE_PIPE,
  TOKEN_CARET,
  TOKEN_EXTERN,
  // Types
  TOKEN_VOID,
  TOKEN_I8,
  TOKEN_U8,
  TOKEN_I16,
  TOKEN_U16,
  TOKEN_I32,
  TOKEN_U32,
  TOKEN_I64,
  TOKEN_U64,
  TOKEN_STRING,
  TOKEN_BOOL,
  TOKEN_TRUE,
  TOKEN_FALSE,
  TOKEN_CSTR,
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
