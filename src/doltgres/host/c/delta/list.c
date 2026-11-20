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
 * A record's lists (design item 14's S6): nine words of the record, its
 * first six entries, its root page's name, its depth and reach, and its
 * length, and past the first six a tree of 4 KiB pages of words, each entry
 * found by its index alone. A list is loaded under its record's lock and
 * stored under it exclusively, and a closed list, which takes no store, is
 * read with no lock by its record's retirement.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/delta/list.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#define RUST_FILE "host" STD_SEP "src" STD_SEP "delta" STD_SEP "list.rs"

/*
 * HOST_DELTA_LIST_ROOT, HOST_DELTA_LIST_DEPTH, and HOST_DELTA_LIST_LENGTH are
 * the places among a list's words of its root's name, its depth and reach,
 * and its length.
 */
#define HOST_DELTA_LIST_ROOT UINT64CONST(6)
#define HOST_DELTA_LIST_DEPTH UINT64CONST(7)
#define HOST_DELTA_LIST_LENGTH UINT64CONST(8)

/* HOST_DELTA_LIST_CLOSED marks a closed list in its length word, whose other 63 bits are its length. */
#define HOST_DELTA_LIST_CLOSED (UINT64CONST(1) << 63)

/*
 * HOST_DELTA_LIST_REACH is the first bit of a list's reach in its depth word,
 * below which is its depth: the reach is the most entries the list ever held,
 * below which its pages exist.
 */
#define HOST_DELTA_LIST_REACH 8

/* HOST_DELTA_LIST_LEVELS masks a list's depth in its depth word. */
#define HOST_DELTA_LIST_LEVELS ((UINT64CONST(1) << HOST_DELTA_LIST_REACH) - 1)

/*
 * HOST_DELTA_LIST_MOST is the most entries a list holds: its inline ones and
 * a leaf page's for each slot of the largest delta.
 */
#define HOST_DELTA_LIST_MOST (HOST_DELTA_LIST_INLINE + HOST_DELTA_LIST_FANOUT * HOST_DELTA_PAGE_SLOTS)

StaticAssertDecl(HOST_DELTA_LIST_FANOUT * HOST_DELTA_LIST_FANOUT * HOST_DELTA_LIST_FANOUT *
				 HOST_DELTA_LIST_FANOUT * HOST_DELTA_LIST_FANOUT >= HOST_DELTA_LIST_MOST &&
				 HOST_DELTA_LIST_DEEPEST <= HOST_DELTA_LIST_LEVELS &&
				 HOST_DELTA_LIST_MOST < (UINT64CONST(1) << (64 - HOST_DELTA_LIST_REACH)),
				 "a list's depth and most entries fit their bits");

/* The items below that an earlier item reaches, declared ahead so the file keeps list.rs's order. */
static uint64 host_delta_list_List_word(const HostDeltaListList *self, uint64 word);
static void host_delta_list_List_set_word(const HostDeltaListList *self, uint64 word, uint64 value);
static HostDeltaListEntry host_delta_list_List_inline(const HostDeltaListList *self, uint64 word);
static void host_delta_list_Entry_store(const HostDeltaListEntry *self, uint64 value);

/* host_delta_list_internal returns the fork's internal error with a message. */
static HostError
host_delta_list_internal(const char *message, size_t message_len)
{
	StdString	owned;

	std_string_from_Checked(&owned, message, message_len);
	return host_Error_fork((const uint8 *) "XX000", &owned);
}

/* host_delta_list_capacity returns the number of entries past the inline ones a tree of a depth holds. */
static uint64
host_delta_list_capacity(uint64 depth)
{
	return std_pow_u64(HOST_DELTA_LIST_FANOUT, (uint32) depth);
}

/*
 * host_delta_list_wants returns how many pages the push of the entry past a
 * length needs at a depth: a first root, a new root and a path below it when
 * the tree is full, or the new page of each level below the root whose first
 * entry it is.
 */
size_t
host_delta_list_wants(uint64 length, uint64 depth)
{
	uint64		entry;
	size_t		count = 0;

	if (length < HOST_DELTA_LIST_INLINE)
		return 0;
	entry = STD_SUB(u64, length, HOST_DELTA_LIST_INLINE, 78, 17);
	if (depth == 0)
		return 1;
	if (entry == host_delta_list_capacity(depth))
		return STD_ADD(usize, (size_t) depth, 1, 83, 16);
	for (uint64 level = 1, level_end = depth; level < level_end; level++)
		count = std_add_usize(count, std_is_multiple_of_u64(entry, host_delta_list_capacity(level)) ? 1 : 0,
							  STD_ACCUM_FILE, 206, 1);
	std_hint_assert_unchecked(count <= (size_t) (depth - 1), STD_ITER_FILTER_FILE, 251, 18);
	return count;
}

/*
 * host_delta_list_pages returns how many pages a list of a length takes past
 * its inline entries: its leaves, and each level above them up to its root.
 */
size_t
host_delta_list_pages(uint64 length)
{
	uint64		below = std_saturating_sub_u64(length, HOST_DELTA_LIST_INLINE);
	uint64		pages = 0;

	while (below > 0)
	{
		uint64		level = std_div_ceil_u64(below, HOST_DELTA_LIST_FANOUT, RUST_FILE, 94, 27);

		pages = STD_ADD(u64, pages, level, 95, 9);
		below = level == 1 ? 0 : level;
	}
	return (size_t) pages;
}

/*
 * host_delta_list_digit returns the index among a page's words of the path to
 * an entry past the inline ones at a level, the leaves being level 1.
 */
static uint64
host_delta_list_digit(uint64 entry, uint64 level)
{
	return STD_REM_FOLDED(u64, STD_DIV(u64, entry, host_delta_list_capacity(STD_SUB(u64, level, 1, 104, 22)), 104, 5), HOST_DELTA_LIST_FANOUT);
}

/* host_delta_list_Guard_list returns one of the guarded record's lists, to read under the guard. */
HostDeltaListListView
host_delta_list_Guard_list(const HostSharedGuard *self, const HostSharedShapeOps *s, size_t i)
{
	return (HostDeltaListListView)
	{
		{
			host_shared_Guard_place(self), s, i
		}
	};
}

/*
 * host_delta_list_Guard_closed returns one of the guarded record's lists once
 * it is closed, so a retirement that didn't close it reads its pages, or
 * nothing while it is open.
 */
bool
host_delta_list_Guard_closed(const HostSharedGuard *self, const HostSharedShapeOps *s, size_t i,
							 HostDeltaListClosed *result)
{
	HostDeltaListList list = {host_shared_Guard_place(self), s, i};

	if ((host_delta_list_List_word(&list, HOST_DELTA_LIST_LENGTH) & HOST_DELTA_LIST_CLOSED) != 0)
	{
		*result = (HostDeltaListClosed)
		{
			list
		};
		return true;
	}
	return false;
}

/*
 * host_delta_list_Made_fill_list fills one of the record's lists with its
 * entries while the record is unlinked, which nothing reaches before its
 * link, so it takes no lock: the first six inline and the rest on pages it
 * takes from the spare ones, as pages counts them, and fills while they are
 * owned; it returns those pages, a Vec of HostDeltaOwned, which the record's
 * lock owns at its link.
 */
bool
host_delta_list_Made_fill_list(const HostDeltaMade *self, const HostSharedShapeOps *s, size_t i,
							   const uint64 *entries, size_t entries_len, StdVec *spare,
							   StdVec *result, HostError *error)
{
	uint64		length = (uint64) entries_len;
	size_t		wanted;
	volatile StdVec filled;
	volatile bool filled_live = false;
	volatile StdVec level;
	volatile bool level_live = false;
	volatile StdVec names;
	volatile bool names_live = false;
	volatile HostError g_error;
	volatile bool g_error_live = false;
	StdUnwind	unwind;

	if (length > HOST_DELTA_LIST_MOST)
	{
		static const char text[] = "a list holds no more entries than the largest delta's";

		*error = host_delta_list_internal(text, sizeof(text) - 1);
		return false;
	}
	wanted = host_delta_list_pages(length);
	if (spare->len < wanted)
	{
		static const char text[] = "a list's fill wasn't given the pages it wants";

		*error = host_delta_list_internal(text, sizeof(text) - 1);
		return false;
	}
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		size_t		split = STD_SUB(usize, spare->len, wanted, 163, 42);
		HostDeltaListList list;
		const uint64 *inline_;
		size_t		inline_len;
		const uint64 *rest;
		size_t		rest_len;
		const uint64 *below;
		size_t		below_len;
		size_t		at;
		uint64		depth;
		bool		failed = false;

		std_vec_split_off(spare, split, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), (StdVec *) &filled, RUST_FILE, 163, 32);
		filled_live = true;
		list = (HostDeltaListList)
		{
			self->unlinked.place, s, i
		};
		std_slice_split_at_Checked(entries, entries_len, std_min_usize(entries_len, (size_t) HOST_DELTA_LIST_INLINE),
								   sizeof(uint64), _Alignof(uint64), (const void **) &inline_, &inline_len, (const void **) &rest, &rest_len,
								   RUST_FILE, 165, 38);
		std_vec_new((StdVec *) &level, sizeof(uint64), _Alignof(uint64));
		level_live = true;
		std_vec_new((StdVec *) &names, sizeof(uint64), _Alignof(uint64));
		names_live = true;
		below = rest;
		below_len = rest_len;
		at = 0;
		depth = 0;
		while (below_len != 0)
		{
			std_vec_clear((StdVec *) &names, sizeof(uint64), NULL);
			if (!std_vec_try_reserve((StdVec *) &names,
									 std_div_ceil_usize(below_len, (size_t) HOST_DELTA_LIST_FANOUT, RUST_FILE, 170, 43),
									 sizeof(uint64), _Alignof(uint64)))
			{
				g_error = host_Error_out_of_memory();
				g_error_live = true;
				failed = true;
				break;
			}
			std_slice_chunks_check((size_t) HOST_DELTA_LIST_FANOUT, RUST_FILE, 171, 32);
			for (size_t chunk = 0; chunk < below_len; chunk += (size_t) HOST_DELTA_LIST_FANOUT)
			{
				size_t		chunk_len = std_min_usize(below_len - chunk, (size_t) HOST_DELTA_LIST_FANOUT);
				HostDeltaOwned *page;
				uint64		name;

				std_index_check(at, ((StdVec *) &filled)->len, RUST_FILE, 172, 39);
				page = &STD_VEC_AT((StdVec *) &filled, HostDeltaOwned, at);
				for (size_t count = 0; count < chunk_len;)
				{
					size_t		word = count;

					count = std_Enumerate_count_step(count);
					if (!host_delta_Owned_store(page, (uint64) word, below[chunk + word], (HostError *) &g_error))
					{
						g_error_live = true;
						failed = true;
						break;
					}
				}
				if (failed)
					break;
				name = host_delta_page_Page_value(host_delta_Owned_name(page));
				std_vec_push_Checked((StdVec *) &names, &name, sizeof(uint64), _Alignof(uint64));
				at = STD_ADD(usize, at, 1, 177, 17);
			}
			if (failed)
				break;
			depth = STD_ADD(u64, depth, 1, 179, 13);
			if (((StdVec *) &names)->len == 1)
			{
				std_index_check(0, ((StdVec *) &names)->len, RUST_FILE, 181, 42);
				host_delta_list_List_set_word(&list, HOST_DELTA_LIST_ROOT, STD_VEC_AT((StdVec *) &names, uint64, 0));
				break;
			}
			{
				StdVec		swapped = *(StdVec *) &level;

				*(StdVec *) &level = *(StdVec *) &names;
				*(StdVec *) &names = swapped;
			}
			below = ((StdVec *) &level)->ptr;
			below_len = ((StdVec *) &level)->len;
		}
		if (failed)
		{
			names_live = false;
			std_vec_free_Checked((StdVec *) &names, sizeof(uint64), _Alignof(uint64));
			level_live = false;
			std_vec_free_Checked((StdVec *) &level, sizeof(uint64), _Alignof(uint64));
			filled_live = false;
			std_vec_drop_Checked((StdVec *) &filled, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
			std_unwind_pop(&unwind);
			*error = *(HostError *) &g_error;
			return false;
		}
		for (size_t count = 0; count < inline_len;)
		{
			size_t		index = count;
			HostDeltaListEntry entry;

			count = std_Enumerate_count_step(count);
			entry = host_delta_list_List_inline(&list, (uint64) index);

			host_delta_list_Entry_store(&entry, inline_[index]);
		}
		host_delta_list_List_set_word(&list, HOST_DELTA_LIST_DEPTH,
									  STD_SHL_FOLDED(u64, length, HOST_DELTA_LIST_REACH) | depth);
		host_delta_list_List_set_word(&list, HOST_DELTA_LIST_LENGTH, length);
		names_live = false;
		std_vec_free_Checked((StdVec *) &names, sizeof(uint64), _Alignof(uint64));
		level_live = false;
		std_vec_free_Checked((StdVec *) &level, sizeof(uint64), _Alignof(uint64));
		filled_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (g_error_live)
			host_Error_drop((HostError *) &g_error);
		if (names_live)
			std_vec_free_Checked((StdVec *) &names, sizeof(uint64), _Alignof(uint64));
		if (level_live)
			std_vec_free_Checked((StdVec *) &level, sizeof(uint64), _Alignof(uint64));
		if (filled_live)
			std_vec_drop_Checked((StdVec *) &filled, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), host_delta_Owned_drop);
		std_unwind_resume(&unwind);
	}
	*result = *(StdVec *) &filled;
	return true;
}

/*
 * host_delta_list_GuardExclusive_list_mut returns one of the guarded
 * record's lists, to read and store under the guard held exclusively.
 */
HostDeltaListListMut
host_delta_list_GuardExclusive_list_mut(HostSharedGuard *self, const HostSharedShapeOps *s, size_t l, size_t i)
{
	return (HostDeltaListListMut)
	{
		{
			host_shared_Guard_place(self), s, i
		}, self, l
	};
}

/* host_delta_list_Entry_load reads the entry. */
uint64
host_delta_list_Entry_load(const HostDeltaListEntry *self)
{
	switch (self->tag)
	{
		case HOST_DELTA_LIST_AT_INLINE:
			{
				HostHost	host = host_current();
				HostSharedOp load = {HOST_SHARED_OP_LOAD};

				return host.ops->atomic(host.self, self->at.inline_.f0, self->at.inline_.f1, load);
			}
		case HOST_DELTA_LIST_AT_PAGED:
			return host_delta_Word_load(&self->at.paged);
	}
	pg_unreachable();
}

/* host_delta_list_Entry_store writes the entry. */
static void
host_delta_list_Entry_store(const HostDeltaListEntry *self, uint64 value)
{
	switch (self->tag)
	{
		case HOST_DELTA_LIST_AT_INLINE:
			host_shared_stored(self->at.inline_.f0, self->at.inline_.f1, value);
			break;
		case HOST_DELTA_LIST_AT_PAGED:
			host_delta_Word_store(&self->at.paged, value);
			break;
	}
}

/* host_delta_list_Entry_Takes_take stores a page's name in the entry. */
static void
host_delta_list_Entry_Takes_take(void *self, uint64 name)
{
	host_delta_list_Entry_store((const HostDeltaListEntry *) self, name);
}

/* HOST_DELTA_LIST_ENTRY_NAMES_OPS is an entry's table of Names. */
const HostDeltaNamesOps HOST_DELTA_LIST_ENTRY_NAMES_OPS = {{host_delta_list_Entry_Takes_take}};

/* host_delta_list_List_atomic returns the index among the record's atomics of one of the list's words. */
static size_t
host_delta_list_List_atomic(const HostDeltaListList *self, uint64 word)
{
	size_t		tmp_1 = STD_MUL(usize, HOST_DELTA_LIST_WORDS, self->i, 248, 22);
	size_t		tmp_2 = STD_ADD(usize, self->s->atomics, tmp_1, 248, 9);

	return STD_ADD(usize, tmp_2, (size_t) word, 248, 9);
}

/* host_delta_list_List_word loads one of the list's words. */
static uint64
host_delta_list_List_word(const HostDeltaListList *self, uint64 word)
{
	HostHost	host = host_current();
	size_t		atomic = host_delta_list_List_atomic(self, word);
	HostSharedOp load = {HOST_SHARED_OP_LOAD};

	return host.ops->atomic(host.self, self->place, atomic, load);
}

/* host_delta_list_List_set_word stores one of the list's words. */
static void
host_delta_list_List_set_word(const HostDeltaListList *self, uint64 word, uint64 value)
{
	host_shared_stored(self->place, host_delta_list_List_atomic(self, word), value);
}

/* host_delta_list_List_inline returns one of the list's words as an entry. */
static HostDeltaListEntry
host_delta_list_List_inline(const HostDeltaListList *self, uint64 word)
{
	HostDeltaListEntry entry = {HOST_DELTA_LIST_AT_INLINE};

	entry.at.inline_.f0 = self->place;
	entry.at.inline_.f1 = host_delta_list_List_atomic(self, word);
	return entry;
}

/* host_delta_list_List_len returns the list's count of entries. */
static uint64
host_delta_list_List_len(const HostDeltaListList *self)
{
	return host_delta_list_List_word(self, HOST_DELTA_LIST_LENGTH) & ~HOST_DELTA_LIST_CLOSED;
}

/* host_delta_list_List_depth returns the list's count of levels of pages. */
static uint64
host_delta_list_List_depth(const HostDeltaListList *self)
{
	return host_delta_list_List_word(self, HOST_DELTA_LIST_DEPTH) & HOST_DELTA_LIST_LEVELS;
}

/* host_delta_list_List_reach returns the most entries the list ever held, below which its pages exist. */
static uint64
host_delta_list_List_reach(const HostDeltaListList *self)
{
	return STD_SHR_FOLDED(u64, host_delta_list_List_word(self, HOST_DELTA_LIST_DEPTH), HOST_DELTA_LIST_REACH);
}

/* host_delta_list_List_counted stores the list's length, its reach growing with it. */
static void
host_delta_list_List_counted(const HostDeltaListList *self, uint64 length)
{
	uint64		reach = std_max_u64(host_delta_list_List_reach(self), length);
	uint64		shifted = STD_SHL_FOLDED(u64, reach, HOST_DELTA_LIST_REACH);

	host_delta_list_List_set_word(self, HOST_DELTA_LIST_DEPTH, shifted | host_delta_list_List_depth(self));
	host_delta_list_List_set_word(self, HOST_DELTA_LIST_LENGTH, length);
}

/* host_delta_list_List_wants returns how many pages the entry past the list's length needs: none below its reach. */
static size_t
host_delta_list_List_wants(const HostDeltaListList *self)
{
	uint64		length = host_delta_list_List_len(self);

	if (length < host_delta_list_List_reach(self))
		return 0;
	return host_delta_list_wants(length, host_delta_list_List_depth(self));
}

/* host_delta_list_List_open refuses a store to a closed list and returns the list's length. */
static bool
host_delta_list_List_open(const HostDeltaListList *self, uint64 *result, HostError *error)
{
	if ((host_delta_list_List_word(self, HOST_DELTA_LIST_LENGTH) & HOST_DELTA_LIST_CLOSED) != 0)
	{
		static const char text[] = "a closed list takes no change";

		*error = host_delta_list_internal(text, sizeof(text) - 1);
		return false;
	}
	*result = host_delta_list_List_len(self);
	return true;
}

static bool host_delta_list_List_at(const HostDeltaListList *self, const HostDeltaHold *hold, uint64 index,
									HostDeltaListEntry *result, HostError *error);

/* host_delta_list_List_entry returns the entry at an index, refusing one past the list's length. */
static bool
host_delta_list_List_entry(const HostDeltaListList *self, const HostDeltaHold *hold, uint64 index,
						   HostDeltaListEntry *result, HostError *error)
{
	if (index >= host_delta_list_List_len(self))
	{
		static const char text[] = "an index of a list is past its length";

		*error = host_delta_list_internal(text, sizeof(text) - 1);
		return false;
	}
	return host_delta_list_List_at(self, hold, index, result, error);
}

/*
 * host_delta_list_List_span opens the entries from an index up to another,
 * which the tree holds, so that a change opens every entry it stores before
 * its first store; the result is a Vec of HostDeltaListEntry.
 */
static bool
host_delta_list_List_span(const HostDeltaListList *self, const HostDeltaHold *hold, uint64 from, uint64 to,
						  StdVec *result, HostError *error)
{
	volatile StdVec entries;
	volatile bool entries_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		std_vec_new((StdVec *) &entries, sizeof(HostDeltaListEntry), _Alignof(HostDeltaListEntry));
		entries_live = true;
		if (!std_vec_try_reserve((StdVec *) &entries, (size_t) std_saturating_sub_u64(to, from),
								 sizeof(HostDeltaListEntry), _Alignof(HostDeltaListEntry)))
		{
			*error = host_Error_out_of_memory();
			entries_live = false;
			std_vec_free_Checked((StdVec *) &entries, sizeof(HostDeltaListEntry), _Alignof(HostDeltaListEntry));
			std_unwind_pop(&unwind);
			return false;
		}
		for (uint64 index = from, index_end = to; index < index_end; index++)
		{
			HostDeltaListEntry entry;

			if (!host_delta_list_List_at(self, hold, index, &entry, error))
			{
				entries_live = false;
				std_vec_free_Checked((StdVec *) &entries, sizeof(HostDeltaListEntry), _Alignof(HostDeltaListEntry));
				std_unwind_pop(&unwind);
				return false;
			}
			std_vec_push_Checked((StdVec *) &entries, &entry, sizeof(HostDeltaListEntry), _Alignof(HostDeltaListEntry));
		}
		entries_live = false;
		std_unwind_pop(&unwind);
	}
	else
	{
		if (entries_live)
			std_vec_free_Checked((StdVec *) &entries, sizeof(HostDeltaListEntry), _Alignof(HostDeltaListEntry));
		std_unwind_resume(&unwind);
	}
	*result = *(StdVec *) &entries;
	return true;
}

/* host_delta_list_List_at returns the entry at an index the tree holds. */
static bool
host_delta_list_List_at(const HostDeltaListList *self, const HostDeltaHold *hold, uint64 index,
						HostDeltaListEntry *result, HostError *error)
{
	uint64		entry;
	HostDeltaPageRef page;
	HostDeltaWord word;

	if (index < HOST_DELTA_LIST_INLINE)
	{
		*result = host_delta_list_List_inline(self, index);
		return true;
	}
	entry = STD_SUB(u64, index, HOST_DELTA_LIST_INLINE, 325, 21);
	if (!host_delta_Hold_page(hold, host_delta_page_Page_named(host_delta_list_List_word(self, HOST_DELTA_LIST_ROOT)),
							  &page, error))
		return false;
	{
		uint64		level_end = 2;
		uint64		level = host_delta_list_List_depth(self);
		bool		level_done = level_end > level;

		for (; !level_done; level_done = (level == level_end), level -= !level_done)
		{
			HostDeltaWord at;

			if (!host_delta_PageRef_word(&page, host_delta_list_digit(entry, level), &at, error))
				return false;
			if (!host_delta_Hold_page(hold, host_delta_page_Page_named(host_delta_Word_load(&at)), &page, error))
				return false;
		}
	}
	if (!host_delta_PageRef_word(&page, STD_REM_FOLDED(u64, entry, HOST_DELTA_LIST_FANOUT), &word, error))
		return false;
	result->tag = HOST_DELTA_LIST_AT_PAGED;
	result->at.paged = word;
	return true;
}

/* host_delta_list_ListView_len returns the list's count of entries. */
uint64
host_delta_list_ListView_len(const HostDeltaListListView *self)
{
	return host_delta_list_List_len(&self->list);
}

/* host_delta_list_ListView_is_empty reports whether the list has no entry. */
bool
host_delta_list_ListView_is_empty(const HostDeltaListListView *self)
{
	return host_delta_list_ListView_len(self) == 0;
}

/*
 * host_delta_list_ListView_wants returns how many list pages the next push or
 * insert can need, which a writer allocates before it takes the lock
 * exclusively.
 */
size_t
host_delta_list_ListView_wants(const HostDeltaListListView *self)
{
	return host_delta_list_List_wants(&self->list);
}

/* host_delta_list_ListView_reserving returns how many list pages a reserve of room for a number of entries needs. */
size_t
host_delta_list_ListView_reserving(const HostDeltaListListView *self, uint64 entries)
{
	uint64		depth = host_delta_list_List_depth(&self->list);
	size_t		wanted = 0;

	for (uint64 index = host_delta_list_List_reach(&self->list), index_end = entries; index < index_end; index++)
	{
		wanted = STD_ADD(usize, wanted, host_delta_list_wants(index, depth), 362, 13);
		if (index >= HOST_DELTA_LIST_INLINE)
		{
			bool		grows = depth == 0;

			if (!grows)
			{
				uint64		tmp_1 = STD_SUB(u64, index, HOST_DELTA_LIST_INLINE, 363, 50);
				uint64		tmp_2 = host_delta_list_capacity(depth);

				grows = tmp_1 == tmp_2;
			}
			if (grows)
				depth = STD_ADD(u64, depth, 1, 364, 17);
		}
	}
	return wanted;
}

/* host_delta_list_ListView_entry returns the entry at an index, refusing one past the list's length, for no longer than the view. */
bool
host_delta_list_ListView_entry(const HostDeltaListListView *self, const HostDeltaHold *hold,
							   uint64 index, HostDeltaListEntry *result, HostError *error)
{
	return host_delta_list_List_entry(&self->list, hold, index, result, error);
}

/* host_delta_list_ListView_get loads the entry at an index, refusing one past the list's length. */
bool
host_delta_list_ListView_get(const HostDeltaListListView *self, const HostDeltaHold *hold,
							 uint64 index, uint64 *result, HostError *error)
{
	HostDeltaListEntry entry;

	if (!host_delta_list_List_entry(&self->list, hold, index, &entry, error))
		return false;
	*result = host_delta_list_Entry_load(&entry);
	return true;
}

/* host_delta_list_ListMut_view returns the list to read. */
HostDeltaListListView
host_delta_list_ListMut_view(const HostDeltaListListMut *self)
{
	return (HostDeltaListListView)
	{
		self->list
	};
}

/* host_delta_list_ListMut_set stores a value over the entry at an index below the list's length. */
bool
host_delta_list_ListMut_set(HostDeltaListListMut *self, const HostDeltaHold *hold, uint64 index,
							uint64 value, HostError *error)
{
	uint64		length;
	HostDeltaListEntry entry;

	if (!host_delta_list_List_open(&self->list, &length, error))
		return false;
	if (!host_delta_list_List_entry(&self->list, hold, index, &entry, error))
		return false;
	host_delta_list_Entry_store(&entry, value);
	return true;
}

/*
 * host_delta_list_ListMut_set_page links a page its owner fills over the
 * entry at an index below the list's length, the record's lock owning it,
 * and returns its name; it takes the page.
 */
bool
host_delta_list_ListMut_set_page(HostDeltaListListMut *self, const HostDeltaHold *hold,
								 uint64 index, HostDeltaOwned *page, HostDeltaPagePage *result,
								 HostError *error)
{
	HostDeltaPagePage name;
	volatile bool page_live = true;
	volatile HostError g_error;
	volatile bool g_error_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		uint64		length;
		HostDeltaListEntry entry;
		HostDeltaNames names;

		if (!host_delta_list_List_open(&self->list, &length, (HostError *) &g_error) ||
			!host_delta_list_List_entry(&self->list, hold, index, &entry, (HostError *) &g_error))
		{
			g_error_live = true;
			page_live = false;
			host_delta_Owned_drop(page);
			std_unwind_pop(&unwind);
			*error = *(HostError *) &g_error;
			return false;
		}
		names = (HostDeltaNames)
		{
			&HOST_DELTA_LIST_ENTRY_NAMES_OPS, &entry
		};
		page_live = false;
		name = host_delta_Owned_link(page, self->list.s, self->l, self->guard, names);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (g_error_live)
			host_Error_drop((HostError *) &g_error);
		if (page_live)
			host_delta_Owned_drop(page);
		std_unwind_resume(&unwind);
	}
	*result = name;
	return true;
}

static bool host_delta_list_ListMut_grow(HostDeltaListListMut *self, const HostDeltaHold *hold, uint64 entry,
										 uint64 depth, StdVec *spare, HostError *error);

/*
 * host_delta_list_ListMut_slot makes room for the entry past the last,
 * growing the tree with the pages it is given where the list's reach ends (a
 * writer gives at least wants of them, and keeps what isn't used), and
 * returns its index.
 */
static bool
host_delta_list_ListMut_slot(HostDeltaListListMut *self, const HostDeltaHold *hold, StdVec *spare, uint64 *result,
							 HostError *error)
{
	uint64		length;

	if (!host_delta_list_List_open(&self->list, &length, error))
		return false;
	if (spare->len < host_delta_list_List_wants(&self->list))
	{
		static const char text[] = "a list's growth wasn't given the pages it wants";

		*error = host_delta_list_internal(text, sizeof(text) - 1);
		return false;
	}
	if (length >= HOST_DELTA_LIST_INLINE && length >= host_delta_list_List_reach(&self->list))
	{
		uint64		tmp_1 = STD_SUB(u64, length, HOST_DELTA_LIST_INLINE, 415, 29);
		uint64		tmp_2 = host_delta_list_List_depth(&self->list);

		if (!host_delta_list_ListMut_grow(self, hold, tmp_1, tmp_2, spare, error))
			return false;
	}
	*result = length;
	return true;
}

/* host_delta_list_ListMut_push adds a value past the last entry: it stores the value and then counts it. */
bool
host_delta_list_ListMut_push(HostDeltaListListMut *self, const HostDeltaHold *hold, uint64 value,
							 StdVec *spare, HostError *error)
{
	uint64		length;
	HostDeltaListEntry entry;

	if (!host_delta_list_ListMut_slot(self, hold, spare, &length, error))
		return false;
	if (!host_delta_list_List_at(&self->list, hold, length, &entry, error))
		return false;
	host_delta_list_Entry_store(&entry, value);
	host_delta_list_List_counted(&self->list, STD_ADD(u64, length, 1, 424, 27));
	return true;
}

/*
 * host_delta_list_ListMut_push_page links a page its owner fills past the
 * last entry, the record's lock owning it, and then counts it; it returns
 * the page's name. It takes the page.
 */
bool
host_delta_list_ListMut_push_page(HostDeltaListListMut *self, const HostDeltaHold *hold,
								  HostDeltaOwned *page, StdVec *spare, HostDeltaPagePage *result,
								  HostError *error)
{
	HostDeltaPagePage name;
	volatile bool page_live = true;
	volatile HostError g_error;
	volatile bool g_error_live = false;
	StdUnwind	unwind;

	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		uint64		length;
		HostDeltaListEntry entry;
		HostDeltaNames names;

		if (!host_delta_list_ListMut_slot(self, hold, spare, &length, (HostError *) &g_error) ||
			!host_delta_list_List_at(&self->list, hold, length, &entry, (HostError *) &g_error))
		{
			g_error_live = true;
			page_live = false;
			host_delta_Owned_drop(page);
			std_unwind_pop(&unwind);
			*error = *(HostError *) &g_error;
			return false;
		}
		names = (HostDeltaNames)
		{
			&HOST_DELTA_LIST_ENTRY_NAMES_OPS, &entry
		};
		page_live = false;
		name = host_delta_Owned_link(page, self->list.s, self->l, self->guard, names);
		host_delta_list_List_counted(&self->list, STD_ADD(u64, length, 1, 433, 27));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (g_error_live)
			host_Error_drop((HostError *) &g_error);
		if (page_live)
			host_delta_Owned_drop(page);
		std_unwind_resume(&unwind);
	}
	*result = name;
	return true;
}

/*
 * host_delta_list_ListMut_grow_closure_0 is grow's wanted: the error of a
 * growth given too few pages. It captures nothing.
 */
static HostError
host_delta_list_ListMut_grow_closure_0(void *captures)
{
	static const char text[] = "a list's growth wasn't given the pages it wants";

	return host_delta_list_internal(text, sizeof(text) - 1);
}

/*
 * host_delta_list_ListMut_grow links the pages that the entry past the inline
 * ones at an index needs: a first root, a new root whose first word names the
 * old one when the tree is full, and a new page at each level below the root
 * whose first entry it is.
 */
static bool
host_delta_list_ListMut_grow(HostDeltaListListMut *self, const HostDeltaHold *hold, uint64 entry, uint64 depth,
							 StdVec *spare, HostError *error)
{
	HostDeltaListEntry root = host_delta_list_List_inline(&self->list, HOST_DELTA_LIST_ROOT);
	HostDeltaNames root_names = {&HOST_DELTA_LIST_ENTRY_NAMES_OPS, &root};
	uint64		depth_2;
	HostDeltaPageRef page;

	if (depth == 0)
	{
		HostDeltaOwned taken;

		if (!std_vec_pop(spare, &taken, sizeof(HostDeltaOwned)))
		{
			*error = host_delta_list_ListMut_grow_closure_0(NULL);
			return false;
		}
		(void) host_delta_Owned_link(&taken, self->list.s, self->l, self->guard, root_names);
		host_delta_list_List_set_word(&self->list, HOST_DELTA_LIST_DEPTH,
									  STD_SHL_FOLDED(u64, host_delta_list_List_reach(&self->list), HOST_DELTA_LIST_REACH) | 1);
		return true;
	}
	else if (entry == host_delta_list_capacity(depth))
	{
		volatile HostDeltaOwned new;
		volatile bool new_live = false;
		volatile HostError g_error;
		volatile bool g_error_live = false;
		StdUnwind	unwind;

		if (!std_vec_pop(spare, (HostDeltaOwned *) &new, sizeof(HostDeltaOwned)))
		{
			*error = host_delta_list_ListMut_grow_closure_0(NULL);
			return false;
		}
		new_live = true;
		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
		{
			uint64		tmp_1 = host_delta_list_List_word(&self->list, HOST_DELTA_LIST_ROOT);
			uint64		tmp_2;
			uint64		tmp_3;

			if (!host_delta_Owned_store((HostDeltaOwned *) &new, 0, tmp_1, (HostError *) &g_error))
			{
				g_error_live = true;
				new_live = false;
				host_delta_Owned_drop((HostDeltaOwned *) &new);
				std_unwind_pop(&unwind);
				*error = *(HostError *) &g_error;
				return false;
			}
			new_live = false;
			(void) host_delta_Owned_link((HostDeltaOwned *) &new, self->list.s, self->l, self->guard, root_names);
			tmp_2 = STD_SHL_FOLDED(u64, host_delta_list_List_reach(&self->list), HOST_DELTA_LIST_REACH);
			tmp_3 = STD_ADD(u64, depth, 1, 451, 68);
			host_delta_list_List_set_word(&self->list, HOST_DELTA_LIST_DEPTH, tmp_2 | tmp_3);
			depth_2 = STD_ADD(u64, depth, 1, 452, 13);
			std_unwind_pop(&unwind);
		}
		else
		{
			if (g_error_live)
				host_Error_drop((HostError *) &g_error);
			if (new_live)
				host_delta_Owned_drop((HostDeltaOwned *) &new);
			std_unwind_resume(&unwind);
		}
	}
	else
		depth_2 = depth;
	if (!host_delta_Hold_page(hold, host_delta_page_Page_named(host_delta_list_List_word(&self->list, HOST_DELTA_LIST_ROOT)),
							  &page, error))
		return false;
	{
		uint64		level_end = 2;
		uint64		level = depth_2;
		bool		level_done = level_end > level;

		for (; !level_done; level_done = (level == level_end), level -= !level_done)
		{
			HostDeltaWord word;

			if (!host_delta_PageRef_word(&page, host_delta_list_digit(entry, level), &word, error))
				return false;
			if (std_is_multiple_of_u64(entry, host_delta_list_capacity(STD_SUB(u64, level, 1, 459, 46))))
			{
				volatile HostDeltaOwned child;
				volatile bool child_live = false;
				volatile HostError g_error;
				volatile bool g_error_live = false;
				StdUnwind	unwind;

				if (!std_vec_pop(spare, (HostDeltaOwned *) &child, sizeof(HostDeltaOwned)))
				{
					*error = host_delta_list_ListMut_grow_closure_0(NULL);
					return false;
				}
				child_live = true;
				STD_UNWIND_PUSH(&unwind);
				if (sigsetjmp(unwind.jump, 0) == 0)
				{
					HostDeltaListEntry child_entry = {HOST_DELTA_LIST_AT_PAGED};
					HostDeltaNames child_names = {&HOST_DELTA_LIST_ENTRY_NAMES_OPS, &child_entry};

					if (!host_delta_PageRef_word(&page, host_delta_list_digit(entry, level), &child_entry.at.paged,
												 (HostError *) &g_error))
					{
						g_error_live = true;
						child_live = false;
						host_delta_Owned_drop((HostDeltaOwned *) &child);
						std_unwind_pop(&unwind);
						*error = *(HostError *) &g_error;
						return false;
					}
					child_live = false;
					(void) host_delta_Owned_link((HostDeltaOwned *) &child, self->list.s, self->l, self->guard,
												 child_names);
					std_unwind_pop(&unwind);
				}
				else
				{
					if (g_error_live)
						host_Error_drop((HostError *) &g_error);
					if (child_live)
						host_delta_Owned_drop((HostDeltaOwned *) &child);
					std_unwind_resume(&unwind);
				}
			}
			if (!host_delta_Hold_page(hold, host_delta_page_Page_named(host_delta_Word_load(&word)), &page, error))
				return false;
		}
	}
	return true;
}

/*
 * host_delta_list_ListMut_reserve grows the list's tree with the pages it is
 * given, reserving of them, so that it holds a number of entries with none
 * added: its reach rises to them, so a later push or insert up to that many
 * takes no page.
 */
bool
host_delta_list_ListMut_reserve(HostDeltaListListMut *self, const HostDeltaHold *hold,
								uint64 entries, StdVec *spare, HostError *error)
{
	uint64		length;
	HostDeltaListListView view;

	if (!host_delta_list_List_open(&self->list, &length, error))
		return false;
	if (entries > HOST_DELTA_LIST_MOST)
	{
		static const char text[] = "a list holds no more entries than the largest delta's";

		*error = host_delta_list_internal(text, sizeof(text) - 1);
		return false;
	}
	view = host_delta_list_ListMut_view(self);
	if (spare->len < host_delta_list_ListView_reserving(&view, entries))
	{
		static const char text[] = "a list's growth wasn't given the pages it wants";

		*error = host_delta_list_internal(text, sizeof(text) - 1);
		return false;
	}
	for (uint64 index = host_delta_list_List_reach(&self->list), index_end = entries; index < index_end; index++)
	{
		uint64		tmp_3;
		uint64		tmp_4;

		if (index >= HOST_DELTA_LIST_INLINE)
		{
			uint64		tmp_1 = STD_SUB(u64, index, HOST_DELTA_LIST_INLINE, 480, 33);
			uint64		tmp_2 = host_delta_list_List_depth(&self->list);

			if (!host_delta_list_ListMut_grow(self, hold, tmp_1, tmp_2, spare, error))
				return false;
		}
		tmp_3 = STD_SHL_FOLDED(u64, STD_ADD(u64, index, 1, 482, 39), HOST_DELTA_LIST_REACH);
		tmp_4 = host_delta_list_List_depth(&self->list);
		host_delta_list_List_set_word(&self->list, HOST_DELTA_LIST_DEPTH, tmp_3 | tmp_4);
	}
	return true;
}

/* host_delta_list_VecEntry_IndexUsize_index returns an entry of a span. */
static HostDeltaListEntry *
host_delta_list_VecEntry_IndexUsize_index(StdVec *entries, size_t at, const char *file, uint32 line, uint32 col)
{
	std_index_check(at, entries->len, file, line, col);
	return &STD_VEC_AT(entries, HostDeltaListEntry, at);
}

/*
 * host_delta_list_ListMut_insert puts a value at an index, moving the entries
 * from it on up by one, and then counts it, opening every entry it stores
 * before its first store; only a list that Removes takes it.
 */
bool
host_delta_list_ListMut_insert(HostDeltaListListMut *self, const HostDeltaHold *hold, uint64 index,
							   uint64 value, StdVec *spare, HostError *error)
{
	uint64		length;
	volatile StdVec entries;
	volatile bool entries_live = false;
	StdUnwind	unwind;

	if (index > host_delta_list_List_len(&self->list))
	{
		static const char text[] = "an index of a list is past its length";

		*error = host_delta_list_internal(text, sizeof(text) - 1);
		return false;
	}
	if (!host_delta_list_ListMut_slot(self, hold, spare, &length, error))
		return false;
	if (!host_delta_list_List_span(&self->list, hold, index, STD_ADD(u64, length, 1, 497, 51), (StdVec *) &entries,
								   error))
		return false;
	entries_live = true;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		for (size_t at_end = 1, at = entries.len; at > at_end;)
		{
			HostDeltaListEntry *to;
			size_t		from_at;
			HostDeltaListEntry *from;
			uint64		loaded;

			at--;
			to = host_delta_list_VecEntry_IndexUsize_index((StdVec *) &entries, at, RUST_FILE, 499, 20);
			from_at = STD_SUB(usize, at, 1, 499, 39);
			from = host_delta_list_VecEntry_IndexUsize_index((StdVec *) &entries, from_at, RUST_FILE, 499, 38);
			loaded = host_delta_list_Entry_load(from);
			host_delta_list_Entry_store(to, loaded);
		}
		host_delta_list_Entry_store(host_delta_list_VecEntry_IndexUsize_index((StdVec *) &entries, 0, RUST_FILE, 501, 16), value);
		host_delta_list_List_counted(&self->list, STD_ADD(u64, length, 1, 502, 27));
		entries_live = false;
		std_vec_free_Checked((StdVec *) &entries, sizeof(HostDeltaListEntry), _Alignof(HostDeltaListEntry));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (entries_live)
			std_vec_free_Checked((StdVec *) &entries, sizeof(HostDeltaListEntry), _Alignof(HostDeltaListEntry));
		std_unwind_resume(&unwind);
	}
	return true;
}

/*
 * host_delta_list_ListMut_replace_with puts two values in the place of the
 * entry at an index, the first at the index and the second after it, moving
 * the later entries up by one, and then counts the one more, opening every
 * entry it stores before its first store; only a list that Removes takes it.
 */
bool
host_delta_list_ListMut_replace_with(HostDeltaListListMut *self, const HostDeltaHold *hold,
									 uint64 index, const uint64 *values, StdVec *spare,
									 HostError *error)
{
	uint64		length;
	volatile StdVec entries;
	volatile bool entries_live = false;
	StdUnwind	unwind;

	if (index >= host_delta_list_List_len(&self->list))
	{
		static const char text[] = "an index of a list is past its length";

		*error = host_delta_list_internal(text, sizeof(text) - 1);
		return false;
	}
	if (!host_delta_list_ListMut_slot(self, hold, spare, &length, error))
		return false;
	if (!host_delta_list_List_span(&self->list, hold, index, STD_ADD(u64, length, 1, 523, 51), (StdVec *) &entries,
								   error))
		return false;
	entries_live = true;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		for (size_t at_end = 2, at = entries.len; at > at_end;)
		{
			HostDeltaListEntry *to;
			size_t		from_at;
			HostDeltaListEntry *from;
			uint64		loaded;

			at--;
			to = host_delta_list_VecEntry_IndexUsize_index((StdVec *) &entries, at, RUST_FILE, 525, 20);
			from_at = STD_SUB(usize, at, 1, 525, 39);
			from = host_delta_list_VecEntry_IndexUsize_index((StdVec *) &entries, from_at, RUST_FILE, 525, 38);
			loaded = host_delta_list_Entry_load(from);
			host_delta_list_Entry_store(to, loaded);
		}
		host_delta_list_Entry_store(host_delta_list_VecEntry_IndexUsize_index((StdVec *) &entries, 1, RUST_FILE, 527, 16), values[1]);
		host_delta_list_Entry_store(host_delta_list_VecEntry_IndexUsize_index((StdVec *) &entries, 0, RUST_FILE, 528, 16), values[0]);
		host_delta_list_List_counted(&self->list, STD_ADD(u64, length, 1, 529, 27));
		entries_live = false;
		std_vec_free_Checked((StdVec *) &entries, sizeof(HostDeltaListEntry), _Alignof(HostDeltaListEntry));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (entries_live)
			std_vec_free_Checked((StdVec *) &entries, sizeof(HostDeltaListEntry), _Alignof(HostDeltaListEntry));
		std_unwind_resume(&unwind);
	}
	return true;
}

/*
 * host_delta_list_ListMut_remove takes the entry at an index out, moving the
 * entries past it down by one, opening every entry it stores before its first
 * store; only a list that Removes takes it.
 */
bool
host_delta_list_ListMut_remove(HostDeltaListListMut *self, const HostDeltaHold *hold, uint64 index,
							   HostError *error)
{
	uint64		length;
	volatile StdVec entries;
	volatile bool entries_live = false;
	StdUnwind	unwind;

	if (!host_delta_list_List_open(&self->list, &length, error))
		return false;
	if (index >= length)
	{
		static const char text[] = "an index of a list is past its length";

		*error = host_delta_list_internal(text, sizeof(text) - 1);
		return false;
	}
	if (!host_delta_list_List_span(&self->list, hold, index, length, (StdVec *) &entries, error))
		return false;
	entries_live = true;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		for (size_t at = 1, at_end = entries.len; at < at_end; at++)
		{
			size_t		to_at = STD_SUB(usize, at, 1, 545, 21);
			HostDeltaListEntry *to = host_delta_list_VecEntry_IndexUsize_index((StdVec *) &entries, to_at, RUST_FILE, 545, 20);
			HostDeltaListEntry *from = host_delta_list_VecEntry_IndexUsize_index((StdVec *) &entries, at, RUST_FILE, 545, 42);
			uint64		loaded = host_delta_list_Entry_load(from);

			host_delta_list_Entry_store(to, loaded);
		}
		host_delta_list_List_counted(&self->list, STD_SUB(u64, length, 1, 547, 27));
		entries_live = false;
		std_vec_free_Checked((StdVec *) &entries, sizeof(HostDeltaListEntry), _Alignof(HostDeltaListEntry));
		std_unwind_pop(&unwind);
	}
	else
	{
		if (entries_live)
			std_vec_free_Checked((StdVec *) &entries, sizeof(HostDeltaListEntry), _Alignof(HostDeltaListEntry));
		std_unwind_resume(&unwind);
	}
	return true;
}

/*
 * host_delta_list_ListMut_close closes the list as the last store it takes,
 * after which every store refuses it and its record's retirement reads its
 * pages with no lock; it takes the list.
 */
HostDeltaListClosed
host_delta_list_ListMut_close(HostDeltaListListMut *self)
{
	host_delta_list_List_set_word(&self->list, HOST_DELTA_LIST_LENGTH,
								  host_delta_list_List_word(&self->list, HOST_DELTA_LIST_LENGTH) | HOST_DELTA_LIST_CLOSED);
	return (HostDeltaListClosed)
	{
		self->list
	};
}

/*
 * host_delta_list_Closed_pages returns the names of the list's pages, level by
 * level from its root, for its record's one retirement call; it opens pages
 * under the hold and takes no lock.
 */
HostDeltaListPages
host_delta_list_Closed_pages(const HostDeltaListClosed *self, const HostDeltaHold *hold)
{
	uint64		depth = host_delta_list_List_depth(&self->list);
	uint64		root = host_delta_list_List_word(&self->list, HOST_DELTA_LIST_ROOT);
	uint64		entries = std_saturating_sub_u64(host_delta_list_List_reach(&self->list), HOST_DELTA_LIST_INLINE);

	return (HostDeltaListPages)
	{
		hold, root, depth, entries, depth, 0
	};
}

/* host_delta_list_Pages_on_level returns the number of the list's pages at a level. */
static uint64
host_delta_list_Pages_on_level(const HostDeltaListPages *self, uint64 level)
{
	if (level == self->depth)
		return 1;
	return std_div_ceil_u64(self->entries, host_delta_list_capacity(level), RUST_FILE, 596, 58);
}

/* host_delta_list_Pages_name returns the name of a page at a level, found from the root by its first entry's path. */
static bool
host_delta_list_Pages_name(const HostDeltaListPages *self, uint64 level, uint64 index, HostDeltaPagePage *result,
						   HostError *error)
{
	uint64		entry = STD_MUL(u64, index, host_delta_list_capacity(level), 601, 21);
	uint64		name = self->root;
	uint64		above_end = STD_ADD(u64, level, 1, 603, 23);
	uint64		above = self->depth;
	bool		above_done = above_end > above;

	for (; !above_done; above_done = (above == above_end), above -= !above_done)
	{
		HostDeltaPageRef page;
		HostDeltaWord word;

		if (!host_delta_Hold_page(self->hold, host_delta_page_Page_named(name), &page, error))
			return false;
		if (!host_delta_PageRef_word(&page, host_delta_list_digit(entry, above), &word, error))
			return false;
		name = host_delta_Word_load(&word);
	}
	*result = host_delta_page_Page_named(name);
	return true;
}

/*
 * host_delta_list_Pages_Iterator_next names the next page, a
 * HostDeltaRetireItem, ending after an error; a panic past the name drops it.
 */
bool
host_delta_list_Pages_Iterator_next(void *self, void *result)
{
	HostDeltaListPages *pages = (HostDeltaListPages *) self;
	HostDeltaRetireItem *named = (HostDeltaRetireItem *) result;
	StdUnwind	unwind;

	if (pages->level == 0)
		return false;
	named->is_ok = host_delta_list_Pages_name(pages, pages->level, pages->next, &named->ok, &named->err);
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		pages->next = STD_ADD(u64, pages->next, 1, 618, 9);
		if (pages->next == host_delta_list_Pages_on_level(pages, pages->level))
		{
			pages->level = STD_SUB(u64, pages->level, 1, 620, 13);
			pages->next = 0;
		}
		std_unwind_pop(&unwind);
	}
	else
	{
		if (!named->is_ok)
			host_Error_drop(&named->err);
		std_unwind_resume(&unwind);
	}
	if (!named->is_ok)
		pages->level = 0;
	return true;
}

/* HOST_DELTA_LIST_PAGES_ITERATOR_OPS is Pages' table of Iterator; its drop does nothing. */
const StdIteratorOps HOST_DELTA_LIST_PAGES_ITERATOR_OPS = {host_delta_list_Pages_Iterator_next, std_Iterator_size_hint, NULL};
