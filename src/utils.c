#include "utils.h"
#include "arena.h"
#include "compiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void print_escaped(const char *bytes, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char byte = (unsigned char)bytes[i];
    switch (byte) {
    case '\n':
      fputs("\\n", stdout);
      break;
    default:
      if (byte >= 0x20 && byte < 0x7F) {
        putchar(byte);
      } else {
        printf("\\x%02X", byte);
      }
    }
  }
}

int read_stdin(const char *path, arena_t *arena, source_t *out) {
  size_t capacity = 4096;
  size_t len = 0;
  char *tmp = malloc(capacity);
  if (tmp == NULL) {
    return -1;
  }

  size_t n;
  while ((n = fread(tmp + len, 1, capacity - len, stdin)) > 0) {
    len += n;
    if (len == capacity) {
      capacity *= 2;
      char *grown = realloc(tmp, capacity);
      if (grown == NULL) {
        free(tmp);
        return -1;
      }
      tmp = grown;
    }
  }

  char *buffer = arena_alloc(arena, len + 1);
  memcpy(buffer, tmp, len);
  buffer[len] = '\0';
  free(tmp);

  out->path = path;
  out->src = buffer;
  out->len = len;
  return 0;
}

int read_entire_file(const char *path, arena_t *arena, source_t *out) {
  if (strcmp(path, "-") == 0) {
    return read_stdin(path, arena, out);
  }

  FILE *fd = fopen(path, "r");
  if (fd == NULL) {
    return -1;
  }

  fseek(fd, 0, SEEK_END);
  size_t len = (size_t)ftell(fd);
  fseek(fd, 0, SEEK_SET);

  char *buffer = arena_alloc(arena, len + 1);
  if (len > 0 && fread(buffer, 1, len, fd) != len) {
    fclose(fd);
    return -1;
  }

  fclose(fd);
  buffer[len] = '\0';

  out->path = path;
  out->src = buffer;
  out->len = len;
  return 0;
}

bool str_ends_with(const char *str, const char *suffix) {
  size_t str_len = strlen(str);
  size_t suffix_len = strlen(suffix);
  return str_len >= suffix_len &&
         strcmp(str + str_len - suffix_len, suffix) == 0;
}
