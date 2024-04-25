#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ruby/ruby.h"
#include "ruby/internal/config.h"
#include "ccan/list/list.h"
#include "ruby/internal/arithmetic/long.h"
#include "ruby/internal/stdalign.h"
#include "ruby/internal/value_type.h"

typedef unsigned long VALUE;
typedef unsigned long ID;
typedef unsigned int rb_atomic_t;
typedef uint32_t rb_event_flag_t;

#include "ruby/st.h"

#define SIZE_POOL_COUNT 1
#define STACK_CHUNK_SIZE 500

/* functions we'll need to call in gc.c */
void *rb_gc_get_objspace(void);

static int
object_id_cmp(st_data_t x, st_data_t y)
{
    if (RB_TYPE_P(x, T_BIGNUM)) {
        return !rb_big_eql(x, y);
    }
    else {
        return x != y;
    }
}

static st_index_t
object_id_hash(st_data_t n)
{
    return FIX2LONG(rb_hash((VALUE)n));
}
static const struct st_hash_type object_id_hash_type = {
    object_id_cmp,
    object_id_hash,
};

typedef struct stack_chunk {
    VALUE data[STACK_CHUNK_SIZE];
    struct stack_chunk *next;
} stack_chunk_t;

typedef struct mark_stack {
    stack_chunk_t *chunk;
    stack_chunk_t *cache;
    int index;
    int limit;
    size_t cache_size;
    size_t unused_cache_size;
} mark_stack_t;

typedef struct rb_heap_struct {
    struct heap_page *free_pages;
    struct ccan_list_head pages;
    struct heap_page *sweeping_page; /* iterator for .pages */
    struct heap_page *compact_cursor;
    uintptr_t compact_cursor_index;
    struct heap_page *pooled_pages;
    size_t total_pages;      /* total page count in a heap */
    size_t total_slots;      /* total slot count (about total_pages * HEAP_PAGE_OBJ_LIMIT) */
} rb_heap_t;

typedef struct rb_size_pool_struct {
    short slot_size;

    size_t allocatable_pages;

    /* Basic statistics */
    size_t total_allocated_pages;
    size_t total_freed_pages;
    size_t force_major_gc_count;
    size_t force_incremental_marking_finish_count;
    size_t total_allocated_objects;
    size_t total_freed_objects;

    /* Sweeping statistics */
    size_t freed_slots;
    size_t empty_slots;

    rb_heap_t eden_heap;
    rb_heap_t tomb_heap;
} rb_size_pool_t;

typedef struct rb_objspace {
    struct {
        size_t limit;
        size_t increase;
    } malloc_params;

    struct {
        unsigned int mode : 2;
        unsigned int immediate_sweep : 1;
        unsigned int dont_gc : 1;
        unsigned int dont_incremental : 1;
        unsigned int during_gc : 1;
        unsigned int during_compacting : 1;
        unsigned int during_reference_updating : 1;
        unsigned int gc_stressful: 1;
        unsigned int has_newobj_hook: 1;
        unsigned int during_minor_gc : 1;
        unsigned int during_incremental_marking : 1;
        unsigned int measure_gc : 1;
    } flags;

    rb_event_flag_t hook_events;
    unsigned long long next_object_id;

    rb_size_pool_t size_pools[SIZE_POOL_COUNT];

    struct {
        rb_atomic_t finalizing;
    } atomic_flags;

    mark_stack_t mark_stack;
    size_t marked_slots;

    struct {
        struct heap_page **sorted;
        size_t allocated_pages;
        size_t allocatable_pages;
        size_t sorted_length;
        uintptr_t range[2];
        size_t freeable_pages;

        /* final */
        size_t final_slots;
        VALUE deferred_final;
    } heap_pages;

    st_table *finalizer_table;

    VALUE gc_stress_mode;

    struct {
        VALUE parent_object;
        int need_major_gc;
        size_t last_major_gc;
        size_t uncollectible_wb_unprotected_objects;
        size_t uncollectible_wb_unprotected_objects_limit;
        size_t old_objects;
        size_t old_objects_limit;
    } rgengc;

    struct {
        size_t considered_count_table[T_MASK];
        size_t moved_count_table[T_MASK];
        size_t moved_up_count_table[T_MASK];
        size_t moved_down_count_table[T_MASK];
        size_t total_moved;
    } rcompactor;

    struct {
        size_t pooled_slots;
        size_t step_slots;
    } rincgc;

    st_table *id_to_obj_tbl;
    st_table *obj_to_id_tbl;

    unsigned long live_ractor_cache_count;

#ifdef RUBY_ASAN_ENABLED
    rb_execution_context_t *marking_machine_context_ec;
#endif

} rb_objspace_t;

void *
rb_gc_impl_objspace_alloc(void)
{
    fprintf(stderr, "gc_impl: objspace_alloc\n");
    return calloc(1, sizeof(rb_objspace_t));
}

void
rb_gc_impl_objspace_init(void *objspace_ptr)
{
    fprintf(stderr, "gc_impl: objspace_init\n");
    rb_objspace_t *objspace = objspace_ptr;

    objspace->flags.gc_stressful = 0;
    objspace->gc_stress_mode = 0;
    objspace->flags.measure_gc = false;
    objspace->malloc_params.limit = 0;

    rb_size_pool_t *size_pool = &objspace->size_pools[0];
    size_pool->slot_size = 0;
    ccan_list_head_init(&size_pool->eden_heap.pages);
    ccan_list_head_init(&size_pool->tomb_heap.pages);

    objspace->next_object_id = 1;
    objspace->id_to_obj_tbl = st_init_table(&object_id_hash_type);
    objspace->obj_to_id_tbl = st_init_numtable();
}

void
rb_gc_impl_objspace_free(void *objspace_ptr)
{
}

void *
rb_gc_impl_ractor_cache_alloc(void *objspace_ptr)
{
    return NULL;
}

void
rb_gc_impl_ractor_cache_free(void *objspace_ptr, void *cache)
{
}

void
rb_gc_impl_set_params(void *objspace_ptr)
{
}

void
rb_gc_impl_init(void)
{
}

int
rb_gc_impl_heap_count(void *objspace_ptr)
{
    return 0;
}

void
rb_gc_impl_initial_stress_set(VALUE flag)
{
}

// Shutdown
void
rb_gc_impl_shutdown_free_objects(void *objspace_ptr)
{
}

// GC
void
rb_gc_impl_start(void *objspace_ptr, bool full_mark, bool immediate_mark, bool immediate_sweep, bool compact)
{
}

bool
rb_gc_impl_during_gc_p(void *objspace_ptr)
{
    return false;
}

void
rb_gc_impl_prepare_heap(void *objspace_ptr)
{
}

void
rb_gc_impl_gc_enable(void *objspace_ptr)
{
}

void
rb_gc_impl_gc_disable(void *objspace_ptr, bool finish_current_gc)
{
}

bool
rb_gc_impl_gc_enabled_p(void *objspace_ptr)
{
    return false;
}

void
rb_gc_impl_stress_set(void *objspace_ptr, VALUE flag)
{
}

VALUE
rb_gc_impl_stress_get(void *objspace_ptr)
{
    return Qnil;
}

void
rb_gc_impl_auto_compact_disable(void *objspace_ptr)
{
}

// Object allocation
VALUE
rb_gc_impl_new_obj(void *objspace_ptr, void *cache_ptr, VALUE klass, VALUE flags, VALUE v1, VALUE v2, VALUE v3, bool wb_protected, size_t alloc_size)
{
    void *obj = malloc(alloc_size);
    memset(obj, 0, alloc_size);
    RBASIC(obj)->flags = flags;
    *((VALUE *)&RBASIC(obj)->klass) = klass;
    VALUE *p = (VALUE *)obj;
    p[2] = v1;
    p[3] = v2;
    p[4] = v3;

    return (VALUE)obj;
}

size_t
rb_gc_impl_obj_slot_size(VALUE obj)
{
    return 0;
}

size_t
rb_gc_impl_size_pool_id_for_size(void *objspace_ptr, size_t size)
{
    return 0;
}

// Malloc
void *
rb_gc_impl_malloc(void *objspace_ptr, size_t size)
{
    return malloc(size);
}

void *
rb_gc_impl_calloc(void *objspace_ptr, size_t size)
{
    return calloc(1, size);
}

void *
rb_gc_impl_realloc(void *objspace_ptr, void *ptr, size_t new_size, size_t old_size)
{
    return realloc(ptr, new_size);
}

void
rb_gc_impl_free(void *objspace_ptr, void *ptr, size_t old_size)
{
    return free(ptr);
}

void
rb_gc_impl_adjust_memory_usage(void *objspace_ptr, ssize_t diff)
{
}

// Marking
void
rb_gc_impl_mark(void *objspace_ptr, VALUE obj)
{
}

void
rb_gc_impl_mark_and_move(void *objspace_ptr, VALUE *ptr)
{
}

void
rb_gc_impl_mark_and_pin(void *objspace_ptr, VALUE obj)
{
}

void
rb_gc_impl_mark_maybe(void *objspace_ptr, VALUE obj)
{
}

void
rb_gc_impl_mark_weak(void *objspace_ptr, VALUE *ptr)
{
}

void
rb_gc_impl_remove_weak(void *objspace_ptr, VALUE parent_obj, VALUE *ptr)
{
}

void
rb_gc_impl_objspace_mark(void *objspace_ptr)
{
}

// Compaction
bool
rb_gc_impl_object_moved_p(void *objspace_ptr, VALUE obj)
{
    return false;
}

VALUE
rb_gc_impl_location(void *objspace_ptr, VALUE value)
{
    return 0;
}

// Write barriers
void
rb_gc_impl_writebarrier(void *objspace_ptr, VALUE a, VALUE b)
{
}

void
rb_gc_impl_writebarrier_unprotect(void *objspace_ptr, VALUE obj)
{
}

void
rb_gc_impl_writebarrier_remember(void *objspace_ptr, VALUE obj)
{
}

// Heap walking
void
rb_gc_impl_each_objects(void *objspace_ptr, int callback(void *, void *, size_t, void *), void *data)
{
}

void
rb_gc_impl_each_object(void *objspace_ptr, void func(VALUE obj, void *data), void *data)
{
}

// Finalizers
void
rb_gc_impl_make_zombie(void *objspace_ptr, VALUE obj, void dfree(void *), void *data)
{
}

VALUE
rb_gc_impl_define_finalizer(void *objspace_ptr, VALUE obj, VALUE block)
{
    return Qnil;
}

VALUE
rb_gc_impl_undefine_finalizer(void *objspace_ptr, VALUE obj)
{
    return Qnil;
}

void
rb_gc_impl_copy_finalizer(void *objspace_ptr, VALUE dest, VALUE obj)
{
}

void
rb_gc_impl_shutdown_call_finalizer(void *objspace_ptr)
{
}

// Object ID
VALUE
rb_gc_impl_object_id(void *objspace_ptr, VALUE obj)
{
    return Qnil;
}

VALUE
rb_gc_impl_object_id_to_ref(void *objspace_ptr, VALUE object_id)
{
    return Qnil;
}

// Statistics
VALUE
rb_gc_impl_set_measure_total_time(void *objspace_ptr, VALUE flag)
{
    return Qnil;
}

VALUE
rb_gc_impl_get_measure_total_time(void *objspace_ptr)
{
    return Qnil;
}

VALUE
rb_gc_impl_get_profile_total_time(void *objspace_ptr)
{
    return Qnil;
}

size_t
rb_gc_impl_gc_count(void *objspace_ptr)
{
    return 0;
}

VALUE
rb_gc_impl_latest_gc_info(void *objspace_ptr, VALUE key)
{
    return Qnil;
}

size_t
rb_gc_impl_stat(void *objspace_ptr, VALUE hash_or_sym)
{
    return 0;
}

size_t
rb_gc_impl_stat_heap(void *objspace_ptr, int size_pool_idx, VALUE hash_or_sym)
{
    return 0;
}

// Miscellaneous
size_t
rb_gc_impl_obj_flags(void *objspace_ptr, VALUE obj, ID* flags, size_t max)
{
    return 0;
}

bool
rb_gc_impl_pointer_to_heap_p(void *objspace_ptr, const void *ptr)
{
    return false;
}

bool
rb_gc_impl_garbage_object_p(void *objspace_ptr, VALUE obj)
{
    return false;
}

void
rb_gc_impl_set_event_hook(void *objspace_ptr, const rb_event_flag_t event)
{
}

void
rb_gc_impl_copy_attributes(void *objspace_ptr, VALUE dest, VALUE obj)
{
}
