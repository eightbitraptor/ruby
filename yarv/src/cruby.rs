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

#[allow(unsafe_op_in_unsafe_fn)]
mod autogened {
    use super::*;
    include!("cruby_bindings.inc.rs");
}
pub use autogened::*;

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
