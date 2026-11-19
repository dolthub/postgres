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
 * A memtable's index (txn's memtable/index.rs): a record found open under
 * its lock held exclusively, its key nodes, and a lookup's node found or
 * prepared. Part of txn's contract.
 */
#ifndef DOLTGRES_TXN_MEMTABLE_INDEX_H
#define DOLTGRES_TXN_MEMTABLE_INDEX_H

#include "doltgres/host/delta.h"
#include "doltgres/host/shared.h"
#include "doltgres/std/tuple.h"
#include "doltgres/std/vec.h"

/* TXN_MEMTABLE_INDEX_HEIGHT is HEIGHT, the most levels a node of a range's index links at. */
#define TXN_MEMTABLE_INDEX_HEIGHT 8

/* TXN_MEMTABLE_INDEX_PAGES is PAGES, the index of a range's list of pages among its record's lists. */
#define TXN_MEMTABLE_INDEX_PAGES 0

/* TXN_MEMTABLE_INDEX_NONE is NONE, a link to no node and a head of no version. */
#define TXN_MEMTABLE_INDEX_NONE UINT64CONST(0)

/* TXN_MEMTABLE_INDEX_LONGEST is LONGEST, the longest key a node holds, whose length fills the 24 bits between its offset and its height. */
#define TXN_MEMTABLE_INDEX_LONGEST ((UINT64CONST(1) << 24) - 1)

/* TXN_MEMTABLE_INDEX_NODE is NODE, the most words a node takes: its head, its place word, and a link at each level. */
#define TXN_MEMTABLE_INDEX_NODE (2 + TXN_MEMTABLE_INDEX_HEIGHT)

/*
 * TxnMemtableIndexClosesOps is the Closes trait: a shape whose records close,
 * the lock of its first list, which opens a record for changes, and its
 * Closed atomic.
 */
typedef struct TxnMemtableIndexClosesOps
{
	const HostSharedShapeOps *shape;
	size_t		lock;
	size_t		closed;
} TxnMemtableIndexClosesOps;

/*
 * TxnMemtableIndexKeyedOps is the Keyed trait: a shape whose records index
 * keys, its Closes, and its atomics First, Count, Bytes, and Newest.
 */
typedef struct TxnMemtableIndexKeyedOps
{
	TxnMemtableIndexClosesOps closes;
	size_t		first;
	size_t		count;
	size_t		bytes;
	size_t		newest;
} TxnMemtableIndexKeyedOps;

/* TxnMemtablePagesRoom is pages.h's Room, which New's store reaches only by pointer, as pages.h includes index.h. */
struct TxnMemtablePagesRoom;

/* TxnMemtableIndexNode is Node: a key node's name, its page's index in the range's list of pages above its first link word's. */
typedef struct TxnMemtableIndexNode
{
	uint64		f0;
} TxnMemtableIndexNode;

/*
 * TxnMemtableIndexOpen is Open<S>: a record found open under its lock held
 * exclusively, the value every change to its index takes; S, a range or a
 * memtable, is the guard's shape, whose Closes it carries.
 */
typedef struct TxnMemtableIndexOpen
{
	const TxnMemtableIndexClosesOps *closes;
	HostSharedGuard *guard;
} TxnMemtableIndexOpen;

/*
 * TxnMemtableIndexKeyedOpen is Open<S> at a Keyed S: an open record and its
 * shape's Keyed, whose Closes the open carries, made only by keyed_open, so
 * the two never disagree.
 */
typedef struct TxnMemtableIndexKeyedOpen
{
	TxnMemtableIndexOpen open;
	const TxnMemtableIndexKeyedOps *keyed;
} TxnMemtableIndexKeyedOpen;

/*
 * TxnMemtableIndexNew is New: a node prepared for a key, its height, its
 * key's length, its link at each level, the old first node it goes before
 * when it is the new first node, and otherwise its predecessor at each level.
 */
typedef struct TxnMemtableIndexNew
{
	uint64		levels;
	uint64		bytes;
	uint64		links[TXN_MEMTABLE_INDEX_HEIGHT];
	uint64		first;
	bool		has_before;
	TxnMemtableIndexNode before[TXN_MEMTABLE_INDEX_HEIGHT];
} TxnMemtableIndexNew;

/* TxnMemtableIndexFoundTag is Found's variants. */
typedef enum TxnMemtableIndexFoundTag
{
	TXN_MEMTABLE_INDEX_FOUND_NODE,
	TXN_MEMTABLE_INDEX_FOUND_NEW,
} TxnMemtableIndexFoundTag;

/* TxnMemtableIndexFound is Found: a lookup's key's node, or a node prepared for a key the index doesn't hold. */
typedef struct TxnMemtableIndexFound
{
	TxnMemtableIndexFoundTag tag;
	union
	{
		TxnMemtableIndexNode node;
		TxnMemtableIndexNew new_;
	}			u;
} TxnMemtableIndexFound;

/*
 * TxnMemtableIndexCopied is Copied: a node of a record's index as a split
 * reads it, its key, its head, its height, and the index of its page in the
 * record's list.
 */
typedef struct TxnMemtableIndexCopied
{
	HostDeltaLent key;
	uint64		head;
	uint64		height;
	uint64		page;
} TxnMemtableIndexCopied;

extern bool txn_memtable_index_closed(const TxnMemtableIndexClosesOps *closes, const HostSharedGuard *guard);
extern bool txn_memtable_index_keyed_open(const TxnMemtableIndexKeyedOps *keyed, HostSharedGuard *guard,
										  TxnMemtableIndexKeyedOpen *result);
extern bool txn_memtable_index_open(const TxnMemtableIndexClosesOps *closes, HostSharedGuard *guard,
									TxnMemtableIndexOpen *result);
extern void txn_memtable_index_Open_close(TxnMemtableIndexOpen self);
extern uint64 txn_memtable_index_Node_page(TxnMemtableIndexNode self);
extern HostDeltaPageSize txn_memtable_index_least(const uint8 *key, size_t key_len);
extern uint64 txn_memtable_index_height(const uint8 seed[32], const uint8 *key, size_t key_len);
extern bool txn_memtable_index_first(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
									 TxnMemtableIndexNode *result);
extern bool txn_memtable_index_moved(const TxnMemtableIndexKeyedOpen *kopen,
									 const HostDeltaHold *hold, uint64 entry,
									 HostDeltaOwned *copy, StdVec *result, HostError *error);
extern bool txn_memtable_index_key(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
								   const HostDeltaHold *hold, TxnMemtableIndexNode node, HostDeltaLent *result,
								   HostError *error);
extern bool txn_memtable_index_unlink(const TxnMemtableIndexKeyedOpen *kopen,
									  const HostDeltaHold *hold,
									  TxnMemtableIndexNode node, HostError *error);
extern bool txn_memtable_index_begin(const TxnMemtableIndexKeyedOps *keyed, const HostDeltaMade *made,
									 HostDeltaOwned *page, uint64 index, const uint8 *key, size_t key_len,
									 HostError *error);
extern bool txn_memtable_index_begun(HostDeltaPageRef page, HostDeltaLent *result, HostError *error);
extern void txn_memtable_index_Copied_drop(TxnMemtableIndexCopied *self);
extern bool txn_memtable_index_copied(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
									  const HostDeltaHold *hold, StdVec *result, HostError *error);
extern std_TupleU64U64_t txn_memtable_index_need(uint64 key, uint64 height);
extern bool txn_memtable_index_built(const TxnMemtableIndexKeyedOps *keyed, const HostDeltaMade *made,
									 HostDeltaOwned *pages, size_t pages_len, const size_t *plan, size_t plan_len,
									 uint64 base, const TxnMemtableIndexCopied *nodes, size_t nodes_len,
									 HostError *error);
extern bool txn_memtable_index_next(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
									const HostDeltaHold *hold, TxnMemtableIndexNode node, bool *some,
									TxnMemtableIndexNode *result, HostError *error);
extern bool txn_memtable_index_seek(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
									const HostDeltaHold *hold, const uint8 *key, size_t key_len, bool *some,
									TxnMemtableIndexNode *result, HostError *error);
extern bool txn_memtable_index_find(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
									const HostDeltaHold *hold, const uint8 *key, size_t key_len, bool *some,
									TxnMemtableIndexNode *result, HostError *error);
extern bool txn_memtable_index_before(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
									  const HostDeltaHold *hold, const uint8 *key, size_t key_len, bool *some,
									  TxnMemtableIndexNode *result, HostError *error);
extern bool txn_memtable_index_last(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
									const HostDeltaHold *hold, bool *some, TxnMemtableIndexNode *result,
									HostError *error);
extern bool txn_memtable_index_head(const TxnMemtableIndexKeyedOps *keyed, const HostSharedGuard *guard,
									const HostDeltaHold *hold, TxnMemtableIndexNode node,
									uint64 *result, HostError *error);
extern bool txn_memtable_index_set_head(const TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold,
										TxnMemtableIndexNode node, uint64 head, HostError *error);
extern bool txn_memtable_index_lookup(const TxnMemtableIndexKeyedOpen *kopen,
									  const HostDeltaHold *hold, const uint8 *key, size_t key_len,
									  TxnMemtableIndexFound *result, HostError *error);
extern uint64 txn_memtable_index_New_words(const TxnMemtableIndexNew *self);
extern bool txn_memtable_index_New_store(const TxnMemtableIndexNew *self, struct TxnMemtablePagesRoom *room, uint64 at,
										 uint32 offset, const uint8 *key, size_t key_len, uint64 head,
										 TxnMemtableIndexNode *result, HostError *error);
extern bool txn_memtable_index_New_publish(TxnMemtableIndexNew self, const TxnMemtableIndexKeyedOpen *kopen,
										   const HostDeltaHold *hold,
										   TxnMemtableIndexNode node, HostError *error);

#endif							/* DOLTGRES_TXN_MEMTABLE_INDEX_H */
