#ifndef DEBUG_H
#define DEBUG_H
#include <stdio.h>

#define NOT_IMPLEMENTED                                                        \
  do {                                                                         \
    fprintf(stderr, "Not implemented: %s(%s:%d)\n", __func__, __FILE__,        \
            __LINE__);                                                         \
  } while (0);

void _dd(const char *file, int line, const char *format, ...);

#define dd(format, ...)                                                        \
  _dd(__FILE__, __LINE__, (format)__VA_OPT__(, ) __VA_ARGS__)

#endif
