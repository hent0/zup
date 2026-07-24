#ifndef SEMANTIC_H
#define SEMANTIC_H
#include "arena.h"
#include "ast.h"

int semantic_check(unit_t *unit, arena_t *arena, bool require_main);

#endif
