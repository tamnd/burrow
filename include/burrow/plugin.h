/* plugin: loading shared libraries and finding the symbols in them.
 *
 * Derived from Go's src/plugin/plugin.go and plugin_dlopen.go.
 * Go source: go1.27.1.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* Package plugin implements loading and symbol resolution of Go plugins.
 *
 * A plugin is a Go main package with exported functions and variables that
 * has been built with:
 *
 *     go build -buildmode=plugin
 *
 * When a plugin is first opened, the init functions of all packages not
 * already part of the program are called. The main function is not run.
 * A plugin is only initialized once, and cannot be closed.
 *
 * Not in Go: in burrow a plugin is any shared library, a .so, a .dylib or a
 * .dll, built from C with the compiler's -shared flag or its equivalent, and
 * opened with dlopen, or LoadLibraryExW on Windows. Its init functions are the
 * library's constructors, which the loader runs when it is opened. A symbol is
 * looked up by its name in C, and there is no package path in front of it:
 *
 *     // hello.c, built with cc -shared -fPIC -o hello.so hello.c
 *     int V;
 *     void F(void) { printf("Hello, number %d\n", V); }
 *
 *     static Error run(void) {
 *         Error err = BURROW_NO_ERROR;
 *         Plugin *p = plugin_open(BURROW_S("./hello.so"), &err);
 *         if (BURROW_FAILED(err))
 *             return err;
 *         PluginSymbol v = plugin_lookup(p, BURROW_S("V"), &err);
 *         if (BURROW_FAILED(err))
 *             return err;
 *         PluginSymbol f = plugin_lookup(p, BURROW_S("F"), &err);
 *         if (BURROW_FAILED(err))
 *             return err;
 *         *(int *)v = 7;
 *         ((void (*)(void))f)(); // prints "Hello, number 7"
 *         return BURROW_NO_ERROR;
 *     }
 *
 * Go checks that the program and the plugin agree on every type they share,
 * and burrow cannot, because a shared library says nothing about the types of
 * its symbols. A symbol is an address, and the cast that turns it into a
 * function or a variable is the caller's promise. docs/ledger.md has the long
 * version.
 *
 * On Linux the loader is reached through weak references, so that a program
 * does not need -ldl. With a glibc older than 2.34, link with -ldl to use this
 * package. Without it, and in a static program, plugin_open fails. wasip1, js
 * and Cosmopolitan have no loader, and it always fails there.
 *
 * # Warnings
 *
 * The ability to dynamically load parts of an application during
 * execution, perhaps based on user-defined configuration, may be a
 * useful building block in some designs. In particular, because
 * applications and dynamically loaded functions can share data
 * structures directly, plugins may enable very high-performance
 * integration of separate parts.
 *
 * However, the plugin mechanism has many significant drawbacks that
 * should be considered carefully during the design. For example:
 *
 *   - Plugins are currently supported only on Linux, FreeBSD, and
 *     macOS, making them unsuitable for applications intended to be
 *     portable.
 *
 *   - Plugins are poorly supported by the Go race detector. Even simple
 *     race conditions may not be automatically detected. See
 *     https://go.dev/issue/24245 for more information.
 *
 *   - Applications that use plugins may require careful configuration
 *     to ensure that the various parts of the program be made available
 *     in the correct location in the file system (or container image).
 *     By contrast, deploying an application consisting of a single static
 *     executable is straightforward.
 *
 *   - Reasoning about program initialization is more difficult when
 *     some packages may not be initialized until long after the
 *     application has started running.
 *
 *   - Bugs in applications that load plugins could be exploited by
 *     an attacker to load dangerous or untrusted libraries.
 *
 *   - Runtime crashes are likely to occur unless all parts of the
 *     program (the application and all its plugins) are compiled
 *     using exactly the same version of the toolchain, the same build
 *     tags, and the same values of certain flags and environment
 *     variables.
 *
 *   - Similar crashing problems are likely to arise unless all common
 *     dependencies of the application and its plugins are built from
 *     exactly the same source code.
 *
 *   - Together, these restrictions mean that, in practice, the
 *     application and its plugins must all be built together by a
 *     single person or component of a system. In that case, it may
 *     be simpler for that person or component to generate Go source
 *     files that blank-import the desired set of plugins and then
 *     compile a static executable in the usual way.
 *
 * For these reasons, many users decide that traditional interprocess
 * communication (IPC) mechanisms such as sockets, pipes, remote
 * procedure call (RPC), shared memory mappings, or file system
 * operations may be more suitable despite the performance overheads.
 *
 * Not in Go: burrow opens plugins on Windows as well, and on every other
 * system that has a dlopen. */

/* burrow:package plugin */

#ifndef BURROW_PLUGIN_H
#define BURROW_PLUGIN_H

#include "burrow/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Plugin is a loaded Go plugin.
 *
 * Not in Go: it is opaque, and it is never freed, since a plugin cannot be
 * closed. */
typedef struct Plugin Plugin;

/* A Symbol is a pointer to a variable or function.
 *
 * Not in Go: it is the address the loader gives, with no type attached, and
 * the caller casts it to what it knows the symbol to be. See the example at
 * the top of this file. */
typedef void *PluginSymbol;

/* Open opens a Go plugin.
 * If a path has already been opened, then the existing *[Plugin] is returned.
 * It is safe for concurrent use by multiple goroutines.
 *
 * Not in Go: "already opened" means the same file once realpath has resolved
 * the path, as in Go. Errors read as Go's do:
 *
 *     plugin.Open("path"): realpath failed
 *     plugin.Open("path"): <what dlerror said>
 *
 * and on Windows the second has the system's message for the error. The
 * library's constructors run while it is being opened, and they may call
 * plugin_open themselves. Go's "(previous failure)" error is for a Go plugin
 * whose runtime checks failed after it was loaded, and burrow has no such
 * checks, so it never gives it. A failure to allocate gives
 * burrow_err_out_of_memory. */
BURROW_STATIC(ret) Plugin *plugin_open(Str path, Error *err);

/* Lookup searches for a symbol named symName in plugin p.
 * A symbol is any exported variable or function.
 * It reports an error if the symbol is not found.
 * It is safe for concurrent use by multiple goroutines.
 *
 * Not in Go: an exported symbol is one with external linkage, and on Windows
 * one the DLL exports. dlsym also looks in the libraries the plugin was linked
 * against, so a symbol it found may not be the plugin's own, where Go only
 * looks at the plugin. The error is "plugin: symbol NAME not found in plugin
 * PATH", where Go's PATH is the plugin's package path and burrow's is the file
 * realpath found. The address of a symbol may be NULL without an error, if the
 * library says that is where it is. */
BURROW_BORROWS(ret, p) PluginSymbol plugin_lookup(Plugin *p, Str sym_name, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_PLUGIN_H */
