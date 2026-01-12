# Immix GC Implementation Plan for Ruby

This document provides comprehensive context for implementing an Immix garbage collector as a modular GC plugin for Ruby. It covers the Ruby modular GC architecture, the Immix algorithm, a phased implementation plan, risks, and build instructions.

## Table of Contents

1. [Ruby Modular GC Architecture](#ruby-modular-gc-architecture)
2. [Build System Integration](#build-system-integration)
3. [Immix Algorithm Overview](#immix-algorithm-overview)
4. [Implementation Phases](#implementation-phases)
5. [Risks and Mitigations](#risks-and-mitigations)
6. [References](#references)

---

## Ruby Modular GC Architecture

Ruby's modular GC feature (introduced in Feature #20470) allows alternative garbage collector implementations to be loaded at runtime. The system is experimental but provides a clean separation between Ruby's VM and its memory management.

### Key Files

| File | Purpose |
|------|---------|
| `gc/gc_impl.h` | Defines all `rb_gc_impl_*` functions a GC must implement |
| `gc/gc.h` | Helper functions GCs can call into Ruby's VM |
| `gc/extconf_base.rb` | Shared build configuration for GC modules |
| `gc/README.md` | Official documentation for modular GC usage |

### API Contract (gc/gc_impl.h)

Every modular GC must implement the following function categories:

#### Bootup & Shutdown
```c
void *rb_gc_impl_objspace_alloc(void);
void rb_gc_impl_objspace_init(void *objspace_ptr);
void *rb_gc_impl_ractor_cache_alloc(void *objspace_ptr, void *ractor);
void rb_gc_impl_set_params(void *objspace_ptr);
void rb_gc_impl_init(void);
size_t *rb_gc_impl_heap_sizes(void *objspace_ptr);
void rb_gc_impl_shutdown_free_objects(void *objspace_ptr);
void rb_gc_impl_objspace_free(void *objspace_ptr);
void rb_gc_impl_ractor_cache_free(void *objspace_ptr, void *cache);
```

#### GC Control
```c
void rb_gc_impl_start(void *objspace_ptr, bool full_mark, bool immediate_mark,
                      bool immediate_sweep, bool compact);
bool rb_gc_impl_during_gc_p(void *objspace_ptr);
void rb_gc_impl_prepare_heap(void *objspace_ptr);
void rb_gc_impl_gc_enable(void *objspace_ptr);
void rb_gc_impl_gc_disable(void *objspace_ptr, bool finish_current_gc);
bool rb_gc_impl_gc_enabled_p(void *objspace_ptr);
void rb_gc_impl_stress_set(void *objspace_ptr, VALUE flag);
VALUE rb_gc_impl_stress_get(void *objspace_ptr);
VALUE rb_gc_impl_config_get(void *objspace_ptr);
void rb_gc_impl_config_set(void *objspace_ptr, VALUE hash);
```

#### Object Allocation
```c
VALUE rb_gc_impl_new_obj(void *objspace_ptr, void *cache_ptr, VALUE klass,
                         VALUE flags, bool wb_protected, size_t alloc_size);
size_t rb_gc_impl_obj_slot_size(VALUE obj);
size_t rb_gc_impl_heap_id_for_size(void *objspace_ptr, size_t size);
bool rb_gc_impl_size_allocatable_p(size_t size);
```

#### Malloc Integration
```c
void *rb_gc_impl_malloc(void *objspace_ptr, size_t size, bool gc_allowed);
void *rb_gc_impl_calloc(void *objspace_ptr, size_t size, bool gc_allowed);
void *rb_gc_impl_realloc(void *objspace_ptr, void *ptr, size_t new_size,
                         size_t old_size, bool gc_allowed);
void rb_gc_impl_free(void *objspace_ptr, void *ptr, size_t old_size);
void rb_gc_impl_adjust_memory_usage(void *objspace_ptr, ssize_t diff);
```

#### Marking
```c
void rb_gc_impl_mark(void *objspace_ptr, VALUE obj);
void rb_gc_impl_mark_and_move(void *objspace_ptr, VALUE *ptr);
void rb_gc_impl_mark_and_pin(void *objspace_ptr, VALUE obj);
void rb_gc_impl_mark_maybe(void *objspace_ptr, VALUE obj);
```

#### Weak References
```c
void rb_gc_impl_declare_weak_references(void *objspace_ptr, VALUE obj);
bool rb_gc_impl_handle_weak_references_alive_p(void *objspace_ptr, VALUE obj);
```

#### Compaction
```c
void rb_gc_impl_register_pinning_obj(void *objspace_ptr, VALUE obj);
bool rb_gc_impl_object_moved_p(void *objspace_ptr, VALUE obj);
VALUE rb_gc_impl_location(void *objspace_ptr, VALUE value);
```

#### Write Barriers
```c
void rb_gc_impl_writebarrier(void *objspace_ptr, VALUE a, VALUE b);
void rb_gc_impl_writebarrier_unprotect(void *objspace_ptr, VALUE obj);
void rb_gc_impl_writebarrier_remember(void *objspace_ptr, VALUE obj);
```

#### Heap Walking
```c
void rb_gc_impl_each_objects(void *objspace_ptr,
                             int (*callback)(void *, void *, size_t, void *),
                             void *data);
void rb_gc_impl_each_object(void *objspace_ptr,
                            void (*func)(VALUE obj, void *data), void *data);
```

#### Finalizers
```c
void rb_gc_impl_make_zombie(void *objspace_ptr, VALUE obj,
                            void (*dfree)(void *), void *data);
VALUE rb_gc_impl_define_finalizer(void *objspace_ptr, VALUE obj, VALUE block);
void rb_gc_impl_undefine_finalizer(void *objspace_ptr, VALUE obj);
void rb_gc_impl_copy_finalizer(void *objspace_ptr, VALUE dest, VALUE obj);
void rb_gc_impl_shutdown_call_finalizer(void *objspace_ptr);
```

#### Forking
```c
void rb_gc_impl_before_fork(void *objspace_ptr);
void rb_gc_impl_after_fork(void *objspace_ptr, rb_pid_t pid);
```

#### Statistics
```c
void rb_gc_impl_set_measure_total_time(void *objspace_ptr, VALUE flag);
bool rb_gc_impl_get_measure_total_time(void *objspace_ptr);
unsigned long long rb_gc_impl_get_total_time(void *objspace_ptr);
size_t rb_gc_impl_gc_count(void *objspace_ptr);
VALUE rb_gc_impl_latest_gc_info(void *objspace_ptr, VALUE key);
VALUE rb_gc_impl_stat(void *objspace_ptr, VALUE hash_or_sym);
VALUE rb_gc_impl_stat_heap(void *objspace_ptr, VALUE heap_name, VALUE hash_or_sym);
const char *rb_gc_impl_active_gc_name(void);
```

#### Miscellaneous
```c
struct rb_gc_object_metadata_entry *rb_gc_impl_object_metadata(void *objspace_ptr,
                                                               VALUE obj);
bool rb_gc_impl_pointer_to_heap_p(void *objspace_ptr, const void *ptr);
bool rb_gc_impl_garbage_object_p(void *objspace_ptr, VALUE obj);
void rb_gc_impl_set_event_hook(void *objspace_ptr, const rb_event_flag_t event);
void rb_gc_impl_copy_attributes(void *objspace_ptr, VALUE dest, VALUE obj);
```

### Helper Functions (gc/gc.h)

GC implementations can call these Ruby VM helpers:

```c
// Locking
unsigned int rb_gc_vm_lock(const char *file, int line);
void rb_gc_vm_unlock(unsigned int lev, const char *file, int line);
unsigned int rb_gc_cr_lock(const char *file, int line);
void rb_gc_cr_unlock(unsigned int lev, const char *file, int line);
void rb_gc_vm_barrier(void);

// Object traversal
void rb_gc_mark_children(void *objspace, VALUE obj);
void rb_gc_update_object_references(void *objspace, VALUE obj);
void rb_gc_update_vm_references(void *objspace);

// Weak tables
void rb_gc_vm_weak_table_foreach(vm_table_foreach_callback_func callback,
                                  vm_table_update_callback_func update_callback,
                                  void *data, bool weak_only,
                                  enum rb_gc_vm_weak_tables table);

// Events and utilities
void rb_gc_event_hook(VALUE obj, rb_event_flag_t event);
void *rb_gc_get_objspace(void);
void rb_gc_run_obj_finalizer(VALUE objid, long count,
                             VALUE (*callback)(long i, void *data), void *data);
bool rb_gc_obj_free(void *objspace, VALUE obj);
void rb_gc_save_machine_context(void);
void rb_gc_mark_roots(void *objspace, const char **categoryp);
void rb_gc_handle_weak_references(VALUE obj);

// Modular GC specific
bool rb_gc_event_hook_required_p(rb_event_flag_t event);
void *rb_gc_get_ractor_newobj_cache(void);
void rb_gc_initialize_vm_context(struct rb_gc_vm_context *context);
void rb_gc_worker_thread_set_vm_context(struct rb_gc_vm_context *context);
void rb_gc_worker_thread_unset_vm_context(struct rb_gc_vm_context *context);
void rb_gc_move_obj_during_marking(VALUE from, VALUE to);
```

### Existing Implementations

#### Default GC (`gc/default/default.c`)

The default GC is a mark-sweep-compact collector with:
- Multiple size-class heaps (HEAP_COUNT = 5)
- Per-ractor allocation caches
- Generational collection (RGenGC)
- Incremental marking
- Lazy sweeping
- Compaction support

Key structures:
- `rb_objspace`: Global GC state
- `rb_ractor_newobj_cache_t`: Per-ractor allocation cache
- `heap_page`: Memory page management
- `gc_profile_record`: Statistics tracking

#### MMTk GC (`gc/mmtk/mmtk.c`)

The MMTk implementation demonstrates how to integrate an external GC framework:
- Rust-based MMTk core with C bindings (`mmtk.h`)
- Stop-the-world coordination via pthread primitives
- Upcalls for VM integration (mark roots, scan objects, etc.)
- Bump pointer allocation
- Finalizer job queues

Key structures:
```c
struct objspace {
    bool measure_gc_time;
    bool gc_stress;
    size_t gc_count;
    size_t total_gc_time;
    size_t total_allocated_objects;
    st_table *finalizer_table;
    struct MMTk_final_job *finalizer_jobs;
    rb_postponed_job_handle_t finalizer_postponed_job;
    struct ccan_list_head ractor_caches;
    unsigned long live_ractor_cache_count;
    pthread_mutex_t mutex;
    rb_atomic_t mutator_blocking_count;
    bool world_stopped;
    pthread_cond_t cond_world_stopped;
    pthread_cond_t cond_world_started;
    struct rb_gc_vm_context vm_context;
};

struct MMTk_ractor_cache {
    struct ccan_list_node list_node;
    MMTk_Mutator *mutator;
    bool gc_mutator_p;
    MMTk_BumpPointer *bump_pointer;
};
```

---

## Build System Integration

### Configuring Ruby with Modular GC Support

```bash
./configure --with-modular-gc=/path/to/gc/libraries
make
```

### Building a GC Module

```bash
make install-modular-gc MODULAR_GC=<name>
```

This builds the GC shared library and installs it to the configured directory.

### Running with a Modular GC

```bash
RUBY_GC_LIBRARY=<name> ruby script.rb
```

### Creating a New GC Module

1. Create directory `gc/<name>/`

2. Create `gc/<name>/extconf.rb`:
```ruby
# frozen_string_literal: true

require_relative "../extconf_base"

# Add any module-specific configuration here
# $CFLAGS << " -DIMMIX_DEBUG"

create_gc_makefile("immix")
```

3. Create `gc/<name>/<name>.c` implementing all `rb_gc_impl_*` functions

4. The `extconf_base.rb` automatically:
   - Sets include path to `gc/` directory
   - Defines `BUILDING_MODULAR_GC` preprocessor macro
   - Adds `-fPIC` compiler flag
   - Creates makefile for `librubygc.<name>.so`

---

## Immix Algorithm Overview

Immix is a mark-region garbage collector designed for high throughput and low fragmentation. It was introduced in the paper "Immix: A Mark-Region Garbage Collector with Space Efficiency, Fast Collection, and Mutator Performance" (Blackburn & McKinley, PLDI 2008).

### Heap Organization

```
+------------------+------------------+------------------+
|     Block 0      |     Block 1      |     Block 2      |
|     (32 KB)      |     (32 KB)      |     (32 KB)      |
+------------------+------------------+------------------+
         |
         v
+---+---+---+---+---+---+---+---+---+---+
| L | L | L | L | L | L | L | L |...| L |  (256 lines per block)
| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |   |255|  (128 bytes each)
+---+---+---+---+---+---+---+---+---+---+
```

- **Blocks**: 32 KB contiguous memory regions
- **Lines**: 128 byte subdivisions within blocks (256 lines per block)
- **Objects**: May span multiple lines but never cross block boundaries

### Block States

| State | Description |
|-------|-------------|
| Free | No live objects, available for allocation |
| Recyclable | Contains holes (free lines), can be reused |
| Unavailable | Fully occupied, cannot allocate |

### Line Metadata

Each line has a 1-byte mark state:
- `0x00`: Unmarked/free
- `0x01`: Marked (contains live object start)
- `0x02`: Conservatively marked (may contain live data)

### Allocation Strategy

1. **Thread-Local Allocators (TLAs)**
   - Each ractor gets a bump-pointer allocator
   - Cursor/limit pair for fast allocation
   - Allocates within "holes" (contiguous free lines)

2. **Hole Scanning**
   - When current hole exhausted, scan line map for next hole
   - Prefer recyclable blocks over fresh blocks

3. **Overflow Path**
   - Medium objects (>128 bytes) that don't fit current hole
   - Allocate from fresh blocks to avoid wasting hole tails

4. **Large Object Space (LOS)**
   - Objects >= 8 KB bypass Immix entirely
   - Use Ruby's malloc hooks for LOS management

### Collection Phases

#### Mark Phase
1. Stop the world
2. Mark roots via `rb_gc_mark_roots`
3. Trace object graph, setting:
   - Per-object mark bits (for graph traversal termination)
   - Per-line mark bytes (for sweep efficiency)

#### Sweep Phase
1. Scan all blocks
2. For each block:
   - Count marked lines
   - Classify as free/recyclable/unavailable
   - Build histograms of live bytes and hole counts
3. Reset unmarked lines

#### Opportunistic Evacuation
Triggered when fragmentation is high:

1. **Reserve Headroom**: ~2.5% of heap for copying
2. **Select Candidates**: Rank blocks by fragmentation (hole count)
3. **Copy Live Objects**: During marking, copy objects from fragmented blocks
4. **Update References**: Use `rb_gc_move_obj_during_marking`
5. **Honor Pins**: Objects with pin bit stay in place

### Object Header Requirements

Immix requires these per-object header bits:
- **Mark bit**: Has object been visited this cycle?
- **Forwarded bit**: Has object been evacuated?
- **Forwarding address**: Where was object moved to?
- **Pin bit**: Is object immovable?
- **Size class bit**: Small (<128B) vs medium object?

Ruby's existing object header (`flags` field) can accommodate these.

### Metadata Overhead

| Metadata | Size | Overhead |
|----------|------|----------|
| Line marks | 1 byte per 128 bytes | 0.78% |
| Block state | 4 bytes per 32 KB | 0.01% |
| **Total** | | **~0.8%** |

### Write Barriers

Immix itself does not require card tables. It relies on:
- Ruby's existing write barrier hooks
- Object-remembering barriers for generational variants
- Header bits for tracking dirty state

---

## Implementation Phases

Each phase has a corresponding `phase-N-<name>-prd.json` file with detailed task breakdowns.

### Phase 1: Module Scaffolding & Build Integration

**Goal**: Create the Immix plugin skeleton that builds and loads correctly.

**Deliverables**:
- `gc/immix/` directory structure
- `gc/immix/extconf.rb` build configuration
- `gc/immix/immix.c` with stubbed API implementations
- Updated documentation

**Success Criteria**:
- `make install-modular-gc MODULAR_GC=immix` succeeds
- `RUBY_GC_LIBRARY=immix ruby -e "puts GC.stat"` runs without crash

### Phase 2: Objspace Structure & Allocation Metadata

**Goal**: Define core data structures for Immix heap management.

**Deliverables**:
- `struct immix_block` definition
- `struct immix_line_map` definition
- `struct immix_objspace` with all required fields
- Per-ractor cache structures
- Histogram data structures

**Success Criteria**:
- Objspace initializes without errors
- Ractor caches can be allocated and freed
- Memory accounting is accurate

### Phase 3: Allocation Fast Paths & Large Object Handling

**Goal**: Implement efficient object allocation.

**Deliverables**:
- Bump-pointer allocation in holes
- Hole scanning logic
- Overflow allocator for medium objects
- LOS integration via malloc hooks
- Size class determination

**Success Criteria**:
- Objects allocate correctly
- Allocation is reasonably fast (benchmark against default)
- LOS objects are tracked properly

### Phase 4: GC Cycle (Mark, Sweep, Evacuation)

**Goal**: Implement the complete collection cycle.

**Deliverables**:
- Stop-the-world coordination
- Root marking integration
- Object graph traversal with line marking
- Block classification sweep
- Histogram building
- Opportunistic evacuation
- Reference updating

**Success Criteria**:
- GC completes without crashes
- Memory is reclaimed correctly
- Evacuation reduces fragmentation
- No dangling pointers

### Phase 5: Runtime Integration

**Goal**: Integrate with Ruby's weak references, finalizers, and barriers.

**Deliverables**:
- Weak reference tracking
- Finalizer queue management
- Write barrier hooks
- Fork handling
- Ractor lifecycle management

**Success Criteria**:
- Finalizers run correctly
- Weak references are cleared appropriately
- Write barriers maintain invariants
- Fork doesn't corrupt state

### Phase 6: Diagnostics & Configuration

**Goal**: Expose statistics and tuning options.

**Deliverables**:
- `GC.stat` integration with Immix metrics
- `GC.config` support for tunables
- Event hooks for profiling
- Debug logging infrastructure

**Success Criteria**:
- Stats are accurate and useful
- Configuration changes take effect
- Events fire at appropriate times

### Phase 7: Validation & Testing

**Goal**: Ensure correctness and stability.

**Deliverables**:
- Bootstrap tests for Immix
- Stress tests for fragmentation
- CI integration
- Documentation for manual testing

**Success Criteria**:
- All tests pass
- No regressions vs default GC
- CI provides useful feedback

---

## Risks and Mitigations

### High-Severity Risks

#### 1. Evacuation Conflicts with Ruby's Compactor

**Risk**: Ruby has an existing compaction mechanism. Immix's opportunistic evacuation may conflict, causing double-moves or corrupted references.

**Mitigations**:
- Respect `rb_gc_impl_register_pinning_obj` absolutely
- Check `RUBY_FL_FINALIZE` and `FL_EXIVAR` before moving
- Provide config option to disable Immix evacuation entirely
- Test extensively with `GC.compact` calls

#### 2. Ractor Metadata Contention

**Risk**: Multiple ractors accessing shared line maps and histograms causes lock contention, destroying parallelism benefits.

**Mitigations**:
- Use thread-local accumulation buffers merged at sync points
- Store line marks as bytes (atomic access without locks)
- Only lock global block pool, not per-line operations
- Profile contention with debug counters

#### 3. Large Object Accounting Divergence

**Risk**: LOS allocations via `rb_gc_impl_malloc` may not be tracked consistently, causing incorrect `GC.stat` output or premature OOM.

**Mitigations**:
- Always call `rb_gc_impl_adjust_memory_usage` for LOS
- Expose separate LOS stats in `rb_gc_impl_stat`
- Add regression tests comparing Immix vs default GC memory reports
- Validate with memory profilers (Valgrind, AddressSanitizer)

### Medium-Severity Risks

#### 4. Weak Reference Staleness After Evacuation

**Risk**: Weak references may hold pointers to evacuated objects without updates, causing use-after-move bugs.

**Mitigations**:
- Call `rb_gc_update_object_references` immediately after moving
- Re-walk finalizer tables post-evacuation
- Assert that weak refs only store `rb_gc_location(obj)` results
- Add targeted weak-ref + evacuation tests

#### 5. Line Scanning Performance

**Risk**: Scanning line maps for holes may become O(n) per allocation under high fragmentation.

**Mitigations**:
- Cache last-known hole position per block
- Skip fully-marked regions using word-at-a-time scanning
- Deprioritize highly fragmented blocks in recyclable queue
- Benchmark against default GC allocation paths

#### 6. Histogram Accuracy

**Risk**: Inaccurate histograms lead to poor evacuation decisions (over-copying or under-copying).

**Mitigations**:
- Rebuild histograms completely each cycle
- Use conservative estimates when uncertain
- Log histogram data under debug mode for analysis
- Compare evacuation effectiveness across workloads

### Low-Severity Risks

#### 7. Statistics Incompatibility

**Risk**: Different stat names or semantics confuse tooling that expects default GC output.

**Mitigations**:
- Mirror default GC key names where applicable
- Document Immix-specific keys clearly
- Return `nil` for unsupported keys (not errors)
- Add compatibility tests against common GC introspection patterns

#### 8. Build System Edge Cases

**Risk**: Unusual platforms or configurations may fail to build Immix.

**Mitigations**:
- Reuse `extconf_base.rb` completely
- Test on Linux, macOS, and (if possible) Windows
- Add CI jobs for multiple Ruby versions
- Document known limitations

---

## References

### Papers

1. Blackburn, S. M., & McKinley, K. S. (2008). **Immix: A Mark-Region Garbage Collector with Space Efficiency, Fast Collection, and Mutator Performance**. PLDI 2008.
   - Original Immix paper describing the algorithm
   - https://www.steveblackburn.org/pubs/papers/immix-pldi-2008.pdf

### Ruby Source Files

- `gc/gc_impl.h` - Modular GC API definition
- `gc/gc.h` - Helper functions for GC implementations
- `gc/extconf_base.rb` - Shared build configuration
- `gc/README.md` - Official modular GC documentation
- `gc/default/default.c` - Default GC implementation
- `gc/mmtk/mmtk.c` - MMTk GC implementation (reference for integration patterns)

### Ruby Feature

- [Feature #20470](https://bugs.ruby-lang.org/issues/20470) - Modular GC proposal and discussion
