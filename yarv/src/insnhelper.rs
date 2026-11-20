//! Ports of the instruction helpers in vm_insnhelper.c.

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

#[unsafe(no_mangle)]
pub extern "C" fn rb_yarv_opt_plus(recv: VALUE, obj: VALUE) -> VALUE {
    unsafe {
        if recv.fixnum_p() && obj.fixnum_p() && basic_op_unredefined_p(BOP_PLUS, INTEGER_REDEFINED_OP_FLAG) {
            fix_plus_fix(recv, obj)
        } else if recv.flonum_p() && obj.flonum_p() && basic_op_unredefined_p(BOP_PLUS, FLOAT_REDEFINED_OP_FLAG) {
            dbl2num(recv.float_value() + obj.float_value())
        } else if recv.special_const_p() || obj.special_const_p() {
            Qundef
        } else if recv.rbasic_class() == rb_cFloat && obj.rbasic_class() == rb_cFloat
            && basic_op_unredefined_p(BOP_PLUS, FLOAT_REDEFINED_OP_FLAG)
        {
            dbl2num(recv.float_value() + obj.float_value())
        } else if recv.rbasic_class() == rb_cString && obj.rbasic_class() == rb_cString
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
    }
}
