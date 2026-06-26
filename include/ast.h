#ifndef AST_H
#define AST_H

#include "arena.h"
#include "compiler.h"
#include "token.h"
#include <stddef.h>

typedef enum {
  TYPE_UNKNOWN,
  TYPE_VOID,
  TYPE_BOOL,
  TYPE_I8,
  TYPE_U8,
  TYPE_I16,
  TYPE_U16,
  TYPE_I32,
  TYPE_U32,
  TYPE_I64,
  TYPE_U64,
  TYPE_F32,
  TYPE_F64,
  TYPE_CSTR,
  TYPE_STR,
  TYPE_STRUCT,
  TYPE_ARRAY,
  TYPE_SLICE,
} TypeKind;

typedef struct type type_t;
struct type {
  TypeKind kind;
  char *name;
  type_t *element;
  size_t array_length;
  unsigned int line;
  unsigned int col;
};

typedef enum {
  BINOP_ADD,
  BINOP_SUB,
  BINOP_MUL,
  BINOP_DIV,
  BINOP_REM,
  BINOP_EQ,
  BINOP_NE,
  BINOP_LT,
  BINOP_LE,
  BINOP_GT,
  BINOP_GE,
  BINOP_AND,
  BINOP_OR,
  BINOP_BITAND,
  BINOP_BITOR,
  BINOP_BITXOR,
  BINOP_SHL,
  BINOP_SHR,
} BinaryOp;

typedef enum {
  UNOP_NOT,
  UNOP_NEG,
} UnaryOp;

typedef enum {
  EXPR_BOOLEAN,
  EXPR_NUMBER,
  EXPR_STRING,
  EXPR_ID,
  EXPR_CALL,
  EXPR_CAST,
  EXPR_BINARY,
  EXPR_UNARY,
  EXPR_STRUCT_LITERAL,
  EXPR_FIELD,
  EXPR_ARRAY,
  EXPR_INDEX,
} ExprKind;

typedef struct expr expr_t;
typedef struct field_init field_init_t;
struct field_init {
  char *name;
  expr_t *value;
  unsigned int line;
  unsigned int col;
  field_init_t *next;
};

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
      bool value;
    } boolean;
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
    struct {
      BinaryOp op;
      expr_t *lhs;
      expr_t *rhs;
    } binary;
    struct {
      UnaryOp op;
      expr_t *operand;
    } unary;
    struct {
      char *type_name;
      field_init_t *inits;
      size_t init_count;
    } struct_literal;
    struct {
      expr_t *base;
      char *name;
    } field;
    struct {
      expr_t *elements;
      size_t element_count;
    } array;
    struct {
      expr_t *base;
      expr_t *index;
    } index;
  };
};

typedef enum {
  STMT_RETURN,
  STMT_EXPR,
  STMT_IF,
  STMT_BINDING,
  STMT_ASSIGN,
  STMT_WHILE,
  STMT_FOR,
  STMT_BREAK,
  STMT_CONTINUE,
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
    struct {
      expr_t *cond;
      stmt_t *then_body;
      stmt_t *else_body;
    } if_stmt;
    struct {
      char *name;
      type_t type;
      bool mutable;
      expr_t *init;
    } binding;
    struct {
      expr_t *target;
      expr_t *value;
    } assign;
    struct {
      expr_t *cond;
      stmt_t *body;
    } while_loop;
    struct {
      char *var;
      expr_t *start;
      expr_t *end;
      expr_t *iterable;
      stmt_t *body;
    } for_loop;
  };
};

typedef struct param param_t;
struct param {
  char *name;
  type_t type;
  bool mutable;
  bool is_self;
  param_t *next;
};

typedef struct field field_t;
struct field {
  char *name;
  type_t type;
  expr_t *default_value;
  unsigned int line;
  unsigned int col;
  field_t *next;
};

typedef enum {
  VISIBILITY_PRIVATE,
  VISIBILITY_PUBLIC,
} Visibility;

typedef enum {
  DECL_FN,
  DECL_CONTAINER,
  DECL_GLOBAL,
  DECL_STRUCT,
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
      bool is_extern;
      bool variadic;
    } fn;
    struct {
      decl_t *members;
      size_t member_count;
    } container;
    struct {
      type_t type;
      bool mutable;
      expr_t *init;
    } global;
    struct {
      field_t *fields;
      size_t field_count;
      decl_t *members;
      size_t member_count;
    } strct;
  };
};

typedef struct {
  source_t *src;
  decl_t *root;
} unit_t;

char *type_kind_to_ir(TypeKind kind);
char *type_kind_to_str(TypeKind kind);
char *type_to_str(type_t type);
bool type_is_integer(TypeKind kind);
bool type_is_signed_integer(TypeKind kind);
bool type_is_float(TypeKind kind);
bool type_is_numeric(TypeKind kind);

char *binop_to_ir(BinaryOp op, bool is_signed);
char *binop_to_ir_float(BinaryOp op);
char *binop_to_str(BinaryOp op);
bool binop_is_comparison(BinaryOp op);
bool binop_is_logical(BinaryOp op);
bool binop_is_arithmetic(BinaryOp op);

char *unop_to_ir(UnaryOp op);
char *unop_to_str(UnaryOp op);

expr_t *ast_binary_init(BinaryOp op, expr_t *lhs, expr_t *rhs, arena_t *arena);
expr_t *ast_unary_init(UnaryOp op, expr_t *operand, arena_t *arena);
expr_t *ast_boolean_init(bool value, token_t token, arena_t *arena);
expr_t *ast_number_init(token_t token, arena_t *arena);
expr_t *ast_string_init(token_t token, arena_t *arena);
expr_t *ast_id_init(token_t token, arena_t *arena);
expr_t *ast_call_init(expr_t *callee, arena_t *arena);
expr_t *ast_cast_init(expr_t *operand, type_t target, arena_t *arena);
expr_t *ast_struct_literal_init(token_t token, arena_t *arena);
expr_t *ast_field_access_init(expr_t *base, token_t name, arena_t *arena);
expr_t *ast_array_init(token_t token, arena_t *arena);
expr_t *ast_index_init(expr_t *base, expr_t *index, arena_t *arena);

stmt_t *ast_return_init(token_t token, expr_t *value, arena_t *arena);
stmt_t *ast_expr_stmt_init(token_t token, expr_t *value, arena_t *arena);
stmt_t *ast_if_init(token_t token, expr_t *cond, stmt_t *then_body,
                    stmt_t *else_body, arena_t *arena);
stmt_t *ast_assign_init(token_t token, expr_t *target, expr_t *value,
                        arena_t *arena);
stmt_t *ast_binding_init(token_t token, char *name, type_t type, bool mutable,
                         expr_t *init, arena_t *arena);
stmt_t *ast_while_init(token_t token, expr_t *cond, stmt_t *body,
                       arena_t *arena);
stmt_t *ast_for_init(token_t token, char *var, expr_t *start, expr_t *end,
                     stmt_t *body, arena_t *arena);
stmt_t *ast_foreach_init(token_t token, char *var, expr_t *iterable,
                         stmt_t *body, arena_t *arena);
stmt_t *ast_stmt_init(token_t token, StmtKind kind, arena_t *arena);

param_t *ast_param_init(arena_t *arena);

field_t *ast_field_init(arena_t *arena);

decl_t *ast_fn_init(arena_t *arena);
decl_t *ast_container_init(char *name, arena_t *arena);
decl_t *ast_struct_init(char *name, arena_t *arena);
decl_t *ast_global_init(token_t token, Visibility visibility, char *name,
                        type_t type, bool mutable, expr_t *init,
                        arena_t *arena);

unit_t *ast_unit_init(source_t *src, arena_t *arena);

int ast_dump(unit_t *unit);

#endif
