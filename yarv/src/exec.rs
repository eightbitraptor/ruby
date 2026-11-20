//! Instruction dispatch for call-threaded code.
//!
//! With `OPT_THREADED_CODE=2`, each instruction returns the control frame of
//! the next instruction to `rb_yarv_exec_loop`. With `OPT_THREADED_CODE=3`
//! (`--cfg yarv_tailcall`), each instruction tail calls the next one and
//! passes the PC as an argument, and the loop only starts the chain.

use crate::cruby::*;

/// `rb_insn_func_t` (mode 2) or `rb_tc_insn_func_t` (mode 3) of vm_exec.h.
#[cfg(not(yarv_tailcall))]
pub type InsnFunc = unsafe extern "C" fn(EcPtr, CfpPtr) -> CfpPtr;
#[cfg(yarv_tailcall)]
pub type InsnFunc = unsafe extern "C" fn(EcPtr, CfpPtr, *const VALUE) -> CfpPtr;

/// Runs instructions from `cfp` until an instruction returns null. The
/// caller reads the result from the thread (`retval`) or `ec->errinfo`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rb_yarv_exec_loop(ec: EcPtr, mut cfp: CfpPtr) {
    unsafe {
        while !cfp.is_null() {
            let pc = (*cfp).pc;
            let insn: InsnFunc = core::mem::transmute((*pc).0);
            #[cfg(not(yarv_tailcall))]
            {
                cfp = insn(ec, cfp);
            }
            #[cfg(yarv_tailcall)]
            {
                cfp = insn(ec, cfp, pc);
            }
        }
    }
}

// Each macro has two definitions because the parser rejects `become` on
// stable Rust even in code that `#[cfg]` removes.

/// `CALL_INSN_FUNC(f)` of vm_exec.h: leaves this instruction for the
/// instruction function `f`, which runs the instruction at `pc`.
#[cfg(yarv_tailcall)]
#[macro_export]
macro_rules! call_insn_func {
    ($f:expr, $ec:expr, $cfp:expr, $pc:expr) => {{
        let f: $crate::exec::InsnFunc = $f;
        become f($ec, $cfp, $pc)
    }};
}

#[cfg(not(yarv_tailcall))]
#[macro_export]
macro_rules! call_insn_func {
    ($f:expr, $ec:expr, $cfp:expr, $pc:expr) => {{
        let f: $crate::exec::InsnFunc = $f;
        return f($ec, $cfp)
    }};
}

/// `END_INSN()` of vm_exec.h: continues with the instruction at `pc`, which
/// is also `cfp->pc`.
#[cfg(yarv_tailcall)]
#[macro_export]
macro_rules! next_insn {
    ($ec:expr, $cfp:expr, $pc:expr) => {{
        let pc: *const VALUE = $pc;
        $crate::call_insn_func!(core::mem::transmute((*pc).0), $ec, $cfp, pc)
    }};
}

#[cfg(not(yarv_tailcall))]
#[macro_export]
macro_rules! next_insn {
    ($ec:expr, $cfp:expr, $pc:expr) => {
        return $cfp
    };
}

unsafe extern "C" {
    // vm_exec.c: stores the result of vm_exec_core in the thread.
    fn rb_yarv_set_retval(ec: EcPtr, val: VALUE);
}

/// `leave` of a FINISH frame: ends the dispatch chain with `val` as the
/// result of vm_exec_core.
#[cold]
#[inline(never)]
pub unsafe fn finish(ec: EcPtr, val: VALUE) -> CfpPtr {
    unsafe { rb_yarv_set_retval(ec, val) };
    core::ptr::null_mut()
}

/// The C opt_send_without_block instruction, for `CALL_SIMPLE_METHOD()`.
#[cold]
#[inline(never)]
pub unsafe fn send_without_block_func() -> InsnFunc {
    unsafe {
        let table = rb_vm_get_insns_address_table();
        core::mem::transmute(*table.add(crate::generated::YARVINSN_OPT_SEND_WITHOUT_BLOCK))
    }
}
