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
ifeq ($(MODE),debug)
  OPT := -O0 -g3
else
  OPT := -O2 -g
  HARDENING += -D_FORTIFY_SOURCE=2
endif

DEFINES := -DBURROW_SOURCE_ID='"$(SOURCE_ID)"'

CFLAGS  ?= $(STD) $(OPT) $(WARNINGS) $(HARDENING) $(INCLUDES) $(DEFINES)
LDFLAGS ?=

# Two levels is what the layout uses, src/version.c and src/mem/arena.c, and
# spelling them out beats a shell find that behaves differently on every box.
SRCS := $(wildcard src/*.c) $(wildcard src/*/*.c)
OBJS := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(SRCS))
LIB  := $(BUILD)/libburrow.a

TEST_SRCS := $(wildcard tests/*_test.c)
TEST_BINS := $(patsubst tests/%.c,$(BUILD)/tests/%,$(TEST_SRCS))

.PHONY: all lib test check clean install fmt tidy

all: lib

lib: $(LIB)

$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $(OBJS)

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tests/%: tests/%.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests $< $(LIB) $(LDFLAGS) -o $@

test: $(TEST_BINS)
	@fail=0; for t in $(TEST_BINS); do ./$$t || fail=1; done; exit $$fail

# What CI runs on a pull request, in the order that fails fastest first.
check:
	@tools/check-banned.sh
	@tools/check-headers.sh
	@$(MAKE) test

install: $(LIB)
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include/burrow
	install -m 644 $(LIB) $(DESTDIR)$(PREFIX)/lib/
	cp -R include/burrow/. $(DESTDIR)$(PREFIX)/include/burrow/

fmt:
	clang-format -i $(shell git ls-files '*.c' '*.h')

tidy:
	clang-tidy $(shell git ls-files 'src/*.c') -- $(CFLAGS)

clean:
	rm -rf $(BUILD)
