# -*- mode: makefile-gmake; indent-tabs-mode: t -*-

# Make recipes for the Rust port of the YARV interpreter. The crate is
# no_std and links into libruby as its own static library, independent of
# the YJIT and ZJIT Rust library.

YARV_SRC_FILES = $(wildcard \
	$(top_srcdir)/yarv/src/*.rs \
	$(top_srcdir)/yarv/src/*/*.rs \
	)

# Overflow checks stay off as in the C interpreter; the Fixnum code uses
# explicit overflowing arithmetic.
YARV_RUSTC_FLAGS = --edition=2024 --crate-name=yarv --crate-type=staticlib \
	-C panic=abort -C opt-level=3 -C codegen-units=1 -C lto -g \
	--check-cfg 'cfg(test)' --check-cfg 'cfg(yarv_tailcall)' \
	--check-cfg 'cfg(yarv_yjit)' --check-cfg 'cfg(yarv_zjit)'

# OPT_THREADED_CODE=3 (tail-call threaded code) needs explicit tail calls,
# which only nightly Rust has.
YARV_NIGHTLY_RUSTC = rustup run nightly rustc
ifneq ($(findstring -DOPT_THREADED_CODE=3,$(CPPFLAGS)),)
YARV_RUSTC = $(YARV_NIGHTLY_RUSTC)
YARV_RUSTC_FLAGS += --cfg yarv_tailcall
else
YARV_RUSTC = $(RUSTC)
endif

# The Rust fast paths step aside when a JIT is enabled at run time.
ifneq ($(YJIT_SUPPORT),no)
YARV_RUSTC_FLAGS += --cfg yarv_yjit
endif
ifneq ($(ZJIT_SUPPORT),no)
YARV_RUSTC_FLAGS += --cfg yarv_zjit
endif

# Struct offsets and constants that depend on the configuration. vm.o
# needs the same headers, so its prerequisites also cover this file.
yarv_layout.rs: $(top_srcdir)/yarv/layout.c vm.$(OBJEXT)
	$(ECHO) generating $@
	$(Q) $(CC) $(CFLAGS) $(XCFLAGS) $(CPPFLAGS) -S -o $(@:.rs=.s) $(top_srcdir)/yarv/layout.c
	$(Q) sed -n 's/.*@@yarv \([A-Z_0-9]*\) \(-*[0-9][0-9]*\).*/pub const \1: i64 = \2;/p' $(@:.rs=.s) > $@
	$(Q) $(RM) $(@:.rs=.s)

$(YARV_LIB): $(YARV_SRC_FILES) yarv_insns.rs yarv_layout.rs target/.rustc-version
	$(ECHO) 'building $(@F)'
	$(Q) $(MAKEDIRS) $(@D)
	$(gnumake_recursive)$(Q) MACOSX_DEPLOYMENT_TARGET=11.0 \
	    YARV_INSNS_RS='$(abspath $(filter %yarv_insns.rs,$^))' \
	    YARV_LAYOUT_RS='$(abspath yarv_layout.rs)' \
	    $(YARV_RUSTC) $(YARV_RUSTC_FLAGS) '--out-dir=$(@D)' '$(top_srcdir)/yarv/src/lib.rs'

YARV_LIB_SYMBOLS = $(YARV_LIB:.a=).symbols
$(YARV_LIBOBJ): $(YARV_LIB)
	$(ECHO) 'partial linking $(YARV_LIB) into $@'
ifneq ($(findstring darwin,$(target_os)),)
	$(Q) $(CC) -nodefaultlibs -r -o $@ -exported_symbols_list $(YARV_LIB_SYMBOLS) $(YARV_LIB)
else
	$(Q) $(LD) -r -o $@ --whole-archive $(YARV_LIB)
	-$(Q) $(OBJCOPY) --wildcard --keep-global-symbol='$(SYMBOL_PREFIX)rb_*' $(@)
endif

ifneq ($(findstring darwin,$(target_os)),)
$(YARV_LIB_SYMBOLS): $(YARV_LIB)
	$(Q) $(tooldir)/darwin-ar $(NM) --defined-only --extern-only $(YARV_LIB) | \
	sed -n -e 's/.* //' -e '/^$(SYMBOL_PREFIX)rb_/p' > $@

$(YARV_LIBOBJ): $(YARV_LIB_SYMBOLS)
endif

.PHONY: yarv-lib yarv-bindgen
yarv-lib: $(YARV_LIBOBJ)

# Regenerates yarv/src/cruby_bindings.inc.rs. Needs cargo and libclang.
YARV_CARGO = $(if $(strip $(CARGO)),$(CARGO),cargo)
yarv-bindgen:
	YARV_SRC_ROOT_PATH='$(top_srcdir)' $(YARV_CARGO) run -q \
	    --manifest-path '$(top_srcdir)/yarv/bindgen/Cargo.toml' -- \
	    $(CFLAGS) $(XCFLAGS) $(CPPFLAGS)
	$(Q) if [ 'x$(HAVE_GIT)' = xyes ]; then $(GIT) -C "$(top_srcdir)" diff --stat yarv/src/cruby_bindings.inc.rs; fi
