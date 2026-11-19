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

/* TXN_MEMTABLE_INDEX_WORDS is WORDS, the count of a node's words before its links: its head and its place word. */
#define TXN_MEMTABLE_INDEX_WORDS UINT64CONST(2)

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
	result->f0 = link;
	return link != TXN_MEMTABLE_INDEX_NONE;
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
	(uint32) (1 + TXN_MEMTABLE_INDEX_WORDS + TXN_MEMTABLE_INDEX_HEIGHT)};

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

/* txn_memtable_index_Place_at opens a node's page through the range's list of pages. */
static bool
txn_memtable_index_Place_at(const TxnMemtableIndexClosesOps *closes, const HostSharedGuard *guard,
							const HostDeltaHold *hold, TxnMemtableIndexNode node, TxnMemtableIndexPlace *result,
							HostError *error)
{
	HostDeltaListListView list = host_delta_list_Guard_list(guard, closes->shape, TXN_MEMTABLE_INDEX_PAGES);
	uint64		name;

	if (!host_delta_list_ListView_get(&list, hold, txn_memtable_index_Node_page(node), &name, error) ||
		!host_delta_Hold_page(hold, host_delta_page_Page_named(name), &result->page, error))
		return false;
	result->word = txn_memtable_index_Node_word(node);
	return true;
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
 * txn_memtable_index_key_cmp compares a key with a node's key, lent and
 * given back at once, answering false on an error.
 */
static bool
txn_memtable_index_key_cmp(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						   const HostDeltaHold *hold, TxnMemtableIndexNode node, const uint8 *key, size_t key_len,
						   StdOrdering *result, HostError *error)
{
	TxnMemtableIndexPlace place;
	HostDeltaLent lent;
	StdBytes	bytes;

	if (!txn_memtable_index_Place_at(&keyed->closes, guard, hold, node, &place, error) ||
		!txn_memtable_index_Place_key(&place, &lent, error))
		return false;
	bytes = host_delta_Lent_Deref_deref(&lent);
	*result = std_bytes_Ord_cmp(key, key_len, bytes.ptr, bytes.len);
	host_delta_Lent_drop(&lent);
	return true;
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
	uint64		link;

	if (!txn_memtable_index_Place_at(&keyed->closes, guard, hold, node, &place, error) ||
		!txn_memtable_index_Place_loaded(&place, level, &link, error))
		return false;
	*some = txn_memtable_index_Node_named(link, result);
	return true;
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
 * key is below the key.
 */
static bool
txn_memtable_index_path(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
						const HostDeltaHold *hold, const uint8 *key, size_t key_len, TxnMemtableIndexPath *result,
						HostError *error)
{
	TxnMemtableIndexNode first;
	TxnMemtableIndexNode at;
	StdOrdering order;

	result->has_before = false;
	result->has_at = false;
	if (!txn_memtable_index_first(keyed, guard, &first))
		return true;
	if (!txn_memtable_index_key_cmp(keyed, guard, hold, first, key, key_len, &order, error))
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

			if (!txn_memtable_index_next_at(keyed, guard, hold, at, level, &some, &next, error))
				return false;
			if (!some)
				break;
			if (!txn_memtable_index_key_cmp(keyed, guard, hold, next, key, key_len, &order, error))
				return false;
			if (order != STD_ORDERING_GREATER)
				break;
			at = next;
		}
		result->before[level] = at;
	}
	result->has_before = true;
	return txn_memtable_index_next_at(keyed, guard, hold, at, 0, &result->has_at, &result->at, error);
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
