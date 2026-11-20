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
 * A memtable's versions (txn's memtable/version.rs): what a version does to
 * its key, read in place, which txn's contract gives every module.
 */
#ifndef DOLTGRES_TXN_MEMTABLE_VERSION_H
#define DOLTGRES_TXN_MEMTABLE_VERSION_H

#include "doltgres/chunks/trailer.h"
#include "doltgres/host/delta.h"
#include "doltgres/host/lib.h"
#include "doltgres/std/str.h"
#include "doltgres/std/vec.h"
#include "doltgres/txn/memtable.h"
#include "doltgres/txn/memtable/index.h"
#include "doltgres/txn/memtable/pages.h"

/*
 * TxnMemtableVersionWriter is Writer: who wrote a version, its writer's full
 * transaction id, its top-level transaction's where a subtransaction wrote
 * it, and its command id.
 */
typedef struct TxnMemtableVersionWriter
{
	uint64		xid;
	bool		has_top;
	uint64		top;
	uint32		command;
} TxnMemtableVersionWriter;

/* TxnMemtableVersionShownTag is Shown's variants. */
typedef enum TxnMemtableVersionShownTag
{
	TXN_MEMTABLE_VERSION_SHOWN_COMMAND,
	TXN_MEMTABLE_VERSION_SHOWN_COMBO,
	TXN_MEMTABLE_VERSION_SHOWN_REPLAYED,
	TXN_MEMTABLE_VERSION_SHOWN_ENDING,
} TxnMemtableVersionShownTag;

/* TxnMemtableVersionShown is Shown: the command a version shows as the raw command of the rows it changes. */
typedef struct TxnMemtableVersionShown
{
	TxnMemtableVersionShownTag tag;
	union
	{
		uint32		combo;
		uint32		ending;
	}			u;
} TxnMemtableVersionShown;

/* TXN_MEMTABLE_VERSION_NO_TRANSACTION is NO_TRANSACTION: the writer of a version no transaction wrote. */
#define TXN_MEMTABLE_VERSION_NO_TRANSACTION ((TxnMemtableVersionWriter) {.xid = 2})

/*
 * TxnMemtableVersionHandle is Handle: a version's page's index in its range's
 * list above its line pointer's first word.
 */
typedef struct TxnMemtableVersionHandle
{
	uint64		f0;
} TxnMemtableVersionHandle;

/* TxnMemtableVersionBodyTag is Body's variants. */
typedef enum TxnMemtableVersionBodyTag
{
	TXN_MEMTABLE_VERSION_BODY_LENT,
	TXN_MEMTABLE_VERSION_BODY_SPILLED,
} TxnMemtableVersionBodyTag;

/*
 * TxnMemtableVersionBody is Body: a version's record, lent from its delta page
 * for the hold or read back from a spill's file.
 */
typedef struct TxnMemtableVersionBody
{
	TxnMemtableVersionBodyTag tag;
	union
	{
		HostDeltaLent lent;
		/* spilled is a Vec<u8>. */
		StdVec		spilled;
	}			u;
} TxnMemtableVersionBody;

/*
 * TxnMemtableVersionVersion is Version: a version read under its record's
 * lock or read back from a spill's file, its handle, its record, its key's
 * older version, and its hint word, which a spilled version has none of.
 */
typedef struct TxnMemtableVersionVersion
{
	TxnMemtableVersionHandle handle;
	TxnMemtableVersionBody record;
	bool		has_older;
	TxnMemtableVersionHandle older;
	bool		has_hint;
	HostDeltaWord hint;
} TxnMemtableVersionVersion;

/*
 * TxnMemtableVersionChange is Change: a version to push, its writer, what it
 * does, the row lock it holds while its transaction runs, the command it
 * shows, and the end of its WAL record, when it has one.
 */
typedef struct TxnMemtableVersionChange
{
	TxnMemtableVersionWriter writer;
	const TxnMemtableOp *op;
	TxnMemtableLockMode lock;
	TxnMemtableVersionShown shown;
	bool		has_lsn;
	uint64		lsn;
} TxnMemtableVersionChange;

/*
 * TxnMemtableVersionPutRef is PutRef: a put read in place, its value, its
 * out-of-line addresses' and value ids' bytes, its tuple id, and its row's
 * heap bytes, 0 for a map's value.
 */
typedef struct TxnMemtableVersionPutRef
{
	StdBytes	value;
	StdBytes	addresses;
	StdBytes	values;
	HostRow		tuple;
	uint32		heap_bytes;
	/* carried is the full xid of the lock the row's update carried over. */
	bool		has_carried;
	uint64		carried;
} TxnMemtableVersionPutRef;

/* TxnMemtableVersionSuccessorRefTag is SuccessorRef's variants. */
typedef enum TxnMemtableVersionSuccessorRefTag
{
	/* Another key of the same tree. */
	TXN_MEMTABLE_VERSION_SUCCESSOR_REF_KEY,
	/* Another table. */
	TXN_MEMTABLE_VERSION_SUCCESSOR_REF_OTHER_TABLE,
} TxnMemtableVersionSuccessorRefTag;

/* TxnMemtableVersionSuccessorRef is SuccessorRef: where an update moved a row, read in place. */
typedef struct TxnMemtableVersionSuccessorRef
{
	TxnMemtableVersionSuccessorRefTag tag;
	union
	{
		StdBytes	key;
	}			u;
} TxnMemtableVersionSuccessorRef;

/* TxnMemtableVersionOpRefTag is OpRef's variants. */
typedef enum TxnMemtableVersionOpRefTag
{
	TXN_MEMTABLE_VERSION_OP_REF_PUT,
	TXN_MEMTABLE_VERSION_OP_REF_DELETE,
	TXN_MEMTABLE_VERSION_OP_REF_EMPTIED,
	TXN_MEMTABLE_VERSION_OP_REF_NAMED,
	TXN_MEMTABLE_VERSION_OP_REF_MOVE,
	TXN_MEMTABLE_VERSION_OP_REF_ADD,
	TXN_MEMTABLE_VERSION_OP_REF_REMOVE,
	TXN_MEMTABLE_VERSION_OP_REF_LOCK,
	TXN_MEMTABLE_VERSION_OP_REF_ADD_ROW,
	TXN_MEMTABLE_VERSION_OP_REF_UPDATE_ROW,
	TXN_MEMTABLE_VERSION_OP_REF_DELETE_ROW,
	TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW,
} TxnMemtableVersionOpRefTag;

/* TxnMemtableVersionOpRef is OpRef: what a version does to its key, read in place. */
typedef struct TxnMemtableVersionOpRef
{
	TxnMemtableVersionOpRefTag tag;
	union
	{
		TxnMemtableVersionPutRef put;
		HostRow		emptied;
		struct
		{
			HostRow		f0;
			TxnMemtableVersionPutRef f1;
		}			named;
		TxnMemtableVersionSuccessorRef move;
		TxnMemtableVersionPutRef add;
		struct
		{
			HostRow		f0;
			bool		has_f1;
			TxnMemtableVersionSuccessorRef f1;
		}			remove;
		TxnMemtableLockMode lock;
		TxnMemtableVersionPutRef add_row;
		struct
		{
			HostRow		f0;
			TxnMemtableVersionPutRef f1;
		}			update_row;
		struct
		{
			HostRow		f0;
			bool		has_f1;
			TxnMemtableVersionSuccessorRef f1;
		}			delete_row;
		struct
		{
			HostRow		f0;
			TxnMemtableLockMode f1;
		}			lock_row;
	}			u;
} TxnMemtableVersionOpRef;

/* TXN_MEMTABLE_VERSION_LINE is the count of a line pointer's words: its link and its hint. */
#define TXN_MEMTABLE_VERSION_LINE ((uint64) 2)

/*
 * TXN_MEMTABLE_VERSION_HEADER is the size of a record's fixed fields: its
 * writer's full transaction id, its command id, its tuple id, its value's
 * length, the count of its out-of-line addresses, its op's tag, its row
 * lock's mode, and its flags.
 */
#define TXN_MEMTABLE_VERSION_HEADER 29

/*
 * TxnMemtableVersionDeleter is Deleter: what a version's deleter shows, the
 * next newer version's writer and its command, which a lock's has none of.
 */
typedef struct TxnMemtableVersionDeleter
{
	uint64		xid;
	bool		has_command;
	uint32		command;
} TxnMemtableVersionDeleter;

/* TxnMemtableVersionAddresses is PutRef::addresses' iterator: the next address's bytes and how many are left. */
typedef struct TxnMemtableVersionAddresses
{
	const uint8 *at;
	size_t		left;
} TxnMemtableVersionAddresses;

/* TxnMemtableVersionValues is PutRef::values' iterator: the next value id's bytes and how many are left. */
typedef struct TxnMemtableVersionValues
{
	const uint8 *at;
	size_t		left;
} TxnMemtableVersionValues;

/* TxnMemtableVersionRecord is Record: a version's record, read in place, a &[u8]. */
typedef struct TxnMemtableVersionRecord
{
	StdBytes	f0;
} TxnMemtableVersionRecord;

/* TxnMemtableVersionHint is Hint: a version's cached outcome of its writer, which any reader stores. */
typedef enum TxnMemtableVersionHint
{
	TXN_MEMTABLE_VERSION_HINT_UNKNOWN,
	TXN_MEMTABLE_VERSION_HINT_COMMITTED,
	TXN_MEMTABLE_VERSION_HINT_ABORTED,
} TxnMemtableVersionHint;

/* TxnMemtableVersionChain is Chain: a key's versions, newest first, read under its record's lock. */
typedef struct TxnMemtableVersionChain
{
	const TxnMemtableIndexKeyedOps *keyed;
	const HostSharedGuard *guard;
	const HostDeltaHold *hold;
	bool		has_next;
	TxnMemtableVersionHandle next;
} TxnMemtableVersionChain;

/* TxnMemtableVersionPushedTag is Pushed's variants. */
typedef enum TxnMemtableVersionPushedTag
{
	/* The version pushed, its key's newest. */
	TXN_MEMTABLE_VERSION_PUSHED_VERSION,

	/*
	 * The push needs the fresh page it wasn't given, or the list more spares
	 * than it was given, and changed nothing.
	 */
	TXN_MEMTABLE_VERSION_PUSHED_WANTS,
} TxnMemtableVersionPushedTag;

/* TxnMemtableVersionPushed is Pushed: what a push did. */
typedef struct TxnMemtableVersionPushed
{
	TxnMemtableVersionPushedTag tag;
	union
	{
		TxnMemtableVersionHandle version;
		TxnMemtablePagesWant wants;
	}			u;
} TxnMemtableVersionPushed;

extern bool txn_memtable_version_Writer_PartialEq_eq(const TxnMemtableVersionWriter *self,
													 const TxnMemtableVersionWriter *other);
extern bool txn_memtable_version_Writer_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_version_Shown_PartialEq_eq(const TxnMemtableVersionShown *self,
													const TxnMemtableVersionShown *other);
extern bool txn_memtable_version_Shown_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_version_Handle_PartialEq_eq(const TxnMemtableVersionHandle *self,
													 const TxnMemtableVersionHandle *other);
extern bool txn_memtable_version_Handle_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_version_Hint_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_version_Deleter_PartialEq_eq(const TxnMemtableVersionDeleter *self,
													  const TxnMemtableVersionDeleter *other);
extern bool txn_memtable_version_Deleter_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_version_PutRef_PartialEq_eq(const TxnMemtableVersionPutRef *self,
													 const TxnMemtableVersionPutRef *other);
extern bool txn_memtable_version_PutRef_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_version_SuccessorRef_PartialEq_eq(const TxnMemtableVersionSuccessorRef *self,
														   const TxnMemtableVersionSuccessorRef *other);
extern bool txn_memtable_version_SuccessorRef_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_version_OpRef_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_version_Pushed_Debug_fmt(const void *value, StdFormatter *f);
extern bool txn_memtable_version_Handle_named(uint64 value, TxnMemtableVersionHandle *result);
extern TxnMemtableVersionHandle txn_memtable_version_Handle_spilled(uint64 xid, uint32 number, uint32 place);
#ifdef DOLTGRES_TEST_TXN
extern uint64 txn_memtable_version_Handle_page(TxnMemtableVersionHandle self);
#endif
extern size_t txn_memtable_version_encoded(const TxnMemtableVersionWriter *writer, const TxnMemtableOp *op,
										   TxnMemtableVersionShown shown, bool has_lsn, uint64 lsn);
extern size_t txn_memtable_version_largest(size_t value);
extern HostDeltaPageSize txn_memtable_version_reserve(uint64 newest, size_t key, size_t record);
extern void txn_memtable_version_encode(const TxnMemtableVersionWriter *writer, const TxnMemtableOp *op,
										TxnMemtableLockMode lock, TxnMemtableVersionShown shown, bool has_lsn,
										uint64 lsn, StdVec *out);
extern bool txn_memtable_version_stored(const uint8 *bytes, size_t bytes_len, TxnMemtableVersionRecord *result,
										HostError *error);
extern TxnMemtableVersionRecord txn_memtable_version_Record_of(const uint8 *bytes, size_t bytes_len);
extern StdBytes txn_memtable_version_Record_bytes(const TxnMemtableVersionRecord *self);
extern TxnMemtableVersionWriter txn_memtable_version_Record_writer(const TxnMemtableVersionRecord *self);
extern bool txn_memtable_version_Record_ender(const TxnMemtableVersionRecord *self, uint32 *result);
extern bool txn_memtable_version_Record_lsn(const TxnMemtableVersionRecord *self, uint64 *result);
extern TxnMemtableVersionShown txn_memtable_version_Record_shown(const TxnMemtableVersionRecord *self);
extern uint32 txn_memtable_version_Record_raw(const TxnMemtableVersionRecord *self);
extern uint32 txn_memtable_version_Record_ended(const TxnMemtableVersionRecord *self);
extern TxnMemtableLockMode txn_memtable_version_Record_lock(const TxnMemtableVersionRecord *self);
extern TxnMemtableVersionOpRef txn_memtable_version_Record_op(const TxnMemtableVersionRecord *self);
extern bool txn_memtable_version_speculate(const TxnMemtableIndexKeyedOps *keyed,
										   const HostSharedGuard *guard, const HostDeltaHold *hold,
										   TxnMemtableVersionHandle handle, uint32 token, HostError *error);
extern bool txn_memtable_version_show(const TxnMemtableIndexKeyedOps *keyed,
									  const HostSharedGuard *guard, const HostDeltaHold *hold,
									  TxnMemtableVersionHandle handle, uint32 combo, HostError *error);
extern bool txn_memtable_version_held(const TxnMemtableVersionVersion *versions, size_t versions_len,
									  TxnMemtableVersionHandle *result);
extern TxnMemtableVersionVersion txn_memtable_version_Version_spilled(TxnMemtableVersionHandle handle,
																	  StdVec *record);
extern TxnMemtableVersionRecord txn_memtable_version_Version_record(const TxnMemtableVersionVersion *self);
extern TxnMemtableVersionHint txn_memtable_version_Version_hint(const TxnMemtableVersionVersion *self);
extern uint32 txn_memtable_version_Version_raw(const TxnMemtableVersionVersion *self);
extern bool txn_memtable_version_Version_ender(const TxnMemtableVersionVersion *self, uint32 *result);
extern void txn_memtable_version_Version_end(TxnMemtableVersionVersion *self, uint32 ender);
extern void txn_memtable_version_Version_set_hint(TxnMemtableVersionVersion *self, TxnMemtableVersionHint hint);
extern bool txn_memtable_version_Version_killed(const TxnMemtableVersionVersion *self);
extern TxnMemtableOp txn_memtable_version_OpRef_owned(const TxnMemtableVersionOpRef *self);
extern void txn_memtable_version_Version_stored(const TxnMemtableVersionVersion *self, StdVec *result);
extern bool txn_memtable_version_carried(const TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold,
										 const uint64 *heads, size_t heads_len, uint64 entry, HostDeltaOwned *copy,
										 HostError *error);
extern bool txn_memtable_version_version(const TxnMemtableIndexKeyedOps *keyed,
										 const HostSharedGuard *guard, const HostDeltaHold *hold,
										 TxnMemtableVersionHandle handle, TxnMemtableVersionVersion *result,
										 HostError *error);
extern bool txn_memtable_version_head(const TxnMemtableIndexKeyedOps *keyed,
									  const HostSharedGuard *guard, const HostDeltaHold *hold,
									  TxnMemtableIndexNode node, bool *has_result, TxnMemtableVersionHandle *result,
									  HostError *error);
extern bool txn_memtable_version_chain(const TxnMemtableIndexKeyedOps *keyed,
									   const HostSharedGuard *guard, const HostDeltaHold *hold,
									   TxnMemtableIndexNode node, TxnMemtableVersionChain *result, HostError *error);
extern bool txn_memtable_version_Chain_Iterator_next(TxnMemtableVersionChain *self, bool *has_result,
													 TxnMemtableVersionVersion *result, HostError *error);
extern bool txn_memtable_version_push(TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold, const uint8 *key,
									  size_t key_len, const TxnMemtableVersionChange *change, bool *has_fresh,
									  HostDeltaOwned *fresh, StdVec *spare, StdVec *buffer,
									  TxnMemtableVersionPushed *result, HostError *error);
extern bool txn_memtable_version_removed(const TxnMemtableIndexKeyedOpen *open, const HostDeltaHold *hold,
										 TxnMemtableIndexNode node, const TxnMemtableVersionHandle *handles,
										 size_t handles_len, bool *taken, size_t taken_len, bool *result_0,
										 StdVec *result_1, uint64 *result_2, HostError *error);

extern TxnMemtableVersionAddresses txn_memtable_version_PutRef_addresses(const TxnMemtableVersionPutRef *self);
extern bool txn_memtable_version_Addresses_Iterator_next(TxnMemtableVersionAddresses *self,
														 chunks_TupleU32Locator_t *result);
extern TxnMemtableVersionValues txn_memtable_version_PutRef_values(const TxnMemtableVersionPutRef *self);
extern bool txn_memtable_version_Values_Iterator_next(TxnMemtableVersionValues *self, uint32 *result);
extern TxnMemtableVersionDeleter txn_memtable_version_Version_deleter(const TxnMemtableVersionVersion *self);
extern bool txn_memtable_version_Version_token(const TxnMemtableVersionVersion *self, uint32 *result);
extern void txn_memtable_version_Version_confirm(TxnMemtableVersionVersion *self);
extern void txn_memtable_version_Version_kill(TxnMemtableVersionVersion *self);
extern void txn_memtable_version_Version_drop(TxnMemtableVersionVersion *self);
extern void txn_memtable_version_versions_drop(StdVec *versions, size_t from);

#endif							/* DOLTGRES_TXN_MEMTABLE_VERSION_H */
