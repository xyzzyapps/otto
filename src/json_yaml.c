#include "json_yaml.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// --- JSON Parser ---

typedef struct {
    const char *src;
    size_t len;
    size_t pos;
    Arena *arena;
    bool ok;
} JsonParser;

static void json_skip_ws(JsonParser *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            p->pos++;
        } else {
            break;
        }
    }
}

static char json_peek(JsonParser *p) {
    json_skip_ws(p);
    if (p->pos >= p->len) return '\0';
    return p->src[p->pos];
}

static char json_next(JsonParser *p) {
    json_skip_ws(p);
    if (p->pos >= p->len) return '\0';
    return p->src[p->pos++];
}

static Value json_parse_value(JsonParser *p);

static Slice json_parse_string(JsonParser *p) {
    if (json_next(p) != '"') {
        p->ok = false;
        return (Slice){"", 0};
    }
    size_t start = p->pos;
    bool has_escape = false;
    while (p->pos < p->len && p->src[p->pos] != '"') {
        if (p->src[p->pos] == '\\') {
            has_escape = true;
            p->pos++;
        }
        p->pos++;
    }
    size_t raw_len = p->pos - start;
    if (p->pos < p->len && p->src[p->pos] == '"') {
        p->pos++; // consume closing quote
    } else {
        p->ok = false;
        return (Slice){"", 0};
    }

    if (!has_escape) {
        return (Slice){p->src + start, raw_len};
    }

    // Unescape into arena
    char *buf = (char *)arena_alloc(p->arena, raw_len + 1);
    size_t out_len = 0;
    for (size_t i = 0; i < raw_len; i++) {
        if (p->src[start + i] == '\\' && i + 1 < raw_len) {
            i++;
            char esc = p->src[start + i];
            switch (esc) {
                case 'n': buf[out_len++] = '\n'; break;
                case 'r': buf[out_len++] = '\r'; break;
                case 't': buf[out_len++] = '\t'; break;
                case '"': buf[out_len++] = '"'; break;
                case '\\': buf[out_len++] = '\\'; break;
                default: buf[out_len++] = esc; break;
            }
        } else {
            buf[out_len++] = p->src[start + i];
        }
    }
    buf[out_len] = '\0';
    return (Slice){buf, out_len};
}

static Value json_parse_number(JsonParser *p) {
    size_t start = p->pos;
    bool is_float = false;
    if (p->src[p->pos] == '-') p->pos++;
    while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    if (p->pos < p->len && p->src[p->pos] == '.') {
        is_float = true;
        p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        is_float = true;
        p->pos++;
        if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-')) p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }

    Slice s = {p->src + start, p->pos - start};
    bool ok;
    if (is_float) {
        return val_float(slice_to_f64(s, &ok));
    } else {
        return val_int(slice_to_i64(s, &ok));
    }
}

static Value json_parse_array(JsonParser *p) {
    json_next(p); // consume '['
    Value list = val_list_create(p->arena, 8);
    if (json_peek(p) == ']') {
        json_next(p);
        return list;
    }
    while (p->ok && p->pos < p->len) {
        Value item = json_parse_value(p);
        val_list_append(&list, item, p->arena);
        char c = json_peek(p);
        if (c == ',') {
            json_next(p);
        } else if (c == ']') {
            json_next(p);
            break;
        } else {
            p->ok = false;
            break;
        }
    }
    return list;
}

static Value json_parse_object(JsonParser *p) {
    json_next(p); // consume '{'
    Value map = val_map_create(p->arena, 8);
    if (json_peek(p) == '}') {
        json_next(p);
        return map;
    }
    while (p->ok && p->pos < p->len) {
        if (json_peek(p) != '"') {
            p->ok = false;
            break;
        }
        Slice key = json_parse_string(p);
        if (json_next(p) != ':') {
            p->ok = false;
            break;
        }
        Value val = json_parse_value(p);
        val_map_set(&map, key, val, p->arena);

        char c = json_peek(p);
        if (c == ',') {
            json_next(p);
        } else if (c == '}') {
            json_next(p);
            break;
        } else {
            p->ok = false;
            break;
        }
    }
    return map;
}

static Value json_parse_value(JsonParser *p) {
    char c = json_peek(p);
    if (c == '"') {
        Slice s = json_parse_string(p);
        return val_slice(s.ptr, s.len);
    }
    if (c == '{') return json_parse_object(p);
    if (c == '[') return json_parse_array(p);
    if (isdigit((unsigned char)c) || c == '-') return json_parse_number(p);
    if (c == 't' && p->pos + 4 <= p->len && strncmp(p->src + p->pos, "true", 4) == 0) {
        p->pos += 4;
        return val_bool(true);
    }
    if (c == 'f' && p->pos + 5 <= p->len && strncmp(p->src + p->pos, "false", 5) == 0) {
        p->pos += 5;
        return val_bool(false);
    }
    if (c == 'n' && p->pos + 4 <= p->len && strncmp(p->src + p->pos, "null", 4) == 0) {
        p->pos += 4;
        return val_nil();
    }
    p->ok = false;
    return val_nil();
}

Value json_parse(const char *json_str, size_t len, Arena *arena, bool *ok) {
    JsonParser p;
    p.src = json_str;
    p.len = len;
    p.pos = 0;
    p.arena = arena;
    p.ok = true;
    Value v = json_parse_value(&p);
    if (ok) *ok = p.ok;
    return v;
}

// --- JSON Serializer ---
static void stringify_value_buf(Value val, char **buf, size_t *len, size_t *cap) {
    char tmp[64];
    switch (val.type) {
        case VAL_NIL: {
            const char *s = "null";
            size_t slen = 4;
            while (*len + slen + 1 >= *cap) { *cap *= 2; *buf = (char *)realloc(*buf, *cap); }
            memcpy(*buf + *len, s, slen);
            *len += slen;
            (*buf)[*len] = '\0';
            break;
        }
        case VAL_BOOL: {
            const char *s = val.as.i64 ? "true" : "false";
            size_t slen = strlen(s);
            while (*len + slen + 1 >= *cap) { *cap *= 2; *buf = (char *)realloc(*buf, *cap); }
            memcpy(*buf + *len, s, slen);
            *len += slen;
            (*buf)[*len] = '\0';
            break;
        }
        case VAL_INT: {
            snprintf(tmp, sizeof(tmp), "%lld", (long long)val.as.i64);
            size_t slen = strlen(tmp);
            while (*len + slen + 1 >= *cap) { *cap *= 2; *buf = (char *)realloc(*buf, *cap); }
            memcpy(*buf + *len, tmp, slen);
            *len += slen;
            (*buf)[*len] = '\0';
            break;
        }
        case VAL_FLOAT: {
            snprintf(tmp, sizeof(tmp), "%g", val.as.f64);
            size_t slen = strlen(tmp);
            while (*len + slen + 1 >= *cap) { *cap *= 2; *buf = (char *)realloc(*buf, *cap); }
            memcpy(*buf + *len, tmp, slen);
            *len += slen;
            (*buf)[*len] = '\0';
            break;
        }
        case VAL_SLICE: {
            while (*len + val.as.slice.len + 3 >= *cap) { *cap *= 2; *buf = (char *)realloc(*buf, *cap); }
            (*buf)[(*len)++] = '"';
            memcpy(*buf + *len, val.as.slice.ptr, val.as.slice.len);
            *len += val.as.slice.len;
            (*buf)[(*len)++] = '"';
            (*buf)[*len] = '\0';
            break;
        }
        case VAL_LIST: {
            while (*len + 2 >= *cap) { *cap *= 2; *buf = (char *)realloc(*buf, *cap); }
            (*buf)[(*len)++] = '[';
            size_t count = val_list_len(val);
            for (size_t i = 0; i < count; i++) {
                if (i > 0) {
                    while (*len + 3 >= *cap) { *cap *= 2; *buf = (char *)realloc(*buf, *cap); }
                    (*buf)[(*len)++] = ',';
                    (*buf)[(*len)++] = ' ';
                }
                stringify_value_buf(val_list_get(val, i), buf, len, cap);
            }
            while (*len + 2 >= *cap) { *cap *= 2; *buf = (char *)realloc(*buf, *cap); }
            (*buf)[(*len)++] = ']';
            (*buf)[*len] = '\0';
            break;
        }
        case VAL_MAP: {
            while (*len + 2 >= *cap) { *cap *= 2; *buf = (char *)realloc(*buf, *cap); }
            (*buf)[(*len)++] = '{';
            ValueMap *m = val.as.map;
            bool first = true;
            for (size_t i = 0; i < m->capacity; i++) {
                if (m->entries[i].occupied) {
                    if (!first) {
                        while (*len + 3 >= *cap) { *cap *= 2; *buf = (char *)realloc(*buf, *cap); }
                        (*buf)[(*len)++] = ',';
                        (*buf)[(*len)++] = ' ';
                    }
                    first = false;
                    while (*len + m->entries[i].key.len + 5 >= *cap) { *cap *= 2; *buf = (char *)realloc(*buf, *cap); }
                    (*buf)[(*len)++] = '"';
                    memcpy(*buf + *len, m->entries[i].key.ptr, m->entries[i].key.len);
                    *len += m->entries[i].key.len;
                    (*buf)[(*len)++] = '"';
                    (*buf)[(*len)++] = ':';
                    (*buf)[(*len)++] = ' ';
                    stringify_value_buf(m->entries[i].val, buf, len, cap);
                }
            }
            while (*len + 2 >= *cap) { *cap *= 2; *buf = (char *)realloc(*buf, *cap); }
            (*buf)[(*len)++] = '}';
            (*buf)[*len] = '\0';
            break;
        }
        default:
            break;
    }
}

char *json_stringify(Value val, Arena *arena, bool pretty) {
    size_t cap = 256;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    buf[0] = '\0';
    stringify_value_buf(val, &buf, &len, &cap);
    char *result = arena_strndup(arena, buf, len);
    free(buf);
    return result;
}

// --- YAML Parser ---
// Lightweight line-oriented YAML parser supporting mappings, lists, and scalars
Value yaml_parse(const char *yaml_str, size_t len, Arena *arena, bool *ok) {
    // If it starts with { or [, use JSON parser directly
    size_t p = 0;
    while (p < len && isspace((unsigned char)yaml_str[p])) p++;
    if (p < len && (yaml_str[p] == '{' || yaml_str[p] == '[')) {
        return json_parse(yaml_str + p, len - p, arena, ok);
    }

    Value root = val_map_create(arena, 16);
    Value cur_list = val_nil();
    bool is_root_list = false;

    // Scan lines
    const char *line_start = yaml_str;
    const char *end = yaml_str + len;

    while (line_start < end) {
        const char *line_end = line_start;
        while (line_end < end && *line_end != '\n') line_end++;

        Slice line = {line_start, (size_t)(line_end - line_start)};
        if (line.len > 0 && line.ptr[line.len - 1] == '\r') line.len--;
        Slice trimmed = slice_trim(line);

        if (trimmed.len > 0 && trimmed.ptr[0] != '#') {
            if (trimmed.ptr[0] == '-') {
                // List item
                if (!is_root_list && val_list_len(cur_list) == 0) {
                    cur_list = val_list_create(arena, 16);
                    is_root_list = true;
                }
                Slice item_s = slice_trim((Slice){trimmed.ptr + 1, trimmed.len - 1});
                bool num_ok;
                int64_t ival = slice_to_i64(item_s, &num_ok);
                if (num_ok) {
                    val_list_append(&cur_list, val_int(ival), arena);
                } else if (slice_eq_str(item_s, "true")) {
                    val_list_append(&cur_list, val_bool(true), arena);
                } else if (slice_eq_str(item_s, "false")) {
                    val_list_append(&cur_list, val_bool(false), arena);
                } else {
                    char *str = arena_strndup(arena, item_s.ptr, item_s.len);
                    val_list_append(&cur_list, val_slice(str, item_s.len), arena);
                }
            } else {
                // Key-value pair: key: value
                const char *colon = (const char *)memchr(trimmed.ptr, ':', trimmed.len);
                if (colon) {
                    Slice key = slice_trim((Slice){trimmed.ptr, (size_t)(colon - trimmed.ptr)});
                    Slice val = slice_trim((Slice){colon + 1, (size_t)(trimmed.ptr + trimmed.len - colon - 1)});

                    // Remove quotes if present
                    if (val.len >= 2 && ((val.ptr[0] == '"' && val.ptr[val.len - 1] == '"') ||
                                         (val.ptr[0] == '\'' && val.ptr[val.len - 1] == '\''))) {
                        val.ptr++;
                        val.len -= 2;
                    }

                    bool num_ok;
                    int64_t ival = slice_to_i64(val, &num_ok);
                    Value val_obj;
                    if (num_ok) {
                        val_obj = val_int(ival);
                    } else if (slice_eq_str(val, "true")) {
                        val_obj = val_bool(true);
                    } else if (slice_eq_str(val, "false")) {
                        val_obj = val_bool(false);
                    } else {
                        char *s = arena_strndup(arena, val.ptr, val.len);
                        val_obj = val_slice(s, val.len);
                    }
                    val_map_set(&root, key, val_obj, arena);
                }
            }
        }

        line_start = (line_end < end && *line_end == '\n') ? line_end + 1 : line_end;
    }

    if (ok) *ok = true;
    return is_root_list ? cur_list : root;
}

char *yaml_stringify(Value val, Arena *arena) {
    return json_stringify(val, arena, true);
}
