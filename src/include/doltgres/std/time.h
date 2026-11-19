/*
 * Copyright 2026 Dolthub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The C forms of Rust's Duration, Instant, and SystemTime (the port text's
 * section 9), ported from core/src/time.rs, std/src/time.rs, and each
 * platform's std/src/sys/time at Rust 1.99.1: Windows keeps a SystemTime as
 * FILETIME intervals and an Instant as a Duration, and Unix keeps both as a
 * timespec, as Rust's own std does on each.
 */
#ifndef DOLTGRES_STD_TIME_H
#define DOLTGRES_STD_TIME_H

#include "doltgres/std/fmt.h"
#include "doltgres/std/int128.h"

/* StdDuration is a Duration: whole seconds and the nanoseconds below one. */
typedef struct StdDuration
{
	uint64		secs;
	uint32		nanos;
} StdDuration;

/* STD_DURATION_ZERO is Duration::ZERO. */
#define STD_DURATION_ZERO ((StdDuration) {0, 0})

#ifdef WIN32

/* StdInstant is Windows' Instant: a Duration from the performance counter's epoch. */
typedef struct StdInstant
{
	StdDuration t;
} StdInstant;

/* StdSystemTime is Windows' SystemTime: FILETIME's halves of 100-nanosecond intervals since 1601. */
typedef struct StdSystemTime
{
	uint32		dwLowDateTime;
	uint32		dwHighDateTime;
} StdSystemTime;

#else

/* StdTimespec is Unix's Timespec: seconds and the nanoseconds below one. */
typedef struct StdTimespec
{
	int64		tv_sec;
	uint32		tv_nsec;
} StdTimespec;

/* StdInstant is Unix's Instant: a reading of the monotonic clock. */
typedef struct StdInstant
{
	StdTimespec t;
} StdInstant;

/* StdSystemTime is Unix's SystemTime: a reading of the realtime clock. */
typedef struct StdSystemTime
{
	StdTimespec t;
} StdSystemTime;

#endif

extern const StdSystemTime STD_UNIX_EPOCH;


/* test_std_time_tests_duration_debug_matches_rust is Duration Debug's edge test, which std's test module runs. */
extern void test_std_time_tests_duration_debug_matches_rust(void);
extern StdDuration std_Duration_new(uint64 secs, uint32 nanos);
extern StdDuration std_Duration_Default_default(void);
extern StdDuration std_Duration_from_secs(uint64 secs);
extern StdDuration std_Duration_from_nanos(uint64 nanos);
extern StdDuration std_Duration_from_micros(uint64 micros);
extern uint64 std_Duration_as_secs(const StdDuration *self);
extern uint32 std_Duration_subsec_nanos(const StdDuration *self);
extern StdU128 std_Duration_as_millis(const StdDuration *self);
extern StdU128 std_Duration_as_nanos(const StdDuration *self);
extern bool std_Duration_checked_add(StdDuration self, StdDuration rhs, StdDuration *result);
extern bool std_Duration_checked_sub(StdDuration self, StdDuration rhs, StdDuration *result);
extern bool std_Duration_eq(const StdDuration *self, const StdDuration *other);
extern int	std_Duration_cmp(const StdDuration *self, const StdDuration *other);
extern bool std_Duration_Debug_fmt(const void *value, StdFormatter *f);
extern bool std_SystemTimeError_Debug_fmt(const void *value, StdFormatter *f);

extern StdInstant std_Instant_now(void);
extern void std_Instant_now_call(void *captures, void *result);
extern bool std_Instant_checked_duration_since(const StdInstant *self, StdInstant earlier,
											   StdDuration *result);
extern StdDuration std_Instant_saturating_duration_since(const StdInstant *self, StdInstant earlier);
extern bool std_Instant_checked_add(const StdInstant *self, StdDuration duration,
									StdInstant *result);
extern StdInstant std_Instant_add(StdInstant self, StdDuration other, const char *file, uint32 line, uint32 col);
extern bool std_Instant_Debug_fmt(const void *value, StdFormatter *f);

extern StdSystemTime std_SystemTime_now(void);
extern bool std_SystemTime_duration_since(const StdSystemTime *self, StdSystemTime earlier,
										  StdDuration *result, StdDuration *error);
extern bool std_SystemTime_checked_add(const StdSystemTime *self, StdDuration duration,
									   StdSystemTime *result);
extern StdSystemTime std_SystemTime_add(StdSystemTime self, StdDuration dur, const char *file,
										uint32 line, uint32 col);
extern bool std_SystemTime_checked_sub(const StdSystemTime *self, StdDuration duration, StdSystemTime *result);
extern StdSystemTime std_SystemTime_sub(StdSystemTime self, StdDuration dur, const char *file, uint32 line,
										uint32 col);
extern bool std_SystemTime_eq(const StdSystemTime *self, const StdSystemTime *other);
extern bool std_SystemTime_Debug_fmt(const void *value, StdFormatter *f);

#endif							/* DOLTGRES_STD_TIME_H */
