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
 * The directory word of a pooled record (design item 14's S2): a slot's
 * generation, its holders, its state, and its two marks in one u64, with
 * every transition a plain function from the word a host loaded to the word
 * it stores by a compare-and-swap, so the server's host and the
 * simulation's make the same transitions. A zeroed word is a free slot at
 * its first generation.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/shared/word.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/test.h"
#include "doltgres/std/vec.h"

#define RUST_FILE "host" STD_SEP "src" STD_SEP "shared" STD_SEP "word.rs"

/* host_shared_word_State_Debug_fmt is State's derived Debug. */
bool
host_shared_word_State_Debug_fmt(const void *value, StdFormatter *f)
{
	static const char *const variants[] = {"Free", "Owned", "Published", "Claimed", "Condemned", "Spent"};
	const char *name = variants[*(const HostSharedWordState *) value];

	return std_fmt_write_str(f, name, strlen(name));
}

/* HOST_SHARED_WORD_STATE masks the word's state. */
#define HOST_SHARED_WORD_STATE UINT64CONST(0x7)

/* HOST_SHARED_WORD_RECLAIMING marks a record a reclaim is looking at. */
#define HOST_SHARED_WORD_RECLAIMING (UINT64CONST(1) << 3)

/* HOST_SHARED_WORD_OPENED marks a record a process opened since the reclaim looking at it took it. */
#define HOST_SHARED_WORD_OPENED (UINT64CONST(1) << 4)

/* HOST_SHARED_WORD_HOLDER is one holder in the word's count of holders. */
#define HOST_SHARED_WORD_HOLDER (UINT64CONST(1) << 8)

/* HOST_SHARED_WORD_GENERATION is the first bit of the word's generation, which is its upper half. */
#define HOST_SHARED_WORD_GENERATION 32

/* HOST_SHARED_WORD_LAST is a slot's last generation, and the mask of a location's generation. */
#define HOST_SHARED_WORD_LAST UINT64CONST(0xFFFFFFFF)

/* The state, the marks, the holders, and the generation follow one another in the word and share no bit. */
StaticAssertDecl(HOST_SHARED_WORD_STATE < HOST_SHARED_WORD_RECLAIMING && HOST_SHARED_WORD_RECLAIMING < HOST_SHARED_WORD_OPENED && HOST_SHARED_WORD_OPENED < HOST_SHARED_WORD_HOLDER,
				 "the word's state and marks are in order");
StaticAssertDecl(HOST_SHARED_WORD_HOLDERS_MAX * HOST_SHARED_WORD_HOLDER < (UINT64CONST(1) << HOST_SHARED_WORD_GENERATION),
				 "the word's holders are below its generation");

/* host_shared_word_state returns a word's state. */
HostSharedWordState
host_shared_word_state(uint64 word)
{
	switch (word & HOST_SHARED_WORD_STATE)
	{
		case 0:
			return HOST_SHARED_WORD_STATE_FREE;
		case 1:
			return HOST_SHARED_WORD_STATE_OWNED;
		case 2:
			return HOST_SHARED_WORD_STATE_PUBLISHED;
		case 3:
			return HOST_SHARED_WORD_STATE_CLAIMED;
		case 4:
			return HOST_SHARED_WORD_STATE_CONDEMNED;
		default:
			return HOST_SHARED_WORD_STATE_SPENT;
	}
}

/* host_shared_word_holders returns a word's count of holders. */
uint64
host_shared_word_holders(uint64 word)
{
	return STD_SHR_FOLDED(u64, word, 8) & HOST_SHARED_WORD_HOLDERS_MAX;
}

/* host_shared_word_generation returns a word's generation. */
uint64
host_shared_word_generation(uint64 word)
{
	return STD_SHR_FOLDED(u64, word, HOST_SHARED_WORD_GENERATION);
}

/*
 * host_shared_word_location returns the location of the record in a slot of
 * the directory with a word: the slot's number, then its generation.
 */
uint64
host_shared_word_location(uint32 slot, uint64 word)
{
	uint64		tmp_1 = STD_SHL_FOLDED(u64, (uint64) slot, HOST_SHARED_WORD_GENERATION);
	uint64		tmp_2 = host_shared_word_generation(word);

	return tmp_1 | tmp_2;
}

/* host_shared_word_slot returns the number of the directory's slot a location names. */
uint64
host_shared_word_slot(uint64 location)
{
	return STD_SHR_FOLDED(u64, location, HOST_SHARED_WORD_GENERATION);
}

/* host_shared_word_names reports whether a location's generation is a word's. */
static bool
host_shared_word_names(uint64 word, uint64 location)
{
	uint64		tmp_1 = host_shared_word_generation(word);

	return tmp_1 == (location & HOST_SHARED_WORD_LAST);
}

/*
 * host_shared_word_live reports whether a location's record still has its
 * slot: the word is at the location's generation, and neither free nor
 * spent. A record freed while processes hold it has its slot until the last
 * of them closes it.
 */
bool
host_shared_word_live(uint64 word, uint64 location)
{
	HostSharedWordState state;

	if (!host_shared_word_names(word, location))
		return false;
	state = host_shared_word_state(word);
	return !(state == HOST_SHARED_WORD_STATE_FREE || state == HOST_SHARED_WORD_STATE_SPENT);
}

/* host_shared_word_published reports whether a word is a published record's. */
bool
host_shared_word_published(uint64 word)
{
	return host_shared_word_state(word) == HOST_SHARED_WORD_STATE_PUBLISHED;
}

/*
 * host_shared_word_swept reports whether a sweep lists a slot, by the words
 * it loaded before and after it read the record's atomic: both are a
 * published record's, at one generation, so no free and no make of the slot
 * came between them. Their holders and marks may differ, as an open or a
 * close of the record between the loads changes them.
 */
bool
host_shared_word_swept(uint64 first, uint64 second)
{
	uint64		tmp_1;
	uint64		tmp_2;

	if (!(host_shared_word_published(first) && host_shared_word_published(second)))
		return false;
	tmp_1 = host_shared_word_generation(first);
	tmp_2 = host_shared_word_generation(second);
	return tmp_1 == tmp_2;
}

/* host_shared_word_stated returns a word with another state. */
static uint64
host_shared_word_stated(uint64 word, HostSharedWordState state)
{
	return (word & ~HOST_SHARED_WORD_STATE) | (uint64) state;
}

/* host_shared_word_released returns the word of a slot whose record is gone: free at the next generation, or spent at the last. */
static uint64
host_shared_word_released(uint64 word)
{
	uint64		generation = host_shared_word_generation(word);

	if (generation == HOST_SHARED_WORD_LAST)
		return ((uint64) HOST_SHARED_WORD_LAST << HOST_SHARED_WORD_GENERATION) |
			(uint64) HOST_SHARED_WORD_STATE_SPENT;
	return STD_SHL_FOLDED(u64, STD_ADD(u64, generation, 1, 127, 23), HOST_SHARED_WORD_GENERATION);
}

/* host_shared_word_make returns the word of a free slot taken by its maker, or nothing for a slot that isn't free. */
bool
host_shared_word_make(uint64 word, uint64 *result)
{
	if (host_shared_word_state(word) == HOST_SHARED_WORD_STATE_FREE)
	{
		*result = host_shared_word_stated(word, HOST_SHARED_WORD_STATE_OWNED);
		return true;
	}
	return false;
}

/* host_shared_word_publish returns the word of a record its owner published. */
uint64
host_shared_word_publish(uint64 word)
{
	return host_shared_word_stated(word, HOST_SHARED_WORD_STATE_PUBLISHED);
}

/*
 * host_shared_word_claim returns the word of a published record a process
 * claimed by its location, whatever its holders, marked as opened, since its
 * owner reaches it as a holder does, or nothing for a location that is stale
 * or a record that isn't published.
 */
bool
host_shared_word_claim(uint64 word, uint64 location, uint64 *result)
{
	if (host_shared_word_state(word) == HOST_SHARED_WORD_STATE_PUBLISHED && host_shared_word_names(word, location))
	{
		*result = host_shared_word_stated(word, HOST_SHARED_WORD_STATE_CLAIMED) | HOST_SHARED_WORD_OPENED;
		return true;
	}
	return false;
}

/*
 * host_shared_word_open returns the word of a published record one more
 * process holds, marked as opened, or nothing for a location that is stale
 * or a record that isn't published.
 */
bool
host_shared_word_open(uint64 word, uint64 location, uint64 *result)
{
	if (host_shared_word_state(word) == HOST_SHARED_WORD_STATE_PUBLISHED && host_shared_word_names(word, location))
	{
		*result = STD_ADD(u64, word, HOST_SHARED_WORD_HOLDER, 151, 72) | HOST_SHARED_WORD_OPENED;
		return true;
	}
	return false;
}

/*
 * host_shared_word_close returns the word of a record one process fewer
 * holds, which frees a condemned record no process holds any more.
 */
uint64
host_shared_word_close(uint64 word)
{
	uint64		left = STD_SUB(u64, word, HOST_SHARED_WORD_HOLDER, 157, 16);

	if (host_shared_word_state(left) == HOST_SHARED_WORD_STATE_CONDEMNED &&
		host_shared_word_holders(left) == 0)
		return host_shared_word_released(left);
	else
		return left;
}

/*
 * host_shared_word_free returns the word of a record its owner freed: gone
 * when no process holds it, and condemned otherwise.
 */
uint64
host_shared_word_free(uint64 word)
{
	if (host_shared_word_holders(word) == 0)
		return host_shared_word_released(word);
	else
		return host_shared_word_stated(word, HOST_SHARED_WORD_STATE_CONDEMNED);
}

/*
 * host_shared_word_give_up returns the word of a record whose owner dropped
 * it: gone when it was never published, and published again when it was
 * claimed.
 */
uint64
host_shared_word_give_up(uint64 word)
{
	if (host_shared_word_state(word) == HOST_SHARED_WORD_STATE_OWNED)
		return host_shared_word_released(word);
	else
		return host_shared_word_stated(word, HOST_SHARED_WORD_STATE_PUBLISHED);
}

/* host_shared_word_Look_Debug_fmt is Look's derived Debug. */
bool
host_shared_word_Look_Debug_fmt(const void *value, StdFormatter *f)
{
	const HostSharedWordLook *self = (const HostSharedWordLook *) value;
	StdDebugTuple b;

	switch (self->tag)
	{
		case HOST_SHARED_WORD_LOOK_TAKE:
			b = std_fmt_debug_tuple(f, "Take", 4);
			std_DebugTuple_field(&b, &self->u.take, std_u64_Debug_fmt);
			return std_DebugTuple_finish(&b);
		case HOST_SHARED_WORD_LOOK_THEIRS:
			return std_fmt_write_str(f, "Theirs", 6);
		case HOST_SHARED_WORD_LOOK_USED:
			return std_fmt_write_str(f, "Used", 4);
	}
	pg_unreachable();
}

/* host_shared_word_Look_PartialEq_eq is Look's derived PartialEq: the variants, then a Take's field. */
bool
host_shared_word_Look_PartialEq_eq(const HostSharedWordLook *self, const HostSharedWordLook *other)
{
	if (self->tag != other->tag)
		return false;
	switch (self->tag)
	{
		case HOST_SHARED_WORD_LOOK_TAKE:
			return self->u.take == other->u.take;
		default:
			return true;
	}
}

/*
 * host_shared_word_reclaim returns what a reclaim's look at a record by its
 * location finds: it takes a published record no process holds and no
 * reclaim is looking at.
 */
HostSharedWordLook
host_shared_word_reclaim(uint64 word, uint64 location)
{
	if (host_shared_word_state(word) != HOST_SHARED_WORD_STATE_PUBLISHED || !host_shared_word_names(word, location))
	{
		return (HostSharedWordLook)
		{
			HOST_SHARED_WORD_LOOK_USED
		};
	}
	else if ((word & HOST_SHARED_WORD_RECLAIMING) != 0)
	{
		return (HostSharedWordLook)
		{
			HOST_SHARED_WORD_LOOK_THEIRS
		};
	}
	else if (host_shared_word_holders(word) != 0)
	{
		return (HostSharedWordLook)
		{
			HOST_SHARED_WORD_LOOK_USED
		};
	}
	else
	{
		return (HostSharedWordLook)
		{
			HOST_SHARED_WORD_LOOK_TAKE,
			{
				(STD_ADD(u64, word, HOST_SHARED_WORD_HOLDER, 193, 21) | HOST_SHARED_WORD_RECLAIMING) & ~HOST_SHARED_WORD_OPENED
			}
		};
	}
}

/*
 * host_shared_word_reclaimed returns the word a reclaim's free stores over
 * exactly the word its take stored, which is there only if no process opened
 * the record since.
 */
uint64
host_shared_word_reclaimed(uint64 taken)
{
	return host_shared_word_released(taken);
}

/*
 * host_shared_word_unreclaim returns the word of a record a reclaim gave up:
 * its holder and its mark taken off, which frees a record claimed and freed
 * meanwhile.
 */
uint64
host_shared_word_unreclaim(uint64 word)
{
	return host_shared_word_close(word & ~HOST_SHARED_WORD_RECLAIMING);
}

#ifdef DOLTGRES_TEST_HOST

/* host_shared_word_tests_taken returns the word a reclaim's look took. */
static uint64
host_shared_word_tests_taken(uint64 word, uint64 location)
{
	HostSharedWordLook look = host_shared_word_reclaim(word, location);
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("the look found "), STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&look, host_shared_word_Look_Debug_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	if (look.tag == HOST_SHARED_WORD_LOOK_TAKE)
		return look.u.take;
	std_panic_fmt(RUST_FILE, 217, 22, &arguments);
}

/*
 * host_shared_word_tests_published returns the word of a record made and
 * published in a slot with a word, and its location in the directory's
 * seventh slot.
 */
static void
host_shared_word_tests_published(uint64 from, uint64 *out_0, uint64 *out_1)
{
	uint64		made;
	bool		has = host_shared_word_make(from, &made);
	uint64		word;

	if (!has)
		STD_OPTION_EXPECT_FAILED(224, 39, "a free slot is made");
	word = host_shared_word_publish(made);

	*out_0 = word;
	*out_1 = host_shared_word_location(7, word);
}

/*
 * test_host_shared_word_tests_the_most_holders_fit_their_field: the most
 * holders fit their field beside every other bit below the generation, and a
 * zeroed word is a free slot at its first generation.
 */
void
test_host_shared_word_tests_the_most_holders_fit_their_field(void)
{
	static const struct
	{
		uint64		bits;
		HostSharedWordState named;
	}			pairs[] = {
		{0, HOST_SHARED_WORD_STATE_FREE}, {1, HOST_SHARED_WORD_STATE_OWNED},
		{2, HOST_SHARED_WORD_STATE_PUBLISHED}, {3, HOST_SHARED_WORD_STATE_CLAIMED},
		{4, HOST_SHARED_WORD_STATE_CONDEMNED}, {5, HOST_SHARED_WORD_STATE_SPENT},
	};
	uint64		full = STD_MUL(u64, HOST_SHARED_WORD_HOLDERS_MAX, HOST_SHARED_WORD_HOLDER, 232, 20) |
		HOST_SHARED_WORD_STATE | HOST_SHARED_WORD_RECLAIMING | HOST_SHARED_WORD_OPENED;

	STD_ASSERT_EQ_U64(233, 9, host_shared_word_holders(full), HOST_SHARED_WORD_HOLDERS_MAX);
	STD_ASSERT_EQ_U64(234, 9, host_shared_word_generation(full), 0);
	{
		HostSharedWordState left_0 = host_shared_word_state(0);
		uint64		left_1 = host_shared_word_holders(0);
		uint64		left_2 = host_shared_word_generation(0);
		HostSharedWordState right_0 = HOST_SHARED_WORD_STATE_FREE;
		uint64		right_1 = 0;
		uint64		right_2 = 0;

		STD_ASSERT_EQ(235, 9, left_0 == right_0 && left_1 == right_1 && left_2 == right_2,
					  &STD_TUPLE(3, STD_ITEM(&left_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&left_1, std_u64_Debug_fmt), STD_ITEM(&left_2, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(3, STD_ITEM(&right_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&right_1, std_u64_Debug_fmt), STD_ITEM(&right_2, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	for (size_t start = 0, i; std_array_IntoIter_next(&start, lengthof(pairs), lengthof(pairs), &i);)
	{
		{
			HostSharedWordState left_ = host_shared_word_state(pairs[i].bits);
			HostSharedWordState right_ = pairs[i].named;

			STD_ASSERT_EQ(244, 13, left_ == right_, &left_, host_shared_word_State_Debug_fmt, &right_,
						  host_shared_word_State_Debug_fmt);
		}
		STD_ASSERT_EQ_U64(245, 13, (uint64) pairs[i].named, pairs[i].bits);
	}
}

/*
 * test_host_shared_word_tests_a_location_names_a_slot_and_a_generation: a
 * location is its slot's number and its record's generation, and only a free
 * slot is made.
 */
void
test_host_shared_word_tests_a_location_names_a_slot_and_a_generation(void)
{
	uint64		word;
	uint64		location;
	uint64		v;
	bool		has;

	host_shared_word_tests_published(((uint64) 5 << HOST_SHARED_WORD_GENERATION), &word, &location);
	{
		uint64		a = host_shared_word_slot(location);
		uint64		b = location & HOST_SHARED_WORD_LAST;
		uint64		ea = 7;
		uint64		eb = 5;

		STD_ASSERT_EQ(253, 9, a == ea && b == eb,
					  &STD_TUPLE(2, STD_ITEM(&a, std_u64_Debug_fmt), STD_ITEM(&b, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&ea, std_u64_Debug_fmt), STD_ITEM(&eb, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	{
		HostSharedWordState left_ = host_shared_word_state(word);
		HostSharedWordState right_ = HOST_SHARED_WORD_STATE_PUBLISHED;

		STD_ASSERT_EQ(254, 9, left_ == right_, &left_, host_shared_word_State_Debug_fmt, &right_,
					  host_shared_word_State_Debug_fmt);
	}
	has = host_shared_word_make(word, &v);
	{
		StdOptionRef left_ = (StdOptionRef) {has, STD_ITEM(&v, std_u64_Debug_fmt)};
		StdOptionRef right_ = (StdOptionRef) {false, STD_ITEM(NULL, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(255, 9, !has, &left_, std_option_Debug_fmt, &right_, std_option_Debug_fmt);
	}
	has = host_shared_word_make(host_shared_word_stated(word, HOST_SHARED_WORD_STATE_SPENT), &v);
	{
		StdOptionRef left_ = (StdOptionRef) {has, STD_ITEM(&v, std_u64_Debug_fmt)};
		StdOptionRef right_ = (StdOptionRef) {false, STD_ITEM(NULL, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(256, 9, !has, &left_, std_option_Debug_fmt, &right_, std_option_Debug_fmt);
	}
}

/*
 * test_host_shared_word_tests_a_freed_record_is_gone_for_its_location: a free
 * with no holder frees the slot at the next generation, where the old location
 * opens nothing and claims nothing.
 */
void
test_host_shared_word_tests_a_freed_record_is_gone_for_its_location(void)
{
	uint64		word;
	uint64		location;
	uint64		v;
	bool		has;
	uint64		claimed;
	uint64		freed;
	uint64		again;
	uint64		unused;

	host_shared_word_tests_published(0, &word, &location);
	has = host_shared_word_claim(word, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(264, 45, "a published record is claimed");
	claimed = v;
	has = host_shared_word_claim(claimed, location, &v);
	{
		StdOptionRef left_ = (StdOptionRef) {has, STD_ITEM(&v, std_u64_Debug_fmt)};
		StdOptionRef right_ = (StdOptionRef) {false, STD_ITEM(NULL, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(265, 9, !has, &left_, std_option_Debug_fmt, &right_, std_option_Debug_fmt);
	}
	has = host_shared_word_open(claimed, location, &v);
	{
		StdOptionRef left_ = (StdOptionRef) {has, STD_ITEM(&v, std_u64_Debug_fmt)};
		StdOptionRef right_ = (StdOptionRef) {false, STD_ITEM(NULL, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(266, 9, !has, &left_, std_option_Debug_fmt, &right_, std_option_Debug_fmt);
	}
	freed = host_shared_word_free(claimed);
	{
		HostSharedWordState left_0 = host_shared_word_state(freed);
		uint64		left_1 = host_shared_word_generation(freed);
		uint64		left_2 = host_shared_word_holders(freed);
		HostSharedWordState right_0 = HOST_SHARED_WORD_STATE_FREE;
		uint64		right_1 = 1;
		uint64		right_2 = 0;

		STD_ASSERT_EQ(268, 9, left_0 == right_0 && left_1 == right_1 && left_2 == right_2,
					  &STD_TUPLE(3, STD_ITEM(&left_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&left_1, std_u64_Debug_fmt), STD_ITEM(&left_2, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(3, STD_ITEM(&right_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&right_1, std_u64_Debug_fmt), STD_ITEM(&right_2, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	host_shared_word_tests_published(freed, &again, &unused);
	has = host_shared_word_open(again, location, &v);
	{
		StdOptionRef left_ = (StdOptionRef) {has, STD_ITEM(&v, std_u64_Debug_fmt)};
		StdOptionRef right_ = (StdOptionRef) {false, STD_ITEM(NULL, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(270, 9, !has, &left_, std_option_Debug_fmt, &right_, std_option_Debug_fmt);
	}
	has = host_shared_word_claim(again, location, &v);
	{
		StdOptionRef left_ = (StdOptionRef) {has, STD_ITEM(&v, std_u64_Debug_fmt)};
		StdOptionRef right_ = (StdOptionRef) {false, STD_ITEM(NULL, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(271, 9, !has, &left_, std_option_Debug_fmt, &right_, std_option_Debug_fmt);
	}
	{
		HostSharedWordLook left_ = host_shared_word_reclaim(again, location);
		HostSharedWordLook right_ = (HostSharedWordLook) {HOST_SHARED_WORD_LOOK_USED};

		STD_ASSERT_EQ(272, 9, host_shared_word_Look_PartialEq_eq(&left_, &right_), &left_, host_shared_word_Look_Debug_fmt,
					  &right_, host_shared_word_Look_Debug_fmt);
	}
}

/*
 * test_host_shared_word_tests_the_last_close_frees_a_condemned_record: a
 * record freed while held is condemned, takes no open, and is gone at its last
 * close.
 */
void
test_host_shared_word_tests_the_last_close_frees_a_condemned_record(void)
{
	uint64		word;
	uint64		location;
	uint64		v;
	bool		has;
	uint64		held;
	uint64		condemned;
	uint64		one;
	uint64		gone;

	host_shared_word_tests_published(0, &word, &location);
	has = host_shared_word_open(word, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(279, 46, "opens");
	has = host_shared_word_open(v, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(279, 73, "opens");
	held = v;
	STD_ASSERT_EQ_U64(280, 9, host_shared_word_holders(held), 2);
	has = host_shared_word_claim(held, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(281, 52, "claims");
	condemned = host_shared_word_free(v);
	{
		HostSharedWordState left_0 = host_shared_word_state(condemned);
		uint64		left_1 = host_shared_word_holders(condemned);
		HostSharedWordState right_0 = HOST_SHARED_WORD_STATE_CONDEMNED;
		uint64		right_1 = 2;

		STD_ASSERT_EQ(282, 9, left_0 == right_0 && left_1 == right_1,
					  &STD_TUPLE(2, STD_ITEM(&left_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&left_1, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&right_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&right_1, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	has = host_shared_word_open(condemned, location, &v);
	{
		StdOptionRef left_ = (StdOptionRef) {has, STD_ITEM(&v, std_u64_Debug_fmt)};
		StdOptionRef right_ = (StdOptionRef) {false, STD_ITEM(NULL, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(283, 9, !has, &left_, std_option_Debug_fmt, &right_, std_option_Debug_fmt);
	}
	one = host_shared_word_close(condemned);
	{
		HostSharedWordState left_0 = host_shared_word_state(one);
		uint64		left_1 = host_shared_word_holders(one);
		HostSharedWordState right_0 = HOST_SHARED_WORD_STATE_CONDEMNED;
		uint64		right_1 = 1;

		STD_ASSERT_EQ(285, 9, left_0 == right_0 && left_1 == right_1,
					  &STD_TUPLE(2, STD_ITEM(&left_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&left_1, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&right_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&right_1, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	gone = host_shared_word_close(one);
	{
		HostSharedWordState left_0 = host_shared_word_state(gone);
		uint64		left_1 = host_shared_word_generation(gone);
		uint64		left_2 = host_shared_word_holders(gone);
		HostSharedWordState right_0 = HOST_SHARED_WORD_STATE_FREE;
		uint64		right_1 = 1;
		uint64		right_2 = 0;

		STD_ASSERT_EQ(287, 9, left_0 == right_0 && left_1 == right_1 && left_2 == right_2,
					  &STD_TUPLE(3, STD_ITEM(&left_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&left_1, std_u64_Debug_fmt), STD_ITEM(&left_2, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(3, STD_ITEM(&right_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&right_1, std_u64_Debug_fmt), STD_ITEM(&right_2, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
}

/*
 * test_host_shared_word_tests_a_dropped_owner_gives_its_record_up: a dropped
 * owner frees a record never published and publishes a claimed one again, with
 * its holders.
 */
void
test_host_shared_word_tests_a_dropped_owner_gives_its_record_up(void)
{
	uint64		v;
	bool		has;
	uint64		owned;
	uint64		word;
	uint64		location;
	uint64		claimed;
	uint64		back;

	has = host_shared_word_make(((uint64) 3 << HOST_SHARED_WORD_GENERATION), &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(293, 43, "makes");
	owned = v;
	{
		HostSharedWordState left_0 = host_shared_word_state(host_shared_word_give_up(owned));
		uint64		left_1 = host_shared_word_generation(host_shared_word_give_up(owned));
		HostSharedWordState right_0 = HOST_SHARED_WORD_STATE_FREE;
		uint64		right_1 = 4;

		STD_ASSERT_EQ(294, 9, left_0 == right_0 && left_1 == right_1,
					  &STD_TUPLE(2, STD_ITEM(&left_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&left_1, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&right_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&right_1, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	host_shared_word_tests_published(0, &word, &location);
	has = host_shared_word_open(word, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(296, 50, "opens");
	has = host_shared_word_claim(v, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(296, 77, "claims");
	claimed = v;
	back = host_shared_word_give_up(claimed);
	{
		HostSharedWordState left_0 = host_shared_word_state(back);
		uint64		left_1 = host_shared_word_holders(back);
		uint64		left_2 = host_shared_word_generation(back);
		HostSharedWordState right_0 = HOST_SHARED_WORD_STATE_PUBLISHED;
		uint64		right_1 = 1;
		uint64		right_2 = 0;

		STD_ASSERT_EQ(298, 9, left_0 == right_0 && left_1 == right_1 && left_2 == right_2,
					  &STD_TUPLE(3, STD_ITEM(&left_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&left_1, std_u64_Debug_fmt), STD_ITEM(&left_2, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(3, STD_ITEM(&right_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&right_1, std_u64_Debug_fmt), STD_ITEM(&right_2, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	STD_ASSERT(299, 9, host_shared_word_open(back, location, &v), "open(back, location).is_some()");
}

/*
 * test_host_shared_word_tests_a_reclaim_frees_only_a_record_not_opened_since:
 * a reclaim takes only a published record no process holds, and frees it only
 * if no process opened it since, closed again or not.
 */
void
test_host_shared_word_tests_a_reclaim_frees_only_a_record_not_opened_since(void)
{
	uint64		word;
	uint64		location;
	uint64		v;
	bool		has;
	uint64		mine;
	uint64		opened;
	uint64		closed;
	uint64		given;

	host_shared_word_tests_published(0, &word, &location);
	mine = host_shared_word_tests_taken(word, location);
	STD_ASSERT_EQ_U64(308, 9, host_shared_word_holders(mine), 1);
	{
		HostSharedWordLook left_ = host_shared_word_reclaim(mine, location);
		HostSharedWordLook right_ = (HostSharedWordLook) {HOST_SHARED_WORD_LOOK_THEIRS};

		STD_ASSERT_EQ(309, 9, host_shared_word_Look_PartialEq_eq(&left_, &right_), &left_, host_shared_word_Look_Debug_fmt,
					  &right_, host_shared_word_Look_Debug_fmt);
	}
	{
		HostSharedWordState left_ = host_shared_word_state(host_shared_word_reclaimed(mine));
		HostSharedWordState right_ = HOST_SHARED_WORD_STATE_FREE;

		STD_ASSERT_EQ(310, 9, left_ == right_, &left_, host_shared_word_State_Debug_fmt, &right_,
					  host_shared_word_State_Debug_fmt);
	}
	has = host_shared_word_open(mine, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(311, 43, "a record a reclaim looks at opens");
	opened = v;
	closed = host_shared_word_close(opened);
	if (closed == mine)
		std_assert_failed(RUST_FILE, 313, 9, "!=", &closed, std_u64_Debug_fmt, &mine, std_u64_Debug_fmt, NULL);
	STD_ASSERT_EQ_U64(314, 9, host_shared_word_holders(closed), 1);
	given = host_shared_word_unreclaim(closed);
	{
		HostSharedWordState left_0 = host_shared_word_state(given);
		uint64		left_1 = host_shared_word_holders(given);
		HostSharedWordState right_0 = HOST_SHARED_WORD_STATE_PUBLISHED;
		uint64		right_1 = 0;

		STD_ASSERT_EQ(316, 9, left_0 == right_0 && left_1 == right_1,
					  &STD_TUPLE(2, STD_ITEM(&left_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&left_1, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&right_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&right_1, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	STD_ASSERT_EQ_U64(317, 9, host_shared_word_tests_taken(given, location), mine);
	has = host_shared_word_open(word, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(318, 49, "opens");
	{
		HostSharedWordLook left_ = host_shared_word_reclaim(v, location);
		HostSharedWordLook right_ = (HostSharedWordLook) {HOST_SHARED_WORD_LOOK_USED};

		STD_ASSERT_EQ(318, 9, host_shared_word_Look_PartialEq_eq(&left_, &right_), &left_, host_shared_word_Look_Debug_fmt,
					  &right_, host_shared_word_Look_Debug_fmt);
	}
	has = host_shared_word_claim(word, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(319, 50, "claims");
	{
		HostSharedWordLook left_ = host_shared_word_reclaim(v, location);
		HostSharedWordLook right_ = (HostSharedWordLook) {HOST_SHARED_WORD_LOOK_USED};

		STD_ASSERT_EQ(319, 9, host_shared_word_Look_PartialEq_eq(&left_, &right_), &left_, host_shared_word_Look_Debug_fmt,
					  &right_, host_shared_word_Look_Debug_fmt);
	}
}

/*
 * test_host_shared_word_tests_a_reclaim_does_not_free_a_record_claimed_since:
 * a record claimed while a reclaim looks at it, and published again by its
 * dropped owner, isn't freed by the reclaim: the claim marked it opened, as
 * its owner may have changed it.
 */
void
test_host_shared_word_tests_a_reclaim_does_not_free_a_record_claimed_since(void)
{
	uint64		word;
	uint64		location;
	uint64		v;
	bool		has;
	uint64		mine;
	uint64		back;
	uint64		given;

	host_shared_word_tests_published(0, &word, &location);
	mine = host_shared_word_tests_taken(word, location);
	has = host_shared_word_claim(mine, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(328, 50, "a record a reclaim looks at is claimed");
	back = host_shared_word_give_up(v);
	{
		HostSharedWordState left_0 = host_shared_word_state(back);
		uint64		left_1 = host_shared_word_holders(back);
		HostSharedWordState right_0 = HOST_SHARED_WORD_STATE_PUBLISHED;
		uint64		right_1 = 1;

		STD_ASSERT_EQ(329, 9, left_0 == right_0 && left_1 == right_1,
					  &STD_TUPLE(2, STD_ITEM(&left_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&left_1, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&right_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&right_1, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	if (back == mine)
		std_assert_failed(RUST_FILE, 330, 9, "!=", &back, std_u64_Debug_fmt, &mine, std_u64_Debug_fmt, NULL);
	given = host_shared_word_unreclaim(back);
	{
		HostSharedWordState left_0 = host_shared_word_state(given);
		uint64		left_1 = host_shared_word_holders(given);
		HostSharedWordState right_0 = HOST_SHARED_WORD_STATE_PUBLISHED;
		uint64		right_1 = 0;

		STD_ASSERT_EQ(332, 9, left_0 == right_0 && left_1 == right_1,
					  &STD_TUPLE(2, STD_ITEM(&left_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&left_1, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(2, STD_ITEM(&right_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&right_1, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
}

/*
 * test_host_shared_word_tests_a_reclaim_given_up_frees_a_condemned_record: a
 * reclaim given up on a record claimed and freed meanwhile frees the condemned
 * slot.
 */
void
test_host_shared_word_tests_a_reclaim_given_up_frees_a_condemned_record(void)
{
	uint64		word;
	uint64		location;
	uint64		v;
	bool		has;
	uint64		mine;
	uint64		condemned;
	uint64		gone;

	host_shared_word_tests_published(0, &word, &location);
	mine = host_shared_word_tests_taken(word, location);
	has = host_shared_word_claim(mine, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(340, 52, "claims");
	condemned = host_shared_word_free(v);
	{
		HostSharedWordState left_ = host_shared_word_state(condemned);
		HostSharedWordState right_ = HOST_SHARED_WORD_STATE_CONDEMNED;

		STD_ASSERT_EQ(341, 9, left_ == right_, &left_, host_shared_word_State_Debug_fmt, &right_,
					  host_shared_word_State_Debug_fmt);
	}
	if (condemned == mine)
		std_assert_failed(RUST_FILE, 342, 9, "!=", &condemned, std_u64_Debug_fmt, &mine, std_u64_Debug_fmt, NULL);
	gone = host_shared_word_unreclaim(condemned);
	{
		HostSharedWordState left_0 = host_shared_word_state(gone);
		uint64		left_1 = host_shared_word_generation(gone);
		uint64		left_2 = host_shared_word_holders(gone);
		HostSharedWordState right_0 = HOST_SHARED_WORD_STATE_FREE;
		uint64		right_1 = 1;
		uint64		right_2 = 0;

		STD_ASSERT_EQ(344, 9, left_0 == right_0 && left_1 == right_1 && left_2 == right_2,
					  &STD_TUPLE(3, STD_ITEM(&left_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&left_1, std_u64_Debug_fmt), STD_ITEM(&left_2, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(3, STD_ITEM(&right_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&right_1, std_u64_Debug_fmt), STD_ITEM(&right_2, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
}

/*
 * test_host_shared_word_tests_a_slot_freed_at_its_last_generation_is_spent: a
 * slot freed at its last generation is spent: it is never made again, and no
 * location reaches it.
 */
void
test_host_shared_word_tests_a_slot_freed_at_its_last_generation_is_spent(void)
{
	uint64		word;
	uint64		location;
	uint64		v;
	bool		has;
	uint64		spent;

	host_shared_word_tests_published(((uint64) HOST_SHARED_WORD_LAST << HOST_SHARED_WORD_GENERATION), &word, &location);
	STD_ASSERT_EQ_U64(351, 9, location & HOST_SHARED_WORD_LAST, HOST_SHARED_WORD_LAST);
	has = host_shared_word_claim(word, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(352, 48, "claims");
	spent = host_shared_word_free(v);
	{
		HostSharedWordState left_0 = host_shared_word_state(spent);
		uint64		left_1 = host_shared_word_generation(spent);
		uint64		left_2 = host_shared_word_holders(spent);
		HostSharedWordState right_0 = HOST_SHARED_WORD_STATE_SPENT;
		uint64		right_1 = HOST_SHARED_WORD_LAST;
		uint64		right_2 = 0;

		STD_ASSERT_EQ(353, 9, left_0 == right_0 && left_1 == right_1 && left_2 == right_2,
					  &STD_TUPLE(3, STD_ITEM(&left_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&left_1, std_u64_Debug_fmt), STD_ITEM(&left_2, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt,
					  &STD_TUPLE(3, STD_ITEM(&right_0, host_shared_word_State_Debug_fmt),
								 STD_ITEM(&right_1, std_u64_Debug_fmt), STD_ITEM(&right_2, std_u64_Debug_fmt)),
					  std_tuple_Debug_fmt);
	}
	has = host_shared_word_make(spent, &v);
	{
		StdOptionRef left_ = (StdOptionRef) {has, STD_ITEM(&v, std_u64_Debug_fmt)};
		StdOptionRef right_ = (StdOptionRef) {false, STD_ITEM(NULL, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(354, 9, !has, &left_, std_option_Debug_fmt, &right_, std_option_Debug_fmt);
	}
	has = host_shared_word_open(spent, location, &v);
	{
		StdOptionRef left_ = (StdOptionRef) {has, STD_ITEM(&v, std_u64_Debug_fmt)};
		StdOptionRef right_ = (StdOptionRef) {false, STD_ITEM(NULL, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(355, 9, !has, &left_, std_option_Debug_fmt, &right_, std_option_Debug_fmt);
	}
	has = host_shared_word_claim(spent, location, &v);
	{
		StdOptionRef left_ = (StdOptionRef) {has, STD_ITEM(&v, std_u64_Debug_fmt)};
		StdOptionRef right_ = (StdOptionRef) {false, STD_ITEM(NULL, std_u64_Debug_fmt)};

		STD_ASSERT_EQ(356, 9, !has, &left_, std_option_Debug_fmt, &right_, std_option_Debug_fmt);
	}
	{
		HostSharedWordLook left_ = host_shared_word_reclaim(spent, location);
		HostSharedWordLook right_ = (HostSharedWordLook) {HOST_SHARED_WORD_LOOK_USED};

		STD_ASSERT_EQ(357, 9, host_shared_word_Look_PartialEq_eq(&left_, &right_), &left_, host_shared_word_Look_Debug_fmt,
					  &right_, host_shared_word_Look_Debug_fmt);
	}
}

/*
 * test_host_shared_word_tests_a_sweep_lists_a_record_published_at_both_loads:
 * a sweep lists a record published at both of its loads, whatever opened,
 * closed, or looked at it between them, and not one that was freed, made
 * again, claimed, or not yet published at either.
 */
void
test_host_shared_word_tests_a_sweep_lists_a_record_published_at_both_loads(void)
{
	uint64		word;
	uint64		location;
	uint64		v;
	bool		has;
	uint64		opened;
	uint64		claimed;
	uint64		freed;
	uint64		again;
	uint64		unused;
	uint64		owned;

	host_shared_word_tests_published(0, &word, &location);
	has = host_shared_word_open(word, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(365, 43, "opens");
	opened = v;
	STD_ASSERT(366, 9, host_shared_word_swept(word, word) && host_shared_word_swept(word, opened) &&
			   host_shared_word_swept(opened, host_shared_word_close(opened)),
			   "swept(word, word) && swept(word, opened) && swept(opened, close(opened))");
	STD_ASSERT(367, 9, host_shared_word_swept(word, host_shared_word_tests_taken(word, location)),
			   "swept(word, taken(word, location))");
	has = host_shared_word_claim(word, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(368, 45, "claims");
	claimed = v;
	STD_ASSERT(369, 9, !host_shared_word_swept(word, claimed) && !host_shared_word_swept(claimed, word),
			   "!swept(word, claimed) && !swept(claimed, word)");
	STD_ASSERT(370, 9, host_shared_word_swept(word, host_shared_word_give_up(claimed)),
			   "swept(word, give_up(claimed))");
	freed = host_shared_word_free(claimed);
	STD_ASSERT(372, 9, !host_shared_word_swept(word, freed) && !host_shared_word_swept(freed, freed),
			   "!swept(word, freed) && !swept(freed, freed)");
	host_shared_word_tests_published(freed, &again, &unused);
	STD_ASSERT(374, 9, !host_shared_word_swept(word, again) && host_shared_word_swept(again, again),
			   "!swept(word, again) && swept(again, again)");
	has = host_shared_word_make(0, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(375, 29, "makes");
	owned = v;
	STD_ASSERT(376, 9, !host_shared_word_swept(owned, host_shared_word_publish(owned)) &&
			   !host_shared_word_swept(host_shared_word_publish(owned), owned),
			   "!swept(owned, publish(owned)) && !swept(publish(owned), owned)");
}

/*
 * test_host_shared_word_tests_a_record_is_live_until_its_slot_is_given_back: a
 * record has its slot from its make until it is freed with no holder or its
 * last holder closes it, and not once the slot is free, made again, or spent.
 */
void
test_host_shared_word_tests_a_record_is_live_until_its_slot_is_given_back(void)
{
	uint64		v;
	bool		has;
	uint64		owned;
	uint64		location;
	uint64		held;
	uint64		condemned;
	uint64		gone;
	uint64		last;
	uint64		at_last;

	has = host_shared_word_make(0, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(383, 29, "makes");
	owned = v;
	location = host_shared_word_location(7, owned);
	STD_ASSERT(385, 9, !host_shared_word_live(0, location), "!live(0, location)");
	STD_ASSERT(386, 9, host_shared_word_live(owned, location), "live(owned, location)");
	has = host_shared_word_open(host_shared_word_publish(owned), location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(387, 51, "opens");
	held = v;
	has = host_shared_word_claim(held, location, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(388, 52, "claims");
	condemned = host_shared_word_free(v);
	STD_ASSERT(389, 9, host_shared_word_live(held, location) && host_shared_word_live(condemned, location),
			   "live(held, location) && live(condemned, location)");
	gone = host_shared_word_close(condemned);
	STD_ASSERT(391, 9, !host_shared_word_live(gone, location), "!live(gone, location)");
	has = host_shared_word_make(gone, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(392, 34, "makes");
	STD_ASSERT(392, 9, !host_shared_word_live(v, location),
			   "!live(make(gone).expect(\"makes\"), location)");
	host_shared_word_tests_published(((uint64) HOST_SHARED_WORD_LAST << HOST_SHARED_WORD_GENERATION), &last, &at_last);
	STD_ASSERT(394, 9, host_shared_word_live(last, at_last), "live(last, at_last)");
	has = host_shared_word_claim(last, at_last, &v);
	if (!has)
		STD_OPTION_EXPECT_FAILED(395, 49, "claims");
	STD_ASSERT(395, 9, !host_shared_word_live(host_shared_word_free(v), at_last),
			   "!live(free(claim(last, at_last).expect(\"claims\")), at_last)");
}

#endif							/* DOLTGRES_TEST_HOST */
