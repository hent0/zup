#include "utils.h"
#include "arena.h"
#include "compiler.h"
#include "diag.h"
#include <errno.h>
#include <stdio.h>
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

// Reads a file into `out`. Returns -1 without emitting a diagnostic; callers
// report the failure with the context they have (entry file vs. import site).
int read_entire_file(const char *path, arena_t *arena, source_t *out) {
  FILE *fd = fopen(path, "r");
  if (fd == NULL) {
    return -1;
  }

  fseek(fd, 0, SEEK_END);
  size_t len = (size_t)ftell(fd);
  fseek(fd, 0, SEEK_SET);
  if (len == 0) {
    fclose(fd);
    return -1;
  }

  char *buffer = arena_alloc(arena, len + 1);
  if (fread(buffer, 1, len, fd) != len) {
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
