#ifndef AST_H
#define AST_H

#include "arena.h"
#include "compiler.h"
#include "token.h"
#include <stddef.h>

typedef enum {
  TYPE_UNKNOWN,
  TYPE_VOID,
  TYPE_I8,
  TYPE_I16,
  TYPE_I32,
  TYPE_I64,
  TYPE_STRING,
} TypeKind;

typedef struct {
  TypeKind kind;
} type_t;

typedef enum {
  EXPR_NUMBER,
  EXPR_STRING,
  EXPR_ID,
  EXPR_CALL,
  EXPR_CAST,
} ExprKind;

typedef struct expr expr_t;
struct expr {
  ExprKind kind;
  unsigned int line;
  unsigned int col;
  type_t type;
  expr_t *next;
  union {
    struct {
      char *value;
    } number;
    struct {
      char *value;
      size_t length;
    } string;
    struct {
      char *name;
    } id;
    struct {
      expr_t *callee;
      expr_t *args;
      size_t arg_count;
    } call;
    struct {
      expr_t *operand;
      type_t target;
    } cast;
  };
};

typedef enum {
  STMT_RETURN,
  STMT_EXPR,
} StmtKind;

typedef struct stmt stmt_t;
struct stmt {
  StmtKind kind;
  unsigned int line;
  unsigned int col;
  stmt_t *next;
  union {
    struct {
      expr_t *value;
    } ret;
    struct {
      expr_t *expr;
    } expr_stmt;
  };
};

typedef struct param param_t;
struct param {
  char *name;
  type_t type;
  param_t *next;
};

typedef enum {
  VISIBILITY_PRIVATE,
  VISIBILITY_PUBLIC,
} Visibility;

typedef enum {
  DECL_FN,
  DECL_CONTAINER,
  // DECL_CONST,
  // DECL_IMPORT,
  // DECL_ENUM,
} DeclKind;

typedef struct decl decl_t;
struct decl {
  DeclKind kind;
  Visibility visibility;
  char *name;
  unsigned int line;
  unsigned int col;
  decl_t *next;
  union {
    struct {
      param_t *params;
      size_t params_count;
      type_t return_type;
      stmt_t *body;
      size_t stmt_count;
    } fn;
    struct {
      decl_t *members;
      size_t member_count;
    } container;
  };
};

typedef struct {
  source_t *src;
  decl_t *root;
} unit_t;

char *type_kind_to_ir(TypeKind kind);
char *type_kind_to_str(TypeKind kind);

expr_t *ast_number_init(token_t token, arena_t *arena);
expr_t *ast_string_init(token_t token, arena_t *arena);
expr_t *ast_id_init(token_t token, arena_t *arena);
expr_t *ast_call_init(expr_t *callee, arena_t *arena);
expr_t *ast_cast_init(expr_t *operand, type_t target, arena_t *arena);
stmt_t *ast_return_init(token_t token, expr_t *value, arena_t *arena);
stmt_t *ast_expr_stmt_init(token_t token, expr_t *value, arena_t *arena);
param_t *ast_param_init(arena_t *arena);
decl_t *ast_fn_init(arena_t *arena);
decl_t *ast_container_init(char *name, arena_t *arena);
unit_t *ast_unit_init(source_t *src, arena_t *arena);

int ast_dump(unit_t *unit);

#endif
