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
 * The C port of txn's memtable/pages.rs: a range's own pages, each page's
 * first four words counting the words handed out of it, its live records,
 * the lists that name it, and its dead records' bytes, and where everything
 * one call stores goes together, on the range's newest page or on the fresh
 * page the call was given, which then goes onto the range's list of pages.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/delta.h"
#include "doltgres/host/delta/list.h"
#include "doltgres/host/delta/page.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/index.h"
#include "doltgres/txn/memtable/pages.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "memtable" STD_SEP "pages.rs"

/* TXN_MEMTABLE_PAGES_SMALLEST is SMALLEST, the body of a range's first page, before what it must hold makes it larger. */
#define TXN_MEMTABLE_PAGES_SMALLEST (UINT64CONST(1) << 10)

/* TXN_MEMTABLE_PAGES_LARGER is LARGER, the body a range's pages double to, past which a page holds one record of its own. */
#define TXN_MEMTABLE_PAGES_LARGER (UINT64CONST(64) << 10)

/* TXN_MEMTABLE_PAGES_ONE is ONE, what a one-record page's body is rounded up to. */
#define TXN_MEMTABLE_PAGES_ONE (UINT64CONST(4) << 10)

/* txn_memtable_pages_sized returns the size of a range page of a body: a quarter of it words, the rest bytes. */
static HostDeltaPageSize
txn_memtable_pages_sized(uint64 body)
{
	uint64		words = STD_DIV_FOLDED(u64, body, 32);
	HostDeltaPageSize size = {(uint32) STD_SUB(u64, body, STD_MUL(u64, words, 8, 59, 27),
											   59, 20), (uint32) words};

	return size;
}

/*
 * txn_memtable_pages_size returns the size of the next page a range takes,
 * after its newest page of a body, or of its first, for a call that stores
 * bytes and words: double the newest up to 64 KiB, or 1 KiB for a first
 * page, and larger until both fit, or past 64 KiB a page of the call's own,
 * rounded up to 4 KiB, whose words are the call's and its counts.
 */
HostDeltaPageSize
txn_memtable_pages_size(bool has_newest, uint64 newest, uint64 bytes, uint64 words)
{
	uint64		body = TXN_MEMTABLE_PAGES_SMALLEST;
	uint64		all;
	uint64		stored;
	HostDeltaPageSize size;

	if (has_newest)
	{
		uint64		doubled = STD_MUL(u64, newest, 2, 67, 52);

		body = doubled < TXN_MEMTABLE_PAGES_LARGER ? doubled : TXN_MEMTABLE_PAGES_LARGER;
	}
	while (body <= TXN_MEMTABLE_PAGES_LARGER)
	{
		size = txn_memtable_pages_sized(body);
		if (bytes <= size.bytes &&
			STD_ADD(u64, TXN_MEMTABLE_PAGES_FIRST, words, 70, 46) <= size.words)
			return size;
		body = STD_MUL(u64, body, 2, 73, 9);
	}
	all = STD_ADD(u64, TXN_MEMTABLE_PAGES_FIRST, words, 75, 17);
	stored = std_next_multiple_of_u64(STD_ADD(u64, bytes, STD_MUL(u64, all, 8, 76, 29),
											  76, 21), TXN_MEMTABLE_PAGES_ONE);
	size.bytes = (uint32) STD_SUB(u64, stored, STD_MUL(u64, all, 8, 76, 64),
								  76, 20);
	size.words = (uint32) all;
	return size;
}

/* txn_memtable_pages_Want_Debug_fmt is Want's derived Debug. */
bool
txn_memtable_pages_Want_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnMemtablePagesWant *self = value;
	StdDebugStruct s = std_fmt_debug_struct(f, "Want", strlen("Want"));

	std_DebugStruct_field(&s, "page", strlen("page"), &self->page, host_delta_page_Size_Debug_fmt);
	std_DebugStruct_field(&s, "spares", strlen("spares"), &self->spares, std_usize_Debug_fmt);
	return std_DebugStruct_finish(&s);
}

/*
 * txn_memtable_pages_room finds where a call's bytes and words all go in an
 * open record: on the range's newest page when they fit there, else on the
 * fresh page given, while the list was given the spares its growth wants.
 * The room it finds borrows the record's Open for as long as it can store;
 * it changes nothing, and when neither holds it answers what the call wants,
 * its fresh page sized after the newest page's body as its words show it.
 */
bool
txn_memtable_pages_room(TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold, uint64 bytes, uint64 words,
						bool *has_fresh, HostDeltaOwned *fresh, const HostDeltaOwned *spare, size_t spare_len,
						bool *result_ok, TxnMemtablePagesRoom *result, TxnMemtablePagesWant *result_err,
						HostError *error)
{
	HostDeltaListListView list = host_delta_list_Guard_list(open->open.guard, open->open.closes->shape,
															TXN_MEMTABLE_INDEX_PAGES);
	uint64		pages = host_delta_list_ListView_len(&list);
	bool		has_body = false;
	uint64		body = 0;
	size_t		spares;

	if (pages != 0)
	{
		uint64		newest = std_wrapping_sub_u64(pages, 1);
		uint64		name;
		HostDeltaPageRef page;
		HostDeltaWord count;
		uint64		used;

		if (!host_delta_list_ListView_get(&list, hold, newest, &name, error) ||
			!host_delta_Hold_page(hold, host_delta_page_Page_named(name), &page, error) ||
			!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_COUNT, &count, error))
			return false;
		used = host_delta_Word_load(&count);
		if (STD_ADD(u64, used, words, 129, 12) <= host_delta_PageRef_words(&page) &&
			bytes <= host_delta_PageRef_room(&page))
		{
			*result_ok = true;
			result->open = open;
			result->target.tag = TXN_MEMTABLE_PAGES_TARGET_NEWEST;
			result->target.u.newest = page;
			result->page = newest;
			result->first = used;
			result->words = words;
			result->records = 0;
			return true;
		}
		has_body = true;
		body = STD_MUL(u64, host_delta_PageRef_words(&page), 32, 132, 21);
	}
	spares = host_delta_list_ListView_wants(&list);
	if (*has_fresh && spare_len >= spares)
	{
		*has_fresh = false;
		*result_ok = true;
		result->open = open;
		result->target.tag = TXN_MEMTABLE_PAGES_TARGET_FRESH;
		result->target.u.fresh = *fresh;
		result->page = pages;
		result->first = TXN_MEMTABLE_PAGES_FIRST;
		result->words = words;
		result->records = 0;
		return true;
	}
	*result_ok = false;
	result_err->page = txn_memtable_pages_size(has_body, body, bytes, words);
	result_err->spares = spares;
	return true;
}

/* txn_memtable_pages_Room_drop drops a room that never finished: its fresh page, which goes back unpublished. */
void
txn_memtable_pages_Room_drop(TxnMemtablePagesRoom *self)
{
	if (self->target.tag == TXN_MEMTABLE_PAGES_TARGET_FRESH)
		host_delta_Owned_drop(&self->target.u.fresh);
}

/* txn_memtable_pages_Room_append appends a record to the room's page and returns its offset. */
bool
txn_memtable_pages_Room_append(TxnMemtablePagesRoom *self, const uint8 *record, size_t record_len, uint32 *result,
							   HostError *error)
{
	self->records = STD_ADD(u64, self->records, 1, 149, 9);
	if (self->target.tag == TXN_MEMTABLE_PAGES_TARGET_NEWEST)
		return host_delta_PageRef_append(&self->target.u.newest, self->open->open.closes->shape,
										 self->open->open.closes->lock,
										 self->open->open.guard, record, record_len, result, error);
	return host_delta_Owned_append(&self->target.u.fresh, record, record_len, result, error);
}

/* txn_memtable_pages_Room_store stores one of the words handed out to the call, by its index among them. */
bool
txn_memtable_pages_Room_store(TxnMemtablePagesRoom *self, uint64 word, uint64 value, HostError *error)
{
	HostDeltaWord stored;

	if (self->target.tag == TXN_MEMTABLE_PAGES_TARGET_FRESH)
		return host_delta_Owned_store(&self->target.u.fresh, STD_ADD(u64, self->first, word, 160, 47),
									  value, error);
	if (!host_delta_PageRef_word(&self->target.u.newest, STD_ADD(u64, self->first, word, 159, 47),
								 &stored, error))
		return false;
	host_delta_Word_store(&stored, value);
	return true;
}

/*
 * txn_memtable_pages_Room_finish counts the call's words on its page and
 * puts a fresh page onto the range's list of pages, whose spares it was found
 * with, storing its words as the newest page's, and gives the open record
 * back for the store that publishes what the call stored; it takes the room.
 */
bool
txn_memtable_pages_Room_finish(TxnMemtablePagesRoom self,
							   const HostDeltaHold *hold, StdVec *spare,
							   TxnMemtableIndexKeyedOpen **result, HostError *error)
{
	const TxnMemtableIndexKeyedOps *keyed = self.open->keyed;

	if (self.target.tag == TXN_MEMTABLE_PAGES_TARGET_NEWEST)
	{
		HostDeltaWord word;

		if (!host_delta_PageRef_word(&self.target.u.newest, TXN_MEMTABLE_PAGES_COUNT, &word, error))
			return false;
		host_delta_Word_store(&word, STD_ADD(u64, self.first, self.words, 171, 41));
		if (!host_delta_PageRef_word(&self.target.u.newest, TXN_MEMTABLE_PAGES_LIVE, &word, error))
			return false;
		host_delta_Word_fetch_add(&word, self.records);
	}
	else
	{
		HostDeltaListListMut list;
		HostDeltaPagePage name;
		HostDeltaPageRef page;
		HostSharedRecord record = host_shared_Guard_record(self.open->open.guard);
		HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
		HostSharedOp exchange = {.tag = HOST_SHARED_OP_EXCHANGE};

		if (!host_delta_Owned_store(&self.target.u.fresh, TXN_MEMTABLE_PAGES_COUNT,
									STD_ADD(u64, self.first, self.words, 175, 35), error) ||
			!host_delta_Owned_store(&self.target.u.fresh, TXN_MEMTABLE_PAGES_LIVE, self.records, error) ||
			!host_delta_Owned_store(&self.target.u.fresh, TXN_MEMTABLE_PAGES_LISTS, 1, error))
		{
			host_delta_Owned_drop(&self.target.u.fresh);
			return false;
		}
		list = host_delta_list_GuardExclusive_list_mut(self.open->open.guard, keyed->closes.shape, keyed->closes.lock,
													   TXN_MEMTABLE_INDEX_PAGES);
		if (!host_delta_list_ListMut_push_page(&list, hold, &self.target.u.fresh, spare, &name, error) ||
			!host_delta_Hold_page(hold, name, &page, error))
			return false;
		exchange.u.exchange.f0 = host_shared_Record_atomic(record, keyed->newest, load);
		exchange.u.exchange.f1 = host_delta_PageRef_words(&page);
		host_shared_Record_atomic(record, keyed->newest, exchange);
	}
	*result = self.open;
	return true;
}
