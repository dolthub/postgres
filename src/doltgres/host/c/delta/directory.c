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
 * The delta's page directory as a host keeps it (design item 14's S6): what
 * each host gives of its slots, its header, its bodies, and the epochs, and
 * the sequences over them, an allocation, an open, an append, a seal, a
 * link, a retirement's batch, and the free pass, written once so the
 * server's host and the simulation's can't differ in them.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/delta.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/test.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"

#define RUST_FILE "host" STD_SEP "src" STD_SEP "delta" STD_SEP "directory.rs"

/* The items below that an earlier item reaches, declared ahead so the file keeps directory.rs's order. */
static bool host_delta_directory_take_all(HostDeltaDirectoryDirectory directory, const HostDeltaPageSize *wanted,
										  size_t wanted_len, StdVec *names, bool *result, HostError *error);
static bool host_delta_directory_take(HostDeltaDirectoryDirectory directory, HostDeltaPageSize size,
									  bool overshooting, bool *has_result, HostDeltaPagePage *result, HostError *error);
static bool host_delta_directory_claim(HostDeltaDirectoryDirectory directory, bool overshooting, bool *has_result,
									   uint32 *result_0, uint64 *result_1, HostError *error);
static void host_delta_directory_freed(HostDeltaDirectoryDirectory directory, uint32 slot, bool unlinked);
static uint64 host_delta_directory_emptied(HostDeltaDirectoryDirectory directory, uint32 slot, uint64 loaded);
static void host_delta_directory_hinted(HostDeltaDirectoryDirectory directory, uint32 slot, uint64 released);
static void host_delta_directory_release(HostDeltaDirectoryDirectory directory, HostDeltaPagePage name);
static bool host_delta_directory_claim_record(HostDeltaDirectoryDirectory directory, uint32 kind, const char *name,
											  size_t name_len, bool *has_result, uint32 *result_0,
											  uint64 *result_1, HostError *error);
static bool host_delta_directory_lowered(HostDeltaDirectoryDirectory directory, uint64 stamp);
static void host_delta_directory_held(HostDeltaDirectoryDirectory directory, uint64 current, uint64 oldest,
									  uint64 kept);
static bool host_delta_directory_raise(HostDeltaDirectoryDirectory directory, uint64 stamp);
static bool host_delta_directory_owns(HostDeltaDirectoryDirectory directory, uint32 slot,
									  HostDeltaDirectoryOwner owner, HostError *error);
static void host_delta_directory_freed_slot(HostDeltaDirectoryDirectory directory, uint32 slot, uint64 loaded,
											uint64 oldest, bool *has_kept, uint64 *kept, bool *keep);

/* host_delta_directory_internal returns the fork's internal error with a message. */
static HostError
host_delta_directory_internal(const char *message, size_t message_len)
{
	StdString	owned;

	std_string_from_Checked(&owned, message, message_len);
	return host_Error_fork((const uint8 *) "XX000", &owned);
}

/* host_delta_directory_sizes returns the word that holds a page's size. */
static uint64
host_delta_directory_sizes(HostDeltaPageSize size)
{
	return STD_SHL_FOLDED(u64, (uint64) size.words, 32) | (uint64) size.bytes;
}

/* host_delta_directory_sized returns the size a slot's word holds. */
static HostDeltaPageSize
host_delta_directory_sized(uint64 word)
{
	return (HostDeltaPageSize)
	{
		(uint32) word, (uint32) STD_SHR_FOLDED(u64, word, 32)
	};
}

/* host_delta_directory_bytes_at returns where a page's bytes start in its body: after its words. */
static uint64
host_delta_directory_bytes_at(HostDeltaPageSize size)
{
	return STD_MUL(u64, (uint64) size.words, (uint64) sizeof(uint64), 284, 5);
}

/* host_delta_directory_slot returns the slot a name names, which the name's number always fits. */
static uint32
host_delta_directory_slot(HostDeltaPagePage name)
{
	return (uint32) host_delta_page_Page_number(name);
}

/*
 * host_delta_directory_ordinary returns the count of the directory's slots
 * below the block past the budget's, which the free hint covers.
 */
static uint32
host_delta_directory_ordinary(HostDeltaDirectoryDirectory directory)
{
	return STD_SUB(u32, directory.ops->slots(directory.self),
				   (uint32) (HOST_DELTA_PAGE_BLOCK / HOST_DELTA_PAGE_SLOT_BYTES), 297, 5);
}

/* The block past the budget's slots are whole words of the free hint. */
StaticAssertDecl(HOST_DELTA_PAGE_BLOCK / HOST_DELTA_PAGE_SLOT_BYTES == (uint64) HOST_DELTA_DIRECTORY_HINTED,
				 "the block past the budget's slots are whole words of the free hint");

/*
 * host_delta_directory_alloc takes a page of each size, all of them or none,
 * listing their names. It answers nothing when it took them, and the count
 * of frees it loaded before it tried when the budget or the slots refused
 * them even after the free pass, which is the budget's signal. Its
 * caller reserved the room for the names, here and in the process's own list.
 */
bool
host_delta_directory_alloc(HostDeltaDirectoryDirectory directory, const void *raise,
						   const HostDeltaPageSize *wanted, size_t wanted_len, StdVec *names,
						   bool *has_result, uint64 *result, HostError *error)
{
	uint64		budget = directory.ops->budget(directory.self);

	for (size_t i = 0; i < wanted_len; i++)
	{
		if (!host_delta_page_Size_allocates(wanted[i], budget))
		{
			static const char text[] = "a page of the delta has no body, or one no empty delta could give";

			*error = host_delta_directory_internal(text, sizeof(text) - 1);
			return false;
		}
	}
	for (;;)
	{
		uint64		frees = directory.ops->frees(directory.self);
		bool		took;
		bool		ok;
		volatile HostError g_error;
		volatile bool g_error_live = false;
		StdUnwind	unwind;

		if (!directory.ops->lock(directory.self, raise, error))
			return false;
		ok = host_delta_directory_take_all(directory, wanted, wanted_len, names, &took, (HostError *) &g_error);
		g_error_live = !ok;
		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
		{
			directory.ops->unlock(directory.self);
			std_unwind_pop(&unwind);
		}
		else
		{
			if (g_error_live)
				host_Error_drop((HostError *) &g_error);
			std_unwind_resume(&unwind);
		}
		if (!ok)
		{
			*error = *(HostError *) &g_error;
			return false;
		}
		if (took)
		{
			*has_result = false;
			return true;
		}
		host_delta_directory_pass(directory);
		if (directory.ops->frees(directory.self) == frees)
		{
			*has_result = true;
			*result = frees;
			return true;
		}
	}
}

/*
 * host_delta_directory_alloc_replacing takes a page of a size for the copy of
 * a page it replaces, a compaction's: as alloc, and, when the budget refuses
 * it, past the budget while no other replacement is, taking the overshoot word
 * for the replaced page and then its copy, which a free of the replaced page,
 * or a release of either unlinked, clears (bugs row 110). The copy is no
 * larger than the page it replaces, so the delta is never more than one page
 * past its budget. It answers as alloc does.
 */
bool
host_delta_directory_alloc_replacing(HostDeltaDirectoryDirectory directory, const void *raise, HostDeltaPageSize size,
									 HostDeltaPagePage replaced, StdVec *names, bool *has_result, uint64 *result,
									 HostError *error)
{
	HostDeltaPageSize wanted[1];
	bool		has_frees;
	uint64		frees;
	uint64		word;
	bool		ok;
	bool		has_taken;
	HostDeltaPagePage name;
	volatile HostError g_error;
	volatile bool g_error_live = false;
	StdUnwind	unwind;

	wanted[0] = size;
	if (!host_delta_directory_alloc(directory, raise, wanted, lengthof(wanted), names, &has_frees, &frees, error))
		return false;
	if (!has_frees)
	{
		*has_result = false;
		return true;
	}
	word = STD_ADD(u64, (uint64) host_delta_directory_slot(replaced), 1, 348, 16);
	if (!directory.ops->overshoot_swap(directory.self, 0, word))
	{
		*has_result = true;
		*result = frees;
		return true;
	}
	if (!directory.ops->lock(directory.self, raise, error))
		return false;
	ok = host_delta_directory_take(directory, size, true, &has_taken, &name, (HostError *) &g_error);
	g_error_live = !ok;
	STD_UNWIND_PUSH(&unwind);
	if (sigsetjmp(unwind.jump, 0) == 0)
	{
		directory.ops->unlock(directory.self);
		std_unwind_pop(&unwind);
	}
	else
	{
		if (g_error_live)
			host_Error_drop((HostError *) &g_error);
		std_unwind_resume(&unwind);
	}
	if (ok && has_taken)
	{
		uint64		copy = STD_ADD(u64, (uint64) host_delta_directory_slot(name), 1, 357, 51);

		directory.ops->overshoot_swap(directory.self, word, word | STD_SHL_FOLDED(u64, copy, 32));
		std_vec_push_Checked(names, &name, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
		*has_result = false;
		return true;
	}
	directory.ops->overshoot_swap(directory.self, word, 0);
	if (!ok)
	{
		*error = *(HostError *) &g_error;
		return false;
	}
	*has_result = true;
	*result = frees;
	return true;
}

/*
 * host_delta_directory_freed clears the overshoot word when a freed slot held
 * the page a replacement past the budget replaced, or, released unlinked, its
 * copy (bugs row 110; orev-18 S1).
 */
static void
host_delta_directory_freed(HostDeltaDirectoryDirectory directory, uint32 slot, bool unlinked)
{
	uint64		over = directory.ops->overshoot(directory.self);
	uint64		named = STD_ADD(u64, (uint64) slot, 1, 372, 17);

	if ((over & (uint64) PG_UINT32_MAX) == named || (unlinked && STD_SHR_FOLDED(u64, over, 32) == named))
		directory.ops->overshoot_swap(directory.self, over, 0);
}

/*
 * host_delta_directory_take_all takes a page of each size under the
 * allocation lock, reporting whether it took them all; when the budget or the
 * slots refuse one, it gives back what it took.
 */
static bool
host_delta_directory_take_all(HostDeltaDirectoryDirectory directory, const HostDeltaPageSize *wanted, size_t wanted_len,
							  StdVec *names, bool *result, HostError *error)
{
	for (size_t i = 0; i < wanted_len; i++)
	{
		HostDeltaPageSize size = wanted[i];
		bool		has_tmp_1;
		HostDeltaPagePage tmp_1;
		volatile HostError g_error;
		bool		ok = host_delta_directory_take(directory, size, false, &has_tmp_1, &tmp_1, (HostError *) &g_error);

		if (ok && has_tmp_1)
		{
			HostDeltaPagePage name = tmp_1;

			std_vec_push_Checked(names, &name, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
		}
		else
		{
			volatile bool g_error_live = !ok;
			volatile StdVecDrain tmp_2;
			volatile bool tmp_2_live = false;
			StdUnwind	unwind;

			STD_UNWIND_PUSH(&unwind);
			if (sigsetjmp(unwind.jump, 0) == 0)
			{
				HostDeltaPagePage name;

				*(StdVecDrain *) &tmp_2 = std_vec_drain_full(names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage), NULL);
				tmp_2_live = true;
				while (std_VecDrain_Iterator_next((StdVecDrain *) &tmp_2, &name))
					host_delta_directory_release(directory, name);
				tmp_2_live = false;
				std_VecDrain_drop((StdVecDrain *) &tmp_2);
				std_unwind_pop(&unwind);
			}
			else
			{
				if (tmp_2_live)
				{
					tmp_2_live = false;
					std_VecDrain_drop((StdVecDrain *) &tmp_2);
				}
				if (g_error_live)
					host_Error_drop((HostError *) &g_error);
				std_unwind_resume(&unwind);
			}
			if (!ok)
			{
				*error = *(HostError *) &g_error;
				return false;
			}
			*result = false;
			return true;
		}
	}
	*result = true;
	return true;
}

/*
 * host_delta_directory_take claims a free slot for a page of a size, lists it
 * as the process's own, and takes its body from the region of blocks,
 * answering nothing when no slot is free or the budget refuses the body, with
 * the slot given back; only a replacement past the budget, overshooting, may
 * take the slots and the block the region keeps past the budget.
 */
static bool
host_delta_directory_take(HostDeltaDirectoryDirectory directory, HostDeltaPageSize size, bool overshooting,
						  bool *has_result, HostDeltaPagePage *result, HostError *error)
{
	bool		has_claimed;
	uint32		slot;
	uint64		word;
	HostDeltaPagePage name;
	static const HostDeltaDirectoryField fields[] = {
		HOST_DELTA_DIRECTORY_FIELD_BODY, HOST_DELTA_DIRECTORY_FIELD_SIZES,
		HOST_DELTA_DIRECTORY_FIELD_MARK, HOST_DELTA_DIRECTORY_FIELD_LOCK,
	};
	uint64		values[4];
	uint64		body;

	if (!host_delta_directory_claim(directory, overshooting, &has_claimed, &slot, &word, error))
		return false;
	if (!has_claimed)
	{
		*has_result = false;
		return true;
	}
	name = host_delta_page_Page_of(slot, word);
	values[0] = 0;
	values[1] = host_delta_directory_sizes(size);
	values[2] = 0;
	values[3] = 0;
	for (size_t i = 0; i < lengthof(fields); i++)
		directory.ops->store(directory.self, slot, fields[i], values[i]);
	directory.ops->own(directory.self, name);
	if (host_delta_blocks_take(directory, size, overshooting, &body))
	{
		directory.ops->store(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_BODY, body);
		*has_result = true;
		*result = name;
		return true;
	}
	host_delta_directory_release(directory, name);
	*has_result = false;
	return true;
}

/*
 * host_delta_directory_claim_closure_0 is claim's taken: a slot's word, and
 * the slot claimed by its word's compare-and-swap when the word is free,
 * answering the owned word. It captures the directory.
 */
static bool
host_delta_directory_claim_closure_0(void *captures, uint32 slot, uint64 *result)
{
	const HostDeltaDirectoryDirectory *directory = (const HostDeltaDirectoryDirectory *) captures;
	uint64		loaded = directory->ops->word(directory->self, slot);
	uint64		owned;

	if (!host_delta_page_claim(loaded, &owned))
		return false;
	if (!directory->ops->swap(directory->self, slot, loaded, owned))
		return false;
	*result = owned;
	return true;
}

/*
 * host_delta_directory_claim claims a free slot, under the allocation lock,
 * returning it with its word, or nothing when none is free. The last block's
 * worth of slots are the block past the budget's (orev-21 R1): an ordinary
 * claim takes only the slots below them, which hold every page the budget's
 * blocks can, and a replacement past the budget sweeps them first, a word at
 * a time, and then the rest, which hold every copy the block past the budget
 * can, since each replaces a page. The slots below are found by the free hint
 * (bugs row 150): from the word the hand names, around, a set bit's slot is
 * taken by its word's compare-and-swap and then its bit cleared, and a word
 * with no bit set is skipped. A directory whose every slot a claim may take
 * is spent refuses with 53200, since no free or spill would end a wait for
 * it.
 */
static bool
host_delta_directory_claim(HostDeltaDirectoryDirectory directory, bool overshooting, bool *has_result, uint32 *result_0,
						   uint64 *result_1, HostError *error)
{
	uint32		slots = directory.ops->slots(directory.self);
	uint32		ordinary = host_delta_directory_ordinary(directory);
	uint32		words;
	uint32		start;
	bool		past;

	if (overshooting)
	{
		for (uint32 slot = ordinary; slot < slots; slot++)
		{
			uint64		owned;

			if (host_delta_directory_claim_closure_0(&directory, slot, &owned))
			{
				*has_result = true;
				*result_0 = slot;
				*result_1 = owned;
				return true;
			}
		}
	}
	words = STD_DIV_FOLDED(u32, ordinary, HOST_DELTA_DIRECTORY_HINTED);
	start = directory.ops->hand(directory.self);
	for (uint32 step = 0; step < words; step++)
	{
		uint64		tmp_1 = STD_ADD(u64, (uint64) start, (uint64) step, 435, 21);
		uint32		word = (uint32) STD_REM(u64, tmp_1, (uint64) words, 435, 20);
		uint64		bits = directory.ops->hint(directory.self, word);

		while (bits != 0)
		{
			uint32		tmp_2 = STD_MUL(u32, word, HOST_DELTA_DIRECTORY_HINTED, 438, 24);
			uint32		slot = STD_ADD(u32, tmp_2, std_trailing_zeros_u64(bits), 438, 24);
			uint64		owned;

			bits &= STD_SUB(u64, bits, 1, 439, 21);
			if (host_delta_directory_claim_closure_0(&directory, slot, &owned))
			{
				directory.ops->hint_clear(directory.self, slot);
				*has_result = true;
				*result_0 = slot;
				*result_1 = owned;
				return true;
			}
		}
	}
	past = !overshooting ||
		directory.ops->spent(directory.self, true) == (uint64) STD_SUB(u32, slots, ordinary, 446, 68);
	if (directory.ops->spent(directory.self, false) == (uint64) ordinary && past)
	{
		static const char message[] = "out of shared memory";
		static const char hint[] = "The delta's page directory has spent every slot; restart the server.";

		*error = host_Error_hinted((const uint8 *) "53200", message, sizeof(message) - 1, hint, sizeof(hint) - 1);
		return false;
	}
	*has_result = false;
	return true;
}

/*
 * host_delta_directory_emptied stores the word of a slot whose page is gone
 * over the one loaded, free at its next generation or spent at its last, and
 * answers it for hinted.
 */
static uint64
host_delta_directory_emptied(HostDeltaDirectoryDirectory directory, uint32 slot, uint64 loaded)
{
	uint64		released = host_delta_page_released(loaded);

	directory.ops->swap(directory.self, slot, loaded, released);
	return released;
}

/*
 * host_delta_directory_hinted follows a slot's emptied word: a free slot
 * below the block past the budget gets its bit of the free hint, after the
 * word, and a spent one is counted spent and never in the free hint again
 * (bugs row 150, H1). Its caller counts any free after, so a waiter woken by
 * the count finds the bit set (H3).
 */
static void
host_delta_directory_hinted(HostDeltaDirectoryDirectory directory, uint32 slot, uint64 released)
{
	bool		past = slot >= host_delta_directory_ordinary(directory);

	if (host_delta_page_state(released) == HOST_DELTA_PAGE_STATE_SPENT)
		directory.ops->spend(directory.self, past);
	else if (!past)
		directory.ops->hint_set(directory.self, slot);
}

/*
 * host_delta_directory_release gives an owned page's body back to its block,
 * frees its slot at the next generation, and takes it off the process's list.
 */
static void
host_delta_directory_release(HostDeltaDirectoryDirectory directory, HostDeltaPagePage name)
{
	uint32		slot = host_delta_directory_slot(name);
	uint64		body = directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_BODY);
	uint64		released;

	if (body != 0)
	{
		host_delta_blocks_give(directory, body);
		directory.ops->store(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_BODY, 0);
	}
	released = host_delta_directory_emptied(directory, slot, directory.ops->word(directory.self, slot));
	host_delta_directory_hinted(directory, slot, released);
	host_delta_directory_freed(directory, slot, true);
	directory.ops->disown(directory.self, name);
}

/* host_delta_directory_give_back gives back a page its owner dropped unlinked, or left at its exit, which counts as a free. */
void
host_delta_directory_give_back(HostDeltaDirectoryDirectory directory, HostDeltaPagePage name)
{
	host_delta_directory_release(directory, name);
	directory.ops->freed(directory.self);
}

/*
 * host_delta_directory_fill copies a record above a page's filled mark and
 * then advances the mark, returning the record's offset, or refuses a record
 * that doesn't fit, changing nothing.
 */
static bool
host_delta_directory_fill(HostDeltaDirectoryDirectory directory, uint32 slot, uint64 body, HostDeltaPageSize size,
						  const uint8 *record, size_t record_len, uint32 *result, HostError *error)
{
	uint64		mark = directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_MARK);
	uint64		filled;

	if (!host_delta_page_appended(mark, (uint64) record_len, (uint64) size.bytes, &filled))
	{
		static const char text[] = "a record doesn't fit the page it is appended to";

		*error = host_delta_directory_internal(text, sizeof(text) - 1);
		return false;
	}
	directory.ops->write(directory.self, body, STD_ADD(u64, host_delta_directory_bytes_at(size), mark, 504, 27), record,
						 record_len);
	directory.ops->store(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_MARK, filled);
	*result = (uint32) mark;
	return true;
}

/* host_delta_directory_owned_append appends a record to a page its owner hasn't linked, returning the record's offset. */
bool
host_delta_directory_owned_append(HostDeltaDirectoryDirectory directory, HostDeltaPagePage name,
								  const uint8 *record, size_t record_len, uint32 *result,
								  HostError *error)
{
	uint32		slot = host_delta_directory_slot(name);
	HostDeltaPageSize size = host_delta_directory_sized(directory.ops->load(directory.self, slot,
																			HOST_DELTA_DIRECTORY_FIELD_SIZES));

	return host_delta_directory_fill(directory, slot,
									 directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_BODY), size,
									 record, record_len, result, error);
}

/* host_delta_directory_owned_store stores a word of a page its owner hasn't linked, refusing an index past the page's words. */
bool
host_delta_directory_owned_store(HostDeltaDirectoryDirectory directory, HostDeltaPagePage name,
								 uint64 index, uint64 value, HostError *error)
{
	uint32		slot = host_delta_directory_slot(name);

	if (!host_delta_directory_worded(host_delta_directory_sized(directory.ops->load(directory.self, slot,
																					HOST_DELTA_DIRECTORY_FIELD_SIZES)),
									 index, error))
		return false;
	directory.ops->set(directory.self, directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_BODY), index,
					   value);
	return true;
}

/*
 * host_delta_directory_make claims a free record of an aged kind for the
 * running process and clears its parts, sweeping the kind's pool once from
 * its hand; when none is free it runs the free pass and sweeps again if the
 * count of frees moved. It answers the record's index and location, or, when
 * none was free, the count of frees it loaded before it looked, which is the
 * budget's signal. A pool whose every slot is spent is refused, since no free
 * would end a wait for it. Its caller reserved the room in the process's list.
 */
bool
host_delta_directory_make(HostDeltaDirectoryDirectory directory, uint32 kind, const char *name,
						  size_t name_len, bool *result_ok, uint32 *result_0, uint64 *result_1,
						  uint64 *result_err, HostError *error)
{
	for (;;)
	{
		uint64		frees = directory.ops->frees(directory.self);
		bool		has_claimed;
		uint32		index;
		uint64		word;

		if (!host_delta_directory_claim_record(directory, kind, name, name_len, &has_claimed, &index, &word, error))
			return false;
		if (has_claimed)
		{
			directory.ops->clear(directory.self, kind, index);
			directory.ops->made(directory.self, kind, index);
			*result_ok = true;
			*result_0 = index;
			*result_1 = host_delta_page_Page_value(host_delta_page_Page_of(index, word));
			return true;
		}
		host_delta_directory_pass(directory);
		if (directory.ops->frees(directory.self) == frees)
		{
			*result_ok = false;
			*result_err = frees;
			return true;
		}
	}
}

/*
 * host_delta_directory_claim_record sweeps an aged kind's pool once from its
 * hand and claims the first free record, returning it with its word, or
 * nothing when none is free.
 */
static bool
host_delta_directory_claim_record(HostDeltaDirectoryDirectory directory, uint32 kind, const char *name, size_t name_len,
								  bool *has_result, uint32 *result_0, uint64 *result_1, HostError *error)
{
	uint32		count = directory.ops->records(directory.self, kind);
	uint32		start = directory.ops->record_hand(directory.self, kind);
	uint32		spent = 0;

	for (uint32 step = 0; step < count; step++)
	{
		uint32		index = (uint32) STD_REM(u64, STD_ADD(u64, (uint64) start, (uint64) step, 550, 22),
											 (uint64) count, 550, 21);
		uint64		loaded = directory.ops->record_word(directory.self, kind, index);
		uint64		owned;

		if (host_delta_page_claim(loaded, &owned))
		{
			if (directory.ops->record_swap(directory.self, kind, index, loaded, owned))
			{
				*has_result = true;
				*result_0 = index;
				*result_1 = owned;
				return true;
			}
		}
		else
			spent = STD_ADD(u32, spent, host_delta_page_state(loaded) == HOST_DELTA_PAGE_STATE_SPENT ? 1 : 0, 555, 21);
	}
	if (spent == count)
	{
		static const StdFmtPiece pieces[] = {
			STD_FMT_LIT("every slot of the delta's "), STD_FMT_ARG(0),
			STD_FMT_LIT(" records is spent; restart the server"),
		};
		StdStr		name_str = {name, name_len};
		StdFmtArg	args[] = {{&name_str, std_str_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		volatile StdString tmp_1;
		volatile bool tmp_1_live = false;
		HostError	internal;
		StdUnwind	unwind;

		std_fmt_format_Checked((StdString *) &tmp_1, &arguments);
		tmp_1_live = true;
		STD_UNWIND_PUSH(&unwind);
		if (sigsetjmp(unwind.jump, 0) == 0)
		{
			internal = host_delta_directory_internal(((StdString *) &tmp_1)->ptr, ((StdString *) &tmp_1)->len);
			tmp_1_live = false;
			std_vec_free_Checked((StdString *) &tmp_1, 1, 1);
			std_unwind_pop(&unwind);
		}
		else
		{
			if (tmp_1_live)
				std_vec_free_Checked((StdString *) &tmp_1, 1, 1);
			std_unwind_resume(&unwind);
		}
		*error = internal;
		return false;
	}
	*has_result = false;
	return true;
}

/* host_delta_directory_unmake gives back a record its maker dropped unlinked, or left at its exit, which counts as a free. */
void
host_delta_directory_unmake(HostDeltaDirectoryDirectory directory, uint32 kind, uint32 index)
{
	uint64		word = directory.ops->record_word(directory.self, kind, index);

	directory.ops->record_swap(directory.self, kind, index, word, host_delta_page_released(word));
	directory.ops->unmade(directory.self, kind, index);
	directory.ops->freed(directory.self);
}

/* host_delta_directory_publish makes a record its maker owns published, off the process's list. */
void
host_delta_directory_publish(HostDeltaDirectoryDirectory directory, uint32 kind, uint32 index)
{
	uint64		word = directory.ops->record_word(directory.self, kind, index);

	directory.ops->record_swap(directory.self, kind, index, word, host_delta_page_link(word));
	directory.ops->unmade(directory.self, kind, index);
}

/*
 * host_delta_directory_open_record reaches the record of an aged kind a
 * location names under a hold begun at an epoch, as open reaches a page: it
 * loads the record's word, then its stamp, then its word again, and refuses
 * an index past the pool and whatever page::admits refuses of those loads.
 * It returns the record's index.
 */
bool
host_delta_directory_open_record(HostDeltaDirectoryDirectory directory, uint32 kind, uint64 location,
								 uint64 epoch, uint32 *result, HostError *error)
{
	HostDeltaPagePage name = host_delta_page_Page_named(location);
	uint32		index;
	uint64		before;
	uint64		stamp;
	uint64		after;

	{
		uint64		number = host_delta_page_Page_number(name);
		uint32		records = directory.ops->records(directory.self, kind);

		if (number >= (uint64) records)
		{
			static const char text[] = "a location of the delta names no record its hold reaches";

			*error = host_delta_directory_internal(text, sizeof(text) - 1);
			return false;
		}
	}
	index = (uint32) host_delta_page_Page_number(name);
	before = directory.ops->record_word(directory.self, kind, index);
	directory.ops->stop(directory.self, HOST_DELTA_DIRECTORY_POINT_OPENING);
	stamp = directory.ops->stamp(directory.self, kind, index);
	after = directory.ops->record_word(directory.self, kind, index);
	{
		HostDeltaPageSeen first = host_delta_page_seen(before);
		HostDeltaPageSeen second = host_delta_page_seen(after);
		uint64		generation = host_delta_page_Page_generation(name);

		if (!host_delta_page_admits(first, stamp, second, generation, epoch))
		{
			static const char text[] = "a location of the delta names no record its hold reaches";

			*error = host_delta_directory_internal(text, sizeof(text) - 1);
			return false;
		}
	}
	*result = index;
	return true;
}

/* host_delta_directory_link records the lock that may append to an owned page and makes the page live, off the process's list. */
void
host_delta_directory_link(HostDeltaDirectoryDirectory directory, HostDeltaPagePage name,
						  HostDeltaDirectoryOwner owner)
{
	uint32		slot = host_delta_directory_slot(name);
	uint64		word;

	directory.ops->store(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_SHAPE, owner.shape);
	directory.ops->store(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_OWNER, owner.location);
	directory.ops->store(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_LOCK, STD_ADD(u64, owner.lock, 1, 603, 40));
	word = directory.ops->word(directory.self, slot);
	directory.ops->swap(directory.self, slot, word, host_delta_page_link(word));
	directory.ops->disown(directory.self, name);
}

/*
 * host_delta_directory_open_closure_0 is open's refused: the error of a name
 * that names no page its hold reaches. It captures nothing.
 */
static HostError
host_delta_directory_open_closure_0(void *captures)
{
	static const char text[] = "a name of the delta names no page its hold reaches";

	return host_delta_directory_internal(text, sizeof(text) - 1);
}

/*
 * host_delta_directory_open reaches the page a name names under a hold begun
 * at an epoch: it loads the slot's word, then its stamp and its other words,
 * then its word again, and refuses a number past the directory and whatever
 * page::admits refuses of those loads.
 */
bool
host_delta_directory_open(HostDeltaDirectoryDirectory directory, HostDeltaPagePage name,
						  uint64 epoch, HostDeltaDirectoryOpened *result, HostError *error)
{
	uint32		slot;
	uint64		before;
	uint64		stamp;
	uint64		body;
	HostDeltaPageSize size;
	uint64		after;

	{
		uint64		number = host_delta_page_Page_number(name);
		uint32		slots = directory.ops->slots(directory.self);

		if (number >= (uint64) slots)
		{
			*error = host_delta_directory_open_closure_0(NULL);
			return false;
		}
	}
	slot = host_delta_directory_slot(name);
	before = directory.ops->word(directory.self, slot);
	directory.ops->stop(directory.self, HOST_DELTA_DIRECTORY_POINT_OPENING);
	stamp = directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_STAMP);
	body = directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_BODY);
	size = host_delta_directory_sized(directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_SIZES));
	after = directory.ops->word(directory.self, slot);
	{
		HostDeltaPageSeen first = host_delta_page_seen(before);
		HostDeltaPageSeen second = host_delta_page_seen(after);
		uint64		generation = host_delta_page_Page_generation(name);

		if (!host_delta_page_admits(first, stamp, second, generation, epoch))
		{
			*error = host_delta_directory_open_closure_0(NULL);
			return false;
		}
	}
	*result = (HostDeltaDirectoryOpened)
	{
		slot, body, size
	};
	return true;
}

/*
 * host_delta_directory_readable checks a read of a length at an offset
 * against a page's filled mark, loaded now, and returns where the read starts
 * in the page's body.
 */
bool
host_delta_directory_readable(HostDeltaDirectoryDirectory directory,
							  const HostDeltaDirectoryOpened *opened, uint64 offset, uint64 len,
							  uint64 *result, HostError *error)
{
	if (!host_delta_page_lends(directory.ops->load(directory.self, opened->slot, HOST_DELTA_DIRECTORY_FIELD_MARK), offset,
							   len))
	{
		static const char text[] = "a read of a page of the delta ends past its filled mark";

		*error = host_delta_directory_internal(text, sizeof(text) - 1);
		return false;
	}
	*result = STD_ADD(u64, host_delta_directory_bytes_at(opened->size), offset, 636, 8);
	return true;
}

/*
 * host_delta_directory_room returns how many bytes an opened page takes above
 * its filled mark, which never passes its capacity, and none for a sealed
 * page, which takes no append.
 */
uint64
host_delta_directory_room(HostDeltaDirectoryDirectory directory,
						  const HostDeltaDirectoryOpened *opened)
{
	uint64		mark;

	if (directory.ops->load(directory.self, opened->slot, HOST_DELTA_DIRECTORY_FIELD_LOCK) == 0)
		return 0;
	mark = directory.ops->load(directory.self, opened->slot, HOST_DELTA_DIRECTORY_FIELD_MARK);
	return STD_SUB(u64, (uint64) opened->size.bytes, mark, 645, 5);
}

/* host_delta_directory_filled returns how many bytes an opened page holds below its filled mark. */
uint64
host_delta_directory_filled(HostDeltaDirectoryDirectory directory,
							const HostDeltaDirectoryOpened *opened)
{
	return directory.ops->load(directory.self, opened->slot, HOST_DELTA_DIRECTORY_FIELD_MARK);
}

/* host_delta_directory_worded checks a word's index against a page's count of words. */
bool
host_delta_directory_worded(HostDeltaPageSize size, uint64 index, HostError *error)
{
	if (index >= (uint64) size.words)
	{
		static const char text[] = "a word of a page of the delta is past the page's words";

		*error = host_delta_directory_internal(text, sizeof(text) - 1);
		return false;
	}
	return true;
}

/*
 * host_delta_directory_owns checks that a lock is the one a page's slot
 * records, which no lock is once the page is sealed: the recorded words and
 * the owner's are each made whole before they are compared, as Rust's tuples
 * are.
 */
static bool
host_delta_directory_owns(HostDeltaDirectoryDirectory directory, uint32 slot, HostDeltaDirectoryOwner owner,
						  HostError *error)
{
	uint64		shape = directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_SHAPE);
	uint64		location = directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_OWNER);
	uint64		lock = directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_LOCK);
	uint64		owner_lock = STD_ADD(u64, owner.lock, 1, 665, 50);

	if (!(shape == owner.shape && location == owner.location && lock == owner_lock))
	{
		static const char text[] = "a page of the delta takes an append or a seal only under the lock that owns it";

		*error = host_delta_directory_internal(text, sizeof(text) - 1);
		return false;
	}
	return true;
}

/*
 * host_delta_directory_append appends a record to an opened page under the
 * exclusive guard of the lock its slot records, returning the record's
 * offset.
 */
bool
host_delta_directory_append(HostDeltaDirectoryDirectory directory,
							const HostDeltaDirectoryOpened *opened, HostDeltaDirectoryOwner owner,
							const uint8 *record, size_t record_len, uint32 *result,
							HostError *error)
{
	if (!host_delta_directory_owns(directory, opened->slot, owner, error))
		return false;
	return host_delta_directory_fill(directory, opened->slot, opened->body, opened->size, record, record_len, result,
									 error);
}

/*
 * host_delta_directory_seal clears an opened page's owner under the exclusive
 * guard of the lock its slot records, after which no append to it is taken.
 * A seal of a page already sealed does nothing, since a split that fails
 * after its seal leaves its range's own pages sealed and the next split of
 * the range seals them again; a seal of an unsealed page under any guard but
 * its owner's is still refused.
 */
bool
host_delta_directory_seal(HostDeltaDirectoryDirectory directory,
						  const HostDeltaDirectoryOpened *opened, HostDeltaDirectoryOwner owner,
						  HostError *error)
{
	if (directory.ops->load(directory.self, opened->slot, HOST_DELTA_DIRECTORY_FIELD_LOCK) == 0)
		return true;
	if (!host_delta_directory_owns(directory, opened->slot, owner, error))
		return false;
	directory.ops->store(directory.self, opened->slot, HOST_DELTA_DIRECTORY_FIELD_LOCK, 0);
	return true;
}

/*
 * host_delta_directory_listed_record pushes an aged record its retirement
 * just retired onto its kind's retired list, by a compare-and-swap of the
 * list's head (bugs row 309).
 */
static void
host_delta_directory_listed_record(HostDeltaDirectoryDirectory directory, uint32 kind, uint32 index)
{
	for (;;)
	{
		uint64		head = directory.ops->record_retired_head(directory.self, kind);

		directory.ops->set_record_link(directory.self, kind, index, head);
		if (directory.ops->record_retired_swap(directory.self, kind, head, (uint64) index + 1))
			return;
	}
}

/*
 * host_delta_directory_listed pushes a slot its retirement just retired onto
 * the retired list, by a compare-and-swap of the list's head (bugs row 309).
 */
static void
host_delta_directory_listed(HostDeltaDirectoryDirectory directory, uint32 slot)
{
	for (;;)
	{
		uint64		head = directory.ops->retired_head(directory.self);

		directory.ops->set_link(directory.self, slot, (uint32) head);
		if (directory.ops->retired_swap(directory.self, head, (uint64) slot + 1))
			return;
	}
}

/*
 * host_delta_directory_retire retires one batch of records and names under
 * one take of the epoch lock: it stores the batch's one stamp and then the
 * retired state in each record that is still published at its location and
 * each page that is still live at its name, pushing each onto its retired
 * list, and then advances the epoch.
 */
void
host_delta_directory_retire(HostDeltaDirectoryDirectory directory,
							const struct HostDeltaLeaving *records, size_t records_len,
							const HostDeltaPagePage *names, size_t names_len)
{
	uint64		stamp = directory.ops->retiring(directory.self);

	for (size_t i = 0; i < records_len; i++)
	{
		const HostDeltaLeaving *leaving = &records[i];
		uint32		kind;
		HostDeltaPagePage name;
		uint32		index;
		uint64		loaded;
		uint64		retired;

		if (!directory.ops->aged_kind(directory.self, leaving->id, &kind))
			continue;
		name = host_delta_page_Page_named(leaving->location);
		{
			uint64		number = host_delta_page_Page_number(name);
			uint32		count = directory.ops->records(directory.self, kind);

			if (number >= (uint64) count)
				continue;
		}
		index = (uint32) host_delta_page_Page_number(name);
		loaded = directory.ops->record_word(directory.self, kind, index);
		if (host_delta_page_retire(loaded, name, &retired))
		{
			directory.ops->set_stamp(directory.self, kind, index, stamp);
			directory.ops->stop(directory.self, HOST_DELTA_DIRECTORY_POINT_STAMPING);
			if (directory.ops->record_swap(directory.self, kind, index, loaded, retired))
				host_delta_directory_listed_record(directory, kind, index);
		}
	}
	for (size_t i = 0; i < names_len; i++)
	{
		HostDeltaPagePage name = names[i];
		uint32		slot;
		uint64		loaded;
		uint64		retired;

		{
			uint64		number = host_delta_page_Page_number(name);
			uint32		slots = directory.ops->slots(directory.self);

			if (number >= (uint64) slots)
				continue;
		}
		slot = host_delta_directory_slot(name);
		loaded = directory.ops->word(directory.self, slot);
		if (host_delta_page_retire(loaded, name, &retired))
		{
			directory.ops->store(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_STAMP, stamp);
			directory.ops->stop(directory.self, HOST_DELTA_DIRECTORY_POINT_STAMPING);
			if (directory.ops->swap(directory.self, slot, loaded, retired))
				host_delta_directory_listed(directory, slot);
		}
	}
	directory.ops->stop(directory.self, HOST_DELTA_DIRECTORY_POINT_STAMPED);
	directory.ops->leave(directory.self);
	host_delta_directory_lowered(directory, stamp);
	directory.ops->retired(directory.self);
}

/*
 * host_delta_directory_freed_slot frees a retired slot whose stamp the oldest
 * epoch has passed, a page's body given back to its block first, counting the
 * free, or else sets keep and lowers the lowest stamp kept; a slot not
 * retired is neither. Its claim can't lose, since only a pass's claim takes a
 * word out of retired and passes run one at a time.
 */
static void
host_delta_directory_freed_slot(HostDeltaDirectoryDirectory directory, uint32 slot, uint64 loaded, uint64 oldest,
								bool *has_kept, uint64 *kept, bool *keep)
{
	uint64		freeing;
	uint64		stamp;
	uint64		released;
	bool		claimed PG_USED_FOR_ASSERTS_ONLY;

	*keep = false;
	if (!host_delta_page_reap(loaded, &freeing))
		return;
	stamp = directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_STAMP);
	if (!host_delta_page_passed(stamp, oldest))
	{
		*kept = *has_kept ? std_min_u64(*kept, stamp) : stamp;
		*has_kept = true;
		*keep = true;
		return;
	}
	claimed = directory.ops->swap(directory.self, slot, loaded, freeing);
	Assert(claimed);
	directory.ops->stop(directory.self, HOST_DELTA_DIRECTORY_POINT_CLAIMED);
	host_delta_blocks_give(directory, directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_BODY));
	directory.ops->store(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_BODY, 0);
	host_delta_directory_freed(directory, slot, false);
	released = host_delta_directory_emptied(directory, slot, freeing);
	directory.ops->stop(directory.self, HOST_DELTA_DIRECTORY_POINT_HINTING);
	host_delta_directory_hinted(directory, slot, released);
	directory.ops->freed(directory.self);
}

/*
 * host_delta_directory_pass is the free pass, one at a time under the pass's
 * lock: with the oldest published epoch loaded once, it takes each aged
 * kind's retired list whole, in the table's order, and then the slots', and
 * claims each retired record or slot whose stamp that epoch has passed and
 * frees it at its next generation, a page's body given back to its block
 * first, each of which counts as a free, pushing the ones it keeps back onto
 * their list as one chain, so its work is the retirements it finds and never
 * the directory's or a pool's size (bugs row 309). Its claims can't lose,
 * since only a pass's claim takes a word out of retired, the pool's doors
 * refusing an aged kind, and passes run one at a time. A pass that left no
 * retired record or slot unfreed clears the count of retirements it loaded,
 * unless a retirement since changed it. It loads the current epoch before the
 * oldest, and records the lowest stamp it kept (held).
 */
void
host_delta_directory_pass(HostDeltaDirectoryDirectory directory)
{
	uint64		left;
	bool		has_kept = false;
	uint64		kept = 0;
	uint64		current;
	uint64		oldest;

	directory.ops->lock_pass(directory.self);
	directory.ops->stop(directory.self, HOST_DELTA_DIRECTORY_POINT_PASSING);
	left = directory.ops->left(directory.self);
	current = directory.ops->epoch(directory.self);
	oldest = directory.ops->oldest(directory.self);
	directory.ops->stop(directory.self, HOST_DELTA_DIRECTORY_POINT_OLDEST);
	for (uint32 kind = 0, kind_len = directory.ops->aged(directory.self); kind < kind_len; kind++)
	{
		uint64		at = directory.ops->record_retired_head(directory.self, kind);
		uint64		first = 0;
		uint32		last = 0;

		while (!directory.ops->record_retired_swap(directory.self, kind, at, 0))
			at = directory.ops->record_retired_head(directory.self, kind);
		while (at != 0)
		{
			uint32		index = (uint32) (at - 1);
			uint64		loaded = directory.ops->record_word(directory.self, kind, index);
			uint64		freeing;
			uint64		stamp;
			bool		claimed PG_USED_FOR_ASSERTS_ONLY;

			at = directory.ops->record_link(directory.self, kind, index);
			if (!host_delta_page_reap(loaded, &freeing))
				continue;
			stamp = directory.ops->stamp(directory.self, kind, index);
			if (!host_delta_page_passed(stamp, oldest))
			{
				kept = has_kept ? std_min_u64(kept, stamp) : stamp;
				has_kept = true;
				if (first == 0)
					last = index;
				directory.ops->set_record_link(directory.self, kind, index, first);
				first = (uint64) index + 1;
				continue;
			}
			claimed = directory.ops->record_swap(directory.self, kind, index, loaded, freeing);
			Assert(claimed);
			directory.ops->record_swap(directory.self, kind, index, freeing, host_delta_page_released(freeing));
			directory.ops->freed(directory.self);
		}
		while (first != 0)
		{
			uint64		head = directory.ops->record_retired_head(directory.self, kind);

			directory.ops->set_record_link(directory.self, kind, last, head);
			if (directory.ops->record_retired_swap(directory.self, kind, head, first))
				break;
		}
	}
	directory.ops->stop(directory.self, HOST_DELTA_DIRECTORY_POINT_WALKED);
	{
		uint64		at = directory.ops->retired_head(directory.self);
		uint32		first = 0;
		uint32		last = 0;

		while (!directory.ops->retired_swap(directory.self, at, 0))
			at = directory.ops->retired_head(directory.self);
		while (at != 0)
		{
			uint32		slot = (uint32) (at - 1);
			bool		keep;

			at = directory.ops->link(directory.self, slot);
			host_delta_directory_freed_slot(directory, slot, directory.ops->word(directory.self, slot), oldest,
											&has_kept, &kept, &keep);
			if (!keep)
				continue;
			if (first == 0)
				last = slot;
			directory.ops->set_link(directory.self, slot, first);
			first = slot + 1;
		}
		while (first != 0)
		{
			uint64		head = directory.ops->retired_head(directory.self);

			directory.ops->set_link(directory.self, last, (uint32) head);
			if (directory.ops->retired_swap(directory.self, head, (uint64) first))
				break;
		}
	}
	if (!has_kept)
		directory.ops->settle(directory.self, left);
	host_delta_directory_held(directory, current, oldest, has_kept ? kept : 0);
	directory.ops->unlock_pass(directory.self);
}

/* host_delta_directory_rank orders kept stamps, zero, for none, after every stamp. */
static uint64
host_delta_directory_rank(uint64 stamp)
{
	if (stamp == 0)
		return PG_UINT64_MAX;
	else
		return stamp;
}

/*
 * host_delta_directory_lowered lowers the kept stamp to a stamp below it, by
 * a compare-exchange retried while the stored stamp is higher, and answers
 * whether it was above it: for a pass that kept a retirement, and for each
 * retirement under the epoch lock, since a retirement is unfreed when it is
 * stamped, so a hold begun at or before it wakes the budget's waiters at its
 * end, whatever pass runs first (PerfPF 11's first lost wake).
 */
static bool
host_delta_directory_lowered(HostDeltaDirectoryDirectory directory, uint64 stamp)
{
	for (;;)
	{
		uint64		stored = directory.ops->kept(directory.self);

		if (host_delta_directory_rank(stamp) >= host_delta_directory_rank(stored))
			return false;
		if (directory.ops->kept_swap(directory.self, stored, stamp))
			return true;
	}
}

/*
 * HOST_DELTA_DIRECTORY_EVERY is a kept stamp past every epoch, so every
 * hold's end wakes the budget's waiters, until a pass or a retirement lowers
 * it (bugs row 160).
 */
#define HOST_DELTA_DIRECTORY_EVERY (PG_UINT64_MAX - 1)

/*
 * host_delta_directory_held records the lowest stamp a free pass kept, zero
 * for none, as the kept stamp, which decides only which hold's end wakes the
 * budget's waiters (released) and never a free, since a pass frees a
 * retirement only once the oldest epoch has passed its own stamp (bugs row
 * 124, row 111's give-back text's item 1). A lower one lowers it at once
 * (lowered). A higher one raises it (raise), since every retirement the walk
 * missed is stamped at or after the epoch the pass loaded before the oldest,
 * so none unfreed is below the pass's lowest, and a stamp left below that
 * keeps the end of the hold that lets one be freed from waking the waiters
 * (bugs row 160). It raises only under the epoch lock held exclusively,
 * which it doesn't wait for: while a retirement holds it between its stores,
 * the pass leaves the stamp to that retirement's own pass. None clears it
 * only while the epoch is still the one the pass loaded, since then no
 * retirement began since and every one unfreed is in the walk; otherwise it
 * raises it to EVERY. A hold that ended between the pass's load of the oldest
 * epoch and its raise read the stamp before it, so the pass reloads the
 * oldest after a raise and, when a reader ended since, counts the wake that
 * hold's end didn't (PerfPF 11's second lost wake).
 */
static void
host_delta_directory_held(HostDeltaDirectoryDirectory directory, uint64 current, uint64 oldest, uint64 kept)
{
	uint64		epoch;
	bool		raised;

	if (host_delta_directory_lowered(directory, kept))
		return;
	if (!directory.ops->retiring_now(directory.self, &epoch))
		return;
	if (kept == 0 && epoch == current)
	{
		uint64		stored = directory.ops->kept(directory.self);

		raised = stored != 0 && directory.ops->kept_swap(directory.self, stored, 0);
	}
	else if (kept == 0)
		raised = host_delta_directory_raise(directory, HOST_DELTA_DIRECTORY_EVERY);
	else
		raised = host_delta_directory_raise(directory, kept);
	directory.ops->unretired(directory.self);
	if (raised && directory.ops->oldest(directory.self) > oldest)
		directory.ops->freed(directory.self);
}

/*
 * host_delta_directory_raise raises the kept stamp to a stamp above it, by a
 * compare-exchange retried while the stored stamp is lower, and answers
 * whether it was below it.
 */
static bool
host_delta_directory_raise(HostDeltaDirectoryDirectory directory, uint64 stamp)
{
	for (;;)
	{
		uint64		stored = directory.ops->kept(directory.self);

		if (host_delta_directory_rank(stamp) <= host_delta_directory_rank(stored))
			return false;
		if (directory.ops->kept_swap(directory.self, stored, stamp))
			return true;
	}
}

/*
 * host_delta_directory_released wakes the budget's waiters at the end of a
 * hold or a reader begun at an epoch, its epoch already released, while one
 * waits and a retirement may be unfreed (bugs row 72): a retirement under a
 * hold, the retirer's own or an older reader's, can't be freed by its own
 * pass, and a waiter wakes only when the count of frees moves, so the end of
 * a hold that may have kept one counts a wake as a free, and the waiter's
 * allocation then runs the free pass that frees what the released epoch let
 * go. Only a hold begun at or before the lowest stamp a pass kept may have
 * kept one, since the reader that keeps that retirement keeps every later one
 * too (bugs row 124); a later hold's end counts nothing, so no waiter wakes
 * to a pass that frees nothing. It allocates nothing and waits on nothing,
 * since a hold ends in a drop.
 */
void
host_delta_directory_released(HostDeltaDirectoryDirectory directory, uint64 epoch)
{
	uint64		kept = directory.ops->kept(directory.self);

	if (directory.ops->waiting(directory.self) > 0 && directory.ops->left(directory.self) != 0 && kept != 0 &&
		epoch <= kept)
		directory.ops->freed(directory.self);
}

/*
 * host_delta_directory_waiting counts a process refused at the budget among
 * its waiters before it sleeps, and then runs the free pass while a
 * retirement may be unfreed, so a hold that ended before the count, reading
 * no waiter, leaves the waiter no page its release let go; woken takes the
 * count back after the sleep.
 */
void
host_delta_directory_waiting(HostDeltaDirectoryDirectory directory)
{
	directory.ops->stop(directory.self, HOST_DELTA_DIRECTORY_POINT_WAITING);
	directory.ops->count_waiter(directory.self, true);
	if (directory.ops->left(directory.self) != 0)
		host_delta_directory_pass(directory);
}

/* host_delta_directory_woken takes a process off the budget's count of waiters after its sleep. */
void
host_delta_directory_woken(HostDeltaDirectoryDirectory directory)
{
	directory.ops->count_waiter(directory.self, false);
}

/*
 * host_delta_directory_packing orders the region's partly used blocks for a
 * packing and returns the live pages to move out of the blocks it drains,
 * each with its owner's shape and location and its size (bugs row 143). A
 * block holding a live page of a shape the caller doesn't move, or a page
 * still owned, which its writer is filling, is pinned, so the packing never
 * plans to drain a block it can't empty. A slot whose body reads 0, in a
 * take, release, or reap, pins and moves nothing (bugs row 486), since a
 * take's page counts in its block's map, and so in its drain's pages, from
 * host_delta_blocks_taken on, so a drain of its block only leaves it
 * unemptied; its loads of a slot's word and body are unordered, and a stale
 * body only pins a block or plans a move its owner's move checks again under
 * the owner's lock. It answers false when it can't reserve its lists.
 */
bool
host_delta_directory_packing(HostDeltaDirectoryDirectory directory, const uint64 *shapes, size_t shapes_len,
							 StdVec *result)
{
	uint32		blocks = directory.ops->blocks(directory.self);
	uint32		slots = directory.ops->slots(directory.self);
	StdVec		pinned;
	StdVec		left;
	StdVec		drains;
	uint8		zero = 0;
	uint32		none = 0;
	bool		ok;

	std_vec_new(result, sizeof(HostDeltaDirectoryPacked), _Alignof(HostDeltaDirectoryPacked));
	std_vec_new(&pinned, 1, 1);
	std_vec_new(&left, sizeof(uint32), _Alignof(uint32));
	if (!std_vec_try_reserve_exact(&pinned, blocks, 1, 1) ||
		!std_vec_try_reserve_exact(&left, blocks, sizeof(uint32), _Alignof(uint32)))
	{
		std_vec_free_Checked(&pinned, 1, 1);
		std_vec_free_Checked(&left, sizeof(uint32), _Alignof(uint32));
		return false;
	}
	std_vec_resize(&pinned, blocks, &zero, 1, 1);
	std_vec_resize(&left, blocks, &none, sizeof(uint32), _Alignof(uint32));
	for (uint32 slot = 0; slot < slots; slot++)
	{
		uint64		word = directory.ops->word(directory.self, slot);
		HostDeltaPageState state = host_delta_page_state(word);
		uint64		body;
		uint64		shape;
		size_t		k;

		if (state != HOST_DELTA_PAGE_STATE_LIVE && state != HOST_DELTA_PAGE_STATE_OWNED)
			continue;
		body = directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_BODY);
		if (body == 0)
			continue;
		Assert(STD_DIV_FOLDED(u64, body - 1, HOST_DELTA_PAGE_BLOCK) < blocks);
		shape = directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_SHAPE);
		for (k = 0; state == HOST_DELTA_PAGE_STATE_LIVE && k < shapes_len && shapes[k] != shape; k++)
			;
		if (state == HOST_DELTA_PAGE_STATE_OWNED || k == shapes_len)
			STD_VEC_AT(&pinned, uint8, STD_DIV_FOLDED(u64, body - 1, HOST_DELTA_PAGE_BLOCK)) = 1;
	}
	ok = host_delta_blocks_packing(directory, pinned.ptr, &drains);
	std_vec_free_Checked(&pinned, 1, 1);
	if (!ok)
	{
		std_vec_free_Checked(&left, sizeof(uint32), _Alignof(uint32));
		return false;
	}
	for (size_t k = 0; k < drains.len; k++)
		STD_VEC_AT(&left, uint32, STD_VEC_AT(&drains, HostDeltaBlocksDrain, k).block) =
			STD_VEC_AT(&drains, HostDeltaBlocksDrain, k).pages;
	for (uint32 slot = 0; ok && slot < slots && drains.len > 0; slot++)
	{
		uint64		word = directory.ops->word(directory.self, slot);
		uint64		body;
		uint64		shape;
		uint32		block;
		size_t		k;

		if (host_delta_page_state(word) != HOST_DELTA_PAGE_STATE_LIVE)
			continue;
		body = directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_BODY);
		if (body == 0)
			continue;
		block = (uint32) STD_DIV_FOLDED(u64, body - 1, HOST_DELTA_PAGE_BLOCK);
		Assert(block < blocks);
		if (STD_VEC_AT(&left, uint32, block) == 0)
			continue;
		shape = directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_SHAPE);
		for (k = 0; k < shapes_len && shapes[k] != shape; k++)
			;
		if (k == shapes_len)
			continue;
		ok = std_vec_try_reserve(result, 1, sizeof(HostDeltaDirectoryPacked), _Alignof(HostDeltaDirectoryPacked));
		if (ok)
		{
			HostDeltaDirectoryPacked packed = {
				host_delta_page_Page_value(host_delta_page_Page_of(slot, word)),
				directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_SHAPE),
				directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_OWNER),
				host_delta_directory_sized(directory.ops->load(directory.self, slot, HOST_DELTA_DIRECTORY_FIELD_SIZES)),
			};

			std_vec_push_Checked(result, &packed, sizeof(packed), _Alignof(HostDeltaDirectoryPacked));
			STD_VEC_AT(&left, uint32, block)--;
		}
	}
	std_vec_free_Checked(&drains, sizeof(HostDeltaBlocksDrain), _Alignof(HostDeltaBlocksDrain));
	std_vec_free_Checked(&left, sizeof(uint32), _Alignof(uint32));
	if (!ok)
		std_vec_free_Checked(result, sizeof(HostDeltaDirectoryPacked), _Alignof(HostDeltaDirectoryPacked));
	return ok;
}

#ifdef DOLTGRES_TEST_HOST

/* host_delta_directory_tests_Array5U8_Debug_fmt is the Debug of a [u8; 5]: its bytes as a list. */
static bool
host_delta_directory_tests_Array5U8_Debug_fmt(const void *value, StdFormatter *f)
{
	const uint8 *bytes = (const uint8 *) value;
	StdDebugList b = std_fmt_debug_list(f);

	for (int i = 0; i < 5; i++)
		std_DebugList_entry(&b, &bytes[i], std_u8_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* host_delta_directory_tests_ResultTupleArray5U8_Debug_fmt is the Debug of a Result<(), [u8; 5]>, NULL for Ok(()). */
static bool
host_delta_directory_tests_ResultTupleArray5U8_Debug_fmt(const void *value, StdFormatter *f)
{
	const uint8 *sqlstate = *(const uint8 *const *) value;
	StdDebugTuple b;

	if (sqlstate == NULL)
	{
		StdTupleRef unit = {0};

		b = std_fmt_debug_tuple(f, "Ok", 2);
		std_DebugTuple_field(&b, &unit, std_tuple_Debug_fmt);
		return std_DebugTuple_finish(&b);
	}
	b = std_fmt_debug_tuple(f, "Err", 3);
	std_DebugTuple_field(&b, sqlstate, host_delta_directory_tests_Array5U8_Debug_fmt);
	return std_DebugTuple_finish(&b);
}

/*
 * test_host_delta_directory_tests_a_size_fits_its_word: a slot's size word
 * holds both of a size's numbers, and a page's bytes start after its words.
 */
void
test_host_delta_directory_tests_a_size_fits_its_word(void)
{
	HostDeltaPageSize size = {((uint32) 128 << 10), PG_UINT32_MAX};
	HostDeltaPageSize back = host_delta_directory_sized(host_delta_directory_sizes(size));
	HostDeltaPageSize three = {4096, 3};
	HostDeltaPageSize zero = {4096, 0};

	STD_ASSERT_EQ(894, 9, back.bytes == size.bytes && back.words == size.words, &back,
				  host_delta_page_Size_Debug_fmt, &size, host_delta_page_Size_Debug_fmt);
	STD_ASSERT_EQ_U64(895, 9, host_delta_directory_bytes_at(three), 24);
	STD_ASSERT_EQ_U64(896, 9, host_delta_directory_bytes_at(zero), 0);
}

/*
 * test_host_delta_directory_tests_a_words_index_is_below_its_pages_count: a
 * word's index is checked against its page's count of words.
 */
void
test_host_delta_directory_tests_a_words_index_is_below_its_pages_count(void)
{
	HostDeltaPageSize size = {0, 2};
	HostError	error;
	uint8		sqlstate[5];
	const uint8 *left = NULL;
	static const uint8 expected[5] = {'X', 'X', '0', '0', '0'};
	const uint8 *right = expected;
	bool		ok;

	ok = host_delta_directory_worded(size, 1, &error);
	if (!ok)
		host_Error_drop(&error);
	STD_ASSERT(903, 9, ok, "worded(size, 1).is_ok()");
	if (!host_delta_directory_worded(size, 2, &error))
	{
		memcpy(sqlstate, host_Error_sqlstate(&error), 5);
		host_Error_drop(&error);
		left = sqlstate;
	}
	STD_ASSERT_EQ(904, 9, left != NULL && memcmp(left, right, 5) == 0, &left,
				  host_delta_directory_tests_ResultTupleArray5U8_Debug_fmt, &right,
				  host_delta_directory_tests_ResultTupleArray5U8_Debug_fmt);
}

/* HOST_DELTA_DIRECTORY_TESTS_MOST is the most slots, and aged records, a test's directory holds. */
#define HOST_DELTA_DIRECTORY_TESTS_MOST 4096

/*
 * host_delta_directory_tests_Directory_t is a test's directory in process
 * memory: its slots and their words and links, one aged kind's records,
 * stamps, and links, the lists' heads, the header's counts, the epochs, the
 * region's words and each block's three words, one block for each slot, and
 * the count of the slot and record words loaded while a case counts them.
 */
typedef struct host_delta_directory_tests_Directory_t
{
	uint32		slots;
	uint64		words[HOST_DELTA_DIRECTORY_TESTS_MOST];
	uint64		fields[HOST_DELTA_DIRECTORY_TESTS_MOST][7];
	uint32		links[HOST_DELTA_DIRECTORY_TESTS_MOST];
	uint64		records[HOST_DELTA_DIRECTORY_TESTS_MOST];
	uint64		stamps[HOST_DELTA_DIRECTORY_TESTS_MOST];
	uint64		record_links[HOST_DELTA_DIRECTORY_TESTS_MOST];
	uint64		region[HOST_DELTA_BLOCKS_WORDS];
	uint64		blocks[HOST_DELTA_DIRECTORY_TESTS_MOST][3];
	uint64		head;
	uint64		record_head;
	uint64		frees;
	uint64		left;
	uint64		kept;
	uint64		overshoot;
	uint64		epoch;
	uint64		oldest;
	/* passing is whether visits counts the slot and record words loaded. */
	bool		passing;
	uint64		visits;
} host_delta_directory_tests_Directory_t;

/* HOST_DELTA_DIRECTORY_TESTS_AGED is the id of a test directory's one aged kind. */
#define HOST_DELTA_DIRECTORY_TESTS_AGED UINT64CONST(0x74657374)

/* host_delta_directory_tests_directory is the directory a case builds, too large for a stack. */
static host_delta_directory_tests_Directory_t host_delta_directory_tests_directory;

/* HOST_DELTA_DIRECTORY_TESTS_SELF is a test directory's ops' self as the directory it is. */
#define HOST_DELTA_DIRECTORY_TESTS_SELF(self) ((host_delta_directory_tests_Directory_t *) (self))

/* host_delta_directory_tests_slots is a test directory's count of slots. */
static uint32
host_delta_directory_tests_slots(void *self)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->slots;
}

/* host_delta_directory_tests_budget is a test directory's budget, one 64 KiB block for each slot. */
static uint64
host_delta_directory_tests_budget(void *self)
{
	return (uint64) HOST_DELTA_DIRECTORY_TESTS_SELF(self)->slots * HOST_DELTA_PAGE_BLOCK;
}

/* host_delta_directory_tests_word loads a slot's word, counting it while a case counts. */
static uint64
host_delta_directory_tests_word(void *self, uint32 slot)
{
	host_delta_directory_tests_Directory_t *d = HOST_DELTA_DIRECTORY_TESTS_SELF(self);

	if (d->passing)
		d->visits++;
	return d->words[slot];
}

/* host_delta_directory_tests_swap stores a slot's word over the one loaded. */
static bool
host_delta_directory_tests_swap(void *self, uint32 slot, uint64 loaded, uint64 word)
{
	host_delta_directory_tests_Directory_t *d = HOST_DELTA_DIRECTORY_TESTS_SELF(self);

	if (d->words[slot] != loaded)
		return false;
	d->words[slot] = word;
	return true;
}

/* host_delta_directory_tests_load loads one of a slot's other words. */
static uint64
host_delta_directory_tests_load(void *self, uint32 slot, HostDeltaDirectoryField field)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->fields[slot][field];
}

/* host_delta_directory_tests_store stores one of a slot's other words. */
static void
host_delta_directory_tests_store(void *self, uint32 slot, HostDeltaDirectoryField field, uint64 value)
{
	HOST_DELTA_DIRECTORY_TESTS_SELF(self)->fields[slot][field] = value;
}

/* host_delta_directory_tests_blocks is a test directory's count of blocks, one for each slot. */
static uint32
host_delta_directory_tests_blocks(void *self)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->slots;
}

/* host_delta_directory_tests_region loads one of a test directory's region's words. */
static uint64
host_delta_directory_tests_region(void *self, HostDeltaBlocksRegion word)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->region[host_delta_blocks_Region_index(word)];
}

/* host_delta_directory_tests_set_region stores one of a test directory's region's words. */
static void
host_delta_directory_tests_set_region(void *self, HostDeltaBlocksRegion word, uint64 value)
{
	HOST_DELTA_DIRECTORY_TESTS_SELF(self)->region[host_delta_blocks_Region_index(word)] = value;
}

/* host_delta_directory_tests_block loads one of a block's words in a test directory's region. */
static uint64
host_delta_directory_tests_block(void *self, uint32 block, HostDeltaBlocksBlock word)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->blocks[block][word];
}

/* host_delta_directory_tests_set_block stores one of a block's words in a test directory's region. */
static void
host_delta_directory_tests_set_block(void *self, uint32 block, HostDeltaBlocksBlock word, uint64 value)
{
	HOST_DELTA_DIRECTORY_TESTS_SELF(self)->blocks[block][word] = value;
}

/* host_delta_directory_tests_nothing does nothing, as a test directory's locks and listings do. */
static void
host_delta_directory_tests_nothing(void *self)
{
}

/* host_delta_directory_tests_frees is a test directory's count of frees. */
static uint64
host_delta_directory_tests_frees(void *self)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->frees;
}

/* host_delta_directory_tests_freed counts a free. */
static void
host_delta_directory_tests_freed(void *self)
{
	HOST_DELTA_DIRECTORY_TESTS_SELF(self)->frees++;
}

/* host_delta_directory_tests_left is a test directory's count of retirements. */
static uint64
host_delta_directory_tests_left(void *self)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->left;
}

/* host_delta_directory_tests_leave counts a retirement. */
static void
host_delta_directory_tests_leave(void *self)
{
	HOST_DELTA_DIRECTORY_TESTS_SELF(self)->left++;
}

/* host_delta_directory_tests_settle clears the count of retirements while it holds the one loaded. */
static void
host_delta_directory_tests_settle(void *self, uint64 loaded)
{
	host_delta_directory_tests_Directory_t *d = HOST_DELTA_DIRECTORY_TESTS_SELF(self);

	if (d->left == loaded)
		d->left = 0;
}

/* host_delta_directory_tests_kept is a test directory's kept stamp. */
static uint64
host_delta_directory_tests_kept(void *self)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->kept;
}

/* host_delta_directory_tests_kept_swap stores the kept stamp over the one loaded. */
static bool
host_delta_directory_tests_kept_swap(void *self, uint64 loaded, uint64 word)
{
	host_delta_directory_tests_Directory_t *d = HOST_DELTA_DIRECTORY_TESTS_SELF(self);

	if (d->kept != loaded)
		return false;
	d->kept = word;
	return true;
}

/* host_delta_directory_tests_zero answers 0 for a count no case moves. */
static uint64
host_delta_directory_tests_zero_count(void *self)
{
	return 0;
}

/* host_delta_directory_tests_count_waiter counts nothing, since no case waits. */
static void
host_delta_directory_tests_count_waiter(void *self, bool add)
{
}

/* host_delta_directory_tests_overshoot is a test directory's overshoot word. */
static uint64
host_delta_directory_tests_overshoot(void *self)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->overshoot;
}

/* host_delta_directory_tests_overshoot_swap stores the overshoot word over the one loaded. */
static bool
host_delta_directory_tests_overshoot_swap(void *self, uint64 loaded, uint64 word)
{
	host_delta_directory_tests_Directory_t *d = HOST_DELTA_DIRECTORY_TESTS_SELF(self);

	if (d->overshoot != loaded)
		return false;
	d->overshoot = word;
	return true;
}

/* host_delta_directory_tests_hand is a test directory's hand, its first slot. */
static uint32
host_delta_directory_tests_hand(void *self)
{
	return 0;
}

/* host_delta_directory_tests_hint is a word of a test directory's free hint, which holds no free slot. */
static uint64
host_delta_directory_tests_hint(void *self, uint32 word)
{
	return 0;
}

/* host_delta_directory_tests_hint_slot sets or clears no bit, since no case frees a slot. */
static void
host_delta_directory_tests_hint_slot(void *self, uint32 slot)
{
}

/* host_delta_directory_tests_spent is a test directory's count of spent slots, none. */
static uint64
host_delta_directory_tests_spent(void *self, bool over)
{
	return 0;
}

/* host_delta_directory_tests_spend counts no spent slot, since no case spends one. */
static void
host_delta_directory_tests_spend(void *self, bool over)
{
}

/* host_delta_directory_tests_lock takes a test directory's allocation lock, which no other process takes. */
static bool
host_delta_directory_tests_lock(void *self, const void *raise, HostError *error)
{
	return true;
}

/* host_delta_directory_tests_zero_body zeroes no body, since no case takes one. */
static void
host_delta_directory_tests_zero_body(void *self, uint64 body, HostDeltaPageSize size)
{
}

/* host_delta_directory_tests_write writes no body, since no case takes one. */
static void
host_delta_directory_tests_write(void *self, uint64 body, uint64 at, const uint8 *bytes, size_t bytes_len)
{
}

/* host_delta_directory_tests_set stores no body word, since no case takes one. */
static void
host_delta_directory_tests_set(void *self, uint64 body, uint64 index, uint64 value)
{
}

/* host_delta_directory_tests_own lists no page, since no case takes one. */
static void
host_delta_directory_tests_own(void *self, HostDeltaPagePage name)
{
}

/* host_delta_directory_tests_retiring is the epoch a retirement stamps: the current one. */
static uint64
host_delta_directory_tests_retiring(void *self)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->epoch;
}

/* host_delta_directory_tests_retiring_now takes the epoch lock, which no other process holds. */
static bool
host_delta_directory_tests_retiring_now(void *self, uint64 *result)
{
	*result = HOST_DELTA_DIRECTORY_TESTS_SELF(self)->epoch;
	return true;
}

/* host_delta_directory_tests_retired advances the epoch. */
static void
host_delta_directory_tests_retired(void *self)
{
	HOST_DELTA_DIRECTORY_TESTS_SELF(self)->epoch++;
}

/* host_delta_directory_tests_epoch is a test directory's current epoch. */
static uint64
host_delta_directory_tests_epoch(void *self)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->epoch;
}

/* host_delta_directory_tests_oldest is a test directory's oldest epoch, which a reader holds below every retirement. */
static uint64
host_delta_directory_tests_oldest(void *self)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->oldest;
}

/* host_delta_directory_tests_stop holds at no point. */
static void
host_delta_directory_tests_stop(void *self, HostDeltaDirectoryPoint point)
{
}

/* host_delta_directory_tests_aged is a test directory's count of aged kinds: one. */
static uint32
host_delta_directory_tests_aged(void *self)
{
	return 1;
}

/* host_delta_directory_tests_aged_kind is the number of a test directory's one aged kind. */
static bool
host_delta_directory_tests_aged_kind(void *self, uint64 id, uint32 *result)
{
	*result = 0;
	return id == HOST_DELTA_DIRECTORY_TESTS_AGED;
}

/* host_delta_directory_tests_records is the count of a test directory's aged records, one for each slot. */
static uint32
host_delta_directory_tests_records(void *self, uint32 kind)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->slots;
}

/* host_delta_directory_tests_record_word loads an aged record's word, counting it while a case counts. */
static uint64
host_delta_directory_tests_record_word(void *self, uint32 kind, uint32 index)
{
	host_delta_directory_tests_Directory_t *d = HOST_DELTA_DIRECTORY_TESTS_SELF(self);

	if (d->passing)
		d->visits++;
	return d->records[index];
}

/* host_delta_directory_tests_record_swap stores an aged record's word over the one loaded. */
static bool
host_delta_directory_tests_record_swap(void *self, uint32 kind, uint32 index, uint64 loaded, uint64 word)
{
	host_delta_directory_tests_Directory_t *d = HOST_DELTA_DIRECTORY_TESTS_SELF(self);

	if (d->records[index] != loaded)
		return false;
	d->records[index] = word;
	return true;
}

/* host_delta_directory_tests_stamp loads an aged record's stamp. */
static uint64
host_delta_directory_tests_stamp(void *self, uint32 kind, uint32 index)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->stamps[index];
}

/* host_delta_directory_tests_set_stamp stores an aged record's stamp. */
static void
host_delta_directory_tests_set_stamp(void *self, uint32 kind, uint32 index, uint64 stamp)
{
	HOST_DELTA_DIRECTORY_TESTS_SELF(self)->stamps[index] = stamp;
}

/* host_delta_directory_tests_record_hand is an aged kind's hand, its first record. */
static uint32
host_delta_directory_tests_record_hand(void *self, uint32 kind)
{
	return 0;
}

/* host_delta_directory_tests_record_nothing does nothing with an aged record, as its clear and listings do here. */
static void
host_delta_directory_tests_record_nothing(void *self, uint32 kind, uint32 index)
{
}

/* host_delta_directory_tests_retired_head loads the retired list's head. */
static uint64
host_delta_directory_tests_retired_head(void *self)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->head;
}

/* host_delta_directory_tests_retired_swap stores the retired list's head over the one loaded. */
static bool
host_delta_directory_tests_retired_swap(void *self, uint64 loaded, uint64 word)
{
	host_delta_directory_tests_Directory_t *d = HOST_DELTA_DIRECTORY_TESTS_SELF(self);

	if (d->head != loaded)
		return false;
	d->head = word;
	return true;
}

/* host_delta_directory_tests_link loads a slot's link. */
static uint32
host_delta_directory_tests_link(void *self, uint32 slot)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->links[slot];
}

/* host_delta_directory_tests_set_link stores a slot's link. */
static void
host_delta_directory_tests_set_link(void *self, uint32 slot, uint32 value)
{
	HOST_DELTA_DIRECTORY_TESTS_SELF(self)->links[slot] = value;
}

/* host_delta_directory_tests_record_retired_head loads the aged kind's retired list's head. */
static uint64
host_delta_directory_tests_record_retired_head(void *self, uint32 kind)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->record_head;
}

/* host_delta_directory_tests_record_retired_swap stores the aged kind's retired list's head over the one loaded. */
static bool
host_delta_directory_tests_record_retired_swap(void *self, uint32 kind, uint64 loaded, uint64 word)
{
	host_delta_directory_tests_Directory_t *d = HOST_DELTA_DIRECTORY_TESTS_SELF(self);

	if (d->record_head != loaded)
		return false;
	d->record_head = word;
	return true;
}

/* host_delta_directory_tests_record_link loads an aged record's link. */
static uint64
host_delta_directory_tests_record_link(void *self, uint32 kind, uint32 index)
{
	return HOST_DELTA_DIRECTORY_TESTS_SELF(self)->record_links[index];
}

/* host_delta_directory_tests_set_record_link stores an aged record's link. */
static void
host_delta_directory_tests_set_record_link(void *self, uint32 kind, uint32 index, uint64 value)
{
	HOST_DELTA_DIRECTORY_TESTS_SELF(self)->record_links[index] = value;
}

/* HOST_DELTA_DIRECTORY_TESTS_OPS is a test directory's Directory table. */
static const HostDeltaDirectoryDirectoryOps HOST_DELTA_DIRECTORY_TESTS_OPS = {
	.slots = host_delta_directory_tests_slots,
	.budget = host_delta_directory_tests_budget,
	.word = host_delta_directory_tests_word,
	.swap = host_delta_directory_tests_swap,
	.load = host_delta_directory_tests_load,
	.store = host_delta_directory_tests_store,
	.blocks = host_delta_directory_tests_blocks,
	.region = host_delta_directory_tests_region,
	.set_region = host_delta_directory_tests_set_region,
	.block = host_delta_directory_tests_block,
	.set_block = host_delta_directory_tests_set_block,
	.lock_region = host_delta_directory_tests_nothing,
	.unlock_region = host_delta_directory_tests_nothing,
	.frees = host_delta_directory_tests_frees,
	.freed = host_delta_directory_tests_freed,
	.left = host_delta_directory_tests_left,
	.leave = host_delta_directory_tests_leave,
	.settle = host_delta_directory_tests_settle,
	.kept = host_delta_directory_tests_kept,
	.kept_swap = host_delta_directory_tests_kept_swap,
	.waiting = host_delta_directory_tests_zero_count,
	.count_waiter = host_delta_directory_tests_count_waiter,
	.overshoot = host_delta_directory_tests_overshoot,
	.overshoot_swap = host_delta_directory_tests_overshoot_swap,
	.hand = host_delta_directory_tests_hand,
	.hint = host_delta_directory_tests_hint,
	.hint_set = host_delta_directory_tests_hint_slot,
	.hint_clear = host_delta_directory_tests_hint_slot,
	.spent = host_delta_directory_tests_spent,
	.spend = host_delta_directory_tests_spend,
	.lock = host_delta_directory_tests_lock,
	.unlock = host_delta_directory_tests_nothing,
	.zero = host_delta_directory_tests_zero_body,
	.write = host_delta_directory_tests_write,
	.set = host_delta_directory_tests_set,
	.own = host_delta_directory_tests_own,
	.disown = host_delta_directory_tests_own,
	.retiring = host_delta_directory_tests_retiring,
	.retiring_now = host_delta_directory_tests_retiring_now,
	.retired = host_delta_directory_tests_retired,
	.unretired = host_delta_directory_tests_nothing,
	.epoch = host_delta_directory_tests_epoch,
	.oldest = host_delta_directory_tests_oldest,
	.stop = host_delta_directory_tests_stop,
	.aged = host_delta_directory_tests_aged,
	.aged_kind = host_delta_directory_tests_aged_kind,
	.records = host_delta_directory_tests_records,
	.record_word = host_delta_directory_tests_record_word,
	.record_swap = host_delta_directory_tests_record_swap,
	.stamp = host_delta_directory_tests_stamp,
	.set_stamp = host_delta_directory_tests_set_stamp,
	.record_hand = host_delta_directory_tests_record_hand,
	.clear = host_delta_directory_tests_record_nothing,
	.made = host_delta_directory_tests_record_nothing,
	.unmade = host_delta_directory_tests_record_nothing,
	.retired_head = host_delta_directory_tests_retired_head,
	.retired_swap = host_delta_directory_tests_retired_swap,
	.link = host_delta_directory_tests_link,
	.set_link = host_delta_directory_tests_set_link,
	.record_retired_head = host_delta_directory_tests_record_retired_head,
	.record_retired_swap = host_delta_directory_tests_record_retired_swap,
	.record_link = host_delta_directory_tests_record_link,
	.set_record_link = host_delta_directory_tests_set_record_link,
	.lock_pass = host_delta_directory_tests_nothing,
	.unlock_pass = host_delta_directory_tests_nothing,
};

/*
 * host_delta_directory_tests_retirements makes a test directory of a count of
 * slots, every slot and aged record live, a reader holding the oldest epoch
 * below every retirement, and retires ten pages and ten aged records, the
 * last slots' and records'.
 */
static HostDeltaDirectoryDirectory
host_delta_directory_tests_retirements(uint32 slots)
{
	host_delta_directory_tests_Directory_t *d = &host_delta_directory_tests_directory;
	HostDeltaDirectoryDirectory directory = {&HOST_DELTA_DIRECTORY_TESTS_OPS, d};
	HostDeltaPagePage names[10];
	HostDeltaLeaving leaving[10];
	uint64		owned;
	uint64		live;

	memset(d, 0, sizeof(*d));
	d->slots = slots;
	d->epoch = 5;
	d->oldest = 1;
	if (!host_delta_page_claim(0, &owned))
		std_panic(__FILE__, __LINE__, 0, "a free slot is claimed", sizeof("a free slot is claimed") - 1);
	live = host_delta_page_link(owned);
	for (uint32 at = 0; at < slots; at++)
	{
		d->words[at] = live;
		d->records[at] = live;
	}
	for (uint32 i = 0; i < 10; i++)
	{
		uint32		at = slots - 1 - i;

		names[i] = host_delta_page_Page_of(at, live);
		leaving[i].id = HOST_DELTA_DIRECTORY_TESTS_AGED;
		leaving[i].location = host_delta_page_Page_value(host_delta_page_Page_of(at, live));
	}
	host_delta_directory_retire(directory, leaving, lengthof(leaving), names, lengthof(names));
	return directory;
}

/* host_delta_directory_tests_visited asserts that a case loaded want words, citing this file's line. */
static void
host_delta_directory_tests_visited(uint32 line, uint64 want)
{
	uint64		visits = host_delta_directory_tests_directory.visits;

	if (visits != want)
		std_assert_failed(__FILE__, line, 0, "==", &visits, std_u64_Debug_fmt, &want, std_u64_Debug_fmt, NULL);
}

/*
 * Bugs row 309's free pass: a pass after ten pages and ten aged records
 * retire loads the words of those twenty and of no other slot or record,
 * at a directory of 128 slots and at one of 4,096, so its work is the
 * retirements and not the directory's size.
 */
void
test_host_delta_directory_tests_a_free_pass_loads_only_the_retirements(void)
{
	static const uint32 sizes[] = {128, HOST_DELTA_DIRECTORY_TESTS_MOST};

	for (size_t i = 0; i < lengthof(sizes); i++)
	{
		HostDeltaDirectoryDirectory directory = host_delta_directory_tests_retirements(sizes[i]);

		host_delta_directory_tests_directory.passing = true;
		host_delta_directory_pass(directory);
		host_delta_directory_tests_visited(__LINE__, 20);
		host_delta_directory_tests_directory.visits = 0;
		host_delta_directory_pass(directory);
		host_delta_directory_tests_visited(__LINE__, 20);
		host_delta_directory_tests_directory.passing = false;
	}
}

/*
 * Bugs row 309's refused allocation: an allocation the directory refuses,
 * every slot live, runs a free pass that loads the words of the ten pages
 * and ten aged records retired and of no other, at both sizes.
 */
void
test_host_delta_directory_tests_a_refused_allocations_pass_loads_only_the_retirements(void)
{
	static const uint32 sizes[] = {128, HOST_DELTA_DIRECTORY_TESTS_MOST};

	for (size_t i = 0; i < lengthof(sizes); i++)
	{
		HostDeltaDirectoryDirectory directory = host_delta_directory_tests_retirements(sizes[i]);
		HostDeltaPageSize wanted[1] = {{4096, 1}};
		StdVec		names;
		bool		has_full;
		uint64		full;
		HostError	error;

		std_vec_with_capacity_Checked(&names, 1, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
		host_delta_directory_tests_directory.passing = true;
		if (!host_delta_directory_alloc(directory, NULL, wanted, lengthof(wanted), &names, &has_full, &full, &error))
			std_panic(__FILE__, __LINE__, 0, "a refused allocation answers its budget's signal",
					  sizeof("a refused allocation answers its budget's signal") - 1);
		host_delta_directory_tests_directory.passing = false;
		if (!has_full || full != 0)
			std_panic(__FILE__, __LINE__, 0, "the allocation is refused", sizeof("the allocation is refused") - 1);
		host_delta_directory_tests_visited(__LINE__, 20);
		std_vec_free_Checked(&names, sizeof(HostDeltaPagePage), _Alignof(HostDeltaPagePage));
	}
}

/* HOST_DELTA_DIRECTORY_TESTS_MOVED is the shape of the pages a packing case's packing moves. */
#define HOST_DELTA_DIRECTORY_TESTS_MOVED UINT64CONST(0x6d6f766564)

/* host_delta_directory_tests_taken takes a 1 KiB body from a test directory's region, as a slot's take does. */
static uint64
host_delta_directory_tests_taken(HostDeltaDirectoryDirectory directory)
{
	HostDeltaPageSize size = {1024, 0};
	uint64		body;

	STD_ASSERT(__LINE__, 9, host_delta_blocks_take(directory, size, false, &body), "the region gives a body");
	return body;
}

/*
 * host_delta_directory_tests_packable makes a test directory of 8 slots whose
 * region's block 0 holds 3 pages and block 1 the page of slot 0, live and of
 * the moved shape, plus a second page when a take is in flight in slot 1;
 * slot 1 holds a body of 0, owned for that take or else live, as a release or
 * a reap leaves it, so a packing drains block 1 into block 0.
 */
static HostDeltaDirectoryDirectory
host_delta_directory_tests_packable(bool taking, uint64 *page, uint64 *taken)
{
	host_delta_directory_tests_Directory_t *d = &host_delta_directory_tests_directory;
	HostDeltaDirectoryDirectory directory = {&HOST_DELTA_DIRECTORY_TESTS_OPS, d};
	uint64		filled[64];
	uint64		owned;

	memset(d, 0, sizeof(*d));
	d->slots = 8;
	for (int i = 0; i < 64; i++)
		filled[i] = host_delta_directory_tests_taken(directory);
	*page = host_delta_directory_tests_taken(directory);
	*taken = taking ? host_delta_directory_tests_taken(directory) : 0;
	for (int i = 3; i < 64; i++)
		host_delta_blocks_give(directory, filled[i]);
	STD_ASSERT(__LINE__, 9, host_delta_page_claim(0, &owned), "a free slot is claimed");
	d->words[0] = host_delta_page_link(owned);
	d->fields[0][HOST_DELTA_DIRECTORY_FIELD_SHAPE] = HOST_DELTA_DIRECTORY_TESTS_MOVED;
	d->fields[0][HOST_DELTA_DIRECTORY_FIELD_BODY] = *page;
	d->words[1] = taking ? owned : host_delta_page_link(owned);
	d->fields[1][HOST_DELTA_DIRECTORY_FIELD_SHAPE] = HOST_DELTA_DIRECTORY_TESTS_MOVED;
	return directory;
}

/*
 * Bugs row 486's live slot: a packing that drains a block skips a live slot
 * whose body reads 0, as a release or a reap leaves it while the packing
 * reads, and plans the move of the drained block's one page alone.
 */
void
test_host_delta_directory_tests_a_packing_skips_a_live_slot_with_no_body(void)
{
	uint64		page;
	uint64		taken;
	HostDeltaDirectoryDirectory directory = host_delta_directory_tests_packable(false, &page, &taken);
	uint64		shapes[] = {HOST_DELTA_DIRECTORY_TESTS_MOVED};
	StdVec		packed;

	STD_ASSERT(__LINE__, 9, host_delta_directory_packing(directory, shapes, lengthof(shapes), &packed),
			   "a packing reserves its lists");
	STD_ASSERT(__LINE__, 9, packed.len == 1, "the packing plans one move");
	STD_ASSERT(__LINE__, 9, STD_VEC_AT(&packed, HostDeltaDirectoryPacked, 0).name ==
			   host_delta_page_Page_value(host_delta_page_Page_of(0, host_delta_directory_tests_directory.words[0])),
			   "the move is the drained block's page");
	std_vec_free_Checked(&packed, sizeof(HostDeltaDirectoryPacked), _Alignof(HostDeltaDirectoryPacked));
}

/*
 * Bugs row 486's take in flight: a packing that reads an owned slot's body as
 * 0, its take's body taken in block 1 but not yet stored, drains block 1 and
 * plans the move of its live page alone; once the take stores its body and
 * the move takes a new body and gives the old one back, block 1 still holds
 * the taken page, so the drain left it unemptied and freed nothing.
 */
void
test_host_delta_directory_tests_a_packing_leaves_a_block_a_take_holds_unemptied(void)
{
	host_delta_directory_tests_Directory_t *d = &host_delta_directory_tests_directory;
	uint64		page;
	uint64		taken;
	HostDeltaDirectoryDirectory directory = host_delta_directory_tests_packable(true, &page, &taken);
	uint64		shapes[] = {HOST_DELTA_DIRECTORY_TESTS_MOVED};
	StdVec		packed;
	uint64		moved;

	STD_ASSERT(__LINE__, 9, STD_DIV_FOLDED(u64, taken - 1, HOST_DELTA_PAGE_BLOCK) == 1, "the take is in block 1");
	STD_ASSERT(__LINE__, 9, host_delta_directory_packing(directory, shapes, lengthof(shapes), &packed),
			   "a packing reserves its lists");
	STD_ASSERT(__LINE__, 9, packed.len == 1, "the packing plans one move");
	STD_ASSERT(__LINE__, 9, STD_VEC_AT(&packed, HostDeltaDirectoryPacked, 0).name ==
			   host_delta_page_Page_value(host_delta_page_Page_of(0, d->words[0])),
			   "the move is the drained block's live page");
	d->fields[1][HOST_DELTA_DIRECTORY_FIELD_BODY] = taken;
	moved = host_delta_directory_tests_taken(directory);
	host_delta_blocks_give(directory, page);
	d->fields[0][HOST_DELTA_DIRECTORY_FIELD_BODY] = moved;
	STD_ASSERT(__LINE__, 9, STD_DIV_FOLDED(u64, moved - 1, HOST_DELTA_PAGE_BLOCK) == 0,
			   "the move takes its new body in block 0");
	STD_ASSERT(__LINE__, 9, d->blocks[1][HOST_DELTA_BLOCKS_BLOCK_MAP] ==
			   UINT64CONST(1) << ((taken - 1) % HOST_DELTA_PAGE_BLOCK / 1024),
			   "block 1 still holds the taken page alone");
	std_vec_free_Checked(&packed, sizeof(HostDeltaDirectoryPacked), _Alignof(HostDeltaDirectoryPacked));
}

#endif							/* DOLTGRES_TEST_HOST */
