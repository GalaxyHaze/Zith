// src/arena.c
#include <zith/zith.hpp>
#include <stddef.h>  // gives you max_align_t on MSVC
#include <stdlib.h>
#include <string.h>

#define ZITH_DEFAULT_BLOCK_SIZE (64 * 1024)

#ifdef _MSC_VER
  typedef union {
      long long   _ll;
      long double _ld;
      void       *_ptr;
  } max_align_t;
#endif

#ifdef _MSC_VER
  #pragma warning(push)
  #pragma warning(disable: 4200)
#endif

typedef struct ZithArenaBlock {
    struct ZithArenaBlock *next;
    size_t offset;
    size_t capacity;
    char data[];
} ZithArenaBlock;

#ifdef _MSC_VER
  #pragma warning(pop)
#endif

struct ZithArena {
    ZithArenaBlock *head;
    size_t initial_block_size;
};

static inline size_t align_up(const size_t size, const size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

struct ZithArena *zith_arena_create(size_t initial_block_size) {
    if (initial_block_size == 0) initial_block_size = ZITH_DEFAULT_BLOCK_SIZE;
    struct ZithArena *arena = calloc(1, sizeof(ZithArena));
    if (!arena) return NULL;
    arena->initial_block_size = initial_block_size;
    return arena;
}

void *zith_arena_alloc(ZithArena *arena, size_t size) {
    if (!arena || size == 0) return NULL;

    const size_t alignment = _Alignof(max_align_t);
    size = align_up(size, alignment);

    ZithArenaBlock *block = arena->head;
    if (!block || block->offset + size > block->capacity) {
        // Allocate new block
        size_t block_size = arena->initial_block_size;
        if (size > block_size) block_size = size; // accommodate large allocs

        const size_t total_alloc = sizeof(ZithArenaBlock) + block_size;
        ZithArenaBlock *new_block = (ZithArenaBlock *) malloc(total_alloc);
        if (!new_block) return NULL;

        new_block->next = arena->head;
        new_block->offset = 0;
        new_block->capacity = block_size;
        arena->head = new_block;
        block = new_block;
    }

    void *ptr = &block->data[block->offset];
    block->offset += size;
    return ptr;
}

char *zith_arena_strdup(ZithArena *arena, const char *str) {
    if (!str) return NULL;
    const size_t len = strlen(str);
    void *copy = zith_arena_alloc(arena, len + 1);
    if (copy) memcpy(copy, str, len + 1);
    return copy;
}

char *zith_arena_str(ZithArena *arena, const char *str, const size_t len) {
    if (!str) return NULL;
    char *copy = zith_arena_alloc(arena, len + 1);
    if (!copy) return NULL;
    memcpy(copy, str, len);
    copy[len] = '\0';
    return copy;
}

void zith_arena_reset(ZithArena *arena) {
    if (!arena) return;
    for (ZithArenaBlock *b = arena->head; b; b = b->next)
        b->offset = 0;
}

void zith_arena_destroy(ZithArena *arena) {
    if (!arena) return;
    zith_arena_reset(arena);
    free(arena);
}