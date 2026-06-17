#include "arena.h"
#include "compiler.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#define VERSION "0.0.1"

enum {
  OPTIONS_TARGET_IR,
  OPTION_TOKENIZE,
  OPTION_AST,
  OPTION_KEEP_IR,
  OPTION_VERSION,
  OPTION_HELP,
};

static const struct option long_options[] = {
    {"ir", no_argument, 0, OPTIONS_TARGET_IR},
    {"tokenize", no_argument, 0, OPTION_TOKENIZE},
    {"ast", no_argument, 0, OPTION_AST},
    {"keep-ir", no_argument, 0, OPTION_KEEP_IR},
    {"version", no_argument, 0, OPTION_VERSION},
    {"help", no_argument, 0, OPTION_HELP},
    {0, 0, 0, 0},
};

static void print_usage(FILE *stream) {
  fprintf(stream,
          "Usage: zup [OPTION...] <file>\n"
          "  -o <file>\n"
          "                     Output path\n"
          "  -tokenize\n"
          "                     Output only tokens\n"
          "  -ir\n"
          "                     Output only LLVM IR\n"
          "  -ast\n"
          "                     Dump ast\n"
          "  -keep-ir\n"
          "                     Doesn't remove generare LLVM IR after compile\n"
          "  -help\n"
          "                     Give this help list\n"
          "  -version\n"
          "                     Print version\n");
}

static int parse_args(int argc, char **argv, options_t *opts, int *exit_code) {
  opts->input = NULL;
  opts->output = NULL;
  opts->mode = MODE_COMPILE;
  opts->keep_ir = false;

  int c;
  while ((c = getopt_long_only(argc, argv, "o:", long_options, NULL)) != -1) {
    switch (c) {
    case 'o':
      opts->output = optarg;
      break;
    case OPTION_TOKENIZE:
      opts->mode = MODE_TOKENIZE;
      break;
    case OPTIONS_TARGET_IR:
      opts->mode = MODE_IR;
      break;
    case OPTION_AST:
      opts->mode = MODE_AST;
      break;
    case OPTION_KEEP_IR:
      opts->keep_ir = true;
      break;
    case OPTION_VERSION:
      fprintf(stdout, "zup %s\n", VERSION);
      *exit_code = EXIT_SUCCESS;
      return 1;
    case OPTION_HELP:
      print_usage(stdout);
      *exit_code = EXIT_SUCCESS;
      return 1;
    case '?':
      print_usage(stderr);
      *exit_code = EXIT_FAILURE;
      return 1;
    }
  }

  if (optind >= argc) {
    fprintf(stderr, "zup: no input file\n");
    print_usage(stderr);
    *exit_code = EXIT_FAILURE;
    return 1;
  }

  opts->input = argv[optind];
  return 0;
}

int main(int argc, char **argv) {
  options_t opts = {0};
  int exit_code;
  if (parse_args(argc, argv, &opts, &exit_code) != 0) {
    return exit_code;
  }

  compiler_t compiler = {.arena = arena_create(1 << 20)};
  int ret = compiler_compile(&compiler, &opts);
  arena_destroy(&compiler.arena);

  return ret;
}
