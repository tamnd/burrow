/* The executable regions of the running process on macOS, from
 * mach_vm_region.
 *
 * Derived from Go's src/runtime/pprof/vminfo_darwin.go. Go source: go1.27.1.
 *
 * Go walks the address space from 1 upwards one region at a time and keeps the
 * ones that are both readable and executable, naming each with
 * proc_regionfilename. The walk ends when mach says there is no region at or
 * above the address, which it says with MACH_SEND_INVALID_DEST. Any other
 * failure ends it too, and then the answer is whether anything was found. Go
 * makes no attempt at a build ID here and neither does this.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32)
#define _DARWIN_C_SOURCE 1
#endif

#include "burrow/platform.h"

#if defined(BURROW_OS_DARWIN)

#include "burrow/pal.h"

#include "internal.h"

#include <libproc.h>
/* Not <mach/mach.h>, which declares a panic() that clashes with burrow's own in the
   amalgamation. */
#include <mach/mach_init.h>
#include <mach/mach_vm.h>
#include <sys/param.h>
#include <unistd.h>

#include <string.h>

static bool pmap_executable(vm_prot_t protection) {
    return (protection & VM_PROT_EXECUTE) != 0 && (protection & VM_PROT_READ) != 0;
}

bool pal_text_mappings(PalMappingFn fn, void *env) {
    bool added = false;
    mach_vm_address_t addr = 1;
    for (;;) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        memset(&info, 0, sizeof info);
        kern_return_t kr =
            mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&info, &count, &object);
        if (kr != KERN_SUCCESS) {
            if (kr == MACH_SEND_INVALID_DEST)
                return true;
            return added;
        }
        if (pmap_executable(info.protection)) {
            char name[MAXPATHLEN];
            int n = proc_regionfilename(getpid(), (uint64_t)addr, name, sizeof name);
            fn(env, (uint64_t)addr, (uint64_t)(addr + size), (uint64_t)info.offset,
               name, n > 0 ? (int64_t)n : 0);
            added = true;
        }
        addr += size;
    }
}

#endif /* BURROW_OS_DARWIN */
