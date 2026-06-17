#include "arena.h"
#include <stdlib.h>
#include <string.h>

static arena_block_t *block_create(size_t capacity) {
  arena_block_t *block = malloc(sizeof(arena_block_t));
  block->buf = malloc(capacity);
  block->offset = 0;
  block->capacity = capacity;
  block->prev = NULL;
  return block;
}

static void block_destroy(arena_block_t *block) {
  free(block->buf);
  free(block);
}

arena_t arena_create(size_t capacity) {
  arena_t arena;
  arena.current = block_create(capacity);
  return arena;
}

void *arena_alloc(arena_t *arena, size_t size) {
  size_t aligned = (size + sizeof(void *) - 1) & ~(sizeof(void *) - 1);

  arena_block_t *b = arena->current;
  if (b->offset + aligned > b->capacity) {
    size_t cap = b->capacity * 2;
    if (cap < aligned)
      cap = aligned;
    arena_block_t *new_block = block_create(cap);
    new_block->prev = b;
    arena->current = new_block;
    b = new_block;
  }

  void *ptr = b->buf + b->offset;
  memset(ptr, 0, aligned);
  b->offset += aligned;
  return ptr;
}

char *arena_strdup(arena_t *arena, const char *str) {
  size_t len = strlen(str) + 1;
  char *copy = arena_alloc(arena, len);
  memcpy(copy, str, len);
  return copy;
}

char *arena_strndup(arena_t *arena, const char *str, size_t n) {
  char *copy = arena_alloc(arena, n + 1);
  memcpy(copy, str, n);
  copy[n] = '\0';
  return copy;
}

void arena_reset(arena_t *arena) {
  arena_block_t *b = arena->current;
  while (b->prev != NULL) {
    arena_block_t *prev = b->prev;
    block_destroy(b);
    b = prev;
  }
  b->offset = 0;
  arena->current = b;
}

void arena_destroy(arena_t *arena) {
  arena_block_t *block = arena->current;
  while (block != NULL) {
    arena_block_t *prev = block->prev;
    block_destroy(block);
    block = prev;
  }
  arena->current = NULL;
}
