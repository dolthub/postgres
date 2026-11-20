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
 * The second part of the doltgres crate's entries (entries.rs from its
 * tuple_tid_valid on), which entries_tail.c holds: the statics and helpers
 * of it that entries.c reaches.
 */
#ifndef DOLTGRES_DOLTGRES_ENTRIES_TAIL_H
#define DOLTGRES_DOLTGRES_ENTRIES_TAIL_H

#include "doltgres/doltgres/entries.h"
#include "doltgres/doltgres/index.h"
#include "doltgres/host/local.h"
#include "doltgres/host/lock.h"
#include "doltgres/host/owned.h"
#include "doltgres/std/vec.h"
#include "doltgres/trees/store.h"

/* doltgres_TupleU32VecU8_t is a (u32, Vec<u8>): an entry a speculative insert's index wrote, by its index and its key. */
typedef struct doltgres_TupleU32VecU8_t
{
	uint32		f0;
	StdVec		f1;
} doltgres_TupleU32VecU8_t;

/*
 * DoltgresEntriesSpeculated is Speculated: a speculative insert the backend
 * made whose completion is still to come, its token, its relation, the
 * tuple id it took, its key, whether its insert found the key's live row and
 * so spent its place with no row written, and the entries its indexes'
 * aminsert wrote (a Vec<(u32, Vec<u8>)>).
 */
typedef struct DoltgresEntriesSpeculated
{
	uint32		token;
	uint32		relation;
	uint64		tuple;
	StdVec		key;
	bool		killed;
	StdVec		entries;
} DoltgresEntriesSpeculated;

extern void doltgres_entries_VecSpeculated_drop(void *speculated);

/* SPECULATED's IoMutex<Vec<Speculated>>, host's lock at the backend's speculative inserts. */
#define HOST_LOCK_SCOPE extern
#define HOST_LOCK_DECLARE
#define HOST_LOCK_TAKEN_TS VecSpeculated
#define HOST_LOCK_TAKEN_T StdVec
#define HOST_LOCK_TAKEN_DEFAULT(place) \
	std_vec_new((place), sizeof(DoltgresEntriesSpeculated), _Alignof(DoltgresEntriesSpeculated))
#define HOST_LOCK_TAKEN_DROP(place) doltgres_entries_VecSpeculated_drop(place)
#define HOST_LOCK_TAKEN_TYPE DOLTGRES_ENTRIES_SPECULATED_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS VecSpeculated
#define HOST_LOCK_T StdVec
#define HOST_LOCK_DEFAULT(place) \
	std_vec_new((place), sizeof(DoltgresEntriesSpeculated), _Alignof(DoltgresEntriesSpeculated))
#include "doltgres/host/lock_template.h"

/*
 * DOLTGRES_ENTRIES_SPECULATED is SPECULATED: the backend's speculative
 * inserts whose completion is still to come, which a transaction's end
 * clears; its BackendLocal header and the process's value, with the type its
 * get takes.
 */
extern struct DoltgresEntriesSpeculatedLocal
{
	HostLocalBackendLocal local;
	HostLockIoMutexVecSpeculated process;
}			DOLTGRES_ENTRIES_SPECULATED;
extern const StdDefaultType DOLTGRES_ENTRIES_SPECULATED_TYPE;

extern void doltgres_entries_record_identity(const DoltgresIndexDescribed *described, const uint8 *row_key,
											 size_t row_key_len, uint64 tuple, StdVec *result);
extern void doltgres_entries_record_change(const DoltgresIndexDescribed *described, const uint8 *identity,
										   size_t identity_len, uint64 tuple, StdVec *entry,
										   txn_TupleVecU8Op_t *result);

/*
 * doltgres_entries_opened_t is what most entries open first: the backend's
 * handle on its database's repository and a reader of the shared state, each
 * while live.
 */
typedef struct doltgres_entries_opened_t
{
	StdArcInner *repo;
	bool		repo_live;
	HostSharedReader reading;
	bool		reading_live;
} doltgres_entries_opened_t;

extern bool doltgres_entries_open(HostIoIo *io, doltgres_entries_opened_t *opened, PgPgError *error);
extern void doltgres_entries_opened_drop(doltgres_entries_opened_t *opened);
extern const RepoRepo *doltgres_entries_opened_repo(const doltgres_entries_opened_t *opened);
extern void doltgres_entries_Record_drop(txn_TupleVecU8Op_t *record);

/* DoltgresEntriesLockedOp is one operation on a backend's value under its IoMutex, with its captures. */
typedef void (*DoltgresEntriesLockedOp) (void *value, void *captures);
extern void doltgres_entries_locked(HostIoIo *io, HostLocalBackendLocal *of, const StdDefaultType *type,
									const HostLockType *lock_type, DoltgresEntriesLockedOp op, void *captures);
extern bool doltgres_entries_entry_pushed(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
										  const PgRel *index, StdVec *key, TxnMemtableOp *op,
										  txn_TupleVecU8Op_t *record, TxnMemtableVersionWriter writer,
										  TxnStatusRoomed roomed, PgPgError *error);

#endif							/* DOLTGRES_DOLTGRES_ENTRIES_TAIL_H */
