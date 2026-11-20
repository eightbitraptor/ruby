#ifndef RUBY_YARV_H
#define RUBY_YARV_H 1
/**********************************************************************

  yarv.h - interface to the Rust port of the YARV interpreter (yarv/)

**********************************************************************/

#include "ruby/ruby.h"
#include "internal/numeric.h"
#include "vm_core.h"
#include "vm_exec.h"

/* yarv/src/cruby.rs hard-codes these configuration choices. */
STATIC_ASSERT(yarv_sizeof_value, SIZEOF_VALUE == 8);
STATIC_ASSERT(yarv_sizeof_long, SIZEOF_LONG == SIZEOF_VALUE);
STATIC_ASSERT(yarv_use_flonum, USE_FLONUM);
STATIC_ASSERT(yarv_sizeof_double, SIZEOF_DOUBLE <= SIZEOF_VALUE);
#if defined(USE_VM_CLOCK) && USE_VM_CLOCK
# error "yarv/src/cruby.rs: vm_check_ints() does not support USE_VM_CLOCK"
#endif
#ifdef WORDS_BIGENDIAN
# error "yarv/src/ivar.rs reads the packed inline caches as little-endian words"
#endif

/* yarv/src/cruby.rs declares these layouts by hand. */
#define YARV_ASSERT_OFFSET(type, field, off) \
    STATIC_ASSERT(yarv_offset_##field, offsetof(type, field) == (off))
YARV_ASSERT_OFFSET(rb_execution_context_t, vm_stack, 0);
YARV_ASSERT_OFFSET(rb_execution_context_t, vm_stack_size, 8);
YARV_ASSERT_OFFSET(rb_execution_context_t, cfp, 16);
YARV_ASSERT_OFFSET(rb_execution_context_t, tag, 24);
YARV_ASSERT_OFFSET(rb_execution_context_t, interrupt_flag, 32);
YARV_ASSERT_OFFSET(rb_execution_context_t, interrupt_mask, 36);
YARV_ASSERT_OFFSET(rb_execution_context_t, fiber_ptr, 40);
YARV_ASSERT_OFFSET(rb_execution_context_t, thread_ptr, 48);
#undef YARV_ASSERT_OFFSET

/* yarv/src/cruby_bindings.inc.rs has no conditional fields. */
STATIC_ASSERT(yarv_sizeof_control_frame, sizeof(rb_control_frame_t) == 7 * SIZEOF_VALUE);

#if OPT_CALL_THREADED_CODE && (USE_YJIT || USE_ZJIT)
# error "call threaded code runs the Rust entries, which do not run JIT code; configure with --disable-yjit --disable-zjit"
#endif

/* yarv/src/exec.rs. The dispatch loop for OPT_CALL_THREADED_CODE. */
void rb_yarv_exec_loop(rb_execution_context_t *ec, rb_control_frame_t *cfp);

/* C functions that yarv/src calls. */
#if OPT_CALL_THREADED_CODE
void rb_yarv_set_retval(rb_execution_context_t *ec, VALUE val);
#else
NORETURN(void rb_yarv_set_retval(rb_execution_context_t *ec, VALUE val));
#endif
VALUE rb_yarv_send(rb_execution_context_t *ec, rb_control_frame_t *cfp, CALL_DATA cd, ISEQ blockiseq);
VALUE rb_yarv_send_without_block(rb_execution_context_t *ec, rb_control_frame_t *cfp, CALL_DATA cd);
bool rb_yarv_leave(rb_execution_context_t *ec, rb_control_frame_t *cfp);
VALUE rb_vm_getinstancevariable(const rb_iseq_t *iseq, VALUE obj, ID id, IVC ic);
void rb_vm_setinstancevariable(const rb_iseq_t *iseq, VALUE obj, ID id, VALUE val, IVC ic);

/* The instruction handlers in yarv/src/insns.rs, generated from insns.def. */
#include "yarv_insns.h"

#endif /* RUBY_YARV_H */
