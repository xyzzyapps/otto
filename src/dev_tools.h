#ifndef OTTO_DEV_TOOLS_H
#define OTTO_DEV_TOOLS_H

#include "value.h"
#include "arena.h"
#include "lexer.h"

// A1: Fast Multi-File Code Search (replaces ripgrep / rg)
Value dev_search(const char *pattern, const char *root_dir, uint16_t adverbs, Arena *arena);

// A2: Rich File Metadata Objects & File Slice Properties (replaces eza -l & fd)
Value dev_stat_path(const char *path, uint16_t adverbs, Arena *arena);
Value dev_stat_prop(Slice path_slice, Slice prop_slice, Arena *arena, bool *handled);

// A3: Syntax-Highlighted Pager (replaces bat / less)
Value dev_view(Value input, Arena *arena);

// A4: Native Git Scanners (status, commits, branches, diff)
Value dev_git_scan(OttoTokenType scanner, const char *arg, Arena *arena);

// A5: Hashing, Encoding/Decoding & UUID v4
Value dev_hash(Value input, const char *algo, Arena *arena);
Value dev_encode(Value input, const char *fmt, bool decode, Arena *arena);
Value dev_uuid(Arena *arena);

// A6: Port Scanner & Disk Usage Analyzer (replaces lsof/procs & dust/duf)
Value dev_scan_ports(Arena *arena);
Value dev_disk_usage(const char *path, Arena *arena);

#endif // OTTO_DEV_TOOLS_H
