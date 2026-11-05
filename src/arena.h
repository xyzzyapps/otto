#ifndef OTTO_ARENA_H
#define OTTO_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct ArenaChunk {
    struct ArenaChunk *next;
    size_t capacity;
    size_t used;
    uint8_t data[];
} ArenaChunk;

typedef struct Arena {
    ArenaChunk *first;
    ArenaChunk *current;
    size_t default_chunk_size;
} Arena;

void arena_init(Arena *arena, size_t default_chunk_size);
void *arena_alloc(Arena *arena, size_t size);
void *arena_alloc_zero(Arena *arena, size_t size);
char *arena_strndup(Arena *arena, const char *s, size_t len);
char *arena_strdup(Arena *arena, const char *s);
void arena_reset(Arena *arena);
void arena_free(Arena *arena);

#endif // OTTO_ARENA_H
