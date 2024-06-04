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
void rb_gc_reachable_objects_from_callback(VALUE obj);
void rb_gc_event_hook(VALUE obj, rb_event_flag_t event);
void *rb_gc_get_objspace(void);
size_t rb_size_mul_or_raise(size_t x, size_t y, VALUE exc);
void rb_gc_run_obj_finalizer(VALUE objid, long count, VALUE (*callback)(long i, void *data), void *data);
void rb_gc_set_pending_interrupt(void);
void rb_gc_unset_pending_interrupt(void);
bool rb_gc_obj_free(void *objspace, VALUE obj);
void rb_gc_ractor_newobj_cache_foreach(void (*func)(void *cache, void *data), void *data);
bool rb_gc_multi_ractor_p(void);
void rb_objspace_reachable_objects_from_root(void (func)(const char *category, VALUE, void *), void *passing_data);
void rb_objspace_reachable_objects_from(VALUE obj, void (func)(VALUE, void *), void *data);
const char *rb_obj_info(VALUE obj);
bool rb_gc_shutdown_call_finalizer_p(VALUE obj);
uint32_t rb_gc_get_shape(VALUE obj);
void rb_gc_set_shape(VALUE obj, uint32_t shape_id);
uint32_t rb_gc_rebuild_shape(VALUE obj, size_t size_pool_id);
size_t rb_obj_memsize_of(VALUE obj);

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

#ifndef GC_HEAP_FREE_SLOTS_MIN_RATIO
#define GC_HEAP_FREE_SLOTS_MIN_RATIO  0.20
#endif
#ifndef GC_HEAP_FREE_SLOTS_GOAL_RATIO
#define GC_HEAP_FREE_SLOTS_GOAL_RATIO 0.40
#endif
#ifndef GC_HEAP_FREE_SLOTS_MAX_RATIO
#define GC_HEAP_FREE_SLOTS_MAX_RATIO  0.65
#endif

#ifndef PRINT_ENTER_EXIT_TICK
# define PRINT_ENTER_EXIT_TICK 0
#endif
#ifndef PRINT_ROOT_TICKS
#define PRINT_ROOT_TICKS 0
#endif

#define USE_TICK_T                 (PRINT_ENTER_EXIT_TICK || PRINT_ROOT_TICKS)

#define GC_ASSERT RUBY_ASSERT

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

    FALSE,
};

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

} gc_profile_record;

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

#define SIZE_POOL_EDEN_HEAP(size_pool) (&(size_pool)->eden_heap)
#define SIZE_POOL_TOMB_HEAP(size_pool) (&(size_pool)->tomb_heap)

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
#if MALLOC_ALLOCATED_SIZE
        size_t allocated_size;
        size_t allocations;
#endif
    } malloc_params;

    struct {
        unsigned int has_newobj_hook: 1;
    } flags;

    rb_event_flag_t hook_events;
    unsigned long long next_object_id;

    rb_size_pool_t size_pools[SIZE_POOL_COUNT];

    struct {
        rb_atomic_t finalizing;
    } atomic_flags;

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
    st_table *id_to_obj_tbl;
    st_table *obj_to_id_tbl;

    rb_postponed_job_handle_t finalize_deferred_pjob;
    unsigned long live_ractor_cache_count;
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

#define ruby_initial_gc_stress	gc_params.gc_stress

VALUE *ruby_initial_gc_stress_ptr = &ruby_initial_gc_stress;

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
#define finalizing		objspace->atomic_flags.finalizing
#define finalizer_table 	objspace->finalizer_table
#define ruby_gc_stressful	objspace->flags.gc_stressful
#define ruby_gc_stress_mode     objspace->gc_stress_mode

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

enum gc_enter_event {
    gc_enter_event_start,
    gc_enter_event_continue,
    gc_enter_event_rest,
    gc_enter_event_finalizer,
};

NO_SANITIZE("memory", static inline bool is_pointer_to_heap(rb_objspace_t *objspace, const void *ptr));

#define gc_prof_record(objspace) (objspace)->profile.current_record
#define gc_prof_enabled(objspace) ((objspace)->profile.run && (objspace)->profile.current_record)

# define gc_report if (!RUBY_DEBUG) {} else gc_report_body
PRINTF_ARGS(static void gc_report_body(rb_objspace_t *objspace, const char *fmt, ...), 2, 3);

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
    return Qnil;
}

VALUE
rb_gc_impl_set_measure_total_time(void *objspace_ptr, VALUE flag)
{
    return flag;
}

VALUE
rb_gc_impl_get_measure_total_time(void *objspace_ptr)
{
    return Qfalse;
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

static void
heap_pages_expand_sorted_to(rb_objspace_t *objspace, size_t next_length)
{
    struct heap_page **sorted;
    size_t size = rb_size_mul_or_raise(next_length, sizeof(struct heap_page *), rb_eRuntimeError);

    gc_report(objspace, "heap_pages_expand_sorted: next_length: %"PRIdSIZE", size: %"PRIdSIZE"\n",
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

// TODO: Can this go?
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

    asan_poison_object(obj);
    gc_report(objspace, "heap_page_add_freeobj: add %p to freelist\n", (void *)obj);
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

    // TODO: Do we need this?
    for (p = start; p != end; p += stride) {
        gc_report(objspace, "assign_heap_page: %p is added to freelist\n", (void *)p);
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
        gc_report(objspace, "heap_increment: heap_pages_sorted_length: %"PRIdSIZE", "
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

#if RACTOR_CHECK_MODE
    void rb_ractor_setup_belonging(VALUE obj);
    rb_ractor_setup_belonging(obj);
#endif

    if (RB_UNLIKELY(wb_protected == FALSE)) {
        MARK_IN_BITMAP(GET_HEAP_WB_UNPROTECTED_BITS(obj), obj);
    }


    gc_report(objspace, "newobj: %s\n", rb_obj_info(obj));

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

    if (p) {
        VALUE obj = (VALUE)p;
        MAYBE_UNUSED(const size_t) stride = size_pool_slot_size(size_pool_idx);
        size_pool_cache->freelist = p->next;
        asan_unpoison_memory_region(p, stride, true);
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
    gc_report(objspace, "ractor_set_cache: Using page %p\n", (void *)GET_PAGE_BODY(page->start));

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

VALUE
rb_gc_impl_new_obj(void *objspace_ptr, void *cache_ptr, VALUE klass, VALUE flags, VALUE v1, VALUE v2, VALUE v3, bool wb_protected, size_t alloc_size)
{
    VALUE obj;
    rb_objspace_t *objspace = objspace_ptr;

    RB_DEBUG_COUNTER_INC(obj_newobj);
    (void)RB_DEBUG_COUNTER_INC_IF(obj_newobj_wb_unprotected, !wb_protected);

    size_t size_pool_idx = size_pool_idx_for_size(alloc_size);

    rb_ractor_newobj_cache_t *cache = (rb_ractor_newobj_cache_t *)cache_ptr;

    obj = newobj_alloc(objspace, cache, size_pool_idx, false);
    newobj_init(klass, flags, wb_protected, objspace, obj);

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

    if (RUBY_ATOMIC_EXCHANGE(finalizing, 1)) return;

    /* run finalizers */
    finalize_deferred(objspace);
    GC_ASSERT(heap_pages_deferred_final == 0);

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
    return value;
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

static void
gc_report_body(rb_objspace_t *objspace, const char *fmt, ...)
{
    char buf[1024];
    FILE *out = stderr;
    va_list args;
    const char *status = " ";

    va_start(args, fmt);
    vsnprintf(buf, 1024, fmt, args);
    va_end(args);

    fprintf(out, "%s|", status);
    fputs(buf, out);
}

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

int ruby_thread_has_gvl_p(void);

void
rb_gc_impl_start(void *objspace_ptr, bool full_mark, bool immediate_mark, bool immediate_sweep, bool compact)
{
    // Starting a GC is a no-op with Epsilon GC
}

void
rb_gc_impl_prepare_heap(void *objspace_ptr)
{
}

void rb_mv_generic_ivar(VALUE src, VALUE dst);

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

size_t
rb_gc_impl_gc_count(void *objspace_ptr)
{
    return 0;
}

static VALUE
gc_info_decode(rb_objspace_t *objspace, const VALUE hash_or_key, const unsigned int orig_flags)
{
    return Qnil;
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
#if RGENGC_ESTIMATE_OLDMALLOC
    gc_stat_sym_oldmalloc_increase_bytes,
    gc_stat_sym_oldmalloc_increase_bytes_limit,
#endif
    gc_stat_sym_weak_references_count,
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
#if RGENGC_ESTIMATE_OLDMALLOC
        S(oldmalloc_increase_bytes);
        S(oldmalloc_increase_bytes_limit);
#endif
        S(weak_references_count);
#undef S
    }
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

    /* implementation dependent counters */
    SET(heap_allocated_pages, heap_allocated_pages);
    SET(heap_sorted_length, heap_pages_sorted_length);
    SET(heap_allocatable_pages, heap_allocatable_pages(objspace));
    SET(heap_available_slots, objspace_available_slots(objspace));
    SET(heap_live_slots, objspace_live_slots(objspace));
    SET(heap_free_slots, objspace_free_slots(objspace));
    SET(heap_final_slots, heap_pages_final_slots);
    SET(heap_eden_pages, heap_eden_total_pages(objspace));
    SET(heap_tomb_pages, heap_tomb_total_pages(objspace));
    SET(total_allocated_pages, total_allocated_pages(objspace));
    SET(total_freed_pages, total_freed_pages(objspace));
    SET(total_allocated_objects, total_allocated_objects(objspace));
    SET(total_freed_objects, total_freed_objects(objspace));
    SET(malloc_increase_bytes, malloc_increase);
#undef SET

    if (!NIL_P(key)) { /* matched key should return above */
        rb_raise(rb_eArgError, "unknown key: %"PRIsVALUE, rb_sym2str(key));
    }

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
    return Qfalse;
}

void
rb_gc_impl_stress_set(void *objspace_ptr, VALUE flag)
{
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
 *
 *  * obsolete
 *    * RUBY_FREE_MIN       -> RUBY_GC_HEAP_FREE_SLOTS (from 2.1)
 *    * RUBY_HEAP_MIN_SLOTS -> RUBY_GC_HEAP_INIT_SLOTS (from 2.1)
 *
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
    get_envparam_double("RUBY_GC_HEAP_REMEMBERED_WB_UNPROTECTED_OBJECTS_LIMIT_RATIO", &gc_params.uncollectible_wb_unprotected_objects_limit_ratio, 0.0, 0.0, TRUE);
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
    }
    else {
        atomic_sub_nounderflow(&malloc_increase, old_size - new_size);
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

#define gc_set_auto_compact rb_f_notimplement

void
rb_gc_impl_objspace_free(void *objspace_ptr)
{
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

#if defined(INIT_HEAP_PAGE_ALLOC_USE_MMAP)
    /* Need to determine if we can use mmap at runtime. */
    heap_page_alloc_use_mmap = INIT_HEAP_PAGE_ALLOC_USE_MMAP;
#endif
    objspace->next_object_id = OBJ_ID_INITIAL;
    objspace->id_to_obj_tbl = st_init_table(&object_id_hash_type);
    objspace->obj_to_id_tbl = st_init_numtable();
    /* Set size pools allocatable pages. */
    for (int i = 0; i < SIZE_POOL_COUNT; i++) {
        rb_size_pool_t *size_pool = &size_pools[i];
        /* Set the default value of size_pool_init_slots. */
        gc_params.size_pool_init_slots[i] = GC_HEAP_INIT_SLOTS;
        size_pool->allocatable_pages = minimum_pages_for_size_pool(objspace, size_pool);
    }

    heap_pages_expand_sorted(objspace);

    finalizer_table = st_init_numtable();
}

void
rb_gc_impl_init(void)
{
    VALUE gc_constants = rb_hash_new();
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("BASE_SLOT_SIZE")), SIZET2NUM(BASE_SLOT_SIZE - RVALUE_OVERHEAD));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("RVALUE_OVERHEAD")), SIZET2NUM(RVALUE_OVERHEAD));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("HEAP_PAGE_OBJ_LIMIT")), SIZET2NUM(HEAP_PAGE_OBJ_LIMIT));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("HEAP_PAGE_BITMAP_SIZE")), SIZET2NUM(HEAP_PAGE_BITMAP_SIZE));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("HEAP_PAGE_SIZE")), SIZET2NUM(HEAP_PAGE_SIZE));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("SIZE_POOL_COUNT")), LONG2FIX(SIZE_POOL_COUNT));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("RVARGC_MAX_ALLOCATE_SIZE")), LONG2FIX(size_pool_slot_size(SIZE_POOL_COUNT - 1)));
    if (RB_BUG_INSTEAD_OF_RB_MEMERROR+0) {
        rb_hash_aset(gc_constants, ID2SYM(rb_intern("RB_BUG_INSTEAD_OF_RB_MEMERROR")), Qtrue);
    }
    OBJ_FREEZE(gc_constants);
    /* Internal constants in the garbage collector. */
    rb_define_const(rb_mGC, "INTERNAL_CONSTANTS", gc_constants);

    rb_define_singleton_method(rb_mGC, "compact", rb_f_notimplement, 0);
    rb_define_singleton_method(rb_mGC, "auto_compact", rb_f_notimplement, 0);
    rb_define_singleton_method(rb_mGC, "auto_compact=", rb_f_notimplement, 1);
    rb_define_singleton_method(rb_mGC, "latest_compact_info", rb_f_notimplement, 0);
    rb_define_singleton_method(rb_mGC, "verify_compaction_references", rb_f_notimplement, -1);

    /* internal methods */
    rb_define_singleton_method(rb_mGC, "verify_internal_consistency", rb_f_notimplement, 0);

#if MALLOC_ALLOCATED_SIZE
    rb_define_singleton_method(rb_mGC, "malloc_allocated_size", gc_malloc_allocated_size, 0);
    rb_define_singleton_method(rb_mGC, "malloc_allocations", gc_malloc_allocations, 0);
#endif

    VALUE rb_mProfiler = rb_define_module_under(rb_mGC, "Profiler");
    rb_define_singleton_method(rb_mProfiler, "enabled?", rb_f_notimplement, 0);
    rb_define_singleton_method(rb_mProfiler, "enable", rb_f_notimplement, 0);
    rb_define_singleton_method(rb_mProfiler, "raw_data", rb_f_notimplement, 0);
    rb_define_singleton_method(rb_mProfiler, "disable", rb_f_notimplement, 0);
    rb_define_singleton_method(rb_mProfiler, "clear", rb_f_notimplement, 0);
    rb_define_singleton_method(rb_mProfiler, "result", rb_f_notimplement, 0);
    rb_define_singleton_method(rb_mProfiler, "report", rb_f_notimplement, -1);
    rb_define_singleton_method(rb_mProfiler, "total_time", rb_f_notimplement, 0);

    {
        VALUE opts;
        /* \GC build options */
        rb_define_const(rb_mGC, "OPTS", opts = rb_ary_new());
#define OPT(o) if (o) rb_ary_push(opts, rb_interned_str(#o, sizeof(#o) - 1))
        OPT(GC_PROFILE_MORE_DETAIL);
        OPT(CALC_EXACT_MALLOC_SIZE);
        OPT(MALLOC_ALLOCATED_SIZE);
        OPT(MALLOC_ALLOCATED_SIZE_CHECK);
        OPT(GC_PROFILE_DETAIL_MEMORY);
#undef OPT
        OBJ_FREEZE(opts);
    }
}
