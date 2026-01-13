# Immix GC: Comparison with Original Paper

This document compares our Ruby Immix implementation against the original paper:
"Immix: A Mark-Region Garbage Collector with Space Efficiency, Fast Collection, and Mutator Performance" (Blackburn & McKinley, PLDI 2008).

## Overview

The original Immix is a **mark-region** collector that combines the locality benefits of copying collection with the efficiency of mark-sweep. Our implementation follows the core design but makes several adaptations for Ruby's object model and C extension compatibility.

## Core Design Elements: What We Kept

### 1. Block and Line Structure ✓

| Parameter | Paper | Our Implementation |
|-----------|-------|-------------------|
| Block size | 32KB typical | 64KB |
| Line size | 128 bytes typical | 256 bytes |
| Lines per block | 256 | 256 |

We use larger blocks and lines to better match Ruby's object sizes and reduce metadata overhead per allocation. The paper notes these are tunable parameters.

### 2. Line Marking ✓

The paper's key insight: mark at **line granularity** for reclamation decisions. We implement this:
- `line_marks[256]` array per block
- Lines marked as `FREE` or `MARKED`
- Sweep determines line state based on whether any live object touches that line

### 3. Block States ✓

```c
enum immix_block_state {
    IMMIX_BLOCK_FREE = 0,      // All lines free
    IMMIX_BLOCK_RECYCLABLE,    // Some free lines (has holes)
    IMMIX_BLOCK_UNAVAILABLE    // No free lines
};
```

### 4. Opportunistic Hole Reuse ✓

The paper describes finding "holes" (contiguous free lines) for bump-pointer allocation within recyclable blocks. We implement this in `immix_find_next_hole()` and `immix_cache_refill()`.

---

## Major Deviations from Paper

### 1. No Object Movement/Evacuation ⚠️

**Paper**: Immix supports **opportunistic evacuation** - when fragmentation is high, it copies live objects from sparsely-populated blocks to fresh blocks during marking.

**Our Implementation**: Objects are **never relocated**. Pure mark-sweep.

**Why**: Ruby has pervasive **interior pointers** and **C extensions** that hold raw `VALUE` pointers. The Ruby C API allows C code to store object references anywhere - in global variables, structs, thread-local storage, etc. Moving objects would require:
- A read barrier or forwarding pointer check on every object access
- Updating all references (including those in C extension memory we don't control)
- A pinning mechanism for objects referenced from C code

This would fundamentally change Ruby's memory model and break every existing C extension.

**Impact**: Without evacuation, fragmentation can accumulate over time. Mitigation strategies for future work include:
- Incremental compaction during idle time
- Ractor-local evacuation (Ractors with no C extensions)
- Best-fit allocation heuristics

### 2. Per-Object Mark Bits ⚠️

**Paper**: Immix uses line marks only; individual object liveness is inferred during marking by examining object headers.

**Our Implementation**: We maintain a separate `mark_bits[1024]` bitmap per block (one bit per 8-byte slot).

**Why**: Ruby needs to identify individual live objects during sweep to:
- Call finalizers via `ObjectSpace.define_finalizer`
- Free object resources via `rb_gc_obj_free`
- Track weak references (`WeakRef`, `WeakMap`)
- Support `ObjectSpace.each_object`
- Clear `T_DATA` free functions correctly

Without per-object tracking, we'd need to re-walk all objects to determine individual liveness.

### 3. Size Prefix Per Object ⚠️

**Paper**: Assumes objects are self-describing (size determinable from object header).

**Our Implementation**: Each allocation includes an 8-byte size prefix:
```
Memory layout:
[size: 8 bytes][flags: 8 bytes][klass: 8 bytes][data...]
      ^                ^
 size prefix      object start (VALUE points here)
```

**Why**: Ruby objects don't encode allocated size in a recoverable way. The `flags` field encodes type, but actual allocation size depends on:
- Embedded vs heap-allocated string/array data
- Variable-length data payloads
- Internal VM padding requirements

Storing explicit size simplifies:
- Sweep iteration (know how far to skip)
- `rb_gc_impl_obj_slot_size()` API requirement
- Debugging and validation

### 4. Allocation Bitmap ⚠️

**Paper**: Does not describe an allocation bitmap.

**Our Implementation**: `alloc_map[1024]` bitmap tracks which slots contain valid object starts.

**Why**:
- Enables O(1) `pointer_to_heap_p()` checks
- Required for conservative stack scanning (Ruby scans C stack for potential object pointers)
- Supports sweep iteration without parsing potentially-corrupt object headers
- Distinguishes "this address has an object" from "this address is inside an object"

### 5. No Large Object Space ⚠️

**Paper**: Objects >= 8KB bypass Immix and use a separate "Large Object Space" with page-level management.

**Our Implementation**: Maximum object size is 640 bytes. Larger allocations crash.

**Why**: This is a known limitation. Ruby's default GC handles large objects separately. A proper implementation would:
- Route large allocations to `malloc()`
- Track them in a separate list
- Mark/sweep them alongside Immix blocks

**Status**: TODO for Phase 8.

### 6. Block Registry for Pointer Validation ⚠️

**Paper**: Does not describe pointer validation mechanism.

**Our Implementation**: `struct immix_block_registry` maintains a sorted array of allocated blocks for O(log n) binary search lookup.

**Why**: Ruby's conservative stack scanning must quickly determine if an arbitrary machine word could be a valid heap pointer. This is called frequently during:
- Stack scanning (every GC)
- `rb_gc_impl_pointer_to_heap_p()` checks
- Debug/validation code

Without a registry, we'd need O(n) block list traversal.

---

## Ruby-Specific Additions

### 1. Weak Reference Handling

```c
struct immix_weak_refs {
    uintptr_t *buffer;
    size_t size;
    size_t capacity;
};
```

Objects with `RUBY_FL_WEAK_REFERENCE` are collected during marking and processed via `rb_gc_handle_weak_references()` after mark phase completes. This supports:
- `WeakRef` standard library
- `ObjectSpace::WeakMap`
- Internal VM weak tables

### 2. Finalizer Integration

```c
st_foreach(objspace->finalizer_table, immix_pin_finalizer_value, ...);
```

Finalizers registered via `ObjectSpace.define_finalizer` must keep their proc/block alive. The finalizer table is walked during root marking.

### 3. Multi-Line Object Fix

The original code had a bug where small objects near line boundaries wouldn't mark all lines they span:

```c
// Bug: Only marked extra lines for large objects
if (size > IMMIX_LINE_SIZE) { ... }

// Fix: Check actual end line regardless of size
size_t end_line = immix_line_index((void *)((uintptr_t)obj + size - 1));
if (end_line > line_idx) {
    for (size_t i = line_idx + 1; i <= end_line; i++) {
        block->line_marks[i] = IMMIX_LINE_MARKED;
    }
}
```

This was a critical correctness bug causing memory corruption.

### 4. Ractor-Aware Caching

```c
struct immix_ractor_cache {
    struct immix_block *current_block;
    char *cursor;
    char *limit;
    size_t current_line;
    ...
};
```

Each Ruby Ractor gets its own allocation cache for thread-safe bump-pointer allocation without locking on the fast path. Global block pools are protected by mutex.

---

## Summary Comparison Table

| Feature | Original Immix | Our Implementation | Reason |
|---------|---------------|-------------------|--------|
| Block size | 32KB | 64KB | Larger Ruby objects |
| Line size | 128B | 256B | Alignment, metadata |
| Object movement | Yes (evacuation) | No | C extension compatibility |
| Line marks | Yes | Yes | Core design |
| Object mark bits | No | Yes | Finalizers, weak refs |
| Size prefix | No | Yes | Size not recoverable |
| Alloc bitmap | No | Yes | Conservative scanning |
| Large objects | Separate LOS | Not implemented | TODO |
| Defragmentation | Evacuation | None | No movement |
| Hole finding | Yes | Yes | Core design |
| Block registry | No | Yes | Pointer validation |

---

## Performance Implications

### Advantages of Our Approach
1. **Simpler implementation**: No forwarding pointers, no reference updating
2. **C extension compatibility**: No changes needed to existing extensions
3. **Predictable pause times**: No copying overhead during GC

### Disadvantages
1. **Fragmentation**: Without evacuation, holes cannot be consolidated
2. **Memory overhead**: Extra bitmaps (alloc_map, mark_bits) add ~0.4% overhead
3. **No locality improvement**: Can't relocate objects for better cache behavior

---

## Future Work

### Short-term
1. **Large Object Space**: Proper handling for objects > 640 bytes
2. **Parallel marking**: Multi-threaded mark phase
3. **Incremental marking**: Reduce pause times for large heaps

### Long-term (Requires Ruby Changes)
1. **Ractor-local evacuation**: Ractors without C extension access could support moving GC
2. **Generational Immix**: The paper describes generational variants
3. **Concurrent marking**: Requires write barrier changes

---

## References

1. Blackburn, S. M., & McKinley, K. S. (2008). "Immix: A Mark-Region Garbage Collector with Space Efficiency, Fast Collection, and Mutator Performance." PLDI 2008.

2. Ruby Feature #20470: Modular GC proposal

3. Immix-Rust reference implementation: https://github.com/ptersilie/rust-immix
