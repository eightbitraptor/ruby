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

/* yarv/src/insnhelper.rs. Each function returns Qundef when the
 * instruction must fall back to a method call. */
VALUE rb_yarv_opt_plus(VALUE recv, VALUE obj);
VALUE rb_yarv_opt_minus(VALUE recv, VALUE obj);
VALUE rb_yarv_opt_mult(VALUE recv, VALUE obj);
VALUE rb_yarv_opt_div(VALUE recv, VALUE obj);
VALUE rb_yarv_opt_mod(VALUE recv, VALUE obj);
VALUE rb_yarv_opt_lt(VALUE recv, VALUE obj);
VALUE rb_yarv_opt_le(VALUE recv, VALUE obj);
VALUE rb_yarv_opt_gt(VALUE recv, VALUE obj);
VALUE rb_yarv_opt_ge(VALUE recv, VALUE obj);
VALUE rb_yarv_opt_ltlt(VALUE recv, VALUE obj);
VALUE rb_yarv_opt_and(VALUE recv, VALUE obj);
VALUE rb_yarv_opt_or(VALUE recv, VALUE obj);
VALUE rb_yarv_opt_aref(VALUE recv, VALUE obj);
VALUE rb_yarv_opt_aset(VALUE recv, VALUE obj, VALUE set);
VALUE rb_yarv_opt_length(VALUE recv, unsigned int bop);
VALUE rb_yarv_opt_empty_p(VALUE recv);
VALUE rb_yarv_opt_succ(VALUE recv);
VALUE rb_yarv_opt_regexpmatch2(VALUE recv, VALUE obj);

/* The instruction handlers in yarv/src/insns.rs, generated from insns.def. */
#include "yarv_insns.h"

#endif /* RUBY_YARV_H */
