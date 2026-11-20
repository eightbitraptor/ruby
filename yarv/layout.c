/**********************************************************************

  yarv/layout.c - struct offsets and constants for yarv/src

  The build compiles this file with -S and turns each YARV_CONST line of
  the assembly into `pub const NAME: i64 = VALUE;` in yarv_layout.rs.
  It covers layouts that depend on the configuration, so that the Rust
  code does not hard-code them.

**********************************************************************/

#include "vm_core.h"
#include "vm_callinfo.h"
#include "method.h"
#include "shape.h"
#include "internal/imemo.h"

#define YARV_CONST(name, value) \
    __asm__ volatile(".ascii \"\\n@@yarv " #name " %c0\\n\"" :: "i"((long)(value)))

#define YARV_OFFSET(name, type, field) YARV_CONST(name, offsetof(type, field))

/* The value of a one-bit bit-field as an integer of the containing word. */
#define YARV_BIT(type, field) \
    (((union { type f; unsigned int u; }){ .f = { .field = 1 } }).u)

typedef __typeof__(((struct rb_iseq_constant_body *)0)->param.flags) yarv_param_flags;

void
rb_yarv_layout(void)
{
    YARV_OFFSET(ISEQ_BODY, struct rb_iseq_struct, body);

    YARV_OFFSET(BODY_TYPE, struct rb_iseq_constant_body, type);
    YARV_OFFSET(BODY_ISEQ_ENCODED, struct rb_iseq_constant_body, iseq_encoded);
    YARV_OFFSET(BODY_PARAM_FLAGS, struct rb_iseq_constant_body, param.flags);
    YARV_OFFSET(BODY_PARAM_SIZE, struct rb_iseq_constant_body, param.size);
    YARV_OFFSET(BODY_PARAM_LEAD_NUM, struct rb_iseq_constant_body, param.lead_num);
    YARV_OFFSET(BODY_LOCAL_TABLE_SIZE, struct rb_iseq_constant_body, local_table_size);
    YARV_OFFSET(BODY_STACK_MAX, struct rb_iseq_constant_body, stack_max);
    YARV_OFFSET(BODY_BUILTIN_ATTRS, struct rb_iseq_constant_body, builtin_attrs);
    YARV_CONST(ISEQ_TYPE_METHOD, ISEQ_TYPE_METHOD);
    YARV_CONST(BUILTIN_ATTR_SINGLE_NOARG_LEAF, BUILTIN_ATTR_SINGLE_NOARG_LEAF);

    /* The parameter kinds that rb_simple_iseq_p() rejects. */
    YARV_CONST(PARAM_FLAGS_NOT_SIMPLE,
               YARV_BIT(yarv_param_flags, has_opt) |
               YARV_BIT(yarv_param_flags, has_rest) |
               YARV_BIT(yarv_param_flags, has_post) |
               YARV_BIT(yarv_param_flags, has_kw) |
               YARV_BIT(yarv_param_flags, has_kwrest) |
               YARV_BIT(yarv_param_flags, accepts_no_kwarg) |
               YARV_BIT(yarv_param_flags, forwardable) |
               YARV_BIT(yarv_param_flags, has_block) |
               YARV_BIT(yarv_param_flags, accepts_no_block));
    YARV_CONST(PARAM_FLAG_FORWARDABLE, YARV_BIT(yarv_param_flags, forwardable));

    YARV_OFFSET(CD_CI, struct rb_call_data, ci);
    YARV_OFFSET(CD_CC, struct rb_call_data, cc);
    YARV_OFFSET(CI_FLAG, struct rb_callinfo, flag);
    YARV_OFFSET(CI_ARGC, struct rb_callinfo, argc);
    YARV_CONST(USE_EMBED_CI, USE_EMBED_CI);
    YARV_CONST(CI_EMBED_ARGC_SHFT, CI_EMBED_ARGC_SHFT);
    YARV_CONST(CI_EMBED_ARGC_MASK, CI_EMBED_ARGC_MASK);
    YARV_CONST(CI_EMBED_FLAG_SHFT, CI_EMBED_FLAG_SHFT);
    YARV_CONST(CI_EMBED_FLAG_MASK, CI_EMBED_FLAG_MASK);
    YARV_CONST(VM_CALL_ARGS_SIMPLE, VM_CALL_ARGS_SIMPLE);
    YARV_CONST(VM_CALL_FCALL, VM_CALL_FCALL);
    YARV_CONST(VM_CALL_VCALL, VM_CALL_VCALL);

    YARV_OFFSET(CC_KLASS, struct rb_callcache, klass);
    YARV_OFFSET(CC_CME, struct rb_callcache, cme_);

    YARV_OFFSET(CME_FLAGS, rb_callable_method_entry_t, flags);
    YARV_OFFSET(CME_DEF, rb_callable_method_entry_t, def);
    YARV_CONST(CME_FLAG_INVALIDATED, IMEMO_FL_USER5);
    YARV_CONST(CME_VISI_MASK, IMEMO_FL_USER0 | IMEMO_FL_USER1);
    YARV_CONST(CME_VISI_SHIFT, IMEMO_FL_USHIFT);
    YARV_CONST(METHOD_VISI_PUBLIC, METHOD_VISI_PUBLIC);
    YARV_CONST(METHOD_VISI_PRIVATE, METHOD_VISI_PRIVATE);
    YARV_CONST(DEF_TYPE_MASK, (1 << VM_METHOD_TYPE_MINIMUM_BITS) - 1);
    YARV_OFFSET(DEF_ISEQPTR, rb_method_definition_t, body.iseq.iseqptr);
    YARV_CONST(VM_METHOD_TYPE_ISEQ, VM_METHOD_TYPE_ISEQ);
    YARV_CONST(VM_METHOD_TYPE_IVAR, VM_METHOD_TYPE_IVAR);
    YARV_CONST(VM_METHOD_TYPE_ATTRSET, VM_METHOD_TYPE_ATTRSET);
    YARV_OFFSET(CC_AUX, struct rb_callcache, aux_.attr.value);

    YARV_CONST(VM_FRAME_MAGIC_METHOD, VM_FRAME_MAGIC_METHOD);
    YARV_CONST(VM_ENV_FLAG_LOCAL, VM_ENV_FLAG_LOCAL);
    YARV_CONST(VM_FRAME_FLAG_FINISH, VM_FRAME_FLAG_FINISH);
    YARV_CONST(VM_FRAME_FLAG_BMETHOD, VM_FRAME_FLAG_BMETHOD);
    YARV_CONST(VM_FRAME_FLAG_CFRAME, VM_FRAME_FLAG_CFRAME);
    YARV_CONST(VM_ENV_DATA_SIZE, VM_ENV_DATA_SIZE);
    YARV_CONST(VM_BLOCK_HANDLER_NONE, VM_BLOCK_HANDLER_NONE);
    YARV_CONST(SIZEOF_CONTROL_FRAME, sizeof(rb_control_frame_t));

    YARV_CONST(RBASIC_SHAPE_ID_FIELD, RBASIC_SHAPE_ID_FIELD);
#if RBASIC_SHAPE_ID_FIELD
    YARV_OFFSET(RBASIC_SHAPE_ID, struct RBasic, shape_id);
#else
    YARV_CONST(RBASIC_SHAPE_ID, 0);
#endif
    YARV_CONST(SHAPE_FLAG_SHIFT, SHAPE_FLAG_SHIFT);
    YARV_CONST(SHAPE_ID_OFFSET_NUM_BITS, SHAPE_ID_OFFSET_NUM_BITS);
    YARV_CONST(SHAPE_ID_OFFSET_MASK, SHAPE_ID_OFFSET_MASK);
    YARV_CONST(SHAPE_ID_CAPACITY_MASK, SHAPE_ID_CAPACITY_MASK);
    YARV_CONST(SHAPE_ID_CAPACITY_OFFSET, SHAPE_ID_CAPACITY_OFFSET);
    YARV_CONST(SHAPE_ID_LAYOUT_MASK, SHAPE_ID_LAYOUT_MASK);
    YARV_CONST(SHAPE_ID_LAYOUT_ROBJECT, SHAPE_ID_LAYOUT_ROBJECT);
    YARV_CONST(SHAPE_ID_FLAGS_MASK, SHAPE_ID_FLAGS_MASK);
    YARV_CONST(SHAPE_ID_FL_PRIVATE_MASK, SHAPE_ID_FL_PRIVATE_MASK);
    YARV_CONST(SHAPE_ID_READ_ONLY_MASK, SHAPE_ID_READ_ONLY_MASK);
    YARV_CONST(SHAPE_ID_WRITE_MASK, SHAPE_ID_WRITE_MASK);
    YARV_CONST(ATTR_INDEX_NOT_SET, ATTR_INDEX_NOT_SET);
    YARV_CONST(ATTR_INDEX_T_NUM_BITS, ATTR_INDEX_T_NUM_BITS);
    YARV_OFFSET(GETIVAR_CACHE_INDEX, rb_getivar_cache, index);
    YARV_OFFSET(GETIVAR_CACHE_SHAPE_OFFSET, rb_getivar_cache, shape_offset);
    YARV_OFFSET(IVC_VALUE, struct iseq_inline_iv_cache_entry, value);
    YARV_OFFSET(ROBJECT_AS_ARY, struct RObject, as.ary);
    YARV_OFFSET(ROBJECT_AS_EXTENDED, struct RObject, as.extended);
    YARV_OFFSET(FIELDS_EMBED, struct rb_fields, as.embed.fields);
}
