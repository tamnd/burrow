/* time/tzdata, a copy of the time zone database built into the program.
 *
 * time_load_location looks for a zone in $ZONEINFO and then in the system's
 * zoneinfo directories. A program that has to work where neither is there,
 * which is any Windows machine and plenty of containers, can carry the
 * database itself. In Go that is import _ "time/tzdata" or the timetzdata
 * build tag. Here it is one call, made before the first lookup:
 *
 *     tzdata_register();
 *     TimeLocation *ny = time_load_location(BURROW_S("America/New_York"), &err);
 *
 * or building burrow with BURROW_TIMETZDATA defined, which registers it for
 * every program built against that library with nothing to call. The system's
 * own database is still tried first either way, the same as in Go, so the
 * built-in copy only answers when the system has no answer.
 *
 * The database is Go's lib/time/zoneinfo.zip and adds about 400 kB to a
 * program, which is why it is not there unless asked for. A static link only
 * pulls it in when tzdata_register is called. The amalgamation leaves it out
 * of burrow.c unless BURROW_TIMETZDATA is defined when burrow.c is compiled.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package time/tzdata */

#ifndef BURROW_TIME_TZDATA_H
#define BURROW_TIME_TZDATA_H

#ifdef __cplusplus
extern "C" {
#endif

/* Makes the built-in database the last place time_load_location looks.
 * Calling it again does nothing more. Go does this in the package's init, so
 * call it before anything uses a time zone: Local is worked out once, the
 * first time it is needed, and a call after that does not change it. */
void tzdata_register(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TIME_TZDATA_H */
