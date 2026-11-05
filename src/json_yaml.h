#ifndef OTTO_JSON_YAML_H
#define OTTO_JSON_YAML_H

#include "value.h"
#include "arena.h"

// Parse JSON string into Value (VAL_MAP, VAL_LIST, or scalar)
Value json_parse(const char *json_str, size_t len, Arena *arena, bool *ok);

// Serialize Value to JSON string
char *json_stringify(Value val, Arena *arena, bool pretty);

// Parse YAML string into Value
Value yaml_parse(const char *yaml_str, size_t len, Arena *arena, bool *ok);

// Serialize Value to YAML string
char *yaml_stringify(Value val, Arena *arena);

#endif // OTTO_JSON_YAML_H
