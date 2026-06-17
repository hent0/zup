#include "compiler.h"
#include "arena.h"
#include "ast.h"
#include "codegen.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "token.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_entire_file(compiler_t *compiler, const char *path,
                            source_t *out) {
  FILE *fd = fopen(path, "r");
  if (fd == NULL) {
    diag_error_nofile("%s: %s", path, strerror(errno));
    return -1;
  }

  fseek(fd, 0, SEEK_END);
  size_t len = (size_t)ftell(fd);
  fseek(fd, 0, SEEK_SET);
  if (len == 0) {
    diag_error_nofile("cannot read '%s'", path);
    return -1;
  }

  char *buffer = arena_alloc(&compiler->arena, len + 1);
  if (fread(buffer, 1, len, fd) != len) {
    diag_error_nofile("cannot read '%s'", path);
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

static unit_t *generate_ast(source_t *src, arena_t *arena) {
  lexer_t lexer = lexer_init(src->src, arena);
  parser_t parser = parser_init(&lexer, src, arena);
  unit_t *unit = parser_parse(&parser);

  if (!semantic_check(unit, arena)) {
    return NULL;
  }

  return unit;
}

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

// Bare basename of `path` with its directory and extension stripped:
// "examples/001.zup" -> "001". A leading dot is kept (dotfiles have no
// extension). Used to derive a default output name from the input file.
static const char *basename_stem(const char *path, arena_t *arena) {
  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  const char *dot = strrchr(base, '.');
  size_t len = (dot && dot != base) ? (size_t)(dot - base) : strlen(base);

  char *out = arena_alloc(arena, len + 1);
  memcpy(out, base, len);
  out[len] = '\0';
  return out;
}

// `path` with its extension replaced by ".ll" (appended if there is none),
// keeping any directory component: "foo" -> "foo.ll", "foo.out" -> "foo.ll",
// "build/app" -> "build/app.ll". Guards the degenerate "foo.ll" -> "foo.ll"
// case by appending instead, so the IR path can never equal its input.
static const char *with_ll_ext(const char *path, arena_t *arena) {
  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  const char *dot = strrchr(base, '.');
  size_t stem = (dot && dot != base) ? (size_t)(dot - path) : strlen(path);

  char *out = arena_alloc(arena, stem + 4); // ".ll" + NUL
  memcpy(out, path, stem);
  memcpy(out + stem, ".ll", 4);

  if (strcmp(out, path) == 0) {
    out = arena_alloc(arena, strlen(path) + 4);
    memcpy(out, path, strlen(path));
    memcpy(out + strlen(path), ".ll", 4);
  }
  return out;
}

static const char *generate_ir(source_t *src, arena_t *arena,
                               const char *output) {
  unit_t *unit = generate_ast(src, arena);
  if (unit == NULL) {
    return NULL;
  }

  FILE *fd = open_output(output);
  if (fd == NULL) {
    return NULL;
  }

  if (codegen_emit(fd, unit, arena) != 0) {
    close_output(fd);
    return NULL;
  }

  close_output(fd);
  return output;
}

int compiler_compile(compiler_t *compiler, const options_t *opts) {
  source_t src;
  if (read_entire_file(compiler, opts->input, &src) != 0) {
    return 1;
  }

  unit_t *unit;
  switch (opts->mode) {
  case MODE_TOKENIZE:
    return tokens_dump(&src, &compiler->arena);
  case MODE_AST: {
    unit = generate_ast(&src, &compiler->arena);
    if (unit == NULL) {
      return 1;
    }
    return ast_dump(unit);
  }
  case MODE_IR: {
    // In IR mode the output IS the final artifact, so -o names it directly;
    // otherwise default to "<input-stem>.ll" next to the cwd.
    const char *output =
        opts->output ? opts->output
                     : with_ll_ext(basename_stem(opts->input, &compiler->arena),
                                   &compiler->arena);
    if (generate_ir(&src, &compiler->arena, output) == NULL) {
      return 1;
    }
    return 0;
  }
  case MODE_COMPILE: {
    const char *output = opts->output
                             ? opts->output
                             : basename_stem(opts->input, &compiler->arena);
    const char *ir = with_ll_ext(output, &compiler->arena);

    if (generate_ir(&src, &compiler->arena, ir) == NULL) {
      return 1;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "clang -Qunused-arguments -Wno-override-module %s -o %s", ir,
             output);

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
