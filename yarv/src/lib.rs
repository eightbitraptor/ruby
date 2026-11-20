//! The Rust port of the YARV interpreter.
//!
//! C code reports Ruby exceptions and other non-local exits with longjmp.
//! A longjmp skips Rust frames and does not run their destructors, so
//! functions in this crate must not hold values that need `Drop` while they
//! call into C. The crate is `no_std` and does not allocate to keep that
//! rule easy to follow.

#![cfg_attr(not(test), no_std)]
#![cfg_attr(yarv_tailcall, feature(explicit_tail_calls))]
#![cfg_attr(yarv_tailcall, allow(incomplete_features))]

pub mod cruby;
pub mod exec;
pub mod insnhelper;
pub mod insns;
pub mod ivar;
pub mod send;

/// The C ABI entry points that tool/ruby_vm/views/yarv_insns.rs.erb
/// generates from insns.def. The build sets `YARV_INSNS_RS`.
#[allow(unused_parens)]
mod generated {
    use crate::cruby::*;
    include!(env!("YARV_INSNS_RS"));
}

#[cfg(not(test))]
#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    let (file, line) = match info.location() {
        Some(loc) => (loc.file(), loc.line()),
        None => ("<unknown>", 0),
    };
    unsafe {
        cruby::rb_bug(
            c"yarv: Rust panic at %.*s:%u".as_ptr(),
            file.len() as core::ffi::c_int,
            file.as_ptr(),
            line,
        )
    }
}
