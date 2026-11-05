#include "arena.h"
#include <stdlib.h>
#include <string.h>

#define ARENA_ALIGN(size, align) (((size) + (align) - 1) & ~((align) - 1))
#define DEFAULT_ALIGNMENT 8

static ArenaChunk *arena_chunk_create(size_t capacity) {
    ArenaChunk *chunk = (ArenaChunk *)malloc(sizeof(ArenaChunk) + capacity);
    if (!chunk) return NULL;
    chunk->next = NULL;
    chunk->capacity = capacity;
    chunk->used = 0;
    return chunk;
}

void arena_init(Arena *arena, size_t default_chunk_size) {
    if (default_chunk_size == 0) default_chunk_size = 64 * 1024; // 64KB default
    arena->default_chunk_size = default_chunk_size;
    arena->first = arena_chunk_create(default_chunk_size);
    arena->current = arena->first;
}

void *arena_alloc(Arena *arena, size_t size) {
    if (size == 0) return NULL;
    size = ARENA_ALIGN(size, DEFAULT_ALIGNMENT);
    
    if (!arena->current) {
        arena_init(arena, arena->default_chunk_size);
    }
    
    ArenaChunk *cur = arena->current;
    if (cur->used + size <= cur->capacity) {
        void *ptr = cur->data + cur->used;
        cur->used += size;
        return ptr;
    }
    
    // Check if next chunk exists and has capacity
    if (cur->next && cur->next->capacity >= size) {
        arena->current = cur->next;
        cur = arena->current;
        cur->used = 0;
        void *ptr = cur->data;
        cur->used += size;
        return ptr;
    }
    
    // Allocate new chunk
    size_t chunk_cap = arena->default_chunk_size;
    if (size > chunk_cap) chunk_cap = size + sizeof(ArenaChunk);
    
    ArenaChunk *new_chunk = arena_chunk_create(chunk_cap);
    if (!new_chunk) return NULL;
    
    cur->next = new_chunk;
    arena->current = new_chunk;
    void *ptr = new_chunk->data;
    new_chunk->used = size;
    return ptr;
}

void *arena_alloc_zero(Arena *arena, size_t size) {
    void *ptr = arena_alloc(arena, size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

char *arena_strndup(Arena *arena, const char *s, size_t len) {
    if (!s) return NULL;
    char *res = (char *)arena_alloc(arena, len + 1);
    if (!res) return NULL;
    memcpy(res, s, len);
    res[len] = '\0';
    return res;
}

char *arena_strdup(Arena *arena, const char *s) {
    if (!s) return NULL;
    return arena_strndup(arena, s, strlen(s));
}

void arena_reset(Arena *arena) {
    for (ArenaChunk *c = arena->first; c != NULL; c = c->next) {
        c->used = 0;
    }
    arena->current = arena->first;
}

void arena_free(Arena *arena) {
    ArenaChunk *c = arena->first;
    while (c) {
        ArenaChunk *next = c->next;
        free(c);
        c = next;
    }
    arena->first = NULL;
    arena->current = NULL;
}
