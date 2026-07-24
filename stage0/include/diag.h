#ifndef DIAG_H
#define DIAG_H

#include "compiler.h"

void diag_error(const source_t *src, unsigned int line, unsigned int col,
                const char *fmt, ...);

void diag_error_nofile(const char *fmt, ...);

#endif
