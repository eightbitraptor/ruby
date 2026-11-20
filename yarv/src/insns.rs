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
use crate::insnhelper;

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

/// `vm_get_ep`. Walks `lv` levels up the environment chain.
#[inline(always)]
unsafe fn get_ep(ep: *const VALUE, lv: rb_num_t) -> *const VALUE {
    let mut ep = ep;
    for _ in 0..lv {
        ep = unsafe { env_prev_ep(ep) };
    }
    ep
}

pub unsafe fn getlocal(_ec: EcPtr, cfp: CfpPtr, idx: lindex_t, level: rb_num_t) -> VALUE {
    unsafe { *get_ep((*cfp).ep, level).sub(idx) }
}

pub unsafe fn setlocal(_ec: EcPtr, cfp: CfpPtr, idx: lindex_t, level: rb_num_t, val: VALUE) {
    unsafe { env_write(get_ep((*cfp).ep, level), -(idx as isize), val) }
}

/// `JUMP(dst)`. The C side copies `cfp.pc` back to its PC register.
#[inline(always)]
unsafe fn jump_to(cfp: CfpPtr, dst: OFFSET) {
    unsafe { (*cfp).pc = (*cfp).pc.offset(dst) }
}

pub unsafe fn jump(ec: EcPtr, cfp: CfpPtr, dst: OFFSET) {
    unsafe {
        vm_check_ints(ec);
        jump_to(cfp, dst);
    }
}

pub unsafe fn branchif(ec: EcPtr, cfp: CfpPtr, dst: OFFSET, val: VALUE) {
    if val.test() {
        unsafe { jump(ec, cfp, dst) }
    }
}

pub unsafe fn branchunless(ec: EcPtr, cfp: CfpPtr, dst: OFFSET, val: VALUE) {
    if !val.test() {
        unsafe { jump(ec, cfp, dst) }
    }
}

pub unsafe fn branchnil(ec: EcPtr, cfp: CfpPtr, dst: OFFSET, val: VALUE) {
    if val.nil_p() {
        unsafe { jump(ec, cfp, dst) }
    }
}

pub unsafe fn jump_without_ints(_ec: EcPtr, cfp: CfpPtr, dst: OFFSET) {
    unsafe { jump_to(cfp, dst) }
}

pub unsafe fn branchif_without_ints(_ec: EcPtr, cfp: CfpPtr, dst: OFFSET, val: VALUE) {
    if val.test() {
        unsafe { jump_to(cfp, dst) }
    }
}

pub unsafe fn branchunless_without_ints(_ec: EcPtr, cfp: CfpPtr, dst: OFFSET, val: VALUE) {
    if !val.test() {
        unsafe { jump_to(cfp, dst) }
    }
}

pub unsafe fn branchnil_without_ints(_ec: EcPtr, cfp: CfpPtr, dst: OFFSET, val: VALUE) {
    if val.nil_p() {
        unsafe { jump_to(cfp, dst) }
    }
}

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

/// `vm_check_keyword`. Returns Qfalse when the caller passed the keyword.
pub unsafe fn checkkeyword(
    _ec: EcPtr,
    cfp: CfpPtr,
    kw_bits_index: lindex_t,
    keyword_index: lindex_t,
) -> VALUE {
    unsafe {
        let kw_bits = *(*cfp).ep.sub(kw_bits_index);
        if kw_bits.fixnum_p() {
            let bits = kw_bits.fix2long() as u32;
            if keyword_index < VM_KW_SPECIFIED_BITS_MAX as usize && bits & (1 << keyword_index) != 0 {
                return Qfalse;
            }
        } else if rb_set_lookup(kw_bits, long2fix(keyword_index as c_long)) {
            return Qfalse;
        }
        Qtrue
    }
}

pub unsafe fn splatkw(_ec: EcPtr, _cfp: CfpPtr, hash: VALUE, block: VALUE) -> (VALUE, VALUE) {
    let obj = if hash.nil_p() { Qnil } else { unsafe { rb_to_hash_type(hash) } };
    (obj, block)
}

unsafe extern "C" {
    // vm_insnhelper.c exports it without a header declaration.
    fn rb_vm_opt_getconstant_path(ec: EcPtr, cfp: CfpPtr, ic: IC) -> VALUE;
}

pub unsafe fn opt_getconstant_path(ec: EcPtr, cfp: CfpPtr, ic: IC) -> VALUE {
    unsafe { rb_vm_opt_getconstant_path(ec, cfp, ic) }
}

// The specialized method call instructions. A handler returns Qundef when
// the receiver does not qualify for the fast path; the generated glue then
// calls the method through opt_send_without_block.

pub unsafe fn opt_plus(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE, obj: VALUE) -> VALUE {
    insnhelper::opt_plus(recv, obj)
}

pub unsafe fn opt_minus(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE, obj: VALUE) -> VALUE {
    insnhelper::opt_minus(recv, obj)
}

pub unsafe fn opt_mult(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE, obj: VALUE) -> VALUE {
    insnhelper::opt_mult(recv, obj)
}

pub unsafe fn opt_div(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE, obj: VALUE) -> VALUE {
    insnhelper::opt_div(recv, obj)
}

pub unsafe fn opt_mod(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE, obj: VALUE) -> VALUE {
    insnhelper::opt_mod(recv, obj)
}

pub unsafe fn opt_lt(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE, obj: VALUE) -> VALUE {
    insnhelper::opt_lt(recv, obj)
}

pub unsafe fn opt_le(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE, obj: VALUE) -> VALUE {
    insnhelper::opt_le(recv, obj)
}

pub unsafe fn opt_gt(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE, obj: VALUE) -> VALUE {
    insnhelper::opt_gt(recv, obj)
}

pub unsafe fn opt_ge(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE, obj: VALUE) -> VALUE {
    insnhelper::opt_ge(recv, obj)
}

pub unsafe fn opt_ltlt(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE, obj: VALUE) -> VALUE {
    insnhelper::opt_ltlt(recv, obj)
}

pub unsafe fn opt_and(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE, obj: VALUE) -> VALUE {
    insnhelper::opt_and(recv, obj)
}

pub unsafe fn opt_or(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE, obj: VALUE) -> VALUE {
    insnhelper::opt_or(recv, obj)
}

pub unsafe fn opt_aref(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE, obj: VALUE) -> VALUE {
    insnhelper::opt_aref(recv, obj)
}

pub unsafe fn opt_aset(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE, obj: VALUE, set: VALUE) -> VALUE {
    insnhelper::opt_aset(recv, obj, set)
}

pub unsafe fn opt_length(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE) -> VALUE {
    insnhelper::opt_length(recv, BOP_LENGTH)
}

pub unsafe fn opt_size(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE) -> VALUE {
    insnhelper::opt_length(recv, BOP_SIZE)
}

pub unsafe fn opt_empty_p(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE) -> VALUE {
    insnhelper::opt_empty_p(recv)
}

pub unsafe fn opt_succ(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, recv: VALUE) -> VALUE {
    insnhelper::opt_succ(recv)
}

pub unsafe fn opt_regexpmatch2(_ec: EcPtr, _cfp: CfpPtr, _cd: CALL_DATA, obj2: VALUE, obj1: VALUE) -> VALUE {
    insnhelper::opt_regexpmatch2(obj2, obj1)
}
