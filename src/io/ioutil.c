/* io/ioutil. See include/burrow/io/ioutil.h.
 *
 * Derived from Go's src/io/ioutil/ioutil.go and tempfile.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/io/ioutil.h"

#include "burrow/func.h"
#include "burrow/io/fs.h"
#include "burrow/mem/heap.h"
#include "burrow/slices.h"
#include "burrow/strings.h"

Slice ioutil_read_all(Alloc *a, IoReader r, Error *err) {
    return io_read_all(a, r, err);
}

Slice ioutil_read_file(Alloc *a, Str filename, Error *err) {
    return os_read_file(a, filename, err);
}

Error ioutil_write_file(Str filename, Slice data, OsFileMode perm) {
    return os_write_file(filename, data, perm);
}

static int ioutil_info_cmp(void *env, const void *x, const void *y) {
    (void)env;
    const FsFileInfo *i1 = (const FsFileInfo *)x;
    const FsFileInfo *i2 = (const FsFileInfo *)y;
    return (int)strings_compare(i1->vt->name(i1->data), i2->vt->name(i2->data));
}

Slice ioutil_read_dir(Alloc *a, Str dirname, Error *err) {
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_open(heap_allocator(), dirname, &e);
    if (f == NULL) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_FS_FILE_INFO);
    }
    Slice list = os_file_readdir(f, a, -1, &e);
    os_file_free(f);
    BURROW_OUT(err, e);
    if (BURROW_FAILED(e))
        return slice_nil(TYPE_FS_FILE_INFO);
    slices_sort_func(list, BURROW_FN(SlicesCmpFunc, ioutil_info_cmp, NULL));
    return list;
}

IoNopCloser ioutil_nop_closer(IoReader r) {
    return io_nop_closer(r);
}

OsFile *ioutil_temp_file(Alloc *a, Str dir, Str pattern, Error *err) {
    return os_create_temp(a, dir, pattern, err);
}

Str ioutil_temp_dir(Alloc *a, Str dir, Str pattern, Error *err) {
    return os_mkdir_temp(a, dir, pattern, err);
}
