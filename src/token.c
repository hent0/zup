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
  case TOKEN_VOID:
    return "TOKEN_VOID";
  case TOKEN_I8:
    return "TOKEN_I8";
  case TOKEN_I16:
    return "TOKEN_I16";
  case TOKEN_I32:
    return "TOKEN_I32";
  case TOKEN_I64:
    return "TOKEN_I64";
  case TOKEN_STRING:
    return "TOKEN_STRING";
  case TOKEN_BOOL:
    return "TOKEN_BOOL";
  case TOKEN_TRUE:
    return "TOKEN_TRUE";
  case TOKEN_FALSE:
    return "TOKEN_FALSE";
  default:
    return "UNKNOWN TOKEN TYPE";
  }
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
