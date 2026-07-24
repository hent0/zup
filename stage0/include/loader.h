#ifndef LOADER_H
#define LOADER_H

#include "ast.h"
typedef struct module module_t;
struct module {
  char *path;
  unit_t *unit;
  const char *prefix;
  bool loading;
  module_t *next;
};

typedef struct {
  module_t *entry;
  module_t *modules;
} compilation_t;

compilation_t *load_modules(const char *path, arena_t *arena);

#endif
