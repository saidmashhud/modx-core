# modx-core — C11, libc/POSIX only.
CC       ?= cc
OPT      ?= -O2
SAN      ?=
STD      := -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
WARN     := -Wall -Wextra -Werror
ZLIB     ?= 1
ifeq ($(ZLIB),1)
LDLIBS   += -lz
else
CFLAGS   += -DMODX_NO_ZLIB
endif
ALL_CFLAGS  := $(OPT) $(SAN) $(STD) $(WARN) -pthread -Iinclude $(CFLAGS)
ALL_LDFLAGS := $(SAN) -pthread $(LDFLAGS)
LDLIBS   += -lm

BUILD    ?= build
SRC      := src/util.c src/json.c src/crypto.c src/http.c src/sse.c src/static.c src/ot.c \
            src/session.c src/store.c src/timer.c src/cache.c src/gzip.c src/ws.c src/idem.c src/version.c \
            src/pool.c
OBJ      := $(SRC:src/%.c=$(BUILD)/%.o)
LIB      := $(BUILD)/libmodxcore.a
TEST_SRC := $(wildcard tests/test_*.c)
TEST_OBJ := $(TEST_SRC:tests/%.c=$(BUILD)/tests/%.o)
HDR      := $(wildcard include/modx/*.h)

.PHONY: all lib shared test test-unit test-example debug test-debug tsan test-tsan examples bench bench-quick fuzz fuzz-libfuzzer \
        install install-static install-shared uninstall test-install pc docs clean

all: lib examples

lib: $(LIB)

$(LIB): $(OBJ)
	rm -f $@
	ar rcs $@ $^

$(BUILD)/%.o: src/%.c $(HDR) | $(BUILD)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(BUILD)/tests/%.o: tests/%.c tests/test.h $(HDR) | $(BUILD)/tests
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(BUILD) $(BUILD)/tests:
	mkdir -p $@

$(BUILD)/test_unit: $(TEST_OBJ) $(LIB)
	$(CC) $(ALL_LDFLAGS) -o $@ $(TEST_OBJ) $(LIB) $(LDLIBS)

$(BUILD)/todo: examples/todo/todo.c $(LIB) $(HDR)
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ examples/todo/todo.c $(LIB) $(LDLIBS)

$(BUILD)/minimal: examples/minimal/minimal.c $(LIB) $(HDR)
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ examples/minimal/minimal.c $(LIB) $(LDLIBS)

$(BUILD)/loadgen: bench/loadgen.c
	$(CC) $(OPT) $(STD) $(WARN) $(ALL_LDFLAGS) -o $@ bench/loadgen.c $(LDLIBS)

examples: $(BUILD)/todo $(BUILD)/minimal

test-unit: $(BUILD)/test_unit
	./$(BUILD)/test_unit

test-example: $(BUILD)/todo
	node tests/todo.mjs ./$(BUILD)/todo
	TODO_POLL=1 node tests/todo.mjs ./$(BUILD)/todo

test: test-unit test-example

# ASan + UBSan: separate object dir.
SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=undefined
debug:
	$(MAKE) BUILD=build-debug OPT="-O1 -g -fno-omit-frame-pointer" SAN="$(SAN_FLAGS)" build-debug/test_unit build-debug/todo

test-debug: debug
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./build-debug/test_unit
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 node tests/todo.mjs ./build-debug/todo

# ThreadSanitizer (the worker pool, async password hashing, the in-process server tests): separate object dir.
tsan:
	$(MAKE) BUILD=build-tsan OPT="-O1 -g -fno-omit-frame-pointer" SAN="-fsanitize=thread" build-tsan/test_unit

test-tsan: tsan
	TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 ./build-tsan/test_unit

bench: $(BUILD)/todo $(BUILD)/loadgen
	node bench/run.mjs --profile full

bench-quick: $(BUILD)/todo $(BUILD)/loadgen
	node bench/run.mjs --profile quick

# Fuzzing. `make fuzz` builds every target with the stand-alone driver under
# ASan + UBSan and runs FUZZ_ITERS mutations each (works with Apple clang).
# `make fuzz-libfuzzer` links the same targets against libFuzzer (LLVM clang).
FUZZ_TARGETS := http json journal ws
FUZZ_ITERS   ?= 200000
FUZZ_CC      ?= $(CC)
fuzz:
	mkdir -p build-fuzz
	for t in $(FUZZ_TARGETS); do \
	  $(FUZZ_CC) -O1 -g $(STD) -pthread -Iinclude $(CFLAGS) $(SAN_FLAGS) -o build-fuzz/fuzz_$$t fuzz/fuzz_$$t.c fuzz/seeds_$$t.c fuzz/driver.c $(SRC) $(LDLIBS) || exit 1; \
	done
	for t in $(FUZZ_TARGETS); do \
	  n=$(FUZZ_ITERS); [ $$t = journal ] && n=$$(( $(FUZZ_ITERS) / 50 )); \
	  ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build-fuzz/fuzz_$$t $$n 2>build-fuzz/$$t.log || { tail -40 build-fuzz/$$t.log; exit 1; }; tail -1 build-fuzz/$$t.log; \
	done

FUZZ_SECONDS ?= 60
fuzz-libfuzzer:
	mkdir -p build-fuzz
	for t in $(FUZZ_TARGETS); do \
	  clang -O1 -g $(STD) -pthread -Iinclude $(CFLAGS) -fsanitize=fuzzer,address,undefined -o build-fuzz/lf_$$t fuzz/fuzz_$$t.c $(SRC) $(LDLIBS) || exit 1; \
	  ./build-fuzz/lf_$$t -max_total_time=$(FUZZ_SECONDS) -max_len=65536 || exit 1; \
	done

# ---- install ----
# make install [PREFIX=/usr/local] [DESTDIR=staging] [SHARED=1] [LIBDIR=...] [INCLUDEDIR=...]
#   $(INCLUDEDIR)/modx/*.h, $(LIBDIR)/libmodx.a, libmodx.so.<v> (soname libmodx.so.<abi>) or
#   libmodx.<v>.dylib (install name libmodx.<abi>.dylib), $(PKGCONFIGDIR)/modx-core.pc and a CMake
#   package ($(LIBDIR)/cmake/modx-core: find_package(modx-core) -> modx::core).
# The ABI version is MAJOR, or 0.MINOR while MAJOR is 0.
PREFIX       ?= /usr/local
LIBDIR       ?= $(PREFIX)/lib
INCLUDEDIR   ?= $(PREFIX)/include
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig
SHARED       ?= 1
VER_MAJOR    := $(shell awk '/define MODX_VERSION_MAJOR/ {print $$3}' include/modx/version.h)
VER_MINOR    := $(shell awk '/define MODX_VERSION_MINOR/ {print $$3}' include/modx/version.h)
VER_PATCH    := $(shell awk '/define MODX_VERSION_PATCH/ {print $$3}' include/modx/version.h)
VERSION      := $(VER_MAJOR).$(VER_MINOR).$(VER_PATCH)
ifeq ($(VER_MAJOR),0)
SOVERSION    := 0.$(VER_MINOR)
else
SOVERSION    := $(VER_MAJOR)
endif
UNAME_S      := $(shell uname -s)
PIC_OBJ      := $(SRC:src/%.c=$(BUILD)/pic/%.o)
ifeq ($(UNAME_S),Darwin)
SHLIB        := libmodx.$(VERSION).dylib
SONAME       := libmodx.$(SOVERSION).dylib
SHLINK       := libmodx.dylib
SHLIB_FLAGS  := -dynamiclib -install_name $(LIBDIR)/$(SONAME) -compatibility_version $(SOVERSION) -current_version $(VERSION)
else
SHLIB        := libmodx.so.$(VERSION)
SONAME       := libmodx.so.$(SOVERSION)
SHLINK       := libmodx.so
SHLIB_FLAGS  := -shared -Wl,-soname,$(SONAME)
endif
LIBS_PRIVATE := $(strip $(LDLIBS))

$(BUILD)/pic/%.o: src/%.c $(HDR) | $(BUILD)/pic
	$(CC) $(ALL_CFLAGS) -fPIC -c -o $@ $<

$(BUILD)/pic:
	mkdir -p $@

$(BUILD)/$(SHLIB): $(PIC_OBJ)
	$(CC) $(ALL_LDFLAGS) $(SHLIB_FLAGS) -o $@ $(PIC_OBJ) $(LDLIBS)

shared: $(BUILD)/$(SHLIB)

# regenerated every time: it records PREFIX / LIBDIR / INCLUDEDIR
pc: | $(BUILD)
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|' \
	    -e 's|@VERSION@|$(VERSION)|' -e 's|@LIBS_PRIVATE@|$(LIBS_PRIVATE)|' modx-core.pc.in > $(BUILD)/modx-core.pc

CMAKEDIR     ?= $(LIBDIR)/cmake/modx-core
SUBST        := -e 's|@LIBDIR@|$(LIBDIR)|g' -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' -e 's|@VERSION@|$(VERSION)|g' \
                -e 's|@VER_MAJOR@|$(VER_MAJOR)|g' -e 's|@VER_MINOR@|$(VER_MINOR)|g' -e 's|@ZLIB@|$(ZLIB)|g'

install-static: lib pc
	mkdir -p $(DESTDIR)$(INCLUDEDIR)/modx $(DESTDIR)$(LIBDIR) $(DESTDIR)$(PKGCONFIGDIR) $(DESTDIR)$(CMAKEDIR)
	for h in $(HDR); do install -m 644 $$h $(DESTDIR)$(INCLUDEDIR)/modx/ || exit 1; done
	install -m 644 $(LIB) $(DESTDIR)$(LIBDIR)/libmodx.a
	install -m 644 $(BUILD)/modx-core.pc $(DESTDIR)$(PKGCONFIGDIR)/modx-core.pc
	sed $(SUBST) cmake/modx-core-config-make.cmake.in > $(DESTDIR)$(CMAKEDIR)/modx-core-config.cmake
	sed $(SUBST) cmake/modx-core-config-version-make.cmake.in > $(DESTDIR)$(CMAKEDIR)/modx-core-config-version.cmake

install-shared: shared
	mkdir -p $(DESTDIR)$(LIBDIR)
	install -m 755 $(BUILD)/$(SHLIB) $(DESTDIR)$(LIBDIR)/$(SHLIB)
	ln -sf $(SHLIB) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/$(SHLINK)

ifeq ($(SHARED),1)
install: install-static install-shared
else
install: install-static
endif

uninstall:
	for h in $(notdir $(HDR)); do rm -f $(DESTDIR)$(INCLUDEDIR)/modx/$$h; done
	-rmdir $(DESTDIR)$(INCLUDEDIR)/modx 2>/dev/null
	rm -f $(DESTDIR)$(LIBDIR)/libmodx.a $(DESTDIR)$(LIBDIR)/$(SHLIB) $(DESTDIR)$(LIBDIR)/$(SONAME) \
	      $(DESTDIR)$(LIBDIR)/$(SHLINK) $(DESTDIR)$(PKGCONFIGDIR)/modx-core.pc \
	      $(DESTDIR)$(CMAKEDIR)/modx-core-config.cmake $(DESTDIR)$(CMAKEDIR)/modx-core-config-version.cmake
	-rmdir $(DESTDIR)$(CMAKEDIR) 2>/dev/null

# installs into a temporary prefix and builds examples/install against it with
# pkg-config (shared and static) and CMake (find_package, add_subdirectory)
test-install:
	sh tests/install_test.sh

# API reference from the header comments (Doxygen); skipped when doxygen is not installed.
# Hand-written guides: docs/README.md.
docs:
	@if command -v doxygen >/dev/null 2>&1; then \
	  mkdir -p build/docs && ( cat Doxyfile; echo "PROJECT_NUMBER = $(VERSION)" ) | doxygen - && \
	  echo "API reference: build/docs/html/index.html (warnings: build/docs/warnings.log)"; \
	else \
	  echo "doxygen not found: skipping the generated API reference (hand-written summary: docs/reference.md)"; \
	fi

clean:
	rm -rf build build-debug build-fuzz build-tsan
