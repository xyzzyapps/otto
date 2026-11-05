#ifndef OTTO_BUNDLE_H
#define OTTO_BUNDLE_H

#include <stdbool.h>
#include <stdint.h>
#include "vm.h"
#include "bc.h"

#define OTTO_MAGIC 0x4F54544F31ULL // "OTTO1" in little-endian
#define SHX_MAGIC  0x53485831ULL   // Legacy "SHX1" in little-endian

// Check if current executable has embedded bytecode footer
bool bundle_has_embedded_bytecode(const char *exe_path, uint8_t **out_bytecode, size_t *out_len);

// Compile script to bytecode and produce a standalone bundled binary
bool bundle_compile_script(const char *script_path, const char *out_binary_path, const char *target, const char *self_exe);

// Deserialize embedded bytecode into VM
bool bundle_load_into_vm(VM *vm, const uint8_t *data, size_t len);

#endif // OTTO_BUNDLE_H
