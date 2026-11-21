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
ALL_CFLAGS  := $(OPT) $(SAN) $(STD) $(WARN) -Iinclude $(CFLAGS)
ALL_LDFLAGS := $(SAN) $(LDFLAGS)
LDLIBS   += -lm

BUILD    ?= build
SRC      := src/util.c src/json.c src/crypto.c src/http.c src/sse.c src/static.c src/ot.c \
            src/session.c src/store.c src/timer.c src/cache.c src/gzip.c src/ws.c
OBJ      := $(SRC:src/%.c=$(BUILD)/%.o)
LIB      := $(BUILD)/libmodxcore.a
TEST_SRC := $(wildcard tests/test_*.c)
TEST_OBJ := $(TEST_SRC:tests/%.c=$(BUILD)/tests/%.o)
HDR      := $(wildcard include/modx/*.h)

.PHONY: all lib test test-unit test-example debug test-debug examples bench bench-quick fuzz fuzz-libfuzzer clean

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

$(BUILD)/loadgen: bench/loadgen.c
	$(CC) $(OPT) $(STD) $(WARN) $(ALL_LDFLAGS) -o $@ bench/loadgen.c $(LDLIBS)

examples: $(BUILD)/todo

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
	  $(FUZZ_CC) -O1 -g $(STD) -Iinclude $(CFLAGS) $(SAN_FLAGS) -o build-fuzz/fuzz_$$t fuzz/fuzz_$$t.c fuzz/seeds_$$t.c fuzz/driver.c $(SRC) $(LDLIBS) || exit 1; \
	done
	for t in $(FUZZ_TARGETS); do \
	  n=$(FUZZ_ITERS); [ $$t = journal ] && n=$$(( $(FUZZ_ITERS) / 50 )); \
	  ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./build-fuzz/fuzz_$$t $$n 2>build-fuzz/$$t.log || { tail -40 build-fuzz/$$t.log; exit 1; }; tail -1 build-fuzz/$$t.log; \
	done

FUZZ_SECONDS ?= 60
fuzz-libfuzzer:
	mkdir -p build-fuzz
	for t in $(FUZZ_TARGETS); do \
	  clang -O1 -g $(STD) -Iinclude $(CFLAGS) -fsanitize=fuzzer,address,undefined -o build-fuzz/lf_$$t fuzz/fuzz_$$t.c $(SRC) $(LDLIBS) || exit 1; \
	  ./build-fuzz/lf_$$t -max_total_time=$(FUZZ_SECONDS) -max_len=65536 || exit 1; \
	done

clean:
	rm -rf build build-debug build-fuzz
