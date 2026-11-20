# -*- mode: makefile-gmake; indent-tabs-mode: t -*-

# Make recipes for the Rust port of the YARV interpreter. The crate is
# no_std and links into libruby as its own static library, independent of
# the YJIT and ZJIT Rust library.

YARV_SRC_FILES = $(wildcard \
	$(top_srcdir)/yarv/src/*.rs \
	$(top_srcdir)/yarv/src/*/*.rs \
	)

YARV_RUSTC_FLAGS = --edition=2024 --crate-name=yarv --crate-type=staticlib \
	-C panic=abort -C opt-level=3 -C codegen-units=1 -C lto -g \
	-C overflow-checks=on

$(YARV_LIB): $(YARV_SRC_FILES) target/.rustc-version
	$(ECHO) 'building $(@F)'
	$(Q) $(MAKEDIRS) $(@D)
	$(gnumake_recursive)$(Q) MACOSX_DEPLOYMENT_TARGET=11.0 \
	    $(RUSTC) $(YARV_RUSTC_FLAGS) '--out-dir=$(@D)' '$(top_srcdir)/yarv/src/lib.rs'

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
