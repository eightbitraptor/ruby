#ifndef RBIMPL_RSTRING_H                             /*-*-C++-*-vi:se ft=cpp:*/
#define RBIMPL_RSTRING_H
/**
 * @file
 * @author     Ruby developers <ruby-core@ruby-lang.org>
 * @copyright  This  file  is   a  part  of  the   programming  language  Ruby.
 *             Permission  is hereby  granted,  to  either redistribute  and/or
 *             modify this file, provided that  the conditions mentioned in the
 *             file COPYING are met.  Consult the file for details.
 * @warning    Symbols   prefixed  with   either  `RBIMPL`   or  `rbimpl`   are
 *             implementation details.   Don't take  them as canon.  They could
 *             rapidly appear then vanish.  The name (path) of this header file
 *             is also an  implementation detail.  Do not expect  it to persist
 *             at the place it is now.  Developers are free to move it anywhere
 *             anytime at will.
 * @note       To  ruby-core:  remember  that   this  header  can  be  possibly
 *             recursively included  from extension  libraries written  in C++.
 *             Do not  expect for  instance `__VA_ARGS__` is  always available.
 *             We assume C99  for ruby itself but we don't  assume languages of
 *             extension libraries.  They could be written in C++98.
 * @brief      Defines struct ::RString.
 */
#include "ruby/internal/config.h"
#include "ruby/internal/arithmetic/long.h"
#include "ruby/internal/attr/artificial.h"
#include "ruby/internal/attr/pure.h"
#include "ruby/internal/cast.h"
#include "ruby/internal/core/rbasic.h"
#include "ruby/internal/dllexport.h"
#include "ruby/internal/fl_type.h"
#include "ruby/internal/value_type.h"
#include "ruby/internal/warning_push.h"
#include "ruby/assert.h"

/**
 * Convenient casting macro.
 *
 * @param   obj  An object, which is in fact an ::RString.
 * @return  The passed object casted to ::RString.
 */
#define RSTRING(obj)            RBIMPL_CAST((struct RString *)(obj))

/** @cond INTERNAL_MACRO */
#define RSTRING_NOEMBED         RSTRING_NOEMBED
#define RSTRING_FSTR            RSTRING_FSTR
#define RSTRING_LEN       RSTRING_LEN
#define RSTRING_LENINT    RSTRING_LENINT
#define RSTRING_PTR       RSTRING_PTR
#define RSTRING_END       RSTRING_END
/** @endcond */

/**
 * @name Conversion of Ruby strings into C's
 *
 * @{
 */

/**
 * Ensures that the parameter object is a  String.  This is done by calling its
 * `to_str` method.
 *
 * @param[in,out]  v              Arbitrary Ruby object.
 * @exception      rb_eTypeError  No implicit conversion defined.
 * @post           `v` is a String.
 */
#define StringValue(v)     rb_string_value(&(v))

/**
 * Identical to #StringValue, except it returns a `char*`.
 *
 * @param[in,out]  v              Arbitrary Ruby object.
 * @exception      rb_eTypeError  No implicit conversion defined.
 * @return         Converted Ruby string's backend C string.
 * @post           `v` is a String.
 */
#define StringValuePtr(v)  rb_string_value_ptr(&(v))

/**
 * Identical to #StringValuePtr, except it additionally checks for the contents
 * for viability  as a C  string.  Ruby can accept  wider range of  contents as
 * strings, compared to C.  This function is to check that.
 *
 * @param[in,out]  v              Arbitrary Ruby object.
 * @exception      rb_eTypeError  No implicit conversion defined.
 * @exception      rb_eArgError   String is not C-compatible.
 * @return         Converted Ruby string's backend C string.
 * @post           `v` is a String.
 */
#define StringValueCStr(v) rb_string_value_cstr(&(v))

/**
 * @private
 *
 * @deprecated  This macro once was a thing in the old days, but makes no sense
 *              any  longer today.   Exists  here  for backwards  compatibility
 *              only.  You can safely forget about it.
 */
#define SafeStringValue(v) StringValue(v)

/**
 * Identical  to #StringValue,  except  it additionally  converts the  string's
 * encoding to default external encoding.  Ruby has a concept called encodings.
 * A string can have different  encoding than the environment expects.  Someone
 * has to make  sure its contents be converted to  something suitable.  This is
 * that routine.  Call it when necessary.
 *
 * @param[in,out]  v              Arbitrary Ruby object.
 * @exception      rb_eTypeError  No implicit conversion defined.
 * @return         Converted Ruby string's backend C string.
 * @post           `v` is a String.
 *
 * @internal
 *
 * Not   sure  but   it  seems   this  macro   does  not   raise  on   encoding
 * incompatibilities?  Doesn't sound right to @shyouhei.
 */
#define ExportStringValue(v) do { \
    StringValue(v);               \
    (v) = rb_str_export(v);       \
} while (0)

/** @} */

/**
 * @private
 *
 * Bits that you can set to ::RBasic::flags.
 *
 * @warning  These enums are not the only bits we use for strings.
 *
 * @internal
 *
 * Actually all bits  through FL_USER1 to FL_USER19 are used  for strings.  Why
 * only this  tiny part of  them are made public  here?  @shyouhei can  find no
 * reason.
 */
enum ruby_rstring_flags {

    /**
     * This flag has  something to do with memory footprint.   If the string is
     * short enough, ruby tries to be  creative to abuse padding bits of struct
     * ::RString for  storing contents.  If this  flag is set that  string does
     * _not_  do that,  to resort  to  good old  fashioned external  allocation
     * strategy instead.
     *
     * @warning  This  bit has  to be  considered read-only.   Setting/clearing
     *           this  bit without  corresponding fix  up must  cause immediate
     *           SEGV.    Also,  internal   structures  of   a  string   change
     *           dynamically  and  transparently  throughout of  its  lifetime.
     *           Don't assume it being persistent.
     *
     * @internal
     *
     * 3rd parties must  not be aware that  there even is more than  one way to
     * store a string.  Might better be hidden.
     */
    RSTRING_NOEMBED         = RUBY_FL_USER1,

    /**
     * The string shares its buffer with another string (the "shared root").
     * When set, the `as.shared` union member holds the root VALUE and `capa`
     * holds a byte offset into the root's buffer.
     */
    RSTRING_SHARED          = RUBY_FL_USER0,

    /**
     * This flag has something to do with infamous "fstring". Fstrings are
     * immutable, globally deduped, and managed by our GC.
     */
    RSTRING_FSTR            = RUBY_FL_USER17
};

/**
 * Ruby's String.  A string in ruby conceptually has these information:
 *
 * - Encoding of the string.
 * - Length of the string.
 * - Contents of the string.
 *
 * It is worth  noting that a string  is _not_ an array of  characters in ruby.
 * It has never been.   In 1.x a string was an array of  integers.  Since 2.x a
 * string is no longer an array of anything.  A string is a string -- just like
 * a Time is not an integer.
 */
struct RString {

    /** Basic part, including flags and class. */
    struct RBasic basic;

    /**
     * Length of the string, not including terminating NUL character.
     * Limited to UINT32_MAX (~4GB).
     *
     * @note  This is in bytes.
     */
    uint32_t len;

    /**
     * Multi-purpose field, meaning depends on flags:
     * - !NOEMBED (embedded): unused
     * - NOEMBED && !SHARED: heap buffer capacity
     * - NOEMBED && SHARED: byte offset into shared root's buffer
     * - PRECOMPUTED_HASH: precomputed hash value (32-bit)
     */
    uint32_t capa;

    /** String's specific fields. */
    union {
        /** Pointer to heap-allocated buffer (independent heap strings). */
        char *ptr;

        /** Shared root VALUE (shared strings; RSTRING_SHARED flag set). */
        VALUE shared;

        /**
         * Embedded contents (length 1 array for C compatibility).
         * @see https://gcc.gnu.org/bugzilla/show_bug.cgi?id=102452
         */
        char ary[1];
    } as;
};

RBIMPL_SYMBOL_EXPORT_BEGIN()
/**
 * Identical to rb_check_string_type(), except it  raises exceptions in case of
 * conversion failures.
 *
 * @param[in]  obj            Target object.
 * @exception  rb_eTypeError  No implicit conversion to String.
 * @return     Return value of `obj.to_str`.
 * @see        rb_io_get_io
 * @see        rb_ary_to_ary
 */
VALUE rb_str_to_str(VALUE obj);

/**
 * Identical to  rb_str_to_str(), except it  fills the passed pointer  with the
 * converted object.
 *
 * @param[in,out]  ptr            Pointer to a variable of target object.
 * @exception      rb_eTypeError  No implicit conversion to String.
 * @return         Return value of `obj.to_str`.
 * @post           `*ptr` is the return value.
 */
VALUE rb_string_value(volatile VALUE *ptr);

/**
 * Identical  to  rb_str_to_str(), except  it  returns  the converted  string's
 * backend memory region.
 *
 * @param[in,out]  ptr            Pointer to a variable of target object.
 * @exception      rb_eTypeError  No implicit conversion to String.
 * @post           `*ptr` is the return value of `obj.to_str`.
 * @return         Pointer to the contents of the return value.
 */
char *rb_string_value_ptr(volatile VALUE *ptr);

/**
 * Identical to  rb_string_value_ptr(), except  it additionally checks  for the
 * contents  for viability  as a  C  string.  Ruby  can accept  wider range  of
 * contents as strings, compared to C.  This function is to check that.
 *
 * @param[in,out]  ptr            Pointer to a variable of target object.
 * @exception      rb_eTypeError  No implicit conversion to String.
 * @exception      rb_eArgError   String is not C-compatible.
 * @post           `*ptr` is the return value of `obj.to_str`.
 * @return         Pointer to the contents of the return value.
 */
char *rb_string_value_cstr(volatile VALUE *ptr);

/**
 * Identical  to rb_str_to_str(),  except it  additionally converts  the string
 * into default  external encoding.   Ruby has a  concept called  encodings.  A
 * string can  have different encoding  than the environment  expects.  Someone
 * has to make  sure its contents be converted to  something suitable.  This is
 * that routine.  Call it when necessary.
 *
 * @param[in]  obj            Target object.
 * @exception  rb_eTypeError  No implicit conversion to String.
 * @return     Converted ruby string of default external encoding.
 */
VALUE rb_str_export(VALUE obj);

/**
 * Identical to  rb_str_export(), except it  converts into the  locale encoding
 * instead.
 *
 * @param[in]  obj            Target object.
 * @exception  rb_eTypeError  No implicit conversion to String.
 * @return     Converted ruby string of locale encoding.
 */
VALUE rb_str_export_locale(VALUE obj);

RBIMPL_ATTR_ERROR(("rb_check_safe_str() and Check_SafeStr() are obsolete; use StringValue() instead"))
/**
 * @private
 *
 * @deprecated  This function  once was a thing  in the old days,  but makes no
 *              sense   any   longer   today.   Exists   here   for   backwards
 *              compatibility only.  You can safely forget about it.
 */
void rb_check_safe_str(VALUE);

/**
 * @private
 *
 * @deprecated  This macro once was a thing in the old days, but makes no sense
 *              any  longer today.   Exists  here  for backwards  compatibility
 *              only.  You can safely forget about it.
 */
#define Check_SafeStr(v) rb_check_safe_str(RBIMPL_CAST((VALUE)(v)))

/**
 * @private
 *
 * Prints diagnostic message to stderr when RSTRING_PTR or RSTRING_END
 * is NULL.
 *
 * @param[in]  func           The function name where encountered NULL pointer.
 */
void rb_debug_rstring_null_ptr(const char *func);
RBIMPL_SYMBOL_EXPORT_END()

RBIMPL_ATTR_PURE_UNLESS_DEBUG()
RBIMPL_ATTR_ARTIFICIAL()
/**
 * Queries the length of the string.
 *
 * @param[in]  str  String in question.
 * @return     Its length, in bytes.
 * @pre        `str` must be an instance of ::RString.
 */
static inline long
RSTRING_LEN(VALUE str)
{
    return (long)RSTRING(str)->len;
}

RBIMPL_ATTR_ARTIFICIAL()
/**
 * Queries the contents pointer of the string.
 *
 * @param[in]  str  String in question.
 * @return     Pointer to its contents.
 * @pre        `str` must be an instance of ::RString.
 */
static inline char *
RSTRING_PTR(VALUE str)
{
    char *ptr;

    if (!RB_FL_TEST_RAW(str, RSTRING_NOEMBED)) {
        ptr = RSTRING(str)->as.ary;
    }
    else if (RB_UNLIKELY(RB_FL_TEST_RAW(str, RSTRING_SHARED))) {
        VALUE root = RSTRING(str)->as.shared;
        RUBY_ASSERT(RB_FL_TEST_RAW(root, RUBY_T_MASK) == RUBY_T_STRING);
        ptr = (RB_FL_TEST_RAW(root, RSTRING_NOEMBED) ?
            RSTRING(root)->as.ptr : RSTRING(root)->as.ary)
            + RSTRING(str)->capa;
    }
    else {
        ptr = RSTRING(str)->as.ptr;
    }

    if (RUBY_DEBUG && RB_UNLIKELY(! ptr)) {
        rb_debug_rstring_null_ptr("RSTRING_PTR");
    }

    return ptr;
}

RBIMPL_ATTR_ARTIFICIAL()
/**
 * Queries the end of the contents pointer of the string.
 *
 * @param[in]  str  String in question.
 * @return     Pointer to its end of contents.
 * @pre        `str` must be an instance of ::RString.
 */
static inline char *
RSTRING_END(VALUE str)
{
    return RSTRING_PTR(str) + RSTRING_LEN(str);
}

RBIMPL_ATTR_ARTIFICIAL()
/**
 * Identical to RSTRING_LEN(), except it differs for the return type.
 *
 * @param[in]  str             String in question.
 * @exception  rb_eRangeError  Too long.
 * @return     Its length, in bytes.
 * @pre        `str` must be an instance of ::RString.
 *
 * @internal
 *
 * This API seems redundant but has actual usages.
 */
static inline int
RSTRING_LENINT(VALUE str)
{
    return rb_long2int(RSTRING_LEN(str));
}

/**
 * Convenient macro to obtain the contents and length at once.
 *
 * @param  str     String in question.
 * @param  ptrvar  Variable where its contents is stored.
 * @param  lenvar  Variable where its length is stored.
 */
# define RSTRING_GETMEM(str, ptrvar, lenvar) \
    ((ptrvar) = RSTRING_PTR(str),           \
     (lenvar) = RSTRING_LEN(str))
#endif /* RBIMPL_RSTRING_H */
