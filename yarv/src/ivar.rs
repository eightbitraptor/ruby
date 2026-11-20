//! Instance variable access: getinstancevariable, setinstancevariable, and
//! the attr_reader/attr_writer methods (yarv/src/send.rs).
//!
//! The fast paths handle a T_OBJECT receiver whose shape matches the
//! inline cache, as vm_getivar() and vm_setivar() do in C. Every other case
//! goes to the C functions.

use core::ffi::c_void;

use crate::cruby::layout as l;
use crate::cruby::*;

/// `RBASIC_SHAPE_ID(obj)`.
#[inline(always)]
unsafe fn shape_id(obj: VALUE) -> u32 {
    unsafe {
        if l::RBASIC_SHAPE_ID_FIELD != 0 {
            field(obj.0 as *const c_void, l::RBASIC_SHAPE_ID)
        } else {
            ((*obj.as_rbasic()).flags.0 >> l::SHAPE_FLAG_SHIFT) as u32
        }
    }
}

/// `RBASIC_SET_FULL_SHAPE_ID(obj, id)`.
#[inline(always)]
unsafe fn set_full_shape_id(obj: VALUE, id: u32) {
    unsafe {
        if l::RBASIC_SHAPE_ID_FIELD != 0 {
            *(obj.0 as *mut u8).add(l::RBASIC_SHAPE_ID as usize).cast::<u32>() = id;
        } else {
            let flags = &raw mut (*(obj.0 as *mut RBasic)).flags;
            let low = (*flags).0 & ((1usize << l::SHAPE_FLAG_SHIFT) - 1);
            *flags = VALUE(low | ((id as usize) << l::SHAPE_FLAG_SHIFT));
        }
    }
}

/// `RBASIC_SET_SHAPE_ID(obj, id)`: keeps the object-specific bits.
#[inline(always)]
unsafe fn set_shape_id(obj: VALUE, id: u32) {
    unsafe {
        let private = l::SHAPE_ID_FL_PRIVATE_MASK as u32;
        set_full_shape_id(obj, (id & !private) | (shape_id(obj) & private));
    }
}

/// `ROBJECT_FIELDS_OBJ(obj)` and the field array of the result.
#[inline(always)]
unsafe fn object_fields(obj: VALUE, obj_shape_id: u32) -> (VALUE, *mut VALUE) {
    unsafe {
        let embedded = obj_shape_id & l::SHAPE_ID_LAYOUT_MASK as u32 == l::SHAPE_ID_LAYOUT_ROBJECT as u32;
        let base = obj.0 as *mut u8;
        if embedded {
            (obj, base.add(l::ROBJECT_AS_ARY as usize).cast())
        } else {
            let fields_obj: VALUE = field(base.cast_const().cast(), l::ROBJECT_AS_EXTENDED);
            (fields_obj, (fields_obj.0 as *mut u8).add(l::FIELDS_EMBED as usize).cast())
        }
    }
}

/// Reads a packed ivar cache: the `value` of an IVC, or `aux_.attr.value`
/// of a call cache.
#[inline(always)]
unsafe fn read_cache(cache: *const u64) -> u64 {
    unsafe {
        core::sync::atomic::AtomicU64::from_ptr(cache.cast_mut())
            .load(core::sync::atomic::Ordering::Relaxed)
    }
}

#[inline(always)]
fn attr_index(packed: u64, byte_offset: i64) -> u32 {
    ((packed >> (8 * byte_offset)) & ((1u64 << l::ATTR_INDEX_T_NUM_BITS) - 1)) as u32
}

/// The cache hit path of vm_getivar() for a T_OBJECT. Returns None when the
/// C version must run.
#[inline(always)]
pub unsafe fn get_fast(obj: VALUE, cache: *const u64) -> Option<VALUE> {
    unsafe {
        if obj.special_const_p() || obj.builtin_type() != RUBY_T_OBJECT {
            return None;
        }
        let (fields_obj, fields) = object_fields(obj, shape_id(obj));
        let read_shape = shape_id(fields_obj) & l::SHAPE_ID_READ_ONLY_MASK as u32;
        // rb_getivar_cache_unpack()
        let packed = read_cache(cache);
        let index = attr_index(packed, l::GETIVAR_CACHE_INDEX);
        let cached_shape =
            (packed >> (8 * l::GETIVAR_CACHE_SHAPE_OFFSET)) as u32 & l::SHAPE_ID_OFFSET_MASK as u32;
        if cached_shape != read_shape {
            return None;
        }
        if index == l::ATTR_INDEX_NOT_SET as u32 {
            return Some(Qnil);
        }
        Some(*fields.add(index as usize))
    }
}

/// The cache hit path of vm_setivar() for an embedded T_OBJECT. Returns
/// false, with no change, when the C version must run.
#[inline(always)]
pub unsafe fn set_fast(obj: VALUE, val: VALUE, cache: *const u64) -> bool {
    unsafe {
        if obj.special_const_p() || obj.builtin_type() != RUBY_T_OBJECT {
            return false;
        }
        // rb_setivar_cache_unpack()
        let packed = read_cache(cache);
        let index = attr_index(packed, 0);
        let offset_mask = l::SHAPE_ID_OFFSET_MASK as u64;
        let source = (packed >> l::ATTR_INDEX_T_NUM_BITS) & offset_mask;
        let dest = (packed >> (l::ATTR_INDEX_T_NUM_BITS + l::SHAPE_ID_OFFSET_NUM_BITS)) & offset_mask;

        // rb_setivar_cache_revalidate(). The fast path only handles an
        // embedded object, and only uses its embedded capacity, which is at
        // most RSHAPE_CAPACITY(). A frozen object fails the source check.
        let id_now = shape_id(obj);
        let embedded = id_now & l::SHAPE_ID_LAYOUT_MASK as u32 == l::SHAPE_ID_LAYOUT_ROBJECT as u32;
        let capacity = (id_now & l::SHAPE_ID_CAPACITY_MASK as u32) >> l::SHAPE_ID_CAPACITY_OFFSET;
        if !embedded || (id_now & l::SHAPE_ID_WRITE_MASK as u32) as u64 != source || index >= capacity {
            return false;
        }
        let (_, fields) = object_fields(obj, id_now);
        // RB_OBJ_WRITE()
        *fields.add(index as usize) = val;
        if !val.special_const_p() {
            rb_gc_writebarrier(obj, val);
        }
        // rb_shape_transition_offset()
        let dest_id = (id_now & l::SHAPE_ID_FLAGS_MASK as u32) | dest as u32;
        if id_now != dest_id {
            set_shape_id(obj, dest_id);
        }
        true
    }
}

#[inline(always)]
unsafe fn ic_cache(ic: IVC) -> *const u64 {
    unsafe { (ic as *const u8).add(l::IVC_VALUE as usize).cast() }
}

/// `vm_getinstancevariable()`.
#[inline(always)]
pub unsafe fn get(cfp: CfpPtr, id: ID, ic: IVC) -> VALUE {
    unsafe {
        let obj = (*cfp).self_;
        match get_fast(obj, ic_cache(ic)) {
            Some(val) => val,
            None => rb_vm_getinstancevariable((*cfp)._iseq, obj, id, ic),
        }
    }
}

/// `vm_setinstancevariable()`.
#[inline(always)]
pub unsafe fn set(cfp: CfpPtr, id: ID, val: VALUE, ic: IVC) {
    unsafe {
        let obj = (*cfp).self_;
        if !set_fast(obj, val, ic_cache(ic)) {
            rb_vm_setinstancevariable((*cfp)._iseq, obj, id, val, ic);
        }
    }
}
