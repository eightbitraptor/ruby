//! Instruction handlers for the instructions that insns.def marks with
//! `// attr bool rust = true;`.
//!
//! The generated shims in yarv_insns.rs call these functions. Each handler
//! takes the execution context, the control frame, the operands, and the
//! popped values in insns.def order, and returns the pushed values.
//!
//! When a handler runs, `cfp.pc` points at the next instruction and
//! `cfp.sp` still includes the popped values, as in a C instruction body.

use core::ffi::{c_int, c_long};

use crate::cruby::*;

#[inline(always)]
unsafe fn sp(cfp: CfpPtr) -> *mut VALUE {
    unsafe { (*cfp).sp }
}

/// `&TOPN(n)`.
#[inline(always)]
unsafe fn topn_ptr(cfp: CfpPtr, n: usize) -> *mut VALUE {
    unsafe { sp(cfp).sub(n + 1) }
}

/// `STACK_ADDR_FROM_TOP(n)`.
#[inline(always)]
unsafe fn stack_addr_from_top(cfp: CfpPtr, n: usize) -> *mut VALUE {
    unsafe { sp(cfp).sub(n) }
}

pub unsafe fn nop(_ec: EcPtr, _cfp: CfpPtr) {}

pub unsafe fn getglobal(_ec: EcPtr, _cfp: CfpPtr, gid: ID) -> VALUE {
    unsafe { rb_gvar_get(gid) }
}

pub unsafe fn setglobal(_ec: EcPtr, _cfp: CfpPtr, gid: ID, val: VALUE) {
    unsafe { rb_gvar_set(gid, val) };
}

pub unsafe fn putnil(_ec: EcPtr, _cfp: CfpPtr) -> VALUE {
    Qnil
}

pub unsafe fn putself(_ec: EcPtr, cfp: CfpPtr) -> VALUE {
    unsafe { (*cfp).self_ }
}

pub unsafe fn putobject(_ec: EcPtr, _cfp: CfpPtr, _val: VALUE) {}

pub unsafe fn dupstring(ec: EcPtr, _cfp: CfpPtr, str: VALUE) -> VALUE {
    unsafe { rb_ec_str_resurrect(ec, str, false) }
}

pub unsafe fn dupchilledstring(ec: EcPtr, _cfp: CfpPtr, str: VALUE) -> VALUE {
    unsafe { rb_ec_str_resurrect(ec, str, true) }
}

pub unsafe fn concatstrings(_ec: EcPtr, cfp: CfpPtr, num: rb_num_t) -> VALUE {
    unsafe { rb_str_concat_literals(num, stack_addr_from_top(cfp, num)) }
}

pub unsafe fn anytostring(_ec: EcPtr, _cfp: CfpPtr, val: VALUE, str: VALUE) -> VALUE {
    unsafe { rb_obj_as_string_result(str, val) }
}

pub unsafe fn toregexp(_ec: EcPtr, cfp: CfpPtr, opt: rb_num_t, cnt: rb_num_t) -> VALUE {
    unsafe { rb_reg_new_from_values(cnt as c_long, stack_addr_from_top(cfp, cnt), opt as c_int) }
}

pub unsafe fn intern(_ec: EcPtr, _cfp: CfpPtr, str: VALUE) -> VALUE {
    unsafe { rb_str_intern(str) }
}

pub unsafe fn newarray(ec: EcPtr, cfp: CfpPtr, num: rb_num_t) -> VALUE {
    unsafe { rb_ec_ary_new_from_values(ec, num as c_long, stack_addr_from_top(cfp, num)) }
}

pub unsafe fn pushtoarraykwsplat(_ec: EcPtr, _cfp: CfpPtr, ary: VALUE, hash: VALUE) -> VALUE {
    unsafe {
        if rhash_size(hash) != 0 {
            rb_ary_push(ary, hash);
        }
    }
    ary
}

pub unsafe fn pushtoarray(_ec: EcPtr, cfp: CfpPtr, num: rb_num_t) -> VALUE {
    unsafe {
        let objp = stack_addr_from_top(cfp, num);
        rb_ary_cat(*objp.sub(1), objp, num as c_long)
    }
}

pub unsafe fn newrange(_ec: EcPtr, _cfp: CfpPtr, flag: rb_num_t, low: VALUE, high: VALUE) -> VALUE {
    unsafe { rb_range_new(low, high, flag as c_int) }
}

pub unsafe fn pop(_ec: EcPtr, _cfp: CfpPtr, _val: VALUE) {}

pub unsafe fn dup(_ec: EcPtr, _cfp: CfpPtr, val: VALUE) -> (VALUE, VALUE) {
    (val, val)
}

pub unsafe fn dupn(_ec: EcPtr, cfp: CfpPtr, n: rb_num_t) {
    unsafe { core::ptr::copy_nonoverlapping(stack_addr_from_top(cfp, n), sp(cfp), n) }
}

pub unsafe fn swap(_ec: EcPtr, _cfp: CfpPtr, val: VALUE, obj: VALUE) -> (VALUE, VALUE) {
    (obj, val)
}

pub unsafe fn opt_reverse(_ec: EcPtr, cfp: CfpPtr, n: rb_num_t) {
    unsafe { core::slice::from_raw_parts_mut(stack_addr_from_top(cfp, n), n).reverse() }
}

pub unsafe fn topn(_ec: EcPtr, cfp: CfpPtr, n: rb_num_t) -> VALUE {
    unsafe { *topn_ptr(cfp, n) }
}

pub unsafe fn setn(_ec: EcPtr, cfp: CfpPtr, n: rb_num_t, val: VALUE) -> VALUE {
    unsafe { *topn_ptr(cfp, n) = val };
    val
}

pub unsafe fn adjuststack(_ec: EcPtr, _cfp: CfpPtr, _n: rb_num_t) {}

pub unsafe fn checktype(_ec: EcPtr, _cfp: CfpPtr, r#type: rb_num_t, val: VALUE) -> VALUE {
    rbool(rb_type(val) == r#type as ruby_value_type)
}
