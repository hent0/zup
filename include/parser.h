#ifndef PARSER_H
#define PARSER_H

#include "arena.h"
#include "ast.h"
#include "compiler.h"
#include "lexer.h"

typedef struct {
  lexer_t *lexer;
  source_t *src;
  arena_t *arena;
  token_t current;
  bool had_error;
} parser_t;

parser_t parser_init(lexer_t *lexer, source_t *source, arena_t *arena);
unit_t *parser_parse(parser_t *parser);

#endif
