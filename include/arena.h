#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

typedef struct ARENA_BLOCK {
  char *buf;
  size_t offset;
  size_t capacity;
  struct ARENA_BLOCK *prev;
} arena_block_t;

typedef struct {
  arena_block_t *current;
} arena_t;

arena_t arena_create(size_t capacity);
void *arena_alloc(arena_t *arena, size_t size);
char *arena_strdup(arena_t *arena, const char *str);
char *arena_strndup(arena_t *arena, const char *str, size_t n);
void arena_reset(arena_t *arena);
void arena_destroy(arena_t *arena);

#endif
