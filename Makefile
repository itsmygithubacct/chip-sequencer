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
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
CFLAGS ?= -O2 -g
# Determinism is a hard invariant: -ffp-contract=off keeps FMA contraction from
# perturbing the single float multiply in the render path (the byte-contract
# rule in the pinned contract). The core links no libm.
override CFLAGS += -std=c11 -fPIC -ffp-contract=off $(WARNINGS)

# The core translation unit is POSIX-free (only <stdatomic.h> beyond
# freestanding C11) and needs no libm. The tools TU (MIDI import + C emission)
# is never linked into a shipping game and uses only stdio.
CORE_OBJS := $(BUILD_DIR)/chip_sequencer.o
TOOLS_OBJS := $(BUILD_DIR)/chipseq_tools.o
LIB_OBJS := $(CORE_OBJS) $(TOOLS_OBJS)

STATIC_LIB := $(BUILD_DIR)/lib$(PROJECT).a
SHARED_LIB := $(BUILD_DIR)/lib$(PROJECT).so
TEST_BIN := $(BUILD_DIR)/test-chipseq
TOOLS_TEST_BIN := $(BUILD_DIR)/test-tools
EXAMPLE_BIN := $(BUILD_DIR)/demo

HEADER := include/chip_sequencer.h

.PHONY: all clean install sanitize test

all: $(STATIC_LIB) $(SHARED_LIB) $(EXAMPLE_BIN)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/chip_sequencer.o: src/chip_sequencer.c $(HEADER) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/chipseq_tools.o: src/chipseq_tools.c $(HEADER) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

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

test: $(TEST_BIN) $(TOOLS_TEST_BIN)
	$(TEST_BIN)
	$(TOOLS_TEST_BIN)

sanitize: | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g3 -ffp-contract=off $(WARNINGS) \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		src/chip_sequencer.c src/chipseq_tools.c tests/test_chipseq.c \
		-fsanitize=address,undefined -o $(BUILD_DIR)/test-chipseq-sanitize
	ASAN_OPTIONS=detect_leaks=1 $(BUILD_DIR)/test-chipseq-sanitize
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g3 -ffp-contract=off $(WARNINGS) \
		-fno-omit-frame-pointer -fsanitize=address,undefined \
		src/chip_sequencer.c src/chipseq_tools.c tests/test_tools.c \
		-fsanitize=address,undefined -o $(BUILD_DIR)/test-tools-sanitize
	ASAN_OPTIONS=detect_leaks=1 $(BUILD_DIR)/test-tools-sanitize

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -m 0644 $(HEADER) $(DESTDIR)$(PREFIX)/include/
	$(INSTALL) -m 0644 $(STATIC_LIB) $(SHARED_LIB) $(DESTDIR)$(PREFIX)/lib/

clean:
	rm -rf $(BUILD_DIR)
