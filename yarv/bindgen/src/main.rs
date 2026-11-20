//! Generates yarv/src/cruby_bindings.inc.rs. Run through `make yarv-bindgen`.
//! The output is checked in, so only allowlist types whose layout is the same
//! on every supported 64-bit platform.

use std::env;
use std::path::PathBuf;

const SRC_ROOT_ENV: &str = "YARV_SRC_ROOT_PATH";

fn main() {
    let src_root = PathBuf::from(env::var(SRC_ROOT_ENV).expect(SRC_ROOT_ENV));
    assert!(src_root.is_dir(), "{SRC_ROOT_ENV} must be the Ruby source root");

    env_logger::init();

    let clang_args = env::args().skip(1).filter(|arg| arg != "-fvisibility=hidden");

    let bindings = bindgen::builder()
        .clang_args(clang_args)
        .header(src_root.join("yarv/bindgen/wrapper.h").to_str().unwrap())
        .rust_target(bindgen::RustTarget::stable(85, 0).unwrap_or_else(|_| panic!("rust target")))
        .rust_edition(bindgen::RustEdition::Edition2024)
        .use_core()
        .ctypes_prefix("core::ffi")
        .generate_comments(false)
        .merge_extern_blocks(true)
        .layout_tests(false)
        .prepend_enum_name(false)
        .translate_enum_integer_types(true)
        .blocklist_type("size_t")
        .blocklist_type("VALUE")
        .blocklist_type("ID")
        .allowlist_recursively(false)
        .allowlist_type("rb_len_t")
        .allowlist_type("vm_frame_env_flags")
        .allowlist_function("rb_gc_writebarrier")
        .allowlist_function("rb_vm_get_insns_address_table")
        .allowlist_function("rb_set_lookup")
        .allowlist_function("rb_to_hash_type")
        .allowlist_var("VM_KW_SPECIFIED_BITS_MAX")
        .allowlist_function("rb_gc_writebarrier_remember")
        .allowlist_type("rb_float_value_type")
        .allowlist_type("rb_iseq_t")
        .allowlist_type("rb_control_frame_t")
        .allowlist_type("rb_control_frame_struct")
        .allowlist_type("RString__.*")
        .allowlist_type("RArray__.*")

        .allowlist_type("ruby_special_consts")
        .allowlist_type("ruby_value_type")
        .allowlist_type("ruby_fl_type")
        .allowlist_type("ruby_fl_ushift")
        .allowlist_type("ruby_rarray_flags")
        .allowlist_type("ruby_rarray_consts")
        .allowlist_type("ruby_basic_operators")
        .allowlist_var(".*_REDEFINED_OP_FLAG")
        .allowlist_var("ruby_vm_redefined_flag")
        .allowlist_type("RBasic")
        .allowlist_type("RArray")
        .allowlist_type("RString")
        .allowlist_type("RFloat")

        .allowlist_var("rb_cArray")
        .allowlist_var("rb_cFalseClass")
        .allowlist_var("rb_cFloat")
        .allowlist_var("rb_cHash")
        .allowlist_var("rb_cInteger")
        .allowlist_var("rb_cNilClass")
        .allowlist_var("rb_cRegexp")
        .allowlist_var("rb_cString")
        .allowlist_var("rb_cSymbol")
        .allowlist_var("rb_cTrueClass")

        .allowlist_function("rb_bug")
        .allowlist_function("rb_int2big")
        .allowlist_function("rb_uint2big")
        .allowlist_function("rb_big_mul")
        .allowlist_function("rb_float_new_in_heap")
        .allowlist_function("rb_flo_div_flo")
        .allowlist_function("ruby_float_mod")
        .allowlist_function("rb_fix_aref")
        .allowlist_function("rb_str_opt_plus")
        .allowlist_function("rb_str_length")
        .allowlist_function("rb_str_buf_append")
        .allowlist_function("rb_str_concat")
        .allowlist_function("rb_str_succ")
        .allowlist_function("rb_ary_plus")
        .allowlist_function("rb_ary_aref1")
        .allowlist_function("rb_ary_store")
        .allowlist_function("rb_ary_push")
        .allowlist_function("rb_hash_aref")
        .allowlist_function("rb_hash_aset")
        .allowlist_function("rb_hash_size_num")
        .allowlist_function("rb_reg_match")
        .allowlist_function("rb_reg_new_from_values")
        .allowlist_function("rb_str_intern")
        .allowlist_function("rb_str_concat_literals")
        .allowlist_function("rb_obj_as_string_result")
        .allowlist_function("rb_ec_str_resurrect")
        .allowlist_function("rb_ec_ary_new_from_values")
        .allowlist_function("rb_ary_cat")
        .allowlist_function("rb_range_new")
        .allowlist_function("rb_gvar_get")
        .allowlist_function("rb_gvar_set")
        .allowlist_function("rb_threadptr_execute_interrupts")

        .generate()
        .expect("Unable to generate bindings");

    let out_path = src_root.join("yarv/src/cruby_bindings.inc.rs");
    bindings.write_to_file(out_path).expect("file output failed");
}
