#ifndef OTTO_SQLITE_STREAM_H
#define OTTO_SQLITE_STREAM_H

#include "value.h"
#include "arena.h"

// Check if string matches "path.db:table" pattern
bool sqlite_is_db_target(const char *target, char *db_path_out, size_t db_len, char *table_out, size_t table_len);

// Write or append a map Value onto a SQLite table (auto-creates table if needed)
bool sqlite_write_row(const char *target, Value val, bool append);

// Query rows from "file.db:table" with optional where clause, returning a list of VAL_MAP
Value sqlite_query_rows(const char *target, const char *where_clause, Arena *arena);

// Stream iterator for `every row "file.db:table" [where ...]:`
StreamDesc *sqlite_open_stream(const char *target, const char *where_clause, Arena *arena);

#endif // OTTO_SQLITE_STREAM_H
