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
 * platform's std/src/sys/time at Rust 1.99.1. Rust's std is built without
 * overflow checks, so its arithmetic here wraps as the Rust's does.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <time.h>

#include "doltgres/std/atomic.h"
#include "doltgres/std/io.h"
#include "doltgres/std/num.h"
#include "doltgres/std/time.h"
#include "doltgres/std/unwind.h"

#define CORE_TIME_FILE STD_RUST_LIBRARY STD_SEP "core" STD_SEP "src" STD_SEP "time.rs"
#define STD_TIME_FILE STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "time.rs"

/* STD_UNWRAP_ERR is Result::unwrap's message of an Err, before the error's Debug. */
#define STD_UNWRAP_ERR "called `Result::unwrap()` on an `Err` value"

/* NANOS_PER_SEC is core's count of nanoseconds in a second. */
#define NANOS_PER_SEC ((uint32) 1000000000)

/* NANOS_PER_MILLI is core's count of nanoseconds in a millisecond. */
#define NANOS_PER_MILLI ((uint32) 1000000)

/* NANOS_PER_MICRO is core's count of nanoseconds in a microsecond. */
#define NANOS_PER_MICRO ((uint32) 1000)

/* MICROS_PER_SEC is core's count of microseconds in a second. */
#define MICROS_PER_SEC UINT64CONST(1000000)

/* MILLIS_PER_SEC is core's count of milliseconds in a second. */
#define MILLIS_PER_SEC UINT64CONST(1000)

/* std_Duration_new is Duration::new: the nanoseconds past a second carried into the seconds. */
StdDuration
std_Duration_new(uint64 secs, uint32 nanos)
{
	if (nanos < NANOS_PER_SEC)
	{
		return (StdDuration)
		{
			secs, nanos
		};
	}
	else
	{
		uint64		carried = secs + (uint64) (nanos / NANOS_PER_SEC);

		if (carried < secs)
		{
			static const char text[] = "overflow in Duration::new";

			std_option_expect_failed(text, sizeof(text) - 1, CORE_TIME_FILE, 201, 18);
		}
		return (StdDuration)
		{
			carried, nanos % NANOS_PER_SEC
		};
	}
}

/* std_Duration_Default_default is Duration's derived Default: zero. */
StdDuration
std_Duration_Default_default(void)
{
	StdDuration duration = {0};

	return duration;
}

/* std_Duration_from_secs is Duration::from_secs. */
StdDuration
std_Duration_from_secs(uint64 secs)
{
	return (StdDuration)
	{
		secs, 0
	};
}

/* std_Duration_from_nanos is Duration::from_nanos. */
StdDuration
std_Duration_from_nanos(uint64 nanos)
{
	uint64		secs = nanos / (uint64) NANOS_PER_SEC;
	uint32		subsec_nanos = (uint32) (nanos % (uint64) NANOS_PER_SEC);

	return (StdDuration)
	{
		secs, subsec_nanos
	};
}

/* std_Duration_from_micros is Duration::from_micros: the seconds, and the rest's nanoseconds. */
StdDuration
std_Duration_from_micros(uint64 micros)
{
	uint64		secs = micros / MICROS_PER_SEC;
	uint32		subsec_micros = (uint32) (micros % MICROS_PER_SEC);

	return (StdDuration)
	{
		secs, subsec_micros * NANOS_PER_MICRO
	};
}

/* std_Duration_as_secs is Duration::as_secs. */
uint64
std_Duration_as_secs(const StdDuration *self)
{
	return self->secs;
}

/* std_Duration_subsec_nanos is Duration::subsec_nanos. */
uint32
std_Duration_subsec_nanos(const StdDuration *self)
{
	return self->nanos;
}

/* std_Duration_as_millis is Duration::as_millis: the seconds' milliseconds and the nanoseconds' whole ones, as u128. */
StdU128
std_Duration_as_millis(const StdDuration *self)
{
	StdU128		millis = std_mul_u64_wide(self->secs, (uint64) MILLIS_PER_SEC);
	uint64		lo = millis.lo + (uint64) (self->nanos / NANOS_PER_MILLI);

	return (StdU128)
	{
		millis.hi + (lo < millis.lo ? 1 : 0), lo
	};
}

/* std_Duration_as_nanos is Duration::as_nanos, which can't overflow a u128. */
StdU128
std_Duration_as_nanos(const StdDuration *self)
{
	StdU128		nanos = std_mul_u64_wide(self->secs, (uint64) NANOS_PER_SEC);
	uint64		lo = nanos.lo + (uint64) self->nanos;

	return (StdU128)
	{
		nanos.hi + (lo < nanos.lo ? 1 : 0), lo
	};
}

/* std_Duration_checked_add is Duration::checked_add. */
bool
std_Duration_checked_add(StdDuration self, StdDuration rhs, StdDuration *result)
{
	uint64		secs = self.secs + rhs.secs;

	if (secs >= self.secs)
	{
		uint32		nanos = self.nanos + rhs.nanos;

		if (nanos >= NANOS_PER_SEC)
		{
			uint64		new_secs = secs + 1;

			nanos -= NANOS_PER_SEC;
			if (new_secs < secs)
				return false;
			secs = new_secs;
		}
		*result = std_Duration_new(secs, nanos);
		return true;
	}
	else
		return false;
}

/* std_Duration_checked_sub is Duration::checked_sub. */
bool
std_Duration_checked_sub(StdDuration self, StdDuration rhs, StdDuration *result)
{
	if (rhs.secs <= self.secs)
	{
		uint64		secs = self.secs - rhs.secs;
		uint32		nanos;

		if (self.nanos >= rhs.nanos)
			nanos = self.nanos - rhs.nanos;
		else if (secs >= 1)
		{
			secs = secs - 1;
			nanos = self.nanos + NANOS_PER_SEC - rhs.nanos;
		}
		else
			return false;
		*result = std_Duration_new(secs, nanos);
		return true;
	}
	else
		return false;
}

/* std_Duration_eq is Duration's derived PartialEq. */
bool
std_Duration_eq(const StdDuration *self, const StdDuration *other)
{
	return self->secs == other->secs && self->nanos == other->nanos;
}

/* std_Duration_cmp is Duration's derived Ord: the seconds, then the nanoseconds. */
int
std_Duration_cmp(const StdDuration *self, const StdDuration *other)
{
	if (self->secs != other->secs)
		return self->secs < other->secs ? -1 : 1;
	if (self->nanos != other->nanos)
		return self->nanos < other->nanos ? -1 : 1;
	return 0;
}

/*
 * std_time_emit_without_padding is fmt_decimal's closure of the same name: the
 * duration's text without its padding, its integer part None for u64::MAX + 1.
 */
static bool
std_time_emit_without_padding(StdFormatter *f, const char *prefix, bool has_integer_part,
							  uint64 integer_part, const char *buf, size_t end, size_t pos,
							  const char *postfix)
{
	if (has_integer_part)
	{
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_ARG(1)};
		StdStr		prefix_str = {prefix, strlen(prefix)};
		StdFmtArg	args[] = {{&prefix_str, std_str_Display_fmt}, {&integer_part, std_u64_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		if (!std_fmt_write_fmt_Unchecked(f, &arguments))
			return false;
	}
	else
	{
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT("18446744073709551616")};
		StdStr		prefix_str = {prefix, strlen(prefix)};
		StdFmtArg	args[] = {{&prefix_str, std_str_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		if (!std_fmt_write_fmt_Unchecked(f, &arguments))
			return false;
	}
	if (end > 0)
	{
		StdStr		s;
		size_t		w;
		StdFmtPiece pieces[] = {STD_FMT_LIT("."), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&s, std_str_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_range_to_check(end, 9, CORE_TIME_FILE, 1504, 74);
		s = (StdStr)
		{
			buf, end
		};
		w = f->options.has_precision ? f->options.precision : pos;
		if (w > PG_UINT16_MAX)
		{
			static const char text[] = "Formatting argument out of range";

			std_panic(CORE_TIME_FILE, 1508, 21, text, sizeof(text) - 1);
		}
		pieces[1].spec.fill = '0';
		pieces[1].spec.align = STD_FMT_ALIGN_LEFT;
		pieces[1].spec.has_width = true;
		pieces[1].spec.width = (uint16) w;
		if (!std_fmt_write_fmt_Unchecked(f, &arguments))
			return false;
	}
	{
		StdStr		postfix_str = {postfix, strlen(postfix)};
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&postfix_str, std_str_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		return std_fmt_write_fmt_Unchecked(f, &arguments);
	}
}

/*
 * std_time_fmt_decimal is Duration's Debug's fmt_decimal: a number in decimal
 * notation from its integer part and its fractional part over a divisor,
 * trailing zeros left out, rounded to the precision, with a prefix and a
 * postfix, padded to the formatter's width.
 */
static bool
std_time_fmt_decimal(StdFormatter *f, uint64 integer_part, uint32 fractional_part, uint32 divisor,
					 const char *prefix, const char *postfix, size_t postfix_chars)
{
	char		buf[9];
	size_t		pos = 0;
	size_t		limit = f->options.has_precision ? f->options.precision : 9;
	bool		has_integer_part = true;
	size_t		end;

	memset(buf, '0', sizeof(buf));
	while (fractional_part > 0 && pos < limit)
	{
		uint32		digit = std_div_u32(fractional_part, divisor, CORE_TIME_FILE, 1420, 35);

		std_index_check(pos, sizeof(buf), CORE_TIME_FILE, 1420, 17);
		buf[pos] = (char) ('0' + (uint8) digit);
		fractional_part = std_rem_u32(fractional_part, divisor, CORE_TIME_FILE, 1422, 17);
		divisor /= 10;
		pos += 1;
	}
	if (fractional_part > 0 && fractional_part >= divisor * 5)
	{
		bool		is_tie = fractional_part == divisor * 5;
		bool		last_digit_is_odd;

		if (pos > 0)
		{
			std_index_check(pos - 1, sizeof(buf), CORE_TIME_FILE, 1439, 22);
			last_digit_is_odd = (uint8) (buf[pos - 1] - '0') % 2 == 1;
		}
		else
			last_digit_is_odd = (integer_part % 2) == 1;

		if (!(is_tie && !last_digit_is_odd))
		{
			size_t		rev_pos = pos;
			bool		carry = true;

			while (carry && rev_pos > 0)
			{
				rev_pos -= 1;
				std_index_check(rev_pos, sizeof(buf), CORE_TIME_FILE, 1459, 28);
				if (buf[rev_pos] < '9')
				{
					std_index_check(rev_pos, sizeof(buf), CORE_TIME_FILE, 1460, 29);
					buf[rev_pos] += 1;
					carry = false;
				}
				else
				{
					std_index_check(rev_pos, sizeof(buf), CORE_TIME_FILE, 1463, 29);
					buf[rev_pos] = '0';
				}
			}
			if (carry)
			{
				has_integer_part = integer_part != PG_UINT64_MAX;
				integer_part = integer_part + 1;
			}
		}
	}
	end = f->options.has_precision ? std_min_usize((size_t) f->options.precision, 9) : pos;

	if (!f->options.has_width)
		return std_time_emit_without_padding(f, prefix, has_integer_part, integer_part, buf, end, pos,
											 postfix);
	else
	{
		size_t		requested_w = f->options.width;
		size_t		actual_w = strlen(prefix) + postfix_chars;

		if (has_integer_part)
		{
			if (integer_part > 0)
			{
				uint32		log = 0;
				uint64		x = integer_part;

				while (x >= 10)
				{
					x /= 10;
					log++;
				}
				actual_w += 1 + (size_t) log;
			}
			else
				actual_w += 1;
		}
		else
			actual_w += 20;
		if (end > 0)
		{
			size_t		frac_part_w = f->options.has_precision ? f->options.precision : pos;

			actual_w += 1 + frac_part_w;
		}

		if (requested_w <= actual_w)
			return std_time_emit_without_padding(f, prefix, has_integer_part, integer_part, buf, end, pos,
												 postfix);
		else
		{
			uint32		fill;
			uint16		post;

			if (!std_fmt_padding(f, (uint16) (requested_w - actual_w), STD_FMT_ALIGN_LEFT, &fill, &post))
				return false;
			if (!std_time_emit_without_padding(f, prefix, has_integer_part, integer_part, buf, end, pos,
											   postfix))
				return false;
			return std_fmt_post_padding(f, fill, post);
		}
	}
}

/* std_Duration_Debug_fmt is Duration's Debug: the largest unit below the value, with a decimal fraction. */
bool
std_Duration_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdDuration *self = (const StdDuration *) value;
	const char *prefix = f->options.sign_plus ? "+" : "";

	if (self->secs > 0)
		return std_time_fmt_decimal(f, self->secs, self->nanos, NANOS_PER_SEC / 10, prefix, "s", 1);
	else if (self->nanos >= NANOS_PER_MILLI)
		return std_time_fmt_decimal(f, (uint64) (self->nanos / NANOS_PER_MILLI),
									self->nanos % NANOS_PER_MILLI, NANOS_PER_MILLI / 10, prefix, "ms", 2);
	else if (self->nanos >= NANOS_PER_MICRO)
		return std_time_fmt_decimal(f, (uint64) (self->nanos / NANOS_PER_MICRO),
									self->nanos % NANOS_PER_MICRO, NANOS_PER_MICRO / 10, prefix,
									"\xC2\xB5s", 2);
	else
		return std_time_fmt_decimal(f, (uint64) self->nanos, 0, 1, prefix, "ns", 2);
}

#ifdef WIN32

/* WINDOWS_TIME_FILE is std's Windows time.rs, where perf_counter's cvt(..).unwrap() calls are. */
#define WINDOWS_TIME_FILE STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "sys" STD_SEP "pal" STD_SEP "windows" \
	STD_SEP "time.rs"

/* INTERVALS_PER_SEC is Windows' count of FILETIME intervals in a second. */
#define INTERVALS_PER_SEC ((uint64) NANOS_PER_SEC / 100)

/* frequency_cache is perf_counter::frequency's FREQUENCY, zero until it is first learned. */
static StdAtomicI64 frequency_cache = STD_ATOMIC_I64_INIT(0);

/*
 * std_time_perf_counter_now is perf_counter::now. The counter's failure, which
 * Windows never gives since XP, panics as std's unwrap does, as the frequency's does.
 */
static int64
std_time_perf_counter_now(void)
{
	LARGE_INTEGER qpc_value;

	if (!QueryPerformanceCounter(&qpc_value))
	{
		StdIoError	error = std_io_Error_last_os_error();

		std_result_unwrap_failed(STD_UNWRAP_ERR, sizeof(STD_UNWRAP_ERR) - 1, &error, std_io_Error_Debug_fmt,
								 WINDOWS_TIME_FILE, 29, 68);
	}
	return qpc_value.QuadPart;
}

/* std_time_perf_counter_frequency_init is perf_counter::frequency_init: the frequency learned and cached. */
static int64
std_time_perf_counter_frequency_init(StdAtomicI64 *cache)
{
	LARGE_INTEGER frequency;

	if (!QueryPerformanceFrequency(&frequency))
	{
		StdIoError	error = std_io_Error_last_os_error();

		std_result_unwrap_failed(STD_UNWRAP_ERR, sizeof(STD_UNWRAP_ERR) - 1, &error, std_io_Error_Debug_fmt,
								 WINDOWS_TIME_FILE, 53, 63);
	}
	std_AtomicI64_store_relaxed(cache, frequency.QuadPart);
	return frequency.QuadPart;
}

/* std_time_perf_counter_frequency is perf_counter::frequency: the counter's ticks in a second, learned once. */
static int64
std_time_perf_counter_frequency(void)
{
	int64		cached = std_AtomicI64_load_relaxed(&frequency_cache);

	if (cached != 0)
		return cached;
	return std_time_perf_counter_frequency_init(&frequency_cache);
}

/* std_time_perf_counter_epsilon is perf_counter::epsilon: the counter's margin of error. */
static StdDuration
std_time_perf_counter_epsilon(void)
{
	uint64		epsilon = std_div_u64((uint64) NANOS_PER_SEC, (uint64) std_time_perf_counter_frequency(),
									  WINDOWS_TIME_FILE, 69, 23);

	return std_Duration_from_nanos(epsilon);
}

/* HELPERS_FILE is std's sys/helpers/mod.rs, where mul_div_u64's divisions report. */
#define HELPERS_FILE STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "sys" STD_SEP "helpers" STD_SEP "mod.rs"

/* std_time_mul_div_u64 is sys::helpers::mul_div_u64: a value times a numerator over a denominator without overflow. */
static uint64
std_time_mul_div_u64(uint64 value, uint64 numerator, uint64 denom)
{
	uint64		q = std_div_u64(value, denom, HELPERS_FILE, 24, 13);
	uint64		r = std_rem_u64(value, denom, HELPERS_FILE, 25, 13);

	return q * numerator + std_div_u64(r * numerator, denom, HELPERS_FILE, 29, 21);
}

/* std_time_checked_dur2intervals is Windows' checked_dur2intervals. */
static bool
std_time_checked_dur2intervals(const StdDuration *dur, int64 *result)
{
	uint64		intervals;
	uint64		total;

	if (!std_checked_mul_u64(std_Duration_as_secs(dur), INTERVALS_PER_SEC, &intervals))
		return false;
	if (!std_checked_add_u64(intervals, (uint64) std_Duration_subsec_nanos(dur) / 100, &total))
		return false;
	if (total > (uint64) PG_INT64_MAX)
		return false;
	*result = (int64) total;
	return true;
}

/* std_time_intervals2dur is Windows' intervals2dur. */
static StdDuration
std_time_intervals2dur(uint64 intervals)
{
	return std_Duration_new(intervals / INTERVALS_PER_SEC,
							(uint32) ((intervals % INTERVALS_PER_SEC) * 100));
}

/* std_time_from_intervals is Windows' SystemTime::from_intervals. */
static StdSystemTime
std_time_from_intervals(int64 intervals)
{
	return (StdSystemTime)
	{
		(uint32) intervals, (uint32) (intervals >> 32)
	};
}

/* std_time_intervals is Windows' SystemTime::intervals. */
static int64
std_time_intervals(const StdSystemTime *self)
{
	return (int64) ((uint64) self->dwLowDateTime | ((uint64) self->dwHighDateTime << 32));
}

/* STD_UNIX_EPOCH is UNIX_EPOCH: 1970 in intervals since 1601. */
const StdSystemTime STD_UNIX_EPOCH = {
	(uint32) (INT64CONST(11644473600) * (int64) INTERVALS_PER_SEC),
	(uint32) ((INT64CONST(11644473600) * (int64) INTERVALS_PER_SEC) >> 32),
};

/* std_Instant_now is Windows' Instant::now: the performance counter in nanoseconds, shifted off zero. */
StdInstant
std_Instant_now(void)
{
	uint64		freq = (uint64) std_time_perf_counter_frequency();
	int64		now = std_time_perf_counter_now();
	uint64		instant_nsec = std_time_mul_div_u64((uint64) now, (uint64) NANOS_PER_SEC, freq);

	instant_nsec = instant_nsec + (PG_UINT64_MAX / 4);
	return (StdInstant)
	{
		std_Duration_from_nanos(instant_nsec)
	};
}

/*
 * std_Duration_Sub_sub is Duration's Sub: the difference, panicking with
 * core's expect at its own location on an overflow.
 */
static StdDuration
std_Duration_Sub_sub(StdDuration self, StdDuration rhs)
{
	StdDuration result;

	if (!std_Duration_checked_sub(self, rhs, &result))
	{
		static const char text[] = "overflow when subtracting durations";

		std_option_expect_failed(text, sizeof(text) - 1, CORE_TIME_FILE, 1277, 31);
	}
	return result;
}

/* std_time_checked_sub_instant is Windows' Instant::checked_sub_instant, equal within the counter's margin. */
static bool
std_time_checked_sub_instant(const StdInstant *self, const StdInstant *other, StdDuration *result)
{
	StdDuration epsilon = std_time_perf_counter_epsilon();

	if (std_Duration_cmp(&other->t, &self->t) > 0)
	{
		StdDuration apart = std_Duration_Sub_sub(other->t, self->t);

		if (std_Duration_cmp(&apart, &epsilon) <= 0)
		{
			*result = std_Duration_new(0, 0);
			return true;
		}
	}
	return std_Duration_checked_sub(self->t, other->t, result);
}

/* std_time_checked_add_instant is Windows' Instant::checked_add_duration. */
static bool
std_time_checked_add_instant(const StdInstant *self, const StdDuration *other, StdInstant *result)
{
	StdDuration t;

	if (!std_Duration_checked_add(self->t, *other, &t))
		return false;
	*result = (StdInstant)
	{
		t
	};
	return true;
}

/* std_SystemTime_now is Windows' SystemTime::now. */
StdSystemTime
std_SystemTime_now(void)
{
	FILETIME	t;

	GetSystemTimePreciseAsFileTime(&t);
	return (StdSystemTime)
	{
		t.dwLowDateTime, t.dwHighDateTime
	};
}

/* std_time_sub_time is Windows' SystemTime::sub_time: the distance, Err when the other is later. */
static bool
std_time_sub_time(const StdSystemTime *self, const StdSystemTime *other, StdDuration *result,
				  StdDuration *error)
{
	int64		me = std_time_intervals(self);
	int64		them = std_time_intervals(other);

	if (me >= them)
	{
		*result = std_time_intervals2dur((uint64) me - (uint64) them);
		return true;
	}
	else
	{
		*error = std_time_intervals2dur((uint64) them - (uint64) me);
		return false;
	}
}

/* std_time_checked_add_time is Windows' SystemTime::checked_add_duration. */
static bool
std_time_checked_add_time(const StdSystemTime *self, const StdDuration *other, StdSystemTime *result)
{
	int64		tmp_1 = std_time_intervals(self);
	int64		add;
	int64		sum;

	if (!std_time_checked_dur2intervals(other, &add))
		return false;
	if (!std_checked_add_i64(tmp_1, add, &sum))
		return false;
	*result = std_time_from_intervals(sum);
	return true;
}

/* std_time_checked_sub_time is Windows' SystemTime::checked_sub_duration. */
static bool
std_time_checked_sub_time(const StdSystemTime *self, const StdDuration *other, StdSystemTime *result)
{
	int64		tmp_1 = std_time_intervals(self);
	int64		sub;
	int64		difference;

	if (!std_time_checked_dur2intervals(other, &sub))
		return false;
	if (!std_checked_sub_i64(tmp_1, sub, &difference))
		return false;
	*result = std_time_from_intervals(difference);
	return true;
}

/* std_SystemTime_eq is Windows' SystemTime's PartialEq, by intervals. */
bool
std_SystemTime_eq(const StdSystemTime *self, const StdSystemTime *other)
{
	return std_time_intervals(self) == std_time_intervals(other);
}

/* std_SystemTime_Debug_fmt is Windows' SystemTime's Debug, its intervals. */
bool
std_SystemTime_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdSystemTime *self = (const StdSystemTime *) value;
	int64		i = std_time_intervals(self);
	StdDebugStruct b = std_fmt_debug_struct(f, "SystemTime", 10);

	std_DebugStruct_field(&b, "intervals", 9, &i, std_i64_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* std_Instant_Debug_fmt is Windows' Instant's derived Debug. */
bool
std_Instant_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdInstant *self = (const StdInstant *) value;
	StdDebugStruct b = std_fmt_debug_struct(f, "Instant", 7);

	std_DebugStruct_field(&b, "t", 1, &self->t, std_Duration_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

#else

#define UNIX_TIME_FILE STD_RUST_LIBRARY STD_SEP "std" STD_SEP "src" STD_SEP "sys" STD_SEP "pal" STD_SEP "unix" STD_SEP "time.rs"

/* NSEC_PER_SEC is Unix's count of nanoseconds in a second. */
#define NSEC_PER_SEC UINT64CONST(1000000000)

/* STD_UNIX_EPOCH is UNIX_EPOCH, Timespec::zero. */
const StdSystemTime STD_UNIX_EPOCH = {{0, 0}};

/*
 * std_time_Timespec_new is Timespec::new: Apple's form of a time before the
 * epoch, negative nanoseconds in its first second, put in the usual form
 * first, then the nanoseconds checked, an InvalidData error when out of
 * range.
 */
static bool
std_time_Timespec_new(int64 tv_sec, int64 tv_nsec, StdTimespec *result, StdIoError *error)
{
#ifdef __APPLE__
	if ((tv_sec <= 0 && tv_sec > PG_INT64_MIN) && (tv_nsec < 0 && tv_nsec > -1000000000))
	{
		tv_sec = tv_sec - 1;
		tv_nsec = tv_nsec + 1000000000;
	}
#endif
	if (tv_nsec >= 0 && tv_nsec < (int64) NSEC_PER_SEC)
	{
		result->tv_sec = tv_sec;
		result->tv_nsec = (uint32) tv_nsec;
		return true;
	}
	{
		static const StdSimpleMessage invalid = {STD_ERROR_KIND_INVALID_DATA, "invalid timestamp",
		sizeof("invalid timestamp") - 1};

		*error = std_io_Error_from_static_message(&invalid);
	}
	return false;
}

/*
 * std_time_timespec_now is Timespec::now: a clock's reading, the clock's
 * failure and Timespec::new's error each unwrapped, as std's cvt(..).unwrap()
 * and Timespec::new(..).unwrap() do.
 */
static StdTimespec
std_time_timespec_now(clockid_t clock)
{
	struct timespec t;
	StdTimespec result;
	StdIoError	error;

	if (clock_gettime(clock, &t) != 0)
	{
		error = std_io_Error_last_os_error();
		std_result_unwrap_failed(STD_UNWRAP_ERR, sizeof(STD_UNWRAP_ERR) - 1, &error, std_io_Error_Debug_fmt,
								 UNIX_TIME_FILE, 107, 68);
	}
	if (!std_time_Timespec_new((int64) t.tv_sec, (int64) t.tv_nsec, &result, &error))
		std_result_unwrap_failed(STD_UNWRAP_ERR, sizeof(STD_UNWRAP_ERR) - 1, &error, std_io_Error_Debug_fmt,
								 UNIX_TIME_FILE, 109, 58);
	return result;
}

/* std_time_timespec_ge is Timespec's derived >=: the seconds, then the nanoseconds. */
static bool
std_time_timespec_ge(const StdTimespec *self, const StdTimespec *other)
{
	if (self->tv_sec != other->tv_sec)
		return self->tv_sec > other->tv_sec;
	return self->tv_nsec >= other->tv_nsec;
}

/* std_time_sub_timespec_sub_ge_to_unsigned is sub_timespec's sub_ge_to_unsigned: a - b, which fits a u64 when a >= b. */
static uint64
std_time_sub_timespec_sub_ge_to_unsigned(int64 a, int64 b)
{
	return (uint64) std_wrapping_sub_i64(a, b);
}

/* std_time_sub_timespec is Timespec::sub_timespec: the distance, Err when the other is later. */
static bool
std_time_sub_timespec(const StdTimespec *self, const StdTimespec *other, StdDuration *result,
					  StdDuration *error)
{
	if (std_time_timespec_ge(self, other))
	{
		uint64		secs;
		uint32		nsec;

		if (self->tv_nsec >= other->tv_nsec)
		{
			secs = std_time_sub_timespec_sub_ge_to_unsigned(self->tv_sec, other->tv_sec);
			nsec = self->tv_nsec - other->tv_nsec;
		}
		else
		{
			secs = std_time_sub_timespec_sub_ge_to_unsigned(std_wrapping_sub_i64(self->tv_sec, 1),
															other->tv_sec);
			nsec = self->tv_nsec + (uint32) NSEC_PER_SEC - other->tv_nsec;
		}
		*result = std_Duration_new(secs, nsec);
		return true;
	}
	else
		return !std_time_sub_timespec(other, self, error, result);
}

/* std_time_checked_add_timespec is Timespec::checked_add_duration, nothing when the seconds overflow. */
static bool
std_time_checked_add_timespec(const StdTimespec *self, const StdDuration *other, StdTimespec *result)
{
	int64		secs;
	uint32		nsec;

	if (!std_checked_add_unsigned_i64(self->tv_sec, std_Duration_as_secs(other), &secs))
		return false;
	nsec = std_Duration_subsec_nanos(other) + self->tv_nsec;
	if (nsec >= (uint32) NSEC_PER_SEC)
	{
		nsec -= (uint32) NSEC_PER_SEC;
		if (!std_checked_add_i64(secs, 1, &secs))
			return false;
	}
	result->tv_sec = secs;
	result->tv_nsec = nsec;
	return true;
}

/* std_time_checked_sub_timespec is Timespec::checked_sub_duration, nothing when the seconds overflow. */
static bool
std_time_checked_sub_timespec(const StdTimespec *self, const StdDuration *other, StdTimespec *result)
{
	int64		secs;
	int32		nsec;

	if (!std_checked_sub_unsigned_i64(self->tv_sec, std_Duration_as_secs(other), &secs))
		return false;
	nsec = (int32) self->tv_nsec - (int32) std_Duration_subsec_nanos(other);
	if (nsec < 0)
	{
		nsec += (int32) NSEC_PER_SEC;
		if (!std_checked_sub_i64(secs, 1, &secs))
			return false;
	}
	result->tv_sec = secs;
	result->tv_nsec = (uint32) nsec;
	return true;
}

/* instant_clock is Instant::CLOCK_ID: CLOCK_UPTIME_RAW on Apple's systems and CLOCK_MONOTONIC elsewhere. */
#ifdef __APPLE__
#define INSTANT_CLOCK CLOCK_UPTIME_RAW
#else
#define INSTANT_CLOCK CLOCK_MONOTONIC
#endif

/* std_Instant_now is Unix's Instant::now. */
StdInstant
std_Instant_now(void)
{
	return (StdInstant)
	{
		std_time_timespec_now(INSTANT_CLOCK)
	};
}

/* std_time_checked_sub_instant is Unix's Instant::checked_sub_instant. */
static bool
std_time_checked_sub_instant(const StdInstant *self, const StdInstant *other, StdDuration *result)
{
	StdDuration error;

	return std_time_sub_timespec(&self->t, &other->t, result, &error);
}

/* std_time_checked_add_instant is Unix's Instant::checked_add_duration. */
static bool
std_time_checked_add_instant(const StdInstant *self, const StdDuration *other, StdInstant *result)
{
	StdTimespec t;

	if (!std_time_checked_add_timespec(&self->t, other, &t))
		return false;
	*result = (StdInstant)
	{
		t
	};
	return true;
}

/* std_SystemTime_now is Unix's SystemTime::now. */
StdSystemTime
std_SystemTime_now(void)
{
	return (StdSystemTime)
	{
		std_time_timespec_now(CLOCK_REALTIME)
	};
}

/* std_time_sub_time is Unix's SystemTime::sub_time. */
static bool
std_time_sub_time(const StdSystemTime *self, const StdSystemTime *other, StdDuration *result,
				  StdDuration *error)
{
	return std_time_sub_timespec(&self->t, &other->t, result, error);
}

/* std_time_checked_add_time is Unix's SystemTime::checked_add_duration. */
static bool
std_time_checked_add_time(const StdSystemTime *self, const StdDuration *other, StdSystemTime *result)
{
	StdTimespec t;

	if (!std_time_checked_add_timespec(&self->t, other, &t))
		return false;
	*result = (StdSystemTime)
	{
		t
	};
	return true;
}

/* std_time_checked_sub_time is Unix's SystemTime::checked_sub_duration. */
static bool
std_time_checked_sub_time(const StdSystemTime *self, const StdDuration *other, StdSystemTime *result)
{
	StdTimespec t;

	if (!std_time_checked_sub_timespec(&self->t, other, &t))
		return false;
	*result = (StdSystemTime)
	{
		t
	};
	return true;
}

/* std_SystemTime_eq is Unix's SystemTime's derived PartialEq. */
bool
std_SystemTime_eq(const StdSystemTime *self, const StdSystemTime *other)
{
	return self->t.tv_sec == other->t.tv_sec && self->t.tv_nsec == other->t.tv_nsec;
}

/* std_SystemTime_Debug_fmt is Unix's SystemTime's Debug, its seconds and nanoseconds. */
bool
std_SystemTime_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdSystemTime *self = (const StdSystemTime *) value;
	StdDebugStruct b = std_fmt_debug_struct(f, "SystemTime", 10);

	std_DebugStruct_field(&b, "tv_sec", 6, &self->t.tv_sec, std_i64_Debug_fmt);
	std_DebugStruct_field(&b, "tv_nsec", 7, &self->t.tv_nsec, std_u32_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* std_Instant_Debug_fmt is Unix's Instant's Debug, its seconds and nanoseconds. */
bool
std_Instant_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdInstant *self = (const StdInstant *) value;
	StdDebugStruct b = std_fmt_debug_struct(f, "Instant", 7);

	std_DebugStruct_field(&b, "tv_sec", 6, &self->t.tv_sec, std_i64_Debug_fmt);
	std_DebugStruct_field(&b, "tv_nsec", 7, &self->t.tv_nsec, std_u32_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

#endif

/* std_Instant_checked_duration_since is Instant::checked_duration_since. */
bool
std_Instant_checked_duration_since(const StdInstant *self, StdInstant earlier,
								   StdDuration *result)
{
	return std_time_checked_sub_instant(self, &earlier, result);
}

/* std_Instant_saturating_duration_since is Instant::saturating_duration_since, zero when the earlier one is later. */
StdDuration
std_Instant_saturating_duration_since(const StdInstant *self, StdInstant earlier)
{
	StdDuration result;

	if (std_Instant_checked_duration_since(self, earlier, &result))
		return result;
	return STD_DURATION_ZERO;
}

/* std_Instant_checked_add is Instant::checked_add. */
bool
std_Instant_checked_add(const StdInstant *self, StdDuration duration, StdInstant *result)
{
	return std_time_checked_add_instant(self, &duration, result);
}

/*
 * std_Instant_add is Instant's Add<Duration>, which panics on overflow at
 * its caller's location, since it is #[track_caller].
 */
StdInstant
std_Instant_add(StdInstant self, StdDuration other, const char *file, uint32 line, uint32 col)
{
	StdInstant	result;

	if (!std_Instant_checked_add(&self, other, &result))
	{
		static const char text[] = "overflow when adding duration to instant";

		std_option_expect_failed(text, sizeof(text) - 1, file, line, col);
	}
	return result;
}

/* std_SystemTime_duration_since is SystemTime::duration_since: Err with the distance when the earlier one is later. */
bool
std_SystemTime_duration_since(const StdSystemTime *self, StdSystemTime earlier,
							  StdDuration *result, StdDuration *error)
{
	return std_time_sub_time(self, &earlier, result, error);
}

/* std_SystemTime_checked_add is SystemTime::checked_add. */
bool
std_SystemTime_checked_add(const StdSystemTime *self, StdDuration duration,
						   StdSystemTime *result)
{
	return std_time_checked_add_time(self, &duration, result);
}

/*
 * std_SystemTime_add is SystemTime's Add<Duration>, which panics on overflow
 * at its caller's location, since it is #[track_caller].
 */
StdSystemTime
std_SystemTime_add(StdSystemTime self, StdDuration dur, const char *file, uint32 line, uint32 col)
{
	StdSystemTime result;

	if (!std_SystemTime_checked_add(&self, dur, &result))
	{
		static const char text[] = "overflow when adding duration to `SystemTime`";

		std_option_expect_failed(text, sizeof(text) - 1, file, line, col);
	}
	return result;
}

/* std_SystemTime_checked_sub is SystemTime::checked_sub, nothing on overflow. */
bool
std_SystemTime_checked_sub(const StdSystemTime *self, StdDuration duration, StdSystemTime *result)
{
	return std_time_checked_sub_time(self, &duration, result);
}

/*
 * std_SystemTime_sub is SystemTime's Sub<Duration>, which panics on overflow
 * at its caller's location, since it is #[track_caller].
 */
StdSystemTime
std_SystemTime_sub(StdSystemTime self, StdDuration dur, const char *file, uint32 line, uint32 col)
{
	StdSystemTime result;

	if (!std_SystemTime_checked_sub(&self, dur, &result))
	{
		static const char text[] = "overflow when subtracting duration from `SystemTime`";

		std_option_expect_failed(text, sizeof(text) - 1, file, line, col);
	}
	return result;
}

/* std_Instant_now_call is Instant::now passed as an FnOnce() -> Instant, with no captures. */
void
std_Instant_now_call(void *captures, void *result)
{
	*(StdInstant *) result = std_Instant_now();
}

/* std_SystemTimeError_Debug_fmt is SystemTimeError's derived Debug: the tuple struct of its Duration. */
bool
std_SystemTimeError_Debug_fmt(const void *value, StdFormatter *f)
{
	StdDebugTuple b = std_fmt_debug_tuple(f, "SystemTimeError", 15);

	std_DebugTuple_field(&b, value, std_Duration_Debug_fmt);
	return std_DebugTuple_finish(&b);
}
