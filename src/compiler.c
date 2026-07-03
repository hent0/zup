#include "compiler.h"
#include "arena.h"
#include "ast.h"
#include "codegen.h"
#include "diag.h"
#include "loader.h"
#include "token.h"
#include "utils.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *open_output(const char *path) {
  if (path == NULL || strcmp(path, "-") == 0) {
    return stdout;
  }

  FILE *fd = fopen(path, "w");
  if (fd == NULL) {
    diag_error_nofile("'%s' %s", path, strerror(errno));
    return NULL;
  }
  return fd;
}

static void close_output(FILE *fd) {
  if (fd != stdout) {
    fclose(fd);
  }
}

static const char *basename_stem(const char *path, arena_t *arena) {
  if (strcmp(path, "-") == 0) {
    return "out";
  }

  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  const char *dot = strrchr(base, '.');
  size_t len = (dot && dot != base) ? (size_t)(dot - base) : strlen(base);

  char *out = arena_alloc(arena, len + 1);
  memcpy(out, base, len);
  out[len] = '\0';
  return out;
}

static const char *with_ll_ext(const char *path, arena_t *arena) {
  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  const char *dot = strrchr(base, '.');
  size_t stem = (dot && dot != base) ? (size_t)(dot - path) : strlen(path);

  char *out = arena_alloc(arena, stem + 4);
  memcpy(out, path, stem);
  memcpy(out + stem, ".ll", 4);

  if (strcmp(out, path) == 0) {
    out = arena_alloc(arena, strlen(path) + 4);
    memcpy(out, path, strlen(path));
    memcpy(out + strlen(path), ".ll", 4);
  }
  return out;
}

static const char *generate_ir(compilation_t *compilation, arena_t *arena,
                               const char *output) {
  FILE *fd = open_output(output);
  if (fd == NULL) {
    return NULL;
  }

  if (codegen_emit(fd, compilation, arena) != 0) {
    close_output(fd);
    return NULL;
  }

  close_output(fd);
  return output;
}

int compiler_compile(compiler_t *compiler, const options_t *opts) {
  if (opts->mode == MODE_TOKENIZE) {
    source_t src;
    if (read_entire_file(opts->input, &compiler->arena, &src) != 0) {
      diag_error_nofile("cannot read '%s'", opts->input);
      return 1;
    }
    return tokens_dump(&src, &compiler->arena);
  }

  compilation_t *compilation = load_modules(opts->input, &compiler->arena);
  if (compilation == NULL) {
    return 1;
  }

  switch (opts->mode) {
  case MODE_AST:
    return ast_dump(compilation->entry->unit);
  case MODE_IR: {
    const char *output =
        opts->output ? opts->output
                     : with_ll_ext(basename_stem(opts->input, &compiler->arena),
                                   &compiler->arena);
    if (generate_ir(compilation, &compiler->arena, output) == NULL) {
      return 1;
    }
    return 0;
  }
  case MODE_COMPILE: {
    const char *output = opts->output
                             ? opts->output
                             : basename_stem(opts->input, &compiler->arena);
    const char *ir = with_ll_ext(output, &compiler->arena);

    if (generate_ir(compilation, &compiler->arena, ir) == NULL) {
      return 1;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "clang -Qunused-arguments -Wno-override-module %s%s -o %s",
             opts->compile_static ? "-static " : "", ir, output);

    if (system(cmd) != 0) {
      diag_error_nofile("failed to link '%s'", output);
      return 1;
    }

    if (!opts->keep_ir) {
      remove(ir);
    }
    return 0;
  }
  default:
    return 1;
  }
}
