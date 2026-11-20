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

VALUE rb_yarv_opt_plus(VALUE recv, VALUE obj);

#endif /* RUBY_YARV_H */
