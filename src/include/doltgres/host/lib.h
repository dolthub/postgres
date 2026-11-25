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
 * The seams the storage crates reach Postgres through (design item 14), as
 * one trait, and the doors that call the host installed for the process: the
 * server's (pghost) or, in tests, the simulation's (host's lib.rs). Its plain
 * types come first, then the module headers' types, which name them, then
 * the trait tables, which name the modules' types.
 */
#ifndef DOLTGRES_HOST_LIB_H
#define DOLTGRES_HOST_LIB_H

#include "doltgres/host/io.h"
#include "doltgres/std/any.h"
#include "doltgres/std/path.h"
#include "doltgres/std/str.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/time.h"
#include "doltgres/std/vec.h"

/*
 * HostMayRaise is what a seam that can raise takes: the capability of pg's an
 * Io borrows, which only the doors of this crate hand a host, and which the
 * host opens to call Postgres.
 */
typedef struct HostMayRaise
{
	StdAny		f0;
} HostMayRaise;

/* HostCauseTag is Cause's variants. */
typedef enum HostCauseTag
{
	/*
	 * HOST_CAUSE_POSTGRES is an error Postgres raised, which only pghost
	 * opens.
	 */
	HOST_CAUSE_POSTGRES,
	/* HOST_CAUSE_FORK is the fork's own error's message. */
	HOST_CAUSE_FORK,
} HostCauseTag;

/* HostCause is what an error raises. */
typedef struct HostCause
{
	HostCauseTag tag;
	union
	{
		StdAny		postgres;
		struct
		{
			StdCowStr	f0;
			StdStr		f1;
		}			fork;
	}			u;
} HostCause;

/*
 * HostError is an error a seam returns, which travels up to the entry to be
 * raised: one Postgres raised, held opaquely with its SQLSTATE as a plain
 * value, or the fork's own, with its SQLSTATE and message.
 */
typedef struct HostError
{
	/* sqlstate is the error's SQLSTATE. */
	uint8		sqlstate[5];

	/*
	 * cause is what pghost raises: Postgres' error, or the fork's message and
	 * hint.
	 */
	HostCause	cause;
} HostError;

/*
 * HostForked is a fork error as pghost raises it: its SQLSTATE, its message,
 * and its hint when it has one.
 */
typedef struct HostForked
{
	uint8		f0[5];
	StdString	f1;
	StdStr		f2;
} HostForked;

struct HostReadHostOps;
struct HostHostOps;

/* HostReadHost is a value of the ReadHost trait: its table and the host. */
typedef struct HostReadHost
{
	const struct HostReadHostOps *ops;
	void	   *self;
} HostReadHost;

/* HostHost is a value of the Host trait: its table and the host. */
typedef struct HostHost
{
	const struct HostHostOps *ops;
	void	   *self;
} HostHost;

/*
 * HostFile is an open file, which closes when it drops. Only open makes one,
 * so a handle can't be forged, used after its file closes, or closed twice,
 * and an error between opening and closing a file can't leak it (design item
 * 13).
 */
typedef struct HostFile
{
	/* number is the host's number for the file. */
	int32		number;
	/* host is the host that opened the file, which closes it. */
	HostReadHost host;
} HostFile;

/*
 * HostScratchFile is a temporary file of the transaction that opened it, as
 * Postgres' own work files are: placed by temp_tablespaces, counted by
 * temp_file_limit, and deleted when it closes. It lives within the entry that
 * opened it, which the resource owner that would close it outlasts, so its
 * drop closes it first.
 */
typedef struct HostScratchFile
{
	HostFile	file;
} HostScratchFile;

/* HostOpen is how a file is opened to write. */
typedef enum HostOpen
{
	/* HOST_OPEN_READ_WRITE opens an existing file to read and write. */
	HOST_OPEN_READ_WRITE,

	/*
	 * HOST_OPEN_CREATE_NEW creates a file that must not exist, to read and
	 * write.
	 */
	HOST_OPEN_CREATE_NEW,
} HostOpen;

/* HostLsn is a position in Postgres' WAL. */
typedef uint64 HostLsn;

/*
 * HostForkRecord is one of the fork's WAL records as a read of the WAL finds
 * it: its kind, its end, and its data.
 */
typedef struct HostForkRecord
{
	uint8		f0;
	HostLsn		f1;
	StdVec		f2;
} HostForkRecord;

/*
 * HostWalStand is where the server's WAL stands: whether recovery is in
 * progress; the insertion timeline, zero until recovery sets it at its end;
 * the flushed position, zero while recovery is in progress; replay's position
 * and timeline, zero before replay begins; and the timeline recovery reads
 * toward.
 */
typedef struct HostWalStand
{
	bool		recovering;
	uint32		insertion_timeline;
	HostLsn		flushed;
	HostLsn		replayed;
	uint32		replayed_timeline;
	uint32		target_timeline;
} HostWalStand;

/* HostStorage names a storage by its relfilenode's locator: its tablespace, its database, and its relation number. */
typedef struct HostStorage
{
	uint32		tablespace;
	uint32		database;
	uint32		relation;
} HostStorage;

/* host_TupleStorageVecTupleU32U64_t is the tuple (Storage, Vec<(u32, u64)>): a storage and its numbered counts. */
typedef struct host_TupleStorageVecTupleU32U64_t
{
	HostStorage f0;
	/* f1 is a Vec<(u32, u64)>. */
	StdVec		f1;
} host_TupleStorageVecTupleU32U64_t;

/* HostTable is a table's object id. */
typedef struct HostTable
{
	uint64		f0;
} HostTable;

/* HostXid is the 64-bit id of a transaction or a subtransaction, Postgres' FullTransactionId. */
typedef struct HostXid
{
	uint64		f0;
} HostXid;

/* HostStatus is what Postgres says of a transaction or a subtransaction: still in progress, committed, or aborted. */
typedef enum HostStatus
{
	HOST_STATUS_IN_PROGRESS,
	HOST_STATUS_COMMITTED,
	HOST_STATUS_ABORTED,
} HostStatus;

/* HostCommand is a command's id within its transaction, Postgres' CommandId. */
typedef struct HostCommand
{
	uint32		f0;
} HostCommand;

/* HostSnapshotKind is a snapshot's type, each of Postgres' SnapshotTypes (seam 2). */
typedef enum HostSnapshotKind
{
	HOST_SNAPSHOT_KIND_MVCC,
	HOST_SNAPSHOT_KIND_CURRENT,
	HOST_SNAPSHOT_KIND_ANY,
	HOST_SNAPSHOT_KIND_TOAST,
	HOST_SNAPSHOT_KIND_DIRTY,
	HOST_SNAPSHOT_KIND_HISTORIC_MVCC,
	HOST_SNAPSHOT_KIND_NON_VACUUMABLE,
} HostSnapshotKind;

/*
 * HostSnapshot is a snapshot lent for a call (seam 2): its kind, its bounds
 * as full xids, its command, whether it was taken in recovery, whether its
 * subtransactions overflowed, and its running xids and subtransaction xids
 * lent from Postgres' own arrays, so txn keeps none. Only a host makes one,
 * from the snapshot its entry got, which it keeps for the doors of a kind's
 * horizon and command map.
 */
typedef struct HostSnapshot
{
	HostSnapshotKind kind;
	HostXid		xmin;
	HostXid		xmax;
	HostCommand command;
	bool		recovery;
	bool		overflowed;
	const uint32 *xip;
	size_t		xip_len;
	const uint32 *subxip;
	size_t		subxip_len;
	/* of is the host's own snapshot the value was read from. */
	StdAny		of;
} HostSnapshot;

/* HostRow is a row version's tuple id, 48 bits, which Postgres shows as its ctid. */
typedef struct HostRow
{
	uint64		f0;
} HostRow;

/* host_TupleRowU32_t is the tuple (Row, u32): a tuple id and a number of it. */
typedef struct host_TupleRowU32_t
{
	HostRow		f0;
	uint32		f1;
} host_TupleRowU32_t;

/*
 * HostWalKind is a kind of the fork's WAL records, the one registry of them
 * (design items 8 and 11): each kind is appended only by the crate its doc
 * names; a value up to 15 is the fork's first resource manager's kind, in the
 * four bits Postgres gives a resource manager in a record's info, and a value
 * from 16 is its second's, 15 less.
 */
typedef enum HostWalKind
{
	HOST_WAL_KIND_REPOSITORY_CREATED,
	HOST_WAL_KIND_REPOSITORY_DROPPED,
	HOST_WAL_KIND_REPOSITORY_MOVED,
	HOST_WAL_KIND_ROW_CHANGE,
	HOST_WAL_KIND_PAGE_LEFT,
	HOST_WAL_KIND_CHUNK,
#ifdef USE_INJECTION_POINTS
	HOST_WAL_KIND_TEST,
#endif
	HOST_WAL_KIND_BASE_ADVANCED,
	HOST_WAL_KIND_STORAGE_EMPTIED,
	HOST_WAL_KIND_CONFIRM,
	HOST_WAL_KIND_TIDS_FREED,
	HOST_WAL_KIND_MAP_CARRIED,
	HOST_WAL_KIND_DATABASE_CHANGE,
	HOST_WAL_KIND_CARRY_UNLISTED,
	HOST_WAL_KIND_FOLD_STATUSES,
} HostWalKind;

/*
 * HostDelayCheckpoint is a window holding off the start of a checkpoint, as
 * Postgres' DELAY_CHKPT_START does, in a critical section, so a checkpoint's
 * redo point never falls between a record and the change it describes (design
 * item 8's host rule 1). It ends through finish once the change is visible;
 * dropped unfinished, as an error leaving the window drops it, it stops the
 * server, as an error in Postgres' own windows does, and recovery replays the
 * record. Windows don't nest, and no Postgres call that takes
 * DELAY_CHKPT_START runs inside one.
 */
typedef struct HostDelayCheckpoint
{
	/*
	 * host is the host that holds off checkpoints, which lets them start
	 * again.
	 */
	HostHost	host;
	/* finished is whether the window's change is visible. */
	bool		finished;
} HostDelayCheckpoint;

/* HostCompressDictionary is a zstd dictionary a host prepared for compressing, in the host's own form. */
typedef struct HostCompressDictionary
{
	StdAny		f0;
} HostCompressDictionary;

/* host_TupleUsizeCompressDictionary_t is the tuple (usize, CompressDictionary). */
typedef struct host_TupleUsizeCompressDictionary_t
{
	size_t		f0;
	HostCompressDictionary f1;
} host_TupleUsizeCompressDictionary_t;

/* HostDecompressDictionary is a zstd dictionary a host prepared for decompressing, in the host's own form. */
typedef struct HostDecompressDictionary
{
	StdAny		f0;
} HostDecompressDictionary;

/* host_ReformRow_stamp_t is ReformRow's stamp: its writer and tuple id. */
typedef struct host_ReformRow_stamp_t
{
	uint64		f0;
	uint64		f1;
} host_ReformRow_stamp_t;

/*
 * HostReformRow is a stored row a fold re-forms through reformed: its table's
 * OID, the table's key columns as a slot keeps them beside a row, its key, its
 * row stamp's writer and tuple id, its tuple, its values' ids out of line, and
 * the encoded transform from its layout to its chain's end.
 */
typedef struct HostReformRow
{
	uint32		relation;
	const uint8 *keyed;
	size_t		keyed_len;
	const uint8 *key;
	size_t		key_len;
	host_ReformRow_stamp_t stamp;
	const uint8 *tuple;
	size_t		tuple_len;
	const uint32 *values;
	size_t		values_len;
	const uint8 *transform;
	size_t		transform_len;
} HostReformRow;

/*
 * HostReformedRow is a row re-formed to its chain's end: its tuple, the
 * offsets of the addresses it holds, its values' ids out of line, and its
 * heap bytes.
 */
typedef struct HostReformedRow
{
	StdVec		tuple;
	StdVec		addresses;
	StdVec		values;
	uint32		heap_bytes;
} HostReformedRow;

/*
 * HostCompared is how btree_compare calls a comparison function: its OID, the
 * collation it compares under, and its type's stored length and whether the
 * type is passed by value, whose values are its word's bytes, low byte first.
 */
typedef struct HostCompared
{
	uint32		function;
	uint32		collation;
	int16		len;
	bool		by_value;
} HostCompared;

/* HostRefused is the error of installing a host in a process that has one. */
typedef struct HostRefused
{
	char		unused;
} HostRefused;

/*
 * HostNewCid is what heap's record of a row's command ids holds, as
 * log_heap_new_cid fills it: the command that wrote the row, the command that
 * changed it, and the combo command id that stands for both, each none when
 * heap's is InvalidCommandId.
 */
typedef struct HostNewCid
{
	bool		has_cmin;
	HostCommand cmin;
	bool		has_cmax;
	HostCommand cmax;
	bool		has_combo;
	HostCommand combo;
} HostNewCid;

/*
 * HostKeyGuard is a key lock's stripes held, one or two, released when it
 * drops. It borrows the entry's whole Io, as host's other guards do, and
 * lends a KeyIo, which reaches no door that waits or runs user code, and it
 * borrows the striping token a writer made from its Waits, so none is held
 * outside the prepare's closure that uses the token.
 */
typedef struct HostKeyGuard
{
	HostHost	host;
	uint32		stripes[2];
	size_t		count;
	HostIoHeld *held;
	StdAny		raise;

	/*
	 * pushed marks a push made under the stripes, after which their release
	 * moves their push counts.
	 */
	bool		pushed;
} HostKeyGuard;

/*
 * HostKeyHold is a stripe's hold-back flag a resolve set after its third
 * start-over ((4b)'s §3): every other writer of the stripe sleeps before its
 * read while it is set, so the resolve finishes within one more round per
 * writer already past its check. Its drop clears it and wakes them, which its
 * setter's finish, error, or wait on another transaction does, and an exit
 * clears what a process set.
 */
typedef struct HostKeyHold
{
	HostHost	host;
	uint32		stripe;
} HostKeyHold;

/*
 * HostOperation is what a wait for a transaction is for, which its error
 * context names: the relation's object id, the row's tuple id, and Postgres'
 * XLTW_Oper (XLTW_Update, XLTW_Delete, XLTW_Lock, and the others).
 */
typedef struct HostOperation
{
	uint32		relation;
	uint64		tid;
	int32		oper;
} HostOperation;

/*
 * HostTupleLock is a row's tuple lock: its database, its relation's object
 * id, its tuple id, and the row lock mode, as Postgres' LockTupleMode numbers
 * it, whose heavyweight mode heap's tupleLockExtraInfo gives.
 */
typedef struct HostTupleLock
{
	uint32		database;
	uint32		relation;
	uint64		tid;
	uint8		mode;
} HostTupleLock;

/*
 * HostTupleGuard is a row's tuple lock held, released when it drops, as heap
 * releases its tuple lock once its change is made.
 */
typedef struct HostTupleGuard
{
	HostHost	host;
	HostTupleLock tuple;
} HostTupleGuard;

/* HostExitOps is the Exit trait's table: work a process does as it exits. */
typedef struct HostExitOps
{
	/* run does the work, with the process's words and handles still its own. */
	bool		(*run) (HostIoIo *io, HostError *error);
} HostExitOps;

/* The modules' types, which the trait tables below name. */
#include "doltgres/host/shared.h"
#include "doltgres/host/cache.h"
#include "doltgres/host/delta.h"

/* HostReadHostOps is ReadHost's table: the seams a process's host gives the storage crates' reads. */
typedef struct HostReadHostOps
{
	bool		(*open_to_read) (void *self, const HostMayRaise *raise, const char *path,
								 size_t path_len, int32 *result, HostError *error);
	bool		(*open_if_there) (void *self, const HostMayRaise *raise, const char *path,
								  size_t path_len, bool *has_result, int32 *result,
								  HostError *error);
	bool		(*read_at) (void *self, const HostMayRaise *raise, int32 file, uint64 offset,
							uint8 *buffer, size_t buffer_len, HostError *error);
	bool		(*size) (void *self, const HostMayRaise *raise, int32 file, uint64 *result,
						 HostError *error);
	void		(*prefetch) (void *self, int32 file, uint64 offset, uint64 len);
	void		(*close) (void *self, int32 file);
	StdPathBuf	(*tablespace_root) (void *self, uint32 tablespace);
	bool		(*list_dir) (void *self, const HostMayRaise *raise, const char *path,
							 size_t path_len, StdVec *result, HostError *error);
	bool		(*list_dir_if_there) (void *self, const HostMayRaise *raise, const char *path,
									  size_t path_len, bool *has_result, StdVec *result,
									  HostError *error);
	bool		(*prepare_decompressing) (void *self, const uint8 *bytes, size_t bytes_len,
										  HostDecompressDictionary *result, HostError *error);
	bool		(*frame_size) (void *self, const uint8 *frame, size_t frame_len, uint64 *result);
	bool		(*decompress) (void *self, const uint8 *frame, size_t frame_len, size_t size,
							   const HostDecompressDictionary *dictionary, bool *has_result,
							   StdVec *result, HostError *error);
} HostReadHostOps;

/* HostHostOps is Host's table: the seams beyond the reads, which only the server's host and the simulation's give. */
typedef struct HostHostOps
{
	HostReadHostOps read_host;
	StdSystemTime (*now) (void *self);
	StdInstant	(*instant) (void *self);
	uint32		(*process_id) (void *self);
	uint64		(*work_mem) (void *self);
	bool		(*session_database) (void *self, uint32 *out_0, uint32 *out_1);
	bool		(*exiting) (void *self);
	uint64		(*unique) (void *self);
	bool		(*check_interrupts) (void *self, const HostMayRaise *raise, HostError *error);
	void		(*hold_interrupts) (void *self);
	void		(*resume_interrupts) (void *self);
	bool		(*open_) (void *self, const HostMayRaise *raise, const char *path, size_t path_len,
						  HostOpen how, int32 *result, HostError *error);
	bool		(*create_if_absent) (void *self, const HostMayRaise *raise, const char *path,
									 size_t path_len, bool *has_result, int32 *result,
									 HostError *error);
	bool		(*open_scratch) (void *self, const HostMayRaise *raise, int32 *result,
								 HostError *error);
	bool		(*write_at) (void *self, const HostMayRaise *raise, int32 file, uint64 offset,
							 const uint8 *bytes, size_t bytes_len, HostError *error);
	bool		(*truncate) (void *self, const HostMayRaise *raise, int32 file, uint64 len,
							 HostError *error);
	bool		(*sync) (void *self, const HostMayRaise *raise, int32 file, HostError *error);
	bool		(*rename_) (void *self, const HostMayRaise *raise, const char *from,
							size_t from_len, const char *to, size_t to_len, HostError *error);
	bool		(*rename_durably) (void *self, const HostMayRaise *raise, const char *from,
								   size_t from_len, const char *to, size_t to_len,
								   HostError *error);
	bool		(*remove_durably) (void *self, const HostMayRaise *raise, const char *path,
								   size_t path_len, HostError *error);
	bool		(*remove_durably_if_there) (void *self, const HostMayRaise *raise,
											const char *path, size_t path_len, bool *result,
											HostError *error);
	void		(*remove_leftover) (void *self, const char *path, size_t path_len);
	bool		(*rename_back) (void *self, const char *from, size_t from_len, const char *to,
								size_t to_len);
	bool		(*remove_tree) (void *self, const HostMayRaise *raise, const char *path,
								size_t path_len, bool *result, HostError *error);
	bool		(*sync_path) (void *self, const HostMayRaise *raise, const char *path,
							  size_t path_len, bool directory, HostError *error);
	bool		(*make_dir) (void *self, const HostMayRaise *raise, const char *path,
							 size_t path_len, HostError *error);
	bool		(*disk_space) (void *self, const HostMayRaise *raise, const char *path,
							   size_t path_len, uint64 *result_0, uint64 *result_1, HostError *error);
	bool		(*tree_bytes) (void *self, const HostMayRaise *raise, const char *path,
							   size_t path_len, uint64 *result, HostError *error);
	bool		(*nap) (void *self, const HostMayRaise *raise, StdDuration duration, HostError *error);
	bool		(*wal_insert) (void *self, const HostMayRaise *raise, HostWalKind kind,
							   const uint8 *body, size_t body_len, HostLsn *result,
							   HostError *error);
	bool		(*wal_new_cid) (void *self, const HostMayRaise *raise, const HostStorage *storage,
								HostRow tid, HostNewCid cid, HostLsn *result, HostError *error);
	bool		(*wal_flush) (void *self, const HostMayRaise *raise, HostLsn lsn, HostError *error);
	bool		(*fold_point) (void *self, const HostMayRaise *raise, HostLsn *result,
							   HostError *error);
	bool		(*checkpoint_now) (void *self, const HostMayRaise *raise, HostError *error);
	bool		(*quiet_database) (void *self, const HostMayRaise *raise, uint32 database,
								   HostError *error);
	bool		(*unquiet_database) (void *self, const HostMayRaise *raise, uint32 database,
									 HostError *error);
	bool		(*last_redo) (void *self, const HostMayRaise *raise, HostLsn *result,
							  HostError *error);
	HostWalStand (*wal_stand) (void *self);
	bool		(*warn_kept_wal) (void *self, const HostMayRaise *raise, HostLsn from, HostLsn to,
								  uint32 timeline, HostLsn upto, HostError *error);
	bool		(*next_fork_record) (void *self, const HostMayRaise *raise, HostLsn from, uint32 kinds,
									 bool received, bool *has_result, HostForkRecord *result, HostError *error);
	bool		(*warn) (void *self, const HostMayRaise *raise, HostError *error_,
						 const char *context, HostError *error);
	bool		(*log) (void *self, const HostMayRaise *raise, const char *message,
						HostError *error);
	void		(*refuse_recovery) (void *self, const char *detail, size_t detail_len);
	bool		(*delay_checkpoints) (void *self, const HostMayRaise *raise,
									  const char *const *pauses, size_t pauses_len,
									  HostError *error);
	bool		(*load_pauses) (void *self, const HostMayRaise *raise, const char *const *pauses,
								size_t pauses_len, HostError *error);
	void		(*resume_checkpoints) (void *self, bool finished);
	void		(*fail_checkpoints) (void *self, const HostError *error);
	bool		(*xid_status) (void *self, const HostMayRaise *raise, HostXid xid,
							   HostStatus *result, HostError *error);
	bool		(*xid_in_progress) (void *self, const HostMayRaise *raise, HostXid xid,
									bool *result, HostError *error);
	bool		(*xids_answered) (void *self);
	bool		(*removable_in) (void *self, const HostMayRaise *raise, StdAny snapshot,
								 HostXid xid, bool *result, HostError *error);
	bool		(*current_writer) (void *self, const HostMayRaise *raise, HostXid *result_0,
								   HostXid *result_1, HostError *error);
	bool		(*current_command) (void *self, const HostMayRaise *raise, bool used,
									HostCommand *result, HostError *error);
	bool		(*xid_is_current) (void *self, HostXid xid);
	bool		(*combo_command) (void *self, const HostMayRaise *raise, HostXid writer,
								  HostCommand cmin, HostCommand cmax, HostCommand *result,
								  HostError *error);
	bool		(*key_lock) (void *self, const HostMayRaise *raise, const uint32 *stripes,
							 size_t stripes_len, HostError *error);
	void		(*key_unlock) (void *self, const uint32 *stripes, size_t stripes_len,
							   bool pushed);
	uint64		(*key_count) (void *self, uint32 stripe);
	bool		(*key_hold) (void *self, uint32 stripe);
	void		(*key_release) (void *self, uint32 stripe);
	bool		(*key_held_wait) (void *self, const HostMayRaise *raise, uint32 stripe,
								  HostError *error);
	bool		(*xact_wait) (void *self, const HostMayRaise *raise, HostXid xid, bool has_on,
							  HostOperation on, bool wait, bool *result, HostError *error);
	bool		(*speculative_wait) (void *self, const HostMayRaise *raise, HostXid xid,
									 uint32 token, HostError *error);
	bool		(*lock_tuple) (void *self, const HostMayRaise *raise, HostTupleLock tuple,
							   bool wait, bool *result, HostError *error);
	void		(*unlock_tuple) (void *self, HostTupleLock tuple);
	bool		(*actor) (void *self, uint64 *result);
	void		(*yield_point) (void *self);
	bool		(*pause) (void *self, const HostMayRaise *raise, const char *point,
						  HostError *error);
	bool		(*injected) (void *self, const HostMayRaise *raise, const char *point, bool *result,
							 HostError *error);
	bool		(*at_exit) (void *self, const StdTypeId *work,
							bool (*run) (HostIoIo *io, HostError *error), HostError *error);
	bool		(*prepare_compressing) (void *self, const uint8 *bytes, size_t bytes_len,
										int32 level, HostCompressDictionary *result,
										HostError *error);
	bool		(*compress) (void *self, const uint8 *bytes, size_t bytes_len, int32 level,
							 const HostCompressDictionary *dictionary, StdVec *result,
							 HostError *error);
	bool		(*train) (void *self, const StdBytes *samples, size_t samples_len,
						  size_t capacity, StdVec *result);
	bool		(*shared_make) (void *self, const HostMayRaise *raise, const HostSharedKind *kind,
								bool *has_result, uint64 *result, HostError *error);
	uint64		(*shared_publish) (void *self, uint64 place);
	bool		(*shared_claim) (void *self, const HostSharedKind *kind, uint64 location,
								 bool *has_result, uint64 *result, HostError *error);
	void		(*shared_free) (void *self, uint64 place);
	void		(*shared_give_up) (void *self, uint64 place);
	bool		(*shared_open) (void *self, const HostMayRaise *raise, const HostSharedKind *kind,
								uint64 location, bool *has_result, uint64 *result,
								HostError *error);
	void		(*shared_close) (void *self, uint64 place);
	bool		(*shared_reclaim) (void *self, const HostSharedKind *kind, uint64 location,
								   HostSharedLooked *result, HostError *error);
	bool		(*shared_reclaimed) (void *self, uint64 place);
	void		(*shared_unreclaim) (void *self, uint64 place);
	bool		(*shared_sweep) (void *self, const HostMayRaise *raise, const HostSharedKind *kind,
								 size_t atomic, StdVec *result, HostError *error);
	bool		(*shared_index_find) (void *self, const HostMayRaise *raise, const HostSharedKind *kind,
									  const uint8 *key, bool *has, uint64 *location, HostError *error);
	bool		(*shared_index_put) (void *self, const HostMayRaise *raise, const HostSharedKind *kind,
									 const uint8 *key, uint64 location, HostError *error);
	bool		(*shared_index_remove) (void *self, const HostMayRaise *raise, const HostSharedKind *kind,
										const uint8 *key, uint64 location, HostError *error);
	bool		(*shared_single) (void *self, const HostSharedKind *kind, uint64 *result,
								  HostError *error);
	bool		(*shared_ready) (void *self);
	bool		(*shared_lock) (void *self, const HostMayRaise *raise, uint64 place, size_t lock,
								bool exclusive, HostError *error);
	void		(*shared_unlock) (void *self, uint64 place, size_t lock);
	bool		(*flag_take) (void *self, const HostMayRaise *raise, uint64 place, size_t flag,
							  HostError *error);
	bool		(*flag_try) (void *self, const HostMayRaise *raise, uint64 place, size_t flag,
							 bool *result, HostError *error);
	void		(*flag_clear) (void *self, uint64 place, size_t flag);
	uint64		(*atomic) (void *self, uint64 place, size_t atomic, HostSharedOp op);
	size_t		(*shared_atomics) (void *self, uint64 place);
	StdVec		(*blob_read) (void *self, uint64 place, size_t blob);
	bool		(*blob_write) (void *self, uint64 place, size_t blob, const uint8 *bytes,
							   size_t bytes_len, HostError *error);
	bool		(*entry_read) (void *self, uint64 place, size_t array, size_t entry,
							   StdVec *result, HostError *error);
	bool		(*entry_write) (void *self, uint64 place, size_t array, size_t entry,
								const uint8 *bytes, size_t bytes_len, HostError *error);
	uint64		(*shared_location) (void *self, uint64 place);
	bool		(*shared_live) (void *self, uint64 location);
	bool		(*shared_gone) (void *self, const HostSharedKind *kind, uint64 location,
								bool *result, HostError *error);
	uint64		(*slots_name) (void *self, uint64 place, size_t array);
	bool		(*slots_rename) (void *self, uint64 place, size_t array, uint64 loaded,
								 uint64 name);
	void		(*slots_empty) (void *self, uint64 place, size_t array);
	bool		(*slots_claim) (void *self, const HostMayRaise *raise, uint64 place, size_t array,
								const uint8 *bytes, size_t bytes_len, bool *result,
								HostError *error);
	StdVec		(*slots_read) (void *self, uint64 place, size_t array);
	bool		(*words_processes) (void *self, const HostSharedKind *kind, StdVec *result,
									HostError *error);
	bool		(*words_mine) (void *self, const HostSharedKind *kind, uint64 *result,
							   HostError *error);
	void		(*word_store) (void *self, uint64 place, size_t word, uint64 value);
	void		(*word_store_own) (void *self, size_t index, const HostSharedKind *kind,
								   size_t word, uint64 value);
	bool		(*word_load) (void *self, const HostSharedKind *kind, uint32 proc, size_t word,
							  uint64 *result, HostError *error);
	bool		(*words_place) (void *self, const HostSharedKind *kind, uint32 proc, uint64 *result,
								HostError *error);
	bool		(*epochs_lock) (void *self, const HostMayRaise *raise, HostError *error);
	void		(*epochs_unlock) (void *self);
	uint64		(*epochs_advance) (void *self);
	uint64		(*epochs_oldest) (void *self);
	bool		(*epochs_read) (void *self, const HostMayRaise *raise, uint64 *result,
								HostError *error);
	void		(*epochs_end) (void *self, uint64 epoch);
	uint32		(*proc_number) (void *self);
	bool		(*delta_hold) (void *self, const HostMayRaise *raise, uint64 *result,
							   HostError *error);
	void		(*delta_release) (void *self, uint64 epoch);
	bool		(*delta_open) (void *self, HostDeltaPagePage name, uint64 epoch,
							   HostDeltaDirectoryOpened *result, HostError *error);
	bool		(*delta_bytes) (void *self, const HostDeltaDirectoryOpened *opened, uint64 offset,
								uint64 len, HostCacheBytes *result, HostError *error);
	uint64		(*delta_room) (void *self, const HostDeltaDirectoryOpened *opened);
	uint64		(*delta_filled) (void *self, const HostDeltaDirectoryOpened *opened);
	uint64		(*delta_word) (void *self, uint64 body, uint64 index, HostDeltaWordOp op);
	bool		(*delta_alloc) (void *self, const HostMayRaise *raise,
								const HostDeltaPageSize *sizes, size_t sizes_len, StdVec *names,
								bool *has_result, uint64 *result, HostError *error);
	bool		(*delta_alloc_replacing) (void *self, const HostMayRaise *raise, HostDeltaPageSize size,
										  HostDeltaPagePage replaced, StdVec *names, bool *has_result, uint64 *result,
										  HostError *error);
	void		(*delta_give_back) (void *self, HostDeltaPagePage name);
	bool		(*delta_owned_append) (void *self, HostDeltaPagePage name, const uint8 *record,
									   size_t record_len, uint32 *result, HostError *error);
	bool		(*delta_owned_store) (void *self, HostDeltaPagePage name, uint64 index,
									  uint64 value, HostError *error);
	void		(*delta_link) (void *self, HostDeltaPagePage name, HostDeltaDirectoryOwner owner);
	bool		(*delta_append) (void *self, const HostDeltaDirectoryOpened *opened,
								 HostDeltaDirectoryOwner owner, const uint8 *record,
								 size_t record_len, uint32 *result, HostError *error);
	bool		(*delta_seal) (void *self, const HostDeltaDirectoryOpened *opened,
							   HostDeltaDirectoryOwner owner, HostError *error);
	void		(*delta_retire) (void *self, const HostDeltaLeaving *records, size_t records_len,
								 const HostDeltaPagePage *names, size_t names_len);

	/*
	 * delta_packing orders the delta's partly used blocks for a packing and
	 * returns the live pages of the shapes given to move out of the blocks it
	 * drains (bugs row 143), answering false when it can't reserve its lists.
	 */
	bool		(*delta_packing) (void *self, const uint64 *shapes, size_t shapes_len, StdVec *result);

	/*
	 * delta_make's inner Result<(u64, u64), u64> is result_ok with result_0
	 * and result_1 for Ok, or result_err for Err.
	 */
	bool		(*delta_make) (void *self, const HostMayRaise *raise, const HostSharedKind *kind,
							   const char *name, size_t name_len, bool *result_ok,
							   uint64 *result_0, uint64 *result_1, uint64 *result_err,
							   HostError *error);
	bool		(*delta_record) (void *self, const HostSharedKind *kind, uint64 location,
								 uint64 epoch, uint64 *result, HostError *error);
	void		(*delta_publish) (void *self, uint64 place);
	void		(*delta_unmake) (void *self, uint64 place);
	void		(*delta_pass) (void *self);
	void		(*delta_advanced) (void *self, uint64 *result_0, uint64 *result_1);
	uint64		(*delta_used) (void *self);
	uint64		(*delta_budget) (void *self);
	void		(*delta_seed) (void *self, uint8 *result);
	bool		(*delta_wait) (void *self, const HostMayRaise *raise, uint64 frees,
							   HostError *error);
	bool		(*backup_begin) (void *self, const HostMayRaise *raise, HostError *error);
	void		(*backup_end) (void *self);
	bool		(*backups_running) (void *self);
	bool		(*collection_begin) (void *self);
	void		(*collection_end) (void *self);
	bool		(*collection_ask) (void *self, const HostMayRaise *raise, uint32 database, uint32 emptied,
								   uint64 *result, HostError *error);
	bool		(*collection_asked) (void *self, uint64 *result_0, uint32 *result_1, uint32 *result_2);
	void		(*collection_answer) (void *self, uint64 word, const uint64 *counts);
	void		(*collector_started) (void *self);
	bool		(*collector_running) (void *self);
	bool		(*database_locked) (void *self, const HostMayRaise *raise, uint32 database, bool *result,
									HostError *error);
	void		(*database_unlocked) (void *self, uint32 database);
	bool		(*database_lock_waited) (void *self, const HostMayRaise *raise, uint32 database,
										 bool *result, HostError *error);
	bool		(*cache_get) (void *self, const HostMayRaise *raise, HostCachePlacement placement,
							  const HostCacheKey *key, bool *has_result, HostCacheFound *result,
							  HostError *error);
	bool		(*cache_install) (void *self, const HostMayRaise *raise,
								  HostCachePlacement placement, const HostCacheKey *key,
								  size_t len, void (*fill) (void *, uint8 *, size_t),
								  void *fill_captures, bool *has_result, HostCacheFound *result,
								  HostError *error);
	bool		(*cache_contains) (void *self, const HostMayRaise *raise,
								   HostCachePlacement placement, const HostCacheKey *key,
								   bool *result, HostError *error);
	void		(*cache_unpin) (void *self, HostCachePlacement placement, uint32 frame);
	bool		(*cache_make_local) (void *self, const HostMayRaise *raise, size_t bytes,
									 HostError *error);
	bool		(*owner_enlarge) (void *self, const HostMayRaise *raise, uint64 *result,
								  HostError *error);
	bool		(*owner_top_enlarge) (void *self, const HostMayRaise *raise, uint64 *result,
									  HostError *error);
	void		(*owner_remember) (void *self, uint64 owner, uint64 pin);
	void		(*owner_forget) (void *self, uint64 owner, uint64 pin);
	void		(*owned_remember) (void *self, uint64 owner, uint64 record,
								   void (*release) (uint32));
	void		(*owned_forget) (void *self, uint64 owner, uint64 record);
	bool		(*btree_compare) (void *self, const HostMayRaise *raise,
								  const HostCompared *compared, const uint8 *a, size_t a_len,
								  const uint8 *b, size_t b_len, int32 *result, HostError *error);
	bool		(*sort_key) (void *self, const HostMayRaise *raise, uint32 collation,
							 const uint8 *text, size_t text_len, StdVec *result, HostError *error);
	bool		(*reformed) (void *self, const HostMayRaise *raise, const HostReformRow *row,
							 HostReformedRow *result, HostError *error);
} HostHostOps;

/*
 * HOST_VERSIONED is the directory, in the data directory and in each
 * tablespace's version directory, that holds each database's versioned files
 * in a directory named by the database's id.
 */
#define HOST_VERSIONED "pg_versioned"

/* HOST_PAUSE_NAME is the bytes of a pause's name with its NUL, Postgres' INJ_NAME_MAXLEN. */
#define HOST_PAUSE_NAME 64

/*
 * HOST_PG_DEFAULT is the id of Postgres' default tablespace, pg_default,
 * whose files are in the data directory itself.
 */
#define HOST_PG_DEFAULT 1663

/* HOST_STRIPES is the count of the key lock's stripes, each one of Postgres' lightweight locks ((4b)'s key lock). */
#define HOST_STRIPES 4096

/* HOST_HOST is the host installed for the process, a OnceLock; HOST_HOST_VALUE is its value. */
extern StdOnceLock HOST_HOST;
extern HostHost HOST_HOST_VALUE;

extern HostMayRaise host_MayRaise_of(StdAny raise);
extern HostMayRaise host_doors(HostIoReadDoors io);

/* host_doors_of_io_doors is host_doors of a view that implements IoDoors, which Rust coerces implicitly. */
#define host_doors_of_io_doors(io) host_doors(HOST_IO_IO_DOORS_READ_DOORS(io))

extern StdAny host_MayRaise_inner(const HostMayRaise *self);
extern HostError host_Error_fork(const uint8 *sqlstate, StdString *message);
extern HostError host_Error_hinted(const uint8 *sqlstate, const char *message, size_t message_len,
								   const char *hint, size_t hint_len);
extern HostError host_Error_out_of_memory(void);
extern HostError host_Error_postgres(const uint8 *sqlstate, StdAny *error);
extern const uint8 *host_Error_sqlstate(const HostError *self);
extern StdStr host_Error_message(const HostError *self);
extern StdStr host_Error_hint(const HostError *self);
extern bool host_Error_into_cause(HostError *self, StdAny *result, HostForked *error);
extern void host_Error_drop(HostError *self);
extern bool host_Error_Debug_fmt(const void *value, StdFormatter *f);
extern int32 host_File_number(const HostFile *self);
extern void host_File_drop(HostFile *self);
extern const HostFile *host_ScratchFile_Deref_deref(const HostScratchFile *self);
extern void host_ScratchFile_drop(HostScratchFile *self);
extern HostSnapshot host_Snapshot_made(HostSnapshotKind kind, HostXid xmin, HostXid xmax,
									   HostCommand command, bool recovery, bool overflowed,
									   const uint32 *xip, size_t xip_len,
									   const uint32 *subxip, size_t subxip_len, StdAny of);
extern StdAny host_Snapshot_of(const HostSnapshot *self);
extern bool host_WalKind_of(uint8 value, HostWalKind *result);
extern uint8 host_WalKind_value(HostWalKind self);
extern const char *host_WalKind_name(HostWalKind self);
extern void host_DelayCheckpoint_finish(HostDelayCheckpoint *self);
extern void host_DelayCheckpoint_drop(HostDelayCheckpoint *self);
extern void host_DelayCheckpoint_failed(HostDelayCheckpoint *self, const HostError *error);
extern HostCompressDictionary host_CompressDictionary_new(StdAny *prepared);
extern StdAny host_CompressDictionary_prepared(const HostCompressDictionary *self);
extern void host_CompressDictionary_drop(HostCompressDictionary *self);
extern HostDecompressDictionary host_DecompressDictionary_new(StdAny *prepared);
extern StdAny host_DecompressDictionary_prepared(const HostDecompressDictionary *self);
extern void host_DecompressDictionary_drop(HostDecompressDictionary *self);
extern bool host_Storage_PartialEq_eq(const HostStorage *self, const HostStorage *other);
extern bool host_Storage_Debug_fmt(const void *value, StdFormatter *f);
extern bool host_Xid_PartialEq_eq(const HostXid *self, const HostXid *other);
extern bool host_Xid_Debug_fmt(const void *value, StdFormatter *f);
extern bool host_Command_Debug_fmt(const void *value, StdFormatter *f);
extern bool host_Row_PartialEq_eq(const HostRow *self, const HostRow *other);
extern bool host_Row_Debug_fmt(const void *value, StdFormatter *f);
extern bool host_Refused_Debug_fmt(const void *value, StdFormatter *f);
extern bool host_install(HostHost host, HostRefused *error);
#ifdef FRONTEND
extern bool host_install_reading(HostReadHost host, HostRefused *error);
#endif
extern bool host_installed(void);
extern bool host_acting(void);
extern void host_yield_point(void);
extern bool host_at_exit(const StdTypeId *e_type, const HostExitOps *e, HostError *error);
extern HostHost host_current(void);
extern HostReadHost host_reading(void);
extern StdSystemTime host_now(void);
extern StdInstant host_instant(void);
extern StdDuration host_elapsed(StdInstant since);
extern uint32 host_process_id(void);
extern uint64 host_work_mem(void);
extern bool host_session_database(uint32 *result_0, uint32 *result_1, HostError *error);
extern bool host_exiting(void);
extern uint64 host_unique(void);
extern bool host_open(HostIoIoDoors io, const char *path, size_t path_len, HostOpen how,
					  HostFile *result, HostError *error);
extern bool host_open_to_read(HostIoReadDoors io, const char *path, size_t path_len,
							  HostFile *result, HostError *error);
extern bool host_open_if_there(HostIoReadDoors io, const char *path, size_t path_len,
							   bool *has_result, HostFile *result, HostError *error);
extern bool host_create_if_absent(HostIoIoDoors io, const char *path, size_t path_len,
								  bool *has_result, HostFile *result, HostError *error);
extern bool host_open_scratch(HostIoIo *io, HostScratchFile *result, HostError *error);
extern bool host_read_at(HostIoReadDoors io, const HostFile *file, uint64 offset,
						 uint8 *buffer, size_t buffer_len, HostError *error);
extern bool host_write_at(HostIoIoDoors io, const HostFile *file, uint64 offset,
						  const uint8 *bytes, size_t bytes_len, HostError *error);
extern bool host_size(HostIoReadDoors io, const HostFile *file, uint64 *result,
					  HostError *error);
extern bool host_truncate(HostIoIoDoors io, const HostFile *file, uint64 len,
						  HostError *error);
extern void host_prefetch(const HostFile *file, uint64 offset, uint64 len);
extern bool host_sync(HostIoIoDoors io, const HostFile *file, HostError *error);
extern bool host_rename(HostIoIoDoors io, const char *from, size_t from_len, const char *to,
						size_t to_len, HostError *error);
extern bool host_rename_durably(HostIoIoDoors io, const char *from, size_t from_len,
								const char *to, size_t to_len, HostError *error);
extern bool host_remove_durably(HostIoIoDoors io, const char *path, size_t path_len,
								HostError *error);
extern bool host_remove_durably_if_there(HostIoIoDoors io, const char *path, size_t path_len,
										 bool *result, HostError *error);
extern void host_remove_leftover(HostIoIoDoors io, const char *path, size_t path_len);
extern StdPathBuf host_tablespace_dir(uint32 database, uint32 tablespace);
extern StdPathBuf host_versioned_dir(uint32 tablespace);
extern bool host_tablespaces(HostIoReadDoors io, StdVec *result, HostError *error);
extern bool host_rename_back(HostIoIoDoors io, const char *from, size_t from_len,
							 const char *to, size_t to_len);
extern bool host_remove_tree(HostIoIoDoors io, const char *path, size_t path_len,
							 bool *result, HostError *error);
extern bool host_sync_path(HostIoIoDoors io, const char *path, size_t path_len,
						   bool directory, HostError *error);
extern StdStr host_directory_of(const char *path, size_t path_len);
extern bool host_make_dir(HostIoIoDoors io, const char *path, size_t path_len,
						  HostError *error);
extern bool host_list_dir(HostIoReadDoors io, const char *path, size_t path_len,
						  StdVec *result, HostError *error);
extern bool host_list_dir_if_there(HostIoReadDoors io, const char *path, size_t path_len,
								   bool *has_result, StdVec *result, HostError *error);
extern bool host_disk_space(HostIoIoDoors io, const char *path, size_t path_len, uint64 *result_0,
							uint64 *result_1, HostError *error);
extern bool host_tree_bytes(HostIoIoDoors io, const char *path, size_t path_len, uint64 *result,
							HostError *error);
extern bool host_nap(HostIoIoDoors io, StdDuration duration, HostError *error);
extern bool host_wal_insert(HostIoIoDoors io, HostWalKind kind, const uint8 *body,
							size_t body_len, HostLsn *result, HostError *error);
extern bool host_NewCid_Debug_fmt(const void *value, StdFormatter *f);
extern bool host_wal_new_cid(HostIoIoDoors io, const HostStorage *storage, HostRow tid,
							 HostNewCid cid, HostLsn *result, HostError *error);
extern bool host_wal_flush(HostIoIoDoors io, HostLsn lsn, HostError *error);
extern bool host_fold_point(HostIoIoDoors io, HostLsn *result, HostError *error);
extern bool host_checkpoint_now(HostIoIoDoors io, HostError *error);
extern bool host_quiet_database(HostIoIoDoors io, uint32 database, HostError *error);
extern bool host_unquiet_database(HostIoIoDoors io, uint32 database, HostError *error);
extern bool host_last_redo(HostIoIoDoors io, HostLsn *result, HostError *error);
extern bool host_status(HostIoIoDoors io, HostXid xid, HostStatus *result, HostError *error);
extern bool host_in_progress(HostIoIoDoors io, HostXid xid, bool *result, HostError *error);
extern bool host_xids_answered(void);
extern bool host_removable_in(HostIoIoDoors io, const HostSnapshot *snapshot, HostXid xid,
							  bool *result, HostError *error);
extern bool host_current_writer(HostIoIoDoors io, HostXid *result_0, HostXid *result_1,
								HostError *error);
extern bool host_btree_compare(HostIoIo *io, const HostCompared *compared, const uint8 *a,
							   size_t a_len, const uint8 *b, size_t b_len, int32 *result,
							   HostError *error);
extern bool host_reformed(HostIoIo *io, const HostReformRow *row, HostReformedRow *result,
						  HostError *error);
extern bool host_sort_key(HostIoIo *io, uint32 collation, const uint8 *text, size_t text_len,
						  StdVec *result, HostError *error);
extern bool host_current_command(HostIoIoDoors io, bool used, HostCommand *result,
								 HostError *error);
extern bool host_is_current(HostIoIoDoors io, HostXid xid);
extern bool host_combo_command(HostIoIoDoors io, HostXid writer, HostCommand cmin,
							   HostCommand cmax, HostCommand *result, HostError *error);
extern HostIoKeyIo host_KeyGuard_io(HostKeyGuard *self);
extern uint64 host_key_count(uint32 stripe);
extern void host_pushed(HostIoTakesShort io);
extern void host_KeyHold_drop(HostKeyHold *self);
extern bool host_key_hold(HostIoKeyIo *io, uint32 stripe, HostKeyHold *result);
extern bool host_key_wait(HostIoWaits *waits, HostIoIoDoors io, uint32 stripe,
						  HostError *error);
extern bool host_key_lock(HostIoStriping *striping, HostIoIo *io, const uint32 *stripes,
						  size_t stripes_len, HostKeyGuard *result, HostError *error);
extern void host_KeyGuard_drop(HostKeyGuard *self);
extern bool host_xact_wait(HostIoWaits *waits, HostIoIoDoors io, HostXid xid, bool has_on,
						   HostOperation on, bool wait, bool *result, HostError *error);
extern bool host_speculative_wait(HostIoWaits *waits, HostIoIoDoors io, HostXid xid,
								  uint32 token, HostError *error);
extern bool host_lock_tuple(HostIoWaits *waits, HostIoIoDoors io, HostTupleLock tuple,
							bool wait, bool *has_result, HostTupleGuard *result,
							HostError *error);
extern void host_TupleGuard_drop(HostTupleGuard *self);
extern HostWalStand host_wal_stand(HostIoIoDoors io);
extern bool host_warn_kept_wal(HostIoIoDoors io, HostLsn from, HostLsn to, uint32 timeline,
							   HostLsn upto, HostError *error);
extern bool host_next_fork_record(HostIoIoDoors io, HostLsn from, uint32 kinds, bool received, bool *has_result,
								  HostForkRecord *result, HostError *error);
extern bool host_warn(HostIoIoDoors io, HostError *error_, const char *context,
					  HostError *error);
extern bool host_log(HostIoIoDoors io, const char *message, HostError *error);
extern void host_refuse_recovery(HostIoIoDoors io, const char *detail, size_t detail_len);
extern bool host_load_pauses(HostIoIoDoors io, const char *const *pauses, size_t pauses_len, HostError *error);
extern bool host_delay_checkpoint(HostIoIoDoors io, const char *const *pauses,
								  size_t pauses_len, HostDelayCheckpoint *result,
								  HostError *error);
extern bool host_prepare_compressing(const uint8 *bytes, size_t bytes_len, int32 level,
									 HostCompressDictionary *result, HostError *error);
extern bool host_prepare_decompressing(const uint8 *bytes, size_t bytes_len,
									   HostDecompressDictionary *result, HostError *error);
extern bool host_compress(const uint8 *bytes, size_t bytes_len, int32 level,
						  const HostCompressDictionary *dictionary, StdVec *result,
						  HostError *error);
extern bool host_frame_size(const uint8 *frame, size_t frame_len, uint64 *result);
extern bool host_decompress(const uint8 *frame, size_t frame_len, size_t size,
							const HostDecompressDictionary *dictionary, bool *has_result,
							StdVec *result, HostError *error);
extern bool host_train(const StdBytes *samples, size_t samples_len, size_t capacity,
					   StdVec *result);
extern bool host_backup_begin(HostIoIo *io, HostError *error);
extern void host_backup_end(void);
extern bool host_backups_running(void);
extern bool host_collection_begin(void);
extern void host_collection_end(void);
extern bool host_collection_ask(HostIoIo *io, uint32 database, uint32 emptied, uint64 *result, HostError *error);
extern bool host_collection_asked(uint64 *result_0, uint32 *result_1, uint32 *result_2);
extern void host_collection_answer(uint64 word, const uint64 *counts);
extern void host_collector_started(void);
extern bool host_collector_running(void);
extern bool host_database_locked(HostIoIo *io, uint32 database, bool *result, HostError *error);
extern void host_database_unlocked(uint32 database);
extern bool host_database_lock_waited(HostIoIo *io, uint32 database, bool *result, HostError *error);

#ifdef DOLTGRES_TEST_HOST
extern void test_host_tests_the_doors_answer_from_the_installed_host(void);
extern void test_host_tests_a_second_host_is_refused(void);
extern void test_host_tests_elapsed_saturates(void);
extern void test_host_tests_the_process_keeps_one_backend_local_value(void);
#endif

#endif							/* DOLTGRES_HOST_LIB_H */
