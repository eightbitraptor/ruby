//! Method calls and returns: the send instructions and `leave`.
//!
//! The fast paths handle a call to a Ruby method with only required
//! parameters through a valid inline cache, and its return. They do in Rust
//! what vm_call_iseq_setup_normal() and vm_pop_frame() do in C, and step
//! aside to the C functions for every other case, including when a JIT runs.

use core::ffi::c_void;

use crate::cruby::layout as l;
use crate::cruby::*;

/// `vm_ci_flag(ci)` and `vm_ci_argc(ci)`.
#[inline(always)]
unsafe fn ci_flag_argc(ci: *const c_void) -> (u32, u32) {
    let v = ci as usize;
    if l::USE_EMBED_CI != 0 && v & 1 != 0 {
        (
            ((v >> l::CI_EMBED_FLAG_SHFT) & l::CI_EMBED_FLAG_MASK as usize) as u32,
            ((v >> l::CI_EMBED_ARGC_SHFT) & l::CI_EMBED_ARGC_MASK as usize) as u32,
        )
    } else {
        unsafe { (field(ci, l::CI_FLAG), field(ci, l::CI_ARGC)) }
    }
}

/// The fast paths of `vm_sendish()` for a call through a valid inline
/// cache with ARGS_SIMPLE call info:
///
/// - a Ruby method with only required parameters: pushes its frame as
///   vm_call_iseq_setup_normal() does and returns Qundef;
/// - attr_reader and attr_writer: vm_call_ivar() and vm_call_attrset().
///
/// Returns None, with no change, when the C version must run.
#[inline(always)]
unsafe fn fast_send(ec: EcPtr, cfp: CfpPtr, cd: CALL_DATA) -> Option<VALUE> {
    unsafe {
        if jit_enabled() {
            return None;
        }
        let cd = cd as *const c_void;
        let (flag, argc) = ci_flag_argc(field(cd, l::CD_CI));
        let simple = (l::VM_CALL_ARGS_SIMPLE | l::VM_CALL_FCALL | l::VM_CALL_VCALL) as u32;
        if flag & !simple != 0 || flag & l::VM_CALL_ARGS_SIMPLE as u32 == 0 {
            return None;
        }

        // vm_search_method_fastpath(): the inline cache must hit.
        let argv = (*cfp).sp.sub(argc as usize);
        let recv = *argv.sub(1);
        let cc: *const c_void = field(cd, l::CD_CC);
        if field::<VALUE>(cc, l::CC_KLASS) != recv.class_of() {
            return None;
        }
        let cme: *const c_void = field(cc, l::CC_CME);
        if cme.is_null() {
            return None;
        }
        let cme_flags: usize = field(cme, l::CME_FLAGS);
        if cme_flags & l::CME_FLAG_INVALIDATED as usize != 0 {
            return None;
        }

        // vm_call_method(): visibility.
        let visi = ((cme_flags & l::CME_VISI_MASK as usize) >> l::CME_VISI_SHIFT) as i64;
        if !(visi == l::METHOD_VISI_PUBLIC
            || (visi == l::METHOD_VISI_PRIVATE && flag & l::VM_CALL_FCALL as u32 != 0))
        {
            return None;
        }

        let def: *const c_void = field(cme, l::CME_DEF);
        match (field::<u32>(def, 0) & l::DEF_TYPE_MASK as u32) as i64 {
            l::VM_METHOD_TYPE_ISEQ => push_method_frame(ec, cfp, cme, def, recv, argv, argc),
            // VM_CALL_METHOD_ATTR(): c_call/c_return hooks need the C path.
            _ if ruby_vm_c_events_enabled != 0 => None,
            l::VM_METHOD_TYPE_IVAR if argc == 0 => {
                let val = crate::ivar::get_fast(recv, cc.cast::<u8>().add(l::CC_AUX as usize).cast())?;
                (*cfp).sp = argv.sub(1);
                Some(val)
            }
            l::VM_METHOD_TYPE_ATTRSET if argc == 1 => {
                let val = *argv;
                if !crate::ivar::set_fast(recv, val, cc.cast::<u8>().add(l::CC_AUX as usize).cast()) {
                    return None;
                }
                (*cfp).sp = argv.sub(1);
                Some(val)
            }
            _ => None,
        }
    }
}

/// Pushes the frame of a Ruby method that takes only required parameters,
/// as vm_call_iseq_setup_normal() does. Returns None, with no change, when
/// the method does not qualify.
#[inline(always)]
unsafe fn push_method_frame(
    ec: EcPtr,
    cfp: CfpPtr,
    cme: *const c_void,
    def: *const c_void,
    recv: VALUE,
    argv: *mut VALUE,
    argc: u32,
) -> Option<VALUE> {
    unsafe {
        // vm_callee_setup_arg(): only required parameters.
        let iseq: *const rb_iseq_t = field(def, l::DEF_ISEQPTR);
        let body: *const c_void = field(iseq.cast(), l::ISEQ_BODY);
        if field::<u32>(body, l::BODY_PARAM_FLAGS) & l::PARAM_FLAGS_NOT_SIMPLE as u32 != 0
            || field::<i32>(body, l::BODY_PARAM_LEAD_NUM) as u32 != argc
            || field::<u32>(body, l::BODY_BUILTIN_ATTRS) & l::BUILTIN_ATTR_SINGLE_NOARG_LEAF as u32 != 0
        {
            return None;
        }
        let param_size = field::<u32>(body, l::BODY_PARAM_SIZE) as usize;
        let local_size = field::<u32>(body, l::BODY_LOCAL_TABLE_SIZE) as usize - param_size;
        let stack_max = field::<u32>(body, l::BODY_STACK_MAX) as usize;

        // vm_push_frame(): the C path raises SystemStackError on overflow.
        let mut sp = argv.add(param_size);
        let new_cfp = cfp.sub(1);
        let bound = sp.add(local_size + stack_max).cast::<rb_control_frame_t>();
        if new_cfp.cast_const() <= bound.add(1).cast_const() {
            return None;
        }

        (*cfp).sp = argv.sub(1);
        for _ in 0..local_size {
            *sp = Qnil;
            sp = sp.add(1);
        }
        *sp = VALUE(cme as usize);
        *sp.add(1) = VALUE(l::VM_BLOCK_HANDLER_NONE as usize);
        *sp.add(2) = VALUE((l::VM_FRAME_MAGIC_METHOD | l::VM_ENV_FLAG_LOCAL) as usize);
        sp = sp.add(3);
        new_cfp.write(rb_control_frame_t {
            pc: field(body, l::BODY_ISEQ_ENCODED),
            sp,
            _iseq: iseq,
            self_: recv,
            ep: sp.sub(1),
            block_code: core::ptr::null(),
            jit_return: core::ptr::null_mut(),
        });
        // vm_push_frame() orders the frame stores before the ec->cfp store.
        core::sync::atomic::compiler_fence(core::sync::atomic::Ordering::SeqCst);
        (*ec).cfp = new_cfp;
        Some(Qundef)
    }
}

/// `vm_sendish(..., VM_BLOCK_HANDLER_NONE, mexp_search_method)`. Returns
/// Qundef after it pushes an iseq frame.
#[inline(always)]
pub unsafe fn send_without_block(ec: EcPtr, cfp: CfpPtr, cd: CALL_DATA) -> VALUE {
    unsafe {
        match fast_send(ec, cfp, cd) {
            Some(val) => val,
            None => rb_yarv_send_without_block(ec, cfp, cd),
        }
    }
}

/// `vm_base_ptr(cfp)` for a method frame. Returns null when the C version
/// must compute it.
#[inline(always)]
unsafe fn base_ptr(cfp: CfpPtr, ep_flags: usize) -> *const VALUE {
    unsafe {
        let iseq = (*cfp)._iseq;
        if iseq.is_null() || ep_flags & l::VM_FRAME_FLAG_CFRAME as usize != 0 {
            return core::ptr::null();
        }
        let body: *const c_void = field(iseq.cast(), l::ISEQ_BODY);
        if field::<u32>(body, l::BODY_PARAM_FLAGS) & l::PARAM_FLAG_FORWARDABLE as u32 != 0 {
            return core::ptr::null();
        }
        let local_size = field::<u32>(body, l::BODY_LOCAL_TABLE_SIZE) as usize;
        let mut bp = (*cfp.add(1)).sp.add(local_size + l::VM_ENV_DATA_SIZE as usize);
        if field::<u32>(body, l::BODY_TYPE) as i64 == l::ISEQ_TYPE_METHOD
            || ep_flags & l::VM_FRAME_FLAG_BMETHOD as usize != 0
        {
            bp = bp.add(1);
        }
        bp
    }
}

/// The body of `leave`: checks the stack (OPT_CHECKED_RUN) and pops the
/// frame. Returns true for a FINISH frame.
#[inline(always)]
pub unsafe fn leave(ec: EcPtr, cfp: CfpPtr) -> bool {
    unsafe {
        if jit_enabled() {
            return rb_yarv_leave(ec, cfp);
        }
        let ep_flags = (*(*cfp).ep).0;
        let bp = base_ptr(cfp, ep_flags);
        if bp.is_null() || (*cfp).sp.cast_const() != bp {
            // The C version computes the uncommon cases and reports errors.
            return rb_yarv_leave(ec, cfp);
        }
        vm_check_ints(ec);
        (*ec).cfp = cfp.add(1);
        ep_flags & l::VM_FRAME_FLAG_FINISH as usize != 0
    }
}
