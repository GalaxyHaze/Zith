// src/arena.c
#include <Nova/nova.h>
#include <stdlib.h>
#include <string.h>

#define NOVA_DEFAULT_BLOCK_SIZE (64 * 1024)

typedef struct NovaArenaBlock {
    struct NovaArenaBlock* next;
    size_t offset;          // current write offset in data
    size_t capacity;        // total size of data
    char data[];            // flexible array
} NovaArenaBlock;

struct NovaArena {
    NovaArenaBlock* head;
    size_t initial_block_size;
};

static inline size_t align_up(const size_t size, const size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

NovaArena* nova_arena_create(size_t initial_block_size) {
    if (initial_block_size == 0) initial_block_size = NOVA_DEFAULT_BLOCK_SIZE;
    NovaArena* arena = calloc(1, sizeof(NovaArena));
    if (!arena) return NULL;
    arena->initial_block_size = initial_block_size;
    return arena;
}

void* nova_arena_alloc(NovaArena* arena, size_t size) {
    if (!arena || size == 0) return NULL;

    const size_t alignment = _Alignof(max_align_t);
    size = align_up(size, alignment);

    NovaArenaBlock* block = arena->head;
    if (!block || block->offset + size > block->capacity) {
        // Allocate new block
        size_t block_size = arena->initial_block_size;
        if (size > block_size) block_size = size; // accommodate large allocs

        const size_t total_alloc = sizeof(NovaArenaBlock) + block_size;
        NovaArenaBlock* new_block = (NovaArenaBlock*)malloc(total_alloc);
        if (!new_block) return NULL;

        new_block->next = arena->head;
        new_block->offset = 0;
        new_block->capacity = block_size;
        arena->head = new_block;
        block = new_block;
    }

    void* ptr = &block->data[block->offset];
    block->offset += size;
    return ptr;
}

char* nova_arena_strdup(NovaArena* arena, const char* str) {
    if (!str) return NULL;
    const size_t len = strlen(str);
    void* copy = nova_arena_alloc(arena, len + 1);
    if (copy) memcpy(copy, str, len + 1);
    return copy;
}

char* nova_arena_str(NovaArena* arena, const char* str, const size_t len)
{
    if (!str) return NULL;
    void* copy = nova_arena_alloc(arena, len + 1);
    if (copy) memcpy(copy, str, len + 1);
    return copy;
}

void nova_arena_reset(NovaArena* arena) {
    if (!arena) return;
    NovaArenaBlock* block = arena->head;
    while (block) {
        NovaArenaBlock* next = block->next;
        free(block);
        block = next;
    }
    arena->head = NULL;
}

void nova_arena_destroy(NovaArena* arena) {
    if (!arena) return;
    nova_arena_reset(arena);
    free(arena);
}