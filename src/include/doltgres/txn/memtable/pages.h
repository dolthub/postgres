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
 * A range's pages (txn's memtable/pages.rs): their counting words, the size
 * of a range's next page, and where one call's bytes and words go, or what it
 * wants before it tries again. Part of txn's contract.
 */
#ifndef DOLTGRES_TXN_MEMTABLE_PAGES_H
#define DOLTGRES_TXN_MEMTABLE_PAGES_H

#include "doltgres/host/delta.h"
#include "doltgres/host/delta/page.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable/index.h"

/* TXN_MEMTABLE_PAGES_COUNT is COUNT, the word of a range page that counts the words handed out of it, the four counts among them. */
#define TXN_MEMTABLE_PAGES_COUNT UINT64CONST(0)

/* TXN_MEMTABLE_PAGES_LIVE is LIVE, the word of a range page that counts its live records. */
#define TXN_MEMTABLE_PAGES_LIVE UINT64CONST(1)

/* TXN_MEMTABLE_PAGES_LISTS is LISTS, the word of a range page that counts the ranges' lists that name it. */
#define TXN_MEMTABLE_PAGES_LISTS UINT64CONST(2)

/* TXN_MEMTABLE_PAGES_DEAD is DEAD, the word of a range page that counts the bytes of the records the remover took out of it. */
#define TXN_MEMTABLE_PAGES_DEAD UINT64CONST(3)

/* TXN_MEMTABLE_PAGES_FIRST is FIRST, the first word a range page hands out, past its four counts. */
#define TXN_MEMTABLE_PAGES_FIRST UINT64CONST(4)

/* TXN_MEMTABLE_PAGES_DROPPED is DROPPED, what a range's list stores over the index of a page the remover dropped. */
#define TXN_MEMTABLE_PAGES_DROPPED PG_UINT64_MAX

/* TXN_MEMTABLE_PAGES_LARGEST is LARGEST, the largest page a range takes, one block of the delta's region. */
#define TXN_MEMTABLE_PAGES_LARGEST HOST_DELTA_PAGE_LARGEST

/* TxnMemtablePagesWant is Want: what a call that found no room takes, a fresh page of a size and its list's spares. */
typedef struct TxnMemtablePagesWant
{
	HostDeltaPageSize page;
	size_t		spares;
} TxnMemtablePagesWant;

/* TxnMemtablePagesTargetTag is Target's variants. */
typedef enum TxnMemtablePagesTargetTag
{
	TXN_MEMTABLE_PAGES_TARGET_NEWEST,
	TXN_MEMTABLE_PAGES_TARGET_FRESH,
} TxnMemtablePagesTargetTag;

/* TxnMemtablePagesTarget is Target: the page a call's bytes and words go on, the range's newest or the fresh page given. */
typedef struct TxnMemtablePagesTarget
{
	TxnMemtablePagesTargetTag tag;
	union
	{
		HostDeltaPageRef newest;
		HostDeltaOwned fresh;
	}			u;
} TxnMemtablePagesTarget;

/*
 * TxnMemtablePagesRoom is Room: where one call's bytes and words go, the open
 * record they go to, its page, the page's index in the range's list, the
 * first of the words handed out to the call, their count, and the count of
 * the live records the call appended.
 */
typedef struct TxnMemtablePagesRoom
{
	TxnMemtableIndexKeyedOpen *open;
	TxnMemtablePagesTarget target;
	uint64		page;
	uint64		first;
	uint64		words;
	uint64		records;
} TxnMemtablePagesRoom;

extern bool txn_memtable_pages_Want_Debug_fmt(const void *value, StdFormatter *f);
extern HostDeltaPageSize txn_memtable_pages_size(bool has_newest, uint64 newest, uint64 bytes, uint64 words);
extern bool txn_memtable_pages_room(TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold, uint64 bytes,
									uint64 words,
									bool *has_fresh, HostDeltaOwned *fresh, const HostDeltaOwned *spare,
									size_t spare_len, bool *result_ok, TxnMemtablePagesRoom *result,
									TxnMemtablePagesWant *result_err, HostError *error);
extern void txn_memtable_pages_Room_drop(TxnMemtablePagesRoom *self);
extern bool txn_memtable_pages_Room_append(TxnMemtablePagesRoom *self, const uint8 *record, size_t record_len,
										   uint32 *result, HostError *error);
extern bool txn_memtable_pages_Room_store(TxnMemtablePagesRoom *self, uint64 word, uint64 value, HostError *error);
extern bool txn_memtable_pages_Room_finish(TxnMemtablePagesRoom self,
										   const HostDeltaHold *hold, StdVec *spare, TxnMemtableIndexKeyedOpen **result,
										   HostError *error);

#endif							/* DOLTGRES_TXN_MEMTABLE_PAGES_H */
