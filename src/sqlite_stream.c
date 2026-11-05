#include "sqlite_stream.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../vendor/sqlite3/sqlite3.h"

bool sqlite_is_db_target(const char *target, char *db_path_out, size_t db_len, char *table_out, size_t table_len) {
    if (!target) return false;
    const char *colon = strchr(target, ':');
    if (!colon) return false;
    // Must look like a db filename before colon (e.g. .db, .sqlite, or has a dot)
    const char *dot = strchr(target, '.');
    if (!dot || dot > colon) return false;

    size_t dlen = (size_t)(colon - target);
    size_t tlen = strlen(colon + 1);
    if (dlen >= db_len || tlen >= table_len || tlen == 0) return false;

    memcpy(db_path_out, target, dlen);
    db_path_out[dlen] = '\0';
    memcpy(table_out, colon + 1, tlen);
    table_out[tlen] = '\0';
    return true;
}

bool sqlite_write_row(const char *target, Value val, bool append) {
    char db_path[256];
    char table[128];
    if (!sqlite_is_db_target(target, db_path, sizeof(db_path), table, sizeof(table))) {
        return false;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    if (val.type != VAL_MAP || !val.as.map) {
        sqlite3_close(db);
        return false;
    }

    ValueMap *m = val.as.map;

    // 1. Auto-create table: CREATE TABLE IF NOT EXISTS table (col1, col2, ...)
    char create_sql[1024];
    snprintf(create_sql, sizeof(create_sql), "CREATE TABLE IF NOT EXISTS %s (", table);
    bool first = true;
    for (size_t i = 0; i < m->capacity; i++) {
        if (m->entries[i].occupied) {
            if (!first) strncat(create_sql, ", ", sizeof(create_sql) - strlen(create_sql) - 1);
            first = false;
            char col_name[64];
            size_t klen = m->entries[i].key.len < 63 ? m->entries[i].key.len : 63;
            memcpy(col_name, m->entries[i].key.ptr, klen);
            col_name[klen] = '\0';
            strncat(create_sql, col_name, sizeof(create_sql) - strlen(create_sql) - 1);
        }
    }
    strncat(create_sql, ");", sizeof(create_sql) - strlen(create_sql) - 1);
    sqlite3_exec(db, create_sql, NULL, NULL, NULL);

    if (!append) {
        char clear_sql[256];
        snprintf(clear_sql, sizeof(clear_sql), "DELETE FROM %s;", table);
        sqlite3_exec(db, clear_sql, NULL, NULL, NULL);
    }

    // 2. Insert row: INSERT INTO table (col1, col2) VALUES (?, ?)
    char insert_sql[1024];
    char values_sql[512] = "";
    snprintf(insert_sql, sizeof(insert_sql), "INSERT INTO %s (", table);
    first = true;
    for (size_t i = 0; i < m->capacity; i++) {
        if (m->entries[i].occupied) {
            if (!first) {
                strncat(insert_sql, ", ", sizeof(insert_sql) - strlen(insert_sql) - 1);
                strncat(values_sql, ", ", sizeof(values_sql) - strlen(values_sql) - 1);
            }
            first = false;
            char col_name[64];
            size_t klen = m->entries[i].key.len < 63 ? m->entries[i].key.len : 63;
            memcpy(col_name, m->entries[i].key.ptr, klen);
            col_name[klen] = '\0';
            strncat(insert_sql, col_name, sizeof(insert_sql) - strlen(insert_sql) - 1);
            strncat(values_sql, "?", sizeof(values_sql) - strlen(values_sql) - 1);
        }
    }
    strncat(insert_sql, ") VALUES (", sizeof(insert_sql) - strlen(insert_sql) - 1);
    strncat(insert_sql, values_sql, sizeof(insert_sql) - strlen(insert_sql) - 1);
    strncat(insert_sql, ");", sizeof(insert_sql) - strlen(insert_sql) - 1);

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }

    int bind_idx = 1;
    for (size_t i = 0; i < m->capacity; i++) {
        if (m->entries[i].occupied) {
            Value v = m->entries[i].val;
            switch (v.type) {
                case VAL_INT:
                    sqlite3_bind_int64(stmt, bind_idx, v.as.i64);
                    break;
                case VAL_FLOAT:
                    sqlite3_bind_double(stmt, bind_idx, v.as.f64);
                    break;
                case VAL_BOOL:
                    sqlite3_bind_int(stmt, bind_idx, v.as.i64 ? 1 : 0);
                    break;
                case VAL_SLICE:
                    sqlite3_bind_text(stmt, bind_idx, v.as.slice.ptr, (int)v.as.slice.len, SQLITE_TRANSIENT);
                    break;
                default:
                    sqlite3_bind_null(stmt, bind_idx);
                    break;
            }
            bind_idx++;
        }
    }

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return true;
}

typedef struct SqliteStreamState {
    sqlite3 *db;
    sqlite3_stmt *stmt;
} SqliteStreamState;

static bool sqlite_stream_next(StreamDesc *s, Value *out, Arena *arena) {
    SqliteStreamState *state = (SqliteStreamState *)s->handle;
    if (!state || !state->stmt) return false;

    int rc = sqlite3_step(state->stmt);
    if (rc == SQLITE_ROW) {
        int col_count = sqlite3_column_count(state->stmt);
        Value row_map = val_map_create(arena, col_count > 0 ? col_count : 4);
        for (int i = 0; i < col_count; i++) {
            const char *col_name = sqlite3_column_name(state->stmt, i);
            int col_type = sqlite3_column_type(state->stmt, i);
            Value val = val_nil();
            switch (col_type) {
                case SQLITE_INTEGER:
                    val = val_int(sqlite3_column_int64(state->stmt, i));
                    break;
                case SQLITE_FLOAT:
                    val = val_float(sqlite3_column_double(state->stmt, i));
                    break;
                case SQLITE_TEXT: {
                    const char *text = (const char *)sqlite3_column_text(state->stmt, i);
                    int text_len = sqlite3_column_bytes(state->stmt, i);
                    char *copied = arena_strndup(arena, text, text_len);
                    val = val_slice(copied, text_len);
                    break;
                }
                case SQLITE_NULL:
                default:
                    val = val_nil();
                    break;
            }
            val_map_set_str(&row_map, col_name, val, arena);
        }
        *out = row_map;
        return true;
    }

    s->eof = true;
    return false;
}

static void sqlite_stream_close(StreamDesc *s) {
    SqliteStreamState *state = (SqliteStreamState *)s->handle;
    if (state) {
        if (state->stmt) sqlite3_finalize(state->stmt);
        if (state->db) sqlite3_close(state->db);
        state->stmt = NULL;
        state->db = NULL;
    }
}

StreamDesc *sqlite_open_stream(const char *target, const char *where_clause, Arena *arena) {
    char db_path[256];
    char table[128];
    if (!sqlite_is_db_target(target, db_path, sizeof(db_path), table, sizeof(table))) {
        return NULL;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }

    char query[1024];
    if (where_clause && where_clause[0]) {
        snprintf(query, sizeof(query), "SELECT * FROM %s WHERE %s;", table, where_clause);
    } else {
        snprintf(query, sizeof(query), "SELECT * FROM %s;", table);
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }

    SqliteStreamState *state = (SqliteStreamState *)arena_alloc_zero(arena, sizeof(SqliteStreamState));
    state->db = db;
    state->stmt = stmt;

    StreamDesc *sd = (StreamDesc *)arena_alloc_zero(arena, sizeof(StreamDesc));
    sd->handle = state;
    sd->next = sqlite_stream_next;
    sd->close = sqlite_stream_close;
    sd->eof = false;
    sd->source = target;
    return sd;
}

Value sqlite_query_rows(const char *target, const char *where_clause, Arena *arena) {
    Value list = val_list_create(arena, 16);
    StreamDesc *sd = sqlite_open_stream(target, where_clause, arena);
    if (!sd) return list;

    Value row;
    while (sd->next(sd, &row, arena)) {
        val_list_append(&list, row, arena);
    }
    if (sd->close) sd->close(sd);
    return list;
}
