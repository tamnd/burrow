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
ASMS := $(wildcard src/*/*.S)
OBJS := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(SRCS)) \
	$(patsubst src/%.S,$(BUILD)/obj/%.asm.o,$(ASMS))
LIB  := $(BUILD)/libburrow.a

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

# Assembly gets its own flags rather than CFLAGS, because most of CFLAGS is
# about C and a compiler handed -Wstrict-prototypes for an assembler file is
# entitled to complain that the argument did nothing. It still needs the
# defines: every .S here is guarded on the same macros the header picks the
# backend with, and an assembler file that disagrees with the header about which
# backend is in use produces a duplicate symbol or a missing one.
ASFLAGS ?= -g $(INCLUDES) $(DEFINES)

TEST_SRCS := $(wildcard tests/*_test.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD)/tests/%,$(TEST_SRCS))

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

.PHONY: all lib test check clean install fmt tidy

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

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(THREADS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/obj/%.asm.o: src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD)/tests/%: tests/%.c $(TEST_GEN) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(THREADS) $(DEPFLAGS) -MF $@.d -Itests $< $(TEST_GEN) $(LIB) $(LDLIBS) $(LDFLAGS) -o $@

-include $(DEPS)

test: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do ./$$t || fail=1; done; exit $$fail

# What CI runs on a pull request, in the order that fails fastest first.
check:
	@tools/check-banned.sh
	@tools/check-headers.sh
	@tools/check-annotations.sh
	@tools/check-alloc.sh
	@tools/check-globals.sh
	@tools/check-pal.sh
	@tools/check-gen.sh
	@$(MAKE) test

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
