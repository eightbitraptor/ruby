# Immix GC Implementation Status

**Last Updated**: January 2026

## Current Status: Phase 7 Complete (Validation & Testing)

The Immix GC is a **functional mark-sweep collector** that passes **all 2037 bootstrap tests**, including all 153 Ractor tests. Key fixes in this phase: shutdown finalizers, `ObjectSpace._id2ref`, and Ractor+fork deadlock.

## Completed Milestones

### Phase 1: Module Scaffolding ✅
- Created `gc/immix/` directory structure
- Implemented `extconf.rb` build configuration
- Stubbed all `rb_gc_impl_*` functions
- Build integration working: `make install-modular-gc MODULAR_GC=immix`

### Phase 2: Data Structures ✅
- `struct immix_block` with 64KB blocks, 256-byte lines
- Per-block metadata: `line_marks[256]`, `alloc_map[1024]`, `mark_bits[1024]`
- `struct immix_objspace` with block lists (free, usable, full)
- Per-ractor allocation caches with cursor/limit bump pointers
- Block registry for O(log n) pointer validation

### Phase 3: Allocation ✅
- Bump-pointer allocation within holes
- Size prefix pattern: `[size][flags][klass][data...]`
- Hole finding via linear line mark scanning
- Block recycling: usable blocks → full blocks → free blocks
- Memory zeroing for clean allocation state

### Phase 4: GC Cycle ✅
- Stop-the-world mark phase
- Iterative marking via explicit mark stack
- Line marking for all lines an object spans
- Sweep phase with object freeing via `rb_gc_obj_free()`
- Block reclassification based on free line count

### Phase 5: Runtime Integration ✅
- Weak reference tracking and processing
- Finalizer table integration
- Proper `pointer_to_heap_p` with block registry
- `garbage_object_p` returns correct status during GC

## Key Bug Fixes

### Multi-Line Object Marking (Critical)
**Problem**: Objects near line boundaries that span into adjacent lines only marked their starting line, causing the next line to be considered "free" and allocated over.

**Root Cause**: The original code only marked additional lines if `size > IMMIX_LINE_SIZE`. But a 40-byte object starting at offset 240 in a line (line boundary at 256) spans two lines.

**Fix**:
```c
// Before (bug):
if (size > IMMIX_LINE_SIZE) { ... mark extra lines ... }

// After (fixed):
size_t end_line = immix_line_index((void *)((uintptr_t)obj + size - 1));
if (end_line > line_idx) { ... mark extra lines ... }
```

This was a **critical correctness bug** that caused memory corruption and crashes.

## Deviations from Original Immix Paper

See `IMMIX_PAPER_COMPARISON.md` for detailed analysis. Key differences:

| Feature | Paper | Our Implementation | Reason |
|---------|-------|-------------------|--------|
| Object Movement | Yes (evacuation) | No | C extension compatibility |
| Per-Object Marks | No | Yes (`mark_bits`) | Finalizers, weak refs |
| Size Storage | In object header | Prefix word | Ruby objects vary |
| Allocation Bitmap | No | Yes (`alloc_map`) | Conservative scanning |
| Large Objects | Separate LOS | Not implemented | TODO |

## Test Results

```bash
# Basic stress test - PASSES
RUBY_GC_LIBRARY=immix ./miniruby -e '
100.times do |i|
  10000.times { "x" * 100 }
  GC.start
end
puts "Success!"
'

# Mixed object types - PASSES
RUBY_GC_LIBRARY=immix ./miniruby -e '
50.times do
  1000.times { "x" * rand(10..500) }
  1000.times { [1, 2, 3, "string", :symbol] }
  1000.times { {key: "value", num: 42} }
  GC.start
end
puts "Success!"
'

# Long-lived objects - PASSES
RUBY_GC_LIBRARY=immix ./miniruby -e '
persistent = []
50.times do |i|
  persistent << "persistent_#{i}" if i % 5 == 0
  5000.times { "garbage" }
  GC.start
end
puts "Success: #{persistent.size} persistent objects"
'

# Statistics after 100 GC cycles:
# {count: 100, time: 26, total_allocated_objects: 2008376,
#  total_freed_objects: 2001088, heap_total_bytes: 2818048,
#  heap_used_bytes: 501248, heap_free_bytes: 2316800,
#  total_blocks: 43, free_blocks: 32, recyclable_blocks: 5, full_blocks: 5}
```

### Phase 6: Diagnostics & Configuration ✅
- Comprehensive `GC.stat` with Immix-specific metrics:
  - `count`, `time` (in ms), `total_allocated_objects`, `total_freed_objects`
  - `heap_total_bytes`, `heap_used_bytes`, `heap_free_bytes`
  - `total_blocks`, `free_blocks`, `recyclable_blocks`, `full_blocks`
- Enhanced `GC.config` with fragmentation statistics:
  - `recyclable_holes`, `recyclable_free_lines`, `avg_hole_size_lines`
  - Implementation info: `block_size`, `line_size`, `max_object_size`
- GC timing infrastructure with nanosecond precision
- Fixed shutdown crash with finalizers (use alloc_map for scanning)

### Phase 7: Validation & Testing (In Progress)
- [x] Bootstrap tests: **2037/2037 pass** (all tests including Ractor)
- [x] Shutdown finalizers: Fixed IO flushing by calling zombie `dfree` at shutdown
- [x] `ObjectSpace._id2ref`: Implemented `rb_gc_impl_each_object`
- [x] `ObjectSpace.each_object`: Fixed callback parameters
- [x] Ractor + fork: Fixed deadlock by implementing proper fork handlers
- [x] WeakRef/WeakMap: Implement `rb_gc_impl_declare_weak_references` to set RUBY_FL_WEAK_REFERENCE flag
- [x] **ObjectSpace iteration**: Fixed crash in `ObjectSpace.count_objects_size` with collect-then-iterate approach
- [ ] Run Ruby's full test suite
- [ ] Benchmark against default GC
- [ ] Memory profiling (Valgrind, ASan)

## Remaining Work

### Phase 8: Large Object Space (Optional)
- [ ] Separate handling for objects > 640 bytes
- [ ] Page-level allocation for large objects

### Phase 9: Evacuation/Compaction (Optional)
- [ ] Opportunistic copying for defragmentation
- [ ] Forwarding pointer support
- [ ] Pin bit handling

## Known Limitations

1. **Maximum object size**: 640 bytes. Larger allocations crash.
2. **No compaction**: Fragmentation can accumulate over time.
3. **Single-threaded GC**: Stop-the-world, no parallel marking.
4. **No generational collection**: Full heap traced every cycle.

## Build & Test Commands

```bash
# Build
cd /path/to/ruby
make install-modular-gc MODULAR_GC=immix

# Run
RUBY_GC_LIBRARY=immix ./miniruby -e 'puts GC.stat'

# Test
RUBY_GC_LIBRARY=immix ./miniruby test_script.rb
```

## Files

| File | Description |
|------|-------------|
| `gc/immix/immix.c` | Main implementation (~1690 lines) |
| `gc/immix/immix.h` | Data structures and inline helpers (~200 lines) |
| `gc/immix/extconf.rb` | Build configuration |
| `gc/immix/STATUS.md` | This file |
| `gc/immix/IMMIX_PAPER_COMPARISON.md` | Comparison with original paper |
