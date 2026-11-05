#ifndef OTTO_FFI_H
#define OTTO_FFI_H

#include "value.h"
#include "arena.h"

// Register an extern C function
bool ffi_register_extern(const char *lib_name, const char *symbol_name, const char *verb_name);

// Call registered extern function with arguments from ACC / frame
bool ffi_invoke(const char *verb_name, Value arg, Value *out_result, Arena *arena);

#endif // OTTO_FFI_H
