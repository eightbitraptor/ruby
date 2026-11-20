//! Method calls and returns: the send instructions and `leave`.

use crate::cruby::*;

/// `vm_sendish(..., VM_BLOCK_HANDLER_NONE, mexp_search_method)`. Returns
/// Qundef after it pushes an iseq frame.
#[inline(always)]
pub unsafe fn send_without_block(ec: EcPtr, cfp: CfpPtr, cd: CALL_DATA) -> VALUE {
    unsafe { rb_yarv_send_without_block(ec, cfp, cd) }
}

/// The body of `leave`. Returns true for a FINISH frame.
#[inline(always)]
pub unsafe fn leave(ec: EcPtr, cfp: CfpPtr) -> bool {
    unsafe { rb_yarv_leave(ec, cfp) }
}
