# Immix Implementation Plan - Critical Review

After examining the Immix-Rust reference implementation, the original PLDI 2008 paper, Ruby's default GC, and the MMTk integration, this document captures corrections, gaps, and refinements to the original plan.

## Critical Issues Identified

### 1. Block/Line Size Constants Are Wrong

**Original Plan**: Stated 32KB blocks, 128B lines (256 lines per block)

**Immix-Rust Reference**:
```rust
pub const LOG_BYTES_IN_LINE  : usize = 8;   // 256 bytes per line
pub const BYTES_IN_LINE      : usize = 256;
pub const LOG_BYTES_IN_BLOCK : usize = 16;  // 64KB blocks
pub const BYTES_IN_BLOCK     : usize = 65536;
pub const LINES_IN_BLOCK     : usize = 256; // 64KB / 256B = 256 lines
```

**Paper (Section 3.3)**: "Each block contains 256 lines of 256 bytes each" - that's 64KB blocks, not 32KB.

**Correction**: Update all constants:
- `IMMIX_BLOCK_SIZE` = 64KB (65536 bytes)
- `IMMIX_LINE_SIZE` = 256 bytes (not 128)
- `IMMIX_LINES_PER_BLOCK` = 256

This changes metadata overhead calculations - still ~1% but for different sizes.

### 2. Line Mark States Are Incomplete

**Original Plan**: Listed only UNMARKED, MARKED, CONSERVATIVELY_MARKED

**Immix-Rust Reference**:
```rust
pub enum LineMark {
    Free,
    Live,
    FreshAlloc,    // Missing from my plan
    ConservLive,
    PrevLive       // Missing from my plan
}
```

**Correction**: Need five states:
- `Free` - Line is available for allocation
- `Live` - Line contains start of live object
- `FreshAlloc` - Line was freshly allocated this cycle (not yet marked)
- `ConservLive` - Line is conservatively live (follows a Live line)
- `PrevLive` - Previous cycle liveness (for sticky-mark generational)

The `FreshAlloc` state is crucial - it distinguishes newly allocated lines from free lines during concurrent marking.

### 3. Object Size Storage Pattern Missed

**MMTk Implementation** (mmtk.c:791-799):
```c
alloc_obj = mmtk_alloc(...);
alloc_obj++;
alloc_obj[-1] = alloc_size - sizeof(VALUE);  // Size stored BEFORE object
alloc_obj[0] = flags;
alloc_obj[1] = klass;
```

**Critical**: MMTk stores the object size in a hidden word **before** the object pointer. This is how `rb_gc_impl_obj_slot_size` works:
```c
size_t rb_gc_impl_obj_slot_size(VALUE obj) {
    return ((VALUE *)obj)[-1];
}
```

**Correction**: Immix must adopt the same pattern. Each allocation needs an extra `sizeof(VALUE)` prefix to store the slot size. This affects:
- Allocation size calculations
- Object header layout
- Pointer-to-heap validation
- Line mark calculations (object starts at `ptr`, not `ptr - sizeof(VALUE)`)

### 4. Conservative Line Marking Algorithm

**Immix-Rust Reference** (immix_space.rs:67-75):
```rust
pub fn mark_line_live(&self, addr: Address) {
    let line_table_index = addr.diff(self.space_start) >> immix::LOG_BYTES_IN_LINE;
    self.set(line_table_index, immix::LineMark::Live);
    
    // Mark NEXT line as conservatively live
    if line_table_index < self.len - 1 {
        self.set(line_table_index + 1, immix::LineMark::ConservLive);
    }
}
```

**Insight**: When marking an object, Immix marks:
1. The line containing the object's start as `Live`
2. The **next** line as `ConservLive` (the object might span)

This is simpler than I described - no need to calculate exact object span for small objects. The conservative approach wastes at most one line per object.

### 5. Block State Machine Simpler Than Described

**Immix-Rust Reference**:
```rust
pub enum BlockMark {
    Usable,  // Has free lines
    Full     // No free lines
}
```

**Paper mentions three states** but the Rust implementation uses only two. The key insight:
- After sweep, blocks with ANY free lines become `Usable`
- Blocks with zero free lines become `Full`
- "Free" blocks (completely empty) are just `Usable` with all lines free

**Correction**: Simplify block states to match Rust implementation. The "recyclable vs free" distinction is handled by checking if any lines are marked, not by separate states.

### 6. Missing Allocation Map

**Immix-Rust** has two separate maps:
```rust
pub alloc_map: Arc<AddressMap<u8>>,  // Set at allocation time
pub trace_map: Arc<AddressMap<u8>>,  // Set during tracing
```

The `alloc_map` records which addresses contain object starts. This is essential for:
- `rb_gc_impl_pointer_to_heap_p` - validate object pointers
- Conservative stack scanning - distinguish objects from random values
- Heap walking - find all objects

**Correction**: Add allocation bitmap/map. Each slot needs a bit indicating "this is an object start." This is separate from line marks.

### 7. Hole Finding Algorithm

**Immix-Rust** (immix_space.rs:241-260):
```rust
pub fn get_next_available_line(&self, cur_line: usize) -> Option<usize> {
    let mut i = cur_line;
    while i < self.line_mark_table.len {
        match self.line_mark_table.get(i) {
            LineMark::Free => return Some(i),
            _ => i += 1,
        }
    }
    None
}

pub fn get_next_unavailable_line(&self, cur_line: usize) -> usize {
    let mut i = cur_line;
    while i < self.line_mark_table.len {
        match self.line_mark_table.get(i) {
            LineMark::Free => i += 1,
            _ => return i,
        }
    }
    i
}
```

**Insight**: Hole finding is simple linear scan. The mutator:
1. Finds first free line (`get_next_available_line`)
2. Finds first non-free line after that (`get_next_unavailable_line`)
3. Sets cursor/limit to that range
4. Marks all lines in range as `FreshAlloc`

### 8. No Opportunistic Evacuation in Reference

**Critical Observation**: The Immix-Rust reference implementation does NOT implement opportunistic evacuation/defragmentation. It's pure mark-region.

**Implications**:
- Evacuation is an optimization, not core algorithm
- We should implement basic mark-region first
- Add evacuation as Phase 4b or Phase 8
- This significantly reduces initial complexity

### 9. Ruby-Specific Object Layout

**Default GC** (default.c:676):
```c
#define BASE_SLOT_SIZE (sizeof(struct RBasic) + sizeof(VALUE[RBIMPL_RVALUE_EMBED_LEN_MAX]) + RVALUE_OVERHEAD)
```

**MMTk** (mmtk.c:587-591):
```c
static size_t heap_sizes[MMTK_HEAP_COUNT + 1] = {
    32, 40, 80, 160, 320, MMTK_MAX_OBJ_SIZE, 0
};
```

**Insight**: Ruby objects have variable sizes based on type. The GC doesn't control object layout - it just allocates requested sizes. Immix needs to handle arbitrary sizes up to block size (minus a line for headers).

### 10. Testing Strategy Gaps

**Critical Missing Tests**:
1. **Object layout verification**: Ensure klass/flags are at correct offsets
2. **Size roundup validation**: MMTk rounds up to size classes - we may need similar
3. **Alignment**: Ruby objects need specific alignment (8 bytes minimum)
4. **T_DATA with dfree**: These objects call back into C - critical for finalizers
5. **Ractor isolation**: Objects must not leak between ractors
6. **JIT code**: YJIT/ZJIT may have special requirements

## Revised Implementation Order

Based on these findings, I recommend:

### Phase 1: Scaffolding (unchanged)
- Build system integration
- Stub implementations

### Phase 2: Data Structures (revised)
- Correct block size (64KB) and line size (256B)
- Add allocation map (not just line marks)
- Implement size-in-header pattern from MMTk
- Simplify block states to Usable/Full

### Phase 3: Allocation (revised)
- Implement bump-pointer with size prefix
- Linear hole scanning (not optimized)
- Mark lines as FreshAlloc
- Handle size classes like MMTk

### Phase 4: GC Cycle (simplified)
- Mark-region only (NO evacuation initially)
- Conservative line marking (mark current + next)
- Simple sweep to Usable/Full states
- Defer evacuation to later phase

### Phase 5: Runtime Integration (unchanged)
- Weak refs, finalizers, barriers, fork

### Phase 6: Diagnostics (unchanged)
- Stats, config, debugging

### Phase 7: Testing (enhanced)
- Add object layout tests
- Add alignment tests  
- Add ractor isolation tests
- Add JIT compatibility tests

### Phase 8: Evacuation (NEW)
- Implement opportunistic evacuation
- Add histograms and candidate selection
- Add forwarding pointers
- This is OPTIONAL for initial release

## Verification Strategy

### Unit Tests for Core Invariants

```ruby
# Test 1: Object layout matches Ruby expectations
def test_object_layout
  obj = Object.new
  # Verify RBASIC fields are accessible
  assert_kind_of Class, obj.class
  assert_respond_to obj, :object_id
end

# Test 2: Size storage
def test_size_storage
  # Allocate objects of known sizes
  small = "x" * 10
  medium = "x" * 200
  # Verify they survive GC
  GC.start
  assert_equal 10, small.length
  assert_equal 200, medium.length
end

# Test 3: Alignment
def test_alignment
  objects = 1000.times.map { Object.new }
  objects.each do |obj|
    # object_id encodes address - check alignment
    addr = obj.object_id << 1
    assert_equal 0, addr % 8, "Object not 8-byte aligned"
  end
end
```

### Stress Tests

```ruby
# Fragmentation stress test
def test_fragmentation
  # Create fragmented heap
  arrays = 10000.times.map { Array.new(rand(10..100)) }
  
  # Delete every other one
  arrays.each_with_index { |a, i| arrays[i] = nil if i.odd? }
  
  # Force GC
  10.times { GC.start }
  
  # Allocate more - should reuse holes
  more = 5000.times.map { Array.new(50) }
  
  # Verify stats show reasonable block usage
  stats = GC.stat
  # Block count shouldn't explode
end

# Ractor isolation test
def test_ractor_isolation
  r1 = Ractor.new { 1000.times.map { Object.new } }
  r2 = Ractor.new { 1000.times.map { Object.new } }
  
  # Each ractor should have independent allocation
  r1.take
  r2.take
  
  # GC shouldn't crash
  GC.start
end
```

### Memory Debugging

```bash
# Run with Valgrind
valgrind --leak-check=full \
  RUBY_GC_LIBRARY=immix ruby -e "10000.times { Object.new }; GC.start"

# Run with AddressSanitizer
RUBY_GC_LIBRARY=immix ASAN_OPTIONS=detect_leaks=1 ruby test.rb
```

## Summary of Changes Needed

| Original | Correction |
|----------|------------|
| 32KB blocks | 64KB blocks |
| 128B lines | 256B lines |
| 3 line states | 5 line states |
| 3 block states | 2 block states |
| No allocation map | Add allocation map |
| Size in object header | Size before object ([-1]) |
| Evacuation in Phase 4 | Defer to Phase 8 |
| Basic tests | Enhanced with layout/alignment/ractor tests |

These corrections align the plan with the actual Immix algorithm and Ruby's object model requirements.
