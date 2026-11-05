#ifndef OTTO_SCANNER_H
#define OTTO_SCANNER_H

#include "value.h"
#include "arena.h"
#include "lexer.h"

// Initialize scanner stream for `every <scanner> <source>:`
StreamDesc *scanner_open_stream(OttoTokenType scanner, const char *source, uint16_t adverbs, Arena *arena);

// Scan tokens from a string or slice directly
Value scanner_extract_all(OttoTokenType scanner, Value input, Arena *arena);

// Glob matching helper
bool scanner_glob_match(const char *pattern, const char *string);

#endif // OTTO_SCANNER_H
