// src/arena.c
#include <kalidous/kalidous.hpp>
#include <stddef.h>  // gives you max_align_t on MSVC
#include <stdlib.h>
#include <string.h>

#define KALIDOUS_DEFAULT_BLOCK_SIZE (64 * 1024)

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

typedef struct KalidousArenaBlock {
    struct KalidousArenaBlock *next;
    size_t offset;
    size_t capacity;
    char data[];
} KalidousArenaBlock;

#ifdef _MSC_VER
  #pragma warning(pop)
#endif

struct KalidousArena {
    KalidousArenaBlock *head;
    size_t initial_block_size;
};

static inline size_t align_up(const size_t size, const size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

struct KalidousArena *kalidous_arena_create(size_t initial_block_size) {
    if (initial_block_size == 0) initial_block_size = KALIDOUS_DEFAULT_BLOCK_SIZE;
    struct KalidousArena *arena = calloc(1, sizeof(KalidousArena));
    if (!arena) return NULL;
    arena->initial_block_size = initial_block_size;
    return arena;
}

void *kalidous_arena_alloc(KalidousArena *arena, size_t size) {
    if (!arena || size == 0) return NULL;

    const size_t alignment = _Alignof(max_align_t);
    size = align_up(size, alignment);

    KalidousArenaBlock *block = arena->head;
    if (!block || block->offset + size > block->capacity) {
        // Allocate new block
        size_t block_size = arena->initial_block_size;
        if (size > block_size) block_size = size; // accommodate large allocs

        const size_t total_alloc = sizeof(KalidousArenaBlock) + block_size;
        KalidousArenaBlock *new_block = (KalidousArenaBlock *) malloc(total_alloc);
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

char *kalidous_arena_strdup(KalidousArena *arena, const char *str) {
    if (!str) return NULL;
    const size_t len = strlen(str);
    void *copy = kalidous_arena_alloc(arena, len + 1);
    if (copy) memcpy(copy, str, len + 1);
    return copy;
}

char *kalidous_arena_str(KalidousArena *arena, const char *str, const size_t len) {
    if (!str) return NULL;
    char *copy = kalidous_arena_alloc(arena, len + 1);
    if (!copy) return NULL;
    memcpy(copy, str, len);
    copy[len] = '\0';
    return copy;
}

void kalidous_arena_reset(KalidousArena *arena) {
    if (!arena) return;
    for (KalidousArenaBlock *b = arena->head; b; b = b->next)
        b->offset = 0;
}

void kalidous_arena_destroy(KalidousArena *arena) {
    if (!arena) return;
    kalidous_arena_reset(arena);
    free(arena);
}