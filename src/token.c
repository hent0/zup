#include "token.h"
#include "compiler.h"
#include "lexer.h"
#include "utils.h"
#include <stdio.h>

token_t _token_init(TokenKind kind, token_opt_t opt) {
  return (token_t){
      .kind = kind,
      .line = opt.line,
      .col = opt.col,
      .value = opt.value,
      .length = opt.length,
  };
}

char *token_kind_to_str(TokenKind kind) {
  switch (kind) {
  case TOKEN_EOF:
    return "TOKEN_EOF";
  case TOKEN_ID:
    return "TOKEN_ID";
  case TOKEN_PUB:
    return "TOKEN_PUB";
  case TOKEN_FN:
    return "TOKEN_FN";
  case TOKEN_LPAREN:
    return "TOKEN_LPAREN";
  case TOKEN_RPAREN:
    return "TOKEN_RPAREN";
  case TOKEN_LBRACE:
    return "TOKEN_LBRACE";
  case TOKEN_RBRACE:
    return "TOKEN_RBRACE";
  case TOKEN_LBRACKET:
    return "TOKEN_LBRACKET";
  case TOKEN_RBRACKET:
    return "TOKEN_RBRACKET";
  case TOKEN_RETURN:
    return "TOKEN_RETURN";
  case TOKEN_NUMBER:
    return "TOKEN_NUMBER";
  case TOKEN_COLON:
    return "TOKEN_COLON";
  case TOKEN_SEMICOLON:
    return "TOKEN_SEMICOLON";
  case TOKEN_COMMA:
    return "TOKEN_COMMA";
  case TOKEN_AS:
    return "TOKEN_AS";
  case TOKEN_PLUS:
    return "TOKEN_PLUS";
  case TOKEN_MINUS:
    return "TOKEN_MINUS";
  case TOKEN_STAR:
    return "TOKEN_STAR";
  case TOKEN_SLASH:
    return "TOKEN_SLASH";
  case TOKEN_PERCENT:
    return "TOKEN_PERCENT";
  case TOKEN_EQUAL:
    return "TOKEN_EQUAL";
  case TOKEN_EQUAL_EQUAL:
    return "TOKEN_EQUAL_EQUAL";
  case TOKEN_FAT_ARROW:
    return "TOKEN_FAT_ARROW";
  case TOKEN_QUESTION:
    return "TOKEN_QUESTION";
  case TOKEN_QUESTION_QUESTION:
    return "TOKEN_QUESTION_QUESTION";
  case TOKEN_QUESTION_QUESTION_EQUAL:
    return "TOKEN_QUESTION_QUESTION_EQUAL";
  case TOKEN_PLUS_EQUAL:
    return "TOKEN_PLUS_EQUAL";
  case TOKEN_MINUS_EQUAL:
    return "TOKEN_MINUS_EQUAL";
  case TOKEN_STAR_EQUAL:
    return "TOKEN_STAR_EQUAL";
  case TOKEN_SLASH_EQUAL:
    return "TOKEN_SLASH_EQUAL";
  case TOKEN_PERCENT_EQUAL:
    return "TOKEN_PERCENT_EQUAL";
  case TOKEN_AMPERSAND_EQUAL:
    return "TOKEN_AMPERSAND_EQUAL";
  case TOKEN_PIPE_EQUAL:
    return "TOKEN_PIPE_EQUAL";
  case TOKEN_CARET_EQUAL:
    return "TOKEN_CARET_EQUAL";
  case TOKEN_LESS_LESS_EQUAL:
    return "TOKEN_LESS_LESS_EQUAL";
  case TOKEN_GREATER_GREATER_EQUAL:
    return "TOKEN_GREATER_GREATER_EQUAL";
  case TOKEN_BANG:
    return "TOKEN_BANG";
  case TOKEN_BANG_EQUAL:
    return "TOKEN_BANG_EQUAL";
  case TOKEN_LESS:
    return "TOKEN_LESS";
  case TOKEN_LESS_LESS:
    return "TOKEN_LESS_LESS";
  case TOKEN_LESS_EQUAL:
    return "TOKEN_LESS_EQUAL";
  case TOKEN_GREATER:
    return "TOKEN_GREATER";
  case TOKEN_GREATER_GREATER:
    return "TOKEN_GREATER_GREATER";
  case TOKEN_GREATER_EQUAL:
    return "TOKEN_GREATER_EQUAL";
  case TOKEN_UNDERSCORE:
    return "TOKEN_UNDERSCORE";
  case TOKEN_IF:
    return "TOKEN_IF";
  case TOKEN_ELSE:
    return "TOKEN_ELSE";
  case TOKEN_CONST:
    return "TOKEN_CONST";
  case TOKEN_LET:
    return "TOKEN_LET";
  case TOKEN_WHILE:
    return "TOKEN_WHILE";
  case TOKEN_FOR:
    return "TOKEN_FOR";
  case TOKEN_BREAK:
    return "TOKEN_BREAK";
  case TOKEN_CONTINUE:
    return "TOKEN_CONTINUE";
  case TOKEN_IN:
    return "TOKEN_IN";
  case TOKEN_DOT:
    return "TOKEN_DOT";
  case TOKEN_DOT_DOT:
    return "TOKEN_DOT_DOT";
  case TOKEN_DOT_DOT_DOT:
    return "TOKEN_DOT_DOT_DOT";
  case TOKEN_AMPERSAND:
    return "TOKEN_AMPERSAND";
  case TOKEN_AMPERSAND_AMPERSAND:
    return "TOKEN_AMPERSAND_AMPERSAND";
  case TOKEN_PIPE:
    return "TOKEN_PIPE";
  case TOKEN_PIPE_PIPE:
    return "TOKEN_PIPE_PIPE";
  case TOKEN_CARET:
    return "TOKEN_CARET";
  case TOKEN_EXTERN:
    return "TOKEN_EXTERN";
  case TOKEN_IMPORT:
    return "TOKEN_IMPORT";
  case TOKEN_ENUM:
    return "TOKEN_ENUM";
  case TOKEN_MATCH:
    return "TOKEN_MATCH";
  case TOKEN_NULL:
    return "TOKEN_NULL";
  case TOKEN_DEFER:
    return "TOKEN_DEFER";
  // Types
  case TOKEN_VOID:
    return "TOKEN_VOID";
  case TOKEN_I8:
    return "TOKEN_I8";
  case TOKEN_U8:
    return "TOKEN_U8";
  case TOKEN_I16:
    return "TOKEN_I16";
  case TOKEN_U16:
    return "TOKEN_U16";
  case TOKEN_I32:
    return "TOKEN_I32";
  case TOKEN_U32:
    return "TOKEN_U32";
  case TOKEN_I64:
    return "TOKEN_I64";
  case TOKEN_U64:
    return "TOKEN_U64";
  case TOKEN_F32:
    return "TOKEN_F32";
  case TOKEN_F64:
    return "TOKEN_F64";
  case TOKEN_STRING:
    return "TOKEN_STRING";
  case TOKEN_BOOL:
    return "TOKEN_BOOL";
  case TOKEN_TRUE:
    return "TOKEN_TRUE";
  case TOKEN_FALSE:
    return "TOKEN_FALSE";
  case TOKEN_CSTR:
    return "TOKEN_CSTR";
  case TOKEN_STR:
    return "TOKEN_STR";
  case TOKEN_STRUCT:
    return "TOKEN_STRUCT";
  }
  return "UNKNOWN TOKEN TYPE";
}

int tokens_dump(source_t *src, arena_t *arena) {
  lexer_t lexer = lexer_init(src->src, arena);

  for (token_t token = lexer_next_token(&lexer); token.kind != TOKEN_EOF;
       token = lexer_next_token(&lexer)) {
    switch (token.kind) {
    case TOKEN_ID:
      printf("%s('%s') [%d:%d]\n", token_kind_to_str(token.kind), token.value,
             token.line, token.col);
      break;
    case TOKEN_STRING:
      printf("%s('", token_kind_to_str(token.kind));
      print_escaped(token.value, token.length);
      printf("') [%d:%d]\n", token.line, token.col);
      break;
    case TOKEN_NUMBER:
      printf("%s(%s) [%d:%d]\n", token_kind_to_str(token.kind), token.value,
             token.line, token.col);
      break;
    default:
      printf("%s [%d:%d]\n", token_kind_to_str(token.kind), token.line,
             token.col);
      break;
    }
  }

  return 0;
}
