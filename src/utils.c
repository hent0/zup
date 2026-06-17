#include "utils.h"
#include <stdio.h>

void print_escaped(const char *bytes, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char byte = (unsigned char)bytes[i];
    switch (byte) {
    case '\n':
      fputs("\\n", stdout);
      break;
    default:
      if (byte >= 0x20 && byte < 0x7F) {
        putchar(byte);
      } else {
        printf("\\x%02X", byte);
      }
    }
  }
}
