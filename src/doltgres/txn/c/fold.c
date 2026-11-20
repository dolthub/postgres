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
 * The fold of a storage's delta into its base (txn's fold.rs): which
 * versions a fold at a fold point applies, leaves, and drops; a key's and a
 * storage's fold; the install of a folded, built, or copied base under the
 * registration that holds its roots, and its record's redo; the drop of the
 * versions an install folded; and the chain of a storage's transforms.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/chunk.h"
#include "doltgres/chunks/header.h"
#include "doltgres/chunks/packs/roots.h"
#include "doltgres/host/delta.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "doltgres/trees/chunker.h"
#include "doltgres/trees/cursor.h"
#include "doltgres/trees/mutate.h"
#include "doltgres/trees/published.h"
#include "doltgres/txn/counted.h"
#include "doltgres/txn/entry.h"
#include "doltgres/txn/fold.h"
#include "doltgres/txn/free_space.h"
#include "doltgres/txn/memtable/branch.h"
#include "doltgres/txn/memtable/map.h"
#include "doltgres/txn/memtable/table.h"
#include "doltgres/txn/memtable/tuples.h"
#include "doltgres/txn/outcome.h"
#include "doltgres/txn/rewrite.h"
#include "doltgres/txn/runs.h"
#include "doltgres/txn/spill.h"
#include "doltgres/txn/state.h"
#include "doltgres/txn/status.h"
#include "doltgres/txn/vacuum.h"
#include "doltgres/txn/values.h"

#define RUST_FILE "txn" STD_SEP "src" STD_SEP "fold.rs"

/* TxnFoldBased is Based: a key's base entry as a fold reads it, its value, addresses, value ids, and copies' stamps. */
typedef struct TxnFoldBased
{
	/* value is a Vec<u8>. */
	StdVec		value;
	/* addresses is a Vec<(u32, Locator)>. */
	StdVec		addresses;
	/* ids is a Vec<u32>. */
	StdVec		ids;
	/* stored is a Vec<Stamp>. */
	StdVec		stored;
} TxnFoldBased;

/* The pin whose scope reads a key's base entry, or none. */
#define TREES_PIN_SCOPE static
#define TREES_PIN_DECLARE
#define TREES_PIN_DEFINE
#define TREES_PIN_RS ResultOptionBased
#define TREES_PIN_RETURNS bool
#define TREES_PIN_RESULT_PARAMS , bool *has_result, TxnFoldBased *result, ChunksErrorError *error
#define TREES_PIN_RESULT_FIELDS bool *has_result; TxnFoldBased *result; ChunksErrorError *error;
#define TREES_PIN_RESULT_INIT .has_result = has_result, .result = result, .error = error
#define TREES_PIN_RESULT_ARGS , frame->has_result, frame->result, frame->error
#include "doltgres/trees/pin_template.h"

/* The element layouts of the vectors the fold builds. */
#define TXN_FOLD_BYTES 1, 1
#define TXN_FOLD_U32 sizeof(uint32), _Alignof(uint32)
#define TXN_FOLD_U64 sizeof(uint64), _Alignof(uint64)
#define TXN_FOLD_POINTER sizeof(void *), _Alignof(void *)
#define TXN_FOLD_ADDRESS sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t)
#define TXN_FOLD_STAMP sizeof(ChunksTrailerStamp), _Alignof(ChunksTrailerStamp)
#define TXN_FOLD_LOCATOR sizeof(ChunksTrailerLocator), _Alignof(ChunksTrailerLocator)
#define TXN_FOLD_EDIT sizeof(TreesMutateEdit), _Alignof(TreesMutateEdit)
#define TXN_FOLD_ENTRY sizeof(TreesBuildEntry), _Alignof(TreesBuildEntry)
#define TXN_FOLD_HANDLE sizeof(TxnMemtableVersionHandle), _Alignof(TxnMemtableVersionHandle)
#define TXN_FOLD_VERSION sizeof(TxnMemtableVersionVersion), _Alignof(TxnMemtableVersionVersion)
#define TXN_FOLD_CTID sizeof(TxnFoldCtidEdit), _Alignof(TxnFoldCtidEdit)
#define TXN_FOLD_PENDED sizeof(txn_TupleU64U64VecU32_t), _Alignof(txn_TupleU64U64VecU32_t)
#define TXN_FOLD_PENDING_ROW sizeof(TxnFoldPendingRow), _Alignof(TxnFoldPendingRow)
#define TXN_FOLD_WRITTEN sizeof(txn_TupleU64U32_t), _Alignof(txn_TupleU64U32_t)
#define TXN_FOLD_FORMED_BY sizeof(txn_OptionTupleU64U32_t), _Alignof(txn_OptionTupleU64U32_t)
#define TXN_FOLD_PAIR sizeof(txn_TupleU64Bool_t), _Alignof(txn_TupleU64Bool_t)
#define TXN_FOLD_PACK sizeof(ChunksPackPackId), _Alignof(ChunksPackPackId)
#define TXN_FOLD_CHAIN sizeof(TxnBasePending), _Alignof(TxnBasePending)
#define TXN_FOLD_LIVE sizeof(TxnVisibleLive), _Alignof(TxnVisibleLive)
#define TXN_FOLD_COPY sizeof(txn_TupleLocatorLocator_t), _Alignof(txn_TupleLocatorLocator_t)

/* txn_fold_bytes copies bytes into a new vector, as to_vec does. */
static void
txn_fold_bytes(const uint8 *bytes, size_t bytes_len, StdVec *result)
{
	std_vec_with_capacity_Checked(result, bytes_len, TXN_FOLD_BYTES);
	std_vec_extend_from_slice_Checked(result, bytes, bytes_len, TXN_FOLD_BYTES);
}

/* txn_fold_Live_drop frees a live transform held as a vector's element. */
static void
txn_fold_Live_drop(void *live)
{
	txn_base_Pending_drop(&((TxnVisibleLive *) live)->f0);
}

/* txn_fold_Pending_drop frees a chain's entry held as a vector's element. */
static void
txn_fold_Pending_drop(void *pending)
{
	txn_base_Pending_drop(pending);
}

static bool txn_fold_in_order(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
							  const HostSharedHeld *held, const TreesKeyOrder *order, bool keyless, StdVec *warned,
							  const TxnInsertPage *page, uint64 upto, bool *installed, bool *left, HostError *error);
static bool txn_fold_bounded(HostIoIo *io, const HostSharedHeld *held, HostLsn point, uint64 horizon,
							 HostLsn *result, HostError *error);

/*
 * txn_fold_mutated applies edits, taking them once it reaches the mutate, to
 * a tree or to an empty one a branded writer writes, and answers the new
 * root's place in the writer.
 */
static bool
txn_fold_mutated(TreesStoreBrandedWriter *writer, HostIoIo *io, bool has_tree, TreesStoreTree tree, StdVec *edits,
				 bool *edits_live, const TreesShape *shape, size_t *result, HostError *error)
{
	ChunksErrorError failed;

	if (!has_tree)
	{
		size_t		empty;
		TreesStoreTreeRef root;

		if (!trees_store_BrandedWriter_empty(writer, io, &empty, &failed))
		{
			*error = txn_base_storage(&failed);
			return false;
		}
		root = trees_store_BrandedWriter_root(writer, empty);
		tree = trees_store_Tree_at(root.address, root.locator);
	}
	*edits_live = false;
	if (!trees_store_BrandedWriter_mutate(writer, io, TREES_STORE_TREE_NAMED(&tree), TREES_CHUNKER_DEFAULT_TARGET,
										  edits, shape, result, &failed))
	{
		*error = txn_base_storage(&failed);
		return false;
	}
	return true;
}


/* txn_fold_fork returns an error of a code and a message. */
static HostError
txn_fold_fork(const char *code, const char *text)
{
	StdString	message;

	std_string_from_Checked(&message, text, strlen(text));
	return host_Error_fork((const uint8 *) code, &message);
}

/* txn_fold_forked returns an error of a code and a message with one number between its two parts. */
static HostError
txn_fold_forked(const char *code, const char *before, uint64 number, const char *after)
{
	StdFmtPiece pieces[] = {{before, strlen(before), 0, {0}}, STD_FMT_ARG(0), {after, strlen(after), 0, {0}}};
	StdFmtArg	args[] = {{&number, std_u64_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};
	StdString	message;

	std_fmt_format_Checked(&message, &arguments);
	return host_Error_fork((const uint8 *) code, &message);
}

/*
 * txn_fold_applied decides what a fold at a fold point does with a version:
 * the commit log first, so an aborted own writer, which has no status entry,
 * is dropped with no lookup and a running one stays; then a committed one is
 * applied when its top-level transaction's commit record ends at or below
 * the point, and stays otherwise. A committed writer's row lock is dropped
 * with no lookup, since a lock counts at no status entry.
 */
bool
txn_fold_applied(HostIoIo *io, TxnMemtableVersionVersion *version, HostLsn point, TxnFoldApplied *result,
				 HostError *error)
{
	HostStatus	status;
	TxnMemtableVersionRecord record;
	TxnMemtableVersionOpRef op;
	uint64		lsn;
	bool		has_lsn;

	if (!txn_outcome_outcome(io, version, &status, error))
		return false;
	record = txn_memtable_version_Version_record(version);
	op = txn_memtable_version_Record_op(&record);
	if (status == HOST_STATUS_COMMITTED &&
		(op.tag == TXN_MEMTABLE_VERSION_OP_REF_LOCK || op.tag == TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW))
	{
		result->tag = TXN_FOLD_APPLIED_DROPPED;
		return true;
	}
	has_lsn = txn_memtable_version_Record_lsn(&record, &lsn);
	return txn_fold_applied_by(io, txn_memtable_version_Record_writer(&record), has_lsn, lsn, status, point, result,
							   error);
}

/*
 * txn_fold_applied_by is applied's decision for a version's writer, its
 * record's LSN, and the outcome the commit log gave its own xid, which a
 * run's writer reads from a version's stored record.
 */
bool
txn_fold_applied_by(HostIoIo *io, TxnMemtableVersionWriter writer, bool has_lsn, uint64 lsn, HostStatus status,
					HostLsn point, TxnFoldApplied *result, HostError *error)
{
	uint64		top = writer.has_top ? writer.top : writer.xid;
	HostXid		xid = {top};
	TxnStatusCommit commit;

	switch (status)
	{
		case HOST_STATUS_ABORTED:
			result->tag = TXN_FOLD_APPLIED_DROPPED;
			return true;
		case HOST_STATUS_IN_PROGRESS:
			result->tag = TXN_FOLD_APPLIED_STAYS;
			return true;
		case HOST_STATUS_COMMITTED:
			break;
	}
	if (top < TXN_STATUS_FIRST_NORMAL)
	{
		if (has_lsn && lsn > point)
			result->tag = TXN_FOLD_APPLIED_STAYS;
		else
		{
			result->tag = TXN_FOLD_APPLIED_YES;
			result->u.yes.has_top = false;
		}
		return true;
	}
	if (!txn_status_committed(io, xid, &commit, error))
		return false;
	switch (commit.tag)
	{
		case TXN_STATUS_COMMIT_AT:
			if (commit.u.at <= point)
			{
				result->tag = TXN_FOLD_APPLIED_YES;
				result->u.yes.has_top = true;
				result->u.yes.top = top;
				return true;
			}
			result->tag = TXN_FOLD_APPLIED_STAYS;
			return true;
		case TXN_STATUS_COMMIT_PENDING:
			result->tag = TXN_FOLD_APPLIED_STAYS;
			return true;
		case TXN_STATUS_COMMIT_MISSING:
			break;
	}
	*error = txn_fold_forked("XX001", "committed transaction ", top, " has no commit status entry");
	return false;
}

/*
 * txn_fold_Applied_PartialEq_eq is Applied's derived PartialEq: the same
 * variant, and for Yes the same top-level xid.
 */
bool
txn_fold_Applied_PartialEq_eq(const TxnFoldApplied *self, const TxnFoldApplied *other)
{
	if (self->tag != other->tag)
		return false;
	if (self->tag != TXN_FOLD_APPLIED_YES)
		return true;
	return self->u.yes.has_top == other->u.yes.has_top && (!self->u.yes.has_top || self->u.yes.top == other->u.yes.top);
}

/* txn_fold_Applied_Debug_fmt is Applied's derived Debug. */
bool
txn_fold_Applied_Debug_fmt(const void *value, StdFormatter *f)
{
	const TxnFoldApplied *self = value;
	StdOptionRef top = {.has = self->u.yes.has_top, .some = {&self->u.yes.top, std_u64_Debug_fmt}};
	StdDebugTuple t;

	switch (self->tag)
	{
		case TXN_FOLD_APPLIED_STAYS:
			return std_fmt_write_str(f, "Stays", 5);
		case TXN_FOLD_APPLIED_DROPPED:
			return std_fmt_write_str(f, "Dropped", 7);
		case TXN_FOLD_APPLIED_YES:
			break;
	}
	t = std_fmt_debug_tuple(f, "Yes", 3);
	std_DebugTuple_field(&t, &top, std_option_Debug_fmt);
	return std_DebugTuple_finish(&t);
}

/* txn_fold_applied_yes reports whether a fold at a point applies a version. */
static bool
txn_fold_applied_yes(HostIoIo *io, TxnMemtableVersionVersion *version, HostLsn point, bool *result,
					 HostError *error)
{
	TxnFoldApplied applied;

	if (!txn_fold_applied(io, version, point, &applied, error))
		return false;
	*result = applied.tag == TXN_FOLD_APPLIED_YES;
	return true;
}

/* txn_fold_Folding_Rule_begun is Folding's begun: whether the fold applies the row's version. */
static bool
txn_fold_Folding_Rule_begun(const void *self, HostIoIo *io, const TxnVisibleRowVersion *row, TxnVisibleDirty *dirty,
							bool *result, HostError *error)
{
	const TxnFoldFolding *folding = self;

	return txn_fold_applied_yes(io, row->version, folding->f0, result, error);
}

/* txn_fold_Folding_Rule_ended is Folding's ended: whether the fold applies the ender. */
static bool
txn_fold_Folding_Rule_ended(const void *self, HostIoIo *io, TxnMemtableVersionVersion *ender,
							TxnVisibleDirty *dirty, bool *result, HostError *error)
{
	const TxnFoldFolding *folding = self;

	return txn_fold_applied_yes(io, ender, folding->f0, result, error);
}

const TxnVisibleRuleOps TXN_FOLD_FOLDING_RULE_OPS = {
	.begun = txn_fold_Folding_Rule_begun,
	.ended = txn_fold_Folding_Rule_ended,
};

/*
 * txn_fold_Committing_applies reports whether the rule applies a version:
 * the committing transaction's own unless its subtransaction rolled back,
 * else one applied at any commit point.
 */
static bool
txn_fold_Committing_applies(const TxnFoldCommitting *self, HostIoIo *io, TxnMemtableVersionVersion *version,
							bool *result, HostError *error)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);
	TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(&record);
	HostStatus	status;

	if (self->own != 0 && (writer.has_top ? writer.top : writer.xid) == self->own)
	{
		if (!txn_outcome_outcome(io, version, &status, error))
			return false;
		*result = status != HOST_STATUS_ABORTED;
		return true;
	}
	return txn_fold_applied_yes(io, version, PG_UINT64_MAX, result, error);
}

/* txn_fold_Committing_Rule_begun is Committing's begun: whether the rule applies the row's version. */
static bool
txn_fold_Committing_Rule_begun(const void *self, HostIoIo *io, const TxnVisibleRowVersion *row,
							   TxnVisibleDirty *dirty, bool *result, HostError *error)
{
	return txn_fold_Committing_applies(self, io, row->version, result, error);
}

/* txn_fold_Committing_Rule_ended is Committing's ended: whether the rule applies the ender. */
static bool
txn_fold_Committing_Rule_ended(const void *self, HostIoIo *io, TxnMemtableVersionVersion *ender,
							   TxnVisibleDirty *dirty, bool *result, HostError *error)
{
	return txn_fold_Committing_applies(self, io, ender, result, error);
}

const TxnVisibleRuleOps TXN_FOLD_COMMITTING_RULE_OPS = {
	.begun = txn_fold_Committing_Rule_begun,
	.ended = txn_fold_Committing_Rule_ended,
};

/* txn_fold_Every_Rule_begun is Every's begun: every row begun. */
static bool
txn_fold_Every_Rule_begun(const void *self, HostIoIo *io, const TxnVisibleRowVersion *row, TxnVisibleDirty *dirty,
						  bool *result, HostError *error)
{
	*result = true;
	return true;
}

/* txn_fold_Every_Rule_ended is Every's ended: no row ended. */
static bool
txn_fold_Every_Rule_ended(const void *self, HostIoIo *io, TxnMemtableVersionVersion *ender, TxnVisibleDirty *dirty,
						  bool *result, HostError *error)
{
	*result = false;
	return true;
}

/* TXN_FOLD_EVERY_RULE_OPS is Every's Rule: every row begun and none ended. */
static const TxnVisibleRuleOps TXN_FOLD_EVERY_RULE_OPS = {
	.begun = txn_fold_Every_Rule_begun,
	.ended = txn_fold_Every_Rule_ended,
};

/* TXN_FOLD_EVERY is the rule under which a drop reads the rows it takes out. */
static const TxnVisibleRule TXN_FOLD_EVERY = {&TXN_FOLD_EVERY_RULE_OPS, NULL};

/* txn_fold_sized returns the sizes of a base entry of a key with a value and a count of rows, its heap bytes aside. */
static TxnBaseSizes
txn_fold_sized(const uint8 *key, size_t key_len, const uint8 *value, size_t value_len, size_t rows)
{
	TxnBaseSizes sizes = {.entries = 1,
		.leaf_bytes = trees_chunker_leaf_entry_size(key, key_len, value, value_len),
	.key_bytes = key_len, .rows = rows};

	return sizes;
}

/* txn_fold_tuple_order orders tuple ids ascending, for qsort. */
static int
txn_fold_tuple_order(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return (x > y) - (x < y);
}

/* txn_fold_tuples returns the tuple ids of stamps, ascending with no repeats, as a BTreeSet holds them. */
static void
txn_fold_tuples(const ChunksTrailerStamp *stamps, size_t stamps_len, StdVec *result)
{
	size_t		kept = 0;

	std_vec_with_capacity_Checked(result, stamps_len, TXN_FOLD_U64);
	for (size_t i = 0; i < stamps_len; i++)
		STD_VEC_AT(result, uint64, i) = stamps[i].tuple;
	result->len = stamps_len;
	qsort(result->ptr, result->len, sizeof(uint64), txn_fold_tuple_order);
	for (size_t i = 0; i < result->len; i++)
		if (kept == 0 || STD_VEC_AT(result, uint64, kept - 1) != STD_VEC_AT(result, uint64, i))
			STD_VEC_AT(result, uint64, kept++) = STD_VEC_AT(result, uint64, i);
	result->len = kept;
}

/* txn_fold_holds reports whether a set of tuple ids, ascending, holds one. */
static bool
txn_fold_holds(const StdVec *tuples, uint64 tuple)
{
	return bsearch(&tuple, tuples->ptr, tuples->len, sizeof(uint64), txn_fold_tuple_order) != NULL;
}

/* txn_fold_CtidEdit_drop frees a tuple id edit's key. */
void
txn_fold_CtidEdit_drop(void *edit_)
{
	TxnFoldCtidEdit *edit = edit_;

	if (edit->has_f1)
		std_vec_free_Checked(&edit->f1.f0, TXN_FOLD_BYTES);
}

/* txn_fold_ctid_edits_frame_t is ctid_edits' state across its pad: the two sets of tuple ids, each while live. */
typedef struct txn_fold_ctid_edits_frame_t
{
	const uint8 *key;
	size_t		key_len;
	const ChunksTrailerStamp *old;
	size_t		old_len;
	const ChunksTrailerStamp *new;
	size_t		new_len;
	const txn_TupleU64U32_t *written;
	size_t		written_len;
	StdVec	   *out;
	StdVec		olds;
	bool		olds_live;
	StdVec		news;
	bool		news_live;
	HostError  *error;
	bool		ok;
} txn_fold_ctid_edits_frame_t;

/* txn_fold_ctid_edits_body adds the old tuple ids' removals, then the new ones' pairs with their rows' heap bytes. */
static void
txn_fold_ctid_edits_body(void *frame_)
{
	txn_fold_ctid_edits_frame_t *frame = frame_;

	txn_fold_tuples(frame->old, frame->old_len, &frame->olds);
	frame->olds_live = true;
	txn_fold_tuples(frame->new, frame->new_len, &frame->news);
	frame->news_live = true;
	for (size_t i = 0; i < frame->olds.len; i++)
	{
		uint64		tuple = STD_VEC_AT(&frame->olds, uint64, i);
		TxnFoldCtidEdit edit = {.f0 = tuple};

		if (!txn_fold_holds(&frame->news, tuple))
			std_vec_push_Checked(frame->out, &edit, TXN_FOLD_CTID);
	}
	for (size_t i = 0; i < frame->news.len; i++)
	{
		uint64		tuple = STD_VEC_AT(&frame->news, uint64, i);
		TxnFoldCtidEdit edit = {.f0 = tuple, .has_f1 = true};
		size_t		at;

		if (txn_fold_holds(&frame->olds, tuple))
			continue;
		for (at = 0; at < frame->written_len && frame->written[at].f0 != tuple; at++)
			;
		if (at == frame->written_len)
		{
			*frame->error = txn_fold_forked("XX000", "a fold's new stamp of tuple id ", tuple, " names no row");
			return;
		}
		edit.f1.f1 = frame->written[at].f1;
		txn_fold_bytes(frame->key, frame->key_len, &edit.f1.f0);
		std_vec_push_Checked(frame->out, &edit, TXN_FOLD_CTID);
	}
	frame->ok = true;
}

/* txn_fold_ctid_edits_pad frees what ctid_edits holds live: its two sets. */
static void
txn_fold_ctid_edits_pad(void *frame_)
{
	txn_fold_ctid_edits_frame_t *frame = frame_;

	if (frame->news_live)
		std_vec_free_Checked(&frame->news, TXN_FOLD_U64);
	if (frame->olds_live)
		std_vec_free_Checked(&frame->olds, TXN_FOLD_U64);
	frame->news_live = frame->olds_live = false;
}

/*
 * txn_fold_ctid_edits adds the tuple id tree's edits that a fold's edit of a
 * key over a base makes: the tuple ids of the stamps of the base's entry it
 * drops, and those of the stamps it writes, with the key and the heap bytes
 * of the delta's row each one names, which only the delta's rows add.
 */
static bool
txn_fold_ctid_edits(const uint8 *key, size_t key_len, const ChunksTrailerStamp *old, size_t old_len,
					const ChunksTrailerStamp *new, size_t new_len, const txn_TupleU64U32_t *written,
					size_t written_len, StdVec *out, HostError *error)
{
	txn_fold_ctid_edits_frame_t frame = {.key = key, .key_len = key_len, .old = old, .old_len = old_len, .new = new,
	.new_len = new_len, .written = written, .written_len = written_len, .out = out, .error = error};

	std_landing(&frame, txn_fold_ctid_edits_body, txn_fold_ctid_edits_pad);
	txn_fold_ctid_edits_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_ctid_order orders tuple id edits by their tuple ids, later ones
 * after earlier ones of the same, for qsort.
 */
static int
txn_fold_ctid_order(const void *a, const void *b)
{
	const TxnFoldCtidEdit *const *x = a;
	const TxnFoldCtidEdit *const *y = b;

	if ((*x)->f0 != (*y)->f0)
		return ((*x)->f0 > (*y)->f0) - ((*x)->f0 < (*y)->f0);
	return (*x > *y) - (*x < *y);
}

/*
 * txn_fold_apply_ctids_frame_t is apply_ctids' state across its pad: the
 * sorted edits and the tree's edits, each while live.
 */
typedef struct txn_fold_apply_ctids_frame_t
{
	HostIoIo   *io;
	TreesStoreBrandedWriter *writer;
	bool		has_ctids;
	TreesStoreTree ctids;
	const TxnFoldCtidEdit *edits;
	size_t		edits_len;
	StdVec		sorted;
	bool		sorted_live;
	StdVec		tree;
	bool		tree_live;
	StdVec		named;
	bool		named_live;
	TreesShape	shape;
	bool		shape_live;
	size_t	   *result;
	HostError  *error;
	bool		ok;
} txn_fold_apply_ctids_frame_t;

/*
 * txn_fold_apply_ctids_body keeps each tuple id's last edit in tuple id order,
 * then mutates the tree, or an empty one.
 */
static void
txn_fold_apply_ctids_body(void *frame_)
{
	txn_fold_apply_ctids_frame_t *frame = frame_;

	std_vec_with_capacity_Checked(&frame->sorted, frame->edits_len, TXN_FOLD_POINTER);
	frame->sorted_live = true;
	for (size_t i = 0; i < frame->edits_len; i++)
	{
		const TxnFoldCtidEdit *edit = &frame->edits[i];

		std_vec_push_Checked(&frame->sorted, &edit, TXN_FOLD_POINTER);
	}
	qsort(frame->sorted.ptr, frame->sorted.len, sizeof(void *), txn_fold_ctid_order);
	std_vec_new(&frame->tree, TXN_FOLD_EDIT);
	frame->tree_live = true;
	for (size_t i = 0; i < frame->sorted.len; i++)
	{
		const TxnFoldCtidEdit *edit = STD_VEC_AT(&frame->sorted, const TxnFoldCtidEdit *, i);
		TreesMutateEdit tree;
		uint8		key[6];

		if (i + 1 < frame->sorted.len && STD_VEC_AT(&frame->sorted, const TxnFoldCtidEdit *, i + 1)->f0 == edit->f0)
			continue;
		txn_base_tuple_id_key(edit->f0, key);
		if (edit->has_f1)
		{
			StdVec		value;

			txn_fold_bytes(key, sizeof(key), &frame->named);
			frame->named_live = true;
			txn_base_pair_value(edit->f1.f0.ptr, edit->f1.f0.len, edit->f1.f1, &value);
			frame->named_live = false;
			tree.tag = TREES_MUTATE_EDIT_PUT;
			trees_build_Entry_new_VecU8VecU8(&frame->named, &value, &tree.u.put);
		}
		else
		{
			tree.tag = TREES_MUTATE_EDIT_DELETE;
			txn_fold_bytes(key, sizeof(key), &tree.u.delete);
		}
		std_vec_push_Checked(&frame->tree, &tree, TXN_FOLD_EDIT);
	}
	frame->sorted_live = false;
	std_vec_free_Checked(&frame->sorted, TXN_FOLD_POINTER);
	frame->shape = trees_Shape_Default_default();
	frame->shape_live = true;
	if (!txn_fold_mutated(frame->writer, frame->io, frame->has_ctids, frame->ctids, &frame->tree, &frame->tree_live,
						  &frame->shape, frame->result, frame->error))
		return;
	frame->ok = true;
}

/* txn_fold_apply_ctids_pad frees what apply_ctids holds live: its shape, the tree's edits, and the sorted edits. */
static void
txn_fold_apply_ctids_pad(void *frame_)
{
	txn_fold_apply_ctids_frame_t *frame = frame_;

	if (frame->shape_live)
		trees_Shape_drop(&frame->shape);
	if (frame->named_live)
		std_vec_free_Checked(&frame->named, TXN_FOLD_BYTES);
	if (frame->tree_live)
		std_vec_drop_Checked(&frame->tree, TXN_FOLD_EDIT, trees_mutate_Edit_drop);
	if (frame->sorted_live)
		std_vec_free_Checked(&frame->sorted, TXN_FOLD_POINTER);
	frame->shape_live = frame->named_live = frame->tree_live = frame->sorted_live = false;
}

/*
 * txn_fold_apply_ctids applies tuple id edits, later ones winning, to a tuple
 * id tree, or to an empty one, and returns its root, its pairs' values the
 * key and then the heap bytes.
 */
static bool
txn_fold_apply_ctids(HostIoIo *io, TreesStoreBrandedWriter *writer, bool has_ctids, TreesStoreTree ctids,
					 const TxnFoldCtidEdit *edits, size_t edits_len, size_t *result, HostError *error)
{
	txn_fold_apply_ctids_frame_t frame = {.io = io, .writer = writer, .has_ctids = has_ctids, .ctids = ctids,
	.edits = edits, .edits_len = edits_len, .result = result, .error = error};

	std_landing(&frame, txn_fold_apply_ctids_body, txn_fold_apply_ctids_pad);
	txn_fold_apply_ctids_pad(&frame);
	return frame.ok;
}

/* txn_fold_Based_drop frees a key's base entry as a fold reads it. */
static void
txn_fold_Based_drop(TxnFoldBased *self)
{
	std_vec_free_Checked(&self->value, TXN_FOLD_BYTES);
	std_vec_free_Checked(&self->addresses, TXN_FOLD_ADDRESS);
	std_vec_free_Checked(&self->ids, TXN_FOLD_U32);
	std_vec_free_Checked(&self->stored, TXN_FOLD_STAMP);
}

/* txn_fold_Pended_drop frees a pending tuple id's value ids. */
static void
txn_fold_Pended_drop(void *pended_)
{
	txn_TupleU64U64VecU32_t *pended = pended_;

	std_vec_free_Checked(&pended->f2, TXN_FOLD_U32);
}

/* txn_fold_KeyFold_drop frees what a fold makes of one key. */
void
txn_fold_KeyFold_drop(TxnFoldKeyFold *self)
{
	trees_mutate_Edit_drop(&self->edit);
	std_vec_drop_Checked(&self->ctids, TXN_FOLD_CTID, txn_fold_CtidEdit_drop);
	std_vec_drop_Checked(&self->pending, TXN_FOLD_PENDED, txn_fold_Pended_drop);
}

/*
 * txn_fold_folded_key_closure_0_captures_t is folded_key's pin scope's
 * captures: the base's root, the key, and its order.
 */
typedef struct txn_fold_folded_key_closure_0_captures_t
{
	TreesStoreTree *root;
	const uint8 *key;
	size_t		key_len;
	const TreesKeyOrder *order;
} txn_fold_folded_key_closure_0_captures_t;

/*
 * txn_fold_folded_key_closure_0_frame_t is the pin scope's state across its
 * pad: its cursor and the parts of the entry it reads, each while live.
 */
typedef struct txn_fold_folded_key_closure_0_frame_t
{
	txn_fold_folded_key_closure_0_captures_t *captures;
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesCursorCursor cursor;
	bool		cursor_live;
	TxnFoldBased read;
	int			read_live;
	bool	   *has_result;
	TxnFoldBased *result;
	ChunksErrorError *error;
	bool		ok;
} txn_fold_folded_key_closure_0_frame_t;

/*
 * txn_fold_folded_key_closure_0_body seeks the key and reads the entry it
 * finds there: its value, addresses, value ids, and stamps.
 */
static void
txn_fold_folded_key_closure_0_body(void *frame_)
{
	txn_fold_folded_key_closure_0_frame_t *frame = frame_;
	txn_fold_folded_key_closure_0_captures_t *captures = frame->captures;
	StdBytes	found;
	StdBytes	value;
	TreesNodeNode node;

	if (!trees_cursor_Cursor_seek(frame->io, frame->pin, TREES_STORE_TREE_NAMED(captures->root), captures->key,
								  captures->key_len, captures->order, &frame->cursor, frame->error))
		return;
	frame->cursor_live = true;
	if (!trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		*frame->has_result = false;
		frame->ok = true;
		return;
	}
	found = trees_cursor_Cursor_key(&frame->cursor);
	if (found.len != captures->key_len || memcmp(found.ptr, captures->key, found.len) != 0)
	{
		*frame->has_result = false;
		frame->ok = true;
		return;
	}
	value = trees_cursor_Cursor_value(&frame->cursor);
	txn_fold_bytes(value.ptr, value.len, &frame->read.value);
	frame->read_live = 1;
	node = trees_cursor_Cursor_node(&frame->cursor);
	trees_node_Node_value_addresses(&node, trees_cursor_Cursor_index(&frame->cursor), &frame->read.addresses);
	frame->read_live = 2;
	if (!trees_cursor_Cursor_values(&frame->cursor, frame->io, frame->pin, &frame->read.ids, frame->error))
		return;
	frame->read_live = 3;
	if (!trees_cursor_Cursor_stamps(&frame->cursor, frame->io, frame->pin, &frame->read.stored, frame->error))
		return;
	*frame->has_result = true;
	*frame->result = frame->read;
	frame->read_live = 0;
	frame->ok = true;
}

/* txn_fold_folded_key_closure_0_pad frees what the pin scope holds live: the parts it read and its cursor. */
static void
txn_fold_folded_key_closure_0_pad(void *frame_)
{
	txn_fold_folded_key_closure_0_frame_t *frame = frame_;

	if (frame->read_live >= 3)
		std_vec_free_Checked(&frame->read.ids, TXN_FOLD_U32);
	if (frame->read_live >= 2)
		std_vec_free_Checked(&frame->read.addresses, TXN_FOLD_ADDRESS);
	if (frame->read_live >= 1)
		std_vec_free_Checked(&frame->read.value, TXN_FOLD_BYTES);
	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
	frame->read_live = 0;
	frame->cursor_live = false;
}

/* txn_fold_folded_key_closure_0 is folded_key's pin scope: the base's entry at the key, or none. */
static bool
txn_fold_folded_key_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, bool *has_result,
							  TxnFoldBased *result, ChunksErrorError *error)
{
	txn_fold_folded_key_closure_0_frame_t frame = {.captures = captures, .io = io, .pin = pin,
	.has_result = has_result, .result = result, .error = error};

	std_landing(&frame, txn_fold_folded_key_closure_0_body, txn_fold_folded_key_closure_0_pad);
	txn_fold_folded_key_closure_0_pad(&frame);
	return frame.ok;
}

/* txn_fold_writer_xid returns the own xid of a version's writer. */
static uint64
txn_fold_writer_xid(TxnMemtableVersionVersion *version)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);

	return txn_memtable_version_Record_writer(&record).xid;
}

/*
 * txn_fold_ending answers the xid of a row's changer when the rule ended the
 * row by it, a lock ending none, or zero, and raises the newest such xid of
 * another writer than the row's.
 */
static bool
txn_fold_ending(HostIoIo *io, TxnVisibleRule rule, TxnMemtableVersionVersion *changer, uint64 writer,
				uint64 *max_ended, uint64 *result, HostError *error)
{
	TxnMemtableVersionRecord record;
	TxnMemtableVersionOpRef op;
	TxnVisibleDirty dirty = txn_visible_Dirty_Default_default();
	bool		ended;
	uint64		xid;

	*result = 0;
	if (changer == NULL)
		return true;
	record = txn_memtable_version_Version_record(changer);
	op = txn_memtable_version_Record_op(&record);
	if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_LOCK || op.tag == TXN_MEMTABLE_VERSION_OP_REF_LOCK_ROW)
		return true;
	if (!rule.ops->ended(rule.self, io, changer, &dirty, &ended, error))
		return false;
	if (!ended)
		return true;
	xid = txn_memtable_version_Record_writer(&record).xid;
	if (xid != writer)
		*max_ended = Max(*max_ended, xid);
	*result = xid;
	return true;
}

/* TxnFoldFormedTag is Formed's variants, and none before a key's entry is formed. */
typedef enum TxnFoldFormedTag
{
	TXN_FOLD_FORMED_NONE,
	/* The base entry's value. */
	TXN_FOLD_FORMED_BASED,
	/* A delta row's value, its bare tuple. */
	TXN_FOLD_FORMED_WRITTEN,
} TxnFoldFormedTag;

/*
 * txn_fold_folded_key_frame_t is folded_key's state across its pad: its two
 * reads of the versions, the base entry, the base copies' enders, the stamps
 * and pending tuple ids it keeps, what the entry is formed from, the rows it
 * writes, its tuple id edits, and the new entry's parts, each while live.
 */
typedef struct txn_fold_folded_key_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	bool		has_base;
	TreesStoreTree base;
	const TreesKeyOrder *order;
	bool		keyless;
	bool		index;
	TxnVisibleRule rule;
	const uint8 *key;
	size_t		key_len;
	TxnMemtableVersionVersion *versions;
	size_t		versions_len;
	TxnVisibleSeen seen;
	bool		seen_live;
	TxnVisibleSeen every;
	bool		every_live;
	TxnFoldBased based;
	bool		based_live;
	StdVec		enders;
	bool		enders_live;
	StdVec		stamps;
	bool		stamps_live;
	StdVec		pending;
	bool		pending_live;
	TxnFoldFormedTag formed;
	StdBytes	formed_value;
	StdVec		formed_addresses;
	bool		formed_addresses_live;
	StdVec		formed_ids;
	bool		formed_ids_live;
	StdVec		written;
	bool		written_live;
	StdVec		ctids;
	bool		ctids_live;
	StdVec		value;
	bool		value_live;
	StdVec		addresses;
	bool		addresses_live;
	uint64		max_ended;
	bool	   *has_result;
	TxnFoldKeyFold *result;
	uint64	   *result_ended;
	HostError  *error;
	bool		ok;
} txn_fold_folded_key_frame_t;

/* txn_fold_folded_key_rows returns the rows a base entry of copies counts: none for an index's record of a row. */
static size_t
txn_fold_folded_key_rows(const txn_fold_folded_key_frame_t *frame, size_t copies)
{
	if (frame->index && frame->key_len > 0 && frame->key[0] == TXN_ENTRY_RECORD)
		return 0;
	return copies;
}

/* txn_fold_stamp_order orders stamps by writer and then tuple id, for qsort. */
static int
txn_fold_stamp_order(const void *a, const void *b)
{
	const ChunksTrailerStamp *x = a;
	const ChunksTrailerStamp *y = b;

	if (x->txn != y->txn)
		return (x->txn > y->txn) - (x->txn < y->txn);
	return (x->tuple > y->tuple) - (x->tuple < y->tuple);
}

/*
 * txn_fold_folded_key_ended reads which rows the rule's appliers ended: each
 * delta row it began, for the newest ender, and each base copy, for its
 * ender's xid.
 */
static bool
txn_fold_folded_key_ended(txn_fold_folded_key_frame_t *frame)
{
	uint64		ender;

	for (size_t i = 0; i < frame->every.rows.len; i++)
	{
		const TxnVisibleSeenRow *seen = &STD_VEC_AT(&frame->every.rows, TxnVisibleSeenRow, i);
		TxnVisibleDirty dirty = txn_visible_Dirty_Default_default();
		bool		begun;

		if (!frame->rule.ops->begun(frame->rule.self, frame->io, &seen->row, &dirty, &begun, frame->error))
			return false;
		if (begun && !txn_fold_ending(frame->io, frame->rule, seen->changer, txn_fold_writer_xid(seen->row.version),
									  &frame->max_ended, &ender, frame->error))
			return false;
	}
	std_vec_new(&frame->enders, TXN_FOLD_U64);
	frame->enders_live = true;
	for (size_t i = 0; frame->based_live && i < frame->based.stored.len; i++)
	{
		const ChunksTrailerStamp *stamp = &STD_VEC_AT(&frame->based.stored, ChunksTrailerStamp, i);
		HostRow		tuple = {stamp->tuple};
		bool		kept;
		TxnVisibleDirty dirty;
		TxnMemtableVersionVersion *changer;
		TxnMemtableVersionVersion *commander;

		if (!txn_visible_Seen_base(&frame->every, frame->io, TXN_FOLD_EVERY, tuple, &kept, &dirty, &changer,
								   &commander, frame->error) ||
			!txn_fold_ending(frame->io, frame->rule, changer, stamp->txn, &frame->max_ended, &ender, frame->error))
			return false;
		std_vec_push_Checked(&frame->enders, &ender, TXN_FOLD_U64);
	}
	return true;
}

/*
 * txn_fold_folded_key_kept keeps each base copy the rule sees, and makes each
 * one it ended pending with its ender's xid and the entry's value ids, then
 * forms the entry from the base's value when it keeps a copy.
 */
static bool
txn_fold_folded_key_kept(txn_fold_folded_key_frame_t *frame, bool *ended)
{
	*ended = false;
	if (!frame->based_live)
		return true;
	for (size_t i = 0; i < frame->based.stored.len; i++)
	{
		const ChunksTrailerStamp *stamp = &STD_VEC_AT(&frame->based.stored, ChunksTrailerStamp, i);
		HostRow		tuple = {stamp->tuple};
		bool		kept;
		TxnVisibleDirty dirty;
		TxnMemtableVersionVersion *changer;
		TxnMemtableVersionVersion *commander;
		txn_TupleU64U64VecU32_t pended;

		if (!txn_visible_Seen_base(&frame->seen, frame->io, frame->rule, tuple, &kept, &dirty, &changer, &commander,
								   frame->error))
			return false;
		if (kept)
		{
			std_vec_push_Checked(&frame->stamps, stamp, TXN_FOLD_STAMP);
			continue;
		}
		*ended = true;
		pended.f0 = stamp->tuple;
		pended.f1 = STD_VEC_AT(&frame->enders, uint64, i);
		std_vec_reserve_Checked(&frame->pending, 1, TXN_FOLD_PENDED);
		std_vec_clone(&pended.f2, &frame->based.ids, TXN_FOLD_U32);
		std_vec_push_Checked(&frame->pending, &pended, TXN_FOLD_PENDED);
	}
	if (frame->stamps.len > 0)
	{
		frame->formed = TXN_FOLD_FORMED_BASED;
		frame->formed_value.ptr = frame->based.value.ptr;
		frame->formed_value.len = frame->based.value.len;
		std_vec_clone(&frame->formed_addresses, &frame->based.addresses, TXN_FOLD_ADDRESS);
		frame->formed_addresses_live = true;
		std_vec_clone(&frame->formed_ids, &frame->based.ids, TXN_FOLD_U32);
		frame->formed_ids_live = true;
	}
	return true;
}

/*
 * txn_fold_folded_key_written stamps each delta row the rule keeps, oldest
 * first, with the row's tuple id and heap bytes, and forms the entry from the
 * oldest one's bare tuple when the base formed none.
 */
static void
txn_fold_folded_key_written(txn_fold_folded_key_frame_t *frame)
{
	std_vec_new(&frame->written, TXN_FOLD_WRITTEN);
	frame->written_live = true;
	for (size_t i = frame->seen.rows.len; i-- > 0;)
	{
		const TxnVisibleSeenRow *seen = &STD_VEC_AT(&frame->seen.rows, TxnVisibleSeenRow, i);
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(seen->row.version);
		TxnMemtableVersionWriter writer = txn_memtable_version_Record_writer(&record);
		ChunksTrailerStamp stamp = {.txn = writer.xid, .tuple = seen->row.put.tuple.f0};
		txn_TupleU64U32_t written = {seen->row.put.tuple.f0, seen->row.put.heap_bytes};
		TxnMemtableVersionValues values;
		TxnMemtableVersionAddresses addresses;
		uint32		id;
		chunks_TupleU32Locator_t address;

		std_vec_push_Checked(&frame->stamps, &stamp, TXN_FOLD_STAMP);
		std_vec_push_Checked(&frame->written, &written, TXN_FOLD_WRITTEN);
		if (frame->formed != TXN_FOLD_FORMED_NONE)
			continue;
		std_vec_new(&frame->formed_ids, TXN_FOLD_U32);
		frame->formed_ids_live = true;
		values = txn_memtable_version_PutRef_values(&seen->row.put);
		while (txn_memtable_version_Values_Iterator_next(&values, &id))
			std_vec_push_Checked(&frame->formed_ids, &id, TXN_FOLD_U32);
		std_vec_new(&frame->formed_addresses, TXN_FOLD_ADDRESS);
		frame->formed_addresses_live = true;
		addresses = txn_memtable_version_PutRef_addresses(&seen->row.put);
		while (txn_memtable_version_Addresses_Iterator_next(&addresses, &address))
			std_vec_push_Checked(&frame->formed_addresses, &address, TXN_FOLD_ADDRESS);
		frame->formed = TXN_FOLD_FORMED_WRITTEN;
		frame->formed_value = seen->row.put.value;
		frame->result->has_formed_by = true;
		frame->result->formed_by.f0 = writer.has_top ? writer.top : writer.xid;
		frame->result->formed_by.f1 = writer.command;
	}
}

/*
 * txn_fold_folded_key_formed makes the new entry's value, addresses, and
 * value ids from what it is formed from: a keyless key's counted to its
 * copies, a keyed key's as they are.
 */
static bool
txn_fold_folded_key_formed(txn_fold_folded_key_frame_t *frame)
{
	uint64		copies = frame->stamps.len;
	ChunksErrorError failed;

	if (frame->keyless && frame->formed == TXN_FOLD_FORMED_BASED)
	{
		std_vec_new(&frame->value, TXN_FOLD_BYTES);
		frame->value_live = true;
		std_vec_new(&frame->addresses, TXN_FOLD_ADDRESS);
		frame->addresses_live = true;
		if (!txn_counted_recount(frame->formed_value.ptr, frame->formed_value.len, frame->formed_addresses.ptr,
								 frame->formed_addresses.len, copies, &frame->value, &frame->addresses, &failed))
		{
			*frame->error = txn_base_storage(&failed);
			return false;
		}
		frame->formed_addresses_live = false;
		std_vec_free_Checked(&frame->formed_addresses, TXN_FOLD_ADDRESS);
	}
	else if (frame->keyless)
	{
		txn_counted_keyless_value(copies, frame->formed_value.ptr, frame->formed_value.len, &frame->value);
		frame->value_live = true;
		txn_counted_keyless_addresses(copies, frame->formed_addresses.ptr, frame->formed_addresses.len,
									  &frame->addresses);
		frame->addresses_live = true;
		frame->formed_addresses_live = false;
		std_vec_free_Checked(&frame->formed_addresses, TXN_FOLD_ADDRESS);
	}
	else
	{
		txn_fold_bytes(frame->formed_value.ptr, frame->formed_value.len, &frame->value);
		frame->value_live = true;
		frame->addresses = frame->formed_addresses;
		frame->addresses_live = true;
		frame->formed_addresses_live = false;
	}
	return true;
}

/*
 * txn_fold_folded_key_body reads the key's rows under the rule and under
 * Every, the base's entry, and which rows the rule's appliers ended, then
 * forms the key's edit and its tuple id tree's, or none.
 */
static void
txn_fold_folded_key_body(void *frame_)
{
	txn_fold_folded_key_frame_t *frame = frame_;
	txn_fold_folded_key_closure_0_captures_t captures = {.root = &frame->base, .key = frame->key,
	.key_len = frame->key_len, .order = frame->order};
	TxnFoldKeyFold *fold = frame->result;
	ChunksErrorError failed;
	bool		ended;
	size_t		base_copies;
	const ChunksTrailerStamp *stored = NULL;
	size_t		stored_len = 0;

	fold->has_formed_by = false;
	if (!txn_visible_seen(frame->io, frame->rule, frame->versions, frame->versions_len, NULL, &frame->seen,
						  frame->error))
		return;
	frame->seen_live = true;
	if (frame->has_base &&
		!trees_pin_pin_ResultOptionBased(frame->io, frame->store, txn_fold_folded_key_closure_0, &captures, NULL,
										 &frame->based_live, &frame->based, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	if (!txn_visible_seen(frame->io, TXN_FOLD_EVERY, frame->versions, frame->versions_len, NULL, &frame->every,
						  frame->error))
		return;
	frame->every_live = true;
	if (!txn_fold_folded_key_ended(frame))
		return;
	if (frame->index && frame->based_live)
	{
		StdBytes	tuple;
		ChunksTrailerStamp none = {0};
		uint64		zero = 0;

		if (!txn_entry_based(frame->based.value.ptr, frame->based.value.len, frame->based.stored.ptr,
							 frame->based.stored.len, frame->keyless, &tuple, frame->error))
			return;
		if (!frame->keyless)
		{
			std_vec_push_Checked(&frame->based.stored, &none, TXN_FOLD_STAMP);
			std_vec_push_Checked(&frame->enders, &zero, TXN_FOLD_U64);
		}
	}
	std_vec_new(&frame->stamps, TXN_FOLD_STAMP);
	frame->stamps_live = true;
	std_vec_new(&frame->pending, TXN_FOLD_PENDED);
	frame->pending_live = true;
	if (!txn_fold_folded_key_kept(frame, &ended))
		return;
	if (frame->based_live)
	{
		stored = frame->based.stored.ptr;
		stored_len = frame->based.stored.len;
	}
	base_copies = stored_len;
	txn_fold_folded_key_written(frame);
	*frame->result_ended = frame->max_ended;
	if (!ended && frame->stamps.len == base_copies)
	{
		*frame->has_result = false;
		frame->ok = true;
		return;
	}
	std_vec_new(&frame->ctids, TXN_FOLD_CTID);
	frame->ctids_live = true;
	if (!frame->index &&
		!txn_fold_ctid_edits(frame->key, frame->key_len, stored, stored_len, frame->stamps.ptr, frame->stamps.len,
							 frame->written.ptr, frame->written.len, &frame->ctids, frame->error))
		return;
	memset(&fold->removed, 0, sizeof(fold->removed));
	memset(&fold->added, 0, sizeof(fold->added));
	if (frame->based_live)
		fold->removed = txn_fold_sized(frame->key, frame->key_len, frame->based.value.ptr, frame->based.value.len,
									   txn_fold_folded_key_rows(frame, stored_len));
	if (frame->formed == TXN_FOLD_FORMED_NONE)
	{
		*frame->has_result = frame->based_live;
		if (frame->based_live)
		{
			fold->edit.tag = TREES_MUTATE_EDIT_DELETE;
			txn_fold_bytes(frame->key, frame->key_len, &fold->edit.u.delete);
			fold->ctids = frame->ctids;
			fold->pending = frame->pending;
			fold->has_formed_by = false;
			frame->ctids_live = frame->pending_live = false;
		}
		frame->ok = true;
		return;
	}
	if (!txn_fold_folded_key_formed(frame))
		return;
	fold->added = txn_fold_sized(frame->key, frame->key_len, frame->value.ptr, frame->value.len,
								 txn_fold_folded_key_rows(frame, frame->stamps.len));
	if (frame->index && !frame->keyless)
		frame->stamps.len = 0;
	else if (frame->index)
		qsort(frame->stamps.ptr, frame->stamps.len, sizeof(ChunksTrailerStamp), txn_fold_stamp_order);
	fold->edit.tag = TREES_MUTATE_EDIT_PUT;
	txn_fold_bytes(frame->key, frame->key_len, &fold->edit.u.put.key);
	fold->edit.u.put.value = frame->value;
	fold->edit.u.put.addresses = frame->addresses;
	fold->edit.u.put.stamps = frame->stamps;
	fold->edit.u.put.values = frame->formed_ids;
	fold->ctids = frame->ctids;
	fold->pending = frame->pending;
	frame->value_live = frame->addresses_live = frame->stamps_live = frame->formed_ids_live = false;
	frame->ctids_live = frame->pending_live = false;
	*frame->has_result = true;
	frame->ok = true;
}

/* txn_fold_folded_key_pad frees what folded_key holds live. */
static void
txn_fold_folded_key_pad(void *frame_)
{
	txn_fold_folded_key_frame_t *frame = frame_;

	if (frame->addresses_live)
		std_vec_free_Checked(&frame->addresses, TXN_FOLD_ADDRESS);
	if (frame->value_live)
		std_vec_free_Checked(&frame->value, TXN_FOLD_BYTES);
	if (frame->ctids_live)
		std_vec_drop_Checked(&frame->ctids, TXN_FOLD_CTID, txn_fold_CtidEdit_drop);
	if (frame->written_live)
		std_vec_free_Checked(&frame->written, TXN_FOLD_WRITTEN);
	if (frame->formed_ids_live)
		std_vec_free_Checked(&frame->formed_ids, TXN_FOLD_U32);
	if (frame->formed_addresses_live)
		std_vec_free_Checked(&frame->formed_addresses, TXN_FOLD_ADDRESS);
	if (frame->pending_live)
		std_vec_drop_Checked(&frame->pending, TXN_FOLD_PENDED, txn_fold_Pended_drop);
	if (frame->stamps_live)
		std_vec_free_Checked(&frame->stamps, TXN_FOLD_STAMP);
	if (frame->enders_live)
		std_vec_free_Checked(&frame->enders, TXN_FOLD_U64);
	if (frame->based_live)
		txn_fold_Based_drop(&frame->based);
	if (frame->every_live)
		txn_visible_Seen_drop(&frame->every);
	if (frame->seen_live)
		txn_visible_Seen_drop(&frame->seen);
	frame->addresses_live = frame->value_live = frame->ctids_live = frame->written_live = false;
	frame->formed_ids_live = frame->formed_addresses_live = frame->pending_live = frame->stamps_live = false;
	frame->enders_live = frame->based_live = frame->every_live = frame->seen_live = false;
}

/*
 * txn_fold_folded_key returns what a fold under a rule, a fold point's
 * (Folding) or a commit's (Committing), makes of a key's base entry from the
 * key's versions, newest first: the rows the rule keeps, the base's copies no
 * applied version ended and the delta's rows it applied, put as the key's
 * entry, a keyless key's counted; a delete when it keeps none of a key the
 * base holds; and none when it keeps the base's entry as it is. It also
 * answers the newest xid of an applied version that ended one of the key's
 * rows whose writer was another xid, zero for none. An index's storage writes
 * PLAN's form of an index storage's base leaf entry: a keyed table's keeps no
 * stamp and is one copy of its row, and a keyless table's counts its copies
 * with a stamp each, by writer and then tuple id; it makes no tuple id edit.
 */
bool
txn_fold_folded_key(HostIoIo *io, TreesStoreNodeStore store, bool has_base, TreesStoreTree base,
					const TreesKeyOrder *order, bool keyless, bool index, TxnVisibleRule rule, const uint8 *key,
					size_t key_len, TxnMemtableVersionVersion *versions, size_t versions_len, bool *has_result,
					TxnFoldKeyFold *result, uint64 *max_ended, HostError *error)
{
	txn_fold_folded_key_frame_t frame = {.io = io, .store = store, .has_base = has_base, .base = base,
		.order = order, .keyless = keyless, .index = index, .rule = rule, .key = key, .key_len = key_len,
		.versions = versions, .versions_len = versions_len, .has_result = has_result, .result = result,
	.result_ended = max_ended, .error = error};

	std_landing(&frame, txn_fold_folded_key_body, txn_fold_folded_key_pad);
	txn_fold_folded_key_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_paired_bytes returns the heap bytes a base's tuple id tree pairs
 * with a tuple id, refusing one it doesn't pair as data corruption, since the
 * base holds the row whose stamp names it.
 */
static bool
txn_fold_paired_bytes(HostIoIo *io, TreesStoreNodeStore store, bool has_tuple_ids, TreesStoreTree tuple_ids,
					  uint64 tuple, uint32 *result, HostError *error)
{
	uint8		key[6];
	bool		some;
	StdVec		value;
	ChunksErrorError failed;
	StdBytes	paired;
	bool		ok;

	if (!has_tuple_ids)
	{
		*error = txn_fold_forked("XX001", "a base's tuple id tree doesn't pair tuple id ", tuple, "");
		return false;
	}
	txn_base_tuple_id_key(tuple, key);
	if (!trees_get(io, store, TREES_STORE_TREE_NAMED(&tuple_ids), key, sizeof(key), &some, &value, &failed))
	{
		*error = txn_base_storage(&failed);
		return false;
	}
	if (!some)
	{
		*error = txn_fold_forked("XX001", "a base's tuple id tree doesn't pair tuple id ", tuple, "");
		return false;
	}
	ok = txn_base_pair_parts(value.ptr, value.len, &paired, result, error);
	std_vec_free_Checked(&value, TXN_FOLD_BYTES);
	return ok;
}

/* txn_fold_moved_field returns a size with what a fold wrote and took out, refusing one below zero. */
static bool
txn_fold_moved_field(uint64 size, uint64 removed, uint64 added, uint64 *result, HostError *error)
{
	uint64		sum = STD_ADD(u64, size, added, 632, 10);

	if (sum < removed)
	{
		*error = txn_fold_fork("XX001", "a fold takes out more of a base's sizes than it holds");
		return false;
	}
	*result = sum - removed;
	return true;
}

/*
 * txn_fold_moved returns a base's sizes with what a fold took out and what it
 * wrote, refusing a size that would fall below zero as data corruption,
 * since the base held every entry and pair the fold took out.
 */
static bool
txn_fold_moved(TxnBaseSizes sizes, TxnBaseSizes removed, TxnBaseSizes added, TxnBaseSizes *result,
			   HostError *error)
{
	return txn_fold_moved_field(sizes.entries, removed.entries, added.entries, &result->entries, error) &&
		txn_fold_moved_field(sizes.leaf_bytes, removed.leaf_bytes, added.leaf_bytes, &result->leaf_bytes, error) &&
		txn_fold_moved_field(sizes.key_bytes, removed.key_bytes, added.key_bytes, &result->key_bytes, error) &&
		txn_fold_moved_field(sizes.heap_bytes, removed.heap_bytes, added.heap_bytes, &result->heap_bytes, error) &&
		txn_fold_moved_field(sizes.rows, removed.rows, added.rows, &result->rows, error);
}

/* txn_fold_PendingRow_drop frees a pending tuple id's value ids. */
static void
txn_fold_PendingRow_drop(void *row_)
{
	TxnFoldPendingRow *row = row_;

	std_vec_free_Checked(&row->ids, TXN_FOLD_U32);
}

/*
 * txn_fold_PendingRows_insert puts a pending tuple id's row in tuple id order,
 * taking it, replacing one of its tuple id.
 */
static void
txn_fold_PendingRows_insert(StdVec *rows, TxnFoldPendingRow *row)
{
	size_t		low = 0;
	size_t		high = rows->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;

		if (STD_VEC_AT(rows, TxnFoldPendingRow, mid).tuple < row->tuple)
			low = mid + 1;
		else
			high = mid;
	}
	if (low < rows->len && STD_VEC_AT(rows, TxnFoldPendingRow, low).tuple == row->tuple)
	{
		TxnFoldPendingRow *old = &STD_VEC_AT(rows, TxnFoldPendingRow, low);

		old->ender = row->ender;
		old->bytes = row->bytes;
		std_vec_free_Checked(&old->ids, TXN_FOLD_U32);
		old->ids = row->ids;
		return;
	}
	std_vec_insert(rows, low, row, TXN_FOLD_PENDING_ROW, RUST_FILE, 480, 25);
}

/* txn_fold_FoldedRows_drop frees what a fold makes of a storage's rows. */
void
txn_fold_FoldedRows_drop(TxnFoldFoldedRows *self)
{
	std_vec_drop_Checked(&self->edits, TXN_FOLD_EDIT, trees_mutate_Edit_drop);
	std_vec_free_Checked(&self->formed, TXN_FOLD_FORMED_BY);
	std_vec_drop_Checked(&self->ctids, TXN_FOLD_CTID, txn_fold_CtidEdit_drop);
	std_vec_drop_Checked(&self->pending, TXN_FOLD_PENDING_ROW, txn_fold_PendingRow_drop);
}

/*
 * txn_fold_folded_rows_frame_t is folded_rows' state across its pad: the rows
 * it makes, the key it walks after, a step's hold and its key's versions, and
 * a key's fold, by how much of it the rows took, each while live.
 */
typedef struct txn_fold_folded_rows_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	bool		has_base;
	TreesStoreTree base;
	bool		has_tuple_ids;
	TreesStoreTree tuple_ids;
	const TreesKeyOrder *order;
	bool		keyless;
	bool		index;
	HostLsn		point;
	HostLsn		folded;
	TxnFoldFoldedRows rows;
	bool		rows_live;
	bool		has_after;
	StdVec		after;
	HostDeltaHold hold;
	bool		hold_live;
	txn_TupleVecU8VecVersion_t next;
	bool		next_live;
	TxnFoldKeyFold fold;

	/*
	 * fold_taken is how much of the fold is the rows': 0 none, 1 its edit, 2
	 * its tuple id edits too, 3 all.
	 */
	int			fold_taken;
	bool		fold_live;
	TxnFoldFoldedRows *result;
	HostError  *error;
	bool		ok;
} txn_fold_folded_rows_frame_t;

/*
 * txn_fold_folded_rows_key adds a key's fold to the rows: each tuple id it
 * unpairs gives back the heap bytes the base's tuple id tree pairs with it,
 * and goes pending with its ender's xid and value ids.
 */
static bool
txn_fold_folded_rows_key(txn_fold_folded_rows_frame_t *frame)
{
	TxnFoldFoldedRows *rows = &frame->rows;
	TxnFoldKeyFold *fold = &frame->fold;
	txn_OptionTupleU64U32_t formed = {fold->has_formed_by, fold->formed_by};

	if (fold->edit.tag == TREES_MUTATE_EDIT_PUT && !frame->keyless && fold->edit.u.put.stamps.len > 1)
		rows->overlapping = true;
	for (size_t i = 0; i < fold->ctids.len; i++)
	{
		const TxnFoldCtidEdit *ctid = &STD_VEC_AT(&fold->ctids, TxnFoldCtidEdit, i);
		TxnFoldPendingRow row = {.tuple = ctid->f0};
		size_t		at;

		if (ctid->has_f1)
		{
			rows->added.heap_bytes = STD_ADD(u64, rows->added.heap_bytes, ctid->f1.f1, 471, 46);
			continue;
		}
		if (!txn_fold_paired_bytes(frame->io, frame->store, frame->has_tuple_ids, frame->tuple_ids, ctid->f0,
								   &row.bytes, frame->error))
			return false;
		rows->removed.heap_bytes = STD_ADD(u64, rows->removed.heap_bytes, row.bytes, 474, 25);
		std_vec_reserve_Checked(&rows->pending, 1, TXN_FOLD_PENDING_ROW);
		for (at = 0; at < fold->pending.len && STD_VEC_AT(&fold->pending, txn_TupleU64U64VecU32_t, at).f0 != ctid->f0;
			 at++)
			;
		if (at < fold->pending.len)
		{
			txn_TupleU64U64VecU32_t pended;

			std_vec_swap_remove(&fold->pending, at, &pended, TXN_FOLD_PENDED);
			row.ender = pended.f1;
			row.ids = pended.f2;
		}
		else
			std_vec_new(&row.ids, TXN_FOLD_U32);
		txn_fold_PendingRows_insert(&rows->pending, &row);
	}
	rows->removed = txn_base_Sizes_summed(rows->removed, fold->removed);
	rows->added = txn_base_Sizes_summed(rows->added, fold->added);
	if (!std_vec_try_reserve(&rows->edits, 1, TXN_FOLD_EDIT))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(&rows->edits, &fold->edit, TXN_FOLD_EDIT);
	frame->fold_taken = 1;
	if (!std_vec_try_reserve(&rows->formed, 1, TXN_FOLD_FORMED_BY))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_push_Checked(&rows->formed, &formed, TXN_FOLD_FORMED_BY);
	if (!std_vec_try_reserve(&rows->ctids, fold->ctids.len, TXN_FOLD_CTID))
	{
		*frame->error = host_Error_out_of_memory();
		return false;
	}
	std_vec_extend_from_slice_Checked(&rows->ctids, fold->ctids.ptr, fold->ctids.len, TXN_FOLD_CTID);
	frame->fold_taken = 2;
	std_vec_free_Checked(&fold->ctids, TXN_FOLD_CTID);
	return true;
}

/* txn_fold_folded_rows_fold_drop frees what of a key's fold the rows didn't take. */
static void
txn_fold_folded_rows_fold_drop(txn_fold_folded_rows_frame_t *frame)
{
	if (!frame->fold_live)
		return;
	if (frame->fold_taken < 1)
		trees_mutate_Edit_drop(&frame->fold.edit);
	if (frame->fold_taken < 2)
		std_vec_drop_Checked(&frame->fold.ctids, TXN_FOLD_CTID, txn_fold_CtidEdit_drop);
	std_vec_drop_Checked(&frame->fold.pending, TXN_FOLD_PENDED, txn_fold_Pended_drop);
	frame->fold_live = false;
	frame->fold_taken = 0;
}

/* txn_fold_folded_rows_next_drop frees a step's key's versions, while live. */
static void
txn_fold_folded_rows_next_drop(txn_fold_folded_rows_frame_t *frame)
{
	if (!frame->next_live)
		return;
	txn_memtable_version_versions_drop(&frame->next.f1, 0);
	std_vec_free_Checked(&frame->next.f0, TXN_FOLD_BYTES);
	frame->next_live = false;
}

/*
 * txn_fold_folded_rows_body walks every key with a version, one scan step
 * under a hold at a time, and folds each one's versions the base didn't
 * apply, counting what the fold applies and what it leaves.
 */
static void
txn_fold_folded_rows_body(void *frame_)
{
	txn_fold_folded_rows_frame_t *frame = frame_;
	TxnFoldFoldedRows *rows = &frame->rows;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	TxnFoldFolding folding = {frame->point};
	TxnVisibleRule rule = {&TXN_FOLD_FOLDING_RULE_OPS, &folding};

	memset(rows, 0, sizeof(*rows));
	std_vec_new(&rows->edits, TXN_FOLD_EDIT);
	std_vec_new(&rows->formed, TXN_FOLD_FORMED_BY);
	std_vec_new(&rows->ctids, TXN_FOLD_CTID);
	std_vec_new(&rows->pending, TXN_FOLD_PENDING_ROW);
	frame->rows_live = true;
	for (;;)
	{
		bool		has_next;
		bool		has_fold;
		uint64		ended;

		if (!host_delta_hold(frame->io, &frame->hold, frame->error))
			return;
		frame->hold_live = true;
		if (!txn_visible_next(frame->io, &frame->hold, record, frame->has_after, frame->after.ptr, frame->after.len,
							  TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, frame->folded, &has_next, &frame->next,
							  frame->error))
			return;
		if (!has_next)
		{
			*frame->result = *rows;
			frame->rows_live = false;
			frame->ok = true;
			return;
		}
		frame->next_live = true;
		for (size_t i = 0; i < frame->next.f1.len; i++)
		{
			TxnFoldApplied applied;

			if (!txn_fold_applied(frame->io, &STD_VEC_AT(&frame->next.f1, TxnMemtableVersionVersion, i), frame->point,
								  &applied, frame->error))
				return;
			if (applied.tag == TXN_FOLD_APPLIED_YES)
			{
				rows->applied = true;
				rows->max_folded = Max(rows->max_folded, applied.u.yes.has_top ? applied.u.yes.top : 0);
			}
			else if (applied.tag == TXN_FOLD_APPLIED_STAYS)
				rows->unresolved = true;
		}
		if (!txn_fold_folded_key(frame->io, frame->store, frame->has_base, frame->base, frame->order, frame->keyless,
								 frame->index, rule, frame->next.f0.ptr, frame->next.f0.len, frame->next.f1.ptr,
								 frame->next.f1.len, &has_fold, &frame->fold, &ended, frame->error))
			return;
		rows->max_ended = Max(rows->max_ended, ended);
		if (has_fold)
		{
			frame->fold_live = true;
			if (!txn_fold_folded_rows_key(frame))
				return;
			txn_fold_folded_rows_fold_drop(frame);
		}
		if (frame->has_after)
			std_vec_free_Checked(&frame->after, TXN_FOLD_BYTES);
		frame->after = frame->next.f0;
		frame->has_after = true;
		txn_memtable_version_versions_drop(&frame->next.f1, 0);
		frame->next_live = false;
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
}

/* txn_fold_folded_rows_pad frees what folded_rows holds live. */
static void
txn_fold_folded_rows_pad(void *frame_)
{
	txn_fold_folded_rows_frame_t *frame = frame_;

	txn_fold_folded_rows_fold_drop(frame);
	txn_fold_folded_rows_next_drop(frame);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->has_after)
		std_vec_free_Checked(&frame->after, TXN_FOLD_BYTES);
	if (frame->rows_live)
		txn_fold_FoldedRows_drop(&frame->rows);
	frame->hold_live = frame->has_after = frame->rows_live = false;
}

/*
 * txn_fold_folded_rows folds a storage's rows at a fold point: it walks
 * every key of the storage's rows with a version, in its memtable or its
 * spills, a scan's steps, holding no range's lock across a key's fold, and
 * for each takes the key's versions the base, folded at folded, didn't
 * apply, forms the key's edit and its tuple id tree's, counts the top-level
 * xids of the versions the fold applies, and adds up the sizes the edits
 * move, each pair it takes out giving back the heap bytes the base's tuple id
 * tree pairs with its tuple id; an index's storage folds as folded_key says.
 */
bool
txn_fold_folded_rows(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, bool has_base,
					 TreesStoreTree base, bool has_tuple_ids, TreesStoreTree tuple_ids, const TreesKeyOrder *order,
					 bool keyless, bool index, HostLsn point, HostLsn folded, TxnFoldFoldedRows *result,
					 HostError *error)
{
	txn_fold_folded_rows_frame_t frame = {.io = io, .store = store, .held = held, .has_base = has_base,
		.base = base, .has_tuple_ids = has_tuple_ids, .tuple_ids = tuple_ids, .order = order, .keyless = keyless,
	.index = index, .point = point, .folded = folded, .result = result, .error = error};

	std_landing(&frame, txn_fold_folded_rows_body, txn_fold_folded_rows_pad);
	txn_fold_folded_rows_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_committed_moves_frame_t is committed_moves' state across its pad:
 * its hold, the key's versions, and a fold of the key, each while live.
 */
typedef struct txn_fold_committed_moves_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	bool		has_base;
	TreesStoreTree base;
	bool		has_tuple_ids;
	TreesStoreTree tuple_ids;
	const TreesKeyOrder *order;
	bool		keyless;
	bool		index;
	uint64		own;
	const uint8 *key;
	size_t		key_len;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		versions;
	bool		versions_live;
	TxnFoldKeyFold fold;
	bool		fold_live;
	TxnBaseSizes *result_0;
	TxnBaseSizes *result_1;
	HostError  *error;
	bool		ok;
} txn_fold_committed_moves_frame_t;

/* txn_fold_committed_moves_closure_0 returns the sizes the key's fold under a commit's rule adds and takes away. */
static bool
txn_fold_committed_moves_closure_0(txn_fold_committed_moves_frame_t *frame, uint64 own, TxnBaseSizes *added,
								   TxnBaseSizes *removed)
{
	TxnFoldCommitting committing = {own};
	TxnVisibleRule rule = {&TXN_FOLD_COMMITTING_RULE_OPS, &committing};
	bool		has_fold;
	uint64		ended;

	memset(added, 0, sizeof(*added));
	memset(removed, 0, sizeof(*removed));
	if (!txn_fold_folded_key(frame->io, frame->store, frame->has_base, frame->base, frame->order, frame->keyless,
							 frame->index, rule, frame->key, frame->key_len, frame->versions.ptr,
							 frame->versions.len, &has_fold, &frame->fold, &ended, frame->error))
		return false;
	if (!has_fold)
		return true;
	frame->fold_live = true;
	*added = frame->fold.added;
	*removed = frame->fold.removed;
	for (size_t i = 0; i < frame->fold.ctids.len; i++)
	{
		const TxnFoldCtidEdit *ctid = &STD_VEC_AT(&frame->fold.ctids, TxnFoldCtidEdit, i);
		uint32		bytes;

		if (ctid->has_f1)
		{
			added->heap_bytes = STD_ADD(u64, added->heap_bytes, ctid->f1.f1, 518, 42);
			continue;
		}
		if (!txn_fold_paired_bytes(frame->io, frame->store, frame->has_tuple_ids, frame->tuple_ids, ctid->f0, &bytes,
								   frame->error))
			return false;
		removed->heap_bytes = STD_ADD(u64, removed->heap_bytes, bytes, 519, 25);
	}
	frame->fold_live = false;
	txn_fold_KeyFold_drop(&frame->fold);
	return true;
}

/*
 * txn_fold_committed_moves_body folds the key with the transaction's versions
 * and without, and answers the two's difference.
 */
static void
txn_fold_committed_moves_body(void *frame_)
{
	txn_fold_committed_moves_frame_t *frame = frame_;
	TxnBaseSizes with_added;
	TxnBaseSizes with_removed;
	TxnBaseSizes without_added;
	TxnBaseSizes without_removed;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_memtable_table_versions(frame->io, &frame->hold, host_shared_Held_record(frame->held), frame->key,
									 frame->key_len, &frame->versions, frame->error))
		return;
	frame->versions_live = true;
	if (!txn_fold_committed_moves_closure_0(frame, frame->own, &with_added, &with_removed) ||
		!txn_fold_committed_moves_closure_0(frame, 0, &without_added, &without_removed))
		return;
	*frame->result_0 = txn_base_Sizes_summed(with_added, without_removed);
	*frame->result_1 = txn_base_Sizes_summed(with_removed, without_added);
	frame->ok = true;
}

/* txn_fold_committed_moves_pad frees what committed_moves holds live: a fold, the versions, and its hold. */
static void
txn_fold_committed_moves_pad(void *frame_)
{
	txn_fold_committed_moves_frame_t *frame = frame_;

	if (frame->fold_live)
		txn_fold_KeyFold_drop(&frame->fold);
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->fold_live = frame->versions_live = frame->hold_live = false;
}

/*
 * txn_fold_committed_moves returns how the committing top-level transaction
 * own moves a storage's totals at a key: what the key's fold over the base
 * writes and gives back with the transaction's versions applied, against
 * what it does without them, as the sizes it adds and those it takes away,
 * neither below zero.
 */
bool
txn_fold_committed_moves(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, bool has_base,
						 TreesStoreTree base, bool has_tuple_ids, TreesStoreTree tuple_ids, const TreesKeyOrder *order,
						 bool keyless, bool index, uint64 own, const uint8 *key, size_t key_len,
						 TxnBaseSizes *result_0, TxnBaseSizes *result_1, HostError *error)
{
	txn_fold_committed_moves_frame_t frame = {.io = io, .store = store, .held = held, .has_base = has_base,
		.base = base, .has_tuple_ids = has_tuple_ids, .tuple_ids = tuple_ids, .order = order, .keyless = keyless,
		.index = index, .own = own, .key = key, .key_len = key_len, .result_0 = result_0, .result_1 = result_1,
	.error = error};

	std_landing(&frame, txn_fold_committed_moves_body, txn_fold_committed_moves_pad);
	txn_fold_committed_moves_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_committed_value_moves_frame_t is committed_value_moves' state
 * across its pad: its hold, the id's versions, and the base's entry, each
 * while live.
 */
typedef struct txn_fold_committed_value_moves_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	bool		has_root;
	TreesStoreTree root;
	uint64		own;
	const uint8 *key;
	size_t		key_len;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		versions;
	bool		versions_live;
	StdVec		based;
	bool		based_live;
	TxnBaseSizes *result_0;
	TxnBaseSizes *result_1;
	HostError  *error;
	bool		ok;
} txn_fold_committed_value_moves_frame_t;

/*
 * txn_fold_committed_value_moves_closure_2 returns the sizes of the id's
 * entry under a commit's rule: its newest version the rule applies, a
 * removal giving none, or with none the base's entry.
 */
static bool
txn_fold_committed_value_moves_closure_2(txn_fold_committed_value_moves_frame_t *frame, uint64 own,
										 TxnBaseSizes *result, HostError *error)
{
	TxnFoldCommitting committing = {own};

	memset(result, 0, sizeof(*result));
	for (size_t i = 0; i < frame->versions.len; i++)
	{
		TxnMemtableVersionVersion *version = &STD_VEC_AT(&frame->versions, TxnMemtableVersionVersion, i);
		TxnMemtableVersionRecord record;
		TxnMemtableVersionOpRef op;
		bool		applies;

		if (!txn_fold_Committing_applies(&committing, frame->io, version, &applies, error))
			return false;
		if (!applies)
			continue;
		record = txn_memtable_version_Version_record(version);
		op = txn_memtable_version_Record_op(&record);
		if (op.tag != TXN_MEMTABLE_VERSION_OP_REF_PUT)
			return true;
		return txn_values_sizes(frame->key, frame->key_len, op.u.put.value.ptr, op.u.put.value.len, result, error);
	}
	if (!frame->based_live)
		return true;
	return txn_values_sizes(frame->key, frame->key_len, frame->based.ptr, frame->based.len, result, error);
}

/*
 * txn_fold_committed_value_moves_body reads the id's versions and base entry,
 * then sizes its entry with and without the transaction.
 */
static void
txn_fold_committed_value_moves_body(void *frame_)
{
	txn_fold_committed_value_moves_frame_t *frame = frame_;
	ChunksErrorError failed;

	if (frame->key_len != 4)
	{
		*frame->error = txn_fold_fork("XX001", "a map's key isn't four bytes");
		return;
	}
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_memtable_map_versions(&TXN_MEMTABLE_MAP_VALUES_TREE_OPS, frame->io, &frame->hold,
								   host_shared_Held_record(frame->held), std_u32_from_be_bytes(frame->key),
								   &frame->versions, frame->error))
		return;
	frame->versions_live = true;
	if (frame->has_root &&
		!trees_get(frame->io, frame->store, TREES_STORE_TREE_NAMED(&frame->root), frame->key, frame->key_len,
				   &frame->based_live, &frame->based, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	frame->ok = txn_fold_committed_value_moves_closure_2(frame, frame->own, frame->result_0, frame->error) &&
		txn_fold_committed_value_moves_closure_2(frame, 0, frame->result_1, frame->error);
}

/*
 * txn_fold_committed_value_moves_pad frees what committed_value_moves holds
 * live: the base's entry, the versions, and its hold.
 */
static void
txn_fold_committed_value_moves_pad(void *frame_)
{
	txn_fold_committed_value_moves_frame_t *frame = frame_;

	if (frame->based_live)
		std_vec_free_Checked(&frame->based, TXN_FOLD_BYTES);
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->based_live = frame->versions_live = frame->hold_live = false;
}

/*
 * txn_fold_committed_value_moves returns how the committing top-level
 * transaction own moves a TOAST storage's totals at a value id's key: the
 * entry its map gives with the transaction's versions applied, against
 * without them, each the key's newest version the Committing rule applies, a
 * removal giving none, and with none the base's entry; the two are sized as
 * values' sizes counts them.
 */
bool
txn_fold_committed_value_moves(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, bool has_root,
							   TreesStoreTree root, uint64 own, const uint8 *key, size_t key_len,
							   TxnBaseSizes *result_0, TxnBaseSizes *result_1, HostError *error)
{
	txn_fold_committed_value_moves_frame_t frame = {.io = io, .store = store, .held = held, .has_root = has_root,
		.root = root, .own = own, .key = key, .key_len = key_len, .result_0 = result_0, .result_1 = result_1,
	.error = error};

	std_landing(&frame, txn_fold_committed_value_moves_body, txn_fold_committed_value_moves_pad);
	txn_fold_committed_value_moves_pad(&frame);
	return frame.ok;
}

/* txn_TupleHandleWriter_t is ran_key's (Handle, Writer): a version a drop removes, with its writer. */
typedef struct txn_TupleHandleWriter_t
{
	TxnMemtableVersionHandle f0;
	TxnMemtableVersionWriter f1;
} txn_TupleHandleWriter_t;

/* txn_TupleU32VecTupleHandleWriter_t is Spaced's (u32, Vec<(Handle, Writer)>): a key's versions a drop removes. */
typedef struct txn_TupleU32VecTupleHandleWriter_t
{
	uint32		f0;
	StdVec		f1;
} txn_TupleU32VecTupleHandleWriter_t;

/*
 * TxnFoldSpaced is Spaced: what the drop through a point takes out of a
 * storage's map, the base's tree's edits, each key's versions it removes with
 * their writers, whether it removes every version the map holds, and
 * whether it met a version whose writer the point leaves for later.
 */
typedef struct TxnFoldSpaced
{
	/* edits is a Vec<Edit>. */
	StdVec		edits;
	/* removed is a Vec<(u32, Vec<(Handle, Writer)>)>. */
	StdVec		removed;
	bool		emptied;
	bool		unresolved;
} TxnFoldSpaced;

#define TXN_FOLD_REMOVED sizeof(txn_TupleU32VecTupleHandleWriter_t), _Alignof(txn_TupleU32VecTupleHandleWriter_t)
#define TXN_FOLD_HANDLE_WRITER sizeof(txn_TupleHandleWriter_t), _Alignof(txn_TupleHandleWriter_t)

/* TxnFoldFits reports whether a run of a key's versions, newest first, may leave the delta. */
typedef bool (*TxnFoldFits) (void *captures, HostIoIo *io, TxnMemtableVersionVersion *run, size_t run_len,
							 bool *result, HostError *error);

/* TxnFoldPaired reports whether the installed base's tuple id tree pairs a tuple id. */
typedef bool (*TxnFoldPaired) (void *captures, HostIoIo *io, uint64 tuple, bool *result, HostError *error);

/* txn_fold_Removed_drop frees a key's versions a drop removes. */
static void
txn_fold_Removed_drop(void *removed_)
{
	txn_TupleU32VecTupleHandleWriter_t *removed = removed_;

	std_vec_free_Checked(&removed->f1, TXN_FOLD_HANDLE_WRITER);
}

/* txn_fold_Spaced_drop frees what a drop takes out of a map. */
static void
txn_fold_Spaced_drop(TxnFoldSpaced *self)
{
	std_vec_drop_Checked(&self->edits, TXN_FOLD_EDIT, trees_mutate_Edit_drop);
	std_vec_drop_Checked(&self->removed, TXN_FOLD_REMOVED, txn_fold_Removed_drop);
}

/* txn_fold_ran_key_frame_t is ran_key's state across its pad: the new entry's key, while live. */
typedef struct txn_fold_ran_key_frame_t
{
	HostIoIo   *io;
	TxnFoldSpaced *spaced;
	const uint8 *key;
	size_t		key_len;
	uint32		id;
	TxnMemtableVersionVersion *versions;
	size_t		versions_len;
	size_t		own;
	HostLsn		point;
	bool		committed;
	TxnFoldFits fits;
	void	   *fits_captures;
	StdVec		named;
	bool		named_live;
	HostError  *error;
	bool		ok;
} txn_fold_ran_key_frame_t;

/* txn_fold_ran_key_named reports whether a version names a values mark, which changes no entry. */
static bool
txn_fold_ran_key_named(TxnMemtableVersionVersion *version)
{
	TxnMemtableVersionRecord record = txn_memtable_version_Version_record(version);

	return txn_memtable_version_Record_op(&record).tag == TXN_MEMTABLE_VERSION_OP_REF_NAMED;
}

/*
 * txn_fold_ran_key_body finds the oldest run of the key's versions the point
 * applies or drops, the longest one that fits, and adds its source's entry
 * and its memtable versions' removal.
 */
static void
txn_fold_ran_key_body(void *frame_)
{
	txn_fold_ran_key_frame_t *frame = frame_;
	TxnFoldSpaced *spaced = frame->spaced;
	size_t		len = frame->versions_len;
	size_t		longest = 0;
	size_t		run;
	size_t		first;
	TxnMemtableVersionVersion *source = NULL;
	txn_TupleU32VecTupleHandleWriter_t removed = {.f0 = frame->id};

	for (size_t i = len; i-- > 0;)
	{
		TxnFoldApplied applied;

		if (!txn_fold_applied(frame->io, &frame->versions[i], frame->point, &applied, frame->error))
			return;
		if (applied.tag == TXN_FOLD_APPLIED_STAYS)
		{
			spaced->unresolved = true;
			break;
		}
		longest++;
	}
	for (run = longest; run > 0; run--)
	{
		bool		fits = true;

		if (frame->fits != NULL &&
			!frame->fits(frame->fits_captures, frame->io, &frame->versions[len - run], run, &fits, frame->error))
			return;
		if (fits)
			break;
	}
	spaced->emptied &= run == len;
	if (run == 0)
	{
		frame->ok = true;
		return;
	}
	first = len - run;
	for (size_t i = first; i < len; i++)
	{
		bool		yes;

		if (txn_fold_ran_key_named(&frame->versions[i]))
			continue;
		if (!frame->committed)
		{
			source = &frame->versions[i];
			break;
		}
		if (!txn_fold_applied_yes(frame->io, &frame->versions[i], frame->point, &yes, frame->error))
			return;
		if (yes)
		{
			source = &frame->versions[i];
			break;
		}
	}
	if (source != NULL)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(source);
		TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);
		TreesMutateEdit edit;

		if (!std_vec_try_reserve(&spaced->edits, 1, TXN_FOLD_EDIT))
		{
			*frame->error = host_Error_out_of_memory();
			return;
		}
		if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_PUT)
		{
			StdVec		value;

			txn_fold_bytes(frame->key, frame->key_len, &frame->named);
			frame->named_live = true;
			txn_fold_bytes(op.u.put.value.ptr, op.u.put.value.len, &value);
			frame->named_live = false;
			edit.tag = TREES_MUTATE_EDIT_PUT;
			trees_build_Entry_new_VecU8VecU8(&frame->named, &value, &edit.u.put);
		}
		else
		{
			edit.tag = TREES_MUTATE_EDIT_DELETE;
			txn_fold_bytes(frame->key, frame->key_len, &edit.u.delete);
		}
		std_vec_push_Checked(&spaced->edits, &edit, TXN_FOLD_EDIT);
	}
	if (!std_vec_try_reserve(&spaced->removed, 1, TXN_FOLD_REMOVED))
	{
		*frame->error = host_Error_out_of_memory();
		return;
	}
	std_vec_new(&removed.f1, TXN_FOLD_HANDLE_WRITER);
	for (size_t i = first; i < Max(frame->own, first); i++)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&frame->versions[i]);
		txn_TupleHandleWriter_t handle = {frame->versions[i].handle, txn_memtable_version_Record_writer(&record)};

		std_vec_push_Checked(&removed.f1, &handle, TXN_FOLD_HANDLE_WRITER);
	}
	std_vec_push_Checked(&spaced->removed, &removed, TXN_FOLD_REMOVED);
	frame->ok = true;
}

/* txn_fold_ran_key_pad frees what ran_key holds live: the new entry's key. */
static void
txn_fold_ran_key_pad(void *frame_)
{
	txn_fold_ran_key_frame_t *frame = frame_;

	if (frame->named_live)
		std_vec_free_Checked(&frame->named, TXN_FOLD_BYTES);
	frame->named_live = false;
}

/*
 * txn_fold_ran_key adds what a drop at a fold point takes out of one key of a
 * map, given its versions, newest first, the first own of them its
 * memtable's, as ran reads it.
 */
static bool
txn_fold_ran_key(HostIoIo *io, TxnFoldSpaced *spaced, const uint8 *key, size_t key_len, uint32 id,
				 TxnMemtableVersionVersion *versions, size_t versions_len, size_t own, HostLsn point, bool committed,
				 TxnFoldFits fits, void *fits_captures, HostError *error)
{
	txn_fold_ran_key_frame_t frame = {.io = io, .spaced = spaced, .key = key, .key_len = key_len, .id = id,
		.versions = versions, .versions_len = versions_len, .own = own, .point = point, .committed = committed,
	.fits = fits, .fits_captures = fits_captures, .error = error};

	std_landing(&frame, txn_fold_ran_key_body, txn_fold_ran_key_pad);
	txn_fold_ran_key_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_ran_frame_t is ran's state across its pad: what it takes out, the
 * ids the walk read, and a step's hold, key, and versions.
 */
typedef struct txn_fold_ran_frame_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	const TxnMemtableTableTreeOps *tree;
	HostLsn		point;
	bool		committed;
	bool		has_spilled;
	TxnRunsKind spilled;
	TxnFoldFits fits;
	void	   *fits_captures;
	TxnFoldSpaced spaced;
	bool		spaced_live;
	StdVec		walked;
	bool		walked_live;
	HostDeltaHold hold;
	bool		hold_live;
	bool		has_after;
	StdVec		after;
	txn_TupleVecU8VecVersion_t next;
	bool		next_live;
	TxnFoldSpaced *result;
	HostError  *error;
	bool		ok;
} txn_fold_ran_frame_t;

/*
 * txn_fold_ran_closure_1_frame_t is ran's walk's state across its pad: its
 * hold and a key's versions, each while live.
 */
typedef struct txn_fold_ran_closure_1_frame_t
{
	txn_fold_ran_frame_t *ran;
	HostIoIo   *io;
	const uint8 *key;
	size_t		key_len;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		versions;
	bool		versions_live;
	StdVec		spills;
	bool		spills_live;
	HostError  *error;
	bool		ok;
} txn_fold_ran_closure_1_frame_t;

/* txn_fold_ran_closure_1_body reads the key's memtable versions, then its spills' below them, and adds its run. */
static void
txn_fold_ran_closure_1_body(void *frame_)
{
	txn_fold_ran_closure_1_frame_t *frame = frame_;
	txn_fold_ran_frame_t *ran = frame->ran;
	HostSharedRecord record = host_shared_Held_record(ran->held);
	uint32		id;
	size_t		own;

	if (frame->key_len != 4)
	{
		*frame->error = txn_fold_fork("XX001", "a map's key isn't four bytes");
		return;
	}
	id = std_u32_from_be_bytes(frame->key);
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_memtable_map_versions(ran->tree, frame->io, &frame->hold, record, id, &frame->versions, frame->error))
		return;
	frame->versions_live = true;
	own = frame->versions.len;
	if (ran->has_spilled)
	{
		if (!txn_spill_versions(&TXN_SPILL_STATE_HOME_OPS, frame->io, &frame->hold, record, ran->spilled, frame->key,
								frame->key_len, &frame->spills, frame->error))
			return;
		frame->spills_live = true;
		std_vec_extend_from_slice_Checked(&frame->versions, frame->spills.ptr, frame->spills.len, TXN_FOLD_VERSION);
		frame->spills_live = false;
		std_vec_free_Checked(&frame->spills, TXN_FOLD_VERSION);
		if (!std_vec_try_reserve(&ran->walked, 1, TXN_FOLD_U32))
		{
			*frame->error = host_Error_out_of_memory();
			return;
		}
		std_vec_push_Checked(&ran->walked, &id, TXN_FOLD_U32);
	}
	frame->ok = txn_fold_ran_key(frame->io, &ran->spaced, frame->key, frame->key_len, id, frame->versions.ptr,
								 frame->versions.len, own, ran->point, ran->committed, ran->fits, ran->fits_captures,
								 frame->error);
}

/* txn_fold_ran_closure_1_pad frees what the walk holds live: the spills' versions, the versions, and its hold. */
static void
txn_fold_ran_closure_1_pad(void *frame_)
{
	txn_fold_ran_closure_1_frame_t *frame = frame_;

	if (frame->spills_live)
		txn_memtable_version_versions_drop(&frame->spills, 0);
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->spills_live = frame->versions_live = frame->hold_live = false;
}

/* txn_fold_ran_closure_1 is ran's walk's closure, of one key of the map's memtable. */
static bool
txn_fold_ran_closure_1(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
					   size_t values_len, HostError *error)
{
	txn_fold_ran_closure_1_frame_t frame = {.ran = captures, .io = io, .key = key, .key_len = key_len,
	.error = error};

	std_landing(&frame, txn_fold_ran_closure_1_body, txn_fold_ran_closure_1_pad);
	txn_fold_ran_closure_1_pad(&frame);
	return frame.ok;
}

/* txn_fold_id_order orders value ids ascending, for bsearch. */
static int
txn_fold_id_order(const void *a, const void *b)
{
	uint32		x = *(const uint32 *) a;
	uint32		y = *(const uint32 *) b;

	return (x > y) - (x < y);
}

/*
 * txn_fold_ran_body walks the map's memtable keys, then for a map whose
 * spills are of a kind the keys its spills alone hold, passing over each the
 * walk read.
 */
static void
txn_fold_ran_body(void *frame_)
{
	txn_fold_ran_frame_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);

	std_vec_new(&frame->spaced.edits, TXN_FOLD_EDIT);
	std_vec_new(&frame->spaced.removed, TXN_FOLD_REMOVED);
	frame->spaced.emptied = true;
	frame->spaced.unresolved = false;
	frame->spaced_live = true;
	std_vec_new(&frame->walked, TXN_FOLD_U32);
	frame->walked_live = true;
	if (!txn_memtable_table_walked(frame->tree, frame->io, record, txn_fold_ran_closure_1, frame, NULL, frame->error))
		return;
	while (frame->has_spilled)
	{
		bool		some;
		uint32		id;

		if (!host_delta_hold(frame->io, &frame->hold, frame->error))
			return;
		frame->hold_live = true;
		if (!txn_spill_next(&TXN_SPILL_STATE_HOME_OPS, frame->io, &frame->hold, record, frame->spilled,
							frame->has_after, frame->after.ptr, frame->after.len,
							TXN_MEMTABLE_TABLE_DIRECTION_FORWARD, &some, &frame->next, frame->error))
			return;
		if (!some)
			break;
		frame->next_live = true;
		if (frame->next.f0.len != 4)
		{
			*frame->error = txn_fold_fork("XX001", "a map's key isn't four bytes");
			return;
		}
		id = std_u32_from_be_bytes(frame->next.f0.ptr);
		if (bsearch(&id, frame->walked.ptr, frame->walked.len, sizeof(uint32), txn_fold_id_order) == NULL &&
			!txn_fold_ran_key(frame->io, &frame->spaced, frame->next.f0.ptr, frame->next.f0.len, id,
							  frame->next.f1.ptr, frame->next.f1.len, 0, frame->point, frame->committed,
							  frame->fits, frame->fits_captures, frame->error))
			return;
		if (frame->has_after)
			std_vec_free_Checked(&frame->after, TXN_FOLD_BYTES);
		frame->after = frame->next.f0;
		frame->has_after = true;
		txn_memtable_version_versions_drop(&frame->next.f1, 0);
		frame->next_live = false;
		frame->hold_live = false;
		host_delta_Hold_drop(&frame->hold);
	}
	*frame->result = frame->spaced;
	frame->spaced_live = false;
	frame->ok = true;
}

/* txn_fold_ran_pad frees what ran holds live. */
static void
txn_fold_ran_pad(void *frame_)
{
	txn_fold_ran_frame_t *frame = frame_;

	if (frame->next_live)
	{
		txn_memtable_version_versions_drop(&frame->next.f1, 0);
		std_vec_free_Checked(&frame->next.f0, TXN_FOLD_BYTES);
	}
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	if (frame->has_after)
		std_vec_free_Checked(&frame->after, TXN_FOLD_BYTES);
	if (frame->walked_live)
		std_vec_free_Checked(&frame->walked, TXN_FOLD_U32);
	if (frame->spaced_live)
		txn_fold_Spaced_drop(&frame->spaced);
	frame->next_live = frame->hold_live = frame->has_after = frame->walked_live = frame->spaced_live = false;
}

/*
 * txn_fold_ran reads what a drop at a fold point takes out of a storage's
 * map: for each key, the oldest run of versions whose writers the point
 * applies or that aborted, the longest such run that fits, whose newest, or
 * its newest committed one when committed, none leaving the base's entry, is
 * the key's entry in the new base, a removal its deletion, a values mark
 * passed over. A map whose spills are of a kind reads each key's spilled
 * versions below its memtable's, then the keys its spills alone hold. Only
 * the memtable's versions are listed for removal.
 */
static bool
txn_fold_ran(HostIoIo *io, const HostSharedHeld *held, const TxnMemtableTableTreeOps *tree, HostLsn point,
			 bool committed, bool has_spilled, TxnRunsKind spilled, TxnFoldFits fits, void *fits_captures,
			 TxnFoldSpaced *result, HostError *error)
{
	txn_fold_ran_frame_t frame = {.io = io, .held = held, .tree = tree, .point = point, .committed = committed,
		.has_spilled = has_spilled, .spilled = spilled, .fits = fits, .fits_captures = fits_captures,
	.result = result, .error = error};

	std_landing(&frame, txn_fold_ran_body, txn_fold_ran_pad);
	txn_fold_ran_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_freed reports whether a run of a block's free space versions,
 * newest first, may leave the delta: each take in it, and each take's values
 * mark, names a tuple id its newest entry holds free, which VACUUM freed, or
 * the installed base's tuple id tree pairs.
 */
static bool
txn_fold_freed(HostIoIo *io, TxnMemtableVersionVersion *run, size_t run_len, HostLsn point, TxnFoldPaired paired,
			   void *paired_captures, bool *result, HostError *error)
{
	bool		has_newest = false;
	StdBytes	newest = {0};

	for (size_t i = 0; i < run_len; i++)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&run[i]);
		TxnMemtableVersionOpRef op = txn_memtable_version_Record_op(&record);

		if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_NAMED)
			continue;
		if (op.tag == TXN_MEMTABLE_VERSION_OP_REF_PUT)
		{
			has_newest = true;
			newest = op.u.put.value;
		}
		break;
	}
	*result = true;
	for (size_t i = 0; i < run_len; i++)
	{
		TxnMemtableVersionRecord record = txn_memtable_version_Version_record(&run[i]);
		HostRow		tuple = txn_memtable_map_taken(txn_memtable_version_Record_op(&record));
		TxnFoldApplied applied;
		TxnFoldApplied stays = {.tag = TXN_FOLD_APPLIED_STAYS};
		bool		frees = false;
		bool		pairs;

		if (tuple.f0 == 0)
			continue;
		if (!txn_fold_applied(io, &run[i], point, &applied, error))
			return false;
		if (txn_fold_Applied_PartialEq_eq(&applied, &stays))
			continue;
		if (has_newest &&
			!txn_free_space_frees(newest.ptr, newest.len, (uint16) (tuple.f0 & 0xffff), &frees, error))
			return false;
		if (frees)
			continue;
		if (!paired(paired_captures, io, tuple.f0, &pairs, error))
			return false;
		if (!pairs)
		{
			*result = false;
			return true;
		}
	}
	return true;
}

/* txn_fold_spaced_closure_0_captures_t is spaced's fits' captures: the point and the pairing. */
typedef struct txn_fold_spaced_closure_0_captures_t
{
	HostLsn		point;
	TxnFoldPaired paired;
	void	   *paired_captures;
} txn_fold_spaced_closure_0_captures_t;

/* txn_fold_spaced_closure_0 is spaced's fits: whether a run of a block's versions is freed. */
static bool
txn_fold_spaced_closure_0(void *captures, HostIoIo *io, TxnMemtableVersionVersion *run, size_t run_len,
						  bool *result, HostError *error)
{
	txn_fold_spaced_closure_0_captures_t *spaced = captures;

	return txn_fold_freed(io, run, run_len, spaced->point, spaced->paired, spaced->paired_captures, result, error);
}

/*
 * txn_fold_spaced reads what the drop at a fold point takes out of a
 * storage's free space map: for each block, the oldest run of versions whose
 * takers the point applies, each take among them naming a tuple id the run's
 * newest entry holds free or the installed base's tuple id tree pairs; the
 * newest of the run is the block's entry in the new base, a removal its
 * deletion.
 */
static bool
txn_fold_spaced(HostIoIo *io, const HostSharedHeld *held, HostLsn point, TxnFoldPaired paired,
				void *paired_captures, TxnFoldSpaced *result, HostError *error)
{
	txn_fold_spaced_closure_0_captures_t fits = {point, paired, paired_captures};

	return txn_fold_ran(io, held, &TXN_MEMTABLE_MAP_FREE_TREE_OPS, point, false, false, 0,
						txn_fold_spaced_closure_0, &fits, result, error);
}

/* txn_fold_valued_frame_t is valued's state across its pad: what it takes out of the value map, while live. */
typedef struct txn_fold_valued_frame_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	HostLsn		point;
	TxnFoldSpaced spaced;
	bool		spaced_live;
	TxnFoldSpaced *result;
	HostError  *error;
	bool		ok;
} txn_fold_valued_frame_t;

/* txn_fold_valued_body reads the value map's drop and lists each entry's blob root in its leaf's address table. */
static void
txn_fold_valued_body(void *frame_)
{
	txn_fold_valued_frame_t *frame = frame_;

	if (!txn_fold_ran(frame->io, frame->held, &TXN_MEMTABLE_MAP_VALUES_TREE_OPS, frame->point, true, true,
					  TXN_RUNS_KIND_SPILLED_VALUES, NULL, NULL, &frame->spaced, frame->error))
		return;
	frame->spaced_live = true;
	for (size_t i = 0; i < frame->spaced.edits.len; i++)
	{
		TreesMutateEdit *edit = &STD_VEC_AT(&frame->spaced.edits, TreesMutateEdit, i);
		TxnValuesEntry entry;
		chunks_TupleU32Locator_t address = {0};
		StdVec		addresses;

		if (edit->tag != TREES_MUTATE_EDIT_PUT)
			continue;
		if (!txn_values_Entry_decode(edit->u.put.value.ptr, edit->u.put.value.len, &entry, frame->error))
			return;
		address.f1 = entry.locator;
		std_vec_with_capacity_Checked(&addresses, 1, TXN_FOLD_ADDRESS);
		std_vec_push_Checked(&addresses, &address, TXN_FOLD_ADDRESS);
		std_vec_free_Checked(&edit->u.put.addresses, TXN_FOLD_ADDRESS);
		edit->u.put.addresses = addresses;
	}
	*frame->result = frame->spaced;
	frame->spaced_live = false;
	frame->ok = true;
}

/* txn_fold_valued_pad frees what valued holds live: the value map's drop. */
static void
txn_fold_valued_pad(void *frame_)
{
	txn_fold_valued_frame_t *frame = frame_;

	if (frame->spaced_live)
		txn_fold_Spaced_drop(&frame->spaced);
	frame->spaced_live = false;
}

/*
 * txn_fold_valued reads what a fold at a point takes out of a TOAST
 * relation's storage's value map: for each value id, the oldest run of
 * versions whose writers the point applies or that aborted, the newest
 * committed one of which is the id's entry in the new base's root, a removal
 * its deletion. Each entry it writes lists its blob's root in its leaf's
 * address table, at offset zero with its locator.
 */
static bool
txn_fold_valued(HostIoIo *io, const HostSharedHeld *held, HostLsn point, TxnFoldSpaced *result, HostError *error)
{
	txn_fold_valued_frame_t frame = {.io = io, .held = held, .point = point, .result = result, .error = error};

	std_landing(&frame, txn_fold_valued_body, txn_fold_valued_pad);
	txn_fold_valued_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_valued_sizes_frame_t is valued_sizes' state across its pad: a
 * base entry an edit replaces, while live.
 */
typedef struct txn_fold_valued_sizes_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	bool		has_root;
	TreesStoreTree root;
	const TreesMutateEdit *edits;
	size_t		edits_len;
	StdVec		old;
	bool		old_live;
	TxnBaseSizes *added;
	TxnBaseSizes *removed;
	HostError  *error;
	bool		ok;
} txn_fold_valued_sizes_frame_t;

/* txn_fold_valued_sizes_body sizes each edit's entry it puts and each base entry it replaces or removes. */
static void
txn_fold_valued_sizes_body(void *frame_)
{
	txn_fold_valued_sizes_frame_t *frame = frame_;

	memset(frame->added, 0, sizeof(*frame->added));
	memset(frame->removed, 0, sizeof(*frame->removed));
	for (size_t i = 0; i < frame->edits_len; i++)
	{
		const TreesMutateEdit *edit = &frame->edits[i];
		StdBytes	key = trees_mutate_Edit_key(edit);
		TxnBaseSizes sizes;
		ChunksErrorError failed;

		if (edit->tag == TREES_MUTATE_EDIT_PUT)
		{
			if (!txn_values_sizes(edit->u.put.key.ptr, edit->u.put.key.len, edit->u.put.value.ptr,
								  edit->u.put.value.len, &sizes, frame->error))
				return;
			*frame->added = txn_base_Sizes_summed(*frame->added, sizes);
		}
		if (!frame->has_root)
			continue;
		if (!trees_get(frame->io, frame->store, TREES_STORE_TREE_NAMED(&frame->root), key.ptr, key.len,
					   &frame->old_live, &frame->old, &failed))
		{
			*frame->error = txn_base_storage(&failed);
			return;
		}
		if (!frame->old_live)
			continue;
		if (!txn_values_sizes(key.ptr, key.len, frame->old.ptr, frame->old.len, &sizes, frame->error))
			return;
		*frame->removed = txn_base_Sizes_summed(*frame->removed, sizes);
		frame->old_live = false;
		std_vec_free_Checked(&frame->old, TXN_FOLD_BYTES);
	}
	frame->ok = true;
}

/* txn_fold_valued_sizes_pad frees what valued_sizes holds live: a base entry. */
static void
txn_fold_valued_sizes_pad(void *frame_)
{
	txn_fold_valued_sizes_frame_t *frame = frame_;

	if (frame->old_live)
		std_vec_free_Checked(&frame->old, TXN_FOLD_BYTES);
	frame->old_live = false;
}

/*
 * txn_fold_valued_sizes returns the sizes a TOAST storage's value map's edits
 * over its base write and take out, each entry sized as values' sizes counts
 * it: an entry put adds its sizes and gives back the base entry it replaces,
 * and a removal gives back the base's.
 */
static bool
txn_fold_valued_sizes(HostIoIo *io, TreesStoreNodeStore store, bool has_root, TreesStoreTree root,
					  const TreesMutateEdit *edits, size_t edits_len, TxnBaseSizes *added, TxnBaseSizes *removed,
					  HostError *error)
{
	txn_fold_valued_sizes_frame_t frame = {.io = io, .store = store, .has_root = has_root, .root = root,
	.edits = edits, .edits_len = edits_len, .added = added, .removed = removed, .error = error};

	std_landing(&frame, txn_fold_valued_sizes_body, txn_fold_valued_sizes_pad);
	txn_fold_valued_sizes_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_recount_frame_t is recount's state across its pad: the base, the
 * rows' fold, and the value map's, each while live.
 */
typedef struct txn_fold_recount_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	const TreesKeyOrder *order;
	bool		keyless;
	TxnBaseBase base;
	bool		base_live;
	TxnFoldFoldedRows folded;
	bool		folded_live;
	TxnFoldSpaced values;
	bool		values_live;
	bool	   *has_result;
	TxnBaseSizes *result;
	HostError  *error;
	bool		ok;
} txn_fold_recount_frame_t;

/* txn_fold_recount_body folds every committed version over the newest base and moves its sizes by what that writes. */
static void
txn_fold_recount_body(void *frame_)
{
	txn_fold_recount_frame_t *frame = frame_;
	TxnBaseBase *base = &frame->base;
	TxnBaseSizes value_added;
	TxnBaseSizes value_removed;
	TxnBaseSizes rows;

	*frame->has_result = false;
	if (!txn_state_base_record(frame->io, frame->held, &frame->base_live, &frame->base, frame->error))
		return;
	if (!frame->base_live)
	{
		frame->ok = true;
		return;
	}
	if (!txn_fold_folded_rows(frame->io, frame->store, frame->held, base->has_root, base->root, base->has_tuple_ids,
							  base->tuple_ids, frame->order, frame->keyless, base->has_index, PG_UINT64_MAX,
							  base->folded, &frame->folded, frame->error))
		return;
	frame->folded_live = true;
	if (!txn_fold_ran(frame->io, frame->held, &TXN_MEMTABLE_MAP_VALUES_TREE_OPS, PG_UINT64_MAX, true, true,
					  TXN_RUNS_KIND_SPILLED_VALUES, NULL, NULL, &frame->values, frame->error))
		return;
	frame->values_live = true;
	if (frame->folded.unresolved || frame->values.unresolved)
	{
		frame->ok = true;
		return;
	}
	if (!txn_fold_valued_sizes(frame->io, frame->store, base->has_root, base->root, frame->values.edits.ptr,
							   frame->values.edits.len, &value_added, &value_removed, frame->error) ||
		!txn_fold_moved(base->sizes, frame->folded.removed, frame->folded.added, &rows, frame->error) ||
		!txn_fold_moved(rows, value_removed, value_added, frame->result, frame->error))
		return;
	*frame->has_result = true;
	frame->ok = true;
}

/* txn_fold_recount_pad frees what recount holds live: the value map's fold, the rows' fold, and the base. */
static void
txn_fold_recount_pad(void *frame_)
{
	txn_fold_recount_frame_t *frame = frame_;

	if (frame->values_live)
		txn_fold_Spaced_drop(&frame->values);
	if (frame->folded_live)
		txn_fold_FoldedRows_drop(&frame->folded);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->values_live = frame->folded_live = frame->base_live = false;
}

/*
 * txn_fold_recount returns a count of a storage's totals: its newest base's
 * sizes moved by a fold of every committed version, which writes nothing, or
 * none when the fold met a version whose writer hasn't ended or whose commit
 * it can't yet place, since that commit may never move the totals.
 */
bool
txn_fold_recount(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, const TreesKeyOrder *order,
				 bool keyless, bool *has_result, TxnBaseSizes *result, HostError *error)
{
	txn_fold_recount_frame_t frame = {.io = io, .store = store, .held = held, .order = order, .keyless = keyless,
	.has_result = has_result, .result = result, .error = error};

	std_landing(&frame, txn_fold_recount_body, txn_fold_recount_pad);
	txn_fold_recount_pad(&frame);
	return frame.ok;
}

/* txn_fold_held_pair reports whether a base's tuple id tree pairs a tuple id, read through a store. */
static bool
txn_fold_held_pair(HostIoIo *io, TreesStoreNodeStore store, bool has_tuple_ids, TreesStoreTree tuple_ids,
				   uint64 tuple, bool *result, HostError *error)
{
	uint8		key[6];
	StdVec		value;
	ChunksErrorError failed;

	*result = false;
	if (!has_tuple_ids)
		return true;
	txn_base_tuple_id_key(tuple, key);
	if (!trees_get(io, store, TREES_STORE_TREE_NAMED(&tuple_ids), key, sizeof(key), result, &value, &failed))
	{
		*error = txn_base_storage(&failed);
		return false;
	}
	if (*result)
		std_vec_free_Checked(&value, TXN_FOLD_BYTES);
	return true;
}

/* txn_fold_held_pairing_t is a pairing by a base's tuple id tree read through a store. */
typedef struct txn_fold_held_pairing_t
{
	TreesStoreNodeStore store;
	bool		has_tuple_ids;
	TreesStoreTree tuple_ids;
} txn_fold_held_pairing_t;

/* txn_fold_held_pairing is a Paired over a base's tuple id tree, held_pair's. */
static bool
txn_fold_held_pairing(void *captures, HostIoIo *io, uint64 tuple, bool *result, HostError *error)
{
	txn_fold_held_pairing_t *pairing = captures;

	return txn_fold_held_pair(io, pairing->store, pairing->has_tuple_ids, pairing->tuple_ids, tuple, result, error);
}

/*
 * txn_fold_unspaced_frame_t is unspaced's state across its pad: the keys it
 * takes, a key's handles and removal, each while live.
 */
typedef struct txn_fold_unspaced_frame_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	const TxnMemtableTableTreeOps *tree;
	StdVec		removed;
	size_t		at;
	StdVec		handles;
	bool		handles_live;
	StdVec		taken;
	bool		taken_live;
	HostError  *error;
	bool		ok;
} txn_fold_unspaced_frame_t;

/* txn_fold_unspaced_body takes each key's versions out and gives each taken one's count back at its writer's entry. */
static void
txn_fold_unspaced_body(void *frame_)
{
	txn_fold_unspaced_frame_t *frame = frame_;
	TxnMemtableVersionWriter none = TXN_MEMTABLE_VERSION_NO_TRANSACTION;

	for (; frame->at < frame->removed.len; frame->at++)
	{
		txn_TupleU32VecTupleHandleWriter_t *key = &STD_VEC_AT(&frame->removed, txn_TupleU32VecTupleHandleWriter_t,
															  frame->at);
		uint8		be[4];
		size_t		zipped;

		std_vec_with_capacity_Checked(&frame->handles, key->f1.len, TXN_FOLD_HANDLE);
		frame->handles_live = true;
		for (size_t i = 0; i < key->f1.len; i++)
			std_vec_push_Checked(&frame->handles, &STD_VEC_AT(&key->f1, txn_TupleHandleWriter_t, i).f0,
								 TXN_FOLD_HANDLE);
		std_u32_to_be_bytes(key->f0, be);
		if (!txn_memtable_table_removed(frame->tree, frame->io, host_shared_Held_record(frame->held), be, sizeof(be),
										frame->handles.ptr, frame->handles.len, &frame->taken, frame->error))
			return;
		frame->taken_live = true;
		zipped = Min(key->f1.len, frame->taken.len);
		for (size_t i = 0; i < zipped; i++)
		{
			const TxnMemtableVersionWriter *writer = &STD_VEC_AT(&key->f1, txn_TupleHandleWriter_t, i).f1;
			HostXid		top = {writer->has_top ? writer->top : writer->xid};
			bool		has_added;
			uint64		added;

			if (!STD_VEC_AT(&frame->taken, bool, i) || txn_memtable_version_Writer_PartialEq_eq(writer, &none))
				continue;
			if (!txn_status_add(frame->io, top, PG_UINT64_MAX, &has_added, &added, frame->error))
				return;
		}
		frame->taken_live = false;
		std_vec_free_Checked(&frame->taken, sizeof(bool), _Alignof(bool));
		frame->handles_live = false;
		std_vec_free_Checked(&frame->handles, TXN_FOLD_HANDLE);
		txn_fold_Removed_drop(key);
	}
	frame->ok = true;
}

/* txn_fold_unspaced_pad frees what unspaced holds live: a key's removal and handles, and the keys left. */
static void
txn_fold_unspaced_pad(void *frame_)
{
	txn_fold_unspaced_frame_t *frame = frame_;

	if (frame->taken_live)
		std_vec_free_Checked(&frame->taken, sizeof(bool), _Alignof(bool));
	if (frame->handles_live)
		std_vec_free_Checked(&frame->handles, TXN_FOLD_HANDLE);
	for (; frame->at < frame->removed.len; frame->at++)
		txn_fold_Removed_drop(&STD_VEC_AT(&frame->removed, txn_TupleU32VecTupleHandleWriter_t, frame->at));
	std_vec_free_Checked(&frame->removed, TXN_FOLD_REMOVED);
	frame->taken_live = frame->handles_live = false;
}

/*
 * txn_fold_unspaced takes out of a storage's map, through the remover, the
 * versions an installed base now holds, as spaced or valued read them,
 * taking the list, and takes back the count of each version this removal
 * took out at its writer's entry; a version written by no transaction counts
 * nothing.
 */
static bool
txn_fold_unspaced(HostIoIo *io, const HostSharedHeld *held, const TxnMemtableTableTreeOps *tree, StdVec *removed,
				  HostError *error)
{
	txn_fold_unspaced_frame_t frame = {.io = io, .held = held, .tree = tree, .removed = *removed, .error = error};

	std_landing(&frame, txn_fold_unspaced_body, txn_fold_unspaced_pad);
	txn_fold_unspaced_pad(&frame);
	return frame.ok;
}

/* txn_fold_dropped_closure_0_captures_t is dropped's walk's captures: the state, the point, and the pairing. */
typedef struct txn_fold_dropped_closure_0_captures_t
{
	HostSharedRecord record;
	HostLsn		point;
	TxnFoldPaired paired;
	void	   *paired_captures;
} txn_fold_dropped_closure_0_captures_t;

/*
 * txn_fold_dropped_closure_0_frame_t is the walk's state across its pad: its
 * hold, the key's versions, those the point is done with, the unpaired
 * rows' tuple ids, the rows under Every, and the removal, each while live.
 */
typedef struct txn_fold_dropped_closure_0_frame_t
{
	txn_fold_dropped_closure_0_captures_t *walk;
	HostIoIo   *io;
	const uint8 *key;
	size_t		key_len;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		versions;
	bool		versions_live;
	StdVec		done;
	bool		done_live;
	StdVec		unpaired;
	bool		unpaired_live;
	TxnVisibleSeen every;
	bool		every_live;
	StdVec		handles;
	bool		handles_live;
	StdVec		taken;
	bool		taken_live;
	HostError  *error;
	bool		ok;
} txn_fold_dropped_closure_0_frame_t;

/* txn_fold_rows_holds reports whether a list of tuple ids holds one. */
static bool
txn_fold_rows_holds(const StdVec *rows, HostRow tuple)
{
	for (size_t i = 0; i < rows->len; i++)
		if (STD_VEC_AT(rows, HostRow, i).f0 == tuple.f0)
			return true;
	return false;
}

/*
 * txn_fold_dropped_closure_0_body lists the key's versions the point is done
 * with, stores each unpaired committed row's ender on its take, then takes
 * the done versions out and uncounts each this removal took.
 */
static void
txn_fold_dropped_closure_0_body(void *frame_)
{
	txn_fold_dropped_closure_0_frame_t *frame = frame_;
	txn_fold_dropped_closure_0_captures_t *walk = frame->walk;

	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (!txn_memtable_table_versions(frame->io, &frame->hold, walk->record, frame->key, frame->key_len,
									 &frame->versions, frame->error))
		return;
	frame->versions_live = true;
	std_vec_new(&frame->done, TXN_FOLD_POINTER);
	frame->done_live = true;
	std_vec_new(&frame->unpaired, sizeof(HostRow), _Alignof(HostRow));
	frame->unpaired_live = true;
	for (size_t i = 0; i < frame->versions.len; i++)
	{
		TxnMemtableVersionVersion *version = &STD_VEC_AT(&frame->versions, TxnMemtableVersionVersion, i);
		TxnFoldApplied applied;
		TxnFoldApplied stays = {.tag = TXN_FOLD_APPLIED_STAYS};
		TxnVisibleRowVersion row;
		bool		paired;

		if (!txn_fold_applied(frame->io, version, walk->point, &applied, frame->error))
			return;
		if (!txn_fold_Applied_PartialEq_eq(&applied, &stays))
		{
			if (!std_vec_try_reserve(&frame->done, 1, TXN_FOLD_POINTER))
			{
				*frame->error = host_Error_out_of_memory();
				return;
			}
			std_vec_push_Checked(&frame->done, &version, TXN_FOLD_POINTER);
		}
		if (!txn_visible_RowVersion_of(version, &row) || applied.tag != TXN_FOLD_APPLIED_YES)
			continue;
		if (!walk->paired(walk->paired_captures, frame->io, row.put.tuple.f0, &paired, frame->error))
			return;
		if (paired)
			continue;
		if (!std_vec_try_reserve(&frame->unpaired, 1, sizeof(HostRow), _Alignof(HostRow)))
		{
			*frame->error = host_Error_out_of_memory();
			return;
		}
		std_vec_push_Checked(&frame->unpaired, &row.put.tuple, sizeof(HostRow), _Alignof(HostRow));
	}
	if (frame->unpaired.len > 0)
	{
		if (!txn_visible_seen(frame->io, TXN_FOLD_EVERY, frame->versions.ptr, frame->versions.len, NULL,
							  &frame->every, frame->error))
			return;
		frame->every_live = true;
		for (size_t i = 0; i < frame->every.rows.len; i++)
		{
			const TxnVisibleSeenRow *seen = &STD_VEC_AT(&frame->every.rows, TxnVisibleSeenRow, i);

			if (seen->changer == NULL || !txn_fold_rows_holds(&frame->unpaired, seen->row.put.tuple))
				continue;
			if (!txn_memtable_map_ended(frame->io, walk->record, seen->row.put.tuple,
										(uint32) txn_fold_writer_xid(seen->changer), frame->error))
				return;
		}
		frame->every_live = false;
		txn_visible_Seen_drop(&frame->every);
	}
	if (frame->done.len == 0)
	{
		frame->ok = true;
		return;
	}
	std_vec_with_capacity_Checked(&frame->handles, frame->done.len, TXN_FOLD_HANDLE);
	frame->handles_live = true;
	for (size_t i = 0; i < frame->done.len; i++)
		std_vec_push_Checked(&frame->handles, &STD_VEC_AT(&frame->done, TxnMemtableVersionVersion *, i)->handle,
							 TXN_FOLD_HANDLE);
	if (!txn_memtable_table_removed(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, frame->io, walk->record, frame->key,
									frame->key_len, frame->handles.ptr, frame->handles.len, &frame->taken,
									frame->error))
		return;
	frame->taken_live = true;
	for (size_t i = 0; i < Min(frame->done.len, frame->taken.len); i++)
		if (STD_VEC_AT(&frame->taken, bool, i) &&
			!txn_memtable_table_uncounted(frame->io, STD_VEC_AT(&frame->done, TxnMemtableVersionVersion *, i),
										  frame->error))
			return;
	frame->ok = true;
}

/* txn_fold_dropped_closure_0_pad frees what the walk holds live. */
static void
txn_fold_dropped_closure_0_pad(void *frame_)
{
	txn_fold_dropped_closure_0_frame_t *frame = frame_;

	if (frame->taken_live)
		std_vec_free_Checked(&frame->taken, sizeof(bool), _Alignof(bool));
	if (frame->handles_live)
		std_vec_free_Checked(&frame->handles, TXN_FOLD_HANDLE);
	if (frame->every_live)
		txn_visible_Seen_drop(&frame->every);
	if (frame->unpaired_live)
		std_vec_free_Checked(&frame->unpaired, sizeof(HostRow), _Alignof(HostRow));
	if (frame->done_live)
		std_vec_free_Checked(&frame->done, TXN_FOLD_POINTER);
	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->taken_live = frame->handles_live = frame->every_live = frame->unpaired_live = false;
	frame->done_live = frame->versions_live = frame->hold_live = false;
}

/* txn_fold_dropped_closure_0 is dropped's walk's closure, of one key of the rows, a killed one's too. */
static bool
txn_fold_dropped_closure_0(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
						   size_t values_len, HostError *error)
{
	txn_fold_dropped_closure_0_frame_t frame = {.walk = captures, .io = io, .key = key, .key_len = key_len,
	.error = error};

	std_landing(&frame, txn_fold_dropped_closure_0_body, txn_fold_dropped_closure_0_pad);
	txn_fold_dropped_closure_0_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_dropped takes out of a storage's rows, through the remover, every
 * version a fold at a point is done with: those it applies and those it
 * drops, so the delta and a base every snapshot may read never hold a change
 * twice. Before it takes out a committed row whose tuple id the installed
 * base doesn't pair (paired), it stores its ender's xid on the row's take. It
 * then takes out the spills of the transactions whose commits the point
 * applies.
 */
static bool
txn_fold_dropped(HostIoIo *io, const HostSharedHeld *held, HostLsn point, TxnFoldPaired paired,
				 void *paired_captures, HostError *error)
{
	HostSharedRecord record = host_shared_Held_record(held);
	txn_fold_dropped_closure_0_captures_t walk = {record, point, paired, paired_captures};

	return txn_memtable_table_walked_killed(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, record,
											txn_fold_dropped_closure_0, &walk, NULL, error) &&
		txn_spill_folded(io, record, point, error) && txn_state_aged(io, record, error);
}

/*
 * txn_fold_pended_closure_0_captures_t is pended's pin scope's captures: the
 * pending tree and the tuple ids it lists.
 */
typedef struct txn_fold_pended_closure_0_captures_t
{
	TreesStoreTree tree;
	StdVec	   *tids;
} txn_fold_pended_closure_0_captures_t;

/* txn_fold_pended_closure_0_frame_t is pended's pin scope's state across its pad: its cursor, while live. */
typedef struct txn_fold_pended_closure_0_frame_t
{
	txn_fold_pended_closure_0_captures_t *captures;
	HostIoIo   *io;
	TreesPinPin *pin;
	TreesCursorCursor cursor;
	bool		cursor_live;
	ChunksErrorError *error;
	bool		ok;
} txn_fold_pended_closure_0_frame_t;

/* txn_fold_pended_closure_0_body walks the pending tree, listing each key's tuple id. */
static void
txn_fold_pended_closure_0_body(void *frame_)
{
	txn_fold_pended_closure_0_frame_t *frame = frame_;

	if (!trees_cursor_Cursor_first(frame->io, frame->pin, TREES_STORE_TREE_NAMED(&frame->captures->tree),
								   &frame->cursor, frame->error))
		return;
	frame->cursor_live = true;
	while (trees_cursor_Cursor_is_valid(&frame->cursor))
	{
		StdBytes	key = trees_cursor_Cursor_key(&frame->cursor);
		uint64		tid;
		HostError	host;

		if (!txn_base_tuple_id_of(key.ptr, key.len, &tid, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			return;
		}
		std_vec_push_Checked(frame->captures->tids, &tid, TXN_FOLD_U64);
		if (!trees_cursor_Cursor_next(&frame->cursor, frame->io, frame->pin, frame->error))
			return;
	}
	frame->ok = true;
}

/* txn_fold_pended_closure_0_pad frees the pin scope's cursor, while live. */
static void
txn_fold_pended_closure_0_pad(void *frame_)
{
	txn_fold_pended_closure_0_frame_t *frame = frame_;

	if (frame->cursor_live)
		trees_cursor_Cursor_drop(&frame->cursor);
	frame->cursor_live = false;
}

/* txn_fold_pended_closure_0 is pended's pin scope: the tuple ids the pending tree holds. */
static bool
txn_fold_pended_closure_0(void *captures, HostIoIo *io, TreesPinPin *pin, ChunksErrorError *error)
{
	txn_fold_pended_closure_0_frame_t frame = {.captures = captures, .io = io, .pin = pin, .error = error};

	std_landing(&frame, txn_fold_pended_closure_0_body, txn_fold_pended_closure_0_pad);
	txn_fold_pended_closure_0_pad(&frame);
	return frame.ok;
}

/* txn_fold_pending_holds reports whether pending rows, in tuple id order, hold a tuple id. */
static bool
txn_fold_pending_holds(const StdVec *pending, uint64 tid)
{
	size_t		low = 0;
	size_t		high = pending->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		uint64		at = STD_VEC_AT(pending, TxnFoldPendingRow, mid).tuple;

		if (at == tid)
			return true;
		if (at < tid)
			low = mid + 1;
		else
			high = mid;
	}
	return false;
}

/* txn_fold_edit_order orders edits by their keys' bytes, for qsort. */
static int
txn_fold_edit_order(const void *a, const void *b)
{
	StdBytes	x = trees_mutate_Edit_key(a);
	StdBytes	y = trees_mutate_Edit_key(b);
	int			c = memcmp(x.ptr, y.ptr, Min(x.len, y.len));

	if (c != 0)
		return c;
	return (x.len > y.len) - (x.len < y.len);
}

/*
 * txn_fold_pended_frame_t is pended's state across its pad: the tuple ids the
 * tree holds and the edits, each while live.
 */
typedef struct txn_fold_pended_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const HostSharedHeld *held;
	const TxnBaseBase *base;
	const StdVec *pending;
	const StdVec *paired;
	StdVec		tids;
	bool		tids_live;
	StdVec		edits;
	bool		edits_live;
	StdVec		named;
	bool		named_live;
	StdVec	   *result;
	bool	   *left;
	HostError  *error;
	bool		ok;
} txn_fold_pended_frame_t;

/*
 * txn_fold_pended_body removes each tuple id the tree holds that the fold
 * doesn't make pending and that a row took again or VACUUM freed, counts
 * those it keeps, then puts each tuple id the fold makes pending, and sorts
 * the edits by key.
 */
static void
txn_fold_pended_body(void *frame_)
{
	txn_fold_pended_frame_t *frame = frame_;
	size_t		left = frame->pending->len;
	ChunksErrorError failed;

	std_vec_new(&frame->tids, TXN_FOLD_U64);
	frame->tids_live = true;
	if (frame->base->has_pending)
	{
		txn_fold_pended_closure_0_captures_t captures = {frame->base->pending, &frame->tids};

		if (!trees_pin_pin_ResultUnit(frame->io, frame->store, txn_fold_pended_closure_0, &captures, NULL, &failed))
		{
			*frame->error = txn_base_storage(&failed);
			return;
		}
	}
	std_vec_new(&frame->edits, TXN_FOLD_EDIT);
	frame->edits_live = true;
	for (size_t i = 0; i < frame->tids.len; i++)
	{
		uint64		tid = STD_VEC_AT(&frame->tids, uint64, i);
		bool		frees;
		TreesMutateEdit edit = {.tag = TREES_MUTATE_EDIT_DELETE};
		uint8		key[6];

		if (txn_fold_pending_holds(frame->pending, tid))
			continue;
		if (!txn_fold_holds(frame->paired, tid))
		{
			if (!txn_vacuum_frees(frame->io, frame->store, frame->held, frame->base, tid, &frees, frame->error))
				return;
			if (!frees)
			{
				left++;
				continue;
			}
		}
		txn_base_tuple_id_key(tid, key);
		std_vec_reserve_Checked(&frame->edits, 1, TXN_FOLD_EDIT);
		txn_fold_bytes(key, sizeof(key), &edit.u.delete);
		std_vec_push_Checked(&frame->edits, &edit, TXN_FOLD_EDIT);
	}
	for (size_t i = 0; i < frame->pending->len; i++)
	{
		const TxnFoldPendingRow *row = &STD_VEC_AT(frame->pending, TxnFoldPendingRow, i);
		TreesMutateEdit edit = {.tag = TREES_MUTATE_EDIT_PUT};
		StdVec		value;
		uint8		key[6];

		txn_base_tuple_id_key(row->tuple, key);
		std_vec_reserve_Checked(&frame->edits, 1, TXN_FOLD_EDIT);
		txn_fold_bytes(key, sizeof(key), &frame->named);
		frame->named_live = true;
		txn_base_pending_value(row->ender, row->bytes, row->ids.ptr, row->ids.len, &value);
		frame->named_live = false;
		trees_build_Entry_new_VecU8VecU8(&frame->named, &value, &edit.u.put);
		std_vec_push_Checked(&frame->edits, &edit, TXN_FOLD_EDIT);
	}
	qsort(frame->edits.ptr, frame->edits.len, sizeof(TreesMutateEdit), txn_fold_edit_order);
	*frame->result = frame->edits;
	frame->edits_live = false;
	*frame->left = left > 0;
	frame->ok = true;
}

/* txn_fold_pended_pad frees what pended holds live: a key, the edits, and the tuple ids. */
static void
txn_fold_pended_pad(void *frame_)
{
	txn_fold_pended_frame_t *frame = frame_;

	if (frame->named_live)
		std_vec_free_Checked(&frame->named, TXN_FOLD_BYTES);
	if (frame->edits_live)
		std_vec_drop_Checked(&frame->edits, TXN_FOLD_EDIT, trees_mutate_Edit_drop);
	if (frame->tids_live)
		std_vec_free_Checked(&frame->tids, TXN_FOLD_U64);
	frame->named_live = frame->edits_live = frame->tids_live = false;
}

/*
 * txn_fold_pended returns the edits of a storage's pending tuple ids' tree a
 * fold makes, in key order, and whether the tree holds a tuple id after
 * them: a put of each base row's tuple id the fold unpairs, with its ender's
 * xid and its heap bytes, and a removal of each pending tuple id the fold
 * doesn't unpair that VACUUM has freed or that a row took again, the fold
 * pairing it (paired, ascending).
 */
static bool
txn_fold_pended(HostIoIo *io, TreesStoreNodeStore store, const HostSharedHeld *held, const TxnBaseBase *base,
				const StdVec *pending, const StdVec *paired, StdVec *result, bool *left, HostError *error)
{
	txn_fold_pended_frame_t frame = {.io = io, .store = store, .held = held, .base = base, .pending = pending,
	.paired = paired, .result = result, .left = left, .error = error};

	std_landing(&frame, txn_fold_pended_body, txn_fold_pended_pad);
	txn_fold_pended_pad(&frame);
	return frame.ok;
}

/* txn_TupleU64Bool_t is folded's (u64, bool): a tuple id the fold's edits pair or unpair, true for a pair. */
typedef struct txn_TupleU64Bool_t
{
	uint64		f0;
	bool		f1;
} txn_TupleU64Bool_t;

/*
 * TxnFoldInstalled is what a register's run of an install hands back: the
 * base it installed and the packs it wrote, or none when it installed
 * nothing.
 */
typedef struct TxnFoldInstalled
{
	bool		some;
	TxnBaseBase next;
	/* packs is a Vec<PackId>. */
	StdVec		packs;
} TxnFoldInstalled;

/* txn_fold_Installed_drop frees an install's base and packs. */
static void
txn_fold_Installed_drop(TxnFoldInstalled *self)
{
	if (!self->some)
		return;
	txn_base_Base_drop(&self->next);
	std_vec_free_Checked(&self->packs, TXN_FOLD_PACK);
	self->some = false;
}

/* txn_fold_published_drop frees a writer's published roots. */
static void
txn_fold_published_drop(StdVec *published)
{
	for (size_t k = 0; k < published->len; k++)
		trees_published_Published_drop(&STD_VEC_AT(published, TreesPublishedPublished, k));
	std_vec_free_Checked(published, sizeof(TreesPublishedPublished), _Alignof(TreesPublishedPublished));
}

static bool txn_fold_reformed(HostIoIo *io, TreesStoreNodeStore store, const TxnBaseBase *base,
							  const TxnVisibleLive *chain, size_t chain_len, StdVec *edits, StdVec *formed,
							  StdVec *ctids, TxnFoldReform *reform, StdVec *out_edits, StdVec *out_ctids,
							  TxnBaseSizes *sizes, uint32 *epoch, HostError *error);
static bool txn_fold_matched(HostIoIoDoors io, const HostSharedHeld *held, const TxnBaseBase *folded,
							 TxnFoldReached *reached, const TreesKeyOrder *order, bool *result, HostError *error);

/*
 * txn_fold_folded_frame_t is folded's state across its pad: the newest base,
 * its chain and live transforms, the rows' and maps' folds, the edits in key
 * order, the writer and its roots' places, the pairing, and the install,
 * each while live.
 */
typedef struct txn_fold_folded_frame_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const HostSharedHeld *held;
	const TreesKeyOrder *order;
	bool		keyless;
	HostLsn		point;
	TxnFoldReform *reform;
	TxnFoldReached *reached;
	uint64		generation;
	TxnBaseBase base;
	bool		base_live;
	TreesStoreReader reader;
	StdVec		chain;
	bool		chain_live;
	StdVec		chained;
	bool		chained_live;
	StdVec		live;
	bool		live_live;
	TxnFoldFoldedRows rows;
	bool		rows_live;
	StdVec		edits;
	bool		edits_live;
	StdVec		ctids;
	bool		ctids_live;
	bool		has_sizes;
	TxnBaseSizes sizes;
	uint32		epoch;
	TxnFoldSpaced values;
	bool		values_live;
	StdVec		keys;
	bool		keys_live;
	StdVec		positions;
	bool		positions_live;
	StdVec		sorted;
	bool		sorted_live;
	TreesShape	shape;
	bool		shape_live;
	TreesStoreWriter plain;
	TreesStoreBrandedWriter writer;
	bool		writer_live;
	size_t		root;
	bool		has_paired;
	size_t		paired;
	StdVec		pairs;
	bool		pairs_live;
	txn_fold_held_pairing_t held_pairing;
	TxnFoldSpaced spaces;
	bool		spaces_live;
	bool		has_spaced;
	size_t		spaced;
	StdVec		paired_now;
	bool		paired_now_live;
	StdVec		pends;
	bool		pends_live;
	bool		keeps;
	bool		has_pended;
	size_t		pended;
	uint64		max_folded;
	uint64		counter;
	uint64		fresh_end;
	TxnFoldInstalled installed;
	bool	   *result;
	HostError  *error;
	bool		ok;
} txn_fold_folded_frame_t;

/*
 * txn_fold_folded_closure_8 is folded's pairing: a tuple id its edits pair or
 * unpair, else the base's tuple id tree's.
 */
static bool
txn_fold_folded_closure_8(void *captures, HostIoIo *io, uint64 tuple, bool *result, HostError *error)
{
	txn_fold_folded_frame_t *frame = captures;
	size_t		low = 0;
	size_t		high = frame->pairs.len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;
		const txn_TupleU64Bool_t *pair = &STD_VEC_AT(&frame->pairs, txn_TupleU64Bool_t, mid);

		if (pair->f0 == tuple)
		{
			*result = pair->f1;
			return true;
		}
		if (pair->f0 < tuple)
			low = mid + 1;
		else
			high = mid;
	}
	return txn_fold_held_pairing(&frame->held_pairing, io, tuple, result, error);
}

/* txn_fold_folded_closure_11_frame_t is folded's register run's state across its pad: the roots it published. */
typedef struct txn_fold_folded_closure_11_frame_t
{
	txn_fold_folded_frame_t *folded;
	HostIoRegistering *registering;
	ChunksPacksRootsRootSlots *slots;
	TxnFoldInstalled *result;
	ChunksErrorError *error;
	StdVec		published;
	bool		published_live;
	bool		ok;
} txn_fold_folded_closure_11_frame_t;

static bool txn_fold_held(TreesStoreBrandedWriter *writer, HostIoTakesShort io, ChunksPacksRootsRootSlot *slot,
						  bool has_root, size_t root, bool held, ChunksErrorError *error) STD_RELEASE(*slot);

/* txn_fold_held holds a root at its slot while every root before it held, or marks the slot unused. */
static bool
txn_fold_held(TreesStoreBrandedWriter *writer, HostIoTakesShort io, ChunksPacksRootsRootSlot *slot, bool has_root,
			  size_t root, bool held, ChunksErrorError *error)
{
	if (has_root && held)
		return trees_store_BrandedWriter_hold(writer, io, slot, root, error);
	chunks_packs_roots_RootSlot_unused(slot);
	return held;
}

/*
 * txn_fold_folded_closure_11_body holds the fold's roots in the fold's four
 * slots, publishes them, and installs the base that names them when the
 * generation is still the one read before the fold and a standby's roots
 * match its primary's.
 */
static void
txn_fold_folded_closure_11_body(void *frame_)
{
	txn_fold_folded_closure_11_frame_t *frame = frame_;
	txn_fold_folded_frame_t *folded = frame->folded;
	HostIoTakesShort io = {&HOST_IO_REGISTERING_TAKES_SHORT_OPS, frame->registering};
	HostIoIoDoors doors = {&HOST_IO_REGISTERING_TAKES_SHORT_OPS.io_doors, frame->registering};
	ChunksPacksRootsFold slot;
	TxnBaseBase *next = &frame->result->next;
	size_t		at = 1;
	bool		held;
	bool		installed;
	HostError	host;

	chunks_packs_roots_RootSlots_fold(frame->slots, &slot);
	held = txn_fold_held(&folded->writer, io, slot.s0, true, folded->root, true, frame->error);
	held = txn_fold_held(&folded->writer, io, slot.s1, folded->has_paired, folded->paired, held, frame->error);
	held = txn_fold_held(&folded->writer, io, slot.s2, folded->has_spaced, folded->spaced, held, frame->error);
	held = txn_fold_held(&folded->writer, io, slot.s3, folded->has_pended, folded->pended, held, frame->error);
	if (!held)
		return;
	folded->writer_live = false;
	if (!trees_store_BrandedWriter_publish(&folded->writer, io, &frame->published, &frame->result->packs,
										   frame->error))
		return;
	frame->published_live = true;
	*next = folded->base;
	folded->base_live = false;
	frame->result->some = true;
	next->has_root = true;
	next->root = trees_published_Published_base(&STD_VEC_AT(&frame->published, TreesPublishedPublished, 0));
	if (folded->has_paired)
		next->tuple_ids = trees_published_Published_base(&STD_VEC_AT(&frame->published, TreesPublishedPublished,
																	 at++));
	next->has_tuple_ids = folded->has_paired || folded->base.has_tuple_ids;
	if (folded->has_spaced)
		next->free_space = trees_published_Published_base(&STD_VEC_AT(&frame->published, TreesPublishedPublished,
																	  at++));
	next->has_free_space = folded->has_spaced || folded->base.has_free_space;
	if (!folded->keeps)
	{
		next->has_pending = folded->has_pended;
		if (folded->has_pended)
			next->pending = trees_published_Published_base(&STD_VEC_AT(&frame->published, TreesPublishedPublished,
																	   at++));
	}
	next->counter = folded->counter;
	next->fresh_end = folded->fresh_end;
	next->folded = folded->point;
	next->max_folded = folded->max_folded;
	next->max_ended = Max(folded->base.max_ended, folded->rows.max_ended);
	next->sizes = folded->sizes;
	next->epoch = folded->epoch;
	next->has_chain = false;
	if (folded->reached != NULL)
	{
		bool		matched;

		if (!txn_fold_matched(doors, folded->held, next, folded->reached, folded->order, &matched, &host))
		{
			*frame->error = chunks_error_Error_FromError_from(&host);
			return;
		}
		if (!matched)
		{
			txn_fold_Installed_drop(frame->result);
			frame->ok = true;
			return;
		}
	}
	if (!txn_state_installed(io, folded->held, folded->generation, next, &installed, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	if (!installed)
		txn_fold_Installed_drop(frame->result);
	frame->ok = true;
}

/*
 * txn_fold_folded_closure_11_pad drops what holds a root's Pin, the writer
 * and its held roots and the published names, so that no Pin outlives the
 * registration; on an unwind it drops the install too.
 */
static void
txn_fold_folded_closure_11_pad(void *frame_)
{
	txn_fold_folded_closure_11_frame_t *frame = frame_;

	if (frame->published_live)
		txn_fold_published_drop(&frame->published);
	if (frame->folded->writer_live)
		trees_store_BrandedWriter_drop(&frame->folded->writer);
	frame->published_live = frame->folded->writer_live = false;
}

/* txn_fold_folded_closure_11_body_then_pad is the run's body, which drops the Pins it made before it returns. */
static void
txn_fold_folded_closure_11_landing(void *frame_)
{
	txn_fold_folded_closure_11_frame_t *frame = frame_;

	txn_fold_folded_closure_11_body(frame);
	if (!frame->ok)
		txn_fold_Installed_drop(frame->result);
	txn_fold_folded_closure_11_pad(frame);
}

/* txn_fold_folded_closure_11_unwound drops what the run holds when it unwinds: its Pins and its install. */
static void
txn_fold_folded_closure_11_unwound(void *frame_)
{
	txn_fold_folded_closure_11_frame_t *frame = frame_;

	txn_fold_Installed_drop(frame->result);
	txn_fold_folded_closure_11_pad(frame);
}

/* txn_fold_folded_closure_11 is folded's run under the root slots: its install, with every Pin dropped inside it. */
static bool
txn_fold_folded_closure_11(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
						   void *result, ChunksErrorError *error)
{
	txn_fold_folded_closure_11_frame_t frame = {.folded = captures, .registering = registering, .slots = slots,
	.result = result, .error = error};

	frame.result->some = false;
	std_landing(&frame, txn_fold_folded_closure_11_landing, txn_fold_folded_closure_11_unwound);
	return frame.ok;
}

/* txn_fold_pair_insert puts a pairing in tuple id order, replacing one of its tuple id, as a BTreeMap's insert does. */
static void
txn_fold_pair_insert(StdVec *pairs, txn_TupleU64Bool_t pair)
{
	size_t		low = 0;
	size_t		high = pairs->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;

		if (STD_VEC_AT(pairs, txn_TupleU64Bool_t, mid).f0 < pair.f0)
			low = mid + 1;
		else
			high = mid;
	}
	if (low < pairs->len && STD_VEC_AT(pairs, txn_TupleU64Bool_t, low).f0 == pair.f0)
	{
		STD_VEC_AT(pairs, txn_TupleU64Bool_t, low) = pair;
		return;
	}
	std_vec_insert(pairs, low, &pair, TXN_FOLD_PAIR, RUST_FILE, 753, 97);
}

/*
 * txn_fold_folded_chain reads the newest base and its chain, its entries
 * whose DDLs haven't ended, and its live transforms, answering false when a
 * live transform needs a re-form the fold lacks or a commit past the point.
 */
static bool
txn_fold_folded_chain(txn_fold_folded_frame_t *frame, bool *go)
{
	TreesStoreNodeStore store = TREES_STORE_READER_NODE_STORE(&frame->reader);
	bool		some;
	bool		committed = true;

	*go = false;
	frame->generation = txn_state_generation(host_shared_Held_record(frame->held));
	if (!txn_state_newest(frame->io, frame->held, &some, &frame->base, frame->error))
		return false;
	if (!some)
	{
		*frame->error = txn_fold_fork("XX000", "a fold found a storage state with no base");
		return false;
	}
	frame->base_live = true;
	frame->reader = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
	store = TREES_STORE_READER_NODE_STORE(&frame->reader);
	if (frame->base.has_chain)
	{
		if (!txn_base_chain_of(frame->io, store, true, frame->base.chain, &frame->chain, frame->error))
			return false;
	}
	else
		std_vec_new(&frame->chain, TXN_FOLD_CHAIN);
	frame->chain_live = true;
	std_vec_new(&frame->chained, TXN_FOLD_U64);
	frame->chained_live = true;
	for (size_t i = 0; i < frame->chain.len; i++)
		if (!STD_VEC_AT(&frame->chain, TxnBasePending, i).has_end)
			std_vec_push_Checked(&frame->chained, &STD_VEC_AT(&frame->chain, TxnBasePending, i).xid, TXN_FOLD_U64);
	frame->chain_live = false;
	if (!txn_visible_live(frame->io, &frame->chain, &frame->live, frame->error))
		return false;
	frame->live_live = true;
	for (size_t i = 0; i < frame->live.len; i++)
	{
		const TxnVisibleLive *live = &STD_VEC_AT(&frame->live, TxnVisibleLive, i);

		if (!live->has_f1 || live->f1 > frame->point)
		{
			committed = false;
			break;
		}
	}
	*go = frame->live.len == 0 || (frame->reform != NULL && committed);
	return true;
}

/* txn_fold_folded_sort puts the base's edits in the storage's key order before any registration. */
static bool
txn_fold_folded_sort(txn_fold_folded_frame_t *frame)
{
	ChunksErrorError failed;

	std_vec_with_capacity_Checked(&frame->keys, frame->edits.len, sizeof(StdBytes), _Alignof(StdBytes));
	frame->keys_live = true;
	for (size_t i = 0; i < frame->edits.len; i++)
	{
		StdBytes	key = trees_mutate_Edit_key(&STD_VEC_AT(&frame->edits, TreesMutateEdit, i));

		std_vec_push_Checked(&frame->keys, &key, sizeof(StdBytes), _Alignof(StdBytes));
	}
	if (!trees_sorted_positions(frame->io, frame->keys.ptr, frame->keys.len, frame->order, &frame->positions,
								&failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	frame->positions_live = true;
	std_vec_with_capacity_Checked(&frame->sorted, frame->edits.len, TXN_FOLD_EDIT);
	frame->sorted_live = true;
	for (size_t i = 0; i < frame->positions.len; i++)
		std_vec_push_Checked(&frame->sorted,
							 &STD_VEC_AT(&frame->edits, TreesMutateEdit, STD_VEC_AT(&frame->positions, size_t, i)),
							 TXN_FOLD_EDIT);
	frame->edits_live = false;
	std_vec_free_Checked(&frame->edits, TXN_FOLD_EDIT);
	frame->edits = frame->sorted;
	frame->edits_live = true;
	frame->sorted_live = false;
	return true;
}

/*
 * txn_fold_folded_write writes the new base's trees in one branded writer:
 * the rows' tree, the tuple id tree's edits, the free space map's edits, and
 * the pending tuple ids' tree's edits.
 */
static bool
txn_fold_folded_write(txn_fold_folded_frame_t *frame)
{
	HostSharedRecord record = host_shared_Held_record(frame->held);
	TreesStoreNodeStore store = TREES_STORE_READER_NODE_STORE(&frame->reader);
	TreesStoreReader reader = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
	ChunksErrorError failed;
	bool		left;

	frame->shape.has_summarizer = false;
	trees_KeyOrder_Clone_clone(frame->order, &frame->shape.order);
	frame->shape_live = true;
	if (!trees_store_Writer_new(frame->io, reader, txn_state_space(record), &frame->plain, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	trees_store_Writer_brand(&frame->plain, &frame->writer);
	frame->writer_live = true;
	if (!txn_fold_mutated(&frame->writer, frame->io, frame->base.has_root, frame->base.root, &frame->edits,
						  &frame->edits_live, &frame->shape, &frame->root, frame->error))
		return false;
	frame->has_paired = frame->ctids.len > 0;
	if (frame->has_paired)
	{
		if (!txn_fold_apply_ctids(frame->io, &frame->writer, frame->base.has_tuple_ids, frame->base.tuple_ids,
								  frame->ctids.ptr, frame->ctids.len, &frame->paired, frame->error))
			return false;
	}
	frame->max_folded = Max(frame->base.max_folded, frame->rows.max_folded);
	frame->counter = txn_state_counter(record);
	std_vec_with_capacity_Checked(&frame->pairs, frame->ctids.len, TXN_FOLD_PAIR);
	frame->pairs_live = true;
	for (size_t i = 0; i < frame->ctids.len; i++)
	{
		const TxnFoldCtidEdit *ctid = &STD_VEC_AT(&frame->ctids, TxnFoldCtidEdit, i);
		txn_TupleU64Bool_t pair = {ctid->f0, ctid->has_f1};

		txn_fold_pair_insert(&frame->pairs, pair);
	}
	frame->held_pairing.store = store;
	frame->held_pairing.has_tuple_ids = frame->base.has_tuple_ids;
	frame->held_pairing.tuple_ids = frame->base.tuple_ids;
	if (!txn_fold_spaced(frame->io, frame->held, frame->point, txn_fold_folded_closure_8, frame, &frame->spaces,
						 frame->error))
		return false;
	frame->spaces_live = true;
	frame->has_spaced = frame->spaces.edits.len > 0;
	if (frame->has_spaced)
	{
		TreesShape	shape = txn_free_space_shape();
		bool		edits_live = true;
		bool		done;

		done = txn_fold_mutated(&frame->writer, frame->io, frame->base.has_free_space, frame->base.free_space,
								&frame->spaces.edits, &edits_live, &shape, &frame->spaced, frame->error);
		trees_Shape_drop(&shape);
		if (edits_live)
			std_vec_drop_Checked(&frame->spaces.edits, TXN_FOLD_EDIT, trees_mutate_Edit_drop);
		std_vec_new(&frame->spaces.edits, TXN_FOLD_EDIT);
		if (!done)
			return false;
	}
	if (frame->spaces.emptied)
		frame->fresh_end = frame->counter;
	else
	{
		frame->counter = frame->base.counter;
		frame->fresh_end = frame->base.fresh_end;
	}
	std_vec_new(&frame->paired_now, TXN_FOLD_U64);
	frame->paired_now_live = true;
	for (size_t i = 0; i < frame->pairs.len; i++)
		if (STD_VEC_AT(&frame->pairs, txn_TupleU64Bool_t, i).f1)
			std_vec_push_Checked(&frame->paired_now, &STD_VEC_AT(&frame->pairs, txn_TupleU64Bool_t, i).f0,
								 TXN_FOLD_U64);
	if (!txn_fold_pended(frame->io, store, frame->held, &frame->base, &frame->rows.pending, &frame->paired_now,
						 &frame->pends, &left, frame->error))
		return false;
	frame->pends_live = true;
	frame->keeps = frame->pends.len == 0;
	frame->has_pended = !frame->keeps && left;
	if (frame->has_pended)
	{
		TreesShape	shape = trees_Shape_Default_default();
		bool		done;

		done = txn_fold_mutated(&frame->writer, frame->io, frame->base.has_pending, frame->base.pending,
								&frame->pends, &frame->pends_live, &shape, &frame->pended, frame->error);
		trees_Shape_drop(&shape);
		if (!done)
			return false;
	}
	return true;
}

/*
 * txn_fold_folded_logged flushes a permanent storage's packs and the WAL
 * through the point, and outside recovery logs the installed base's record,
 * taking the install.
 */
static bool
txn_fold_folded_logged(txn_fold_folded_frame_t *frame)
{
	HostIoIoDoors doors = HOST_IO_IO_IO_DOORS(frame->io);
	HostStorage storage;
	TxnStatePersistence persistence;
	ChunksErrorError failed;
	TxnRecordBaseAdvanced advanced;
	bool		injected;
	StdVec		body;
	HostLsn		lsn;
	bool		ok;

	if (!txn_state_described(host_shared_Held_record(frame->held), &storage, &persistence) ||
		persistence != TXN_STATE_PERSISTENCE_PERMANENT)
		return true;
	if (!chunks_packs_backend_Packs_flush(&frame->repo->packs, frame->io, frame->reading, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	if (!host_wal_flush(doors, frame->point, frame->error))
		return false;
	if (host_wal_stand(doors).recovering)
		return true;
	advanced.storage = storage;
	advanced.folded = frame->point;
	advanced.max_folded = frame->max_folded;
	advanced.base = frame->installed.next;
	advanced.packs = frame->installed.packs;
	advanced.built = false;
	advanced.horizon = 0;
	advanced.has_chain = false;
	frame->installed.some = false;
	ok = host_injected(doors, "doltgres-fold-record-root", &injected, frame->error);
	if (ok && injected && advanced.base.has_root)
		advanced.base.root.address = chunks_address_Address_of((const uint8 *) "doltgres-fold-record-root", 25);
	if (ok)
	{
		txn_record_BaseAdvanced_encode(&advanced, &body);
		ok = host_wal_insert(doors, HOST_WAL_KIND_BASE_ADVANCED, body.ptr, body.len, &lsn, frame->error);
		std_vec_free_Checked(&body, TXN_FOLD_BYTES);
	}
	txn_record_BaseAdvanced_drop(&advanced);
	return ok;
}

/*
 * txn_fold_folded_body folds the storage's rows and maps at the point,
 * re-forming its rows through a live chain, writes the new base's trees, and
 * installs the base under the registration that holds its roots, then gives
 * back the chain's counts, logs the base, and drops what the install folded.
 */
static void
txn_fold_folded_body(void *frame_)
{
	txn_fold_folded_frame_t *frame = frame_;
	TreesStoreNodeStore store;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	TxnBaseSizes value_added;
	TxnBaseSizes value_removed;
	ChunksErrorError failed;
	bool		go;

	*frame->result = false;
	if (!txn_fold_folded_chain(frame, &go))
		return;
	if (!go)
	{
		frame->ok = true;
		return;
	}
	store = TREES_STORE_READER_NODE_STORE(&frame->reader);
	if (!txn_fold_folded_rows(frame->io, store, frame->held, frame->base.has_root, frame->base.root,
							  frame->base.has_tuple_ids, frame->base.tuple_ids, frame->order, frame->keyless,
							  frame->base.has_index, frame->point, frame->base.folded, &frame->rows, frame->error))
		return;
	frame->rows_live = true;
	if (frame->rows.overlapping)
	{
		frame->ok = true;
		return;
	}
	if (frame->reform != NULL && frame->live.len > 0)
	{
		StdVec		edits = frame->rows.edits;
		StdVec		formed = frame->rows.formed;
		StdVec		ctids = frame->rows.ctids;

		std_vec_new(&frame->rows.edits, TXN_FOLD_EDIT);
		std_vec_new(&frame->rows.formed, TXN_FOLD_FORMED_BY);
		std_vec_new(&frame->rows.ctids, TXN_FOLD_CTID);
		if (!txn_fold_reformed(frame->io, store, &frame->base, frame->live.ptr, frame->live.len, &edits, &formed,
							   &ctids, frame->reform, &frame->edits, &frame->ctids, &frame->sizes, &frame->epoch,
							   frame->error))
			return;
		frame->has_sizes = true;
	}
	else
	{
		frame->edits = frame->rows.edits;
		frame->ctids = frame->rows.ctids;
		std_vec_new(&frame->rows.edits, TXN_FOLD_EDIT);
		std_vec_new(&frame->rows.ctids, TXN_FOLD_CTID);
		frame->epoch = frame->base.epoch;
	}
	frame->edits_live = frame->ctids_live = true;
	if (!txn_fold_valued(frame->io, frame->held, frame->point, &frame->values, frame->error))
		return;
	frame->values_live = true;
	if (!frame->rows.applied && frame->values.removed.len == 0 && !frame->has_sizes)
	{
		txn_fold_held_pairing_t pairing = {store, frame->base.has_tuple_ids, frame->base.tuple_ids};

		frame->ok = txn_fold_dropped(frame->io, frame->held, frame->base.folded, txn_fold_held_pairing, &pairing,
									 frame->error) &&
			txn_memtable_tuples_emptied(frame->io, record, frame->error);
		return;
	}
	if (!txn_fold_valued_sizes(frame->io, store, frame->base.has_root, frame->base.root, frame->values.edits.ptr,
							   frame->values.edits.len, &value_added, &value_removed, frame->error))
		return;
	if (!frame->has_sizes)
	{
		TxnBaseSizes rows;

		if (!txn_fold_moved(frame->base.sizes, frame->rows.removed, frame->rows.added, &rows, frame->error) ||
			!txn_fold_moved(rows, value_removed, value_added, &frame->sizes, frame->error))
			return;
	}
	if (!std_vec_try_reserve(&frame->edits, frame->values.edits.len, TXN_FOLD_EDIT))
	{
		*frame->error = host_Error_out_of_memory();
		return;
	}
	std_vec_extend_from_slice_Checked(&frame->edits, frame->values.edits.ptr, frame->values.edits.len, TXN_FOLD_EDIT);
	std_vec_free_Checked(&frame->values.edits, TXN_FOLD_EDIT);
	std_vec_new(&frame->values.edits, TXN_FOLD_EDIT);
	if (!txn_fold_folded_sort(frame) || !txn_fold_folded_write(frame))
		return;
	if (!chunks_packs_roots_RootSlots_register(frame->io, txn_fold_folded_closure_11, frame, NULL, &frame->installed,
											   &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	if (!frame->installed.some)
	{
		frame->ok = true;
		return;
	}
	for (size_t i = 0; i < frame->chained.len; i++)
	{
		HostXid		xid = {STD_VEC_AT(&frame->chained, uint64, i)};
		bool		has_added;
		uint64		added;

		if (!txn_status_add(frame->io, xid, PG_UINT64_MAX, &has_added, &added, frame->error))
			return;
	}
	if (!txn_fold_folded_logged(frame) ||
		!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-fold-installed", frame->error) ||
		!txn_fold_dropped(frame->io, frame->held, frame->point, txn_fold_folded_closure_8, frame, frame->error) ||
		!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-drop-installed", frame->error))
		return;
	frame->spaces_live = false;
	std_vec_free_Checked(&frame->spaces.edits, TXN_FOLD_EDIT);
	if (!txn_fold_unspaced(frame->io, frame->held, &TXN_MEMTABLE_MAP_FREE_TREE_OPS, &frame->spaces.removed,
						   frame->error))
		return;
	frame->values_live = false;
	std_vec_free_Checked(&frame->values.edits, TXN_FOLD_EDIT);
	if (!txn_fold_unspaced(frame->io, frame->held, &TXN_MEMTABLE_MAP_VALUES_TREE_OPS, &frame->values.removed,
						   frame->error) ||
		!txn_memtable_tuples_emptied(frame->io, record, frame->error))
		return;
	*frame->result = true;
	frame->ok = true;
}

/* txn_fold_folded_pad frees what folded holds live. */
static void
txn_fold_folded_pad(void *frame_)
{
	txn_fold_folded_frame_t *frame = frame_;

	txn_fold_Installed_drop(&frame->installed);
	if (frame->pends_live)
		std_vec_drop_Checked(&frame->pends, TXN_FOLD_EDIT, trees_mutate_Edit_drop);
	if (frame->paired_now_live)
		std_vec_free_Checked(&frame->paired_now, TXN_FOLD_U64);
	if (frame->spaces_live)
		txn_fold_Spaced_drop(&frame->spaces);
	if (frame->pairs_live)
		std_vec_free_Checked(&frame->pairs, TXN_FOLD_PAIR);
	if (frame->writer_live)
		trees_store_BrandedWriter_drop(&frame->writer);
	if (frame->shape_live)
		trees_Shape_drop(&frame->shape);
	if (frame->sorted_live)
		std_vec_free_Checked(&frame->sorted, TXN_FOLD_EDIT);
	if (frame->positions_live)
		std_vec_free_Checked(&frame->positions, sizeof(size_t), _Alignof(size_t));
	if (frame->keys_live)
		std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
	if (frame->values_live)
		txn_fold_Spaced_drop(&frame->values);
	if (frame->ctids_live)
		std_vec_drop_Checked(&frame->ctids, TXN_FOLD_CTID, txn_fold_CtidEdit_drop);
	if (frame->edits_live)
		std_vec_drop_Checked(&frame->edits, TXN_FOLD_EDIT, trees_mutate_Edit_drop);
	if (frame->rows_live)
		txn_fold_FoldedRows_drop(&frame->rows);
	if (frame->live_live)
		std_vec_drop_Checked(&frame->live, TXN_FOLD_LIVE, txn_fold_Live_drop);
	if (frame->chained_live)
		std_vec_free_Checked(&frame->chained, TXN_FOLD_U64);
	if (frame->chain_live)
		std_vec_drop_Checked(&frame->chain, TXN_FOLD_CHAIN, txn_fold_Pending_drop);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->pends_live = frame->paired_now_live = frame->spaces_live = frame->pairs_live = false;
	frame->writer_live = frame->shape_live = frame->sorted_live = frame->positions_live = false;
	frame->keys_live = frame->values_live = frame->ctids_live = frame->edits_live = frame->rows_live = false;
	frame->live_live = frame->chained_live = frame->chain_live = frame->base_live = false;
}

/*
 * txn_fold_folded folds a storage at a fold point and installs the new base:
 * its rows' edits, sorted by the storage's key order before any
 * registration, applied to its newest base's tree and their tuple id tree's
 * edits to its tuple id tree, with its sizes moved by what it took out and
 * wrote, in a pack of its own and installed in its state under the one
 * registration that holds the new roots, when its base generation is still
 * the one read before the fold. A TOAST relation's storage's value map folds
 * into the same root at the same point. A fold that applies versions
 * installs even when they change no entry; one that applies none and has no
 * value map version to fold installs nothing and drops what the newest
 * base's install is done with. A fold that would leave a keyed table's key
 * with more than one row refuses its install. A base whose chain holds a live
 * transform installs only with a re-form and every live transform's commit
 * at or below the point. A fold in recovery logs no record, and with reached
 * installs only a base whose roots matched accepts. It answers whether it
 * installed.
 */
bool
txn_fold_folded(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const HostSharedHeld *held,
				const TreesKeyOrder *order, bool keyless, HostLsn point, TxnFoldReform *reform,
				TxnFoldReached *reached, bool *result, HostError *error)
{
	txn_fold_folded_frame_t frame = {.io = io, .repo = repo, .reading = reading, .held = held, .order = order,
		.keyless = keyless, .point = point, .reform = reform, .reached = reached, .result = result,
	.error = error};

	std_landing(&frame, txn_fold_folded_body, txn_fold_folded_pad);
	txn_fold_folded_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_point chooses a fold point for fold_now: past every commit
 * delaying a checkpoint, whose end it waits for, and lowered below each
 * transaction the status table holds pending; in recovery there is none,
 * since a standby folds only at its primary's points.
 */
bool
txn_fold_point(HostIoIo *io, bool *has_result, HostLsn *result, HostError *error)
{
	HostIoIoDoors doors = HOST_IO_IO_IO_DOORS(io);
	HostLsn		position;

	*has_result = false;
	if (host_wal_stand(doors).recovering)
		return true;
	if (!host_fold_point(doors, &position, error) || !txn_status_lowered(io, position, result, error))
		return false;
	*has_result = true;
	return true;
}

/*
 * txn_fold_fold_now folds a storage at a point its caller chose and answers
 * whether it installed a base: it first folds at each point a replayed
 * fold's record reached past the newest base, in order, whose roots the
 * fold's must match, warned gathering a comparator tree's report that
 * doesn't; in recovery, with no point chosen, it folds at no other point, and
 * otherwise, once no reached point is left, at the chosen point, bounded by
 * the storage's horizon.
 */
bool
txn_fold_fold_now(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const HostSharedHeld *held,
				  const TreesKeyOrder *order, bool keyless, bool has_point, HostLsn point, uint64 horizon,
				  StdVec *warned, const TxnInsertPage *page, TxnFoldReform *reform, bool *result, HostError *error)
{
	bool		installed;
	bool		left;
	HostLsn		bounded;
	bool		folded;

	if (!txn_fold_in_order(io, repo, reading, held, order, keyless, warned, page, 0, &installed, &left, error))
		return false;
	*result = installed;
	if (!has_point || left)
		return true;
	if (!txn_fold_bounded(io, held, point, horizon, &bounded, error) ||
		!txn_fold_folded(io, repo, reading, held, order, keyless, bounded, reform, NULL, &folded, error))
		return false;
	*result = folded || installed;
	return true;
}

/* txn_fold_in_order_frame_t is in_order's state across its pad: a reached point's base, while live. */
typedef struct txn_fold_in_order_frame_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const HostSharedHeld *held;
	const TreesKeyOrder *order;
	bool		keyless;
	StdVec	   *warned;
	const TxnInsertPage *page;
	uint64		upto;
	TxnBaseBase base;
	bool		base_live;
	bool	   *installed;
	bool	   *left;
	HostError  *error;
	bool		ok;
} txn_fold_in_order_frame_t;

/* txn_fold_in_order_body folds at each reached point, oldest first, reading the records past each before the next. */
static void
txn_fold_in_order_body(void *frame_)
{
	txn_fold_in_order_frame_t *frame = frame_;

	*frame->installed = false;
	*frame->left = false;
	for (;;)
	{
		TxnFoldCaught caught;

		if (!txn_state_behind(frame->io, frame->held, &frame->base_live, &frame->base, frame->error))
			return;
		if (frame->base_live)
		{
			TxnFoldReached reached = {&frame->base, frame->warned};
			bool		folded;

			if (!txn_fold_folded(frame->io, frame->repo, frame->reading, frame->held, frame->order, frame->keyless,
								 frame->base.folded, NULL, &reached, &folded, frame->error))
				return;
			frame->base_live = false;
			txn_base_Base_drop(&frame->base);
			if (!folded)
			{
				*frame->left = true;
				frame->ok = true;
				return;
			}
			*frame->installed = true;
		}
		if (!txn_fold_caught_up(frame->io, frame->held, frame->page, frame->upto, &caught, frame->error))
			return;
		if (caught == TXN_FOLD_CAUGHT_NEXT)
			continue;
		*frame->left = caught == TXN_FOLD_CAUGHT_WAITING;
		frame->ok = true;
		return;
	}
}

/* txn_fold_in_order_pad frees what in_order holds live: a reached point's base. */
static void
txn_fold_in_order_pad(void *frame_)
{
	txn_fold_in_order_frame_t *frame = frame_;

	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->base_live = false;
}

/*
 * txn_fold_in_order folds a storage at each point its replayed fold records
 * reached past its newest base, oldest first, and reads the records past
 * each point's record before the next, answering whether it installed a
 * base and whether a reached point or a read past one is left. A record its
 * caller redoes now is upto.
 */
static bool
txn_fold_in_order(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const HostSharedHeld *held,
				  const TreesKeyOrder *order, bool keyless, StdVec *warned, const TxnInsertPage *page, uint64 upto,
				  bool *installed, bool *left, HostError *error)
{
	txn_fold_in_order_frame_t frame = {.io = io, .repo = repo, .reading = reading, .held = held, .order = order,
		.keyless = keyless, .warned = warned, .page = page, .upto = upto, .installed = installed, .left = left,
	.error = error};

	std_landing(&frame, txn_fold_in_order_body, txn_fold_in_order_pad);
	txn_fold_in_order_pad(&frame);
	return frame.ok;
}

/* txn_fold_startup_fold_frame_t is startup_fold's state across its pad: the oldest reached base, its warnings, and its order. */
typedef struct txn_fold_startup_fold_frame_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const HostSharedHeld *held;
	const TxnInsertPage *page;
	uint64		upto;
	TxnBaseBase base;
	bool		base_live;
	StdVec		warned;
	bool		warned_live;
	TreesKeyOrder bytes;
	bool		bytes_live;
	bool	   *result;
	HostError  *error;
	bool		ok;
} txn_fold_startup_fold_frame_t;

/* txn_fold_startup_fold_body folds at the reached points when the oldest one's key orders the rows by their bytes. */
static void
txn_fold_startup_fold_body(void *frame_)
{
	txn_fold_startup_fold_frame_t *frame = frame_;
	bool		keyless = false;
	bool		left;

	*frame->result = false;
	if (!txn_state_behind(frame->io, frame->held, &frame->base_live, &frame->base, frame->error))
		return;
	if (frame->base_live)
	{
		if (frame->base.key.tag == TXN_BASE_KEY_NONE && !frame->base.has_index)
			keyless = true;
		else if (frame->base.key.tag == TXN_BASE_KEY_COLUMNS)
		{
			for (size_t i = 0; i < frame->base.key.u.columns.len; i++)
				if (STD_VEC_AT(&frame->base.key.u.columns, TxnBaseKeyColumn, i).compared)
				{
					frame->ok = true;
					return;
				}
		}
		else
		{
			frame->ok = true;
			return;
		}
		if (!txn_state_founded(frame->io, frame->held, frame->base.table, frame->base.has_index, frame->base.index,
							   frame->error))
			return;
		frame->base_live = false;
		txn_base_Base_drop(&frame->base);
	}
	frame->bytes = trees_KeyOrder_Default_default();
	frame->bytes_live = true;
	std_vec_new(&frame->warned, sizeof(StdString), _Alignof(StdString));
	frame->warned_live = true;
	frame->ok = txn_fold_in_order(frame->io, frame->repo, frame->reading, frame->held, &frame->bytes, keyless,
								  &frame->warned, frame->page, frame->upto, frame->result, &left, frame->error);
}

/* txn_fold_startup_fold_pad frees what startup_fold holds live: the warnings, the order, and the base. */
static void
txn_fold_startup_fold_pad(void *frame_)
{
	txn_fold_startup_fold_frame_t *frame = frame_;

	if (frame->warned_live)
		std_vec_drop_Checked(&frame->warned, sizeof(StdString), _Alignof(StdString), std_String_drop);
	if (frame->bytes_live)
		trees_KeyOrder_drop(&frame->bytes);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->warned_live = frame->bytes_live = frame->base_live = false;
}

/*
 * txn_fold_startup_fold folds a storage in the startup process, which reads
 * no catalog, at each point its replayed fold records reached past its
 * newest base, in order, while the oldest one's key orders the storage's rows
 * by their bytes: a keyless table's, or one whose key has no column compared
 * through its class. It answers whether it installed a base.
 */
bool
txn_fold_startup_fold(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const HostSharedHeld *held,
					  const TxnInsertPage *page, uint64 upto, bool *result, HostError *error)
{
	txn_fold_startup_fold_frame_t frame = {.io = io, .repo = repo, .reading = reading, .held = held, .page = page,
	.upto = upto, .result = result, .error = error};

	std_landing(&frame, txn_fold_startup_fold_body, txn_fold_startup_fold_pad);
	txn_fold_startup_fold_pad(&frame);
	return frame.ok;
}

/* txn_fold_caught_up_frame_t is caught_up's state across its pad: a record's body and what it decoded, each while live. */
typedef struct txn_fold_caught_up_frame_t
{
	HostIoIo   *io;
	const HostSharedHeld *held;
	const TxnInsertPage *page;
	uint64		upto;
	HostForkRecord record;
	bool		record_live;
	TxnRecordBaseAdvanced advanced;
	bool		advanced_live;
	TxnRecordTidsFreed freed;
	bool		freed_live;
	TxnFoldCaught *result;
	HostError  *error;
	bool		ok;
} txn_fold_caught_up_frame_t;

/* txn_fold_caught_up_record_drop frees a record's body and what it decoded. */
static void
txn_fold_caught_up_record_drop(txn_fold_caught_up_frame_t *frame)
{
	if (frame->freed_live)
		txn_record_TidsFreed_drop(&frame->freed);
	if (frame->advanced_live)
		txn_record_BaseAdvanced_drop(&frame->advanced);
	if (frame->record_live)
		std_vec_free_Checked(&frame->record.f2, TXN_FOLD_BYTES);
	frame->freed_live = frame->advanced_live = frame->record_live = false;
}

/* txn_fold_caught_up_body does each free's deferred take-outs past the slot's record, up to the next fold's record. */
static void
txn_fold_caught_up_body(void *frame_)
{
	txn_fold_caught_up_frame_t *frame = frame_;
	HostIoIoDoors doors = HOST_IO_IO_IO_DOORS(frame->io);
	bool		some;
	uint64		from;
	HostStorage storage;
	TxnStatePersistence persistence;
	uint32		kinds = (1u << host_WalKind_value(HOST_WAL_KIND_BASE_ADVANCED)) |
		(1u << host_WalKind_value(HOST_WAL_KIND_TIDS_FREED));
	bool		done;

	*frame->result = TXN_FOLD_CAUGHT_DONE;
	if (!txn_state_past(frame->io, frame->held, &some, &from, frame->error))
		return;
	if (!some || !txn_state_described(host_shared_Held_record(frame->held), &storage, &persistence))
	{
		frame->ok = true;
		return;
	}
	for (;;)
	{
		if (!host_next_fork_record(doors, from, kinds, &frame->record_live, &frame->record, frame->error))
			return;
		if (!frame->record_live)
			break;
		from = frame->record.f1;
		if (frame->record.f0 == host_WalKind_value(HOST_WAL_KIND_BASE_ADVANCED))
		{
			if (!txn_record_BaseAdvanced_decode(frame->record.f2.ptr, frame->record.f2.len, &frame->advanced,
												frame->error))
				return;
			frame->advanced_live = true;
			if (host_Storage_PartialEq_eq(&frame->advanced.storage, &storage))
			{
				if (!txn_state_read_past(frame->io, frame->held, frame->record.f1, true, &frame->advanced.base,
										 frame->record.f1, &done, frame->error))
					return;
				*frame->result = TXN_FOLD_CAUGHT_NEXT;
				frame->ok = true;
				return;
			}
		}
		else
		{
			if (!txn_record_TidsFreed_decode(frame->record.f2.ptr, frame->record.f2.len, frame->page->most,
											 frame->page->bytes, &frame->freed, frame->error))
				return;
			frame->freed_live = true;
			if (host_Storage_PartialEq_eq(&frame->freed.storage, &storage) &&
				!txn_vacuum_taken_out(frame->io, frame->held, &frame->freed, frame->record.f1, frame->error))
				return;
		}
		txn_fold_caught_up_record_drop(frame);
	}
	if (!txn_state_read_past(frame->io, frame->held, Max(from, frame->upto), false, NULL, 0, &done, frame->error))
		return;
	*frame->result = done ? TXN_FOLD_CAUGHT_DONE : TXN_FOLD_CAUGHT_WAITING;
	frame->ok = true;
}

/* txn_fold_caught_up_pad frees what caught_up holds live: a record and what it decoded. */
static void
txn_fold_caught_up_pad(void *frame_)
{
	txn_fold_caught_up_record_drop(frame_);
}

/*
 * txn_fold_caught_up reads, once the fold at a state's slot's point has
 * installed, its storage's records past the slot's record, in order: each
 * free's take-outs, which its redo deferred, are done as the primary's VACUUM
 * did them after that fold, up to the next fold's record, which the slot
 * takes for the next fold; with none left the read is done once no record
 * past its last was deferred, upto being one its caller redoes now.
 */
bool
txn_fold_caught_up(HostIoIo *io, const HostSharedHeld *held, const TxnInsertPage *page, uint64 upto,
				   TxnFoldCaught *result, HostError *error)
{
	txn_fold_caught_up_frame_t frame = {.io = io, .held = held, .page = page, .upto = upto, .result = result,
	.error = error};

	std_landing(&frame, txn_fold_caught_up_body, txn_fold_caught_up_pad);
	txn_fold_caught_up_pad(&frame);
	return frame.ok;
}

/* txn_fold_shown writes a root's address, or "empty" for none, as matched's detail shows it. */
static void
txn_fold_shown(bool has_root, const ChunksAddressAddress *root, StdString *result)
{
	StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{root, chunks_address_Address_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	if (!has_root)
	{
		std_string_from_Checked(result, "empty", 5);
		return;
	}
	std_fmt_format_Checked(result, &arguments);
}

/*
 * txn_fold_matched checks a standby's folded base against the one its
 * primary's record named at the same point, answering whether the fold
 * installs it. A byte-ordered tree whose root differs publishes the
 * mismatch, whose FATAL ends recovery, and the fold installs nothing; a rows
 * tree in a comparator's order whose root differs is reported and the
 * standby keeps its own.
 */
static bool
txn_fold_matched(HostIoIoDoors io, const HostSharedHeld *held, const TxnBaseBase *folded, TxnFoldReached *reached,
				 const TreesKeyOrder *order, bool *result, HostError *error)
{
	static const char *const names[] = {"rows", "tuple id", "free space", "pending tuple id"};
	const TxnBaseBase *theirs = reached->base;
	bool		ours_has[4] = {folded->has_root, folded->has_tuple_ids, folded->has_free_space, folded->has_pending};
	const ChunksAddressAddress *ours_at[4] = {&folded->root.address, &folded->tuple_ids.address,
	&folded->free_space.address, &folded->pending.address};
	bool		theirs_has[4] = {theirs->has_root, theirs->has_tuple_ids, theirs->has_free_space, theirs->has_pending};
	const ChunksAddressAddress *theirs_at[4] = {&theirs->root.address, &theirs->tuple_ids.address,
	&theirs->free_space.address, &theirs->pending.address};
	HostStorage storage;
	TxnStatePersistence persistence;

	*result = true;
	if (!txn_state_described(host_shared_Held_record(held), &storage, &persistence))
		return true;
	for (int at = 0; at < lengthof(names); at++)
	{
		StdString	ours;
		StdString	their;
		StdString	detail;
		StdStr		name = {names[at], strlen(names[at])};
		StdFmtPiece pieces[] = {STD_FMT_LIT("The "), STD_FMT_ARG(0), STD_FMT_LIT(" tree of storage "), STD_FMT_ARG(1),
			STD_FMT_LIT("/"), STD_FMT_ARG(2), STD_FMT_LIT("/"), STD_FMT_ARG(3), STD_FMT_LIT(" has root "),
			STD_FMT_ARG(4), STD_FMT_LIT(" on this standby and "), STD_FMT_ARG(5),
		STD_FMT_LIT(" on the primary.")};
		StdFmtArg	args[6];
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		if (ours_has[at] == theirs_has[at] &&
			(!ours_has[at] || chunks_address_Address_PartialEq_eq(ours_at[at], theirs_at[at])))
			continue;
		txn_fold_shown(ours_has[at], ours_at[at], &ours);
		txn_fold_shown(theirs_has[at], theirs_at[at], &their);
		args[0] = (StdFmtArg)
		{
			&name, std_str_Display_fmt
		};
		args[1] = (StdFmtArg)
		{
			&storage.tablespace, std_u32_Display_fmt
		};
		args[2] = (StdFmtArg)
		{
			&storage.database, std_u32_Display_fmt
		};
		args[3] = (StdFmtArg)
		{
			&storage.relation, std_u32_Display_fmt
		};
		args[4] = (StdFmtArg)
		{
			&ours, std_String_Display_fmt
		};
		args[5] = (StdFmtArg)
		{
			&their, std_String_Display_fmt
		};
		std_fmt_format_Checked(&detail, &arguments);
		std_String_drop(&their);
		std_String_drop(&ours);
		if (at == 0 && !trees_KeyOrder_is_bytes(order))
		{
			std_vec_push_Checked(reached->warned, &detail, sizeof(StdString), _Alignof(StdString));
			continue;
		}
		host_refuse_recovery(io, detail.ptr, detail.len);
		std_String_drop(&detail);
		*result = false;
		return true;
	}
	return true;
}

/*
 * txn_fold_kept_out lowers a fold point below a committed top-level writer's
 * commit end when the writer is at or past a horizon.
 */
bool
txn_fold_kept_out(HostIoIo *io, uint64 top, uint64 horizon, HostLsn *bound, HostError *error)
{
	TxnStatusCommit commit;
	HostXid		xid = {top};

	if (top < horizon)
		return true;
	if (!txn_status_committed(io, xid, &commit, error))
		return false;
	if (commit.tag == TXN_STATUS_COMMIT_AT)
		*bound = Min(*bound, STD_SUB(u64, commit.u.at, 1, 1910, 31));
	return true;
}

/*
 * txn_fold_lower lowers a fold point below a version's commit end when the
 * point would apply it and its top-level writer is at or past a horizon.
 */
static bool
txn_fold_lower(HostIoIo *io, TxnMemtableVersionVersion *version, uint64 horizon, HostLsn *bound, HostError *error)
{
	TxnFoldApplied applied;

	if (!txn_fold_applied(io, version, *bound, &applied, error))
		return false;
	if (applied.tag == TXN_FOLD_APPLIED_YES && applied.u.yes.has_top)
		return txn_fold_kept_out(io, applied.u.yes.top, horizon, bound, error);
	return true;
}

/*
 * txn_fold_bounded_captures_t is bounded's walks' captures: the state, its
 * newest base's fold point, the horizon, and the bound.
 */
typedef struct txn_fold_bounded_captures_t
{
	HostSharedRecord record;
	HostLsn		folded;
	const TxnMemtableTableTreeOps *tree;
	uint64		horizon;
	HostLsn    *bound;
} txn_fold_bounded_captures_t;

/*
 * txn_fold_bounded_walk_frame_t is a bounded walk's state across its pad: its
 * hold and a key's versions, each while live.
 */
typedef struct txn_fold_bounded_walk_frame_t
{
	txn_fold_bounded_captures_t *walk;
	HostIoIo   *io;
	const uint8 *key;
	size_t		key_len;
	bool		map;
	HostDeltaHold hold;
	bool		hold_live;
	StdVec		versions;
	bool		versions_live;
	HostError  *error;
	bool		ok;
} txn_fold_bounded_walk_frame_t;

/* txn_fold_bounded_walk_body lowers the bound over a key's versions, a row's or a map's. */
static void
txn_fold_bounded_walk_body(void *frame_)
{
	txn_fold_bounded_walk_frame_t *frame = frame_;
	txn_fold_bounded_captures_t *walk = frame->walk;

	if (frame->map && frame->key_len != 4)
	{
		*frame->error = txn_fold_fork("XX001", "a map's key isn't four bytes");
		return;
	}
	if (!host_delta_hold(frame->io, &frame->hold, frame->error))
		return;
	frame->hold_live = true;
	if (frame->map ? !txn_memtable_map_versions(walk->tree, frame->io, &frame->hold, walk->record,
												std_u32_from_be_bytes(frame->key), &frame->versions, frame->error)
		: !txn_visible_versions(frame->io, &frame->hold, walk->record, frame->key, frame->key_len, walk->folded,
								&frame->versions, frame->error))
		return;
	frame->versions_live = true;
	for (size_t i = 0; i < frame->versions.len; i++)
		if (!txn_fold_lower(frame->io, &STD_VEC_AT(&frame->versions, TxnMemtableVersionVersion, i), walk->horizon,
							walk->bound, frame->error))
			return;
	frame->ok = true;
}

/* txn_fold_bounded_walk_pad frees what a bounded walk holds live: the versions and its hold. */
static void
txn_fold_bounded_walk_pad(void *frame_)
{
	txn_fold_bounded_walk_frame_t *frame = frame_;

	if (frame->versions_live)
		txn_memtable_version_versions_drop(&frame->versions, 0);
	if (frame->hold_live)
		host_delta_Hold_drop(&frame->hold);
	frame->versions_live = frame->hold_live = false;
}

/* txn_fold_bounded_closure_1 is bounded's walk's closure, of one key of the rows. */
static bool
txn_fold_bounded_closure_1(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
						   size_t values_len, HostError *error)
{
	txn_fold_bounded_walk_frame_t frame = {.walk = captures, .io = io, .key = key, .key_len = key_len,
	.error = error};

	std_landing(&frame, txn_fold_bounded_walk_body, txn_fold_bounded_walk_pad);
	txn_fold_bounded_walk_pad(&frame);
	return frame.ok;
}

/* txn_fold_bounded_map_closure_0 is bounded_map's walk's closure, of one key of a map. */
static bool
txn_fold_bounded_map_closure_0(void *captures, HostIoIo *io, const uint8 *key, size_t key_len, const StdVec *values,
							   size_t values_len, HostError *error)
{
	txn_fold_bounded_walk_frame_t frame = {.walk = captures, .io = io, .key = key, .key_len = key_len, .map = true,
	.error = error};

	std_landing(&frame, txn_fold_bounded_walk_body, txn_fold_bounded_walk_pad);
	txn_fold_bounded_walk_pad(&frame);
	return frame.ok;
}

/* txn_fold_bounded_map lowers a fold point as bounded does over the versions of a storage's map. */
static bool
txn_fold_bounded_map(HostIoIo *io, HostSharedRecord record, const TxnMemtableTableTreeOps *tree, uint64 horizon,
					 HostLsn *bound, HostError *error)
{
	txn_fold_bounded_captures_t walk = {.record = record, .tree = tree, .horizon = horizon, .bound = bound};

	return txn_memtable_table_walked(tree, io, record, txn_fold_bounded_map_closure_0, &walk, NULL, error);
}

/*
 * txn_fold_bounded lowers a fold point so the fold applies no top-level
 * transaction at or past a horizon: below the commit end, at or below the
 * point, of each committed writer at or past the horizon among the storage's
 * row versions its newest base didn't apply and its free space and value
 * maps' versions, every version a fold applies.
 */
static bool
txn_fold_bounded(HostIoIo *io, const HostSharedHeld *held, HostLsn point, uint64 horizon, HostLsn *result,
				 HostError *error)
{
	HostSharedRecord record = host_shared_Held_record(held);
	txn_fold_bounded_captures_t walk = {.record = record, .horizon = horizon, .bound = result};
	bool		some;
	TxnBaseBase newest;

	*result = point;
	if (!txn_state_newest(io, held, &some, &newest, error))
		return false;
	if (some)
	{
		walk.folded = newest.folded;
		txn_base_Base_drop(&newest);
	}
	return txn_memtable_table_walked(&TXN_MEMTABLE_TABLE_ROWS_TREE_OPS, io, record, txn_fold_bounded_closure_1, &walk,
									 NULL, error) &&
		txn_fold_bounded_map(io, record, &TXN_MEMTABLE_MAP_FREE_TREE_OPS, horizon, result, error) &&
		txn_fold_bounded_map(io, record, &TXN_MEMTABLE_MAP_VALUES_TREE_OPS, horizon, result, error);
}

/*
 * txn_fold_rechained_base_frame_t is rechained_base's state across its pad:
 * the base it takes, its writer, the chain tree's edits, and the run's
 * install, each while live.
 */
typedef struct txn_fold_rechained_base_frame_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const HostSharedHeld *held;
	uint64		generation;
	TxnBaseBase base;
	bool		base_live;
	const TxnBasePending *entries;
	size_t		entries_len;
	TreesStoreWriter plain;
	TreesStoreBrandedWriter writer;
	bool		writer_live;
	StdVec		edits;
	bool		edits_live;
	StdVec		named;
	bool		named_live;
	TreesShape	shape;
	bool		shape_live;
	size_t		tree;
	TxnFoldInstalled *result;
	HostError  *error;
	bool		ok;
} txn_fold_rechained_base_frame_t;

/* txn_fold_rechained_base_closure_1_frame_t is rechained_base's run's state across its pad: the root it published. */
typedef struct txn_fold_rechained_base_closure_1_frame_t
{
	txn_fold_rechained_base_frame_t *rechained;
	HostIoRegistering *registering;
	ChunksPacksRootsRootSlots *slots;
	TxnFoldInstalled *result;
	ChunksErrorError *error;
	StdVec		published;
	bool		published_live;
	bool		ok;
} txn_fold_rechained_base_closure_1_frame_t;

/* txn_fold_rechained_base_closure_1_body holds the chain's root in one slot, publishes it, and replaces the base. */
static void
txn_fold_rechained_base_closure_1_body(void *frame_)
{
	txn_fold_rechained_base_closure_1_frame_t *frame = frame_;
	txn_fold_rechained_base_frame_t *rechained = frame->rechained;
	HostIoTakesShort io = {&HOST_IO_REGISTERING_TAKES_SHORT_OPS, frame->registering};
	ChunksPacksRootsRootSlot *slot;
	bool		replaced;
	HostError	host;

	chunks_packs_roots_RootSlots_one(frame->slots, &slot);
	if (!trees_store_BrandedWriter_hold(&rechained->writer, io, slot, rechained->tree, frame->error))
		return;
	rechained->writer_live = false;
	if (!trees_store_BrandedWriter_publish(&rechained->writer, io, &frame->published, &frame->result->packs,
										   frame->error))
		return;
	frame->published_live = true;
	frame->result->next = rechained->base;
	rechained->base_live = false;
	frame->result->some = true;
	frame->result->next.has_chain = true;
	frame->result->next.chain = trees_published_Published_base(&STD_VEC_AT(&frame->published,
																		   TreesPublishedPublished, 0));
	if (!txn_state_rechained(io, rechained->held, rechained->generation, &frame->result->next, &replaced, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	if (!replaced)
		txn_fold_Installed_drop(frame->result);
	frame->ok = true;
}

/* txn_fold_rechained_base_closure_1_pad drops what holds a root's Pin: the writer and the published name. */
static void
txn_fold_rechained_base_closure_1_pad(void *frame_)
{
	txn_fold_rechained_base_closure_1_frame_t *frame = frame_;

	if (frame->published_live)
		txn_fold_published_drop(&frame->published);
	if (frame->rechained->writer_live)
		trees_store_BrandedWriter_drop(&frame->rechained->writer);
	frame->published_live = frame->rechained->writer_live = false;
}

/* txn_fold_rechained_base_closure_1_landing is the run's body, which drops its Pins before it returns. */
static void
txn_fold_rechained_base_closure_1_landing(void *frame_)
{
	txn_fold_rechained_base_closure_1_frame_t *frame = frame_;

	txn_fold_rechained_base_closure_1_body(frame);
	if (!frame->ok)
		txn_fold_Installed_drop(frame->result);
	txn_fold_rechained_base_closure_1_pad(frame);
}

/* txn_fold_rechained_base_closure_1_unwound drops what the run holds when it unwinds: its Pins and its install. */
static void
txn_fold_rechained_base_closure_1_unwound(void *frame_)
{
	txn_fold_rechained_base_closure_1_frame_t *frame = frame_;

	txn_fold_Installed_drop(frame->result);
	txn_fold_rechained_base_closure_1_pad(frame);
}

/* txn_fold_rechained_base_closure_1 is rechained_base's run under the root slots. */
static bool
txn_fold_rechained_base_closure_1(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
								  void *result, ChunksErrorError *error)
{
	txn_fold_rechained_base_closure_1_frame_t frame = {.rechained = captures, .registering = registering,
	.slots = slots, .result = result, .error = error};

	frame.result->some = false;
	std_landing(&frame, txn_fold_rechained_base_closure_1_landing, txn_fold_rechained_base_closure_1_unwound);
	return frame.ok;
}

/*
 * txn_fold_rechained_base_body writes the chain tree of the entries in a
 * writer and installs the base naming it under one slot.
 */
static void
txn_fold_rechained_base_body(void *frame_)
{
	txn_fold_rechained_base_frame_t *frame = frame_;
	TreesStoreReader reader = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
	ChunksErrorError failed;
	TreesStoreTree none = {0};

	if (!trees_store_Writer_new(frame->io, reader, txn_state_space(host_shared_Held_record(frame->held)),
								&frame->plain, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	trees_store_Writer_brand(&frame->plain, &frame->writer);
	frame->writer_live = true;
	std_vec_with_capacity_Checked(&frame->edits, frame->entries_len, TXN_FOLD_EDIT);
	frame->edits_live = true;
	for (size_t i = 0; i < frame->entries_len; i++)
	{
		TreesMutateEdit edit = {.tag = TREES_MUTATE_EDIT_PUT};
		uint8		key[4];
		StdVec		value;

		txn_base_chain_key((uint32) (i + 1), key);
		txn_fold_bytes(key, sizeof(key), &frame->named);
		frame->named_live = true;
		txn_base_chain_value(&frame->entries[i], &value);
		frame->named_live = false;
		trees_build_Entry_new_VecU8VecU8(&frame->named, &value, &edit.u.put);
		std_vec_push_Checked(&frame->edits, &edit, TXN_FOLD_EDIT);
	}
	frame->shape = trees_Shape_Default_default();
	frame->shape_live = true;
	if (!txn_fold_mutated(&frame->writer, frame->io, false, none, &frame->edits, &frame->edits_live, &frame->shape,
						  &frame->tree, frame->error))
		return;
	if (!chunks_packs_roots_RootSlots_register(frame->io, txn_fold_rechained_base_closure_1, frame, NULL,
											   frame->result, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	frame->ok = true;
}

/* txn_fold_rechained_base_pad frees what rechained_base holds live. */
static void
txn_fold_rechained_base_pad(void *frame_)
{
	txn_fold_rechained_base_frame_t *frame = frame_;

	if (frame->shape_live)
		trees_Shape_drop(&frame->shape);
	if (frame->named_live)
		std_vec_free_Checked(&frame->named, TXN_FOLD_BYTES);
	if (frame->edits_live)
		std_vec_drop_Checked(&frame->edits, TXN_FOLD_EDIT, trees_mutate_Edit_drop);
	if (frame->writer_live)
		trees_store_BrandedWriter_drop(&frame->writer);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->shape_live = frame->named_live = frame->edits_live = frame->writer_live = frame->base_live = false;
}

/*
 * txn_fold_rechained_base writes a chain tree of entries, in order, and
 * replaces a state's newest base, which it takes, with one naming it when the
 * base generation is still the one read with the base, answering the new
 * base and the packs it wrote, or nothing when another install moved the
 * generation.
 */
static bool
txn_fold_rechained_base(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
						const HostSharedHeld *held, uint64 generation, TxnBaseBase *base,
						const TxnBasePending *entries, size_t entries_len, TxnFoldInstalled *result, HostError *error)
{
	txn_fold_rechained_base_frame_t frame = {.io = io, .repo = repo, .reading = reading, .held = held,
		.generation = generation, .base = *base, .base_live = true, .entries = entries, .entries_len = entries_len,
	.result = result, .error = error};

	result->some = false;
	std_landing(&frame, txn_fold_rechained_base_body, txn_fold_rechained_base_pad);
	txn_fold_rechained_base_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_chain_frame_t is the state across its pad of the chain functions'
 * loops: a pass's base, its chain, the entries it writes, the counts it gives
 * back, the live entries, and the install, each while live.
 */
typedef struct txn_fold_chain_frame_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const HostSharedHeld *held;
	TxnBasePending pending;
	bool		pending_live;
	const TxnBasePending *recorded_entries;
	size_t		recorded_entries_len;
	TxnBaseBase recorded;
	bool		recorded_live;
	TxnBaseBase base;
	bool		base_live;
	StdVec		current;
	bool		current_live;
	StdVec		entries;
	bool		entries_live;
	StdVec		given;
	bool		given_live;
	StdVec		live;
	bool		live_live;
	TxnFoldInstalled installed;
	StdVec	   *result;
	HostError  *error;
	bool		ok;
} txn_fold_chain_frame_t;

/* txn_fold_chain_pass_drop frees what a pass of a chain loop holds: its install, counts, entries, chain, and base. */
static void
txn_fold_chain_pass_drop(txn_fold_chain_frame_t *frame)
{
	txn_fold_Installed_drop(&frame->installed);
	if (frame->given_live)
		std_vec_free_Checked(&frame->given, TXN_FOLD_U64);
	if (frame->entries_live)
		std_vec_drop_Checked(&frame->entries, TXN_FOLD_CHAIN, txn_fold_Pending_drop);
	if (frame->current_live)
		std_vec_drop_Checked(&frame->current, TXN_FOLD_CHAIN, txn_fold_Pending_drop);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->given_live = frame->entries_live = frame->current_live = frame->base_live = false;
}

/* txn_fold_chain_pad frees what a chain loop holds live. */
static void
txn_fold_chain_pad(void *frame_)
{
	txn_fold_chain_frame_t *frame = frame_;

	txn_fold_chain_pass_drop(frame);
	if (frame->live_live)
		std_vec_free_Checked(&frame->live, TXN_FOLD_U64);
	if (frame->recorded_live)
		txn_base_Base_drop(&frame->recorded);
	if (frame->pending_live)
		txn_base_Pending_drop(&frame->pending);
	frame->live_live = frame->recorded_live = frame->pending_live = false;
}

/* txn_fold_chain_read reads a pass's base, its record's or the recorded one, and its chain's entries. */
static bool
txn_fold_chain_read(txn_fold_chain_frame_t *frame, const char *missing, bool *some, uint64 *generation)
{
	TreesStoreReader reader = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);

	*generation = txn_state_generation(host_shared_Held_record(frame->held));
	if (!txn_state_base_record(frame->io, frame->held, some, &frame->base, frame->error))
		return false;
	if (!*some && frame->recorded_live)
	{
		frame->base = frame->recorded;
		frame->recorded_live = false;
		*some = true;
	}
	if (!*some)
	{
		if (missing == NULL)
			return true;
		*frame->error = txn_fold_fork("XX000", missing);
		return false;
	}
	frame->base_live = true;
	if (missing == NULL && !frame->base.has_chain)
	{
		*some = false;
		return true;
	}
	if (!txn_base_chain_of(frame->io, TREES_STORE_READER_NODE_STORE(&reader), frame->base.has_chain,
						   frame->base.chain, &frame->current, frame->error))
		return false;
	frame->current_live = true;
	return true;
}

/* txn_fold_chain_given gives back each count of the xids a pass gave up. */
static bool
txn_fold_chain_given(txn_fold_chain_frame_t *frame)
{
	for (size_t i = 0; i < frame->given.len; i++)
	{
		HostXid		xid = {STD_VEC_AT(&frame->given, uint64, i)};
		bool		has_added;
		uint64		added;

		if (!txn_status_add(frame->io, xid, PG_UINT64_MAX, &has_added, &added, frame->error))
			return false;
	}
	return true;
}

/*
 * txn_fold_chained_body rewrites the chain with the aborted DDLs' entries
 * dropped and the new one appended, then logs it.
 */
static void
txn_fold_chained_body(void *frame_)
{
	txn_fold_chain_frame_t *frame = frame_;

	for (;;)
	{
		bool		some;
		uint64		generation;
		TxnBasePending pending;
		HostXid		xid = {frame->pending.xid};
		bool		has_added;
		uint64		added;
		HostStorage storage;
		TxnStatePersistence persistence;

		txn_fold_chain_pass_drop(frame);
		if (!txn_fold_chain_read(frame, "a DDL's transform found its storage with no base", &some, &generation))
			return;
		std_vec_with_capacity_Checked(&frame->entries, frame->current.len + 1, TXN_FOLD_CHAIN);
		frame->entries_live = true;
		std_vec_new(&frame->given, TXN_FOLD_U64);
		frame->given_live = true;
		for (; frame->current.len > 0;)
		{
			TxnBasePending *entry = &STD_VEC_AT(&frame->current, TxnBasePending, 0);
			HostXid		sub = {entry->sub};
			HostStatus	status;
			bool		gone = false;

			if (!entry->has_end)
			{
				if (!host_status(HOST_IO_IO_IO_DOORS(frame->io), sub, &status, frame->error))
					return;
				gone = status == HOST_STATUS_ABORTED;
			}
			if (gone)
				std_vec_push_Checked(&frame->given, &entry->xid, TXN_FOLD_U64);
			std_vec_remove(&frame->current, 0, &pending, TXN_FOLD_CHAIN, RUST_FILE, 1352, 9);
			if (gone)
				txn_base_Pending_drop(&pending);
			else
				std_vec_push_Checked(&frame->entries, &pending, TXN_FOLD_CHAIN);
		}
		txn_base_Pending_Clone_clone(&frame->pending, &pending);
		std_vec_push_Checked(&frame->entries, &pending, TXN_FOLD_CHAIN);
		frame->base_live = false;
		if (!txn_fold_rechained_base(frame->io, frame->repo, frame->reading, frame->held, generation, &frame->base,
									 frame->entries.ptr, frame->entries.len, &frame->installed, frame->error))
			return;
		if (!frame->installed.some)
			continue;
		if (!txn_fold_chain_given(frame) ||
			!txn_status_add(frame->io, xid, 1, &has_added, &added, frame->error))
			return;
		if (txn_state_described(host_shared_Held_record(frame->held), &storage, &persistence) &&
			persistence == TXN_STATE_PERSISTENCE_PERMANENT)
		{
			TxnRecordBaseAdvanced advanced = {.storage = storage, .folded = frame->installed.next.folded,
				.max_folded = frame->installed.next.max_folded, .base = frame->installed.next,
			.packs = frame->installed.packs, .has_chain = true, .chain = frame->entries};
			StdVec		body;
			HostLsn		lsn;
			bool		ok;

			frame->installed.some = false;
			frame->entries_live = false;
			txn_record_BaseAdvanced_encode(&advanced, &body);
			ok = host_wal_insert(HOST_IO_IO_IO_DOORS(frame->io), HOST_WAL_KIND_BASE_ADVANCED, body.ptr, body.len,
								 &lsn, frame->error);
			std_vec_free_Checked(&body, TXN_FOLD_BYTES);
			txn_record_BaseAdvanced_drop(&advanced);
			if (!ok)
				return;
		}
		frame->ok = true;
		return;
	}
}

/*
 * txn_fold_chained records a DDL's transform in its storage's chain, taking
 * it, in the DDL's statement: it writes the newest base's chain with the
 * entries whose DDLs aborted dropped and the new one appended, and replaces
 * the newest base with it, rereading and rewriting when a fold installed
 * between its read and its replace. The new entry counts at its DDL's
 * top-level entry, each dropped one gives its count back, and a permanent
 * storage's record names the chain's entries.
 */
bool
txn_fold_chained(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const HostSharedHeld *held,
				 TxnBasePending *pending, HostError *error)
{
	txn_fold_chain_frame_t frame = {.io = io, .repo = repo, .reading = reading, .held = held, .pending = *pending,
	.pending_live = true, .error = error};

	std_landing(&frame, txn_fold_chained_body, txn_fold_chain_pad);
	txn_fold_chain_pad(&frame);
	return frame.ok;
}

/* txn_fold_chain_holds reports whether a chain holds an entry the same as one. */
static const TxnBasePending *
txn_fold_chain_holds(const StdVec *chain, const TxnBasePending *entry)
{
	for (size_t i = 0; i < chain->len; i++)
		if (txn_base_Pending_same(&STD_VEC_AT(chain, TxnBasePending, i), entry))
			return &STD_VEC_AT(chain, TxnBasePending, i);
	return NULL;
}

/*
 * txn_fold_redo_chained_body writes the record's chain for this server's
 * newest base, each entry keeping the end this server's save wrote, then
 * gives back the counts of the entries the record lacks and counts the new
 * ones.
 */
static void
txn_fold_redo_chained_body(void *frame_)
{
	txn_fold_chain_frame_t *frame = frame_;

	frame->recorded.has_chain = false;
	for (;;)
	{
		bool		some;
		uint64		generation;

		txn_fold_chain_pass_drop(frame);
		if (!txn_fold_chain_read(frame, "a replayed DDL's transform found its storage with no base", &some,
								 &generation))
			return;
		std_vec_with_capacity_Checked(&frame->entries, frame->recorded_entries_len, TXN_FOLD_CHAIN);
		frame->entries_live = true;
		for (size_t i = 0; i < frame->recorded_entries_len; i++)
		{
			const TxnBasePending *entry = &frame->recorded_entries[i];
			const TxnBasePending *known = txn_fold_chain_holds(&frame->current, entry);
			TxnBasePending written;

			txn_base_Pending_Clone_clone(entry, &written);
			if (!written.has_end && known != NULL && known->has_end)
			{
				written.has_end = true;
				written.end = known->end;
			}
			std_vec_push_Checked(&frame->entries, &written, TXN_FOLD_CHAIN);
		}
		frame->base_live = false;
		if (!txn_fold_rechained_base(frame->io, frame->repo, frame->reading, frame->held, generation, &frame->base,
									 frame->entries.ptr, frame->entries.len, &frame->installed, frame->error))
			return;
		if (!frame->installed.some)
			continue;
		for (size_t i = 0; i < frame->current.len; i++)
		{
			const TxnBasePending *entry = &STD_VEC_AT(&frame->current, TxnBasePending, i);
			HostXid		xid = {entry->xid};
			bool		has_added;
			uint64		added;

			if (entry->has_end || txn_fold_chain_holds(&frame->entries, entry) != NULL)
				continue;
			if (!txn_status_add(frame->io, xid, PG_UINT64_MAX, &has_added, &added, frame->error))
				return;
		}
		for (size_t i = 0; i < frame->entries.len; i++)
		{
			const TxnBasePending *entry = &STD_VEC_AT(&frame->entries, TxnBasePending, i);
			TxnMemtableVersionWriter writer = {.xid = entry->sub, .has_top = entry->sub != entry->xid,
			.top = entry->xid, .command = entry->command};
			HostXid		xid = {entry->xid};
			bool		has_roomed;
			TxnStatusRoomed roomed;
			bool		has_added;
			uint64		added;

			if (entry->has_end || txn_fold_chain_holds(&frame->current, entry) != NULL)
				continue;
			if (!txn_status_redo_roomed(frame->io, &writer, &has_roomed, &roomed, frame->error))
				return;
			if (has_roomed && !txn_status_add(frame->io, xid, 1, &has_added, &added, frame->error))
				return;
		}
		frame->ok = true;
		return;
	}
}

/*
 * txn_fold_redo_chained replays a DDL's chain record: it writes the chain of
 * the record's entries for this server's newest base and replaces that base
 * with it, whatever its other trees, an entry keeping the commit's end this
 * server's save wrote; each entry without an end the replaced chain lacked
 * counts at its top-level entry, and each without one it held that the
 * record lacks gives its count back. A storage this server holds no base of
 * yet takes the record's base, which it takes, with no chain.
 */
bool
txn_fold_redo_chained(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
					  const HostSharedHeld *held, const TxnBasePending *entries, size_t entries_len,
					  TxnBaseBase *recorded, HostError *error)
{
	txn_fold_chain_frame_t frame = {.io = io, .repo = repo, .reading = reading, .held = held,
		.recorded_entries = entries, .recorded_entries_len = entries_len, .recorded = *recorded,
	.recorded_live = true, .error = error};

	std_landing(&frame, txn_fold_redo_chained_body, txn_fold_chain_pad);
	txn_fold_chain_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_settled_body writes the commit's end of each committed DDL's
 * entry and drops each aborted one's, giving its count back, when an entry
 * changed, and answers the live writers' xids.
 */
static void
txn_fold_settled_body(void *frame_)
{
	txn_fold_chain_frame_t *frame = frame_;

	for (;;)
	{
		bool		some;
		uint64		generation;

		txn_fold_chain_pass_drop(frame);
		if (frame->live_live)
			std_vec_free_Checked(&frame->live, TXN_FOLD_U64);
		std_vec_new(&frame->live, TXN_FOLD_U64);
		frame->live_live = true;
		if (!txn_fold_chain_read(frame, NULL, &some, &generation))
			return;
		if (!some)
			break;
		std_vec_with_capacity_Checked(&frame->entries, frame->current.len, TXN_FOLD_CHAIN);
		frame->entries_live = true;
		std_vec_new(&frame->given, TXN_FOLD_U64);
		frame->given_live = true;
		for (; frame->current.len > 0;)
		{
			TxnBasePending *entry = &STD_VEC_AT(&frame->current, TxnBasePending, 0);
			HostXid		sub = {entry->sub};
			HostXid		xid = {entry->xid};
			HostStatus	status;
			TxnStatusCommit commit;
			TxnBasePending kept;
			bool		gone = false;

			if (!entry->has_end)
			{
				if (!host_status(HOST_IO_IO_IO_DOORS(frame->io), sub, &status, frame->error))
					return;
				if (status == HOST_STATUS_ABORTED)
				{
					std_vec_push_Checked(&frame->given, &entry->xid, TXN_FOLD_U64);
					gone = true;
				}
				else if (status == HOST_STATUS_IN_PROGRESS)
					std_vec_push_Checked(&frame->live, &entry->xid, TXN_FOLD_U64);
				else
				{
					if (!txn_status_committed(frame->io, xid, &commit, frame->error))
						return;
					if (commit.tag == TXN_STATUS_COMMIT_MISSING)
					{
						*frame->error = txn_fold_forked("XX001", "committed transaction ", entry->xid,
														" has no commit status entry");
						return;
					}
					if (commit.tag == TXN_STATUS_COMMIT_AT)
					{
						std_vec_push_Checked(&frame->given, &entry->xid, TXN_FOLD_U64);
						entry->has_end = true;
						entry->end = commit.u.at;
					}
					else
						std_vec_push_Checked(&frame->live, &entry->xid, TXN_FOLD_U64);
				}
			}
			std_vec_remove(&frame->current, 0, &kept, TXN_FOLD_CHAIN, RUST_FILE, 1436, 9);
			if (gone)
				txn_base_Pending_drop(&kept);
			else
				std_vec_push_Checked(&frame->entries, &kept, TXN_FOLD_CHAIN);
		}
		if (frame->given.len == 0)
			break;
		frame->base_live = false;
		if (!txn_fold_rechained_base(frame->io, frame->repo, frame->reading, frame->held, generation, &frame->base,
									 frame->entries.ptr, frame->entries.len, &frame->installed, frame->error))
			return;
		if (!frame->installed.some)
			continue;
		if (!txn_fold_chain_given(frame))
			return;
		break;
	}
	*frame->result = frame->live;
	frame->live_live = false;
	frame->ok = true;
}

/*
 * txn_fold_settled writes, at a save, the commit's end of each entry of a
 * storage's chain whose DDL committed and drops each whose DDL aborted,
 * giving each one's count back at its top-level entry, so the saved chain
 * holds only committed entries with their ends and entries of live writers,
 * which it answers by their top-level xids. It writes the chain only when an
 * entry changed, rereading on a moved generation, and logs nothing.
 */
bool
txn_fold_settled(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const HostSharedHeld *held,
				 StdVec *result, HostError *error)
{
	txn_fold_chain_frame_t frame = {.io = io, .repo = repo, .reading = reading, .held = held, .result = result,
	.error = error};

	std_landing(&frame, txn_fold_settled_body, txn_fold_chain_pad);
	txn_fold_chain_pad(&frame);
	return frame.ok;
}

/* txn_fold_Copies_drop frees a move's memo of the chunks it copied. */
void
txn_fold_Copies_drop(TxnFoldCopies *self)
{
	std_vec_free_Checked(&self->copies, TXN_FOLD_COPY);
}

/*
 * txn_fold_whole_frame_t is whole's state across its pad: the writer it
 * takes, its roots' places, the nodes it logged, and the install, each while
 * live.
 */
typedef struct txn_fold_whole_frame_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const HostSharedHeld *held;
	TreesStoreBrandedWriter *writer;
	bool		writer_live;
	const bool *has_roots;
	const size_t *roots;
	TxnBaseBase base;
	bool		base_live;
	HostLsn		point;
	uint64		generation;
	uint64		horizon;
	bool		has_nodes;
	uint64		nodes;
	StdVec		logged;
	bool		logged_live;
	size_t		logged_at;
	TxnFoldInstalled installed;
	bool		grows;
	bool	   *result;
	HostError  *error;
	bool		ok;
} txn_fold_whole_frame_t;

/* txn_fold_whole_closure_0_frame_t is whole's run's state across its pad: the roots it published. */
typedef struct txn_fold_whole_closure_0_frame_t
{
	txn_fold_whole_frame_t *whole;
	HostIoRegistering *registering;
	ChunksPacksRootsRootSlots *slots;
	TxnFoldInstalled *result;
	ChunksErrorError *error;
	StdVec		published;
	bool		published_live;
	bool		ok;
} txn_fold_whole_closure_0_frame_t;

/*
 * txn_fold_whole_closure_0_body holds the writer's four roots in the fold's
 * slots, publishes them, and installs the base that names them as the
 * state's newest when the generation is still the one read.
 */
static void
txn_fold_whole_closure_0_body(void *frame_)
{
	txn_fold_whole_closure_0_frame_t *frame = frame_;
	txn_fold_whole_frame_t *whole = frame->whole;
	HostIoTakesShort io = {&HOST_IO_REGISTERING_TAKES_SHORT_OPS, frame->registering};
	ChunksPacksRootsFold slot;
	TxnBaseBase *next = &frame->result->next;
	bool		held;
	bool		installed;
	size_t		at = 0;
	HostError	host;
	bool	   *has[4];
	TreesStoreTree *named[4];

	chunks_packs_roots_RootSlots_fold(frame->slots, &slot);
	held = txn_fold_held(whole->writer, io, slot.s0, whole->has_roots[0], whole->roots[0], true, frame->error);
	held = txn_fold_held(whole->writer, io, slot.s1, whole->has_roots[1], whole->roots[1], held, frame->error);
	held = txn_fold_held(whole->writer, io, slot.s2, whole->has_roots[2], whole->roots[2], held, frame->error);
	held = txn_fold_held(whole->writer, io, slot.s3, whole->has_roots[3], whole->roots[3], held, frame->error);
	if (!held)
		return;
	whole->writer_live = false;
	if (!trees_store_BrandedWriter_publish(whole->writer, io, &frame->published, &frame->result->packs,
										   frame->error))
		return;
	frame->published_live = true;
	*next = whole->base;
	whole->base_live = false;
	frame->result->some = true;
	has[0] = &next->has_root;
	has[1] = &next->has_tuple_ids;
	has[2] = &next->has_free_space;
	has[3] = &next->has_pending;
	named[0] = &next->root;
	named[1] = &next->tuple_ids;
	named[2] = &next->free_space;
	named[3] = &next->pending;
	for (int i = 0; i < 4; i++)
	{
		*has[i] = whole->has_roots[i];
		if (whole->has_roots[i])
			*named[i] = trees_published_Published_base(&STD_VEC_AT(&frame->published, TreesPublishedPublished, at++));
	}
	next->folded = whole->point;
	if (!txn_state_installed(io, whole->held, whole->generation, next, &installed, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	if (installed)
		txn_state_grown(host_shared_Held_record(whole->held), next);
	else
		txn_fold_Installed_drop(frame->result);
	frame->ok = true;
}

/* txn_fold_whole_closure_0_pad drops what holds a root's Pin: the writer and the published names. */
static void
txn_fold_whole_closure_0_pad(void *frame_)
{
	txn_fold_whole_closure_0_frame_t *frame = frame_;

	if (frame->published_live)
		txn_fold_published_drop(&frame->published);
	if (frame->whole->writer_live)
		trees_store_BrandedWriter_drop(frame->whole->writer);
	frame->published_live = frame->whole->writer_live = false;
}

/* txn_fold_whole_closure_0_landing is the run's body, which drops its Pins before it returns. */
static void
txn_fold_whole_closure_0_landing(void *frame_)
{
	txn_fold_whole_closure_0_frame_t *frame = frame_;

	txn_fold_whole_closure_0_body(frame);
	if (!frame->ok)
		txn_fold_Installed_drop(frame->result);
	txn_fold_whole_closure_0_pad(frame);
}

/* txn_fold_whole_closure_0_unwound drops what the run holds when it unwinds: its Pins and its install. */
static void
txn_fold_whole_closure_0_unwound(void *frame_)
{
	txn_fold_whole_closure_0_frame_t *frame = frame_;

	txn_fold_Installed_drop(frame->result);
	txn_fold_whole_closure_0_pad(frame);
}

/* txn_fold_whole_closure_0 is whole's run under the root slots. */
static bool
txn_fold_whole_closure_0(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
						 void *result, ChunksErrorError *error)
{
	txn_fold_whole_closure_0_frame_t frame = {.whole = captures, .registering = registering, .slots = slots,
	.result = result, .error = error};

	frame.result->some = false;
	std_landing(&frame, txn_fold_whole_closure_0_landing, txn_fold_whole_closure_0_unwound);
	return frame.ok;
}

/* txn_fold_Logged_drop frees a node a logging writer kept. */
static void
txn_fold_Logged_drop(void *logged_)
{
	trees_TupleAddressVecU8VecU8_t *logged = logged_;

	std_vec_free_Checked(&logged->f1, TXN_FOLD_BYTES);
	std_vec_free_Checked(&logged->f2, TXN_FOLD_BYTES);
}

/*
 * txn_fold_whole_logged logs a permanent storage's built base: its packs and
 * the WAL flushed, each node the writer kept as a Chunk record in its own
 * checkpoint window, children before parents, then the base's record.
 */
static bool
txn_fold_whole_logged(txn_fold_whole_frame_t *frame)
{
	HostIoIoDoors doors = HOST_IO_IO_IO_DOORS(frame->io);
	HostStorage storage;
	TxnStatePersistence persistence;
	ChunksErrorError failed;
	TxnRecordBaseAdvanced advanced;
	StdVec		body;
	HostLsn		lsn;
	bool		ok;

	if (!txn_state_described(host_shared_Held_record(frame->held), &storage, &persistence) ||
		persistence != TXN_STATE_PERSISTENCE_PERMANENT)
		return true;
	if (!chunks_packs_backend_Packs_flush(&frame->repo->packs, frame->io, frame->reading, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return false;
	}
	if (!host_wal_flush(doors, frame->point, frame->error))
		return false;
	if (storage.database == 0)
	{
		*frame->error = txn_fold_fork("XX000", "a built base's storage has no database");
		return false;
	}
	for (; frame->has_nodes && frame->logged_at < frame->logged.len; frame->logged_at++)
	{
		trees_TupleAddressVecU8VecU8_t *node = &STD_VEC_AT(&frame->logged, trees_TupleAddressVecU8VecU8_t,
														   frame->logged_at);
		uint8		kind = node->f1.len > 0 ? ((const uint8 *) node->f1.ptr)[0] : 0;
		TxnRecordChunkWrite write = {.repository = storage.database, .address = node->f0, .bytes = node->f1};
		HostDelayCheckpoint window;

		if (kind == CHUNKS_HEADER_TREE_LEAF || kind == CHUNKS_HEADER_TREE_INTERNAL)
		{
			write.has_node = true;
			write.node.stamps = node->f2;
			write.node.tablespace = storage.tablespace;
			write.node.relation = storage.relation;
			write.node.point = frame->point;
			write.node.writer = frame->nodes;
		}
		txn_record_ChunkWrite_encode(&write, &body);
		ok = host_delay_checkpoint(doors, NULL, 0, &window, frame->error);
		if (ok)
		{
			ok = host_wal_insert(doors, HOST_WAL_KIND_CHUNK, body.ptr, body.len, &lsn, frame->error);
			if (ok)
				host_DelayCheckpoint_finish(&window);
			else
				host_DelayCheckpoint_drop(&window);
		}
		std_vec_free_Checked(&body, TXN_FOLD_BYTES);
		if (!ok)
			return false;
	}
	if (!host_pause(HOST_SHARED_IO_PAUSES(frame->io), "doltgres-built-logged", frame->error))
		return false;
	advanced.storage = storage;
	advanced.folded = frame->point;
	advanced.max_folded = frame->installed.next.max_folded;
	advanced.base = frame->installed.next;
	advanced.packs = frame->installed.packs;
	advanced.built = true;
	advanced.horizon = frame->horizon;
	advanced.has_chain = false;
	frame->installed.some = false;
	txn_record_BaseAdvanced_encode(&advanced, &body);
	ok = host_wal_insert(doors, HOST_WAL_KIND_BASE_ADVANCED, body.ptr, body.len, &lsn, frame->error);
	std_vec_free_Checked(&body, TXN_FOLD_BYTES);
	txn_record_BaseAdvanced_drop(&advanced);
	return ok;
}

/* txn_fold_whole_body takes the writer's logged nodes, installs the base under the registration, and logs it. */
static void
txn_fold_whole_body(void *frame_)
{
	txn_fold_whole_frame_t *frame = frame_;
	ChunksErrorError failed;

	trees_store_Writer_logged(&frame->writer->core, &frame->logged);
	frame->logged_live = true;
	if (!chunks_packs_roots_RootSlots_register(frame->io, txn_fold_whole_closure_0, frame, NULL, &frame->installed,
											   &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	*frame->result = frame->installed.some;
	if (!frame->installed.some)
	{
		frame->ok = true;
		return;
	}
	frame->ok = txn_fold_whole_logged(frame);
}

/* txn_fold_whole_pad frees what whole holds live: the install, the logged nodes, the base, and the writer. */
static void
txn_fold_whole_pad(void *frame_)
{
	txn_fold_whole_frame_t *frame = frame_;

	txn_fold_Installed_drop(&frame->installed);
	if (frame->logged_live)
		std_vec_drop_Checked(&frame->logged, sizeof(trees_TupleAddressVecU8VecU8_t),
							 _Alignof(trees_TupleAddressVecU8VecU8_t), txn_fold_Logged_drop);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	if (frame->writer_live)
		trees_store_BrandedWriter_drop(frame->writer);
	frame->logged_live = frame->base_live = frame->writer_live = false;
}

/*
 * txn_fold_whole installs a base a backend wrote whole, taking the base and
 * the writer whose packs hold its trees' roots, held in the registration's
 * slots until the base that names them is installed as the state's newest,
 * at a fold point, when its base generation is still the one read before the
 * build, and for a permanent storage logged as built, after every node the
 * writer kept as a Chunk record, each in its own checkpoint window, children
 * before parents; with nodes none it logs no chunk. The record carries its
 * replay's conflict horizon. It answers whether it installed.
 */
static bool
txn_fold_whole(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const HostSharedHeld *held,
			   TreesStoreBrandedWriter *writer, const bool *has_roots, const size_t *roots, TxnBaseBase *base,
			   HostLsn point, uint64 generation, uint64 horizon, bool has_nodes, uint64 nodes, bool *result,
			   HostError *error)
{
	txn_fold_whole_frame_t frame = {.io = io, .repo = repo, .reading = reading, .held = held, .writer = writer,
		.writer_live = true, .has_roots = has_roots, .roots = roots, .base = *base, .base_live = true,
		.point = point, .generation = generation, .horizon = horizon, .has_nodes = has_nodes, .nodes = nodes,
	.result = result, .error = error};

	std_landing(&frame, txn_fold_whole_body, txn_fold_whole_pad);
	txn_fold_whole_pad(&frame);
	return frame.ok;
}

/* txn_fold_entry_order orders entries by their keys' bytes, for qsort. */
static int
txn_fold_entry_order(const void *a, const void *b)
{
	const TreesBuildEntry *x = a;
	const TreesBuildEntry *y = b;
	int			c = memcmp(x->key.ptr, y->key.ptr, Min(x->key.len, y->key.len));

	if (c != 0)
		return c;
	return (x->key.len > y->key.len) - (x->key.len < y->key.len);
}

/* txn_fold_Entry_drop frees an entry held as a vector's element. */
static void
txn_fold_Entry_drop(void *entry)
{
	trees_build_Entry_drop(entry);
}

/*
 * txn_fold_built_frame_t is built's state across its pad: the trees' entries
 * it takes, the base it takes, the keys and their order, its writer and its
 * builder, and its shapes, each while live.
 */
typedef struct txn_fold_built_frame_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const HostSharedHeld *held;
	const TreesKeyOrder *order;
	TxnFoldTrees trees;
	bool		trees_live;
	TxnBaseBase base;
	bool		base_live;
	HostLsn		point;
	bool		has_nodes;
	uint64		nodes;
	uint64		horizon;
	StdVec		keys;
	bool		keys_live;
	StdVec		positions;
	bool		positions_live;
	StdVec		rows;
	bool		rows_live;
	TreesShape	shapes[4];
	int			shapes_live;
	TreesStoreWriter plain;
	TreesStoreBrandedWriter writer;
	bool		writer_live;
	TreesStoreBrandedBuilder builder;
	bool		builder_live;
	StdVec	   *building;
	size_t		at;
	bool		has_roots[4];
	size_t		roots[4];
	bool	   *result;
	HostError  *error;
	bool		ok;
} txn_fold_built_frame_t;

/*
 * txn_fold_built_body sorts the trees' entries, builds each nonempty tree in
 * one writer, and installs the base whole.
 */
static void
txn_fold_built_body(void *frame_)
{
	txn_fold_built_frame_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	uint64		generation = txn_state_generation(record);
	TreesStoreReader reader = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
	ChunksErrorError failed;
	StdVec	   *lists[4];

	std_vec_with_capacity_Checked(&frame->keys, frame->trees.rows.len, sizeof(StdBytes), _Alignof(StdBytes));
	frame->keys_live = true;
	for (size_t i = 0; i < frame->trees.rows.len; i++)
	{
		const TreesBuildEntry *entry = &STD_VEC_AT(&frame->trees.rows, TreesBuildEntry, i);
		StdBytes	key = {entry->key.ptr, entry->key.len};

		std_vec_push_Checked(&frame->keys, &key, sizeof(StdBytes), _Alignof(StdBytes));
	}
	if (!trees_sorted_positions(frame->io, frame->keys.ptr, frame->keys.len, frame->order, &frame->positions,
								&failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	frame->positions_live = true;
	std_vec_with_capacity_Checked(&frame->rows, frame->trees.rows.len, TXN_FOLD_ENTRY);
	frame->rows_live = true;
	for (size_t i = 0; i < frame->positions.len; i++)
		std_vec_push_Checked(&frame->rows, &STD_VEC_AT(&frame->trees.rows, TreesBuildEntry,
													   STD_VEC_AT(&frame->positions, size_t, i)), TXN_FOLD_ENTRY);
	std_vec_free_Checked(&frame->trees.rows, TXN_FOLD_ENTRY);
	frame->trees.rows = frame->rows;
	frame->rows_live = false;
	qsort(frame->trees.tuple_ids.ptr, frame->trees.tuple_ids.len, sizeof(TreesBuildEntry), txn_fold_entry_order);
	qsort(frame->trees.free_space.ptr, frame->trees.free_space.len, sizeof(TreesBuildEntry), txn_fold_entry_order);
	qsort(frame->trees.pending.ptr, frame->trees.pending.len, sizeof(TreesBuildEntry), txn_fold_entry_order);
	frame->shapes[0].has_summarizer = false;
	trees_KeyOrder_Clone_clone(frame->order, &frame->shapes[0].order);
	frame->shapes_live = 1;
	frame->shapes[1] = trees_Shape_Default_default();
	frame->shapes_live = 2;
	frame->shapes[2] = txn_free_space_shape();
	frame->shapes_live = 3;
	frame->shapes[3] = trees_Shape_Default_default();
	frame->shapes_live = 4;
	if (!trees_store_Writer_new(frame->io, reader, txn_state_space(record), &frame->plain, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	if (frame->has_nodes)
		trees_store_Writer_logging(&frame->plain);
	trees_store_Writer_brand(&frame->plain, &frame->writer);
	frame->writer_live = true;
	lists[0] = &frame->trees.rows;
	lists[1] = &frame->trees.tuple_ids;
	lists[2] = &frame->trees.free_space;
	lists[3] = &frame->trees.pending;
	for (int t = 0; t < 4; t++)
	{
		frame->has_roots[t] = lists[t]->len > 0;
		if (!frame->has_roots[t])
			continue;
		trees_store_BrandedWriter_shaped(&frame->writer, TREES_CHUNKER_DEFAULT_TARGET, &frame->shapes[t],
										 &frame->builder);
		frame->builder_live = true;
		frame->building = lists[t];
		for (frame->at = 0; frame->at < lists[t]->len;)
		{
			TreesBuildEntry *entry = &STD_VEC_AT(lists[t], TreesBuildEntry, frame->at);

			frame->at++;
			if (!trees_build_Builder_add(&frame->builder.builder, frame->io, entry, &failed))
			{
				*frame->error = txn_base_storage(&failed);
				return;
			}
		}
		frame->building = NULL;
		std_vec_free_Checked(lists[t], TXN_FOLD_ENTRY);
		std_vec_new(lists[t], TXN_FOLD_ENTRY);
		frame->builder_live = false;
		if (!trees_store_BrandedBuilder_finish(&frame->builder, frame->io, &frame->roots[t], &failed))
		{
			*frame->error = txn_base_storage(&failed);
			return;
		}
	}
	frame->writer_live = frame->base_live = false;
	frame->ok = txn_fold_whole(frame->io, frame->repo, frame->reading, frame->held, &frame->writer, frame->has_roots,
							   frame->roots, &frame->base, frame->point, generation, frame->horizon,
							   frame->has_nodes, frame->nodes, frame->result, frame->error);
}

/* txn_fold_built_pad frees what built holds live. */
static void
txn_fold_built_pad(void *frame_)
{
	txn_fold_built_frame_t *frame = frame_;

	if (frame->builder_live)
		trees_build_Builder_drop(&frame->builder.builder);
	if (frame->building != NULL)
	{
		for (size_t i = frame->at; i < frame->building->len; i++)
			trees_build_Entry_drop(&STD_VEC_AT(frame->building, TreesBuildEntry, i));
		frame->building->len = 0;
	}
	if (frame->writer_live)
		trees_store_BrandedWriter_drop(&frame->writer);
	while (frame->shapes_live > 0)
		trees_Shape_drop(&frame->shapes[--frame->shapes_live]);
	if (frame->rows_live)
		std_vec_free_Checked(&frame->rows, TXN_FOLD_ENTRY);
	if (frame->positions_live)
		std_vec_free_Checked(&frame->positions, sizeof(size_t), _Alignof(size_t));
	if (frame->keys_live)
		std_vec_free_Checked(&frame->keys, sizeof(StdBytes), _Alignof(StdBytes));
	if (frame->trees_live)
	{
		std_vec_drop_Checked(&frame->trees.rows, TXN_FOLD_ENTRY, txn_fold_Entry_drop);
		std_vec_drop_Checked(&frame->trees.tuple_ids, TXN_FOLD_ENTRY, txn_fold_Entry_drop);
		std_vec_drop_Checked(&frame->trees.free_space, TXN_FOLD_ENTRY, txn_fold_Entry_drop);
		std_vec_drop_Checked(&frame->trees.pending, TXN_FOLD_ENTRY, txn_fold_Entry_drop);
	}
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->builder_live = frame->writer_live = frame->rows_live = frame->positions_live = false;
	frame->keys_live = frame->trees_live = frame->base_live = false;
	frame->building = NULL;
}

/*
 * txn_fold_built installs a base a backend built for a storage that holds
 * none of its own yet, taking the trees and the base: its trees written
 * from empty in one writer's packs, sorted before any registration, then
 * held in the registration's slots until the base that names them is
 * installed as the state's newest, at a fold point, when its base
 * generation is still the one read before the build. A permanent storage's
 * base is logged as built after every node of its trees as a Chunk record;
 * a relfilenode whose writes skip the WAL (nodes none) logs none. It
 * answers whether it installed.
 */
bool
txn_fold_built(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const HostSharedHeld *held,
			   const TreesKeyOrder *order, TxnFoldTrees *trees, TxnBaseBase *base, HostLsn point, bool has_nodes,
			   uint64 nodes, uint64 horizon, bool *result, HostError *error)
{
	txn_fold_built_frame_t frame = {.io = io, .repo = repo, .reading = reading, .held = held, .order = order,
		.trees = *trees, .trees_live = true, .base = *base, .base_live = true, .point = point,
	.has_nodes = has_nodes, .nodes = nodes, .horizon = horizon, .result = result, .error = error};

	std_landing(&frame, txn_fold_built_body, txn_fold_built_pad);
	txn_fold_built_pad(&frame);
	return frame.ok;
}

/* TxnFoldReformRow is reformed's (Entry, u32): a row the re-form writes, with the epoch it is stored at. */
typedef struct TxnFoldReformRow
{
	TreesBuildEntry entry;
	uint32		epoch;
} TxnFoldReformRow;

/*
 * txn_TupleU64VecU8U32_t is reformed's (u64, (Vec<u8>, u32)): a tuple id the
 * re-form pairs with its row's key and heap bytes.
 */
typedef struct txn_TupleU64VecU8U32_t
{
	uint64		f0;
	StdVec		f1;
	uint32		f2;
} txn_TupleU64VecU8U32_t;

#define TXN_FOLD_REFORM_ROW sizeof(TxnFoldReformRow), _Alignof(TxnFoldReformRow)
#define TXN_FOLD_PAIRED sizeof(txn_TupleU64VecU8U32_t), _Alignof(txn_TupleU64VecU8U32_t)
#define TXN_FOLD_BASED sizeof(TxnRewriteBased), _Alignof(TxnRewriteBased)

/* txn_fold_ReformRow_drop frees a row the re-form writes. */
static void
txn_fold_ReformRow_drop(void *row)
{
	trees_build_Entry_drop(&((TxnFoldReformRow *) row)->entry);
}

/* txn_fold_Paired_drop frees a pairing's key. */
static void
txn_fold_Paired_drop(void *paired)
{
	std_vec_free_Checked(&((txn_TupleU64VecU8U32_t *) paired)->f1, TXN_FOLD_BYTES);
}

/* txn_fold_bytes_order orders byte strings by their bytes, for qsort and bsearch. */
static int
txn_fold_bytes_order(const void *a, const void *b)
{
	const StdBytes *x = a;
	const StdBytes *y = b;
	int			c = memcmp(x->ptr, y->ptr, Min(x->len, y->len));

	if (c != 0)
		return c;
	return (x->len > y->len) - (x->len < y->len);
}

/*
 * txn_fold_reformed_frame_t is reformed's state across its pad: the edits,
 * formed rows, and tuple id edits it takes, the edited keys, the rows it
 * re-forms, the base's entries, the written heap bytes, the pairings, and
 * its outputs, each while live.
 */
typedef struct txn_fold_reformed_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	const TxnBaseBase *base;
	const TxnVisibleLive *chain;
	size_t		chain_len;
	StdVec		edits;
	size_t		edits_at;
	StdVec		formed;
	StdVec		ctids;
	bool		ctids_live;
	TxnFoldReform *reform;
	StdVec		edited;
	bool		edited_live;
	StdVec		rows;
	size_t		rows_at;
	bool		rows_live;
	StdVec		based;
	size_t		based_at;
	bool		based_live;
	StdVec		written;
	bool		written_live;
	StdVec		paired;
	bool		paired_live;
	StdVec		out;
	bool		out_live;
	StdVec		kept;
	bool		kept_live;
	TxnFoldReformed reformed;
	bool		reformed_live;
	StdVec	   *out_edits;
	StdVec	   *out_ctids;
	TxnBaseSizes *sizes;
	uint32	   *epoch;
	HostError  *error;
	bool		ok;
} txn_fold_reformed_frame_t;

/* txn_fold_reformed_corrupt sets reformed's error of a row's epoch past its chain's end. */
static void
txn_fold_reformed_corrupt(txn_fold_reformed_frame_t *frame)
{
	*frame->error = txn_fold_fork("XX001", "a re-formed row's epoch is past its chain's end");
}

/*
 * txn_fold_reformed_rows lists the rows the re-form writes: each edit's row
 * from its epoch, then each base row no edit replaces.
 */
static bool
txn_fold_reformed_rows(txn_fold_reformed_frame_t *frame)
{
	std_vec_with_capacity_Checked(&frame->edited, frame->edits.len, sizeof(StdBytes), _Alignof(StdBytes));
	frame->edited_live = true;
	for (size_t i = 0; i < frame->edits.len; i++)
	{
		StdBytes	key = trees_mutate_Edit_key(&STD_VEC_AT(&frame->edits, TreesMutateEdit, i));

		std_vec_push_Checked(&frame->edited, &key, sizeof(StdBytes), _Alignof(StdBytes));
	}
	qsort(frame->edited.ptr, frame->edited.len, sizeof(StdBytes), txn_fold_bytes_order);
	std_vec_with_capacity_Checked(&frame->rows, frame->edits.len, TXN_FOLD_REFORM_ROW);
	frame->rows_live = true;
	std_vec_with_capacity_Checked(&frame->out, frame->edits.len, TXN_FOLD_EDIT);
	frame->out_live = true;
	for (; frame->edits_at < Min(frame->edits.len, frame->formed.len); frame->edits_at++)
	{
		TreesMutateEdit *edit = &STD_VEC_AT(&frame->edits, TreesMutateEdit, frame->edits_at);
		const txn_OptionTupleU64U32_t *formed = &STD_VEC_AT(&frame->formed, txn_OptionTupleU64U32_t,
															frame->edits_at);
		TxnFoldReformRow row = {.epoch = frame->base->epoch};

		if (edit->tag != TREES_MUTATE_EDIT_PUT)
		{
			std_vec_push_Checked(&frame->out, edit, TXN_FOLD_EDIT);
			continue;
		}
		if (formed->has)
		{
			TxnStatusCommit commit;
			HostXid		top = {formed->v.f0};

			if (!txn_status_committed(frame->io, top, &commit, frame->error))
				return false;
			row.epoch = txn_visible_epoch_after(frame->base->epoch, frame->chain, frame->chain_len, formed->v.f0,
												formed->v.f1, commit.tag == TXN_STATUS_COMMIT_AT,
												commit.tag == TXN_STATUS_COMMIT_AT ? commit.u.at : 0);
		}
		row.entry = edit->u.put;
		std_vec_push_Checked(&frame->rows, &row, TXN_FOLD_REFORM_ROW);
	}
	if (!txn_rewrite_based(frame->io, frame->store, frame->base->has_root, frame->base->root, &frame->based,
						   frame->error))
		return false;
	frame->based_live = true;
	for (; frame->based_at < frame->based.len; frame->based_at++)
	{
		TxnRewriteBased *based = &STD_VEC_AT(&frame->based, TxnRewriteBased, frame->based_at);
		StdBytes	key = {based->key.ptr, based->key.len};
		TxnFoldReformRow row = {.epoch = frame->base->epoch};

		if (bsearch(&key, frame->edited.ptr, frame->edited.len, sizeof(StdBytes), txn_fold_bytes_order) != NULL)
		{
			txn_rewrite_Based_drop(based);
			continue;
		}
		row.entry.key = based->key;
		row.entry.value = based->value;
		row.entry.addresses = based->addresses;
		row.entry.stamps = based->stamps;
		row.entry.values = based->values;
		std_vec_reserve_Checked(&frame->rows, 1, TXN_FOLD_REFORM_ROW);
		std_vec_push_Checked(&frame->rows, &row, TXN_FOLD_REFORM_ROW);
	}
	return true;
}

/* txn_fold_paired_insert puts a tuple id's pairing in tuple id order, taking it, replacing one of its tuple id. */
static void
txn_fold_paired_insert(StdVec *paired, txn_TupleU64VecU8U32_t *pair)
{
	size_t		low = 0;
	size_t		high = paired->len;

	while (low < high)
	{
		size_t		mid = low + (high - low) / 2;

		if (STD_VEC_AT(paired, txn_TupleU64VecU8U32_t, mid).f0 < pair->f0)
			low = mid + 1;
		else
			high = mid;
	}
	if (low < paired->len && STD_VEC_AT(paired, txn_TupleU64VecU8U32_t, low).f0 == pair->f0)
	{
		txn_fold_Paired_drop(&STD_VEC_AT(paired, txn_TupleU64VecU8U32_t, low));
		STD_VEC_AT(paired, txn_TupleU64VecU8U32_t, low) = *pair;
		return;
	}
	std_vec_insert(paired, low, pair, TXN_FOLD_PAIRED, RUST_FILE, 1845, 9);
}

/* txn_fold_written_heap returns the heap bytes the fold's tuple id edits pair with a tuple id, the last edit's. */
static bool
txn_fold_written_heap(const StdVec *ctids, uint64 tuple, uint32 *result)
{
	bool		found = false;

	for (size_t i = 0; i < ctids->len; i++)
	{
		const TxnFoldCtidEdit *ctid = &STD_VEC_AT(ctids, TxnFoldCtidEdit, i);

		if (ctid->f0 == tuple && ctid->has_f1)
		{
			*result = ctid->f1.f1;
			found = true;
		}
	}
	return found;
}

/*
 * txn_fold_reformed_body re-forms each row from its epoch to the chain's
 * end, a row written at the end kept as it is, pairs each one's tuple id
 * with its heap bytes, and counts the base's sizes afresh.
 */
static void
txn_fold_reformed_body(void *frame_)
{
	txn_fold_reformed_frame_t *frame = frame_;
	uint32		end;

	if (frame->chain_len > PG_UINT32_MAX || frame->base->epoch > PG_UINT32_MAX - (uint32) frame->chain_len)
	{
		txn_fold_reformed_corrupt(frame);
		return;
	}
	end = frame->base->epoch + (uint32) frame->chain_len;
	if (!txn_fold_reformed_rows(frame))
		return;
	memset(frame->sizes, 0, sizeof(*frame->sizes));
	std_vec_new(&frame->paired, TXN_FOLD_PAIRED);
	frame->paired_live = true;
	for (; frame->rows_at < frame->rows.len; frame->rows_at++)
	{
		TxnFoldReformRow *row = &STD_VEC_AT(&frame->rows, TxnFoldReformRow, frame->rows_at);
		TreesBuildEntry *entry = &row->entry;
		ChunksTrailerStamp stamp;
		uint32		heap_bytes;
		txn_TupleU64VecU8U32_t pair;
		TreesMutateEdit edit = {.tag = TREES_MUTATE_EDIT_PUT};

		if (entry->stamps.len != 1)
		{
			*frame->error = txn_fold_fork("XX001", "a keyed table's entry holds other than one row");
			return;
		}
		stamp = STD_VEC_AT(&entry->stamps, ChunksTrailerStamp, 0);
		if (row->epoch == end)
		{
			if (!txn_fold_written_heap(&frame->ctids, stamp.tuple, &heap_bytes))
			{
				txn_fold_reformed_corrupt(frame);
				return;
			}
		}
		else
		{
			size_t		from;

			if (row->epoch < frame->base->epoch || row->epoch - frame->base->epoch > frame->chain_len)
			{
				txn_fold_reformed_corrupt(frame);
				return;
			}
			from = row->epoch - frame->base->epoch;
			if (!frame->reform->ops->reformed(frame->reform->self, frame->io, entry->key.ptr, entry->key.len, stamp,
											  entry->value.ptr, entry->value.len, entry->addresses.ptr,
											  entry->addresses.len, entry->values.ptr, entry->values.len,
											  frame->chain + from, frame->chain_len - from, &frame->reformed,
											  frame->error))
				return;
			std_vec_free_Checked(&entry->value, TXN_FOLD_BYTES);
			std_vec_free_Checked(&entry->addresses, TXN_FOLD_ADDRESS);
			std_vec_free_Checked(&entry->values, TXN_FOLD_U32);
			entry->value = frame->reformed.tuple;
			entry->addresses = frame->reformed.addresses;
			entry->values = frame->reformed.values;
			heap_bytes = frame->reformed.heap_bytes;
		}
		*frame->sizes = txn_base_Sizes_summed(*frame->sizes, txn_fold_sized(entry->key.ptr, entry->key.len,
																			entry->value.ptr, entry->value.len, 1));
		frame->sizes->heap_bytes = STD_ADD(u64, frame->sizes->heap_bytes, heap_bytes, 1844, 9);
		pair.f0 = stamp.tuple;
		pair.f2 = heap_bytes;
		std_vec_reserve_Checked(&frame->paired, 1, TXN_FOLD_PAIRED);
		txn_fold_bytes(entry->key.ptr, entry->key.len, &pair.f1);
		txn_fold_paired_insert(&frame->paired, &pair);
		edit.u.put = *entry;
		std_vec_reserve_Checked(&frame->out, 1, TXN_FOLD_EDIT);
		std_vec_push_Checked(&frame->out, &edit, TXN_FOLD_EDIT);
	}
	std_vec_new(&frame->kept, TXN_FOLD_CTID);
	frame->kept_live = true;
	for (size_t i = 0; i < frame->ctids.len; i++)
	{
		TxnFoldCtidEdit *ctid = &STD_VEC_AT(&frame->ctids, TxnFoldCtidEdit, i);
		bool		pairs = false;

		for (size_t j = 0; j < frame->paired.len && !pairs; j++)
			pairs = STD_VEC_AT(&frame->paired, txn_TupleU64VecU8U32_t, j).f0 == ctid->f0;
		if (ctid->has_f1 || pairs)
		{
			txn_fold_CtidEdit_drop(ctid);
			continue;
		}
		std_vec_push_Checked(&frame->kept, ctid, TXN_FOLD_CTID);
	}
	std_vec_free_Checked(&frame->ctids, TXN_FOLD_CTID);
	frame->ctids_live = false;
	std_vec_reserve_Checked(&frame->kept, frame->paired.len, TXN_FOLD_CTID);
	for (size_t i = 0; i < frame->paired.len; i++)
	{
		txn_TupleU64VecU8U32_t *pair = &STD_VEC_AT(&frame->paired, txn_TupleU64VecU8U32_t, i);
		TxnFoldCtidEdit ctid = {.f0 = pair->f0, .has_f1 = true, .f1 = {pair->f1, pair->f2}};

		std_vec_push_Checked(&frame->kept, &ctid, TXN_FOLD_CTID);
	}
	std_vec_free_Checked(&frame->paired, TXN_FOLD_PAIRED);
	frame->paired_live = false;
	*frame->out_edits = frame->out;
	*frame->out_ctids = frame->kept;
	*frame->epoch = end;
	frame->out_live = frame->kept_live = false;
	frame->ok = true;
}

/* txn_fold_reformed_pad frees what reformed holds live. */
static void
txn_fold_reformed_pad(void *frame_)
{
	txn_fold_reformed_frame_t *frame = frame_;

	if (frame->kept_live)
		std_vec_drop_Checked(&frame->kept, TXN_FOLD_CTID, txn_fold_CtidEdit_drop);
	if (frame->out_live)
		std_vec_drop_Checked(&frame->out, TXN_FOLD_EDIT, trees_mutate_Edit_drop);
	if (frame->paired_live)
		std_vec_drop_Checked(&frame->paired, TXN_FOLD_PAIRED, txn_fold_Paired_drop);
	if (frame->based_live)
	{
		for (; frame->based_at < frame->based.len; frame->based_at++)
			txn_rewrite_Based_drop(&STD_VEC_AT(&frame->based, TxnRewriteBased, frame->based_at));
		std_vec_free_Checked(&frame->based, TXN_FOLD_BASED);
	}
	if (frame->rows_live)
	{
		for (; frame->rows_at < frame->rows.len; frame->rows_at++)
			txn_fold_ReformRow_drop(&STD_VEC_AT(&frame->rows, TxnFoldReformRow, frame->rows_at));
		std_vec_free_Checked(&frame->rows, TXN_FOLD_REFORM_ROW);
	}
	if (frame->edited_live)
		std_vec_free_Checked(&frame->edited, sizeof(StdBytes), _Alignof(StdBytes));
	for (; frame->edits_at < frame->edits.len; frame->edits_at++)
		trees_mutate_Edit_drop(&STD_VEC_AT(&frame->edits, TreesMutateEdit, frame->edits_at));
	std_vec_free_Checked(&frame->edits, TXN_FOLD_EDIT);
	std_vec_free_Checked(&frame->formed, TXN_FOLD_FORMED_BY);
	if (frame->ctids_live)
		std_vec_drop_Checked(&frame->ctids, TXN_FOLD_CTID, txn_fold_CtidEdit_drop);
	frame->kept_live = frame->out_live = frame->paired_live = frame->based_live = false;
	frame->rows_live = frame->edited_live = frame->ctids_live = false;
}

/*
 * txn_fold_reformed re-forms a fold's base to its chain's end, taking the
 * fold's edits, their formed rows' writers, and its tuple id edits: each
 * edit's row and each base row the fold leaves as it is, from the epoch it
 * is stored at, through the caller's re-form, its tuple id paired again with
 * its new heap bytes, a row written at the chain's end kept as it is, and
 * the base's sizes counted afresh over every row it keeps. A keyed table's
 * entry is one row with one stamp. It answers the edits, the tuple id tree's
 * edits, the sizes, and the chain's end as the base's epoch.
 */
static bool
txn_fold_reformed(HostIoIo *io, TreesStoreNodeStore store, const TxnBaseBase *base, const TxnVisibleLive *chain,
				  size_t chain_len, StdVec *edits, StdVec *formed, StdVec *ctids, TxnFoldReform *reform,
				  StdVec *out_edits, StdVec *out_ctids, TxnBaseSizes *sizes, uint32 *epoch, HostError *error)
{
	txn_fold_reformed_frame_t frame = {.io = io, .store = store, .base = base, .chain = chain,
		.chain_len = chain_len, .edits = *edits, .formed = *formed, .ctids = *ctids, .ctids_live = true,
		.reform = reform, .out_edits = out_edits, .out_ctids = out_ctids, .sizes = sizes, .epoch = epoch,
	.error = error};

	std_landing(&frame, txn_fold_reformed_body, txn_fold_reformed_pad);
	txn_fold_reformed_pad(&frame);
	return frame.ok;
}

/* txn_fold_named_count answers how many addresses a chunk's hashed bytes name, by its header. */
static bool
txn_fold_named_count(void *captures, const uint8 *hashed, size_t hashed_len, size_t *result, ChunksErrorError *error)
{
	ChunksHeaderHeader header;
	size_t		read;

	if (!chunks_header_Header_read(hashed, hashed_len, &header, &read, error))
		return false;
	*result = chunks_header_Addresses_len(&header.addresses);
	chunks_header_Header_drop(&header);
	return true;
}

/* txn_fold_copy_order orders a memo's copies by the locator each was read at, for bsearch. */
static int
txn_fold_copy_order(const void *a, const void *b)
{
	return (int) chunks_trailer_Locator_Ord_cmp(a, b);
}

/*
 * txn_fold_copied_chunk_frame_t is copied_chunk's state across its pad: the
 * chunk it read, its addresses, the locators it gives them, and a child's
 * parts, each while live.
 */
typedef struct txn_fold_copied_chunk_frame_t
{
	HostIoIo   *io;
	TreesStoreNodeStore store;
	TreesStoreBrandedWriter *writer;
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;
	TxnFoldCopies *memo;
	ChunksChunkChunk chunk;
	bool		chunk_live;
	StdVec		addresses;
	bool		addresses_live;
	StdVec		locators;
	bool		locators_live;
	StdVec		child;
	StdVec		child_locators;
	ChunksTrailerStamps child_stamps;
	StdVec	   *hashed;
	StdVec	   *result_locators;
	ChunksTrailerStamps *stamps;
	HostError  *error;
	bool		ok;
} txn_fold_copied_chunk_frame_t;

static bool txn_fold_copied_chunk(HostIoIo *io, TreesStoreNodeStore store, TreesStoreBrandedWriter *writer,
								  ChunksAddressAddress address, ChunksTrailerLocator locator, TxnFoldCopies *memo,
								  StdVec *hashed, StdVec *locators, ChunksTrailerStamps *stamps, HostError *error);

/*
 * txn_fold_copied_chunk_body reads the chunk and gives each child a pack holds
 * its copy's locator, written first once.
 */
static void
txn_fold_copied_chunk_body(void *frame_)
{
	txn_fold_copied_chunk_frame_t *frame = frame_;
	TreesStoreNodeStore written = TREES_STORE_WRITER_NODE_STORE(&frame->writer->core);
	ChunksErrorError failed;
	ChunksTrailerStored split;

	if (!frame->store.ops->read_record(frame->store.self, frame->io, &frame->address, frame->locator, &frame->chunk,
									   &failed))
		goto storage;
	frame->chunk_live = true;
	if (!chunks_chunk_Chunk_split(&frame->chunk, txn_fold_named_count, NULL, NULL, &split, &failed) ||
		!chunks_header_addresses(split.hashed.ptr, split.hashed.len, &frame->addresses, &failed))
		goto storage;
	frame->addresses_live = true;
	std_vec_with_capacity_Checked(&frame->locators, frame->addresses.len, TXN_FOLD_LOCATOR);
	frame->locators_live = true;
	for (size_t at = 0; at < frame->addresses.len; at++)
	{
		ChunksTrailerLocator known = chunks_trailer_Stored_locator(&split, at);
		const txn_TupleLocatorLocator_t *copy;
		TreesStoreTreeRef ref;

		if (!chunks_trailer_Locator_is_known(&known))
		{
			std_vec_push_Checked(&frame->locators, &known, TXN_FOLD_LOCATOR);
			continue;
		}
		copy = bsearch(&known, frame->memo->copies.ptr, frame->memo->copies.len, sizeof(txn_TupleLocatorLocator_t),
					   txn_fold_copy_order);
		if (copy != NULL)
		{
			std_vec_push_Checked(&frame->locators, &copy->f1, TXN_FOLD_LOCATOR);
			continue;
		}
		if (!txn_fold_copied_chunk(frame->io, frame->store, frame->writer,
								   STD_VEC_AT(&frame->addresses, ChunksAddressAddress, at), known, frame->memo,
								   &frame->child, &frame->child_locators, &frame->child_stamps, frame->error))
			return;
		if (!written.ops->write(written.self, frame->io, &frame->child, frame->child_locators.ptr,
								frame->child_locators.len, &frame->child_stamps, &ref, &failed))
		{
			std_vec_free_Checked(&frame->child_locators, TXN_FOLD_LOCATOR);
			chunks_trailer_Stamps_drop(&frame->child_stamps);
			goto storage;
		}
		std_vec_free_Checked(&frame->child_locators, TXN_FOLD_LOCATOR);
		chunks_trailer_Stamps_drop(&frame->child_stamps);
		{
			txn_TupleLocatorLocator_t memo = {known, ref.locator};
			size_t		low = 0;
			size_t		high = frame->memo->copies.len;

			while (low < high)
			{
				size_t		mid = low + (high - low) / 2;

				if (chunks_trailer_Locator_Ord_cmp(&STD_VEC_AT(&frame->memo->copies, txn_TupleLocatorLocator_t,
															   mid).f0, &known) < 0)
					low = mid + 1;
				else
					high = mid;
			}
			std_vec_insert(&frame->memo->copies, low, &memo, TXN_FOLD_COPY, RUST_FILE, 1295, 17);
		}
		std_vec_push_Checked(&frame->locators, &ref.locator, TXN_FOLD_LOCATOR);
	}
	if (!chunks_trailer_Stored_stamps(&split, frame->stamps, &failed))
		goto storage;
	txn_fold_bytes(split.hashed.ptr, split.hashed.len, frame->hashed);
	*frame->result_locators = frame->locators;
	frame->locators_live = false;
	frame->ok = true;
	return;
storage:
	*frame->error = txn_base_storage(&failed);
}

/* txn_fold_copied_chunk_pad frees what copied_chunk holds live: the locators, the addresses, and the chunk. */
static void
txn_fold_copied_chunk_pad(void *frame_)
{
	txn_fold_copied_chunk_frame_t *frame = frame_;

	if (frame->locators_live)
		std_vec_free_Checked(&frame->locators, TXN_FOLD_LOCATOR);
	if (frame->addresses_live)
		std_vec_free_Checked(&frame->addresses, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (frame->chunk_live)
		chunks_chunk_Chunk_drop(&frame->chunk);
	frame->locators_live = frame->addresses_live = frame->chunk_live = false;
}

/*
 * txn_fold_copied_chunk readies a chunk of a base for a writer, each chunk
 * it names that a pack holds written first, once, at the writer's locator
 * (memo): its hashed bytes, its addresses' locators, and its stamps.
 */
static bool
txn_fold_copied_chunk(HostIoIo *io, TreesStoreNodeStore store, TreesStoreBrandedWriter *writer,
					  ChunksAddressAddress address, ChunksTrailerLocator locator, TxnFoldCopies *memo,
					  StdVec *hashed, StdVec *locators, ChunksTrailerStamps *stamps, HostError *error)
{
	txn_fold_copied_chunk_frame_t frame = {.io = io, .store = store, .writer = writer, .address = address,
		.locator = locator, .memo = memo, .hashed = hashed, .result_locators = locators, .stamps = stamps,
	.error = error};

	std_landing(&frame, txn_fold_copied_chunk_body, txn_fold_copied_chunk_pad);
	txn_fold_copied_chunk_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_copied_frame_t is copied's state across its pad: the base it takes,
 * its writer, and a root's readied parts.
 */
typedef struct txn_fold_copied_frame_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const HostSharedHeld *held;
	TxnBaseBase base;
	bool		base_live;
	HostLsn		point;
	bool		has_nodes;
	uint64		nodes;
	TxnFoldCopies *memo;
	TreesStoreWriter plain;
	TreesStoreBrandedWriter writer;
	bool		writer_live;
	StdVec		hashed;
	bool		hashed_live;
	StdVec		locators;
	ChunksTrailerStamps stamps;
	bool	   *result;
	HostError  *error;
	bool		ok;
} txn_fold_copied_frame_t;

/* txn_fold_copied_body copies each of the base's trees chunk by chunk into one writer and installs the base whole. */
static void
txn_fold_copied_body(void *frame_)
{
	txn_fold_copied_frame_t *frame = frame_;
	HostSharedRecord record = host_shared_Held_record(frame->held);
	uint64		generation = txn_state_generation(record);
	TreesStoreReader reader = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
	TreesStoreReader written = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
	ChunksErrorError failed;
	bool		has_roots[4] = {frame->base.has_root, frame->base.has_tuple_ids, frame->base.has_free_space,
	frame->base.has_pending};
	const TreesStoreTree *trees[4] = {&frame->base.root, &frame->base.tuple_ids, &frame->base.free_space,
	&frame->base.pending};
	size_t		roots[4];

	if (!trees_store_Writer_new(frame->io, written, txn_state_space(record), &frame->plain, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	if (frame->has_nodes)
		trees_store_Writer_logging(&frame->plain);
	trees_store_Writer_brand(&frame->plain, &frame->writer);
	frame->writer_live = true;
	for (int t = 0; t < 4; t++)
	{
		if (!has_roots[t])
			continue;
		if (!txn_fold_copied_chunk(frame->io, TREES_STORE_READER_NODE_STORE(&reader), &frame->writer,
								   trees[t]->address, trees[t]->locator, frame->memo, &frame->hashed,
								   &frame->locators, &frame->stamps, frame->error))
			return;
		if (!trees_store_BrandedWriter_write_root(&frame->writer, frame->io, &frame->hashed, frame->locators.ptr,
												  frame->locators.len, &frame->stamps, &roots[t], &failed))
		{
			std_vec_free_Checked(&frame->locators, TXN_FOLD_LOCATOR);
			chunks_trailer_Stamps_drop(&frame->stamps);
			*frame->error = txn_base_storage(&failed);
			return;
		}
		std_vec_free_Checked(&frame->locators, TXN_FOLD_LOCATOR);
		chunks_trailer_Stamps_drop(&frame->stamps);
	}
	frame->writer_live = frame->base_live = false;
	frame->ok = txn_fold_whole(frame->io, frame->repo, frame->reading, frame->held, &frame->writer, has_roots, roots,
							   &frame->base, frame->point, generation, frame->base.max_folded, frame->has_nodes,
							   frame->nodes, frame->result, frame->error);
}

/* txn_fold_copied_pad frees what copied holds live: its writer and the base. */
static void
txn_fold_copied_pad(void *frame_)
{
	txn_fold_copied_frame_t *frame = frame_;

	if (frame->writer_live)
		trees_store_BrandedWriter_drop(&frame->writer);
	if (frame->base_live)
		txn_base_Base_drop(&frame->base);
	frame->writer_live = frame->base_live = false;
}

/*
 * txn_fold_copied installs a copy of a base in a new storage, taking the
 * base, its four trees written again chunk by chunk into packs of the
 * storage's tablespace, every chunk they reach that a pack holds, each
 * keeping its address, hashed bytes, and stamps, at a fold point, and logged
 * as a built base whose replay's conflict horizon is its maxFolded. A chunk
 * the working state's delta holds keeps its unknown locator. A memo of the
 * chunks copied writes a chunk the base names more than once once. It
 * answers whether it installed.
 */
bool
txn_fold_copied(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const HostSharedHeld *held,
				TxnBaseBase *base, HostLsn point, bool has_nodes, uint64 nodes, TxnFoldCopies *memo, bool *result,
				HostError *error)
{
	txn_fold_copied_frame_t frame = {.io = io, .repo = repo, .reading = reading, .held = held, .base = *base,
		.base_live = true, .point = point, .has_nodes = has_nodes, .nodes = nodes, .memo = memo, .result = result,
	.error = error};

	std_landing(&frame, txn_fold_copied_body, txn_fold_copied_pad);
	txn_fold_copied_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_installs_done drops the versions of a storage's delta an
 * installed base folded through its point, its rows', its free space
 * map's, and its value map's, as a fold's own install drops them.
 */
static bool
txn_fold_installs_done(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
					   const HostSharedHeld *held, const TxnBaseBase *base, HostError *error)
{
	TreesStoreReader reader = repo_Repo_reader(repo, HOST_CACHE_PLACEMENT_SERVER, reading);
	txn_fold_held_pairing_t paired = {TREES_STORE_READER_NODE_STORE(&reader), base->has_tuple_ids, base->tuple_ids};
	TxnFoldSpaced spaced;

	if (!txn_fold_dropped(io, held, base->folded, txn_fold_held_pairing, &paired, error) ||
		!txn_fold_spaced(io, held, base->folded, txn_fold_held_pairing, &paired, &spaced, error))
		return false;
	std_vec_drop_Checked(&spaced.edits, TXN_FOLD_EDIT, trees_mutate_Edit_drop);
	if (!txn_fold_unspaced(io, held, &TXN_MEMTABLE_MAP_FREE_TREE_OPS, &spaced.removed, error) ||
		!txn_fold_valued(io, held, base->folded, &spaced, error))
		return false;
	std_vec_drop_Checked(&spaced.edits, TXN_FOLD_EDIT, trees_mutate_Edit_drop);
	return txn_fold_unspaced(io, held, &TXN_MEMTABLE_MAP_VALUES_TREE_OPS, &spaced.removed, error) &&
		txn_memtable_tuples_emptied(io, host_shared_Held_record(held), error);
}

/*
 * txn_fold_unbuilt_closure_0_captures_t is unbuilt's pick's captures: the
 * built base's tablespace, relation, and point.
 */
typedef struct txn_fold_unbuilt_closure_0_captures_t
{
	uint32		tablespace;
	uint32		relation;
	uint64		point;
} txn_fold_unbuilt_closure_0_captures_t;

/* txn_fold_unbuilt_closure_0 is unbuilt's pick: a node of the built base. */
static bool
txn_fold_unbuilt_closure_0(void *captures, HostIoIo *io, uint32 tablespace, uint32 relation, uint64 point,
						   uint64 writer, bool *result, HostError *error)
{
	txn_fold_unbuilt_closure_0_captures_t *base = captures;

	*result = tablespace == base->tablespace && relation == base->relation && point == base->point;
	return true;
}

/*
 * txn_fold_unbuilt takes out of the working state's delta the nodes a built
 * base's record logged, which this server installs the base without
 * rebuilding from.
 */
static bool
txn_fold_unbuilt(HostIoIo *io, const TxnRecordBaseAdvanced *advanced, HostError *error)
{
	txn_fold_unbuilt_closure_0_captures_t base = {advanced->storage.tablespace, advanced->storage.relation,
	advanced->folded};
	bool		some;
	HostSharedHeld deltas;
	bool		ok;

	if (advanced->storage.database == 0)
		return true;
	if (!txn_memtable_branch_find(HOST_SHARED_IO_WITHIN(io), advanced->storage.database, &some, &deltas, error))
		return false;
	if (!some)
		return true;
	ok = txn_memtable_branch_nodes_taken(io, host_shared_Held_record(&deltas), txn_fold_unbuilt_closure_0, &base,
										 NULL, error);
	host_shared_Held_drop(&deltas);
	return ok;
}

/*
 * txn_TupleAddressBool_t is rewritten's (Address, bool): a chunk the delta
 * held, true for a tree's node under the base.
 */
typedef struct txn_TupleAddressBool_t
{
	ChunksAddressAddress f0;
	bool		f1;
} txn_TupleAddressBool_t;

#define TXN_FOLD_TAKEN sizeof(txn_TupleAddressBool_t), _Alignof(txn_TupleAddressBool_t)

/*
 * txn_fold_rewritten_frame_t is rewritten's state across its pad: the bytes
 * the delta held, a chunk a pack held, the chunk's addresses and the
 * locators it gives them, and a child's readied parts, each while live.
 */
typedef struct txn_fold_rewritten_frame_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	HostSharedRecord deltas;
	uint32		tablespace;
	uint32		relation;
	uint64		point;
	TreesStoreBrandedWriter *writer;
	ChunksAddressAddress address;
	bool		blob;
	StdVec	   *taken;
	StdVec		stored;
	bool		stored_live;
	ChunksChunkChunk chunk;
	bool		chunk_live;
	StdVec		addresses;
	bool		addresses_live;
	StdVec		locators;
	bool		locators_live;
	ChunksTrailerStamps stamps;
	bool		stamps_live;
	StdVec		child;
	StdVec		child_locators;
	ChunksTrailerStamps child_stamps;
	bool	   *has_result;
	StdVec	   *hashed;
	StdVec	   *result_locators;
	ChunksTrailerStamps *result_stamps;
	HostError  *error;
	bool		ok;
} txn_fold_rewritten_frame_t;

static bool txn_fold_rewritten(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading,
							   HostSharedRecord deltas, uint32 tablespace, uint32 relation, uint64 point,
							   TreesStoreBrandedWriter *writer, ChunksAddressAddress address, bool blob, StdVec *taken,
							   bool *has_result, StdVec *hashed, StdVec *locators, ChunksTrailerStamps *stamps,
							   HostError *error);

/* txn_fold_rewritten_packed readies a blob's chunk this server's packs hold, as its record left it. */
static bool
txn_fold_rewritten_packed(txn_fold_rewritten_frame_t *frame, ChunksTrailerLocator locator)
{
	TreesStoreReader reader = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
	ChunksErrorError failed;
	ChunksTrailerStored split;
	size_t		named;

	if (!trees_store_Reader_read_record(&reader, frame->io, &frame->address, locator, &frame->chunk, &failed))
		goto storage;
	frame->chunk_live = true;
	if (!chunks_chunk_Chunk_split(&frame->chunk, txn_fold_named_count, NULL, NULL, &split, &failed) ||
		!chunks_header_addresses(split.hashed.ptr, split.hashed.len, &frame->addresses, &failed))
		goto storage;
	named = frame->addresses.len;
	frame->addresses_live = true;
	std_vec_with_capacity_Checked(&frame->locators, named, TXN_FOLD_LOCATOR);
	frame->locators_live = true;
	for (size_t at = 0; at < named; at++)
	{
		ChunksTrailerLocator known = chunks_trailer_Stored_locator(&split, at);

		std_vec_push_Checked(&frame->locators, &known, TXN_FOLD_LOCATOR);
	}
	if (!chunks_trailer_Stored_stamps(&split, frame->result_stamps, &failed))
		goto storage;
	txn_fold_bytes(split.hashed.ptr, split.hashed.len, frame->hashed);
	*frame->result_locators = frame->locators;
	frame->locators_live = false;
	*frame->has_result = true;
	return true;
storage:
	*frame->error = txn_base_storage(&failed);
	return false;
}

/*
 * txn_fold_rewritten_body readies the chunk the delta holds, each chunk it
 * names readied first and written at this server's locators, or a blob's
 * chunk the packs hold, or nothing.
 */
static void
txn_fold_rewritten_body(void *frame_)
{
	txn_fold_rewritten_frame_t *frame = frame_;
	TreesStoreNodeStore written = TREES_STORE_WRITER_NODE_STORE(&frame->writer->core);
	uint8		key[TXN_MEMTABLE_BRANCH_NODE_KEY];
	bool		some;
	txn_TupleU64VecU8_t node;
	bool		built = false;
	txn_TupleAddressBool_t taken;
	StdBytes	hashed;
	ChunksErrorError failed;
	bool		under;

	*frame->has_result = false;
	txn_memtable_branch_node_key(frame->address.f0, frame->tablespace, frame->relation, frame->point, key);
	if (!txn_memtable_branch_node(frame->io, frame->deltas, key, &some, &node, frame->error))
		return;
	if (some)
	{
		frame->stored = node.f1;
		built = true;
	}
	else if (frame->blob &&
			 !txn_memtable_branch_get(frame->io, frame->deltas, frame->address.f0, sizeof(frame->address.f0), &some,
									  &frame->stored, frame->error))
		return;
	if (!some)
	{
		ChunksTrailerLocator locator;

		if (frame->blob &&
			!chunks_packs_backend_Packs_find(&frame->repo->packs, frame->io, frame->reading, &frame->address, &some,
											 &locator, &failed))
		{
			*frame->error = txn_base_storage(&failed);
			return;
		}
		frame->ok = !some || txn_fold_rewritten_packed(frame, locator);
		return;
	}
	frame->stored_live = true;
	if (!std_vec_try_reserve(frame->taken, 1, TXN_FOLD_TAKEN))
	{
		*frame->error = host_Error_out_of_memory();
		return;
	}
	taken.f0 = frame->address;
	taken.f1 = built;
	std_vec_push_Checked(frame->taken, &taken, TXN_FOLD_TAKEN);
	if (built)
	{
		if (!chunks_trailer_unlocated(frame->stored.ptr, frame->stored.len, &hashed, &frame->stamps, &failed))
			goto storage;
	}
	else
	{
		hashed.ptr = frame->stored.ptr;
		hashed.len = frame->stored.len;
		frame->stamps = chunks_trailer_Stamps_Default_default();
	}
	frame->stamps_live = true;
	if (!chunks_header_addresses(hashed.ptr, hashed.len, &frame->addresses, &failed))
		goto storage;
	frame->addresses_live = true;
	if (!frame->blob)
		std_index_check(0, hashed.len, RUST_FILE, 2127, 25);
	under = frame->blob || ((const uint8 *) hashed.ptr)[0] == CHUNKS_HEADER_TREE_LEAF;
	std_vec_with_capacity_Checked(&frame->locators, frame->addresses.len, TXN_FOLD_LOCATOR);
	frame->locators_live = true;
	for (size_t at = 0; at < frame->addresses.len; at++)
	{
		ChunksAddressAddress child = STD_VEC_AT(&frame->addresses, ChunksAddressAddress, at);
		bool		readied;
		TreesStoreTreeRef ref;
		ChunksTrailerLocator locator;

		if (!txn_fold_rewritten(frame->io, frame->repo, frame->reading, frame->deltas, frame->tablespace,
								frame->relation, frame->point, frame->writer, child, under, frame->taken, &readied,
								&frame->child, &frame->child_locators, &frame->child_stamps, frame->error))
			return;
		if (readied)
		{
			bool		wrote = written.ops->write(written.self, frame->io, &frame->child, frame->child_locators.ptr,
												   frame->child_locators.len, &frame->child_stamps, &ref, &failed);

			std_vec_free_Checked(&frame->child_locators, TXN_FOLD_LOCATOR);
			chunks_trailer_Stamps_drop(&frame->child_stamps);
			if (!wrote)
				goto storage;
			locator = ref.locator;
		}
		else
		{
			std_index_check(0, hashed.len, RUST_FILE, 2134, 21);
			if (((const uint8 *) hashed.ptr)[0] == CHUNKS_HEADER_TREE_INTERNAL)
			{
				StdFmtPiece pieces[] = {STD_FMT_LIT("a built base's node "), STD_FMT_ARG(0),
				STD_FMT_LIT(" has no Chunk record")};
				StdFmtArg	args[] = {{&child, chunks_address_Address_Display_fmt}};
				StdFmtArguments arguments = {pieces, lengthof(pieces), args};
				StdString	message;

				std_fmt_format_Checked(&message, &arguments);
				*frame->error = host_Error_fork((const uint8 *) "XX001", &message);
				return;
			}
			locator = chunks_trailer_Locator_Default_default();
		}
		std_vec_push_Checked(&frame->locators, &locator, TXN_FOLD_LOCATOR);
	}
	txn_fold_bytes(hashed.ptr, hashed.len, frame->hashed);
	*frame->result_locators = frame->locators;
	*frame->result_stamps = frame->stamps;
	frame->locators_live = frame->stamps_live = false;
	*frame->has_result = true;
	frame->ok = true;
	return;
storage:
	*frame->error = txn_base_storage(&failed);
}

/* txn_fold_rewritten_pad frees what rewritten holds live. */
static void
txn_fold_rewritten_pad(void *frame_)
{
	txn_fold_rewritten_frame_t *frame = frame_;

	if (frame->locators_live)
		std_vec_free_Checked(&frame->locators, TXN_FOLD_LOCATOR);
	if (frame->addresses_live)
		std_vec_free_Checked(&frame->addresses, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	if (frame->stamps_live)
		chunks_trailer_Stamps_drop(&frame->stamps);
	if (frame->chunk_live)
		chunks_chunk_Chunk_drop(&frame->chunk);
	if (frame->stored_live)
		std_vec_free_Checked(&frame->stored, TXN_FOLD_BYTES);
	frame->locators_live = frame->addresses_live = frame->stamps_live = frame->chunk_live = false;
	frame->stored_live = false;
}

/*
 * txn_fold_rewritten readies a built base's chunk for a writer by its
 * address: one the working state's delta holds, each chunk it names readied
 * first and written at this server's locators, its key added to taken, a
 * tree's node under the base with the stamps its Chunk record carried; or,
 * for a blob's chunk, one this server's packs hold; or nothing for a chunk
 * neither holds. A leaf names such a chunk under an unknown locator; an
 * internal node naming one is data corruption. A chunk under a leaf (blob)
 * is held in the delta under the base when it is an internal node and by its
 * address alone when it is a leaf.
 */
static bool
txn_fold_rewritten(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, HostSharedRecord deltas,
				   uint32 tablespace, uint32 relation, uint64 point, TreesStoreBrandedWriter *writer,
				   ChunksAddressAddress address, bool blob, StdVec *taken, bool *has_result, StdVec *hashed,
				   StdVec *locators, ChunksTrailerStamps *stamps, HostError *error)
{
	txn_fold_rewritten_frame_t frame = {.io = io, .repo = repo, .reading = reading, .deltas = deltas,
		.tablespace = tablespace, .relation = relation, .point = point, .writer = writer, .address = address,
		.blob = blob, .taken = taken, .has_result = has_result, .hashed = hashed, .result_locators = locators,
	.result_stamps = stamps, .error = error};

	std_landing(&frame, txn_fold_rewritten_body, txn_fold_rewritten_pad);
	txn_fold_rewritten_pad(&frame);
	return frame.ok;
}

/*
 * txn_fold_rebuilt_frame_t is rebuilt's state across its pad: its deltas,
 * writer, the nodes it read, and a root's parts.
 */
typedef struct txn_fold_rebuilt_frame_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const HostSharedHeld *held;
	const TxnRecordBaseAdvanced *advanced;
	uint64		generation;
	HostSharedHeld deltas;
	bool		deltas_live;
	TreesStoreWriter plain;
	TreesStoreBrandedWriter writer;
	bool		writer_live;
	StdVec		nodes;
	bool		nodes_live;
	StdVec		hashed;
	StdVec		locators;
	ChunksTrailerStamps stamps;
	bool		has_roots[4];
	size_t		roots[4];
	TxnFoldInstalled installed;
	bool	   *result;
	HostError  *error;
	bool		ok;
} txn_fold_rebuilt_frame_t;

/* txn_fold_rebuilt_closure_2_frame_t is rebuilt's run's state across its pad: the roots it published. */
typedef struct txn_fold_rebuilt_closure_2_frame_t
{
	txn_fold_rebuilt_frame_t *rebuilt;
	HostIoRegistering *registering;
	ChunksPacksRootsRootSlots *slots;
	TxnFoldInstalled *result;
	ChunksErrorError *error;
	StdVec		published;
	bool		published_live;
	bool		ok;
} txn_fold_rebuilt_closure_2_frame_t;

/*
 * txn_fold_rebuilt_closure_2_body holds and publishes the rebuilt roots and
 * installs the base naming them at this server's locators.
 */
static void
txn_fold_rebuilt_closure_2_body(void *frame_)
{
	txn_fold_rebuilt_closure_2_frame_t *frame = frame_;
	txn_fold_rebuilt_frame_t *rebuilt = frame->rebuilt;
	HostIoTakesShort io = {&HOST_IO_REGISTERING_TAKES_SHORT_OPS, frame->registering};
	ChunksPacksRootsFold slot;
	TxnBaseBase *next = &frame->result->next;
	bool		held;
	bool		installed;
	size_t		at = 0;
	HostError	host;
	bool	   *has[4];
	TreesStoreTree *named[4];

	chunks_packs_roots_RootSlots_fold(frame->slots, &slot);
	held = txn_fold_held(&rebuilt->writer, io, slot.s0, rebuilt->has_roots[0], rebuilt->roots[0], true, frame->error);
	held = txn_fold_held(&rebuilt->writer, io, slot.s1, rebuilt->has_roots[1], rebuilt->roots[1], held, frame->error);
	held = txn_fold_held(&rebuilt->writer, io, slot.s2, rebuilt->has_roots[2], rebuilt->roots[2], held, frame->error);
	held = txn_fold_held(&rebuilt->writer, io, slot.s3, rebuilt->has_roots[3], rebuilt->roots[3], held, frame->error);
	if (!held)
		return;
	rebuilt->writer_live = false;
	if (!trees_store_BrandedWriter_publish(&rebuilt->writer, io, &frame->published, &frame->result->packs,
										   frame->error))
		return;
	frame->published_live = true;
	*next = rebuilt->advanced->base;
	txn_base_Key_Clone_clone(&rebuilt->advanced->base.key, &next->key);
	frame->result->some = true;
	has[0] = &next->has_root;
	has[1] = &next->has_tuple_ids;
	has[2] = &next->has_free_space;
	has[3] = &next->has_pending;
	named[0] = &next->root;
	named[1] = &next->tuple_ids;
	named[2] = &next->free_space;
	named[3] = &next->pending;
	for (int i = 0; i < 4; i++)
	{
		*has[i] = rebuilt->has_roots[i];
		if (rebuilt->has_roots[i])
			*named[i] = trees_published_Published_base(&STD_VEC_AT(&frame->published, TreesPublishedPublished, at++));
	}
	if (!txn_state_installed(io, rebuilt->held, rebuilt->generation, next, &installed, &host))
	{
		*frame->error = chunks_error_Error_FromError_from(&host);
		return;
	}
	if (installed)
		txn_state_grown(host_shared_Held_record(rebuilt->held), next);
	txn_fold_Installed_drop(frame->result);
	frame->result->some = installed;
	frame->ok = true;
}

/* txn_fold_rebuilt_closure_2_pad drops what holds a root's Pin: the writer and the published names. */
static void
txn_fold_rebuilt_closure_2_pad(void *frame_)
{
	txn_fold_rebuilt_closure_2_frame_t *frame = frame_;

	if (frame->published_live)
		txn_fold_published_drop(&frame->published);
	if (frame->rebuilt->writer_live)
		trees_store_BrandedWriter_drop(&frame->rebuilt->writer);
	frame->published_live = frame->rebuilt->writer_live = false;
}

/* txn_fold_rebuilt_closure_2_landing is the run's body, which drops its Pins before it returns. */
static void
txn_fold_rebuilt_closure_2_landing(void *frame_)
{
	txn_fold_rebuilt_closure_2_frame_t *frame = frame_;

	txn_fold_rebuilt_closure_2_body(frame);
	if (!frame->ok)
		txn_fold_Installed_drop(frame->result);
	txn_fold_rebuilt_closure_2_pad(frame);
}

/* txn_fold_rebuilt_closure_2_unwound drops what the run holds when it unwinds: its Pins and its base. */
static void
txn_fold_rebuilt_closure_2_unwound(void *frame_)
{
	txn_fold_rebuilt_closure_2_frame_t *frame = frame_;

	txn_fold_Installed_drop(frame->result);
	txn_fold_rebuilt_closure_2_pad(frame);
}

/* txn_fold_rebuilt_closure_2 is rebuilt's run under the root slots; its install's some is whether it installed. */
static bool
txn_fold_rebuilt_closure_2(void *captures, HostIoRegistering *registering, ChunksPacksRootsRootSlots *slots,
						   void *result, ChunksErrorError *error)
{
	txn_fold_rebuilt_closure_2_frame_t frame = {.rebuilt = captures, .registering = registering, .slots = slots,
	.result = result, .error = error};

	frame.result->some = false;
	std_landing(&frame, txn_fold_rebuilt_closure_2_landing, txn_fold_rebuilt_closure_2_unwound);
	return frame.ok;
}

/*
 * txn_fold_rebuilt_body writes each of the built base's trees again from the
 * nodes the delta holds, installs the base, and once the packs are flushed
 * takes those nodes out of the delta.
 */
static void
txn_fold_rebuilt_body(void *frame_)
{
	txn_fold_rebuilt_frame_t *frame = frame_;
	const TxnRecordBaseAdvanced *advanced = frame->advanced;
	const TxnBaseBase *base = &advanced->base;
	TreesStoreReader reader = repo_Repo_reader(frame->repo, HOST_CACHE_PLACEMENT_SERVER, frame->reading);
	HostSharedRecord deltas;
	ChunksErrorError failed;
	bool		has_trees[4] = {base->has_root, base->has_tuple_ids, base->has_free_space, base->has_pending};
	const TreesStoreTree *trees[4] = {&base->root, &base->tuple_ids, &base->free_space, &base->pending};

	*frame->result = false;
	if (advanced->storage.database == 0)
	{
		*frame->error = txn_fold_fork("XX001", "a built base's record names no database");
		return;
	}
	if (!txn_memtable_branch_deltas(frame->io, advanced->storage.database, repo_Repo_own(frame->repo),
									&frame->deltas, frame->error))
		return;
	frame->deltas_live = true;
	deltas = host_shared_Held_record(&frame->deltas);
	if (!trees_store_Writer_new(frame->io, reader, txn_state_space(host_shared_Held_record(frame->held)),
								&frame->plain, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	trees_store_Writer_brand(&frame->plain, &frame->writer);
	frame->writer_live = true;
	std_vec_new(&frame->nodes, TXN_FOLD_TAKEN);
	frame->nodes_live = true;
	for (int t = 0; t < 4; t++)
	{
		bool		readied;
		bool		wrote;

		frame->has_roots[t] = has_trees[t];
		if (!has_trees[t])
			continue;
		if (!txn_fold_rewritten(frame->io, frame->repo, frame->reading, deltas, advanced->storage.tablespace,
								advanced->storage.relation, advanced->folded, &frame->writer, trees[t]->address,
								false, &frame->nodes, &readied, &frame->hashed, &frame->locators, &frame->stamps,
								frame->error))
			return;
		if (!readied)
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("a built base's root "), STD_FMT_ARG(0),
			STD_FMT_LIT(" has no Chunk record")};
			StdFmtArg	args[] = {{&trees[t]->address, chunks_address_Address_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};
			StdString	message;

			std_fmt_format_Checked(&message, &arguments);
			*frame->error = host_Error_fork((const uint8 *) "XX001", &message);
			return;
		}
		wrote = trees_store_BrandedWriter_write_root(&frame->writer, frame->io, &frame->hashed, frame->locators.ptr,
													 frame->locators.len, &frame->stamps, &frame->roots[t], &failed);
		std_vec_free_Checked(&frame->locators, TXN_FOLD_LOCATOR);
		chunks_trailer_Stamps_drop(&frame->stamps);
		if (!wrote)
		{
			*frame->error = txn_base_storage(&failed);
			return;
		}
	}
	if (!chunks_packs_roots_RootSlots_register(frame->io, txn_fold_rebuilt_closure_2, frame, NULL, &frame->installed,
											   &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	*frame->result = frame->installed.some;
	if (!frame->installed.some)
	{
		frame->ok = true;
		return;
	}
	if (!chunks_packs_backend_Packs_flush(&frame->repo->packs, frame->io, frame->reading, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	for (size_t i = 0; i < frame->nodes.len; i++)
	{
		const txn_TupleAddressBool_t *node = &STD_VEC_AT(&frame->nodes, txn_TupleAddressBool_t, i);
		uint8		key[TXN_MEMTABLE_BRANCH_NODE_KEY];

		if (node->f1)
		{
			txn_memtable_branch_node_key(node->f0.f0, advanced->storage.tablespace, advanced->storage.relation,
										 advanced->folded, key);
			if (!txn_memtable_branch_node_taken(frame->io, deltas, key, frame->error))
				return;
		}
		else if (!txn_memtable_branch_taken(frame->io, deltas, node->f0.f0, sizeof(node->f0.f0), frame->error))
			return;
	}
	frame->ok = true;
}

/* txn_fold_rebuilt_pad frees what rebuilt holds live: the nodes, its writer, and its deltas. */
static void
txn_fold_rebuilt_pad(void *frame_)
{
	txn_fold_rebuilt_frame_t *frame = frame_;

	frame->installed.some = false;
	if (frame->nodes_live)
		std_vec_free_Checked(&frame->nodes, TXN_FOLD_TAKEN);
	if (frame->writer_live)
		trees_store_BrandedWriter_drop(&frame->writer);
	if (frame->deltas_live)
		host_shared_Held_drop(&frame->deltas);
	frame->nodes_live = frame->writer_live = frame->deltas_live = false;
}

/*
 * txn_fold_rebuilt writes a built base's trees again from the nodes its
 * Chunk records left in the working state's delta, into this server's own
 * packs, children before parents, and installs the base that names them at
 * this server's locators, when its base generation is still the one read;
 * once the packs are flushed it takes those nodes out of the delta. It
 * clones the record's key, since the installed base outlives the record.
 */
static bool
txn_fold_rebuilt(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const HostSharedHeld *held,
				 const TxnRecordBaseAdvanced *advanced, uint64 generation, bool *result, HostError *error)
{
	txn_fold_rebuilt_frame_t frame = {.io = io, .repo = repo, .reading = reading, .held = held,
	.advanced = advanced, .generation = generation, .result = result, .error = error};

	std_landing(&frame, txn_fold_rebuilt_body, txn_fold_rebuilt_pad);
	txn_fold_rebuilt_pad(&frame);
	return frame.ok;
}

/* txn_fold_named_same reports whether two bases name the same root, tuple id, and free space trees. */
static bool
txn_fold_named_same(const TxnBaseBase *a, const TxnBaseBase *b)
{
	return a->has_root == b->has_root && (!a->has_root ||
										  chunks_address_Address_PartialEq_eq(&a->root.address, &b->root.address)) &&
		a->has_tuple_ids == b->has_tuple_ids &&
		(!a->has_tuple_ids || chunks_address_Address_PartialEq_eq(&a->tuple_ids.address, &b->tuple_ids.address)) &&
		a->has_free_space == b->has_free_space &&
		(!a->has_free_space || chunks_address_Address_PartialEq_eq(&a->free_space.address, &b->free_space.address));
}

/* txn_fold_redo_frame_t is redo's state across its pad: a pass's newest base, while live. */
typedef struct txn_fold_redo_frame_t
{
	HostIoIo   *io;
	const RepoRepo *repo;
	const HostSharedReader *reading;
	const HostSharedHeld *held;
	const TxnRecordBaseAdvanced *advanced;
	uint64		end;
	TxnBaseBase newest;
	bool		newest_live;
	bool	   *result;
	HostError  *error;
	bool		ok;
} txn_fold_redo_frame_t;

/* txn_fold_redo_body installs the record's base, rebuilds a built one, or records the point, unless a newer base passed it. */
static void
txn_fold_redo_body(void *frame_)
{
	txn_fold_redo_frame_t *frame = frame_;
	const TxnRecordBaseAdvanced *advanced = frame->advanced;
	ChunksErrorError failed;
	bool		holds;

	*frame->result = true;
	if (!chunks_packs_backend_Packs_holds(&frame->repo->packs, frame->io, frame->reading, advanced->packs.ptr,
										  advanced->packs.len, &holds, &failed))
	{
		*frame->error = txn_base_storage(&failed);
		return;
	}
	if (!holds && !advanced->built)
	{
		frame->ok = txn_state_reach(frame->io, frame->held, &advanced->base, frame->end, frame->result,
									frame->error);
		return;
	}
	for (;;)
	{
		uint64		generation = txn_state_generation(host_shared_Held_record(frame->held));
		bool		passed = false;
		bool		installed;

		if (!txn_state_newest(frame->io, frame->held, &frame->newest_live, &frame->newest, frame->error))
			return;
		if (frame->newest_live)
			passed = advanced->built ? txn_fold_named_same(&frame->newest, &advanced->base) :
				frame->newest.folded >= advanced->folded;
		if (passed)
		{
			frame->ok = advanced->built ? txn_fold_unbuilt(frame->io, advanced, frame->error) :
				txn_fold_installs_done(frame->io, frame->repo, frame->reading, frame->held, &frame->newest,
									   frame->error);
			return;
		}
		if (frame->newest_live)
			txn_base_Base_drop(&frame->newest);
		frame->newest_live = false;
		if (!holds)
		{
			bool		rebuilt;

			if (!txn_fold_rebuilt(frame->io, frame->repo, frame->reading, frame->held, advanced, generation, &rebuilt,
								  frame->error))
				return;
			if (rebuilt)
			{
				frame->ok = true;
				return;
			}
			continue;
		}
		if (!txn_state_installed(HOST_IO_IO_TAKES_SHORT(frame->io), frame->held, generation, &advanced->base,
								 &installed, frame->error))
			return;
		if (!installed)
			continue;
		if (advanced->built)
		{
			txn_state_grown(host_shared_Held_record(frame->held), &advanced->base);
			frame->ok = txn_fold_unbuilt(frame->io, advanced, frame->error);
			return;
		}
		frame->ok = txn_fold_installs_done(frame->io, frame->repo, frame->reading, frame->held, &advanced->base,
										   frame->error);
		return;
	}
}

/* txn_fold_redo_pad frees what redo holds live: a pass's newest base. */
static void
txn_fold_redo_pad(void *frame_)
{
	txn_fold_redo_frame_t *frame = frame_;

	if (frame->newest_live)
		txn_base_Base_drop(&frame->newest);
	frame->newest_live = false;
}

/*
 * txn_fold_redo replays a fold's record: when this server holds every pack
 * the record names, it installs the record's base in the storage's state; a
 * base a backend built it otherwise writes again from the nodes its Chunk
 * records left in the working state's delta; and for a fold's base on a
 * server without its packs it records the fold point for this server's own
 * fold to reach and installs nothing. An install is passed over when the
 * state's newest base is already the record's or a later one. It answers
 * false when a standby's slot holds an earlier point its fold hasn't
 * reached, and true otherwise.
 */
bool
txn_fold_redo(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, const HostSharedHeld *held,
			  const TxnRecordBaseAdvanced *advanced, uint64 end, bool *result, HostError *error)
{
	txn_fold_redo_frame_t frame = {.io = io, .repo = repo, .reading = reading, .held = held, .advanced = advanced,
	.end = end, .result = result, .error = error};

	std_landing(&frame, txn_fold_redo_body, txn_fold_redo_pad);
	txn_fold_redo_pad(&frame);
	return frame.ok;
}
