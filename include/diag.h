#ifndef DIAG_H
#define DIAG_H

#include "compiler.h"

// Reports an error tied to a source location. Emits:
//   "zup: error: <formatted msg> [line:col]\n"
// to stderr. `src` is accepted for future enrichment (caret/line snippets);
// it is currently unused.
void diag_error(const source_t *src, unsigned int line, unsigned int col,
                const char *fmt, ...);

// Reports an error with no source location (e.g. file open/read failures).
// Emits "zup: error: <formatted msg>\n" to stderr.
void diag_error_nofile(const char *fmt, ...);

#endif
