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
 * The C port of txn's memtable/index.rs: a range's key index, a skip list of
 * key nodes on the range's own pages, each node its key's bytes, a word of
 * where they are, and one forward link a level. It is inserted to under the
 * range's lock held exclusively, only while its record is open, every
 * fallible step before the store that publishes a node, and walked under the
 * lock either way.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "blake3.h"
#include "doltgres/host/delta.h"
#include "doltgres/host/delta/list.h"
#include "doltgres/host/delta/page.h"
#include "doltgres/std/cmp.h"
#include "doltgres/std/num.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/index.h"
#include "doltgres/txn/memtable/pages.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "memtable" STD_SEP "index.rs"

/* TXN_MEMTABLE_INDEX_LENGTH is LENGTH, the first bit of a key's length in its node's place word, above the key's offset. */
#define TXN_MEMTABLE_INDEX_LENGTH 32

/* TXN_MEMTABLE_INDEX_LEVELS is LEVELS, the first bit of a node's height in its place word, above its key's length. */
#define TXN_MEMTABLE_INDEX_LEVELS 56

/* A range's pages are at most the delta's slots, so a page's index fits a node's name above its word. */
StaticAssertDecl(HOST_DELTA_PAGE_SLOTS <= UINT64CONST(1) << 32, "a page's index fits a node's name");

/* txn_memtable_index_internal returns an internal error of the index. */
static HostError
txn_memtable_index_internal(const char *text)
{
	StdString	message;

	std_string_from_Checked(&message, text, strlen(text));
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* txn_memtable_index_closed loads the record's Closed atomic under its lock either way. */
bool
txn_memtable_index_closed(const TxnMemtableIndexClosesOps *closes, const HostSharedGuard *guard)
{
	HostSharedOp op = {.tag = HOST_SHARED_OP_LOAD};

	return host_shared_Record_atomic(host_shared_Guard_record(guard), closes->closed, op) != 0;
}

/*
 * txn_memtable_index_open loads the record's Closed atomic under its lock
 * held exclusively and, finding it clear, returns the value every change to
 * the record's index takes, or nothing for a closed record, which takes no
 * change.
 */
bool
txn_memtable_index_open(const TxnMemtableIndexClosesOps *closes, HostSharedGuard *guard, TxnMemtableIndexOpen *result)
{
	if (txn_memtable_index_closed(closes, guard))
		return false;
	result->closes = closes;
	result->guard = guard;
	return true;
}

/*
 * txn_memtable_index_keyed_open opens a Keyed shape's record as open does,
 * the open carrying the shape's Closes beside its Keyed.
 */
bool
txn_memtable_index_keyed_open(const TxnMemtableIndexKeyedOps *keyed, HostSharedGuard *guard,
							  TxnMemtableIndexKeyedOpen *result)
{
	result->keyed = keyed;
	return txn_memtable_index_open(&keyed->closes, guard, &result->open);
}

/*
 * txn_memtable_index_Open_close closes the record's first list, whose pages
 * its retirement then reads with no store, and then marks the record closed,
 * the last store it takes.
 */
void
txn_memtable_index_Open_close(TxnMemtableIndexOpen self)
{
	HostDeltaListListMut list = host_delta_list_GuardExclusive_list_mut(self.guard, self.closes->shape,
																		self.closes->lock, TXN_MEMTABLE_INDEX_PAGES);
	HostSharedOp op = {.tag = HOST_SHARED_OP_EXCHANGE, .u.exchange = {0, 1}};

	host_delta_list_ListMut_close(&list);
	host_shared_Record_atomic(host_shared_Guard_record(self.guard), self.closes->closed, op);
}

/* txn_memtable_index_Node_at returns the name of the node whose first link is a word of a page of the range's list. */
static TxnMemtableIndexNode
txn_memtable_index_Node_at(uint64 page, uint64 word)
{
	TxnMemtableIndexNode node = {STD_SHL_FOLDED(u64, page, 32) | word};

	return node;
}

/* txn_memtable_index_Node_named returns the node a link names, or nothing for a link to none. */
static bool
txn_memtable_index_Node_named(uint64 link, TxnMemtableIndexNode *result)
{
	if (link == TXN_MEMTABLE_INDEX_NONE)
		return false;
	result->f0 = link;
	return true;
}

/* txn_memtable_index_Node_page returns the index of the node's page in the range's list of pages. */
uint64
txn_memtable_index_Node_page(TxnMemtableIndexNode self)
{
	return STD_SHR_FOLDED(u64, self.f0, 32);
}

/* txn_memtable_index_Node_word returns the index of the node's first link word in its page. */
static uint64
txn_memtable_index_Node_word(TxnMemtableIndexNode self)
{
	return self.f0 & PG_UINT32_MAX;
}

/* txn_memtable_index_least returns the smallest size of a range page that holds a node of a key alone, of the full height. */
HostDeltaPageSize
txn_memtable_index_least(const uint8 *key, size_t key_len)
{
	HostDeltaPageSize size = {(uint32) key_len,
	(uint32) TXN_MEMTABLE_INDEX_LEAST_WORDS};

	return size;
}

/*
 * txn_memtable_index_height returns a node's height from a hash of its key
 * keyed by the server's seed: one more than the count of the hash's leading
 * 2-bit groups that are zero, at most HEIGHT, so that each level holds about
 * a quarter of the nodes of the level below. A height shapes nothing stored
 * or returned.
 */
uint64
txn_memtable_index_height(const uint8 seed[32], const uint8 *key, size_t key_len)
{
	blake3_hasher hasher;
	uint8		hash[32];
	uint64		height;

	blake3_hasher_init_keyed(&hasher, seed);
	blake3_hasher_update(&hasher, key, key_len);
	blake3_hasher_finalize(&hasher, hash, 32);
	height = STD_ADD(u64, 1, STD_DIV_FOLDED(u32, std_leading_zeros_u64(std_u64_from_be_bytes(hash)), 2),
					 147, 5);
	return height < TXN_MEMTABLE_INDEX_HEIGHT ? height : TXN_MEMTABLE_INDEX_HEIGHT;
}

/* TxnMemtableIndexPlace is Place: a node's place, its page, opened under the hold, and the index of its first link word. */
typedef struct TxnMemtableIndexPlace
{
	HostDeltaPageRef page;
	uint64		word;
} TxnMemtableIndexPlace;

/* txn_memtable_index_Place_cursor opens a node's page through the range's list of pages with a walk's cursor. */
static bool
txn_memtable_index_Place_cursor(const TxnMemtableIndexClosesOps *closes, const HostSharedGuard *guard,
								const HostDeltaHold *hold, HostDeltaListCursor *cursor, TxnMemtableIndexNode node,
								TxnMemtableIndexPlace *result, HostError *error)
{
	HostDeltaListListView list = host_delta_list_Guard_list(guard, closes->shape, TXN_MEMTABLE_INDEX_PAGES);
	uint64		name;

	if (!host_delta_list_ListView_get_at(&list, hold, cursor, txn_memtable_index_Node_page(node), &name, error) ||
		!host_delta_Hold_page(hold, host_delta_page_Page_named(name), &result->page, error))
		return false;
	result->word = txn_memtable_index_Node_word(node);
	return true;
}

/* txn_memtable_index_Place_at opens a node's page through the range's list of pages. */
static bool
txn_memtable_index_Place_at(const TxnMemtableIndexClosesOps *closes, const HostSharedGuard *guard,
							const HostDeltaHold *hold, TxnMemtableIndexNode node, TxnMemtableIndexPlace *result,
							HostError *error)
{
	HostDeltaListCursor cursor = {0};

	return txn_memtable_index_Place_cursor(closes, guard, hold, &cursor, node, result, error);
}

/* txn_memtable_index_Place_head returns the node's head word, which names its key's newest version. */
static bool
txn_memtable_index_Place_head(const TxnMemtableIndexPlace *self, HostDeltaWord *result, HostError *error)
{
	return host_delta_PageRef_word(&self->page, STD_SUB(u64, self->word, 2, 165, 24), result, error);
}

/* txn_memtable_index_Place_placed returns the node's place word: its key's offset, its key's length, and its height. */
static bool
txn_memtable_index_Place_placed(const TxnMemtableIndexPlace *self, uint64 *result, HostError *error)
{
	HostDeltaWord word;

	if (!host_delta_PageRef_word(&self->page, STD_SUB(u64, self->word, 1, 170, 27), &word, error))
		return false;
	*result = host_delta_Word_load(&word);
	return true;
}

/* txn_memtable_index_Place_key lends the node's key. */
static bool
txn_memtable_index_Place_key(const TxnMemtableIndexPlace *self, HostDeltaLent *result, HostError *error)
{
	uint64		placed;

	if (!txn_memtable_index_Place_placed(self, &placed, error))
		return false;
	return host_delta_PageRef_bytes(&self->page, placed & PG_UINT32_MAX,
									STD_SHR_FOLDED(u64, placed, TXN_MEMTABLE_INDEX_LENGTH) & TXN_MEMTABLE_INDEX_LONGEST,
									result, error);
}

/* txn_memtable_index_Place_link returns the node's link word at a level below its height. */
static bool
txn_memtable_index_Place_link(const TxnMemtableIndexPlace *self, uint64 level, HostDeltaWord *result,
							  HostError *error)
{
	return host_delta_PageRef_word(&self->page, STD_ADD(u64, self->word, level, 181, 24), result,
								   error);
}

/* txn_memtable_index_Place_loaded loads the node's link at a level below its height. */
static bool
txn_memtable_index_Place_loaded(const TxnMemtableIndexPlace *self, uint64 level, uint64 *result, HostError *error)
{
	HostDeltaWord word;

	if (!txn_memtable_index_Place_link(self, level, &word, error))
		return false;
	*result = host_delta_Word_load(&word);
	return true;
}

/*
 * txn_memtable_index_Place_cmp compares a key with the key of the node at an
 * opened place, lent and given back at once, answering false on an error.
 */
static bool
txn_memtable_index_Place_cmp(const TxnMemtableIndexPlace *self, const uint8 *key, size_t key_len,
							 StdOrdering *result, HostError *error)
{
	HostDeltaLent lent;
	StdBytes	bytes;

	if (!txn_memtable_index_Place_key(self, &lent, error))
		return false;
	bytes = host_delta_Lent_Deref_deref(&lent);
	*result = std_bytes_Ord_cmp(key, key_len, bytes.ptr, bytes.len);
	host_delta_Lent_drop(&lent);
	return true;
}

/*
 * txn_memtable_index_Place_next loads the link at a level of the node at an
 * opened place afresh, answering false on an error and setting some when it
 * names a node.
 */
static bool
txn_memtable_index_Place_next(const TxnMemtableIndexPlace *self, uint64 level, bool *some,
							  TxnMemtableIndexNode *result, HostError *error)
{
	uint64		link;

	if (!txn_memtable_index_Place_loaded(self, level, &link, error))
		return false;
	*some = txn_memtable_index_Node_named(link, result);
	return true;
}

/*
 * txn_memtable_index_key_cmp compares a key with a node's key, lent and
 * given back at once, answering false on an error.
 */
static bool
txn_memtable_index_key_cmp(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						   const HostDeltaHold *hold, TxnMemtableIndexNode node, const uint8 *key, size_t key_len,
						   StdOrdering *result, HostError *error)
{
	TxnMemtableIndexPlace place;

	return txn_memtable_index_Place_at(&keyed->closes, guard, hold, node, &place, error) &&
		txn_memtable_index_Place_cmp(&place, key, key_len, result, error);
}

/*
 * txn_memtable_index_next_at loads the link at a level of the node at a
 * place, answering false on an error and setting some when it names a node.
 */
static bool
txn_memtable_index_next_at(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						   const HostDeltaHold *hold, TxnMemtableIndexNode node, uint64 level, bool *some,
						   TxnMemtableIndexNode *result, HostError *error)
{
	TxnMemtableIndexPlace place;

	return txn_memtable_index_Place_at(&keyed->closes, guard, hold, node, &place, error) &&
		txn_memtable_index_Place_next(&place, level, some, result, error);
}

/* txn_memtable_index_first returns the index's first node, or nothing while it holds no key. */
bool
txn_memtable_index_first(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						 TxnMemtableIndexNode *result)
{
	HostSharedOp op = {.tag = HOST_SHARED_OP_LOAD};

	return txn_memtable_index_Node_named(host_shared_Record_atomic(host_shared_Guard_record(guard), keyed->first, op),
										 result);
}

/* txn_memtable_index_moved_frame_0_t is moved's state across its pad: the heads, and the key lent, while live. */
typedef struct txn_memtable_index_moved_frame_0_t
{
	const TxnMemtableIndexKeyedOps *keyed;
	const TxnMemtableIndexOpen *open;
	const HostDeltaHold *hold;
	uint64		entry;
	HostDeltaOwned *copy;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	StdVec		heads;
	bool		heads_live;
	HostDeltaLent key;
	bool		key_live;
} txn_memtable_index_moved_frame_0_t;

/* txn_memtable_index_moved_pad_0 is moved's pad: the key, then the heads, while live. */
static void
txn_memtable_index_moved_pad_0(void *frame_)
{
	txn_memtable_index_moved_frame_0_t *frame = frame_;

	if (frame->key_live)
	{
		frame->key_live = false;
		host_delta_Lent_drop(&frame->key);
	}
	if (frame->heads_live)
	{
		frame->heads_live = false;
		std_vec_free_Checked(&frame->heads, sizeof(uint64), _Alignof(uint64));
	}
}

/* txn_memtable_index_moved_copied copies one node at a place onto the copy, answering false on an error. */
static bool
txn_memtable_index_moved_copied(txn_memtable_index_moved_frame_0_t *frame, const TxnMemtableIndexPlace *place,
								TxnMemtableIndexNode node, uint64 head)
{
	uint64		placed;
	StdBytes	bytes;
	uint32		offset;
	uint64		word = txn_memtable_index_Node_word(node);

	if (!txn_memtable_index_Place_placed(place, &placed, frame->error) ||
		!txn_memtable_index_Place_key(place, &frame->key, frame->error))
		return false;
	frame->key_live = true;
	bytes = host_delta_Lent_Deref_deref(&frame->key);
	if (!host_delta_Owned_append(frame->copy, bytes.ptr, bytes.len, &offset, frame->error))
		return false;
	frame->key_live = false;
	host_delta_Lent_drop(&frame->key);
	if (!host_delta_Owned_store(frame->copy, STD_SUB(u64, word, 2, 204, 24), head, frame->error) ||
		!host_delta_Owned_store(frame->copy, STD_SUB(u64, word, 1, 205, 24),
								STD_SHL_FOLDED(u64, STD_SHR_FOLDED(u64, placed, TXN_MEMTABLE_INDEX_LENGTH),
											   TXN_MEMTABLE_INDEX_LENGTH) | offset, frame->error))
		return false;
	for (uint64 level = 0, levels = STD_SHR_FOLDED(u64, placed, TXN_MEMTABLE_INDEX_LEVELS); level < levels; level++)
	{
		uint64		link;

		if (!txn_memtable_index_Place_loaded(place, level, &link, frame->error) ||
			!host_delta_Owned_store(frame->copy, STD_ADD(u64, word, level, 207, 28), link,
									frame->error))
			return false;
	}
	return true;
}

/* txn_memtable_index_moved_body_0 is moved's body. */
static void
txn_memtable_index_moved_body_0(void *frame_)
{
	txn_memtable_index_moved_frame_0_t *frame = frame_;
	const HostSharedGuard *guard = frame->open->guard;
	const TxnMemtableIndexKeyedOps *keyed = frame->keyed;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	TxnMemtableIndexNode node;
	bool		some;

	std_vec_new(&frame->heads, sizeof(uint64), _Alignof(uint64));
	frame->heads_live = true;
	if (!std_vec_try_reserve(&frame->heads,
							 (size_t) host_shared_Record_atomic(host_shared_Guard_record(guard), keyed->count, load),
							 sizeof(uint64), _Alignof(uint64)))
	{
		*frame->error = host_Error_out_of_memory();
		txn_memtable_index_moved_pad_0(frame);
		return;
	}
	for (some = txn_memtable_index_first(keyed, guard, &node); some;)
	{
		TxnMemtableIndexPlace place;
		HostDeltaWord word;
		uint64		head;
		uint64		link;

		if (!txn_memtable_index_Place_at(&keyed->closes, guard, frame->hold, node, &place, frame->error) ||
			!txn_memtable_index_Place_head(&place, &word, frame->error))
		{
			txn_memtable_index_moved_pad_0(frame);
			return;
		}
		head = host_delta_Word_load(&word);
		if (txn_memtable_index_Node_page(node) == frame->entry &&
			!txn_memtable_index_moved_copied(frame, &place, node, head))
		{
			txn_memtable_index_moved_pad_0(frame);
			return;
		}
		std_vec_push_Checked(&frame->heads, &head, sizeof(head), _Alignof(uint64));
		if (!txn_memtable_index_Place_loaded(&place, 0, &link, frame->error))
		{
			txn_memtable_index_moved_pad_0(frame);
			return;
		}
		some = txn_memtable_index_Node_named(link, &node);
	}
	frame->heads_live = false;
	*frame->result = frame->heads;
	frame->ok = true;
}

/*
 * txn_memtable_index_moved copies the nodes of an open record's index that
 * are on a page of its list onto the page's copy, a compaction's: each
 * node's key appended to the copy and its words stored at the same indexes,
 * with only its key's offset anew. It returns the head of every node of the
 * index, for the copy of the page's lines from their chains.
 */
bool
txn_memtable_index_moved(const TxnMemtableIndexKeyedOpen *kopen,
						 const HostDeltaHold *hold, uint64 entry,
						 HostDeltaOwned *copy, StdVec *result, HostError *error)
{
	txn_memtable_index_moved_frame_0_t frame = {.keyed = kopen->keyed, .open = &kopen->open, .hold = hold,
		.entry = entry,
		.copy = copy,
	.result = result, .error = error};

	std_landing(&frame, txn_memtable_index_moved_body_0, txn_memtable_index_moved_pad_0);
	return frame.ok;
}

/* txn_memtable_index_key lends a node's key for as long as the hold lasts. */
bool
txn_memtable_index_key(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard, const HostDeltaHold *hold,
					   TxnMemtableIndexNode node, HostDeltaLent *result, HostError *error)
{
	TxnMemtableIndexPlace place;

	return txn_memtable_index_Place_at(&keyed->closes, guard, hold, node, &place, error) &&
		txn_memtable_index_Place_key(&place, result, error);
}

/* txn_memtable_index_head returns the handle of a node's key's newest version. */
bool
txn_memtable_index_head(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						const HostDeltaHold *hold, TxnMemtableIndexNode node, uint64 *result, HostError *error)
{
	TxnMemtableIndexPlace place;
	HostDeltaWord word;

	if (!txn_memtable_index_Place_at(&keyed->closes, guard, hold, node, &place, error) ||
		!txn_memtable_index_Place_head(&place, &word, error))
		return false;
	*result = host_delta_Word_load(&word);
	return true;
}

/*
 * txn_memtable_index_set_head makes a version its node's key's newest in an
 * open record, which publishes it, borrowing the record's Open for the
 * store, which follows every step that can fail.
 */
bool
txn_memtable_index_set_head(const TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold, TxnMemtableIndexNode node,
							uint64 head, HostError *error)
{
	TxnMemtableIndexPlace place;
	HostDeltaWord word;

	if (!txn_memtable_index_Place_at(open->open.closes, open->open.guard, hold, node, &place, error) ||
		!txn_memtable_index_Place_head(&place, &word, error))
		return false;
	host_delta_Word_store(&word, head);
	return true;
}

/*
 * TxnMemtableIndexPath is Path: where a key's search ended, the last node
 * before the key at each level, none at all for a key at or before the first
 * node's, and the node at or after the key.
 */
typedef struct TxnMemtableIndexPath
{
	bool		has_before;
	TxnMemtableIndexNode before[TXN_MEMTABLE_INDEX_HEIGHT];
	bool		has_at;
	TxnMemtableIndexNode at;
} TxnMemtableIndexPath;

/*
 * txn_memtable_index_path searches the index for a key from its first node,
 * at each level from the highest down following links while the next node's
 * key is below the key. It opens each node it reaches once and keeps its
 * place, a page the hold keeps open, loading each link from it afresh, so a
 * link stored since is seen as a reopen would see it (bugs row 355). A node
 * on the page of the node before it takes that page, since its caller holds
 * the range's lock through the walk and the range's list changes its entries
 * only under that lock held exclusively.
 */
static bool
txn_memtable_index_path(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						const HostDeltaHold *hold, const uint8 *key, size_t key_len, TxnMemtableIndexPath *result,
						HostError *error)
{
	TxnMemtableIndexNode first;
	TxnMemtableIndexNode at;
	TxnMemtableIndexPlace at_place;
	HostDeltaListCursor cursor = {0};
	StdOrdering order;

	result->has_before = false;
	result->has_at = false;
	if (!txn_memtable_index_first(keyed, guard, &first))
		return true;
	if (!txn_memtable_index_Place_cursor(&keyed->closes, guard, hold, &cursor, first, &at_place, error) ||
		!txn_memtable_index_Place_cmp(&at_place, key, key_len, &order, error))
		return false;
	if (order != STD_ORDERING_GREATER)
	{
		result->has_at = true;
		result->at = first;
		return true;
	}
	at = first;
	for (uint64 level = TXN_MEMTABLE_INDEX_HEIGHT; level-- > 0;)
	{
		for (;;)
		{
			bool		some;
			TxnMemtableIndexNode next;
			TxnMemtableIndexPlace next_place;

			if (!txn_memtable_index_Place_next(&at_place, level, &some, &next, error))
				return false;
			if (!some)
				break;
			if (txn_memtable_index_Node_page(next) == txn_memtable_index_Node_page(at))
			{
				next_place.page = at_place.page;
				next_place.word = txn_memtable_index_Node_word(next);
			}
			else if (!txn_memtable_index_Place_cursor(&keyed->closes, guard, hold, &cursor, next, &next_place,
													  error))
				return false;
			if (!txn_memtable_index_Place_cmp(&next_place, key, key_len, &order, error))
				return false;
			if (order != STD_ORDERING_GREATER)
				break;
			at = next;
			at_place = next_place;
		}
		result->before[level] = at;
	}
	result->has_before = true;
	return txn_memtable_index_Place_next(&at_place, 0, &result->has_at, &result->at, error);
}

/*
 * txn_memtable_index_node_is reports whether a node's key is a key,
 * answering false on an error.
 */
static bool
txn_memtable_index_node_is(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						   const HostDeltaHold *hold, TxnMemtableIndexNode node, const uint8 *key, size_t key_len,
						   bool *result, HostError *error)
{
	StdOrdering order;

	if (!txn_memtable_index_key_cmp(keyed, guard, hold, node, key, key_len, &order, error))
		return false;
	*result = order == STD_ORDERING_EQUAL;
	return true;
}

/*
 * txn_memtable_index_unlink takes a node out of an open record's index at
 * every level of its height, from the highest down, its predecessors found
 * by the search for its key under the record's exclusive lock, and counts
 * its key out and its record and its key's bytes dead on its page; it finds
 * every link word before it stores any. The record's first node, the range's
 * first key's, never leaves.
 */
bool
txn_memtable_index_unlink(const TxnMemtableIndexKeyedOpen *kopen,
						  const HostDeltaHold *hold, TxnMemtableIndexNode node,
						  HostError *error)
{
	const TxnMemtableIndexKeyedOps *keyed = kopen->keyed;
	const TxnMemtableIndexOpen *open = &kopen->open;
	const HostSharedGuard *guard = open->guard;
	TxnMemtableIndexPlace place;
	HostDeltaLent key;
	StdBytes	bytes;
	uint64		placed;
	uint64		levels;
	TxnMemtableIndexPath path;
	HostDeltaWord words[TXN_MEMTABLE_INDEX_HEIGHT];
	uint64		nexts[TXN_MEMTABLE_INDEX_HEIGHT];
	HostDeltaWord counted;
	HostSharedRecord record = host_shared_Guard_record(guard);
	HostSharedOp count = {.tag = HOST_SHARED_OP_ADD, .u.add = PG_UINT64_MAX};
	HostSharedOp sum = {.tag = HOST_SHARED_OP_ADD};

	if (!txn_memtable_index_Place_at(&keyed->closes, guard, hold, node, &place, error) ||
		!txn_memtable_index_Place_key(&place, &key, error))
		return false;
	bytes = host_delta_Lent_Deref_deref(&key);
	if (!txn_memtable_index_Place_placed(&place, &placed, error) ||
		!txn_memtable_index_path(keyed, guard, hold, bytes.ptr, bytes.len, &path, error))
	{
		host_delta_Lent_drop(&key);
		return false;
	}
	if (!path.has_before)
	{
		host_delta_Lent_drop(&key);
		*error = txn_memtable_index_internal("a removal unlinks a record's first node or a node it can't reach");
		return false;
	}
	levels = STD_SHR_FOLDED(u64, placed, TXN_MEMTABLE_INDEX_LEVELS);
	for (uint64 level = 0; level < levels; level++)
	{
		TxnMemtableIndexPlace before;

		if (level >= TXN_MEMTABLE_INDEX_HEIGHT)
			host_delta_Lent_drop(&key);
		std_index_check(level, TXN_MEMTABLE_INDEX_HEIGHT, RUST_FILE, 256, 43);
		if (!txn_memtable_index_Place_at(&keyed->closes, guard, hold, path.before[level], &before, error) ||
			!txn_memtable_index_Place_link(&before, level, &words[level], error))
		{
			host_delta_Lent_drop(&key);
			return false;
		}
		if (host_delta_Word_load(&words[level]) != node.f0)
		{
			host_delta_Lent_drop(&key);
			*error = txn_memtable_index_internal("a removal unlinks a record's first node or a node it can't reach");
			return false;
		}
		if (!txn_memtable_index_Place_loaded(&place, level, &nexts[level], error))
		{
			host_delta_Lent_drop(&key);
			return false;
		}
		if (level >= TXN_MEMTABLE_INDEX_HEIGHT)
			host_delta_Lent_drop(&key);
		std_index_check(level, TXN_MEMTABLE_INDEX_HEIGHT, RUST_FILE, 260, 9);
	}
	for (uint64 level = levels; level-- > 0;)
		host_delta_Word_store(&words[level], nexts[level]);
	if (!host_delta_PageRef_word(&place.page, TXN_MEMTABLE_PAGES_LIVE, &counted, error))
	{
		host_delta_Lent_drop(&key);
		return false;
	}
	host_delta_Word_fetch_add(&counted, PG_UINT64_MAX);
	if (!host_delta_PageRef_word(&place.page, TXN_MEMTABLE_PAGES_DEAD, &counted, error))
	{
		host_delta_Lent_drop(&key);
		return false;
	}
	host_delta_Word_fetch_add(&counted, (uint64) bytes.len);
	host_shared_Record_atomic(record, keyed->count, count);
	sum.u.add = std_wrapping_sub_u64(0, (uint64) bytes.len);
	host_shared_Record_atomic(record, keyed->bytes, sum);
	host_delta_Lent_drop(&key);
	return true;
}

/*
 * txn_memtable_index_begin writes an unlinked record's first node on its
 * first page of its own while both are still owned: its key at the full
 * height, naming no version and linking to no node, after the page's
 * counts, which count it as the page's one live record and the record's
 * list as the one list naming the page; the node is named by the page's
 * index in the record's list, and the record's First names it and its counts
 * count its key.
 */
bool
txn_memtable_index_begin(const TxnMemtableIndexKeyedOps *keyed, const HostDeltaMade *made, HostDeltaOwned *page,
						 uint64 index, const uint8 *key, size_t key_len, HostError *error)
{
	uint32		offset;
	uint64		at = TXN_MEMTABLE_PAGES_FIRST;
	HostSharedOp op = {.tag = HOST_SHARED_OP_EXCHANGE};

	if (key_len > TXN_MEMTABLE_INDEX_LONGEST)
	{
		*error = txn_memtable_index_internal("a key is longer than a node of a range's index holds");
		return false;
	}
	if (!host_delta_Owned_append(page, key, key_len, &offset, error) ||
		!host_delta_Owned_store(page, at, TXN_MEMTABLE_INDEX_NONE, error) ||
		!host_delta_Owned_store(page, STD_ADD(u64, at, 1, 287, 16),
								(uint64) offset | STD_SHL_FOLDED(u64, (uint64) key_len, TXN_MEMTABLE_INDEX_LENGTH) |
								STD_SHL_FOLDED(u64, TXN_MEMTABLE_INDEX_HEIGHT, TXN_MEMTABLE_INDEX_LEVELS), error))
		return false;
	for (uint64 level = 0; level < TXN_MEMTABLE_INDEX_HEIGHT; level++)
		if (!host_delta_Owned_store(page,
									STD_ADD(u64, STD_ADD(u64, at, TXN_MEMTABLE_INDEX_WORDS, 289, 20),
											level, 289, 20),
									TXN_MEMTABLE_INDEX_NONE, error))
			return false;
	if (!host_delta_Owned_store(page, TXN_MEMTABLE_PAGES_COUNT,
								STD_ADD(u64, STD_ADD(u64, at, TXN_MEMTABLE_INDEX_WORDS, 291, 30),
										TXN_MEMTABLE_INDEX_HEIGHT, 291, 30), error) ||
		!host_delta_Owned_store(page, TXN_MEMTABLE_PAGES_LIVE, 1, error) ||
		!host_delta_Owned_store(page, TXN_MEMTABLE_PAGES_LISTS, 1, error))
		return false;
	op.u.exchange.f0 = TXN_MEMTABLE_INDEX_NONE;
	op.u.exchange.f1 = txn_memtable_index_Node_at(index, STD_ADD(u64, at, TXN_MEMTABLE_INDEX_WORDS,
																 294, 64)).f0;
	host_delta_Made_atomic(made, keyed->first, op);
	op.u.exchange.f0 = 0;
	op.u.exchange.f1 = 1;
	host_delta_Made_atomic(made, keyed->count, op);
	op.u.exchange.f1 = (uint64) key_len;
	host_delta_Made_atomic(made, keyed->bytes, op);
	return true;
}

/*
 * txn_memtable_index_begun lends the first key of a record whose first page
 * is opened under a hold, the key begin wrote there, which never changes.
 */
bool
txn_memtable_index_begun(HostDeltaPageRef page, HostDeltaLent *result, HostError *error)
{
	TxnMemtableIndexPlace place = {page, TXN_MEMTABLE_PAGES_FIRST + TXN_MEMTABLE_INDEX_WORDS};

	return txn_memtable_index_Place_key(&place, result, error);
}

/* txn_memtable_index_Copied_drop is Copied's drop glue: its key's lend. */
void
txn_memtable_index_Copied_drop(TxnMemtableIndexCopied *self)
{
	host_delta_Lent_drop(&self->key);
}

/* txn_memtable_index_copied_frame_0_t is copied's state across its pad: the nodes read, while live. */
typedef struct txn_memtable_index_copied_frame_0_t
{
	const TxnMemtableIndexKeyedOps *keyed;
	const HostSharedGuard *guard;
	const HostDeltaHold *hold;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
	StdVec		nodes;
	bool		nodes_live;
} txn_memtable_index_copied_frame_0_t;

/* txn_memtable_index_copied_pad_0 is copied's pad: the nodes read, while live. */
static void
txn_memtable_index_copied_pad_0(void *frame_)
{
	txn_memtable_index_copied_frame_0_t *frame = frame_;

	if (frame->nodes_live)
	{
		frame->nodes_live = false;
		for (size_t k = 0, k_len = frame->nodes.len; k < k_len; k++)
			txn_memtable_index_Copied_drop(&STD_VEC_AT(&frame->nodes, TxnMemtableIndexCopied, k));
		std_vec_free_Checked(&frame->nodes, sizeof(TxnMemtableIndexCopied), _Alignof(TxnMemtableIndexCopied));
	}
}

/* txn_memtable_index_copied_body_0 is copied's body. */
static void
txn_memtable_index_copied_body_0(void *frame_)
{
	txn_memtable_index_copied_frame_0_t *frame = frame_;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	TxnMemtableIndexNode node;
	bool		some;

	std_vec_new(&frame->nodes, sizeof(TxnMemtableIndexCopied), _Alignof(TxnMemtableIndexCopied));
	frame->nodes_live = true;
	if (!std_vec_try_reserve(&frame->nodes,
							 (size_t) host_shared_Record_atomic(host_shared_Guard_record(frame->guard),
																frame->keyed->count, load),
							 sizeof(TxnMemtableIndexCopied), _Alignof(TxnMemtableIndexCopied)))
	{
		*frame->error = host_Error_out_of_memory();
		txn_memtable_index_copied_pad_0(frame);
		return;
	}
	for (some = txn_memtable_index_first(frame->keyed, frame->guard, &node); some;)
	{
		TxnMemtableIndexPlace place;
		TxnMemtableIndexCopied copied;
		uint64		placed;
		HostDeltaWord head;
		uint64		link;

		if (!txn_memtable_index_Place_at(&frame->keyed->closes, frame->guard, frame->hold, node, &place,
										 frame->error) ||
			!txn_memtable_index_Place_placed(&place, &placed, frame->error) ||
			!txn_memtable_index_Place_key(&place, &copied.key, frame->error))
		{
			txn_memtable_index_copied_pad_0(frame);
			return;
		}
		if (!txn_memtable_index_Place_head(&place, &head, frame->error))
		{
			host_delta_Lent_drop(&copied.key);
			txn_memtable_index_copied_pad_0(frame);
			return;
		}
		copied.head = host_delta_Word_load(&head);
		copied.height = STD_SHR_FOLDED(u64, placed, TXN_MEMTABLE_INDEX_LEVELS);
		copied.page = txn_memtable_index_Node_page(node);
		std_vec_push_Checked(&frame->nodes, &copied, sizeof(copied), _Alignof(TxnMemtableIndexCopied));
		if (!txn_memtable_index_Place_loaded(&place, 0, &link, frame->error))
		{
			txn_memtable_index_copied_pad_0(frame);
			return;
		}
		some = txn_memtable_index_Node_named(link, &node);
	}
	frame->nodes_live = false;
	*frame->result = frame->nodes;
	frame->ok = true;
}

/* txn_memtable_index_copied reads every node of a record's index in key order from its first, under its lock either way. */
bool
txn_memtable_index_copied(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						  const HostDeltaHold *hold, StdVec *result, HostError *error)
{
	txn_memtable_index_copied_frame_0_t frame = {.keyed = keyed, .guard = guard, .hold = hold, .result = result,
	.error = error};

	std_landing(&frame, txn_memtable_index_copied_body_0, txn_memtable_index_copied_pad_0);
	return frame.ok;
}

/* txn_memtable_index_need returns the bytes and the words a node of a key's length and a height takes on a page. */
std_TupleU64U64_t
txn_memtable_index_need(uint64 key, uint64 height)
{
	std_TupleU64U64_t need = {key, STD_ADD(u64, TXN_MEMTABLE_INDEX_WORDS, height, 334, 11)};

	return need;
}

/* txn_memtable_index_built_frame_0_t is built's state across its pad: the pages' counts, while live. */
typedef struct txn_memtable_index_built_frame_0_t
{
	const TxnMemtableIndexKeyedOps *keyed;
	const HostDeltaMade *made;
	HostDeltaOwned *pages;
	size_t		pages_len;
	const size_t *plan;
	size_t		plan_len;
	uint64		base;
	const TxnMemtableIndexCopied *nodes;
	size_t		nodes_len;
	HostError  *error;
	bool		ok;
	StdVec		counts;
	bool		counts_live;
} txn_memtable_index_built_frame_0_t;

/* txn_memtable_index_built_pad_0 is built's pad: the pages' counts, while live. */
static void
txn_memtable_index_built_pad_0(void *frame_)
{
	txn_memtable_index_built_frame_0_t *frame = frame_;

	if (frame->counts_live)
	{
		frame->counts_live = false;
		std_vec_free_Checked(&frame->counts, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	}
}

/* TxnMemtableIndexLast is a level's last node built so far: its page's place in the plan and its link word. */
typedef struct TxnMemtableIndexLast
{
	bool		has;
	size_t		page;
	uint64		word;
} TxnMemtableIndexLast;

/* txn_memtable_index_built_node writes one node of built's on its page, answering false on an error. */
static bool
txn_memtable_index_built_node(txn_memtable_index_built_frame_0_t *frame, size_t index, size_t page,
							  TxnMemtableIndexLast last[TXN_MEMTABLE_INDEX_HEIGHT], uint64 *bytes)
{
	const TxnMemtableIndexCopied *node = &frame->nodes[index];
	uint64		height = index == 0 ? TXN_MEMTABLE_INDEX_HEIGHT : node->height;
	StdBytes	key = host_delta_Lent_Deref_deref(&node->key);
	HostDeltaOwned *owned;
	std_TupleU64U64_t *count;
	uint64		at;
	uint64		live;
	uint32		offset;
	TxnMemtableIndexNode name;

	std_index_check(page, frame->pages_len, RUST_FILE, 356, 22);
	owned = &frame->pages[page];
	if (!host_delta_Owned_append(owned, key.ptr, key.len, &offset, frame->error))
		return false;
	std_index_check(page, frame->counts.len, RUST_FILE, 357, 26);
	count = &STD_VEC_AT(&frame->counts, std_TupleU64U64_t, page);
	at = count->f0;
	live = count->f1;
	std_index_check(page, frame->pages_len, RUST_FILE, 358, 9);
	if (!host_delta_Owned_store(owned, at, node->head, frame->error))
		return false;
	std_index_check(page, frame->pages_len, RUST_FILE, 359, 9);
	if (!host_delta_Owned_store(owned, STD_ADD(u64, at, 1, 359, 27),
								(uint64) offset | STD_SHL_FOLDED(u64, (uint64) key.len, TXN_MEMTABLE_INDEX_LENGTH) |
								STD_SHL_FOLDED(u64, height, TXN_MEMTABLE_INDEX_LEVELS), frame->error))
		return false;
	for (uint64 level = 0; level < height; level++)
	{
		std_index_check(page, frame->pages_len, RUST_FILE, 361, 13);
		if (!host_delta_Owned_store(owned,
									STD_ADD(u64, STD_ADD(u64, at, TXN_MEMTABLE_INDEX_WORDS, 361, 31),
											level, 361, 31),
									TXN_MEMTABLE_INDEX_NONE, frame->error))
			return false;
	}
	name = txn_memtable_index_Node_at(STD_ADD(u64, frame->base, (uint64) page, 363, 29),
									  STD_ADD(u64, at, TXN_MEMTABLE_INDEX_WORDS, 363, 49));
	for (uint64 level = 0; level < height; level++)
	{
		std_index_check(level, TXN_MEMTABLE_INDEX_HEIGHT, RUST_FILE, 365, 43);
		if (last[level].has)
		{
			std_index_check(last[level].page, frame->pages_len, RUST_FILE, 366, 17);
			if (!host_delta_Owned_store(&frame->pages[last[level].page], last[level].word, name.f0, frame->error))
				return false;
		}
		std_index_check(level, TXN_MEMTABLE_INDEX_HEIGHT, RUST_FILE, 368, 13);
		last[level].has = true;
		last[level].page = page;
		last[level].word = STD_ADD(u64, STD_ADD(u64, at, TXN_MEMTABLE_INDEX_WORDS, 368, 48),
								   level, 368, 48);
	}
	std_index_check(page, frame->counts.len, RUST_FILE, 370, 9);
	count->f0 = STD_ADD(u64, STD_ADD(u64, at, TXN_MEMTABLE_INDEX_WORDS, 370, 25), height,
						370, 25);
	count->f1 = STD_ADD(u64, live, 1, 370, 46);
	*bytes = STD_ADD(u64, *bytes, (uint64) key.len, 371, 9);
	if (index == 0)
	{
		HostSharedOp op = {.tag = HOST_SHARED_OP_EXCHANGE, .u.exchange = {TXN_MEMTABLE_INDEX_NONE, name.f0}};

		host_delta_Made_atomic(frame->made, frame->keyed->first, op);
	}
	return true;
}

/* txn_memtable_index_built_body_0 is built's body. */
static void
txn_memtable_index_built_body_0(void *frame_)
{
	txn_memtable_index_built_frame_0_t *frame = frame_;
	TxnMemtableIndexLast last[TXN_MEMTABLE_INDEX_HEIGHT] = {{0}};
	uint64		bytes = 0;
	std_TupleU64U64_t first = {TXN_MEMTABLE_PAGES_FIRST, 0};
	HostSharedOp op = {.tag = HOST_SHARED_OP_EXCHANGE};

	std_vec_new(&frame->counts, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->counts_live = true;
	if (!std_vec_try_reserve(&frame->counts, frame->pages_len, sizeof(std_TupleU64U64_t),
							 _Alignof(std_TupleU64U64_t)))
	{
		*frame->error = host_Error_out_of_memory();
		txn_memtable_index_built_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < frame->pages_len; k++)
		std_vec_push_Checked(&frame->counts, &first, sizeof(first), _Alignof(std_TupleU64U64_t));
	for (size_t index = 0, len = std_min_usize(frame->nodes_len, frame->plan_len); index < len; index++)
		if (!txn_memtable_index_built_node(frame, index, frame->plan[index], last, &bytes))
		{
			txn_memtable_index_built_pad_0(frame);
			return;
		}
	for (size_t k = 0, k_len = std_min_usize(frame->pages_len, frame->counts.len); k < k_len; k++)
	{
		const std_TupleU64U64_t *count = &STD_VEC_AT(&frame->counts, std_TupleU64U64_t, k);

		if (!host_delta_Owned_store(&frame->pages[k], TXN_MEMTABLE_PAGES_COUNT, count->f0, frame->error) ||
			!host_delta_Owned_store(&frame->pages[k], TXN_MEMTABLE_PAGES_LIVE, count->f1, frame->error) ||
			!host_delta_Owned_store(&frame->pages[k], TXN_MEMTABLE_PAGES_LISTS, 1, frame->error))
		{
			txn_memtable_index_built_pad_0(frame);
			return;
		}
	}
	op.u.exchange.f1 = (uint64) frame->nodes_len;
	host_delta_Made_atomic(frame->made, frame->keyed->count, op);
	op.u.exchange.f1 = bytes;
	host_delta_Made_atomic(frame->made, frame->keyed->bytes, op);
	frame->ok = true;
	txn_memtable_index_built_pad_0(frame);
}

/*
 * txn_memtable_index_built writes an unlinked record's index on its own
 * pages while they are owned, from nodes in key order, the first at the full
 * height: each node goes on the page the plan gives it, after the words
 * handed out there before it, with its head and its height, and each level
 * links its nodes in order. The pages' counts count their nodes as live
 * records and the one list naming them; the nodes are named by their pages'
 * indexes in the record's list, the first of them base, and the record's
 * First names its first node and its counts count the keys.
 */
bool
txn_memtable_index_built(const TxnMemtableIndexKeyedOps *keyed, const HostDeltaMade *made, HostDeltaOwned *pages,
						 size_t pages_len, const size_t *plan, size_t plan_len, uint64 base,
						 const TxnMemtableIndexCopied *nodes, size_t nodes_len, HostError *error)
{
	txn_memtable_index_built_frame_0_t frame = {.keyed = keyed, .made = made, .pages = pages, .pages_len = pages_len,
	.plan = plan, .plan_len = plan_len, .base = base, .nodes = nodes, .nodes_len = nodes_len, .error = error};

	std_landing(&frame, txn_memtable_index_built_body_0, txn_memtable_index_built_pad_0);
	return frame.ok;
}

/* txn_memtable_index_next returns the node after one in key order, or nothing after the last. */
bool
txn_memtable_index_next(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						const HostDeltaHold *hold, TxnMemtableIndexNode node, bool *some, TxnMemtableIndexNode *result,
						HostError *error)
{
	return txn_memtable_index_next_at(keyed, guard, hold, node, 0, some, result, error);
}

/* txn_memtable_index_seek returns the first node whose key is at or after a key, or nothing past the last. */
bool
txn_memtable_index_seek(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						const HostDeltaHold *hold, const uint8 *key, size_t key_len, bool *some,
						TxnMemtableIndexNode *result, HostError *error)
{
	TxnMemtableIndexPath path;

	if (!txn_memtable_index_path(keyed, guard, hold, key, key_len, &path, error))
		return false;
	*some = path.has_at;
	if (path.has_at)
		*result = path.at;
	return true;
}

/* txn_memtable_index_find returns the node of a key, or nothing for a key the index doesn't hold. */
bool
txn_memtable_index_find(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						const HostDeltaHold *hold, const uint8 *key, size_t key_len, bool *some,
						TxnMemtableIndexNode *result, HostError *error)
{
	if (!txn_memtable_index_seek(keyed, guard, hold, key, key_len, some, result, error))
		return false;
	return !*some || txn_memtable_index_node_is(keyed, guard, hold, *result, key, key_len, some, error);
}

/* txn_memtable_index_before returns the last node whose key is below a key, or nothing for a key at or before the first node's. */
bool
txn_memtable_index_before(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						  const HostDeltaHold *hold, const uint8 *key, size_t key_len, bool *some,
						  TxnMemtableIndexNode *result, HostError *error)
{
	TxnMemtableIndexPath path;

	if (!txn_memtable_index_path(keyed, guard, hold, key, key_len, &path, error))
		return false;
	*some = path.has_before;
	if (path.has_before)
		*result = path.before[0];
	return true;
}

/*
 * txn_memtable_index_last returns the index's last node, or nothing while it
 * holds no key, following each level's links from the first node, which has
 * every level.
 */
bool
txn_memtable_index_last(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						const HostDeltaHold *hold, bool *some, TxnMemtableIndexNode *result, HostError *error)
{
	*some = txn_memtable_index_first(keyed, guard, result);
	if (!*some)
		return true;
	for (uint64 level = TXN_MEMTABLE_INDEX_HEIGHT; level-- > 0;)
	{
		for (;;)
		{
			bool		has;
			TxnMemtableIndexNode next;

			if (!txn_memtable_index_next_at(keyed, guard, hold, *result, level, &has, &next, error))
				return false;
			if (!has)
				break;
			*result = next;
		}
	}
	return true;
}

/*
 * txn_memtable_index_lookup finds a key's node in an open record, or
 * prepares the node a key the index doesn't hold takes: of the full height
 * before the first node's key, else of its key's height under the server's
 * seed.
 */
bool
txn_memtable_index_lookup(const TxnMemtableIndexKeyedOpen *kopen,
						  const HostDeltaHold *hold, const uint8 *key,
						  size_t key_len, TxnMemtableIndexFound *result, HostError *error)
{
	const TxnMemtableIndexKeyedOps *keyed = kopen->keyed;
	const TxnMemtableIndexOpen *open = &kopen->open;
	TxnMemtableIndexPath path;
	TxnMemtableIndexNew *made = &result->u.new_;
	uint8		seed[32];

	if (key_len > TXN_MEMTABLE_INDEX_LONGEST)
	{
		*error = txn_memtable_index_internal("a key is longer than a node of a range's index holds");
		return false;
	}
	if (!txn_memtable_index_path(keyed, open->guard, hold, key, key_len, &path, error))
		return false;
	if (path.has_at)
	{
		bool		is;

		if (!txn_memtable_index_node_is(keyed, open->guard, hold, path.at, key, key_len, &is, error))
			return false;
		if (is)
		{
			result->tag = TXN_MEMTABLE_INDEX_FOUND_NODE;
			result->u.node = path.at;
			return true;
		}
	}
	result->tag = TXN_MEMTABLE_INDEX_FOUND_NEW;
	made->first = path.has_at ? path.at.f0 : TXN_MEMTABLE_INDEX_NONE;
	made->bytes = (uint64) key_len;
	made->has_before = path.has_before;
	if (!path.has_before)
	{
		made->levels = TXN_MEMTABLE_INDEX_HEIGHT;
		for (size_t level = 0; level < TXN_MEMTABLE_INDEX_HEIGHT; level++)
			made->links[level] = made->first;
		return true;
	}
	host_delta_seed(seed);
	made->levels = txn_memtable_index_height(seed, key, key_len);
	for (size_t level = 0; level < TXN_MEMTABLE_INDEX_HEIGHT; level++)
	{
		made->links[level] = TXN_MEMTABLE_INDEX_NONE;
		made->before[level] = path.before[level];
	}
	for (uint64 level = 0; level < made->levels; level++)
	{
		TxnMemtableIndexPlace place;

		if (!txn_memtable_index_Place_at(&keyed->closes, open->guard, hold, path.before[level], &place, error) ||
			!txn_memtable_index_Place_loaded(&place, level, &made->links[level], error))
			return false;
	}
	return true;
}

/* txn_memtable_index_New_words returns the count of the node's words. */
uint64
txn_memtable_index_New_words(const TxnMemtableIndexNew *self)
{
	return STD_ADD(u64, TXN_MEMTABLE_INDEX_WORDS, self->levels, 498, 9);
}

/*
 * txn_memtable_index_New_store stores the node's head, place word, and links
 * in a call's room, which holds the record's Open, from one of its words on,
 * with its key at an offset of the room's page, and returns the node's name.
 */
bool
txn_memtable_index_New_store(const TxnMemtableIndexNew *self, TxnMemtablePagesRoom *room, uint64 at, uint32 offset,
							 const uint8 *key, size_t key_len, uint64 head, TxnMemtableIndexNode *result,
							 HostError *error)
{
	if (!txn_memtable_pages_Room_store(room, at, head, error) ||
		!txn_memtable_pages_Room_store(room, STD_ADD(u64, at, 1, 512, 20),
									   (uint64) offset |
									   STD_SHL_FOLDED(u64, (uint64) key_len, TXN_MEMTABLE_INDEX_LENGTH) |
									   STD_SHL_FOLDED(u64, self->levels, TXN_MEMTABLE_INDEX_LEVELS), error))
		return false;
	for (uint64 level = 0; level < self->levels; level++)
		if (!txn_memtable_pages_Room_store(room,
										   STD_ADD(u64, STD_ADD(u64, at, TXN_MEMTABLE_INDEX_WORDS,
																514, 24),
												   level, 514, 24),
										   self->links[level], error))
			return false;
	*result = txn_memtable_index_Node_at(room->page,
										 STD_ADD(u64, STD_ADD(u64, room->first, at, 516, 32),
												 TXN_MEMTABLE_INDEX_WORDS, 516, 32));
	return true;
}

/*
 * txn_memtable_index_New_publish stores the node's name in its predecessors'
 * links in an open record from the lowest level up, the first of them
 * publishing it, or as the record's first node, and counts its key,
 * borrowing the record's Open for its stores; it finds every link word, in an
 * array it allocates nothing for, before it stores any.
 */
bool
txn_memtable_index_New_publish(TxnMemtableIndexNew self, const TxnMemtableIndexKeyedOpen *kopen,
							   const HostDeltaHold *hold,
							   TxnMemtableIndexNode node, HostError *error)
{
	const TxnMemtableIndexKeyedOps *keyed = kopen->keyed;
	const TxnMemtableIndexOpen *open = &kopen->open;
	HostSharedRecord record = host_shared_Guard_record(open->guard);
	HostSharedOp op = {.tag = HOST_SHARED_OP_EXCHANGE, .u.exchange = {self.first, node.f0}};

	if (!self.has_before)
		host_shared_Record_atomic(record, keyed->first, op);
	else
	{
		HostDeltaWord words[TXN_MEMTABLE_INDEX_HEIGHT];

		for (uint64 level = 0; level < self.levels; level++)
		{
			TxnMemtableIndexPlace place;

			if (!txn_memtable_index_Place_at(open->closes, open->guard, hold, self.before[level], &place, error) ||
				!txn_memtable_index_Place_link(&place, level, &words[level], error))
				return false;
		}
		for (uint64 level = 0; level < self.levels; level++)
			host_delta_Word_store(&words[level], node.f0);
	}
	op.tag = HOST_SHARED_OP_ADD;
	op.u.add = 1;
	host_shared_Record_atomic(record, keyed->count, op);
	op.u.add = self.bytes;
	host_shared_Record_atomic(record, keyed->bytes, op);
	return true;
}

#ifdef DOLTGRES_TEST_TXN
#include "doltgres/simc/simc.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/test.h"
#include "doltgres/txn/memtable/testing.h"
#include "doltgres/txn/runs.h"

/* txn_memtable_index_tests_key_Debug_fmt is a key's Debug, of a Vec<u8>: its bytes as a list. */
static bool
txn_memtable_index_tests_key_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *key = value;
	StdBytes	bytes = {key->ptr, key->len};

	return std_bytes_Debug_fmt(&bytes, f);
}

/* txn_memtable_index_tests_keys_Debug_fmt is keys' Debug, of a Vec<Vec<u8>>: each key as a list. */
static bool
txn_memtable_index_tests_keys_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *keys = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t k = 0; k < keys->len; k++)
		std_DebugList_entry(&list, &STD_VEC_AT(keys, StdVec, k), txn_memtable_index_tests_key_Debug_fmt);
	return std_DebugList_finish(&list);
}

/* txn_memtable_index_tests_keys_eq reports whether two Vec<Vec<u8>> hold the same keys in the same order. */
static bool
txn_memtable_index_tests_keys_eq(const StdVec *a, const StdVec *b)
{
	if (a->len != b->len)
		return false;
	for (size_t k = 0; k < a->len; k++)
	{
		const StdVec *x = &STD_VEC_AT(a, StdVec, k);
		const StdVec *y = &STD_VEC_AT(b, StdVec, k);

		if (x->len != y->len || (x->len > 0 && memcmp(x->ptr, y->ptr, x->len) != 0))
			return false;
	}
	return true;
}

/* txn_memtable_index_tests_keys_free frees a Vec<Vec<u8>>. */
static void
txn_memtable_index_tests_keys_free(StdVec *keys)
{
	for (size_t k = 0; k < keys->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(keys, StdVec, k), 1, 1);
	std_vec_free_Checked(keys, sizeof(StdVec), _Alignof(StdVec));
}

/* txn_memtable_index_tests_walked_free frees walked's Vec<(Vec<u8>, u64)>. */
static void
txn_memtable_index_tests_walked_free(StdVec *walked)
{
	for (size_t k = 0; k < walked->len; k++)
		std_vec_free_Checked(&STD_VEC_AT(walked, txn_TupleVecU8U64_t, k).f0, 1, 1);
	std_vec_free_Checked(walked, sizeof(txn_TupleVecU8U64_t), _Alignof(txn_TupleVecU8U64_t));
}

/* txn_memtable_index_tests_opened takes a hold, the Keys record at a location, and its lock, shared or not. */
static bool
txn_memtable_index_tests_opened(HostIoIo *io, uint64 keys, bool exclusive, HostDeltaHold *hold,
								HostSharedGuard *guard, HostError *error)
{
	HostSharedRecord record;

	if (!host_delta_hold(io, hold, error))
		return false;
	if (host_delta_Hold_record(hold, &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS, keys, &record, error) &&
		(exclusive ?
		 host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_TESTING_KEYS_LOCK, guard, error) :
		 host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_TESTING_KEYS_LOCK, guard,
										error)))
		return true;
	host_delta_Hold_drop(hold);
	return false;
}

/* txn_memtable_index_tests_walked reads the index's keys from its first node, in its order, with each node's height. */
static bool
txn_memtable_index_tests_walked(HostIoIo *io, uint64 keys, StdVec *result, HostError *error)
{
	const TxnMemtableIndexKeyedOps *keyed = &TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS;
	HostDeltaHold hold;
	HostSharedGuard guard;
	TxnMemtableIndexNode at;
	bool		some;
	bool		ok = true;

	if (!txn_memtable_index_tests_opened(io, keys, false, &hold, &guard, error))
		return false;
	std_vec_new(result, sizeof(txn_TupleVecU8U64_t), _Alignof(txn_TupleVecU8U64_t));
	some = txn_memtable_index_first(keyed, &guard, &at);
	while (ok && some)
	{
		TxnMemtableIndexPlace place;
		HostDeltaLent key;
		StdBytes	bytes;
		txn_TupleVecU8U64_t entry;
		uint64		placed;

		ok = txn_memtable_index_Place_at(&keyed->closes, &guard, &hold, at, &place, error) &&
			txn_memtable_index_Place_key(&place, &key, error);
		if (!ok)
			break;
		bytes = host_delta_Lent_Deref_deref(&key);
		std_slice_to_vec_Checked(&entry.f0, bytes.ptr, bytes.len, 1, 1);
		host_delta_Lent_drop(&key);
		ok = txn_memtable_index_Place_placed(&place, &placed, error);
		if (!ok)
		{
			std_vec_free_Checked(&entry.f0, 1, 1);
			break;
		}
		entry.f1 = placed >> TXN_MEMTABLE_INDEX_LEVELS;
		std_vec_push_Checked(result, &entry, sizeof(entry), _Alignof(txn_TupleVecU8U64_t));
		ok = txn_memtable_index_next(keyed, &guard, &hold, at, &some, &at, error);
	}
	if (!ok)
		txn_memtable_index_tests_walked_free(result);
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	return ok;
}

/* txn_memtable_index_tests_walked_keys reads the index's keys in its order, as walked's unwrap and the map to its keys do at their place. */
static void
txn_memtable_index_tests_walked_keys(uint32 line, uint32 col, HostIoIo *io, uint64 keys, StdVec *result)
{
	StdVec		walked;
	HostError	error;

	if (!txn_memtable_index_tests_walked(io, keys, &walked, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_with_capacity_Checked(result, walked.len, sizeof(StdVec), _Alignof(StdVec));
	for (size_t k = 0; k < walked.len; k++)
		STD_VEC_AT(result, StdVec, k) = STD_VEC_AT(&walked, txn_TupleVecU8U64_t, k).f0;
	result->len = walked.len;
	std_vec_free_Checked(&walked, sizeof(txn_TupleVecU8U64_t), _Alignof(txn_TupleVecU8U64_t));
}

/* txn_memtable_index_tests_walked_len asserts how many nodes the index reads, as assert_eq on walked's unwrap's len does at their places. */
static void
txn_memtable_index_tests_walked_len(uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col, HostIoIo *io,
									uint64 keys, size_t expected)
{
	StdVec		walked;
	HostError	error;
	size_t		len;

	if (!txn_memtable_index_tests_walked(io, keys, &walked, &error))
		STD_RESULT_EXPECT_FAILED(unwrap_line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	len = walked.len;
	txn_memtable_index_tests_walked_free(&walked);
	STD_ASSERT_EQ(line, col, len == expected, &len, std_usize_Debug_fmt, &expected, std_usize_Debug_fmt);
}

/* txn_memtable_index_tests_searched returns a key's search: the key of the node at or after it, and whether the index holds it. */
static bool
txn_memtable_index_tests_searched(HostIoIo *io, uint64 keys, const uint8 *key, size_t key_len, bool *has_at,
								  StdVec *at, bool *held, HostError *error)
{
	const TxnMemtableIndexKeyedOps *keyed = &TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS;
	HostDeltaHold hold;
	HostSharedGuard guard;
	TxnMemtableIndexNode node;
	HostDeltaLent lent;
	StdBytes	bytes;
	bool		ok;

	if (!txn_memtable_index_tests_opened(io, keys, false, &hold, &guard, error))
		return false;
	ok = txn_memtable_index_seek(keyed, &guard, &hold, key, key_len, has_at, &node, error);
	if (ok && *has_at)
	{
		ok = txn_memtable_index_key(keyed, &guard, &hold, node, &lent, error);
		if (ok)
		{
			bytes = host_delta_Lent_Deref_deref(&lent);
			std_slice_to_vec_Checked(at, bytes.ptr, bytes.len, 1, 1);
			host_delta_Lent_drop(&lent);
		}
	}
	if (ok)
	{
		ok = txn_memtable_index_find(keyed, &guard, &hold, key, key_len, held, &node, error);
		if (!ok && *has_at)
			std_vec_free_Checked(at, 1, 1);
	}
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	return ok;
}

/*
 * txn_memtable_index_tests_searched_is asserts a key's search, as assert_eq
 * on searched's unwrap does at their places, with a probe's message when
 * one is given.
 */
static void
txn_memtable_index_tests_searched_is(uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col, HostIoIo *io,
									 uint64 keys, const uint8 *key, size_t key_len, const StdVec *at, bool held,
									 const uint64 *probe)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[1];
	StdFmtArguments message;
	HostError	error;
	StdVec		found;
	bool		has_found;
	bool		found_held;
	StdOptionRef left_at;
	StdOptionRef right_at;
	StdTupleRef left;
	StdTupleRef right;
	bool		eq;

	if (!txn_memtable_index_tests_searched(io, keys, key, key_len, &has_found, &found, &found_held, &error))
		STD_RESULT_EXPECT_FAILED(unwrap_line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	eq = has_found == (at != NULL) && found_held == held &&
		(!has_found || (found.len == at->len && (found.len == 0 || memcmp(found.ptr, at->ptr, found.len) == 0)));
	if (!eq)
	{
		left_at.has = has_found;
		left_at.some = STD_ITEM(&found, txn_memtable_index_tests_key_Debug_fmt);
		right_at.has = at != NULL;
		right_at.some = STD_ITEM(at, txn_memtable_index_tests_key_Debug_fmt);
		left = STD_TUPLE(2, STD_ITEM(&left_at, std_option_Debug_fmt), STD_ITEM(&found_held, std_bool_Debug_fmt));
		right = STD_TUPLE(2, STD_ITEM(&right_at, std_option_Debug_fmt), STD_ITEM(&held, std_bool_Debug_fmt));
		if (probe == NULL)
			std_assert_failed(RUST_FILE, line, col, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt,
							  NULL);
		args[0].value = probe;
		args[0].fmt = std_u64_Display_fmt;
		message.pieces = pieces;
		message.pieces_len = lengthof(pieces);
		message.args = args;
		std_assert_failed(RUST_FILE, line, col, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt,
						  &message);
	}
	if (has_found)
		std_vec_free_Checked(&found, 1, 1);
}

/* txn_memtable_index_tests_found returns the node of a key the index holds. */
static bool
txn_memtable_index_tests_found(HostIoIo *io, uint64 keys, const uint8 *key, size_t key_len,
							   TxnMemtableIndexNode *result, HostError *error)
{
	HostDeltaHold hold;
	HostSharedGuard guard;
	bool		some;
	bool		ok;

	if (!txn_memtable_index_tests_opened(io, keys, false, &hold, &guard, error))
		return false;
	ok = txn_memtable_index_find(&TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS, &guard, &hold, key, key_len, &some, result,
								 error);
	if (ok && !some)
	{
		*error = txn_memtable_testing_failed("the index doesn't hold a key it was given");
		ok = false;
	}
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	return ok;
}

/* txn_memtable_index_tests_pushed_value pushes a numbered key's version of a value, as pushed's unwrap does at its place. */
static TxnMemtableVersionPushed
txn_memtable_index_tests_pushed_value(uint32 line, uint32 col, HostIoIo *io, uint64 keys, uint64 number,
									  const char *value)
{
	StdVec		key;
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) value, strlen(value));
	TxnMemtableVersionPushed pushed;
	HostError	error;

	txn_memtable_testing_numbered(number, &key);
	if (!txn_memtable_testing_pushed(io, keys, key.ptr, key.len, &op, TXN_MEMTABLE_TESTING_PAGE, &pushed, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_Op_drop(&op);
	std_vec_free_Checked(&key, 1, 1);
	return pushed;
}

/* txn_memtable_index_tests_keyed makes a Keys record, as keys' unwrap does at its place. */
static uint64
txn_memtable_index_tests_keyed(uint32 line, uint32 col, HostIoIo *io, HostSharedHeld *held)
{
	uint64		keys;
	HostError	error;

	if (!txn_memtable_testing_keys(io, held, &keys, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return keys;
}

/* Keys pushed in any order read back in key order, each once: a second version of a key adds no node. */
void
test_txn_memtable_index_tests_keys_pushed_in_any_order_read_back_in_key_order(void)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(": "), STD_FMT_ARG(1)};
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held;
	uint64		keys = txn_memtable_index_tests_keyed(594, 38, io, &held);
	TxnMemtableVersionPushed added;
	StdVec		read;
	StdVec		expected;

	for (uint64 i = 0; i < 300; i++)
	{
		added = txn_memtable_index_tests_pushed_value(596, 87, io, keys, i * 7919 % 300, "v");
		if (added.tag != TXN_MEMTABLE_VERSION_PUSHED_VERSION)
		{
			StdFmtArg	args[2] = {{&i, std_u64_Display_fmt}, {&added, txn_memtable_version_Pushed_Debug_fmt}};
			StdFmtArguments message = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 597, 13, &message);
		}
	}
	txn_memtable_index_tests_walked_keys(599, 51, io, keys, &read);
	std_vec_with_capacity_Checked(&expected, 300, sizeof(StdVec), _Alignof(StdVec));
	for (uint64 i = 0; i < 300; i++)
		txn_memtable_testing_numbered(i, &STD_VEC_AT(&expected, StdVec, i));
	expected.len = 300;
	STD_ASSERT_EQ(600, 9, txn_memtable_index_tests_keys_eq(&read, &expected), &read,
				  txn_memtable_index_tests_keys_Debug_fmt, &expected, txn_memtable_index_tests_keys_Debug_fmt);
	txn_memtable_index_tests_keys_free(&expected);
	txn_memtable_index_tests_keys_free(&read);
	added = txn_memtable_index_tests_pushed_value(601, 77, io, keys, 150, "w");
	STD_ASSERT(601, 9, added.tag == TXN_MEMTABLE_VERSION_PUSHED_VERSION,
			   "matches!(pushed(io, keys, &numbered(150), &put(b\"w\"), PAGE).unwrap(), Pushed::Version(_))");
	txn_memtable_index_tests_walked_len(602, 9, 602, 37, io, keys, 300);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * A search finds the node at or after each key, between, at, and past every
 * node's, and finds only the keys the index holds; an empty index finds
 * nothing.
 */
void
test_txn_memtable_index_tests_a_search_finds_the_node_at_or_after_a_key(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held;
	uint64		keys = txn_memtable_index_tests_keyed(612, 38, io, &held);
	StdVec		probed;
	StdVec		at;
	StdVec		zero;

	txn_memtable_index_tests_searched_is(613, 9, 613, 47, io, keys, (const uint8 *) "any", 3, NULL,
										 false, NULL);
	for (uint64 i = 0; i < 50; i++)
		(void) txn_memtable_index_tests_pushed_value(615, 76, io, keys, i * 17 % 50 * 2, "v");
	for (uint64 probe = 0; probe <= 100; probe++)
	{
		bool		has_at = probe < 99;

		if (has_at)
			txn_memtable_testing_numbered(probe + probe % 2, &at);
		txn_memtable_testing_numbered(probe, &probed);
		txn_memtable_index_tests_searched_is(619, 13, 619, 61, io, keys, probed.ptr, probed.len,
											 has_at ? &at : NULL, probe % 2 == 0 && probe < 100, &probe);
		std_vec_free_Checked(&probed, 1, 1);
		if (has_at)
			std_vec_free_Checked(&at, 1, 1);
	}
	txn_memtable_testing_numbered(0, &zero);
	txn_memtable_index_tests_searched_is(621, 9, 621, 44, io, keys, (const uint8 *) "", 0, &zero,
										 false, NULL);
	std_vec_free_Checked(&zero, 1, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_index_tests_counts_Debug_fmt is the cap case's counts' Debug, of a [usize; HEIGHT + 1]. */
static bool
txn_memtable_index_tests_counts_Debug_fmt(const void *value, StdFormatter *f)
{
	const size_t *at = value;
	StdDebugList list = std_fmt_debug_list(f);

	for (size_t h = 0; h <= TXN_MEMTABLE_INDEX_HEIGHT; h++)
		std_DebugList_entry(&list, &at[h], std_usize_Debug_fmt);
	return std_DebugList_finish(&list);
}

/*
 * Over the most keys a range holds before it splits, no height passes the
 * cap, the cap is reached, and each level holds fewer nodes than the one
 * below.
 */
void
test_txn_memtable_index_tests_no_height_passes_the_cap(void)
{
	static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	uint8		seed[32];
	size_t		at[TXN_MEMTABLE_INDEX_HEIGHT + 1] = {0};
	size_t		zero = 0;
	StdVec		key;
	bool		fewer = true;
	StdFmtArg	args[1] = {{at, txn_memtable_index_tests_counts_Debug_fmt}};
	StdFmtArguments message = {pieces, lengthof(pieces), args};

	memset(seed, 7, sizeof(seed));
	for (uint64 i = 0; i < 32768; i++)
	{
		uint64		h;

		txn_memtable_testing_numbered(i, &key);
		h = txn_memtable_index_height(seed, key.ptr, key.len);
		std_index_check(h, lengthof(at), RUST_FILE, 668, 13);
		at[h]++;
		std_vec_free_Checked(&key, 1, 1);
	}
	STD_ASSERT_EQ(670, 9, at[0] == 0, &at[0], std_usize_Debug_fmt, &zero, std_usize_Debug_fmt);
	if (!(at[TXN_MEMTABLE_INDEX_HEIGHT] > 0))
		std_panic_fmt(RUST_FILE, 671, 9, &message);
	for (size_t h = 1; h < TXN_MEMTABLE_INDEX_HEIGHT && fewer; h++)
		fewer = at[h] > at[h + 1];
	if (!fewer)
		std_panic_fmt(RUST_FILE, 672, 9, &message);
}

/* Each level links, in key order, exactly the nodes whose height passes it, from the first node. */
void
test_txn_memtable_index_tests_every_level_links_the_nodes_that_reach_it_in_key_order(void)
{
	static const StdFmtPiece pieces[] = {STD_FMT_LIT("level "), STD_FMT_ARG(0)};
	const TxnMemtableIndexKeyedOps *keyed = &TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS;
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held;
	uint64		keys = txn_memtable_index_tests_keyed(723, 38, io, &held);
	HostDeltaHold hold;
	HostSharedRecord record;
	HostSharedGuard guard;
	HostError	error;
	StdVec		read;

	for (uint64 i = 0; i < 300; i++)
		(void) txn_memtable_index_tests_pushed_value(725, 75, io, keys, i * 7919 % 300, "v");
	if (!txn_memtable_index_tests_walked(io, keys, &read, &error))
		STD_RESULT_EXPECT_FAILED(727, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(728, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS, keys, &record, &error))
		STD_RESULT_EXPECT_FAILED(729, 48, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_TESTING_KEYS_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(730, 56, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	for (uint64 level = 0; level < TXN_MEMTABLE_INDEX_HEIGHT; level++)
	{
		StdVec		linked;
		StdVec		reaching;
		TxnMemtableIndexNode at;
		bool		some = txn_memtable_index_first(keyed, &guard, &at);

		std_vec_new(&linked, sizeof(StdVec), _Alignof(StdVec));
		while (some)
		{
			TxnMemtableIndexPlace place;
			HostDeltaLent key;
			HostDeltaWord link;
			StdBytes	bytes;
			StdVec		copy;

			if (!txn_memtable_index_Place_at(&keyed->closes, &guard, &hold, at, &place, &error))
				STD_RESULT_EXPECT_FAILED(735, 60, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			if (!txn_memtable_index_Place_key(&place, &key, &error))
				STD_RESULT_EXPECT_FAILED(736, 41, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			bytes = host_delta_Lent_Deref_deref(&key);
			std_slice_to_vec_Checked(&copy, bytes.ptr, bytes.len, 1, 1);
			host_delta_Lent_drop(&key);
			std_vec_push_Checked(&linked, &copy, sizeof(copy), _Alignof(StdVec));
			if (!txn_memtable_index_Place_link(&place, level, &link, &error))
				STD_RESULT_EXPECT_FAILED(737, 52, "called `Result::unwrap()` on an `Err` value", &error,
										 host_Error_Debug_fmt);
			some = txn_memtable_index_Node_named(host_delta_Word_load(&link), &at);
		}
		std_vec_new(&reaching, sizeof(StdVec), _Alignof(StdVec));
		for (size_t k = 0; k < read.len; k++)
		{
			const txn_TupleVecU8U64_t *entry = &STD_VEC_AT(&read, txn_TupleVecU8U64_t, k);
			StdVec		copy;

			if (entry->f1 <= level)
				continue;
			std_slice_to_vec_Checked(&copy, entry->f0.ptr, entry->f0.len, 1, 1);
			std_vec_push_Checked(&reaching, &copy, sizeof(copy), _Alignof(StdVec));
		}
		if (!txn_memtable_index_tests_keys_eq(&linked, &reaching))
		{
			StdFmtArg	args[1] = {{&level, std_u64_Display_fmt}};
			StdFmtArguments message = {pieces, lengthof(pieces), args};

			std_assert_failed(RUST_FILE, 741, 13, "==", &linked, txn_memtable_index_tests_keys_Debug_fmt,
							  &reaching, txn_memtable_index_tests_keys_Debug_fmt, &message);
		}
		txn_memtable_index_tests_keys_free(&reaching);
		txn_memtable_index_tests_keys_free(&linked);
	}
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	txn_memtable_index_tests_walked_free(&read);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_index_tests_count is the word count case's count: a word of the node's page. */
static uint64
txn_memtable_index_tests_count(const TxnMemtableIndexPlace *place, uint64 word)
{
	HostDeltaWord loaded;
	HostError	error;

	if (!host_delta_PageRef_word(&place->page, word, &loaded, &error))
		STD_RESULT_EXPECT_FAILED(761, 50, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	return host_delta_Word_load(&loaded);
}

/*
 * A page counts the words its versions and nodes take: its three counts,
 * then each version's line pointer and each node's head, place word, and
 * links; it counts each version and node as a live record, and the one list
 * naming it.
 */
void
test_txn_memtable_index_tests_a_page_counts_the_words_its_versions_and_nodes_take(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held;
	uint64		keys = txn_memtable_index_tests_keyed(752, 38, io, &held);
	TxnMemtableIndexNode second;
	TxnMemtableIndexPlace place;
	HostDeltaHold hold;
	HostSharedRecord record;
	HostSharedGuard guard;
	HostError	error;
	StdVec		one;
	uint64		placed;
	uint64		height;
	uint64		counted[3];
	uint64		expected[3];
	StdTupleRef left;
	StdTupleRef right;

	(void) txn_memtable_index_tests_pushed_value(753, 58, io, keys, 0, "v");
	(void) txn_memtable_index_tests_pushed_value(754, 58, io, keys, 1, "v");
	txn_memtable_testing_numbered(1, &one);
	if (!txn_memtable_index_tests_found(io, keys, one.ptr, one.len, &second, &error))
		STD_RESULT_EXPECT_FAILED(755, 52, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	std_vec_free_Checked(&one, 1, 1);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(756, 36, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS, keys, &record, &error))
		STD_RESULT_EXPECT_FAILED(757, 48, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock_shared(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_TESTING_KEYS_LOCK, &guard,
										&error))
		STD_RESULT_EXPECT_FAILED(758, 56, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_index_Place_at(&TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS.closes, &guard, &hold, second, &place,
									 &error))
		STD_RESULT_EXPECT_FAILED(759, 54, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_index_Place_placed(&place, &placed, &error))
		STD_RESULT_EXPECT_FAILED(760, 37, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	height = placed >> TXN_MEMTABLE_INDEX_LEVELS;
	counted[0] = txn_memtable_index_tests_count(&place, TXN_MEMTABLE_PAGES_COUNT);
	expected[0] = TXN_MEMTABLE_PAGES_FIRST + (2 + 2 + TXN_MEMTABLE_INDEX_HEIGHT) + (2 + 2 + height);
	STD_ASSERT_EQ(762, 9, counted[0] == expected[0], &counted[0], std_u64_Debug_fmt, &expected[0],
				  std_u64_Debug_fmt);
	counted[1] = txn_memtable_index_tests_count(&place, TXN_MEMTABLE_PAGES_LIVE);
	counted[2] = txn_memtable_index_tests_count(&place, TXN_MEMTABLE_PAGES_LISTS);
	expected[1] = 4;
	expected[2] = 1;
	left = STD_TUPLE(2, STD_ITEM(&counted[1], std_u64_Debug_fmt), STD_ITEM(&counted[2], std_u64_Debug_fmt));
	right = STD_TUPLE(2, STD_ITEM(&expected[1], std_u64_Debug_fmt), STD_ITEM(&expected[2], std_u64_Debug_fmt));
	STD_ASSERT_EQ(763, 9, counted[1] == expected[1] && counted[2] == expected[2], &left,
				  std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt);
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* A closed record takes no change: its open finds it closed, and its index still reads. */
void
test_txn_memtable_index_tests_a_closed_record_takes_no_change(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held;
	uint64		keys = txn_memtable_index_tests_keyed(772, 38, io, &held);
	HostDeltaHold hold;
	HostSharedRecord record;
	HostSharedGuard guard;
	HostError	error;
	TxnMemtableIndexOpen open;
	StdVec		one;

	(void) txn_memtable_index_tests_pushed_value(773, 58, io, keys, 1, "v");
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(775, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS, keys, &record, &error))
		STD_RESULT_EXPECT_FAILED(776, 52, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_TESTING_KEYS_LOCK, &guard, &error))
		STD_RESULT_EXPECT_FAILED(777, 57, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_index_open(&TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS.closes, &guard, &open))
		STD_OPTION_UNWRAP_FAILED(778, 30);
	txn_memtable_index_Open_close(open);
	STD_ASSERT(779, 13, !txn_memtable_index_open(&TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS.closes, &guard, &open),
			   "open(&mut guard).is_none()");
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	txn_memtable_testing_numbered(1, &one);
	txn_memtable_index_tests_searched_is(781, 9, 781, 53, io, keys, one.ptr, one.len, &one, true,
										 NULL);
	std_vec_free_Checked(&one, 1, 1);
	txn_memtable_index_tests_walked_len(782, 9, 782, 37, io, keys, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_index_tests_pushed_v pushes a numbered key's version of "v", answering false with the error. */
static bool
txn_memtable_index_tests_pushed_v(HostIoIo *io, uint64 keys, uint64 number, HostError *error)
{
	StdVec		key;
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "v", 1);
	TxnMemtableVersionPushed pushed;
	bool		ok;

	txn_memtable_testing_numbered(number, &key);
	ok = txn_memtable_testing_pushed(io, keys, key.ptr, key.len, &op, TXN_MEMTABLE_TESTING_PAGE, &pushed, error);
	txn_memtable_Op_drop(&op);
	std_vec_free_Checked(&key, 1, 1);
	return ok;
}

/*
 * txn_memtable_index_tests_heights_writer is the heights case's writer: it
 * pushes 64 keys and checks that its index reads them in key order, each
 * node of its key's height under the run's seed, the first of the full
 * height.
 */
static bool
txn_memtable_index_tests_heights_writer(void *captures, HostIoIo *io, HostError *error)
{
	HostSharedHeld held;
	uint64		keys;
	StdVec		read;
	uint8		seed[32];
	bool		ordered = true;
	bool		heights = true;

	if (!txn_memtable_testing_keys(io, &held, &keys, error))
		return false;
	for (uint64 i = 0; i < 64; i++)
		if (!txn_memtable_index_tests_pushed_v(io, keys, i * 37 % 64, error))
		{
			host_shared_Held_drop(&held);
			return false;
		}
	if (!txn_memtable_index_tests_walked(io, keys, &read, error))
	{
		host_shared_Held_drop(&held);
		return false;
	}
	host_delta_seed(seed);
	ordered = read.len == 64;
	heights = read.len == 64;
	for (uint64 i = 0; i < read.len && ordered; i++)
	{
		const txn_TupleVecU8U64_t *entry = &STD_VEC_AT(&read, txn_TupleVecU8U64_t, i);
		StdVec		key;
		uint64		height;

		txn_memtable_testing_numbered(i, &key);
		ordered = entry->f0.len == key.len && memcmp(entry->f0.ptr, key.ptr, key.len) == 0;
		height = i == 0 ? TXN_MEMTABLE_INDEX_HEIGHT : txn_memtable_index_height(seed, key.ptr, key.len);
		heights = heights && entry->f1 == height;
		std_vec_free_Checked(&key, 1, 1);
	}
	txn_memtable_index_tests_walked_free(&read);
	host_shared_Held_drop(&held);
	if (!ordered)
	{
		*error = txn_memtable_testing_failed("a run's index read its keys out of order");
		return false;
	}
	if (!heights)
	{
		*error = txn_memtable_testing_failed("a node's height isn't its key's under the run's seed");
		return false;
	}
	return true;
}

/*
 * A node's height follows its key's hash under the run's seed: two runs of
 * one seed give every key the same height, two seeds give some keys others,
 * and each run's index reads back the same keys in the same order; the first
 * node is of the full height.
 */
void
test_txn_memtable_index_tests_heights_follow_the_seed_and_reads_dont(void)
{
	static const uint64 seeds[] = {3, 3, 4};
	StdVec		heights[3];

	for (size_t s = 0; s < lengthof(seeds); s++)
	{
		uint64		seed = seeds[s];
		SimcRun		run = simc_Run_new(seed);
		SimcActor	actor = {txn_memtable_index_tests_heights_writer};
		SimcReport	report;
		uint8		seeded[32] = {0};
		bool		returned = true;

		simc_Run_actor(&run, "writer", 6, actor);
		report = simc_Run_finish(run);
		if (report.outcome.tag != SIMC_OUTCOME_ENDED)
		{
			static const StdFmtPiece pieces[] = {STD_FMT_LIT("seed "), STD_FMT_ARG(0), STD_FMT_LIT(" didn't end: "),
			STD_FMT_ARG(1)};
			StdFmtArg	args[2] = {{&seed, std_u64_Display_fmt}, {&report, simc_Report_Debug_fmt}};
			StdFmtArguments message = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 650, 70, &message);
		}
		for (size_t k = 0; k < report.outcome.u.ended.len && returned; k++)
		{
			const SimcEnding *ending = &STD_VEC_AT(&report.outcome.u.ended, simc_TupleStringEnding_t, k).f1;

			returned = ending->tag == SIMC_ENDING_RETURNED && !ending->is_err;
		}
		if (!returned)
		{
			static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
			StdFmtArg	args[1] = {{&report, simc_Report_Debug_fmt}};
			StdFmtArguments message = {pieces, lengthof(pieces), args};

			std_panic_fmt(RUST_FILE, 651, 17, &message);
		}
		simc_Report_drop(&report);
		for (int b = 0; b < 8; b++)
			seeded[b] = (uint8) (seed >> (8 * b));
		std_vec_with_capacity_Checked(&heights[s], 63, sizeof(uint64), _Alignof(uint64));
		for (uint64 i = 1; i < 64; i++)
		{
			StdVec		key;

			txn_memtable_testing_numbered(i, &key);
			STD_VEC_AT(&heights[s], uint64, i - 1) = txn_memtable_index_height(seeded, key.ptr, key.len);
			std_vec_free_Checked(&key, 1, 1);
		}
		heights[s].len = 63;
	}
	STD_ASSERT_EQ(657, 9, memcmp(heights[0].ptr, heights[1].ptr, 63 * sizeof(uint64)) == 0, &heights[0],
				  chunks_trailer_VecU64_Debug_fmt, &heights[1], chunks_trailer_VecU64_Debug_fmt);
	if (memcmp(heights[0].ptr, heights[2].ptr, 63 * sizeof(uint64)) == 0)
		std_assert_failed(RUST_FILE, 658, 9, "!=", &heights[0], chunks_trailer_VecU64_Debug_fmt,
						  &heights[2], chunks_trailer_VecU64_Debug_fmt, NULL);
	for (size_t s = 0; s < lengthof(seeds); s++)
		std_vec_free_Checked(&heights[s], sizeof(uint64), _Alignof(uint64));
}

/* txn_memtable_index_tests_pushed_small pushes a key's empty version onto pages of a size, as pushed's unwrap does at its place. */
static void
txn_memtable_index_tests_pushed_small(uint32 line, uint32 col, HostIoIo *io, uint64 keys, const uint8 *key,
									  HostDeltaPageSize size)
{
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "", 0);
	TxnMemtableVersionPushed pushed;
	HostError	error;

	if (!txn_memtable_testing_pushed(io, keys, key, 40, &op, size, &pushed, &error))
		STD_RESULT_EXPECT_FAILED(line, col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	txn_memtable_Op_drop(&op);
}

/* txn_memtable_index_tests_page_is asserts the page of a key's node, as assert_eq on found's unwrap's page does at their places. */
static void
txn_memtable_index_tests_page_is(uint32 line, uint32 col, uint32 unwrap_line, uint32 unwrap_col, HostIoIo *io,
								 uint64 keys, const uint8 *key, uint64 expected)
{
	TxnMemtableIndexNode node;
	HostError	error;
	uint64		page;

	if (!txn_memtable_index_tests_found(io, keys, key, 40, &node, &error))
		STD_RESULT_EXPECT_FAILED(unwrap_line, unwrap_col, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	page = txn_memtable_index_Node_page(node);
	STD_ASSERT_EQ(line, col, page == expected, &page, std_u64_Debug_fmt, &expected, std_u64_Debug_fmt);
}

/*
 * A new key's version and node that don't both fit the newest page go
 * together on the fresh page, which the list then names, even where the
 * newest page has room for the version alone; with no fresh page the push
 * wants one and changes nothing.
 */
void
test_txn_memtable_index_tests_a_new_keys_version_and_node_go_together_on_a_fresh_page(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held;
	uint64		keys = txn_memtable_index_tests_keyed(683, 38, io, &held);
	HostDeltaPageSize small;
	uint8		a[40];
	uint8		b[40];
	uint8		c[40];
	HostDeltaHold hold;
	HostSharedRecord record;
	HostSharedGuard guard;
	HostError	error;
	TxnMemtableIndexKeyedOpen open;
	TxnMemtableOp op = txn_memtable_testing_put((const uint8 *) "", 0);
	TxnMemtableVersionChange change = {.writer = txn_memtable_testing_writer(7), .op = &op,
	.lock = TXN_MEMTABLE_LOCK_MODE_UPDATE, .shown = {.tag = TXN_MEMTABLE_VERSION_SHOWN_COMMAND}};
	TxnMemtableVersionPushed wanted;
	HostDeltaOwned fresh;
	bool		has_fresh = false;
	StdVec		spare;
	StdVec		buffer;
	HostDeltaListListView list;
	uint64		pages;
	uint64		two = 2;

	small.bytes = 29 + 40 + 40;
	small.words = TXN_MEMTABLE_PAGES_FIRST + 2 + 2 + TXN_MEMTABLE_INDEX_HEIGHT + 2;
	memset(a, 'a', sizeof(a));
	memset(b, 'b', sizeof(b));
	memset(c, 'c', sizeof(c));
	txn_memtable_index_tests_pushed_small(685, 57, io, keys, a, small);
	txn_memtable_index_tests_page_is(686, 9, 686, 49, io, keys, a, 0);
	txn_memtable_index_tests_pushed_small(687, 57, io, keys, b, small);
	txn_memtable_index_tests_page_is(688, 9, 688, 49, io, keys, b, 1);
	if (!host_delta_hold(io, &hold, &error))
		STD_RESULT_EXPECT_FAILED(690, 40, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_delta_Hold_record(&hold, &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS, keys, &record, &error))
		STD_RESULT_EXPECT_FAILED(691, 52, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(io), TXN_MEMTABLE_TESTING_KEYS_LOCK, &guard, &error))
		STD_RESULT_EXPECT_FAILED(692, 57, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	if (!txn_memtable_index_keyed_open(&TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS, &guard, &open))
		STD_OPTION_UNWRAP_FAILED(693, 45);
	std_vec_new(&spare, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	std_vec_new(&buffer, 1, 1);
	if (!txn_memtable_version_push(&open, &hold, c, sizeof(c), &change, false, &has_fresh, &fresh, &spare, &buffer,
								   &wanted,
								   &error))
		STD_RESULT_EXPECT_FAILED(710, 21, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	list = host_delta_list_Guard_list(open.open.guard, &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS.shape,
									  TXN_MEMTABLE_INDEX_PAGES);
	pages = host_delta_list_ListView_len(&list);
	std_vec_free_Checked(&buffer, 1, 1);
	std_vec_free_Checked(&spare, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	txn_memtable_Op_drop(&op);
	STD_ASSERT(712, 9, wanted.tag == TXN_MEMTABLE_VERSION_PUSHED_WANTS, "matches!(wanted, Pushed::Wants(_))");
	STD_ASSERT_EQ(713, 9, pages == 2, &pages, std_u64_Debug_fmt, &two, std_u64_Debug_fmt);
	txn_memtable_index_tests_walked_len(714, 9, 714, 37, io, keys, 2);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/*
 * test_txn_memtable_index_tests_a_seek_past_the_last_key_leaves_its_node_as_it_was:
 * a seek that finds no node at or after its key answers none and leaves its
 * caller's node as the caller had it (bugs row 288).
 */
void
test_txn_memtable_index_tests_a_seek_past_the_last_key_leaves_its_node_as_it_was(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held;
	uint64		keys = txn_memtable_index_tests_keyed(__LINE__, 5, io, &held);
	HostDeltaHold hold;
	HostSharedGuard guard;
	HostError	error;
	TxnMemtableIndexNode node = {UINT64CONST(0x5EED)};
	StdVec		past;
	bool		some = true;

	(void) txn_memtable_index_tests_pushed_value(__LINE__, 5, io, keys, 0, "v");
	txn_memtable_testing_numbered(1, &past);
	if (!txn_memtable_index_tests_opened(io, keys, false, &hold, &guard, &error) ||
		!txn_memtable_index_seek(&TXN_MEMTABLE_TESTING_KEYS_KEYED_OPS, &guard, &hold, past.ptr, past.len, &some, &node,
								 &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 5, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 5, !some && node.f0 == UINT64CONST(0x5EED),
			   "a seek past the last key answers none and leaves its node as it was");
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	std_vec_free_Checked(&past, 1, 1);
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

/* txn_memtable_index_tests_listed is the value a cursor's test stores at an index of its list. */
static uint64
txn_memtable_index_tests_listed(uint64 index)
{
	return UINT64CONST(0x100000) + index;
}

/*
 * test_txn_memtable_index_tests_a_walks_cursor_reads_each_leaf_it_moves_to:
 * one cursor reads a list of two levels whose entries fill three leaves, back
 * and forth between leaves and in and out of the record's inline entries,
 * each read the value stored at its index, and refuses an index at the
 * list's length (bugs row 355).
 */
void
test_txn_memtable_index_tests_a_walks_cursor_reads_each_leaf_it_moves_to(void)
{
	static const uint64 reads[] = {
		HOST_DELTA_LIST_INLINE + 5, HOST_DELTA_LIST_INLINE + HOST_DELTA_LIST_FANOUT + 5, HOST_DELTA_LIST_INLINE + 7, 2,
		HOST_DELTA_LIST_INLINE + 9, HOST_DELTA_LIST_INLINE + 2 * HOST_DELTA_LIST_FANOUT,
		HOST_DELTA_LIST_INLINE + HOST_DELTA_LIST_FANOUT + 9,
	};
	static const char past[] = "an index of a list is past its length";
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	HostSharedHeld held;
	uint64		keys = txn_memtable_index_tests_keyed(__LINE__, 5, io, &held);
	uint64		length = HOST_DELTA_LIST_INLINE + 2 * HOST_DELTA_LIST_FANOUT + 1;
	HostDeltaPageSize list_page = HOST_DELTA_LIST_PAGE;
	HostDeltaHold hold;
	HostSharedGuard guard;
	HostDeltaListListView view;
	HostDeltaListListMut list;
	HostDeltaListCursor cursor = {0};
	HostDeltaFull full;
	StdVec		sizes;
	StdVec		spare;
	size_t		wanted;
	bool		allocated;
	uint64		got;
	StdStr		message;
	HostError	error;

	if (!txn_memtable_index_tests_opened(io, keys, false, &hold, &guard, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 5, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	view = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS.shape, TXN_MEMTABLE_INDEX_PAGES);
	wanted = host_delta_list_ListView_reserving(&view, length);
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	std_vec_new(&sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	for (size_t k = 0; k < wanted; k++)
		std_vec_push_Checked(&sizes, &list_page, sizeof(list_page), _Alignof(HostDeltaPageSize));
	if (!host_delta_alloc(io, sizes.ptr, sizes.len, &allocated, &spare, &full, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 5, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	STD_ASSERT(__LINE__, 5, allocated, "the delta gives a list's pages");
	if (!txn_memtable_index_tests_opened(io, keys, true, &hold, &guard, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 5, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	list = host_delta_list_GuardExclusive_list_mut(&guard, &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS.shape,
												   TXN_MEMTABLE_TESTING_KEYS_LOCK, TXN_MEMTABLE_INDEX_PAGES);
	for (uint64 index = 0; index < length; index++)
		if (!host_delta_list_ListMut_push(&list, &hold, txn_memtable_index_tests_listed(index), &spare, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 13, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	if (!txn_memtable_index_tests_opened(io, keys, false, &hold, &guard, &error))
		STD_RESULT_EXPECT_FAILED(__LINE__, 5, "called `Result::unwrap()` on an `Err` value", &error,
								 host_Error_Debug_fmt);
	view = host_delta_list_Guard_list(&guard, &TXN_MEMTABLE_TESTING_KEYS_AGED_OPS.shape, TXN_MEMTABLE_INDEX_PAGES);
	for (size_t k = 0; k < lengthof(reads); k++)
	{
		uint64		stored = txn_memtable_index_tests_listed(reads[k]);

		if (!host_delta_list_ListView_get_at(&view, &hold, &cursor, reads[k], &got, &error))
			STD_RESULT_EXPECT_FAILED(__LINE__, 9, "called `Result::unwrap()` on an `Err` value", &error,
									 host_Error_Debug_fmt);
		STD_ASSERT_EQ(__LINE__, 9, got == stored, &got, std_u64_Debug_fmt, &stored, std_u64_Debug_fmt);
	}
	STD_ASSERT(__LINE__, 5, !host_delta_list_ListView_get_at(&view, &hold, &cursor, length, &got, &error),
			   "a cursor refuses an index at the list's length");
	message = host_Error_message(&error);
	STD_ASSERT(__LINE__, 5, message.len == sizeof(past) - 1 && memcmp(message.ptr, past, message.len) == 0,
			   "an index at the list's length is refused as past it");
	host_Error_drop(&error);
	host_shared_Guard_drop(&guard);
	host_delta_Hold_drop(&hold);
	for (size_t k = 0; k < spare.len; k++)
		host_delta_Owned_drop(&STD_VEC_AT(&spare, HostDeltaOwned, k));
	std_vec_free_Checked(&spare, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	std_vec_free_Checked(&sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	host_shared_Held_drop(&held);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_TXN */
