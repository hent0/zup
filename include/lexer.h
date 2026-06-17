#ifndef LEXER_H
#define LEXER_H

#include "arena.h"
#include "token.h"
typedef struct {
  const char *src;
  const char *current;
  unsigned int line;
  unsigned int col;
  arena_t *arena;
} lexer_t;

lexer_t lexer_init(const char *src, arena_t *arena);
token_t lexer_next_token(lexer_t *lexer);

#endif
