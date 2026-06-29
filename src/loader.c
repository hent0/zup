#include "loader.h"
#include "arena.h"
#include "ast.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "semantic.h"
#include "utils.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  arena_t *arena;
  module_t *modules;
  module_t *modules_tail;
} loader_t;

static module_t *cache_lookup(loader_t *loader, const char *path) {
  for (module_t *module = loader->modules; module != NULL;
       module = module->next) {
    if (strcmp(module->path, path) == 0) {
      return module;
    }
  }
  return NULL;
}

static char *dirname(const char *path, arena_t *arena) {
  const char *slash = strrchr(path, '/');
  if (!slash) {
    return arena_format(arena, ".");
  }
  return arena_format(arena, "%.*s", (int)(slash - path), path);
}

// The std library root: relative to the compiler binary, falling back to the
// ZUP_STD environment variable when the executable path can't be determined.
static const char *std_root(arena_t *arena) {
  char exe[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n > 0) {
    exe[n] = '\0';
    return arena_format(arena, "%s/../std", dirname(exe, arena));
  }
  const char *env = getenv("ZUP_STD");
  return env ? env : "std";
}

static char *resolve_module(const char *spec, const char *dir, arena_t *arena) {
  if (str_ends_with(spec, ".zup")) {
    return arena_format(arena, "%s/%s", dir, spec);
  }
  return arena_format(arena, "%s/%s.zup", std_root(arena), spec);
}

static module_t *load_module(loader_t *loader, const char *path, arena_t *arena,
                             const source_t *importer_src,
                             const decl_t *import) {
  module_t *cached = cache_lookup(loader, path);
  if (cached != NULL) {
    return cached;
  }

  source_t src;
  if (read_entire_file(path, arena, &src) != 0) {
    if (import != NULL) {
      diag_error(importer_src, import->line, import->col,
                 "cannot find module '%s'", import->import.path);
    } else {
      diag_error_nofile("cannot read '%s'", path);
    }
    return NULL;
  }

  lexer_t lexer = lexer_init(src.src, arena);
  parser_t parser = parser_init(&lexer, &src, arena);
  unit_t *unit = parser_parse(&parser);
  if (unit == NULL) {
    return NULL;
  }

  module_t *module = arena_alloc(arena, sizeof(module_t));
  module->path = arena_strdup(arena, path);
  module->unit = unit;
  module->prefix = NULL;
  module->loading = true;
  module->next = NULL;

  if (loader->modules_tail == NULL) {
    loader->modules = module;
  } else {
    loader->modules_tail->next = module;
  }
  loader->modules_tail = module;

  char *dir = dirname(path, arena);
  for (decl_t *decl = unit->root->container.members; decl; decl = decl->next) {
    if (decl->kind != DECL_IMPORT) {
      continue;
    }

    char *child = resolve_module(decl->import.path, dir, arena);
    module_t *cached = cache_lookup(loader, child);
    if (cached != NULL && cached->loading) {
      diag_error(unit->src, decl->line, decl->col, "circular import '%s'",
                 decl->import.path);
      return NULL;
    }

    module_t *mod = load_module(loader, child, arena, unit->src, decl);
    if (mod == NULL) {
      return NULL;
    }
    decl->import.resolved = mod;
  }

  if (!semantic_check(unit, arena, import == NULL)) {
    return NULL;
  }

  module->loading = false;
  return module;
}

compilation_t *load_modules(const char *path, arena_t *arena) {
  loader_t loader = {.arena = arena};
  module_t *entry = load_module(&loader, path, arena, NULL, NULL);
  if (entry == NULL) {
    return NULL;
  }
  compilation_t *compilation = arena_alloc(arena, sizeof(compilation_t));
  compilation->entry = entry;
  compilation->modules = loader.modules;
  return compilation;
}
