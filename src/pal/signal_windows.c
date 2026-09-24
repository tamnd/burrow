/* Handlers on Windows, which has no signals at all.
 *
 * Three of the group work here. A bad memory access raises a structured
 * exception rather than a signal, and a vectored exception handler is what sees
 * it first, so PAL_SIGFAULT is that. Ctrl-C and the other console events go to
 * a console control handler, and they are mapped the way Go's os/signal maps
 * them: Ctrl-C and Ctrl-Break are PAL_SIGINT, and closing the console, logging
 * off and shutting down are PAL_SIGTERM. The rest say PAL_ENOTSUP rather than
 * pretending.
 *
 * This was the Windows half of src/runtime/stack.c until the platform layer
 * existed, and the reasoning in the comments came with it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if defined(_WIN32) && !defined(_WIN32_WINNT)
/* Windows 7, which is a floor no machine anybody compiles for today is below.
 * Before every include, because a Windows header that has already been read has
 * already decided. */
#define _WIN32_WINNT 0x0601
#endif

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <stdint.h>

#include <windows.h>

/* Somewhere to put a function pointer that an atomic will take. See the same
 * union in src/pal/signal_posix.c for why it is a union and not a cast. Windows
 * makes the same promise POSIX does, and for the same reason: GetProcAddress
 * would be unusable otherwise. */
typedef union Handler {
    void *object;
    PalSignalHandler fn;
} Handler;

_Static_assert(sizeof(PalSignalHandler) == sizeof(void *),
               "a function pointer does not fit where this file puts one");

/* The one handler there is, because PAL_SIGFAULT is the one signal this backend
 * takes. Published with a release store so that a fault on another thread the
 * moment AddVectoredExceptionHandler returns finds a whole pointer. */
static Handler fault_handler;

/* The handle from AddVectoredExceptionHandler, kept so that installing twice
 * replaces the handler rather than stacking a second one in front of it. */
static void *veh;

/* The PAL_SIGINT and PAL_SIGTERM handlers, published the same way. */
static Handler int_handler;
static Handler term_handler;

/* Whether SetConsoleCtrlHandler has been called, so that installing twice
 * does not put a second copy of on_console in the list. */
static uint32_t console_armed;

/* Runs on a thread the system starts for the event, not on one of the
 * program's, which is the main way a console event is not a signal. True from
 * the handler means it was dealt with. False passes it on down the list, which
 * ends at the default handler and ExitProcess, as an unclaimed SIGINT ends a
 * POSIX process. For the close, logoff and shutdown events the system ends
 * the process a few seconds after the handler returns anyway. */
static BOOL WINAPI on_console(DWORD event) {
    int32_t sig = 0;
    Handler h;
    h.object = NULL;
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        sig = PAL_SIGINT;
        h.object = burrow__atomic_load_acquire_ptr(&int_handler.object);
        break;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        sig = PAL_SIGTERM;
        h.object = burrow__atomic_load_acquire_ptr(&term_handler.object);
        break;
    default:
        return FALSE;
    }
    if (h.fn == NULL)
        return FALSE;
    return h.fn(sig, NULL, NULL) ? TRUE : FALSE;
}

static bool install_console(int32_t sig, PalSignalHandler handler, PalErrno *err) {
    Handler h;
    h.object = NULL;
    h.fn = handler;
    burrow__atomic_store_release_ptr(
        sig == PAL_SIGINT ? &int_handler.object : &term_handler.object, h.object);

    uint32_t want = 0;
    if (!burrow__atomic_cas_u32(&console_armed, &want, 1))
        return true;
    if (!SetConsoleCtrlHandler(on_console, TRUE)) {
        burrow__atomic_store_u32(&console_armed, 0);
        BURROW_OUT(err, PAL_EOTHER);
        return false;
    }
    return true;
}

static LONG CALLBACK on_exception(EXCEPTION_POINTERS *info) {
    if (info == NULL || info->ExceptionRecord == NULL)
        return EXCEPTION_CONTINUE_SEARCH;

    Handler h;
    h.object = burrow__atomic_load_acquire_ptr(&fault_handler.object);
    if (h.fn == NULL)
        return EXCEPTION_CONTINUE_SEARCH;

    /* True from the handler means resume the interrupted instruction, which is
     * what a POSIX handler returning normally does, so this is the code that
     * matches it. False means the handler did not claim it and the search goes
     * on to whoever is next, which is how a program that has its own handler
     * keeps it. Nothing is put back first, the way the POSIX backend has to:
     * a vectored handler that declines is simply not in the way. */
    if (h.fn(PAL_SIGFAULT, info->ExceptionRecord, info->ContextRecord))
        return EXCEPTION_CONTINUE_EXECUTION;

    return EXCEPTION_CONTINUE_SEARCH;
}

bool pal_signal_install(int32_t sig, PalSignalHandler handler, PalErrno *err) {
    BURROW_OUT(err, PAL_OK);

    if (handler == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    if (sig == PAL_SIGINT || sig == PAL_SIGTERM)
        return install_console(sig, handler, err);
    if (sig != PAL_SIGFAULT) {
        BURROW_OUT(err, PAL_ENOTSUP);
        return false;
    }

    Handler h;
    h.object = NULL;
    h.fn = handler;
    burrow__atomic_store_release_ptr(&fault_handler.object, h.object);

    if (veh != NULL)
        return true;

    /* First rather than last, so that a debugger's own handler does not get
     * there before this one and decide the access violation is worth stopping
     * on. A real access violation is passed on untouched. */
    veh = AddVectoredExceptionHandler(1, on_exception);
    if (veh == NULL) {
        burrow__atomic_store_release_ptr(&fault_handler.object, NULL);
        BURROW_OUT(err, PAL_EOTHER);
        return false;
    }

    return true;
}

bool pal_signal_mask(int32_t sig, bool block, PalErrno *err) {
    /* There is no mask because there is nothing to mask. Blocking a structured
     * exception is not a thing Windows offers and there is no honest way to
     * pretend otherwise. */
    (void)sig;
    (void)block;
    BURROW_OUT(err, PAL_ENOTSUP);
    return false;
}

bool pal_signal_stack_install(PalErrno *err) {
    /* A vectored handler runs on whatever stack faulted, so there is nothing to
     * set up and nothing that could fail. It is also why burrow/stack.h says
     * the overflow message is best effort on Windows: the stack that just ran
     * out is exactly the one the handler has to run on. */
    BURROW_OUT(err, PAL_OK);
    return true;
}

void pal_signal_stack_remove(void) {}

const void *pal_signal_fault_addr(const void *info) {
    if (info == NULL)
        return NULL;

    const EXCEPTION_RECORD *rec = (const EXCEPTION_RECORD *)info;
    if (rec->ExceptionCode != (DWORD)EXCEPTION_ACCESS_VIOLATION)
        return NULL;

    /* ExceptionInformation[0] says read or write and [1] is the address. The
     * count is checked because the record is a fixed size array and a different
     * exception code fills in fewer of them. */
    if (rec->NumberParameters < 2)
        return NULL;

    return (const void *)rec->ExceptionInformation[1];
}

#endif /* BURROW_OS_WINDOWS */
