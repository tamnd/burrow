# burrow's own build, for people working on burrow.
#
# If you are using burrow rather than writing it, you do not want this file.
# You want the amalgamation from the releases page, which is one .c and one .h
# and builds with whatever you already have.
#
# There is no configure step and there never will be. Every platform difference
# is decided at runtime or by a predefined macro the compiler already sets, so
# this file only has to find a compiler and hand it a list of files.

CC      ?= cc
AR      ?= ar
BUILD   ?= build
PREFIX  ?= /usr/local

# The source id goes into the binary so that a bug report can name the commit
# it came from. A dirty tree gets nothing, because a source id that is almost
# right is worse than one that admits it does not know.
SOURCE_ID := $(shell git describe --always --dirty --match 'v[0-9]*' 2>/dev/null || echo unknown)

# C11 without the extensions, because MSVC is a supported compiler and it is
# the one that decides what the floor is. Anything that needs more than C11 is
# behind a runtime check or a feature macro the compiler itself defines.
STD       := -std=c11
INCLUDES  := -Iinclude

# The warning set. It is long because every one of these caught something real
# in some other C project, and it is -Werror because a warning nobody turns into
# an error is a warning everybody scrolls past.
WARNINGS := \
	-Wall \
	-Wextra \
	-Werror \
	-Wpedantic \
	-Wshadow \
	-Wcast-qual \
	-Wcast-align \
	-Wconversion \
	-Wsign-conversion \
	-Wwrite-strings \
	-Wpointer-arith \
	-Wstrict-prototypes \
	-Wmissing-prototypes \
	-Wmissing-declarations \
	-Wredundant-decls \
	-Wundef \
	-Wswitch-enum \
	-Wswitch-default \
	-Wdouble-promotion \
	-Wformat=2 \
	-Wformat-security \
	-Wnull-dereference \
	-Wstack-protector \
	-Wvla \
	-Wno-unused-parameter

# Hardening that costs nothing measurable and turns a class of memory bug into a
# clean crash instead of a foothold. _FORTIFY_SOURCE needs optimisation to do
# anything, so it is only set when we are optimising.
HARDENING := -fstack-protector-strong -fno-common

# Except under Cosmopolitan, where a test built with the stack protector crashes
# on the first instruction of main, reading the canary through a thread pointer
# that is still zero. Asking the preprocessor is the one check that works for
# cosmocc and for the per architecture compilers next to it.
COSMO := $(shell echo | $(CC) -dM -E -x c - 2>/dev/null | grep -c __COSMOPOLITAN__)
ifneq ($(COSMO),0)
  HARDENING := -fno-common
endif

# The compiler's coverage counters, which tests/testing_cover_test.c is built
# with and nothing else is. clang has 8-bit counters and gcc has only trace-pc,
# so this asks for the first and falls back to the second. Empty when neither
# works, and then that test skips.
COVERFLAGS := $(shell for f in -fsanitize-coverage=inline-8bit-counters -fsanitize-coverage=trace-pc; do \
	echo 'int f(int x) { return x ? 1 : 2; }' | $(CC) $$f -x c -c - -o /dev/null 2>/dev/null && { echo $$f; break; }; done)

# The stack walker needs the frame pointer to still be there to walk. Without
# it a panic prints its message and no trace, which is the difference between a
# bug report somebody can act on and one they cannot. It costs a register on
# amd64 and nothing on arm64, macOS has kept frame pointers all along, and the
# Linux distributions have been turning them back on for the same reason: the
# profilers and the debuggers all want them. Anybody building the amalgamation
# who wants tracebacks wants this flag too, which is what burrow/trace.h says.
CFRAME := -fno-omit-frame-pointer
ifeq ($(MODE),debug)
  OPT := -O0 -g3
else
  OPT := -O2 -g
  HARDENING += -D_FORTIFY_SOURCE=2
endif

DEFINES := -DBURROW_SOURCE_ID='"$(SOURCE_ID)"'

# The one optional dependency in the whole project. make BOEHM=1 turns the gc
# backend into a real collector and links it; without it gc_allocator answers
# NULL and nothing else in the tree notices either way. It is a variable rather
# than a configure check because there is nothing to detect: you either asked
# for the collector or you did not.
LDLIBS :=
ifeq ($(BOEHM),1)
  DEFINES += -DBURROW_ENABLE_BOEHM=1
  LDLIBS  += -lgc
endif

# Winsock, on Windows only. The completion port the netpoller runs on is
# kernel32 and comes in for free, so the library itself does not need this. What
# needs it is anything holding a socket to submit an operation on, which is the
# tests today and net later, and putting it here is what saves each of them
# having to ask.
ifeq ($(OS),Windows_NT)
  LDLIBS += -lws2_32
endif

# Forces the portable context switch on a machine that has assembly for it.
# This is not a fallback you would ship, it is how you find out whether a bug is
# in the assembly or above it, and it has to go in DEFINES rather than on one
# compile line because it changes the shape of burrow__MContext and so every
# file that sees the header has to agree about it.
ifeq ($(PORTABLE_CONTEXT),1)
  DEFINES += -DBURROW_PORTABLE_CONTEXT=1
endif

CFLAGS  ?= $(STD) $(OPT) $(WARNINGS) $(HARDENING) $(CFRAME) $(INCLUDES) $(DEFINES)
LDFLAGS ?=

# Threads. -pthread is a compile flag and a link flag at the same time, which is
# why it is on both rules below and why -lpthread on its own is not enough on
# every system. Windows has threads in the CRT and no flag to ask for them, and
# this file is not the Windows build anyway. It is separate from CFLAGS for the
# same reason DEPFLAGS is: CI overrides CFLAGS wholesale.
ifneq ($(OS),Windows_NT)
  THREADS := -pthread
endif

# Two levels is what the layout uses, src/version.c and src/mem/arena.c, and
# spelling them out beats a shell find that behaves differently on every box.
SRCS := $(wildcard src/*.c) $(wildcard src/*/*.c)
OBJS := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(SRCS))
LIB  := $(BUILD)/libburrow.a

# The single file build, which is what most people who use burrow will compile.
# tools/burrow-gen writes burrow.c and burrow.h into $(AMALG_DIR), and
# AMALGAMATION=1 builds the library out of that one file instead of the tree,
# so `make AMALGAMATION=1 test` runs every test against exactly what ships.
# Everything else stays the same: the tests link the same archive and the name
# table is read out of the one object the same way, which is why the copy of
# the table burrow.c carries for itself is turned off here.
AMALG_DIR  := $(BUILD)/amalgamation
AMALG_SRCS := $(AMALG_DIR)/burrow.c $(AMALG_DIR)/burrow.h
AMALG_DEPS := $(SRCS) $(wildcard include/burrow/*.h) $(wildcard include/burrow/*/*.h) \
	$(wildcard src/*/*.h) tools/burrow-gen
ifeq ($(AMALGAMATION),1)
  OBJS := $(BUILD)/obj/burrow.o
endif

# The table of names a traceback prints, read out of the objects above by
# tools/burrow-symtab once they are built. See burrow/symtab.h for what it is
# and what it costs. SYMTAB=0 builds an empty one, which gives back the bare
# addresses and lets the linker drop objects nothing calls.
SYMTAB     ?= 1
SYMTAB_SRC := $(BUILD)/gen/symtab_gen.c
SYMTAB_OBJ := $(BUILD)/obj/gen/symtab_gen.o
ifeq ($(SYMTAB),0)
  SYMTAB_ARGS := --empty
else
  SYMTAB_ARGS :=
endif

TEST_SRCS := $(wildcard tests/*_test.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD)/tests/%,$(TEST_SRCS))

# What burrow-gen tests writes for the fixture in tools/gen-tests/testdata. It
# is checked in, tools/check-gen.sh keeps it current, and it is built and run
# with the tests so the C the generator writes is known to compile everywhere.
GEN_TESTS_FIXTURE := tools/gen-tests/testdata/strconv/gen_tests_fixture_test.c
TEST_BINS += $(BUILD)/tests/gen_tests_fixture_test

# Descriptors that tools/burrow-gen produced from the annotated structs in
# tests/gen. They are checked in and every test binary links them, because
# generating them needs libclang and building burrow must never need libclang.
# tools/check-gen.sh regenerates them and diffs, on a machine that has one.
TEST_GEN := $(wildcard tests/gen/*.c)

# Header dependencies, written by the compiler as it goes. Without these, make
# only rebuilds a .o when its .c changes, so editing a header leaves stale
# objects in the tree and the tests you then run are testing the old code. It is
# separate from CFLAGS because CI overrides CFLAGS wholesale and this should
# survive that.
DEPFLAGS := -MMD -MP
DEPS     := $(OBJS:.o=.d) $(TEST_BINS:=.d)

.PHONY: all lib test check collisions clean install fmt tidy amalgamation

all: lib

lib: $(LIB)

$(LIB): $(OBJS) $(SYMTAB_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(OBJS) $(SYMTAB_OBJ)

# Generated after the objects and before the archive, which is the only order
# that works: the names come out of the objects and the table goes in beside
# them. Nothing else in the tree depends on it, so a change anywhere rebuilds
# one object and relinks.
$(SYMTAB_SRC): $(OBJS) tools/burrow-symtab
	@mkdir -p $(dir $@)
	@tools/burrow-symtab $(SYMTAB_ARGS) -o $@ $(OBJS)

$(SYMTAB_OBJ): $(SYMTAB_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(THREADS) -c $< -o $@

# One run writes both files. burrow.c is the one make tracks, and burrow.h rides
# along with it, because a grouped target needs a newer make than macOS has.
$(AMALG_DIR)/burrow.c: $(AMALG_DEPS)
	@tools/burrow-gen amalgamate --out $(AMALG_DIR) --source-id '$(SOURCE_ID)'

$(AMALG_DIR)/burrow.h: $(AMALG_DIR)/burrow.c
	@:

amalgamation: $(AMALG_SRCS)

$(BUILD)/obj/burrow.o: $(AMALG_DIR)/burrow.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(THREADS) -DBURROW_EXTERNAL_SYMTAB -c $< -o $@

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(THREADS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/tests/%: tests/%.c $(TEST_GEN) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(TEST_EXTRA) $(THREADS) $(DEPFLAGS) -MF $@.d -Itests $< $(TEST_GEN) $(LIB) $(LDLIBS) $(LDFLAGS) -o $@

$(BUILD)/tests/gen_tests_fixture_test: $(GEN_TESTS_FIXTURE) $(TEST_GEN) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(THREADS) $(DEPFLAGS) -MF $@.d -Itests $< $(TEST_GEN) $(LIB) $(LDLIBS) $(LDFLAGS) -o $@

$(BUILD)/tests/testing_cover_test: TEST_EXTRA = $(COVERFLAGS)

-include $(DEPS)

# One line for each binary, the way go test prints a package, and the whole of
# what it wrote only when it failed.
test: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do \
		if ./$$t > $$t.out 2>&1; then printf 'ok\t%s\n' "$${t##*/}"; \
		else cat $$t.out; printf 'FAIL\t%s\n' "$${t##*/}"; fail=1; fi; \
	done; exit $$fail

# What CI runs on a pull request, in the order that fails fastest first.
check:
	@tools/check-banned.sh
	@tools/check-headers.sh
	@tools/check-annotations.sh
	@tools/check-alloc.sh
	@tools/check-globals.sh
	@tools/check-statics.sh
	@tools/check-pal.sh
	@tools/check-gen.sh
	@tools/check-amalg.sh
	@$(MAKE) test

# Not part of check, because what it finds depends on which libraries the
# machine has installed. CI runs it against a fixed set.
collisions: $(LIB)
	@BURROW_LIB=$(LIB) tools/check-collisions.sh

# The differential fuzzer, which runs burrow and Go's own standard library on
# the same inputs and fails on the first byte where they disagree. It needs Go
# and a clang with libFuzzer, so it is not part of check. On a Mac the clang
# that ships with Xcode has no libFuzzer and Homebrew's llvm does:
#
#     make fuzz FUZZ_CC=$(brew --prefix llvm)/bin/clang
#
# make fuzz runs every target for FUZZTIME seconds, or only the one named by
# FUZZ=utf8. make fuzz-replay needs no libFuzzer: it builds with $(CC) and
# replays the committed corpus through the same comparison, which is how a
# crasher that was fixed stays fixed. See fuzz/README.md.
FUZZ_CC      ?= clang
FUZZ_BUILD   ?= build-fuzz
FUZZTIME     ?= 60
FUZZ_ORACLE  := $(FUZZ_BUILD)/oracle.a
FUZZ_TARGETS := $(patsubst fuzz/%.c,%,$(filter-out fuzz/driver.c,$(wildcard fuzz/*.c)))
FUZZ         ?= $(FUZZ_TARGETS)

ifeq ($(shell uname -s),Darwin)
  FUZZ_LDLIBS := -framework CoreFoundation
else
  FUZZ_LDLIBS := -lpthread -ldl -lm
endif

.PHONY: fuzz fuzz-replay fuzz-lib

$(FUZZ_ORACLE): $(wildcard fuzz/oracle/*.go) fuzz/oracle/go.mod
	@mkdir -p $(dir $@)
	cd fuzz/oracle && go build -buildmode=c-archive -o $(abspath $@) .

# burrow itself is built again with coverage instrumentation, so that libFuzzer
# can see which branches of burrow an input reached and not only which
# branches of the driver.
#
# It is a phony target rather than a rule for the archive, because inside the
# child make the archive is $(LIB), and a rule for it here would match there
# and start the child again.
fuzz-lib:
	@$(MAKE) --no-print-directory BUILD=$(FUZZ_BUILD)/burrow CC=$(FUZZ_CC) \
		OPT="-O1 -g" HARDENING="-fno-common -fsanitize=address,fuzzer-no-link" lib

# The replay rule comes first, because the make that ships with macOS takes
# the first pattern that matches rather than the most specific one.
$(FUZZ_BUILD)/replay-%: fuzz/%.c fuzz/driver.c fuzz/fuzz.h $(FUZZ_ORACLE) $(LIB)
	$(CC) -std=c11 -O1 -g $(INCLUDES) -DFUZZ_STANDALONE fuzz/driver.c $< \
		$(LIB) $(FUZZ_ORACLE) $(THREADS) $(FUZZ_LDLIBS) -o $@

$(FUZZ_BUILD)/%: fuzz/%.c fuzz/driver.c fuzz/fuzz.h $(FUZZ_ORACLE) fuzz-lib
	$(FUZZ_CC) -std=c11 -O1 -g $(INCLUDES) -fsanitize=address,fuzzer fuzz/driver.c $< \
		$(FUZZ_BUILD)/burrow/libburrow.a $(FUZZ_ORACLE) $(THREADS) $(FUZZ_LDLIBS) -o $@

# New inputs go into a scratch corpus under the build directory, with the
# committed one read alongside it, so a fuzzing run never writes into the tree.
# A disagreement lands in $(FUZZ_BUILD)/crash-<target>-<hash>.
fuzz: $(addprefix $(FUZZ_BUILD)/,$(FUZZ))
	@for t in $(FUZZ); do \
		mkdir -p $(FUZZ_BUILD)/corpus/$$t fuzz/corpus/$$t && \
		./$(FUZZ_BUILD)/$$t -max_total_time=$(FUZZTIME) -print_final_stats=1 \
			-artifact_prefix=$(FUZZ_BUILD)/crash-$$t- \
			$(FUZZ_BUILD)/corpus/$$t fuzz/corpus/$$t || exit 1; \
	done

fuzz-replay: $(addprefix $(FUZZ_BUILD)/replay-,$(FUZZ))
	@for t in $(FUZZ); do ./$(FUZZ_BUILD)/replay-$$t fuzz/corpus/$$t || exit 1; done

install: $(LIB)
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include/burrow
	install -m 644 $(LIB) $(DESTDIR)$(PREFIX)/lib/
	cp -R include/burrow/. $(DESTDIR)$(PREFIX)/include/burrow/

# --others so that a file you have written but not staged gets formatted too,
# since that is the one you are about to commit.
GIT_FILES := git ls-files --cached --others --exclude-standard

# CI pins clang-format to 20.1.7, because the releases disagree about a handful
# of constructs and a tree formatted by a different one fails the gate. Override
# this if yours lives somewhere else: make fmt CLANG_FORMAT=clang-format-20
CLANG_FORMAT ?= clang-format
CLANG_TIDY   ?= clang-tidy

fmt:
	$(CLANG_FORMAT) -i $(shell $(GIT_FILES) '*.c' '*.h')

tidy:
	$(CLANG_TIDY) $(shell $(GIT_FILES) 'src/*.c' 'src/**/*.c') -- $(CFLAGS)

clean:
	rm -rf $(BUILD)
