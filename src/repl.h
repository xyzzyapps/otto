#ifndef OTTO_REPL_H
#define OTTO_REPL_H

#include "vm.h"
#include "lexer.h"

// Run interactive REPL with ghost autocomplete and syntax highlighting
void repl_run(VM *vm);

// Syntax highlighting printer
void repl_print_highlighted(const char *input, size_t len);

#endif // OTTO_REPL_H
