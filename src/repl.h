#ifndef OTTO_REPL_H
#define OTTO_REPL_H

#include "vm.h"
#include "lexer.h"

// Run interactive REPL with live preview and syntax highlighting
void repl_run(VM *vm);

// Syntax highlighting printer
void repl_print_highlighted(const char *input, size_t len);

// Check if a verb is pure (read-only, safe to live-preview)
bool repl_is_pure_verb(OttoTokenType verb);

#endif // OTTO_REPL_H
