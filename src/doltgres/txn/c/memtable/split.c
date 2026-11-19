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
 * The C port of txn's memtable/split.rs: a range past SPLIT keys replaced by
 * two halves at its middle key, each a new record whose list names the old
 * range's pages at their indexes and then pages of its own that hold its key
 * index, built while it is unlinked under the old range's lock alone; the
 * memtable's list takes the halves in the old range's place through an entry
 * kept for the split, and the old range is retired as any closed range is.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/host/delta.h"
#include "doltgres/host/delta/list.h"
#include "doltgres/host/delta/page.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/sort.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/index.h"
#include "doltgres/txn/memtable/pages.h"
#include "doltgres/txn/memtable/range.h"
#include "doltgres/txn/memtable/split.h"
#include "doltgres/txn/memtable/table.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "memtable" STD_SEP "split.rs"

/* TXN_MEMTABLE_SPLIT_TRIES is TRIES, how many takes of the old range's lock a split makes before it gives up. */
#define TXN_MEMTABLE_SPLIT_TRIES 3

/* txn_memtable_split_u64_lt is the fn item u64's PartialOrd::lt's FnMut shim, which sort_unstable passes. */
static bool
txn_memtable_split_u64_lt(void *captures, const void *a, const void *b)
{
	(void) captures;
	return *(const uint64 *) a < *(const uint64 *) b;
}

/* The dead nodes' sort of their pages' indexes: sort_unstable at u64. */
#define STD_SORT_SCOPE static
#define STD_SORT_DECLARE
#define STD_SORT_DEFINE
#define STD_SORT_TS U64
#define STD_SORT_T uint64
#define STD_SORT_FREEZE 1
#define STD_SORT_COPY 1
#define STD_SORT_BRANCHLESS 1
#define STD_SORT_LT txn_memtable_split_u64_lt
#include "doltgres/std/sort_template.h"

/* txn_memtable_split_Needs_push pushes a count of one need onto a half's needs, whose room is reserved. */
static void
txn_memtable_split_Needs_push(StdVec *needs, std_TupleU64U64_t need, uint64 count)
{
	for (uint64 k = 0; k < count; k++)
		std_vec_push_Checked(needs, &need, sizeof(need), _Alignof(std_TupleU64U64_t));
}

/*
 * txn_memtable_split_estimated writes a half's needs from counts alone into
 * an empty Vec: its first node at the full height and its others at two
 * levels, each at the range's mean key length, then one node more of the
 * longest key seen, and as many more as a margin asks for.
 */
static bool
txn_memtable_split_estimated(uint64 nodes, uint64 keys, uint64 bytes, uint64 longest, uint64 margin,
							 StdVec *result, HostError *error)
{
	uint64		each = std_div_ceil_u64(bytes, Max(keys, 1), RUST_FILE, 54, 16);

	if (!std_vec_try_reserve(result,
							 (size_t) STD_ADD(u64, STD_ADD(u64, nodes, 1, 56, 24), margin,
											  56, 24),
							 sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	txn_memtable_split_Needs_push(result, txn_memtable_index_need(each, TXN_MEMTABLE_INDEX_HEIGHT), 1);
	txn_memtable_split_Needs_push(result, txn_memtable_index_need(each, 2), nodes > 0 ? nodes - 1 : 0);
	txn_memtable_split_Needs_push(result, txn_memtable_index_need(longest, 2), 1);
	txn_memtable_split_Needs_push(result, txn_memtable_index_need(each, 2), margin);
	return true;
}

/* txn_memtable_split_exact writes a half's needs from its nodes into an empty Vec, its first at the full height. */
static bool
txn_memtable_split_exact(const TxnMemtableIndexCopied *nodes, size_t nodes_len, StdVec *result, HostError *error)
{
	if (!std_vec_try_reserve(result, nodes_len, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	for (size_t index = 0; index < nodes_len; index++)
	{
		StdBytes	key = host_delta_Lent_Deref_deref(&nodes[index].key);

		txn_memtable_split_Needs_push(result,
									  txn_memtable_index_need((uint64) key.len,
															  index == 0 ? TXN_MEMTABLE_INDEX_HEIGHT : nodes[index].height),
									  1);
	}
	return true;
}

/*
 * txn_memtable_split_fits reports whether a node's need fits a page of a size
 * after the bytes and words handed out there, as layout and planned check
 * it, with their checked sums' locations.
 */
static bool
txn_memtable_split_fits(HostDeltaPageSize size, uint64 bytes, uint64 words, std_TupleU64U64_t need,
						uint32 bytes_line, uint32 bytes_col, uint32 words_line, uint32 words_col)
{
	return STD_ADD(u64, bytes, need.f0, bytes_line, bytes_col) <= (uint64) size.bytes &&
		STD_ADD(u64, STD_ADD(u64, TXN_MEMTABLE_PAGES_FIRST, words, words_line, words_col), need.f1, words_line,
				words_col) <= (uint64) size.words;
}

/*
 * txn_memtable_split_layout writes the sizes of the pages a half's nodes go
 * on into an empty Vec: each node on the current page while its bytes and
 * its words fit there, else on a next page, which pages' size sizes after
 * the current one.
 */
static bool
txn_memtable_split_layout(const StdVec *needs, StdVec *result, HostError *error)
{
	uint64		bytes = 0;
	uint64		words = 0;

	for (size_t k = 0; k < needs->len; k++)
	{
		std_TupleU64U64_t need = STD_VEC_AT(needs, std_TupleU64U64_t, k);
		HostDeltaPageSize *last = result->len > 0 ? &STD_VEC_AT(result, HostDeltaPageSize, result->len - 1) : NULL;
		HostDeltaPageSize size;

		if (last != NULL && txn_memtable_split_fits(*last, bytes, words, need, 81, 16,
													82, 16))
		{
			bytes = STD_ADD(u64, bytes, need.f0, 84, 31);
			words = STD_ADD(u64, words, need.f1, 84, 51);
			continue;
		}
		if (!std_vec_try_reserve(result, 1, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize)))
		{
			*error = host_Error_out_of_memory();
			return false;
		}
		size = txn_memtable_pages_size(last != NULL,
									   last != NULL ? STD_MUL(u64, (uint64) last->words, 32,
															  87, 46) : 0,
									   need.f0, need.f1);
		std_vec_push_Checked(result, &size, sizeof(size), _Alignof(HostDeltaPageSize));
		bytes = need.f0;
		words = need.f1;
	}
	return true;
}

/*
 * txn_memtable_split_planned places a half's nodes on pages of the sizes
 * given as layout places them, writing each node's page into an empty Vec and
 * the count of pages used, or answering some false when the pages run out.
 */
static bool
txn_memtable_split_planned(const StdVec *sizes, const StdVec *needs, bool *some, StdVec *plan, size_t *used,
						   HostError *error)
{
	size_t		page = 0;
	uint64		bytes = 0;
	uint64		words = 0;

	*some = false;
	if (!std_vec_try_reserve(plan, needs->len, sizeof(size_t), _Alignof(size_t)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	for (size_t k = 0; k < needs->len; k++)
	{
		std_TupleU64U64_t need = STD_VEC_AT(needs, std_TupleU64U64_t, k);

		for (;;)
		{
			if (page >= sizes->len)
				return true;
			if (txn_memtable_split_fits(STD_VEC_AT(sizes, HostDeltaPageSize, page), bytes, words, need,
										104, 16, 104, 63))
			{
				bytes = STD_ADD(u64, bytes, need.f0, 106, 35);
				words = STD_ADD(u64, words, need.f1, 106, 55);
				break;
			}
			page = STD_ADD(usize, page, 1, 109, 37);
			bytes = 0;
			words = 0;
		}
		std_vec_push_Checked(plan, &page, sizeof(page), _Alignof(size_t));
	}
	*used = STD_ADD(usize, page, 1, 113, 20);
	*some = true;
	return true;
}

/*
 * txn_memtable_split_margined writes a half's exact needs and a margin past
 * them into an empty Vec, a sixteenth of SPLIT more nodes at the range's mean
 * key length and two levels, for the pushes a retry can meet.
 */
static bool
txn_memtable_split_margined(const StdVec *needs, const TxnMemtableSplitSeen *seen, StdVec *result, HostError *error)
{
	if (!std_vec_try_reserve(result,
							 STD_ADD(usize, needs->len, (size_t) (TXN_MEMTABLE_SPLIT_SPLIT / 16),
									 243, 26),
							 sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t)))
	{
		*error = host_Error_out_of_memory();
		return false;
	}
	std_vec_extend_from_slice_Checked(result, needs->ptr, needs->len, sizeof(std_TupleU64U64_t),
									  _Alignof(std_TupleU64U64_t));
	txn_memtable_split_Needs_push(result,
								  txn_memtable_index_need(std_div_ceil_u64(seen->bytes, Max(seen->keys, 1), RUST_FILE,
																		   245, 53), 2),
								  TXN_MEMTABLE_SPLIT_SPLIT / 16);
	return true;
}

/* txn_memtable_split_Owneds_drop drops a Vec<Owned>'s pages from an index on, and its buffer. */
static void
txn_memtable_split_Owneds_drop(StdVec *pages, size_t from)
{
	std_ptr_drop_in_place_slice(STD_VEC_AT_SIZE(pages, from, sizeof(HostDeltaOwned)), pages->len - from,
								sizeof(HostDeltaOwned), host_delta_Owned_drop);
	std_vec_free_Checked(pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
}

/*
 * TxnMemtableSplitHalf is Half, what one half takes before the old range's
 * lock: its record, its pages and their sizes, and its list's tree pages,
 * each while live, and how many of its pages its With has taken.
 */
typedef struct TxnMemtableSplitHalf
{
	HostDeltaMade made;
	bool		made_live;
	/* sizes is a Vec<Size>. */
	StdVec		sizes;
	bool		sizes_live;
	/* pages is a Vec<Owned>, of which moved are taken. */
	StdVec		pages;
	bool		pages_live;
	size_t		moved;
	/* tree is a Vec<Owned>. */
	StdVec		tree;
	bool		tree_live;
} TxnMemtableSplitHalf;

/* txn_memtable_split_Half_drop drops what of a half is live: its record, its sizes, its pages, and its tree pages. */
static void
txn_memtable_split_Half_drop(TxnMemtableSplitHalf *self)
{
	if (self->made_live)
	{
		self->made_live = false;
		host_delta_Made_drop(&self->made);
	}
	if (self->sizes_live)
	{
		self->sizes_live = false;
		std_vec_free_Checked(&self->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	}
	if (self->pages_live)
	{
		self->pages_live = false;
		txn_memtable_split_Owneds_drop(&self->pages, self->moved);
	}
	if (self->tree_live)
	{
		self->tree_live = false;
		txn_memtable_split_Owneds_drop(&self->tree, 0);
	}
}

/* TxnMemtableSplitStepTag is Step's variants: what step 3 found under the old range's lock. */
typedef enum TxnMemtableSplitStepTag
{
	TXN_MEMTABLE_SPLIT_STEP_SPLIT,
	TXN_MEMTABLE_SPLIT_STEP_SHORT,
	TXN_MEMTABLE_SPLIT_STEP_DONE,
} TxnMemtableSplitStepTag;

/* TxnMemtableSplitKeepTag is Keep's variants: what step 2 found under the memtable's lock. */
typedef enum TxnMemtableSplitKeepTag
{
	TXN_MEMTABLE_SPLIT_KEEP_KEPT,
	TXN_MEMTABLE_SPLIT_KEEP_WANTS,
	TXN_MEMTABLE_SPLIT_KEEP_CLOSED,
} TxnMemtableSplitKeepTag;

/* txn_memtable_split_split_frame_0_t is split's state across its pad. */
typedef struct txn_memtable_split_split_frame_0_t
{
	const TxnMemtableTableTreeOps *tree;
	HostIoIo   *io;
	HostSharedRecord owner;
	TxnMemtableTableAt at;
	const TxnMemtableSplitSeen *seen;
	HostError  *error;
	bool		ok;

	/*
	 * known are each half's exact needs and the list's length from a short
	 * take, while known_live.
	 */
	StdVec		known[2];
	bool		known_live;
	uint64		known_length;
	size_t		reserve;
	uint32		tries;
	bool		kept;
	bool		ours;
	/* needs are each half's needs for this try, a Vec<(u64, u64)> each. */
	StdVec		needs[2];
	bool		needs_live[2];
	/* sizes is a Vec<Size> of every page this try takes. */
	StdVec		sizes;
	bool		sizes_live;
	/* reserved is a Vec<Owned> of the list pages a kept entry takes. */
	StdVec		reserved;
	bool		reserved_live;
	TxnMemtableSplitHalf halves[2];
	HostDeltaHold hold;
	bool		hold_live;

	/*
	 * step is what step 3 found, with whether this split's entry goes back
	 * and a short take's needs and length.
	 */
	TxnMemtableSplitStepTag step;
	bool		back;
	StdVec		short_needs[2];
	bool		short_live;
	uint64		short_length;
} txn_memtable_split_split_frame_0_t;

/* txn_memtable_split_free_needs frees a Vec<(u64, u64)> of needs. */
static void
txn_memtable_split_free_needs(StdVec *needs)
{
	std_vec_free_Checked(needs, sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
}

/* txn_memtable_split_split_try_drop drops what one try of split holds: the hold, the halves, the reserve, the sizes, and the needs, while live. */
static void
txn_memtable_split_split_try_drop(txn_memtable_split_split_frame_0_t *frame)
{
	if (frame->hold_live)
	{
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
	txn_memtable_split_Half_drop(&frame->halves[1]);
	txn_memtable_split_Half_drop(&frame->halves[0]);
	if (frame->reserved_live)
	{
		frame->reserved_live = false;
		txn_memtable_split_Owneds_drop(&frame->reserved, 0);
	}
	if (frame->sizes_live)
	{
		frame->sizes_live = false;
		std_vec_free_Checked(&frame->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	}
	for (int k = 1; k >= 0; k--)
		if (frame->needs_live[k])
		{
			frame->needs_live[k] = false;
			txn_memtable_split_free_needs(&frame->needs[k]);
		}
	if (frame->short_live)
	{
		frame->short_live = false;
		txn_memtable_split_free_needs(&frame->short_needs[0]);
		txn_memtable_split_free_needs(&frame->short_needs[1]);
	}
}

/* txn_memtable_split_split_pad_0 is split's pad: a try's holdings, then the needs a short take found, while live. */
static void
txn_memtable_split_split_pad_0(void *frame_)
{
	txn_memtable_split_split_frame_0_t *frame = frame_;

	txn_memtable_split_split_try_drop(frame);
	if (frame->known_live)
	{
		frame->known_live = false;
		txn_memtable_split_free_needs(&frame->known[0]);
		txn_memtable_split_free_needs(&frame->known[1]);
	}
}

/* txn_memtable_split_memtable_frame_0_t is the state of keep and given across their pad: the memtable's guard. */
typedef struct txn_memtable_split_memtable_frame_0_t
{
	HostIoIo   *io;
	const HostDeltaHold *hold;
	uint64		location;
	StdVec	   *spare;
	TxnMemtableSplitKeepTag keep;
	size_t		wants;
	HostError  *error;
	bool		ok;
	HostSharedGuard guard;
	bool		guard_live;
} txn_memtable_split_memtable_frame_0_t;

/* txn_memtable_split_memtable_pad_0 is the pad of keep and given: the guard, while live. */
static void
txn_memtable_split_memtable_pad_0(void *frame_)
{
	txn_memtable_split_memtable_frame_0_t *frame = frame_;

	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
}

/* txn_memtable_split_locked takes a memtable's exclusive lock under a hold, answering whether it is open. */
static bool
txn_memtable_split_locked(txn_memtable_split_memtable_frame_0_t *frame, TxnMemtableIndexOpen *open, bool *opened)
{
	HostSharedRecord memtable;

	if (!host_delta_Hold_record(frame->hold, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS, frame->location, &memtable,
								frame->error) ||
		!host_shared_Record_lock(memtable, HOST_IO_IO_TAKES_SHORT(frame->io), TXN_MEMTABLE_RANGE_MEMTABLE_LOCK,
								 &frame->guard, frame->error))
		return false;
	frame->guard_live = true;
	*opened = txn_memtable_index_open(&TXN_MEMTABLE_RANGE_MEMTABLE_CLOSES_OPS, &frame->guard, open);
	return true;
}

/* txn_memtable_split_keep_body_0 is keep's body. */
static void
txn_memtable_split_keep_body_0(void *frame_)
{
	txn_memtable_split_memtable_frame_0_t *frame = frame_;
	TxnMemtableIndexOpen open;
	bool		opened;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostSharedOp add = {.tag = HOST_SHARED_OP_ADD, .u.add = 1};
	HostDeltaListListView view;
	HostDeltaListListMut list;
	uint64		entries;

	if (!txn_memtable_split_locked(frame, &open, &opened))
	{
		txn_memtable_split_memtable_pad_0(frame);
		return;
	}
	frame->ok = true;
	if (!opened)
	{
		frame->keep = TXN_MEMTABLE_SPLIT_KEEP_CLOSED;
		txn_memtable_split_memtable_pad_0(frame);
		return;
	}
	view = host_delta_list_Guard_list(&frame->guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape,
									  TXN_MEMTABLE_RANGE_RANGES);
	entries = STD_ADD(u64,
					  STD_ADD(u64, host_delta_list_ListView_len(&view),
							  host_shared_Record_atomic(host_shared_Guard_record(&frame->guard),
														TXN_MEMTABLE_RANGE_MEMTABLE_KEPT, load),
							  255, 19),
					  1, 255, 19);
	frame->wants = host_delta_list_ListView_reserving(&view, entries);
	if (frame->wants > frame->spare->len)
	{
		frame->keep = TXN_MEMTABLE_SPLIT_KEEP_WANTS;
		txn_memtable_split_memtable_pad_0(frame);
		return;
	}
	list = host_delta_list_GuardExclusive_list_mut(&frame->guard, &TXN_MEMTABLE_RANGE_MEMTABLE_AGED_OPS.shape,
												   TXN_MEMTABLE_RANGE_MEMTABLE_LOCK, TXN_MEMTABLE_RANGE_RANGES);
	frame->ok = host_delta_list_ListMut_reserve(&list, frame->hold, entries, frame->spare, frame->error);
	if (frame->ok)
	{
		host_shared_Record_atomic(host_shared_Guard_record(&frame->guard), TXN_MEMTABLE_RANGE_MEMTABLE_KEPT, add);
		frame->keep = TXN_MEMTABLE_SPLIT_KEEP_KEPT;
	}
	txn_memtable_split_memtable_pad_0(frame);
}

/*
 * txn_memtable_split_keep keeps an entry of an open memtable's list for a
 * split, step 2: it grows the list's pages so that its entries, its kept
 * ones, and one more fit, and counts one more kept, or answers the list pages
 * it wants past those taken.
 */
static bool
txn_memtable_split_keep(HostIoIo *io, const HostDeltaHold *hold, uint64 location, StdVec *spare,
						TxnMemtableSplitKeepTag *result, size_t *wants, HostError *error)
{
	txn_memtable_split_memtable_frame_0_t frame = {.io = io, .hold = hold, .location = location, .spare = spare,
	.error = error};

	std_landing(&frame, txn_memtable_split_keep_body_0, txn_memtable_split_memtable_pad_0);
	*result = frame.keep;
	*wants = frame.wants;
	return frame.ok;
}

/* txn_memtable_split_given_body_0 is given's body. */
static void
txn_memtable_split_given_body_0(void *frame_)
{
	txn_memtable_split_memtable_frame_0_t *frame = frame_;
	TxnMemtableIndexOpen open;
	bool		opened;

	if (txn_memtable_split_locked(frame, &open, &opened))
	{
		HostSharedOp less = {.tag = HOST_SHARED_OP_ADD, .u.add = PG_UINT64_MAX};

		if (opened)
			host_shared_Record_atomic(host_shared_Guard_record(&frame->guard), TXN_MEMTABLE_RANGE_MEMTABLE_KEPT,
									  less);
		frame->ok = true;
	}
	txn_memtable_split_memtable_pad_0(frame);
}

/* txn_memtable_split_given gives an entry kept for a split back to an open memtable's list, step 5. */
static bool
txn_memtable_split_given(HostIoIo *io, const HostDeltaHold *hold, uint64 location, HostError *error)
{
	txn_memtable_split_memtable_frame_0_t frame = {.io = io, .hold = hold, .location = location, .error = error};

	std_landing(&frame, txn_memtable_split_given_body_0, txn_memtable_split_memtable_pad_0);
	return frame.ok;
}

/* txn_memtable_split_DeadNodes_t is an element of dead_nodes' result: an old page's live count and the old index's nodes on it. */
typedef struct txn_memtable_split_DeadNodes_t
{
	HostDeltaWord f0;
	uint64		f1;
} txn_memtable_split_DeadNodes_t;

/* txn_memtable_split_internal returns an internal error of a split. */
static HostError
txn_memtable_split_internal(const char *text)
{
	StdString	message;

	std_string_from_Checked(&message, text, strlen(text));
	return host_Error_fork((const uint8 *) "XX000", &message);
}

/* txn_memtable_split_halves_frame_0_t is halves' state across its pad. */
typedef struct txn_memtable_split_halves_frame_0_t
{
	txn_memtable_split_split_frame_0_t *split;
	HostError  *error;
	bool		ok;
	HostSharedGuard guard;
	bool		guard_live;
	/* nodes is a Vec<Copied> of the old range's index. */
	StdVec		nodes;
	bool		nodes_live;
	/* old is a Vec<u64> of the old range's list. */
	StdVec		old;
	bool		old_live;
	/* needs are each half's exact needs, a Vec<(u64, u64)> each. */
	StdVec		needs[2];
	bool		needs_live;

	/*
	 * plans are each half's nodes' pages, a Vec<usize> each, and the count of
	 * pages each uses.
	 */
	StdVec		plans[2];
	bool		plans_live[2];
	size_t		used[2];
	/* entries is a Vec<u64> of a half's list's entries. */
	StdVec		entries;
	bool		entries_live;

	/*
	 * filled is a Vec<Owned> a half's list's fill took, of which filled_moved
	 * are taken.
	 */
	StdVec		filled;
	bool		filled_live;
	size_t		filled_moved;
	HostDeltaWith withs[2];
	bool		withs_live[2];
	/* counts is a Vec<Word> of the old pages' counts of lists. */
	StdVec		counts;
	bool		counts_live;
	/* places is a Vec<u64> of the old index's nodes' pages. */
	StdVec		places;
	bool		places_live;
	/* dead is a Vec<(Word, u64)>. */
	StdVec		dead;
	bool		dead_live;
} txn_memtable_split_halves_frame_0_t;

/* txn_memtable_split_halves_pad_0 is halves' pad: its lists, the halves, the With's, and the guard, while live. */
static void
txn_memtable_split_halves_pad_0(void *frame_)
{
	txn_memtable_split_halves_frame_0_t *frame = frame_;

	if (frame->dead_live)
	{
		frame->dead_live = false;
		std_vec_free_Checked(&frame->dead, sizeof(txn_memtable_split_DeadNodes_t),
							 _Alignof(txn_memtable_split_DeadNodes_t));
	}
	if (frame->places_live)
	{
		frame->places_live = false;
		std_vec_free_Checked(&frame->places, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->counts_live)
	{
		frame->counts_live = false;
		std_vec_free_Checked(&frame->counts, sizeof(HostDeltaWord), _Alignof(HostDeltaWord));
	}
	if (frame->filled_live)
	{
		frame->filled_live = false;
		txn_memtable_split_Owneds_drop(&frame->filled, frame->filled_moved);
	}
	if (frame->entries_live)
	{
		frame->entries_live = false;
		std_vec_free_Checked(&frame->entries, sizeof(uint64), _Alignof(uint64));
	}
	for (int k = 1; k >= 0; k--)
	{
		txn_memtable_split_Half_drop(&frame->split->halves[k]);
		if (frame->withs_live[k])
		{
			frame->withs_live[k] = false;
			host_delta_With_drop(&frame->withs[k]);
		}
		if (frame->plans_live[k])
		{
			frame->plans_live[k] = false;
			std_vec_free_Checked(&frame->plans[k], sizeof(size_t), _Alignof(size_t));
		}
	}
	if (frame->needs_live)
	{
		frame->needs_live = false;
		txn_memtable_split_free_needs(&frame->needs[0]);
		txn_memtable_split_free_needs(&frame->needs[1]);
	}
	if (frame->old_live)
	{
		frame->old_live = false;
		std_vec_free_Checked(&frame->old, sizeof(uint64), _Alignof(uint64));
	}
	if (frame->nodes_live)
	{
		frame->nodes_live = false;
		for (size_t k = 0; k < frame->nodes.len; k++)
			txn_memtable_index_Copied_drop(&STD_VEC_AT(&frame->nodes, TxnMemtableIndexCopied, k));
		std_vec_free_Checked(&frame->nodes, sizeof(TxnMemtableIndexCopied), _Alignof(TxnMemtableIndexCopied));
	}
	if (frame->guard_live)
	{
		frame->guard_live = false;
		host_shared_Guard_drop(&frame->guard);
	}
}

/*
 * txn_memtable_split_dead_nodes gathers, for each old page that holds nodes
 * of the old range's index, its live count and how many of those nodes it
 * holds, which the split subtracts as the old index goes with the old range.
 */
static bool
txn_memtable_split_dead_nodes(txn_memtable_split_halves_frame_0_t *frame)
{
	const HostDeltaHold *hold = &frame->split->hold;
	size_t		start = 0;

	std_vec_new(&frame->places, sizeof(uint64), _Alignof(uint64));
	frame->places_live = true;
	if (!std_vec_try_reserve(&frame->places, frame->nodes.len, sizeof(uint64), _Alignof(uint64)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	for (size_t k = 0; k < frame->nodes.len; k++)
		std_vec_push_Checked(&frame->places, &STD_VEC_AT(&frame->nodes, TxnMemtableIndexCopied, k).page,
							 sizeof(uint64), _Alignof(uint64));
	std_slice_sort_unstable_U64(frame->places.ptr, frame->places.len);
	std_vec_new(&frame->dead, sizeof(txn_memtable_split_DeadNodes_t), _Alignof(txn_memtable_split_DeadNodes_t));
	frame->dead_live = true;
	while (start < frame->places.len)
	{
		uint64		first = STD_VEC_AT(&frame->places, uint64, start);
		size_t		end = start;
		txn_memtable_split_DeadNodes_t dead;
		HostDeltaPageRef page;

		while (end < frame->places.len && STD_VEC_AT(&frame->places, uint64, end) == first)
			end++;
		std_index_check(0, end - start, RUST_FILE, 285, 28);
		if (first >= frame->old.len)
		{
			*frame->error = txn_memtable_split_internal("a node names a page past its range's list");
			return false;
		}
		if (!std_vec_try_reserve(&frame->dead, 1, sizeof(dead), _Alignof(txn_memtable_split_DeadNodes_t)))
		{
			*frame->error = host_Error_out_of_memory();
			return false;
		}
		if (!host_delta_Hold_page(hold, host_delta_page_Page_named(STD_VEC_AT(&frame->old, uint64, first)), &page,
								  frame->error) ||
			!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_LIVE, &dead.f0, frame->error))
			return false;
		dead.f1 = (uint64) (end - start);
		std_vec_push_Checked(&frame->dead, &dead, sizeof(dead), _Alignof(txn_memtable_split_DeadNodes_t));
		start = end;
	}
	return true;
}

/*
 * txn_memtable_split_halves_built builds one half's index on its own pages,
 * fills its list with the old pages' names and its own, and gathers its
 * pages and its list's pages into its With.
 */
static bool
txn_memtable_split_halves_built(txn_memtable_split_halves_frame_0_t *frame, int k, const TxnMemtableIndexCopied *nodes,
								size_t nodes_len)
{
	TxnMemtableSplitHalf *half = &frame->split->halves[k];
	size_t		used = frame->used[k];
	HostSharedOp op = {.tag = HOST_SHARED_OP_EXCHANGE};

	std_vec_truncate(&half->pages, used, sizeof(HostDeltaOwned), host_delta_Owned_drop);
	if (!txn_memtable_index_built(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &half->made, half->pages.ptr, half->pages.len,
								  frame->plans[k].ptr, frame->plans[k].len, (uint64) frame->old.len, nodes, nodes_len,
								  frame->error))
		return false;
	std_index_check(0, half->pages.len, RUST_FILE, 338, 56);
	op.u.exchange.f1 = host_delta_page_Page_value(host_delta_Owned_name(&STD_VEC_AT(&half->pages, HostDeltaOwned, 0)));
	host_delta_Made_atomic(&half->made, TXN_MEMTABLE_RANGE_RANGE_START, op);
	std_index_check(STD_SUB(usize, used, 1, 339, 78), half->sizes.len, RUST_FILE,
					339, 67);
	op.u.exchange.f1 = (uint64) STD_VEC_AT(&half->sizes, HostDeltaPageSize, used - 1).words;
	host_delta_Made_atomic(&half->made, TXN_MEMTABLE_RANGE_RANGE_NEWEST, op);
	std_vec_new(&frame->entries, sizeof(uint64), _Alignof(uint64));
	frame->entries_live = true;
	if (!std_vec_try_reserve(&frame->entries, STD_ADD(usize, frame->old.len, used, 341, 29),
							 sizeof(uint64), _Alignof(uint64)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_extend_from_slice_Checked(&frame->entries, frame->old.ptr, frame->old.len, sizeof(uint64),
									  _Alignof(uint64));
	for (size_t p = 0; p < half->pages.len; p++)
	{
		uint64		name = host_delta_page_Page_value(host_delta_Owned_name(&STD_VEC_AT(&half->pages, HostDeltaOwned,
																						p)));

		std_vec_push_Checked(&frame->entries, &name, sizeof(name), _Alignof(uint64));
	}
	if (!host_delta_list_Made_fill_list(&half->made, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape, TXN_MEMTABLE_INDEX_PAGES,
										frame->entries.ptr, frame->entries.len, &half->tree, &frame->filled,
										frame->error))
		return false;
	frame->filled_live = true;
	frame->filled_moved = 0;
	frame->withs[k] = host_delta_With_Default_default();
	frame->withs_live[k] = true;
	while (half->moved < half->pages.len)
	{
		HostDeltaOwned page = STD_VEC_AT(&half->pages, HostDeltaOwned, half->moved);

		half->moved = std_wrapping_add_usize(half->moved, 1);
		if (!host_delta_With_page(&frame->withs[k], TXN_MEMTABLE_RANGE_RANGE_LOCK, &page, frame->error))
			return false;
	}
	while (frame->filled_moved < frame->filled.len)
	{
		HostDeltaOwned page = STD_VEC_AT(&frame->filled, HostDeltaOwned, frame->filled_moved);

		frame->filled_moved = std_wrapping_add_usize(frame->filled_moved, 1);
		if (!host_delta_With_page(&frame->withs[k], TXN_MEMTABLE_RANGE_RANGE_LOCK, &page, frame->error))
			return false;
	}
	frame->filled_live = false;
	std_vec_free_Checked(&frame->filled, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	half->pages_live = false;
	std_vec_free_Checked(&half->pages, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned));
	frame->entries_live = false;
	std_vec_free_Checked(&frame->entries, sizeof(uint64), _Alignof(uint64));
	frame->plans_live[k] = false;
	std_vec_free_Checked(&frame->plans[k], sizeof(size_t), _Alignof(size_t));
	half->sizes_live = false;
	std_vec_free_Checked(&half->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	half->tree_live = false;
	txn_memtable_split_Owneds_drop(&half->tree, 0);
	return true;
}

/*
 * txn_memtable_split_halves_read reads the old range under its lock, found
 * open and past SPLIT: it stores the range's kept or finds an earlier
 * splitter's, reads the index and the range's list, and plans each half on
 * the pages taken, answering a short step when they don't hold it.
 */
static bool
txn_memtable_split_halves_read(txn_memtable_split_halves_frame_0_t *frame, size_t *own, bool *planned)
{
	txn_memtable_split_split_frame_0_t *split = frame->split;
	HostSharedRecord record = host_shared_Guard_record(&frame->guard);
	HostSharedOp claim = {.tag = HOST_SHARED_OP_EXCHANGE, .u.exchange = {0, 1}};
	HostDeltaListListView view;
	TxnMemtableIndexNode first;
	size_t		low;

	*planned = false;
	split->back = false;
	if (!split->ours)
	{
		if (host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_KEPT, claim) == 0)
			split->ours = true;
		else
			split->back = true;
	}
	if (!txn_memtable_index_copied(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &frame->guard, &split->hold, &frame->nodes,
								   frame->error))
		return false;
	frame->nodes_live = true;
	low = STD_DIV_FOLDED(usize, frame->nodes.len, 2);
	view = host_delta_list_Guard_list(&frame->guard, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape,
									  TXN_MEMTABLE_INDEX_PAGES);
	std_vec_new(&frame->old, sizeof(uint64), _Alignof(uint64));
	frame->old_live = true;
	for (uint64 entry = 0, len = host_delta_list_ListView_len(&view); entry < len; entry++)
	{
		uint64		name;

		if (!host_delta_list_ListView_get(&view, &split->hold, entry, &name, frame->error))
			return false;
		std_vec_push_Checked(&frame->old, &name, sizeof(name), _Alignof(uint64));
	}
	*own = txn_memtable_index_first(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS, &frame->guard, &first) ?
		(size_t) txn_memtable_index_Node_page(first) : 0;
	std_vec_new(&frame->needs[0], sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	std_vec_new(&frame->needs[1], sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
	frame->needs_live = true;
	if (!txn_memtable_split_exact(frame->nodes.ptr, low, &frame->needs[0], frame->error) ||
		!txn_memtable_split_exact(&STD_VEC_AT(&frame->nodes, TxnMemtableIndexCopied, low), frame->nodes.len - low,
								  &frame->needs[1], frame->error))
		return false;
	for (int k = 0; k < 2; k++)
	{
		bool		some;

		std_vec_new(&frame->plans[k], sizeof(size_t), _Alignof(size_t));
		frame->plans_live[k] = true;
		if (!txn_memtable_split_planned(&split->halves[k].sizes, &frame->needs[k], &some, &frame->plans[k],
										&frame->used[k], frame->error))
			return false;
		if (!some || host_delta_list_pages((uint64) STD_ADD(usize, frame->old.len, frame->used[k],
															324, 48)) >
			split->halves[k].tree.len)
		{
			frame->needs_live = false;
			split->short_needs[0] = frame->needs[0];
			split->short_needs[1] = frame->needs[1];
			split->short_live = true;
			split->short_length = (uint64) frame->old.len;
			split->step = TXN_MEMTABLE_SPLIT_STEP_SHORT;
			return true;
		}
	}
	*planned = true;
	return true;
}

/*
 * txn_memtable_split_halves_body_0 is halves' body, step 3 under the old
 * range's exclusive lock: it reads and plans the halves, seals the old
 * range's own pages, builds each half, takes the old pages' counts of lists
 * and the old index's pages' live counts, and last, with nothing that can
 * fail, counts each old page in both halves' lists, takes the old index's
 * nodes out of their pages' live counts, links each half into the range's
 * replacement words, and closes the range.
 */
static void
txn_memtable_split_halves_body_0(void *frame_)
{
	txn_memtable_split_halves_frame_0_t *frame = frame_;
	txn_memtable_split_split_frame_0_t *split = frame->split;
	HostSharedRecord record;
	TxnMemtableIndexOpen open;
	HostSharedOp load = {.tag = HOST_SHARED_OP_LOAD};
	HostSharedPast lower;
	HostSharedPast upper;
	HostDeltaNames words[2];
	size_t		own;
	size_t		low;
	bool		planned;

	split->step = TXN_MEMTABLE_SPLIT_STEP_DONE;
	if (!host_delta_Hold_record(&split->hold, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, split->at.range, &record,
								frame->error) ||
		!host_shared_Record_lock(record, HOST_IO_IO_TAKES_SHORT(split->io), TXN_MEMTABLE_RANGE_RANGE_LOCK,
								 &frame->guard, frame->error))
	{
		txn_memtable_split_halves_pad_0(frame);
		return;
	}
	frame->guard_live = true;
	if (!txn_memtable_index_open(&TXN_MEMTABLE_RANGE_RANGE_KEYED_OPS.closes, &frame->guard, &open) ||
		host_shared_Record_atomic(record, TXN_MEMTABLE_RANGE_RANGE_KEYS, load) <= TXN_MEMTABLE_SPLIT_SPLIT)
	{
		frame->ok = true;
		txn_memtable_split_halves_pad_0(frame);
		return;
	}
	if (!txn_memtable_split_halves_read(frame, &own, &planned) || !planned)
	{
		frame->ok = split->step == TXN_MEMTABLE_SPLIT_STEP_SHORT;
		txn_memtable_split_halves_pad_0(frame);
		return;
	}
	std_range_from_check(own, frame->old.len, RUST_FILE, 331, 18);
	for (size_t k = own; k < frame->old.len; k++)
	{
		uint64		name = STD_VEC_AT(&frame->old, uint64, k);
		HostDeltaPageRef page;

		if (name == TXN_MEMTABLE_PAGES_DROPPED)
			continue;
		if (!host_delta_Hold_page(&split->hold, host_delta_page_Page_named(name), &page, frame->error) ||
			!host_delta_PageRef_seal(&page, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS.shape, TXN_MEMTABLE_RANGE_RANGE_LOCK,
									 &frame->guard, frame->error))
		{
			txn_memtable_split_halves_pad_0(frame);
			return;
		}
	}
	low = STD_DIV_FOLDED(usize, frame->nodes.len, 2);
	if (!txn_memtable_split_halves_built(frame, 0, frame->nodes.ptr, low) ||
		!txn_memtable_split_halves_built(frame, 1, &STD_VEC_AT(&frame->nodes, TxnMemtableIndexCopied, low),
										 frame->nodes.len - low))
	{
		txn_memtable_split_halves_pad_0(frame);
		return;
	}
	std_vec_new(&frame->counts, sizeof(HostDeltaWord), _Alignof(HostDeltaWord));
	frame->counts_live = true;
	for (size_t k = 0; k < frame->old.len; k++)
	{
		uint64		name = STD_VEC_AT(&frame->old, uint64, k);
		HostDeltaPageRef page;
		HostDeltaWord count;

		if (name == TXN_MEMTABLE_PAGES_DROPPED)
			continue;
		if (!host_delta_Hold_page(&split->hold, host_delta_page_Page_named(name), &page, frame->error) ||
			!host_delta_PageRef_word(&page, TXN_MEMTABLE_PAGES_LISTS, &count, frame->error))
		{
			txn_memtable_split_halves_pad_0(frame);
			return;
		}
		std_vec_push_Checked(&frame->counts, &count, sizeof(count), _Alignof(HostDeltaWord));
	}
	if (!txn_memtable_split_dead_nodes(frame))
	{
		txn_memtable_split_halves_pad_0(frame);
		return;
	}
	for (size_t k = 0; k < frame->counts.len; k++)
		host_delta_Word_fetch_add(&STD_VEC_AT(&frame->counts, HostDeltaWord, k), 2);
	for (size_t k = 0; k < frame->dead.len; k++)
	{
		txn_memtable_split_DeadNodes_t *dead = &STD_VEC_AT(&frame->dead, txn_memtable_split_DeadNodes_t, k);

		host_delta_Word_fetch_add(&dead->f0, std_wrapping_sub_u64(0, dead->f1));
	}
	lower = host_shared_Record_names_past(record, TXN_MEMTABLE_RANGE_RANGE_LOWER);
	upper = host_shared_Record_names_past(record, TXN_MEMTABLE_RANGE_RANGE_UPPER);
	words[0].ops = &HOST_DELTA_PAST_NAMES_OPS;
	words[0].self = &lower;
	words[1].ops = &HOST_DELTA_PAST_NAMES_OPS;
	words[1].self = &upper;
	for (int k = 0; k < 2; k++)
	{
		split->halves[k].made_live = false;
		frame->withs_live[k] = false;
		host_delta_Made_link(&split->halves[k].made, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, words[k], &frame->withs[k]);
	}
	txn_memtable_index_Open_close(open);
	split->step = TXN_MEMTABLE_SPLIT_STEP_SPLIT;
	frame->ok = true;
	txn_memtable_split_halves_pad_0(frame);
}

/* txn_memtable_split_halves runs step 3 on split's halves, which it takes, setting split's step. */
static bool
txn_memtable_split_halves(txn_memtable_split_split_frame_0_t *split, HostError *error)
{
	txn_memtable_split_halves_frame_0_t frame = {.split = split, .error = error};

	std_landing(&frame, txn_memtable_split_halves_body_0, txn_memtable_split_halves_pad_0);
	return frame.ok;
}

/* txn_memtable_split_split_needs writes each half's needs for a try: estimated from what the writer saw, or margined past a short take's. */
static bool
txn_memtable_split_split_needs(txn_memtable_split_split_frame_0_t *frame, uint64 *length)
{
	const TxnMemtableSplitSeen *seen = frame->seen;

	for (int k = 0; k < 2; k++)
	{
		std_vec_new(&frame->needs[k], sizeof(std_TupleU64U64_t), _Alignof(std_TupleU64U64_t));
		frame->needs_live[k] = true;
	}
	if (frame->known_live)
	{
		*length = frame->known_length;
		return txn_memtable_split_margined(&frame->known[0], seen, &frame->needs[0], frame->error) &&
			txn_memtable_split_margined(&frame->known[1], seen, &frame->needs[1], frame->error);
	}
	*length = seen->pages;
	return txn_memtable_split_estimated(STD_DIV_FOLDED(u64, seen->keys, 2), seen->keys, seen->bytes, seen->longest, 0,
										&frame->needs[0], frame->error) &&
		txn_memtable_split_estimated(STD_SUB(u64, seen->keys, STD_DIV_FOLDED(u64, seen->keys, 2),
											 162, 35),
									 seen->keys, seen->bytes, seen->longest, 0, &frame->needs[1], frame->error);
}

/*
 * txn_memtable_split_split_taken takes a try's pages and the halves'
 * records, step 1, with no hold and no lock, answering ok false after giving
 * them back and waiting when the delta can't give them now.
 */
static bool
txn_memtable_split_split_taken(txn_memtable_split_split_frame_0_t *frame, bool *ok)
{
	TxnMemtableSplitHalf *lower = &frame->halves[0];
	TxnMemtableSplitHalf *upper = &frame->halves[1];
	HostDeltaPageSize list_page = HOST_DELTA_LIST_PAGE;
	uint64		length;
	size_t		trees[2];
	size_t		lists;
	size_t		at;
	StdVec		taken;
	HostDeltaFull full;
	bool		made_ok[2];
	HostDeltaFull made_full[2];

	if (!txn_memtable_split_split_needs(frame, &length))
		return false;
	for (int k = 0; k < 2; k++)
	{
		std_vec_new(&frame->halves[k].sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
		frame->halves[k].sizes_live = true;
		if (!txn_memtable_split_layout(&frame->needs[k], &frame->halves[k].sizes, frame->error))
			return false;
	}
	trees[0] = host_delta_list_pages(STD_ADD(u64, length, (uint64) lower->sizes.len,
											 167, 34));
	trees[1] = host_delta_list_pages(STD_ADD(u64, length, (uint64) upper->sizes.len,
											 167, 82));
	std_vec_new(&frame->sizes, sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize));
	frame->sizes_live = true;
	if (!std_vec_try_reserve(&frame->sizes,
							 STD_ADD(usize,
									 STD_ADD(usize,
											 STD_ADD(usize,
													 STD_ADD(usize, lower->sizes.len, upper->sizes.len,
															 170, 26),
													 trees[0], 170, 26),
											 trees[1], 170, 26),
									 frame->reserve, 170, 26),
							 sizeof(HostDeltaPageSize), _Alignof(HostDeltaPageSize)))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_extend_from_slice_Checked(&frame->sizes, lower->sizes.ptr, lower->sizes.len, sizeof(HostDeltaPageSize),
									  _Alignof(HostDeltaPageSize));
	std_vec_extend_from_slice_Checked(&frame->sizes, upper->sizes.ptr, upper->sizes.len, sizeof(HostDeltaPageSize),
									  _Alignof(HostDeltaPageSize));
	lists = STD_ADD(usize, STD_ADD(usize, trees[0], trees[1], 174, 54), frame->reserve,
					174, 54);
	for (size_t k = 0; k < lists; k++)
		std_vec_push_Checked(&frame->sizes, &list_page, sizeof(list_page), _Alignof(HostDeltaPageSize));
	if (!host_delta_alloc(frame->io, frame->sizes.ptr, frame->sizes.len, ok, &taken, &full, frame->error))
		return false;
	if (!*ok)
		return txn_memtable_table_waited(frame->io, full, false, frame->error);
	lower->pages = taken;
	lower->pages_live = true;
	lower->moved = 0;
	at = STD_ADD(usize, STD_ADD(usize, STD_ADD(usize, lower->sizes.len, upper->sizes.len,
											   182, 44),
								trees[0], 182, 44),
				 trees[1], 182, 44);
	std_vec_split_off(&lower->pages, at, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), &frame->reserved, RUST_FILE,
					  182, 28);
	frame->reserved_live = true;
	at = STD_ADD(usize, STD_ADD(usize, lower->sizes.len, upper->sizes.len,
								183, 42),
				 trees[0], 183, 42);
	std_vec_split_off(&lower->pages, at, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), &upper->tree, RUST_FILE,
					  183, 26);
	upper->tree_live = true;
	at = STD_ADD(usize, lower->sizes.len, upper->sizes.len, 184, 42);
	std_vec_split_off(&lower->pages, at, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), &lower->tree, RUST_FILE,
					  184, 26);
	lower->tree_live = true;
	std_vec_split_off(&lower->pages, lower->sizes.len, sizeof(HostDeltaOwned), _Alignof(HostDeltaOwned), &upper->pages,
					  RUST_FILE, 185, 27);
	upper->pages_live = true;
	upper->moved = 0;
	if (!host_delta_make(frame->io, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, &made_ok[0], &lower->made, &made_full[0],
						 frame->error))
		return false;
	lower->made_live = made_ok[0];
	if (!host_delta_make(frame->io, &TXN_MEMTABLE_RANGE_RANGE_AGED_OPS, &made_ok[1], &upper->made, &made_full[1],
						 frame->error))
		return false;
	upper->made_live = made_ok[1];
	*ok = made_ok[0] && made_ok[1];
	if (*ok)
		return true;
	lower->pages_live = false;
	txn_memtable_split_Owneds_drop(&lower->pages, 0);
	upper->pages_live = false;
	txn_memtable_split_Owneds_drop(&upper->pages, 0);
	lower->tree_live = false;
	txn_memtable_split_Owneds_drop(&lower->tree, 0);
	upper->tree_live = false;
	txn_memtable_split_Owneds_drop(&upper->tree, 0);
	frame->reserved_live = false;
	txn_memtable_split_Owneds_drop(&frame->reserved, 0);
	return txn_memtable_table_waited(frame->io, made_ok[0] ? made_full[1] : made_full[0], false, frame->error);
}

/* txn_memtable_split_split_stepped acts on what step 3 found, answering whether the split is done. */
static bool
txn_memtable_split_split_stepped(txn_memtable_split_split_frame_0_t *frame, bool *done)
{
	*done = true;
	switch (frame->step)
	{
		case TXN_MEMTABLE_SPLIT_STEP_SPLIT:
			return host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-memtable-split", frame->error) &&
				txn_memtable_table_unlisted(frame->tree, frame->io, &frame->hold, frame->owner, frame->at,
											frame->error) &&
				(!frame->back || txn_memtable_split_given(frame->io, &frame->hold, frame->at.memtable, frame->error));
		case TXN_MEMTABLE_SPLIT_STEP_SHORT:
			frame->tries = STD_ADD(u32, frame->tries, 1, 219, 17);
			if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-memtable-split-short", frame->error))
				return false;
			if (frame->tries == TXN_MEMTABLE_SPLIT_TRIES)
				return frame->ours || txn_memtable_split_given(frame->io, &frame->hold, frame->at.memtable,
															   frame->error);
			if (frame->known_live)
			{
				frame->known_live = false;
				txn_memtable_split_free_needs(&frame->known[0]);
				txn_memtable_split_free_needs(&frame->known[1]);
			}
			frame->short_live = false;
			frame->known[0] = frame->short_needs[0];
			frame->known[1] = frame->short_needs[1];
			frame->known_length = frame->short_length;
			frame->known_live = true;
			*done = false;
			return true;
		case TXN_MEMTABLE_SPLIT_STEP_DONE:
			return frame->ours || txn_memtable_split_given(frame->io, &frame->hold, frame->at.memtable, frame->error);
	}
	return true;
}

/* txn_memtable_split_split_body_0 is split's body. */
static void
txn_memtable_split_split_body_0(void *frame_)
{
	txn_memtable_split_split_frame_0_t *frame = frame_;

	for (;;)
	{
		bool		ok;
		bool		done;

		if (!txn_memtable_split_split_taken(frame, &ok))
			break;
		if (!ok)
		{
			txn_memtable_split_split_try_drop(frame);
			continue;
		}
		if (!host_delta_hold(frame->io, &frame->hold, frame->error))
			break;
		frame->hold_live = true;
		if (!frame->kept)
		{
			TxnMemtableSplitKeepTag keep;
			size_t		wants;

			if (!txn_memtable_split_keep(frame->io, &frame->hold, frame->at.memtable, &frame->reserved, &keep, &wants,
										 frame->error))
				break;
			if (keep == TXN_MEMTABLE_SPLIT_KEEP_CLOSED)
			{
				frame->ok = true;
				break;
			}
			if (keep == TXN_MEMTABLE_SPLIT_KEEP_WANTS)
			{
				frame->reserve = wants;
				txn_memtable_split_split_try_drop(frame);
				continue;
			}
			frame->kept = true;
		}
		frame->reserved_live = false;
		txn_memtable_split_Owneds_drop(&frame->reserved, 0);
		if (!txn_memtable_split_halves(frame, frame->error) || !txn_memtable_split_split_stepped(frame, &done))
			break;
		if (done)
		{
			frame->ok = true;
			break;
		}
		txn_memtable_split_split_try_drop(frame);
	}
	txn_memtable_split_split_pad_0(frame);
}

/*
 * txn_memtable_split_split replaces a range the pushing writer found past
 * SPLIT keys by its two halves, PLAN's steps: with no hold and no lock it
 * takes the halves' pages and records (1); keeps an entry of the memtable's
 * list (2); under the old range's lock builds the halves and links them into
 * the range's replacement words, and closes it (3); puts the halves in the
 * list as any finder of the closed range would (4); and gives its entry back
 * when it split nothing or an earlier splitter's entry was the range's (5).
 * Once the range's kept is its own, every exit leaves its entry with the
 * range for the next split, and after TRIES short takes it gives up, leaving
 * the range whole. An error before the range's kept is stored, of the take
 * of the range's record or its lock, leaves the split's entry kept, which
 * costs its place in the list until the memtable goes, since a give-back's
 * own error would leave one error unreturned.
 */
bool
txn_memtable_split_split(const TxnMemtableTableTreeOps *tree, HostIoIo *io, HostSharedRecord owner,
						 TxnMemtableTableAt at, const TxnMemtableSplitSeen *seen, HostError *error)
{
	txn_memtable_split_split_frame_0_t frame = {.tree = tree, .io = io, .owner = owner, .at = at, .seen = seen,
	.error = error, .reserve = 1};

	std_landing(&frame, txn_memtable_split_split_body_0, txn_memtable_split_split_pad_0);
	return frame.ok;
}
