#ifndef COMPILER_H
#define COMPILER_H

#include "arena.h"
#include <stddef.h>

typedef enum {
  MODE_COMPILE,
  MODE_TOKENIZE,
  MODE_AST,
  MODE_IR,
} CompileMode;

typedef struct {
  const char *input;
  const char *output;
  CompileMode mode;
  bool keep_ir;
  bool compile_static;
} options_t;

typedef struct {
  const char *path;
  char *src;
  size_t len;
} source_t;

typedef struct {
  arena_t arena;
} compiler_t;

int compiler_compile(compiler_t *compiler, const options_t *opts);
#endif
