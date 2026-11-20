//! Ports of the instruction helpers in vm_insnhelper.c.
//!
//! Each `opt_*` function has the contract of the C `vm_opt_*` function it
//! replaces. It returns `Qundef` when the instruction must fall back to a
//! method call.

use core::ffi::c_long;

use crate::cruby::*;

/// `rb_overflowed_fix_to_int`.
#[inline]
fn overflowed_fix_to_int(x: isize) -> isize {
    (((x >> 1) as usize) ^ (1usize << (usize::BITS - 1))) as isize
}

/// `rb_fix_plus_fix`.
#[inline]
fn fix_plus_fix(x: VALUE, y: VALUE) -> VALUE {
    match (x.0 as isize).overflowing_add(y.0 as isize - 1) {
        (z, false) => VALUE(z as usize),
        (z, true) => unsafe { rb_int2big(overflowed_fix_to_int(z)) },
    }
}

/// `rb_fix_minus_fix`.
#[inline]
fn fix_minus_fix(x: VALUE, y: VALUE) -> VALUE {
    match (x.0 as isize).overflowing_sub(y.0 as isize - 1) {
        (z, false) => VALUE(z as usize),
        (z, true) => unsafe { rb_int2big(overflowed_fix_to_int(z)) },
    }
}

/// `rb_fix_mul_fix`.
#[inline]
fn fix_mul_fix(x: VALUE, y: VALUE) -> VALUE {
    let (lx, ly) = (x.fix2long(), y.fix2long());
    let product = lx as i128 * ly as i128;
    if fixable(product) {
        long2fix(product as c_long)
    } else {
        unsafe { rb_big_mul(rb_int2big(lx as isize), rb_int2big(ly as isize)) }
    }
}

/// `rb_fix_divmod_fix`. Ruby rounds the quotient toward negative infinity.
/// The divisor must not be zero.
#[inline]
fn fix_divmod_fix(a: VALUE, b: VALUE) -> (VALUE, VALUE) {
    let (x, y) = (a.fix2long(), b.fix2long());
    if x == FIXNUM_MIN && y == -1 {
        return (long2num(-FIXNUM_MIN), long2fix(0));
    }
    let (mut div, mut m) = (x / y, x % y);
    if if y > 0 { m < 0 } else { m > 0 } {
        m += y;
        div -= 1;
    }
    (long2fix(div), long2fix(m))
}

/// The operand kinds that the numeric fast paths check, in the order that
/// the C helpers check them.
enum Num {
    Fix,
    Flo(f64, f64),
    Special,
    Heap,
}

#[inline(always)]
fn classify(recv: VALUE, obj: VALUE, bop: ruby_basic_operators) -> Num {
    unsafe {
        if recv.fixnum_p() && obj.fixnum_p() && basic_op_unredefined_p(bop, INTEGER_REDEFINED_OP_FLAG) {
            Num::Fix
        } else if recv.flonum_p() && obj.flonum_p() && basic_op_unredefined_p(bop, FLOAT_REDEFINED_OP_FLAG) {
            Num::Flo(recv.float_value(), obj.float_value())
        } else if recv.special_const_p() || obj.special_const_p() {
            Num::Special
        } else if recv.rbasic_class() == rb_cFloat && obj.rbasic_class() == rb_cFloat
            && basic_op_unredefined_p(bop, FLOAT_REDEFINED_OP_FLAG)
        {
            Num::Flo(recv.float_value(), obj.float_value())
        } else {
            Num::Heap
        }
    }
}

/// True when `recv` is a heap object of class `klass` and `bop` is not
/// redefined for it. The value must not be a special constant.
#[inline(always)]
unsafe fn heap_class_p(recv: VALUE, klass: VALUE, bop: ruby_basic_operators, flag: u32) -> bool {
    unsafe { recv.rbasic_class() == klass && basic_op_unredefined_p(bop, flag) }
}

#[inline]
pub fn opt_plus(recv: VALUE, obj: VALUE) -> VALUE {
    match classify(recv, obj, BOP_PLUS) {
        Num::Fix => fix_plus_fix(recv, obj),
        Num::Flo(a, b) => dbl2num(a + b),
        Num::Special => Qundef,
        Num::Heap => unsafe {
            if recv.rbasic_class() == rb_cString && obj.rbasic_class() == rb_cString
                && basic_op_unredefined_p(BOP_PLUS, STRING_REDEFINED_OP_FLAG)
            {
                rb_str_opt_plus(recv, obj)
            } else if recv.rbasic_class() == rb_cArray && obj.rbasic_class() == rb_cArray
                && basic_op_unredefined_p(BOP_PLUS, ARRAY_REDEFINED_OP_FLAG)
            {
                rb_ary_plus(recv, obj)
            } else {
                Qundef
            }
        },
    }
}

#[inline]
pub fn opt_minus(recv: VALUE, obj: VALUE) -> VALUE {
    match classify(recv, obj, BOP_MINUS) {
        Num::Fix => fix_minus_fix(recv, obj),
        Num::Flo(a, b) => dbl2num(a - b),
        Num::Special | Num::Heap => Qundef,
    }
}

#[inline]
pub fn opt_mult(recv: VALUE, obj: VALUE) -> VALUE {
    match classify(recv, obj, BOP_MULT) {
        Num::Fix => fix_mul_fix(recv, obj),
        Num::Flo(a, b) => dbl2num(a * b),
        Num::Special | Num::Heap => Qundef,
    }
}

#[inline]
pub fn opt_div(recv: VALUE, obj: VALUE) -> VALUE {
    match classify(recv, obj, BOP_DIV) {
        Num::Fix if obj.fix2long() == 0 => Qundef,
        Num::Fix => fix_divmod_fix(recv, obj).0,
        Num::Flo(..) => unsafe { rb_flo_div_flo(recv, obj) },
        Num::Special | Num::Heap => Qundef,
    }
}

#[inline]
pub fn opt_mod(recv: VALUE, obj: VALUE) -> VALUE {
    match classify(recv, obj, BOP_MOD) {
        Num::Fix if obj.fix2long() == 0 => Qundef,
        Num::Fix => fix_divmod_fix(recv, obj).1,
        Num::Flo(a, b) => dbl2num(unsafe { ruby_float_mod(a, b) }),
        Num::Special | Num::Heap => Qundef,
    }
}

macro_rules! comparison {
    ($name:ident, $bop:expr, $op:tt) => {
        #[inline]
        pub fn $name(recv: VALUE, obj: VALUE) -> VALUE {
            match classify(recv, obj, $bop) {
                Num::Fix => rbool((recv.0 as isize) $op (obj.0 as isize)),
                Num::Flo(a, b) => rbool(a $op b),
                Num::Special | Num::Heap => Qundef,
            }
        }
    };
}

comparison!(opt_lt, BOP_LT, <);
comparison!(opt_le, BOP_LE, <=);
comparison!(opt_gt, BOP_GT, >);
comparison!(opt_ge, BOP_GE, >=);

#[inline]
pub fn opt_ltlt(recv: VALUE, obj: VALUE) -> VALUE {
    if recv.special_const_p() {
        return Qundef;
    }
    unsafe {
        if heap_class_p(recv, rb_cString, BOP_LTLT, STRING_REDEFINED_OP_FLAG) {
            if obj.type_p(RUBY_T_STRING) {
                rb_str_buf_append(recv, obj)
            } else {
                rb_str_concat(recv, obj)
            }
        } else if heap_class_p(recv, rb_cArray, BOP_LTLT, ARRAY_REDEFINED_OP_FLAG) {
            rb_ary_push(recv, obj)
        } else {
            Qundef
        }
    }
}

#[inline]
pub fn opt_and(recv: VALUE, obj: VALUE) -> VALUE {
    // Both tag bits must be 1 for the result to be a Fixnum.
    let ret = VALUE(recv.0 & obj.0);
    if ret.fixnum_p() && basic_op_unredefined_p(BOP_AND, INTEGER_REDEFINED_OP_FLAG) {
        ret
    } else {
        Qundef
    }
}

#[inline]
pub fn opt_or(recv: VALUE, obj: VALUE) -> VALUE {
    if recv.fixnum_p() && obj.fixnum_p() && basic_op_unredefined_p(BOP_OR, INTEGER_REDEFINED_OP_FLAG) {
        VALUE(recv.0 | obj.0)
    } else {
        Qundef
    }
}

/// `rb_ary_entry_internal`.
#[inline]
unsafe fn ary_entry(ary: VALUE, mut offset: c_long) -> VALUE {
    unsafe {
        let len = rarray_len(ary);
        if len == 0 {
            return Qnil;
        }
        if offset < 0 {
            offset += len;
            if offset < 0 {
                return Qnil;
            }
        } else if len <= offset {
            return Qnil;
        }
        *rarray_const_ptr(ary).add(offset as usize)
    }
}

#[inline]
pub fn opt_aref(recv: VALUE, obj: VALUE) -> VALUE {
    unsafe {
        if recv.special_const_p() {
            if recv.fixnum_p() && obj.fixnum_p() && basic_op_unredefined_p(BOP_AREF, INTEGER_REDEFINED_OP_FLAG) {
                return rb_fix_aref(recv, obj);
            }
            Qundef
        } else if heap_class_p(recv, rb_cArray, BOP_AREF, ARRAY_REDEFINED_OP_FLAG) {
            if obj.fixnum_p() {
                ary_entry(recv, obj.fix2long())
            } else {
                rb_ary_aref1(recv, obj)
            }
        } else if heap_class_p(recv, rb_cHash, BOP_AREF, HASH_REDEFINED_OP_FLAG) {
            rb_hash_aref(recv, obj)
        } else {
            Qundef
        }
    }
}

#[inline]
pub fn opt_aset(recv: VALUE, obj: VALUE, set: VALUE) -> VALUE {
    unsafe {
        if recv.special_const_p() {
            Qundef
        } else if heap_class_p(recv, rb_cArray, BOP_ASET, ARRAY_REDEFINED_OP_FLAG) && obj.fixnum_p() {
            rb_ary_store(recv, obj.fix2long(), set);
            set
        } else if heap_class_p(recv, rb_cHash, BOP_ASET, HASH_REDEFINED_OP_FLAG) {
            rb_hash_aset(recv, obj, set);
            set
        } else {
            Qundef
        }
    }
}

/// `vm_opt_length`. `bop` is `BOP_LENGTH`, `BOP_SIZE`, or `BOP_EMPTY_P`.
#[inline]
pub fn opt_length(recv: VALUE, bop: ruby_basic_operators) -> VALUE {
    unsafe {
        if recv.special_const_p() {
            Qundef
        } else if heap_class_p(recv, rb_cString, bop, STRING_REDEFINED_OP_FLAG) {
            if bop == BOP_EMPTY_P {
                long2num(rstring_len(recv))
            } else {
                rb_str_length(recv)
            }
        } else if heap_class_p(recv, rb_cArray, bop, ARRAY_REDEFINED_OP_FLAG) {
            long2num(rarray_len(recv))
        } else if heap_class_p(recv, rb_cHash, bop, HASH_REDEFINED_OP_FLAG) {
            long2fix(rb_hash_size_num(recv) as c_long)
        } else {
            Qundef
        }
    }
}

#[inline]
pub fn opt_empty_p(recv: VALUE) -> VALUE {
    match opt_length(recv, BOP_EMPTY_P) {
        len if len.undef_p() => len,
        len if len == long2fix(0) => Qtrue,
        _ => Qfalse,
    }
}

/// `fix_succ`.
#[inline]
fn fix_succ(x: VALUE) -> VALUE {
    if x.0 == !0 {
        long2fix(0)
    } else if x.0 == !0 >> 1 {
        unsafe { rb_uint2big(1usize << (usize::BITS - 2)) }
    } else {
        VALUE(x.0 + 2)
    }
}

#[inline]
pub fn opt_succ(recv: VALUE) -> VALUE {
    unsafe {
        if recv.fixnum_p() && basic_op_unredefined_p(BOP_SUCC, INTEGER_REDEFINED_OP_FLAG) {
            fix_succ(recv)
        } else if recv.special_const_p() {
            Qundef
        } else if heap_class_p(recv, rb_cString, BOP_SUCC, STRING_REDEFINED_OP_FLAG) {
            rb_str_succ(recv)
        } else {
            Qundef
        }
    }
}

#[inline]
pub fn opt_regexpmatch2(recv: VALUE, obj: VALUE) -> VALUE {
    unsafe {
        if recv.special_const_p() {
            Qundef
        } else if recv.rbasic_class() == rb_cString && obj.class_of() == rb_cRegexp
            && basic_op_unredefined_p(BOP_MATCH, STRING_REDEFINED_OP_FLAG)
        {
            rb_reg_match(obj, recv)
        } else if heap_class_p(recv, rb_cRegexp, BOP_MATCH, REGEXP_REDEFINED_OP_FLAG) {
            rb_reg_match(recv, obj)
        } else {
            Qundef
        }
    }
}
