#ifndef OTTO_POSIX_IO_H
#define OTTO_POSIX_IO_H

#include <stdbool.h>
#include <stdint.h>
#include "value.h"
#include "arena.h"

// Run external command through $PATH with ACC piped to stdin, stdout captured into out_acc
bool posix_run_command(const char *cmd_line, Value stdin_val, Value *out_acc, Value *out_err, Arena *arena);
bool posix_spawn_background(const char *cmd_line, int *out_pid, void **out_handle);

// Incremental build check: returns true if target is stale or missing compared to src
bool posix_build_needs_rebuild(const char *target, const char *src);

// Relational Set Verbs
Value posix_set_exclude(Value blacklist, Value candidates, Arena *arena);
Value posix_set_intersect(Value a, Value b, Arena *arena);
Value posix_set_zip(Value a, Value b, Arena *arena);

// Binary Unpacking
bool posix_unpack_bytes(Value byte_slice, const char *fmt, Value *out_values, size_t max_values, size_t *out_count);

#endif // OTTO_POSIX_IO_H
