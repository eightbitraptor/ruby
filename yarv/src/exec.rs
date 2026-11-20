//! The dispatch loop of `vm_exec_core` for call-threaded code.

use crate::cruby::*;

/// `rb_insn_func_t`. Each instruction returns the control frame of the
/// next instruction, or null to leave the loop.
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
