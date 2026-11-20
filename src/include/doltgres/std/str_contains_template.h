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
 * The template of core's #[inline] str::contains of a char or a &str, whose
 * precondition checks run where it is codegened (the port text's section 3):
 * <char as Pattern>::is_contained_in, <[u8]>::contains through memchr,
 * char::encode_utf8, and <&str as Pattern>::is_contained_in with
 * simd_contains and StrSearcher's TwoWaySearcher, ported from
 * core/src/str/pattern.rs, core/src/slice/memchr.rs, core/src/slice/iter.rs,
 * and core/src/char/methods.rs at Rust 1.99.1. Both arms are kept, as rustc
 * keeps them at opt-level 0. The including file defines STD_STR_PATTERN_FILE,
 * STD_SLICE_MEMCHR_FILE, STD_SLICE_ITER_FILE, and STD_CHAR_METHODS_FILE.
 * STD_CONTAINS_DECLARE and STD_CONTAINS_DEFINE choose the declarations and
 * definitions, STD_CONTAINS_UB_CHECKS is 1 for the checked instance and 0 for
 * the other, and STD_CONTAINS_SUFFIX names the instance; every parameter is
 * undefined at the end.
 */

/* STD_CONTAINS_CAT pastes two tokens after expanding them. */
#define STD_CONTAINS_CAT_(a, b) a##b
#define STD_CONTAINS_CAT(a, b) STD_CONTAINS_CAT_(a, b)

/* STD_CONTAINS_NAME names the instance's functions. */
#define STD_CONTAINS_NAME(name) STD_CONTAINS_CAT(std_str_##name##_, STD_CONTAINS_SUFFIX)

#ifdef STD_CONTAINS_DECLARE
extern size_t STD_CONTAINS_NAME(encode_utf8) (uint32 code, uint8 *dst, size_t dst_len);
extern bool STD_CONTAINS_NAME(memchr) (uint8 x, const uint8 *text, size_t len, size_t *result);
#ifdef STD_CONTAINS_WITH_CONTAINS
extern bool STD_CONTAINS_NAME(contains_char) (const char *haystack, size_t haystack_len, uint32 c);
extern bool STD_CONTAINS_NAME(contains_str) (const char *haystack, size_t haystack_len, const char *pat,
											 size_t pat_len);
extern bool STD_CONTAINS_NAME(slice_contains_u8) (uint8 x, const uint8 *text, size_t len);
extern bool STD_CONTAINS_NAME(ends_with_str) (const char *haystack, size_t haystack_len, const char *pat,
											  size_t pat_len);
#endif
#ifdef STD_CONTAINS_WITH_SPLIT
extern StdSplitInclusiveChar STD_CONTAINS_NAME(split_inclusive_char) (const char *haystack, size_t haystack_len,
																	  uint32 c);
extern bool STD_CONTAINS_NAME(SplitInclusive_next) (StdSplitInclusiveChar *self, const char **elt, size_t *elt_len);
extern bool STD_CONTAINS_NAME(ends_with_char) (const char *haystack, size_t haystack_len, uint32 c);
#endif
#endif

#ifdef STD_CONTAINS_DEFINE

/* memchr_naive is memchr.rs's memchr_naive: the first index of a byte, by index. */
static bool
STD_CONTAINS_NAME(memchr_naive) (uint8 x, const uint8 *text, size_t len, size_t *result)
{
	size_t		i = 0;

	while (i < len)
	{
		std_index_check(i, len, STD_SLICE_MEMCHR_FILE, 42, 12);
		if (text[i] == x)
		{
			*result = i;
			return true;
		}
		i += 1;
	}
	return false;
}

/* memchr_contains_zero_byte is memchr.rs's contains_zero_byte: whether a word has a zero byte. */
static bool
STD_CONTAINS_NAME(memchr_contains_zero_byte) (size_t x)
{
	const size_t LO_USIZE = (size_t) UINT64CONST(0x0101010101010101);
	const size_t HI_USIZE = (size_t) UINT64CONST(0x8080808080808080);

	return ((x - LO_USIZE) & ~x & HI_USIZE) != 0;
}

/*
 * memchr_aligned is memchr.rs's memchr_aligned at run time: the unaligned
 * head byte by byte, aligned words two at a time while neither has the byte,
 * then the rest byte by byte.
 */
static bool
STD_CONTAINS_NAME(memchr_aligned) (uint8 x, const uint8 *text, size_t len, size_t *result)
{
	const size_t USIZE_BYTES = sizeof(size_t);
	size_t		offset = std_ptr_const_align_offset(text, 1, USIZE_BYTES);
	size_t		repeated_x = (size_t) UINT64CONST(0x0101010101010101) * x;
	size_t		index;

	if (offset > 0)
	{
		offset = std_min_usize(offset, len);
		std_range_to_check(offset, len, STD_SLICE_MEMCHR_FILE, 75, 34);
		if (STD_CONTAINS_NAME(memchr_naive) (x, text, offset, &index))
		{
			*result = index;
			return true;
		}
	}
	while (offset <= len - 2 * USIZE_BYTES)
	{
		size_t		u;
		size_t		v;

		memcpy(&u, text +offset, sizeof(size_t));
		memcpy(&v, text +offset + USIZE_BYTES, sizeof(size_t));
		if (STD_CONTAINS_NAME(memchr_contains_zero_byte) (u ^ repeated_x) ||
			STD_CONTAINS_NAME(memchr_contains_zero_byte) (v ^ repeated_x))
			break;
		offset += USIZE_BYTES * 2;
	}
	if (STD_CONTAINS_NAME(memchr_naive) (x, text +offset, len - offset, &index))
	{
		*result = offset + index;
		return true;
	}
	return false;
}

/* memchr is memchr.rs's memchr: a short text's naive search, a longer one's aligned search. */
bool
STD_CONTAINS_NAME(memchr) (uint8 x, const uint8 *text, size_t len, size_t *result)
{
	bool		found = len < 2 * sizeof(size_t) ?
		STD_CONTAINS_NAME(memchr_naive) (x, text, len, result) :
		STD_CONTAINS_NAME(memchr_aligned) (x, text, len, result);

#if STD_CONTAINS_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	if (found)
		std_hint_assert_unchecked(*result < len, STD_SLICE_MEMCHR_FILE, 31, 18);
#endif
	return found;
}

#ifdef STD_CONTAINS_WITH_CONTAINS
/*
 * slice_contains_u8 is SliceContains for u8: whether memchr finds the byte,
 * read by transmute_copy from a slice from_raw_parts makes.
 */
bool
STD_CONTAINS_NAME(slice_contains_u8) (uint8 x, const uint8 *text, size_t len)
{
	size_t		index;

#if STD_CONTAINS_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	std_mem_transmute_copy_check(1, 1, STD_SLICE_CMP_FILE, 402, 18);
	std_slice_from_raw_parts_check(text, len, 1, 1, STD_SLICE_CMP_FILE, 402, 49);
#endif
	return STD_CONTAINS_NAME(memchr) (x, text, len, &index);
}

/*
 * ends_with_str is str::ends_with of a &str: its is_suffix_of, <[u8]>::ends_with
 * of the haystack's tail.
 */
bool
STD_CONTAINS_NAME(ends_with_str) (const char *haystack, size_t haystack_len, const char *pat, size_t pat_len)
{
	if (!(haystack_len >= pat_len))
		return false;
	std_range_from_check(haystack_len - pat_len, haystack_len, STD_SLICE_MOD_FILE, 2666, 34);
	return memcmp(pat, haystack + haystack_len - pat_len, pat_len) == 0;
}
#endif

/*
 * encode_utf8 is char::encode_utf8 through encode_utf8_raw: the char's
 * UTF-8 in dst, panicking with core's message for a buffer too short.
 */
size_t
STD_CONTAINS_NAME(encode_utf8) (uint32 code, uint8 *dst, size_t dst_len)
{
	size_t		len = std_char_len_utf8(code);

	if (dst_len < len)
	{
		static const StdFmtPiece pieces[] = {STD_FMT_LIT("encode_utf8: need "), STD_FMT_ARG(0),
			STD_FMT_LIT(" bytes to encode U+"), {NULL, 0, 1, {.sign_aware_zero_pad = true, .fill = ' ',
			.align = STD_FMT_ALIGN_UNKNOWN, .has_width = true, .width = 4}},
		STD_FMT_LIT(" but buffer has just "), STD_FMT_ARG(2)};
		StdFmtArg	args[] = {{&len, std_usize_Display_fmt}, {&code, std_u32_UpperHex_fmt},
		{&dst_len, std_usize_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(STD_CHAR_METHODS_FILE, 2518, 9, &arguments);
	}
	(void) std_fmt_encode_utf8(code, (char *) dst);
#if STD_CONTAINS_UB_CHECKS && defined(USE_ASSERT_CHECKING)
	std_slice_from_raw_parts_mut_check(dst, len, 1, 1, STD_CHAR_METHODS_FILE, 2531, 14);
#endif
	return len;
}

#ifdef STD_CONTAINS_WITH_CONTAINS
/* TwoWaySearcher is pattern.rs's TwoWaySearcher: the needle's factorization and the search's position. */
typedef struct STD_CONTAINS_NAME (TwoWaySearcher)
{
	size_t		crit_pos;
	size_t		crit_pos_back;
	size_t		period;
	uint64		byteset;
	size_t		position;
	size_t		end;
	size_t		memory;
	size_t		memory_back;
} STD_CONTAINS_NAME(TwoWaySearcher);

/* maximal_suffix is TwoWaySearcher::maximal_suffix: the critical position and period of a needle's suffix. */
static void
STD_CONTAINS_NAME(maximal_suffix) (const uint8 *arr, size_t arr_len, bool order_greater, size_t *result_left,
								   size_t *result_period)
{
	size_t		left = 0;
	size_t		right = 1;
	size_t		offset = 0;
	size_t		period = 1;

	while (right + offset < arr_len)
	{
		uint8		a = arr[right + offset];
		uint8		b;

		std_index_check(left + offset, arr_len, STD_STR_PATTERN_FILE, 1665, 21);
		b = arr[left + offset];
		if ((a < b && !order_greater) || (a > b && order_greater))
		{
			right += offset + 1;
			offset = 0;
			period = right - left;
		}
		else if (a == b)
		{
			if (offset + 1 == period)
			{
				right += offset + 1;
				offset = 0;
			}
			else
				offset += 1;
		}
		else
		{
			left = right;
			right += 1;
			offset = 0;
			period = 1;
		}
	}
	*result_left = left;
	*result_period = period;
}

/* reverse_maximal_suffix is TwoWaySearcher::reverse_maximal_suffix: the reverse critical position. */
static size_t
STD_CONTAINS_NAME(reverse_maximal_suffix) (const uint8 *arr, size_t n, size_t known_period, bool order_greater)
{
	size_t		left = 0;
	size_t		right = 1;
	size_t		offset = 0;
	size_t		period = 1;

	while (right + offset < n)
	{
		uint8		a;
		uint8		b;

		std_index_check(n - (1 + right + offset), n, STD_STR_PATTERN_FILE, 1711, 21);
		a = arr[n - (1 + right + offset)];
		std_index_check(n - (1 + left + offset), n, STD_STR_PATTERN_FILE, 1712, 21);
		b = arr[n - (1 + left + offset)];
		if ((a < b && !order_greater) || (a > b && order_greater))
		{
			right += offset + 1;
			offset = 0;
			period = right - left;
		}
		else if (a == b)
		{
			if (offset + 1 == period)
			{
				right += offset + 1;
				offset = 0;
			}
			else
				offset += 1;
		}
		else
		{
			left = right;
			right += 1;
			offset = 0;
			period = 1;
		}
		if (period == known_period)
			break;
	}
	return left;
}

/* byteset_create is TwoWaySearcher::byteset_create: a bit per byte's low six bits. */
static uint64
STD_CONTAINS_NAME(byteset_create) (const uint8 *bytes, size_t len)
{
	uint64		a = 0;

	for (size_t i = 0; i < len; i++)
		a = ((uint64) 1 << (bytes[i] & 0x3f)) | a;
	return a;
}

/* TwoWaySearcher_new is TwoWaySearcher::new: the needle's critical factorization and its period's kind. */
static STD_CONTAINS_NAME(TwoWaySearcher)
STD_CONTAINS_NAME(TwoWaySearcher_new) (const uint8 *needle, size_t needle_len, size_t end)
{
	STD_CONTAINS_NAME(TwoWaySearcher) self =
	{
		0
	};
	size_t		crit_pos_false;
	size_t		period_false;
	size_t		crit_pos_true;
	size_t		period_true;
	size_t		crit_pos;
	size_t		period;

	STD_CONTAINS_NAME(maximal_suffix) (needle, needle_len, false, &crit_pos_false, &period_false);
	STD_CONTAINS_NAME(maximal_suffix) (needle, needle_len, true, &crit_pos_true, &period_true);
	if (crit_pos_false > crit_pos_true)
	{
		crit_pos = crit_pos_false;
		period = period_false;
	}
	else
	{
		crit_pos = crit_pos_true;
		period = period_true;
	}
	std_range_to_check(crit_pos, needle_len, STD_STR_PATTERN_FILE, 1398, 18);
	std_range_check(period, period + crit_pos, needle_len, STD_STR_PATTERN_FILE, 1398, 40);
	if (memcmp(needle, needle + period, crit_pos) == 0)
	{
		self.crit_pos = crit_pos;
		self.crit_pos_back = needle_len -
			Max(STD_CONTAINS_NAME(reverse_maximal_suffix) (needle, needle_len, period, false),
				STD_CONTAINS_NAME(reverse_maximal_suffix) (needle, needle_len, period, true));
		self.period = period;
		std_range_to_check(period, needle_len, STD_STR_PATTERN_FILE, 1418, 54);
		self.byteset = STD_CONTAINS_NAME(byteset_create) (needle, period);
		self.end = end;
		self.memory = 0;
		self.memory_back = needle_len;
	}
	else
	{
		self.crit_pos = crit_pos;
		self.crit_pos_back = crit_pos;
		self.period = Max(crit_pos, needle_len - crit_pos) + 1;
		self.byteset = STD_CONTAINS_NAME(byteset_create) (needle, needle_len);
		self.end = end;
		self.memory = SIZE_MAX;
		self.memory_back = SIZE_MAX;
	}
	return self;
}

/*
 * TwoWaySearcher_next is TwoWaySearcher::next of a strategy: MatchOnly's
 * next match (a Done for none), or RejectAndMatch's, which rejects early,
 * as a step: true with its range for a match.
 */
static bool
STD_CONTAINS_NAME(TwoWaySearcher_next) (STD_CONTAINS_NAME(TwoWaySearcher) * self, const uint8 *haystack,
										size_t haystack_len, const uint8 *needle, size_t needle_len,
										bool long_period, bool use_early_reject, size_t *a, size_t *b)
{
	size_t		old_pos = self->position;
	size_t		needle_last = needle_len - 1;

	for (;;)
	{
		uint8		tail_byte;
		size_t		start;
		bool		mismatched = false;

		if (!(self->position + needle_last < haystack_len))
		{
			self->position = haystack_len;
			*a = old_pos;
			*b = self->position;
			return false;
		}
		tail_byte = haystack[self->position + needle_last];
		if (use_early_reject && old_pos != self->position)
		{
			*a = old_pos;
			*b = self->position;
			return false;
		}
		if (!((self->byteset >> (tail_byte & 0x3f)) & 1))
		{
			self->position += needle_len;
			if (!long_period)
				self->memory = 0;
			continue;
		}
		start = long_period ? self->crit_pos : Max(self->crit_pos, self->memory);
		for (size_t i = start; i < needle_len; i++)
		{
			std_index_check(i, needle_len, STD_STR_PATTERN_FILE, 1505, 20);
#if STD_CONTAINS_UB_CHECKS && defined(USE_ASSERT_CHECKING)
			std_slice_get_unchecked_check(self->position + i, haystack_len, STD_STR_PATTERN_FILE, 1505, 52);
#endif
			if (needle[i] != haystack[self->position + i])
			{
				self->position += i - self->crit_pos + 1;
				if (!long_period)
					self->memory = 0;
				mismatched = true;
				break;
			}
		}
		if (mismatched)
			continue;
		start = long_period ? 0 : self->memory;
		for (size_t i = self->crit_pos; i > start; i--)
		{
			std_index_check(i - 1, needle_len, STD_STR_PATTERN_FILE, 1523, 20);
#if STD_CONTAINS_UB_CHECKS && defined(USE_ASSERT_CHECKING)
			std_slice_get_unchecked_check(self->position + i - 1, haystack_len, STD_STR_PATTERN_FILE, 1523, 52);
#endif
			if (needle[i - 1] != haystack[self->position + i - 1])
			{
				self->position += self->period;
				if (!long_period)
					self->memory = needle_len - self->period;
				mismatched = true;
				break;
			}
		}
		if (mismatched)
			continue;
		*a = self->position;
		self->position += needle_len;
		if (!long_period)
			self->memory = 0;
		*b = *a + needle_len;
		return true;
	}
}

#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)
/* small_slice_eq is pattern.rs's small_slice_eq: two slices of a length compared a byte, or four, at a time. */
static bool
STD_CONTAINS_NAME(small_slice_eq) (const uint8 *x, const uint8 *y, size_t len)
{
	const uint8 *px = x;
	const uint8 *py = y;
	const uint8 *pxend;
	const uint8 *pyend;
	uint32		vx;
	uint32		vy;

	if (len < 4)
	{
		for (size_t i = 0; i < len; i++)
			if (x[i] != y[i])
				return false;
		return true;
	}
	pxend = px + len - 4;
	pyend = py + len - 4;
	while (px < pxend)
	{
		memcpy(&vx, px, 4);
		memcpy(&vy, py, 4);
		if (vx != vy)
			return false;
		px += 4;
		py += 4;
	}
	memcpy(&vx, pxend, 4);
	memcpy(&vy, pyend, 4);
	return vx == vy;
}

/* simd_contains_test_chunk is simd_contains' test_chunk: the bitmask of lanes where both probes match. */
static uint16
STD_CONTAINS_NAME(simd_contains_test_chunk) (const uint8 *haystack, size_t idx, size_t second_probe_offset,
											 uint8 first_probe, uint8 second_probe)
{
#if defined(__x86_64__) || defined(_M_X64)
	__m128i		a = _mm_loadu_si128((const __m128i *) (haystack + idx));
	__m128i		b = _mm_loadu_si128((const __m128i *) (haystack + idx + second_probe_offset));
	__m128i		both = _mm_and_si128(_mm_cmpeq_epi8(a, _mm_set1_epi8((char) first_probe)),
									 _mm_cmpeq_epi8(b, _mm_set1_epi8((char) second_probe)));

	return (uint16) _mm_movemask_epi8(both);
#else
	static const uint8 BITS[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
	uint8x16_t	a = vld1q_u8(haystack + idx);
	uint8x16_t	b = vld1q_u8(haystack + idx + second_probe_offset);
	uint8x16_t	both = vandq_u8(vceqq_u8(a, vdupq_n_u8(first_probe)), vceqq_u8(b, vdupq_n_u8(second_probe)));
	uint8x16_t	lanes = vandq_u8(both, vld1q_u8(BITS));

	return (uint16) (vaddv_u8(vget_low_u8(lanes)) | (vaddv_u8(vget_high_u8(lanes)) << 8));
#endif
}

/*
 * simd_contains_check_mask is simd_contains' check_mask: whether a candidate
 * the mask marks is the needle, compared over the haystack's
 * get_unchecked(offset..).get_unchecked(..trimmed_len).
 */
static bool
STD_CONTAINS_NAME(simd_contains_check_mask) (const uint8 *haystack, size_t haystack_len, const uint8 *trimmed_needle,
											 size_t trimmed_len, size_t idx, uint16 mask, bool skip)
{
	if (skip)
		return false;
	while (mask != 0)
	{
		uint32		trailing = pg_rightmost_one_pos32(mask);
		size_t		offset = idx + trailing + 1;

#if STD_CONTAINS_UB_CHECKS && defined(USE_ASSERT_CHECKING)
		std_slice_get_unchecked_range_check(offset, haystack_len, haystack_len, STD_SLICE_INDEX_FILE, 533, 44);
		std_slice_get_unchecked_range_check(0, trimmed_len, haystack_len - offset, STD_SLICE_INDEX_FILE, 492, 32);
#endif
		if (STD_CONTAINS_NAME(small_slice_eq) (haystack + offset, trimmed_needle, trimmed_len))
			return true;
		mask &= (uint16) ~(1 << trailing);
	}
	return false;
}

/*
 * simd_contains is pattern.rs's simd_contains: whether a needle of 2 to 32
 * bytes is in the haystack, by 16-byte blocks probed at its first byte and a
 * later one that differs, or none when no later byte differs.
 */
static bool
STD_CONTAINS_NAME(simd_contains) (const uint8 *needle, size_t needle_len, const uint8 *haystack,
								  size_t haystack_len, bool *result_out)
{
	const size_t LEN = 16;
	const size_t UNROLL = 4;
	uint8		first_probe;
	size_t		last_byte_offset = needle_len - 1;
	size_t		second_probe_offset;
	uint8		second_probe;
	const uint8 *trimmed_needle;
	size_t		i = 0;
	bool		result = false;
	uint16		mask;

	std_index_check(0, needle_len, STD_STR_PATTERN_FILE, 1826, 23);
	first_probe = needle[0];
	if (needle_len == 2)
		second_probe_offset = 1;
	else
	{
		size_t		idx = needle_len;
		size_t		low = needle_len >= 4 ? needle_len - 4 : 0;
		bool		found = false;

		while (idx > low)
		{
			idx -= 1;
			std_index_check(idx, needle_len, STD_STR_PATTERN_FILE, 1837, 73);
			if (needle[idx] != first_probe)
			{
				found = true;
				break;
			}
		}
		if (!found)
			return false;
		second_probe_offset = idx;
	}
	if (haystack_len < LEN + last_byte_offset)
	{
		const uint8 *v = haystack;
		size_t		v_len = haystack_len;

		if (needle_len == 0)
		{
			static const char text[] = "window size must be non-zero";

			std_option_expect_failed(text, sizeof(text) - 1, STD_STR_PATTERN_FILE, 1848, 30);
		}
		*result_out = false;
		while (needle_len <= v_len)
		{
			std_range_to_check(needle_len, v_len, STD_SLICE_ITER_FILE, 1358, 35);
			std_range_from_check(1, v_len, STD_SLICE_ITER_FILE, 1359, 29);
			if (memcmp(v, needle, needle_len) == 0)
			{
				*result_out = true;
				break;
			}
			v += 1;
			v_len -= 1;
		}
		return true;
	}
	std_index_check(second_probe_offset, needle_len, STD_STR_PATTERN_FILE, 1852, 44);
	second_probe = needle[second_probe_offset];
	std_range_from_check(1, needle_len, STD_STR_PATTERN_FILE, 1855, 33);
	trimmed_needle = needle + 1;
	while (i + last_byte_offset + UNROLL * LEN < haystack_len && !result)
	{
		uint16		masks[4] = {0};

		for (size_t j = 0; j < UNROLL; j++)
		{
			std_index_check(j, lengthof(masks), STD_STR_PATTERN_FILE, 1905, 13);
			masks[j] = STD_CONTAINS_NAME(simd_contains_test_chunk) (haystack, i + j * LEN, second_probe_offset,
																	first_probe, second_probe);
		}
		for (size_t j = 0; j < UNROLL; j++)
		{
			std_index_check(j, lengthof(masks), STD_STR_PATTERN_FILE, 1908, 24);
			mask = masks[j];
			if (mask != 0)
				result |= STD_CONTAINS_NAME(simd_contains_check_mask) (haystack, haystack_len, trimmed_needle,
																	   needle_len - 1, i + j * LEN, mask, result);
		}
		i += UNROLL * LEN;
	}
	while (i + last_byte_offset + LEN < haystack_len && !result)
	{
		mask = STD_CONTAINS_NAME(simd_contains_test_chunk) (haystack, i, second_probe_offset, first_probe,
															second_probe);
		if (mask != 0)
			result |= STD_CONTAINS_NAME(simd_contains_check_mask) (haystack, haystack_len, trimmed_needle,
																   needle_len - 1, i, mask, result);
		i += LEN;
	}
	i = haystack_len - last_byte_offset - LEN;
	mask = STD_CONTAINS_NAME(simd_contains_test_chunk) (haystack, i, second_probe_offset, first_probe, second_probe);
	if (mask != 0)
		result |= STD_CONTAINS_NAME(simd_contains_check_mask) (haystack, haystack_len, trimmed_needle,
															   needle_len - 1, i, mask, result);
	*result_out = result;
	return true;
}
#endif

/*
 * str_next_match is StrSearcher's next_match: an empty needle's first step
 * that matches, through next's Empty arm, or the two-way search's next
 * match, MatchOnly's.
 */
static bool
STD_CONTAINS_NAME(str_next_match) (const uint8 *haystack, size_t haystack_len, const uint8 *needle,
								   size_t needle_len)
{
	size_t		a;
	size_t		b;

	if (needle_len == 0)
	{
		size_t		position = 0;
		bool		is_match_fw = true;

		for (;;)
		{
			bool		is_match = is_match_fw;

			is_match_fw = !is_match_fw;
			std_str_index_range_from((const char *) haystack, haystack_len, position, STD_STR_PATTERN_FILE, 1131,
									 36);
#if STD_CONTAINS_UB_CHECKS && defined(USE_ASSERT_CHECKING)
			std_str_get_unchecked_range_check(position, haystack_len, haystack_len, STD_STR_TRAITS_FILE, 515, 36);
#endif
			if (is_match)
				return true;
			if (position == haystack_len)
				return false;
			{
				StdChars	chars = std_str_chars((const char *) haystack + position, haystack_len - position);
				uint32		ch;

				(void) STD_CONTAINS_CAT(std_str_Chars_next_, STD_CONTAINS_SUFFIX) (&chars, &ch);
				position += std_char_len_utf8(ch);
			}
		}
	}
	{
		STD_CONTAINS_NAME(TwoWaySearcher) searcher =
			STD_CONTAINS_NAME(TwoWaySearcher_new) (needle, needle_len, haystack_len);
		bool		is_long = searcher.memory == SIZE_MAX;

		return STD_CONTAINS_NAME(TwoWaySearcher_next) (&searcher, haystack, haystack_len, needle, needle_len,
													   is_long, false, &a, &b);
	}
}

/*
 * str_is_contained_in is <&str as Pattern>::is_contained_in: an empty
 * needle always, a shorter one by one byte's memchr, simd_contains, or the
 * searcher, and one as long or longer by equality.
 */
static bool
STD_CONTAINS_NAME(str_is_contained_in) (const uint8 *needle, size_t needle_len, const uint8 *haystack,
										size_t haystack_len)
{
	if (needle_len == 0)
		return true;
	if (needle_len < haystack_len)
	{
		if (needle_len == 1)
		{
			std_index_check(0, needle_len, STD_STR_PATTERN_FILE, 997, 58);
			return STD_CONTAINS_NAME(slice_contains_u8) (needle[0], haystack, haystack_len);
		}
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)
		if (needle_len <= 32)
		{
			bool		result;

			if (STD_CONTAINS_NAME(simd_contains) (needle, needle_len, haystack, haystack_len, &result))
				return result;
		}
#endif
		return STD_CONTAINS_NAME(str_next_match) (haystack, haystack_len, needle, needle_len);
	}
	return needle_len == haystack_len && memcmp(needle, haystack, needle_len) == 0;
}

/*
 * contains_char is str::contains of a char, <char as Pattern>::is_contained_in:
 * an ASCII char's byte through memchr, any other's UTF-8 as a needle.
 */
bool
STD_CONTAINS_NAME(contains_char) (const char *haystack, size_t haystack_len, uint32 c)
{
	if (c < 128)
		return STD_CONTAINS_NAME(slice_contains_u8) ((uint8) c, (const uint8 *) haystack, haystack_len);
	else
	{
		uint8		buffer[4] = {0};
		size_t		len = STD_CONTAINS_NAME(encode_utf8) (c, buffer, sizeof(buffer));

		return STD_CONTAINS_NAME(str_is_contained_in) (buffer, len, (const uint8 *) haystack, haystack_len);
	}
}

/* contains_str is str::contains of a &str, <&str as Pattern>::is_contained_in. */
bool
STD_CONTAINS_NAME(contains_str) (const char *haystack, size_t haystack_len, const char *pat, size_t pat_len)
{
	return STD_CONTAINS_NAME(str_is_contained_in) ((const uint8 *) pat, pat_len, (const uint8 *) haystack,
												   haystack_len);
}
#endif							/* STD_CONTAINS_WITH_CONTAINS */

#ifdef STD_CONTAINS_WITH_SPLIT

/*
 * CharSearcher_new is <char as Pattern>::into_searcher: the char's UTF-8,
 * its length's conversion to u8 expected, and fingers at both ends.
 */
static StdCharSearcher
STD_CONTAINS_NAME(CharSearcher_new) (const char *haystack, size_t haystack_len, uint32 c)
{
	StdCharSearcher self = {0};
	size_t		len = STD_CONTAINS_NAME(encode_utf8) (c, self.utf8_encoded, sizeof(self.utf8_encoded));

	if (len > PG_UINT8_MAX)
	{
		static const char text[] = "char len should be less than 255";
		char		error = 0;

		std_result_unwrap_failed(text, sizeof(text) - 1, &error, std_fmt_TryFromIntError_Debug_fmt,
								 STD_STR_PATTERN_FILE, 572, 14);
	}
	self.haystack = (const uint8 *) haystack;
	self.haystack_len = haystack_len;
	self.finger = 0;
	self.finger_back = haystack_len;
	self.utf8_size = (uint8) len;
	return self;
}

/*
 * CharSearcher_next_match is CharSearcher's next_match: memchr of the
 * char's last byte between the fingers, until the bytes before it are the
 * rest of the char, or none.
 */
static bool
STD_CONTAINS_NAME(CharSearcher_next_match) (StdCharSearcher *self, size_t *a, size_t *b)
{
	for (;;)
	{
		uint8		last_byte;
		size_t		index;

		if (!(self->finger <= self->finger_back && self->finger_back <= self->haystack_len))
			return false;
#if STD_CONTAINS_UB_CHECKS && defined(USE_ASSERT_CHECKING)
		std_slice_get_unchecked_check(self->utf8_size - 1, sizeof(self->utf8_encoded), STD_STR_PATTERN_FILE, 440,
									  57);
#endif
		last_byte = self->utf8_encoded[self->utf8_size - 1];
		if (STD_CONTAINS_NAME(memchr) (last_byte, self->haystack + self->finger, self->finger_back - self->finger,
									   &index))
		{
			self->finger += index + 1;
			if (self->finger >= self->utf8_size)
			{
				size_t		found_char = self->finger - self->utf8_size;

				if (found_char <= self->finger && self->finger <= self->haystack_len)
				{
					std_range_check(0, self->utf8_size, sizeof(self->utf8_encoded), STD_STR_PATTERN_FILE, 463, 55);
					if (memcmp(self->haystack + found_char, self->utf8_encoded, self->utf8_size) == 0)
					{
						*a = found_char;
						*b = self->finger;
						return true;
					}
				}
			}
		}
		else
		{
			self->finger = self->finger_back;
			return false;
		}
	}
}

/* split_inclusive_char is str::split_inclusive of a char: its SplitInternal, which keeps no trailing empty piece. */
StdSplitInclusiveChar
STD_CONTAINS_NAME(split_inclusive_char) (const char *haystack, size_t haystack_len, uint32 c)
{
	StdSplitInclusiveChar self;

	self.start = 0;
	self.end = haystack_len;
	self.matcher = STD_CONTAINS_NAME(CharSearcher_new) (haystack, haystack_len, c);
	self.finished = false;
	return self;
}

/*
 * SplitInclusive_next is SplitInclusive's next, SplitInternal's
 * next_inclusive: the piece through the next match, else get_end's last
 * piece when it isn't empty.
 */
bool
STD_CONTAINS_NAME(SplitInclusive_next) (StdSplitInclusiveChar *self, const char **elt, size_t *elt_len)
{
	size_t		a;
	size_t		b;

	if (self->finished)
		return false;
	if (STD_CONTAINS_NAME(CharSearcher_next_match) (&self->matcher, &a, &b))
	{
#if STD_CONTAINS_UB_CHECKS && defined(USE_ASSERT_CHECKING)
		std_str_get_unchecked_range_check(self->start, b, self->matcher.haystack_len, STD_STR_MOD_FILE, 704, 22);
#endif
		*elt = (const char *) self->matcher.haystack + self->start;
		*elt_len = b - self->start;
		self->start = b;
		return true;
	}
	self->finished = true;
	if (self->end - self->start > 0)
	{
#if STD_CONTAINS_UB_CHECKS && defined(USE_ASSERT_CHECKING)
		std_str_get_unchecked_range_check(self->start, self->end, self->matcher.haystack_len, STD_STR_MOD_FILE, 704,
										  22);
#endif
		*elt = (const char *) self->matcher.haystack + self->start;
		*elt_len = self->end - self->start;
		return true;
	}
	return false;
}

/*
 * ends_with_char is str::ends_with of a char: its UTF-8's is_suffix_of,
 * <[u8]>::ends_with of the haystack's tail.
 */
bool
STD_CONTAINS_NAME(ends_with_char) (const char *haystack, size_t haystack_len, uint32 c)
{
	uint8		buffer[4] = {0};
	size_t		n = STD_CONTAINS_NAME(encode_utf8) (c, buffer, sizeof(buffer));

	if (!(haystack_len >= n))
		return false;
	std_range_from_check(haystack_len - n, haystack_len, STD_SLICE_MOD_FILE, 2666, 34);
	return memcmp(buffer, haystack + haystack_len - n, n) == 0;
}
#endif							/* STD_CONTAINS_WITH_SPLIT */

#endif							/* STD_CONTAINS_DEFINE */

#undef STD_CONTAINS_CAT_
#undef STD_CONTAINS_CAT
#undef STD_CONTAINS_NAME
#undef STD_CONTAINS_DECLARE
#undef STD_CONTAINS_DEFINE
#undef STD_CONTAINS_UB_CHECKS
#undef STD_CONTAINS_SUFFIX
#undef STD_CONTAINS_WITH_CONTAINS
#undef STD_CONTAINS_WITH_SPLIT
