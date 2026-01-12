#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "ruby/ruby.h"
#include "gc/gc.h"
#include "gc/gc_impl.h"
#include "gc/immix/immix.h"

#define IMMIX_HEAP_COUNT 6
#define IMMIX_MAX_OBJ_SIZE 640

static size_t heap_sizes[IMMIX_HEAP_COUNT + 1] = {
    32, 40, 80, 160, 320, IMMIX_MAX_OBJ_SIZE, 0
};

struct objspace {
    bool gc_enabled;
    bool gc_stress;
    bool measure_gc_time;
    size_t gc_count;
    size_t total_allocated_objects;
    unsigned long long total_gc_time;
    st_table *finalizer_table;
};

struct ractor_cache {
    void *bump_cursor;
    void *bump_limit;
};

void *
rb_gc_impl_objspace_alloc(void)
{
    struct objspace *objspace = calloc(1, sizeof(struct objspace));
    return objspace;
}

void
rb_gc_impl_objspace_init(void *objspace_ptr)
{
    struct objspace *objspace = objspace_ptr;
    objspace->gc_enabled = true;
    objspace->measure_gc_time = true;
    objspace->finalizer_table = st_init_numtable();
}

void *
rb_gc_impl_ractor_cache_alloc(void *objspace_ptr, void *ractor)
{
    struct ractor_cache *cache = calloc(1, sizeof(struct ractor_cache));
    return cache;
}

void
rb_gc_impl_set_params(void *objspace_ptr)
{
}

static VALUE
gc_verify_internal_consistency(VALUE self)
{
    return Qnil;
}

void
rb_gc_impl_init(void)
{
    VALUE gc_constants = rb_hash_new();
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("BASE_SLOT_SIZE")), SIZET2NUM(sizeof(VALUE) * 5));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("RBASIC_SIZE")), SIZET2NUM(sizeof(struct RBasic)));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("RVALUE_OVERHEAD")), INT2NUM(0));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("RVARGC_MAX_ALLOCATE_SIZE")), LONG2FIX(IMMIX_MAX_OBJ_SIZE));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("SIZE_POOL_COUNT")), LONG2FIX(IMMIX_HEAP_COUNT));
    rb_hash_aset(gc_constants, ID2SYM(rb_intern("RVALUE_OLD_AGE")), INT2FIX(0));
    OBJ_FREEZE(gc_constants);
    rb_define_const(rb_mGC, "INTERNAL_CONSTANTS", gc_constants);
    rb_define_singleton_method(rb_mGC, "verify_internal_consistency", gc_verify_internal_consistency, 0);
    rb_define_singleton_method(rb_mGC, "compact", rb_f_notimplement, 0);
    rb_define_singleton_method(rb_mGC, "auto_compact", rb_f_notimplement, 0);
    rb_define_singleton_method(rb_mGC, "auto_compact=", rb_f_notimplement, 1);
    rb_define_singleton_method(rb_mGC, "latest_compact_info", rb_f_notimplement, 0);
    rb_define_singleton_method(rb_mGC, "verify_compaction_references", rb_f_notimplement, -1);
}

size_t *
rb_gc_impl_heap_sizes(void *objspace_ptr)
{
    return heap_sizes;
}

void
rb_gc_impl_shutdown_free_objects(void *objspace_ptr)
{
}

void
rb_gc_impl_objspace_free(void *objspace_ptr)
{
    struct objspace *objspace = objspace_ptr;
    if (objspace->finalizer_table) {
        st_free_table(objspace->finalizer_table);
    }
    free(objspace);
}

void
rb_gc_impl_ractor_cache_free(void *objspace_ptr, void *cache_ptr)
{
    free(cache_ptr);
}

void
rb_gc_impl_start(void *objspace_ptr, bool full_mark, bool immediate_mark, bool immediate_sweep, bool compact)
{
    struct objspace *objspace = objspace_ptr;
    objspace->gc_count++;
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
    struct objspace *objspace = objspace_ptr;
    objspace->gc_enabled = true;
}

void
rb_gc_impl_gc_disable(void *objspace_ptr, bool finish_current_gc)
{
    struct objspace *objspace = objspace_ptr;
    objspace->gc_enabled = false;
}

bool
rb_gc_impl_gc_enabled_p(void *objspace_ptr)
{
    struct objspace *objspace = objspace_ptr;
    return objspace->gc_enabled;
}

void
rb_gc_impl_stress_set(void *objspace_ptr, VALUE flag)
{
    struct objspace *objspace = objspace_ptr;
    objspace->gc_stress = RTEST(flag);
}

VALUE
rb_gc_impl_stress_get(void *objspace_ptr)
{
    struct objspace *objspace = objspace_ptr;
    return objspace->gc_stress ? Qtrue : Qfalse;
}

VALUE
rb_gc_impl_config_get(void *objspace_ptr)
{
    VALUE hash = rb_hash_new();
    rb_hash_aset(hash, ID2SYM(rb_intern("implementation")), rb_str_new_cstr("immix"));
    rb_hash_aset(hash, ID2SYM(rb_intern("block_size")), SIZET2NUM(IMMIX_BLOCK_SIZE));
    rb_hash_aset(hash, ID2SYM(rb_intern("line_size")), SIZET2NUM(IMMIX_LINE_SIZE));
    return hash;
}

void
rb_gc_impl_config_set(void *objspace_ptr, VALUE hash)
{
}

VALUE
rb_gc_impl_new_obj(void *objspace_ptr, void *cache_ptr, VALUE klass, VALUE flags, bool wb_protected, size_t alloc_size)
{
    struct objspace *objspace = objspace_ptr;
    if (alloc_size > IMMIX_MAX_OBJ_SIZE) {
        rb_bug("immix: allocation size %zu exceeds maximum %d", alloc_size, IMMIX_MAX_OBJ_SIZE);
    }
    for (int i = 0; i < IMMIX_HEAP_COUNT; i++) {
        if (alloc_size <= heap_sizes[i]) {
            alloc_size = heap_sizes[i];
            break;
        }
    }
    size_t total_size = alloc_size + sizeof(VALUE);
    VALUE *alloc_obj = malloc(total_size);
    if (!alloc_obj) {
        rb_bug("immix: malloc failed for size %zu", total_size);
    }
    alloc_obj++;
    alloc_obj[-1] = alloc_size;
    alloc_obj[0] = flags;
    alloc_obj[1] = klass;
    objspace->total_allocated_objects++;
    return (VALUE)alloc_obj;
}

size_t
rb_gc_impl_obj_slot_size(VALUE obj)
{
    return ((VALUE *)obj)[-1];
}

size_t
rb_gc_impl_heap_id_for_size(void *objspace_ptr, size_t size)
{
    for (int i = 0; i < IMMIX_HEAP_COUNT; i++) {
        if (size <= heap_sizes[i]) return i;
    }
    rb_bug("immix: size %zu too large for any heap", size);
}

bool
rb_gc_impl_size_allocatable_p(size_t size)
{
    return size <= IMMIX_MAX_OBJ_SIZE;
}

void *
rb_gc_impl_malloc(void *objspace_ptr, size_t size, bool gc_allowed)
{
    return malloc(size);
}

void *
rb_gc_impl_calloc(void *objspace_ptr, size_t size, bool gc_allowed)
{
    return calloc(1, size);
}

void *
rb_gc_impl_realloc(void *objspace_ptr, void *ptr, size_t new_size, size_t old_size, bool gc_allowed)
{
    return realloc(ptr, new_size);
}

void
rb_gc_impl_free(void *objspace_ptr, void *ptr, size_t old_size)
{
    free(ptr);
}

void
rb_gc_impl_adjust_memory_usage(void *objspace_ptr, ssize_t diff)
{
}

void
rb_gc_impl_mark(void *objspace_ptr, VALUE obj)
{
    if (RB_SPECIAL_CONST_P(obj)) return;
}

void
rb_gc_impl_mark_and_move(void *objspace_ptr, VALUE *ptr)
{
    if (RB_SPECIAL_CONST_P(*ptr)) return;
}

void
rb_gc_impl_mark_and_pin(void *objspace_ptr, VALUE obj)
{
    if (RB_SPECIAL_CONST_P(obj)) return;
}

void
rb_gc_impl_mark_maybe(void *objspace_ptr, VALUE obj)
{
    if (rb_gc_impl_pointer_to_heap_p(objspace_ptr, (const void *)obj)) {
        rb_gc_impl_mark_and_pin(objspace_ptr, obj);
    }
}

void
rb_gc_impl_declare_weak_references(void *objspace_ptr, VALUE obj)
{
}

bool
rb_gc_impl_handle_weak_references_alive_p(void *objspace_ptr, VALUE obj)
{
    return true;
}

void
rb_gc_impl_register_pinning_obj(void *objspace_ptr, VALUE obj)
{
}

bool
rb_gc_impl_object_moved_p(void *objspace_ptr, VALUE obj)
{
    return false;
}

VALUE
rb_gc_impl_location(void *objspace_ptr, VALUE obj)
{
    return obj;
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
rb_gc_impl_writebarrier_remember(void *objspace_ptr, VALUE obj)
{
}

void
rb_gc_impl_each_objects(void *objspace_ptr, int (*callback)(void *, void *, size_t, void *), void *data)
{
}

void
rb_gc_impl_each_object(void *objspace_ptr, void (*func)(VALUE obj, void *data), void *data)
{
}

void
rb_gc_impl_make_zombie(void *objspace_ptr, VALUE obj, void (*dfree)(void *), void *data)
{
    if (dfree) {
        dfree(data);
    }
}

VALUE
rb_gc_impl_define_finalizer(void *objspace_ptr, VALUE obj, VALUE block)
{
    struct objspace *objspace = objspace_ptr;
    VALUE table;
    st_data_t data;
    RBASIC(obj)->flags |= FL_FINALIZE;
    int lev = RB_GC_VM_LOCK();
    if (st_lookup(objspace->finalizer_table, obj, &data)) {
        table = (VALUE)data;
        rb_ary_push(table, block);
    }
    else {
        table = rb_ary_new3(2, rb_obj_id(obj), block);
        rb_obj_hide(table);
        st_add_direct(objspace->finalizer_table, obj, table);
    }
    RB_GC_VM_UNLOCK(lev);
    return block;
}

void
rb_gc_impl_undefine_finalizer(void *objspace_ptr, VALUE obj)
{
    struct objspace *objspace = objspace_ptr;
    st_data_t data = obj;
    int lev = RB_GC_VM_LOCK();
    st_delete(objspace->finalizer_table, &data, 0);
    RB_GC_VM_UNLOCK(lev);
    FL_UNSET(obj, FL_FINALIZE);
}

void
rb_gc_impl_copy_finalizer(void *objspace_ptr, VALUE dest, VALUE obj)
{
    struct objspace *objspace = objspace_ptr;
    VALUE table;
    st_data_t data;
    if (!FL_TEST(obj, FL_FINALIZE)) return;
    int lev = RB_GC_VM_LOCK();
    if (st_lookup(objspace->finalizer_table, obj, &data)) {
        table = rb_ary_dup((VALUE)data);
        RARRAY_ASET(table, 0, rb_obj_id(dest));
        st_insert(objspace->finalizer_table, dest, table);
        FL_SET(dest, FL_FINALIZE);
    }
    RB_GC_VM_UNLOCK(lev);
}

void
rb_gc_impl_shutdown_call_finalizer(void *objspace_ptr)
{
}

void
rb_gc_impl_before_fork(void *objspace_ptr)
{
}

void
rb_gc_impl_after_fork(void *objspace_ptr, rb_pid_t pid)
{
}

void
rb_gc_impl_set_measure_total_time(void *objspace_ptr, VALUE flag)
{
    struct objspace *objspace = objspace_ptr;
    objspace->measure_gc_time = RTEST(flag);
}

bool
rb_gc_impl_get_measure_total_time(void *objspace_ptr)
{
    struct objspace *objspace = objspace_ptr;
    return objspace->measure_gc_time;
}

unsigned long long
rb_gc_impl_get_total_time(void *objspace_ptr)
{
    struct objspace *objspace = objspace_ptr;
    return objspace->total_gc_time;
}

size_t
rb_gc_impl_gc_count(void *objspace_ptr)
{
    struct objspace *objspace = objspace_ptr;
    return objspace->gc_count;
}

VALUE
rb_gc_impl_latest_gc_info(void *objspace_ptr, VALUE hash_or_key)
{
    VALUE hash = Qnil, key = Qnil;
    if (SYMBOL_P(hash_or_key)) {
        key = hash_or_key;
    }
    else if (RB_TYPE_P(hash_or_key, T_HASH)) {
        hash = hash_or_key;
    }
    else {
        rb_bug("gc_info_decode: non-hash or symbol given");
    }
#define SET(name, attr) \
    if (key == ID2SYM(rb_intern_const(#name))) \
        return (attr); \
    else if (hash != Qnil) \
        rb_hash_aset(hash, ID2SYM(rb_intern_const(#name)), (attr));
    SET(state, ID2SYM(rb_intern_const("none")));
#undef SET
    if (!NIL_P(key)) {
        return Qundef;
    }
    return hash;
}

enum gc_stat_sym {
    gc_stat_sym_count,
    gc_stat_sym_time,
    gc_stat_sym_total_allocated_objects,
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
        S(total_allocated_objects);
#undef S
    }
}

VALUE
rb_gc_impl_stat(void *objspace_ptr, VALUE hash_or_sym)
{
    struct objspace *objspace = objspace_ptr;
    VALUE hash = Qnil, key = Qnil;
    setup_gc_stat_symbols();
    if (RB_TYPE_P(hash_or_sym, T_HASH)) {
        hash = hash_or_sym;
    }
    else if (SYMBOL_P(hash_or_sym)) {
        key = hash_or_sym;
    }
    else {
        rb_bug("non-hash or symbol given");
    }
#define SET(name, attr) \
    if (key == gc_stat_symbols[gc_stat_sym_##name]) \
        return SIZET2NUM(attr); \
    else if (hash != Qnil) \
        rb_hash_aset(hash, gc_stat_symbols[gc_stat_sym_##name], SIZET2NUM(attr));
    SET(count, objspace->gc_count);
    SET(time, objspace->total_gc_time / (1000 * 1000));
    SET(total_allocated_objects, objspace->total_allocated_objects);
#undef SET
    if (!NIL_P(key)) {
        return Qundef;
    }
    return hash;
}

VALUE
rb_gc_impl_stat_heap(void *objspace_ptr, VALUE heap_name, VALUE hash_or_sym)
{
    if (RB_TYPE_P(hash_or_sym, T_HASH)) {
        return hash_or_sym;
    }
    else {
        return Qundef;
    }
}

#define RB_GC_OBJECT_METADATA_ENTRY_COUNT 1
static struct rb_gc_object_metadata_entry object_metadata_entries[RB_GC_OBJECT_METADATA_ENTRY_COUNT + 1];

struct rb_gc_object_metadata_entry *
rb_gc_impl_object_metadata(void *objspace_ptr, VALUE obj)
{
    static ID ID_object_id;
    if (!ID_object_id) {
        ID_object_id = rb_intern("object_id");
    }
    size_t n = 0;
#define SET_ENTRY(na, v) do { \
    object_metadata_entries[n].name = ID_##na; \
    object_metadata_entries[n].val = v; \
    n++; \
} while (0)
    if (rb_obj_id_p(obj)) SET_ENTRY(object_id, rb_obj_id(obj));
#undef SET_ENTRY
    object_metadata_entries[n].name = 0;
    object_metadata_entries[n].val = 0;
    return object_metadata_entries;
}

bool
rb_gc_impl_pointer_to_heap_p(void *objspace_ptr, const void *ptr)
{
    if (ptr == NULL) return false;
    if ((uintptr_t)ptr % sizeof(void*) != 0) return false;
    return true;
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
    rb_gc_impl_copy_finalizer(objspace_ptr, dest, obj);
}

const char *
rb_gc_impl_active_gc_name(void)
{
    return "immix";
}
