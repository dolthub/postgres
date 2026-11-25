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
 * The binding (design item 17): the only way the fork's C reaches Postgres,
 * through the generated wrappers of calls.list, which catch what Postgres
 * raises, and the only way Postgres reaches the fork's C, through the entries
 * of entries.list, which pg_run turns into outcomes their C thunks raise.
 */
#ifndef FRONTEND
#include "postgres.h"

#include "doltgres/pg/lib.h"
#include "pg_calls.h"
#include "doltgres/std/alloc.h"
#include "doltgres/std/c_str.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/iter.h"
#include "doltgres/std/num.h"
#include "doltgres/std/ptr.h"
#include "doltgres/std/sync.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#ifdef DOLTGRES_TEST_PG
#include "doltgres/std/test.h"
#endif

#define RUST_FILE "pg" STD_SEP "src" STD_SEP "lib.rs"

/* pg_Caps_may_raise returns the capability to call a wrapper that can raise. */
const PgMayRaise *
pg_Caps_may_raise(const PgCaps *self)
{
	return &self->raise;
}

/* pg_CatalogCaps_may_raise returns the capability to call a wrapper that can raise. */
const PgMayRaise *
pg_CatalogCaps_may_raise(const PgCatalogCaps *self)
{
	return &self->raise;
}

/* pg_CatalogCaps_catalog returns the capability to call a wrapper that can reach a catalog. */
const PgCatalog *
pg_CatalogCaps_catalog(const PgCatalogCaps *self)
{
	return &self->catalog;
}

/* pg_Caps_Grant_grant makes an ordinary entry's capabilities, which may raise. */
static void
pg_Caps_Grant_grant(PgSealedToken token, void *result)
{
	PgCaps	   *caps = result;

	(void) token;
	*caps = (PgCaps)
	{
		0
	};
}

/* PG_CAPS_GRANT_OPS is Caps' Grant: an ordinary entry's. */
const PgGrantOps PG_CAPS_GRANT_OPS = {pg_Caps_Grant_grant, true};

/* pg_CatalogCaps_Grant_grant makes an ordinary entry's capabilities, which may raise and reach a catalog. */
static void
pg_CatalogCaps_Grant_grant(PgSealedToken token, void *result)
{
	PgCatalogCaps *caps = result;

	(void) token;
	*caps = (PgCatalogCaps)
	{
		0
	};
}

/* PG_CATALOG_CAPS_GRANT_OPS is CatalogCaps' Grant: an ordinary entry's. */
const PgGrantOps PG_CATALOG_CAPS_GRANT_OPS = {pg_CatalogCaps_Grant_grant, true};

/* pg_Cleanup_Grant_grant makes a cleanup entry's capabilities, which are none. */
static void
pg_Cleanup_Grant_grant(PgSealedToken token, void *result)
{
	PgCleanup  *cleanup = result;

	(void) token;
	*cleanup = (PgCleanup)
	{
		0
	};
}

/* PG_CLEANUP_GRANT_OPS is Cleanup's Grant: not an ordinary entry's. */
const PgGrantOps PG_CLEANUP_GRANT_OPS = {pg_Cleanup_Grant_grant, false};

/* pg_Locale_FromIcuLocale_from takes an ICU locale's pointer. */
PgLocale
pg_Locale_FromIcuLocale_from(PgIcuLocale *locale)
{
	return (PgLocale)
	{
		locale->f0
	};
}

/* pg_Locale_FromBuiltinLocale_from takes a builtin locale's pointer. */
PgLocale
pg_Locale_FromBuiltinLocale_from(PgBuiltinLocale *locale)
{
	return (PgLocale)
	{
		locale->f0
	};
}

/* pg_Locale_into_raw gives the locale up as the pointer the generated thunk of an entry stores, and is for that alone. */
DgLocale
pg_Locale_into_raw(PgLocale *self)
{
	return self->f0;
}

/* pg_TableRoutine_into_raw returns the routine's address, for the generated entry alone, whose thunk stores it. */
const TableAmRoutine *
pg_TableRoutine_into_raw(PgTableRoutine *self)
{
	return self->f0;
}

/* pg_PgString_into_raw gives the string up as the pointer the generated thunk of an entry stores, and is for that alone. */
char *
pg_PgString_into_raw(PgPgString *self)
{
	return self->f0;
}

/* pg_ScanDesc_scan lends the descriptor to the scan wrappers. */
PgScan *
pg_ScanDesc_scan(PgScanDesc *self)
{
	return (PgScan *) self->f0;
}

/*
 * pg_IndexScan_heap_fetch lends the scan's fetch of its table's rows, which
 * index_beginscan makes once the scan's begin returns, or none for a scan
 * with no table to fetch from, a bitmap scan's.
 */
PgIndexFetch *
pg_IndexScan_heap_fetch(PgIndexScan *self)
{
	void	   *fetch = pg_calls_index_scan_heapfetch(self);

	return !(fetch == NULL) ? (PgIndexFetch *) fetch : NULL;
}

/* pg_IndexScan_index lends the index the scan reads, which Postgres keeps open for as long as the scan. */
const PgRel *
pg_IndexScan_index(const PgIndexScan *self)
{
	return (const PgRel *) pg_calls_index_scan_index(self);
}

/* pg_IndexScan_snapshot lends the snapshot the scan reads under, none before index_beginscan sets it. */
const PgSnapshot *
pg_IndexScan_snapshot(const PgIndexScan *self)
{
	void	   *snapshot = pg_calls_index_scan_snapshot(self);

	return !(snapshot == NULL) ? (const PgSnapshot *) snapshot : NULL;
}

/* pg_IndexScanDesc_scan lends the descriptor to the index_scan wrappers. */
PgIndexScan *
pg_IndexScanDesc_scan(PgIndexScanDesc *self)
{
	return (PgIndexScan *) self->f0;
}

/* pg_snapshot_view_closure_0 is lent: a snapshot's array of xids of a count, none for a NULL array or a count of zero. */
static void
pg_snapshot_view_closure_0(void *captures, const uint32 *xids, size_t count, const uint32 **result,
						   size_t *result_len)
{
	(void) captures;
	if (xids == NULL || count == 0)
	{
		*result = STD_DANGLING;
		*result_len = 0;
	}
	else
	{
		std_slice_from_raw_parts_check(xids, count, sizeof(uint32), _Alignof(uint32), RUST_FILE, 574, 22);
		*result = xids;
		*result_len = count;
	}
}

/* pg_snapshot_view_of reads a snapshot's fields into its view, its arrays lent for as long as the snapshot is. */
static void
pg_snapshot_view_of(DgSnapshotView fields, PgSnapshotView *result)
{
	size_t		subxcnt;
	StdTryFromIntError tried;

	result->kind = fields.kind;
	result->xmin = fields.xmin;
	result->xmax = fields.xmax;
	result->command = fields.command;
	result->recovery = fields.recovery;
	result->overflowed = fields.overflowed;
	pg_snapshot_view_closure_0(NULL, fields.xip, (size_t) fields.xcnt, &result->xip, &result->xip_len);
	if (!std_usize_try_from_i32(fields.subxcnt, &subxcnt, &tried))
		subxcnt = 0;
	pg_snapshot_view_closure_0(NULL, fields.subxip, subxcnt, &result->subxip, &result->subxip_len);
}

/*
 * pg_transaction_snapshot_view reads the transaction's snapshot, as
 * GetTransactionSnapshot takes it, into its view, its arrays lent until the
 * backend's next snapshot.
 */
bool
pg_transaction_snapshot_view(const PgMayRaise *raise, PgSnapshotView *result, PgPgError *error)
{
	DgSnapshotView fields;

	if (!pg_calls_transaction_snapshot_fields(raise, &fields, error))
		return false;
	pg_snapshot_view_of(fields, result);
	return true;
}

/* pg_snapshot_view reads a snapshot an entry got into its view, its arrays lent for as long as the snapshot is. */
bool
pg_snapshot_view(const PgMayRaise *raise, const PgSnapshot *snapshot, PgSnapshotView *result, PgPgError *error)
{
	DgSnapshotView fields;

	if (!pg_calls_snapshot_fields(raise, snapshot, &fields, error))
		return false;
	pg_snapshot_view_of(fields, result);
	return true;
}

/*
 * pg_ScanKeys_with_count returns a scan's keys and their count as the one
 * value scan_new takes, as Postgres handed them to the scan's begin.
 */
DgScanKeys
pg_ScanKeys_with_count(const PgScanKeys *keys, int32 count)
{
	return (DgScanKeys)
	{
		count, keys == NULL ? NULL : (ScanKey) keys
	};
}

/* pg_DeadItems_drop is DeadItems' drop: its TidStore destroyed. */
void
pg_DeadItems_drop(PgDeadItems *self)
{
	pg_calls_free_dead_items(self);
}

/* pg_IndexStats_from_raw takes the statistics a wrapper returned, none for a NULL pointer. */
PgIndexStatsOption
pg_IndexStats_from_raw(IndexBulkDeleteResult *stats)
{
	if (stats == NULL)
		return (PgIndexStatsOption)
	{
		false
	};
	return (PgIndexStatsOption)
	{
		true,
		{
			stats
		}
	};
}

/*
 * pg_IndexStats_into_raw gives the statistics to a wrapper that takes them,
 * or to the generated thunk of an entry that returns them, and is for those
 * alone.
 */
IndexBulkDeleteResult *
pg_IndexStats_into_raw(PgIndexStats *self)
{
	return self->f0;
}

/*
 * pg_slots returns the slots Postgres hands a multi-row insert as one slice,
 * refusing a negative count as an internal error before the slice is made.
 * When count is positive, slots points to count distinct, non-NULL slots, as
 * CopyMultiInsertBufferFlush and ExecBatchInsert pass them.
 */
bool
pg_slots(TupleTableSlot **slots, int32 count, PgSlot ***result, size_t *result_len, PgPgError *error)
{
	size_t		given;
	StdTryFromIntError tried;

	if (std_usize_try_from_i32(count, &given, &tried))
	{
		if (given == 0)
		{
			*result = STD_DANGLING;
			*result_len = 0;
			return true;
		}
		std_slice_from_raw_parts_mut_check(slots, given, sizeof(PgSlot *), _Alignof(PgSlot *), RUST_FILE,
										   709, 34);
		*result = (PgSlot **) slots;
		*result_len = given;
		return true;
	}
	{
		static const StdFmtPiece pieces[] = {STD_FMT_LIT("a multi-row insert was handed "), STD_FMT_ARG(0),
		STD_FMT_LIT(" slots")};
		StdFmtArg	args[] = {{&count, std_i32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*error = pg_PgError_fork((const uint8 *) "XX000", &message);
		return false;
	}
}

/*
 * pg_toast_slice returns the bytes past a varlena's header that a TOAST
 * slice fetch fills, as many as the slice's length, refusing a negative
 * length as an internal error before the slice is made. result is a varlena
 * with room for length bytes past its four-byte header, as toast_fetch_datum
 * and toast_fetch_datum_slice allocate it.
 */
bool
pg_toast_slice(void *result_, int32 length, uint8 **result, size_t *result_len, PgPgError *error)
{
	size_t		given;
	StdTryFromIntError tried;

	if (std_usize_try_from_i32(length, &given, &tried))
	{
		uint8	   *bytes = (uint8 *) STD_PTR_AT(result_, 4, sizeof(uint8));

		std_slice_from_raw_parts_mut_check(bytes, given, sizeof(uint8), _Alignof(uint8), RUST_FILE,
										   726, 35);
		*result = bytes;
		*result_len = given;
		return true;
	}
	{
		static const StdFmtPiece pieces[] = {STD_FMT_LIT("a TOAST slice of "), STD_FMT_ARG(0),
		STD_FMT_LIT(" bytes was asked for")};
		StdFmtArg	args[] = {{&length, std_i32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};
		StdString	message;

		std_fmt_format_Checked(&message, &arguments);
		*error = pg_PgError_fork((const uint8 *) "XX000", &message);
		return false;
	}
}

/*
 * pg_OwnedSnapshot_nonvacuumable makes a NonVacuumable snapshot of a
 * relation's horizon, as InitNonVacuumableSnapshot sets one up with
 * GlobalVisTestFor.
 */
bool
pg_OwnedSnapshot_nonvacuumable(const PgMayRaise *raise, const PgRel *rel, PgOwnedSnapshot *result,
							   PgPgError *error)
{
	void	   *snapshot;

	if (!pg_calls_nonvacuumable_snapshot(raise, rel, &snapshot, error))
		return false;
	*result = (PgOwnedSnapshot)
	{
		snapshot
	};
	return true;
}

/*
 * pg_OwnedSnapshot_dirty makes a Dirty snapshot, as InitDirtySnapshot sets
 * one up, which sees running writers' rows and reports them in its own
 * fields.
 */
bool
pg_OwnedSnapshot_dirty(const PgMayRaise *raise, PgOwnedSnapshot *result, PgPgError *error)
{
	void	   *snapshot;

	if (!pg_calls_dirty_snapshot(raise, &snapshot, error))
		return false;
	*result = (PgOwnedSnapshot)
	{
		snapshot
	};
	return true;
}

/* pg_OwnedSnapshot_snapshot lends the snapshot for as long as its owner is borrowed. */
const PgSnapshot *
pg_OwnedSnapshot_snapshot(const PgOwnedSnapshot *self)
{
	return (const PgSnapshot *) self->f0;
}

/* pg_OwnedSnapshot_drop is OwnedSnapshot's drop: the snapshot freed. */
void
pg_OwnedSnapshot_drop(PgOwnedSnapshot *self)
{
	pg_calls_free_snapshot(self->f0);
}

/*
 * pg_RefRel_opened takes a reference to a relation's relcache entry, or
 * answers none for a relation that no longer exists.
 */
bool
pg_RefRel_opened(const PgMayRaise *raise, const PgCatalog *catalog, uint32 relid, bool *has_result,
				 PgRefRel *result, PgPgError *error)
{
	void	   *rel;

	if (!pg_calls_relation_opened(raise, catalog, relid, &rel, error))
		return false;
	if (rel == NULL)
	{
		*has_result = false;
		return true;
	}
	*has_result = true;
	*result = (PgRefRel)
	{
		rel
	};
	return true;
}

/* pg_RefRel_rel lends the relation for as long as its owner is borrowed. */
const PgRel *
pg_RefRel_rel(const PgRefRel *self)
{
	return (const PgRel *) self->f0;
}

/* pg_RefRel_drop is RefRel's drop: the relcache entry's reference given back. */
void
pg_RefRel_drop(PgRefRel *self)
{
	pg_calls_relation_closed(self->f0);
}

/*
 * pg_OwnedRel_index_table opens an index's table under AccessShareLock,
 * which a planner or a scan of the index already holds.
 */
bool
pg_OwnedRel_index_table(const PgMayRaise *raise, const PgCatalog *catalog, const PgRel *index,
						PgOwnedRel *result, PgPgError *error)
{
	void	   *table;

	if (!pg_calls_index_table_open(raise, catalog, index, &table, error))
		return false;
	*result = (PgOwnedRel)
	{
		table
	};
	return true;
}

/* pg_OwnedRel_path_index opens the index of a path the planner costs, whose lock the planner already holds. */
bool
pg_OwnedRel_path_index(const PgMayRaise *raise, const PgCatalog *catalog, PgIndexPath *path,
					   PgOwnedRel *result, PgPgError *error)
{
	void	   *index;

	if (!pg_calls_index_path_open(raise, catalog, path, &index, error))
		return false;
	*result = (PgOwnedRel)
	{
		index
	};
	return true;
}

/* pg_OwnedRel_rel lends the relation for as long as its owner is borrowed. */
const PgRel *
pg_OwnedRel_rel(const PgOwnedRel *self)
{
	return (const PgRel *) self->f0;
}

/* pg_OwnedRel_drop is OwnedRel's drop: the relation closed, its lock kept. */
void
pg_OwnedRel_drop(PgOwnedRel *self)
{
	pg_calls_table_close_kept(self->f0);
}

/* pg_OwnedSlot_slot lends the slot for as long as its owner is borrowed. */
PgSlot *
pg_OwnedSlot_slot(PgOwnedSlot *self)
{
	return (PgSlot *) self->f0;
}

/* pg_OwnedSlot_drop is OwnedSlot's drop: the slot dropped. */
void
pg_OwnedSlot_drop(PgOwnedSlot *self)
{
	pg_calls_drop_slot(pg_OwnedSlot_slot(self));
}

/* pg_OwnedBuild_build lends the state for as long as its owner is borrowed. */
PgBuild *
pg_OwnedBuild_build(PgOwnedBuild *self)
{
	return (PgBuild *) self->f0;
}

/* pg_OwnedBuild_drop is OwnedBuild's drop: the state freed. */
void
pg_OwnedBuild_drop(PgOwnedBuild *self)
{
	pg_calls_build_free(pg_OwnedBuild_build(self));
}

/* pg_OwnedSort_sort lends the sort for as long as its owner is borrowed. */
PgSort *
pg_OwnedSort_sort(PgOwnedSort *self)
{
	return (PgSort *) self->f0;
}

/* pg_OwnedSort_drop is OwnedSort's drop: the sort ended. */
void
pg_OwnedSort_drop(PgOwnedSort *self)
{
	pg_calls_sort_end(pg_OwnedSort_sort(self));
}

/* pg_Att_of returns the attribute a word of slot_att holds, or nothing for the zero of an attribute out of range. */
PgAttOption
pg_Att_of(uint64 word)
{
	if (word != 0)
		return (PgAttOption)
	{
		true,
		{
			word
		}
	};
	return (PgAttOption)
	{
		false
	};
}

/* pg_Att_type_id returns the OID of the attribute's type. */
uint32
pg_Att_type_id(PgAtt self)
{
	return (uint32) STD_SHR_FOLDED(u64, self.f0, 32);
}

/* pg_Att_length returns the type's stored length: positive for a fixed length, -1 for a varlena, -2 for a C string. */
int16
pg_Att_length(PgAtt self)
{
	return (int16) (uint16) self.f0;
}

/* pg_Att_by_value reports whether the type's values are passed by value. */
bool
pg_Att_by_value(PgAtt self)
{
	return (STD_SHR_FOLDED(u64, self.f0, 16) & 1) != 0;
}

/* pg_Att_dropped reports whether the attribute was dropped. */
bool
pg_Att_dropped(PgAtt self)
{
	return (STD_SHR_FOLDED(u64, self.f0, 17) & 1) != 0;
}

/* pg_Att_align returns the type's alignment in bytes. */
size_t
pg_Att_align(PgAtt self)
{
	return (size_t) (STD_SHR_FOLDED(u64, self.f0, 24) & 0xff);
}

/*
 * pg_borrowed returns the bytes a record, argument, or slot wrapper
 * returned, borrowed for as long as the caller says they live: bytes points
 * to len bytes that stay readable and unchanged, or len is zero.
 */
const uint8 *
pg_borrowed(DgBytes bytes, size_t *result_len)
{
	if (bytes.len == 0)
	{
		*result_len = 0;
		return STD_DANGLING;
	}
	std_slice_from_raw_parts_check(bytes.data, bytes.len, sizeof(uint8), _Alignof(uint8), RUST_FILE,
								   1103, 18);
	*result_len = bytes.len;
	return (const uint8 *) bytes.data;
}

StaticAssertDecl(sizeof(Datum) == 8, "a Datum is 64 bits");

/* pg_Datum_word returns the datum's 64 bits, which word's readers read as a type passed by value. */
uint64
pg_Datum_word(PgDatum self)
{
	return (uint64) self.f0;
}

/* pg_PgError_new takes the error a wrapper caught. */
PgPgError
pg_PgError_new(DgError raw)
{
	return (PgPgError)
	{
		raw, false, STD_VEC_NEW,
		{
			NULL, 0
		}, false
	};
}

/* pg_PgError_fork makes the fork's own error, with its SQLSTATE and message in Postgres' wording; it takes the message. */
PgPgError
pg_PgError_fork(const uint8 *sqlstate, StdString *message)
{
	StdStr		hint = {NULL, 0};

	return pg_PgError_hinted(sqlstate, message, hint);
}

/* pg_PgError_hinted makes the fork's own error as pg_PgError_fork does, with its hint, when it has one. */
PgPgError
pg_PgError_hinted(const uint8 *sqlstate, StdString *message, StdStr hint)
{
	int32		code = 0;
	PgPgError	result;

	for (size_t i = 0; i < 5; i++)
	{
		uint8		c = sqlstate[i];
		int32		tmp_1 = (int32) std_wrapping_sub_u8(c, '0') & 0x3F;
		size_t		tmp_2 = STD_MUL(usize, 6, i, 1154, 106);
		int32		tmp_3 = STD_SHL(i32, tmp_1, tmp_2, 1154, 61);

		code = std_add_i32(code, tmp_3, STD_ACCUM_FILE, 149, 1);
	}
	result.raw = (DgError)
	{
		NULL, code
	};
	result.has_fork = true;
	result.fork = *message;
	result.hint = hint;
	result.raised = false;
	return result;
}

/* pg_PgError_sqlerrcode returns the error's SQLSTATE in Postgres' encoding. */
int32
pg_PgError_sqlerrcode(const PgPgError *self)
{
	return self->raw.sqlerrcode;
}

/* pg_PgError_message returns the message of the fork's own error, and nothing, a NULL data, for one Postgres raised. */
StdStr
pg_PgError_message(const PgPgError *self)
{
	if (!self->has_fork)
		return (StdStr)
	{
		NULL, 0
	};
	return (StdStr)
	{
		(const char *) self->fork.ptr, self->fork.len
	};
}

/* pg_PgError_into_raw gives the error to the entry thunk to raise, with its hint. */
void
pg_PgError_into_raw(PgPgError *self, DgError *out_0, bool *has_out_1, StdString *out_1, StdStr *out_2)
{
	self->raised = true;
	*out_0 = self->raw;
	*has_out_1 = self->has_fork;
	if (self->has_fork)
		*out_1 = self->fork;
	self->has_fork = false;
	*out_2 = self->hint;
	pg_PgError_drop(self);
}

/* pg_PgError_drop_frame_0_t is PgError's drop glue's state across its pad: the error. */
typedef struct pg_PgError_drop_frame_0_t
{
	PgPgError  *self;
} pg_PgError_drop_frame_0_t;

/*
 * pg_PgError_drop_body_0 is PgError's Drop::drop: in a build with
 * assertions, a Postgres error dropped neither raised nor while unwinding
 * panics.
 */
static void
pg_PgError_drop_body_0(void *frame_)
{
	pg_PgError_drop_frame_0_t *frame = frame_;

#ifdef USE_ASSERT_CHECKING
	if (!frame->self->has_fork && !frame->self->raised && !std_thread_panicking())
		STD_PANIC(1180, 13, "a PgError was dropped instead of raised");
#else
	(void) frame;
#endif
}

/* pg_PgError_drop_pad_0 is PgError's drop glue's pad: the fork's message, which a panic in Drop::drop still frees. */
static void
pg_PgError_drop_pad_0(void *frame_)
{
	pg_PgError_drop_frame_0_t *frame = frame_;

	if (frame->self->has_fork)
		std_vec_free_Checked(&frame->self->fork, 1, 1);
}

/* pg_PgError_drop is PgError's drop glue: its Drop::drop, and then the fork's message freed. */
void
pg_PgError_drop(void *self_)
{
	PgPgError  *self = self_;
	pg_PgError_drop_frame_0_t frame = {.self = self};

	std_landing(&frame, pg_PgError_drop_body_0, pg_PgError_drop_pad_0);
	if (self->has_fork)
		std_vec_free_Checked(&self->fork, 1, 1);
}

/* PG_MAY_RAISE_TYPE_ID is MayRaise's identity, which pghost's Io holds as an Any. */
const StdTypeId PG_MAY_RAISE_TYPE_ID = {"pg::MayRaise", sizeof(PgMayRaise), _Alignof(PgMayRaise), NULL};

/* PG_CATALOG_CAPS_TYPE_ID is CatalogCaps' identity, which pghost's Io holds as an Any for a catalog entry. */
const StdTypeId PG_CATALOG_CAPS_TYPE_ID = {"pg::CatalogCaps", sizeof(PgCatalogCaps), _Alignof(PgCatalogCaps), NULL};

/* PG_SNAPSHOT_TYPE_ID is Snapshot's identity, which host's snapshot holds as an Any. */
const StdTypeId PG_SNAPSHOT_TYPE_ID = {"pg::Snapshot", sizeof(PgSnapshot), _Alignof(PgSnapshot), NULL};

/* PG_PG_ERROR_TYPE_ID is PgError's identity, which host's error holds as a Box<dyn Any>. */
const StdTypeId PG_PG_ERROR_TYPE_ID = {"pg::PgError", sizeof(PgPgError), _Alignof(PgPgError), pg_PgError_drop};

/*
 * pg_warn_error reports an error at WARNING with a context before its
 * message, a Postgres error by its own data, for an entry that goes on past
 * it in place of raising it; a fork error's hint isn't reported, since none
 * it warns of has one. It takes the error.
 */
bool
pg_warn_error(const PgMayRaise *raise, PgPgError *error_, const char *context, PgPgError *error)
{
	DgError		raw;
	bool		has_fork;
	StdString	fork;
	StdStr		hint;
	StdCString	message;
	StdNulError nul;
	bool		done;

	pg_PgError_into_raw(error_, &raw, &has_fork, &fork, &hint);
	if (!has_fork)
		fork = (StdString) STD_VEC_NEW;
	if (!std_CString_new_String(&fork, &message, &nul))
	{
		std_NulError_drop(&nul);
		std_CString_default(&message);
	}
	done = pg_calls_report_warning(raise, raw.data, raw.sqlerrcode, std_CString_as_ptr(&message), context, error);
	std_CString_drop(&message);
	return done;
}

/* pg_sort_key_of_frame_0_t is sort_key_of's state across its pad: its arguments, its result, and the key. */
typedef struct pg_sort_key_of_frame_0_t
{
	const PgMayRaise *raise;
	const PgCatalog *catalog;
	uint32		collation;
	const uint8 *text_;
	size_t		text_len;
	StdVec	   *result;
	PgPgError  *error;
	bool		done;
	StdVec		key;
	bool		key_live;
} pg_sort_key_of_frame_0_t;

/* pg_sort_key_of_body_0 is sort_key_of's body. */
static void
pg_sort_key_of_body_0(void *frame_)
{
	pg_sort_key_of_frame_0_t *frame = frame_;
	size_t		tmp_1 = STD_MUL(usize, frame->text_len, 2, 1202, 27);
	size_t		tmp_2 = STD_ADD(usize, tmp_1, 16, 1202, 27);
	size_t		len;

	std_vec_u8_from_elem(&frame->key, 0, tmp_2);
	frame->key_live = true;
	if (!pg_calls_sort_key(frame->raise, frame->catalog, frame->collation, frame->text_, frame->text_len,
						   frame->key.ptr, frame->key.len, &len, frame->error))
	{
		frame->done = false;
		frame->key_live = false;
		std_vec_free_Checked(&frame->key, 1, 1);
		return;
	}
	if (len >= frame->key.len)
	{
		static const uint8 zero = 0;
		size_t		tmp_3 = STD_ADD(usize, len, 1, 1205, 20);

		std_vec_resize(&frame->key, tmp_3, &zero, 1, 1);
		if (!pg_calls_sort_key(frame->raise, frame->catalog, frame->collation, frame->text_, frame->text_len,
							   frame->key.ptr, frame->key.len, &len, frame->error))
		{
			frame->done = false;
			frame->key_live = false;
			std_vec_free_Checked(&frame->key, 1, 1);
			return;
		}
	}
	std_vec_truncate(&frame->key, len, 1, NULL);
	frame->key_live = false;
	*frame->result = frame->key;
	frame->done = true;
}

/* pg_sort_key_of_pad_0 is sort_key_of's pad: the key, while it is live. */
static void
pg_sort_key_of_pad_0(void *frame_)
{
	pg_sort_key_of_frame_0_t *frame = frame_;

	if (frame->key_live)
		std_vec_free_Checked(&frame->key, 1, 1);
}

/*
 * pg_sort_key_of returns a text's sort key under a collation, as Postgres'
 * pg_strnxfrm writes it, asking for it into a buffer of twice the text's
 * length and more, and again into one as long as the answer when that was
 * too short.
 */
bool
pg_sort_key_of(const PgMayRaise *raise, const PgCatalog *catalog, uint32 collation, const uint8 *text_,
			   size_t text_len, StdVec *result, PgPgError *error)
{
	pg_sort_key_of_frame_0_t frame = {.raise = raise, .catalog = catalog, .collation = collation, .text_ = text_,
	.text_len = text_len, .result = result, .error = error};

	std_landing(&frame, pg_sort_key_of_body_0, pg_sort_key_of_pad_0);
	return frame.done;
}

/*
 * pg_replayed returns replay's position and timeline as one pair, read in one
 * call under the spinlock that guards both.
 */
void
pg_replayed(uint64 *out_0, uint32 *out_1)
{
	uint32		timeline = 0;
	uint64		position = pg_calls_replay_read(&timeline);

	*out_0 = position;
	*out_1 = timeline;
}

/* PG_RESERVE_SIZE is the size of the block the allocator frees when an allocation fails. */
#define PG_RESERVE_SIZE ((size_t) 1 << 20)

/* PG_RESERVE is the reserve block's address, or zero while it is spent; an AtomicPtr<u8>. */
static pg_atomic_uint64 PG_RESERVE = {0};

/* PG_FAILED_SIZE is the size of the last allocation that failed. */
pg_atomic_uint64 PG_FAILED_SIZE = {0};

#if defined(USE_INJECTION_POINTS) || defined(DOLTGRES_PG_OUTSIDE_POSTGRES)
/*
 * PG_FAILING is how many requests come before the one the allocator fails,
 * plus one, or PG_UINT32_MAX while it fails every request, or 0 for none,
 * which the fork's tests set in a process of one thread.
 */
static pg_atomic_uint32 PG_FAILING = {0};

/*
 * pg_fail_allocations makes every allocation fail as one the system refuses
 * does, or stops that, for the fork's tests of what a failed allocation
 * leaves behind, in a process of one thread.
 */
void
pg_fail_allocations(bool fail)
{
	pg_atomic_write_membarrier_u32(&PG_FAILING, fail ? PG_UINT32_MAX : 0);
}

/*
 * pg_fail_nth_allocation makes the nth of the next allocations fail, counting
 * from 1, as one the system refuses does, so a test fails one allocation and
 * the unwinding its failure starts can allocate.
 */
void
pg_fail_nth_allocation(uint32 nth)
{
	pg_atomic_write_membarrier_u32(&PG_FAILING, nth);
}

/* PG_REFUSED_SIZE is the size of the request pg_fail_nth_allocation's count refused last. */
static pg_atomic_uint64 PG_REFUSED_SIZE = {0};

/*
 * pg_refused_size returns the size of the request pg_fail_nth_allocation's
 * count refused last, so a test checks it failed the allocation it means.
 */
uint64
pg_refused_size(void)
{
	return pg_atomic_read_membarrier_u64(&PG_REFUSED_SIZE);
}
#endif

#if defined(DOLTGRES_TEST_PG) || defined(DOLTGRES_PG_OUTSIDE_POSTGRES)
/* PG_IN_USE is the bytes the allocator gave and has not been given back, which a test binary built without Postgres counts. */
static pg_atomic_uint64 PG_IN_USE = {0};

/*
 * pg_heap_in_use returns the bytes the allocator gave and has not been given
 * back, for the fork's tests of what a piece of work leaves on the heap.
 */
size_t
pg_heap_in_use(void)
{
	return (size_t) pg_atomic_read_u64(&PG_IN_USE);
}

/* pg_count counts the bytes of a block the system gave, if it gave one, less the bytes given back for it. */
static void
pg_count(void *block, size_t given, size_t back)
{
	if (!(block == NULL))
		(void) pg_atomic_fetch_add_u64(&PG_IN_USE, (int64) std_wrapping_sub_usize(given, back));
}
#endif

static void pg_release_reserve(void);

/* pg_refused reports whether the fork's tests fail a request of a size, after freeing the reserve as a refused request does. */
static bool
pg_refused(size_t size)
{
#if defined(USE_INJECTION_POINTS) || defined(DOLTGRES_PG_OUTSIDE_POSTGRES)
	uint32		failing = pg_atomic_read_membarrier_u32(&PG_FAILING);

	if (failing != 0 && failing != PG_UINT32_MAX)
		pg_atomic_write_membarrier_u32(&PG_FAILING, failing - 1);
	if (failing == 1)
		pg_atomic_write_membarrier_u64(&PG_REFUSED_SIZE, size);
	if (failing == 1 || failing == PG_UINT32_MAX)
	{
		pg_release_reserve();
		return true;
	}
#endif
	return false;
}

/* pg_reserve_layout returns the reserve block's layout. */
static StdLayout
pg_reserve_layout(void)
{
	StdLayout	layout;
	StdLayoutError error;

	if (!std_Layout_from_size_align(PG_RESERVE_SIZE, 16, &layout, &error))
		std_result_unwrap_failed("the reserve's layout is valid", sizeof("the reserve's layout is valid") - 1,
								 &error, std_LayoutError_Debug_fmt, RUST_FILE, 1288, 47);
	return layout;
}

/* pg_release_reserve frees the reserve block, so the unwind after a failed allocation can allocate. */
static void
pg_release_reserve(void)
{
	void	   *block = (void *) (uintptr_t) pg_atomic_exchange_u64(&PG_RESERVE, 0);

	if (!(block == NULL))
	{
		StdLayout	layout = pg_reserve_layout();

		std_System_GlobalAlloc_dealloc(block, layout.size, layout.align);
	}
}

/* pg_retake_reserve takes the reserve block again from the system's allocator directly, returning false if it can't. */
bool
pg_retake_reserve(void)
{
	StdLayout	layout;
	void	   *block;

	if (!(pg_atomic_read_membarrier_u64(&PG_RESERVE) == 0))
		return true;
	layout = pg_reserve_layout();
	block = std_System_GlobalAlloc_alloc(layout.size, layout.align);
	if (block == NULL)
	{
		pg_atomic_write_membarrier_u64(&PG_FAILED_SIZE, PG_RESERVE_SIZE);
		return false;
	}
	pg_atomic_write_membarrier_u64(&PG_RESERVE, (uint64) (uintptr_t) block);
	return true;
}

/* PgReserving is the global allocator: the system's, which frees the reserve when an allocation fails. */
typedef struct PgReserving
{
	char		unused;
} PgReserving;

/* pg_Reserving_GlobalAlloc_alloc is Reserving's alloc: the system's, which frees the reserve when it fails. */
static void *
pg_Reserving_GlobalAlloc_alloc(const PgReserving *self, size_t size, size_t align)
{
	void	   *block;

	(void) self;
	if (pg_refused(size))
		return NULL;
	block = std_System_GlobalAlloc_alloc(size, align);
	if (block == NULL)
		pg_release_reserve();
#if defined(DOLTGRES_TEST_PG) || defined(DOLTGRES_PG_OUTSIDE_POSTGRES)
	pg_count(block, size, 0);
#endif
	return block;
}

/* pg_Reserving_GlobalAlloc_alloc_zeroed is Reserving's alloc_zeroed: the system's, which frees the reserve when it fails. */
static void *
pg_Reserving_GlobalAlloc_alloc_zeroed(const PgReserving *self, size_t size, size_t align)
{
	void	   *block;

	(void) self;
	if (pg_refused(size))
		return NULL;
	block = std_System_GlobalAlloc_alloc_zeroed(size, align);
	if (block == NULL)
		pg_release_reserve();
#if defined(DOLTGRES_TEST_PG) || defined(DOLTGRES_PG_OUTSIDE_POSTGRES)
	pg_count(block, size, 0);
#endif
	return block;
}

/* pg_Reserving_GlobalAlloc_realloc is Reserving's realloc: the system's, which frees the reserve when it fails. */
static void *
pg_Reserving_GlobalAlloc_realloc(const PgReserving *self, void *block, size_t old_size, size_t align, size_t size)
{
	void	   *moved;

	(void) self;
	if (pg_refused(size))
		return NULL;
	moved = std_System_GlobalAlloc_realloc(block, old_size, align, size);
	if (moved == NULL)
		pg_release_reserve();
#if defined(DOLTGRES_TEST_PG) || defined(DOLTGRES_PG_OUTSIDE_POSTGRES)
	pg_count(moved, size, old_size);
#endif
	return moved;
}

/* pg_Reserving_GlobalAlloc_dealloc is Reserving's dealloc: the system's. */
static void
pg_Reserving_GlobalAlloc_dealloc(const PgReserving *self, void *block, size_t size, size_t align)
{
	(void) self;
#if defined(DOLTGRES_TEST_PG) || defined(DOLTGRES_PG_OUTSIDE_POSTGRES)
	pg_count(block, 0, size);
#endif
	std_System_GlobalAlloc_dealloc(block, size, align);
}

/* PG_ALLOCATOR is the global allocator, Reserving. */
static const PgReserving PG_ALLOCATOR = {0};

#ifdef DOLTGRES_GLOBAL_ALLOCATOR
/* std_global_alloc is the #[global_allocator] shim of alloc: PG_ALLOCATOR's. */
void *
std_global_alloc(size_t size, size_t align)
{
	return pg_Reserving_GlobalAlloc_alloc(&PG_ALLOCATOR, size, align);
}

/* std_global_alloc_zeroed is the #[global_allocator] shim of alloc_zeroed: PG_ALLOCATOR's. */
void *
std_global_alloc_zeroed(size_t size, size_t align)
{
	return pg_Reserving_GlobalAlloc_alloc_zeroed(&PG_ALLOCATOR, size, align);
}

/* std_global_realloc is the #[global_allocator] shim of realloc: PG_ALLOCATOR's. */
void *
std_global_realloc(void *ptr, size_t old_size, size_t align, size_t new_size)
{
	return pg_Reserving_GlobalAlloc_realloc(&PG_ALLOCATOR, ptr, old_size, align, new_size);
}

/* std_global_dealloc is the #[global_allocator] shim of dealloc: PG_ALLOCATOR's. */
void
std_global_dealloc(void *ptr, size_t size, size_t align)
{
	pg_Reserving_GlobalAlloc_dealloc(&PG_ALLOCATOR, ptr, size, align);
}
#endif

/* PgMessage is a panic's message, kept without allocating so an entry can report it. */
typedef struct PgMessage
{
	uint8		bytes[PG_MESSAGE_SIZE];
	size_t		len;
} PgMessage;

/* pg_Message_Write_write_str appends text to the message, cut to the room it has. */
static bool
pg_Message_Write_write_str(void *self_, const char *s, size_t len)
{
	PgMessage  *self = self_;
	size_t		room = STD_SUB(usize, (size_t) (PG_MESSAGE_SIZE - 1), self->len, 1399, 20);
	size_t		take = std_min_usize(len, room);
	size_t		end = STD_ADD(usize, self->len, take, 1401, 30);

	std_range_check(self->len, end, PG_MESSAGE_SIZE, RUST_FILE, 1401, 19);
	std_range_to_check(take, len, RUST_FILE, 1401, 76);
	std_slice_copy_from_slice_Checked(&self->bytes[self->len], take, s, take, 1, 1, RUST_FILE,
									  1401, 47);
	self->len = STD_ADD(usize, self->len, take, 1402, 9);
	return true;
}

/* pg_Message_Write_write_char is fmt::Write's write_char at Message: the character's UTF-8 through write_str. */
static bool
pg_Message_Write_write_char(void *self, uint32 c)
{
	return std_fmt_Write_write_char(pg_Message_Write_write_str, self, c);
}

/* PG_MESSAGE_WRITE_OPS is Message's fmt::Write. */
static const StdFmtWriteOps PG_MESSAGE_WRITE_OPS = {pg_Message_Write_write_str, pg_Message_Write_write_char};

/* PG_MESSAGE_LOCK is MESSAGE's Mutex, which guards PG_MESSAGE. */
static StdRwLock PG_MESSAGE_LOCK = STD_RW_LOCK_NEW;

/* PG_MESSAGE is the message of the last panic. */
static PgMessage PG_MESSAGE = {{0}, 0};

/* pg_keep_message_frame_0_t is keep_message's state across its pad: the hook's information and the message's guard. */
typedef struct pg_keep_message_frame_0_t
{
	const StdPanicHookInfo *info;
	StdRwLockWriteGuard message;
	bool		message_live;
} pg_keep_message_frame_0_t;

/* pg_keep_message_body_0 is keep_message's body. */
static void
pg_keep_message_body_0(void *frame_)
{
	pg_keep_message_frame_0_t *frame = frame_;
	const StdPanicHookInfo *info = frame->info;
	const char *text_;
	size_t		len;

	(void) std_rw_lock_write(&PG_MESSAGE_LOCK, &PG_MESSAGE, &frame->message);
	frame->message_live = true;
	((PgMessage *) frame->message.data)->len = 0;
	if (!std_PanicHookInfo_payload_as_str(info, &text_, &len))
	{
		text_ = "a panic without a message";
		len = sizeof("a panic without a message") - 1;
	}
	{
		static const StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdStr		value = {text_, len};
		StdFmtArg	args[] = {{&value, std_str_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		(void) std_fmt_write(&PG_MESSAGE_WRITE_OPS, frame->message.data, &arguments);
	}
	if (info->location != NULL)
	{
		static const StdFmtPiece pieces[] = {STD_FMT_LIT(" at "), STD_FMT_ARG(0), STD_FMT_LIT(":"), STD_FMT_ARG(1)};
		StdStr		file = {info->location->file, strlen(info->location->file)};
		StdFmtArg	args[] = {{&file, std_str_Display_fmt}, {&info->location->line, std_u32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		(void) std_fmt_write(&PG_MESSAGE_WRITE_OPS, frame->message.data, &arguments);
	}
	frame->message_live = false;
	std_RwLockWriteGuard_drop(&frame->message);
}

/* pg_keep_message_pad_0 is keep_message's pad: the message's guard, which poisons the lock. */
static void
pg_keep_message_pad_0(void *frame_)
{
	pg_keep_message_frame_0_t *frame = frame_;

	if (frame->message_live)
		std_RwLockWriteGuard_drop(&frame->message);
}

/* pg_keep_message is the panic hook: it keeps a panic's message and location for the entry's report and prints nothing. */
static void
pg_keep_message(const StdPanicHookInfo *info)
{
	pg_keep_message_frame_0_t frame = {.info = info};

	std_landing(&frame, pg_keep_message_body_0, pg_keep_message_pad_0);
}

/* pg_fail_allocation is the allocation error hook: it turns a failed allocation into an OutOfMemory panic. */
static void
pg_fail_allocation(size_t size, size_t align)
{
	(void) align;
	pg_atomic_write_membarrier_u64(&PG_FAILED_SIZE, size);
	std_resume_unwind(STD_PAYLOAD_OUT_OF_MEMORY, STD_DANGLING);
}

/* PG_FAIL_ALLOCATION is fail_allocation as the hook set_alloc_error_hook takes. */
static const StdAllocErrorHook PG_FAIL_ALLOCATION = {pg_fail_allocation};

/* PG_INSTALLED makes pg_install run once per process. */
static StdOnce PG_INSTALLED = {STD_ONCE_INCOMPLETE};

/* pg_install_closure_0 installs the allocation error hook and the panic hook. */
static void
pg_install_closure_0(void *captures)
{
	(void) captures;
	std_alloc_set_alloc_error_hook(&PG_FAIL_ALLOCATION);
	std_panic_set_hook(pg_keep_message);
}

/* pg_install installs the allocation error hook and the panic hook in this process, if they aren't yet. */
void
pg_install(void)
{
	std_once_call_once(&PG_INSTALLED, pg_install_closure_0, NULL, RUST_FILE, 1442, 15);
}

/* pg_written writes a message or a hint into its room in the outcome its C thunk reports, cut to fit. */
void
pg_written(char room[PG_MESSAGE_SIZE], const uint8 *text_, size_t text_len)
{
	size_t		len = std_min_usize(text_len, (size_t) (PG_MESSAGE_SIZE - 1));

	std_range_to_check(len, text_len, RUST_FILE, 1465, 48);
	for (size_t i = 0; i < PG_MESSAGE_SIZE && i < len; i++)
		room[i] = (char) text_[i];
	std_index_check(len, PG_MESSAGE_SIZE, RUST_FILE, 1468, 5);
	room[len] = 0;
}

/* pg_report_frame_0_t is report's state across its pad: the outcome, what the body ended with, and the message's guard. */
typedef struct pg_report_frame_0_t
{
	DgOutcome  *outcome;
	StdCaught  *ended;
	bool		ended_live;
	StdRwLockWriteGuard message;
	bool		message_live;
} pg_report_frame_0_t;

/* pg_report_body_0 is report's body. */
static void
pg_report_body_0(void *frame_)
{
	pg_report_frame_0_t *frame = frame_;
	PgMessage  *kept;

	if (frame->ended->payload == STD_PAYLOAD_OUT_OF_MEMORY)
	{
		frame->outcome->kind = DG_OUT_OF_MEMORY;
		frame->outcome->size = (size_t) pg_atomic_read_membarrier_u64(&PG_FAILED_SIZE);
		frame->ended_live = false;
		std_Caught_drop(frame->ended);
		return;
	}
	frame->outcome->kind = DG_PANIC;
	(void) std_rw_lock_write(&PG_MESSAGE_LOCK, &PG_MESSAGE, &frame->message);
	frame->message_live = true;
	kept = frame->message.data;
	std_range_to_check(kept->len, PG_MESSAGE_SIZE, RUST_FILE, 1458, 49);
	pg_written(frame->outcome->message, kept->bytes, kept->len);
	frame->message_live = false;
	std_RwLockWriteGuard_drop(&frame->message);
	frame->ended_live = false;
	std_Caught_drop(frame->ended);
}

/* pg_report_pad_0 is report's pad: the message's guard, then what the body ended with. */
static void
pg_report_pad_0(void *frame_)
{
	pg_report_frame_0_t *frame = frame_;

	if (frame->message_live)
		std_RwLockWriteGuard_drop(&frame->message);
	if (frame->ended_live)
		std_Caught_drop(frame->ended);
}

/* pg_report writes what an entry's body ended with into the outcome its C thunk reads; it takes what the body ended with. */
void
pg_report(DgOutcome *outcome, StdCaught *ended)
{
	pg_report_frame_0_t frame = {.outcome = outcome, .ended = ended, .ended_live = true};

	std_landing(&frame, pg_report_body_0, pg_report_pad_0);
}

#ifdef DOLTGRES_TEST_PG

/* run::<Cleanup, ()>, the instance the reserve's test runs a cleanup entry with. */
#define PG_RUN_SCOPE static
#define PG_RUN_DECLARE
#define PG_RUN_DEFINE
#define PG_RUN_GS Cleanup
#define PG_RUN_RS
#define PG_RUN_G PgCleanup
#define PG_RUN_G_OPS PG_CLEANUP_GRANT_OPS
#include "doltgres/pg/run_template.h"

/* run::<Caps, ()>, the instance the reserve's test runs an ordinary entry with. */
#define PG_RUN_SCOPE static
#define PG_RUN_DECLARE
#define PG_RUN_DEFINE
#define PG_RUN_GS Caps
#define PG_RUN_RS
#define PG_RUN_G PgCaps
#define PG_RUN_G_OPS PG_CAPS_GRANT_OPS
#include "doltgres/pg/run_template.h"

/* test_pg_tests_dropped_error_panics: an unraised PgError fails its test when dropped (item 17's Order 2a cases). */
void
test_pg_tests_dropped_error_panics(void)
{
	DgError		raw = {NULL, 0};
	PgPgError	tmp_1 = pg_PgError_new(raw);

	pg_PgError_drop(&tmp_1);
}

/* TEST_PG_TESTS_RESERVE_TESTS_LOCK is RESERVE_TESTS' Mutex, which keeps the tests that spend and retake the process's one reserve from running at once. */
static StdRwLock TEST_PG_TESTS_RESERVE_TESTS_LOCK = STD_RW_LOCK_NEW;

/* TEST_PG_TESTS_RESERVE_TESTS is the () RESERVE_TESTS guards. */
static char TEST_PG_TESTS_RESERVE_TESTS;

/* test_pg_tests_only_ordinary_entries_retake_the_reserve_frame_0_t is the test's state across its pad: its guard. */
typedef struct test_pg_tests_only_ordinary_entries_retake_the_reserve_frame_0_t
{
	StdRwLockWriteGuard alone;
	bool		alone_live;
} test_pg_tests_only_ordinary_entries_retake_the_reserve_frame_0_t;

/* test_pg_tests_only_ordinary_entries_retake_the_reserve_closure_0 is the cleanup entry's body, which does nothing. */
static bool
test_pg_tests_only_ordinary_entries_retake_the_reserve_closure_0(void *captures, const PgCleanup *caps, PgPgError *error)
{
	(void) captures;
	(void) caps;
	(void) error;
	return true;
}

/* test_pg_tests_only_ordinary_entries_retake_the_reserve_closure_1 is the ordinary entry's body, which does nothing. */
static bool
test_pg_tests_only_ordinary_entries_retake_the_reserve_closure_1(void *captures, const PgCaps *caps, PgPgError *error)
{
	(void) captures;
	(void) caps;
	(void) error;
	return true;
}

/* test_pg_tests_only_ordinary_entries_retake_the_reserve_body_0 is the test's body. */
static void
test_pg_tests_only_ordinary_entries_retake_the_reserve_body_0(void *frame_)
{
	test_pg_tests_only_ordinary_entries_retake_the_reserve_frame_0_t *frame = frame_;
	DgOutcome	outcome;

	(void) std_rw_lock_write(&TEST_PG_TESTS_RESERVE_TESTS_LOCK, &TEST_PG_TESTS_RESERVE_TESTS, &frame->alone);
	frame->alone_live = true;
	memset(&outcome, 0, sizeof(outcome));
	STD_ASSERT(1540, 9, pg_retake_reserve(), "retake_reserve()");
	pg_release_reserve();
	pg_run_Cleanup(&outcome, test_pg_tests_only_ordinary_entries_retake_the_reserve_closure_0, NULL, NULL);
	STD_ASSERT(1545, 9, pg_atomic_read_membarrier_u64(&PG_RESERVE) == 0, "RESERVE.load(Ordering::SeqCst).is_null()");
	pg_run_Caps(&outcome, test_pg_tests_only_ordinary_entries_retake_the_reserve_closure_1, NULL, NULL);
	STD_ASSERT(1548, 9, !(pg_atomic_read_membarrier_u64(&PG_RESERVE) == 0),
			   "!RESERVE.load(Ordering::SeqCst).is_null()");
	frame->alone_live = false;
	std_RwLockWriteGuard_drop(&frame->alone);
}

/* test_pg_tests_only_ordinary_entries_retake_the_reserve_pad_0 is the test's pad: its guard while it is live. */
static void
test_pg_tests_only_ordinary_entries_retake_the_reserve_pad_0(void *frame_)
{
	test_pg_tests_only_ordinary_entries_retake_the_reserve_frame_0_t *frame = frame_;

	if (frame->alone_live)
		std_RwLockWriteGuard_drop(&frame->alone);
}

/* test_pg_tests_only_ordinary_entries_retake_the_reserve: only an ordinary entry retakes a spent reserve, since a cleanup entry allocates nothing. */
void
test_pg_tests_only_ordinary_entries_retake_the_reserve(void)
{
	test_pg_tests_only_ordinary_entries_retake_the_reserve_frame_0_t frame = {0};

	std_landing(&frame, test_pg_tests_only_ordinary_entries_retake_the_reserve_body_0,
				test_pg_tests_only_ordinary_entries_retake_the_reserve_pad_0);
}

/* test_pg_tests_the_allocator_passes_requests_through_frame_0_t is the test's state across its pad: the vectors it makes. */
typedef struct test_pg_tests_the_allocator_passes_requests_through_frame_0_t
{
	StdVec		grown;
	bool		grown_live;
	StdVec		zeroed;
	bool		zeroed_live;
} test_pg_tests_the_allocator_passes_requests_through_frame_0_t;

/* test_pg_tests_the_allocator_passes_requests_through_body_0 is the test's body. */
static void
test_pg_tests_the_allocator_passes_requests_through_body_0(void *frame_)
{
	test_pg_tests_the_allocator_passes_requests_through_frame_0_t *frame = frame_;
	StdSizeHint hint = {1000, true, 1000};
	bool		all = true;

	std_vec_with_capacity_Checked(&frame->grown, 1, sizeof(uint64), _Alignof(uint64));
	frame->grown_live = true;
	std_vec_extend_trusted_reserve(&frame->grown, hint, sizeof(uint64), _Alignof(uint64));
	for (uint64 n = 0; n < 1000; n = std_forward_unchecked_u64_Checked(n, 1))
	{
		STD_VEC_AT(&frame->grown, uint64, frame->grown.len) = n;
		frame->grown.len = std_wrapping_add_usize(frame->grown.len, 1);
	}
	{
		uint64		left = 0;
		uint64		right = 499500;

		for (size_t i = 0, i_len = frame->grown.len; i < i_len; i++)
			left = std_add_u64(left, STD_VEC_AT(&frame->grown, uint64, i), STD_ACCUM_FILE, 149, 1);
		STD_ASSERT_EQ(1556, 9, left == right, &left, std_u64_Debug_fmt, &right, std_u64_Debug_fmt);
	}
	std_vec_u8_from_elem(&frame->zeroed, 0, 4096);
	frame->zeroed_live = true;
	for (size_t i = 0, i_len = frame->zeroed.len; i < i_len; i++)
	{
		if (!(STD_VEC_AT(&frame->zeroed, uint8, i) == 0))
		{
			all = false;
			break;
		}
	}
	frame->zeroed_live = false;
	std_vec_free_Checked(&frame->zeroed, 1, 1);
	STD_ASSERT(1557, 9, all, "vec![0u8; 4096].iter().all(|b| *b == 0)");
	frame->grown_live = false;
	std_vec_free_Checked(&frame->grown, sizeof(uint64), _Alignof(uint64));
}

/* test_pg_tests_the_allocator_passes_requests_through_pad_0 is the test's pad: each vector while it is live. */
static void
test_pg_tests_the_allocator_passes_requests_through_pad_0(void *frame_)
{
	test_pg_tests_the_allocator_passes_requests_through_frame_0_t *frame = frame_;

	if (frame->zeroed_live)
		std_vec_free_Checked(&frame->zeroed, 1, 1);
	if (frame->grown_live)
		std_vec_free_Checked(&frame->grown, sizeof(uint64), _Alignof(uint64));
}

/* test_pg_tests_the_allocator_passes_requests_through: the allocator hands every request to the system's unchanged: allocating, zeroing, growing, and freeing. */
void
test_pg_tests_the_allocator_passes_requests_through(void)
{
	test_pg_tests_the_allocator_passes_requests_through_frame_0_t frame = {0};

	std_landing(&frame, test_pg_tests_the_allocator_passes_requests_through_body_0,
				test_pg_tests_the_allocator_passes_requests_through_pad_0);
}

/* test_pg_tests_a_failed_allocation_frees_the_reserve_frame_0_t is the test's state across its pad: its guard. */
typedef struct test_pg_tests_a_failed_allocation_frees_the_reserve_frame_0_t
{
	StdRwLockWriteGuard alone;
	bool		alone_live;
} test_pg_tests_a_failed_allocation_frees_the_reserve_frame_0_t;

/* test_pg_tests_a_failed_allocation_frees_the_reserve_body_0 is the test's body. */
static void
test_pg_tests_a_failed_allocation_frees_the_reserve_body_0(void *frame_)
{
	test_pg_tests_a_failed_allocation_frees_the_reserve_frame_0_t *frame = frame_;
	StdLayout	huge;
	StdLayoutError tried;

	(void) std_rw_lock_write(&TEST_PG_TESTS_RESERVE_TESTS_LOCK, &TEST_PG_TESTS_RESERVE_TESTS, &frame->alone);
	frame->alone_live = true;
	STD_ASSERT(1565, 9, pg_retake_reserve(), "retake_reserve()");
	if (!std_Layout_from_size_align(((size_t) ((size_t) PG_INT64_MAX / 2)), 16, &huge, &tried))
		STD_RESULT_EXPECT_FAILED(1566, 73, "the layout is valid", &tried, std_LayoutError_Debug_fmt);
	STD_ASSERT(1569, 9, pg_Reserving_GlobalAlloc_alloc(&PG_ALLOCATOR, huge.size, huge.align) == NULL,
			   "std::hint::black_box(unsafe { ALLOCATOR.alloc(std::hint::black_box(huge)) }).is_null()");
	STD_ASSERT(1570, 9, pg_atomic_read_membarrier_u64(&PG_RESERVE) == 0, "RESERVE.load(Ordering::SeqCst).is_null()");
	STD_ASSERT(1571, 9, pg_retake_reserve(), "retake_reserve()");
	frame->alone_live = false;
	std_RwLockWriteGuard_drop(&frame->alone);
}

/* test_pg_tests_a_failed_allocation_frees_the_reserve_pad_0 is the test's pad: its guard while it is live. */
static void
test_pg_tests_a_failed_allocation_frees_the_reserve_pad_0(void *frame_)
{
	test_pg_tests_a_failed_allocation_frees_the_reserve_frame_0_t *frame = frame_;

	if (frame->alone_live)
		std_RwLockWriteGuard_drop(&frame->alone);
}

/* test_pg_tests_a_failed_allocation_frees_the_reserve: a failed allocation frees the reserve, so the unwind after it can allocate. */
void
test_pg_tests_a_failed_allocation_frees_the_reserve(void)
{
	test_pg_tests_a_failed_allocation_frees_the_reserve_frame_0_t frame = {0};

	std_landing(&frame, test_pg_tests_a_failed_allocation_frees_the_reserve_body_0,
				test_pg_tests_a_failed_allocation_frees_the_reserve_pad_0);
}

/* pg_tests_OptionRefStr_PartialEq_eq is Option<&str>'s PartialEq, a NULL ptr being None: both None, or both Some with equal bytes. */
static bool
pg_tests_OptionRefStr_PartialEq_eq(const StdStr *self, const StdStr *other)
{
	if (self->ptr == NULL || other->ptr == NULL)
		return self->ptr == NULL && other->ptr == NULL;
	return self->len == other->len && memcmp(self->ptr, other->ptr, self->len) == 0;
}

/* test_pg_tests_fork_errors_encode_their_sqlstate_frame_0_t is the test's state across its pad: the error. */
typedef struct test_pg_tests_fork_errors_encode_their_sqlstate_frame_0_t
{
	PgPgError	error;
	bool		error_live;
} test_pg_tests_fork_errors_encode_their_sqlstate_frame_0_t;

/* test_pg_tests_fork_errors_encode_their_sqlstate_body_0 is the test's body. */
static void
test_pg_tests_fork_errors_encode_their_sqlstate_body_0(void *frame_)
{
	test_pg_tests_fork_errors_encode_their_sqlstate_frame_0_t *frame = frame_;
	StdString	tmp_1;

	std_string_from_Checked(&tmp_1, "could not open file", 19);
	frame->error = pg_PgError_fork((const uint8 *) "58P01", &tmp_1);
	frame->error_live = true;
	{
		int32		left = pg_PgError_sqlerrcode(&frame->error);
		int32		right = (int32) (5 | (8 << 6) | (32 << 12) | (1 << 24));

		STD_ASSERT_EQ(1579, 9, left == right, &left, std_i32_Debug_fmt, &right, std_i32_Debug_fmt);
	}
	{
		StdStr		message = pg_PgError_message(&frame->error);
		StdStr		expected = {"could not open file", 19};
		StdOptionRef left = {message.ptr != NULL, {&message, std_str_Debug_fmt}};
		StdOptionRef right = {true, {&expected, std_str_Debug_fmt}};

		STD_ASSERT_EQ(1580, 9, pg_tests_OptionRefStr_PartialEq_eq(&message, &expected), &left,
					  std_option_Debug_fmt, &right, std_option_Debug_fmt);
	}
	frame->error_live = false;
	pg_PgError_drop(&frame->error);
}

/* test_pg_tests_fork_errors_encode_their_sqlstate_pad_0 is the test's pad: the error while it is live. */
static void
test_pg_tests_fork_errors_encode_their_sqlstate_pad_0(void *frame_)
{
	test_pg_tests_fork_errors_encode_their_sqlstate_frame_0_t *frame = frame_;

	if (frame->error_live)
		pg_PgError_drop(&frame->error);
}

/*
 * test_pg_tests_fork_errors_encode_their_sqlstate: the fork's own error
 * carries its SQLSTATE in Postgres' encoding (MAKE_SQLSTATE), and dropping it
 * needs no raise.
 */
void
test_pg_tests_fork_errors_encode_their_sqlstate(void)
{
	test_pg_tests_fork_errors_encode_their_sqlstate_frame_0_t frame = {0};

	std_landing(&frame, test_pg_tests_fork_errors_encode_their_sqlstate_body_0,
				test_pg_tests_fork_errors_encode_their_sqlstate_pad_0);
}

/* test_pg_tests_borrowed_bytes_are_the_callers: borrowed bytes are the ones the pointer and length name, and a zero length borrows nothing. */
void
test_pg_tests_borrowed_bytes_are_the_callers(void)
{
	uint8		record[9];
	DgBytes		given;
	size_t		len;
	const uint8 *bytes;

	memcpy(record, "versioned", sizeof(record));
	given = (DgBytes)
	{
		(const char *) record, sizeof(record)
	};
	bytes = pg_borrowed(given, &len);
	{
		StdBytes	left = {bytes, len};
		StdBytes	right = {(const uint8 *) "versioned", 9};

		STD_ASSERT_EQ(1590, 9, left.len == right.len && memcmp(left.ptr, right.ptr, right.len) == 0, &left,
					  std_bytes_Debug_fmt, &right, std_bytes_Debug_fmt);
	}
	given = (DgBytes)
	{
		NULL, 0
	};
	(void) pg_borrowed(given, &len);
	STD_ASSERT(1592, 9, len == 0, "unsafe { borrowed(ffi::DgBytes { data: core::ptr::null(), len: 0 }) }.is_empty()");
}

/* TEST_PG_TESTS_BLOCK is the size of the block the heap test allocates. */
#define TEST_PG_TESTS_BLOCK ((size_t) (8 << 20))

/* test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_closure_0 is near: whether a measured count is within a quarter block of the expected one. */
static bool
test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_closure_0(size_t measured, size_t expected)
{
	return std_abs_diff_usize(measured, expected) < ((size_t) (TEST_PG_TESTS_BLOCK / 4));
}

/* test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_frame_0_t is the test's state across its pad: the blocks it makes. */
typedef struct test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_frame_0_t
{
	StdVec		block;
	bool		block_live;
	StdVec		zeroed;
	bool		zeroed_live;
} test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_frame_0_t;

/* test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_body_0 is the test's body. */
static void
test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_body_0(void *frame_)
{
	test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_frame_0_t *frame = frame_;
	size_t		before = pg_heap_in_use();

	std_vec_with_capacity_Checked(&frame->block, TEST_PG_TESTS_BLOCK, 1, 1);
	frame->block_live = true;
	STD_ASSERT(1603, 9,
			   test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_closure_0(pg_heap_in_use(),
																								   STD_ADD(usize, before, TEST_PG_TESTS_BLOCK, 1603, 37)),
			   "near(heap_in_use(), before + BLOCK)");
	std_vec_reserve_exact_Checked(&frame->block, ((size_t) (2 * TEST_PG_TESTS_BLOCK)), 1, 1);
	STD_ASSERT(1605, 9,
			   test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_closure_0(pg_heap_in_use(),
																								   STD_ADD(usize, before, ((size_t) (2 * TEST_PG_TESTS_BLOCK)), 1605, 37)),
			   "near(heap_in_use(), before + 2 * BLOCK)");
	frame->block_live = false;
	std_vec_free_Checked(&frame->block, 1, 1);
	STD_ASSERT(1607, 9,
			   test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_closure_0(pg_heap_in_use(), before),
			   "near(heap_in_use(), before)");
	std_vec_u8_from_elem(&frame->zeroed, 0, TEST_PG_TESTS_BLOCK);
	frame->zeroed_live = true;
	STD_ASSERT(1609, 9,
			   test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_closure_0(pg_heap_in_use(),
																								   STD_ADD(usize, before, TEST_PG_TESTS_BLOCK, 1609, 37)),
			   "near(heap_in_use(), before + BLOCK)");
	frame->zeroed_live = false;
	std_vec_free_Checked(&frame->zeroed, 1, 1);
	STD_ASSERT(1611, 9,
			   test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_closure_0(pg_heap_in_use(), before),
			   "near(heap_in_use(), before)");
}

/* test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_pad_0 is the test's pad: each block while it is live. */
static void
test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_pad_0(void *frame_)
{
	test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_frame_0_t *frame = frame_;

	if (frame->zeroed_live)
		std_vec_free_Checked(&frame->zeroed, 1, 1);
	if (frame->block_live)
		std_vec_free_Checked(&frame->block, 1, 1);
}

/*
 * test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free:
 * the count of heap bytes in use rises by a block's size when it is
 * allocated or grown and falls when it is freed, within what the other
 * tests' threads allocate meanwhile, which is far less than the block.
 */
void
test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free(void)
{
	test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_frame_0_t frame = {0};

	std_landing(&frame, test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_body_0,
				test_pg_tests_the_heap_in_use_follows_a_blocks_allocation_growth_and_free_pad_0);
}

/* PgTestsSliceMutTupleTableSlot is a &[*mut TupleTableSlot], as the slots test compares them. */
typedef struct PgTestsSliceMutTupleTableSlot
{
	TupleTableSlot *const *ptr;
	size_t		len;
} PgTestsSliceMutTupleTableSlot;

/* pg_tests_SliceMutTupleTableSlot_Debug_fmt is [*mut TupleTableSlot]'s Debug, which Vec's and an array's give: a list of each pointer's. */
static bool
pg_tests_SliceMutTupleTableSlot_Debug_fmt(const void *value, StdFormatter *f)
{
	const PgTestsSliceMutTupleTableSlot *self = value;
	StdDebugList b = std_fmt_debug_list(f);

	for (size_t i = 0, i_len = self->len; i < i_len; i++)
		std_DebugList_entry(&b, STD_PTR_AT(self->ptr, i, sizeof(TupleTableSlot *)), std_ptr_Debug_fmt);
	return std_DebugList_finish(&b);
}

/* test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count_frame_0_t is the test's state across its pad: what it makes. */
typedef struct test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count_frame_0_t
{
	StdVec		passed;
	bool		passed_live;
} test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count_frame_0_t;

/* test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count_body_0 is the test's body. */
static void
test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count_body_0(void *frame_)
{
	test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count_frame_0_t *frame = frame_;
	uint8		places[2] = {0};
	TupleTableSlot *first;
	TupleTableSlot *second;
	TupleTableSlot *given[2];
	PgSlot	  **slice;
	size_t		slice_len;
	PgPgError	error;
	StdSizeHint hint;
	bool		ok;

	first = (TupleTableSlot *) &places[0];
	second = (TupleTableSlot *) &places[1];
	given[0] = first;
	given[1] = second;
	if (!pg_slots(given, 2, &slice, &slice_len, &error))
	{
		pg_PgError_drop(&error);
		STD_PANIC(1623, 74, "two slots were refused");
	}
	hint = (StdSizeHint)
	{
		slice_len, true, slice_len
	};
	std_vec_with_capacity_trusted(&frame->passed, hint, sizeof(TupleTableSlot *), _Alignof(TupleTableSlot *));
	frame->passed_live = true;
	std_vec_extend_trusted_reserve(&frame->passed, hint, sizeof(TupleTableSlot *), _Alignof(TupleTableSlot *));
	for (size_t i = 0, i_len = slice_len; i < i_len; i++)
	{
		STD_VEC_AT(&frame->passed, TupleTableSlot *, frame->passed.len) =
			(TupleTableSlot *) *(PgSlot **) STD_PTR_AT(slice, i, sizeof(PgSlot *));
		frame->passed.len = std_wrapping_add_usize(frame->passed.len, 1);
	}
	{
		TupleTableSlot *expected[2] = {first, second};
		PgTestsSliceMutTupleTableSlot left = {frame->passed.ptr, frame->passed.len};
		PgTestsSliceMutTupleTableSlot right = {expected, lengthof(expected)};

		STD_ASSERT_EQ(1625, 9,
					  left.len == right.len && memcmp(left.ptr, right.ptr, sizeof(expected)) == 0, &left,
					  pg_tests_SliceMutTupleTableSlot_Debug_fmt, &right, pg_tests_SliceMutTupleTableSlot_Debug_fmt);
	}
	ok = pg_slots(NULL, 0, &slice, &slice_len, &error);
	if (!ok)
		pg_PgError_drop(&error);
	STD_ASSERT(1627, 9, ok && slice_len == 0, "unsafe { slots(ptr::null_mut(), 0) }.is_ok_and(|none| none.is_empty())");
	ok = pg_slots(NULL, -1, &slice, &slice_len, &error);
	{
		bool		held = false;

		if (!ok)
		{
			StdStr		message = pg_PgError_message(&error);
			StdStr		expected = {"a multi-row insert was handed -1 slots", 38};

			held = pg_tests_OptionRefStr_PartialEq_eq(&message, &expected);
			pg_PgError_drop(&error);
		}
		STD_ASSERT(1631, 9, held,
				   "refused.is_err_and(|error| error.message() == Some(\"a multi-row insert was handed -1 slots\"))");
	}
	frame->passed_live = false;
	std_vec_free_Checked(&frame->passed, sizeof(TupleTableSlot *), _Alignof(TupleTableSlot *));
}

/* test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count_pad_0 is the test's pad: the passed slots while they are live. */
static void
test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count_pad_0(void *frame_)
{
	test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count_frame_0_t *frame = frame_;

	if (frame->passed_live)
		std_vec_free_Checked(&frame->passed, sizeof(TupleTableSlot *), _Alignof(TupleTableSlot *));
}

/*
 * test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count: a
 * multi-row insert's slots are one slice of their count, each the slot
 * Postgres passed, none for a count of zero, and a negative count is an
 * internal error.
 */
void
test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count(void)
{
	test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count_frame_0_t frame = {0};

	std_landing(&frame, test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count_body_0,
				test_pg_tests_a_multi_row_inserts_slots_are_one_slice_of_their_count_pad_0);
}

/* test_pg_tests_a_toast_slices_bytes_are_past_its_header: a TOAST slice's bytes are the varlena's past its header, as many as its length, and a negative length is refused. */
void
test_pg_tests_a_toast_slices_bytes_are_past_its_header(void)
{
	uint8		varlena_[10];
	uint8	   *slice;
	size_t		slice_len;
	PgPgError	error;
	bool		ok;

	memset(varlena_, 9, sizeof(varlena_));
	if (!pg_toast_slice(varlena_, 6, &slice, &slice_len, &error))
	{
		pg_PgError_drop(&error);
		STD_PANIC(1641, 13, "six bytes were refused");
	}
	std_slice_copy_from_slice_Checked(slice, slice_len, "sliced", 6, 1, 1, RUST_FILE, 1643, 15);
	{
		StdBytes	left = {varlena_, sizeof(varlena_)};
		StdBytes	right = {(const uint8 *) "\t\t\t\tsliced", 10};

		STD_ASSERT_EQ(1644, 9, memcmp(left.ptr, right.ptr, right.len) == 0, &left, std_bytes_Debug_fmt, &right,
					  std_bytes_Debug_fmt);
	}
	ok = pg_toast_slice(NULL, -1, &slice, &slice_len, &error);
	{
		bool		held = false;

		if (!ok)
		{
			StdStr		message = pg_PgError_message(&error);
			StdStr		expected = {"a TOAST slice of -1 bytes was asked for", 39};

			held = pg_tests_OptionRefStr_PartialEq_eq(&message, &expected);
			pg_PgError_drop(&error);
		}
		STD_ASSERT(1648, 9, held,
				   "refused.is_err_and(|error| error.message() == Some(\"a TOAST slice of -1 bytes was asked for\"))");
	}
}

#endif							/* DOLTGRES_TEST_PG */

#endif							/* FRONTEND */
