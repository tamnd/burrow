#!/bin/sh
# Nothing outside src/pal/ talks to an operating system.
#
# That is the first sentence of include/burrow/pal.h and the third bullet of
# docs/design/10-packages-os.md section 2, and a rule stated in two documents
# and checked by neither is a rule that lasts until the first afternoon somebody
# needs a file descriptor. This is the check.
#
# What it looks at is includes and feature test macros, not calls. A file that
# includes <sys/socket.h> is reaching for the system whatever it then does with
# it, and a file that defines _GNU_SOURCE is reaching for something that is not
# C. Both are one line and both are visible in a diff, which is where the
# conversation should happen.
#
# It fails in both directions, the same way tools/check-globals.sh does. A file
# outside src/pal/ that reaches for the system and is not in the exception list
# fails, which is the case everybody expects. A file in the exception list that
# has stopped reaching fails too, which is the case that matters more: the list
# is a record of work not yet done, and a list that can rot is a list that turns
# into a permanent excuse.
#
# What it will miss: a system call reached through a header that is not on the
# list below, and one reached through a burrow header that includes a system
# header itself. The second is the real gap and it is why include/ is scanned as
# well as src/, so that a header cannot be the hiding place.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.

set -eu

cd "$(dirname "$0")/.."

LIST=tools/pal-exceptions.txt

# --others so that a file you have written but not staged is still checked,
# since that is exactly when somebody runs this.
sources=$(git ls-files --cached --others --exclude-standard \
	'src/*.c' 'src/**/*.c' 'src/**/*.h' 'include/**/*.h' 2>/dev/null || true)
[ -n "$sources" ] || exit 0

found=$(mktemp)
listed=$(mktemp)
trap 'rm -f "$found" "$listed"' EXIT INT TERM

# shellcheck disable=SC2086
awk '
# The system headers. Anything under one of these directories counts, so the
# list is prefixes rather than names for the ones that have a directory.
BEGIN {
    split("sys mach linux netinet arpa net bits asm", dirs, " ")
    split("unistd.h fcntl.h pthread.h sched.h signal.h dlfcn.h poll.h dirent.h \
           netdb.h semaphore.h spawn.h grp.h pwd.h termios.h utime.h syscall.h \
           ifaddrs.h langinfo.h monetary.h nl_types.h ucontext.h \
           windows.h winsock2.h ws2tcpip.h winbase.h winnt.h ntsecapi.h \
           processthreadsapi.h synchapi.h memoryapi.h fileapi.h handleapi.h \
           io.h process.h direct.h bcrypt.h iphlpapi.h mswsock.h", names, " ")

    for (i in dirs) dirset[dirs[i]] = 1
    for (i in names) if (names[i] != "") nameset[names[i]] = 1

    # Defining one of these is asking a libc for something that is not C, which
    # is the same reach by a different route.
    split("_POSIX_C_SOURCE _GNU_SOURCE _XOPEN_SOURCE _DEFAULT_SOURCE \
           _BSD_SOURCE _DARWIN_C_SOURCE _CRT_RAND_S _WIN32_WINNT", macros, " ")
    for (i in macros) if (macros[i] != "") macroset[macros[i]] = 1
}

# src/pal/ is the one place allowed to do any of this.
FILENAME ~ /^src\/pal\// { next }

/^[ \t]*#[ \t]*include[ \t]*</ {
    line = $0
    sub(/^[^<]*</, "", line)
    sub(/>.*$/, "", line)

    what = ""
    if (index(line, "/") > 0) {
        dir = line
        sub(/\/.*$/, "", dir)
        if (dir in dirset) what = "<" line ">"
    } else if (line in nameset) {
        what = "<" line ">"
    }

    if (what != "") print FILENAME "\t" what
    next
}

/^[ \t]*#[ \t]*define[ \t]/ {
    name = $0
    sub(/^[ \t]*#[ \t]*define[ \t]+/, "", name)
    sub(/[ \t(].*$/, "", name)
    if (name in macroset) print FILENAME "\t" name
}
' $sources | sort -u >"$found"

# One line per offending file, which is what the list holds. A file that reaches
# for six headers is one entry and not six, because the entry is about the file
# moving behind the PAL and not about any one include.
cut -f1 "$found" | sort -u >"$found.files"

grep -v '^#' "$LIST" 2>/dev/null | grep -v '^[ \t]*$' | cut -f1 | sort -u >"$listed" ||
	true

status=0

while IFS= read -r f; do
	[ -n "$f" ] || continue
	if ! grep -qx "$f" "$listed"; then
		echo "$f: reaches for an operating system outside src/pal/"
		awk -F'\t' -v f="$f" '$1 == f { print "    " $2 }' "$found"
		status=1
	fi
done <"$found.files"

while IFS= read -r f; do
	[ -n "$f" ] || continue
	if ! grep -qx "$f" "$found.files"; then
		echo "$f: is in $LIST and no longer reaches for an operating system"
		echo "    delete the line. the list is only allowed to get shorter."
		status=1
	fi
done <"$listed"

rm -f "$found.files"

if [ "$status" -ne 0 ]; then
	echo
	echo "Everything above src/pal/ is portable C. See include/burrow/pal.h."
	exit 1
fi

n=$(git ls-files --cached --others --exclude-standard 'src/pal/*' 2>/dev/null | wc -l)
left=$(wc -l <"$listed")
printf 'ok\tpal\t%s files in src/pal, %s still to move\n' "$(echo "$n" | tr -d ' ')" \
	"$(echo "$left" | tr -d ' ')"
