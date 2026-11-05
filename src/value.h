#ifndef OTTO_VALUE_H
#define OTTO_VALUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "arena.h"

typedef enum {
    VAL_NIL = 0,
    VAL_BOOL,
    VAL_INT,
    VAL_FLOAT,
    VAL_SLICE,   // Zero-copy {const char* ptr, size_t len}
    VAL_LIST,    // Dynamic Value array
    VAL_MAP,     // Key-value map
    VAL_FUNC     // Bytecode offset + arity + slot mask
} ValueType;

struct Value;
struct ValueList;
struct ValueMap;
struct StreamDesc;

typedef struct Slice {
    const char *ptr;
    size_t len;
} Slice;

typedef struct Value {
    ValueType type;
    uint32_t flags;
    union {
        int64_t i64;
        double f64;
        Slice slice;
        struct ValueList *list;
        struct ValueMap *map;
        struct {
            uint32_t ip_offset;
            uint8_t arity;
            uint16_t slot_mask;
        } func;
    } as;
} Value;

typedef struct ValueList {
    Value *items;
    size_t count;
    size_t capacity;
} ValueList;

typedef struct MapEntry {
    Slice key;
    Value val;
    bool occupied;
} MapEntry;

typedef struct ValueMap {
    MapEntry *entries;
    size_t count;
    size_t capacity;
} ValueMap;

typedef struct StreamDesc {
    void *handle;
    const char *source;
    bool eof;
    // Function pointer to fetch next item
    bool (*next)(struct StreamDesc *s, Value *out, Arena *arena);
    void (*close)(struct StreamDesc *s);
} StreamDesc;

// Constructors
static inline Value val_nil(void) {
    Value v; v.type = VAL_NIL; v.flags = 0; v.as.i64 = 0; return v;
}
static inline Value val_bool(bool b) {
    Value v; v.type = VAL_BOOL; v.flags = 0; v.as.i64 = b ? 1 : 0; return v;
}
static inline Value val_int(int64_t i) {
    Value v; v.type = VAL_INT; v.flags = 0; v.as.i64 = i; return v;
}
static inline Value val_float(double f) {
    Value v; v.type = VAL_FLOAT; v.flags = 0; v.as.f64 = f; return v;
}
static inline Value val_slice(const char *ptr, size_t len) {
    Value v; v.type = VAL_SLICE; v.flags = 0; v.as.slice.ptr = ptr; v.as.slice.len = len; return v;
}
static inline Value val_cstr(const char *s) {
    if (!s) return val_nil();
    Value v; v.type = VAL_SLICE; v.flags = 0;
    v.as.slice.ptr = s;
    size_t len = 0;
    while (s[len]) len++;
    v.as.slice.len = len;
    return v;
}

// Slice helpers
bool slice_eq(Slice a, Slice b);
bool slice_eq_str(Slice a, const char *b);
int64_t slice_to_i64(Slice s, bool *ok);
double slice_to_f64(Slice s, bool *ok);
Slice slice_trim(Slice s);
char *slice_to_cstr(Slice s, Arena *arena);

// List operations
Value val_list_create(Arena *arena, size_t capacity);
void val_list_append(Value *list, Value item, Arena *arena);
size_t val_list_len(Value list);
Value val_list_get(Value list, size_t idx);
void val_list_set(Value *list, size_t idx, Value item);

// Map operations
Value val_map_create(Arena *arena, size_t capacity);
void val_map_set(Value *map, Slice key, Value val, Arena *arena);
void val_map_set_str(Value *map, const char *key, Value val, Arena *arena);
bool val_map_get(Value map, Slice key, Value *out_val);
bool val_map_get_str(Value map, const char *key, Value *out_val);

// Generic operations
bool val_is_truthy(Value v);
bool val_equal(Value a, Value b);
int val_compare(Value a, Value b, bool numeric);
int val_compare_ex(Value a, Value b, bool numeric, bool casefold);
char *val_to_string(Value v, Arena *arena);
void val_print(Value v, FILE *out);
void val_println(Value v, FILE *out);

// Clone or promote to arena
Value val_clone_to_arena(Value v, Arena *dst);

#endif // OTTO_VALUE_H
