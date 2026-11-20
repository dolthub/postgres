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
 * Which row versions a read sees (txn's visible.rs): heap's visibility rules
 * over a key's versions in the delta and its row in the base, a change's
 * HeapTupleSatisfiesUpdate, a rewrite's HeapTupleSatisfiesVacuum, and the
 * live transforms of a storage's chain.
 */
#ifndef DOLTGRES_TXN_VISIBLE_H
#define DOLTGRES_TXN_VISIBLE_H

#include "doltgres/host/delta.h"
#include "doltgres/host/io.h"
#include "doltgres/host/lib.h"
#include "doltgres/host/shared.h"
#include "doltgres/txn/base.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/version.h"

/*
 * TxnVisibleDirty is Dirty: what a read under a Dirty snapshot found
 * running, a running writer, a running deleter, and a speculative insert's
 * token.
 */
typedef struct TxnVisibleDirty
{
	bool		has_xmin;
	HostXid		xmin;
	bool		has_xmax;
	HostXid		xmax;
	bool		has_token;
	uint32		token;
} TxnVisibleDirty;

/* TxnVisibleRowVersion is RowVersion: a version that writes a row, with what it puts. */
typedef struct TxnVisibleRowVersion
{
	TxnMemtableVersionVersion *version;
	TxnMemtableVersionPutRef put;
} TxnVisibleRowVersion;

/*
 * TxnVisibleSerial is Serial: what a serializable read checks the row
 * versions it examines against, its transaction's xmin and its own
 * top-level xid, none before it wrote.
 */
typedef struct TxnVisibleSerial
{
	HostXid		xmin;
	bool		has_own;
	HostXid		own;
} TxnVisibleSerial;

/*
 * TxnVisibleSeenRow is SeenRow: a row version a snapshot sees, what a Dirty
 * read found running, its changer, and its commander, NULL for none.
 */
typedef struct TxnVisibleSeenRow
{
	TxnVisibleRowVersion row;
	TxnVisibleDirty dirty;
	TxnMemtableVersionVersion *changer;
	TxnMemtableVersionVersion *commander;
} TxnVisibleSeenRow;

/* txn_TupleRowXid_t is the tuple (Row, Xid): a row version's tuple id and the writer it conflicts out with. */
typedef struct txn_TupleRowXid_t
{
	HostRow		f0;
	HostXid		f1;
} txn_TupleRowXid_t;

/*
 * TxnVisibleSeen is Seen: what a snapshot sees of a key's versions in the
 * delta, the row versions it sees, newest first, the versions that change
 * rows, which base asks about the key's rows in the base, and for a
 * serializable read the conflicts it found.
 */
typedef struct TxnVisibleSeen
{
	/* rows is a Vec<SeenRow>. */
	StdVec		rows;
	/* above is the versions that change rows, newest first, the C's own. */
	StdVec		above;
	/* conflicts is a Vec<(Row, Xid)>, newest first. */
	StdVec		conflicts;
} TxnVisibleSeen;

/* TxnVisibleRuleOps is Rule: whether a row version's writer began the row, and its ender ended it, for a read. */
typedef struct TxnVisibleRuleOps
{
	bool		(*begun) (const void *self, HostIoIo *io, const TxnVisibleRowVersion *row, TxnVisibleDirty *dirty,
						  bool *result, HostError *error);
	bool		(*ended) (const void *self, HostIoIo *io, TxnMemtableVersionVersion *ender,
						  TxnVisibleDirty *dirty, bool *result, HostError *error);
} TxnVisibleRuleOps;

/* TxnVisibleRule is an impl Rule: its table and its value. */
typedef struct TxnVisibleRule
{
	const TxnVisibleRuleOps *ops;
	const void *self;
} TxnVisibleRule;

/* TXN_VISIBLE_SNAPSHOT_RULE_OPS is Snapshot's Rule, heap's rule for the snapshot's kind. */
extern const TxnVisibleRuleOps TXN_VISIBLE_SNAPSHOT_RULE_OPS;

/* TxnVisibleVacuumed is Vacuumed: heap's HTSV_Result for a row version. */
typedef enum TxnVisibleVacuumed
{
	TXN_VISIBLE_VACUUMED_DEAD,
	TXN_VISIBLE_VACUUMED_RECENTLY_DEAD,
	TXN_VISIBLE_VACUUMED_DELETE_IN_PROGRESS,
	TXN_VISIBLE_VACUUMED_LIVE,
	TXN_VISIBLE_VACUUMED_INSERT_IN_PROGRESS,
} TxnVisibleVacuumed;

/*
 * TxnVisibleLive is Live: a pending transform whose DDL didn't abort, with
 * its commit's end, none while its transaction runs.
 */
typedef struct TxnVisibleLive
{
	TxnBasePending f0;
	bool		has_f1;
	uint64		f1;
} TxnVisibleLive;

/* TxnVisibleResolvedTag is Resolved's variants. */
typedef enum TxnVisibleResolvedTag
{
	/* A live row: its value, and its tuple id when the delta wrote it. */
	TXN_VISIBLE_RESOLVED_EXISTS,
	/* A running write to wait for: its writer, its row, and its token. */
	TXN_VISIBLE_RESOLVED_WAIT,
	/* The key is free. */
	TXN_VISIBLE_RESOLVED_FREE,
} TxnVisibleResolvedTag;

/* TxnVisibleResolved is Resolved: what an immediate key's resolve found before a Put. */
typedef struct TxnVisibleResolved
{
	TxnVisibleResolvedTag tag;
	union
	{
		struct
		{
			/* f0 is the row's value, a Vec<u8>. */
			StdVec		f0;
			bool		has_f1;
			HostRow		f1;
		}			exists;
		struct
		{
			HostXid		f0;
			bool		has_f1;
			HostRow		f1;
			bool		has_f2;
			uint32		f2;
		}			wait;
	}			u;
} TxnVisibleResolved;

/* txn_TupleWriterLockMode_t is the tuple (Writer, LockMode): a lock-only version's writer and its mode. */
typedef struct txn_TupleWriterLockMode_t
{
	TxnMemtableVersionWriter f0;
	TxnMemtableLockMode f1;
} txn_TupleWriterLockMode_t;

/* TxnVisibleUpdatableTag is Updatable's variants. */
typedef enum TxnVisibleUpdatableTag
{
	TXN_VISIBLE_UPDATABLE_INVISIBLE,
	TXN_VISIBLE_UPDATABLE_SELF_MODIFIED,
	TXN_VISIBLE_UPDATABLE_BEING_MODIFIED,
	TXN_VISIBLE_UPDATABLE_UPDATED,
	TXN_VISIBLE_UPDATABLE_DELETED,
	TXN_VISIBLE_UPDATABLE_OK,
} TxnVisibleUpdatableTag;

/* TxnVisibleUpdatable is Updatable: heap's TM_Result for a change of a row version, by the versions it names. */
typedef struct TxnVisibleUpdatable
{
	TxnVisibleUpdatableTag tag;
	union
	{
		TxnMemtableVersionVersion *self_modified;
		struct
		{
			/* holders is a Vec<&Version>. */
			StdVec		holders;
			/* ended is the committed change under them, NULL for none. */
			TxnMemtableVersionVersion *ended;
		}			being_modified;
		TxnMemtableVersionVersion *updated;
		TxnMemtableVersionVersion *deleted;
	}			u;
} TxnVisibleUpdatable;

/* TxnVisibleChangedTag is Changed's variants. */
typedef enum TxnVisibleChangedTag
{
	/* A delta row version, by its place among its key's versions. */
	TXN_VISIBLE_CHANGED_DELTA,
	/* A base row, by its tuple id, whose writer committed. */
	TXN_VISIBLE_CHANGED_BASE,
} TxnVisibleChangedTag;

/* TxnVisibleChanged is Changed: the row a change is asked of. */
typedef struct TxnVisibleChanged
{
	TxnVisibleChangedTag tag;
	union
	{
		struct
		{
			size_t		at;
			TxnVisibleRowVersion row;
		}			delta;
		HostRow		base;
	}			u;
} TxnVisibleChanged;

extern TxnVisibleDirty txn_visible_Dirty_Default_default(void);
extern bool txn_visible_Dirty_PartialEq_eq(const TxnVisibleDirty *self, const TxnVisibleDirty *other);
extern bool txn_visible_Dirty_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_visible_in_snapshot(const HostSnapshot *snapshot, uint64 xid, uint64 top);
extern bool txn_visible_RowVersion_of(TxnMemtableVersionVersion *version, TxnVisibleRowVersion *result);
extern bool txn_visible_Seen_base(const TxnVisibleSeen *self, HostIoIo *io, TxnVisibleRule rule, HostRow tuple,
								  bool *result_0, TxnVisibleDirty *result_1,
								  TxnMemtableVersionVersion **result_2_0,
								  TxnMemtableVersionVersion **result_2_1, HostError *error);
extern bool txn_visible_Seen_base_conflict(const TxnVisibleSeen *self, HostIoIo *io, const TxnVisibleSerial *serial,
										   HostRow tuple, uint64 writer, bool visible, bool *has_result,
										   HostXid *result, HostError *error);
extern void txn_visible_Seen_drop(TxnVisibleSeen *self);
extern bool txn_visible_vacuumed(HostIoIo *io, HostStatus wrote, TxnMemtableVersionVersion *ender,
								 uint64 horizon, TxnVisibleVacuumed *result, HostError *error);
extern bool txn_visible_versions(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, const uint8 *key,
								 size_t key_len, HostLsn folded, StdVec *result, HostError *error);
extern bool txn_visible_next(HostIoIo *io, const HostDeltaHold *hold, HostSharedRecord state, bool has_after,
							 const uint8 *after, size_t after_len, TxnMemtableTableDirection way, HostLsn folded,
							 bool *has_result, txn_TupleVecU8VecVersion_t *result, HostError *error);
extern bool txn_visible_unfolded(HostIoIo *io, StdVec *versions, HostLsn folded, StdVec *result, HostError *error);
extern bool txn_visible_in_base(HostIoIo *io, TxnMemtableVersionVersion *version, HostLsn folded,
								bool *result, HostError *error);
extern bool txn_visible_live(HostIoIo *io, StdVec *chain, StdVec *result, HostError *error);
extern bool txn_visible_epoch(HostIoIo *io, uint32 base_epoch, const TxnVisibleLive *chain, size_t chain_len,
							  TxnMemtableVersionVersion *version, uint32 *result, HostError *error);
extern uint32 txn_visible_epoch_after(uint32 base_epoch, const TxnVisibleLive *chain, size_t chain_len, uint64 top,
									  uint32 command, bool has_end, uint64 end);
extern bool txn_visible_seen(HostIoIo *io, TxnVisibleRule rule, TxnMemtableVersionVersion *versions,
							 size_t versions_len, const TxnVisibleSerial *serial, TxnVisibleSeen *result,
							 HostError *error);
extern bool txn_visible_resolved(HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len,
								 StdBytes based, TxnVisibleResolved *result, HostError *error);
extern bool txn_visible_Resolved_PartialEq_eq(const TxnVisibleResolved *self, const TxnVisibleResolved *other);
extern bool txn_visible_Resolved_Debug_fmt(const void *value, StdFormatter *f);
extern void txn_visible_Resolved_drop(TxnVisibleResolved *self);
extern bool txn_visible_updatable(HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len,
								  TxnVisibleChanged changed, uint32 command, TxnVisibleUpdatable *result,
								  HostError *error);
extern void txn_visible_Updatable_drop(TxnVisibleUpdatable *self);
extern bool txn_visible_lockers(HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len,
								HostRow tuple, uint32 command, StdVec *result, HostError *error);
extern bool txn_visible_carries(HostIoIo *io, TxnMemtableVersionVersion *versions, size_t versions_len,
								HostRow tuple, uint32 command, bool *has, uint64 *result, HostError *error);

#ifdef DOLTGRES_TEST_TXN
extern void test_txn_visible_tests_a_running_writers_version_leaves_its_ran_answer_as_it_was(void);
#endif

#endif							/* DOLTGRES_TXN_VISIBLE_H */
