#ifndef CODEGEN_H
#define CODEGEN_H
#include "arena.h"
#include "ast.h"
#include <stdio.h>
int codegen_emit(FILE *out, unit_t *unit, arena_t *arena);
#endif
