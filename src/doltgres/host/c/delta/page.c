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
 * The directory word of a page of the delta (design item 14's S6): a slot's
 * generation and its state in one u64, a page's name, and the checks of an
 * open, a read, an append, and the budget, each a plain function from what a
 * host loaded to what it stores or answers, so the server's host and the
 * simulation's decide alike. A zeroed word is a free slot at its first
 * generation.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/delta/page.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"

#define RUST_FILE "host" STD_SEP "src" STD_SEP "delta" STD_SEP "page.rs"

/* host_delta_page_State_Debug_fmt is State's derived Debug. */
bool
host_delta_page_State_Debug_fmt(const void *value, StdFormatter *f)
{
	static const char *const names[] = {"Free", "Owned", "Live", "Retired", "Freeing", "Spent"};
	const char *name = names[*(const HostDeltaPageState *) value];

	return std_fmt_write_str(f, name, strlen(name));
}

/* HOST_DELTA_PAGE_STATE masks the word's state. */
#define HOST_DELTA_PAGE_STATE UINT64CONST(0x7)

/* HOST_DELTA_PAGE_GENERATION is the first bit of the word's generation, which is its upper 33 bits. */
#define HOST_DELTA_PAGE_GENERATION 31

/* HOST_DELTA_PAGE_LAST is a slot's last generation, and the mask of a name's generation. */
#define HOST_DELTA_PAGE_LAST ((UINT64CONST(1) << 33) - 1)

/* HOST_DELTA_PAGE_NUMBER is the first bit of a name's slot number, above its generation. */
#define HOST_DELTA_PAGE_NUMBER 33

/*
 * The most slots any delta_memory gives, at its largest value of INT_MAX kB,
 * are numbered by a name, so an aged kind's records, as many as the slots,
 * are numbered by a location built as a name is.
 */
StaticAssertDecl((uint64) PG_INT32_MAX * 1024 / HOST_DELTA_PAGE_SLOT_BYTES < HOST_DELTA_PAGE_SLOTS,
				 "every slot is numbered by a name");

/* The state and the generation share no bit, and a name's number and generation fill it. */
StaticAssertDecl(HOST_DELTA_PAGE_STATE < (UINT64CONST(1) << HOST_DELTA_PAGE_GENERATION) && HOST_DELTA_PAGE_LAST == (PG_UINT64_MAX >> HOST_DELTA_PAGE_GENERATION) &&
				 HOST_DELTA_PAGE_SLOTS == (UINT64CONST(1) << (64 - HOST_DELTA_PAGE_NUMBER)),
				 "a directory word's parts");

/* host_delta_page_state returns a word's state. */
HostDeltaPageState
host_delta_page_state(uint64 word)
{
	switch (word & HOST_DELTA_PAGE_STATE)
	{
		case 0:
			return HOST_DELTA_PAGE_STATE_FREE;
		case 1:
			return HOST_DELTA_PAGE_STATE_OWNED;
		case 2:
			return HOST_DELTA_PAGE_STATE_LIVE;
		case 3:
			return HOST_DELTA_PAGE_STATE_RETIRED;
		case 4:
			return HOST_DELTA_PAGE_STATE_FREEING;
		default:
			return HOST_DELTA_PAGE_STATE_SPENT;
	}
}

/* host_delta_page_generation returns a word's generation. */
uint64
host_delta_page_generation(uint64 word)
{
	return STD_SHR_FOLDED(u64, word, HOST_DELTA_PAGE_GENERATION);
}

/* host_delta_page_stated returns a word with another state. */
static uint64
host_delta_page_stated(uint64 word, HostDeltaPageState state)
{
	return (word & ~HOST_DELTA_PAGE_STATE) | (uint64) state;
}

/* host_delta_page_slots returns the number of slots the directory of a delta of a size has. */
uint64
host_delta_page_slots(uint64 delta_memory)
{
	return STD_DIV_FOLDED(u64, delta_memory, HOST_DELTA_PAGE_SLOT_BYTES);
}

/*
 * host_delta_page_directory_slots returns the page directory's slots: one
 * for each 1 KiB page the region's blocks can hold, the block past the
 * budget's included, so a full directory still holds the copies the
 * compactions past the budget put there (bugs rows 110 and 116).
 */
uint64
host_delta_page_directory_slots(uint64 delta_memory)
{
	return STD_MUL(u64, host_delta_page_blocks(delta_memory), (HOST_DELTA_PAGE_BLOCK / HOST_DELTA_PAGE_SLOT_BYTES), 100, 5);
}

/*
 * host_delta_page_blocks returns the blocks of the delta's region: the
 * budget's whole blocks and one more, which only the one compaction past the
 * budget may take for its copy (bugs rows 110 and 116).
 */
uint64
host_delta_page_blocks(uint64 delta_memory)
{
	return STD_ADD(u64, STD_DIV_FOLDED(u64, delta_memory, HOST_DELTA_PAGE_BLOCK), 1, 106, 5);
}

/* host_delta_page_Page_Debug_fmt is Page's derived Debug. */
bool
host_delta_page_Page_Debug_fmt(const void *value, StdFormatter *f)
{
	const HostDeltaPagePage *self = (const HostDeltaPagePage *) value;
	StdDebugTuple b = std_fmt_debug_tuple(f, "Page", 4);

	std_DebugTuple_field(&b, &self->f0, std_u64_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/* host_delta_page_Page_of returns the name of the page in a slot with a word. */
HostDeltaPagePage
host_delta_page_Page_of(uint32 number, uint64 word)
{
	uint64		tmp_1 = STD_SHL_FOLDED(u64, (uint64) number, HOST_DELTA_PAGE_NUMBER);
	uint64		tmp_2 = host_delta_page_generation(word);

	return (HostDeltaPagePage)
	{
		tmp_1 | tmp_2
	};
}

/* host_delta_page_Page_named returns the name a word holds. */
HostDeltaPagePage
host_delta_page_Page_named(uint64 value)
{
	return (HostDeltaPagePage)
	{
		value
	};
}

/* host_delta_page_Page_value returns the name as a word stores it. */
uint64
host_delta_page_Page_value(HostDeltaPagePage self)
{
	return self.f0;
}

/* host_delta_page_Page_number returns the number of the slot the name names. */
uint64
host_delta_page_Page_number(HostDeltaPagePage self)
{
	return STD_SHR_FOLDED(u64, self.f0, HOST_DELTA_PAGE_NUMBER);
}

/* host_delta_page_Page_generation returns the generation the name names its slot at. */
uint64
host_delta_page_Page_generation(HostDeltaPagePage self)
{
	return self.f0 & HOST_DELTA_PAGE_LAST;
}

/* host_delta_page_Page_names reports whether the name's generation is a word's. */
bool
host_delta_page_Page_names(HostDeltaPagePage self, uint64 word)
{
	uint64		tmp_1 = host_delta_page_generation(word);
	uint64		tmp_2 = host_delta_page_Page_generation(self);

	return tmp_1 == tmp_2;
}

/* host_delta_page_claim returns the word of a free slot its allocator took, or nothing for a slot that isn't free. */
bool
host_delta_page_claim(uint64 word, uint64 *result)
{
	if (host_delta_page_state(word) == HOST_DELTA_PAGE_STATE_FREE)
	{
		*result = host_delta_page_stated(word, HOST_DELTA_PAGE_STATE_OWNED);
		return true;
	}
	return false;
}

/* host_delta_page_link returns the word of an owned page its owner linked. */
uint64
host_delta_page_link(uint64 word)
{
	return host_delta_page_stated(word, HOST_DELTA_PAGE_STATE_LIVE);
}

/*
 * host_delta_page_retire returns the word of a live page a name names once it
 * is retired, or nothing for a page that isn't live or that the name doesn't
 * name.
 */
bool
host_delta_page_retire(uint64 word, HostDeltaPagePage name, uint64 *result)
{
	if (host_delta_page_state(word) == HOST_DELTA_PAGE_STATE_LIVE && host_delta_page_Page_names(name, word))
	{
		*result = host_delta_page_stated(word, HOST_DELTA_PAGE_STATE_RETIRED);
		return true;
	}
	return false;
}

/* host_delta_page_passed reports whether every published epoch has passed a retired page's stamp, given the oldest one. */
bool
host_delta_page_passed(uint64 stamp, uint64 oldest)
{
	return stamp < oldest;
}

/* host_delta_page_reap returns the word of a retired page a free pass claimed, or nothing for a slot that isn't retired. */
bool
host_delta_page_reap(uint64 word, uint64 *result)
{
	if (host_delta_page_state(word) == HOST_DELTA_PAGE_STATE_RETIRED)
	{
		*result = host_delta_page_stated(word, HOST_DELTA_PAGE_STATE_FREEING);
		return true;
	}
	return false;
}

/* host_delta_page_released returns the word of a slot whose page is gone: free at the next generation, or spent at the last. */
uint64
host_delta_page_released(uint64 word)
{
	uint64		generation = host_delta_page_generation(word);

	if (generation == HOST_DELTA_PAGE_LAST)
		return ((uint64) HOST_DELTA_PAGE_LAST << HOST_DELTA_PAGE_GENERATION) |
			(uint64) HOST_DELTA_PAGE_STATE_SPENT;
	return STD_SHL_FOLDED(u64, STD_ADD(u64, generation, 1, 176, 23), HOST_DELTA_PAGE_GENERATION);
}

/* host_delta_page_Phase_Debug_fmt is Phase's derived Debug. */
bool
host_delta_page_Phase_Debug_fmt(const void *value, StdFormatter *f)
{
	static const char *const names[] = {"Current", "Retired", "Neither"};
	const char *name = names[*(const HostDeltaPagePhase *) value];

	return std_fmt_write_str(f, name, strlen(name));
}

/* host_delta_page_Seen_Debug_fmt is Seen's derived Debug. */
bool
host_delta_page_Seen_Debug_fmt(const void *value, StdFormatter *f)
{
	const HostDeltaPageSeen *self = (const HostDeltaPageSeen *) value;
	StdDebugStruct b = std_fmt_debug_struct(f, "Seen", 4);

	std_DebugStruct_field(&b, "generation", 10, &self->generation, std_u64_Debug_fmt);
	std_DebugStruct_field(&b, "phase", 5, &self->phase, host_delta_page_Phase_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* host_delta_page_seen returns what a load of a page's word shows an open. */
HostDeltaPageSeen
host_delta_page_seen(uint64 word)
{
	HostDeltaPagePhase phase;

	switch (host_delta_page_state(word))
	{
		case HOST_DELTA_PAGE_STATE_LIVE:
			phase = HOST_DELTA_PAGE_PHASE_CURRENT;
			break;
		case HOST_DELTA_PAGE_STATE_RETIRED:
			phase = HOST_DELTA_PAGE_PHASE_RETIRED;
			break;
		default:
			phase = HOST_DELTA_PAGE_PHASE_NEITHER;
			break;
	}
	return (HostDeltaPageSeen)
	{
		host_delta_page_generation(word), phase
	};
}

/*
 * host_delta_page_admits reports whether an open under a hold reaches a slot,
 * by what it loaded: the slot's word, then its stamp and its other words,
 * then its word again. The generation is the name's at both loads, so every
 * word between belongs to it; the slot is current or retired at both; and a
 * slot found retired at the first load has a stamp no older than the hold's
 * epoch, since a hold begun after a retirement must not reach it. A slot
 * retired between the loads opens, since its retirement came after the hold
 * began.
 */
bool
host_delta_page_admits(HostDeltaPageSeen before, uint64 stamp, HostDeltaPageSeen after,
					   uint64 generation, uint64 epoch)
{
	if (!(before.generation == generation && after.generation == generation &&
		  after.phase != HOST_DELTA_PAGE_PHASE_NEITHER))
		return false;
	switch (before.phase)
	{
		case HOST_DELTA_PAGE_PHASE_CURRENT:
			return true;
		case HOST_DELTA_PAGE_PHASE_RETIRED:
			return stamp >= epoch;
		case HOST_DELTA_PAGE_PHASE_NEITHER:
			return false;
	}
	pg_unreachable();
}

/* host_delta_page_Size_Debug_fmt is Size's derived Debug. */
bool
host_delta_page_Size_Debug_fmt(const void *value, StdFormatter *f)
{
	const HostDeltaPageSize *self = (const HostDeltaPageSize *) value;
	StdDebugStruct b = std_fmt_debug_struct(f, "Size", 4);

	std_DebugStruct_field(&b, "bytes", 5, &self->bytes, std_u32_Debug_fmt);
	std_DebugStruct_field(&b, "words", 5, &self->words, std_u32_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* host_delta_page_Size_body returns the bytes the page's body takes and the budget counts: its bytes, then its words. */
uint64
host_delta_page_Size_body(HostDeltaPageSize self)
{
	uint64		tmp_1 = STD_MUL(u64, (uint64) self.words, (uint64) sizeof(uint64), 238, 33);

	return STD_ADD(u64, (uint64) self.bytes, tmp_1, 238, 9);
}

/*
 * host_delta_page_Size_allocates reports whether an empty area of a delta
 * could give a page of this size: it has a body, no larger than the largest
 * page or the delta itself.
 */
bool
host_delta_page_Size_allocates(HostDeltaPageSize self, uint64 delta_memory)
{
	uint64		most = std_min_u64(HOST_DELTA_PAGE_LARGEST, delta_memory);
	uint64		body = host_delta_page_Size_body(self);

	return body >= 1 && body <= most;
}

/* host_delta_page_lends reports whether a read of a length at an offset lies below a page's filled mark. */
bool
host_delta_page_lends(uint64 mark, uint64 offset, uint64 len)
{
	uint64		end;

	return std_checked_add_u64(offset, len, &end) && end <= mark;
}

/*
 * host_delta_page_appended returns a page's filled mark once a record of a
 * length is appended above it, or nothing for a record that doesn't fit the
 * page's capacity.
 */
bool
host_delta_page_appended(uint64 mark, uint64 len, uint64 capacity, uint64 *result)
{
	uint64		filled;

	if (!std_checked_add_u64(mark, len, &filled))
		return false;
	if (!(filled <= capacity))
		return false;
	*result = filled;
	return true;
}

#ifdef DOLTGRES_TEST_HOST

/*
 * host_delta_page_tests_linked returns the word of a page claimed and linked
 * in a slot with a word, and its name in the directory's seventh slot.
 */
static void
host_delta_page_tests_linked(uint64 from, uint64 *out_0, HostDeltaPagePage *out_1)
{
	uint64		claimed;
	uint64		word;

	if (!host_delta_page_claim(from, &claimed))
		STD_OPTION_EXPECT_FAILED(266, 37, "a free slot is claimed");
	word = host_delta_page_link(claimed);
	*out_0 = word;
	*out_1 = host_delta_page_Page_of(7, word);
}

/*
 * test_host_delta_page_tests_a_word_holds_a_state_and_a_generation: a zeroed
 * word is a free slot at its first generation, each state has its bits, and
 * the state and the generation don't overlap.
 */
void
test_host_delta_page_tests_a_word_holds_a_state_and_a_generation(void)
{
	static const struct
	{
		uint64		bits;
		HostDeltaPageState named;
	}			pairs[] = {
		{0, HOST_DELTA_PAGE_STATE_FREE}, {1, HOST_DELTA_PAGE_STATE_OWNED}, {2, HOST_DELTA_PAGE_STATE_LIVE},
		{3, HOST_DELTA_PAGE_STATE_RETIRED}, {4, HOST_DELTA_PAGE_STATE_FREEING}, {5, HOST_DELTA_PAGE_STATE_SPENT},
	};
	uint64		full;

	{
		HostDeltaPageState s = host_delta_page_state(0);
		uint64		g = host_delta_page_generation(0);
		HostDeltaPageState es = HOST_DELTA_PAGE_STATE_FREE;
		uint64		eg = 0;

		STD_ASSERT_EQ(274, 9, s == es && g == eg,
					  &STD_TUPLE(2, STD_ITEM(&s, host_delta_page_State_Debug_fmt), STD_ITEM(&g, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&es, host_delta_page_State_Debug_fmt), STD_ITEM(&eg, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	for (size_t start = 0, i; std_array_IntoIter_next(&start, lengthof(pairs), lengthof(pairs), &i);)
	{
		HostDeltaPageState s = host_delta_page_state(pairs[i].bits);
		HostDeltaPageState named = pairs[i].named;

		STD_ASSERT_EQ(283, 13, s == named, &s, host_delta_page_State_Debug_fmt, &named,
					  host_delta_page_State_Debug_fmt);
		STD_ASSERT_EQ_U64(284, 13, (uint64) named, pairs[i].bits);
	}
	full = ((uint64) HOST_DELTA_PAGE_LAST << HOST_DELTA_PAGE_GENERATION) | HOST_DELTA_PAGE_STATE;
	{
		uint64		a = host_delta_page_generation(full);
		uint64		b = host_delta_page_generation(HOST_DELTA_PAGE_STATE);
		uint64		ea = HOST_DELTA_PAGE_LAST;
		uint64		eb = 0;

		STD_ASSERT_EQ(287, 9, a == ea && b == eb,
					  &STD_TUPLE(2, STD_ITEM(&a, std_u64_Debug_fmt), STD_ITEM(&b, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&ea, std_u64_Debug_fmt), STD_ITEM(&eb, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
}

/*
 * test_host_delta_page_tests_a_name_is_a_number_and_a_generation: a name is
 * its slot's number in its upper 31 bits and the slot's generation in its
 * lower 33, and the largest delta's slots are all numbered, its last slot at
 * its last generation round-tripping (bugs row 150).
 */
void
test_host_delta_page_tests_a_name_is_a_number_and_a_generation(void)
{
	uint64		word;
	HostDeltaPagePage name;
	HostDeltaPagePage last;
	HostDeltaPagePage largest;

	host_delta_page_tests_linked(((uint64) 5 << HOST_DELTA_PAGE_GENERATION), &word, &name);
	{
		uint64		a = host_delta_page_Page_number(name);
		uint64		b = host_delta_page_Page_value(name) & HOST_DELTA_PAGE_LAST;
		uint64		ea = 7;
		uint64		eb = 5;

		STD_ASSERT_EQ(295, 9, a == ea && b == eb,
					  &STD_TUPLE(2, STD_ITEM(&a, std_u64_Debug_fmt), STD_ITEM(&b, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&ea, std_u64_Debug_fmt), STD_ITEM(&eb, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	STD_ASSERT(296, 9, host_delta_page_Page_names(name, word), "name.names(word)");
	{
		HostDeltaPagePage named = host_delta_page_Page_named(host_delta_page_Page_value(name));

		STD_ASSERT_EQ(297, 9, named.f0 == name.f0, &named, host_delta_page_Page_Debug_fmt, &name,
					  host_delta_page_Page_Debug_fmt);
	}
	{
		uint32		number = (uint32) ((uint64) HOST_DELTA_PAGE_SLOTS - 1);
		uint64		at = ((uint64) HOST_DELTA_PAGE_LAST << HOST_DELTA_PAGE_GENERATION);

		last = host_delta_page_Page_of(number, at);
	}
	{
		uint64		a = host_delta_page_Page_number(last);
		uint64		b = host_delta_page_Page_value(last);
		uint64		ea = ((uint64) HOST_DELTA_PAGE_SLOTS - 1);
		uint64		eb = PG_UINT64_MAX;

		STD_ASSERT_EQ(299, 9, a == ea && b == eb,
					  &STD_TUPLE(2, STD_ITEM(&a, std_u64_Debug_fmt), STD_ITEM(&b, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&ea, std_u64_Debug_fmt), STD_ITEM(&eb, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	STD_ASSERT(300, 9, host_delta_page_slots(STD_MUL(u64, (uint64) PG_INT32_MAX, 1024, 300, 23)) <= HOST_DELTA_PAGE_SLOTS,
			   "slots(i32::MAX as u64 * 1024) <= SLOTS");
	{
		uint64		tmp_1 = host_delta_page_slots(STD_MUL(u64, (uint64) PG_INT32_MAX, 1024, 301, 39));
		uint32		number = (uint32) STD_SUB(u64, tmp_1, 1, 301, 32);
		uint64		at = ((uint64) HOST_DELTA_PAGE_LAST << HOST_DELTA_PAGE_GENERATION);

		largest = host_delta_page_Page_of(number, at);
	}
	{
		uint64		a = host_delta_page_Page_number(largest);
		uint64		b = host_delta_page_Page_generation(largest);
		uint64		tmp_1 = host_delta_page_slots(STD_MUL(u64, (uint64) PG_INT32_MAX, 1024, 302, 69));
		uint64		ea = STD_SUB(u64, tmp_1, 1, 302, 63);
		uint64		eb = HOST_DELTA_PAGE_LAST;

		STD_ASSERT_EQ(302, 9, a == ea && b == eb,
					  &STD_TUPLE(2, STD_ITEM(&a, std_u64_Debug_fmt), STD_ITEM(&b, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&ea, std_u64_Debug_fmt), STD_ITEM(&eb, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	STD_ASSERT_EQ_U64(303, 9, host_delta_page_slots(((uint64) 32 << 20)), (uint64) 32 << 10);
}

/*
 * test_host_delta_page_tests_each_transition_takes_only_its_own_state: only a
 * free slot is claimed, a retire takes a live page by its own name alone, and
 * a pass claims only a retired page.
 */
void
test_host_delta_page_tests_each_transition_takes_only_its_own_state(void)
{
	uint64		v;
	bool		has;
	uint64		owned;
	uint64		live;
	HostDeltaPagePage name;
	HostDeltaPagePage other;
	uint64		retired;
	uint64		freeing;
	StdOptionRef none = {false, STD_ITEM(NULL, std_u64_Debug_fmt)};

	has = host_delta_page_claim(((uint64) 3 << HOST_DELTA_PAGE_GENERATION), &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(310, 44, "a free slot is claimed");
	owned = v;
	{
		HostDeltaPageState s = host_delta_page_state(owned);
		uint64		g = host_delta_page_generation(owned);
		HostDeltaPageState es = HOST_DELTA_PAGE_STATE_OWNED;
		uint64		eg = 3;

		STD_ASSERT_EQ(311, 9, s == es && g == eg,
					  &STD_TUPLE(2, STD_ITEM(&s, host_delta_page_State_Debug_fmt), STD_ITEM(&g, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&es, host_delta_page_State_Debug_fmt), STD_ITEM(&eg, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	has = host_delta_page_claim(owned, &v);
	{
		StdOptionRef left_ = {has, STD_ITEM(&v, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(312, 9, !has, &left_, std_option_Debug_fmt, &none, std_option_Debug_fmt);
	}
	host_delta_page_tests_linked(((uint64) 3 << HOST_DELTA_PAGE_GENERATION), &live, &name);
	{
		uint64		a,
					b,
					c;
		bool		ha = host_delta_page_claim(live, &a);
		bool		hb = host_delta_page_reap(live, &b);
		bool		hc = host_delta_page_retire(owned, name, &c);
		StdOptionRef oa = {ha, STD_ITEM(&a, std_u64_Debug_fmt)};
		StdOptionRef ob = {hb, STD_ITEM(&b, std_u64_Debug_fmt)};
		StdOptionRef oc = {hc, STD_ITEM(&c, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(314, 9, !ha && !hb && !hc,
					  &STD_TUPLE(3, STD_ITEM(&oa, std_option_Debug_fmt), STD_ITEM(&ob, std_option_Debug_fmt),
								 STD_ITEM(&oc, std_option_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(3, STD_ITEM(&none, std_option_Debug_fmt), STD_ITEM(&none, std_option_Debug_fmt),
								 STD_ITEM(&none, std_option_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	other = host_delta_page_Page_of(7, ((uint64) 4 << HOST_DELTA_PAGE_GENERATION));
	has = host_delta_page_retire(live, other, &v);
	{
		StdOptionRef left_ = {has, STD_ITEM(&v, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(316, 9, !has, &left_, std_option_Debug_fmt, &none, std_option_Debug_fmt);
	}
	has = host_delta_page_retire(live, name, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(317, 42, "a live page is retired by its name");
	retired = v;
	{
		HostDeltaPageState s = host_delta_page_state(retired);
		HostDeltaPageState es = HOST_DELTA_PAGE_STATE_RETIRED;

		STD_ASSERT_EQ(318, 9, s == es, &s, host_delta_page_State_Debug_fmt, &es, host_delta_page_State_Debug_fmt);
	}
	has = host_delta_page_retire(retired, name, &v);
	{
		StdOptionRef left_ = {has, STD_ITEM(&v, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(319, 9, !has, &left_, std_option_Debug_fmt, &none, std_option_Debug_fmt);
	}
	has = host_delta_page_reap(retired, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(320, 37, "a retired page is claimed by a pass");
	freeing = v;
	{
		HostDeltaPageState s = host_delta_page_state(freeing);
		uint64		b,
					c;
		bool		hb = host_delta_page_reap(freeing, &b);
		bool		hc = host_delta_page_claim(freeing, &c);
		StdOptionRef ob = {hb, STD_ITEM(&b, std_u64_Debug_fmt)};
		StdOptionRef oc = {hc, STD_ITEM(&c, std_u64_Debug_fmt)};
		HostDeltaPageState es = HOST_DELTA_PAGE_STATE_FREEING;

		STD_ASSERT_EQ(321, 9, s == es && !hb && !hc,
					  &STD_TUPLE(3, STD_ITEM(&s, host_delta_page_State_Debug_fmt), STD_ITEM(&ob, std_option_Debug_fmt),
								 STD_ITEM(&oc, std_option_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(3, STD_ITEM(&es, host_delta_page_State_Debug_fmt), STD_ITEM(&none, std_option_Debug_fmt),
								 STD_ITEM(&none, std_option_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
}

/*
 * test_host_delta_page_tests_a_released_slot_moves_on_and_its_last_generation_is_spent:
 * a released slot is free at the next generation, where its old name names
 * nothing, and a slot released at its last generation is spent: never claimed,
 * and named by no name's retire.
 */
void
test_host_delta_page_tests_a_released_slot_moves_on_and_its_last_generation_is_spent(void)
{
	uint64		v;
	bool		has;
	uint64		live;
	HostDeltaPagePage name;
	uint64		free;
	uint64		last;
	HostDeltaPagePage named;
	uint64		spent;
	StdOptionRef none = {false, STD_ITEM(NULL, std_u64_Debug_fmt)};

	host_delta_page_tests_linked(((uint64) 3 << HOST_DELTA_PAGE_GENERATION), &live, &name);
	has = host_delta_page_retire(live, name, &v);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(329, 53);
	has = host_delta_page_reap(v, &v);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(329, 63);
	free = host_delta_page_released(v);
	{
		HostDeltaPageState s = host_delta_page_state(free);
		uint64		g = host_delta_page_generation(free);
		HostDeltaPageState es = HOST_DELTA_PAGE_STATE_FREE;
		uint64		eg = 4;

		STD_ASSERT_EQ(330, 9, s == es && g == eg,
					  &STD_TUPLE(2, STD_ITEM(&s, host_delta_page_State_Debug_fmt), STD_ITEM(&g, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&es, host_delta_page_State_Debug_fmt), STD_ITEM(&eg, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	STD_ASSERT(331, 9, !host_delta_page_Page_names(name, free), "!name.names(free)");
	has = host_delta_page_claim(free, &v);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(332, 44);
	has = host_delta_page_retire(host_delta_page_link(v), name, &v);
	{
		StdOptionRef left_ = {has, STD_ITEM(&v, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(332, 9, !has, &left_, std_option_Debug_fmt, &none, std_option_Debug_fmt);
	}
	host_delta_page_tests_linked(((uint64) HOST_DELTA_PAGE_LAST << HOST_DELTA_PAGE_GENERATION), &last, &named);
	STD_ASSERT_EQ_U64(334, 9, host_delta_page_Page_value(named) & HOST_DELTA_PAGE_LAST, HOST_DELTA_PAGE_LAST);
	spent = host_delta_page_released(last);
	{
		HostDeltaPageState s = host_delta_page_state(spent);
		uint64		g = host_delta_page_generation(spent);
		HostDeltaPageState es = HOST_DELTA_PAGE_STATE_SPENT;
		uint64		eg = HOST_DELTA_PAGE_LAST;

		STD_ASSERT_EQ(336, 9, s == es && g == eg,
					  &STD_TUPLE(2, STD_ITEM(&s, host_delta_page_State_Debug_fmt), STD_ITEM(&g, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&es, host_delta_page_State_Debug_fmt), STD_ITEM(&eg, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	{
		uint64		a,
					b,
					c;
		bool		ha = host_delta_page_claim(spent, &a);
		bool		hb = host_delta_page_retire(spent, named, &b);
		bool		hc = host_delta_page_reap(spent, &c);
		StdOptionRef oa = {ha, STD_ITEM(&a, std_u64_Debug_fmt)};
		StdOptionRef ob = {hb, STD_ITEM(&b, std_u64_Debug_fmt)};
		StdOptionRef oc = {hc, STD_ITEM(&c, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(337, 9, !ha && !hb && !hc,
					  &STD_TUPLE(3, STD_ITEM(&oa, std_option_Debug_fmt), STD_ITEM(&ob, std_option_Debug_fmt),
								 STD_ITEM(&oc, std_option_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(3, STD_ITEM(&none, std_option_Debug_fmt), STD_ITEM(&none, std_option_Debug_fmt),
								 STD_ITEM(&none, std_option_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	{
		HostDeltaPagePhase p = host_delta_page_seen(spent).phase;
		HostDeltaPagePhase ep = HOST_DELTA_PAGE_PHASE_NEITHER;

		STD_ASSERT_EQ(338, 9, p == ep, &p, host_delta_page_Phase_Debug_fmt, &ep, host_delta_page_Phase_Debug_fmt);
	}
	has = host_delta_page_claim(0, &v);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(339, 38);
	STD_ASSERT_EQ_U64(339, 9, host_delta_page_released(v), ((uint64) 1 << HOST_DELTA_PAGE_GENERATION));
}

/*
 * test_host_delta_page_tests_a_stamp_is_passed_by_a_later_oldest_epoch: a pass
 * frees a page only once every published epoch has passed its stamp.
 */
void
test_host_delta_page_tests_a_stamp_is_passed_by_a_later_oldest_epoch(void)
{
	STD_ASSERT(345, 9, host_delta_page_passed(4, 5), "passed(4, 5)");
	STD_ASSERT(346, 9, !host_delta_page_passed(5, 5), "!passed(5, 5)");
	STD_ASSERT(347, 9, !host_delta_page_passed(6, 5), "!passed(6, 5)");
}

/*
 * test_host_delta_page_tests_an_open_reaches_a_page_a_hold_may_reach: an open
 * reaches a page that is live or retired at its name's generation at both
 * loads, a page retired before the first load only under a hold no younger
 * than its stamp, and a page retired between the loads whatever stamp was
 * loaded.
 */
void
test_host_delta_page_tests_an_open_reaches_a_page_a_hold_may_reach(void)
{
	uint64		live;
	HostDeltaPagePage name;
	uint64		v;
	bool		has;
	uint64		retired;
	HostDeltaPageSeen at;
	HostDeltaPageSeen was;

	host_delta_page_tests_linked(((uint64) 3 << HOST_DELTA_PAGE_GENERATION), &live, &name);
	has = host_delta_page_retire(live, name, &v);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(356, 42);
	retired = v;
	at = host_delta_page_seen(live);
	was = host_delta_page_seen(retired);
	STD_ASSERT(358, 9, host_delta_page_admits(at, 0, at, 3, 9), "admits(at, 0, at, 3, 9)");
	STD_ASSERT(359, 9, host_delta_page_admits(at, 0, was, 3, 9), "admits(at, 0, was, 3, 9)");
	STD_ASSERT(360, 9, host_delta_page_admits(was, 9, was, 3, 9), "admits(was, 9, was, 3, 9)");
	STD_ASSERT(361, 9, host_delta_page_admits(was, 12, was, 3, 9), "admits(was, 12, was, 3, 9)");
	STD_ASSERT(362, 9, !host_delta_page_admits(was, 8, was, 3, 9), "!admits(was, 8, was, 3, 9)");
	STD_ASSERT(363, 9, !host_delta_page_admits(at, 0, at, 4, 9), "!admits(at, 0, at, 4, 9)");
}

/*
 * test_host_delta_page_tests_an_open_refuses_a_slot_remade_between_its_loads:
 * an open refuses a slot whose generation moved between its two loads, with
 * the loads scripted: the page freed and the slot claimed and linked again
 * between them reads as live at both, at two generations.
 */
void
test_host_delta_page_tests_an_open_refuses_a_slot_remade_between_its_loads(void)
{
	uint64		live;
	HostDeltaPagePage name;
	uint64		v;
	bool		has;
	uint64		freeing;
	uint64		again;
	HostDeltaPagePage unused;

	host_delta_page_tests_linked(((uint64) 3 << HOST_DELTA_PAGE_GENERATION), &live, &name);
	has = host_delta_page_retire(live, name, &v);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(371, 47);
	has = host_delta_page_reap(v, &v);
	if (!has)
		STD_OPTION_UNWRAP_FAILED(371, 57);
	freeing = v;
	host_delta_page_tests_linked(host_delta_page_released(freeing), &again, &unused);
	{
		HostDeltaPagePhase a = host_delta_page_seen(live).phase;
		HostDeltaPagePhase b = host_delta_page_seen(again).phase;
		HostDeltaPagePhase e = HOST_DELTA_PAGE_PHASE_CURRENT;

		STD_ASSERT_EQ(373, 9, a == e && b == e,
					  &STD_TUPLE(2, STD_ITEM(&a, host_delta_page_Phase_Debug_fmt), STD_ITEM(&b, host_delta_page_Phase_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&e, host_delta_page_Phase_Debug_fmt), STD_ITEM(&e, host_delta_page_Phase_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	{
		HostDeltaPageSeen first = host_delta_page_seen(live);
		HostDeltaPageSeen second = host_delta_page_seen(again);

		STD_ASSERT(374, 9, !host_delta_page_admits(first, 0, second, 3, 0), "!admits(seen(live), 0, seen(again), 3, 0)");
	}
	{
		HostDeltaPageSeen first = host_delta_page_seen(live);
		HostDeltaPageSeen second = host_delta_page_seen(freeing);

		STD_ASSERT(375, 9, !host_delta_page_admits(first, 0, second, 3, 0), "!admits(seen(live), 0, seen(freeing), 3, 0)");
	}
	{
		HostDeltaPageSeen first = host_delta_page_seen(freeing);
		HostDeltaPageSeen second = host_delta_page_seen(freeing);

		STD_ASSERT(376, 9, !host_delta_page_admits(first, 0, second, 3, 0),
				   "!admits(seen(freeing), 0, seen(freeing), 3, 0)");
	}
	{
		HostDeltaPageSeen first;
		HostDeltaPageSeen second;

		has = host_delta_page_claim(((uint64) 3 << HOST_DELTA_PAGE_GENERATION), &v);
		if (!has)
			STD_OPTION_UNWRAP_FAILED(377, 53);
		first = host_delta_page_seen(v);
		second = host_delta_page_seen(live);
		STD_ASSERT(377, 9, !host_delta_page_admits(first, 0, second, 3, 0),
				   "!admits(seen(claim(3 << GENERATION).unwrap()), 0, seen(live), 3, 0)");
	}
}

/*
 * test_host_delta_page_tests_a_size_is_a_body_no_larger_than_a_block: a
 * page's body is its bytes and its words, a size with no body or past a block
 * or the delta is one no empty region gives, and the region holds the
 * budget's whole blocks and one more (bugs row 116).
 */
void
test_host_delta_page_tests_a_size_is_a_body_no_larger_than_a_block(void)
{
	HostDeltaPageSize size = {((uint32) 8 << 10), 16};

	STD_ASSERT_EQ_U64(385, 9, host_delta_page_Size_body(size), ((uint64) ((uint64) 8 << 10) + 128));
	STD_ASSERT(386, 9, host_delta_page_Size_allocates(size, ((uint64) 1 << 20)), "size.allocates(1 << 20)");
	{
		HostDeltaPageSize empty = {0, 0};

		STD_ASSERT(387, 9, !host_delta_page_Size_allocates(empty, ((uint64) 1 << 20)),
				   "!Size { bytes: 0, words: 0 }.allocates(1 << 20)");
	}
	{
		HostDeltaPageSize words = {0, 512};

		STD_ASSERT(388, 9, host_delta_page_Size_allocates(words, ((uint64) 1 << 20)),
				   "Size { bytes: 0, words: 512 }.allocates(1 << 20)");
	}
	{
		HostDeltaPageSize most = {((uint32) 64 << 10), 0};

		STD_ASSERT(389, 9, host_delta_page_Size_allocates(most, ((uint64) 1 << 20)),
				   "Size { bytes: 64 << 10, words: 0 }.allocates(1 << 20)");
	}
	{
		HostDeltaPageSize past = {((uint32) 64 << 10), 1};

		STD_ASSERT(390, 9, !host_delta_page_Size_allocates(past, ((uint64) 1 << 20)),
				   "!Size { bytes: 64 << 10, words: 1 }.allocates(1 << 20)");
	}
	{
		HostDeltaPageSize over = {((uint32) 8 << 10), 0};

		STD_ASSERT(391, 9, !host_delta_page_Size_allocates(over, ((uint64) 4 << 10)),
				   "!Size { bytes: 8 << 10, words: 0 }.allocates(4 << 10)");
	}
	{
		uint64		left_0 = host_delta_page_blocks((uint64) 1 << 20);
		uint64		left_1 = host_delta_page_blocks(((uint64) 1 << 20) + ((uint64) 32 << 10));
		uint64		right_0 = 17;
		uint64		right_1 = 17;
		StdTupleRef left_ = {2, {STD_ITEM(&left_0, std_u64_Debug_fmt), STD_ITEM(&left_1, std_u64_Debug_fmt)}};
		StdTupleRef right_ = {2, {STD_ITEM(&right_0, std_u64_Debug_fmt), STD_ITEM(&right_1, std_u64_Debug_fmt)}};

		STD_ASSERT_EQ(392, 9, left_0 == right_0 && left_1 == right_1, &left_, std_tuple_Debug_fmt, &right_,
					  std_tuple_Debug_fmt);
	}
}

/*
 * test_host_delta_page_tests_reads_end_at_the_mark_and_appends_at_the_capacity:
 * a read lies below the filled mark, and an append fits the capacity above it.
 */
void
test_host_delta_page_tests_reads_end_at_the_mark_and_appends_at_the_capacity(void)
{
	uint64		v;
	bool		has;
	StdOptionRef none = {false, STD_ITEM(NULL, std_u64_Debug_fmt)};

	STD_ASSERT(398, 9, host_delta_page_lends(10, 0, 10), "lends(10, 0, 10)");
	STD_ASSERT(399, 9, host_delta_page_lends(10, 10, 0), "lends(10, 10, 0)");
	STD_ASSERT(400, 9, !host_delta_page_lends(10, 4, 7), "!lends(10, 4, 7)");
	STD_ASSERT(401, 9, !host_delta_page_lends(10, PG_UINT64_MAX, 2), "!lends(10, u64::MAX, 2)");
	has = host_delta_page_appended(10, 6, 16, &v);
	{
		uint64		e = 16;
		StdOptionRef left_ = {has, STD_ITEM(&v, std_u64_Debug_fmt)};
		StdOptionRef right_ = {true, STD_ITEM(&e, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(402, 9, has && v == e, &left_, std_option_Debug_fmt, &right_, std_option_Debug_fmt);
	}
	has = host_delta_page_appended(10, 7, 16, &v);
	{
		StdOptionRef left_ = {has, STD_ITEM(&v, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(403, 9, !has, &left_, std_option_Debug_fmt, &none, std_option_Debug_fmt);
	}
	has = host_delta_page_appended(PG_UINT64_MAX, 1, PG_UINT64_MAX, &v);
	{
		StdOptionRef left_ = {has, STD_ITEM(&v, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(404, 9, !has, &left_, std_option_Debug_fmt, &none, std_option_Debug_fmt);
	}
}

#endif							/* DOLTGRES_TEST_HOST */
