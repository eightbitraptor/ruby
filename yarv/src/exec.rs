//! Instruction dispatch for call-threaded code.
//!
//! With `OPT_THREADED_CODE=2`, each instruction returns the control frame of
//! the next instruction to `rb_yarv_exec_loop`. With `OPT_THREADED_CODE=3`
//! (`--cfg yarv_tailcall`), each instruction tail calls the next one, and
//! the loop only starts the chain.

use crate::cruby::*;

/// `rb_insn_func_t`.
pub type InsnFunc = unsafe extern "C" fn(EcPtr, CfpPtr) -> CfpPtr;

/// Runs instructions from `cfp` until an instruction returns null. The
/// caller reads the result from the thread (`retval`) or `ec->errinfo`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rb_yarv_exec_loop(ec: EcPtr, mut cfp: CfpPtr) {
    unsafe {
        while !cfp.is_null() {
            let insn: InsnFunc = core::mem::transmute((*(*cfp).pc).0);
            cfp = insn(ec, cfp);
        }
    }
}

// Each macro has two definitions because the parser rejects `become` on
// stable Rust even in code that `#[cfg]` removes.

/// `CALL_INSN_FUNC(f)` of vm_exec.h: leaves this instruction for the
/// instruction function `f`.
#[cfg(yarv_tailcall)]
#[macro_export]
macro_rules! call_insn_func {
    ($f:expr, $ec:expr, $cfp:expr) => {{
        let f: $crate::exec::InsnFunc = $f;
        become f($ec, $cfp)
    }};
}

#[cfg(not(yarv_tailcall))]
#[macro_export]
macro_rules! call_insn_func {
    ($f:expr, $ec:expr, $cfp:expr) => {{
        let f: $crate::exec::InsnFunc = $f;
        return f($ec, $cfp)
    }};
}

/// `END_INSN()` of vm_exec.h: continues with the instruction at `cfp->pc`.
#[cfg(yarv_tailcall)]
#[macro_export]
macro_rules! next_insn {
    ($ec:expr, $cfp:expr) => {
        $crate::call_insn_func!(core::mem::transmute((*(*$cfp).pc).0), $ec, $cfp)
    };
}

#[cfg(not(yarv_tailcall))]
#[macro_export]
macro_rules! next_insn {
    ($ec:expr, $cfp:expr) => {
        $cfp
    };
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
