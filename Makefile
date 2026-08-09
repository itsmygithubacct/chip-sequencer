PROJECT := chip-sequencer
BUILD_DIR ?= build
PREFIX ?= /usr/local
DESTDIR ?=

CC ?= cc
AR ?= ar
INSTALL ?= install

CPPFLAGS += -Iinclude
WARNINGS := \
	-Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2 -Werror
CFLAGS ?= -O2 -g
# Determinism is a hard invariant: -ffp-contract=off keeps FMA contraction from
# perturbing the single float multiply in the render path (part of the pinned
# byte contract). The core links no libm.
override CFLAGS += -std=c11 -fPIC -ffp-contract=off $(WARNINGS)
DEPFLAGS := -MMD -MP

# The core translation unit is POSIX-free, uses only the C11 standard library,
# and needs no libm. The tools TU (MIDI import + C emission) is never linked
# into a shipping game and uses only standard C file I/O.
CORE_OBJS := $(BUILD_DIR)/chip_sequencer.o
TOOLS_OBJS := $(BUILD_DIR)/chipseq_tools.o
LIB_OBJS := $(CORE_OBJS) $(TOOLS_OBJS)

STATIC_LIB := $(BUILD_DIR)/lib$(PROJECT).a
SHARED_LIB := $(BUILD_DIR)/lib$(PROJECT).so
TEST_BIN := $(BUILD_DIR)/test-chipseq
TOOLS_TEST_BIN := $(BUILD_DIR)/test-tools
THREAD_TEST_BIN := $(BUILD_DIR)/test-thread-sanitize
EXAMPLE_BIN := $(BUILD_DIR)/demo
BENCHMARK_BIN := $(BUILD_DIR)/benchmark-chipseq

HEADER := include/chip_sequencer.h

.PHONY: all benchmark clean install sanitize test tsanitize

all: $(STATIC_LIB) $(SHARED_LIB) $(EXAMPLE_BIN)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/chip_sequencer.o: src/chip_sequencer.c $(HEADER) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/chipseq_tools.o: src/chipseq_tools.c $(HEADER) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(LIB_OBJS)
	$(CC) -shared $(LDFLAGS) $^ -o $@

$(TEST_BIN): tests/test_chipseq.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -o $@

$(TOOLS_TEST_BIN): tests/test_tools.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -o $@

$(EXAMPLE_BIN): examples/demo.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDFLAGS) -o $@

$(BENCHMARK_BIN): benchmarks/benchmark_chipseq.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -D_POSIX_C_SOURCE=200809L \
		$< $(STATIC_LIB) $(LDFLAGS) -o $@

benchmark: $(BENCHMARK_BIN)
	$(BENCHMARK_BIN)

test: $(TEST_BIN) $(TOOLS_TEST_BIN)
	$(TEST_BIN)
	cd $(BUILD_DIR) && CHIPSEQ_KEEP_TEST_OUTPUT=1 ./test-tools
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $(BUILD_DIR)/chipseq_tools_out1.c -o $(BUILD_DIR)/generated-song.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -trigraphs -c $(BUILD_DIR)/chipseq_tools_out2.c -o $(BUILD_DIR)/generated-escaped-song.o
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $(BUILD_DIR)/chipseq_tools_out3.c -o $(BUILD_DIR)/generated-empty-song.o
	$(RM) $(BUILD_DIR)/chipseq_tools_out1.c $(BUILD_DIR)/chipseq_tools_out2.c $(BUILD_DIR)/chipseq_tools_out3.c

sanitize: | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g3 -ffp-contract=off $(WARNINGS) \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		src/chip_sequencer.c src/chipseq_tools.c tests/test_chipseq.c \
		-fsanitize=address,undefined -o $(BUILD_DIR)/test-chipseq-sanitize
	cd $(BUILD_DIR) && ASAN_OPTIONS=detect_leaks=1 ./test-chipseq-sanitize
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g3 -ffp-contract=off $(WARNINGS) \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		src/chip_sequencer.c src/chipseq_tools.c tests/test_tools.c \
		-fsanitize=address,undefined -o $(BUILD_DIR)/test-tools-sanitize
	cd $(BUILD_DIR) && ASAN_OPTIONS=detect_leaks=1 ./test-tools-sanitize

tsanitize: | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g3 -ffp-contract=off $(WARNINGS) \
		-fno-omit-frame-pointer -fsanitize=thread -pthread \
		src/chip_sequencer.c tests/test_thread.c \
		-fsanitize=thread -pthread -o $(THREAD_TEST_BIN)
	TSAN_OPTIONS=halt_on_error=1 $(THREAD_TEST_BIN)

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -m 0644 $(HEADER) $(DESTDIR)$(PREFIX)/include/
	$(INSTALL) -m 0644 $(STATIC_LIB) $(SHARED_LIB) $(DESTDIR)$(PREFIX)/lib/

clean:
	rm -rf $(BUILD_DIR)

-include $(LIB_OBJS:.o=.d)
