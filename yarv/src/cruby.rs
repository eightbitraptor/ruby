//! The Rust view of CRuby types, globals, and functions.
//!
//! `cruby_bindings.inc.rs` comes from `make yarv-bindgen`. This module adds
//! `VALUE` and Rust versions of the C macros and `static inline` functions
//! that bindgen cannot import.

#![allow(non_upper_case_globals, non_camel_case_types, non_snake_case, dead_code)]

use core::ffi::c_long;

#[repr(transparent)]
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct VALUE(pub usize);

pub type ID = usize;
pub type size_t = usize;

/// The leading fields of `rb_execution_context_t`. The struct continues
/// past them; yarv.h asserts the offsets.
#[repr(C)]
pub struct rb_execution_context_struct {
    pub vm_stack: *mut VALUE,
    pub vm_stack_size: usize,
    pub cfp: *mut rb_control_frame_t,
    pub tag: *mut core::ffi::c_void,
    pub interrupt_flag: u32,
    pub interrupt_mask: u32,
    pub fiber_ptr: *mut core::ffi::c_void,
    pub thread_ptr: *mut rb_thread_struct,
}
pub type rb_execution_context_t = rb_execution_context_struct;

/// Opaque C structs. Rust code only holds pointers to them.
#[repr(C)]
pub struct rb_iseq_struct {
    _private: [u8; 0],
}
#[repr(C)]
pub struct rb_thread_struct {
    _private: [u8; 0],
}
#[repr(C)]
pub struct rb_call_data {
    _private: [u8; 0],
}
#[repr(C)]
pub struct iseq_inline_constant_cache {
    _private: [u8; 0],
}
#[repr(C)]
pub struct iseq_inline_iv_cache_entry {
    _private: [u8; 0],
}

pub type EcPtr = *mut rb_execution_context_t;
pub type CfpPtr = *mut rb_control_frame_t;

/// Operand types of insns.def. tool/ruby_vm/models/bare_instruction.rb
/// lists the names that the generated code may use.
pub type rb_num_t = usize;
pub type lindex_t = usize;
pub type OFFSET = isize;
pub type ISEQ = *const rb_iseq_t;
pub type CALL_DATA = *mut rb_call_data;
pub type IC = *mut iseq_inline_constant_cache;
pub type IVC = *mut iseq_inline_iv_cache_entry;

#[allow(unsafe_op_in_unsafe_fn)]
mod autogened {
    use super::*;
    include!("cruby_bindings.inc.rs");
}
pub use autogened::*;

/// Offsets and constants from yarv/layout.c for this configuration.
pub mod layout {
    include!(env!("YARV_LAYOUT_RS"));
}

/// Reads a field at byte offset `off` (a `layout` constant) of `base`.
#[inline(always)]
pub unsafe fn field<T: Copy>(base: *const core::ffi::c_void, off: i64) -> T {
    unsafe { base.cast::<u8>().add(off as usize).cast::<T>().read() }
}

// C functions that yarv.h declares for this crate.
unsafe extern "C" {
    pub fn rb_yarv_send(ec: EcPtr, cfp: CfpPtr, cd: CALL_DATA, blockiseq: ISEQ) -> VALUE;
    pub fn rb_yarv_send_without_block(ec: EcPtr, cfp: CfpPtr, cd: CALL_DATA) -> VALUE;
    pub fn rb_yarv_leave(ec: EcPtr, cfp: CfpPtr) -> bool;
}

#[cfg(yarv_yjit)]
unsafe extern "C" {
    static rb_yjit_enabled_p: bool;
}
#[cfg(yarv_zjit)]
unsafe extern "C" {
    static rb_zjit_entry: *mut core::ffi::c_void;
}

/// True when YJIT or ZJIT runs. The Rust fast paths for calls and returns
/// then step aside, so that JIT_EXEC and the JIT frame rules still apply.
#[inline(always)]
pub fn jit_enabled() -> bool {
    #[cfg(yarv_yjit)]
    if unsafe { rb_yjit_enabled_p } {
        return true;
    }
    #[cfg(yarv_zjit)]
    if unsafe { !rb_zjit_entry.is_null() } {
        return true;
    }
    false
}

pub const Qfalse: VALUE = VALUE(RUBY_Qfalse as usize);
pub const Qtrue: VALUE = VALUE(RUBY_Qtrue as usize);
pub const Qnil: VALUE = VALUE(RUBY_Qnil as usize);
pub const Qundef: VALUE = VALUE(RUBY_Qundef as usize);

pub const FIXNUM_MAX: c_long = c_long::MAX >> 1;
pub const FIXNUM_MIN: c_long = c_long::MIN >> 1;

impl VALUE {
    #[inline(always)]
    pub fn test(self) -> bool {
        self.0 & !(RUBY_Qnil as usize) != 0
    }

    #[inline(always)]
    pub fn nil_p(self) -> bool {
        self == Qnil
    }

    #[inline(always)]
    pub fn undef_p(self) -> bool {
        self == Qundef
    }

    #[inline(always)]
    pub fn immediate_p(self) -> bool {
        self.0 & (RUBY_IMMEDIATE_MASK as usize) != 0
    }

    #[inline(always)]
    pub fn special_const_p(self) -> bool {
        self == Qfalse || self.immediate_p()
    }

    #[inline(always)]
    pub fn fixnum_p(self) -> bool {
        self.0 & (RUBY_FIXNUM_FLAG as usize) != 0
    }

    #[inline(always)]
    pub fn flonum_p(self) -> bool {
        self.0 & (RUBY_FLONUM_MASK as usize) == RUBY_FLONUM_FLAG as usize
    }

    #[inline(always)]
    pub fn fix2long(self) -> c_long {
        (self.0 as c_long) >> 1
    }

    #[inline(always)]
    pub fn as_rbasic(self) -> *const RBasic {
        self.0 as *const RBasic
    }

    /// `RBASIC_CLASS`. The value must not be a special constant.
    #[inline(always)]
    pub unsafe fn rbasic_class(self) -> VALUE {
        unsafe { (*self.as_rbasic()).klass }
    }

    /// `RB_TYPE_P(obj, t)` for a heap object.
    #[inline(always)]
    pub unsafe fn builtin_type(self) -> u32 {
        unsafe { ((*self.as_rbasic()).flags.0 & (RUBY_T_MASK as usize)) as u32 }
    }

    #[inline(always)]
    pub fn type_p(self, t: ruby_value_type) -> bool {
        !self.special_const_p() && unsafe { self.builtin_type() } == t
    }

    /// `CLASS_OF`.
    #[inline]
    pub fn class_of(self) -> VALUE {
        if !self.special_const_p() {
            return unsafe { self.rbasic_class() };
        }
        unsafe {
            if self == Qfalse {
                rb_cFalseClass
            } else if self == Qnil {
                rb_cNilClass
            } else if self == Qtrue {
                rb_cTrueClass
            } else if self.fixnum_p() {
                rb_cInteger
            } else if self.flonum_p() {
                rb_cFloat
            } else {
                rb_cSymbol
            }
        }
    }

    /// `RFLOAT_VALUE`. The value must be a Float.
    #[inline]
    pub unsafe fn float_value(self) -> f64 {
        if self.flonum_p() {
            flonum_value(self)
        } else {
            unsafe { (*(self.0 as *const RFloat)).float_value }
        }
    }
}

#[inline(always)]
pub fn rbool(b: bool) -> VALUE {
    if b { Qtrue } else { Qfalse }
}

#[inline(always)]
pub fn long2fix(l: c_long) -> VALUE {
    VALUE(((l as usize) << 1) | RUBY_FIXNUM_FLAG as usize)
}

#[inline(always)]
pub fn fixable(l: i128) -> bool {
    l >= FIXNUM_MIN as i128 && l <= FIXNUM_MAX as i128
}

#[inline]
pub fn long2num(l: c_long) -> VALUE {
    if fixable(l as i128) {
        long2fix(l)
    } else {
        unsafe { rb_int2big(l as isize) }
    }
}

/// `rb_float_flonum_value`.
#[inline]
fn flonum_value(v: VALUE) -> f64 {
    if v.0 == 0x8000000000000002 {
        return 0.0;
    }
    let b63 = v.0 >> 63;
    let bits = ((2 - b63) | (v.0 & !0x03)).rotate_right(3);
    f64::from_bits(bits as u64)
}

/// `DBL2NUM`, which is `rb_float_new_inline`.
#[inline]
pub fn dbl2num(d: f64) -> VALUE {
    let v = d.to_bits() as usize;
    let bits = ((v >> 60) & 0x7) as i32;
    if v != 0x3000000000000000 && ((bits - 3) & !0x01) == 0 {
        VALUE((v.rotate_left(3) & !0x01) | 0x02)
    } else if v == 0 {
        VALUE(0x8000000000000002)
    } else {
        unsafe { rb_float_new_in_heap(d) }
    }
}

/// `BASIC_OP_UNREDEFINED_P(op, klass)`.
#[inline(always)]
pub fn basic_op_unredefined_p(op: ruby_basic_operators, klass: u32) -> bool {
    let flag = unsafe {
        (&raw const ruby_vm_redefined_flag)
            .cast::<i16>()
            .add(op as usize)
            .read()
    };
    (flag as u32 & klass) == 0
}

/// `RARRAY_LEN`. The value must be an Array.
#[inline]
pub unsafe fn rarray_len(ary: VALUE) -> c_long {
    unsafe {
        let flags = (*ary.as_rbasic()).flags.0;
        if flags & RARRAY_EMBED_FLAG as usize != 0 {
            ((flags & RARRAY_EMBED_LEN_MASK as usize) >> RARRAY_EMBED_LEN_SHIFT) as c_long
        } else {
            let heap = (&raw const (*(ary.0 as *const RArray)).as_)
                .cast::<RArray__bindgen_ty_1__bindgen_ty_1>();
            (*heap).len
        }
    }
}

/// `RARRAY_CONST_PTR`. The value must be an Array.
#[inline]
pub unsafe fn rarray_const_ptr(ary: VALUE) -> *const VALUE {
    unsafe {
        let flags = (*ary.as_rbasic()).flags.0;
        let as_ = &raw const (*(ary.0 as *const RArray)).as_;
        if flags & RARRAY_EMBED_FLAG as usize != 0 {
            as_.cast::<VALUE>()
        } else {
            (*as_.cast::<RArray__bindgen_ty_1__bindgen_ty_1>()).ptr
        }
    }
}

/// `RSTRING_LEN`. The value must be a String.
#[inline]
pub unsafe fn rstring_len(s: VALUE) -> c_long {
    unsafe { (*(s.0 as *const RString)).len }
}

/// `RHASH_SIZE`. The value must be a Hash.
#[inline]
pub unsafe fn rhash_size(h: VALUE) -> usize {
    unsafe { rb_hash_size_num(h) }
}

/// `rb_type`, which `TYPE()` expands to.
#[inline]
pub fn rb_type(v: VALUE) -> ruby_value_type {
    if !v.special_const_p() {
        unsafe { v.builtin_type() }
    } else if v == Qfalse {
        RUBY_T_FALSE
    } else if v == Qnil {
        RUBY_T_NIL
    } else if v == Qtrue {
        RUBY_T_TRUE
    } else if v == Qundef {
        RUBY_T_UNDEF
    } else if v.fixnum_p() {
        RUBY_T_FIXNUM
    } else if v.0 & 0xff == RUBY_SYMBOL_FLAG as usize {
        RUBY_T_SYMBOL
    } else {
        RUBY_T_FLOAT
    }
}

/// `RUBY_VM_CHECK_INTS`. yarv.h asserts that `USE_VM_CLOCK` is off, so the
/// check only reads the interrupt flag and mask.
#[inline(always)]
pub unsafe fn vm_check_ints(ec: EcPtr) {
    unsafe {
        let flag = core::sync::atomic::AtomicU32::from_ptr(&raw mut (*ec).interrupt_flag)
            .load(core::sync::atomic::Ordering::Relaxed);
        if flag & !(*ec).interrupt_mask != 0 {
            rb_threadptr_execute_interrupts((*ec).thread_ptr, 0);
        }
    }
}

pub const VM_ENV_DATA_INDEX_SPECVAL: isize = -1;
pub const VM_ENV_DATA_INDEX_FLAGS: isize = 0;
pub const VM_ENV_DATA_INDEX_ENV: isize = 1;

/// `VM_ENV_PREV_EP`.
#[inline(always)]
pub unsafe fn env_prev_ep(ep: *const VALUE) -> *const VALUE {
    unsafe { ((*ep.offset(VM_ENV_DATA_INDEX_SPECVAL)).0 & !0x03) as *const VALUE }
}

/// `vm_env_write`.
#[inline(always)]
pub unsafe fn env_write(ep: *const VALUE, index: isize, v: VALUE) {
    unsafe {
        let flags = (*ep.offset(VM_ENV_DATA_INDEX_FLAGS)).0;
        if flags & VM_ENV_FLAG_WB_REQUIRED as usize == 0 {
            *(ep.offset(index) as *mut VALUE) = v;
        } else {
            env_write_slowpath(ep, index, v);
        }
    }
}

/// `vm_env_write_slowpath`.
#[inline(never)]
unsafe fn env_write_slowpath(ep: *const VALUE, index: isize, v: VALUE) {
    unsafe {
        let envval = *ep.offset(VM_ENV_DATA_INDEX_ENV);
        let slot = ep.offset(index) as *mut VALUE;
        if (*envval.as_rbasic()).flags.0 & RUBY_FL_SHAREABLE as usize != 0 {
            // A write to a shareable env needs the full barrier. The env
            // keeps WB_REQUIRED for later writes.
            if !v.special_const_p() {
                rb_gc_writebarrier(envval, v);
            }
            *slot = v;
        } else {
            rb_gc_writebarrier_remember(envval);
            *slot = v;
            let flags = ep.offset(VM_ENV_DATA_INDEX_FLAGS) as *mut VALUE;
            *flags = VALUE((*flags).0 & !(VM_ENV_FLAG_WB_REQUIRED as usize));
        }
    }
}
