#ifndef RUBY_YARV_H
#define RUBY_YARV_H 1
/**********************************************************************

  yarv.h - interface to the Rust port of the YARV interpreter (yarv/)

**********************************************************************/

#include "ruby/ruby.h"
#include "internal/numeric.h"

/* yarv/src/cruby.rs hard-codes these configuration choices. */
STATIC_ASSERT(yarv_sizeof_value, SIZEOF_VALUE == 8);
STATIC_ASSERT(yarv_sizeof_long, SIZEOF_LONG == SIZEOF_VALUE);
STATIC_ASSERT(yarv_use_flonum, USE_FLONUM);
STATIC_ASSERT(yarv_sizeof_double, SIZEOF_DOUBLE <= SIZEOF_VALUE);

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

#endif /* RUBY_YARV_H */
