#ifndef UTILS_H
#define UTILS_H
#include "arena.h"
#include "compiler.h"
#include <stddef.h>

void print_escaped(const char *bytes, size_t len);
int read_entire_file(const char *path, arena_t *arena, source_t *out);
bool str_ends_with(const char *str, const char *suffix);
#endif
