# modx-core — C11, libc/POSIX only.
CC       ?= cc
OPT      ?= -O2
SAN      ?=
STD      := -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
WARN     := -Wall -Wextra -Werror
ALL_CFLAGS  := $(OPT) $(SAN) $(STD) $(WARN) -Iinclude $(CFLAGS)
ALL_LDFLAGS := $(SAN) $(LDFLAGS)
LDLIBS   += -lm

BUILD    ?= build
SRC      := src/util.c src/json.c src/crypto.c src/http.c src/sse.c src/static.c src/ot.c \
            src/session.c src/store.c src/timer.c
OBJ      := $(SRC:src/%.c=$(BUILD)/%.o)
LIB      := $(BUILD)/libmodxcore.a
TEST_SRC := $(wildcard tests/test_*.c)
TEST_OBJ := $(TEST_SRC:tests/%.c=$(BUILD)/tests/%.o)
HDR      := $(wildcard include/modx/*.h)

.PHONY: all lib test test-unit test-example debug test-debug examples bench bench-quick clean

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

clean:
	rm -rf build build-debug
