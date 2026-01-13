#ifndef IMMIX_H
#define IMMIX_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

struct st_table;

#define IMMIX_LOG_BYTES_IN_LINE   8
#define IMMIX_LOG_BYTES_IN_BLOCK  16
#define IMMIX_LINE_SIZE           (1 << IMMIX_LOG_BYTES_IN_LINE)   /* 256 bytes */
#define IMMIX_BLOCK_SIZE          (1 << IMMIX_LOG_BYTES_IN_BLOCK)  /* 64KB */
#define IMMIX_LINES_PER_BLOCK     (IMMIX_BLOCK_SIZE / IMMIX_LINE_SIZE) /* 256 */
#define IMMIX_BLOCK_MASK          (~(uintptr_t)(IMMIX_BLOCK_SIZE - 1))
#define IMMIX_LINE_MASK           (~(uintptr_t)(IMMIX_LINE_SIZE - 1))
#define IMMIX_LARGE_OBJECT_THRESHOLD (IMMIX_BLOCK_SIZE / 4)
#define IMMIX_ALLOC_MAP_BITS_PER_BLOCK (IMMIX_BLOCK_SIZE / sizeof(void *))
#define IMMIX_ALLOC_MAP_BYTES    ((IMMIX_ALLOC_MAP_BITS_PER_BLOCK + 7) / 8)

enum immix_line_mark {
    IMMIX_LINE_FREE = 0,
    IMMIX_LINE_MARKED,
    IMMIX_LINE_MARKED_CONSERVATIVE
};

enum immix_block_state {
    IMMIX_BLOCK_FREE = 0,
    IMMIX_BLOCK_RECYCLABLE,
    IMMIX_BLOCK_UNAVAILABLE
};

struct immix_block {
    uint32_t magic;
    enum immix_block_state state;
    uint16_t free_lines;
    uint16_t hole_count;
    struct immix_block *next;
    struct immix_block *prev;
    uint8_t line_marks[IMMIX_LINES_PER_BLOCK];
    uint8_t alloc_map[IMMIX_ALLOC_MAP_BYTES];
};

#define IMMIX_BLOCK_MAGIC 0x494D4D58  /* "IMMX" */
#define IMMIX_METADATA_LINES ((sizeof(struct immix_block) + IMMIX_LINE_SIZE - 1) / IMMIX_LINE_SIZE)
#define IMMIX_USABLE_LINES (IMMIX_LINES_PER_BLOCK - IMMIX_METADATA_LINES)
#define IMMIX_METADATA_BYTES (IMMIX_METADATA_LINES * IMMIX_LINE_SIZE)

struct immix_ractor_cache {
    struct immix_ractor_cache *next;
    struct immix_ractor_cache *prev;
    struct immix_block *current_block;
    char *cursor;
    char *limit;
    size_t current_line;
    size_t allocated_bytes;
};

struct immix_objspace {
    struct immix_block *free_blocks;
    struct immix_block *usable_blocks;
    struct immix_block *full_blocks;
    size_t total_blocks;
    size_t free_block_count;
    size_t usable_block_count;
    size_t full_block_count;
    size_t total_heap_bytes;
    size_t used_heap_bytes;
    struct immix_ractor_cache *ractor_caches;
    size_t ractor_cache_count;
    bool gc_enabled;
    bool gc_stress;
    bool during_gc;
    size_t gc_count;
    bool measure_gc_time;
    unsigned long long total_gc_time;
    size_t total_allocated_objects;
    size_t total_freed_objects;
    st_table *finalizer_table;
    pthread_mutex_t lock;
};

static inline struct immix_block *
immix_block_for_ptr(void *ptr)
{
    return (struct immix_block *)((uintptr_t)ptr & IMMIX_BLOCK_MASK);
}

static inline uintptr_t
immix_block_data_start(struct immix_block *block)
{
    return (uintptr_t)block + IMMIX_METADATA_BYTES;
}

static inline uintptr_t
immix_block_data_end(struct immix_block *block)
{
    return (uintptr_t)block + IMMIX_BLOCK_SIZE;
}

static inline size_t
immix_line_index(void *ptr)
{
    struct immix_block *block = immix_block_for_ptr(ptr);
    return ((uintptr_t)ptr - (uintptr_t)block) >> IMMIX_LOG_BYTES_IN_LINE;
}

static inline void
immix_set_alloc_bit(struct immix_block *block, void *ptr)
{
    size_t slot = ((uintptr_t)ptr - (uintptr_t)block) / sizeof(void *);
    block->alloc_map[slot / 8] |= (1 << (slot % 8));
}

static inline bool
immix_get_alloc_bit(struct immix_block *block, void *ptr)
{
    size_t slot = ((uintptr_t)ptr - (uintptr_t)block) / sizeof(void *);
    return (block->alloc_map[slot / 8] & (1 << (slot % 8))) != 0;
}

static inline void
immix_clear_alloc_bit(struct immix_block *block, void *ptr)
{
    size_t slot = ((uintptr_t)ptr - (uintptr_t)block) / sizeof(void *);
    block->alloc_map[slot / 8] &= ~(1 << (slot % 8));
}

static inline bool
immix_ptr_in_block(struct immix_block *block, void *ptr)
{
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t block_addr = (uintptr_t)block;
    return addr >= block_addr + IMMIX_METADATA_BYTES && addr < block_addr + IMMIX_BLOCK_SIZE;
}

#endif /* IMMIX_H */
