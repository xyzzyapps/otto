#include "value.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

bool slice_eq(Slice a, Slice b) {
    if (a.len != b.len) return false;
    return memcmp(a.ptr, b.ptr, a.len) == 0;
}

bool slice_eq_str(Slice a, const char *b) {
    if (!b) return a.len == 0;
    size_t blen = strlen(b);
    if (a.len != blen) return false;
    return memcmp(a.ptr, b, a.len) == 0;
}

bool slice_case_eq_str(Slice a, const char *b) {
    if (!b) return a.len == 0;
    size_t blen = strlen(b);
    if (a.len != blen) return false;
    for (size_t i = 0; i < a.len; i++) {
        if (tolower((unsigned char)a.ptr[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

Slice slice_trim(Slice s) {
    while (s.len > 0 && isspace((unsigned char)*s.ptr)) {
        s.ptr++;
        s.len--;
    }
    while (s.len > 0 && isspace((unsigned char)s.ptr[s.len - 1])) {
        s.len--;
    }
    return s;
}

bool slice_starts_with(Slice s, const char *prefix) {
    if (!prefix) return true;
    size_t plen = strlen(prefix);
    if (s.len < plen) return false;
    return memcmp(s.ptr, prefix, plen) == 0;
}

bool slice_ends_with(Slice s, const char *suffix) {
    if (!suffix) return true;
    size_t slen = strlen(suffix);
    if (s.len < slen) return false;
    return memcmp(s.ptr + s.len - slen, suffix, slen) == 0;
}

bool slice_contains(Slice haystack, Slice needle) {
    if (needle.len == 0) return true;
    if (haystack.len < needle.len) return false;
    for (size_t i = 0; i <= haystack.len - needle.len; i++) {
        if (memcmp(haystack.ptr + i, needle.ptr, needle.len) == 0) {
            return true;
        }
    }
    return false;
}

int64_t slice_to_i64(Slice s, bool *ok) {
    s = slice_trim(s);
    if (s.len == 0) {
        if (ok) *ok = false;
        return 0;
    }
    size_t i = 0;
    int sign = 1;
    if (s.ptr[0] == '-') {
        sign = -1;
        i++;
    } else if (s.ptr[0] == '+') {
        i++;
    }
    if (i >= s.len) {
        if (ok) *ok = false;
        return 0;
    }
    int64_t val = 0;
    for (; i < s.len; i++) {
        char c = s.ptr[i];
        if (c < '0' || c > '9') {
            if (ok) *ok = false;
            return 0;
        }
        val = val * 10 + (c - '0');
    }
    if (ok) *ok = true;
    return val * sign;
}

double slice_to_f64(Slice s, bool *ok) {
    s = slice_trim(s);
    if (s.len == 0) {
        if (ok) *ok = false;
        return 0.0;
    }
    char buf[64];
    size_t copy_len = s.len < 63 ? s.len : 63;
    memcpy(buf, s.ptr, copy_len);
    buf[copy_len] = '\0';
    char *end = NULL;
    double val = strtod(buf, &end);
    if (end == buf) {
        if (ok) *ok = false;
        return 0.0;
    }
    if (ok) *ok = true;
    return val;
}

char *slice_to_cstr(Slice s, Arena *arena) {
    return arena_strndup(arena, s.ptr, s.len);
}

// List operations
Value val_list_create(Arena *arena, size_t capacity) {
    if (capacity < 4) capacity = 4;
    ValueList *list = (ValueList *)arena_alloc(arena, sizeof(ValueList));
    list->items = (Value *)arena_alloc(arena, capacity * sizeof(Value));
    list->count = 0;
    list->capacity = capacity;
    Value v;
    v.type = VAL_LIST;
    v.flags = 0;
    v.as.list = list;
    return v;
}

void val_list_append(Value *list, Value item, Arena *arena) {
    if (!list || list->type != VAL_LIST) return;
    ValueList *l = list->as.list;
    if (l->count >= l->capacity) {
        size_t new_cap = l->capacity * 2;
        Value *new_items = (Value *)arena_alloc(arena, new_cap * sizeof(Value));
        memcpy(new_items, l->items, l->count * sizeof(Value));
        l->items = new_items;
        l->capacity = new_cap;
    }
    l->items[l->count++] = item;
}

size_t val_list_len(Value list) {
    if (list.type != VAL_LIST || !list.as.list) return 0;
    return list.as.list->count;
}

Value val_list_get(Value list, size_t idx) {
    if (list.type != VAL_LIST || !list.as.list || idx >= list.as.list->count) {
        return val_nil();
    }
    return list.as.list->items[idx];
}

void val_list_set(Value *list, size_t idx, Value item) {
    if (!list || list->type != VAL_LIST || !list->as.list || idx >= list->as.list->count) {
        return;
    }
    list->as.list->items[idx] = item;
}

// Hash map
static uint32_t hash_slice(Slice s) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < s.len; i++) {
        h ^= (uint8_t)s.ptr[i];
        h *= 16777619u;
    }
    return h;
}

Value val_map_create(Arena *arena, size_t capacity) {
    if (capacity < 8) capacity = 8;
    ValueMap *map = (ValueMap *)arena_alloc(arena, sizeof(ValueMap));
    map->entries = (MapEntry *)arena_alloc_zero(arena, capacity * sizeof(MapEntry));
    map->count = 0;
    map->capacity = capacity;
    Value v;
    v.type = VAL_MAP;
    v.flags = 0;
    v.as.map = map;
    return v;
}

void val_map_set(Value *map, Slice key, Value val, Arena *arena) {
    if (!map || map->type != VAL_MAP) return;
    ValueMap *m = map->as.map;
    
    // Grow if load factor >= 0.7
    if (m->count * 10 >= m->capacity * 7) {
        size_t old_cap = m->capacity;
        MapEntry *old_entries = m->entries;
        size_t new_cap = old_cap * 2;
        MapEntry *new_entries = (MapEntry *)arena_alloc_zero(arena, new_cap * sizeof(MapEntry));
        m->capacity = new_cap;
        m->entries = new_entries;
        m->count = 0;
        for (size_t i = 0; i < old_cap; i++) {
            if (old_entries[i].occupied) {
                val_map_set(map, old_entries[i].key, old_entries[i].val, arena);
            }
        }
        m = map->as.map;
    }
    
    uint32_t mask = (uint32_t)(m->capacity - 1);
    uint32_t idx = hash_slice(key) & mask;
    while (m->entries[idx].occupied) {
        if (slice_eq(m->entries[idx].key, key)) {
            m->entries[idx].val = val;
            return;
        }
        idx = (idx + 1) & mask;
    }
    
    m->entries[idx].occupied = true;
    m->entries[idx].key = key;
    m->entries[idx].val = val;
    m->count++;
}

void val_map_set_str(Value *map, const char *key, Value val, Arena *arena) {
    Slice k;
    k.ptr = key;
    k.len = strlen(key);
    val_map_set(map, k, val, arena);
}

bool val_map_get(Value map, Slice key, Value *out_val) {
    if (map.type != VAL_MAP || !map.as.map || map.as.map->count == 0) return false;
    ValueMap *m = map.as.map;
    uint32_t mask = (uint32_t)(m->capacity - 1);
    uint32_t idx = hash_slice(key) & mask;
    size_t probes = 0;
    while (m->entries[idx].occupied && probes < m->capacity) {
        if (slice_eq(m->entries[idx].key, key)) {
            if (out_val) *out_val = m->entries[idx].val;
            return true;
        }
        idx = (idx + 1) & mask;
        probes++;
    }
    return false;
}

bool val_map_get_str(Value map, const char *key, Value *out_val) {
    Slice k;
    k.ptr = key;
    k.len = strlen(key);
    return val_map_get(map, k, out_val);
}

bool val_is_truthy(Value v) {
    switch (v.type) {
        case VAL_NIL: return false;
        case VAL_BOOL: return v.as.i64 != 0;
        case VAL_INT: return v.as.i64 != 0;
        case VAL_FLOAT: return v.as.f64 != 0.0;
        case VAL_SLICE: return v.as.slice.len > 0;
        case VAL_LIST: return v.as.list && v.as.list->count > 0;
        case VAL_MAP: return v.as.map && v.as.map->count > 0;
        default: return true;
    }
}

bool val_equal(Value a, Value b) {
    if (a.type == VAL_SLICE && b.type == VAL_SLICE) {
        return slice_eq(a.as.slice, b.as.slice);
    }
    if ((a.type == VAL_INT || a.type == VAL_FLOAT) && (b.type == VAL_INT || b.type == VAL_FLOAT)) {
        double da = (a.type == VAL_INT) ? (double)a.as.i64 : a.as.f64;
        double db = (b.type == VAL_INT) ? (double)b.as.i64 : b.as.f64;
        return da == db;
    }
    if (a.type != b.type) return false;
    switch (a.type) {
        case VAL_NIL: return true;
        case VAL_BOOL: return a.as.i64 == b.as.i64;
        case VAL_INT: return a.as.i64 == b.as.i64;
        case VAL_FLOAT: return a.as.f64 == b.as.f64;
        default: return false;
    }
}

int val_compare(Value a, Value b, bool numeric) {
    if (numeric || ((a.type == VAL_INT || a.type == VAL_FLOAT) && (b.type == VAL_INT || b.type == VAL_FLOAT))) {
        double da = 0.0, db = 0.0;
        if (a.type == VAL_INT) da = (double)a.as.i64;
        else if (a.type == VAL_FLOAT) da = a.as.f64;
        else if (a.type == VAL_SLICE) { bool ok; da = slice_to_f64(a.as.slice, &ok); }
        
        if (b.type == VAL_INT) db = (double)b.as.i64;
        else if (b.type == VAL_FLOAT) db = b.as.f64;
        else if (b.type == VAL_SLICE) { bool ok; db = slice_to_f64(b.as.slice, &ok); }
        
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }
    
    // String comparison
    Slice sa = (a.type == VAL_SLICE) ? a.as.slice : (Slice){"", 0};
    Slice sb = (b.type == VAL_SLICE) ? b.as.slice : (Slice){"", 0};
    size_t min_len = sa.len < sb.len ? sa.len : sb.len;
    int cmp = memcmp(sa.ptr, sb.ptr, min_len);
    if (cmp != 0) return cmp;
    if (sa.len < sb.len) return -1;
    if (sa.len > sb.len) return 1;
    return 0;
}

char *val_to_string(Value v, Arena *arena) {
    char buf[128];
    switch (v.type) {
        case VAL_NIL: return arena_strdup(arena, "nil");
        case VAL_BOOL: return arena_strdup(arena, v.as.i64 ? "true" : "false");
        case VAL_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long)v.as.i64);
            return arena_strdup(arena, buf);
        case VAL_FLOAT:
            snprintf(buf, sizeof(buf), "%g", v.as.f64);
            return arena_strdup(arena, buf);
        case VAL_SLICE:
            return slice_to_cstr(v.as.slice, arena);
        case VAL_LIST: {
            size_t len = val_list_len(v);
            if (len == 0) return arena_strdup(arena, "[]");
            // Simple string format
            return arena_strdup(arena, "[...]");
        }
        case VAL_MAP:
            return arena_strdup(arena, "{...}");
        default:
            return arena_strdup(arena, "<value>");
    }
}

void val_print(Value v, FILE *out) {
    if (!out) out = stdout;
    switch (v.type) {
        case VAL_NIL: break;
        case VAL_BOOL: fputs(v.as.i64 ? "true" : "false", out); break;
        case VAL_INT: fprintf(out, "%lld", (long long)v.as.i64); break;
        case VAL_FLOAT: fprintf(out, "%g", v.as.f64); break;
        case VAL_SLICE:
            if (v.as.slice.len > 0 && v.as.slice.ptr) {
                fwrite(v.as.slice.ptr, 1, v.as.slice.len, out);
            }
            break;
        case VAL_LIST: {
            size_t len = val_list_len(v);
            for (size_t i = 0; i < len; i++) {
                val_println(val_list_get(v, i), out);
            }
            break;
        }
        case VAL_MAP: {
            ValueMap *m = v.as.map;
            fputs("{", out);
            bool first = true;
            for (size_t i = 0; i < m->capacity; i++) {
                if (m->entries[i].occupied) {
                    if (!first) fputs(", ", out);
                    first = false;
                    fwrite(m->entries[i].key.ptr, 1, m->entries[i].key.len, out);
                    fputs(": ", out);
                    val_print(m->entries[i].val, out);
                }
            }
            fputs("}", out);
            break;
        }
        default:
            fprintf(out, "<object type=%d>", v.type);
            break;
    }
}

void val_println(Value v, FILE *out) {
    if (!out) out = stdout;
    val_print(v, out);
    fputc('\n', out);
}

Value val_clone_to_arena(Value v, Arena *dst) {
    switch (v.type) {
        case VAL_SLICE: {
            char *buf = arena_strndup(dst, v.as.slice.ptr, v.as.slice.len);
            return val_slice(buf, v.as.slice.len);
        }
        case VAL_LIST: {
            size_t len = val_list_len(v);
            Value new_l = val_list_create(dst, len);
            for (size_t i = 0; i < len; i++) {
                val_list_append(&new_l, val_clone_to_arena(val_list_get(v, i), dst), dst);
            }
            return new_l;
        }
        case VAL_MAP: {
            ValueMap *m = v.as.map;
            Value new_m = val_map_create(dst, m ? m->capacity : 8);
            if (m) {
                for (size_t i = 0; i < m->capacity; i++) {
                    if (m->entries[i].occupied) {
                        char *kstr = arena_strndup(dst, m->entries[i].key.ptr, m->entries[i].key.len);
                        Slice k = { kstr, m->entries[i].key.len };
                        val_map_set(&new_m, k, val_clone_to_arena(m->entries[i].val, dst), dst);
                    }
                }
            }
            return new_m;
        }
        default:
            return v;
    }
}
