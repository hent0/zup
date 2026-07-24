#ifndef COMPILER_H
#define COMPILER_H

#include "arena.h"
#include <stddef.h>

#define MAX_LINK_ARGS 32

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
  const char *link_libs[MAX_LINK_ARGS];
  size_t link_libs_count;
  const char *link_paths[MAX_LINK_ARGS];
  size_t link_paths_count;
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
