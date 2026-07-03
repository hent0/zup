#include "diag.h"
#include <stdarg.h>
#include <stdio.h>

void diag_error(const source_t *src, unsigned int line, unsigned int col,
                const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  fputs("zup: error: ", stderr);
  vfprintf(stderr, fmt, args);
  fprintf(stderr, " %s [%u:%u]\n", src->path, line, col);
  va_end(args);
}

void diag_error_nofile(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  fputs("zup: error: ", stderr);
  vfprintf(stderr, fmt, args);
  fputc('\n', stderr);
  va_end(args);
}
