#include "ruby/internal/config.h"

#include <signal.h>

#ifndef _WIN32
# include <sys/mman.h>
# include <unistd.h>
#endif

#if !defined(PAGE_SIZE) && defined(HAVE_SYS_USER_H)
/* LIST_HEAD conflicts with sys/queue.h on macOS */
# include <sys/user.h>
#endif

#include "ruby/ruby.h"
#include "ruby/atomic.h"
#include "ruby/debug.h"
#include "ruby/thread.h"
#include "ruby/util.h"
#include "ruby/vm.h"
#include "ruby/internal/encoding/string.h"
#include "ccan/list/list.h"
#include "darray.h"
#include "probes.h"

#include "debug_counter.h"
#include "internal/sanitizers.h"

#ifdef HAVE_MALLOC_USABLE_SIZE
# ifdef RUBY_ALTERNATIVE_MALLOC_HEADER
/* Alternative malloc header is included in ruby/missing.h */
# elif defined(HAVE_MALLOC_H)
#  include <malloc.h>
# elif defined(HAVE_MALLOC_NP_H)
#  include <malloc_np.h>
# elif defined(HAVE_MALLOC_MALLOC_H)
#  include <malloc/malloc.h>
# endif
#endif

#ifdef HAVE_MALLOC_TRIM
# include <malloc.h>

# ifdef __EMSCRIPTEN__
/* malloc_trim is defined in emscripten/emmalloc.h on emscripten. */
#  include <emscripten/emmalloc.h>
# endif
#endif

#ifdef HAVE_MACH_TASK_EXCEPTION_PORTS
# include <mach/task.h>
# include <mach/mach_init.h>
# include <mach/mach_port.h>
#endif

/* Headers from gc.c */
unsigned int rb_gc_vm_lock(void);
void rb_gc_vm_unlock(unsigned int lev);
unsigned int rb_gc_cr_lock(void);
void rb_gc_cr_unlock(unsigned int lev);
unsigned int rb_gc_vm_lock_no_barrier(void);
void rb_gc_vm_unlock_no_barrier(unsigned int lev);
void rb_gc_vm_barrier(void);
size_t rb_gc_obj_optimal_size(VALUE obj);
void rb_gc_mark_children(void *objspace, VALUE obj);
void rb_gc_update_object_references(void *objspace, VALUE obj);
void rb_gc_update_vm_references(void *objspace);
void rb_gc_reachable_objects_from_callback(VALUE obj);
void rb_gc_event_hook(VALUE obj, rb_event_flag_t event);
void *rb_gc_get_objspace(void);
size_t rb_size_mul_or_raise(size_t x, size_t y, VALUE exc);
void rb_gc_run_obj_finalizer(VALUE objid, long count, VALUE (*callback)(long i, void *data), void *data);
void rb_gc_set_pending_interrupt(void);
void rb_gc_unset_pending_interrupt(void);
bool rb_gc_obj_free(void *objspace, VALUE obj);
void rb_gc_mark_roots(void *objspace, const char **categoryp);
void rb_gc_ractor_newobj_cache_foreach(void (*func)(void *cache, void *data), void *data);
bool rb_gc_multi_ractor_p(void);
void rb_objspace_reachable_objects_from_root(void (func)(const char *category, VALUE, void *), void *passing_data);
void rb_objspace_reachable_objects_from(VALUE obj, void (func)(VALUE, void *), void *data);
void rb_obj_info_dump(VALUE obj);
const char *rb_obj_info(VALUE obj);
bool rb_gc_shutdown_call_finalizer_p(VALUE obj);
uint32_t rb_gc_get_shape(VALUE obj);
void rb_gc_set_shape(VALUE obj, uint32_t shape_id);
uint32_t rb_gc_rebuild_shape(VALUE obj, size_t size_pool_id);
size_t rb_obj_memsize_of(VALUE obj);

void rb_ractor_finish_marking(void);

#ifndef VM_CHECK_MODE
# define VM_CHECK_MODE RUBY_DEBUG
#endif

// From ractor_core.h
#ifndef RACTOR_CHECK_MODE
# define RACTOR_CHECK_MODE (VM_CHECK_MODE || RUBY_DEBUG) && (SIZEOF_UINT64_T == SIZEOF_VALUE)
#endif

#ifndef RUBY_DEBUG_LOG
# define RUBY_DEBUG_LOG(...)
#endif

#ifndef GC_HEAP_INIT_SLOTS
#define GC_HEAP_INIT_SLOTS 10000
#endif
#ifndef GC_HEAP_FREE_SLOTS
#define GC_HEAP_FREE_SLOTS  4096
#endif
#ifndef GC_HEAP_GROWTH_FACTOR
#define GC_HEAP_GROWTH_FACTOR 1.8
#endif
#ifndef GC_HEAP_GROWTH_MAX_SLOTS
#define GC_HEAP_GROWTH_MAX_SLOTS 0 /* 0 is disable */
#endif
#ifndef GC_HEAP_REMEMBERED_WB_UNPROTECTED_OBJECTS_LIMIT_RATIO
# define GC_HEAP_REMEMBERED_WB_UNPROTECTED_OBJECTS_LIMIT_RATIO 0.01
#endif
#ifndef GC_HEAP_OLDOBJECT_LIMIT_FACTOR
#define GC_HEAP_OLDOBJECT_LIMIT_FACTOR 2.0
#endif

#ifndef GC_HEAP_FREE_SLOTS_MIN_RATIO
#define GC_HEAP_FREE_SLOTS_MIN_RATIO  0.20
#endif
#ifndef GC_HEAP_FREE_SLOTS_GOAL_RATIO
#define GC_HEAP_FREE_SLOTS_GOAL_RATIO 0.40
#endif
#ifndef GC_HEAP_FREE_SLOTS_MAX_RATIO
#define GC_HEAP_FREE_SLOTS_MAX_RATIO  0.65
#endif

#ifndef GC_MALLOC_LIMIT_MIN
#define GC_MALLOC_LIMIT_MIN (16 * 1024 * 1024 /* 16MB */)
#endif
#ifndef GC_MALLOC_LIMIT_MAX
#define GC_MALLOC_LIMIT_MAX (32 * 1024 * 1024 /* 32MB */)
#endif
#ifndef GC_MALLOC_LIMIT_GROWTH_FACTOR
#define GC_MALLOC_LIMIT_GROWTH_FACTOR 1.4
#endif

#ifndef GC_OLDMALLOC_LIMIT_MIN
#define GC_OLDMALLOC_LIMIT_MIN (16 * 1024 * 1024 /* 16MB */)
#endif
#ifndef GC_OLDMALLOC_LIMIT_GROWTH_FACTOR
#define GC_OLDMALLOC_LIMIT_GROWTH_FACTOR 1.2
#endif
#ifndef GC_OLDMALLOC_LIMIT_MAX
#define GC_OLDMALLOC_LIMIT_MAX (128 * 1024 * 1024 /* 128MB */)
#endif


#ifndef GC_CAN_COMPILE_COMPACTION
# define GC_CAN_COMPILE_COMPACTION 0
#endif

#ifndef PRINT_ENTER_EXIT_TICK
# define PRINT_ENTER_EXIT_TICK 0
#endif
#ifndef PRINT_ROOT_TICKS
#define PRINT_ROOT_TICKS 0
#endif

#define USE_TICK_T                 (PRINT_ENTER_EXIT_TICK || PRINT_ROOT_TICKS)

#ifndef SIZE_POOL_COUNT
# define SIZE_POOL_COUNT 5
#endif

typedef struct ractor_newobj_size_pool_cache {
    struct free_slot *freelist;
    struct heap_page *using_page;
} rb_ractor_newobj_size_pool_cache_t;

typedef struct ractor_newobj_cache {
    size_t incremental_mark_step_allocated_slots;
    rb_ractor_newobj_size_pool_cache_t size_pool_caches[SIZE_POOL_COUNT];
} rb_ractor_newobj_cache_t;

typedef struct {
    size_t size_pool_init_slots[SIZE_POOL_COUNT];
    size_t heap_free_slots;
    double growth_factor;
    size_t growth_max_slots;

    double heap_free_slots_min_ratio;
    double heap_free_slots_goal_ratio;
    double heap_free_slots_max_ratio;
    double uncollectible_wb_unprotected_objects_limit_ratio;
    double oldobject_limit_factor;

    size_t malloc_limit_min;
    size_t malloc_limit_max;
    double malloc_limit_growth_factor;

    size_t oldmalloc_limit_min;
    size_t oldmalloc_limit_max;
    double oldmalloc_limit_growth_factor;

    VALUE gc_stress;
} ruby_gc_params_t;

static ruby_gc_params_t gc_params = {
    { 0 },
    GC_HEAP_FREE_SLOTS,
    GC_HEAP_GROWTH_FACTOR,
    GC_HEAP_GROWTH_MAX_SLOTS,

    GC_HEAP_FREE_SLOTS_MIN_RATIO,
    GC_HEAP_FREE_SLOTS_GOAL_RATIO,
    GC_HEAP_FREE_SLOTS_MAX_RATIO,
    GC_HEAP_REMEMBERED_WB_UNPROTECTED_OBJECTS_LIMIT_RATIO,
    GC_HEAP_OLDOBJECT_LIMIT_FACTOR,

    GC_MALLOC_LIMIT_MIN,
    GC_MALLOC_LIMIT_MAX,
    GC_MALLOC_LIMIT_GROWTH_FACTOR,

    GC_OLDMALLOC_LIMIT_MIN,
    GC_OLDMALLOC_LIMIT_MAX,
    GC_OLDMALLOC_LIMIT_GROWTH_FACTOR,

    FALSE,
};

/* GC_DEBUG:
 *  enable to embed GC debugging information.
 */
#ifndef GC_DEBUG
#define GC_DEBUG 0
#endif

/* RGENGC_DEBUG:
 * 1: basic information
 * 2: remember set operation
 * 3: mark
 * 4:
 * 5: sweep
 */
#ifndef RGENGC_DEBUG
#ifdef RUBY_DEVEL
#define RGENGC_DEBUG       -1
#else
#define RGENGC_DEBUG       0
#endif
#endif
#if RGENGC_DEBUG < 0 && !defined(_MSC_VER)
# define RGENGC_DEBUG_ENABLED(level) (-(RGENGC_DEBUG) >= (level) && ruby_rgengc_debug >= (level))
#elif defined(HAVE_VA_ARGS_MACRO)
# define RGENGC_DEBUG_ENABLED(level) ((RGENGC_DEBUG) >= (level))
#else
# define RGENGC_DEBUG_ENABLED(level) 0
#endif
int ruby_rgengc_debug;

/* RGENGC_CHECK_MODE
 * 0: disable all assertions
 * 1: enable assertions (to debug RGenGC)
 * 2: enable internal consistency check at each GC (for debugging)
 * 3: enable internal consistency check at each GC steps (for debugging)
 * 4: enable liveness check
 * 5: show all references
 */
#ifndef RGENGC_CHECK_MODE
# define RGENGC_CHECK_MODE  0
#endif

// Note: using RUBY_ASSERT_WHEN() extend a macro in expr (info by nobu).
#define GC_ASSERT(expr) RUBY_ASSERT_MESG_WHEN(RGENGC_CHECK_MODE > 0, expr, #expr)

/* RGENGC_PROFILE
 * 0: disable RGenGC profiling
 * 1: enable profiling for basic information
 * 2: enable profiling for each types
 */
#ifndef RGENGC_PROFILE
# define RGENGC_PROFILE     0
#endif

/* RGENGC_ESTIMATE_OLDMALLOC
 * Enable/disable to estimate increase size of malloc'ed size by old objects.
 * If estimation exceeds threshold, then will invoke full GC.
 * 0: disable estimation.
 * 1: enable estimation.
 */
#ifndef RGENGC_ESTIMATE_OLDMALLOC
# define RGENGC_ESTIMATE_OLDMALLOC 1
#endif

/* RGENGC_FORCE_MAJOR_GC
 * Force major/full GC if this macro is not 0.
 */
#ifndef RGENGC_FORCE_MAJOR_GC
# define RGENGC_FORCE_MAJOR_GC 0
#endif

#ifndef GC_PROFILE_MORE_DETAIL
# define GC_PROFILE_MORE_DETAIL 0
#endif
#ifndef GC_PROFILE_DETAIL_MEMORY
# define GC_PROFILE_DETAIL_MEMORY 0
#endif
#ifndef GC_ENABLE_LAZY_SWEEP
# define GC_ENABLE_LAZY_SWEEP   1
#endif
#ifndef CALC_EXACT_MALLOC_SIZE
# define CALC_EXACT_MALLOC_SIZE USE_GC_MALLOC_OBJ_INFO_DETAILS
#endif
#if defined(HAVE_MALLOC_USABLE_SIZE) || CALC_EXACT_MALLOC_SIZE > 0
# ifndef MALLOC_ALLOCATED_SIZE
#  define MALLOC_ALLOCATED_SIZE 0
# endif
#else
# define MALLOC_ALLOCATED_SIZE 0
#endif
#ifndef MALLOC_ALLOCATED_SIZE_CHECK
# define MALLOC_ALLOCATED_SIZE_CHECK 0
#endif

#ifndef GC_DEBUG_STRESS_TO_CLASS
# define GC_DEBUG_STRESS_TO_CLASS RUBY_DEBUG
#endif

typedef enum {
    GPR_FLAG_NONE               = 0x000,
    /* major reason */
    GPR_FLAG_MAJOR_BY_NOFREE    = 0x001,
    GPR_FLAG_MAJOR_BY_OLDGEN    = 0x002,
    GPR_FLAG_MAJOR_BY_SHADY     = 0x004,
    GPR_FLAG_MAJOR_BY_FORCE     = 0x008,
#if RGENGC_ESTIMATE_OLDMALLOC
    GPR_FLAG_MAJOR_BY_OLDMALLOC = 0x020,
#endif
    GPR_FLAG_MAJOR_MASK         = 0x0ff,

    /* gc reason */
    GPR_FLAG_NEWOBJ             = 0x100,
    GPR_FLAG_MALLOC             = 0x200,
    GPR_FLAG_METHOD             = 0x400,
    GPR_FLAG_CAPI               = 0x800,
    GPR_FLAG_STRESS            = 0x1000,

    /* others */
    GPR_FLAG_IMMEDIATE_SWEEP   = 0x2000,
    GPR_FLAG_HAVE_FINALIZE     = 0x4000,
    GPR_FLAG_IMMEDIATE_MARK    = 0x8000,
    GPR_FLAG_FULL_MARK        = 0x10000,
    GPR_FLAG_COMPACT          = 0x20000,

    GPR_DEFAULT_REASON =
        (GPR_FLAG_FULL_MARK | GPR_FLAG_IMMEDIATE_MARK |
         GPR_FLAG_IMMEDIATE_SWEEP | GPR_FLAG_CAPI),
} gc_profile_record_flag;

typedef struct gc_profile_record {
    unsigned int flags;

    double gc_time;
    double gc_invoke_time;

    size_t heap_total_objects;
    size_t heap_use_size;
    size_t heap_total_size;
    size_t moved_objects;

#if GC_PROFILE_MORE_DETAIL
    double gc_mark_time;
    double gc_sweep_time;

    size_t heap_use_pages;
    size_t heap_live_objects;
    size_t heap_free_objects;

    size_t allocate_increase;
    size_t allocate_limit;

    double prepare_time;
    size_t removing_objects;
    size_t empty_objects;
#if GC_PROFILE_DETAIL_MEMORY
    long maxrss;
    long minflt;
    long majflt;
#endif
#endif
#if MALLOC_ALLOCATED_SIZE
    size_t allocated_size;
#endif

#if RGENGC_PROFILE > 0
    size_t old_objects;
    size_t remembered_normal_objects;
    size_t remembered_shady_objects;
#endif
} gc_profile_record;

struct RMoved {
    VALUE flags;
    VALUE dummy;
    VALUE destination;
    uint32_t original_shape_id;
};

#define RMOVED(obj) ((struct RMoved *)(obj))

typedef uintptr_t bits_t;
enum {
    BITS_SIZE = sizeof(bits_t),
    BITS_BITLENGTH = ( BITS_SIZE * CHAR_BIT )
};

struct heap_page_header {
    struct heap_page *page;
};

struct heap_page_body {
    struct heap_page_header header;
    /* char gap[];      */
    /* RVALUE values[]; */
};

#define STACK_CHUNK_SIZE 500

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

#define SIZE_POOL_EDEN_HEAP(size_pool) (&(size_pool)->eden_heap)
#define SIZE_POOL_TOMB_HEAP(size_pool) (&(size_pool)->tomb_heap)

typedef int (*gc_compact_compare_func)(const void *l, const void *r, void *d);

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

enum {
    gc_stress_no_major,
    gc_stress_no_immediate_sweep,
    gc_stress_full_mark_after_malloc,
    gc_stress_max
};

enum gc_mode {
    gc_mode_none,
    gc_mode_marking,
    gc_mode_sweeping,
    gc_mode_compacting,
};

typedef struct rb_objspace {
    struct {
        size_t limit;
        size_t increase;
#if MALLOC_ALLOCATED_SIZE
        size_t allocated_size;
        size_t allocations;
#endif
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

    struct {
        int run;
        unsigned int latest_gc_info;
        gc_profile_record *records;
        gc_profile_record *current_record;
        size_t next_index;
        size_t size;

#if GC_PROFILE_MORE_DETAIL
        double prepare_time;
#endif
        double invoke_time;

        size_t minor_gc_count;
        size_t major_gc_count;
        size_t compact_count;
        size_t read_barrier_faults;
#if RGENGC_PROFILE > 0
        size_t total_generated_normal_object_count;
        size_t total_generated_shady_object_count;
        size_t total_shade_operation_count;
        size_t total_promoted_count;
        size_t total_remembered_normal_object_count;
        size_t total_remembered_shady_object_count;

#if RGENGC_PROFILE >= 2
        size_t generated_normal_object_count_types[RUBY_T_MASK];
        size_t generated_shady_object_count_types[RUBY_T_MASK];
        size_t shade_operation_count_types[RUBY_T_MASK];
        size_t promoted_types[RUBY_T_MASK];
        size_t remembered_normal_object_count_types[RUBY_T_MASK];
        size_t remembered_shady_object_count_types[RUBY_T_MASK];
#endif
#endif /* RGENGC_PROFILE */

        /* temporary profiling space */
        double gc_sweep_start_time;
        size_t total_allocated_objects_at_gc_start;
        size_t heap_used_at_gc_start;

        /* basic statistics */
        size_t count;
        uint64_t marking_time_ns;
        struct timespec marking_start_time;
        uint64_t sweeping_time_ns;
        struct timespec sweeping_start_time;

        /* Weak references */
        size_t weak_references_count;
        size_t retained_weak_references_count;
    } profile;

    VALUE gc_stress_mode;

    struct {
        VALUE parent_object;
        int need_major_gc;
        size_t last_major_gc;
        size_t uncollectible_wb_unprotected_objects;
        size_t uncollectible_wb_unprotected_objects_limit;
        size_t old_objects;
        size_t old_objects_limit;

#if RGENGC_ESTIMATE_OLDMALLOC
        size_t oldmalloc_increase;
        size_t oldmalloc_increase_limit;
#endif

#if RGENGC_CHECK_MODE >= 2
        struct st_table *allrefs_table;
        size_t error_count;
#endif
    } rgengc;

    struct {
        size_t considered_count_table[T_MASK];
        size_t moved_count_table[T_MASK];
        size_t moved_up_count_table[T_MASK];
        size_t moved_down_count_table[T_MASK];
        size_t total_moved;

        /* This function will be used, if set, to sort the heap prior to compaction */
        gc_compact_compare_func compare_func;
    } rcompactor;

    struct {
        size_t pooled_slots;
        size_t step_slots;
    } rincgc;

    st_table *id_to_obj_tbl;
    st_table *obj_to_id_tbl;

#if GC_DEBUG_STRESS_TO_CLASS
    VALUE stress_to_class;
#endif

    rb_darray(VALUE *) weak_references;
    rb_postponed_job_handle_t finalize_deferred_pjob;

    unsigned long live_ractor_cache_count;

#ifdef RUBY_ASAN_ENABLED
    rb_execution_context_t *marking_machine_context_ec;
#endif

} rb_objspace_t;

#ifndef HEAP_PAGE_ALIGN_LOG
/* default tiny heap size: 64KiB */
#define HEAP_PAGE_ALIGN_LOG 16
#endif

#if RACTOR_CHECK_MODE || GC_DEBUG
struct rvalue_overhead {
# if RACTOR_CHECK_MODE
    uint32_t _ractor_belonging_id;
# endif
# if GC_DEBUG
    const char *file;
    int line;
# endif
};

// Make sure that RVALUE_OVERHEAD aligns to sizeof(VALUE)
# define RVALUE_OVERHEAD (sizeof(struct { \
    union { \
        struct rvalue_overhead overhead; \
        VALUE value; \
    }; \
}))
# define GET_RVALUE_OVERHEAD(obj) ((struct rvalue_overhead *)((uintptr_t)obj + rb_gc_obj_slot_size(obj)))
#else
# define RVALUE_OVERHEAD 0
#endif

#define BASE_SLOT_SIZE (sizeof(struct RBasic) + sizeof(VALUE[RBIMPL_RVALUE_EMBED_LEN_MAX]) + RVALUE_OVERHEAD)

#ifndef MAX
# define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
# define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#define roomof(x, y) (((x) + (y) - 1) / (y))
#define CEILDIV(i, mod) roomof(i, mod)
enum {
    HEAP_PAGE_ALIGN = (1UL << HEAP_PAGE_ALIGN_LOG),
    HEAP_PAGE_ALIGN_MASK = (~(~0UL << HEAP_PAGE_ALIGN_LOG)),
    HEAP_PAGE_SIZE = HEAP_PAGE_ALIGN,
    HEAP_PAGE_OBJ_LIMIT = (unsigned int)((HEAP_PAGE_SIZE - sizeof(struct heap_page_header)) / BASE_SLOT_SIZE),
    HEAP_PAGE_BITMAP_LIMIT = CEILDIV(CEILDIV(HEAP_PAGE_SIZE, BASE_SLOT_SIZE), BITS_BITLENGTH),
    HEAP_PAGE_BITMAP_SIZE = (BITS_SIZE * HEAP_PAGE_BITMAP_LIMIT),
};
#define HEAP_PAGE_ALIGN (1 << HEAP_PAGE_ALIGN_LOG)
#define HEAP_PAGE_SIZE HEAP_PAGE_ALIGN

#if !defined(INCREMENTAL_MARK_STEP_ALLOCATIONS)
# define INCREMENTAL_MARK_STEP_ALLOCATIONS 500
#endif

#undef INIT_HEAP_PAGE_ALLOC_USE_MMAP
/* Must define either HEAP_PAGE_ALLOC_USE_MMAP or
 * INIT_HEAP_PAGE_ALLOC_USE_MMAP. */

#ifndef HAVE_MMAP
/* We can't use mmap of course, if it is not available. */
static const bool HEAP_PAGE_ALLOC_USE_MMAP = false;

#elif defined(__wasm__)
/* wasmtime does not have proper support for mmap.
 * See https://github.com/bytecodealliance/wasmtime/blob/main/docs/WASI-rationale.md#why-no-mmap-and-friends
 */
static const bool HEAP_PAGE_ALLOC_USE_MMAP = false;

#elif HAVE_CONST_PAGE_SIZE
/* If we have the PAGE_SIZE and it is a constant, then we can directly use it. */
static const bool HEAP_PAGE_ALLOC_USE_MMAP = (PAGE_SIZE <= HEAP_PAGE_SIZE);

#elif defined(PAGE_MAX_SIZE) && (PAGE_MAX_SIZE <= HEAP_PAGE_SIZE)
/* If we can use the maximum page size. */
static const bool HEAP_PAGE_ALLOC_USE_MMAP = true;

#elif defined(PAGE_SIZE)
/* If the PAGE_SIZE macro can be used dynamically. */
# define INIT_HEAP_PAGE_ALLOC_USE_MMAP (PAGE_SIZE <= HEAP_PAGE_SIZE)

#elif defined(HAVE_SYSCONF) && defined(_SC_PAGE_SIZE)
/* If we can use sysconf to determine the page size. */
# define INIT_HEAP_PAGE_ALLOC_USE_MMAP (sysconf(_SC_PAGE_SIZE) <= HEAP_PAGE_SIZE)

#else
/* Otherwise we can't determine the system page size, so don't use mmap. */
static const bool HEAP_PAGE_ALLOC_USE_MMAP = false;
#endif

#ifdef INIT_HEAP_PAGE_ALLOC_USE_MMAP
/* We can determine the system page size at runtime. */
# define HEAP_PAGE_ALLOC_USE_MMAP (heap_page_alloc_use_mmap != false)

static bool heap_page_alloc_use_mmap;
#endif

#define RVALUE_AGE_BIT_COUNT 2
#define RVALUE_AGE_BIT_MASK (((bits_t)1 << RVALUE_AGE_BIT_COUNT) - 1)
#define RVALUE_OLD_AGE   3

struct free_slot {
    VALUE flags;		/* always 0 for freed obj */
    struct free_slot *next;
};

struct heap_page {
    short slot_size;
    short total_slots;
    short free_slots;
    short final_slots;
    short pinned_slots;
    struct {
        unsigned int before_sweep : 1;
        unsigned int has_remembered_objects : 1;
        unsigned int has_uncollectible_wb_unprotected_objects : 1;
        unsigned int in_tomb : 1;
    } flags;

    rb_size_pool_t *size_pool;

    struct heap_page *free_next;
    uintptr_t start;
    struct free_slot *freelist;
    struct ccan_list_node page_node;

    bits_t wb_unprotected_bits[HEAP_PAGE_BITMAP_LIMIT];
    /* the following three bitmaps are cleared at the beginning of full GC */
    bits_t mark_bits[HEAP_PAGE_BITMAP_LIMIT];
    bits_t uncollectible_bits[HEAP_PAGE_BITMAP_LIMIT];
    bits_t marking_bits[HEAP_PAGE_BITMAP_LIMIT];

    bits_t remembered_bits[HEAP_PAGE_BITMAP_LIMIT];

    /* If set, the object is not movable */
    bits_t pinned_bits[HEAP_PAGE_BITMAP_LIMIT];
    bits_t age_bits[HEAP_PAGE_BITMAP_LIMIT * RVALUE_AGE_BIT_COUNT];
};

/*
 * When asan is enabled, this will prohibit writing to the freelist until it is unlocked
 */
static void
asan_lock_freelist(struct heap_page *page)
{
    asan_poison_memory_region(&page->freelist, sizeof(struct free_list *));
}

/*
 * When asan is enabled, this will enable the ability to write to the freelist
 */
static void
asan_unlock_freelist(struct heap_page *page)
{
    asan_unpoison_memory_region(&page->freelist, sizeof(struct free_list *), false);
}

#define GET_PAGE_BODY(x)   ((struct heap_page_body *)((bits_t)(x) & ~(HEAP_PAGE_ALIGN_MASK)))
#define GET_PAGE_HEADER(x) (&GET_PAGE_BODY(x)->header)
#define GET_HEAP_PAGE(x)   (GET_PAGE_HEADER(x)->page)

#define NUM_IN_PAGE(p)   (((bits_t)(p) & HEAP_PAGE_ALIGN_MASK) / BASE_SLOT_SIZE)
#define BITMAP_INDEX(p)  (NUM_IN_PAGE(p) / BITS_BITLENGTH )
#define BITMAP_OFFSET(p) (NUM_IN_PAGE(p) & (BITS_BITLENGTH-1))
#define BITMAP_BIT(p)    ((bits_t)1 << BITMAP_OFFSET(p))

/* Bitmap Operations */
#define MARKED_IN_BITMAP(bits, p)    ((bits)[BITMAP_INDEX(p)] & BITMAP_BIT(p))
#define MARK_IN_BITMAP(bits, p)      ((bits)[BITMAP_INDEX(p)] = (bits)[BITMAP_INDEX(p)] | BITMAP_BIT(p))
#define CLEAR_IN_BITMAP(bits, p)     ((bits)[BITMAP_INDEX(p)] = (bits)[BITMAP_INDEX(p)] & ~BITMAP_BIT(p))

/* getting bitmap */
#define GET_HEAP_MARK_BITS(x)           (&GET_HEAP_PAGE(x)->mark_bits[0])
#define GET_HEAP_PINNED_BITS(x)         (&GET_HEAP_PAGE(x)->pinned_bits[0])
#define GET_HEAP_UNCOLLECTIBLE_BITS(x)  (&GET_HEAP_PAGE(x)->uncollectible_bits[0])
#define GET_HEAP_WB_UNPROTECTED_BITS(x) (&GET_HEAP_PAGE(x)->wb_unprotected_bits[0])
#define GET_HEAP_MARKING_BITS(x)        (&GET_HEAP_PAGE(x)->marking_bits[0])

#define GC_SWEEP_PAGES_FREEABLE_PER_STEP 3

#define RVALUE_AGE_BITMAP_INDEX(n)  (NUM_IN_PAGE(n) / (BITS_BITLENGTH / RVALUE_AGE_BIT_COUNT))
#define RVALUE_AGE_BITMAP_OFFSET(n) ((NUM_IN_PAGE(n) % (BITS_BITLENGTH / RVALUE_AGE_BIT_COUNT)) * RVALUE_AGE_BIT_COUNT)

static int
RVALUE_AGE_GET(VALUE obj)
{
    bits_t *age_bits = GET_HEAP_PAGE(obj)->age_bits;
    return (int)(age_bits[RVALUE_AGE_BITMAP_INDEX(obj)] >> RVALUE_AGE_BITMAP_OFFSET(obj)) & RVALUE_AGE_BIT_MASK;
}

static void
RVALUE_AGE_SET(VALUE obj, int age)
{
    RUBY_ASSERT(age <= RVALUE_OLD_AGE);
    bits_t *age_bits = GET_HEAP_PAGE(obj)->age_bits;
    // clear the bits
    age_bits[RVALUE_AGE_BITMAP_INDEX(obj)] &= ~(RVALUE_AGE_BIT_MASK << (RVALUE_AGE_BITMAP_OFFSET(obj)));
    // shift the correct value in
    age_bits[RVALUE_AGE_BITMAP_INDEX(obj)] |= ((bits_t)age << RVALUE_AGE_BITMAP_OFFSET(obj));
    if (age == RVALUE_OLD_AGE) {
        RB_FL_SET_RAW(obj, RUBY_FL_PROMOTED);
    }
    else {
        RB_FL_UNSET_RAW(obj, RUBY_FL_PROMOTED);
    }
}

#define ruby_initial_gc_stress	gc_params.gc_stress

VALUE *ruby_initial_gc_stress_ptr = &ruby_initial_gc_stress;

#define malloc_limit		objspace->malloc_params.limit
#define malloc_increase 	objspace->malloc_params.increase
#define malloc_allocated_size 	objspace->malloc_params.allocated_size
#define heap_pages_sorted       objspace->heap_pages.sorted
#define heap_allocated_pages    objspace->heap_pages.allocated_pages
#define heap_pages_sorted_length objspace->heap_pages.sorted_length
#define heap_pages_lomem	objspace->heap_pages.range[0]
#define heap_pages_himem	objspace->heap_pages.range[1]
#define heap_pages_freeable_pages	objspace->heap_pages.freeable_pages
#define heap_pages_final_slots		objspace->heap_pages.final_slots
#define heap_pages_deferred_final	objspace->heap_pages.deferred_final
#define size_pools              objspace->size_pools
#define during_gc		objspace->flags.during_gc
#define finalizing		objspace->atomic_flags.finalizing
#define finalizer_table 	objspace->finalizer_table
#define ruby_gc_stressful	objspace->flags.gc_stressful
#define ruby_gc_stress_mode     objspace->gc_stress_mode
#if GC_DEBUG_STRESS_TO_CLASS
#define stress_to_class         objspace->stress_to_class
#define set_stress_to_class(c)  (stress_to_class = (c))
#else
#define stress_to_class         (objspace, 0)
#define set_stress_to_class(c)  (objspace, (c))
#endif

static inline enum gc_mode
gc_mode_verify(enum gc_mode mode)
{
#if RGENGC_CHECK_MODE > 0
    switch (mode) {
      case gc_mode_none:
      case gc_mode_marking:
      case gc_mode_sweeping:
      case gc_mode_compacting:
        break;
      default:
        rb_bug("gc_mode_verify: unreachable (%d)", (int)mode);
    }
#endif
    return mode;
}

static inline bool
has_sweeping_pages(rb_objspace_t *objspace)
{
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        if (SIZE_POOL_EDEN_HEAP(&size_pools[i])->sweeping_page) {
            return TRUE;
        }
    }
    return FALSE;
}

static inline size_t
heap_eden_total_pages(rb_objspace_t *objspace)
{
    size_t count = 0;
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        count += SIZE_POOL_EDEN_HEAP(&size_pools[i])->total_pages;
    }
    return count;
}

static inline size_t
heap_tomb_total_pages(rb_objspace_t *objspace)
{
    size_t count = 0;
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        count += SIZE_POOL_TOMB_HEAP(&size_pools[i])->total_pages;
    }
    return count;
}

static inline size_t
heap_allocatable_pages(rb_objspace_t *objspace)
{
    size_t count = 0;
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        count += size_pools[i].allocatable_pages;
    }
    return count;
}

static inline size_t
total_allocated_pages(rb_objspace_t *objspace)
{
    size_t count = 0;
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        rb_size_pool_t *size_pool = &size_pools[i];
        count += size_pool->total_allocated_pages;
    }
    return count;
}

static inline size_t
total_freed_pages(rb_objspace_t *objspace)
{
    size_t count = 0;
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        rb_size_pool_t *size_pool = &size_pools[i];
        count += size_pool->total_freed_pages;
    }
    return count;
}

static inline size_t
total_allocated_objects(rb_objspace_t *objspace)
{
    size_t count = 0;
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        rb_size_pool_t *size_pool = &size_pools[i];
        count += size_pool->total_allocated_objects;
    }
    return count;
}

static inline size_t
total_freed_objects(rb_objspace_t *objspace)
{
    size_t count = 0;
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        rb_size_pool_t *size_pool = &size_pools[i];
        count += size_pool->total_freed_objects;
    }
    return count;
}

#define gc_mode(objspace)                gc_mode_verify((enum gc_mode)(objspace)->flags.mode)
#define gc_mode_set(objspace, m)         ((objspace)->flags.mode = (unsigned int)gc_mode_verify(m))
#define gc_needs_major_flags objspace->rgengc.need_major_gc

#define is_marking(objspace)             (gc_mode(objspace) == gc_mode_marking)
#define is_sweeping(objspace)            (gc_mode(objspace) == gc_mode_sweeping)
#define is_full_marking(objspace)        ((objspace)->flags.during_minor_gc == FALSE)
#define is_incremental_marking(objspace) ((objspace)->flags.during_incremental_marking != FALSE)
#define will_be_incremental_marking(objspace) ((objspace)->rgengc.need_major_gc != GPR_FLAG_NONE)
#define GC_INCREMENTAL_SWEEP_SLOT_COUNT 2048
#define GC_INCREMENTAL_SWEEP_POOL_SLOT_COUNT 1024
#define is_lazy_sweeping(objspace)           (GC_ENABLE_LAZY_SWEEP && has_sweeping_pages(objspace))

#if SIZEOF_LONG == SIZEOF_VOIDP
# define obj_id_to_ref(objid) ((objid) ^ FIXNUM_FLAG) /* unset FIXNUM_FLAG */
#elif SIZEOF_LONG_LONG == SIZEOF_VOIDP
# define obj_id_to_ref(objid) (FIXNUM_P(objid) ? \
   ((objid) ^ FIXNUM_FLAG) : (NUM2PTR(objid) << 1))
#else
# error not supported
#endif

struct RZombie {
    struct RBasic basic;
    VALUE next;
    void (*dfree)(void *);
    void *data;
};

#define RZOMBIE(o) ((struct RZombie *)(o))

int ruby_disable_gc = 0;
int ruby_enable_autocompact = 0;
#if RGENGC_CHECK_MODE
gc_compact_compare_func ruby_autocompact_compare_func;
#endif

static void init_mark_stack(mark_stack_t *stack);

enum gc_enter_event {
    gc_enter_event_start,
    gc_enter_event_continue,
    gc_enter_event_rest,
    gc_enter_event_finalizer,
};

static inline void gc_mark(rb_objspace_t *objspace, VALUE ptr);
static inline void gc_pin(rb_objspace_t *objspace, VALUE ptr);
static inline void gc_mark_and_pin(rb_objspace_t *objspace, VALUE ptr);

NO_SANITIZE("memory", static inline bool is_pointer_to_heap(rb_objspace_t *objspace, const void *ptr));

static void rb_gc_impl_verify_internal_consistency(void *objspace_ptr);

static double getrusage_time(void);

#define gc_prof_record(objspace) (objspace)->profile.current_record
#define gc_prof_enabled(objspace) ((objspace)->profile.run && (objspace)->profile.current_record)

#ifdef HAVE_VA_ARGS_MACRO
# define gc_report(level, objspace, ...) \
    if (!RGENGC_DEBUG_ENABLED(level)) {} else gc_report_body(level, objspace, __VA_ARGS__)
#else
# define gc_report if (!RGENGC_DEBUG_ENABLED(0)) {} else gc_report_body
#endif
PRINTF_ARGS(static void gc_report_body(int level, rb_objspace_t *objspace, const char *fmt, ...), 3, 4);

static void gc_finalize_deferred(void *dmy);

#if USE_TICK_T

/* the following code is only for internal tuning. */

/* Source code to use RDTSC is quoted and modified from
 * https://www.mcs.anl.gov/~kazutomo/rdtsc.html
 * written by Kazutomo Yoshii <kazutomo@mcs.anl.gov>
 */

#if defined(__GNUC__) && defined(__i386__)
typedef unsigned long long tick_t;
#define PRItick "llu"
static inline tick_t
tick(void)
{
    unsigned long long int x;
    __asm__ __volatile__ ("rdtsc" : "=A" (x));
    return x;
}

#elif defined(__GNUC__) && defined(__x86_64__)
typedef unsigned long long tick_t;
#define PRItick "llu"

static __inline__ tick_t
tick(void)
{
    unsigned long hi, lo;
    __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)lo)|( ((unsigned long long)hi)<<32);
}

#elif defined(__powerpc64__) && (GCC_VERSION_SINCE(4,8,0) || defined(__clang__))
typedef unsigned long long tick_t;
#define PRItick "llu"

static __inline__ tick_t
tick(void)
{
    unsigned long long val = __builtin_ppc_get_timebase();
    return val;
}

/* Implementation for macOS PPC by @nobu
 * See: https://github.com/ruby/ruby/pull/5975#discussion_r890045558
 */
#elif defined(__POWERPC__) && defined(__APPLE__)
typedef unsigned long long tick_t;
#define PRItick "llu"

static __inline__ tick_t
tick(void)
{
    unsigned long int upper, lower, tmp;
    # define mftbu(r) __asm__ volatile("mftbu   %0" : "=r"(r))
    # define mftb(r)  __asm__ volatile("mftb    %0" : "=r"(r))
        do {
            mftbu(upper);
            mftb(lower);
            mftbu(tmp);
        } while (tmp != upper);
    return ((tick_t)upper << 32) | lower;
}

#elif defined(__aarch64__) &&  defined(__GNUC__)
typedef unsigned long tick_t;
#define PRItick "lu"

static __inline__ tick_t
tick(void)
{
    unsigned long val;
    __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (val));
    return val;
}


#elif defined(_WIN32) && defined(_MSC_VER)
#include <intrin.h>
typedef unsigned __int64 tick_t;
#define PRItick "llu"

static inline tick_t
tick(void)
{
    return __rdtsc();
}

#else /* use clock */
typedef clock_t tick_t;
#define PRItick "llu"

static inline tick_t
tick(void)
{
    return clock();
}
#endif /* TSC */
#else /* USE_TICK_T */
#define MEASURE_LINE(expr) expr
#endif /* USE_TICK_T */

#define asan_unpoisoning_object(obj) \
    for (void *poisoned = asan_unpoison_object_temporary(obj), \
              *unpoisoning = &poisoned; /* flag to loop just once */ \
         unpoisoning; \
         unpoisoning = asan_poison_object_restore(obj, poisoned))

#define FL_CHECK2(name, x, pred) \
    ((RGENGC_CHECK_MODE && SPECIAL_CONST_P(x)) ? \
     (rb_bug(name": SPECIAL_CONST (%p)", (void *)(x)), 0) : (pred))
#define FL_TEST2(x,f)  FL_CHECK2("FL_TEST2",  x, FL_TEST_RAW((x),(f)) != 0)
#define FL_SET2(x,f)   FL_CHECK2("FL_SET2",   x, RBASIC(x)->flags |= (f))
#define FL_UNSET2(x,f) FL_CHECK2("FL_UNSET2", x, RBASIC(x)->flags &= ~(f))

#define RVALUE_MARK_BITMAP(obj)           MARKED_IN_BITMAP(GET_HEAP_MARK_BITS(obj), (obj))
#define RVALUE_PIN_BITMAP(obj)            MARKED_IN_BITMAP(GET_HEAP_PINNED_BITS(obj), (obj))
#define RVALUE_PAGE_MARKED(page, obj)     MARKED_IN_BITMAP((page)->mark_bits, (obj))

#define RVALUE_WB_UNPROTECTED_BITMAP(obj) MARKED_IN_BITMAP(GET_HEAP_WB_UNPROTECTED_BITS(obj), (obj))
#define RVALUE_UNCOLLECTIBLE_BITMAP(obj)  MARKED_IN_BITMAP(GET_HEAP_UNCOLLECTIBLE_BITS(obj), (obj))
#define RVALUE_MARKING_BITMAP(obj)        MARKED_IN_BITMAP(GET_HEAP_MARKING_BITS(obj), (obj))

#define RVALUE_PAGE_WB_UNPROTECTED(page, obj) MARKED_IN_BITMAP((page)->wb_unprotected_bits, (obj))
#define RVALUE_PAGE_UNCOLLECTIBLE(page, obj)  MARKED_IN_BITMAP((page)->uncollectible_bits, (obj))
#define RVALUE_PAGE_MARKING(page, obj)        MARKED_IN_BITMAP((page)->marking_bits, (obj))

static int rgengc_remember(rb_objspace_t *objspace, VALUE obj);

static int
check_rvalue_consistency_force(rb_objspace_t *objspace, const VALUE obj, int terminate)
{
    int err = 0;

    int lev = rb_gc_vm_lock_no_barrier();
    {
        if (SPECIAL_CONST_P(obj)) {
            fprintf(stderr, "check_rvalue_consistency: %p is a special const.\n", (void *)obj);
            err++;
        }
        else if (!is_pointer_to_heap(objspace, (void *)obj)) {
            /* check if it is in tomb_pages */
            struct heap_page *page = NULL;
            for (int i = 0; i < SIZE_POOL_COUNT; i++) {
                rb_size_pool_t *size_pool = &size_pools[i];
                ccan_list_for_each(&size_pool->tomb_heap.pages, page, page_node) {
                    if (page->start <= (uintptr_t)obj &&
                            (uintptr_t)obj < (page->start + (page->total_slots * size_pool->slot_size))) {
                        fprintf(stderr, "check_rvalue_consistency: %p is in a tomb_heap (%p).\n",
                                (void *)obj, (void *)page);
                        err++;
                        goto skip;
                    }
                }
            }
            fprintf(stderr, "check_rvalue_consistency: %p is not a Ruby object.\n", (void *)obj);
            err++;
          skip:
            ;
        }
        else {
            const int wb_unprotected_bit = RVALUE_WB_UNPROTECTED_BITMAP(obj) != 0;
            const int uncollectible_bit = RVALUE_UNCOLLECTIBLE_BITMAP(obj) != 0;
            const int mark_bit = RVALUE_MARK_BITMAP(obj) != 0;
            const int marking_bit = RVALUE_MARKING_BITMAP(obj) != 0;
            const int remembered_bit = MARKED_IN_BITMAP(GET_HEAP_PAGE(obj)->remembered_bits, obj) != 0;
            const int age = RVALUE_AGE_GET((VALUE)obj);

            if (GET_HEAP_PAGE(obj)->flags.in_tomb) {
                fprintf(stderr, "check_rvalue_consistency: %s is in tomb page.\n", rb_obj_info(obj));
                err++;
            }
            if (BUILTIN_TYPE(obj) == T_NONE) {
                fprintf(stderr, "check_rvalue_consistency: %s is T_NONE.\n", rb_obj_info(obj));
                err++;
            }
            if (BUILTIN_TYPE(obj) == T_ZOMBIE) {
                fprintf(stderr, "check_rvalue_consistency: %s is T_ZOMBIE.\n", rb_obj_info(obj));
                err++;
            }

            if (BUILTIN_TYPE(obj) != T_DATA) {
                rb_obj_memsize_of((VALUE)obj);
            }

            /* check generation
             *
             * OLD == age == 3 && old-bitmap && mark-bit (except incremental marking)
             */
            if (age > 0 && wb_unprotected_bit) {
                fprintf(stderr, "check_rvalue_consistency: %s is not WB protected, but age is %d > 0.\n", rb_obj_info(obj), age);
                err++;
            }

            if (!is_marking(objspace) && uncollectible_bit && !mark_bit) {
                fprintf(stderr, "check_rvalue_consistency: %s is uncollectible, but is not marked while !gc.\n", rb_obj_info(obj));
                err++;
            }

            if (!is_full_marking(objspace)) {
                if (uncollectible_bit && age != RVALUE_OLD_AGE && !wb_unprotected_bit) {
                    fprintf(stderr, "check_rvalue_consistency: %s is uncollectible, but not old (age: %d) and not WB unprotected.\n",
                            rb_obj_info(obj), age);
                    err++;
                }
                if (remembered_bit && age != RVALUE_OLD_AGE) {
                    fprintf(stderr, "check_rvalue_consistency: %s is remembered, but not old (age: %d).\n",
                            rb_obj_info(obj), age);
                    err++;
                }
            }

            /*
             * check coloring
             *
             *               marking:false marking:true
             * marked:false  white         *invalid*
             * marked:true   black         grey
             */
            if (is_incremental_marking(objspace) && marking_bit) {
                if (!is_marking(objspace) && !mark_bit) {
                    fprintf(stderr, "check_rvalue_consistency: %s is marking, but not marked.\n", rb_obj_info(obj));
                    err++;
                }
            }
        }
    }
    rb_gc_vm_unlock_no_barrier(lev);

    if (err > 0 && terminate) {
        rb_bug("check_rvalue_consistency_force: there is %d errors.", err);
    }
    return err;
}

#if RGENGC_CHECK_MODE == 0
static inline VALUE
check_rvalue_consistency(rb_objspace_t *objspace, const VALUE obj)
{
    return obj;
}
#else
static VALUE
check_rvalue_consistency(rb_objspace_t *objspace, const VALUE obj)
{
    check_rvalue_consistency_force(objspace, obj, TRUE);
    return obj;
}
#endif

static inline bool
gc_object_moved_p(rb_objspace_t *objspace, VALUE obj)
{
    if (RB_SPECIAL_CONST_P(obj)) {
        return FALSE;
    }
    else {
        void *poisoned = asan_unpoison_object_temporary(obj);

        int ret =  BUILTIN_TYPE(obj) == T_MOVED;
        /* Re-poison slot if it's not the one we want */
        if (poisoned) {
            GC_ASSERT(BUILTIN_TYPE(obj) == T_NONE);
            asan_poison_object(obj);
        }
        return ret;
    }
}

static inline int
RVALUE_MARKED(rb_objspace_t *objspace, VALUE obj)
{
    check_rvalue_consistency(objspace, obj);
    return RVALUE_MARK_BITMAP(obj) != 0;
}

static inline int
RVALUE_WB_UNPROTECTED(rb_objspace_t *objspace, VALUE obj)
{
    check_rvalue_consistency(objspace, obj);
    return RVALUE_WB_UNPROTECTED_BITMAP(obj) != 0;
}

static inline int
RVALUE_MARKING(rb_objspace_t *objspace, VALUE obj)
{
    check_rvalue_consistency(objspace, obj);
    return RVALUE_MARKING_BITMAP(obj) != 0;
}

static inline int
RVALUE_REMEMBERED(rb_objspace_t *objspace, VALUE obj)
{
    check_rvalue_consistency(objspace, obj);
    return MARKED_IN_BITMAP(GET_HEAP_PAGE(obj)->remembered_bits, obj) != 0;
}

static inline int
RVALUE_UNCOLLECTIBLE(rb_objspace_t *objspace, VALUE obj)
{
    check_rvalue_consistency(objspace, obj);
    return RVALUE_UNCOLLECTIBLE_BITMAP(obj) != 0;
}

static inline int
RVALUE_OLD_P(rb_objspace_t *objspace, VALUE obj)
{
    GC_ASSERT(!RB_SPECIAL_CONST_P(obj));
    check_rvalue_consistency(objspace, obj);
    // Because this will only ever be called on GC controlled objects,
    // we can use the faster _RAW function here
    return RB_OBJ_PROMOTED_RAW(obj);
}

static inline void
RVALUE_PAGE_OLD_UNCOLLECTIBLE_SET(rb_objspace_t *objspace, struct heap_page *page, VALUE obj)
{
    MARK_IN_BITMAP(&page->uncollectible_bits[0], obj);
    objspace->rgengc.old_objects++;

#if RGENGC_PROFILE >= 2
    objspace->profile.total_promoted_count++;
    objspace->profile.promoted_types[BUILTIN_TYPE(obj)]++;
#endif
}

static inline void
RVALUE_OLD_UNCOLLECTIBLE_SET(rb_objspace_t *objspace, VALUE obj)
{
    RB_DEBUG_COUNTER_INC(obj_promote);
    RVALUE_PAGE_OLD_UNCOLLECTIBLE_SET(objspace, GET_HEAP_PAGE(obj), obj);
}

/* set age to age+1 */
static inline void
RVALUE_AGE_INC(rb_objspace_t *objspace, VALUE obj)
{
    int age = RVALUE_AGE_GET((VALUE)obj);

    if (RGENGC_CHECK_MODE && age == RVALUE_OLD_AGE) {
        rb_bug("RVALUE_AGE_INC: can not increment age of OLD object %s.", rb_obj_info(obj));
    }

    age++;
    RVALUE_AGE_SET(obj, age);

    if (age == RVALUE_OLD_AGE) {
        RVALUE_OLD_UNCOLLECTIBLE_SET(objspace, obj);
    }

    check_rvalue_consistency(objspace, obj);
}

static inline void
RVALUE_AGE_SET_CANDIDATE(rb_objspace_t *objspace, VALUE obj)
{
    check_rvalue_consistency(objspace, obj);
    GC_ASSERT(!RVALUE_OLD_P(objspace, obj));
    RVALUE_AGE_SET(obj, RVALUE_OLD_AGE - 1);
    check_rvalue_consistency(objspace, obj);
}

static inline void
RVALUE_AGE_RESET(VALUE obj)
{
    RVALUE_AGE_SET(obj, 0);
}

static inline int
RVALUE_BLACK_P(rb_objspace_t *objspace, VALUE obj)
{
    return RVALUE_MARKED(objspace, obj) && !RVALUE_MARKING(objspace, obj);
}

static inline int
RVALUE_WHITE_P(rb_objspace_t *objspace, VALUE obj)
{
    return !RVALUE_MARKED(objspace, obj);
}

bool
rb_gc_impl_gc_enabled_p(void *objspace_ptr)
{
    return FALSE;
}

void
rb_gc_impl_gc_enable(void *objspace_ptr)
{
    // NO-OP: GC Cannot be enabled
}

void
rb_gc_impl_gc_disable(void *objspace_ptr, bool finish_current_gc)
{
    // NO-OP: GC Cannot be disabled
}

/*
  --------------------------- ObjectSpace -----------------------------
*/

static inline void *
calloc1(size_t n)
{
    return calloc(1, n);
}

void
rb_gc_impl_set_event_hook(void *objspace_ptr, const rb_event_flag_t event)
{
    rb_objspace_t *objspace = objspace_ptr;
    objspace->hook_events = event & RUBY_INTERNAL_EVENT_OBJSPACE_MASK;
    objspace->flags.has_newobj_hook = !!(objspace->hook_events & RUBY_INTERNAL_EVENT_NEWOBJ);
}

VALUE
rb_gc_impl_get_profile_total_time(void *objspace_ptr)
{
    rb_objspace_t *objspace = objspace_ptr;

    uint64_t marking_time = objspace->profile.marking_time_ns;
    uint64_t sweeping_time = objspace->profile.sweeping_time_ns;

    return ULL2NUM(marking_time + sweeping_time);
}

VALUE
rb_gc_impl_set_measure_total_time(void *objspace_ptr, VALUE flag)
{
    rb_objspace_t *objspace = objspace_ptr;

    objspace->flags.measure_gc = RTEST(flag) ? TRUE : FALSE;

    return flag;
}

VALUE
rb_gc_impl_get_measure_total_time(void *objspace_ptr)
{
    rb_objspace_t *objspace = objspace_ptr;

    return objspace->flags.measure_gc ? Qtrue : Qfalse;
}

static size_t
slots_to_pages_for_size_pool(rb_objspace_t *objspace, rb_size_pool_t *size_pool, size_t slots)
{
    size_t multiple = size_pool->slot_size / BASE_SLOT_SIZE;
    /* Due to alignment, heap pages may have one less slot. We should
     * ensure there is enough pages to guarantee that we will have at
     * least the required number of slots after allocating all the pages. */
    size_t slots_per_page = (HEAP_PAGE_OBJ_LIMIT / multiple) - 1;
    return CEILDIV(slots, slots_per_page);
}

static size_t
minimum_pages_for_size_pool(rb_objspace_t *objspace, rb_size_pool_t *size_pool)
{
    size_t size_pool_idx = size_pool - size_pools;
    size_t init_slots = gc_params.size_pool_init_slots[size_pool_idx];
    return slots_to_pages_for_size_pool(objspace, size_pool, init_slots);
}

static VALUE initial_stress = Qfalse;

void
rb_gc_impl_initial_stress_set(VALUE flag)
{
    initial_stress = flag;
}

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

#define OBJ_ID_INCREMENT (BASE_SLOT_SIZE)
#define OBJ_ID_INITIAL (OBJ_ID_INCREMENT)

static const struct st_hash_type object_id_hash_type = {
    object_id_cmp,
    object_id_hash,
};

/* garbage objects will be collected soon. */
bool
rb_gc_impl_garbage_object_p(void *objspace_ptr, VALUE ptr)
{
    switch (BUILTIN_TYPE(ptr)) {
      case T_NONE:
      case T_MOVED:
      case T_ZOMBIE:
        return true;
      default:
        return false;
    }
}

VALUE
rb_gc_impl_object_id_to_ref(void *objspace_ptr, VALUE object_id)
{
    rb_objspace_t *objspace = objspace_ptr;

    VALUE obj;
    if (st_lookup(objspace->id_to_obj_tbl, object_id, &obj) &&
            !rb_gc_impl_garbage_object_p(objspace, obj)) {
        return obj;
    }

    if (rb_funcall(object_id, rb_intern(">="), 1, ULL2NUM(objspace->next_object_id))) {
        rb_raise(rb_eRangeError, "%+"PRIsVALUE" is not id value", rb_funcall(object_id, rb_intern("to_s"), 1, INT2FIX(10)));
    }
    else {
        rb_raise(rb_eRangeError, "%+"PRIsVALUE" is recycled object", rb_funcall(object_id, rb_intern("to_s"), 1, INT2FIX(10)));
    }
}

VALUE
rb_gc_impl_object_id(void *objspace_ptr, VALUE obj)
{
    VALUE id;
    rb_objspace_t *objspace = objspace_ptr;

    unsigned int lev = rb_gc_vm_lock();
    if (st_lookup(objspace->obj_to_id_tbl, (st_data_t)obj, &id)) {
        GC_ASSERT(FL_TEST(obj, FL_SEEN_OBJ_ID));
    }
    else {
        GC_ASSERT(!FL_TEST(obj, FL_SEEN_OBJ_ID));

        id = ULL2NUM(objspace->next_object_id);
        objspace->next_object_id += OBJ_ID_INCREMENT;

        st_insert(objspace->obj_to_id_tbl, (st_data_t)obj, (st_data_t)id);
        st_insert(objspace->id_to_obj_tbl, (st_data_t)id, (st_data_t)obj);
        FL_SET(obj, FL_SEEN_OBJ_ID);
    }
    rb_gc_vm_unlock(lev);

    return id;
}

static void free_stack_chunks(mark_stack_t *);
static void mark_stack_free_cache(mark_stack_t *);
static void heap_page_free(rb_objspace_t *objspace, struct heap_page *page);

static void
heap_pages_expand_sorted_to(rb_objspace_t *objspace, size_t next_length)
{
    struct heap_page **sorted;
    size_t size = rb_size_mul_or_raise(next_length, sizeof(struct heap_page *), rb_eRuntimeError);

    gc_report(3, objspace, "heap_pages_expand_sorted: next_length: %"PRIdSIZE", size: %"PRIdSIZE"\n",
              next_length, size);

    if (heap_pages_sorted_length > 0) {
        sorted = (struct heap_page **)realloc(heap_pages_sorted, size);
        if (sorted) heap_pages_sorted = sorted;
    }
    else {
        sorted = heap_pages_sorted = (struct heap_page **)malloc(size);
    }

    if (sorted == 0) {
        rb_memerror();
    }

    heap_pages_sorted_length = next_length;
}

static void
heap_pages_expand_sorted(rb_objspace_t *objspace)
{
    /* usually heap_allocatable_pages + heap_eden->total_pages == heap_pages_sorted_length
     * because heap_allocatable_pages contains heap_tomb->total_pages (recycle heap_tomb pages).
     * however, if there are pages which do not have empty slots, then try to create new pages
     * so that the additional allocatable_pages counts (heap_tomb->total_pages) are added.
     */
    size_t next_length = heap_allocatable_pages(objspace);
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        rb_size_pool_t *size_pool = &size_pools[i];
        next_length += SIZE_POOL_EDEN_HEAP(size_pool)->total_pages;
        next_length += SIZE_POOL_TOMB_HEAP(size_pool)->total_pages;
    }

    if (next_length > heap_pages_sorted_length) {
        heap_pages_expand_sorted_to(objspace, next_length);
    }

    GC_ASSERT(heap_allocatable_pages(objspace) + heap_eden_total_pages(objspace) <= heap_pages_sorted_length);
    GC_ASSERT(heap_allocated_pages <= heap_pages_sorted_length);
}

static void
size_pool_allocatable_pages_set(rb_objspace_t *objspace, rb_size_pool_t *size_pool, size_t s)
{
    size_pool->allocatable_pages = s;
    heap_pages_expand_sorted(objspace);
}

static inline void
heap_page_add_freeobj(rb_objspace_t *objspace, struct heap_page *page, VALUE obj)
{
    asan_unpoison_object(obj, false);

    asan_unlock_freelist(page);

    struct free_slot *slot = (struct free_slot *)obj;
    slot->flags = 0;
    slot->next = page->freelist;
    page->freelist = slot;
    asan_lock_freelist(page);

    RVALUE_AGE_RESET(obj);

    if (RGENGC_CHECK_MODE &&
        /* obj should belong to page */
        !(page->start <= (uintptr_t)obj &&
          (uintptr_t)obj   <  ((uintptr_t)page->start + (page->total_slots * page->slot_size)) &&
          obj % BASE_SLOT_SIZE == 0)) {
        rb_bug("heap_page_add_freeobj: %p is not rvalue.", (void *)obj);
    }

    asan_poison_object(obj);
    gc_report(3, objspace, "heap_page_add_freeobj: add %p to freelist\n", (void *)obj);
}

static size_t
heap_extend_pages(rb_objspace_t *objspace, rb_size_pool_t *size_pool, size_t free_slots, size_t total_slots, size_t used);

static void
size_pool_allocatable_pages_expand(rb_objspace_t *objspace, rb_size_pool_t *size_pool,
                                   size_t swept_slots, size_t total_slots, size_t total_pages)
{
    size_t extend_page_count = heap_extend_pages(objspace, size_pool, swept_slots, total_slots, total_pages);

    if (extend_page_count > size_pool->allocatable_pages) {
        size_pool_allocatable_pages_set(objspace, size_pool, extend_page_count);
    }
}

static inline void
heap_add_freepage(rb_heap_t *heap, struct heap_page *page)
{
    asan_unlock_freelist(page);
    GC_ASSERT(page->free_slots != 0);
    GC_ASSERT(page->freelist != NULL);

    page->free_next = heap->free_pages;
    heap->free_pages = page;

    RUBY_DEBUG_LOG("page:%p freelist:%p", (void *)page, (void *)page->freelist);

    asan_lock_freelist(page);
}

static void
heap_unlink_page(rb_objspace_t *objspace, rb_heap_t *heap, struct heap_page *page)
{
    ccan_list_del(&page->page_node);
    heap->total_pages--;
    heap->total_slots -= page->total_slots;
}

static void
gc_aligned_free(void *ptr, size_t size)
{
#if defined __MINGW32__
    __mingw_aligned_free(ptr);
#elif defined _WIN32
    _aligned_free(ptr);
#elif defined(HAVE_POSIX_MEMALIGN) || defined(HAVE_MEMALIGN)
    free(ptr);
#else
    free(((void**)ptr)[-1]);
#endif
}

static void
heap_page_body_free(struct heap_page_body *page_body)
{
    GC_ASSERT((uintptr_t)page_body % HEAP_PAGE_ALIGN == 0);

    if (HEAP_PAGE_ALLOC_USE_MMAP) {
#ifdef HAVE_MMAP
        GC_ASSERT(HEAP_PAGE_SIZE % sysconf(_SC_PAGE_SIZE) == 0);
        if (munmap(page_body, HEAP_PAGE_SIZE)) {
            rb_bug("heap_page_body_free: munmap failed");
        }
#endif
    }
    else {
        gc_aligned_free(page_body, HEAP_PAGE_SIZE);
    }
}

static void
heap_page_free(rb_objspace_t *objspace, struct heap_page *page)
{
    heap_allocated_pages--;
    page->size_pool->total_freed_pages++;
    heap_page_body_free(GET_PAGE_BODY(page->start));
    free(page);
}

static void
heap_pages_free_unused_pages(rb_objspace_t *objspace)
{
    size_t i, j;

    bool has_pages_in_tomb_heap = FALSE;
    for (i = 0; i < SIZE_POOL_COUNT; i++) {
        if (!ccan_list_empty(&SIZE_POOL_TOMB_HEAP(&size_pools[i])->pages)) {
            has_pages_in_tomb_heap = TRUE;
            break;
        }
    }

    if (has_pages_in_tomb_heap) {
        for (i = j = 0; j < heap_allocated_pages; i++) {
            struct heap_page *page = heap_pages_sorted[i];

            if (page->flags.in_tomb && page->free_slots == page->total_slots) {
                heap_unlink_page(objspace, SIZE_POOL_TOMB_HEAP(page->size_pool), page);
                heap_page_free(objspace, page);
            }
            else {
                if (i != j) {
                    heap_pages_sorted[j] = page;
                }
                j++;
            }
        }

        struct heap_page *hipage = heap_pages_sorted[heap_allocated_pages - 1];
        uintptr_t himem = (uintptr_t)hipage->start + (hipage->total_slots * hipage->slot_size);
        GC_ASSERT(himem <= heap_pages_himem);
        heap_pages_himem = himem;

        struct heap_page *lopage = heap_pages_sorted[0];
        uintptr_t lomem = (uintptr_t)lopage->start;
        GC_ASSERT(lomem >= heap_pages_lomem);
        heap_pages_lomem = lomem;

        GC_ASSERT(j == heap_allocated_pages);
    }
}

static void *
gc_aligned_malloc(size_t alignment, size_t size)
{
    /* alignment must be a power of 2 */
    GC_ASSERT(((alignment - 1) & alignment) == 0);
    GC_ASSERT(alignment % sizeof(void*) == 0);

    void *res;

#if defined __MINGW32__
    res = __mingw_aligned_malloc(size, alignment);
#elif defined _WIN32
    void *_aligned_malloc(size_t, size_t);
    res = _aligned_malloc(size, alignment);
#elif defined(HAVE_POSIX_MEMALIGN)
    if (posix_memalign(&res, alignment, size) != 0) {
        return NULL;
    }
#elif defined(HAVE_MEMALIGN)
    res = memalign(alignment, size);
#else
    char* aligned;
    res = malloc(alignment + size + sizeof(void*));
    aligned = (char*)res + alignment + sizeof(void*);
    aligned -= ((VALUE)aligned & (alignment - 1));
    ((void**)aligned)[-1] = res;
    res = (void*)aligned;
#endif

    GC_ASSERT((uintptr_t)res % alignment == 0);

    return res;
}

static struct heap_page_body *
heap_page_body_allocate(void)
{
    struct heap_page_body *page_body;

    if (HEAP_PAGE_ALLOC_USE_MMAP) {
#ifdef HAVE_MMAP
        GC_ASSERT(HEAP_PAGE_ALIGN % sysconf(_SC_PAGE_SIZE) == 0);

        char *ptr = mmap(NULL, HEAP_PAGE_ALIGN + HEAP_PAGE_SIZE,
                         PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) {
            return NULL;
        }

        char *aligned = ptr + HEAP_PAGE_ALIGN;
        aligned -= ((VALUE)aligned & (HEAP_PAGE_ALIGN - 1));
        GC_ASSERT(aligned > ptr);
        GC_ASSERT(aligned <= ptr + HEAP_PAGE_ALIGN);

        size_t start_out_of_range_size = aligned - ptr;
        GC_ASSERT(start_out_of_range_size % sysconf(_SC_PAGE_SIZE) == 0);
        if (start_out_of_range_size > 0) {
            if (munmap(ptr, start_out_of_range_size)) {
                rb_bug("heap_page_body_allocate: munmap failed for start");
            }
        }

        size_t end_out_of_range_size = HEAP_PAGE_ALIGN - start_out_of_range_size;
        GC_ASSERT(end_out_of_range_size % sysconf(_SC_PAGE_SIZE) == 0);
        if (end_out_of_range_size > 0) {
            if (munmap(aligned + HEAP_PAGE_SIZE, end_out_of_range_size)) {
                rb_bug("heap_page_body_allocate: munmap failed for end");
            }
        }

        page_body = (struct heap_page_body *)aligned;
#endif
    }
    else {
        page_body = gc_aligned_malloc(HEAP_PAGE_ALIGN, HEAP_PAGE_SIZE);
    }

    GC_ASSERT((uintptr_t)page_body % HEAP_PAGE_ALIGN == 0);

    return page_body;
}

static struct heap_page *
heap_page_allocate(rb_objspace_t *objspace, rb_size_pool_t *size_pool)
{
    uintptr_t start, end, p;
    struct heap_page *page;
    uintptr_t hi, lo, mid;
    size_t stride = size_pool->slot_size;
    unsigned int limit = (unsigned int)((HEAP_PAGE_SIZE - sizeof(struct heap_page_header)))/(int)stride;

    /* assign heap_page body (contains heap_page_header and RVALUEs) */
    struct heap_page_body *page_body = heap_page_body_allocate();
    if (page_body == 0) {
        rb_memerror();
    }

    /* assign heap_page entry */
    page = calloc1(sizeof(struct heap_page));
    if (page == 0) {
        heap_page_body_free(page_body);
        rb_memerror();
    }

    /* adjust obj_limit (object number available in this page) */
    start = (uintptr_t)((VALUE)page_body + sizeof(struct heap_page_header));

    if (start % BASE_SLOT_SIZE != 0) {
        int delta = BASE_SLOT_SIZE - (start % BASE_SLOT_SIZE);
        start = start + delta;
        GC_ASSERT(NUM_IN_PAGE(start) == 0 || NUM_IN_PAGE(start) == 1);

        /* Find a num in page that is evenly divisible by `stride`.
         * This is to ensure that objects are aligned with bit planes.
         * In other words, ensure there are an even number of objects
         * per bit plane. */
        if (NUM_IN_PAGE(start) == 1) {
            start += stride - BASE_SLOT_SIZE;
        }

        GC_ASSERT(NUM_IN_PAGE(start) * BASE_SLOT_SIZE % stride == 0);

        limit = (HEAP_PAGE_SIZE - (int)(start - (uintptr_t)page_body))/(int)stride;
    }
    end = start + (limit * (int)stride);

    /* setup heap_pages_sorted */
    lo = 0;
    hi = (uintptr_t)heap_allocated_pages;
    while (lo < hi) {
        struct heap_page *mid_page;

        mid = (lo + hi) / 2;
        mid_page = heap_pages_sorted[mid];
        if ((uintptr_t)mid_page->start < start) {
            lo = mid + 1;
        }
        else if ((uintptr_t)mid_page->start > start) {
            hi = mid;
        }
        else {
            rb_bug("same heap page is allocated: %p at %"PRIuVALUE, (void *)page_body, (VALUE)mid);
        }
    }

    if (hi < (uintptr_t)heap_allocated_pages) {
        MEMMOVE(&heap_pages_sorted[hi+1], &heap_pages_sorted[hi], struct heap_page_header*, heap_allocated_pages - hi);
    }

    heap_pages_sorted[hi] = page;

    heap_allocated_pages++;

    GC_ASSERT(heap_eden_total_pages(objspace) + heap_allocatable_pages(objspace) <= heap_pages_sorted_length);
    GC_ASSERT(heap_eden_total_pages(objspace) + heap_tomb_total_pages(objspace) == heap_allocated_pages - 1);
    GC_ASSERT(heap_allocated_pages <= heap_pages_sorted_length);

    size_pool->total_allocated_pages++;

    if (heap_allocated_pages > heap_pages_sorted_length) {
        rb_bug("heap_page_allocate: allocated(%"PRIdSIZE") > sorted(%"PRIdSIZE")",
               heap_allocated_pages, heap_pages_sorted_length);
    }

    if (heap_pages_lomem == 0 || heap_pages_lomem > start) heap_pages_lomem = start;
    if (heap_pages_himem < end) heap_pages_himem = end;

    page->start = start;
    page->total_slots = limit;
    page->slot_size = size_pool->slot_size;
    page->size_pool = size_pool;
    page_body->header.page = page;

    for (p = start; p != end; p += stride) {
        gc_report(3, objspace, "assign_heap_page: %p is added to freelist\n", (void *)p);
        heap_page_add_freeobj(objspace, page, (VALUE)p);
    }
    page->free_slots = limit;

    asan_lock_freelist(page);
    return page;
}

static struct heap_page *
heap_page_resurrect(rb_objspace_t *objspace, rb_size_pool_t *size_pool)
{
    struct heap_page *page = 0, *next;

    ccan_list_for_each_safe(&SIZE_POOL_TOMB_HEAP(size_pool)->pages, page, next, page_node) {
        asan_unlock_freelist(page);
        if (page->freelist != NULL) {
            heap_unlink_page(objspace, &size_pool->tomb_heap, page);
            asan_lock_freelist(page);
            return page;
        }
    }

    return NULL;
}

static struct heap_page *
heap_page_create(rb_objspace_t *objspace, rb_size_pool_t *size_pool)
{
    struct heap_page *page;
    const char *method = "recycle";

    size_pool->allocatable_pages--;

    page = heap_page_resurrect(objspace, size_pool);

    if (page == NULL) {
        page = heap_page_allocate(objspace, size_pool);
        method = "allocate";
    }
    if (0) fprintf(stderr, "heap_page_create: %s - %p, "
                   "heap_allocated_pages: %"PRIdSIZE", "
                   "heap_allocated_pages: %"PRIdSIZE", "
                   "tomb->total_pages: %"PRIdSIZE"\n",
                   method, (void *)page, heap_pages_sorted_length, heap_allocated_pages, SIZE_POOL_TOMB_HEAP(size_pool)->total_pages);
    return page;
}

static void
heap_add_page(rb_objspace_t *objspace, rb_size_pool_t *size_pool, rb_heap_t *heap, struct heap_page *page)
{
    /* Adding to eden heap during incremental sweeping is forbidden */
    GC_ASSERT(!(heap == SIZE_POOL_EDEN_HEAP(size_pool) && heap->sweeping_page));
    page->flags.in_tomb = (heap == SIZE_POOL_TOMB_HEAP(size_pool));
    ccan_list_add_tail(&heap->pages, &page->page_node);
    heap->total_pages++;
    heap->total_slots += page->total_slots;
}

static void
heap_assign_page(rb_objspace_t *objspace, rb_size_pool_t *size_pool, rb_heap_t *heap)
{
    struct heap_page *page = heap_page_create(objspace, size_pool);
    heap_add_page(objspace, size_pool, heap, page);
    heap_add_freepage(heap, page);
}

static size_t
heap_extend_pages(rb_objspace_t *objspace, rb_size_pool_t *size_pool, size_t free_slots, size_t total_slots, size_t used)
{
    double goal_ratio = gc_params.heap_free_slots_goal_ratio;
    size_t next_used;

    if (goal_ratio == 0.0) {
        next_used = (size_t)(used * gc_params.growth_factor);
    }
    else if (total_slots == 0) {
        next_used = minimum_pages_for_size_pool(objspace, size_pool);
    }
    else {
        /* Find `f' where free_slots = f * total_slots * goal_ratio
         * => f = (total_slots - free_slots) / ((1 - goal_ratio) * total_slots)
         */
        double f = (double)(total_slots - free_slots) / ((1 - goal_ratio) * total_slots);

        if (f > gc_params.growth_factor) f = gc_params.growth_factor;
        if (f < 1.0) f = 1.1;

        next_used = (size_t)(f * used);

        if (0) {
            fprintf(stderr,
                    "free_slots(%8"PRIuSIZE")/total_slots(%8"PRIuSIZE")=%1.2f,"
                    " G(%1.2f), f(%1.2f),"
                    " used(%8"PRIuSIZE") => next_used(%8"PRIuSIZE")\n",
                    free_slots, total_slots, free_slots/(double)total_slots,
                    goal_ratio, f, used, next_used);
        }
    }

    if (gc_params.growth_max_slots > 0) {
        size_t max_used = (size_t)(used + gc_params.growth_max_slots/HEAP_PAGE_OBJ_LIMIT);
        if (next_used > max_used) next_used = max_used;
    }

    size_t extend_page_count = next_used - used;
    /* Extend by at least 1 page. */
    if (extend_page_count == 0) extend_page_count = 1;

    return extend_page_count;
}

static int
heap_increment(rb_objspace_t *objspace, rb_size_pool_t *size_pool, rb_heap_t *heap)
{
    if (size_pool->allocatable_pages > 0) {
        gc_report(1, objspace, "heap_increment: heap_pages_sorted_length: %"PRIdSIZE", "
                  "heap_pages_inc: %"PRIdSIZE", heap->total_pages: %"PRIdSIZE"\n",
                  heap_pages_sorted_length, size_pool->allocatable_pages, heap->total_pages);

        GC_ASSERT(heap_allocatable_pages(objspace) + heap_eden_total_pages(objspace) <= heap_pages_sorted_length);
        GC_ASSERT(heap_allocated_pages <= heap_pages_sorted_length);

        heap_assign_page(objspace, size_pool, heap);
        return TRUE;
    }
    return FALSE;
}

static void
heap_prepare(rb_objspace_t *objspace, rb_size_pool_t *size_pool, rb_heap_t *heap)
{
    GC_ASSERT(heap->free_pages == NULL);
    size_pool_allocatable_pages_expand(objspace, size_pool,
                                       size_pool->freed_slots + size_pool->empty_slots,
                                       heap->total_slots + SIZE_POOL_TOMB_HEAP(size_pool)->total_slots,
                                       heap->total_pages + SIZE_POOL_TOMB_HEAP(size_pool)->total_pages);
    GC_ASSERT(size_pool->allocatable_pages > 0);
    heap_increment(objspace, size_pool, heap);
    GC_ASSERT(heap->free_pages != NULL);
}

static inline VALUE
newobj_init(VALUE klass, VALUE flags, int wb_protected, rb_objspace_t *objspace, VALUE obj)
{
#if !__has_feature(memory_sanitizer)
    GC_ASSERT(BUILTIN_TYPE(obj) == T_NONE);
    GC_ASSERT((flags & FL_WB_PROTECTED) == 0);
#endif
    RBASIC(obj)->flags = flags;
    *((VALUE *)&RBASIC(obj)->klass) = klass;

    int t = flags & RUBY_T_MASK;
    if (t == T_CLASS || t == T_MODULE || t == T_ICLASS) {
        RVALUE_AGE_SET_CANDIDATE(objspace, obj);
    }

#if RACTOR_CHECK_MODE
    void rb_ractor_setup_belonging(VALUE obj);
    rb_ractor_setup_belonging(obj);
#endif

#if RGENGC_CHECK_MODE
    p->as.values.v1 = p->as.values.v2 = p->as.values.v3 = 0;

    int lev = rb_gc_vm_lock_no_barrier();
    {
        check_rvalue_consistency(objspace, obj);

        GC_ASSERT(RVALUE_MARKED(objspace, obj) == FALSE);
        GC_ASSERT(RVALUE_MARKING(objspace, obj) == FALSE);
        GC_ASSERT(RVALUE_OLD_P(objspace, obj) == FALSE);
        GC_ASSERT(RVALUE_WB_UNPROTECTED(objspace, obj) == FALSE);

        if (RVALUE_REMEMBERED(objspace, obj)) rb_bug("newobj: %s is remembered.", rb_obj_info(obj));
    }
    rb_gc_vm_unlock_no_barrier(lev);
#endif

    if (RB_UNLIKELY(wb_protected == FALSE)) {
        MARK_IN_BITMAP(GET_HEAP_WB_UNPROTECTED_BITS(obj), obj);
    }

#if RGENGC_PROFILE
    if (wb_protected) {
        objspace->profile.total_generated_normal_object_count++;
#if RGENGC_PROFILE >= 2
        objspace->profile.generated_normal_object_count_types[BUILTIN_TYPE(obj)]++;
#endif
    }
    else {
        objspace->profile.total_generated_shady_object_count++;
#if RGENGC_PROFILE >= 2
        objspace->profile.generated_shady_object_count_types[BUILTIN_TYPE(obj)]++;
#endif
    }
#endif

#if GC_DEBUG
    GET_RVALUE_OVERHEAD(obj)->file = rb_source_location_cstr(&GET_RVALUE_OVERHEAD(obj)->line);
    GC_ASSERT(!SPECIAL_CONST_P(obj)); /* check alignment */
#endif

    gc_report(5, objspace, "newobj: %s\n", rb_obj_info(obj));

    RUBY_DEBUG_LOG("obj:%p (%s)", (void *)obj, rb_obj_info(obj));
    return obj;
}

size_t
rb_gc_impl_obj_slot_size(VALUE obj)
{
    return GET_HEAP_PAGE(obj)->slot_size - RVALUE_OVERHEAD;
}

static inline size_t
size_pool_slot_size(unsigned char pool_id)
{
    GC_ASSERT(pool_id < SIZE_POOL_COUNT);

    size_t slot_size = (1 << pool_id) * BASE_SLOT_SIZE;

#if RGENGC_CHECK_MODE
    rb_objspace_t *objspace = rb_gc_get_objspace();
    GC_ASSERT(size_pools[pool_id].slot_size == (short)slot_size);
#endif

    slot_size -= RVALUE_OVERHEAD;

    return slot_size;
}

bool
rb_gc_size_allocatable_p(size_t size)
{
    return size <= size_pool_slot_size(SIZE_POOL_COUNT - 1);
}

static inline VALUE
ractor_cache_allocate_slot(rb_objspace_t *objspace, rb_ractor_newobj_cache_t *cache,
                           size_t size_pool_idx)
{
    rb_ractor_newobj_size_pool_cache_t *size_pool_cache = &cache->size_pool_caches[size_pool_idx];
    struct free_slot *p = size_pool_cache->freelist;

    if (is_incremental_marking(objspace)) {
        // Not allowed to allocate without running an incremental marking step
        if (cache->incremental_mark_step_allocated_slots >= INCREMENTAL_MARK_STEP_ALLOCATIONS) {
            return Qfalse;
        }

        if (p) {
            cache->incremental_mark_step_allocated_slots++;
        }
    }

    if (p) {
        VALUE obj = (VALUE)p;
        MAYBE_UNUSED(const size_t) stride = size_pool_slot_size(size_pool_idx);
        size_pool_cache->freelist = p->next;
        asan_unpoison_memory_region(p, stride, true);
#if RGENGC_CHECK_MODE
        GC_ASSERT(rb_gc_obj_slot_size(obj) == stride);
        // zero clear
        MEMZERO((char *)obj, char, stride);
#endif
        return obj;
    }
    else {
        return Qfalse;
    }
}

static struct heap_page *
heap_next_free_page(rb_objspace_t *objspace, rb_size_pool_t *size_pool, rb_heap_t *heap)
{
    struct heap_page *page;

    if (heap->free_pages == NULL) {
        heap_prepare(objspace, size_pool, heap);
    }

    page = heap->free_pages;
    heap->free_pages = page->free_next;

    GC_ASSERT(page->free_slots != 0);

    asan_unlock_freelist(page);

    return page;
}

static inline void
ractor_cache_set_page(rb_objspace_t *objspace, rb_ractor_newobj_cache_t *cache, size_t size_pool_idx,
                      struct heap_page *page)
{
    gc_report(3, objspace, "ractor_set_cache: Using page %p\n", (void *)GET_PAGE_BODY(page->start));

    rb_ractor_newobj_size_pool_cache_t *size_pool_cache = &cache->size_pool_caches[size_pool_idx];

    GC_ASSERT(size_pool_cache->freelist == NULL);
    GC_ASSERT(page->free_slots != 0);
    GC_ASSERT(page->freelist != NULL);

    size_pool_cache->using_page = page;
    size_pool_cache->freelist = page->freelist;
    page->free_slots = 0;
    page->freelist = NULL;

    asan_unpoison_object((VALUE)size_pool_cache->freelist, false);
    GC_ASSERT(RB_TYPE_P((VALUE)size_pool_cache->freelist, T_NONE));
    asan_poison_object((VALUE)size_pool_cache->freelist);
}

static inline VALUE
newobj_fill(VALUE obj, VALUE v1, VALUE v2, VALUE v3)
{
    VALUE *p = (VALUE *)obj;
    p[2] = v1;
    p[3] = v2;
    p[4] = v3;
    return obj;
}

static inline size_t
size_pool_idx_for_size(size_t size)
{
    size += RVALUE_OVERHEAD;

    size_t slot_count = CEILDIV(size, BASE_SLOT_SIZE);

    /* size_pool_idx is ceil(log2(slot_count)) */
    size_t size_pool_idx = 64 - nlz_int64(slot_count - 1);

    if (size_pool_idx >= SIZE_POOL_COUNT) {
        rb_bug("size_pool_idx_for_size: allocation size too large "
               "(size=%"PRIuSIZE"u, size_pool_idx=%"PRIuSIZE"u)", size, size_pool_idx);
    }

#if RGENGC_CHECK_MODE
    rb_objspace_t *objspace = rb_gc_get_objspace();
    GC_ASSERT(size <= (size_t)size_pools[size_pool_idx].slot_size);
    if (size_pool_idx > 0) GC_ASSERT(size > (size_t)size_pools[size_pool_idx - 1].slot_size);
#endif

    return size_pool_idx;
}

size_t
rb_gc_impl_size_pool_id_for_size(void *objspace_ptr, size_t size)
{
    return size_pool_idx_for_size(size);
}


static size_t size_pool_sizes[SIZE_POOL_COUNT + 1] = { 0 };

size_t *
rb_gc_size_pool_sizes(void)
{
    if (size_pool_sizes[0] == 0) {
        for (unsigned char i = 0; i < SIZE_POOL_COUNT; i++) {
            size_pool_sizes[i] = size_pool_slot_size(i);
        }
    }

    return size_pool_sizes;
}

static VALUE
newobj_alloc(rb_objspace_t *objspace, rb_ractor_newobj_cache_t *cache, size_t size_pool_idx, bool vm_locked)
{
    rb_size_pool_t *size_pool = &size_pools[size_pool_idx];
    rb_heap_t *heap = SIZE_POOL_EDEN_HEAP(size_pool);

    VALUE obj = ractor_cache_allocate_slot(objspace, cache, size_pool_idx);

    if (RB_UNLIKELY(obj == Qfalse)) {
        unsigned int lev;
        bool unlock_vm = false;

        if (!vm_locked) {
            lev = rb_gc_cr_lock();
            vm_locked = true;
            unlock_vm = true;
        }

        {
            if (obj == Qfalse) {
                // Get next free page (possibly running GC)
                struct heap_page *page = heap_next_free_page(objspace, size_pool, heap);
                ractor_cache_set_page(objspace, cache, size_pool_idx, page);

                // Retry allocation after moving to new page
                obj = ractor_cache_allocate_slot(objspace, cache, size_pool_idx);

                GC_ASSERT(obj != Qfalse);
            }
        }

        if (unlock_vm) {
            rb_gc_cr_unlock(lev);
        }
    }

    size_pool->total_allocated_objects++;

    return obj;
}

ALWAYS_INLINE(static VALUE newobj_slowpath(VALUE klass, VALUE flags, rb_objspace_t *objspace, rb_ractor_newobj_cache_t *cache, int wb_protected, size_t size_pool_idx));

static inline VALUE
newobj_slowpath(VALUE klass, VALUE flags, rb_objspace_t *objspace, rb_ractor_newobj_cache_t *cache, int wb_protected, size_t size_pool_idx)
{
    VALUE obj;
    unsigned int lev;

    lev = rb_gc_cr_lock();
    obj = newobj_alloc(objspace, cache, size_pool_idx, true);
    newobj_init(klass, flags, wb_protected, objspace, obj);
    rb_gc_cr_unlock(lev);

    return obj;
}

NOINLINE(static VALUE newobj_slowpath_wb_protected(VALUE klass, VALUE flags,
                                                   rb_objspace_t *objspace, rb_ractor_newobj_cache_t *cache, size_t size_pool_idx));
NOINLINE(static VALUE newobj_slowpath_wb_unprotected(VALUE klass, VALUE flags,
                                                     rb_objspace_t *objspace, rb_ractor_newobj_cache_t *cache, size_t size_pool_idx));

static VALUE
newobj_slowpath_wb_protected(VALUE klass, VALUE flags, rb_objspace_t *objspace, rb_ractor_newobj_cache_t *cache, size_t size_pool_idx)
{
    return newobj_slowpath(klass, flags, objspace, cache, TRUE, size_pool_idx);
}

static VALUE
newobj_slowpath_wb_unprotected(VALUE klass, VALUE flags, rb_objspace_t *objspace, rb_ractor_newobj_cache_t *cache, size_t size_pool_idx)
{
    return newobj_slowpath(klass, flags, objspace, cache, FALSE, size_pool_idx);
}

VALUE
rb_gc_impl_new_obj(void *objspace_ptr, void *cache_ptr, VALUE klass, VALUE flags, VALUE v1, VALUE v2, VALUE v3, bool wb_protected, size_t alloc_size)
{
    VALUE obj;
    rb_objspace_t *objspace = objspace_ptr;

    RB_DEBUG_COUNTER_INC(obj_newobj);
    (void)RB_DEBUG_COUNTER_INC_IF(obj_newobj_wb_unprotected, !wb_protected);

    if (RB_UNLIKELY(stress_to_class)) {
        long cnt = RARRAY_LEN(stress_to_class);
        for (long i = 0; i < cnt; i++) {
            if (klass == RARRAY_AREF(stress_to_class, i)) rb_memerror();
        }
    }

    size_t size_pool_idx = size_pool_idx_for_size(alloc_size);

    rb_ractor_newobj_cache_t *cache = (rb_ractor_newobj_cache_t *)cache_ptr;

    if (!RB_UNLIKELY(during_gc || ruby_gc_stressful) &&
            wb_protected) {
        obj = newobj_alloc(objspace, cache, size_pool_idx, false);
        newobj_init(klass, flags, wb_protected, objspace, obj);
    }
    else {
        RB_DEBUG_COUNTER_INC(obj_newobj_slowpath);

        obj = wb_protected ?
          newobj_slowpath_wb_protected(klass, flags, objspace, cache, size_pool_idx) :
          newobj_slowpath_wb_unprotected(klass, flags, objspace, cache, size_pool_idx);
    }

    return newobj_fill(obj, v1, v2, v3);
}

static int
ptr_in_page_body_p(const void *ptr, const void *memb)
{
    struct heap_page *page = *(struct heap_page **)memb;
    uintptr_t p_body = (uintptr_t)GET_PAGE_BODY(page->start);

    if ((uintptr_t)ptr >= p_body) {
        return (uintptr_t)ptr < (p_body + HEAP_PAGE_SIZE) ? 0 : 1;
    }
    else {
        return -1;
    }
}

PUREFUNC(static inline struct heap_page *heap_page_for_ptr(rb_objspace_t *objspace, uintptr_t ptr);)
static inline struct heap_page *
heap_page_for_ptr(rb_objspace_t *objspace, uintptr_t ptr)
{
    struct heap_page **res;

    if (ptr < (uintptr_t)heap_pages_lomem ||
            ptr > (uintptr_t)heap_pages_himem) {
        return NULL;
    }

    res = bsearch((void *)ptr, heap_pages_sorted,
                  (size_t)heap_allocated_pages, sizeof(struct heap_page *),
                  ptr_in_page_body_p);

    if (res) {
        return *res;
    }
    else {
        return NULL;
    }
}

PUREFUNC(static inline bool is_pointer_to_heap(rb_objspace_t *objspace, const void *ptr);)
static inline bool
is_pointer_to_heap(rb_objspace_t *objspace, const void *ptr)
{
    register uintptr_t p = (uintptr_t)ptr;
    register struct heap_page *page;

    RB_DEBUG_COUNTER_INC(gc_isptr_trial);

    if (p < heap_pages_lomem || p > heap_pages_himem) return FALSE;
    RB_DEBUG_COUNTER_INC(gc_isptr_range);

    if (p % BASE_SLOT_SIZE != 0) return FALSE;
    RB_DEBUG_COUNTER_INC(gc_isptr_align);

    page = heap_page_for_ptr(objspace, (uintptr_t)ptr);
    if (page) {
        RB_DEBUG_COUNTER_INC(gc_isptr_maybe);
        if (page->flags.in_tomb) {
            return FALSE;
        }
        else {
            if (p < page->start) return FALSE;
            if (p >= page->start + (page->total_slots * page->slot_size)) return FALSE;
            if ((NUM_IN_PAGE(p) * BASE_SLOT_SIZE) % page->slot_size != 0) return FALSE;

            return TRUE;
        }
    }
    return FALSE;
}

bool
rb_gc_impl_pointer_to_heap_p(void *objspace_ptr, const void *ptr)
{
    return is_pointer_to_heap(objspace_ptr, ptr);
}

#define ZOMBIE_OBJ_KEPT_FLAGS (FL_SEEN_OBJ_ID | FL_FINALIZE)

void
rb_gc_impl_make_zombie(void *objspace_ptr, VALUE obj, void (*dfree)(void *), void *data)
{
    rb_objspace_t *objspace = objspace_ptr;

    struct RZombie *zombie = RZOMBIE(obj);
    zombie->basic.flags = T_ZOMBIE | (zombie->basic.flags & ZOMBIE_OBJ_KEPT_FLAGS);
    zombie->dfree = dfree;
    zombie->data = data;
    VALUE prev, next = heap_pages_deferred_final;
    do {
        zombie->next = prev = next;
        next = RUBY_ATOMIC_VALUE_CAS(heap_pages_deferred_final, prev, obj);
    } while (next != prev);

    struct heap_page *page = GET_HEAP_PAGE(obj);
    page->final_slots++;
    heap_pages_final_slots++;
}

static void
obj_free_object_id(rb_objspace_t *objspace, VALUE obj)
{
    st_data_t o = (st_data_t)obj, id;

    GC_ASSERT(BUILTIN_TYPE(obj) == T_NONE || FL_TEST(obj, FL_SEEN_OBJ_ID));
    FL_UNSET(obj, FL_SEEN_OBJ_ID);

    if (st_delete(objspace->obj_to_id_tbl, &o, &id)) {
        GC_ASSERT(id);
        st_delete(objspace->id_to_obj_tbl, &id, NULL);
    }
    else {
        rb_bug("Object ID seen, but not in mapping table: %s", rb_obj_info(obj));
    }
}

typedef int each_obj_callback(void *, void *, size_t, void *);
typedef int each_page_callback(struct heap_page *, void *);

struct each_obj_data {
    rb_objspace_t *objspace;
    bool reenable_incremental;

    each_obj_callback *each_obj_callback;
    each_page_callback *each_page_callback;
    void *data;

    struct heap_page **pages[SIZE_POOL_COUNT];
    size_t pages_counts[SIZE_POOL_COUNT];
};

static VALUE
objspace_each_objects_ensure(VALUE arg)
{
    struct each_obj_data *data = (struct each_obj_data *)arg;
    rb_objspace_t *objspace = data->objspace;

    /* Reenable incremental GC */
    if (data->reenable_incremental) {
        objspace->flags.dont_incremental = FALSE;
    }

    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        struct heap_page **pages = data->pages[i];
        free(pages);
    }

    return Qnil;
}

static VALUE
objspace_each_objects_try(VALUE arg)
{
    struct each_obj_data *data = (struct each_obj_data *)arg;
    rb_objspace_t *objspace = data->objspace;

    /* Copy pages from all size_pools to their respective buffers. */
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        rb_size_pool_t *size_pool = &size_pools[i];
        size_t size = SIZE_POOL_EDEN_HEAP(size_pool)->total_pages * sizeof(struct heap_page *);

        struct heap_page **pages = malloc(size);
        if (!pages) rb_memerror();

        /* Set up pages buffer by iterating over all pages in the current eden
         * heap. This will be a snapshot of the state of the heap before we
         * call the callback over each page that exists in this buffer. Thus it
         * is safe for the callback to allocate objects without possibly entering
         * an infinite loop. */
        struct heap_page *page = 0;
        size_t pages_count = 0;
        ccan_list_for_each(&SIZE_POOL_EDEN_HEAP(size_pool)->pages, page, page_node) {
            pages[pages_count] = page;
            pages_count++;
        }
        data->pages[i] = pages;
        data->pages_counts[i] = pages_count;
        GC_ASSERT(pages_count == SIZE_POOL_EDEN_HEAP(size_pool)->total_pages);
    }

    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        rb_size_pool_t *size_pool = &size_pools[i];
        size_t pages_count = data->pages_counts[i];
        struct heap_page **pages = data->pages[i];

        struct heap_page *page = ccan_list_top(&SIZE_POOL_EDEN_HEAP(size_pool)->pages, struct heap_page, page_node);
        for (size_t i = 0; i < pages_count; i++) {
            /* If we have reached the end of the linked list then there are no
             * more pages, so break. */
            if (page == NULL) break;

            /* If this page does not match the one in the buffer, then move to
             * the next page in the buffer. */
            if (pages[i] != page) continue;

            uintptr_t pstart = (uintptr_t)page->start;
            uintptr_t pend = pstart + (page->total_slots * size_pool->slot_size);

            if (data->each_obj_callback &&
                (*data->each_obj_callback)((void *)pstart, (void *)pend, size_pool->slot_size, data->data)) {
                break;
            }
            if (data->each_page_callback &&
                (*data->each_page_callback)(page, data->data)) {
                break;
            }

            page = ccan_list_next(&SIZE_POOL_EDEN_HEAP(size_pool)->pages, page, page_node);
        }
    }

    return Qnil;
}

static void
objspace_each_exec(bool protected, struct each_obj_data *each_obj_data)
{
    each_obj_data->reenable_incremental = FALSE;
    memset(&each_obj_data->pages, 0, sizeof(each_obj_data->pages));
    memset(&each_obj_data->pages_counts, 0, sizeof(each_obj_data->pages_counts));
    rb_ensure(objspace_each_objects_try, (VALUE)each_obj_data,
              objspace_each_objects_ensure, (VALUE)each_obj_data);
}

static void
objspace_each_objects(rb_objspace_t *objspace, each_obj_callback *callback, void *data, bool protected)
{
    struct each_obj_data each_obj_data = {
        .objspace = objspace,
        .each_obj_callback = callback,
        .each_page_callback = NULL,
        .data = data,
    };
    objspace_each_exec(protected, &each_obj_data);
}

void
rb_gc_impl_each_objects(void *objspace_ptr, each_obj_callback *callback, void *data)
{
    objspace_each_objects(objspace_ptr, callback, data, TRUE);
}

VALUE
rb_gc_impl_define_finalizer(void *objspace_ptr, VALUE obj, VALUE block)
{
    rb_objspace_t *objspace = objspace_ptr;
    VALUE table;
    st_data_t data;

    RBASIC(obj)->flags |= FL_FINALIZE;

    if (st_lookup(finalizer_table, obj, &data)) {
        table = (VALUE)data;

        /* avoid duplicate block, table is usually small */
        {
            long len = RARRAY_LEN(table);
            long i;

            for (i = 0; i < len; i++) {
                VALUE recv = RARRAY_AREF(table, i);
                if (rb_equal(recv, block)) {
                    block = recv;
                    goto end;
                }
            }
        }

        rb_ary_push(table, block);
    }
    else {
        table = rb_ary_new3(1, block);
        *(VALUE *)&RBASIC(table)->klass = 0;
        st_add_direct(finalizer_table, obj, table);
    }
  end:
    block = rb_ary_new3(2, INT2FIX(0), block);
    OBJ_FREEZE(block);
    return block;
}

VALUE
rb_gc_impl_undefine_finalizer(void *objspace_ptr, VALUE obj)
{
    rb_objspace_t *objspace = objspace_ptr;
    st_data_t data = obj;
    rb_check_frozen(obj);
    st_delete(finalizer_table, &data, 0);
    FL_UNSET(obj, FL_FINALIZE);
    return obj;
}

VALUE
rb_gc_impl_get_finalizers(void *objspace_ptr, VALUE obj)
{
    rb_objspace_t *objspace = objspace_ptr;

    if (FL_TEST(obj, FL_FINALIZE)) {
        st_data_t data;
        if (st_lookup(finalizer_table, obj, &data)) {
            return (VALUE)data;
        }
    }

    return Qnil;
}

void
rb_gc_impl_copy_finalizer(void *objspace_ptr, VALUE dest, VALUE obj)
{
    rb_objspace_t *objspace = objspace_ptr;
    VALUE table;
    st_data_t data;

    if (!FL_TEST(obj, FL_FINALIZE)) return;

    if (RB_LIKELY(st_lookup(finalizer_table, obj, &data))) {
        table = (VALUE)data;
        st_insert(finalizer_table, dest, table);
        FL_SET(dest, FL_FINALIZE);
    }
    else {
        rb_bug("rb_gc_copy_finalizer: FL_FINALIZE set but not found in finalizer_table: %s", rb_obj_info(obj));
    }
}

static VALUE
get_final(long i, void *data)
{
    VALUE table = (VALUE)data;

    return RARRAY_AREF(table, i);
}

static void
run_final(rb_objspace_t *objspace, VALUE zombie)
{
    if (RZOMBIE(zombie)->dfree) {
        RZOMBIE(zombie)->dfree(RZOMBIE(zombie)->data);
    }

    st_data_t key = (st_data_t)zombie;
    if (FL_TEST_RAW(zombie, FL_FINALIZE)) {
        FL_UNSET(zombie, FL_FINALIZE);
        st_data_t table;
        if (st_delete(finalizer_table, &key, &table)) {
            rb_gc_run_obj_finalizer(rb_gc_impl_object_id(objspace, zombie), RARRAY_LEN(table), get_final, (void *)table);
        }
        else {
            rb_bug("FL_FINALIZE flag is set, but finalizers are not found");
        }
    }
    else {
        GC_ASSERT(!st_lookup(finalizer_table, key, NULL));
    }
}

static void
finalize_list(rb_objspace_t *objspace, VALUE zombie)
{
    while (zombie) {
        VALUE next_zombie;
        struct heap_page *page;
        asan_unpoison_object(zombie, false);
        next_zombie = RZOMBIE(zombie)->next;
        page = GET_HEAP_PAGE(zombie);

        run_final(objspace, zombie);

        int lev = rb_gc_vm_lock();
        {
            GC_ASSERT(BUILTIN_TYPE(zombie) == T_ZOMBIE);
            if (FL_TEST(zombie, FL_SEEN_OBJ_ID)) {
                obj_free_object_id(objspace, zombie);
            }

            GC_ASSERT(heap_pages_final_slots > 0);
            GC_ASSERT(page->final_slots > 0);

            heap_pages_final_slots--;
            page->final_slots--;
            page->free_slots++;
            heap_page_add_freeobj(objspace, page, zombie);
            page->size_pool->total_freed_objects++;
        }
        rb_gc_vm_unlock(lev);

        zombie = next_zombie;
    }
}

static void
finalize_deferred_heap_pages(rb_objspace_t *objspace)
{
    VALUE zombie;
    while ((zombie = RUBY_ATOMIC_VALUE_EXCHANGE(heap_pages_deferred_final, 0)) != 0) {
        finalize_list(objspace, zombie);
    }
}

static void
finalize_deferred(rb_objspace_t *objspace)
{
    rb_gc_set_pending_interrupt();
    finalize_deferred_heap_pages(objspace);
    rb_gc_unset_pending_interrupt();
}

static void
gc_finalize_deferred(void *dmy)
{
    rb_objspace_t *objspace = dmy;
    if (RUBY_ATOMIC_EXCHANGE(finalizing, 1)) return;

    finalize_deferred(objspace);
    RUBY_ATOMIC_SET(finalizing, 0);
}

struct force_finalize_list {
    VALUE obj;
    VALUE table;
    struct force_finalize_list *next;
};

static int
force_chain_object(st_data_t key, st_data_t val, st_data_t arg)
{
    struct force_finalize_list **prev = (struct force_finalize_list **)arg;
    struct force_finalize_list *curr = ALLOC(struct force_finalize_list);
    curr->obj = key;
    curr->table = val;
    curr->next = *prev;
    *prev = curr;
    return ST_CONTINUE;
}

void
rb_gc_impl_shutdown_free_objects(void *objspace_ptr)
{
    rb_objspace_t *objspace = objspace_ptr;

    for (size_t i = 0; i < heap_allocated_pages; i++) {
        struct heap_page *page = heap_pages_sorted[i];
        short stride = page->slot_size;

        uintptr_t p = (uintptr_t)page->start;
        uintptr_t pend = p + page->total_slots * stride;
        for (; p < pend; p += stride) {
            VALUE vp = (VALUE)p;
            switch (BUILTIN_TYPE(vp)) {
              case T_NONE:
              case T_SYMBOL:
                break;
              default:
                rb_gc_obj_free(objspace, vp);
                break;
            }
        }
    }
}

void
rb_gc_impl_shutdown_call_finalizer(void *objspace_ptr)
{
    rb_objspace_t *objspace = objspace_ptr;

#if RGENGC_CHECK_MODE >= 2
    rb_gc_impl_verify_internal_consistency(objspace);
#endif
    if (RUBY_ATOMIC_EXCHANGE(finalizing, 1)) return;

    /* run finalizers */
    finalize_deferred(objspace);
    GC_ASSERT(heap_pages_deferred_final == 0);

    /* prohibit incremental GC */
    objspace->flags.dont_incremental = 1;

    /* force to run finalizer */
    while (finalizer_table->num_entries) {
        struct force_finalize_list *list = 0;
        st_foreach(finalizer_table, force_chain_object, (st_data_t)&list);
        while (list) {
            struct force_finalize_list *curr = list;

            st_data_t obj = (st_data_t)curr->obj;
            st_delete(finalizer_table, &obj, 0);
            FL_UNSET(curr->obj, FL_FINALIZE);

            rb_gc_run_obj_finalizer(rb_gc_impl_object_id(objspace, curr->obj), RARRAY_LEN(curr->table), get_final, (void *)curr->table);

            list = curr->next;
            xfree(curr);
        }
    }

    /* run data/file object's finalizers */
    for (size_t i = 0; i < heap_allocated_pages; i++) {
        struct heap_page *page = heap_pages_sorted[i];
        short stride = page->slot_size;

        uintptr_t p = (uintptr_t)page->start;
        uintptr_t pend = p + page->total_slots * stride;
        for (; p < pend; p += stride) {
            VALUE vp = (VALUE)p;
            void *poisoned = asan_unpoison_object_temporary(vp);

            if (rb_gc_shutdown_call_finalizer_p(vp)) {
                rb_gc_obj_free(objspace, vp);
            }

            if (poisoned) {
                GC_ASSERT(BUILTIN_TYPE(vp) == T_NONE);
                asan_poison_object(vp);
            }
        }
    }

    finalize_deferred_heap_pages(objspace);

    st_free_table(finalizer_table);
    finalizer_table = 0;
    RUBY_ATOMIC_SET(finalizing, 0);
}

void
rb_gc_impl_each_object(void *objspace_ptr, void (*func)(VALUE obj, void *data), void *data)
{
    rb_objspace_t *objspace = objspace_ptr;

    for (size_t i = 0; i < heap_allocated_pages; i++) {
        struct heap_page *page = heap_pages_sorted[i];
        short stride = page->slot_size;

        uintptr_t p = (uintptr_t)page->start;
        uintptr_t pend = p + page->total_slots * stride;
        for (; p < pend; p += stride) {
            VALUE obj = (VALUE)p;

            void *poisoned = asan_unpoison_object_temporary(obj);

            func(obj, data);

            if (poisoned) {
                GC_ASSERT(BUILTIN_TYPE(obj) == T_NONE);
                asan_poison_object(obj);
            }
        }
    }
}

/*
  ------------------------ Garbage Collection ------------------------
*/

/* Sweeping */

static size_t
objspace_available_slots(rb_objspace_t *objspace)
{
    size_t total_slots = 0;
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        rb_size_pool_t *size_pool = &size_pools[i];
        total_slots += SIZE_POOL_EDEN_HEAP(size_pool)->total_slots;
        total_slots += SIZE_POOL_TOMB_HEAP(size_pool)->total_slots;
    }
    return total_slots;
}

static size_t
objspace_live_slots(rb_objspace_t *objspace)
{
    return total_allocated_objects(objspace) - total_freed_objects(objspace) - heap_pages_final_slots;
}

static size_t
objspace_free_slots(rb_objspace_t *objspace)
{
    return objspace_available_slots(objspace) - objspace_live_slots(objspace) - heap_pages_final_slots;
}

#if defined(_WIN32)
enum {HEAP_PAGE_LOCK = PAGE_NOACCESS, HEAP_PAGE_UNLOCK = PAGE_READWRITE};

static BOOL
protect_page_body(struct heap_page_body *body, DWORD protect)
{
    DWORD old_protect;
    return VirtualProtect(body, HEAP_PAGE_SIZE, protect, &old_protect) != 0;
}
#else
enum {HEAP_PAGE_LOCK = PROT_NONE, HEAP_PAGE_UNLOCK = PROT_READ | PROT_WRITE};
#define protect_page_body(body, protect) !mprotect((body), HEAP_PAGE_SIZE, (protect))
#endif

#if defined(__MINGW32__) || defined(_WIN32)
# define GC_COMPACTION_SUPPORTED 1
#else
/* If not MinGW, Windows, or does not have mmap, we cannot use mprotect for
 * the read barrier, so we must disable compaction. */
# define GC_COMPACTION_SUPPORTED (GC_CAN_COMPILE_COMPACTION && HEAP_PAGE_ALLOC_USE_MMAP)
#endif

#if GC_CAN_COMPILE_COMPACTION
static void
read_barrier_handler(uintptr_t original_address)
{
    VALUE obj;
    rb_objspace_t *objspace = (rb_objspace_t *)rb_gc_get_objspace();

    /* Calculate address aligned to slots. */
    uintptr_t address = original_address - (original_address % BASE_SLOT_SIZE);

    obj = (VALUE)address;

    struct heap_page_body *page_body = GET_PAGE_BODY(obj);

    /* If the page_body is NULL, then mprotect cannot handle it and will crash
     * with "Cannot allocate memory". */
    if (page_body == NULL) {
        rb_bug("read_barrier_handler: segmentation fault at %p", (void *)original_address);
    }

    int lev = rb_gc_vm_lock();
    {
        unlock_page_body(objspace, page_body);

        objspace->profile.read_barrier_faults++;

        invalidate_moved_page(objspace, GET_HEAP_PAGE(obj));
    }
    rb_gc_vm_unlock(lev);
}
#endif

#if !GC_CAN_COMPILE_COMPACTION
#elif defined(_WIN32)
static LPTOP_LEVEL_EXCEPTION_FILTER old_handler;
typedef void (*signal_handler)(int);
static signal_handler old_sigsegv_handler;

static LONG WINAPI
read_barrier_signal(EXCEPTION_POINTERS *info)
{
    /* EXCEPTION_ACCESS_VIOLATION is what's raised by access to protected pages */
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        /* > The second array element specifies the virtual address of the inaccessible data.
         * https://docs.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-exception_record
         *
         * Use this address to invalidate the page */
        read_barrier_handler((uintptr_t)info->ExceptionRecord->ExceptionInformation[1]);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    else {
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

static void
uninstall_handlers(void)
{
    signal(SIGSEGV, old_sigsegv_handler);
    SetUnhandledExceptionFilter(old_handler);
}

static void
install_handlers(void)
{
    /* Remove SEGV handler so that the Unhandled Exception Filter handles it */
    old_sigsegv_handler = signal(SIGSEGV, NULL);
    /* Unhandled Exception Filter has access to the violation address similar
     * to si_addr from sigaction */
    old_handler = SetUnhandledExceptionFilter(read_barrier_signal);
}
#else
static struct sigaction old_sigbus_handler;
static struct sigaction old_sigsegv_handler;

#ifdef HAVE_MACH_TASK_EXCEPTION_PORTS
static exception_mask_t old_exception_masks[32];
static mach_port_t old_exception_ports[32];
static exception_behavior_t old_exception_behaviors[32];
static thread_state_flavor_t old_exception_flavors[32];
static mach_msg_type_number_t old_exception_count;

static void
disable_mach_bad_access_exc(void)
{
    old_exception_count = sizeof(old_exception_masks) / sizeof(old_exception_masks[0]);
    task_swap_exception_ports(
        mach_task_self(), EXC_MASK_BAD_ACCESS,
        MACH_PORT_NULL, EXCEPTION_DEFAULT, 0,
        old_exception_masks, &old_exception_count,
        old_exception_ports, old_exception_behaviors, old_exception_flavors
    );
}

static void
restore_mach_bad_access_exc(void)
{
    for (mach_msg_type_number_t i = 0; i < old_exception_count; i++) {
        task_set_exception_ports(
            mach_task_self(),
            old_exception_masks[i], old_exception_ports[i],
            old_exception_behaviors[i], old_exception_flavors[i]
        );
    }
}
#endif

static void
read_barrier_signal(int sig, siginfo_t *info, void *data)
{
    // setup SEGV/BUS handlers for errors
    struct sigaction prev_sigbus, prev_sigsegv;
    sigaction(SIGBUS, &old_sigbus_handler, &prev_sigbus);
    sigaction(SIGSEGV, &old_sigsegv_handler, &prev_sigsegv);

    // enable SIGBUS/SEGV
    sigset_t set, prev_set;
    sigemptyset(&set);
    sigaddset(&set, SIGBUS);
    sigaddset(&set, SIGSEGV);
    sigprocmask(SIG_UNBLOCK, &set, &prev_set);
#ifdef HAVE_MACH_TASK_EXCEPTION_PORTS
    disable_mach_bad_access_exc();
#endif
    // run handler
    read_barrier_handler((uintptr_t)info->si_addr);

    // reset SEGV/BUS handlers
#ifdef HAVE_MACH_TASK_EXCEPTION_PORTS
    restore_mach_bad_access_exc();
#endif
    sigaction(SIGBUS, &prev_sigbus, NULL);
    sigaction(SIGSEGV, &prev_sigsegv, NULL);
    sigprocmask(SIG_SETMASK, &prev_set, NULL);
}

static void
uninstall_handlers(void)
{
#ifdef HAVE_MACH_TASK_EXCEPTION_PORTS
    restore_mach_bad_access_exc();
#endif
    sigaction(SIGBUS, &old_sigbus_handler, NULL);
    sigaction(SIGSEGV, &old_sigsegv_handler, NULL);
}

static void
install_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = read_barrier_signal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;

    sigaction(SIGBUS, &action, &old_sigbus_handler);
    sigaction(SIGSEGV, &action, &old_sigsegv_handler);
#ifdef HAVE_MACH_TASK_EXCEPTION_PORTS
    disable_mach_bad_access_exc();
#endif
}
#endif

static void
heap_page_freelist_append(struct heap_page *page, struct free_slot *freelist)
{
    if (freelist) {
        asan_unlock_freelist(page);
        if (page->freelist) {
            struct free_slot *p = page->freelist;
            asan_unpoison_object((VALUE)p, false);
            while (p->next) {
                struct free_slot *prev = p;
                p = p->next;
                asan_poison_object((VALUE)prev);
                asan_unpoison_object((VALUE)p, false);
            }
            p->next = freelist;
            asan_poison_object((VALUE)p);
        }
        else {
            page->freelist = freelist;
        }
        asan_lock_freelist(page);
    }
}

#if defined(__GNUC__) && __GNUC__ == 4 && __GNUC_MINOR__ == 4
__attribute__((noinline))
#endif

#if GC_CAN_COMPILE_COMPACTION
static void gc_sort_heap_by_compare_func(rb_objspace_t *objspace, gc_compact_compare_func compare_func);
static int compare_pinned_slots(const void *left, const void *right, void *d);
#endif

static void
gc_ractor_newobj_cache_clear(void *c, void *data)
{
    rb_ractor_newobj_cache_t *newobj_cache = c;

    newobj_cache->incremental_mark_step_allocated_slots = 0;

    for (size_t size_pool_idx = 0; size_pool_idx < SIZE_POOL_COUNT; size_pool_idx++) {
        rb_ractor_newobj_size_pool_cache_t *cache = &newobj_cache->size_pool_caches[size_pool_idx];

        struct heap_page *page = cache->using_page;
        struct free_slot *freelist = cache->freelist;
        RUBY_DEBUG_LOG("ractor using_page:%p freelist:%p", (void *)page, (void *)freelist);

        heap_page_freelist_append(page, freelist);

        cache->using_page = NULL;
        cache->freelist = NULL;
    }
}

VALUE
rb_gc_impl_location(void *objspace_ptr, VALUE value)
{
    VALUE destination;

    if (!SPECIAL_CONST_P(value)) {
        void *poisoned = asan_unpoison_object_temporary(value);

        if (BUILTIN_TYPE(value) == T_MOVED) {
            destination = (VALUE)RMOVED(value)->destination;
            GC_ASSERT(BUILTIN_TYPE(destination) != T_NONE);
        }
        else {
            destination = value;
        }

        /* Re-poison slot if it's not the one we want */
        if (poisoned) {
            GC_ASSERT(BUILTIN_TYPE(value) == T_NONE);
            asan_poison_object(value);
        }
    }
    else {
        destination = value;
    }

    return destination;
}

static stack_chunk_t *
stack_chunk_alloc(void)
{
    stack_chunk_t *res;

    res = malloc(sizeof(stack_chunk_t));
    if (!res)
        rb_memerror();

    return res;
}

static void
add_stack_chunk_cache(mark_stack_t *stack, stack_chunk_t *chunk)
{
    chunk->next = stack->cache;
    stack->cache = chunk;
    stack->cache_size++;
}

static void
push_mark_stack_chunk(mark_stack_t *stack)
{
    stack_chunk_t *next;

    GC_ASSERT(stack->index == stack->limit);

    if (stack->cache_size > 0) {
        next = stack->cache;
        stack->cache = stack->cache->next;
        stack->cache_size--;
        if (stack->unused_cache_size > stack->cache_size)
            stack->unused_cache_size = stack->cache_size;
    }
    else {
        next = stack_chunk_alloc();
    }
    next->next = stack->chunk;
    stack->chunk = next;
    stack->index = 0;
}

static void
mark_stack_chunk_list_free(stack_chunk_t *chunk)
{
    stack_chunk_t *next = NULL;

    while (chunk != NULL) {
        next = chunk->next;
        free(chunk);
        chunk = next;
    }
}

static void
free_stack_chunks(mark_stack_t *stack)
{
    mark_stack_chunk_list_free(stack->chunk);
}

static void
mark_stack_free_cache(mark_stack_t *stack)
{
    mark_stack_chunk_list_free(stack->cache);
    stack->cache_size = 0;
    stack->unused_cache_size = 0;
}

static void
push_mark_stack(mark_stack_t *stack, VALUE obj)
{
    switch (BUILTIN_TYPE(obj)) {
      case T_OBJECT:
      case T_CLASS:
      case T_MODULE:
      case T_FLOAT:
      case T_STRING:
      case T_REGEXP:
      case T_ARRAY:
      case T_HASH:
      case T_STRUCT:
      case T_BIGNUM:
      case T_FILE:
      case T_DATA:
      case T_MATCH:
      case T_COMPLEX:
      case T_RATIONAL:
      case T_TRUE:
      case T_FALSE:
      case T_SYMBOL:
      case T_IMEMO:
      case T_ICLASS:
        if (stack->index == stack->limit) {
            push_mark_stack_chunk(stack);
        }
        stack->chunk->data[stack->index++] = obj;
        return;

      case T_NONE:
      case T_NIL:
      case T_FIXNUM:
      case T_MOVED:
      case T_ZOMBIE:
      case T_UNDEF:
      case T_MASK:
        rb_bug("push_mark_stack() called for broken object");
        break;

      case T_NODE:
        rb_bug("push_mark_stack: unexpected T_NODE object");
        break;
    }

    rb_bug("rb_gc_mark(): unknown data type 0x%x(%p) %s",
            BUILTIN_TYPE(obj), (void *)obj,
            is_pointer_to_heap((rb_objspace_t *)rb_gc_get_objspace(), (void *)obj) ? "corrupted object" : "non object");
}

static void
init_mark_stack(mark_stack_t *stack)
{
    int i;

    MEMZERO(stack, mark_stack_t, 1);
    stack->index = stack->limit = STACK_CHUNK_SIZE;

    for (i=0; i < 4; i++) {
        add_stack_chunk_cache(stack, stack_chunk_alloc());
    }
    stack->unused_cache_size = stack->cache_size;
}

void
rb_gc_impl_mark_and_move(void *objspace_ptr, VALUE *ptr)
{
}

void
rb_gc_impl_mark(void *objspace_ptr, VALUE obj)
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

#if RGENGC_CHECK_MODE >= 4

#define MAKE_ROOTSIG(obj) (((VALUE)(obj) << 1) | 0x01)
#define IS_ROOTSIG(obj)   ((VALUE)(obj) & 0x01)
#define GET_ROOTSIG(obj)  ((const char *)((VALUE)(obj) >> 1))

struct reflist {
    VALUE *list;
    int pos;
    int size;
};

static struct reflist *
reflist_create(VALUE obj)
{
    struct reflist *refs = xmalloc(sizeof(struct reflist));
    refs->size = 1;
    refs->list = ALLOC_N(VALUE, refs->size);
    refs->list[0] = obj;
    refs->pos = 1;
    return refs;
}

static void
reflist_destruct(struct reflist *refs)
{
    xfree(refs->list);
    xfree(refs);
}

static void
reflist_add(struct reflist *refs, VALUE obj)
{
    if (refs->pos == refs->size) {
        refs->size *= 2;
        SIZED_REALLOC_N(refs->list, VALUE, refs->size, refs->size/2);
    }

    refs->list[refs->pos++] = obj;
}

static void
reflist_dump(struct reflist *refs)
{
    int i;
    for (i=0; i<refs->pos; i++) {
        VALUE obj = refs->list[i];
        if (IS_ROOTSIG(obj)) { /* root */
            fprintf(stderr, "<root@%s>", GET_ROOTSIG(obj));
        }
        else {
            fprintf(stderr, "<%s>", rb_obj_info(obj));
        }
        if (i+1 < refs->pos) fprintf(stderr, ", ");
    }
}

static int
reflist_referred_from_machine_context(struct reflist *refs)
{
    int i;
    for (i=0; i<refs->pos; i++) {
        VALUE obj = refs->list[i];
        if (IS_ROOTSIG(obj) && strcmp(GET_ROOTSIG(obj), "machine_context") == 0) return 1;
    }
    return 0;
}

struct allrefs {
    rb_objspace_t *objspace;
    /* a -> obj1
     * b -> obj1
     * c -> obj1
     * c -> obj2
     * d -> obj3
     * #=> {obj1 => [a, b, c], obj2 => [c, d]}
     */
    struct st_table *references;
    const char *category;
    VALUE root_obj;
    mark_stack_t mark_stack;
};

static int
allrefs_add(struct allrefs *data, VALUE obj)
{
    struct reflist *refs;
    st_data_t r;

    if (st_lookup(data->references, obj, &r)) {
        refs = (struct reflist *)r;
        reflist_add(refs, data->root_obj);
        return 0;
    }
    else {
        refs = reflist_create(data->root_obj);
        st_insert(data->references, obj, (st_data_t)refs);
        return 1;
    }
}

static void
allrefs_i(VALUE obj, void *ptr)
{
    struct allrefs *data = (struct allrefs *)ptr;

    if (allrefs_add(data, obj)) {
        push_mark_stack(&data->mark_stack, obj);
    }
}

static void
allrefs_roots_i(VALUE obj, void *ptr)
{
    struct allrefs *data = (struct allrefs *)ptr;
    if (strlen(data->category) == 0) rb_bug("!!!");
    data->root_obj = MAKE_ROOTSIG(data->category);

    if (allrefs_add(data, obj)) {
        push_mark_stack(&data->mark_stack, obj);
    }
}
#define PUSH_MARK_FUNC_DATA(v) do { \
    struct gc_mark_func_data_struct *prev_mark_func_data = GET_RACTOR()->mfd; \
    GET_RACTOR()->mfd = (v);

#define POP_MARK_FUNC_DATA() GET_RACTOR()->mfd = prev_mark_func_data;} while (0)

static st_table *
objspace_allrefs(rb_objspace_t *objspace)
{
    struct allrefs data;
    struct gc_mark_func_data_struct mfd;
    VALUE obj;
    int prev_dont_gc = dont_gc_val();
    dont_gc_on();

    data.objspace = objspace;
    data.references = st_init_numtable();
    init_mark_stack(&data.mark_stack);

    mfd.mark_func = allrefs_roots_i;
    mfd.data = &data;

    /* traverse root objects */
    PUSH_MARK_FUNC_DATA(&mfd);
    GET_RACTOR()->mfd = &mfd;
    rb_gc_mark_roots(objspace, &data.category);
    POP_MARK_FUNC_DATA();

    /* traverse rest objects reachable from root objects */
    while (pop_mark_stack(&data.mark_stack, &obj)) {
        rb_objspace_reachable_objects_from(data.root_obj = obj, allrefs_i, &data);
    }
    free_stack_chunks(&data.mark_stack);

    dont_gc_set(prev_dont_gc);
    return data.references;
}

static int
objspace_allrefs_destruct_i(st_data_t key, st_data_t value, st_data_t ptr)
{
    struct reflist *refs = (struct reflist *)value;
    reflist_destruct(refs);
    return ST_CONTINUE;
}

static void
objspace_allrefs_destruct(struct st_table *refs)
{
    st_foreach(refs, objspace_allrefs_destruct_i, 0);
    st_free_table(refs);
}

#if RGENGC_CHECK_MODE >= 5
static int
allrefs_dump_i(st_data_t k, st_data_t v, st_data_t ptr)
{
    VALUE obj = (VALUE)k;
    struct reflist *refs = (struct reflist *)v;
    fprintf(stderr, "[allrefs_dump_i] %s <- ", rb_obj_info(obj));
    reflist_dump(refs);
    fprintf(stderr, "\n");
    return ST_CONTINUE;
}

static void
allrefs_dump(rb_objspace_t *objspace)
{
    VALUE size = objspace->rgengc.allrefs_table->num_entries;
    fprintf(stderr, "[all refs] (size: %"PRIuVALUE")\n", size);
    st_foreach(objspace->rgengc.allrefs_table, allrefs_dump_i, 0);
}
#endif

static int
gc_check_after_marks_i(st_data_t k, st_data_t v, st_data_t ptr)
{
    VALUE obj = k;
    struct reflist *refs = (struct reflist *)v;
    rb_objspace_t *objspace = (rb_objspace_t *)ptr;

    /* object should be marked or oldgen */
    if (!MARKED_IN_BITMAP(GET_HEAP_MARK_BITS(obj), obj)) {
        fprintf(stderr, "gc_check_after_marks_i: %s is not marked and not oldgen.\n", rb_obj_info(obj));
        fprintf(stderr, "gc_check_after_marks_i: %p is referred from ", (void *)obj);
        reflist_dump(refs);

        if (reflist_referred_from_machine_context(refs)) {
            fprintf(stderr, " (marked from machine stack).\n");
            /* marked from machine context can be false positive */
        }
        else {
            objspace->rgengc.error_count++;
            fprintf(stderr, "\n");
        }
    }
    return ST_CONTINUE;
}

static void
gc_marks_check(rb_objspace_t *objspace, st_foreach_callback_func *checker_func, const char *checker_name)
{
    size_t saved_malloc_increase = objspace->malloc_params.increase;
#if RGENGC_ESTIMATE_OLDMALLOC
    size_t saved_oldmalloc_increase = objspace->rgengc.oldmalloc_increase;
#endif
    VALUE already_disabled = rb_objspace_gc_disable(objspace);

    objspace->rgengc.allrefs_table = objspace_allrefs(objspace);

    if (checker_func) {
        st_foreach(objspace->rgengc.allrefs_table, checker_func, (st_data_t)objspace);
    }

    if (objspace->rgengc.error_count > 0) {
#if RGENGC_CHECK_MODE >= 5
        allrefs_dump(objspace);
#endif
        if (checker_name) rb_bug("%s: GC has problem.", checker_name);
    }

    objspace_allrefs_destruct(objspace->rgengc.allrefs_table);
    objspace->rgengc.allrefs_table = 0;

    if (already_disabled == Qfalse) rb_objspace_gc_enable(objspace);
    objspace->malloc_params.increase = saved_malloc_increase;
#if RGENGC_ESTIMATE_OLDMALLOC
    objspace->rgengc.oldmalloc_increase = saved_oldmalloc_increase;
#endif
}
#endif /* RGENGC_CHECK_MODE >= 4 */

struct verify_internal_consistency_struct {
    rb_objspace_t *objspace;
    int err_count;
    size_t live_object_count;
    size_t zombie_object_count;

    VALUE parent;
    size_t old_object_count;
    size_t remembered_shady_count;
};

static void
check_generation_i(const VALUE child, void *ptr)
{
    struct verify_internal_consistency_struct *data = (struct verify_internal_consistency_struct *)ptr;
    const VALUE parent = data->parent;

    if (RGENGC_CHECK_MODE) GC_ASSERT(RVALUE_OLD_P(data->objspace, parent));

    if (!RVALUE_OLD_P(data->objspace, child)) {
        if (!RVALUE_REMEMBERED(data->objspace, parent) &&
            !RVALUE_REMEMBERED(data->objspace, child) &&
            !RVALUE_UNCOLLECTIBLE(data->objspace, child)) {
            fprintf(stderr, "verify_internal_consistency_reachable_i: WB miss (O->Y) %s -> %s\n", rb_obj_info(parent), rb_obj_info(child));
            data->err_count++;
        }
    }
}

static void
check_color_i(const VALUE child, void *ptr)
{
    struct verify_internal_consistency_struct *data = (struct verify_internal_consistency_struct *)ptr;
    const VALUE parent = data->parent;

    if (!RVALUE_WB_UNPROTECTED(data->objspace, parent) && RVALUE_WHITE_P(data->objspace, child)) {
        fprintf(stderr, "verify_internal_consistency_reachable_i: WB miss (B->W) - %s -> %s\n",
                rb_obj_info(parent), rb_obj_info(child));
        data->err_count++;
    }
}

static void
check_children_i(const VALUE child, void *ptr)
{
    struct verify_internal_consistency_struct *data = (struct verify_internal_consistency_struct *)ptr;
    if (check_rvalue_consistency_force(data->objspace, child, FALSE) != 0) {
        fprintf(stderr, "check_children_i: %s has error (referenced from %s)",
                rb_obj_info(child), rb_obj_info(data->parent));

        data->err_count++;
    }
}

static int
verify_internal_consistency_i(void *page_start, void *page_end, size_t stride,
                              struct verify_internal_consistency_struct *data)
{
    VALUE obj;
    rb_objspace_t *objspace = data->objspace;

    for (obj = (VALUE)page_start; obj != (VALUE)page_end; obj += stride) {
        void *poisoned = asan_unpoison_object_temporary(obj);

        if (!rb_gc_impl_garbage_object_p(objspace, obj)) {
            /* count objects */
            data->live_object_count++;
            data->parent = obj;

            /* Normally, we don't expect T_MOVED objects to be in the heap.
             * But they can stay alive on the stack, */
            if (!gc_object_moved_p(objspace, obj)) {
                /* moved slots don't have children */
                rb_objspace_reachable_objects_from(obj, check_children_i, (void *)data);
            }

            /* check health of children */
            if (RVALUE_OLD_P(objspace, obj)) data->old_object_count++;
            if (RVALUE_WB_UNPROTECTED(objspace, obj) && RVALUE_UNCOLLECTIBLE(objspace, obj)) data->remembered_shady_count++;

            if (!is_marking(objspace) && RVALUE_OLD_P(objspace, obj)) {
                /* reachable objects from an oldgen object should be old or (young with remember) */
                data->parent = obj;
                rb_objspace_reachable_objects_from(obj, check_generation_i, (void *)data);
            }

            if (is_incremental_marking(objspace)) {
                if (RVALUE_BLACK_P(objspace, obj)) {
                    /* reachable objects from black objects should be black or grey objects */
                    data->parent = obj;
                    rb_objspace_reachable_objects_from(obj, check_color_i, (void *)data);
                }
            }
        }
        else {
            if (BUILTIN_TYPE(obj) == T_ZOMBIE) {
                data->zombie_object_count++;

                if ((RBASIC(obj)->flags & ~ZOMBIE_OBJ_KEPT_FLAGS) != T_ZOMBIE) {
                    fprintf(stderr, "verify_internal_consistency_i: T_ZOMBIE has extra flags set: %s\n",
                            rb_obj_info(obj));
                    data->err_count++;
                }

                if (!!FL_TEST(obj, FL_FINALIZE) != !!st_is_member(finalizer_table, obj)) {
                    fprintf(stderr, "verify_internal_consistency_i: FL_FINALIZE %s but %s finalizer_table: %s\n",
                            FL_TEST(obj, FL_FINALIZE) ? "set" : "not set", st_is_member(finalizer_table, obj) ? "in" : "not in",
                            rb_obj_info(obj));
                    data->err_count++;
                }
            }
        }
        if (poisoned) {
            GC_ASSERT(BUILTIN_TYPE(obj) == T_NONE);
            asan_poison_object(obj);
        }
    }

    return 0;
}

static int
gc_verify_heap_page(rb_objspace_t *objspace, struct heap_page *page, VALUE obj)
{
    unsigned int has_remembered_shady = FALSE;
    unsigned int has_remembered_old = FALSE;
    int remembered_old_objects = 0;
    int free_objects = 0;
    int zombie_objects = 0;

    short slot_size = page->slot_size;
    uintptr_t start = (uintptr_t)page->start;
    uintptr_t end = start + page->total_slots * slot_size;

    for (uintptr_t ptr = start; ptr < end; ptr += slot_size) {
        VALUE val = (VALUE)ptr;
        void *poisoned = asan_unpoison_object_temporary(val);
        enum ruby_value_type type = BUILTIN_TYPE(val);

        if (type == T_NONE) free_objects++;
        if (type == T_ZOMBIE) zombie_objects++;
        if (RVALUE_PAGE_UNCOLLECTIBLE(page, val) && RVALUE_PAGE_WB_UNPROTECTED(page, val)) {
            has_remembered_shady = TRUE;
        }
        if (RVALUE_PAGE_MARKING(page, val)) {
            has_remembered_old = TRUE;
            remembered_old_objects++;
        }

        if (poisoned) {
            GC_ASSERT(BUILTIN_TYPE(val) == T_NONE);
            asan_poison_object(val);
        }
    }

    if (!is_incremental_marking(objspace) &&
        page->flags.has_remembered_objects == FALSE && has_remembered_old == TRUE) {

        for (uintptr_t ptr = start; ptr < end; ptr += slot_size) {
            VALUE val = (VALUE)ptr;
            if (RVALUE_PAGE_MARKING(page, val)) {
                fprintf(stderr, "marking -> %s\n", rb_obj_info(val));
            }
        }
        rb_bug("page %p's has_remembered_objects should be false, but there are remembered old objects (%d). %s",
               (void *)page, remembered_old_objects, obj ? rb_obj_info(obj) : "");
    }

    if (page->flags.has_uncollectible_wb_unprotected_objects == FALSE && has_remembered_shady == TRUE) {
        rb_bug("page %p's has_remembered_shady should be false, but there are remembered shady objects. %s",
               (void *)page, obj ? rb_obj_info(obj) : "");
    }

    if (0) {
        /* free_slots may not equal to free_objects */
        if (page->free_slots != free_objects) {
            rb_bug("page %p's free_slots should be %d, but %d", (void *)page, page->free_slots, free_objects);
        }
    }
    if (page->final_slots != zombie_objects) {
        rb_bug("page %p's final_slots should be %d, but %d", (void *)page, page->final_slots, zombie_objects);
    }

    return remembered_old_objects;
}

static int
gc_verify_heap_pages_(rb_objspace_t *objspace, struct ccan_list_head *head)
{
    int remembered_old_objects = 0;
    struct heap_page *page = 0;

    ccan_list_for_each(head, page, page_node) {
        asan_unlock_freelist(page);
        struct free_slot *p = page->freelist;
        while (p) {
            VALUE vp = (VALUE)p;
            VALUE prev = vp;
            asan_unpoison_object(vp, false);
            if (BUILTIN_TYPE(vp) != T_NONE) {
                fprintf(stderr, "freelist slot expected to be T_NONE but was: %s\n", rb_obj_info(vp));
            }
            p = p->next;
            asan_poison_object(prev);
        }
        asan_lock_freelist(page);

        if (page->flags.has_remembered_objects == FALSE) {
            remembered_old_objects += gc_verify_heap_page(objspace, page, Qfalse);
        }
    }

    return remembered_old_objects;
}

static int
gc_verify_heap_pages(rb_objspace_t *objspace)
{
    int remembered_old_objects = 0;
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        remembered_old_objects += gc_verify_heap_pages_(objspace, &(SIZE_POOL_EDEN_HEAP(&size_pools[i])->pages));
        remembered_old_objects += gc_verify_heap_pages_(objspace, &(SIZE_POOL_TOMB_HEAP(&size_pools[i])->pages));
    }
    return remembered_old_objects;
}

static void
gc_verify_internal_consistency_(rb_objspace_t *objspace)
{
    struct verify_internal_consistency_struct data = {0};

    data.objspace = objspace;
    gc_report(5, objspace, "gc_verify_internal_consistency: start\n");

    /* check relations */
    for (size_t i = 0; i < heap_allocated_pages; i++) {
        struct heap_page *page = heap_pages_sorted[i];
        short slot_size = page->slot_size;

        uintptr_t start = (uintptr_t)page->start;
        uintptr_t end = start + page->total_slots * slot_size;

        verify_internal_consistency_i((void *)start, (void *)end, slot_size, &data);
    }

    if (data.err_count != 0) {
#if RGENGC_CHECK_MODE >= 5
        objspace->rgengc.error_count = data.err_count;
        gc_marks_check(objspace, NULL, NULL);
        allrefs_dump(objspace);
#endif
        rb_bug("gc_verify_internal_consistency: found internal inconsistency.");
    }

    /* check heap_page status */
    gc_verify_heap_pages(objspace);

    /* check counters */

    if (!is_lazy_sweeping(objspace) &&
            !finalizing) {
        if (objspace_live_slots(objspace) != data.live_object_count) {
            fprintf(stderr, "heap_pages_final_slots: %"PRIdSIZE", total_freed_objects: %"PRIdSIZE"\n",
                    heap_pages_final_slots, total_freed_objects(objspace));
            rb_bug("inconsistent live slot number: expect %"PRIuSIZE", but %"PRIuSIZE".",
                   objspace_live_slots(objspace), data.live_object_count);
        }
    }

    if (!is_marking(objspace)) {
        if (objspace->rgengc.old_objects != data.old_object_count) {
            rb_bug("inconsistent old slot number: expect %"PRIuSIZE", but %"PRIuSIZE".",
                   objspace->rgengc.old_objects, data.old_object_count);
        }
        if (objspace->rgengc.uncollectible_wb_unprotected_objects != data.remembered_shady_count) {
            rb_bug("inconsistent number of wb unprotected objects: expect %"PRIuSIZE", but %"PRIuSIZE".",
                   objspace->rgengc.uncollectible_wb_unprotected_objects, data.remembered_shady_count);
        }
    }

    if (!finalizing) {
        size_t list_count = 0;

        {
            VALUE z = heap_pages_deferred_final;
            while (z) {
                list_count++;
                z = RZOMBIE(z)->next;
            }
        }

        if (heap_pages_final_slots != data.zombie_object_count ||
            heap_pages_final_slots != list_count) {

            rb_bug("inconsistent finalizing object count:\n"
                    "  expect %"PRIuSIZE"\n"
                    "  but    %"PRIuSIZE" zombies\n"
                    "  heap_pages_deferred_final list has %"PRIuSIZE" items.",
                    heap_pages_final_slots,
                    data.zombie_object_count,
                    list_count);
        }
    }

    gc_report(5, objspace, "gc_verify_internal_consistency: OK\n");
}

void
rb_gc_impl_verify_internal_consistency(void *objspace_ptr)
{
    rb_objspace_t *objspace = objspace_ptr;

    unsigned int lev = rb_gc_vm_lock();
    {
        rb_gc_vm_barrier(); // stop other ractors

        unsigned int prev_during_gc = during_gc;
        during_gc = FALSE; // stop gc here
        {
            gc_verify_internal_consistency_(objspace);
        }
        during_gc = prev_during_gc;
    }
    rb_gc_vm_unlock(lev);
}

static void
gc_report_body(int level, rb_objspace_t *objspace, const char *fmt, ...)
{
    if (level <= RGENGC_DEBUG) {
        char buf[1024];
        FILE *out = stderr;
        va_list args;
        const char *status = " ";

        if (during_gc) {
            status = is_full_marking(objspace) ? "+" : "-";
        }
        else {
            if (is_lazy_sweeping(objspace)) {
                status = "S";
            }
            if (is_incremental_marking(objspace)) {
                status = "M";
            }
        }

        va_start(args, fmt);
        vsnprintf(buf, 1024, fmt, args);
        va_end(args);

        fprintf(out, "%s|", status);
        fputs(buf, out);
    }
}

/* bit operations */

static int
rgengc_remembersetbits_set(rb_objspace_t *objspace, VALUE obj)
{
    struct heap_page *page = GET_HEAP_PAGE(obj);
    bits_t *bits = &page->remembered_bits[0];

    if (MARKED_IN_BITMAP(bits, obj)) {
        return FALSE;
    }
    else {
        page->flags.has_remembered_objects = TRUE;
        MARK_IN_BITMAP(bits, obj);
        return TRUE;
    }
}

/* wb, etc */

/* return FALSE if already remembered */
static int
rgengc_remember(rb_objspace_t *objspace, VALUE obj)
{
    gc_report(6, objspace, "rgengc_remember: %s %s\n", rb_obj_info(obj),
              RVALUE_REMEMBERED(objspace, obj) ? "was already remembered" : "is remembered now");

    check_rvalue_consistency(objspace, obj);

    if (RGENGC_CHECK_MODE) {
        if (RVALUE_WB_UNPROTECTED(objspace, obj)) rb_bug("rgengc_remember: %s is not wb protected.", rb_obj_info(obj));
    }

#if RGENGC_PROFILE > 0
    if (!RVALUE_REMEMBERED(obj)) {
        if (RVALUE_WB_UNPROTECTED(obj) == 0) {
            objspace->profile.total_remembered_normal_object_count++;
#if RGENGC_PROFILE >= 2
            objspace->profile.remembered_normal_object_count_types[BUILTIN_TYPE(obj)]++;
#endif
        }
    }
#endif /* RGENGC_PROFILE > 0 */

    return rgengc_remembersetbits_set(objspace, obj);
}

#ifndef PROFILE_REMEMBERSET_MARK
#define PROFILE_REMEMBERSET_MARK 0
#endif

void
rb_gc_impl_writebarrier(void *objspace_ptr, VALUE a, VALUE b)
{
}

void
rb_gc_impl_writebarrier_unprotect(void *objspace_ptr, VALUE obj)
{
}

void
rb_gc_impl_copy_attributes(void *objspace_ptr, VALUE dest, VALUE obj)
{
}

void
rb_gc_impl_writebarrier_remember(void *objspace_ptr, VALUE obj)
{
}

// TODO: rearchitect this function to work for a generic GC
size_t
rb_gc_impl_obj_flags(void *objspace_ptr, VALUE obj, ID* flags, size_t max)
{
    return 0;
}

void *
rb_gc_impl_ractor_cache_alloc(void *objspace_ptr)
{
    rb_objspace_t *objspace = objspace_ptr;

    objspace->live_ractor_cache_count++;

    return calloc1(sizeof(rb_ractor_newobj_cache_t));
}

void
rb_gc_impl_ractor_cache_free(void *objspace_ptr, void *cache)
{
    rb_objspace_t *objspace = objspace_ptr;

    objspace->live_ractor_cache_count--;

    gc_ractor_newobj_cache_clear(cache, NULL);
    free(cache);
}

static bool current_process_time(struct timespec *ts);

int ruby_thread_has_gvl_p(void);

static int
gc_set_candidate_object_i(void *vstart, void *vend, size_t stride, void *data)
{
    rb_objspace_t *objspace = (rb_objspace_t *)data;

    VALUE v = (VALUE)vstart;
    for (; v != (VALUE)vend; v += stride) {
        asan_unpoisoning_object(v) {
            switch (BUILTIN_TYPE(v)) {
              case T_NONE:
              case T_ZOMBIE:
                break;
              case T_STRING:
                // precompute the string coderange. This both save time for when it will be
                // eventually needed, and avoid mutating heap pages after a potential fork.
                rb_enc_str_coderange(v);
                // fall through
              default:
                if (!RVALUE_OLD_P(objspace, v) && !RVALUE_WB_UNPROTECTED(objspace, v)) {
                    RVALUE_AGE_SET_CANDIDATE(objspace, v);
                }
            }
        }
    }

    return 0;
}

void
rb_gc_impl_start(void *objspace_ptr, bool full_mark, bool immediate_mark, bool immediate_sweep, bool compact)
{
    // Starting a GC is a no-op with Epsilon GC
}

static void
free_empty_pages(void *objspace_ptr)
{
    rb_objspace_t *objspace = objspace_ptr;

    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        /* Move all empty pages to the tomb heap for freeing. */
        rb_size_pool_t *size_pool = &size_pools[i];
        rb_heap_t *heap = SIZE_POOL_EDEN_HEAP(size_pool);
        rb_heap_t *tomb_heap = SIZE_POOL_TOMB_HEAP(size_pool);

        size_t freed_pages = 0;

        struct heap_page **next_page_ptr = &heap->free_pages;
        struct heap_page *page = heap->free_pages;
        while (page) {
            /* All finalizers should have been ran in gc_start_internal, so there
            * should be no objects that require finalization. */
            GC_ASSERT(page->final_slots == 0);

            struct heap_page *next_page = page->free_next;

            if (page->free_slots == page->total_slots) {
                heap_unlink_page(objspace, heap, page);
                heap_add_page(objspace, size_pool, tomb_heap, page);
                freed_pages++;
            }
            else {
                *next_page_ptr = page;
                next_page_ptr = &page->free_next;
            }

            page = next_page;
        }

        *next_page_ptr = NULL;

        size_pool_allocatable_pages_set(objspace, size_pool, size_pool->allocatable_pages + freed_pages);
    }

    heap_pages_free_unused_pages(objspace);
}

void
rb_gc_impl_prepare_heap(void *objspace_ptr)
{
    rb_gc_impl_each_objects(objspace_ptr, gc_set_candidate_object_i, objspace_ptr);
    rb_gc_impl_start(objspace_ptr, true, true, true, true);
    free_empty_pages(objspace_ptr);

#if defined(HAVE_MALLOC_TRIM) && !defined(RUBY_ALTERNATIVE_MALLOC_HEADER)
    malloc_trim(0);
#endif
}

void rb_mv_generic_ivar(VALUE src, VALUE dst);

#if GC_CAN_COMPILE_COMPACTION
static int
compare_pinned_slots(const void *left, const void *right, void *dummy)
{
    struct heap_page *left_page;
    struct heap_page *right_page;

    left_page = *(struct heap_page * const *)left;
    right_page = *(struct heap_page * const *)right;

    return left_page->pinned_slots - right_page->pinned_slots;
}

static int
compare_free_slots(const void *left, const void *right, void *dummy)
{
    struct heap_page *left_page;
    struct heap_page *right_page;

    left_page = *(struct heap_page * const *)left;
    right_page = *(struct heap_page * const *)right;

    return left_page->free_slots - right_page->free_slots;
}

static void
gc_sort_heap_by_compare_func(rb_objspace_t *objspace, gc_compact_compare_func compare_func)
{
    for (int j = 0; j < SIZE_POOL_COUNT; j++) {
        rb_size_pool_t *size_pool = &size_pools[j];

        size_t total_pages = SIZE_POOL_EDEN_HEAP(size_pool)->total_pages;
        size_t size = rb_size_mul_or_raise(total_pages, sizeof(struct heap_page *), rb_eRuntimeError);
        struct heap_page *page = 0, **page_list = malloc(size);
        size_t i = 0;

        SIZE_POOL_EDEN_HEAP(size_pool)->free_pages = NULL;
        ccan_list_for_each(&SIZE_POOL_EDEN_HEAP(size_pool)->pages, page, page_node) {
            page_list[i++] = page;
            GC_ASSERT(page);
        }

        GC_ASSERT((size_t)i == total_pages);

        /* Sort the heap so "filled pages" are first. `heap_add_page` adds to the
         * head of the list, so empty pages will end up at the start of the heap */
        ruby_qsort(page_list, total_pages, sizeof(struct heap_page *), compare_func, NULL);

        /* Reset the eden heap */
        ccan_list_head_init(&SIZE_POOL_EDEN_HEAP(size_pool)->pages);

        for (i = 0; i < total_pages; i++) {
            ccan_list_add(&SIZE_POOL_EDEN_HEAP(size_pool)->pages, &page_list[i]->page_node);
            if (page_list[i]->free_slots != 0) {
                heap_add_freepage(SIZE_POOL_EDEN_HEAP(size_pool), page_list[i]);
            }
        }

        free(page_list);
    }
}
#endif

bool
rb_gc_impl_object_moved_p(void *objspace_ptr, VALUE obj)
{
    return FALSE;
}


struct desired_compaction_pages_i_data {
    rb_objspace_t *objspace;
    size_t required_slots[SIZE_POOL_COUNT];
};


bool
rb_gc_impl_during_gc_p(void *objspace_ptr)
{
    return FALSE;
}

#if RGENGC_PROFILE >= 2

static const char *type_name(int type, VALUE obj);

static void
gc_count_add_each_types(VALUE hash, const char *name, const size_t *types)
{
    VALUE result = rb_hash_new_with_size(T_MASK);
    int i;
    for (i=0; i<T_MASK; i++) {
        const char *type = type_name(i, 0);
        rb_hash_aset(result, ID2SYM(rb_intern(type)), SIZET2NUM(types[i]));
    }
    rb_hash_aset(hash, ID2SYM(rb_intern(name)), result);
}
#endif

size_t
rb_gc_impl_gc_count(void *objspace_ptr)
{
    return 0;
}

static VALUE
gc_info_decode(rb_objspace_t *objspace, const VALUE hash_or_key, const unsigned int orig_flags)
{
    static VALUE sym_major_by = Qnil, sym_gc_by, sym_immediate_sweep, sym_have_finalizer, sym_state, sym_need_major_by;
    static VALUE sym_nofree, sym_oldgen, sym_shady, sym_force, sym_stress;
#if RGENGC_ESTIMATE_OLDMALLOC
    static VALUE sym_oldmalloc;
#endif
    static VALUE sym_newobj, sym_malloc, sym_method, sym_capi;
    static VALUE sym_none, sym_marking, sym_sweeping;
    static VALUE sym_weak_references_count, sym_retained_weak_references_count;
    VALUE hash = Qnil, key = Qnil;
    VALUE major_by, need_major_by;
    unsigned int flags = orig_flags ? orig_flags : objspace->profile.latest_gc_info;

    if (SYMBOL_P(hash_or_key)) {
        key = hash_or_key;
    }
    else if (RB_TYPE_P(hash_or_key, T_HASH)) {
        hash = hash_or_key;
    }
    else {
        rb_raise(rb_eTypeError, "non-hash or symbol given");
    }

    if (NIL_P(sym_major_by)) {
#define S(s) sym_##s = ID2SYM(rb_intern_const(#s))
        S(major_by);
        S(gc_by);
        S(immediate_sweep);
        S(have_finalizer);
        S(state);
        S(need_major_by);

        S(stress);
        S(nofree);
        S(oldgen);
        S(shady);
        S(force);
#if RGENGC_ESTIMATE_OLDMALLOC
        S(oldmalloc);
#endif
        S(newobj);
        S(malloc);
        S(method);
        S(capi);

        S(none);
        S(marking);
        S(sweeping);

        S(weak_references_count);
        S(retained_weak_references_count);
#undef S
    }

#define SET(name, attr) \
    if (key == sym_##name) \
        return (attr); \
    else if (hash != Qnil) \
        rb_hash_aset(hash, sym_##name, (attr));

    major_by =
      (flags & GPR_FLAG_MAJOR_BY_NOFREE) ? sym_nofree :
      (flags & GPR_FLAG_MAJOR_BY_OLDGEN) ? sym_oldgen :
      (flags & GPR_FLAG_MAJOR_BY_SHADY)  ? sym_shady :
      (flags & GPR_FLAG_MAJOR_BY_FORCE)  ? sym_force :
#if RGENGC_ESTIMATE_OLDMALLOC
      (flags & GPR_FLAG_MAJOR_BY_OLDMALLOC) ? sym_oldmalloc :
#endif
      Qnil;
    SET(major_by, major_by);

    if (orig_flags == 0) { /* set need_major_by only if flags not set explicitly */
        unsigned int need_major_flags = gc_needs_major_flags;
        need_major_by =
            (need_major_flags & GPR_FLAG_MAJOR_BY_NOFREE) ? sym_nofree :
            (need_major_flags & GPR_FLAG_MAJOR_BY_OLDGEN) ? sym_oldgen :
            (need_major_flags & GPR_FLAG_MAJOR_BY_SHADY)  ? sym_shady :
            (need_major_flags & GPR_FLAG_MAJOR_BY_FORCE)  ? sym_force :
#if RGENGC_ESTIMATE_OLDMALLOC
            (need_major_flags & GPR_FLAG_MAJOR_BY_OLDMALLOC) ? sym_oldmalloc :
#endif
            Qnil;
        SET(need_major_by, need_major_by);
    }

    SET(gc_by,
        (flags & GPR_FLAG_NEWOBJ) ? sym_newobj :
        (flags & GPR_FLAG_MALLOC) ? sym_malloc :
        (flags & GPR_FLAG_METHOD) ? sym_method :
        (flags & GPR_FLAG_CAPI)   ? sym_capi :
        (flags & GPR_FLAG_STRESS) ? sym_stress :
        Qnil
    );

    SET(have_finalizer, (flags & GPR_FLAG_HAVE_FINALIZE) ? Qtrue : Qfalse);
    SET(immediate_sweep, (flags & GPR_FLAG_IMMEDIATE_SWEEP) ? Qtrue : Qfalse);

    if (orig_flags == 0) {
        SET(state, gc_mode(objspace) == gc_mode_none ? sym_none :
                   gc_mode(objspace) == gc_mode_marking ? sym_marking : sym_sweeping);
    }

    SET(weak_references_count, LONG2FIX(objspace->profile.weak_references_count));
    SET(retained_weak_references_count, LONG2FIX(objspace->profile.retained_weak_references_count));
#undef SET

    if (!NIL_P(key)) {/* matched key should return above */
        rb_raise(rb_eArgError, "unknown key: %"PRIsVALUE, rb_sym2str(key));
    }

    return hash;
}

VALUE
rb_gc_impl_latest_gc_info(void *objspace_ptr, VALUE key)
{
    rb_objspace_t *objspace = objspace_ptr;

    return gc_info_decode(objspace, key, 0);
}


enum gc_stat_sym {
    gc_stat_sym_count,
    gc_stat_sym_time,
    gc_stat_sym_marking_time,
    gc_stat_sym_sweeping_time,
    gc_stat_sym_heap_allocated_pages,
    gc_stat_sym_heap_sorted_length,
    gc_stat_sym_heap_allocatable_pages,
    gc_stat_sym_heap_available_slots,
    gc_stat_sym_heap_live_slots,
    gc_stat_sym_heap_free_slots,
    gc_stat_sym_heap_final_slots,
    gc_stat_sym_heap_marked_slots,
    gc_stat_sym_heap_eden_pages,
    gc_stat_sym_heap_tomb_pages,
    gc_stat_sym_total_allocated_pages,
    gc_stat_sym_total_freed_pages,
    gc_stat_sym_total_allocated_objects,
    gc_stat_sym_total_freed_objects,
    gc_stat_sym_malloc_increase_bytes,
    gc_stat_sym_malloc_increase_bytes_limit,
    gc_stat_sym_minor_gc_count,
    gc_stat_sym_major_gc_count,
    gc_stat_sym_compact_count,
    gc_stat_sym_read_barrier_faults,
    gc_stat_sym_total_moved_objects,
    gc_stat_sym_remembered_wb_unprotected_objects,
    gc_stat_sym_remembered_wb_unprotected_objects_limit,
    gc_stat_sym_old_objects,
    gc_stat_sym_old_objects_limit,
#if RGENGC_ESTIMATE_OLDMALLOC
    gc_stat_sym_oldmalloc_increase_bytes,
    gc_stat_sym_oldmalloc_increase_bytes_limit,
#endif
    gc_stat_sym_weak_references_count,
#if RGENGC_PROFILE
    gc_stat_sym_total_generated_normal_object_count,
    gc_stat_sym_total_generated_shady_object_count,
    gc_stat_sym_total_shade_operation_count,
    gc_stat_sym_total_promoted_count,
    gc_stat_sym_total_remembered_normal_object_count,
    gc_stat_sym_total_remembered_shady_object_count,
#endif
    gc_stat_sym_last
};

static VALUE gc_stat_symbols[gc_stat_sym_last];

static void
setup_gc_stat_symbols(void)
{
    if (gc_stat_symbols[0] == 0) {
#define S(s) gc_stat_symbols[gc_stat_sym_##s] = ID2SYM(rb_intern_const(#s))
        S(count);
        S(time);
        S(marking_time),
        S(sweeping_time),
        S(heap_allocated_pages);
        S(heap_sorted_length);
        S(heap_allocatable_pages);
        S(heap_available_slots);
        S(heap_live_slots);
        S(heap_free_slots);
        S(heap_final_slots);
        S(heap_marked_slots);
        S(heap_eden_pages);
        S(heap_tomb_pages);
        S(total_allocated_pages);
        S(total_freed_pages);
        S(total_allocated_objects);
        S(total_freed_objects);
        S(malloc_increase_bytes);
        S(malloc_increase_bytes_limit);
        S(minor_gc_count);
        S(major_gc_count);
        S(compact_count);
        S(read_barrier_faults);
        S(total_moved_objects);
        S(remembered_wb_unprotected_objects);
        S(remembered_wb_unprotected_objects_limit);
        S(old_objects);
        S(old_objects_limit);
#if RGENGC_ESTIMATE_OLDMALLOC
        S(oldmalloc_increase_bytes);
        S(oldmalloc_increase_bytes_limit);
#endif
        S(weak_references_count);
#if RGENGC_PROFILE
        S(total_generated_normal_object_count);
        S(total_generated_shady_object_count);
        S(total_shade_operation_count);
        S(total_promoted_count);
        S(total_remembered_normal_object_count);
        S(total_remembered_shady_object_count);
#endif /* RGENGC_PROFILE */
#undef S
    }
}

static uint64_t
ns_to_ms(uint64_t ns)
{
    return ns / (1000 * 1000);
}

size_t
rb_gc_impl_stat(void *objspace_ptr, VALUE hash_or_sym)
{
    rb_objspace_t *objspace = objspace_ptr;
    VALUE hash = Qnil, key = Qnil;

    setup_gc_stat_symbols();

    if (RB_TYPE_P(hash_or_sym, T_HASH)) {
        hash = hash_or_sym;
    }
    else if (SYMBOL_P(hash_or_sym)) {
        key = hash_or_sym;
    }
    else {
        rb_raise(rb_eTypeError, "non-hash or symbol argument");
    }

#define SET(name, attr) \
    if (key == gc_stat_symbols[gc_stat_sym_##name]) \
        return attr; \
    else if (hash != Qnil) \
        rb_hash_aset(hash, gc_stat_symbols[gc_stat_sym_##name], SIZET2NUM(attr));

    SET(count, objspace->profile.count);
    SET(time, (size_t)ns_to_ms(objspace->profile.marking_time_ns + objspace->profile.sweeping_time_ns)); // TODO: UINT64T2NUM
    SET(marking_time, (size_t)ns_to_ms(objspace->profile.marking_time_ns));
    SET(sweeping_time, (size_t)ns_to_ms(objspace->profile.sweeping_time_ns));

    /* implementation dependent counters */
    SET(heap_allocated_pages, heap_allocated_pages);
    SET(heap_sorted_length, heap_pages_sorted_length);
    SET(heap_allocatable_pages, heap_allocatable_pages(objspace));
    SET(heap_available_slots, objspace_available_slots(objspace));
    SET(heap_live_slots, objspace_live_slots(objspace));
    SET(heap_free_slots, objspace_free_slots(objspace));
    SET(heap_final_slots, heap_pages_final_slots);
    SET(heap_marked_slots, objspace->marked_slots);
    SET(heap_eden_pages, heap_eden_total_pages(objspace));
    SET(heap_tomb_pages, heap_tomb_total_pages(objspace));
    SET(total_allocated_pages, total_allocated_pages(objspace));
    SET(total_freed_pages, total_freed_pages(objspace));
    SET(total_allocated_objects, total_allocated_objects(objspace));
    SET(total_freed_objects, total_freed_objects(objspace));
    SET(malloc_increase_bytes, malloc_increase);
    SET(malloc_increase_bytes_limit, malloc_limit);
    SET(minor_gc_count, objspace->profile.minor_gc_count);
    SET(major_gc_count, objspace->profile.major_gc_count);
    SET(compact_count, objspace->profile.compact_count);
    SET(read_barrier_faults, objspace->profile.read_barrier_faults);
    SET(total_moved_objects, objspace->rcompactor.total_moved);
    SET(remembered_wb_unprotected_objects, objspace->rgengc.uncollectible_wb_unprotected_objects);
    SET(remembered_wb_unprotected_objects_limit, objspace->rgengc.uncollectible_wb_unprotected_objects_limit);
    SET(old_objects, objspace->rgengc.old_objects);
    SET(old_objects_limit, objspace->rgengc.old_objects_limit);
#if RGENGC_ESTIMATE_OLDMALLOC
    SET(oldmalloc_increase_bytes, objspace->rgengc.oldmalloc_increase);
    SET(oldmalloc_increase_bytes_limit, objspace->rgengc.oldmalloc_increase_limit);
#endif

#if RGENGC_PROFILE
    SET(total_generated_normal_object_count, objspace->profile.total_generated_normal_object_count);
    SET(total_generated_shady_object_count, objspace->profile.total_generated_shady_object_count);
    SET(total_shade_operation_count, objspace->profile.total_shade_operation_count);
    SET(total_promoted_count, objspace->profile.total_promoted_count);
    SET(total_remembered_normal_object_count, objspace->profile.total_remembered_normal_object_count);
    SET(total_remembered_shady_object_count, objspace->profile.total_remembered_shady_object_count);
#endif /* RGENGC_PROFILE */
#undef SET

    if (!NIL_P(key)) { /* matched key should return above */
        rb_raise(rb_eArgError, "unknown key: %"PRIsVALUE, rb_sym2str(key));
    }

#if defined(RGENGC_PROFILE) && RGENGC_PROFILE >= 2
    if (hash != Qnil) {
        gc_count_add_each_types(hash, "generated_normal_object_count_types", objspace->profile.generated_normal_object_count_types);
        gc_count_add_each_types(hash, "generated_shady_object_count_types", objspace->profile.generated_shady_object_count_types);
        gc_count_add_each_types(hash, "shade_operation_count_types", objspace->profile.shade_operation_count_types);
        gc_count_add_each_types(hash, "promoted_types", objspace->profile.promoted_types);
        gc_count_add_each_types(hash, "remembered_normal_object_count_types", objspace->profile.remembered_normal_object_count_types);
        gc_count_add_each_types(hash, "remembered_shady_object_count_types", objspace->profile.remembered_shady_object_count_types);
    }
#endif

    return 0;
}

enum gc_stat_heap_sym {
    gc_stat_heap_sym_slot_size,
    gc_stat_heap_sym_heap_allocatable_pages,
    gc_stat_heap_sym_heap_eden_pages,
    gc_stat_heap_sym_heap_eden_slots,
    gc_stat_heap_sym_heap_tomb_pages,
    gc_stat_heap_sym_heap_tomb_slots,
    gc_stat_heap_sym_total_allocated_pages,
    gc_stat_heap_sym_total_freed_pages,
    gc_stat_heap_sym_force_major_gc_count,
    gc_stat_heap_sym_force_incremental_marking_finish_count,
    gc_stat_heap_sym_total_allocated_objects,
    gc_stat_heap_sym_total_freed_objects,
    gc_stat_heap_sym_last
};

static VALUE gc_stat_heap_symbols[gc_stat_heap_sym_last];

int
rb_gc_impl_heap_count(void *objspace_ptr)
{
    return SIZE_POOL_COUNT;
}

static void
setup_gc_stat_heap_symbols(void)
{
    if (gc_stat_heap_symbols[0] == 0) {
#define S(s) gc_stat_heap_symbols[gc_stat_heap_sym_##s] = ID2SYM(rb_intern_const(#s))
        S(slot_size);
        S(heap_allocatable_pages);
        S(heap_eden_pages);
        S(heap_eden_slots);
        S(heap_tomb_pages);
        S(heap_tomb_slots);
        S(total_allocated_pages);
        S(total_freed_pages);
        S(force_major_gc_count);
        S(force_incremental_marking_finish_count);
        S(total_allocated_objects);
        S(total_freed_objects);
#undef S
    }
}

size_t
rb_gc_impl_stat_heap(void *objspace_ptr, int size_pool_idx, VALUE hash_or_sym)
{
    rb_objspace_t *objspace = objspace_ptr;
    VALUE hash = Qnil, key = Qnil;

    setup_gc_stat_heap_symbols();

    if (RB_TYPE_P(hash_or_sym, T_HASH)) {
        hash = hash_or_sym;
    }
    else if (SYMBOL_P(hash_or_sym)) {
        key = hash_or_sym;
    }
    else {
        rb_raise(rb_eTypeError, "non-hash or symbol argument");
    }

    if (size_pool_idx < 0 || size_pool_idx >= SIZE_POOL_COUNT) {
        rb_raise(rb_eArgError, "size pool index out of range");
    }

    rb_size_pool_t *size_pool = &size_pools[size_pool_idx];

#define SET(name, attr) \
    if (key == gc_stat_heap_symbols[gc_stat_heap_sym_##name]) \
        return attr; \
    else if (hash != Qnil) \
        rb_hash_aset(hash, gc_stat_heap_symbols[gc_stat_heap_sym_##name], SIZET2NUM(attr));

    SET(slot_size, size_pool->slot_size);
    SET(heap_allocatable_pages, size_pool->allocatable_pages);
    SET(heap_eden_pages, SIZE_POOL_EDEN_HEAP(size_pool)->total_pages);
    SET(heap_eden_slots, SIZE_POOL_EDEN_HEAP(size_pool)->total_slots);
    SET(heap_tomb_pages, SIZE_POOL_TOMB_HEAP(size_pool)->total_pages);
    SET(heap_tomb_slots, SIZE_POOL_TOMB_HEAP(size_pool)->total_slots);
    SET(total_allocated_pages, size_pool->total_allocated_pages);
    SET(total_freed_pages, size_pool->total_freed_pages);
    SET(force_major_gc_count, size_pool->force_major_gc_count);
    SET(force_incremental_marking_finish_count, size_pool->force_incremental_marking_finish_count);
    SET(total_allocated_objects, size_pool->total_allocated_objects);
    SET(total_freed_objects, size_pool->total_freed_objects);
#undef SET

    if (!NIL_P(key)) { /* matched key should return above */
        rb_raise(rb_eArgError, "unknown key: %"PRIsVALUE, rb_sym2str(key));
    }

    return 0;
}

VALUE
rb_gc_impl_stress_get(void *objspace_ptr)
{
    rb_objspace_t *objspace = objspace_ptr;
    return ruby_gc_stress_mode;
}

void
rb_gc_impl_stress_set(void *objspace_ptr, VALUE flag)
{
    rb_objspace_t *objspace = objspace_ptr;

    objspace->flags.gc_stressful = RTEST(flag);
    objspace->gc_stress_mode = flag;
}

static int
get_envparam_size(const char *name, size_t *default_value, size_t lower_bound)
{
    const char *ptr = getenv(name);
    ssize_t val;

    if (ptr != NULL && *ptr) {
        size_t unit = 0;
        char *end;
#if SIZEOF_SIZE_T == SIZEOF_LONG_LONG
        val = strtoll(ptr, &end, 0);
#else
        val = strtol(ptr, &end, 0);
#endif
        switch (*end) {
          case 'k': case 'K':
            unit = 1024;
            ++end;
            break;
          case 'm': case 'M':
            unit = 1024*1024;
            ++end;
            break;
          case 'g': case 'G':
            unit = 1024*1024*1024;
            ++end;
            break;
        }
        while (*end && isspace((unsigned char)*end)) end++;
        if (*end) {
            if (RTEST(ruby_verbose)) fprintf(stderr, "invalid string for %s: %s\n", name, ptr);
            return 0;
        }
        if (unit > 0) {
            if (val < -(ssize_t)(SIZE_MAX / 2 / unit) || (ssize_t)(SIZE_MAX / 2 / unit) < val) {
                if (RTEST(ruby_verbose)) fprintf(stderr, "%s=%s is ignored because it overflows\n", name, ptr);
                return 0;
            }
            val *= unit;
        }
        if (val > 0 && (size_t)val > lower_bound) {
            if (RTEST(ruby_verbose)) {
                fprintf(stderr, "%s=%"PRIdSIZE" (default value: %"PRIuSIZE")\n", name, val, *default_value);
            }
            *default_value = (size_t)val;
            return 1;
        }
        else {
            if (RTEST(ruby_verbose)) {
                fprintf(stderr, "%s=%"PRIdSIZE" (default value: %"PRIuSIZE") is ignored because it must be greater than %"PRIuSIZE".\n",
                        name, val, *default_value, lower_bound);
            }
            return 0;
        }
    }
    return 0;
}

static int
get_envparam_double(const char *name, double *default_value, double lower_bound, double upper_bound, int accept_zero)
{
    const char *ptr = getenv(name);
    double val;

    if (ptr != NULL && *ptr) {
        char *end;
        val = strtod(ptr, &end);
        if (!*ptr || *end) {
            if (RTEST(ruby_verbose)) fprintf(stderr, "invalid string for %s: %s\n", name, ptr);
            return 0;
        }

        if (accept_zero && val == 0.0) {
            goto accept;
        }
        else if (val <= lower_bound) {
            if (RTEST(ruby_verbose)) {
                fprintf(stderr, "%s=%f (default value: %f) is ignored because it must be greater than %f.\n",
                        name, val, *default_value, lower_bound);
            }
        }
        else if (upper_bound != 0.0 && /* ignore upper_bound if it is 0.0 */
                 val > upper_bound) {
            if (RTEST(ruby_verbose)) {
                fprintf(stderr, "%s=%f (default value: %f) is ignored because it must be lower than %f.\n",
                        name, val, *default_value, upper_bound);
            }
        }
        else {
            goto accept;
        }
    }
    return 0;

  accept:
    if (RTEST(ruby_verbose)) fprintf(stderr, "%s=%f (default value: %f)\n", name, val, *default_value);
    *default_value = val;
    return 1;
}

static void
gc_set_initial_pages(rb_objspace_t *objspace)
{
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        rb_size_pool_t *size_pool = &size_pools[i];
        char env_key[sizeof("RUBY_GC_HEAP_" "_INIT_SLOTS") + DECIMAL_SIZE_OF_BITS(sizeof(int) * CHAR_BIT)];
        snprintf(env_key, sizeof(env_key), "RUBY_GC_HEAP_%d_INIT_SLOTS", i);

        size_t size_pool_init_slots = gc_params.size_pool_init_slots[i];
        if (get_envparam_size(env_key, &size_pool_init_slots, 0)) {
            gc_params.size_pool_init_slots[i] = size_pool_init_slots;
        }

        if (size_pool_init_slots > size_pool->eden_heap.total_slots) {
            size_t slots = size_pool_init_slots - size_pool->eden_heap.total_slots;
            size_pool->allocatable_pages = slots_to_pages_for_size_pool(objspace, size_pool, slots);
        }
        else {
            /* We already have more slots than size_pool_init_slots allows, so
             * prevent creating more pages. */
            size_pool->allocatable_pages = 0;
        }
    }
    heap_pages_expand_sorted(objspace);
}

/*
 * GC tuning environment variables
 *
 * * RUBY_GC_HEAP_FREE_SLOTS
 *   - Prepare at least this amount of slots after GC.
 *   - Allocate slots if there are not enough slots.
 * * RUBY_GC_HEAP_GROWTH_FACTOR (new from 2.1)
 *   - Allocate slots by this factor.
 *   - (next slots number) = (current slots number) * (this factor)
 * * RUBY_GC_HEAP_GROWTH_MAX_SLOTS (new from 2.1)
 *   - Allocation rate is limited to this number of slots.
 * * RUBY_GC_HEAP_FREE_SLOTS_MIN_RATIO (new from 2.4)
 *   - Allocate additional pages when the number of free slots is
 *     lower than the value (total_slots * (this ratio)).
 * * RUBY_GC_HEAP_FREE_SLOTS_GOAL_RATIO (new from 2.4)
 *   - Allocate slots to satisfy this formula:
 *       free_slots = total_slots * goal_ratio
 *   - In other words, prepare (total_slots * goal_ratio) free slots.
 *   - if this value is 0.0, then use RUBY_GC_HEAP_GROWTH_FACTOR directly.
 * * RUBY_GC_HEAP_FREE_SLOTS_MAX_RATIO (new from 2.4)
 *   - Allow to free pages when the number of free slots is
 *     greater than the value (total_slots * (this ratio)).
 * * RUBY_GC_HEAP_OLDOBJECT_LIMIT_FACTOR (new from 2.1.1)
 *   - Do full GC when the number of old objects is more than R * N
 *     where R is this factor and
 *           N is the number of old objects just after last full GC.
 *
 *  * obsolete
 *    * RUBY_FREE_MIN       -> RUBY_GC_HEAP_FREE_SLOTS (from 2.1)
 *    * RUBY_HEAP_MIN_SLOTS -> RUBY_GC_HEAP_INIT_SLOTS (from 2.1)
 *
 * * RUBY_GC_MALLOC_LIMIT
 * * RUBY_GC_MALLOC_LIMIT_MAX (new from 2.1)
 * * RUBY_GC_MALLOC_LIMIT_GROWTH_FACTOR (new from 2.1)
 *
 * * RUBY_GC_OLDMALLOC_LIMIT (new from 2.1)
 * * RUBY_GC_OLDMALLOC_LIMIT_MAX (new from 2.1)
 * * RUBY_GC_OLDMALLOC_LIMIT_GROWTH_FACTOR (new from 2.1)
 */

void
rb_gc_impl_set_params(void *objspace_ptr)
{
    rb_objspace_t *objspace = objspace_ptr;
    /* RUBY_GC_HEAP_FREE_SLOTS */
    if (get_envparam_size("RUBY_GC_HEAP_FREE_SLOTS", &gc_params.heap_free_slots, 0)) {
        /* ok */
    }

    gc_set_initial_pages(objspace);

    get_envparam_double("RUBY_GC_HEAP_GROWTH_FACTOR", &gc_params.growth_factor, 1.0, 0.0, FALSE);
    get_envparam_size  ("RUBY_GC_HEAP_GROWTH_MAX_SLOTS", &gc_params.growth_max_slots, 0);
    get_envparam_double("RUBY_GC_HEAP_FREE_SLOTS_MIN_RATIO", &gc_params.heap_free_slots_min_ratio,
                        0.0, 1.0, FALSE);
    get_envparam_double("RUBY_GC_HEAP_FREE_SLOTS_MAX_RATIO", &gc_params.heap_free_slots_max_ratio,
                        gc_params.heap_free_slots_min_ratio, 1.0, FALSE);
    get_envparam_double("RUBY_GC_HEAP_FREE_SLOTS_GOAL_RATIO", &gc_params.heap_free_slots_goal_ratio,
                        gc_params.heap_free_slots_min_ratio, gc_params.heap_free_slots_max_ratio, TRUE);
    get_envparam_double("RUBY_GC_HEAP_OLDOBJECT_LIMIT_FACTOR", &gc_params.oldobject_limit_factor, 0.0, 0.0, TRUE);
    get_envparam_double("RUBY_GC_HEAP_REMEMBERED_WB_UNPROTECTED_OBJECTS_LIMIT_RATIO", &gc_params.uncollectible_wb_unprotected_objects_limit_ratio, 0.0, 0.0, TRUE);

    if (get_envparam_size("RUBY_GC_MALLOC_LIMIT", &gc_params.malloc_limit_min, 0)) {
        malloc_limit = gc_params.malloc_limit_min;
    }
    get_envparam_size  ("RUBY_GC_MALLOC_LIMIT_MAX", &gc_params.malloc_limit_max, 0);
    if (!gc_params.malloc_limit_max) { /* ignore max-check if 0 */
        gc_params.malloc_limit_max = SIZE_MAX;
    }
    get_envparam_double("RUBY_GC_MALLOC_LIMIT_GROWTH_FACTOR", &gc_params.malloc_limit_growth_factor, 1.0, 0.0, FALSE);

#if RGENGC_ESTIMATE_OLDMALLOC
    if (get_envparam_size("RUBY_GC_OLDMALLOC_LIMIT", &gc_params.oldmalloc_limit_min, 0)) {
        objspace->rgengc.oldmalloc_increase_limit = gc_params.oldmalloc_limit_min;
    }
    get_envparam_size  ("RUBY_GC_OLDMALLOC_LIMIT_MAX", &gc_params.oldmalloc_limit_max, 0);
    get_envparam_double("RUBY_GC_OLDMALLOC_LIMIT_GROWTH_FACTOR", &gc_params.oldmalloc_limit_growth_factor, 1.0, 0.0, FALSE);
#endif
}

static inline size_t
objspace_malloc_size(rb_objspace_t *objspace, void *ptr, size_t hint)
{
#ifdef HAVE_MALLOC_USABLE_SIZE
    return malloc_usable_size(ptr);
#else
    return hint;
#endif
}

enum memop_type {
    MEMOP_TYPE_MALLOC  = 0,
    MEMOP_TYPE_FREE,
    MEMOP_TYPE_REALLOC
};

static inline void
atomic_sub_nounderflow(size_t *var, size_t sub)
{
    if (sub == 0) return;

    while (1) {
        size_t val = *var;
        if (val < sub) sub = val;
        if (RUBY_ATOMIC_SIZE_CAS(*var, val, val-sub) == val) break;
    }
}

static inline bool
objspace_malloc_increase_report(rb_objspace_t *objspace, void *mem, size_t new_size, size_t old_size, enum memop_type type)
{
    if (0) fprintf(stderr, "increase - ptr: %p, type: %s, new_size: %"PRIdSIZE", old_size: %"PRIdSIZE"\n",
                   mem,
                   type == MEMOP_TYPE_MALLOC  ? "malloc" :
                   type == MEMOP_TYPE_FREE    ? "free  " :
                   type == MEMOP_TYPE_REALLOC ? "realloc": "error",
                   new_size, old_size);
    return false;
}

static bool
objspace_malloc_increase_body(rb_objspace_t *objspace, void *mem, size_t new_size, size_t old_size, enum memop_type type)
{
    if (new_size > old_size) {
        RUBY_ATOMIC_SIZE_ADD(malloc_increase, new_size - old_size);
#if RGENGC_ESTIMATE_OLDMALLOC
        RUBY_ATOMIC_SIZE_ADD(objspace->rgengc.oldmalloc_increase, new_size - old_size);
#endif
    }
    else {
        atomic_sub_nounderflow(&malloc_increase, old_size - new_size);
#if RGENGC_ESTIMATE_OLDMALLOC
        atomic_sub_nounderflow(&objspace->rgengc.oldmalloc_increase, old_size - new_size);
#endif
    }

#if MALLOC_ALLOCATED_SIZE
    if (new_size >= old_size) {
        RUBY_ATOMIC_SIZE_ADD(objspace->malloc_params.allocated_size, new_size - old_size);
    }
    else {
        size_t dec_size = old_size - new_size;
        size_t allocated_size = objspace->malloc_params.allocated_size;

#if MALLOC_ALLOCATED_SIZE_CHECK
        if (allocated_size < dec_size) {
            rb_bug("objspace_malloc_increase: underflow malloc_params.allocated_size.");
        }
#endif
        atomic_sub_nounderflow(&objspace->malloc_params.allocated_size, dec_size);
    }

    switch (type) {
      case MEMOP_TYPE_MALLOC:
        RUBY_ATOMIC_SIZE_INC(objspace->malloc_params.allocations);
        break;
      case MEMOP_TYPE_FREE:
        {
            size_t allocations = objspace->malloc_params.allocations;
            if (allocations > 0) {
                atomic_sub_nounderflow(&objspace->malloc_params.allocations, 1);
            }
#if MALLOC_ALLOCATED_SIZE_CHECK
            else {
                GC_ASSERT(objspace->malloc_params.allocations > 0);
            }
#endif
        }
        break;
      case MEMOP_TYPE_REALLOC: /* ignore */ break;
    }
#endif
    return true;
}

#define objspace_malloc_increase(...) \
    for (bool malloc_increase_done = objspace_malloc_increase_report(__VA_ARGS__); \
         !malloc_increase_done; \
         malloc_increase_done = objspace_malloc_increase_body(__VA_ARGS__))

struct malloc_obj_info { /* 4 words */
    size_t size;
#if USE_GC_MALLOC_OBJ_INFO_DETAILS
    size_t gen;
    const char *file;
    size_t line;
#endif
};

#if USE_GC_MALLOC_OBJ_INFO_DETAILS
const char *ruby_malloc_info_file;
int ruby_malloc_info_line;
#endif

static inline size_t
objspace_malloc_prepare(rb_objspace_t *objspace, size_t size)
{
    if (size == 0) size = 1;

#if CALC_EXACT_MALLOC_SIZE
    size += sizeof(struct malloc_obj_info);
#endif

    return size;
}

static inline void *
objspace_malloc_fixup(rb_objspace_t *objspace, void *mem, size_t size)
{
    size = objspace_malloc_size(objspace, mem, size);
    objspace_malloc_increase(objspace, mem, size, 0, MEMOP_TYPE_MALLOC) {}

#if CALC_EXACT_MALLOC_SIZE
    {
        struct malloc_obj_info *info = mem;
        info->size = size;
#if USE_GC_MALLOC_OBJ_INFO_DETAILS
        info->gen = objspace->profile.count;
        info->file = ruby_malloc_info_file;
        info->line = info->file ? ruby_malloc_info_line : 0;
#endif
        mem = info + 1;
    }
#endif

    return mem;
}

#if defined(__GNUC__) && RUBY_DEBUG
#define RB_BUG_INSTEAD_OF_RB_MEMERROR 1
#endif

#ifndef RB_BUG_INSTEAD_OF_RB_MEMERROR
# define RB_BUG_INSTEAD_OF_RB_MEMERROR 0
#endif

#define GC_MEMERROR(...) \
    ((RB_BUG_INSTEAD_OF_RB_MEMERROR+0) ? rb_bug("" __VA_ARGS__) : rb_memerror())

void
rb_gc_impl_free(void *objspace_ptr, void *ptr, size_t old_size)
{
    rb_objspace_t *objspace = objspace_ptr;

    if (!ptr) {
        /*
         * ISO/IEC 9899 says "If ptr is a null pointer, no action occurs" since
         * its first version.  We would better follow.
         */
        return;
    }
#if CALC_EXACT_MALLOC_SIZE
    struct malloc_obj_info *info = (struct malloc_obj_info *)ptr - 1;
    ptr = info;
    old_size = info->size;

#if USE_GC_MALLOC_OBJ_INFO_DETAILS
    {
        int gen = (int)(objspace->profile.count - info->gen);
        int gen_index = gen >= MALLOC_INFO_GEN_SIZE ? MALLOC_INFO_GEN_SIZE-1 : gen;
        int i;

        malloc_info_gen_cnt[gen_index]++;
        malloc_info_gen_size[gen_index] += info->size;

        for (i=0; i<MALLOC_INFO_SIZE_SIZE; i++) {
            size_t s = 16 << i;
            if (info->size <= s) {
                malloc_info_size[i]++;
                goto found;
            }
        }
        malloc_info_size[i]++;
      found:;

        {
            st_data_t key = (st_data_t)info->file, d;
            size_t *data;

            if (malloc_info_file_table == NULL) {
                malloc_info_file_table = st_init_numtable_with_size(1024);
            }
            if (st_lookup(malloc_info_file_table, key, &d)) {
                /* hit */
                data = (size_t *)d;
            }
            else {
                data = malloc(xmalloc2_size(2, sizeof(size_t)));
                if (data == NULL) rb_bug("objspace_xfree: can not allocate memory");
                data[0] = data[1] = 0;
                st_insert(malloc_info_file_table, key, (st_data_t)data);
            }
            data[0] ++;
            data[1] += info->size;
        };
        if (0 && gen >= 2) {         /* verbose output */
            if (info->file) {
                fprintf(stderr, "free - size:%"PRIdSIZE", gen:%d, pos: %s:%"PRIdSIZE"\n",
                        info->size, gen, info->file, info->line);
            }
            else {
                fprintf(stderr, "free - size:%"PRIdSIZE", gen:%d\n",
                        info->size, gen);
            }
        }
    }
#endif
#endif
    old_size = objspace_malloc_size(objspace, ptr, old_size);

    objspace_malloc_increase(objspace, ptr, 0, old_size, MEMOP_TYPE_FREE) {
        free(ptr);
        ptr = NULL;
        RB_DEBUG_COUNTER_INC(heap_xfree);
    }
}

void *
rb_gc_impl_malloc(void *objspace_ptr, size_t size)
{
    rb_objspace_t *objspace = objspace_ptr;
    void *mem;

    size = objspace_malloc_prepare(objspace, size);
    mem = malloc(size);
    RB_DEBUG_COUNTER_INC(heap_xmalloc);
    return objspace_malloc_fixup(objspace, mem, size);
}

void *
rb_gc_impl_calloc(void *objspace_ptr, size_t size)
{
    rb_objspace_t *objspace = objspace_ptr;
    void *mem;

    size = objspace_malloc_prepare(objspace, size);
    mem = calloc1(size);
    return objspace_malloc_fixup(objspace, mem, size);
}

void *
rb_gc_impl_realloc(void *objspace_ptr, void *ptr, size_t new_size, size_t old_size)
{
    rb_objspace_t *objspace = objspace_ptr;
    void *mem;

    if (!ptr) return rb_gc_impl_malloc(objspace, new_size);

    /*
     * The behavior of realloc(ptr, 0) is implementation defined.
     * Therefore we don't use realloc(ptr, 0) for portability reason.
     * see http://www.open-std.org/jtc1/sc22/wg14/www/docs/dr_400.htm
     */
    if (new_size == 0) {
        if ((mem = rb_gc_impl_malloc(objspace, 0)) != NULL) {
            /*
             * - OpenBSD's malloc(3) man page says that when 0 is passed, it
             *   returns a non-NULL pointer to an access-protected memory page.
             *   The returned pointer cannot be read / written at all, but
             *   still be a valid argument of free().
             *
             *   https://man.openbsd.org/malloc.3
             *
             * - Linux's malloc(3) man page says that it _might_ perhaps return
             *   a non-NULL pointer when its argument is 0.  That return value
             *   is safe (and is expected) to be passed to free().
             *
             *   https://man7.org/linux/man-pages/man3/malloc.3.html
             *
             * - As I read the implementation jemalloc's malloc() returns fully
             *   normal 16 bytes memory region when its argument is 0.
             *
             * - As I read the implementation musl libc's malloc() returns
             *   fully normal 32 bytes memory region when its argument is 0.
             *
             * - Other malloc implementations can also return non-NULL.
             */
            rb_gc_impl_free(objspace, ptr, old_size);
            return mem;
        }
        else {
            /*
             * It is dangerous to return NULL here, because that could lead to
             * RCE.  Fallback to 1 byte instead of zero.
             *
             * https://cve.mitre.org/cgi-bin/cvename.cgi?name=CVE-2019-11932
             */
            new_size = 1;
        }
    }

#if CALC_EXACT_MALLOC_SIZE
    {
        struct malloc_obj_info *info = (struct malloc_obj_info *)ptr - 1;
        new_size += sizeof(struct malloc_obj_info);
        ptr = info;
        old_size = info->size;
    }
#endif

    old_size = objspace_malloc_size(objspace, ptr, old_size);
    mem = RB_GNUC_EXTENSION_BLOCK(realloc(ptr, new_size));
    new_size = objspace_malloc_size(objspace, mem, new_size);

#if CALC_EXACT_MALLOC_SIZE
    {
        struct malloc_obj_info *info = mem;
        info->size = new_size;
        mem = info + 1;
    }
#endif

    objspace_malloc_increase(objspace, mem, new_size, old_size, MEMOP_TYPE_REALLOC);

    RB_DEBUG_COUNTER_INC(heap_xrealloc);
    return mem;
}

void
rb_gc_impl_adjust_memory_usage(void *objspace_ptr, ssize_t diff)
{
    rb_objspace_t *objspace = objspace_ptr;

    if (diff > 0) {
        objspace_malloc_increase(objspace, 0, diff, 0, MEMOP_TYPE_REALLOC);
    }
    else if (diff < 0) {
        objspace_malloc_increase(objspace, 0, 0, -diff, MEMOP_TYPE_REALLOC);
    }
}

// TODO: move GC profiler stuff back into gc.c
/*
  ------------------------------ GC profiler ------------------------------
*/

#define GC_PROFILE_RECORD_DEFAULT_SIZE 100

static bool
current_process_time(struct timespec *ts)
{
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_PROCESS_CPUTIME_ID)
    {
        static int try_clock_gettime = 1;
        if (try_clock_gettime && clock_gettime(CLOCK_PROCESS_CPUTIME_ID, ts) == 0) {
            return true;
        }
        else {
            try_clock_gettime = 0;
        }
    }
#endif

#ifdef RUSAGE_SELF
    {
        struct rusage usage;
        struct timeval time;
        if (getrusage(RUSAGE_SELF, &usage) == 0) {
            time = usage.ru_utime;
            ts->tv_sec = time.tv_sec;
            ts->tv_nsec = (int32_t)time.tv_usec * 1000;
            return true;
        }
    }
#endif

#ifdef _WIN32
    {
        FILETIME creation_time, exit_time, kernel_time, user_time;
        ULARGE_INTEGER ui;

        if (GetProcessTimes(GetCurrentProcess(),
                            &creation_time, &exit_time, &kernel_time, &user_time) != 0) {
            memcpy(&ui, &user_time, sizeof(FILETIME));
#define PER100NSEC (uint64_t)(1000 * 1000 * 10)
            ts->tv_nsec = (long)(ui.QuadPart % PER100NSEC);
            ts->tv_sec  = (time_t)(ui.QuadPart / PER100NSEC);
            return true;
        }
    }
#endif

    return false;
}

static double
getrusage_time(void)
{
    struct timespec ts;
    if (current_process_time(&ts)) {
        return ts.tv_sec + ts.tv_nsec * 1e-9;
    }
    else {
        return 0.0;
    }
}

/*
 *  call-seq:
 *    GC::Profiler.clear          -> nil
 *
 *  Clears the \GC profiler data.
 *
 */

static VALUE
gc_profile_clear(VALUE _)
{
    rb_objspace_t *objspace = rb_gc_get_objspace();
    void *p = objspace->profile.records;
    objspace->profile.records = NULL;
    objspace->profile.size = 0;
    objspace->profile.next_index = 0;
    objspace->profile.current_record = 0;
    free(p);
    return Qnil;
}

/*
 *  call-seq:
 *     GC::Profiler.raw_data	-> [Hash, ...]
 *
 *  Returns an Array of individual raw profile data Hashes ordered
 *  from earliest to latest by +:GC_INVOKE_TIME+.
 *
 *  For example:
 *
 *    [
 *	{
 *	   :GC_TIME=>1.3000000000000858e-05,
 *	   :GC_INVOKE_TIME=>0.010634999999999999,
 *	   :HEAP_USE_SIZE=>289640,
 *	   :HEAP_TOTAL_SIZE=>588960,
 *	   :HEAP_TOTAL_OBJECTS=>14724,
 *	   :GC_IS_MARKED=>false
 *	},
 *      # ...
 *    ]
 *
 *  The keys mean:
 *
 *  +:GC_TIME+::
 *	Time elapsed in seconds for this GC run
 *  +:GC_INVOKE_TIME+::
 *	Time elapsed in seconds from startup to when the GC was invoked
 *  +:HEAP_USE_SIZE+::
 *	Total bytes of heap used
 *  +:HEAP_TOTAL_SIZE+::
 *	Total size of heap in bytes
 *  +:HEAP_TOTAL_OBJECTS+::
 *	Total number of objects
 *  +:GC_IS_MARKED+::
 *	Returns +true+ if the GC is in mark phase
 *
 *  If ruby was built with +GC_PROFILE_MORE_DETAIL+, you will also have access
 *  to the following hash keys:
 *
 *  +:GC_MARK_TIME+::
 *  +:GC_SWEEP_TIME+::
 *  +:ALLOCATE_INCREASE+::
 *  +:ALLOCATE_LIMIT+::
 *  +:HEAP_USE_PAGES+::
 *  +:HEAP_LIVE_OBJECTS+::
 *  +:HEAP_FREE_OBJECTS+::
 *  +:HAVE_FINALIZE+::
 *
 */

static VALUE
gc_profile_record_get(VALUE _)
{
    VALUE prof;
    VALUE gc_profile = rb_ary_new();
    size_t i;
    rb_objspace_t *objspace = rb_gc_get_objspace();

    if (!objspace->profile.run) {
        return Qnil;
    }

    for (i =0; i < objspace->profile.next_index; i++) {
        gc_profile_record *record = &objspace->profile.records[i];

        prof = rb_hash_new();
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_FLAGS")), gc_info_decode(objspace, rb_hash_new(), record->flags));
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_TIME")), DBL2NUM(record->gc_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_INVOKE_TIME")), DBL2NUM(record->gc_invoke_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_USE_SIZE")), SIZET2NUM(record->heap_use_size));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_TOTAL_SIZE")), SIZET2NUM(record->heap_total_size));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_TOTAL_OBJECTS")), SIZET2NUM(record->heap_total_objects));
        rb_hash_aset(prof, ID2SYM(rb_intern("MOVED_OBJECTS")), SIZET2NUM(record->moved_objects));
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_IS_MARKED")), Qtrue);
#if GC_PROFILE_MORE_DETAIL
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_MARK_TIME")), DBL2NUM(record->gc_mark_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("GC_SWEEP_TIME")), DBL2NUM(record->gc_sweep_time));
        rb_hash_aset(prof, ID2SYM(rb_intern("ALLOCATE_INCREASE")), SIZET2NUM(record->allocate_increase));
        rb_hash_aset(prof, ID2SYM(rb_intern("ALLOCATE_LIMIT")), SIZET2NUM(record->allocate_limit));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_USE_PAGES")), SIZET2NUM(record->heap_use_pages));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_LIVE_OBJECTS")), SIZET2NUM(record->heap_live_objects));
        rb_hash_aset(prof, ID2SYM(rb_intern("HEAP_FREE_OBJECTS")), SIZET2NUM(record->heap_free_objects));

        rb_hash_aset(prof, ID2SYM(rb_intern("REMOVING_OBJECTS")), SIZET2NUM(record->removing_objects));
        rb_hash_aset(prof, ID2SYM(rb_intern("EMPTY_OBJECTS")), SIZET2NUM(record->empty_objects));

        rb_hash_aset(prof, ID2SYM(rb_intern("HAVE_FINALIZE")), (record->flags & GPR_FLAG_HAVE_FINALIZE) ? Qtrue : Qfalse);
#endif

#if RGENGC_PROFILE > 0
        rb_hash_aset(prof, ID2SYM(rb_intern("OLD_OBJECTS")), SIZET2NUM(record->old_objects));
        rb_hash_aset(prof, ID2SYM(rb_intern("REMEMBERED_NORMAL_OBJECTS")), SIZET2NUM(record->remembered_normal_objects));
        rb_hash_aset(prof, ID2SYM(rb_intern("REMEMBERED_SHADY_OBJECTS")), SIZET2NUM(record->remembered_shady_objects));
#endif
        rb_ary_push(gc_profile, prof);
    }

    return gc_profile;
}

#if GC_PROFILE_MORE_DETAIL
#define MAJOR_REASON_MAX 0x10

static char *
gc_profile_dump_major_reason(unsigned int flags, char *buff)
{
    unsigned int reason = flags & GPR_FLAG_MAJOR_MASK;
    int i = 0;

    if (reason == GPR_FLAG_NONE) {
        buff[0] = '-';
        buff[1] = 0;
    }
    else {
#define C(x, s) \
  if (reason & GPR_FLAG_MAJOR_BY_##x) { \
      buff[i++] = #x[0]; \
      if (i >= MAJOR_REASON_MAX) rb_bug("gc_profile_dump_major_reason: overflow"); \
      buff[i] = 0; \
  }
        C(NOFREE, N);
        C(OLDGEN, O);
        C(SHADY,  S);
#if RGENGC_ESTIMATE_OLDMALLOC
        C(OLDMALLOC, M);
#endif
#undef C
    }
    return buff;
}
#endif



static void
gc_profile_dump_on(VALUE out, VALUE (*append)(VALUE, VALUE))
{
    rb_objspace_t *objspace = rb_gc_get_objspace();
    size_t count = objspace->profile.next_index;
#ifdef MAJOR_REASON_MAX
    char reason_str[MAJOR_REASON_MAX];
#endif

    if (objspace->profile.run && count /* > 1 */) {
        size_t i;
        const gc_profile_record *record;

        append(out, rb_sprintf("GC %"PRIuSIZE" invokes.\n", objspace->profile.count));
        append(out, rb_str_new_cstr("Index    Invoke Time(sec)       Use Size(byte)     Total Size(byte)         Total Object                    GC Time(ms)\n"));

        for (i = 0; i < count; i++) {
            record = &objspace->profile.records[i];
            append(out, rb_sprintf("%5"PRIuSIZE" %19.3f %20"PRIuSIZE" %20"PRIuSIZE" %20"PRIuSIZE" %30.20f\n",
                                   i+1, record->gc_invoke_time, record->heap_use_size,
                                   record->heap_total_size, record->heap_total_objects, record->gc_time*1000));
        }

#if GC_PROFILE_MORE_DETAIL
        const char *str = "\n\n" \
                                    "More detail.\n" \
                                    "Prepare Time = Previously GC's rest sweep time\n"
                                    "Index Flags          Allocate Inc.  Allocate Limit"
#if CALC_EXACT_MALLOC_SIZE
                                    "  Allocated Size"
#endif
                                    "  Use Page     Mark Time(ms)    Sweep Time(ms)  Prepare Time(ms)  LivingObj    FreeObj RemovedObj   EmptyObj"
#if RGENGC_PROFILE
                                    " OldgenObj RemNormObj RemShadObj"
#endif
#if GC_PROFILE_DETAIL_MEMORY
                                    " MaxRSS(KB) MinorFLT MajorFLT"
#endif
                                    "\n";
        append(out, rb_str_new_cstr(str));

        for (i = 0; i < count; i++) {
            record = &objspace->profile.records[i];
            append(out, rb_sprintf("%5"PRIuSIZE" %4s/%c/%6s%c %13"PRIuSIZE" %15"PRIuSIZE
#if CALC_EXACT_MALLOC_SIZE
                                   " %15"PRIuSIZE
#endif
                                   " %9"PRIuSIZE" %17.12f %17.12f %17.12f %10"PRIuSIZE" %10"PRIuSIZE" %10"PRIuSIZE" %10"PRIuSIZE
#if RGENGC_PROFILE
                                   "%10"PRIuSIZE" %10"PRIuSIZE" %10"PRIuSIZE
#endif
#if GC_PROFILE_DETAIL_MEMORY
                                   "%11ld %8ld %8ld"
#endif

                                   "\n",
                                   i+1,
                                   gc_profile_dump_major_reason(record->flags, reason_str),
                                   (record->flags & GPR_FLAG_HAVE_FINALIZE) ? 'F' : '.',
                                   (record->flags & GPR_FLAG_NEWOBJ) ? "NEWOBJ" :
                                   (record->flags & GPR_FLAG_MALLOC) ? "MALLOC" :
                                   (record->flags & GPR_FLAG_METHOD) ? "METHOD" :
                                   (record->flags & GPR_FLAG_CAPI)   ? "CAPI__" : "??????",
                                   (record->flags & GPR_FLAG_STRESS) ? '!' : ' ',
                                   record->allocate_increase, record->allocate_limit,
#if CALC_EXACT_MALLOC_SIZE
                                   record->allocated_size,
#endif
                                   record->heap_use_pages,
                                   record->gc_mark_time*1000,
                                   record->gc_sweep_time*1000,
                                   record->prepare_time*1000,

                                   record->heap_live_objects,
                                   record->heap_free_objects,
                                   record->removing_objects,
                                   record->empty_objects
#if RGENGC_PROFILE
                                   ,
                                   record->old_objects,
                                   record->remembered_normal_objects,
                                   record->remembered_shady_objects
#endif
#if GC_PROFILE_DETAIL_MEMORY
                                   ,
                                   record->maxrss / 1024,
                                   record->minflt,
                                   record->majflt
#endif

                       ));
        }
#endif
    }
}

/*
 *  call-seq:
 *     GC::Profiler.result  -> String
 *
 *  Returns a profile data report such as:
 *
 *    GC 1 invokes.
 *    Index    Invoke Time(sec)       Use Size(byte)     Total Size(byte)         Total Object                    GC time(ms)
 *        1               0.012               159240               212940                10647         0.00000000000001530000
 */

static VALUE
gc_profile_result(VALUE _)
{
    VALUE str = rb_str_buf_new(0);
    gc_profile_dump_on(str, rb_str_buf_append);
    return str;
}

/*
 *  call-seq:
 *     GC::Profiler.report
 *     GC::Profiler.report(io)
 *
 *  Writes the GC::Profiler.result to <tt>$stdout</tt> or the given IO object.
 *
 */

static VALUE
gc_profile_report(int argc, VALUE *argv, VALUE self)
{
    VALUE out;

    out = (!rb_check_arity(argc, 0, 1) ? rb_stdout : argv[0]);
    gc_profile_dump_on(out, rb_io_write);

    return Qnil;
}

/*
 *  call-seq:
 *     GC::Profiler.total_time	-> float
 *
 *  The total time used for garbage collection in seconds
 */

static VALUE
gc_profile_total_time(VALUE self)
{
    double time = 0;
    rb_objspace_t *objspace = rb_gc_get_objspace();

    if (objspace->profile.run && objspace->profile.next_index > 0) {
        size_t i;
        size_t count = objspace->profile.next_index;

        for (i = 0; i < count; i++) {
            time += objspace->profile.records[i].gc_time;
        }
    }
    return DBL2NUM(time);
}

/*
 *  call-seq:
 *    GC::Profiler.enabled?	-> true or false
 *
 *  The current status of \GC profile mode.
 */

static VALUE
gc_profile_enable_get(VALUE self)
{
    rb_objspace_t *objspace = rb_gc_get_objspace();
    return objspace->profile.run ? Qtrue : Qfalse;
}

/*
 *  call-seq:
 *    GC::Profiler.enable	-> nil
 *
 *  Starts the \GC profiler.
 *
 */

static VALUE
gc_profile_enable(VALUE _)
{
    rb_objspace_t *objspace = rb_gc_get_objspace();
    objspace->profile.run = TRUE;
    objspace->profile.current_record = 0;
    return Qnil;
}

/*
 *  call-seq:
 *    GC::Profiler.disable	-> nil
 *
 *  Stops the \GC profiler.
 *
 */

static VALUE
gc_profile_disable(VALUE _)
{
    rb_objspace_t *objspace = rb_gc_get_objspace();

    objspace->profile.run = FALSE;
    objspace->profile.current_record = 0;
    return Qnil;
}

/*
 *  call-seq:
 *     GC.verify_internal_consistency                  -> nil
 *
 *  Verify internal consistency.
 *
 *  This method is implementation specific.
 *  Now this method checks generational consistency
 *  if RGenGC is supported.
 */
static VALUE
gc_verify_internal_consistency_m(VALUE dummy)
{
    rb_gc_impl_verify_internal_consistency(rb_gc_get_objspace());
    return Qnil;
}

#if GC_CAN_COMPILE_COMPACTION
/*
 *  call-seq:
 *     GC.auto_compact = flag
 *
 *  Updates automatic compaction mode.
 *
 *  When enabled, the compactor will execute on every major collection.
 *
 *  Enabling compaction will degrade performance on major collections.
 */
static VALUE
gc_set_auto_compact(VALUE _, VALUE v)
{
    GC_ASSERT(GC_COMPACTION_SUPPORTED);

    ruby_enable_autocompact = RTEST(v);

#if RGENGC_CHECK_MODE
    ruby_autocompact_compare_func = NULL;

    if (SYMBOL_P(val)) {
        ID id = RB_SYM2ID(val);
        if (id == rb_intern("empty")) {
            ruby_autocompact_compare_func = compare_free_slots;
        }
    }
#endif

    return v;
}
#else
#  define gc_set_auto_compact rb_f_notimplement
#endif

#if GC_CAN_COMPILE_COMPACTION
/*
 *  call-seq:
 *     GC.auto_compact    -> true or false
 *
 *  Returns whether or not automatic compaction has been enabled.
 */
static VALUE
gc_get_auto_compact(VALUE _)
{
    return ruby_enable_autocompact ? Qtrue : Qfalse;
}
#else
#  define gc_get_auto_compact rb_f_notimplement
#endif

#if GC_CAN_COMPILE_COMPACTION
static VALUE
type_sym(size_t type)
{
    switch (type) {
#define COUNT_TYPE(t) case (t): return ID2SYM(rb_intern(#t)); break;
        COUNT_TYPE(T_NONE);
        COUNT_TYPE(T_OBJECT);
        COUNT_TYPE(T_CLASS);
        COUNT_TYPE(T_MODULE);
        COUNT_TYPE(T_FLOAT);
        COUNT_TYPE(T_STRING);
        COUNT_TYPE(T_REGEXP);
        COUNT_TYPE(T_ARRAY);
        COUNT_TYPE(T_HASH);
        COUNT_TYPE(T_STRUCT);
        COUNT_TYPE(T_BIGNUM);
        COUNT_TYPE(T_FILE);
        COUNT_TYPE(T_DATA);
        COUNT_TYPE(T_MATCH);
        COUNT_TYPE(T_COMPLEX);
        COUNT_TYPE(T_RATIONAL);
        COUNT_TYPE(T_NIL);
        COUNT_TYPE(T_TRUE);
        COUNT_TYPE(T_FALSE);
        COUNT_TYPE(T_SYMBOL);
        COUNT_TYPE(T_FIXNUM);
        COUNT_TYPE(T_IMEMO);
        COUNT_TYPE(T_UNDEF);
        COUNT_TYPE(T_NODE);
        COUNT_TYPE(T_ICLASS);
        COUNT_TYPE(T_ZOMBIE);
        COUNT_TYPE(T_MOVED);
#undef COUNT_TYPE
        default:              return SIZET2NUM(type); break;
    }
}

/*
 *  call-seq:
 *     GC.latest_compact_info -> hash
 *
 * Returns information about object moved in the most recent \GC compaction.
 *
 * The returned +hash+ contains the following keys:
 *
 * [considered]
 *   Hash containing the type of the object as the key and the number of
 *   objects of that type that were considered for movement.
 * [moved]
 *   Hash containing the type of the object as the key and the number of
 *   objects of that type that were actually moved.
 * [moved_up]
 *   Hash containing the type of the object as the key and the number of
 *   objects of that type that were increased in size.
 * [moved_down]
 *   Hash containing the type of the object as the key and the number of
 *   objects of that type that were decreased in size.
 *
 * Some objects can't be moved (due to pinning) so these numbers can be used to
 * calculate compaction efficiency.
 */
static VALUE
gc_compact_stats(VALUE self)
{
    rb_objspace_t *objspace = rb_gc_get_objspace();
    VALUE h = rb_hash_new();
    VALUE considered = rb_hash_new();
    VALUE moved = rb_hash_new();
    VALUE moved_up = rb_hash_new();
    VALUE moved_down = rb_hash_new();

    for (size_t i = 0; i < T_MASK; i++) {
        if (objspace->rcompactor.considered_count_table[i]) {
            rb_hash_aset(considered, type_sym(i), SIZET2NUM(objspace->rcompactor.considered_count_table[i]));
        }

        if (objspace->rcompactor.moved_count_table[i]) {
            rb_hash_aset(moved, type_sym(i), SIZET2NUM(objspace->rcompactor.moved_count_table[i]));
        }

        if (objspace->rcompactor.moved_up_count_table[i]) {
            rb_hash_aset(moved_up, type_sym(i), SIZET2NUM(objspace->rcompactor.moved_up_count_table[i]));
        }

        if (objspace->rcompactor.moved_down_count_table[i]) {
            rb_hash_aset(moved_down, type_sym(i), SIZET2NUM(objspace->rcompactor.moved_down_count_table[i]));
        }
    }

    rb_hash_aset(h, ID2SYM(rb_intern("considered")), considered);
    rb_hash_aset(h, ID2SYM(rb_intern("moved")), moved);
    rb_hash_aset(h, ID2SYM(rb_intern("moved_up")), moved_up);
    rb_hash_aset(h, ID2SYM(rb_intern("moved_down")), moved_down);

    return h;
}
#else
#  define gc_compact_stats rb_f_notimplement
#endif

#if GC_CAN_COMPILE_COMPACTION
/*
 *  call-seq:
 *     GC.compact -> hash
 *
 * This function compacts objects together in Ruby's heap. It eliminates
 * unused space (or fragmentation) in the heap by moving objects in to that
 * unused space.
 *
 * The returned +hash+ contains statistics about the objects that were moved;
 * see GC.latest_compact_info.
 *
 * This method is only expected to work on CRuby.
 *
 * To test whether \GC compaction is supported, use the idiom:
 *
 *   GC.respond_to?(:compact)
 */
static VALUE
gc_compact(VALUE self)
{
    /* Run GC with compaction enabled */
    rb_gc_impl_start(rb_gc_get_objspace(), true, true, true, true);

    return gc_compact_stats(self);
}
#else
#  define gc_compact rb_f_notimplement
#endif

#if GC_CAN_COMPILE_COMPACTION
/* call-seq:
 *    GC.verify_compaction_references(toward: nil, double_heap: false) -> hash
 *
 * Verify compaction reference consistency.
 *
 * This method is implementation specific.  During compaction, objects that
 * were moved are replaced with T_MOVED objects.  No object should have a
 * reference to a T_MOVED object after compaction.
 *
 * This function expands the heap to ensure room to move all objects,
 * compacts the heap to make sure everything moves, updates all references,
 * then performs a full \GC.  If any object contains a reference to a T_MOVED
 * object, that object should be pushed on the mark stack, and will
 * make a SEGV.
 */
static VALUE
gc_verify_compaction_references(int argc, VALUE* argv, VALUE self)
{
    static ID keywords[3] = {0};
    if (!keywords[0]) {
        keywords[0] = rb_intern("toward");
        keywords[1] = rb_intern("double_heap");
        keywords[2] = rb_intern("expand_heap");
    }

    VALUE options;
    rb_scan_args_kw(rb_keyword_given_p(), argc, argv, ":", &options);

    VALUE arguments[3] = { Qnil, Qfalse, Qfalse };
    int kwarg_count = rb_get_kwargs(options, keywords, 0, 3, arguments);
    bool toward_empty = kwarg_count > 0 && SYMBOL_P(arguments[0]) && SYM2ID(arguments[0]) == rb_intern("empty");
    bool expand_heap = (kwarg_count > 1 && RTEST(arguments[1])) || (kwarg_count > 2 && RTEST(arguments[2]));

    rb_objspace_t *objspace = rb_gc_get_objspace();

    /* Clear the heap. */
    rb_gc_impl_start(objspace, true, true, true, false);

    unsigned int lev = rb_gc_vm_lock();
    {
        /* if both double_heap and expand_heap are set, expand_heap takes precedence */
        if (expand_heap) {
            struct desired_compaction_pages_i_data desired_compaction = {
                .objspace = objspace,
                .required_slots = {0},
            };
            /* Work out how many objects want to be in each size pool, taking account of moves */
            objspace_each_pages(objspace, desired_compaction_pages_i, &desired_compaction, TRUE);

            /* Find out which pool has the most pages */
            size_t max_existing_pages = 0;
            for (int i = 0; i < SIZE_POOL_COUNT; i++) {
                rb_size_pool_t *size_pool = &size_pools[i];
                rb_heap_t *heap = SIZE_POOL_EDEN_HEAP(size_pool);
                max_existing_pages = MAX(max_existing_pages, heap->total_pages);
            }
            /* Add pages to each size pool so that compaction is guaranteed to move every object */
            for (int i = 0; i < SIZE_POOL_COUNT; i++) {
                rb_size_pool_t *size_pool = &size_pools[i];
                rb_heap_t *heap = SIZE_POOL_EDEN_HEAP(size_pool);

                size_t pages_to_add = 0;
                /*
                 * Step 1: Make sure every pool has the same number of pages, by adding empty pages
                 * to smaller pools. This is required to make sure the compact cursor can advance
                 * through all of the pools in `gc_sweep_compact` without hitting the "sweep &
                 * compact cursors met" condition on some pools before fully compacting others
                 */
                pages_to_add += max_existing_pages - heap->total_pages;
                /*
                 * Step 2: Now add additional free pages to each size pool sufficient to hold all objects
                 * that want to be in that size pool, whether moved into it or moved within it
                 */
                pages_to_add += slots_to_pages_for_size_pool(objspace, size_pool, desired_compaction.required_slots[i]);
                /*
                 * Step 3: Add two more pages so that the compact & sweep cursors will meet _after_ all objects
                 * have been moved, and not on the last iteration of the `gc_sweep_compact` loop
                 */
                pages_to_add += 2;

                heap_add_pages(objspace, size_pool, heap, pages_to_add);
            }
        }

        if (toward_empty) {
            objspace->rcompactor.compare_func = compare_free_slots;
        }
    }
    rb_gc_vm_unlock(lev);

    rb_gc_impl_start(rb_gc_get_objspace(), true, true, true, true);

    rb_objspace_reachable_objects_from_root(root_obj_check_moved_i, objspace);
    objspace_each_objects(objspace, heap_check_moved_i, objspace, TRUE);

    objspace->rcompactor.compare_func = NULL;

    return gc_compact_stats(self);
}
#else
# define gc_verify_compaction_references rb_f_notimplement
#endif

void
rb_gc_impl_objspace_free(void *objspace_ptr)
{
    rb_objspace_t *objspace = objspace_ptr;

    if (is_lazy_sweeping(objspace))
        rb_bug("lazy sweeping underway when freeing object space");

    free(objspace->profile.records);
    objspace->profile.records = NULL;

    if (heap_pages_sorted) {
        size_t i;
        size_t total_heap_pages = heap_allocated_pages;
        for (i = 0; i < total_heap_pages; ++i) {
            heap_page_free(objspace, heap_pages_sorted[i]);
        }
        free(heap_pages_sorted);
        heap_allocated_pages = 0;
        heap_pages_sorted_length = 0;
        heap_pages_lomem = 0;
        heap_pages_himem = 0;

        for (int i = 0; i < SIZE_POOL_COUNT; i++) {
            rb_size_pool_t *size_pool = &size_pools[i];
            SIZE_POOL_EDEN_HEAP(size_pool)->total_pages = 0;
            SIZE_POOL_EDEN_HEAP(size_pool)->total_slots = 0;
        }
    }
    st_free_table(objspace->id_to_obj_tbl);
    st_free_table(objspace->obj_to_id_tbl);

    free_stack_chunks(&objspace->mark_stack);
    mark_stack_free_cache(&objspace->mark_stack);

    rb_darray_free(objspace->weak_references);

    free(objspace);
}

static int
pin_value(st_data_t key, st_data_t value, st_data_t data)
{
    rb_gc_impl_mark_and_pin((void *)data, (VALUE)value);

    return ST_CONTINUE;
}

void rb_gc_impl_mark(void *objspace_ptr, VALUE obj);

static int
gc_mark_tbl_no_pin_i(st_data_t key, st_data_t value, st_data_t data)
{
    rb_gc_impl_mark((void *)data, (VALUE)value);

    return ST_CONTINUE;
}

#if MALLOC_ALLOCATED_SIZE
/*
 *  call-seq:
 *     GC.malloc_allocated_size -> Integer
 *
 *  Returns the size of memory allocated by malloc().
 *
 *  Only available if ruby was built with +CALC_EXACT_MALLOC_SIZE+.
 */

static VALUE
gc_malloc_allocated_size(VALUE self)
{
    return UINT2NUM(rb_objspace.malloc_params.allocated_size);
}

/*
 *  call-seq:
 *     GC.malloc_allocations -> Integer
 *
 *  Returns the number of malloc() allocations.
 *
 *  Only available if ruby was built with +CALC_EXACT_MALLOC_SIZE+.
 */

static VALUE
gc_malloc_allocations(VALUE self)
{
    return UINT2NUM(rb_objspace.malloc_params.allocations);
}
#endif

void
rb_gc_impl_objspace_mark(void *objspace_ptr)
{
    rb_objspace_t *objspace = objspace_ptr;

    objspace->rgengc.parent_object = Qfalse;

    if (finalizer_table != NULL) {
        st_foreach(finalizer_table, pin_value, (st_data_t)objspace);
    }

    st_foreach(objspace->obj_to_id_tbl, gc_mark_tbl_no_pin_i, (st_data_t)objspace);

    if (stress_to_class) rb_gc_mark(stress_to_class);
}

void *
rb_gc_impl_objspace_alloc(void)
{
    rb_objspace_t *objspace = calloc1(sizeof(rb_objspace_t));

    return objspace;
}

void
rb_gc_impl_objspace_init(void *objspace_ptr)
{
    rb_objspace_t *objspace = objspace_ptr;

    objspace->flags.gc_stressful = RTEST(initial_stress);
    objspace->gc_stress_mode = initial_stress;

    objspace->flags.measure_gc = true;
    malloc_limit = gc_params.malloc_limit_min;
    objspace->finalize_deferred_pjob = rb_postponed_job_preregister(0, gc_finalize_deferred, objspace);
    if (objspace->finalize_deferred_pjob == POSTPONED_JOB_HANDLE_INVALID) {
        rb_bug("Could not preregister postponed job for GC");
    }

    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        rb_size_pool_t *size_pool = &size_pools[i];

        size_pool->slot_size = (1 << i) * BASE_SLOT_SIZE;

        ccan_list_head_init(&SIZE_POOL_EDEN_HEAP(size_pool)->pages);
        ccan_list_head_init(&SIZE_POOL_TOMB_HEAP(size_pool)->pages);
    }

    rb_darray_make(&objspace->weak_references, 0);

    // TODO: debug why on Windows Ruby crashes on boot when GC is on.
#ifdef _WIN32
    dont_gc_on();
#endif

#if defined(INIT_HEAP_PAGE_ALLOC_USE_MMAP)
    /* Need to determine if we can use mmap at runtime. */
    heap_page_alloc_use_mmap = INIT_HEAP_PAGE_ALLOC_USE_MMAP;
#endif
    objspace->next_object_id = OBJ_ID_INITIAL;
    objspace->id_to_obj_tbl = st_init_table(&object_id_hash_type);
    objspace->obj_to_id_tbl = st_init_numtable();
#if RGENGC_ESTIMATE_OLDMALLOC
    objspace->rgengc.oldmalloc_increase_limit = gc_params.oldmalloc_limit_min;
#endif
    /* Set size pools allocatable pages. */
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        rb_size_pool_t *size_pool = &size_pools[i];
        /* Set the default value of size_pool_init_slots. */
        gc_params.size_pool_init_slots[i] = GC_HEAP_INIT_SLOTS;
        size_pool->allocatable_pages = minimum_pages_for_size_pool(objspace, size_pool);
    }

    heap_pages_expand_sorted(objspace);

    init_mark_stack(&objspace->mark_stack);

    objspace->profile.invoke_time = getrusage_time();
    finalizer_table = st_init_numtable();
}

void
rb_gc_impl_init(void)
{
    VALUE gc_constants = rb_hash_new();
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("DEBUG")), GC_DEBUG ? Qtrue : Qfalse);
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("BASE_SLOT_SIZE")), SIZET2NUM(BASE_SLOT_SIZE - RVALUE_OVERHEAD));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("RVALUE_OVERHEAD")), SIZET2NUM(RVALUE_OVERHEAD));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("HEAP_PAGE_OBJ_LIMIT")), SIZET2NUM(HEAP_PAGE_OBJ_LIMIT));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("HEAP_PAGE_BITMAP_SIZE")), SIZET2NUM(HEAP_PAGE_BITMAP_SIZE));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("HEAP_PAGE_SIZE")), SIZET2NUM(HEAP_PAGE_SIZE));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("SIZE_POOL_COUNT")), LONG2FIX(SIZE_POOL_COUNT));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("RVARGC_MAX_ALLOCATE_SIZE")), LONG2FIX(size_pool_slot_size(SIZE_POOL_COUNT - 1)));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("RVALUE_OLD_AGE")), LONG2FIX(RVALUE_OLD_AGE));
    if (RB_BUG_INSTEAD_OF_RB_MEMERROR+0) {
        rb_hash_aset(gc_constants, ID2SYM(rb_intern("RB_BUG_INSTEAD_OF_RB_MEMERROR")), Qtrue);
    }
    OBJ_FREEZE(gc_constants);
    /* Internal constants in the garbage collector. */
    rb_define_const(rb_mGC, "INTERNAL_CONSTANTS", gc_constants);

    if (GC_COMPACTION_SUPPORTED) {
        rb_define_singleton_method(rb_mGC, "compact", gc_compact, 0);
        rb_define_singleton_method(rb_mGC, "auto_compact", gc_get_auto_compact, 0);
        rb_define_singleton_method(rb_mGC, "auto_compact=", gc_set_auto_compact, 1);
        rb_define_singleton_method(rb_mGC, "latest_compact_info", gc_compact_stats, 0);
        rb_define_singleton_method(rb_mGC, "verify_compaction_references", gc_verify_compaction_references, -1);
    }
    else {
        rb_define_singleton_method(rb_mGC, "compact", rb_f_notimplement, 0);
        rb_define_singleton_method(rb_mGC, "auto_compact", rb_f_notimplement, 0);
        rb_define_singleton_method(rb_mGC, "auto_compact=", rb_f_notimplement, 1);
        rb_define_singleton_method(rb_mGC, "latest_compact_info", rb_f_notimplement, 0);
        rb_define_singleton_method(rb_mGC, "verify_compaction_references", rb_f_notimplement, -1);
    }

    /* internal methods */
    rb_define_singleton_method(rb_mGC, "verify_internal_consistency", gc_verify_internal_consistency_m, 0);

#if MALLOC_ALLOCATED_SIZE
    rb_define_singleton_method(rb_mGC, "malloc_allocated_size", gc_malloc_allocated_size, 0);
    rb_define_singleton_method(rb_mGC, "malloc_allocations", gc_malloc_allocations, 0);
#endif

    VALUE rb_mProfiler = rb_define_module_under(rb_mGC, "Profiler");
    rb_define_singleton_method(rb_mProfiler, "enabled?", gc_profile_enable_get, 0);
    rb_define_singleton_method(rb_mProfiler, "enable", gc_profile_enable, 0);
    rb_define_singleton_method(rb_mProfiler, "raw_data", gc_profile_record_get, 0);
    rb_define_singleton_method(rb_mProfiler, "disable", gc_profile_disable, 0);
    rb_define_singleton_method(rb_mProfiler, "clear", gc_profile_clear, 0);
    rb_define_singleton_method(rb_mProfiler, "result", gc_profile_result, 0);
    rb_define_singleton_method(rb_mProfiler, "report", gc_profile_report, -1);
    rb_define_singleton_method(rb_mProfiler, "total_time", gc_profile_total_time, 0);

    {
        VALUE opts;
        /* \GC build options */
        rb_define_const(rb_mGC, "OPTS", opts = rb_ary_new());
#define OPT(o) if (o) rb_ary_push(opts, rb_interned_str(#o, sizeof(#o) - 1))
        OPT(GC_DEBUG);
        OPT(USE_RGENGC);
        OPT(RGENGC_DEBUG);
        OPT(RGENGC_CHECK_MODE);
        OPT(RGENGC_PROFILE);
        OPT(RGENGC_ESTIMATE_OLDMALLOC);
        OPT(GC_PROFILE_MORE_DETAIL);
        OPT(GC_ENABLE_LAZY_SWEEP);
        OPT(CALC_EXACT_MALLOC_SIZE);
        OPT(MALLOC_ALLOCATED_SIZE);
        OPT(MALLOC_ALLOCATED_SIZE_CHECK);
        OPT(GC_PROFILE_DETAIL_MEMORY);
        OPT(GC_COMPACTION_SUPPORTED);
#undef OPT
        OBJ_FREEZE(opts);
    }
}
