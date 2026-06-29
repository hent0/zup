#ifndef CODEGEN_H
#define CODEGEN_H
#include "arena.h"
#include "ast.h"
#include "loader.h"
#include <stdio.h>
int codegen_emit(FILE *out, compilation_t *compilation, arena_t *arena);
#endif
