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
 * The fork's test module: SQL functions over the test entries postgres
 * exports from entries.list (design items 6, 14, and 17), so the fork's tests
 * can drive its shared memory, host, and WAL records without the server's
 * catalogs naming them.
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/detoast.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/relation.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/toast_compression.h"
#include "access/toast_internals.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_class.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_type_d.h"
#include "commands/progress.h"
#include "commands/repack.h"
#include "commands/tablecmds.h"
#include "commands/tablespace.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "fmgr.h"
#include "port/pg_bswap.h"
#include "portability/instr_time.h"
#include "storage/bufmgr.h"
#include "storage/read_stream.h"
#include "storage/lwlock.h"
#include "tcop/utility.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/tidbitmap.h"
#include "utils/array.h"
#include "utils/backend_progress.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/sampling.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"
#include "varatt.h"

PG_MODULE_MAGIC;

/* The fork's test entries, which entries.list lists and postgres exports. */
extern void doltgres_test_shmem_store(int64 value);
extern int64 doltgres_test_shmem_load(void);
extern void doltgres_test_lwlock_pairs(int64 pairs, int32 way);
extern void doltgres_test_escalate(void);
extern int64 doltgres_test_host(int32 which);
extern Datum doltgres_test_file(int32 step);
extern int64 doltgres_test_wal(int64 value);
extern void doltgres_test_wal_unknown(int32 len);
extern void doltgres_test_delay(int32 step);
extern void doltgres_test_check(void);
extern int64 doltgres_test_lock(int32 step);
extern int64 doltgres_test_shared(int32 step, int64 location, int64 value);
extern Datum doltgres_test_zstd(int32 input, int32 dictionary);
extern int64 doltgres_test_cache(int32 step, int64 key, int64 value);
extern int64 doltgres_test_delta(int32 step, int64 a, int64 b);
extern int64 doltgres_test_status(int64 xid);
extern int64 doltgres_test_in_progress(int64 xid);
extern int64 doltgres_test_writer(int32 which);
extern int64 doltgres_test_command(int32 used);
extern int64 doltgres_test_is_current(int64 xid);
extern int64 doltgres_test_status_table(int32 step, int64 xid, int64 value);
extern void doltgres_test_kept_wal(int64 from, int64 to);
extern void doltgres_test_repository(int id);
extern void doltgres_test_registered(void);
extern int64 doltgres_test_session(bool save);
extern int64 doltgres_test_owned(int32 step, int64 value);
extern Datum doltgres_test_key(FunctionCallInfo fcinfo);
extern int64 doltgres_test_words(void);
extern void doltgres_test_slot(TupleTableSlot *slot, TupleTableSlot *source, int keys, bool descending,
							   const char *version);
extern int64 doltgres_test_att(TupleTableSlot *slot, int att, int field);
extern int64 doltgres_test_states(int64 repository, int64 count);
extern int64 doltgres_test_relation_locks(void);
extern void doltgres_test_base_row(Relation target, TupleTableSlot *source);
extern int64 doltgres_test_base_save(Relation target, int32 extra);
extern Datum doltgres_test_free_space(Relation target, int32 block);
extern void doltgres_test_index_fetch_key(struct IndexFetchTableData *fetch, Relation target, TupleTableSlot *source,
										  uint64 copy);
extern bool doltgres_test_fold(Relation target);
extern Datum doltgres_test_base_sizes(Relation target);
extern Datum doltgres_test_totals(Relation target);
extern Datum doltgres_test_count(Relation target);
extern Datum doltgres_test_index_entries(Relation target);
extern bool doltgres_test_held(Relation target, uint64 tid);
extern Datum doltgres_test_index_base(Relation target);
extern int	doltgres_test_base_levels(Relation target);
extern Datum doltgres_test_index_fold(Relation heap, Relation index);
extern Datum doltgres_test_collect(void);
extern Datum doltgres_test_disk_space(void);
extern bool doltgres_test_database_lock(Oid database, bool hold);
extern Datum doltgres_test_changes(Relation target);
extern Datum doltgres_test_built_nodes(void);
extern bool doltgres_test_mapped(Relation target);
extern Datum doltgres_test_identity(int32 kind, Oid number, int32 attnum);
extern Datum doltgres_test_epochs(void);

/* The table routine's entries, which entries.list lists and postgres exports. */
extern bool doltgres_table_am_routine(Relation relation, const TableAmRoutine **routine);
extern void doltgres_relation_creating(Oid relid, Oid relrewrite);
extern void doltgres_index_routine(Relation relation);

/* doltgres_shmem_base is the fork's shared region, whose lock comes first. */
extern PGDLLIMPORT void *doltgres_shmem_base;

/* doltgres_slot_ops is the fork's kind of tuple table slot. */
extern PGDLLIMPORT const TupleTableSlotOps doltgres_slot_ops;

/* doltgres_table_am is the fork's table access method routine. */
extern PGDLLIMPORT const TableAmRoutine doltgres_table_am;

PG_FUNCTION_INFO_V1(dg_store);
PG_FUNCTION_INFO_V1(dg_load);
PG_FUNCTION_INFO_V1(dg_lock_pairs);
PG_FUNCTION_INFO_V1(dg_escalate);
PG_FUNCTION_INFO_V1(dg_host);
PG_FUNCTION_INFO_V1(dg_file);
PG_FUNCTION_INFO_V1(dg_wal);
PG_FUNCTION_INFO_V1(dg_wal_unknown);
PG_FUNCTION_INFO_V1(dg_delay);
PG_FUNCTION_INFO_V1(dg_check);
PG_FUNCTION_INFO_V1(dg_lock);
PG_FUNCTION_INFO_V1(dg_shared);
PG_FUNCTION_INFO_V1(dg_zstd);
PG_FUNCTION_INFO_V1(dg_toast_lz4);
PG_FUNCTION_INFO_V1(dg_cache);
PG_FUNCTION_INFO_V1(dg_delta);
PG_FUNCTION_INFO_V1(dg_sort_key);
PG_FUNCTION_INFO_V1(dg_sort_key_prefix);
PG_FUNCTION_INFO_V1(dg_status);
PG_FUNCTION_INFO_V1(dg_in_progress);
PG_FUNCTION_INFO_V1(dg_writer);
PG_FUNCTION_INFO_V1(dg_command);
PG_FUNCTION_INFO_V1(dg_is_current);
PG_FUNCTION_INFO_V1(dg_status_table);
PG_FUNCTION_INFO_V1(dg_owned);
PG_FUNCTION_INFO_V1(dg_key);
PG_FUNCTION_INFO_V1(dg_words);
PG_FUNCTION_INFO_V1(dg_slot);
PG_FUNCTION_INFO_V1(dg_slot_atts);
PG_FUNCTION_INFO_V1(dg_table_routine);
PG_FUNCTION_INFO_V1(dg_relation_creating);
PG_FUNCTION_INFO_V1(dg_states);
PG_FUNCTION_INFO_V1(dg_relation_locks);
PG_FUNCTION_INFO_V1(dg_base);
PG_FUNCTION_INFO_V1(dg_scan);
PG_FUNCTION_INFO_V1(dg_fetch);
PG_FUNCTION_INFO_V1(dg_tid_valid);
PG_FUNCTION_INFO_V1(dg_latest);
PG_FUNCTION_INFO_V1(dg_fetch_memory);
PG_FUNCTION_INFO_V1(dg_tidrange);
PG_FUNCTION_INFO_V1(dg_tidrange_again);
PG_FUNCTION_INFO_V1(dg_blocks);
PG_FUNCTION_INFO_V1(dg_estimate);
PG_FUNCTION_INFO_V1(dg_parallel);
PG_FUNCTION_INFO_V1(dg_insert);
PG_FUNCTION_INFO_V1(dg_index_fetch);
PG_FUNCTION_INFO_V1(dg_scan_dirty);
PG_FUNCTION_INFO_V1(dg_toast);
PG_FUNCTION_INFO_V1(dg_index_fetch_key);
PG_FUNCTION_INFO_V1(dg_bitmap);
PG_FUNCTION_INFO_V1(dg_standing_query);
PG_FUNCTION_INFO_V1(dg_analyze);
PG_FUNCTION_INFO_V1(dg_write);
PG_FUNCTION_INFO_V1(dg_delete);
PG_FUNCTION_INFO_V1(dg_scan_nonvacuumable);
PG_FUNCTION_INFO_V1(dg_speculative);
PG_FUNCTION_INFO_V1(dg_index_delete);
PG_FUNCTION_INFO_V1(dg_index_routine);

PG_FUNCTION_INFO_V1(dg_kept_wal);
PG_FUNCTION_INFO_V1(dg_repository);
PG_FUNCTION_INFO_V1(dg_registered);
PG_FUNCTION_INFO_V1(dg_session);
PG_FUNCTION_INFO_V1(dg_fold);
PG_FUNCTION_INFO_V1(dg_vacuum);
PG_FUNCTION_INFO_V1(dg_free_space);
PG_FUNCTION_INFO_V1(dg_base_sizes);
PG_FUNCTION_INFO_V1(dg_index_entries);
PG_FUNCTION_INFO_V1(dg_index_buildempty);
PG_FUNCTION_INFO_V1(dg_held);
PG_FUNCTION_INFO_V1(dg_index_build);
PG_FUNCTION_INFO_V1(dg_index_base);
PG_FUNCTION_INFO_V1(dg_index_vacuumed);
PG_FUNCTION_INFO_V1(dg_index_fold);
PG_FUNCTION_INFO_V1(dg_index_validate);
PG_FUNCTION_INFO_V1(dg_collect);
PG_FUNCTION_INFO_V1(dg_disk_space);
PG_FUNCTION_INFO_V1(dg_database_lock);
PG_FUNCTION_INFO_V1(dg_changes);
PG_FUNCTION_INFO_V1(dg_new_file);
PG_FUNCTION_INFO_V1(dg_truncate);
PG_FUNCTION_INFO_V1(dg_rewrite);
PG_FUNCTION_INFO_V1(dg_built_nodes);
PG_FUNCTION_INFO_V1(dg_move);
PG_FUNCTION_INFO_V1(dg_mapped);
PG_FUNCTION_INFO_V1(dg_identity);
PG_FUNCTION_INFO_V1(dg_object_access_replaced);
PG_FUNCTION_INFO_V1(dg_epochs);
PG_FUNCTION_INFO_V1(dg_index_tree_height);
PG_FUNCTION_INFO_V1(dg_index_parallel);
PG_FUNCTION_INFO_V1(dg_index_members);
PG_FUNCTION_INFO_V1(dg_base_levels);
PG_FUNCTION_INFO_V1(dg_watch_rewrites);
PG_FUNCTION_INFO_V1(dg_rewrites_seen);
PG_FUNCTION_INFO_V1(dg_twin_handler);
PG_FUNCTION_INFO_V1(dg_totals);
PG_FUNCTION_INFO_V1(dg_count);
PG_FUNCTION_INFO_V1(dg_drop_constraint);
PG_FUNCTION_INFO_V1(dg_unchain);

/* dg_store writes the shared area's root record under the fork's lock. */
Datum
dg_store(PG_FUNCTION_ARGS)
{
	doltgres_test_shmem_store(PG_GETARG_INT64(0));
	PG_RETURN_VOID();
}

/* dg_load reads the shared area's root record under the fork's lock. */
Datum
dg_load(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_shmem_load());
}

/*
 * dg_lock_pairs returns the nanoseconds one take and release of the fork's
 * lock costs, over a number of pairs: through the fork's guarded call (way 0),
 * through a trait object over it (way 1), or by calling Postgres directly
 * (way 2).
 */
Datum
dg_lock_pairs(PG_FUNCTION_ARGS)
{
	int64		pairs = PG_GETARG_INT64(0);
	int32		way = PG_GETARG_INT32(1);
	instr_time	start;
	instr_time	duration;

	INSTR_TIME_SET_CURRENT(start);
	if (way == 2)
	{
		LWLock	   *lock = (LWLock *) doltgres_shmem_base;

		for (int64 i = 0; i < pairs; i++)
		{
			LWLockAcquire(lock, LW_EXCLUSIVE);
			LWLockRelease(lock);
		}
	}
	else
		doltgres_test_lwlock_pairs(pairs, way);
	INSTR_TIME_SET_CURRENT(duration);
	INSTR_TIME_SUBTRACT(duration, start);
	PG_RETURN_FLOAT8((double) INSTR_TIME_GET_NANOSEC(duration) / pairs);
}

/* dg_escalate raises inside an escalating wrapper, which PANICs. */
Datum
dg_escalate(PG_FUNCTION_ARGS)
{
	doltgres_test_escalate();
	PG_RETURN_VOID();
}

/* dg_host returns one of the server's host's answers, as doltgres_test_host. */
Datum
dg_host(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_host(PG_GETARG_INT32(0)));
}

/* dg_file runs one of the file door's steps, as doltgres_test_file. */
Datum
dg_file(PG_FUNCTION_ARGS)
{
	return doltgres_test_file(PG_GETARG_INT32(0));
}

/* dg_wal inserts and flushes a test record, returning the position past it. */
Datum
dg_wal(PG_FUNCTION_ARGS)
{
	PG_RETURN_LSN((XLogRecPtr) doltgres_test_wal(PG_GETARG_INT64(0)));
}

/* dg_wal_unknown inserts and flushes a record of a kind no crate owns. */
Datum
dg_wal_unknown(PG_FUNCTION_ARGS)
{
	doltgres_test_wal_unknown(PG_GETARG_INT32(0));
	PG_RETURN_VOID();
}

/* dg_delay runs one of the checkpoint delay's steps, as doltgres_test_delay. */
Datum
dg_delay(PG_FUNCTION_ARGS)
{
	doltgres_test_delay(PG_GETARG_INT32(0));
	PG_RETURN_VOID();
}

/* dg_check checks for a pending interrupt until one is, as doltgres_test_check. */
Datum
dg_check(PG_FUNCTION_ARGS)
{
	doltgres_test_check();
	PG_RETURN_VOID();
}

/* dg_lock runs one of the counter's steps, as doltgres_test_lock. */
Datum
dg_lock(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_lock(PG_GETARG_INT32(0)));
}

/* dg_shared runs one of the shared record's steps, as doltgres_test_shared. */
Datum
dg_shared(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_shared(PG_GETARG_INT32(0), PG_GETARG_INT64(1), PG_GETARG_INT64(2)));
}

/* dg_zstd returns the fork's zstd output for a corpus input, as doltgres_test_zstd. */
Datum
dg_zstd(PG_FUNCTION_ARGS)
{
	return doltgres_test_zstd(PG_GETARG_INT32(0), PG_GETARG_INT32(1));
}

/*
 * dg_toast_lz4 returns TOAST's lz4 compression of a value, its header
 * included, or NULL when the value doesn't shrink.
 */
Datum
dg_toast_lz4(PG_FUNCTION_ARGS)
{
	Datum		compressed;
	bytea	   *result;

	compressed = toast_compress_datum(PointerGetDatum(PG_DETOAST_DATUM(PG_GETARG_DATUM(0))),
									  TOAST_LZ4_COMPRESSION);
	if (DatumGetPointer(compressed) == NULL)
		PG_RETURN_NULL();
	result = (bytea *) palloc(VARHDRSZ + VARSIZE(DatumGetPointer(compressed)));
	SET_VARSIZE(result, VARHDRSZ + VARSIZE(DatumGetPointer(compressed)));
	memcpy(VARDATA(result), DatumGetPointer(compressed), VARSIZE(DatumGetPointer(compressed)));
	PG_RETURN_BYTEA_P(result);
}

/* dg_cache runs one of the node cache's steps, as doltgres_test_cache. */
Datum
dg_cache(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_cache(PG_GETARG_INT32(0), PG_GETARG_INT64(1), PG_GETARG_INT64(2)));
}

/* dg_delta runs one of the delta's pages' steps, as doltgres_test_delta. */
Datum
dg_delta(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_delta(PG_GETARG_INT32(0), PG_GETARG_INT64(1), PG_GETARG_INT64(2)));
}

/*
 * dg_sort_key returns the sort key Postgres' pg_strnxfrm gives a text under
 * the call's collation, which has sort keys, as every locale the fork builds
 * has: a libc-named one is the collator's.
 */
Datum
dg_sort_key(PG_FUNCTION_ARGS)
{
	text	   *value = PG_GETARG_TEXT_PP(0);
	pg_locale_t locale = pg_newlocale_from_collation(PG_GET_COLLATION());
	size_t		len;
	bytea	   *result;

	len = pg_strnxfrm(NULL, 0, VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value), locale);
	result = (bytea *) palloc(VARHDRSZ + len + 1);
	pg_strnxfrm(VARDATA(result), len + 1, VARDATA_ANY(value), VARSIZE_ANY_EXHDR(value), locale);
	SET_VARSIZE(result, VARHDRSZ + strlen(VARDATA(result)));
	PG_RETURN_BYTEA_P(result);
}

/*
 * dg_sort_key_prefix returns what Postgres' pg_strnxfrm_prefix writes into
 * a buffer of the given size for a text under the call's collation, which
 * gives a key's first bytes, as every locale the fork builds does: a
 * libc-named one is the collator's.
 */
Datum
dg_sort_key_prefix(PG_FUNCTION_ARGS)
{
	text	   *value = PG_GETARG_TEXT_PP(0);
	int32		size = PG_GETARG_INT32(1);
	pg_locale_t locale = pg_newlocale_from_collation(PG_GET_COLLATION());
	bytea	   *result;

	if (size < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("size must not be negative")));
	result = (bytea *) palloc(VARHDRSZ + size);
	SET_VARSIZE(result, VARHDRSZ + pg_strnxfrm_prefix(VARDATA(result), size, VARDATA_ANY(value),
													  VARSIZE_ANY_EXHDR(value), locale));
	PG_RETURN_BYTEA_P(result);
}

/* dg_status returns what the status door says of an xid, as doltgres_test_status. */
Datum
dg_status(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_status(PG_GETARG_INT64(0)));
}

/* dg_in_progress returns what the in_progress door says of an xid, as doltgres_test_in_progress. */
Datum
dg_in_progress(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_in_progress(PG_GETARG_INT64(0)));
}

/* dg_writer returns the current_writer door's xids, as doltgres_test_writer. */
Datum
dg_writer(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_writer(PG_GETARG_INT32(0)));
}

/* dg_command returns the current_command door's command, as doltgres_test_command. */
Datum
dg_command(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_command(PG_GETARG_INT32(0)));
}

/* dg_is_current returns what the is_current door says of an xid, as doltgres_test_is_current. */
Datum
dg_is_current(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_is_current(PG_GETARG_INT64(0)));
}

/* dg_status_table runs one of the commit status table's steps, as doltgres_test_status_table. */
Datum
dg_status_table(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_status_table(PG_GETARG_INT32(0), PG_GETARG_INT64(1), PG_GETARG_INT64(2)));
}

/* dg_kept_wal checks the WAL between two positions as a checkpoint checks the WAL it keeps, as doltgres_test_kept_wal. */
Datum
dg_kept_wal(PG_FUNCTION_ARGS)
{
	doltgres_test_kept_wal(PG_GETARG_INT64(0), PG_GETARG_INT64(1));
	PG_RETURN_VOID();
}

/* dg_repository makes the repository of a database id, created and saved once, as doltgres_test_repository. */
Datum
dg_repository(PG_FUNCTION_ARGS)
{
	doltgres_test_repository(PG_GETARG_INT32(0));
	PG_RETURN_VOID();
}

/* dg_registered enters a root registration and pauses inside it, as doltgres_test_registered. */
Datum
dg_registered(PG_FUNCTION_ARGS)
{
	doltgres_test_registered();
	PG_RETURN_VOID();
}

/* dg_session opens the session repository, saving it first when asked, as doltgres_test_session. */
Datum
dg_session(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_session(PG_GETARG_BOOL(0)));
}

/* dg_owned runs one of the owned records' test steps, as doltgres_test_owned. */
Datum
dg_owned(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_owned(PG_GETARG_INT32(0), PG_GETARG_INT64(1)));
}

/* dg_key returns the key column of its first argument, as doltgres_test_key. */
Datum
dg_key(PG_FUNCTION_ARGS)
{
	return doltgres_test_key(fcinfo);
}

/* dg_words counts the word readers and writers that differ from their macros, as doltgres_test_words. */
Datum
dg_words(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_words());
}

/*
 * dg_same raises unless a slot holds the first natts attributes of the row heap's own slot holds: for each that
 * wasn't dropped, the same NULL flag, and for a value passed by value the same whole word, for one of a fixed length
 * the same bytes at the type's alignment, and for a varlena the same bytes, at the type's alignment when its header
 * is four bytes, and under the header heap gives a value it stores inline and uncompressed in a column whose values
 * it packs; in any other column heap keeps the header a value came with.
 */
static void
dg_same(TupleTableSlot *slot, TupleTableSlot *heap, int natts)
{
	TupleDesc	desc = heap->tts_tupleDescriptor;

	for (int i = 0; i < natts; i++)
	{
		CompactAttribute *att = TupleDescCompactAttr(desc, i);
		bool		isnull;
		bool		heapnull;
		Datum		value = slot_getattr(slot, i + 1, &isnull);
		Datum		heapvalue = slot_getattr(heap, i + 1, &heapnull);

		if (att->attisdropped)
			continue;
		if (isnull != heapnull)
			elog(ERROR, "attribute %d: one slot holds NULL", i + 1);
		if (isnull)
			continue;
		if (att->attbyval)
		{
			if (value != heapvalue)
				elog(ERROR, "attribute %d: the words differ", i + 1);
		}
		else if (att->attlen > 0)
		{
			if ((uintptr_t) DatumGetPointer(value) % att->attalignby != 0 ||
				memcmp(DatumGetPointer(value), DatumGetPointer(heapvalue), att->attlen) != 0)
				elog(ERROR, "attribute %d: the bytes differ", i + 1);
		}
		else
		{
			struct varlena *mine = (struct varlena *) DatumGetPointer(value);
			struct varlena *heaps = (struct varlena *) DatumGetPointer(heapvalue);
			struct varlena *plain = pg_detoast_datum_packed(mine);
			struct varlena *heapplain = pg_detoast_datum_packed(heaps);

			if (att->attispackable && !VARATT_IS_EXTERNAL(heaps) && !VARATT_IS_COMPRESSED(heaps) &&
				VARATT_IS_SHORT(mine) != VARATT_IS_SHORT(heaps))
				elog(ERROR, "attribute %d: the headers differ", i + 1);
			if (VARATT_IS_4B(mine) && (uintptr_t) mine % att->attalignby != 0)
				elog(ERROR, "attribute %d: the value is not aligned", i + 1);
			if (VARSIZE_ANY_EXHDR(plain) != VARSIZE_ANY_EXHDR(heapplain) ||
				memcmp(VARDATA_ANY(plain), VARDATA_ANY(heapplain), VARSIZE_ANY_EXHDR(plain)) != 0)
				elog(ERROR, "attribute %d: the bytes differ", i + 1);
		}
	}
}

/*
 * dg_slot runs one step of the slot's test over every row of a heap table and returns how many rows it compared with
 * heap's own. Steps 0 to 4 store the row in a slot of the fork's kind as its table's tree would store it, with the
 * first keys attributes as its primary key: 0 reads it back an attribute at a time, with a dropped attribute NULL and
 * a full header on a value of a column that is never packed, 1 copies it to a second slot of
 * the fork's kind, 2 clears the slot and stores the row again, 3 reads the slot's minimal tuple, and 4 its heap
 * tuple. Step 5 copies heap's row from a slot of another kind and frees that slot's row, and 6 has the slot take the
 * executor's values and then the memory they point into freed. Step 7 asks the stored row for its xmin, and 8
 * whether this transaction wrote it. Step 9 copies the row's attributes but the last from a slot that has no more,
 * and any other step copies the row into a slot of the fork's kind that lacks the last attribute.
 */
Datum
dg_slot(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	int			keys = PG_GETARG_INT32(1);
	bool		descending = PG_GETARG_BOOL(2);
	int			step = PG_GETARG_INT32(3);
	char	   *version = text_to_cstring(PG_GETARG_TEXT_PP(4));
	TupleDesc	desc = RelationGetDescr(rel);
	TupleTableSlot *heap = table_slot_create(rel, NULL);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &doltgres_slot_ops);
	TupleDesc	fewer = CreateTupleDescTruncatedCopy(desc, desc->natts - 1);
	TupleTableSlot *other = MakeSingleTupleTableSlot(step > 8 ? fewer : desc,
													 step == 1 || step > 9 ? &doltgres_slot_ops :
													 step == 3 ? &TTSOpsMinimalTuple :
													 step == 9 ? &TTSOpsVirtual : &TTSOpsHeapTuple);
	TableScanDesc scan = table_beginscan(rel, GetActiveSnapshot(), 0, NULL, 0);
	int64		rows = 0;
	bool		isnull;
	HeapTuple	tuple;

	while (table_scan_getnextslot(scan, ForwardScanDirection, heap))
	{
		slot_getallattrs(heap);
		if (step < 5 || step == 7 || step == 8)
			doltgres_test_slot(slot, heap, keys, descending, "");
		switch (step)
		{
			case 0:
				for (int i = 0; i < desc->natts; i++)
				{
					CompactAttribute *att = TupleDescCompactAttr(desc, i);
					Datum		value = slot_getattr(slot, i + 1, &isnull);

					if (att->attisdropped && !isnull)
						elog(ERROR, "dropped attribute %d is not NULL", i + 1);
					if (!isnull && att->attlen == -1 && !att->attispackable &&
						VARATT_IS_SHORT(DatumGetPointer(value)))
						elog(ERROR, "attribute %d: a value that is never packed has a short header", i + 1);
				}
				dg_same(slot, heap, desc->natts);
				for (int attnum = MaxCommandIdAttributeNumber; attnum <= MinTransactionIdAttributeNumber; attnum++)
				{
					bool		heapnull;
					Datum		mine = slot_getsysattr(slot, attnum, &isnull);
					Datum		heaps = slot_getsysattr(heap, attnum, &heapnull);

					if (isnull != heapnull || mine != heaps)
						elog(ERROR, "system attribute %d differs", attnum);
				}
				break;
			case 1:
				ExecCopySlot(other, slot);
				ExecClearTuple(slot);
				dg_same(other, heap, desc->natts);
				break;
			case 2:
				(void) slot_getattr(slot, 1, &isnull);
				ExecClearTuple(slot);
				if (!TTS_EMPTY(slot))
					elog(ERROR, "a cleared slot is not empty");
				doltgres_test_slot(slot, heap, keys, descending, "");
				dg_same(slot, heap, desc->natts);
				break;
			case 3:
				ExecStoreMinimalTuple(ExecCopySlotMinimalTuple(slot), other, true);
				ExecClearTuple(slot);
				dg_same(other, heap, desc->natts);
				break;
			case 4:
				ExecStoreHeapTuple(ExecCopySlotHeapTuple(slot), other, true);
				ExecClearTuple(slot);
				dg_same(other, heap, desc->natts);
				break;
			case 5:
				ExecStoreHeapTuple(ExecCopySlotHeapTuple(heap), other, true);
				ExecCopySlot(slot, other);
				ExecClearTuple(other);
				dg_same(slot, heap, desc->natts);
				break;
			case 6:
				tuple = ExecCopySlotHeapTuple(heap);
				ExecForceStoreHeapTuple(tuple, slot, false);
				ExecMaterializeSlot(slot);
				heap_freetuple(tuple);
				dg_same(slot, heap, desc->natts);
				break;
			case 7:
				(void) slot_getsysattr(slot, MinTransactionIdAttributeNumber, &isnull);
				break;
			case 8:
				if (slot->tts_ops->is_current_xact_tuple(slot))
					elog(ERROR, "a row another transaction wrote reads as the current one's");
				break;
			case 9:
				ExecClearTuple(other);
				memcpy(other->tts_values, heap->tts_values, fewer->natts * sizeof(Datum));
				memcpy(other->tts_isnull, heap->tts_isnull, fewer->natts * sizeof(bool));
				ExecStoreVirtualTuple(other);
				slot->tts_ops->copyslot(slot, other);
				if (slot->tts_nvalid != fewer->natts)
					elog(ERROR, "a copy of fewer attributes holds another number of them");
				dg_same(slot, heap, fewer->natts);
				break;
			case 11:
				{
					MemoryContext oldcontext = CurrentMemoryContext;
					ResourceOwner oldowner = CurrentResourceOwner;
					bool		before;
					bool		after;

					BeginInternalSubTransaction(NULL);
					doltgres_test_slot(slot, heap, keys, descending,
									   psprintf(UINT64_FORMAT ",0,0,%u",
												U64FromFullTransactionId(GetCurrentFullTransactionId()),
												InvalidCommandId));
					before = slot->tts_ops->is_current_xact_tuple(slot);
					RollbackAndReleaseCurrentSubTransaction();
					MemoryContextSwitchTo(oldcontext);
					CurrentResourceOwner = oldowner;
					after = slot->tts_ops->is_current_xact_tuple(slot);
					if (!before || after)
						elog(ERROR, "a row its subtransaction wrote reads as current %s and %s after the rollback",
							 before ? "before" : "not before", after ? "still" : "not");
				}
				break;
			case 12:
				{
					doltgres_test_slot(slot, heap, keys, descending, version);
					for (int attnum = MaxCommandIdAttributeNumber; attnum <= MinTransactionIdAttributeNumber; attnum++)
					{
						bool		heapnull;
						Datum		mine = slot_getsysattr(slot, attnum, &isnull);
						Datum		heaps = slot_getsysattr(heap, attnum, &heapnull);

						if (isnull != heapnull || mine != heaps)
							elog(ERROR, "system attribute %d differs", attnum);
					}
				}
				break;
			default:
				other->tts_ops->copyslot(other, heap);
				break;
		}
		rows++;
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(other);
	ExecDropSingleTupleTableSlot(slot);
	ExecDropSingleTupleTableSlot(heap);
	table_close(rel, AccessShareLock);
	PG_RETURN_INT64(rows);
}

/*
 * dg_slot_atts compares each attribute of a table as the binding unpacks it from a slot's descriptor with the
 * descriptor itself, and returns how many attributes it compared.
 */
Datum
dg_slot_atts(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	TupleDesc	desc = RelationGetDescr(rel);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &doltgres_slot_ops);
	int			natts = desc->natts;

	for (int i = 0; i < natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (doltgres_test_att(slot, i, 0) != att->atttypid ||
			doltgres_test_att(slot, i, 1) != att->attlen ||
			doltgres_test_att(slot, i, 2) != att->attbyval ||
			doltgres_test_att(slot, i, 3) != att->attisdropped)
			elog(ERROR, "attribute %d is not unpacked as its descriptor has it", i + 1);
	}
	if (doltgres_test_att(slot, natts, 0) != -1 || doltgres_test_att(slot, -1, 0) != -1)
		elog(ERROR, "an attribute out of range is unpacked");
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, AccessShareLock);
	PG_RETURN_INT64(natts);
}

/*
 * dg_table_routine asks the entry that chooses a relation's table routine about a relation and returns whether it
 * gave one, which must be the fork's, whose slots are of the fork's kind; where it gives none it must leave the
 * caller's routine alone.
 */
Datum
dg_table_routine(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	const TableAmRoutine *routine = NULL;
	bool		given = doltgres_table_am_routine(rel, &routine);

	if (given ? routine != &doltgres_table_am || routine->slot_callbacks(rel) != &doltgres_slot_ops : routine != NULL)
		elog(ERROR, "the entry's answer and the routine it gave disagree");
	relation_close(rel, AccessShareLock);
	PG_RETURN_BOOL(given);
}

/*
 * dg_relation_creating tells the fork that a relation is being created and which relation it is made to rewrite, as
 * the creation's call site does.
 */
Datum
dg_relation_creating(PG_FUNCTION_ARGS)
{
	doltgres_relation_creating(PG_GETARG_OID(0), PG_GETARG_OID(1));
	PG_RETURN_VOID();
}

/* dg_states makes and holds a count of storage states in a new repository, as doltgres_test_states. */
Datum
dg_states(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_states(PG_GETARG_INT64(0), PG_GETARG_INT64(1)));
}

/* dg_relation_locks returns the relation locks Postgres can grant at once, as doltgres_test_relation_locks. */
Datum
dg_relation_locks(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(doltgres_test_relation_locks());
}

/*
 * dg_base saves the rows of a heap table as another table's base through the test writer, before any scan of that
 * table, each entry with a count of value ids its row holds no value out of line for, and returns how many rows it
 * saved, as doltgres_test_base_row and doltgres_test_base_save.
 */
Datum
dg_base(PG_FUNCTION_ARGS)
{
	Relation	target = table_open(PG_GETARG_OID(0), AccessExclusiveLock);
	Relation	source = table_open(PG_GETARG_OID(1), AccessShareLock);
	TupleTableSlot *heap = table_slot_create(source, NULL);
	TableScanDesc scan = table_beginscan(source, GetActiveSnapshot(), 0, NULL, 0);
	int64		saved;

	while (table_scan_getnextslot(scan, ForwardScanDirection, heap))
	{
		slot_getallattrs(heap);
		doltgres_test_base_row(target, heap);
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(heap);
	saved = doltgres_test_base_save(target, PG_GETARG_INT32(2));
	table_close(source, AccessShareLock);
	table_close(target, NoLock);
	PG_RETURN_INT64(saved);
}

/*
 * row_text returns the row a slot holds as its ctid, its xmin, and its columns' text, apart by spaces.
 */
static char *
row_text(TupleTableSlot *slot, TupleDesc desc)
{
	StringInfoData row;
	bool		isnull;
	Datum		xmin = slot_getsysattr(slot, MinTransactionIdAttributeNumber, &isnull);

	initStringInfo(&row);
	appendStringInfo(&row, "(%u,%u) %u", ItemPointerGetBlockNumber(&slot->tts_tid),
					 ItemPointerGetOffsetNumber(&slot->tts_tid), DatumGetTransactionId(xmin));
	for (int i = 0; i < desc->natts; i++)
	{
		Datum		value = slot_getattr(slot, i + 1, &isnull);
		Oid			output;
		bool		is_varlena;

		getTypeOutputInfo(TupleDescAttr(desc, i)->atttypid, &output, &is_varlena);
		appendStringInfo(&row, " %s", isnull ? "NULL" : OidOutputFunctionCall(output, value));
	}
	return row.data;
}

/*
 * standing calls a test function's body over its first argument's table with the fork's table access method routine
 * standing as the table's own, which the relcache's call site will make true of every relation the fork stores: the
 * fork's scans keep their record only in a scan of such a relation, and Postgres asks a relation's own routine its
 * size. The routine stands as the table's TOAST relation's own too, which a detoast asks for a value's bytes. Each
 * relation's own routine is put back whether the body returns or raises.
 */
static Datum
standing(FunctionCallInfo fcinfo, Datum (*body) (Relation rel, FunctionCallInfo fcinfo))
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	Relation	toast = OidIsValid(rel->rd_rel->reltoastrelid) ? table_open(rel->rd_rel->reltoastrelid, AccessShareLock) : NULL;
	const TableAmRoutine *own = rel->rd_tableam;
	const TableAmRoutine *toast_own = toast != NULL ? toast->rd_tableam : NULL;
	Datum		result;

	PG_TRY();
	{
		rel->rd_tableam = &doltgres_table_am;
		if (toast != NULL)
			toast->rd_tableam = &doltgres_table_am;
		result = body(rel, fcinfo);
	}
	PG_FINALLY();
	{
		rel->rd_tableam = own;
		if (toast != NULL)
			toast->rd_tableam = toast_own;
	}
	PG_END_TRY();
	if (toast != NULL)
		table_close(toast, AccessShareLock);
	table_close(rel, AccessShareLock);
	return result;
}

/*
 * dg_scan_body reads a table through the fork's table access method routine, forward or backward, with a key that its
 * first column equals a number or with none for a negative one, and returns each row as row_text writes it.
 */
static Datum
dg_scan_body(Relation rel, FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ScanDirection direction = PG_GETARG_BOOL(1) ? BackwardScanDirection : ForwardScanDirection;
	int64		equals = PG_GETARG_INT64(2);
	TupleDesc	desc = RelationGetDescr(rel);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &doltgres_slot_ops);
	ScanKeyData key;
	TableScanDesc scan;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	if (equals >= 0)
		ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum((int32) equals));
	scan = doltgres_table_am.scan_begin(rel, GetActiveSnapshot(), equals >= 0 ? 1 : 0, equals >= 0 ? &key : NULL, NULL,
										SO_TYPE_SEQSCAN | SO_ALLOW_STRAT | SO_ALLOW_SYNC | SO_ALLOW_PAGEMODE);
	while (doltgres_table_am.scan_getnextslot(scan, direction, slot))
	{
		Datum		line = CStringGetTextDatum(row_text(slot, desc));
		bool		null = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &line, &null);
	}
	doltgres_table_am.scan_end(scan);
	ExecDropSingleTupleTableSlot(slot);
	return (Datum) 0;
}

/* dg_scan is dg_scan_body with the fork's routine standing as its table's own. */
Datum
dg_scan(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_scan_body);
}

/*
 * dg_fetch_body fetches a table's row by its ctid through the fork's table access method routine, and returns it as
 * row_text writes it, "not visible" for a row the routine says its snapshot doesn't see, or null when no row has the
 * ctid.
 */
static Datum
dg_fetch_body(Relation rel, FunctionCallInfo fcinfo)
{
	ItemPointer tid = PG_GETARG_ITEMPOINTER(1);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &doltgres_slot_ops);
	char	   *row = NULL;

	if (doltgres_table_am.tuple_fetch_row_version(rel, tid, GetActiveSnapshot(), slot))
		row = doltgres_table_am.tuple_satisfies_snapshot(rel, slot, GetActiveSnapshot()) ?
			row_text(slot, RelationGetDescr(rel)) : "not visible";
	ExecDropSingleTupleTableSlot(slot);
	if (row == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(row));
}

/* dg_fetch is dg_fetch_body with the fork's routine standing as its table's own. */
Datum
dg_fetch(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_fetch_body);
}

/*
 * dg_tid_valid_body reports whether a ctid is one a table's storage can hold, through a TID scan of the fork's table access
 * method routine.
 */
static Datum
dg_tid_valid_body(Relation rel, FunctionCallInfo fcinfo)
{
	TableScanDesc scan = doltgres_table_am.scan_begin(rel, GetActiveSnapshot(), 0, NULL, NULL, SO_TYPE_TIDSCAN);
	bool		valid = doltgres_table_am.tuple_tid_valid(scan, PG_GETARG_ITEMPOINTER(1));

	doltgres_table_am.scan_end(scan);
	PG_RETURN_BOOL(valid);
}

/* dg_tid_valid is dg_tid_valid_body with the fork's routine standing as its table's own. */
Datum
dg_tid_valid(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_tid_valid_body);
}

/*
 * dg_latest_body returns the latest version's ctid of a table's row by a ctid, through a TID scan of the fork's table access
 * method routine.
 */
static Datum
dg_latest_body(Relation rel, FunctionCallInfo fcinfo)
{
	TableScanDesc scan = doltgres_table_am.scan_begin(rel, GetActiveSnapshot(), 0, NULL, NULL, SO_TYPE_TIDSCAN);
	ItemPointer tid = palloc_object(ItemPointerData);

	ItemPointerCopy(PG_GETARG_ITEMPOINTER(1), tid);
	doltgres_table_am.tuple_get_latest_tid(scan, tid);
	doltgres_table_am.scan_end(scan);
	PG_RETURN_ITEMPOINTER(tid);
}

/* dg_latest is dg_latest_body with the fork's routine standing as its table's own. */
Datum
dg_latest(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_latest_body);
}

/*
 * dg_fetch_memory_body fetches a table's row by its ctid a number of times through the fork's table access method routine,
 * and returns how much the entry's memory context grew from after the first fetch to after the last.
 */
static Datum
dg_fetch_memory_body(Relation rel, FunctionCallInfo fcinfo)
{
	ItemPointer tid = PG_GETARG_ITEMPOINTER(1);
	int32		times = PG_GETARG_INT32(2);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &doltgres_slot_ops);
	Size		first = 0;
	int64		growth;

	for (int i = 0; i < times; i++)
	{
		doltgres_table_am.tuple_fetch_row_version(rel, tid, GetActiveSnapshot(), slot);
		if (i == 0)
			first = MemoryContextMemAllocated(CurrentMemoryContext, true);
	}
	growth = (int64) MemoryContextMemAllocated(CurrentMemoryContext, true) - (int64) first;
	ExecDropSingleTupleTableSlot(slot);
	PG_RETURN_INT64(growth);
}

/* dg_fetch_memory is dg_fetch_memory_body with the fork's routine standing as its table's own. */
Datum
dg_fetch_memory(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_fetch_memory_body);
}

/*
 * dg_tidrange_body reads a table's rows whose ctids are in a range through a TID range scan of the fork's table access
 * method routine, forward or backward, and returns each row as row_text writes it.
 */
static Datum
dg_tidrange_body(Relation rel, FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	ScanDirection direction = PG_GETARG_BOOL(3) ? BackwardScanDirection : ForwardScanDirection;
	TupleDesc	desc = RelationGetDescr(rel);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &doltgres_slot_ops);
	TableScanDesc scan;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	scan = doltgres_table_am.scan_begin(rel, GetActiveSnapshot(), 0, NULL, NULL,
										SO_TYPE_TIDRANGESCAN | SO_ALLOW_PAGEMODE);
	doltgres_table_am.scan_set_tidrange(scan, PG_GETARG_ITEMPOINTER(1), PG_GETARG_ITEMPOINTER(2));
	while (doltgres_table_am.scan_getnextslot_tidrange(scan, direction, slot))
	{
		Datum		line = CStringGetTextDatum(row_text(slot, desc));
		bool		null = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &line, &null);
	}
	doltgres_table_am.scan_end(scan);
	ExecDropSingleTupleTableSlot(slot);
	return (Datum) 0;
}

/* dg_tidrange is dg_tidrange_body with the fork's routine standing as its table's own. */
Datum
dg_tidrange(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_tidrange_body);
}

/*
 * dg_tidrange_again_body reads one row of a first ctid range through a TID range scan of the fork's table access method
 * routine, rescans it, sets a second range, and returns that range's rows forward as row_text writes them.
 */
static Datum
dg_tidrange_again_body(Relation rel, FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	desc = RelationGetDescr(rel);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &doltgres_slot_ops);
	TableScanDesc scan;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	scan = doltgres_table_am.scan_begin(rel, GetActiveSnapshot(), 0, NULL, NULL,
										SO_TYPE_TIDRANGESCAN | SO_ALLOW_PAGEMODE);
	doltgres_table_am.scan_set_tidrange(scan, PG_GETARG_ITEMPOINTER(1), PG_GETARG_ITEMPOINTER(2));
	doltgres_table_am.scan_getnextslot_tidrange(scan, ForwardScanDirection, slot);
	doltgres_table_am.scan_rescan(scan, NULL, false, false, false, false);
	doltgres_table_am.scan_set_tidrange(scan, PG_GETARG_ITEMPOINTER(3), PG_GETARG_ITEMPOINTER(4));
	while (doltgres_table_am.scan_getnextslot_tidrange(scan, ForwardScanDirection, slot))
	{
		Datum		line = CStringGetTextDatum(row_text(slot, desc));
		bool		null = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &line, &null);
	}
	doltgres_table_am.scan_end(scan);
	ExecDropSingleTupleTableSlot(slot);
	return (Datum) 0;
}

/* dg_tidrange_again is dg_tidrange_again_body with the fork's routine standing as its table's own. */
Datum
dg_tidrange_again(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_tidrange_again_body);
}

/*
 * dg_blocks_body returns a table's block count as Postgres reads it, RelationGetNumberOfBlocks.
 */
static Datum
dg_blocks_body(Relation rel, FunctionCallInfo fcinfo)
{
	BlockNumber blocks = RelationGetNumberOfBlocks(rel);

	PG_RETURN_INT64(blocks);
}

/* dg_blocks is dg_blocks_body with the fork's routine standing as its table's own. */
Datum
dg_blocks(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_blocks_body);
}

/*
 * dg_estimate_body returns a table's size estimate as the planner reads it, table_relation_estimate_size, as its pages
 * and its tuples apart by a space.
 */
static Datum
dg_estimate_body(Relation rel, FunctionCallInfo fcinfo)
{
	BlockNumber pages;
	double		tuples;
	double		allvisfrac;

	table_relation_estimate_size(rel, NULL, &pages, &tuples, &allvisfrac);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%u %.0f", pages, tuples)));
}

/*
 * dg_estimate is dg_estimate_body with the fork's routine standing as its table's own, or, when its second argument
 * says so, with the table's own routine, heap's.
 */
Datum
dg_estimate(PG_FUNCTION_ARGS)
{
	Relation	rel;
	Datum		estimate;

	if (!PG_GETARG_BOOL(1))
		return standing(fcinfo, dg_estimate_body);
	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	estimate = dg_estimate_body(rel, fcinfo);
	table_close(rel, AccessShareLock);
	return estimate;
}

/*
 * dg_parallel_body reads a table through scans of the fork's table access method routine that share one parallel scan,
 * the routine's own, made in local memory, and returns every row as row_text writes it. The scans are begun on it
 * in turn and read in turn, a row from each, until each has none, as a parallel query's workers would. With a
 * second source, that heap table's rows are saved as the table's base after the shared scan is made and before any
 * scan begins; with a tid range, the scans are TID range scans of it. When asked to, it reads the shared scan a
 * second time after reinitializing it and rescanning each scan, and returns only the second reading's rows.
 */
static Datum
dg_parallel_body(Relation rel, FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	int			workers = PG_GETARG_INT32(1);
	bool		again = PG_GETARG_BOOL(2);
	Oid			resave = PG_GETARG_OID(3);
	bool		ranged = !PG_ARGISNULL(4);
	TupleDesc	desc = RelationGetDescr(rel);
	ParallelTableScanDesc pscan = palloc0(doltgres_table_am.parallelscan_estimate(rel));
	TableScanDesc *scans = palloc_array(TableScanDesc, workers);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &doltgres_slot_ops);
	bool	   *done = palloc0_array(bool, workers);
	uint32		flags = ranged ? SO_TYPE_TIDRANGESCAN | SO_ALLOW_PAGEMODE :
		SO_TYPE_SEQSCAN | SO_ALLOW_STRAT | SO_ALLOW_SYNC | SO_ALLOW_PAGEMODE;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	doltgres_table_am.parallelscan_initialize(rel, pscan);
	pscan->phs_snapshot_any = true;
	if (OidIsValid(resave))
	{
		Relation	source = table_open(resave, AccessShareLock);
		TupleTableSlot *heap = table_slot_create(source, NULL);
		TableScanDesc read = table_beginscan(source, GetActiveSnapshot(), 0, NULL, 0);

		while (table_scan_getnextslot(read, ForwardScanDirection, heap))
		{
			slot_getallattrs(heap);
			doltgres_test_base_row(rel, heap);
		}
		table_endscan(read);
		ExecDropSingleTupleTableSlot(heap);
		doltgres_test_base_save(rel, 0);
		table_close(source, AccessShareLock);
	}
	for (int i = 0; i < workers; i++)
	{
		scans[i] = doltgres_table_am.scan_begin(rel, GetActiveSnapshot(), 0, NULL, pscan, flags);
		if (ranged)
			doltgres_table_am.scan_set_tidrange(scans[i], PG_GETARG_ITEMPOINTER(4), PG_GETARG_ITEMPOINTER(5));
	}
	for (int pass = 0; pass < (again ? 2 : 1); pass++)
	{
		int			left = workers;

		if (pass == 1)
		{
			doltgres_table_am.parallelscan_reinitialize(rel, pscan);
			for (int i = 0; i < workers; i++)
			{
				doltgres_table_am.scan_rescan(scans[i], NULL, false, false, false, false);
				if (ranged)
					doltgres_table_am.scan_set_tidrange(scans[i], PG_GETARG_ITEMPOINTER(4), PG_GETARG_ITEMPOINTER(5));
				done[i] = false;
			}
		}
		while (left > 0)
		{
			for (int i = 0; i < workers; i++)
			{
				bool		found;

				if (done[i])
					continue;
				found = ranged ?
					doltgres_table_am.scan_getnextslot_tidrange(scans[i], ForwardScanDirection, slot) :
					doltgres_table_am.scan_getnextslot(scans[i], ForwardScanDirection, slot);
				if (!found)
				{
					done[i] = true;
					left--;
				}
				else if (pass == (again ? 1 : 0))
				{
					Datum		line = CStringGetTextDatum(row_text(slot, desc));
					bool		null = false;

					tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &line, &null);
				}
			}
		}
	}
	for (int i = 0; i < workers; i++)
		doltgres_table_am.scan_end(scans[i]);
	ExecDropSingleTupleTableSlot(slot);
	return (Datum) 0;
}

/* dg_parallel is dg_parallel_body with the fork's routine standing as its table's own. */
Datum
dg_parallel(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_parallel_body);
}

/*
 * dg_insert inserts a heap table's rows into another table through the fork's table access method routine, one at
 * a time by tuple_insert or all at once by multi_insert, as COPY's path does, with an insert's options, and returns
 * each inserted row's ctid in order.
 */
Datum
dg_insert(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	target = table_open(PG_GETARG_OID(0), RowExclusiveLock);
	Relation	source = table_open(PG_GETARG_OID(1), AccessShareLock);
	bool		multi = PG_GETARG_BOOL(2);
	int			options = PG_GETARG_INT32(3);
	CommandId	cid = GetCurrentCommandId(true);
	TupleTableSlot *heap = table_slot_create(source, NULL);
	TableScanDesc scan = table_beginscan(source, GetActiveSnapshot(), 0, NULL, 0);
	TupleTableSlot **slots = NULL;
	int			count = 0;
	int			size = 0;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	while (table_scan_getnextslot(scan, ForwardScanDirection, heap))
	{
		if (count == size)
		{
			size = size == 0 ? 16 : size * 2;
			slots = slots == NULL ? palloc(size * sizeof(TupleTableSlot *)) :
				repalloc(slots, size * sizeof(TupleTableSlot *));
		}
		slots[count] = table_slot_create(target, NULL);
		ExecCopySlot(slots[count], heap);
		ExecMaterializeSlot(slots[count]);
		if (!multi)
			doltgres_table_am.tuple_insert(target, slots[count], cid, options, NULL);
		count++;
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(heap);
	if (multi && count > 0)
		doltgres_table_am.multi_insert(target, slots, count, cid, options, NULL);
	for (int i = 0; i < count; i++)
	{
		Datum		ctid = ItemPointerGetDatum(&slots[i]->tts_tid);
		bool		null = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &ctid, &null);
		ExecDropSingleTupleTableSlot(slots[i]);
	}
	table_close(source, AccessShareLock);
	table_close(target, NoLock);
	return (Datum) 0;
}

/*
 * dg_index_fetch_body fetches a table's row by its ctid through the fork's routine's index fetch, before and after a
 * reset, the second time without all_dead, and returns the first fetch's row as row_text writes it, or "none", then the
 * fetch's call_again and all_dead flags, refusing a second fetch that gives another answer.
 */
static Datum
dg_index_fetch_body(Relation rel, FunctionCallInfo fcinfo)
{
	ItemPointer tid = PG_GETARG_ITEMPOINTER(1);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &doltgres_slot_ops);
	IndexFetchTableData *fetch = doltgres_table_am.index_fetch_begin(rel, 0);
	bool		call_again = false;
	bool		all_dead = false;
	char	   *first;
	char	   *second;

	first = doltgres_table_am.index_fetch_tuple(fetch, tid, GetActiveSnapshot(), slot, &call_again, &all_dead) ?
		row_text(slot, RelationGetDescr(rel)) : "none";
	doltgres_table_am.index_fetch_reset(fetch);
	second = doltgres_table_am.index_fetch_tuple(fetch, tid, GetActiveSnapshot(), slot, &call_again, NULL) ?
		row_text(slot, RelationGetDescr(rel)) : "none";
	doltgres_table_am.index_fetch_end(fetch);
	ExecDropSingleTupleTableSlot(slot);
	if (strcmp(first, second) != 0)
		elog(ERROR, "an index fetch after a reset gave \"%s\" for \"%s\"", second, first);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%s %s %s", first, call_again ? "t" : "f", all_dead ? "t" : "f")));
}

/* dg_index_fetch is dg_index_fetch_body with the fork's routine standing as its table's own. */
Datum
dg_index_fetch(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_index_fetch_body);
}

/*
 * dg_scan_dirty_body reads a table through the fork's table access method routine under a Dirty snapshot, whose other
 * fields hold bytes a caller's stack may leave, since InitDirtySnapshot sets only its type, and returns each row as
 * row_text writes it, then the xmin and xmax the read stored in the snapshot for it.
 */
static Datum
dg_scan_dirty_body(Relation rel, FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	desc = RelationGetDescr(rel);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &doltgres_slot_ops);
	SnapshotData dirty;
	TableScanDesc scan;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	memset(&dirty, 0x7f, sizeof(dirty));
	InitDirtySnapshot(dirty);
	scan = doltgres_table_am.scan_begin(rel, &dirty, 0, NULL, NULL, SO_TYPE_SEQSCAN);
	while (doltgres_table_am.scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		Datum		line = CStringGetTextDatum(psprintf("%s %u %u", row_text(slot, desc), dirty.xmin, dirty.xmax));
		bool		null = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &line, &null);
	}
	doltgres_table_am.scan_end(scan);
	ExecDropSingleTupleTableSlot(slot);
	return (Datum) 0;
}

/* dg_scan_dirty is dg_scan_dirty_body with the fork's routine standing as its table's own. */
Datum
dg_scan_dirty(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_scan_dirty_body);
}

/*
 * dg_toast_body fetches a table's row by its ctid through the fork's table access method routine and returns a slice
 * of its column's value out of line, detoasted through the TOAST relation's routine, the value's id first replaced by
 * a given one unless that is InvalidOid.
 */
static Datum
dg_toast_body(Relation rel, FunctionCallInfo fcinfo)
{
	ItemPointer tid = PG_GETARG_ITEMPOINTER(1);
	int			attnum = PG_GETARG_INT32(2);
	int32		offset = PG_GETARG_INT32(3);
	int32		length = PG_GETARG_INT32(4);
	Oid			value = PG_GETARG_OID(5);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &doltgres_slot_ops);
	varatt_external pointer;
	varlena    *external = (varlena *) palloc(TOAST_POINTER_SIZE);
	bool		isnull;
	Datum		datum;

	if (!doltgres_table_am.tuple_fetch_row_version(rel, tid, GetActiveSnapshot(), slot))
		elog(ERROR, "no row has the ctid");
	datum = slot_getattr(slot, attnum, &isnull);
	if (isnull || !VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(datum)))
		elog(ERROR, "the column holds no value out of line");
	memcpy(external, DatumGetPointer(datum), TOAST_POINTER_SIZE);
	ExecDropSingleTupleTableSlot(slot);
	if (OidIsValid(value))
	{
		VARATT_EXTERNAL_GET_POINTER(pointer, external);
		pointer.va_valueid = value;
		memcpy(VARDATA_EXTERNAL(external), &pointer, sizeof(pointer));
	}
	PG_RETURN_TEXT_P(detoast_attr_slice(external, offset, length));
}

/* dg_toast is dg_toast_body with the fork's routine standing as its table's own and its TOAST relation's. */
Datum
dg_toast(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_toast_body);
}

/*
 * dg_index_fetch_key_body names each row of a heap table to an index fetch of a table through the fork's table access
 * method routine as the fork's index method names an entry's key, with a copy's tuple id, then fetches by row key, and
 * returns each fetched row as row_text writes it with the tid the fetch wrote, or "none".
 */
static Datum
dg_index_fetch_key_body(Relation rel, FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	source = table_open(PG_GETARG_OID(1), AccessShareLock);
	uint64		copy = (uint64) PG_GETARG_INT64(2);
	TupleTableSlot *heap = table_slot_create(source, NULL);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), &doltgres_slot_ops);
	TableScanDesc scan = table_beginscan(source, GetActiveSnapshot(), 0, NULL, 0);
	IndexFetchTableData *fetch = doltgres_table_am.index_fetch_begin(rel, 0);

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	while (table_scan_getnextslot(scan, ForwardScanDirection, heap))
	{
		ItemPointerData tid;
		bool		call_again = false;
		bool		all_dead = false;
		bool		null = false;
		char	   *row = "none";
		Datum		line;

		slot_getallattrs(heap);
		doltgres_test_index_fetch_key(fetch, rel, heap, copy);
		ItemPointerSet(&tid, InvalidBlockNumber, MaxOffsetNumber);
		if (doltgres_table_am.index_fetch_tuple(fetch, &tid, GetActiveSnapshot(), slot, &call_again, &all_dead))
			row = psprintf("%s (%u,%u)", row_text(slot, RelationGetDescr(rel)), ItemPointerGetBlockNumber(&tid),
						   ItemPointerGetOffsetNumber(&tid));
		line = CStringGetTextDatum(row);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &line, &null);
	}
	doltgres_table_am.index_fetch_end(fetch);
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	ExecDropSingleTupleTableSlot(heap);
	table_close(source, AccessShareLock);
	return (Datum) 0;
}

/* dg_index_fetch_key is dg_index_fetch_key_body with the fork's routine standing as its table's own. */
Datum
dg_index_fetch_key(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_index_fetch_key_body);
}

/*
 * dg_bitmap_body reads a table through the fork's table access method routine as a bitmap scan of a TID bitmap of
 * exact tids and lossy pages, and returns each row as row_text writes it with whether the scan asked for a recheck,
 * then the counts of lossy and exact pages it read.
 */
static Datum
dg_bitmap_body(Relation rel, FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	desc = RelationGetDescr(rel);
	TIDBitmap  *bitmap = tbm_create(work_mem * (Size) 1024, NULL);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &doltgres_slot_ops);
	TableScanDesc scan;
	Datum	   *elems;
	bool	   *nulls;
	int			count;
	bool		recheck = false;
	uint64		lossy = 0;
	uint64		exact = 0;
	Datum		line;
	bool		null = false;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(1), TIDOID, &elems, &nulls, &count);
	for (int i = 0; i < count; i++)
		tbm_add_tuples(bitmap, DatumGetItemPointer(elems[i]), 1, false);
	deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(2), INT4OID, &elems, &nulls, &count);
	for (int i = 0; i < count; i++)
		tbm_add_page(bitmap, (BlockNumber) DatumGetInt32(elems[i]));
	scan = doltgres_table_am.scan_begin(rel, GetActiveSnapshot(), 0, NULL, NULL, SO_TYPE_BITMAPSCAN | SO_ALLOW_PAGEMODE);
	scan->st.rs_tbmiterator = tbm_begin_iterate(bitmap, NULL, InvalidDsaPointer);
	while (doltgres_table_am.scan_bitmap_next_tuple(scan, slot, &recheck, &lossy, &exact))
	{
		line = CStringGetTextDatum(psprintf("%s %s", row_text(slot, desc), recheck ? "recheck" : "exact"));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &line, &null);
	}
	line = CStringGetTextDatum(psprintf("pages " UINT64_FORMAT " " UINT64_FORMAT, lossy, exact));
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &line, &null);
	tbm_end_iterate(&scan->st.rs_tbmiterator);
	doltgres_table_am.scan_end(scan);
	ExecDropSingleTupleTableSlot(slot);
	tbm_free(bitmap);
	return (Datum) 0;
}

/* dg_bitmap is dg_bitmap_body with the fork's routine standing as its table's own. */
Datum
dg_bitmap(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_bitmap_body);
}

/*
 * dg_standing_query_body runs a read-only query through SPI while the fork's table access method routine stands as a
 * table's own, so the executor reads the table through it, and returns each row's first column as text.
 */
static Datum
dg_standing_query_body(Relation rel, FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(1));

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	SPI_connect();
	if (SPI_execute(query, true, 0) != SPI_OK_SELECT)
		elog(ERROR, "the query is no SELECT");
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		char	   *value = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);
		Datum		line = CStringGetTextDatum(value != NULL ? value : "NULL");
		bool		null = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &line, &null);
	}
	SPI_finish();
	return (Datum) 0;
}

/* dg_standing_query is dg_standing_query_body with the fork's routine standing as its table's own. */
Datum
dg_standing_query(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_standing_query_body);
}

/* next_sampled returns the next block a block sampler picks, as analyze.c's block_sampling_read_stream_next does. */
static BlockNumber
next_sampled(ReadStream *stream, void *callback_private_data, void *per_buffer_data)
{
	BlockSampler sampler = (BlockSampler) callback_private_data;

	return BlockSampler_HasMore(sampler) ? BlockSampler_Next(sampler) : InvalidBlockNumber;
}

/*
 * dg_analyze_body samples a table as acquire_sample_rows does, a block sampler's blocks of the table's size through
 * a read stream and each block's rows through its table access method's analyze scan, and returns each sampled row's
 * first column, then the counts of live and dead rows.
 */
static Datum
dg_analyze_body(Relation rel, FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	BlockNumber blocks = (BlockNumber) (table_relation_size(rel, MAIN_FORKNUM) / BLCKSZ);
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	BlockSamplerData sampler;
	ReadStream *stream;
	TableScanDesc scan;
	double		live = 0;
	double		dead = 0;
	Oid			output;
	bool		is_varlena;
	Datum		line;
	bool		null = false;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	getTypeOutputInfo(TupleDescAttr(RelationGetDescr(rel), 0)->atttypid, &output, &is_varlena);
	BlockSampler_Init(&sampler, blocks, PG_GETARG_INT32(2), (uint32) PG_GETARG_INT32(3));
	stream = read_stream_begin_relation(READ_STREAM_MAINTENANCE | READ_STREAM_USE_BATCHING, NULL, rel, MAIN_FORKNUM,
										next_sampled, &sampler, 0);
	scan = table_beginscan_analyze(rel);
	while (table_scan_analyze_next_block(scan, stream))
	{
		while (table_scan_analyze_next_tuple(scan, &live, &dead, slot))
		{
			bool		isnull;
			Datum		value = slot_getattr(slot, 1, &isnull);

			line = CStringGetTextDatum(isnull ? "NULL" : OidOutputFunctionCall(output, value));
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &line, &null);
		}
	}
	table_endscan(scan);
	read_stream_end(stream);
	ExecDropSingleTupleTableSlot(slot);
	line = CStringGetTextDatum(psprintf("live %.0f dead %.0f", live, dead));
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &line, &null);
	return (Datum) 0;
}

/* dg_analyze is dg_analyze_body over a table, with the fork's routine standing as its own when asked to. */
Datum
dg_analyze(PG_FUNCTION_ARGS)
{
	Relation	rel;
	Datum		result;

	if (PG_GETARG_BOOL(1))
		return standing(fcinfo, dg_analyze_body);
	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	result = dg_analyze_body(rel, fcinfo);
	table_close(rel, AccessShareLock);
	return result;
}

/*
 * dg_write_body runs a query that writes through SPI, which deletes, updates, and locks a table's rows through the
 * table's own routine, and returns the count of rows the query processed.
 */
static Datum
dg_write_body(Relation rel, FunctionCallInfo fcinfo)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(2));
	int64		processed;

	SPI_connect();
	if (SPI_execute(query, false, 0) < 0)
		elog(ERROR, "the query failed");
	processed = (int64) SPI_processed;
	SPI_finish();
	PG_RETURN_INT64(processed);
}

/* dg_write is dg_write_body over a table, with the fork's routine standing as its own when asked to. */
Datum
dg_write(PG_FUNCTION_ARGS)
{
	Relation	rel;
	Datum		result;

	if (PG_GETARG_BOOL(1))
		return standing(fcinfo, dg_write_body);
	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	result = dg_write_body(rel, fcinfo);
	table_close(rel, AccessShareLock);
	return result;
}

/*
 * dg_delete_body deletes the row a tid names through the table's own routine's tuple_delete, as the executor calls it,
 * waiting for a running holder, and returns the TM_Result it answers.
 */
static Datum
dg_delete_body(Relation rel, FunctionCallInfo fcinfo)
{
	TM_FailureData tmfd;

	PG_RETURN_INT32(rel->rd_tableam->tuple_delete(rel, PG_GETARG_ITEMPOINTER(2), GetCurrentCommandId(true), 0,
												  GetActiveSnapshot(), InvalidSnapshot, true, &tmfd));
}

/* dg_delete is dg_delete_body over a table, with the fork's routine standing as its own when asked to. */
Datum
dg_delete(PG_FUNCTION_ARGS)
{
	Relation	rel;
	Datum		result;

	if (PG_GETARG_BOOL(1))
		return standing(fcinfo, dg_delete_body);
	rel = table_open(PG_GETARG_OID(0), RowExclusiveLock);
	result = dg_delete_body(rel, fcinfo);
	table_close(rel, RowExclusiveLock);
	return result;
}

/*
 * dg_scan_nonvacuumable_body reads a table through the fork's table access method routine under a NonVacuumable
 * snapshot of the relation's horizon, as VACUUM and index cleanup read, and returns each row as row_text writes it.
 */
static Datum
dg_scan_nonvacuumable_body(Relation rel, FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	desc = RelationGetDescr(rel);
	TupleTableSlot *slot = MakeSingleTupleTableSlot(desc, &doltgres_slot_ops);
	SnapshotData nonvacuumable;
	TableScanDesc scan;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	InitNonVacuumableSnapshot(nonvacuumable, GlobalVisTestFor(rel));
	scan = doltgres_table_am.scan_begin(rel, &nonvacuumable, 0, NULL, NULL, SO_TYPE_SEQSCAN);
	while (doltgres_table_am.scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		Datum		line = CStringGetTextDatum(row_text(slot, desc));
		bool		null = false;

		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &line, &null);
	}
	doltgres_table_am.scan_end(scan);
	ExecDropSingleTupleTableSlot(slot);
	return (Datum) 0;
}

/* dg_scan_nonvacuumable is dg_scan_nonvacuumable_body with the fork's routine standing as its table's own. */
Datum
dg_scan_nonvacuumable(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_scan_nonvacuumable_body);
}

/*
 * dg_speculative_body inserts a row of an int4 key and a text value speculatively through the table's own routine,
 * as ExecInsert does for INSERT ... ON CONFLICT, under its transaction's speculative insertion lock, inserts its
 * entries into the table's indexes through their routines when asked to, completes it as succeeded or not unless
 * asked to leave it running, its lock held to the transaction's end, and returns the ctid the insert gave the row.
 */
static Datum
dg_speculative_body(Relation rel, FunctionCallInfo fcinfo)
{
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	TransactionId xid = GetCurrentTransactionId();
	uint32		token = SpeculativeInsertionLockAcquire(xid);
	char	   *tid;

	ExecClearTuple(slot);
	slot->tts_values[0] = Int32GetDatum(PG_GETARG_INT32(2));
	slot->tts_isnull[0] = false;
	slot->tts_values[1] = PointerGetDatum(PG_GETARG_TEXT_PP(3));
	slot->tts_isnull[1] = false;
	ExecStoreVirtualTuple(slot);
	rel->rd_tableam->tuple_insert_speculative(rel, slot, GetCurrentCommandId(true), 0, NULL, token);
	if (PG_GETARG_BOOL(6))
	{
		EState	   *estate = CreateExecutorState();
		ResultRelInfo *info = makeNode(ResultRelInfo);
		bool		conflict = false;

		InitResultRelInfo(info, rel, 1, NULL, 0);
		ExecOpenIndices(info, true);
		list_free(ExecInsertIndexTuples(info, estate, EIIT_NO_DUPE_ERROR, slot, NIL, &conflict));
		ExecCloseIndices(info);
		FreeExecutorState(estate);
	}
	if (PG_GETARG_BOOL(5))
	{
		rel->rd_tableam->tuple_complete_speculative(rel, slot, token, PG_GETARG_BOOL(4));
		SpeculativeInsertionLockRelease(xid);
	}
	tid = psprintf("(%u,%u)", ItemPointerGetBlockNumber(&slot->tts_tid), ItemPointerGetOffsetNumber(&slot->tts_tid));
	ExecDropSingleTupleTableSlot(slot);
	PG_RETURN_TEXT_P(cstring_to_text(tid));
}

/* dg_speculative is dg_speculative_body over a table, with the fork's routine standing as its own when asked to. */
Datum
dg_speculative(PG_FUNCTION_ARGS)
{
	Relation	rel;
	Datum		result;

	if (PG_GETARG_BOOL(1))
		return standing(fcinfo, dg_speculative_body);
	rel = table_open(PG_GETARG_OID(0), RowExclusiveLock);
	result = dg_speculative_body(rel, fcinfo);
	table_close(rel, RowExclusiveLock);
	return result;
}

/*
 * dg_index_delete_body asks the table's own routine which of a list of its tuple ids an index may delete, as an
 * index's simple deletion asks when the space target is negative and a bottom-up deletion with that target asks
 * otherwise, each entry known deletable and promising as given and freeing 16 bytes, and returns the entries the
 * routine kept, in its order, each marked when deletable, and the conflict horizon.
 */
static Datum
dg_index_delete_body(Relation rel, FunctionCallInfo fcinfo)
{
	int32		target = PG_GETARG_INT32(5);
	Datum	   *tids;
	Datum	   *known;
	Datum	   *promising;
	int			count;
	TM_IndexDeleteOp op = {.irel = rel, .iblknum = InvalidBlockNumber, .bottomup = target >= 0, .bottomupfreespace = Max(target, 0)};
	TransactionId horizon;
	StringInfoData out;

	deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(2), TIDOID, &tids, NULL, &count);
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(3), BOOLOID, 1, true, TYPALIGN_CHAR, &known, NULL, &count);
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(4), BOOLOID, 1, true, TYPALIGN_CHAR, &promising, NULL, &count);
	op.ndeltids = count;
	op.deltids = palloc_array(TM_IndexDelete, count);
	op.status = palloc0_array(TM_IndexStatus, count);
	for (int i = 0; i < count; i++)
	{
		op.deltids[i].tid = *DatumGetItemPointer(tids[i]);
		op.deltids[i].id = i;
		op.status[i].idxoffnum = i + 1;
		op.status[i].knowndeletable = DatumGetBool(known[i]);
		op.status[i].promising = DatumGetBool(promising[i]);
		op.status[i].freespace = 16;
	}
	horizon = table_index_delete_tuples(rel, &op);
	initStringInfo(&out);
	for (int i = 0; i < op.ndeltids; i++)
		appendStringInfo(&out, "(%u,%u)%s ", ItemPointerGetBlockNumber(&op.deltids[i].tid),
						 ItemPointerGetOffsetNumber(&op.deltids[i].tid),
						 op.status[op.deltids[i].id].knowndeletable ? " deletable" : "");
	appendStringInfo(&out, "horizon %u", horizon);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}

/* dg_index_delete is dg_index_delete_body over a table, with the fork's routine standing as its own when asked to. */
Datum
dg_index_delete(PG_FUNCTION_ARGS)
{
	Relation	rel;
	Datum		result;

	if (PG_GETARG_BOOL(1))
		return standing(fcinfo, dg_index_delete_body);
	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	result = dg_index_delete_body(rel, fcinfo);
	table_close(rel, AccessShareLock);
	return result;
}

/*
 * dg_round is an injection point callback that counts the times the process its private data names, by its process
 * id in network byte order, reaches the point and, at each, waits for the session advisory lock of that count, which a
 * test holds to keep the process there until it unlocks it; any other process passes.
 */
PGDLLEXPORT void dg_round(const char *name, const void *private_data, void *arg);

void
dg_round(const char *name, const void *private_data, void *arg)
{
	static int64 round = 0;
	LOCKTAG		tag;
	uint32		pid;

	memcpy(&pid, private_data, sizeof(pid));
	if (pg_ntoh32(pid) != (uint32) MyProcPid)
		return;
	round++;
	SET_LOCKTAG_ADVISORY(tag, MyDatabaseId, (uint32) (round >> 32), (uint32) round, 1);
	(void) LockAcquire(&tag, ShareLock, false, false);
	LockRelease(&tag, ShareLock, false);
}

/* dg_fold folds a table's storage now, as doltgres_test_fold, and returns whether it installed a base. */
Datum
dg_fold(PG_FUNCTION_ARGS)
{
	Relation	target = table_open(PG_GETARG_OID(0), AccessShareLock);
	bool		installed = doltgres_test_fold(target);

	table_close(target, AccessShareLock);
	PG_RETURN_BOOL(installed);
}

/*
 * dg_index_routine gives an index the fork's routine as its relcache build will, and returns whether the fork's
 * copy took the place of the routine its handler returned.
 */
Datum
dg_index_routine(PG_FUNCTION_ARGS)
{
	Relation	rel = index_open(PG_GETARG_OID(0), AccessShareLock);
	const IndexAmRoutine *before = rel->rd_indam;
	bool		replaced;

	doltgres_index_routine(rel);
	replaced = rel->rd_indam != before;
	index_close(rel, AccessShareLock);
	PG_RETURN_BOOL(replaced);
}

/*
 * dg_vacuum runs the fork's relation_vacuum over a table as VACUUM calls it, under VACUUM's lock, with INDEX_CLEANUP
 * on, off, or auto for any other text.
 */
Datum
dg_vacuum(PG_FUNCTION_ARGS)
{
	char	   *cleanup = text_to_cstring(PG_GETARG_TEXT_PP(1));
	VacuumParams params = {
		.options = VACOPT_VACUUM,
		.index_cleanup = strcmp(cleanup, "on") == 0 ? VACOPTVALUE_ENABLED :
		strcmp(cleanup, "off") == 0 ? VACOPTVALUE_DISABLED : VACOPTVALUE_AUTO,
		.truncate = VACOPTVALUE_AUTO,
	};
	Relation	rel = table_open(PG_GETARG_OID(0), ShareUpdateExclusiveLock);

	doltgres_table_am.relation_vacuum(rel, &params, NULL);
	table_close(rel, ShareUpdateExclusiveLock);
	PG_RETURN_VOID();
}

/*
 * dg_free_space returns a block's newest free space entry in a fork table's delta, as doltgres_test_free_space reads
 * it.
 */
Datum
dg_free_space(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	Datum		entry = doltgres_test_free_space(rel, PG_GETARG_INT32(1));

	table_close(rel, AccessShareLock);
	PG_RETURN_DATUM(entry);
}

/*
 * dg_base_sizes returns a fork table's newest base's sizes, fresh end, and counter, as doltgres_test_base_sizes reads
 * them.
 */
Datum
dg_base_sizes(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	Datum		sizes = doltgres_test_base_sizes(rel);

	table_close(rel, AccessShareLock);
	PG_RETURN_DATUM(sizes);
}

/*
 * dg_index_entries returns the versions an index's storage's delta holds, as doltgres_test_index_entries lists them.
 */
Datum
dg_index_entries(PG_FUNCTION_ARGS)
{
	Relation	rel = index_open(PG_GETARG_OID(0), AccessShareLock);
	Datum		entries = doltgres_test_index_entries(rel);

	index_close(rel, AccessShareLock);
	PG_RETURN_DATUM(entries);
}

/*
 * dg_index_buildempty gives an index the fork's routine as its relcache build will, and makes its unlogged storage
 * through the routine's ambuildempty, as index_build calls it once the index's init fork is made.
 */
Datum
dg_index_buildempty(PG_FUNCTION_ARGS)
{
	Relation	rel = index_open(PG_GETARG_OID(0), AccessExclusiveLock);

	doltgres_index_routine(rel);
	rel->rd_indam->ambuildempty(rel);
	index_close(rel, AccessExclusiveLock);
	PG_RETURN_VOID();
}

/*
 * dg_held reports whether a row version in a fork table's delta holds a ctid, as doltgres_test_held reads it.
 */
Datum
dg_held(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	ItemPointer tid = PG_GETARG_ITEMPOINTER(1);
	bool		held = doltgres_test_held(rel, (uint64) ItemPointerGetBlockNumber(tid) << 16 | ItemPointerGetOffsetNumber(tid));

	table_close(rel, AccessShareLock);
	PG_RETURN_BOOL(held);
}

/*
 * dg_index_build_body gives an index of a table the fork's routine as its relcache build will and builds it through
 * the routine's ambuild, as index_build calls it, under CREATE INDEX's progress command with index_build's first
 * report, and returns the build's counts of the table's live rows and of its entries, and whether it marked the
 * build's IndexInfo as having a broken chain.
 */
static Datum
dg_index_build_body(Relation rel, FunctionCallInfo fcinfo)
{
	Relation	index = index_open(PG_GETARG_OID(1), AccessExclusiveLock);
	IndexInfo  *info = BuildIndexInfo(index);
	IndexBuildResult *result;
	const int	progress_index[] = {
		PROGRESS_CREATEIDX_COMMAND,
		PROGRESS_CREATEIDX_INDEX_OID,
		PROGRESS_CREATEIDX_ACCESS_METHOD_OID,
		PROGRESS_CREATEIDX_PHASE,
		PROGRESS_CREATEIDX_SUBPHASE,
		PROGRESS_CREATEIDX_TUPLES_DONE,
		PROGRESS_CREATEIDX_TUPLES_TOTAL,
		PROGRESS_SCAN_BLOCKS_DONE,
		PROGRESS_SCAN_BLOCKS_TOTAL
	};
	const int64 progress_vals[] = {
		PROGRESS_CREATEIDX_COMMAND_CREATE,
		RelationGetRelid(index),
		index->rd_rel->relam,
		PROGRESS_CREATEIDX_PHASE_BUILD,
		PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE,
		0, 0, 0, 0
	};

	doltgres_index_routine(index);
	pgstat_progress_start_command(PROGRESS_COMMAND_CREATE_INDEX, RelationGetRelid(rel));
	pgstat_progress_update_multi_param(lengthof(progress_index), progress_index, progress_vals);
	result = index->rd_indam->ambuild(rel, index, info);
	pgstat_progress_end_command();
	index_close(index, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%.0f %.0f %s", result->heap_tuples, result->index_tuples,
											  info->ii_BrokenHotChain ? "broken" : "whole")));
}

/* dg_index_build is dg_index_build_body over a table, with the fork's routine standing as its own. */
Datum
dg_index_build(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_index_build_body);
}

/* dg_index_base returns the entries an index's storage's base holds, as doltgres_test_index_base lists them. */
Datum
dg_index_base(PG_FUNCTION_ARGS)
{
	Relation	rel = index_open(PG_GETARG_OID(0), AccessShareLock);
	Datum		entries = doltgres_test_index_base(rel);

	index_close(rel, AccessShareLock);
	PG_RETURN_DATUM(entries);
}

/* dg_refuse_dead fails a bulk delete that asks whether a tid is dead, which no fork index's ever asks. */
static bool
dg_refuse_dead(ItemPointer itemptr, void *state)
{
	elog(ERROR, "a fork index's bulk delete asked whether a tid is dead");
	return false;
}

/*
 * dg_index_vacuumed gives an index the fork's routine as its relcache build will and runs its bulk delete and then its
 * cleanup as VACUUM's index pass does, and returns the entries the bulk delete removed and the cleanup's pages and
 * tuples.
 */
Datum
dg_index_vacuumed(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), RowExclusiveLock);
	IndexVacuumInfo info = {.index = index, .message_level = DEBUG2, .estimated_count = true};
	IndexBulkDeleteResult *stats;

	doltgres_index_routine(index);
	info.heaprel = table_open(index->rd_index->indrelid, AccessShareLock);
	stats = index->rd_indam->ambulkdelete(&info, NULL, dg_refuse_dead, NULL);
	stats = index->rd_indam->amvacuumcleanup(&info, stats);
	table_close(info.heaprel, AccessShareLock);
	index_close(index, RowExclusiveLock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%.0f %u %.0f", stats->tuples_removed, stats->num_pages,
											  stats->num_index_tuples)));
}

/* dg_index_fold folds an index's storage now and returns its base's entries, as doltgres_test_index_fold lists them. */
Datum
dg_index_fold(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	Datum		entries = doltgres_test_index_fold(heap, index);

	table_close(heap, AccessShareLock);
	index_close(index, AccessShareLock);
	PG_RETURN_DATUM(entries);
}

/*
 * dg_index_validate_body runs a concurrent index build's validation of an index of a table, as CREATE INDEX
 * CONCURRENTLY's third phase does, under a snapshot of its own.
 */
static Datum
dg_index_validate_body(Relation rel, FunctionCallInfo fcinfo)
{
	Snapshot	snapshot = RegisterSnapshot(GetTransactionSnapshot());

	validate_index(RelationGetRelid(rel), PG_GETARG_OID(1), snapshot);
	UnregisterSnapshot(snapshot);
	PG_RETURN_VOID();
}

/* dg_index_validate is dg_index_validate_body over a table, with the fork's routine standing as its own. */
Datum
dg_index_validate(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_index_validate_body);
}

/*
 * dg_collect runs one full collection of the session's database's repository and returns its counts, as
 * doltgres_test_collect reads them.
 */
Datum
dg_collect(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(doltgres_test_collect());
}

/*
 * dg_disk_space returns the free bytes and the size of the data directory's volume, as doltgres_test_disk_space reads
 * them.
 */
Datum
dg_disk_space(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(doltgres_test_disk_space());
}

/*
 * dg_database_lock takes or gives back the lock the collector worker takes on a
 * database, as doltgres_test_database_lock does.
 */
Datum
dg_database_lock(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(doltgres_test_database_lock(PG_GETARG_OID(0), PG_GETARG_BOOL(1)));
}

/* dg_new_file_body gives a table a new relfilenode through its routine, as a transactional TRUNCATE does. */
static Datum
dg_new_file_body(Relation rel, FunctionCallInfo fcinfo)
{
	RelationSetNewRelfilenumber(rel, rel->rd_rel->relpersistence);
	CommandCounterIncrement();
	PG_RETURN_VOID();
}

/* dg_new_file is dg_new_file_body with the fork's routine standing as its table's own. */
Datum
dg_new_file(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_new_file_body);
}

/* dg_truncate_body empties a table its transaction gave its relfilenode, as TRUNCATE does such a table in place. */
static Datum
dg_truncate_body(Relation rel, FunctionCallInfo fcinfo)
{
	table_relation_nontransactional_truncate(rel);
	PG_RETURN_VOID();
}

/* dg_truncate is dg_truncate_body with the fork's routine standing as its table's own. */
Datum
dg_truncate(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_truncate_body);
}

/*
 * dg_rewrite_body copies a table into a new relfilenode as CLUSTER and VACUUM FULL do (repack.c's rebuild_relation and
 * copy_table_data), on an index or on none, with CLUSTER's sort or without, through the table's own routine, sets the
 * new relfilenode's page and tuple counts, swaps the files, and returns the copy's counts of the tuples it kept,
 * vacuumed, and found recently dead.
 */
static Datum
dg_rewrite_body(Relation rel, FunctionCallInfo fcinfo)
{
	Oid			table = RelationGetRelid(rel);
	char		persistence = rel->rd_rel->relpersistence;
	Relation	index = PG_ARGISNULL(1) ? NULL : index_open(PG_GETARG_OID(1), AccessExclusiveLock);
	Oid			made;
	Relation	new_rel;
	Relation	classes;
	HeapTuple	tuple;
	VacuumParams params;
	struct VacuumCutoffs cutoffs;
	bool		by_content;
	double		tuples = 0;
	double		vacuumed = 0;
	double		dead = 0;

	LockRelationOid(table, AccessExclusiveLock);
	if (OidIsValid(rel->rd_rel->reltoastrelid))
		LockRelationOid(rel->rd_rel->reltoastrelid, AccessExclusiveLock);
	made = make_new_heap(table, rel->rd_rel->reltablespace, rel->rd_rel->relam, persistence, NoLock);
	new_rel = table_open(made, NoLock);
	by_content = OidIsValid(rel->rd_rel->reltoastrelid) && OidIsValid(new_rel->rd_rel->reltoastrelid);
	if (by_content)
		new_rel->rd_toastoid = rel->rd_rel->reltoastrelid;
	memset(&params, 0, sizeof(params));
	vacuum_get_cutoffs(rel, &params, &cutoffs);
	if (TransactionIdIsValid(rel->rd_rel->relfrozenxid) &&
		TransactionIdPrecedes(cutoffs.FreezeLimit, rel->rd_rel->relfrozenxid))
		cutoffs.FreezeLimit = rel->rd_rel->relfrozenxid;
	if (MultiXactIdIsValid(rel->rd_rel->relminmxid) &&
		MultiXactIdPrecedes(cutoffs.MultiXactCutoff, rel->rd_rel->relminmxid))
		cutoffs.MultiXactCutoff = rel->rd_rel->relminmxid;
	table_relation_copy_for_cluster(rel, new_rel, index, PG_GETARG_BOOL(2), cutoffs.OldestXmin, NULL,
									&cutoffs.FreezeLimit, &cutoffs.MultiXactCutoff, &tuples, &vacuumed, &dead);
	new_rel->rd_toastoid = InvalidOid;
	new_rel->rd_tableam = rel->rd_tableam;
	classes = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(made));
	((Form_pg_class) GETSTRUCT(tuple))->relpages = RelationGetNumberOfBlocks(new_rel);
	((Form_pg_class) GETSTRUCT(tuple))->reltuples = tuples;
	CatalogTupleUpdate(classes, &tuple->t_self, tuple);
	heap_freetuple(tuple);
	table_close(classes, RowExclusiveLock);
	table_close(new_rel, NoLock);
	if (index != NULL)
		index_close(index, NoLock);
	CommandCounterIncrement();
	finish_heap_swap(table, made, false, by_content, false, true, true, cutoffs.FreezeLimit, cutoffs.MultiXactCutoff,
					 persistence);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("%.0f %.0f %.0f", tuples, vacuumed, dead)));
}

/* dg_rewrite is dg_rewrite_body over a table, with the fork's routine standing as its own when asked to. */
Datum
dg_rewrite(PG_FUNCTION_ARGS)
{
	Relation	rel;
	Datum		result;

	if (PG_GETARG_BOOL(3))
		return standing(fcinfo, dg_rewrite_body);
	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	result = dg_rewrite_body(rel, fcinfo);
	table_close(rel, NoLock);
	return result;
}

/*
 * dg_changes returns a fork table's delta's row versions as each key's ops, as doltgres_test_changes reads them.
 */
Datum
dg_changes(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	Datum		changes = doltgres_test_changes(rel);

	table_close(rel, AccessShareLock);
	PG_RETURN_DATUM(changes);
}

/*
 * dg_built_nodes returns how many built bases' nodes the working states' deltas
 * hold, as doltgres_test_built_nodes reads them.
 */
Datum
dg_built_nodes(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(doltgres_test_built_nodes());
}

/*
 * moved moves a relation's storage into a tablespace as ATExecSetTableSpace does: a new relfilenode there, the copy
 * through the relation's own routine, and its pg_class row.
 */
static void
moved(Relation rel, Oid space)
{
	RelFileNumber number = GetNewRelFileNumber(space, NULL, rel->rd_rel->relpersistence);
	RelFileLocator locator = rel->rd_locator;

	locator.relNumber = number;
	locator.spcOid = space;
	table_relation_copy_data(rel, &locator);
	SetRelationTableSpace(rel, space, number);
	RelationAssumeNewRelfilelocator(rel);
}

/*
 * dg_move_body moves a table into a tablespace as ALTER TABLE SET TABLESPACE does, the table and then its TOAST
 * relation, in one command.
 */
static Datum
dg_move_body(Relation rel, FunctionCallInfo fcinfo)
{
	Oid			space = get_tablespace_oid(text_to_cstring(PG_GETARG_TEXT_PP(1)), false);
	Oid			toast = rel->rd_rel->reltoastrelid;

	LockRelationOid(RelationGetRelid(rel), AccessExclusiveLock);
	if (OidIsValid(toast))
		LockRelationOid(toast, AccessExclusiveLock);
	moved(rel, space);
	CommandCounterIncrement();
	if (OidIsValid(toast))
	{
		Relation	toast_rel = table_open(toast, NoLock);

		moved(toast_rel, space);
		table_close(toast_rel, NoLock);
		CommandCounterIncrement();
	}
	PG_RETURN_VOID();
}

/* dg_move is dg_move_body with the fork's routine standing as its table's own. */
Datum
dg_move(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_move_body);
}

/*
 * dg_mapped says whether a fork table's storage's delta holds a version in any
 * of its maps, as doltgres_test_mapped reads it.
 */
Datum
dg_mapped(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	bool		mapped = doltgres_test_mapped(rel);

	table_close(rel, AccessShareLock);
	PG_RETURN_BOOL(mapped);
}

/*
 * dg_identity returns the id the database's identity tree pairs with an object's number, as doltgres_test_identity
 * reads it.
 */
Datum
dg_identity(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(doltgres_test_identity(PG_GETARG_INT32(0), PG_GETARG_OID(1), PG_GETARG_INT32(2)));
}

/*
 * dg_object_access is an object access hook that calls no hook before it, as
 * an extension that replaces the hook without chaining it does.
 */
static void
dg_object_access(ObjectAccessType access, Oid classId, Oid objectId, int subId, void *arg)
{
}

/*
 * dg_object_access_replaced makes dg_object_access the backend's object access
 * hook, dropping the one before it, for the rest of the session.
 */
Datum
dg_object_access_replaced(PG_FUNCTION_ARGS)
{
	object_access_hook = dg_object_access;
	PG_RETURN_VOID();
}

/*
 * dg_epochs returns the oldest epoch a live reader started at and the current
 * epoch, as doltgres_test_epochs reads them.
 */
Datum
dg_epochs(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(doltgres_test_epochs());
}

/*
 * dg_index_tree_height gives an index the fork's routine as its relcache build will and answers its routine's
 * amgettreeheight, as the planner's get_relation_info asks it.
 */
Datum
dg_index_tree_height(PG_FUNCTION_ARGS)
{
	Relation	rel = index_open(PG_GETARG_OID(0), AccessShareLock);
	int			height;

	doltgres_index_routine(rel);
	height = rel->rd_indam->amgettreeheight(rel);
	index_close(rel, AccessShareLock);
	PG_RETURN_INT32(height);
}

/*
 * dg_index_parallel_body runs one parallel scan of a fork btree index, every row in its order, with a number of
 * participants in this backend, each in turn reading its next row until all are done, as a parallel scan's workers
 * read their units, and returns each row a participant read as the participant's number and the row's first column.
 */
static Datum
dg_index_parallel_body(Relation rel, FunctionCallInfo fcinfo)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(1), AccessShareLock);
	int			participants = PG_GETARG_INT32(2);
	ScanDirection direction = PG_GETARG_BOOL(3) ? BackwardScanDirection : ForwardScanDirection;
	Snapshot	snapshot = GetActiveSnapshot();
	ParallelIndexScanDesc pscan;
	IndexScanDesc *scans = palloc_array(IndexScanDesc, participants);
	bool	   *done = palloc0_array(bool, participants);
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	int			left = participants;

	InitMaterializedSRF(fcinfo, MAT_SRF_USE_EXPECTED_DESC);
	doltgres_index_routine(index);
	pscan = palloc0(index_parallelscan_estimate(index, 0, 0, snapshot));
	index_parallelscan_initialize(rel, index, snapshot, pscan);
	for (int i = 0; i < participants; i++)
	{
		scans[i] = index_beginscan_parallel(rel, index, NULL, 0, 0, pscan, 0);
		index_rescan(scans[i], NULL, 0, NULL, 0);
	}
	while (left > 0)
	{
		for (int i = 0; i < participants; i++)
		{
			bool		isnull;
			Datum		first;
			Datum		line;
			bool		null = false;

			if (done[i])
				continue;
			if (!index_getnext_slot(scans[i], direction, slot))
			{
				done[i] = true;
				left--;
				continue;
			}
			first = slot_getattr(slot, 1, &isnull);
			line = CStringGetTextDatum(psprintf("%d:%s", i, isnull ? "NULL" :
												OidOutputFunctionCall(TupleDescAttr(RelationGetDescr(rel), 0)->atttypid == INT4OID ? F_INT4OUT : F_TEXTOUT, first)));
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, &line, &null);
		}
	}
	for (int i = 0; i < participants; i++)
		index_endscan(scans[i]);
	ExecDropSingleTupleTableSlot(slot);
	index_close(index, AccessShareLock);
	return (Datum) 0;
}

/* dg_index_parallel is dg_index_parallel_body with the fork's routine standing as its table's own. */
Datum
dg_index_parallel(PG_FUNCTION_ARGS)
{
	return standing(fcinfo, dg_index_parallel_body);
}

/* dg_members are IndexAmRoutine's callbacks, each with its place in the routine. */
static const struct
{
	const char *name;
	size_t		at;
}			dg_members[] = {
#define DG_MEMBER(member) {#member, offsetof(IndexAmRoutine, member)}
	DG_MEMBER(ambuild), DG_MEMBER(ambuildempty), DG_MEMBER(aminsert), DG_MEMBER(aminsertcleanup),
	DG_MEMBER(ambulkdelete), DG_MEMBER(amvacuumcleanup), DG_MEMBER(amcanreturn), DG_MEMBER(amcostestimate),
	DG_MEMBER(amgettreeheight), DG_MEMBER(amoptions), DG_MEMBER(amproperty), DG_MEMBER(ambuildphasename),
	DG_MEMBER(amvalidate), DG_MEMBER(amadjustmembers), DG_MEMBER(ambeginscan), DG_MEMBER(amrescan),
	DG_MEMBER(amgettuple), DG_MEMBER(amgetbitmap), DG_MEMBER(amendscan), DG_MEMBER(ammarkpos),
	DG_MEMBER(amrestrpos), DG_MEMBER(amestimateparallelscan), DG_MEMBER(aminitparallelscan),
	DG_MEMBER(amparallelrescan), DG_MEMBER(amtranslatestrategy), DG_MEMBER(amtranslatecmptype),
#undef DG_MEMBER
};

/*
 * dg_index_members gives an index the fork's routine as its relcache build will and reads it member by member against
 * the routine bthandler returns: the callbacks it replaced, those it kept, its `ampredlocks`, and whether every other
 * flag and number before the callbacks is the same.
 */
Datum
dg_index_members(PG_FUNCTION_ARGS)
{
	Relation	rel = index_open(PG_GETARG_OID(0), AccessShareLock);
	IndexAmRoutine bt = *GetIndexAmRoutine(F_BTHANDLER);
	StringInfoData replaced;
	StringInfoData kept;
	bool		predlocks;
	bool		same;

	StaticAssertStmt(lengthof(dg_members) == 26, "IndexAmRoutine's callbacks changed");
	doltgres_index_routine(rel);
	initStringInfo(&replaced);
	initStringInfo(&kept);
	for (int i = 0; i < lengthof(dg_members); i++)
	{
		bool		differs = memcmp((char *) rel->rd_indam + dg_members[i].at, (char *) &bt + dg_members[i].at,
									 sizeof(void (*) (void))) != 0;

		appendStringInfo(differs ? &replaced : &kept, "%s%s", (differs ? replaced.len : kept.len) > 0 ? "," : "",
						 dg_members[i].name);
	}
	predlocks = rel->rd_indam->ampredlocks;
	bt.ampredlocks = predlocks;
	same = memcmp(rel->rd_indam, &bt, offsetof(IndexAmRoutine, ambuild)) == 0;
	index_close(rel, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("replaced=%s kept=%s ampredlocks=%s same=%s", replaced.data,
											  kept.data, predlocks ? "t" : "f", same ? "t" : "f")));
}

/*
 * dg_base_levels returns the levels above the leaves of a relation's newest base tree, as doltgres_test_base_levels
 * walks it.
 */
Datum
dg_base_levels(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(PG_GETARG_OID(0), AccessShareLock);
	int			levels = doltgres_test_base_levels(rel);

	relation_close(rel, AccessShareLock);
	PG_RETURN_INT32(levels);
}

/* dg_prior_access_hook is the object access hook the watcher of rewrites runs first. */
static object_access_hook_type dg_prior_access_hook = NULL;

/* dg_seen is what the watcher saw of the relations made for a rewrite, one line each, in TopMemoryContext. */
static StringInfo dg_seen = NULL;

/* dg_see appends a relation made for a rewrite: when it was seen, its kind, and whose code its routine is. */
static void
dg_see(const char *when, Relation rel)
{
	bool		postgres = rel->rd_rel->relkind == RELKIND_INDEX ?
		rel->rd_indam->ambuild == GetIndexAmRoutine(rel->rd_amhandler)->ambuild :
		rel->rd_tableam == GetHeapamTableAmRoutine();

	appendStringInfo(dg_seen, "%s %c %s\n", when, rel->rd_rel->relkind, postgres ? "postgres" : "fork");
}

/*
 * dg_watch_access sees each relation whose pg_class row says it was made to rewrite another, when it is made and
 * when its files are swapped, with its TOAST table and that table's indexes at the swap.
 */
static void
dg_watch_access(ObjectAccessType access, Oid classId, Oid objectId, int subId, void *arg)
{
	Relation	rel;

	if (dg_prior_access_hook != NULL)
		dg_prior_access_hook(access, classId, objectId, subId, arg);
	if ((access != OAT_POST_CREATE && access != OAT_POST_ALTER) || classId != RelationRelationId || subId != 0)
		return;
	rel = RelationIdGetRelation(objectId);
	if (RelationIsValid(rel) && OidIsValid(rel->rd_rel->relrewrite))
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		dg_see(access == OAT_POST_CREATE ? "made" : "swapped", rel);
		if (access == OAT_POST_ALTER && OidIsValid(rel->rd_rel->reltoastrelid))
		{
			Relation	toast = RelationIdGetRelation(rel->rd_rel->reltoastrelid);
			ListCell   *cell;

			dg_see("swapped", toast);
			foreach(cell, RelationGetIndexList(toast))
			{
				Relation	index = RelationIdGetRelation(lfirst_oid(cell));

				dg_see("swapped", index);
				RelationClose(index);
			}
			RelationClose(toast);
		}
		MemoryContextSwitchTo(old);
	}
	if (RelationIsValid(rel))
		RelationClose(rel);
}

/* dg_watch_rewrites starts the backend's watcher of the relations made for a rewrite, forgetting what it saw. */
Datum
dg_watch_rewrites(PG_FUNCTION_ARGS)
{
	if (dg_seen == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

		dg_seen = makeStringInfo();
		MemoryContextSwitchTo(old);
		dg_prior_access_hook = object_access_hook;
		object_access_hook = dg_watch_access;
	}
	resetStringInfo(dg_seen);
	PG_RETURN_VOID();
}

/* dg_rewrites_seen returns what the watcher saw since it started or was last asked, forgetting it. */
Datum
dg_rewrites_seen(PG_FUNCTION_ARGS)
{
	text	   *seen;

	if (dg_seen == NULL)
		elog(ERROR, "the watcher of rewrites was not started");
	seen = cstring_to_text_with_len(dg_seen->data, dg_seen->len);
	resetStringInfo(dg_seen);
	PG_RETURN_TEXT_P(seen);
}

/*
 * dg_twin_handler is the handler of the heap twins' access method, doltgres_twin: it returns heap's routine, and the
 * fork, in a build with injection points, keeps a relation whose method's handler is this function off its storage,
 * so the fork's tests can compare its storage with heap's.
 */
Datum
dg_twin_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(GetHeapamTableAmRoutine());
}

/* dg_totals returns a fork table's storage's totals as its state holds them, as doltgres_test_totals reads them. */
Datum
dg_totals(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	Datum		totals = doltgres_test_totals(rel);

	table_close(rel, AccessShareLock);
	PG_RETURN_DATUM(totals);
}

/* dg_count returns a fresh count of a fork table's storage's totals, as doltgres_test_count makes it. */
Datum
dg_count(PG_FUNCTION_ARGS)
{
	Relation	rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	Datum		count = doltgres_test_count(rel);

	table_close(rel, AccessShareLock);
	PG_RETURN_DATUM(count);
}

/*
 * dg_drop_constraint drops a table's constraint through performDeletion, as a C extension does, outside any utility
 * statement.
 */
Datum
dg_drop_constraint(PG_FUNCTION_ARGS)
{
	ObjectAddress object;

	ObjectAddressSet(object, ConstraintRelationId,
					 get_relation_constraint_oid(PG_GETARG_OID(0), text_to_cstring(PG_GETARG_TEXT_PP(1)), false));
	performDeletion(&object, DROP_RESTRICT, 0);
	PG_RETURN_VOID();
}

/*
 * unchained_utility runs a utility statement through standard_ProcessUtility
 * and calls no earlier hook, as an extension's hook that doesn't chain does.
 */
static void
unchained_utility(PlannedStmt *pstmt, const char *queryString, bool readOnlyTree, ProcessUtilityContext context,
				  ParamListInfo params, QueryEnvironment *queryEnv, DestReceiver *dest, QueryCompletion *qc)
{
	standard_ProcessUtility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
}

/* dg_unchain makes unchained_utility the backend's utility hook. */
Datum
dg_unchain(PG_FUNCTION_ARGS)
{
	ProcessUtility_hook = unchained_utility;
	PG_RETURN_VOID();
}
