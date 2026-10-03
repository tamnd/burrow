/* Derived from Go's src/path/filepath/path_test.go, match_test.go and
 * path_windows_test.go. Go source: go1.27.1.
 *
 * The tables came out of burrow-gen tests and the test bodies were ported by
 * hand. Go runs its Windows tables only on Windows. Here every test runs twice,
 * once with the Unix rules and once with the Windows rules, through the
 * functions in src/path/filepath_internal.h, so both sets of tables run on
 * every machine. The tests that need os (Abs, EvalSymlinks, Glob and Walk) are
 * in filepath_os_test.c. The tests after TestIssue52476 are not from Go.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/path/filepath.h"

#include "../src/path/filepath_internal.h"

#include <string.h>

#define S BURROW_S
#define SI BURROW_S_INIT
#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

#if defined(BURROW_OS_WINDOWS)
#define HOST_WIN true
#else
#define HOST_WIN false
#endif

typedef struct PathTest {
    Str path;
    Str result;
} PathTest;

typedef struct IsLocalTest {
    Str path;
    bool isLocal;
} IsLocalTest;

typedef struct LocalizeTest {
    Str path;
    Str want;
} LocalizeTest;

typedef struct SplitListTest {
    Str list;
    const Str *result;
    Int result_len;
} SplitListTest;

typedef struct SplitTest {
    Str path;
    Str dir;
    Str file;
} SplitTest;

typedef struct JoinTest {
    const Str *elem;
    Int elem_len;
    Str path;
} JoinTest;

typedef struct ExtTest {
    Str path;
    Str ext;
} ExtTest;

typedef struct IsAbsTest {
    Str path;
    bool isAbs;
} IsAbsTest;

typedef struct RelTests {
    Str root;
    Str path;
    Str want;
} RelTests;

typedef struct VolumeNameTest {
    Str path;
    Str vol;
} VolumeNameTest;

static const PathTest cleantests[] = {
    {SI("abc"), SI("abc")},
    {SI("abc/def"), SI("abc/def")},
    {SI("a/b/c"), SI("a/b/c")},
    {SI("."), SI(".")},
    {SI(".."), SI("..")},
    {SI("../.."), SI("../..")},
    {SI("../../abc"), SI("../../abc")},
    {SI("/abc"), SI("/abc")},
    {SI("/"), SI("/")},
    {SI(""), SI(".")},
    {SI("abc/"), SI("abc")},
    {SI("abc/def/"), SI("abc/def")},
    {SI("a/b/c/"), SI("a/b/c")},
    {SI("./"), SI(".")},
    {SI("../"), SI("..")},
    {SI("../../"), SI("../..")},
    {SI("/abc/"), SI("/abc")},
    {SI("abc//def//ghi"), SI("abc/def/ghi")},
    {SI("abc//"), SI("abc")},
    {SI("abc/./def"), SI("abc/def")},
    {SI("/./abc/def"), SI("/abc/def")},
    {SI("abc/."), SI("abc")},
    {SI("abc/def/ghi/../jkl"), SI("abc/def/jkl")},
    {SI("abc/def/../ghi/../jkl"), SI("abc/jkl")},
    {SI("abc/def/.."), SI("abc")},
    {SI("abc/def/../.."), SI(".")},
    {SI("/abc/def/../.."), SI("/")},
    {SI("abc/def/../../.."), SI("..")},
    {SI("/abc/def/../../.."), SI("/")},
    {SI("abc/def/../../../ghi/jkl/../../../mno"), SI("../../mno")},
    {SI("/../abc"), SI("/abc")},
    {SI("a/../b:/../../c"), SI("../c")},
    {SI("abc/./../def"), SI("def")},
    {SI("abc//./../def"), SI("def")},
    {SI("abc/../../././../def"), SI("../../def")},
};

static const PathTest nonwincleantests[] = {
    {SI("//abc"), SI("/abc")},
    {SI("///abc"), SI("/abc")},
    {SI("//abc//"), SI("/abc")},
};

static const PathTest wincleantests[] = {
    {SI("c:"), SI("c:.")},
    {SI("c:\\"), SI("c:\\")},
    {SI("c:\\abc"), SI("c:\\abc")},
    {SI("c:abc\\..\\..\\.\\.\\..\\def"), SI("c:..\\..\\def")},
    {SI("c:\\abc\\def\\..\\.."), SI("c:\\")},
    {SI("c:\\..\\abc"), SI("c:\\abc")},
    {SI("c:..\\abc"), SI("c:..\\abc")},
    {SI("c:\\b:\\..\\..\\..\\d"), SI("c:\\d")},
    {SI("\\"), SI("\\")},
    {SI("/"), SI("\\")},
    {SI("\\\\i\\..\\c$"), SI("\\c$")},
    {SI("\\\\i\\..\\i\\c$"), SI("\\i\\c$")},
    {SI("\\\\i\\..\\I\\c$"), SI("\\I\\c$")},
    {SI("\\\\..\\..\\a"), SI("\\a")},
    {SI("//../../a"), SI("\\a")},
    {SI("\\\\host\\share\\foo\\..\\bar"), SI("\\\\host\\share\\bar")},
    {SI("//host/share/foo/../baz"), SI("\\\\host\\share\\baz")},
    {SI("\\\\host\\share\\foo\\..\\..\\..\\..\\bar"), SI("\\\\host\\share\\bar")},
    {SI("\\\\?\\UNC\\host\\share\\foo\\..\\..\\..\\..\\bar"),
     SI("\\\\?\\UNC\\host\\share\\bar")},
    {SI("\\?\?\\UNC\\host\\share\\foo\\..\\..\\..\\..\\bar"),
     SI("\\?\?\\UNC\\host\\share\\bar")},
    {SI("\\\\.\\C:\\a\\..\\..\\..\\..\\bar"), SI("\\\\.\\C:\\bar")},
    {SI("\\\\.\\C:\\\\\\\\a"), SI("\\\\.\\C:\\a")},
    {SI("\\\\a\\b\\..\\c"), SI("\\\\a\\b\\c")},
    {SI("\\\\a\\b"), SI("\\\\a\\b")},
    {SI(".\\c:"), SI(".\\c:")},
    {SI(".\\c:\\foo"), SI(".\\c:\\foo")},
    {SI(".\\c:foo"), SI(".\\c:foo")},
    {SI("//abc"), SI("\\\\abc")},
    {SI("///abc"), SI("\\\\\\abc")},
    {SI("//abc//"), SI("\\\\abc\\\\")},
    {SI("\\\\?\\C:\\"), SI("\\\\?\\C:\\")},
    {SI("\\\\?\\C:\\a"), SI("\\\\?\\C:\\a")},
    {SI("a/../c:"), SI(".\\c:")},
    {SI("a\\..\\c:"), SI(".\\c:")},
    {SI("a/../c:/a"), SI(".\\c:\\a")},
    {SI("a/../../c:"), SI("..\\c:")},
    {SI("foo:bar"), SI("foo:bar")},
    {SI("/a/../?\?/a"), SI("\\.\\?\?\\a")},
};

static const IsLocalTest islocaltests[] = {
    {SI(""), false},
    {SI("."), true},
    {SI(".."), false},
    {SI("../a"), false},
    {SI("/"), false},
    {SI("/a"), false},
    {SI("/a/../.."), false},
    {SI("a"), true},
    {SI("a/../a"), true},
    {SI("a/"), true},
    {SI("a/."), true},
    {SI("a/./b/./c"), true},
    {SI("a/../b:/../../c"), false},
};

static const IsLocalTest winislocaltests[] = {
    {SI("NUL"), false},     {SI("nul"), false},      {SI("nul "), false},
    {SI("nul."), false},    {SI("a/nul:"), false},   {SI("a/nul : a"), false},
    {SI("com0"), true},     {SI("com1"), false},     {SI("com2"), false},
    {SI("com3"), false},    {SI("com4"), false},     {SI("com5"), false},
    {SI("com6"), false},    {SI("com7"), false},     {SI("com8"), false},
    {SI("com9"), false},    {SI("com¹"), false},     {SI("com²"), false},
    {SI("com³"), false},    {SI("com¹ : a"), false}, {SI("cOm1"), false},
    {SI("lpt1"), false},    {SI("LPT1"), false},     {SI("lpt³"), false},
    {SI("./nul"), false},   {SI("\\"), false},       {SI("\\a"), false},
    {SI("C:"), false},      {SI("C:\\a"), false},    {SI("..\\a"), false},
    {SI("a/../c:"), false}, {SI("CONIN$"), false},   {SI("conin$"), false},
    {SI("CONOUT$"), false}, {SI("conout$"), false},  {SI("dollar$"), true},
};

static const LocalizeTest localizetests[] = {
    {SI(""), SI("")},
    {SI("."), SI(".")},
    {SI(".."), SI("")},
    {SI("a/.."), SI("")},
    {SI("/"), SI("")},
    {SI("/a"), SI("")},
    {SI("a\xff"
        "b"),
     SI("")},
    {SI("a/"), SI("")},
    {SI("a/./b"), SI("")},
    {SI("\x00"), SI("")},
    {SI("a"), SI("a")},
    {SI("a/b/c"), SI("a/b/c")},
};

static const LocalizeTest unixlocalizetests[] = {
    {SI("#a"), SI("#a")},
    {SI("a\\b:c"), SI("a\\b:c")},
};

static const LocalizeTest winlocalizetests[] = {
    {SI("#a"), SI("#a")},  {SI("c:"), SI("")},     {SI("a\\b"), SI("")},
    {SI("a:b"), SI("")},   {SI("a/b:c"), SI("")},  {SI("NUL"), SI("")},
    {SI("a/NUL"), SI("")}, {SI("./com1"), SI("")}, {SI("a/nul/b"), SI("")},
};

static const SplitListTest winsplitlisttests[] = {
    {SI("\"a\""), (const Str[]){SI("a")}, 1},
    {SI("\";\""), (const Str[]){SI(";")}, 1},
    {SI("\"a;b\""), (const Str[]){SI("a;b")}, 1},
    {SI("\";\";"), (const Str[]){SI(";"), SI("")}, 2},
    {SI(";\";\""), (const Str[]){SI(""), SI(";")}, 2},
    {SI("a\";\"b"), (const Str[]){SI("a;b")}, 1},
    {SI("a; \"\"b"), (const Str[]){SI("a"), SI(" b")}, 2},
    {SI("\"a;b"), (const Str[]){SI("a;b")}, 1},
    {SI("\"\"a;b"), (const Str[]){SI("a"), SI("b")}, 2},
    {SI("\"\"\"a;b"), (const Str[]){SI("a;b")}, 1},
    {SI("\"\"\"\"a;b"), (const Str[]){SI("a"), SI("b")}, 2},
    {SI("a\";b"), (const Str[]){SI("a;b")}, 1},
    {SI("a;b\";c"), (const Str[]){SI("a"), SI("b;c")}, 2},
    {SI("\"a\";b\";c"), (const Str[]){SI("a"), SI("b;c")}, 2},
};

static const SplitTest unixsplittests[] = {
    {SI("a/b"), SI("a/"), SI("b")}, {SI("a/b/"), SI("a/b/"), SI("")},
    {SI("a/"), SI("a/"), SI("")},   {SI("a"), SI(""), SI("a")},
    {SI("/"), SI("/"), SI("")},
};

static const SplitTest winsplittests[] = {
    {SI("c:"), SI("c:"), SI("")},
    {SI("c:/"), SI("c:/"), SI("")},
    {SI("c:/foo"), SI("c:/"), SI("foo")},
    {SI("c:/foo/bar"), SI("c:/foo/"), SI("bar")},
    {SI("//host/share"), SI("//host/share"), SI("")},
    {SI("//host/share/"), SI("//host/share/"), SI("")},
    {SI("//host/share/foo"), SI("//host/share/"), SI("foo")},
    {SI("\\\\host\\share"), SI("\\\\host\\share"), SI("")},
    {SI("\\\\host\\share\\"), SI("\\\\host\\share\\"), SI("")},
    {SI("\\\\host\\share\\foo"), SI("\\\\host\\share\\"), SI("foo")},
};

static const JoinTest jointests[] = {
    {NULL, 0, SI("")},
    {(const Str[]){SI("")}, 1, SI("")},
    {(const Str[]){SI("/")}, 1, SI("/")},
    {(const Str[]){SI("a")}, 1, SI("a")},
    {(const Str[]){SI("a"), SI("b")}, 2, SI("a/b")},
    {(const Str[]){SI("a"), SI("")}, 2, SI("a")},
    {(const Str[]){SI(""), SI("b")}, 2, SI("b")},
    {(const Str[]){SI("/"), SI("a")}, 2, SI("/a")},
    {(const Str[]){SI("/"), SI("a/b")}, 2, SI("/a/b")},
    {(const Str[]){SI("/"), SI("")}, 2, SI("/")},
    {(const Str[]){SI("/a"), SI("b")}, 2, SI("/a/b")},
    {(const Str[]){SI("a"), SI("/b")}, 2, SI("a/b")},
    {(const Str[]){SI("/a"), SI("/b")}, 2, SI("/a/b")},
    {(const Str[]){SI("a/"), SI("b")}, 2, SI("a/b")},
    {(const Str[]){SI("a/"), SI("")}, 2, SI("a")},
    {(const Str[]){SI(""), SI("")}, 2, SI("")},
    {(const Str[]){SI("/"), SI("a"), SI("b")}, 3, SI("/a/b")},
};

static const JoinTest nonwinjointests[] = {
    {(const Str[]){SI("//"), SI("a")}, 2, SI("/a")},
};

static const JoinTest winjointests[] = {
    {(const Str[]){SI("directory"), SI("file")}, 2, SI("directory\\file")},
    {(const Str[]){SI("C:\\Windows\\"), SI("System32")}, 2,
     SI("C:\\Windows\\System32")},
    {(const Str[]){SI("C:\\Windows\\"), SI("")}, 2, SI("C:\\Windows")},
    {(const Str[]){SI("C:\\"), SI("Windows")}, 2, SI("C:\\Windows")},
    {(const Str[]){SI("C:"), SI("a")}, 2, SI("C:a")},
    {(const Str[]){SI("C:"), SI("a\\b")}, 2, SI("C:a\\b")},
    {(const Str[]){SI("C:"), SI("a"), SI("b")}, 3, SI("C:a\\b")},
    {(const Str[]){SI("C:"), SI(""), SI("b")}, 3, SI("C:b")},
    {(const Str[]){SI("C:"), SI(""), SI(""), SI("b")}, 4, SI("C:b")},
    {(const Str[]){SI("C:"), SI("")}, 2, SI("C:.")},
    {(const Str[]){SI("C:"), SI(""), SI("")}, 3, SI("C:.")},
    {(const Str[]){SI("C:"), SI("\\a")}, 2, SI("C:\\a")},
    {(const Str[]){SI("C:"), SI(""), SI("\\a")}, 3, SI("C:\\a")},
    {(const Str[]){SI("C:."), SI("a")}, 2, SI("C:a")},
    {(const Str[]){SI("C:a"), SI("b")}, 2, SI("C:a\\b")},
    {(const Str[]){SI("C:a"), SI("b"), SI("d")}, 3, SI("C:a\\b\\d")},
    {(const Str[]){SI("\\\\host\\share"), SI("foo")}, 2, SI("\\\\host\\share\\foo")},
    {(const Str[]){SI("\\\\host\\share\\foo")}, 1, SI("\\\\host\\share\\foo")},
    {(const Str[]){SI("//host/share"), SI("foo/bar")}, 2,
     SI("\\\\host\\share\\foo\\bar")},
    {(const Str[]){SI("\\")}, 1, SI("\\")},
    {(const Str[]){SI("\\"), SI("")}, 2, SI("\\")},
    {(const Str[]){SI("\\"), SI("a")}, 2, SI("\\a")},
    {(const Str[]){SI("\\\\"), SI("a")}, 2, SI("\\\\a")},
    {(const Str[]){SI("\\"), SI("a"), SI("b")}, 3, SI("\\a\\b")},
    {(const Str[]){SI("\\\\"), SI("a"), SI("b")}, 3, SI("\\\\a\\b")},
    {(const Str[]){SI("\\"), SI("\\\\a\\b"), SI("c")}, 3, SI("\\a\\b\\c")},
    {(const Str[]){SI("\\\\a"), SI("b"), SI("c")}, 3, SI("\\\\a\\b\\c")},
    {(const Str[]){SI("\\\\a\\"), SI("b"), SI("c")}, 3, SI("\\\\a\\b\\c")},
    {(const Str[]){SI("//"), SI("a")}, 2, SI("\\\\a")},
    {(const Str[]){SI("a:\\b\\c"), SI("x\\..\\y:\\..\\..\\z")}, 2, SI("a:\\b\\z")},
    {(const Str[]){SI("\\"), SI("?\?\\a")}, 2, SI("\\.\\?\?\\a")},
};

static const ExtTest exttests[] = {
    {SI("path.go"), SI(".go")}, {SI("path.pb.go"), SI(".go")},
    {SI("a.dir/b"), SI("")},    {SI("a.dir/b.go"), SI(".go")},
    {SI("a.dir/"), SI("")},
};

static const PathTest basetests[] = {
    {SI(""), SI(".")},        {SI("."), SI(".")},         {SI("/."), SI(".")},
    {SI("/"), SI("/")},       {SI("////"), SI("/")},      {SI("x/"), SI("x")},
    {SI("abc"), SI("abc")},   {SI("abc/def"), SI("def")}, {SI("a/b/.x"), SI(".x")},
    {SI("a/b/c."), SI("c.")}, {SI("a/b/c.x"), SI("c.x")},
};

static const PathTest winbasetests[] = {
    {SI("c:\\"), SI("\\")},
    {SI("c:."), SI(".")},
    {SI("c:\\a\\b"), SI("b")},
    {SI("c:a\\b"), SI("b")},
    {SI("c:a\\b\\c"), SI("c")},
    {SI("\\\\host\\share\\"), SI("\\")},
    {SI("\\\\host\\share\\a"), SI("a")},
    {SI("\\\\host\\share\\a\\b"), SI("b")},
};

static const PathTest dirtests[] = {
    {SI(""), SI(".")},         {SI("."), SI(".")},         {SI("/."), SI("/")},
    {SI("/"), SI("/")},        {SI("/foo"), SI("/")},      {SI("x/"), SI("x")},
    {SI("abc"), SI(".")},      {SI("abc/def"), SI("abc")}, {SI("a/b/.x"), SI("a/b")},
    {SI("a/b/c."), SI("a/b")}, {SI("a/b/c.x"), SI("a/b")},
};

static const PathTest nonwindirtests[] = {
    {SI("////"), SI("/")},
};

static const PathTest windirtests[] = {
    {SI("c:\\"), SI("c:\\")},
    {SI("c:."), SI("c:.")},
    {SI("c:\\a\\b"), SI("c:\\a")},
    {SI("c:a\\b"), SI("c:a")},
    {SI("c:a\\b\\c"), SI("c:a\\b")},
    {SI("\\\\host\\share"), SI("\\\\host\\share")},
    {SI("\\\\host\\share\\"), SI("\\\\host\\share\\")},
    {SI("\\\\host\\share\\a"), SI("\\\\host\\share\\")},
    {SI("\\\\host\\share\\a\\b"), SI("\\\\host\\share\\a")},
    {SI("\\\\\\\\"), SI("\\\\\\\\")},
};

static const IsAbsTest isabstests[] = {
    {SI(""), false},   {SI("/"), true},        {SI("/usr/bin/gcc"), true},
    {SI(".."), false}, {SI("/a/../bb"), true}, {SI("."), false},
    {SI("./"), false}, {SI("lala"), false},
};

static const IsAbsTest winisabstests[] = {
    {SI("C:\\"), true},
    {SI("c\\"), false},
    {SI("c::"), false},
    {SI("c:"), false},
    {SI("/"), false},
    {SI("\\"), false},
    {SI("\\Windows"), false},
    {SI("c:a\\b"), false},
    {SI("c:\\a\\b"), true},
    {SI("c:/a/b"), true},
    {SI("\\\\host\\share"), true},
    {SI("\\\\host\\share\\"), true},
    {SI("\\\\host\\share\\foo"), true},
    {SI("//host/share/foo/bar"), true},
    {SI("\\\\..\\..\\a"), false},
    {SI("//../../a"), false},
    {SI("\\\\i\\..\\c$"), false},
    {SI("//?/../x"), false},
    {SI("//./../x"), false},
    {SI("\\\\?\\a\\b\\c"), true},
    {SI("\\?\?\\a\\b\\c"), true},
};

static const RelTests reltests[] = {
    {SI("a/b"), SI("a/b"), SI(".")},
    {SI("a/b/."), SI("a/b"), SI(".")},
    {SI("a/b"), SI("a/b/."), SI(".")},
    {SI("./a/b"), SI("a/b"), SI(".")},
    {SI("a/b"), SI("./a/b"), SI(".")},
    {SI("ab/cd"), SI("ab/cde"), SI("../cde")},
    {SI("ab/cd"), SI("ab/c"), SI("../c")},
    {SI("a/b"), SI("a/b/c/d"), SI("c/d")},
    {SI("a/b"), SI("a/b/../c"), SI("../c")},
    {SI("a/b/../c"), SI("a/b"), SI("../b")},
    {SI("a/b/c"), SI("a/c/d"), SI("../../c/d")},
    {SI("a/b"), SI("c/d"), SI("../../c/d")},
    {SI("a/b/c/d"), SI("a/b"), SI("../..")},
    {SI("a/b/c/d"), SI("a/b/"), SI("../..")},
    {SI("a/b/c/d/"), SI("a/b"), SI("../..")},
    {SI("a/b/c/d/"), SI("a/b/"), SI("../..")},
    {SI("../../a/b"), SI("../../a/b/c/d"), SI("c/d")},
    {SI("/a/b"), SI("/a/b"), SI(".")},
    {SI("/a/b/."), SI("/a/b"), SI(".")},
    {SI("/a/b"), SI("/a/b/."), SI(".")},
    {SI("/ab/cd"), SI("/ab/cde"), SI("../cde")},
    {SI("/ab/cd"), SI("/ab/c"), SI("../c")},
    {SI("/a/b"), SI("/a/b/c/d"), SI("c/d")},
    {SI("/a/b"), SI("/a/b/../c"), SI("../c")},
    {SI("/a/b/../c"), SI("/a/b"), SI("../b")},
    {SI("/a/b/c"), SI("/a/c/d"), SI("../../c/d")},
    {SI("/a/b"), SI("/c/d"), SI("../../c/d")},
    {SI("/a/b/c/d"), SI("/a/b"), SI("../..")},
    {SI("/a/b/c/d"), SI("/a/b/"), SI("../..")},
    {SI("/a/b/c/d/"), SI("/a/b"), SI("../..")},
    {SI("/a/b/c/d/"), SI("/a/b/"), SI("../..")},
    {SI("/../../a/b"), SI("/../../a/b/c/d"), SI("c/d")},
    {SI("."), SI("a/b"), SI("a/b")},
    {SI("."), SI(".."), SI("..")},
    {SI(""), SI("../../."), SI("../..")},
    {SI(".."), SI("."), SI("err")},
    {SI(".."), SI("a"), SI("err")},
    {SI("../.."), SI(".."), SI("err")},
    {SI("a"), SI("/a"), SI("err")},
    {SI("/a"), SI("a"), SI("err")},
};

static const RelTests winreltests[] = {
    {SI("C:a\\b\\c"), SI("C:a/b/d"), SI("..\\d")},
    {SI("C:\\"), SI("D:\\"), SI("err")},
    {SI("C:"), SI("D:"), SI("err")},
    {SI("C:\\Projects"), SI("c:\\projects\\src"), SI("src")},
    {SI("C:\\Projects"), SI("c:\\projects"), SI(".")},
    {SI("C:\\Projects\\a\\.."), SI("c:\\projects"), SI(".")},
    {SI("\\\\host\\share"), SI("\\\\host\\share\\file.txt"), SI("file.txt")},
};

static const VolumeNameTest volumenametests[] = {
    {SI("c:/foo/bar"), SI("c:")},
    {SI("c:"), SI("c:")},
    {SI("c:\\"), SI("c:")},
    {SI("2:"), SI("2:")},
    {SI(""), SI("")},
    {SI("\\\\\\host"), SI("\\\\\\host")},
    {SI("\\\\\\host\\"), SI("\\\\\\host")},
    {SI("\\\\\\host\\share"), SI("\\\\\\host")},
    {SI("\\\\\\host\\\\share"), SI("\\\\\\host")},
    {SI("\\\\host"), SI("\\\\host")},
    {SI("//host"), SI("\\\\host")},
    {SI("\\\\host\\"), SI("\\\\host\\")},
    {SI("//host/"), SI("\\\\host\\")},
    {SI("\\\\host\\share"), SI("\\\\host\\share")},
    {SI("//host/share"), SI("\\\\host\\share")},
    {SI("\\\\host\\share\\"), SI("\\\\host\\share")},
    {SI("//host/share/"), SI("\\\\host\\share")},
    {SI("\\\\host\\share\\foo"), SI("\\\\host\\share")},
    {SI("//host/share/foo"), SI("\\\\host\\share")},
    {SI("\\\\host\\share\\\\foo\\\\\\bar\\\\\\\\baz"), SI("\\\\host\\share")},
    {SI("//host/share//foo///bar////baz"), SI("\\\\host\\share")},
    {SI("\\\\host\\share\\foo\\..\\bar"), SI("\\\\host\\share")},
    {SI("//host/share/foo/../bar"), SI("\\\\host\\share")},
    {SI("\\\\..\\..\\a"), SI("")},
    {SI("//../../a"), SI("")},
    {SI("\\\\i\\..\\c$"), SI("")},
    {SI("//./UNC/../share"), SI("")},
    {SI("//?/../x"), SI("")},
    {SI("//./../x"), SI("")},
    {SI("//.../share"), SI("\\\\...\\share")},
    {SI("//host/..."), SI("\\\\host\\...")},
    {SI("//?/..x"), SI("\\\\?\\..x")},
    {SI("//."), SI("\\\\.")},
    {SI("//./"), SI("\\\\.\\")},
    {SI("//./NUL"), SI("\\\\.\\NUL")},
    {SI("//?"), SI("\\\\?")},
    {SI("//?/"), SI("\\\\?\\")},
    {SI("//?/NUL"), SI("\\\\?\\NUL")},
    {SI("/?\?"), SI("\\?\?")},
    {SI("/?\?/"), SI("\\?\?\\")},
    {SI("/?\?/NUL"), SI("\\?\?\\NUL")},
    {SI("//./a/b"), SI("\\\\.\\a")},
    {SI("//./C:"), SI("\\\\.\\C:")},
    {SI("//./C:/"), SI("\\\\.\\C:")},
    {SI("//./C:/a/b/c"), SI("\\\\.\\C:")},
    {SI("//./UNC/host/share/a/b/c"), SI("\\\\.\\UNC\\host\\share")},
    {SI("//?/UNC/host/share/a/b/c"), SI("\\\\?\\UNC\\host\\share")},
    {SI("/?\?/UNC/host/share/a/b/c"), SI("\\?\?\\UNC\\host\\share")},
    {SI("//./UNC/host"), SI("\\\\.\\UNC\\host")},
    {SI("//./UNC/host\\"), SI("\\\\.\\UNC\\host\\")},
    {SI("//./UNC"), SI("\\\\.\\UNC")},
    {SI("//./UNC/"), SI("\\\\.\\UNC\\")},
    {SI("\\\\?\\x"), SI("\\\\?\\x")},
    {SI("\\?\?\\x"), SI("\\?\?\\x")},
};
typedef struct MatchTest {
    Str pattern;
    Str s;
    bool match;
    bool bad;
} MatchTest;

static const MatchTest matchTests[] = {
    {SI("abc"), SI("abc"), true, false},
    {SI("*"), SI("abc"), true, false},
    {SI("*c"), SI("abc"), true, false},
    {SI("a*"), SI("a"), true, false},
    {SI("a*"), SI("abc"), true, false},
    {SI("a*"), SI("ab/c"), false, false},
    {SI("a*/b"), SI("abc/b"), true, false},
    {SI("a*/b"), SI("a/c/b"), false, false},
    {SI("a*b*c*d*e*/f"), SI("axbxcxdxe/f"), true, false},
    {SI("a*b*c*d*e*/f"), SI("axbxcxdxexxx/f"), true, false},
    {SI("a*b*c*d*e*/f"), SI("axbxcxdxe/xxx/f"), false, false},
    {SI("a*b*c*d*e*/f"), SI("axbxcxdxexxx/fff"), false, false},
    {SI("a*b?c*x"), SI("abxbbxdbxebxczzx"), true, false},
    {SI("a*b?c*x"), SI("abxbbxdbxebxczzy"), false, false},
    {SI("ab[c]"), SI("abc"), true, false},
    {SI("ab[b-d]"), SI("abc"), true, false},
    {SI("ab[e-g]"), SI("abc"), false, false},
    {SI("ab[^c]"), SI("abc"), false, false},
    {SI("ab[^b-d]"), SI("abc"), false, false},
    {SI("ab[^e-g]"), SI("abc"), true, false},
    {SI("a\\*b"), SI("a*b"), true, false},
    {SI("a\\*b"), SI("ab"), false, false},
    {SI("a?b"),
     SI("a\xe2\x98\xba"
        "b"),
     true, false},
    {SI("a[^a]b"),
     SI("a\xe2\x98\xba"
        "b"),
     true, false},
    {SI("a???b"),
     SI("a\xe2\x98\xba"
        "b"),
     false, false},
    {SI("a[^a][^a][^a]b"),
     SI("a\xe2\x98\xba"
        "b"),
     false, false},
    {SI("[a-\xce\xb6]*"), SI("\xce\xb1"), true, false},
    {SI("*[a-\xce\xb6]"), SI("A"), false, false},
    {SI("a?b"), SI("a/b"), false, false},
    {SI("a*b"), SI("a/b"), false, false},
    {SI("[\\]a]"), SI("]"), true, false},
    {SI("[\\-]"), SI("-"), true, false},
    {SI("[x\\-]"), SI("x"), true, false},
    {SI("[x\\-]"), SI("-"), true, false},
    {SI("[x\\-]"), SI("z"), false, false},
    {SI("[\\-x]"), SI("x"), true, false},
    {SI("[\\-x]"), SI("-"), true, false},
    {SI("[\\-x]"), SI("a"), false, false},
    {SI("[]a]"), SI("]"), false, true},
    {SI("[-]"), SI("-"), false, true},
    {SI("[x-]"), SI("x"), false, true},
    {SI("[x-]"), SI("-"), false, true},
    {SI("[x-]"), SI("z"), false, true},
    {SI("[-x]"), SI("x"), false, true},
    {SI("[-x]"), SI("-"), false, true},
    {SI("[-x]"), SI("a"), false, true},
    {SI("\\"), SI("a"), false, true},
    {SI("[a-b-c]"), SI("a"), false, true},
    {SI("["), SI("a"), false, true},
    {SI("[^"), SI("a"), false, true},
    {SI("[^bc"), SI("a"), false, true},
    {SI("a["), SI("a"), false, true},
    {SI("a["), SI("ab"), false, true},
    {SI("a["), SI("x"), false, true},
    {SI("a/b["), SI("x"), false, true},
    {SI("*x"), SI("xxx"), true, false},
};

static const char *flavour(bool win) {
    return win ? "windows" : "unix";
}

static bool str_has_byte(Str s, Byte c) {
    return s.len > 0 && memchr(s.p, c, (size_t)s.len) != NULL;
}

/* Clean, checked to make no allocation when it is given a clean path: an
 * allocator with no room fails every request, and a failed one gives the empty
 * string. The exception is a path that Windows cleaning puts .\ or \. in front
 * of, which takes a copy in Go as well. Go's own check of this is skipped
 * whenever GOMAXPROCS is above one, so it never sees those. */
static void check_clean(TestingT *t, Alloc *a, Str path, Str want, bool win) {
    Str s = burrow__filepath_clean(a, path, win);
    if (!str_eq(s, want))
        testing_t_errorf_v(t, "%s: Clean(%q) = %q, want %q", flavour(win), path, s,
                           want);
    s = burrow__filepath_clean(a, want, win);
    if (!str_eq(s, want))
        testing_t_errorf_v(t, "%s: Clean(%q) = %q, want %q", flavour(win), want, s,
                           want);
    if (win && want.len >= 2 &&
        ((want.p[0] == '.' && want.p[1] == '\\') ||
         (want.p[0] == '\\' && want.p[1] == '.')))
        return;
    Fixed fx;
    fixed_init(&fx, NULL, 0);
    s = burrow__filepath_clean(fixed_allocator(&fx), want, win);
    if (!str_eq(s, want))
        testing_t_errorf_v(t, "%s: Clean(%q) allocates", flavour(win), want);
}

static void TestClean(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        for (Int i = 0; i < LEN(cleantests); i++) {
            Str want = cleantests[i].result;
            if (win)
                want = burrow__filepath_from_slash(a, want, true);
            check_clean(t, a, cleantests[i].path, want, win);
        }
        if (win) {
            for (Int i = 0; i < LEN(wincleantests); i++)
                check_clean(t, a, wincleantests[i].path, wincleantests[i].result, true);
        } else {
            for (Int i = 0; i < LEN(nonwincleantests); i++)
                check_clean(t, a, nonwincleantests[i].path, nonwincleantests[i].result,
                            false);
        }
    }
    arena_free(&ar);
}

/* Names that only RtlIsDosDeviceName_U can rule on, because a reserved name
 * has something after it. Elsewhere pal_is_dos_device_name says no, so the
 * Windows rules on another system call these local. */
static bool needs_windows(Str path) {
    static const Str names[] = {SI("nul "), SI("nul."), SI("a/nul:"), SI("a/nul : a"),
                                SI("com\xc2\xb9 : a")};
    for (Int i = 0; i < LEN(names); i++) {
        if (str_eq(path, names[i]))
            return !HOST_WIN;
    }
    return false;
}

static void check_is_local(TestingT *t, const IsLocalTest *test, bool win) {
    bool got = burrow__filepath_is_local(test->path, win);
    if (got != test->isLocal)
        testing_t_errorf_v(t, "%s: IsLocal(%q) = %v, want %v", flavour(win), test->path,
                           got, test->isLocal);
}

static void TestIsLocal(TestingT *t) {
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        for (Int i = 0; i < LEN(islocaltests); i++)
            check_is_local(t, &islocaltests[i], win);
        if (!win)
            continue;
        for (Int i = 0; i < LEN(winislocaltests); i++) {
            if (needs_windows(winislocaltests[i].path)) {
                testing_t_logf_v(t, "skipping IsLocal(%q): only Windows can say",
                                 winislocaltests[i].path);
                continue;
            }
            check_is_local(t, &winislocaltests[i], true);
        }
    }
}

static void check_localize(TestingT *t, Alloc *a, const LocalizeTest *test, bool win) {
    Str want = win ? burrow__filepath_from_slash(a, test->want, true) : test->want;
    Error err;
    Str got = burrow__filepath_localize(a, test->path, win, &err);
    if (!str_eq(got, want) || BURROW_FAILED(err) != (want.len == 0))
        testing_t_errorf_v(t, "%s: Localize(%q) = %q, %v want %q", flavour(win),
                           test->path, got, err, want);
    if (BURROW_FAILED(err) && !errors_is(err, burrow__filepath_err_invalid_path))
        testing_t_errorf_v(t, "%s: Localize(%q): error %v", flavour(win), test->path,
                           err);
}

static void TestLocalize(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        for (Int i = 0; i < LEN(localizetests); i++)
            check_localize(t, a, &localizetests[i], win);
        if (win) {
            for (Int i = 0; i < LEN(winlocalizetests); i++)
                check_localize(t, a, &winlocalizetests[i], true);
        } else {
            for (Int i = 0; i < LEN(unixlocalizetests); i++)
                check_localize(t, a, &unixlocalizetests[i], false);
        }
    }
    arena_free(&ar);
}

static void TestFromAndToSlash(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        Byte sep = win ? '\\' : '/';
        Byte ab[4] = {sep, 'a', sep, 'b'};
        Byte aab[4] = {'a', sep, sep, 'b'};
        const PathTest slashtests[] = {
            {S(""), S("")},
            {S("/"), (Str){&sep, 1}},
            {S("/a/b"), (Str){ab, 4}},
            {S("a//b"), (Str){aab, 4}},
        };
        for (Int i = 0; i < LEN(slashtests); i++) {
            const PathTest *test = &slashtests[i];
            Str s = burrow__filepath_from_slash(a, test->path, win);
            if (!str_eq(s, test->result))
                testing_t_errorf_v(t, "%s: FromSlash(%q) = %q, want %q", flavour(win),
                                   test->path, s, test->result);
            s = burrow__filepath_to_slash(a, test->result, win);
            if (!str_eq(s, test->path))
                testing_t_errorf_v(t, "%s: ToSlash(%q) = %q, want %q", flavour(win),
                                   test->result, s, test->path);
        }
    }
    arena_free(&ar);
}

static void check_split_list(TestingT *t, Alloc *a, Str list, const Str *want,
                             Int want_len, bool win) {
    Slice l = burrow__filepath_split_list(a, list, win);
    bool same = l.len == want_len;
    for (Int i = 0; same && i < want_len; i++)
        same = str_eq(((const Str *)l.p)[i], want[i]);
    if (!same)
        testing_t_errorf_v(t, "%s: SplitList(%q) has %d elements, want %d",
                           flavour(win), list, l.len, want_len);
}

static void TestSplitList(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        Byte lsep = win ? ';' : ':';
        Byte one[3] = {'a', lsep, 'b'};
        Byte two[4] = {lsep, 'a', lsep, 'b'};
        static const Str ab[] = {SI("a"), SI("b")};
        static const Str eab[] = {SI(""), SI("a"), SI("b")};
        check_split_list(t, a, S(""), NULL, 0, win);
        check_split_list(t, a, (Str){one, 3}, ab, 2, win);
        check_split_list(t, a, (Str){two, 4}, eab, 3, win);
        if (!win)
            continue;
        for (Int i = 0; i < LEN(winsplitlisttests); i++) {
            const SplitListTest *test = &winsplitlisttests[i];
            check_split_list(t, a, test->list, test->result, test->result_len, true);
        }
    }
    arena_free(&ar);
}

static void check_split(TestingT *t, const SplitTest *test, bool win) {
    Str f;
    Str d = burrow__filepath_split(test->path, &f, win);
    if (!str_eq(d, test->dir) || !str_eq(f, test->file))
        testing_t_errorf_v(t, "%s: Split(%q) = %q, %q, want %q, %q", flavour(win),
                           test->path, d, f, test->dir, test->file);
}

static void TestSplit(TestingT *t) {
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        for (Int i = 0; i < LEN(unixsplittests); i++)
            check_split(t, &unixsplittests[i], win);
        if (!win)
            continue;
        for (Int i = 0; i < LEN(winsplittests); i++)
            check_split(t, &winsplittests[i], true);
    }
}

static void check_join(TestingT *t, Alloc *a, const JoinTest *test, bool win) {
    Str expected = burrow__filepath_from_slash(a, test->path, win);
    Slice elem = slice_from((void *)(Uintptr)test->elem, test->elem_len, test->elem_len,
                            TYPE_STRING);
    Str p = burrow__filepath_join(a, elem, win);
    if (!str_eq(p, expected))
        testing_t_errorf_v(t, "%s: join of %d elements = %q, want %q", flavour(win),
                           test->elem_len, p, expected);
}

static void TestJoin(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        for (Int i = 0; i < LEN(jointests); i++)
            check_join(t, a, &jointests[i], win);
        if (win) {
            for (Int i = 0; i < LEN(winjointests); i++)
                check_join(t, a, &winjointests[i], true);
        } else {
            for (Int i = 0; i < LEN(nonwinjointests); i++)
                check_join(t, a, &nonwinjointests[i], false);
        }
    }
    arena_free(&ar);
}

static void TestExt(TestingT *t) {
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        for (Int i = 0; i < LEN(exttests); i++) {
            Str x = burrow__filepath_ext(exttests[i].path, win);
            if (!str_eq(x, exttests[i].ext))
                testing_t_errorf_v(t, "%s: Ext(%q) = %q, want %q", flavour(win),
                                   exttests[i].path, x, exttests[i].ext);
        }
    }
}

static void check_base(TestingT *t, Str path, Str want, bool win) {
    Str s = burrow__filepath_base(path, win);
    if (!str_eq(s, want))
        testing_t_errorf_v(t, "%s: Base(%q) = %q, want %q", flavour(win), path, s,
                           want);
}

static void TestBase(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        for (Int i = 0; i < LEN(basetests); i++) {
            Str want = basetests[i].result;
            if (win)
                want = burrow__filepath_clean(a, want, true);
            check_base(t, basetests[i].path, want, win);
        }
        if (!win)
            continue;
        for (Int i = 0; i < LEN(winbasetests); i++)
            check_base(t, winbasetests[i].path, winbasetests[i].result, true);
    }
    arena_free(&ar);
}

static void check_dir(TestingT *t, Alloc *a, Str path, Str want, bool win) {
    Str s = burrow__filepath_dir(a, path, win);
    if (!str_eq(s, want))
        testing_t_errorf_v(t, "%s: Dir(%q) = %q, want %q", flavour(win), path, s, want);
}

static void TestDir(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        for (Int i = 0; i < LEN(dirtests); i++) {
            Str want = dirtests[i].result;
            if (win)
                want = burrow__filepath_clean(a, want, true);
            check_dir(t, a, dirtests[i].path, want, win);
        }
        if (win) {
            for (Int i = 0; i < LEN(windirtests); i++)
                check_dir(t, a, windirtests[i].path, windirtests[i].result, true);
        } else {
            for (Int i = 0; i < LEN(nonwindirtests); i++)
                check_dir(t, a, nonwindirtests[i].path, nonwindirtests[i].result,
                          false);
        }
    }
    arena_free(&ar);
}

static void check_is_abs(TestingT *t, Str path, bool want, bool win) {
    bool r = burrow__filepath_is_abs(path, win);
    if (r != want)
        testing_t_errorf_v(t, "%s: IsAbs(%q) = %v, want %v", flavour(win), path, r,
                           want);
}

static void TestIsAbs(TestingT *t) {
    for (Int i = 0; i < LEN(isabstests); i++)
        check_is_abs(t, isabstests[i].path, isabstests[i].isAbs, false);
    for (Int i = 0; i < LEN(winisabstests); i++)
        check_is_abs(t, winisabstests[i].path, winisabstests[i].isAbs, true);
    for (Int i = 0; i < LEN(isabstests); i++)
        check_is_abs(t, isabstests[i].path, false, true);
    for (Int i = 0; i < LEN(isabstests); i++) {
        Byte buf[64];
        Str p = isabstests[i].path;
        buf[0] = 'c';
        buf[1] = ':';
        if (p.len > 0)
            memcpy(buf + 2, p.p, (size_t)p.len);
        check_is_abs(t, (Str){buf, p.len + 2}, isabstests[i].isAbs, true);
    }
}

static void check_rel(TestingT *t, Alloc *a, const RelTests *test, Str want, bool win) {
    Error err;
    Str got = burrow__filepath_rel(a, test->root, test->path, win, &err);
    if (str_eq(want, S("err"))) {
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: Rel(%q, %q)=%q, want error", flavour(win),
                               test->root, test->path, got);
        return;
    }
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s: Rel(%q, %q): want %q, got error: %v", flavour(win),
                           test->root, test->path, want, err);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "%s: Rel(%q, %q)=%q, want %q", flavour(win), test->root,
                           test->path, got, want);
}

static void TestRel(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        for (Int i = 0; i < LEN(reltests); i++) {
            Str want = reltests[i].want;
            if (win)
                want = burrow__filepath_from_slash(a, want, true);
            check_rel(t, a, &reltests[i], want, win);
        }
        if (!win)
            continue;
        for (Int i = 0; i < LEN(winreltests); i++)
            check_rel(t, a, &winreltests[i], winreltests[i].want, true);
    }
    arena_free(&ar);
}

static void TestVolumeName(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(volumenametests); i++) {
        const VolumeNameTest *v = &volumenametests[i];
        Str vol = burrow__filepath_volume_name(a, v->path, true);
        if (!str_eq(vol, v->vol))
            testing_t_errorf_v(t, "VolumeName(%q)=%q, want %q", v->path, vol, v->vol);
        /* And there are no volume names anywhere else. */
        vol = burrow__filepath_volume_name(a, v->path, false);
        if (vol.len != 0)
            testing_t_errorf_v(t, "unix: VolumeName(%q)=%q, want \"\"", v->path, vol);
    }
    arena_free(&ar);
}

static void TestMatch(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        for (Int i = 0; i < LEN(matchTests); i++) {
            const MatchTest *tt = &matchTests[i];
            Str pattern = tt->pattern;
            Str s = tt->s;
            if (win) {
                /* No escape allowed on Windows. */
                if (str_has_byte(pattern, '\\'))
                    continue;
                pattern = burrow__filepath_clean(a, pattern, true);
                s = burrow__filepath_clean(a, s, true);
            }
            Error err;
            bool ok = burrow__filepath_match(pattern, s, win, &err);
            bool bad = BURROW_FAILED(err);
            if (ok != tt->match || bad != tt->bad ||
                (bad && !errors_is(err, filepath_err_bad_pattern)))
                testing_t_errorf_v(
                    t, "%s: Match(%q, %q) = %v, %v want %v, bad pattern %v",
                    flavour(win), pattern, s, ok, err, tt->match, tt->bad);
        }
    }
    arena_free(&ar);
}

static void TestIssue52476(TestingT *t) {
    static const struct {
        Str lhs, rhs, want;
    } tests[] = {
        {SI("..\\."), SI("C:"), SI("..\\C:")},
        {SI(".."), SI("C:"), SI("..\\C:")},
        {SI("."), SI(":"), SI(".\\:")},
        {SI("."), SI("C:"), SI(".\\C:")},
        {SI("."), SI("C:/a/b/../c"), SI(".\\C:\\a\\c")},
        {SI("."), SI("\\C:"), SI(".\\C:")},
        {SI("C:\\"), SI("."), SI("C:\\")},
        {SI("C:\\"), SI("C:\\"), SI("C:\\C:")},
        {SI("C"), SI(":"), SI("C\\:")},
        {SI("\\."), SI("C:"), SI("\\C:")},
        {SI("\\"), SI("C:"), SI("\\C:")},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < LEN(tests); i++) {
        Str e[2] = {tests[i].lhs, tests[i].rhs};
        Str got = burrow__filepath_join(a, slice_from(e, 2, 2, TYPE_STRING), true);
        if (!str_eq(got, tests[i].want))
            testing_t_errorf_v(t, "Join(%q, %q): got %q, want %q", tests[i].lhs,
                               tests[i].rhs, got, tests[i].want);
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------ not from Go */

/* The public functions are the internal ones with the host's rules. */
static void TestHostRules(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str p = S("C:/a\\b/../c.txt");
    CHECK(str_eq(filepath_clean(a, p), burrow__filepath_clean(a, p, HOST_WIN)));
    CHECK(str_eq(filepath_base(p), burrow__filepath_base(p, HOST_WIN)));
    CHECK(str_eq(filepath_dir(a, p), burrow__filepath_dir(a, p, HOST_WIN)));
    CHECK(str_eq(filepath_volume_name(a, p),
                 burrow__filepath_volume_name(a, p, HOST_WIN)));
    CHECK(filepath_is_abs(p) == burrow__filepath_is_abs(p, HOST_WIN));
    CHECK(filepath_is_local(p) == burrow__filepath_is_local(p, HOST_WIN));
    CHECK(filepath_is_path_separator('/'));
    CHECK(filepath_is_path_separator('\\') == HOST_WIN);
    CHECK(filepath_is_path_separator(FILEPATH_SEPARATOR));
    CHECK(FILEPATH_LIST_SEPARATOR == (HOST_WIN ? ';' : ':'));
    Str file;
    CHECK(str_eq(filepath_split(p, &file), burrow__filepath_split(p, NULL, HOST_WIN)));
    CHECK(str_eq(filepath_ext(p), S(".txt")));
    CHECK(filepath_split_list(a, S("a;b:c")).len == 2);
    CHECK(filepath_match(S("a*"), S("abc"), NULL));
    Error err;
    CHECK(
        str_eq(filepath_localize(a, S("a/b"), &err), HOST_WIN ? S("a\\b") : S("a/b")));
    CHECK(!BURROW_FAILED(err));
    CHECK(str_eq(filepath_rel(a, S("a"), S("a/b"), &err), S("b")));
    CHECK(str_eq(filepath_to_slash(a, filepath_from_slash(a, S("a/b"))), S("a/b")));
    arena_free(&ar);
}

static void TestJoinVariadic(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str want = HOST_WIN ? S("a\\c\\d") : S("a/c/d");
    CHECK(str_eq(filepath_join_v(a, 3, S("a"), S("b/../c"), S("/d/")), want));
    CHECK(str_eq(filepath_join_v(a, 0), S("")));
    /* More elements than the stack array in filepath_join_v holds. */
    Str p = filepath_join_v(a, 20, S("0"), S("1"), S("2"), S("3"), S("4"), S("5"),
                            S("6"), S("7"), S("8"), S("9"), S("a"), S("b"), S("c"),
                            S("d"), S("e"), S("f"), S("g"), S("h"), S(".."), S("i"));
    CHECK(str_eq(filepath_to_slash(a, p), S("0/1/2/3/4/5/6/7/8/9/a/b/c/d/e/f/g/i")));
    arena_free(&ar);
}

static void TestOutOfMemory(TestingT *t) {
    Fixed fx;
    fixed_init(&fx, NULL, 0);
    Alloc *a = fixed_allocator(&fx);
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        CHECK(str_eq(burrow__filepath_clean(a, S("a//b"), win), S("")));
        CHECK(str_eq(burrow__filepath_dir(a, S("a/./b/c"), win), S("")));
        Str e[2] = {S("a"), S("b")};
        CHECK(str_eq(burrow__filepath_join(a, slice_from(e, 2, 2, TYPE_STRING), win),
                     S("")));
        CHECK(burrow__filepath_split_list(a, S("a:b;c"), win).len == 0);
        /* These have nothing to copy, so they still work. */
        Str in = win ? S("..\\a\\b\\..") : S("../a/b/..");
        CHECK(str_eq(burrow__filepath_clean(a, in, win), win ? S("..\\a") : S("../a")));
        in = win ? S("a\\b\\..") : S("a/b/..");
        CHECK(str_eq(burrow__filepath_clean(a, in, win), S("a")));
    }
    CHECK(str_eq(burrow__filepath_dir(a, S("a/b/c"), false), S("a/b")));
}

/* The answer is a view of the input wherever Go's would share its memory. */
static void TestResultsBorrowTheInput(TestingT *t) {
    Str in = S("/usr/lib/libc.so");
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        Str file;
        Str dir = burrow__filepath_split(in, &file, win);
        CHECK(dir.p == in.p);
        CHECK(file.p == in.p + 9);
        CHECK(burrow__filepath_ext(in, win).p == in.p + 13);
        CHECK(burrow__filepath_base(in, win).p == in.p + 9);
    }
    Str c = S("a/b");
    CHECK(burrow__filepath_clean(NULL, c, false).p == c.p);
    CHECK(burrow__filepath_to_slash(NULL, c, true).p == c.p);
    CHECK(burrow__filepath_from_slash(NULL, c, false).p == c.p);
}

/* Go's filepath, unlike its path, does not look at the rest of a pattern once
 * the match has failed, so the bad class at the end here is never seen. */
static void TestMatchStopsAtTheFirstAnswer(TestingT *t) {
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        Error err;
        CHECK(!burrow__filepath_match(S("x*[]"), S("abc"), win, &err));
        CHECK(!BURROW_FAILED(err));
        CHECK(!burrow__filepath_match(S("x*["), S("abc"), win, &err));
        CHECK(!BURROW_FAILED(err));
        CHECK(!burrow__filepath_match(S("a["), S("a"), win, &err));
        CHECK(errors_is(err, filepath_err_bad_pattern));
        CHECK(!errors_is(err, path_err_bad_pattern));
    }
    /* On Windows a backslash is the separator, and it matches itself. */
    CHECK(burrow__filepath_match(S("a\\*"), S("a\\b"), true, NULL));
    CHECK(!burrow__filepath_match(S("a\\*"), S("a\\b"), false, NULL));
    CHECK(burrow__filepath_match(S("a\\*"), S("a*"), false, NULL));
    CHECK(!burrow__filepath_match(S("a*"), S("a\\b"), true, NULL));
    CHECK(str_eq(error_text(filepath_err_bad_pattern), S("syntax error in pattern")));
}

static void TestRelErrorText(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err;
    Str r = burrow__filepath_rel(a, S("/a"), S("a"), false, &err);
    CHECK(str_eq(r, S("")));
    CHECK(str_eq(error_text(err), S("Rel: can't make a relative to /a")));
    r = burrow__filepath_rel(a, S("C:\\"), S("D:\\x"), true, &err);
    CHECK(str_eq(error_text(err), S("Rel: can't make D:\\x relative to C:\\")));
    arena_free(&ar);
}

static void TestHasPrefix(TestingT *t) {
    CHECK(burrow__filepath_has_prefix(S("/a/bc"), S("/a/b"), false));
    CHECK(!burrow__filepath_has_prefix(S("/A/b"), S("/a"), false));
    CHECK(burrow__filepath_has_prefix(S("C:\\Users"), S("c:\\users"), true));
    CHECK(!burrow__filepath_has_prefix(S("C:\\Users"), S("d:"), true));
    CHECK(filepath_has_prefix(S("abc"), S("ab")));
}

/* BURROW_STR_EMPTY has a NULL p, and every function has to take it. */
static void TestNullEmptyString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str e = BURROW_STR_EMPTY;
    for (int w = 0; w < 2; w++) {
        bool win = w == 1;
        Error err;
        Str file;
        CHECK(str_eq(burrow__filepath_clean(a, e, win), S(".")));
        CHECK(!burrow__filepath_is_local(e, win));
        CHECK(str_eq(burrow__filepath_localize(a, e, win, &err), S("")));
        CHECK(BURROW_FAILED(err));
        CHECK(str_eq(burrow__filepath_to_slash(a, e, win), S("")));
        CHECK(str_eq(burrow__filepath_from_slash(a, e, win), S("")));
        CHECK(burrow__filepath_split_list(a, e, win).len == 0);
        CHECK(str_eq(burrow__filepath_split(e, &file, win), S("")));
        CHECK(str_eq(file, S("")));
        CHECK(str_eq(burrow__filepath_ext(e, win), S("")));
        CHECK(str_eq(burrow__filepath_base(e, win), S(".")));
        CHECK(str_eq(burrow__filepath_dir(a, e, win), S(".")));
        CHECK(!burrow__filepath_is_abs(e, win));
        CHECK(str_eq(burrow__filepath_volume_name(a, e, win), S("")));
        CHECK(str_eq(burrow__filepath_rel(a, e, e, win, &err), S(".")));
        CHECK(str_eq(burrow__filepath_rel(a, e, S("a"), win, &err), S("a")));
        CHECK(str_eq(burrow__filepath_rel(a, S("a"), e, win, &err), S("..")));
        CHECK(burrow__filepath_match(e, e, win, &err));
        CHECK(!burrow__filepath_match(S("a"), e, win, &err));
        CHECK(burrow__filepath_has_prefix(e, e, win));
        Str two[2] = {e, e};
        CHECK(str_eq(burrow__filepath_join(a, slice_from(two, 2, 2, TYPE_STRING), win),
                     S("")));
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestClean)                                                                       \
    X(TestIsLocal)                                                                     \
    X(TestLocalize)                                                                    \
    X(TestFromAndToSlash)                                                              \
    X(TestSplitList)                                                                   \
    X(TestSplit)                                                                       \
    X(TestJoin)                                                                        \
    X(TestExt)                                                                         \
    X(TestBase)                                                                        \
    X(TestDir)                                                                         \
    X(TestIsAbs)                                                                       \
    X(TestRel)                                                                         \
    X(TestVolumeName)                                                                  \
    X(TestMatch)                                                                       \
    X(TestIssue52476)                                                                  \
    X(TestHostRules)                                                                   \
    X(TestJoinVariadic)                                                                \
    X(TestOutOfMemory)                                                                 \
    X(TestResultsBorrowTheInput)                                                       \
    X(TestMatchStopsAtTheFirstAnswer)                                                  \
    X(TestRelErrorText)                                                                \
    X(TestHasPrefix)                                                                   \
    X(TestNullEmptyString)

TESTING_MAIN(TESTS)
