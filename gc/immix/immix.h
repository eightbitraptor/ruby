#ifndef IMMIX_H
#define IMMIX_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define IMMIX_LOG_BYTES_IN_LINE   8
#define IMMIX_LOG_BYTES_IN_BLOCK  16
#define IMMIX_LINE_SIZE           (1 << IMMIX_LOG_BYTES_IN_LINE)   /* 256 bytes */
#define IMMIX_BLOCK_SIZE          (1 << IMMIX_LOG_BYTES_IN_BLOCK)  /* 64KB */
#define IMMIX_LINES_PER_BLOCK     (IMMIX_BLOCK_SIZE / IMMIX_LINE_SIZE) /* 256 */
#define IMMIX_LARGE_OBJECT_THRESHOLD (IMMIX_BLOCK_SIZE - IMMIX_LINE_SIZE)

enum immix_line_mark {
    IMMIX_LINE_FREE = 0,
    IMMIX_LINE_LIVE,
    IMMIX_LINE_FRESH_ALLOC,
    IMMIX_LINE_CONSERV_LIVE,
    IMMIX_LINE_PREV_LIVE
};

enum immix_block_state {
    IMMIX_BLOCK_USABLE = 0,
    IMMIX_BLOCK_FULL
};

struct immix_block {
    uint8_t line_marks[IMMIX_LINES_PER_BLOCK];
    enum immix_block_state state;
    struct immix_block *next;
};

struct immix_objspace;

#endif /* IMMIX_H */
