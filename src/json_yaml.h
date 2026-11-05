#ifndef OTTO_JSON_YAML_H
#define OTTO_JSON_YAML_H

#include "value.h"
#include "arena.h"

// Parse JSON string into Value (VAL_MAP, VAL_LIST, or scalar)
Value json_parse(const char *json_str, size_t len, Arena *arena, bool *ok);

// Serialize Value to JSON string
char *json_stringify(Value val, Arena *arena);

// Parse YAML string into Value
Value yaml_parse(const char *yaml_str, size_t len, Arena *arena, bool *ok);

// Parse CSV string into VAL_LIST of VAL_MAP (header-keyed rows)
Value csv_parse(const char *csv_str, size_t len, Arena *arena);

#endif // OTTO_JSON_YAML_H
